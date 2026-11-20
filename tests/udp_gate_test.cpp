// Use real loopback UDP to prove quit/heartbeat cannot pass after cut.
#include "udp_gate.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Socket {
    int value = ::socket(AF_INET, SOCK_DGRAM, 0);
    Socket() {
        require(value >= 0, "socket failed");
        require(fcntl(value, F_SETFL, O_NONBLOCK) == 0, "nonblocking failed");
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(value, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "bind failed");
    }
    ~Socket() { close(value); }
    sockaddr_in address() {
        sockaddr_in local{}; socklen_t size = sizeof(local);
        require(getsockname(value, reinterpret_cast<sockaddr*>(&local), &size) == 0, "getsockname failed");
        return local;
    }
};
int main() {
    ghost::SocketRuntime runtime;
    Socket server, client, otherClient;
    ghost::Target target; target.address = "127.0.0.1"; target.port = ntohs(server.address().sin_port);
    ghost::UdpGateFactory factory; auto gate = factory.create(target);
    auto front = client.address(); front.sin_port = htons(gate->port());
    auto sendPacket = [](int socket, const sockaddr_in& to, const char* data) {
        const auto length = std::strlen(data);
        require(sendto(socket, data, length, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) ==
            static_cast<ssize_t>(length), "sendto failed");
    };
    auto pump = [&] {
        for (int i = 0; i < 10; ++i) { gate->pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    };
    char buffer[128]{};
    sockaddr_in upstream{}; socklen_t size = sizeof(upstream);
    sendPacket(client.value, front, "join"); pump();
    require(recvfrom(server.value, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&upstream), &size) == 4,
        "client-to-server forwarding failed");
    require(std::memcmp(buffer, "join", 4) == 0, "payload changed");
    sendPacket(server.value, upstream, "accepted"); pump();
    require(recv(client.value, buffer, sizeof(buffer), 0) == 8, "server-to-client forwarding failed");
    sendPacket(otherClient.value, front, "unrelated"); pump();
    require(recv(server.value, buffer, sizeof(buffer), 0) < 0, "must pin local endpoint");
    gate->cut();
    sendPacket(client.value, front, "quit"); sendPacket(server.value, upstream, "heartbeat"); pump();
    require(recv(server.value, buffer, sizeof(buffer), 0) < 0, "quit leaked after cut");
    require(recv(client.value, buffer, sizeof(buffer), 0) < 0, "heartbeat leaked after cut");
    int probe = ::socket(AF_INET, SOCK_DGRAM, 0);
    require(probe >= 0, "probe socket failed");
    require(bind(probe, reinterpret_cast<sockaddr*>(&upstream), sizeof(upstream)) < 0, "remote port closed too early");
    close(probe);
    target.address = "example.invalid";
    bool rejected = false; try { factory.create(target); } catch (const std::exception&) { rejected = true; }
    require(rejected, "endpoint must be numeric");
    std::cout << "PASS real UDP forwarding, peer pinning, bidirectional cut, endpoint retention\n";
}
