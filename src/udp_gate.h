#pragma once

#include "controller.h"

namespace ghost {
class SocketRuntime {
public:
    SocketRuntime();
    ~SocketRuntime();
    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};

class UdpGateFactory final : public GateFactory {
public:
    std::unique_ptr<Gate> create(const Target& target) override;
};
} // namespace ghost
