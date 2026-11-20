#include "udp_gate.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ghost {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
using Socklen = int;
constexpr Socket invalidSocket = INVALID_SOCKET;
int socketError() { return WSAGetLastError(); }
bool transient(int error) { return error == WSAEWOULDBLOCK || error == WSAEINTR; }
void closeSocket(Socket socket) { closesocket(socket); }
void nonblocking(Socket socket) {
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled)) throw std::runtime_error("无法设置非阻塞 UDP");
}
#else
using Socket = int;
using Socklen = socklen_t;
constexpr Socket invalidSocket = -1;
int socketError() { return errno; }
bool transient(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }
void closeSocket(Socket socket) { ::close(socket); }
void nonblocking(Socket socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1)
        throw std::runtime_error("无法设置非阻塞 UDP");
}
#endif
struct Handle {
    Socket value = invalidSocket;
    ~Handle() { if (value != invalidSocket) closeSocket(value); }
};

class UdpGate final : public Gate {
public:
    explicit UdpGate(const Target& target) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
        addrinfo* addresses = nullptr;
        const auto service = std::to_string(target.port);
        if (getaddrinfo(target.address.c_str(), service.c_str(), &hints, &addresses))
            throw std::runtime_error("客户端未提供数字 IP 地址，无法固定测试端点");
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addressOwner(addresses, freeaddrinfo);
        remote_.value = socket(addresses->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        local_.value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (remote_.value == invalidSocket || local_.value == invalidSocket) fail("创建 UDP socket");
        if (::connect(remote_.value, addresses->ai_addr, static_cast<Socklen>(addresses->ai_addrlen)))
            fail("固定服务器端点");
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (bind(local_.value, reinterpret_cast<sockaddr*>(&local), sizeof(local))) fail("绑定回环端口");
        Socklen size = sizeof(local);
        if (getsockname(local_.value, reinterpret_cast<sockaddr*>(&local), &size)) fail("读取回环端口");
        port_ = ntohs(local.sin_port);
        nonblocking(local_.value);
        nonblocking(remote_.value);
    }

    std::uint16_t port() const override { return port_; }
    void cut() noexcept override { forwarding_ = false; }

    void pump() override {
        // Limit per-tick work so the native GUI stays responsive.
        std::array<char, 65536> packet{};
        for (int i = 0; i < 64; ++i) {
            sockaddr_in sender{};
            Socklen size = sizeof(sender);
            const int count = static_cast<int>(recvfrom(local_.value, packet.data(), static_cast<int>(packet.size()), 0,
                reinterpret_cast<sockaddr*>(&sender), &size));
            if (count < 0) { receiveError(); break; }
            if (sender.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
            if (!hasPeer_) { peer_ = sender; hasPeer_ = true; }
            if (forwarding_ && sender.sin_port == peer_.sin_port) {
                const int sent = static_cast<int>(send(remote_.value, packet.data(), count, 0));
                if (sent < 0 && !transient(socketError())) fail("转发到服务器");
            }
        }
        for (int i = 0; i < 64; ++i) {
            const int count = static_cast<int>(recv(remote_.value, packet.data(), static_cast<int>(packet.size()), 0));
            if (count < 0) { receiveError(); break; }
            if (forwarding_ && hasPeer_) {
                const int sent = static_cast<int>(sendto(local_.value, packet.data(), count, 0,
                    reinterpret_cast<const sockaddr*>(&peer_), sizeof(peer_)));
                if (sent < 0 && !transient(socketError())) fail("转发到客户端");
            }
        }
        // After cut, retain BOTH bound sockets and discard all traffic, including quit.
        // Closing the remote port immediately can produce ICMP and change the timeout being measured.
    }

private:
    [[noreturn]] static void fail(const char* action) {
        throw std::runtime_error(std::string(action) + "失败 (socket " + std::to_string(socketError()) + ")");
    }
    void receiveError() {
        if (forwarding_ && !transient(socketError())) fail("接收 UDP");
    }
    Handle local_, remote_;
    sockaddr_in peer_{};
    std::uint16_t port_ = 0;
    bool hasPeer_ = false;
    bool forwarding_ = true;
};
} // namespace

SocketRuntime::SocketRuntime() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup 失败");
#endif
}
SocketRuntime::~SocketRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
}
std::unique_ptr<Gate> UdpGateFactory::create(const Target& target) {
    return std::make_unique<UdpGate>(target);
}
} // namespace ghost
