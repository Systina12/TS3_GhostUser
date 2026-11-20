#include "plugin_api.h"
#include "teamspeak/public_errors.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Module {
public:
    explicit Module(const std::filesystem::path& path) {
#ifdef _WIN32
        handle_ = LoadLibraryW(path.c_str());
        if (!handle_)
            throw std::runtime_error("LoadLibraryW failed: " + std::to_string(GetLastError()));
#else
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) throw std::runtime_error(dlerror());
#endif
    }
    ~Module() {
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    template <typename Fn> Fn symbol(const char* name) const {
#ifdef _WIN32
        const auto address = GetProcAddress(handle_, name);
#else
        const auto address = dlsym(handle_, name);
#endif
        if (!address) throw std::runtime_error(std::string("Missing export: ") + name);
        return reinterpret_cast<Fn>(address);
    }

private:
#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
};

int unexpectedCalls = 0;

// The simulated host represents a disconnected client. No server APIs are used.
template <typename... Args>
unsigned int unexpected(Args...) {
    ++unexpectedCalls;
    return ERROR_not_connected;
}

TS3Functions disconnectedHost() {
    TS3Functions host{};
    host.getCurrentServerConnectionHandlerID = []() -> uint64 { return 1; };
    host.getConnectionStatus = [](uint64, int* status) -> unsigned int {
        *status = STATUS_DISCONNECTED;
        return ERROR_ok;
    };
    host.freeMemory = [](void* memory) -> unsigned int { std::free(memory); return ERROR_ok; };
    host.getClientID = unexpected;
    host.getClientSelfVariableAsString = unexpected;
    host.getConnectionVariableAsString = unexpected;
    host.getConnectionVariableAsUInt64 = unexpected;
    host.getServerVariableAsString = unexpected;
    host.getChannelOfClient = unexpected;
    host.getServerConnectInfo = unexpected;
    host.getChannelConnectInfo = unexpected;
    host.getChannelVariableAsString = unexpected;
    host.getParentChannelOfChannel = unexpected;
    host.guiConnect = unexpected;
    host.stopConnection = unexpected;
    host.setClientSelfVariableAsInt = unexpected;
    host.flushClientSelfUpdates = unexpected;
    host.requestClientMove = unexpected;
    return host;
}

#ifdef _WIN32
struct Windows {
    HWND first = nullptr;
    int count = 0;
};

BOOL CALLBACK countPluginWindow(HWND hwnd, LPARAM context) {
    wchar_t title[128]{};
    GetWindowTextW(hwnd, title, 128);
    if (std::wstring_view(title).find(L"Ghost User") == 0) {
        auto& windows = *reinterpret_cast<Windows*>(context);
        windows.first = hwnd;
        ++windows.count;
    }
    return TRUE;
}

Windows pluginWindows() {
    Windows result;
    EnumThreadWindows(GetCurrentThreadId(), countPluginWindow, reinterpret_cast<LPARAM>(&result));
    return result;
}
#endif

struct Menu {
    decltype(&ts3plugin_freeMemory) release;
    PluginMenuItem** items = nullptr;
    char* icon = nullptr;
    ~Menu() {
        if (items) {
            for (auto item = items; *item; ++item) release(*item);
            release(items);
        }
        if (icon) release(icon);
    }
};

void testPlugin(const Module& module, std::string_view expectedVersion) {
#define LOAD(function) module.symbol<decltype(&ts3plugin_##function)>("ts3plugin_" #function)
    const auto name = LOAD(name);
    const auto version = LOAD(version);
    const auto apiVersion = LOAD(apiVersion);
    const auto author = LOAD(author);
    const auto description = LOAD(description);
    const auto setFunctions = LOAD(setFunctionPointers);
    const auto init = LOAD(init);
    const auto shutdown = LOAD(shutdown);
    const auto registerId = LOAD(registerPluginID);
    const auto initMenus = LOAD(initMenus);
    const auto release = LOAD(freeMemory);
    const auto menuEvent = LOAD(onMenuItemEvent);
#undef LOAD

    require(name() && *name() && author() && *author() && description() && *description(),
            "Required plugin metadata is empty");
    require(version() && version() == expectedVersion, "Unexpected DLL version for this build");
    require(apiVersion() == 26, "Unexpected TeamSpeak plugin API version");
    require(init() == 1, "Plugin accepted a missing host API table");
    setFunctions(disconnectedHost());

    // Run two load/disable cycles to catch stale plugin IDs and surviving windows.
    for (int cycle = 0; cycle < 2; ++cycle) {
        require(init() == 0, "Initialization failed for a valid offline host");
        try {
            Menu beforeRegistration{release};
            initMenus(&beforeRegistration.items, &beforeRegistration.icon);
            require(!beforeRegistration.items && !beforeRegistration.icon,
                    "Menu registration state leaked into a new load");

            char id[] = "offline-menu-test";
            registerId(id);
            id[0] = '\0';
            Menu menu{release};
            initMenus(&menu.items, &menu.icon);
            require(menu.items && menu.items[0] && !menu.items[1], "Expected one menu entry and a null terminator");
            const auto& item = *menu.items[0];
            require(item.type == PLUGIN_MENU_TYPE_GLOBAL && item.id == 1, "Expected a global menu item with ID 1");
            require(std::string_view(item.text) == "Ghost User GUI", "Wrong menu label");

#ifdef _WIN32
            menuEvent(1, PLUGIN_MENU_TYPE_CLIENT, item.id, 0);
            require(pluginWindows().count == 0, "A client menu event opened the global window");
            menuEvent(1, item.type, item.id, 0);
            const auto open = pluginWindows();
            require(open.count == 1 && IsWindowVisible(open.first), "Menu did not open a visible window");
            SendMessageW(open.first, WM_CLOSE, 0, 0);
            require(!IsWindowVisible(open.first), "Window remained visible after close");
            menuEvent(1, item.type, item.id, 0);
            const auto reopened = pluginWindows();
            require(reopened.count == 1 && IsWindowVisible(reopened.first), "Reopen did not show exactly one window");
#else
            (void)menuEvent;
#endif
        } catch (...) {
            shutdown();
            throw;
        }
        shutdown();
#ifdef _WIN32
        require(pluginWindows().count == 0, "Plugin left a window behind after shutdown");
#endif
        require(unexpectedCalls == 0, "Menu or window lifecycle called a server API");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "Usage: plugin_menu_smoke <plugin> <expected-version>");
        const Module plugin(std::filesystem::absolute(argv[1]));
        testPlugin(plugin, argv[2]);
        std::cout << "PASS offline plugin load, registration, menu, and reload\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
