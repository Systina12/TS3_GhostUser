#include "controller.h"
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace ghost;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template <typename Fn> void rejects(Fn&& fn) {
    bool failed = false;
    try { fn(); } catch (const std::exception&) { failed = true; }
    require(failed, "expected rejection");
}
struct Shared { std::vector<std::string> events; int cuts = 0; int closed = 0; int pumps = 0; bool failPump = false; };
class FakeGate final : public Gate {
public:
    explicit FakeGate(Shared& s) : shared(s) {}
    ~FakeGate() { ++shared.closed; }
    std::uint16_t port() const override { return 23000; }
    void pump() override { ++shared.pumps; if (shared.failPump) throw std::runtime_error("transport unavailable"); }
    void cut() noexcept override { ++shared.cuts; shared.events.push_back("cut"); }
    Shared& shared;
};
class FakeFactory final : public GateFactory {
public:
    explicit FakeFactory(Shared& s) : shared(s) {}
    std::unique_ptr<Gate> create(const Target&) override { return std::make_unique<FakeGate>(shared); }
    Shared& shared;
};
class FakeHost final : public Host {
public:
    explicit FakeHost(Shared& s) : shared(s) {}
    std::uint64_t current = 7;
    Connection state = Connection::established;
    std::string error;
    bool inTargetChannel = true, finishDisconnect = true;
    int connects = 0, directConnects = 0;
    std::uint64_t currentHandler() override { return current; }
    Connection connection(std::uint64_t) override { return state; }
    void disconnect(std::uint64_t) override {
        shared.events.push_back("disconnect");
        if (finishDisconnect) state = Connection::disconnected;
    }
    void forgetConnection(std::uint64_t) override { shared.events.push_back("forget"); }
    std::uint64_t connect(const Target&, std::uint16_t port) override {
        require(port == 23000, "must use the gate port");
        shared.events.push_back("connect");
        ++connects; state = Connection::connecting; return 7;
    }
    std::uint64_t connectDirect(const Target&) override {
        shared.events.push_back("restore");
        ++directConnects; state = Connection::connecting; return 7;
    }
    void prepare(const Target&) override { shared.events.push_back("prepare"); }
    bool inChannel(const Target&) override { return inTargetChannel; }
    std::string takeError() override { auto result = error; error.clear(); return result; }
    Shared& shared;
};
struct Fixture {
    Shared shared;
    FakeHost host{shared};
    FakeFactory factory{shared};
    Controller controller{host, factory};
    Target target;
    Fixture() { target.handler = 7; target.address = "127.0.0.1"; target.port = 9987; target.identityUid = "test"; }
    void connected(Millis now) {
        controller.tick(now); host.state = Connection::established;
        controller.tick(now + 1); controller.tick(now + 2);
        require(controller.phase() == Phase::holding, "must join before hold");
    }
};
int main() {
    int passed = 0;
    auto test = [&](const char* name, std::function<void()> fn) {
        try { fn(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { std::cerr << "FAIL " << name << ": " << e.what() << '\n'; throw; }
    };
    test("strict GUI numeric validation", [] {
        require(positiveInteger(L"18446744073709551615") == std::numeric_limits<std::uint64_t>::max(), "uint64 max");
        for (auto value : {L"", L"0", L"-1", L"+1", L" 1", L"1 ", L"1.0", L"nan", L"18446744073709551616"})
            rejects([&] { positiveInteger(value); });
        rejects([] { validate({0, 1500, 500}); });
        require(unsignedInteger(L"0") == 0, "zero delay must be accepted");
        rejects([] { unsignedInteger(L"-1"); });
        validate({3, 0, 0});
        validate({3, 999, 249});
        validate({3, std::numeric_limits<Millis>::max(), std::numeric_limits<Millis>::max()});
        validate({std::numeric_limits<std::uint64_t>::max(), 1500, 500});
    });
    test("cut then GUI reconnect; finite batch restores original server", [] {
        Fixture f;
        f.controller.start(f.target, {2, 1000, 250}, 0); f.connected(10);
        f.controller.tick(262);
        require(f.controller.completed() == 1, "one cut");
        const auto& events = f.shared.events;
        require(events.back() == "cut", "silent cut must not call raw disconnect");
        f.controller.tick(263); f.controller.tick(1261);
        require(f.host.connects == 1, "respect gap");
        f.connected(1263); f.controller.tick(1515); f.controller.tick(2515);
        f.host.state = Connection::established;
        f.controller.tick(2516); f.controller.tick(2517);
        require(!f.controller.running() && f.controller.completed() == 2 && f.host.connects == 2 &&
            f.host.directConnects == 1 && f.host.state == Connection::established, "exact batch and restore");
        f.controller.tick(100000);
        require(f.controller.retained() == 0 && f.shared.closed == 2, "endpoints must expire");
        require(f.host.connects == 2, "no automatic replenishment");
    });
    test("ordinary stop is graceful", [] {
        Fixture f; f.controller.start(f.target, {}, 0); f.connected(10);
        f.controller.stop(); f.controller.tick(100);
        require(!f.controller.running() && f.shared.cuts == 0, "stop must not cut");
    });
    test("zero delays progress on the next tick without an injected minimum", [] {
        Fixture f;
        f.controller.start(f.target, {2, 0, 0}, 0);
        f.connected(1);
        f.controller.tick(4);
        require(f.controller.completed() == 1, "zero hold must not wait 250 ms");
        f.controller.tick(5);
        f.controller.tick(6);
        require(f.host.connects == 2, "zero gap must not wait 1000 ms");
    });
    test("switching tab cancels", [] {
        Fixture f; f.controller.start(f.target, {}, 0); f.connected(10);
        f.host.current = 8; f.controller.tick(2000);
        require(!f.controller.running() && f.shared.cuts == 0 && f.host.connects == 1, "tab switch must cancel");
    });
    test("server rejection stops without retry", [] {
        Fixture f; f.controller.start(f.target, {}, 0); f.controller.tick(1);
        f.host.error = "maximum number of connections with the same identity exceeded";
        f.controller.tick(2); f.controller.tick(100000);
        require(!f.controller.running() && f.host.connects == 1 && f.shared.cuts == 0, "must not retry rejection");
    });
    test("wrong channel cannot be cut", [] {
        Fixture f; f.host.inTargetChannel = false;
        f.controller.start(f.target, {}, 0); f.controller.tick(1);
        f.host.state = Connection::established; f.controller.tick(2); f.controller.tick(10002);
        require(!f.controller.running() && f.shared.cuts == 0, "must not cut wrong channel");
    });
    test("connect timeout cancels", [] {
        Fixture f; f.controller.start(f.target, {}, 0); f.controller.tick(1); f.controller.tick(20001);
        require(!f.controller.running() && f.shared.cuts == 0, "must stop on timeout");
    });
    test("manual reconnect in gap is preserved", [] {
        Fixture f; f.controller.start(f.target, {2, 1000, 250}, 0); f.connected(10);
        f.controller.tick(262); f.controller.tick(263);
        f.host.state = Connection::connecting;
        f.controller.tick(2000);
        require(!f.controller.running() && f.host.connects == 1 && f.shared.cuts == 1 &&
            f.host.state == Connection::connecting,
            "manual reconnect must cancel rather than start a second test connection");
    });
    test("expected transport timeout during a long gap does not cancel the batch", [] {
        Fixture f;
        f.controller.start(f.target, {2, 60000, 0}, 0);
        f.connected(10); f.controller.tick(13);
        f.host.error = "Connection lost";
        f.host.state = Connection::connecting;
        f.controller.tick(30000);
        require(f.controller.running() && f.host.connects == 1, "expected timeout must preserve configured gap");
        f.controller.tick(60013);
        require(f.host.connects == 2, "batch must resume after the configured gap");
    });
    test("cannot overwrite gate during graceful cleanup", [] {
        Fixture f; f.controller.start(f.target, {}, 0); f.connected(10);
        f.host.finishDisconnect = false; f.controller.stop();
        rejects([&] { f.controller.start(f.target, {}, 100); });
        require(f.shared.closed == 0 && f.shared.cuts == 0, "must retain live forwarding gate");
        f.host.state = Connection::disconnected; f.controller.tick(101);
        require(f.shared.closed == 1, "release gate after disconnect");
    });
    test("stop after cut blocks another batch until cancellation completes", [] {
        Fixture f; f.controller.start(f.target, {2, 0, 0}, 0); f.connected(1);
        f.controller.tick(4);
        f.host.finishDisconnect = false;
        f.controller.stop();
        require(f.controller.cleaning() && f.shared.cuts == 1, "cut cancellation must be tracked without an active gate");
        rejects([&] { f.controller.start(f.target, {}, 5); });
        f.host.state = Connection::disconnected; f.controller.tick(6);
        require(!f.controller.cleaning(), "disconnected cancellation must finish");
    });
    test("unload drains the asynchronous quit before destroying the gate", [] {
        Fixture f; f.controller.start(f.target, {}, 0); f.connected(1);
        f.host.finishDisconnect = false;
        const int pumps = f.shared.pumps;
        int dispatches = 0;
        require(f.controller.shutdown([&] {
            ++dispatches;
            require(f.shared.closed == 0 && f.shared.cuts == 0, "unload must retain the forwarding transport");
            require(f.shared.pumps > pumps, "unload must pump the quit");
            f.host.state = Connection::disconnected;
        }), "unload must finish cleanly");
        require(dispatches == 1 && f.shared.closed == 1, "release only after the async quit finishes");
    });
    test("relay metadata resolves to the real server without rewriting other loopback servers", [] {
        Fixture f; f.controller.start(f.target, {2, 0, 0}, 0); f.connected(1);
        auto observed = f.target; observed.port = 23000;
        require(f.controller.originalTarget(observed).port == 9987, "resolve active relay");
        f.controller.tick(4);
        require(f.controller.originalTarget(observed).port == 9987, "resolve retired relay");
        observed.handler = 8;
        require(f.controller.originalTarget(observed).port == 23000, "do not rewrite another tab");
        observed.handler = 7; observed.port = 9990;
        require(f.controller.originalTarget(observed).port == 9990, "do not reject legitimate loopback servers");
    });
    test("manual restore verifies the saved channel and never cuts", [] {
        Fixture f; f.host.state = Connection::disconnected;
        f.controller.restore(f.target, 0);
        f.host.state = Connection::established; f.host.inTargetChannel = false;
        f.controller.tick(1); f.controller.tick(2);
        require(f.controller.running(), "restore must wait for the correct channel");
        f.host.inTargetChannel = true; f.controller.tick(3);
        require(!f.controller.running() && f.shared.cuts == 0 && f.host.directConnects == 1, "restore must only connect directly");
    });
    test("transport errors do not prevent cancellation from finishing", [] {
        Fixture f; f.controller.start(f.target, {}, 0); f.connected(1);
        f.host.finishDisconnect = false; f.shared.failPump = true; f.controller.stop();
        f.host.state = Connection::disconnected; f.controller.tick(5);
        require(!f.controller.cleaning() && f.shared.closed == 1 && f.shared.cuts == 0,
            "must still poll disconnect when the forwarding pump fails");
    });
    std::cout << passed << " controller tests passed\n";
}
