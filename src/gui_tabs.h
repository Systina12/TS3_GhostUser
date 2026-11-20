#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ghost {
enum class TabCleanup { missing, kept, closed };

class GuiTabActions {
public:
    using Guard = std::function<bool()>;
    virtual ~GuiTabActions() = default;
    virtual bool closeCurrent(std::uint64_t token, const Guard& guard) = 0;
    virtual TabCleanup closeInactive(std::uint64_t token, const Guard& guard) = 0;
    virtual std::string description() const = 0;
};

std::unique_ptr<GuiTabActions> platformGuiTabs();
}
