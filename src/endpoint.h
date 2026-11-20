#pragma once
#include "controller.h"

namespace ghost {
std::string endpoint(const std::string& host, std::uint16_t port);
std::string addressDescription(const std::string& address);
std::string connectionDetails(const Target& target, std::uint16_t localPort);
}
