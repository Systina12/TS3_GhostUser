#include "endpoint.h"

#include <array>

namespace ghost {
std::string endpoint(const std::string& host, std::uint16_t port) {
    const bool bracketed = host.size() >= 2 && host.front() == '[' && host.back() == ']';
    return (host.find(':') != std::string::npos && !bracketed ? "[" + host + "]" : host) +
        ":" + std::to_string(port);
}

std::string addressDescription(const std::string& address) {
    if (address == "::1" || address == "[::1]") return "本机回环地址";
    if (address.find(':') != std::string::npos) return "IPv6 地址";
    std::array<unsigned, 4> octets{};
    std::size_t offset = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned digits = 0;
        while (offset < address.size() && address[offset] >= '0' && address[offset] <= '9') {
            octets[i] = octets[i] * 10 + (address[offset++] - '0');
            if (++digits > 3 || octets[i] > 255) return "域名或其他主机地址";
        }
        if (!digits) return "域名或其他主机地址";
        if (i < 3 && (offset >= address.size() || address[offset++] != '.'))
            return "域名或其他主机地址";
    }
    if (offset != address.size()) return "域名或其他主机地址";
    const auto a = octets[0], b = octets[1];
    if (a == 127) return "本机回环地址";
    if (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168))
        return "RFC 1918 内网地址";
    if (a == 198 && (b == 18 || b == 19))
        return "基准测试地址段；也常用于代理 Fake-IP 映射";
    if (a == 100 && b >= 64 && b <= 127) return "运营商共享地址段";
    if (a == 169 && b == 254) return "链路本地地址";
    return "IPv4 地址（非 RFC 1918 内网段）";
}

std::string connectionDetails(const Target& target, std::uint16_t localPort) {
    const auto host = target.connectHost.empty() ? target.address : target.connectHost;
    const auto port = target.connectPort ? target.connectPort : target.port;
    return "Ghost User 连接信息\r\n服务器：" + target.serverName +
        "\r\n连接 host（TS3 getServerConnectInfo）：" + endpoint(host, port) +
        "\r\n传输 IP（TS3 CONNECTION_SERVER_IP）：" + endpoint(target.address, target.port) +
        "\r\n地址类型：" + addressDescription(target.address) +
        "\r\n本地转发：" + (localPort ? endpoint("127.0.0.1", localPort) : "当前无活动转发") +
        "\r\n恢复连接：" + endpoint(host, port) +
        "\r\n服务器 UID：" + target.serverUid +
        "\r\n身份 UID：" + target.identityUid +
        "\r\n频道 ID：" + std::to_string(target.channelId) +
        "\r\n说明：传输 IP 由 TS3 当前连接提供；地址段分类不能单独证明代理或路由来源。\r\n";
}
}
