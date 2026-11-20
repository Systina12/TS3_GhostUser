// Native Windows GUI integration follows Systina12/PokeWave's Plugins menu pattern.
#include "plugin_api.h"
#include "controller.h"
#include "udp_gate.h"
#include "teamspeak/public_errors.h"
#include "teamspeak/public_rare_definitions.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#endif

namespace {
TS3Functions ts{};
std::string pluginId;
std::atomic<std::uint64_t> managed{0};
std::atomic<unsigned int> pendingError{0};

std::string errorText(unsigned int code) {
    char* result = nullptr;
    if (ts.getErrorMessage && ts.getErrorMessage(code, &result) == ERROR_ok && result) {
        const std::string text(result);
        ts.freeMemory(result);
        return text;
    }
    return "TeamSpeak error " + std::to_string(code);
}
void check(unsigned int code) {
    if (code != ERROR_ok && code != ERROR_ok_no_update)
        throw std::runtime_error(errorText(code));
}
template <typename Fn>
std::string readString(Fn&& fn) {
    char* result = nullptr;
    check(fn(&result));
    if (!result) throw std::runtime_error("TeamSpeak 返回了空字符串指针");
    const std::string value(result);
    ts.freeMemory(result);
    return value;
}

ghost::Target resolveTarget(const ghost::Target& observed);

[[maybe_unused]] ghost::Target readTarget() {
    ghost::Target target;
    target.handler = ts.getCurrentServerConnectionHandlerID();
    int status = 0;
    check(ts.getConnectionStatus(target.handler, &status));
    if (status != STATUS_CONNECTION_ESTABLISHED) throw std::runtime_error("请先连接到测试服务器");
    anyID id = 0;
    check(ts.getClientID(target.handler, &id));
    target.address = readString([&](char** out) {
        return ts.getConnectionVariableAsString(target.handler, id, CONNECTION_SERVER_IP, out);
    });
    uint64 port = 0;
    check(ts.getConnectionVariableAsUInt64(target.handler, id, CONNECTION_SERVER_PORT, &port));
    if (!port || port > 65535) throw std::runtime_error("服务器 UDP 端口无效");
    target.port = static_cast<std::uint16_t>(port);
    target.serverName = readString([&](char** out) {
        return ts.getServerVariableAsString(target.handler, VIRTUALSERVER_NAME, out);
    });
    target.serverUid = readString([&](char** out) {
        return ts.getServerVariableAsString(target.handler, VIRTUALSERVER_UNIQUE_IDENTIFIER, out);
    });
    target.identityUid = readString([&](char** out) {
        return ts.getClientSelfVariableAsString(target.handler, CLIENT_UNIQUE_IDENTIFIER, out);
    });
    target.nickname = readString([&](char** out) {
        return ts.getClientSelfVariableAsString(target.handler, CLIENT_NICKNAME, out);
    });
    uint64 channelId = 0;
    check(ts.getChannelOfClient(target.handler, id, &channelId));
    target.channelId = channelId;
    char host[4096]{}, serverPassword[4096]{}, channel[4096]{}, channelPassword[4096]{};
    unsigned short unusedPort = 0;
    check(ts.getServerConnectInfo(target.handler, host, &unusedPort, serverPassword, sizeof(host)));
    target.serverPassword = serverPassword;
    const auto channelInfoError = ts.getChannelConnectInfo(target.handler, channelId, channel, channelPassword, sizeof(channel));
    if (channelInfoError == ERROR_ok) {
        target.channelPath = channel;
        target.channelPassword = channelPassword;
    } else {
        // Some client builds reject this GUI convenience function for a valid channel.
        // Use the regular read APIs, including only the password the client already knows.
        target.channelPassword = readString([&](char** out) {
            return ts.getChannelVariableAsString(target.handler, channelId, CHANNEL_PASSWORD, out);
        });
        // A raw name can contain '/', so it cannot safely be assembled into a GUI path.
        // Connect without a path, then move and verify by the saved channel ID.
    }
    return resolveTarget(target);
}

class TsHost final : public ghost::Host {
public:
    bool replacingConnection(std::uint64_t handler) const {
        return awaitingEndpoint_.load() && expectedHandler_.load() == handler;
    }
    void statusChanged(std::uint64_t handler, int status, unsigned int error) {
        if (cancelHandler_.load() == handler && status == STATUS_DISCONNECTED)
            cancelConfirmed_.store(error == ERROR_ok || error == ERROR_ok_no_update);
    }
    void reset() {
        awaitingEndpoint_.store(false);
        expectedHandler_.store(0);
        cancelHandler_.store(0);
        cancelConfirmed_.store(false);
        disconnectedSince_ = {};
    }
    std::uint64_t currentHandler() override { return ts.getCurrentServerConnectionHandlerID(); }
    ghost::Connection connection(std::uint64_t handler) override {
        int status = 0;
        const auto result = ts.getConnectionStatus(handler, &status);
        if (result == ERROR_invalid_server_connection_handler_id || result == ERROR_not_connected) {
            forgetConnection(handler);
            return ghost::Connection::disconnected;
        }
        check(result);
        if (cancelHandler_.load() == handler) {
            if (cancelConfirmed_.load() && status == STATUS_DISCONNECTED) {
                forgetConnection(handler);
                return ghost::Connection::disconnected;
            }
            if (status == STATUS_DISCONNECTED) {
                const auto now = std::chrono::steady_clock::now();
                if (disconnectedSince_ == std::chrono::steady_clock::time_point{}) disconnectedSince_ = now;
                // With auto-reconnect disabled there is no retry to cancel. A quiet
                // disconnect is complete; the normal GUI retry occurs after one second.
                if (now - disconnectedSince_ >= std::chrono::seconds(2)) {
                    forgetConnection(handler);
                    return ghost::Connection::disconnected;
                }
            } else {
                disconnectedSince_ = {};
                if (status != STATUS_CONNECTION_ESTABLISHED) {
                    // Cancelling the GUI's retry while it is connecting produces a clean
                    // disconnect event and clears its auto-reconnect intent.
                    const auto error = ts.stopConnection(handler, "Ghost User cancelled");
                    if (error != ERROR_not_connected && error != ERROR_invalid_server_connection_handler_id) check(error);
                }
            }
            return ghost::Connection::connecting;
        }
        if (replacingConnection(handler)) {
            if (status != STATUS_CONNECTION_ESTABLISHED) return ghost::Connection::connecting;
            anyID ownId = 0;
            uint64 port = 0;
            if (ts.getClientID(handler, &ownId) != ERROR_ok ||
                ts.getConnectionVariableAsUInt64(handler, ownId, CONNECTION_SERVER_PORT, &port) != ERROR_ok)
                return ghost::Connection::connecting;
            const auto address = readString([&](char** out) {
                return ts.getConnectionVariableAsString(handler, ownId, CONNECTION_SERVER_IP, out);
            });
            if (port != expectedPort_ || address != expectedAddress_) return ghost::Connection::connecting;
            awaitingEndpoint_.store(false);
        }
        if (status == STATUS_CONNECTION_ESTABLISHED) return ghost::Connection::established;
        if (status == STATUS_DISCONNECTED) return ghost::Connection::disconnected;
        return ghost::Connection::connecting;
    }
    void disconnect(std::uint64_t handler) override {
        forgetConnection(handler);
        int status = STATUS_DISCONNECTED;
        const auto result = ts.getConnectionStatus(handler, &status);
        if (result == ERROR_invalid_server_connection_handler_id || result == ERROR_not_connected) return;
        check(result);
        cancelConfirmed_.store(false);
        cancelHandler_.store(handler);
        disconnectedSince_ = {};
        const auto error = ts.stopConnection(handler, "Ghost User test");
        if (error != ERROR_not_connected && error != ERROR_invalid_server_connection_handler_id) check(error);
    }
    void forgetConnection(std::uint64_t handler) override {
        if (expectedHandler_.load() == handler) {
            awaitingEndpoint_.store(false);
            expectedHandler_.store(0);
        }
        if (cancelHandler_.load() == handler) {
            cancelHandler_.store(0);
            cancelConfirmed_.store(false);
            disconnectedSince_ = {};
        }
    }
    std::uint64_t connect(const ghost::Target& target, std::uint16_t localPort) override {
        uint64 handler = 0;
        const auto address = "127.0.0.1:" + std::to_string(localPort);
        expectedAddress_ = "127.0.0.1";
        expectedPort_ = localPort;
        expectedHandler_.store(target.handler);
        awaitingEndpoint_.store(true);
        check(ts.guiConnect(PLUGIN_CONNECT_TAB_CURRENT, target.serverName.c_str(), address.c_str(),
            target.serverPassword.c_str(), target.nickname.c_str(), target.channelPath.c_str(),
            target.channelPassword.c_str(), "", "", "", "", target.identityUid.c_str(), "", "", &handler));
        if (!handler) throw std::runtime_error("guiConnect 未返回连接句柄");
        expectedHandler_.store(handler);
        managed.store(handler);
        return handler;
    }
    std::uint64_t connectDirect(const ghost::Target& target) override {
        const auto address = (target.address.find(':') != std::string::npos
            ? "[" + target.address + "]" : target.address) + ":" + std::to_string(target.port);
        uint64 handler = 0;
        expectedAddress_ = target.address;
        expectedPort_ = target.port;
        expectedHandler_.store(target.handler);
        awaitingEndpoint_.store(true);
        check(ts.guiConnect(PLUGIN_CONNECT_TAB_CURRENT, target.serverName.c_str(), address.c_str(),
            target.serverPassword.c_str(), target.nickname.c_str(), target.channelPath.c_str(),
            target.channelPassword.c_str(), "", "", "", "", target.identityUid.c_str(), "", "", &handler));
        if (!handler) throw std::runtime_error("guiConnect 未返回连接句柄");
        expectedHandler_.store(handler);
        managed.store(handler);
        return handler;
    }
    void prepare(const ghost::Target& target) override {
        const auto uid = readString([&](char** out) {
            return ts.getClientSelfVariableAsString(target.handler, CLIENT_UNIQUE_IDENTIFIER, out);
        });
        const auto server = readString([&](char** out) {
            return ts.getServerVariableAsString(target.handler, VIRTUALSERVER_UNIQUE_IDENTIFIER, out);
        });
        if (uid != target.identityUid) throw std::runtime_error("重连使用的身份不匹配，已取消本次断网");
        if (server != target.serverUid) throw std::runtime_error("服务器身份不匹配，已取消本次断网");
        check(ts.setClientSelfVariableAsInt(target.handler, CLIENT_INPUT_MUTED, 1));
        check(ts.setClientSelfVariableAsInt(target.handler, CLIENT_OUTPUT_MUTED, 1));
        check(ts.flushClientSelfUpdates(target.handler, nullptr));
        if (!inChannel(target)) {
            anyID id = 0;
            check(ts.getClientID(target.handler, &id));
            check(ts.requestClientMove(target.handler, id, target.channelId, target.channelPassword.c_str(), nullptr));
        }
    }
    bool inChannel(const ghost::Target& target) override {
        anyID id = 0;
        uint64 channel = 0;
        check(ts.getClientID(target.handler, &id));
        check(ts.getChannelOfClient(target.handler, id, &channel));
        return channel == target.channelId;
    }
    std::string takeError() override {
        const auto code = pendingError.exchange(0);
        return code ? errorText(code) : std::string();
    }
private:
    std::atomic_bool awaitingEndpoint_{false};
    std::atomic<std::uint64_t> expectedHandler_{0}, cancelHandler_{0};
    std::atomic_bool cancelConfirmed_{false};
    std::chrono::steady_clock::time_point disconnectedSince_{};
    std::string expectedAddress_;
    std::uint16_t expectedPort_ = 0;
};

TsHost host;
ghost::UdpGateFactory factory;
std::unique_ptr<ghost::SocketRuntime> sockets;
std::unique_ptr<ghost::Controller> controller;

ghost::Target resolveTarget(const ghost::Target& observed) {
    return controller ? controller->originalTarget(observed) : observed;
}

#ifdef _WIN32
constexpr wchar_t windowClass[] = L"TS3GhostUserWindowV1";
constexpr UINT timerId = 1;
constexpr int menuOpen = 1;
enum { countId = 101, gapId, holdId, startId, stopId, refreshId, restoreId };
HINSTANCE module = nullptr;
HWND window = nullptr, countBox = nullptr, gapBox = nullptr, holdBox = nullptr;
HWND targetLabel = nullptr, statusLabel = nullptr, progress = nullptr, progressLabel = nullptr;
HFONT font = nullptr, titleFont = nullptr;
HBRUSH background = nullptr;
ghost::Target selected;
bool hasSelection = false;
bool busy = false;
bool closeRequested = false;
std::wstring notice;
ULONGLONG lastUi = 0;

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::wstring controlText(HWND control) {
    const int size = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(size) + 1, L'\0');
    GetWindowTextW(control, result.data(), size + 1);
    result.resize(static_cast<std::size_t>(size));
    return result;
}
void setText(HWND control, const std::wstring& text) { SetWindowTextW(control, text.c_str()); }
void showError(const std::exception& error) {
    MessageBoxW(window, wide(error.what()).c_str(), L"Ghost User", MB_OK | MB_ICONERROR);
}
HWND control(const wchar_t* kind, const wchar_t* text, DWORD style,
             int x, int y, int width, int height, int id = 0) {
    HWND result = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module, nullptr);
    if (!result) throw std::runtime_error("无法创建 GUI 控件");
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return result;
}
void updateUi() {
    if (!controller || !statusLabel) return;
    const bool running = controller->running();
    const bool idle = !running && !controller->cleaning();
    for (const auto id : {countId, gapId, holdId, refreshId}) EnableWindow(GetDlgItem(window, id), idle);
    EnableWindow(GetDlgItem(window, startId), idle);
    EnableWindow(GetDlgItem(window, stopId), running);
    EnableWindow(GetDlgItem(window, restoreId), idle && hasSelection);
    setText(statusLabel, controller->cleaning() ? wide(controller->message()) + L"；正在清理连接…" :
        (!running && !notice.empty() ? notice : wide(controller->message())));
    setText(progressLabel, L"已执行异常断线  " + std::to_wstring(controller->completed()) + L" / " +
        std::to_wstring(controller->total()) + L"    ·    本地保留端点 " + std::to_wstring(controller->retained()));
    SendMessageW(progress, PBM_SETPOS, controller->total()
        ? static_cast<WPARAM>(1000.0L * controller->completed() / controller->total()) : 0, 0);
}
void selectCurrent() {
    if (controller->running() || controller->cleaning()) return;
    selected = readTarget();
    hasSelection = true;
    notice.clear();
    setText(targetLabel, wide(selected.serverName + "   ·   " + selected.address + ":" + std::to_string(selected.port)
        + "\r\n昵称：" + selected.nickname + "    频道：" +
        (selected.channelPath.empty() ? "ID " + std::to_string(selected.channelId) : selected.channelPath)
        + "\r\n身份 UID：" + selected.identityUid));
    updateUi();
}
void start() {
    ghost::Options options;
    options.count = ghost::positiveInteger(controlText(countBox));
    options.gap = ghost::unsignedInteger(controlText(gapBox));
    options.hold = ghost::unsignedInteger(controlText(holdBox));
    ghost::validate(options);
    if (!hasSelection) selectCurrent();
    // Read fresh metadata, but never replace the saved real endpoint with a loopback gate.
    if (host.currentHandler() != selected.handler)
        throw std::runtime_error("请切回面板显示的 TS3 测试标签页，或点击“读取当前连接”");
    const auto current = readTarget();
    if (current.serverUid != selected.serverUid || current.identityUid != selected.identityUid ||
        current.address != selected.address || current.port != selected.port || current.channelId != selected.channelId)
        throw std::runtime_error("测试连接已变化，请重新点击“读取当前连接”");
    selected = current;
    notice.clear();
    managed.store(selected.handler);
    pendingError.store(0);
    controller->start(selected, options, GetTickCount64());
    updateUi();
}
void restore() {
    if (!hasSelection || controller->running() || controller->cleaning()) return;
    if (host.currentHandler() != controller->handler() && host.currentHandler() != selected.handler)
        throw std::runtime_error("请先切回测试标签页，恢复操作不会改动其他标签页");
    if (host.connection(host.currentHandler()) != ghost::Connection::disconnected)
        throw std::runtime_error("测试连接仍在清理，请稍后再恢复");
    auto target = selected;
    target.handler = host.currentHandler();
    pendingError.store(0);
    notice.clear();
    controller->restore(target, GetTickCount64());
    selected.handler = controller->handler();
    updateUi();
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    try {
        switch (message) {
        case WM_CREATE: {
            window = hwnd;
            font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            titleFont = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            background = CreateSolidBrush(RGB(245, 247, 251));
            auto title = control(L"STATIC", L"Ghost User", 0, 28, 22, 480, 40);
            SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
            control(L"STATIC", L"TeamSpeak 3  /  连接超时测试", 0, 30, 66, 600, 24);
            control(L"STATIC", L"当前测试连接", 0, 30, 110, 420, 24);
            control(L"BUTTON", L"读取当前连接", BS_PUSHBUTTON | WS_TABSTOP, 520, 105, 148, 32, refreshId);
            targetLabel = control(L"STATIC", L"连接测试服务器后，点击“读取当前连接”。", 0, 30, 144, 638, 88);
            control(L"STATIC", L"批次次数", 0, 30, 252, 190, 24);
            control(L"STATIC", L"重连间隔 / 毫秒", 0, 250, 252, 190, 24);
            control(L"STATIC", L"连接保持 / 毫秒", 0, 470, 252, 198, 24);
            countBox = control(L"EDIT", L"3", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                30, 284, 198, 38, countId);
            gapBox = control(L"EDIT", L"1500", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                250, 284, 198, 38, gapId);
            holdBox = control(L"EDIT", L"500", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                470, 284, 198, 38, holdId);
            for (auto box : {countBox, gapBox, holdBox}) SendMessageW(box, EM_SETLIMITTEXT, 20, 0);
            control(L"STATIC", L"次数是累计异常断线次数；实际同时残留数取决于服务器超时和连接限制。",
                0, 30, 336, 638, 44);
            control(L"BUTTON", L"开始", BS_DEFPUSHBUTTON | WS_TABSTOP, 30, 392, 190, 42, startId);
            control(L"BUTTON", L"停止", BS_PUSHBUTTON | WS_TABSTOP, 244, 392, 190, 42, stopId);
            control(L"BUTTON", L"恢复普通连接", BS_PUSHBUTTON | WS_TABSTOP, 458, 392, 210, 42, restoreId);
            progress = control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 30, 460, 638, 12);
            SendMessageW(progress, PBM_SETRANGE32, 0, 1000);
            progressLabel = control(L"STATIC", L"", 0, 30, 486, 638, 24);
            statusLabel = control(L"STATIC", L"就绪", 0, 30, 524, 638, 68);
            control(L"STATIC", L"使用当前身份和频道；切换 TS3 标签页或关闭窗口会停止批次。",
                0, 30, 606, 638, 44);
            if (!SetTimer(hwnd, timerId, 20, nullptr)) throw std::runtime_error("无法启动 GUI 定时器");
            updateUi();
            return 0;
        }
        case WM_TIMER:
            if (controller && !busy) {
                busy = true; // guiConnect can dispatch nested Windows messages.
                const auto now = GetTickCount64();
                if (closeRequested) {
                    closeRequested = false;
                    controller->stop("已停止：控制窗口已关闭");
                    ShowWindow(hwnd, SW_HIDE);
                }
                controller->tick(now);
                if (now - lastUi >= 200 && IsWindowVisible(hwnd)) { updateUi(); lastUi = now; }
                busy = false;
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED && !busy) {
                busy = true;
                try {
                    switch (LOWORD(wParam)) {
                    case refreshId: selectCurrent(); break;
                    case startId: start(); break;
                    case stopId: notice.clear(); controller->stop(); updateUi(); break;
                    case restoreId: restore(); break;
                    default: break;
                    }
                } catch (...) { busy = false; throw; }
                busy = false;
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(45, 55, 72));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(245, 247, 251));
            return reinterpret_cast<LRESULT>(background);
        case WM_ERASEBKGND: {
            RECT area{}; GetClientRect(hwnd, &area);
            FillRect(reinterpret_cast<HDC>(wParam), &area, background);
            return 1;
        }
        case WM_CLOSE:
            if (busy) { closeRequested = true; return 0; }
            notice.clear();
            controller->stop("已停止：控制窗口已关闭");
            ShowWindow(hwnd, SW_HIDE); // Keep the timer alive to drain retired endpoints for 60 s.
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, timerId);
            if (font) DeleteObject(font);
            if (titleFont) DeleteObject(titleFont);
            if (background) DeleteObject(background);
            font = titleFont = nullptr;
            background = nullptr;
            window = nullptr;
            return 0;
        default: break;
        }
    } catch (const std::exception& error) {
        busy = false;
        if (message == WM_CREATE) return -1;
        showError(error);
    } catch (...) {
        busy = false;
        if (message == WM_CREATE) return -1;
        MessageBoxW(hwnd, L"GUI 操作失败", L"Ghost User", MB_OK | MB_ICONERROR);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void showWindow() {
    if (!window) {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
        if (!InitCommonControlsEx(&controls)) throw std::runtime_error("无法初始化 Windows 控件");
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.lpfnWndProc = windowProc;
        klass.hInstance = module;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.lpszClassName = windowClass;
        if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("无法注册 GUI 窗口类");
        RECT bounds{0, 0, 700, 660};
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectEx(&bounds, style, FALSE, WS_EX_APPWINDOW);
        if (!CreateWindowExW(WS_EX_APPWINDOW, windowClass, L"Ghost User — TeamSpeak 3", style,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, module, nullptr)) throw std::runtime_error("无法创建 GUI 窗口");
    }
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    updateUi();
    if (!hasSelection) {
        try { selectCurrent(); }
        catch (const std::exception& error) { setText(targetLabel, wide(error.what())); }
    }
}
#endif
} // namespace

extern "C" {
const char* ts3plugin_name() { return "Ghost User"; }
const char* ts3plugin_version() { return "0.1.3"; }
int ts3plugin_apiVersion() { return 26; }
const char* ts3plugin_author() { return "TS3_GhostUser contributors"; }
const char* ts3plugin_description() { return "Native GUI for finite connection-timeout tests on your TeamSpeak server."; }
void ts3plugin_setFunctionPointers(const TS3Functions funcs) { ts = funcs; }
void ts3plugin_registerPluginID(const char* id) {
    // TeamSpeak invalidates the supplied buffer after this callback returns.
    // The SDK requires this callback, together with freeMemory, for plugin menus.
    pluginId = id ? id : "";
}
int ts3plugin_init() {
    if (!ts.guiConnect || !ts.stopConnection || !ts.getCurrentServerConnectionHandlerID ||
        !ts.getConnectionStatus || !ts.getClientID || !ts.getClientSelfVariableAsString ||
        !ts.getConnectionVariableAsString || !ts.getConnectionVariableAsUInt64 ||
        !ts.getServerVariableAsString || !ts.getChannelOfClient || !ts.getServerConnectInfo ||
        !ts.getChannelConnectInfo || !ts.getChannelVariableAsString || !ts.getParentChannelOfChannel ||
        !ts.setClientSelfVariableAsInt || !ts.flushClientSelfUpdates ||
        !ts.requestClientMove || !ts.freeMemory) return 1;
    try {
#ifdef _WIN32
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ts3plugin_init), &module)) return 1;
#endif
        sockets = std::make_unique<ghost::SocketRuntime>();
        host.reset();
        controller = std::make_unique<ghost::Controller>(host, factory);
        return 0;
    } catch (...) { sockets.reset(); return 1; }
}
void ts3plugin_shutdown() {
    managed.store(0);
#ifdef _WIN32
    busy = true;
    if (window) KillTimer(window, timerId);
#endif
    bool settled = true;
    if (controller) settled = controller->shutdown([] {
#ifdef _WIN32
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
#endif
    });
    if (!settled && ts.printMessage)
        ts.printMessage(host.currentHandler(), "Ghost User: disconnect cleanup timed out; disconnect the test tab in TeamSpeak.", PLUGIN_MESSAGE_TARGET_SERVER);
    host.reset();
#ifdef _WIN32
    if (window) DestroyWindow(window);
    if (module) UnregisterClassW(windowClass, module);
    hasSelection = false;
    closeRequested = false;
    notice.clear();
    selected = {};
    busy = false;
#endif
    controller.reset();
    sockets.reset();
    pluginId.clear();
    pendingError.store(0);
}
int ts3plugin_offersConfigure() { return PLUGIN_OFFERS_NO_CONFIGURE; }
void ts3plugin_configure(void*, void*) {}
int ts3plugin_requestAutoload() { return 1; }
void ts3plugin_initMenus(PluginMenuItem*** items, char** icon) {
    if (icon) *icon = nullptr;
    if (!items) return;
    *items = nullptr;
    if (pluginId.empty()) return;
    auto list = static_cast<PluginMenuItem**>(std::calloc(2, sizeof(PluginMenuItem*)));
    if (!list) return;
    list[0] = static_cast<PluginMenuItem*>(std::calloc(1, sizeof(PluginMenuItem)));
    if (!list[0]) { std::free(list); return; }
    list[0]->type = PLUGIN_MENU_TYPE_GLOBAL;
    list[0]->id = 1;
    std::strcpy(list[0]->text, "Ghost User GUI");
    *items = list;
}
void ts3plugin_freeMemory(void* data) { std::free(data); }
void ts3plugin_onMenuItemEvent(uint64, PluginMenuType type, int menuId, uint64) {
    if (type != PLUGIN_MENU_TYPE_GLOBAL || menuId != 1 || !controller) return;
#ifdef _WIN32
    try { showWindow(); } catch (const std::exception& error) { showError(error); }
#endif
}
void ts3plugin_onConnectStatusChangeEvent(uint64 handler, int status, unsigned int error) {
    host.statusChanged(handler, status, error);
    if (host.replacingConnection(handler) && error == ERROR_connection_lost) return;
    if (handler == managed.load() && status == STATUS_DISCONNECTED && error != ERROR_ok && error != ERROR_ok_no_update)
        pendingError.store(error);
}
int ts3plugin_onServerErrorEvent(uint64 handler, const char*, unsigned int error, const char*, const char*) {
    if (handler == managed.load() && error != ERROR_ok && error != ERROR_ok_no_update) pendingError.store(error);
    return 0; // Preserve TeamSpeak's own error display.
}
} // extern C
