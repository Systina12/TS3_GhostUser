// Exercise the production SDK adapter, including asynchronous GUI callbacks.
#include "../src/plugin.cpp"

#include <functional>
#include <iostream>
#include <map>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
unsigned int copyString(const std::string& value, char** out) {
    *out = static_cast<char*>(std::malloc(value.size() + 1));
    if (!*out) throw std::bad_alloc();
    std::memcpy(*out, value.c_str(), value.size() + 1);
    return ERROR_ok;
}
struct Tab {
    bool valid = true;
    int status = STATUS_CONNECTION_ESTABLISHED;
    std::string address = "203.0.113.10";
    uint64 port = 19987, channel = 2;
};
struct State {
    uint64 current = 1;
    std::map<uint64, Tab> tabs{{1, {}}, {2, {}}};
    bool finishStop = true, moveImmediately = true, failChannelInfo = false;
    int stops = 0, cuts = 0, closed = 0, moves = 0;
    std::string connectAddress, connectPath, movePassword;
    uint64 moveChannel = 0;
    void event(uint64 handler, int status, unsigned int code = ERROR_ok) {
        tabs.at(handler).status = status;
        ts3plugin_onConnectStatusChangeEvent(handler, status, code);
    }
};
State* state = nullptr;
struct TestGate final : ghost::Gate {
    State& data;
    explicit TestGate(State& s) : data(s) {}
    ~TestGate() { ++data.closed; }
    std::uint16_t port() const override { return 40111; }
    void pump() override {}
    void cut() noexcept override { ++data.cuts; }
};
struct TestFactory final : ghost::GateFactory {
    State& data;
    explicit TestFactory(State& s) : data(s) {}
    std::unique_ptr<ghost::Gate> create(const ghost::Target&) override { return std::make_unique<TestGate>(data); }
};
TS3Functions functions() {
    TS3Functions f{};
    f.getCurrentServerConnectionHandlerID = []() -> uint64 { return state->current; };
    f.getConnectionStatus = [](uint64 handler, int* out) -> unsigned int {
        const auto it = state->tabs.find(handler);
        if (it == state->tabs.end() || !it->second.valid) return ERROR_invalid_server_connection_handler_id;
        *out = it->second.status;
        return ERROR_ok;
    };
    f.stopConnection = [](uint64 handler, const char*) -> unsigned int {
        ++state->stops;
        if (!state->tabs.at(handler).valid) return ERROR_invalid_server_connection_handler_id;
        if (state->finishStop) state->event(handler, STATUS_DISCONNECTED);
        return ERROR_ok;
    };
    f.guiConnect = [](PluginConnectTab tab, const char*, const char* address, const char*, const char*,
        const char* channel, const char*, const char*, const char*, const char*, const char*, const char*,
        const char*, const char*, uint64* result) -> unsigned int {
        require(tab == PLUGIN_CONNECT_TAB_CURRENT, "must only use the selected tab");
        state->connectAddress = address;
        state->connectPath = channel;
        *result = state->current;
        // Deliberately leave status and endpoint stale, as guiConnect does in TS3.
        return ERROR_ok;
    };
    f.getClientID = [](uint64, anyID* out) -> unsigned int { *out = 7; return ERROR_ok; };
    f.getConnectionVariableAsUInt64 = [](uint64 handler, anyID, size_t, uint64* out) -> unsigned int {
        *out = state->tabs.at(handler).port; return ERROR_ok;
    };
    f.getConnectionVariableAsString = [](uint64 handler, anyID, size_t, char** out) -> unsigned int {
        return copyString(state->tabs.at(handler).address, out);
    };
    f.getServerVariableAsString = [](uint64, size_t flag, char** out) -> unsigned int {
        return copyString(flag == VIRTUALSERVER_UNIQUE_IDENTIFIER ? "server uid" : "test server", out);
    };
    f.getClientSelfVariableAsString = [](uint64, size_t flag, char** out) -> unsigned int {
        return copyString(flag == CLIENT_UNIQUE_IDENTIFIER ? "identity uid" : "test user", out);
    };
    f.getChannelOfClient = [](uint64 handler, anyID, uint64* out) -> unsigned int {
        *out = state->tabs.at(handler).channel; return ERROR_ok;
    };
    f.getServerConnectInfo = [](uint64, char* address, unsigned short* port, char* password, size_t) -> unsigned int {
        std::strcpy(address, "203.0.113.10"); *port = 19987; *password = '\0'; return ERROR_ok;
    };
    f.getChannelConnectInfo = [](uint64, uint64, char* path, char* password, size_t) -> unsigned int {
        if (state->failChannelInfo) return ERROR_channel_invalid_id;
        std::strcpy(path, "Root/Channel"); std::strcpy(password, "known password"); return ERROR_ok;
    };
    f.getChannelVariableAsString = [](uint64, uint64 id, size_t flag, char** out) -> unsigned int {
        // IDs 2 and 3 represent Root > "A/B" and Root > A > B respectively.
        return copyString(flag == CHANNEL_PASSWORD ? "known password" : id == 2 ? "A/B" : "B", out);
    };
    f.getParentChannelOfChannel = [](uint64, uint64 id, uint64* out) -> unsigned int {
        *out = id == 1 ? 0 : id == 3 ? 4 : 1; return ERROR_ok;
    };
    f.setClientSelfVariableAsInt = [](uint64, size_t, int) -> unsigned int { return ERROR_ok_no_update; };
    f.flushClientSelfUpdates = [](uint64, const char*) -> unsigned int { return ERROR_ok_no_update; };
    f.requestClientMove = [](uint64 handler, anyID, uint64 channel, const char* password, const char*) -> unsigned int {
        ++state->moves; state->moveChannel = channel; state->movePassword = password;
        if (state->moveImmediately) state->tabs.at(handler).channel = channel;
        return ERROR_ok;
    };
    f.freeMemory = [](void* p) -> unsigned int { std::free(p); return ERROR_ok; };
    return f;
}
struct Fixture {
    State data;
    TestFactory gates{data};
    ghost::Target target;
    Fixture() {
        state = &data;
        ts3plugin_setFunctionPointers(functions());
        host.reset(); managed.store(0); pendingError.store(0);
        controller = std::make_unique<ghost::Controller>(host, gates);
        target = readTarget();
    }
    ~Fixture() { controller.reset(); host.reset(); sockets.reset(); }
    void relay() {
        controller->start(target, {2, 0, 0}, 0);
        controller->tick(1);
        data.tabs[1].address = "127.0.0.1"; data.tabs[1].port = 40111;
        data.event(1, STATUS_CONNECTION_ESTABLISHED);
        controller->tick(2); controller->tick(3);
        require(controller->phase() == ghost::Phase::holding, "must establish relay before the test");
    }
};
}

int main() {
    int passed = 0;
    const auto test = [&](const char* name, const std::function<void()>& fn) {
        try { fn(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n'; throw;
        }
    };
    test("pending endpoint confirmation is scoped to its handler", [] {
        Fixture f;
        host.connect(f.target, 40111);
        require(host.connection(1) == ghost::Connection::connecting, "stale endpoint must not be accepted");
        require(host.connection(2) == ghost::Connection::established, "another tab must remain usable");
        f.data.tabs[1].valid = false;
        require(host.connection(1) == ghost::Connection::disconnected && !host.replacingConnection(1),
            "closed handler must clear its expectation");
    });
    test("closing the test tab allows a new batch in another tab", [] {
        Fixture f;
        controller->start(f.target, {2, 0, 0}, 0); controller->tick(1);
        f.data.tabs[1].valid = false; f.data.current = 2;
        controller->tick(2); controller->tick(3);
        require(!controller->cleaning() && !host.replacingConnection(1), "closed tab must finish cancellation");
        const auto target = readTarget();
        controller->start(target, {2, 0, 0}, 4);
        require(controller->running(), "second connected tab must start");
    });
    test("stop cancels the GUI retry after a silent cut", [] {
        Fixture f; f.relay(); controller->tick(4);
        f.data.finishStop = false; controller->stop(); controller->tick(5);
        require(controller->cleaning(), "raw established status must not complete cancellation");
        f.data.event(1, STATUS_DISCONNECTED, ERROR_connection_lost); controller->tick(6);
        require(controller->cleaning(), "loss must wait for the queued GUI retry");
        f.data.event(2, STATUS_DISCONNECTED); controller->tick(6);
        require(controller->cleaning(), "an unrelated tab's clean disconnect must not confirm this cancellation");
        f.data.event(1, STATUS_CONNECTING); f.data.finishStop = true;
        controller->tick(7); controller->tick(8);
        require(!controller->cleaning() && f.data.stops == 3 && f.data.cuts == 1,
            "must cancel the retry without an extra cut or replacement connection");
    });
    test("cancelling an inactive tab leaves the active tab untouched", [] {
        Fixture f; f.relay(); controller->tick(4);
        f.data.current = 2; controller->tick(5); controller->tick(6);
        require(!controller->running() && !controller->cleaning(), "inactive test must cancel");
        require(f.data.tabs[2].status == STATUS_CONNECTION_ESTABLISHED && f.data.connectAddress == "127.0.0.1:40111",
            "must not stop or reconnect the active tab");
    });
    test("reading relay metadata preserves the real upstream", [] {
        Fixture f; f.relay();
        const auto active = readTarget();
        require(active.address == f.target.address && active.port == f.target.port, "active relay must resolve");
        controller->tick(4); f.data.finishStop = false; controller->stop();
        const auto stopped = readTarget();
        require(stopped.address == f.target.address && stopped.port == f.target.port, "cut relay must resolve during cleanup");
        f.data.current = 2; f.data.tabs[2].address = "127.0.0.1"; f.data.tabs[2].port = 40111;
        require(readTarget().port == 40111, "another loopback tab is a legitimate upstream");
    });
    test("ambiguous channel names restore by distinct IDs and confirm the move", [] {
        Fixture f; f.data.failChannelInfo = true;
        for (uint64 channel : {2u, 3u}) {
            f.data.tabs[1].channel = channel; f.data.event(1, STATUS_CONNECTION_ESTABLISHED);
            const auto target = readTarget();
            require(target.channelPath.empty() && target.channelId == channel, "fallback must not invent a GUI path");
            f.data.event(1, STATUS_DISCONNECTED); f.data.tabs[1].channel = 1;
            f.data.moveImmediately = false;
            controller->restore(target, 10);
            require(f.data.connectPath.empty(), "connect must omit the ambiguous path");
            f.data.event(1, STATUS_CONNECTION_ESTABLISHED); controller->tick(11); controller->tick(12);
            require(controller->running() && f.data.moveChannel == channel && f.data.movePassword == "known password",
                "manual restore must move by saved ID and password and wait for confirmation");
            f.data.tabs[1].channel = channel; controller->tick(13);
            require(!controller->running() && f.data.cuts == 0, "confirmed restore must finish without cutting");
        }
    });
#ifdef _WIN32
    test("GUI blocks refresh, start and restore until cancellation completes", [] {
        Fixture f; f.relay();
        module = GetModuleHandleW(nullptr);
        showWindow();
        controller->tick(4); f.data.finishStop = false; controller->stop(); updateUi();
        for (const auto id : {refreshId, startId, restoreId})
            require(!IsWindowEnabled(GetDlgItem(window, id)), "cleanup must disable connection-changing controls");
        selectCurrent();
        require(selected.address == f.target.address && selected.port == f.target.port,
            "refresh during cleanup must preserve the real endpoint");
        f.data.event(1, STATUS_DISCONNECTED); controller->tick(5); updateUi();
        for (const auto id : {refreshId, startId, restoreId})
            require(IsWindowEnabled(GetDlgItem(window, id)) != FALSE, "clean disconnect must reenable controls");
        ts3plugin_shutdown();
        require(!window, "GUI test must release its window");
    });
#endif
    test("plugin shutdown resets pending state before reinitialization", [] {
        Fixture f;
        host.connect(f.target, 40111);
        ts3plugin_shutdown();
        require(!host.replacingConnection(1), "shutdown must clear the SDK adapter");
        require(ts3plugin_init() == 0 && host.connection(2) == ghost::Connection::established,
            "reinitialized adapter must accept another connected tab");
        ts3plugin_shutdown();
    });
    std::cout << passed << " SDK connection tests passed\n";
}
