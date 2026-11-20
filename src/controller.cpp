#include "controller.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>

namespace ghost {
std::uint64_t unsignedInteger(const std::wstring& text) {
    if (text.empty()) throw std::invalid_argument("请输入非负整数");
    std::uint64_t result = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') throw std::invalid_argument("请输入不带符号的十进制非负整数");
        const auto digit = static_cast<std::uint64_t>(ch - L'0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            throw std::invalid_argument("数值超过 64 位整数范围");
        result = result * 10 + digit;
    }
    return result;
}

std::uint64_t positiveInteger(const std::wstring& text) {
    const auto result = unsignedInteger(text);
    if (!result) throw std::invalid_argument("次数必须大于零");
    return result;
}

void validate(const Options& options) {
    if (!options.count) throw std::invalid_argument("次数必须大于零");
}

void Controller::enter(Phase phase, Millis now, const char* message) {
    phase_ = phase;
    since_ = now;
    message_ = message;
    if (phase == Phase::gap) gapTransportLost_ = false;
}

void Controller::start(const Target& target, const Options& options, Millis now) {
    if (running()) throw std::runtime_error("已有任务正在运行");
    if (cleaning_)
        throw std::runtime_error("上一个测试连接仍在清理，请稍后再开始");
    validate(options);
    if (!target.handler || !target.port || target.address.empty() || target.identityUid.empty())
        throw std::runtime_error("请先读取一个已连接的测试标签页");
    if (host_.currentHandler() != target.handler)
        throw std::runtime_error("请先切回所选的 TS3 测试标签页");
    if (host_.connection(target.handler) != Connection::established)
        throw std::runtime_error("所选标签页尚未连接");
    // Validate the endpoint and reserve sockets before changing the current connection.
    auto gate = factory_.create(target);
    target_ = target;
    options_ = options;
    completed_ = 0;
    active_ = std::move(gate);
    host_.takeError();
    enter(Phase::initialDisconnect, now, "正在将测试连接切换到本地转发…");
    try { host_.disconnect(target_.handler); }
    catch (...) { phase_ = Phase::idle; active_.reset(); throw; }
}

void Controller::beginConnection(Millis now) {
    host_.takeError();
    if (completed_ == options_.count) {
        enter(Phase::connecting, now, "断网次数已完成，正在恢复原服务器连接…");
        target_.handler = host_.connectDirect(target_);
    } else {
        if (!active_) active_ = factory_.create(target_);
        enter(Phase::connecting, now, "正在连接测试会话…");
        target_.handler = host_.connect(target_, active_->port());
    }
}

void Controller::stop(const std::string& reason) {
    if (!running()) return;
    phase_ = Phase::idle; // Clear this before calling into TeamSpeak (callbacks may reenter).
    message_ = reason;
    cleaning_ = true;
    try {
        // Even an invalid/closed handler must release its pending endpoint expectation.
        host_.disconnect(target_.handler);
        if (active_) active_->pump();
    } catch (const std::exception& error) {
        message_ += std::string("；清理：") + error.what();
    }
    // A live gate stays forwarding until the client completes its graceful disconnect.
    // tick() then retires it normally. Never turn an ordinary stop into an extra silent cut.
}

void Controller::restore(const Target& target, Millis now) {
    if (running() || cleaning_) throw std::runtime_error("测试连接仍在清理，请稍后再恢复");
    if (host_.currentHandler() != target.handler)
        throw std::runtime_error("请先切回测试标签页");
    if (host_.connection(target.handler) != Connection::disconnected)
        throw std::runtime_error("测试标签页尚未断开");
    target_ = target;
    completed_ = 0;
    options_ = {0, 0, 0}; // A restore uses the same identity/channel verification, with no cuts.
    try { beginConnection(now); }
    catch (...) { stop("恢复连接失败"); throw; }
}

Target Controller::originalTarget(const Target& observed) const {
    auto result = observed;
    if (observed.handler != target_.handler || observed.address != "127.0.0.1" ||
        observed.serverUid != target_.serverUid || observed.identityUid != target_.identityUid)
        return result;
    const auto isPort = [&](const auto& entry) { return entry.gate->port() == observed.port; };
    if ((active_ && active_->port() == observed.port) ||
        std::any_of(retired_.begin(), retired_.end(), isPort)) {
        result.address = target_.address;
        result.port = target_.port;
        result.connectHost = target_.connectHost;
        result.connectPort = target_.connectPort;
    }
    return result;
}

bool Controller::shutdown(const std::function<void()>& processEvents) noexcept {
    try { stop("插件已卸载"); } catch (...) {}
    // stopConnection is asynchronous. Keep forwarding and dispatch the GUI's events
    // until its disconnect (including cancellation of a queued auto-reconnect) finishes.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (cleaning_ && std::chrono::steady_clock::now() < deadline) {
        tick(since_); // No new work; avoid mixing the GUI and steady-clock time origins.
        if (!cleaning_) break;
        try {
            if (processEvents) processEvents();
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } catch (...) { break; }
    }
    const bool settled = !cleaning_;
    host_.forgetConnection(target_.handler);
    active_.reset();
    retired_.clear();
    target_ = {};
    cleaning_ = false;
    return settled;
}

void Controller::tick(Millis now) noexcept {
    try {
        for (auto& entry : retired_) entry.gate->pump();
        retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
            [now](const Retired& entry) { return now >= entry.expires; }), retired_.end());
        if (!running()) {
            // A transport error must not prevent cancellation of the GUI's next retry.
            if (active_) { try { active_->pump(); } catch (...) {} }
            if (cleaning_ && host_.connection(target_.handler) == Connection::disconnected) {
                active_.reset();
                cleaning_ = false;
            }
            return;
        }
        if (active_) active_->pump();
        if (host_.currentHandler() != target_.handler) {
            stop("已停止：你切换了 TS3 标签页");
            return;
        }
        const auto error = host_.takeError();
        if (!error.empty() && phase_ == Phase::gap) gapTransportLost_ = true;
        if (!error.empty() && phase_ != Phase::gap) { stop("已停止：" + error); return; }
        advance(now);
    } catch (const std::exception& error) {
        try { stop(std::string("已停止：") + error.what()); } catch (...) { phase_ = Phase::idle; }
    } catch (...) {
        try { stop("已停止：未知错误"); } catch (...) { phase_ = Phase::idle; }
    }
}

void Controller::advance(Millis now) {
    const auto elapsed = now - since_;
    switch (phase_) {
    case Phase::initialDisconnect:
        if (host_.connection(target_.handler) == Connection::disconnected) beginConnection(now);
        else if (elapsed >= 10000) stop("初始断开超时");
        break;
    case Phase::connecting: {
        const auto status = host_.connection(target_.handler);
        if (status == Connection::established) {
            host_.prepare(target_); // Verify identity/server, mute audio, and join the chosen channel.
            enter(Phase::joining, now, "正在确认测试频道…");
        } else if (elapsed >= 20000 || (status == Connection::disconnected && elapsed >= 5000)) {
            stop("连接失败或超时；请查看 TS3 的连接错误");
        }
        break;
    }
    case Phase::joining:
        if (host_.connection(target_.handler) != Connection::established) stop("进入频道前连接已断开");
        else if (host_.inChannel(target_)) {
            if (completed_ == options_.count) {
                phase_ = Phase::idle;
                message_ = options_.count ? "批次完成，已恢复原服务器连接" : "已恢复原服务器连接";
            } else enter(Phase::holding, now, "会话已进入频道，等待模拟断网…");
        }
        else if (elapsed >= 10000) stop("进入测试频道超时");
        break;
    case Phase::holding:
        if (host_.connection(target_.handler) != Connection::established) {
            stop("模拟断网前连接已断开");
        } else if (elapsed >= options_.hold) {
            // Reserve storage before the irreversible test action so allocation cannot strand a cut gate.
            retired_.reserve(retired_.size() + 1);
            active_->cut();
            retired_.push_back({now + 60000, std::move(active_)});
            ++completed_;
            // guiConnect manages the GUI's disconnect/reconnect state. A raw stop here
            // waits for an ACK behind the cut gate and triggers GUI autoreconnect.
            enter(Phase::gap, now, "已切断转发，等待 GUI 重连…");
        }
        break;
    case Phase::gap:
        if (host_.connection(target_.handler) == Connection::connecting && !gapTransportLost_) {
            // There is no live test gate in this phase. Leave the user's new connection alone.
            phase_ = Phase::idle;
            host_.forgetConnection(target_.handler);
            message_ = "已停止：检测到新的手动连接";
        }
        else if (elapsed >= options_.gap) beginConnection(now);
        break;
    case Phase::idle:
        break;
    }
}
} // namespace ghost
