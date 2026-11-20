#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ghost {
using Millis = std::uint64_t;

struct Options {
    std::uint64_t count = 3;
    Millis gap = 1500;
    Millis hold = 500;
};

std::uint64_t positiveInteger(const std::wstring& text);
std::uint64_t unsignedInteger(const std::wstring& text);
void validate(const Options& options);

struct Target {
    std::uint64_t handler = 0;
    std::uint64_t channelId = 0;
    std::uint16_t port = 0;
    std::uint16_t connectPort = 0;
    std::string address, serverName, serverUid, identityUid;
    std::string connectHost;
    std::string nickname, channelPath, serverPassword, channelPassword;
};

enum class Connection { disconnected, connecting, established };

class Host {
public:
    virtual ~Host() = default;
    virtual std::uint64_t currentHandler() = 0;
    virtual Connection connection(std::uint64_t handler) = 0;
    virtual void disconnect(std::uint64_t handler) = 0;
    virtual void forgetConnection(std::uint64_t handler) = 0;
    virtual std::uint64_t connect(const Target& target, std::uint16_t localPort) = 0;
    virtual std::uint64_t connectDirect(const Target& target) = 0;
    virtual void prepare(const Target& target) = 0;
    virtual bool inChannel(const Target& target) = 0;
    virtual std::string takeError() = 0;
};

class Gate {
public:
    virtual ~Gate() = default;
    virtual std::uint16_t port() const = 0;
    virtual void pump() = 0;
    virtual void cut() noexcept = 0;
};

class GateFactory {
public:
    virtual ~GateFactory() = default;
    virtual std::unique_ptr<Gate> create(const Target& target) = 0;
};

enum class Phase { idle, initialDisconnect, connecting, joining, holding, gap };

class Controller {
public:
    Controller(Host& host, GateFactory& factory) : host_(host), factory_(factory) {}
    void start(const Target& target, const Options& options, Millis now);
    void tick(Millis now) noexcept;
    void stop(const std::string& reason = "已停止");
    void restore(const Target& target, Millis now);
    bool shutdown(const std::function<void()>& processEvents = {}) noexcept;
    Target originalTarget(const Target& observed) const;
    bool running() const { return phase_ != Phase::idle; }
    bool cleaning() const { return cleaning_; }
    Phase phase() const { return phase_; }
    std::uint64_t completed() const { return completed_; }
    std::uint64_t total() const { return options_.count; }
    std::size_t retained() const { return retired_.size(); }
    std::uint64_t handler() const { return target_.handler; }
    std::uint16_t localPort() const { return active_ ? active_->port() : 0; }
    const std::string& message() const { return message_; }

private:
    struct Retired { Millis expires; std::unique_ptr<Gate> gate; };
    void advance(Millis now);
    void beginConnection(Millis now);
    void enter(Phase phase, Millis now, const char* message);
    Host& host_;
    GateFactory& factory_;
    Target target_;
    Options options_;
    Phase phase_ = Phase::idle;
    Millis since_ = 0;
    std::uint64_t completed_ = 0;
    bool gapTransportLost_ = false;
    bool cleaning_ = false;
    std::string message_ = "就绪";
    std::unique_ptr<Gate> active_;
    std::vector<Retired> retired_;
};
} // namespace ghost
