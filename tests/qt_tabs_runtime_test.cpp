// Exercise the runtime bridge against the Qt DLLs shipped by official TS3.
// This test creates its own widgets; it never opens a server connection.
#include "../src/gui_tabs.cpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Fn> Fn symbol(HMODULE module, const char* name) {
    auto* value = GetProcAddress(module, name);
    if (!value) throw std::runtime_error(std::string("Missing test Qt export: ") + name);
    return reinterpret_cast<Fn>(value);
}
struct alignas(16) Storage { unsigned char bytes[4096]{}; };
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Usage: qt_tabs_runtime_test <official TS3 runtime directory>");
        const std::filesystem::path directory(argv[1]);
        SetDllDirectoryW(directory.c_str());
        SetEnvironmentVariableW(L"QT_QPA_PLATFORM_PLUGIN_PATH", (directory / L"platforms").c_str());
        require(LoadLibraryW((directory / L"Qt5Core.dll").c_str()) != nullptr, "Load Qt5Core");
        require(LoadLibraryW((directory / L"Qt5Gui.dll").c_str()) != nullptr, "Load Qt5Gui");
        require(LoadLibraryW((directory / L"Qt5Widgets.dll").c_str()) != nullptr, "Load Qt5Widgets");
        const auto core = GetModuleHandleW(L"Qt5Core.dll"), widgets = GetModuleHandleW(L"Qt5Widgets.dll");
        ghost::QtApi api;
        require(api.load(), "All production bridge symbols must resolve");
        std::cout << "Qt runtime " << api.version() << std::endl;
        const auto applicationConstruct = symbol<void (*)(void*, int*, char**, int)>(widgets, "??0QApplication@@QEAA@AEAHPEAPEADH@Z");
        const auto applicationDestroy = symbol<void (*)(void*)>(widgets, "??1QApplication@@UEAA@XZ");
        const auto widgetConstruct = symbol<void (*)(void*, void*, unsigned)>(widgets, "??0QWidget@@QEAA@PEAV0@V?$QFlags@W4WindowType@Qt@@@@@Z");
        const auto widgetDestroy = symbol<void (*)(void*)>(widgets, "??1QWidget@@UEAA@XZ");
        const auto barConstruct = symbol<void (*)(void*, void*)>(widgets, "??0QTabBar@@QEAA@PEAVQWidget@@@Z");
        const auto barDestroy = symbol<void (*)(void*)>(widgets, "??1QTabBar@@UEAA@XZ");
        const auto addTab = symbol<int (*)(void*, const ghost::QtString*)>(widgets, "?addTab@QTabBar@@QEAAHAEBVQString@@@Z");
        const auto closable = symbol<void (*)(void*, bool)>(widgets, "?setTabsClosable@QTabBar@@QEAAX_N@Z");
        const auto selectTab = symbol<void (*)(void*, int)>(widgets, "?setCurrentIndex@QTabBar@@QEAAXH@Z");
        const auto show = symbol<void (*)(void*)>(widgets, "?show@QWidget@@QEAAXXZ");
        const auto setName = symbol<void (*)(void*, const ghost::QtString*)>(core, "?setObjectName@QObject@@QEAAXAEBVQString@@@Z");
        const auto connect = symbol<void (*)(void*, const void*, const char*, const void*, const char*, int)>(core,
            "?connect@QObject@@SA?AVConnection@QMetaObject@@PEBV1@PEBD01W4ConnectionType@Qt@@@Z");
        const auto connectionDestroy = symbol<void (*)(void*)>(core, "??1Connection@QMetaObject@@QEAA@XZ");
        unsigned major = 0, minor = 0, patch = 0;
        std::sscanf(api.version(), "%u.%u.%u", &major, &minor, &patch);
        Storage application, root, bar;
        int appArgc = 1; char appName[] = "qt_tabs_runtime_test"; char* appArgv[]{appName, nullptr};
        applicationConstruct(&application, &appArgc, appArgv, static_cast<int>((major << 16) | (minor << 8) | patch));
        widgetConstruct(&root, nullptr, 0);
        barConstruct(&bar, &root);
        closable(&bar, true);
        ghost::QtString text, name;
        api.stringConstruct(&text, L"Owned test tab", 14);
        api.stringConstruct(&name, L"ServerTabCloseButton", 20);
        require(addTab(&bar, &text) == 0, "create first test tab");
        auto* first = api.tabButton(&bar, 0, 1);
        require(first != nullptr, "Qt must create a close button");
        setName(first, &name);
        Storage connection;
        connect(&connection, first, "2clicked(bool)", &root, "1hide()", 1);
        connectionDestroy(&connection);
        show(&bar); show(&root);
        auto actions = ghost::platformGuiTabs();
        constexpr std::uint64_t token = 0x1234567812345678ULL;
        require(!actions->closeCurrent(token, [] { return false; }), "reject a failed ownership guard");
        require(api.visible(&root), "failed ownership guard must not activate the close button");
        require(actions->closeCurrent(token, [] { return true; }), "invoke owned Qt close button");
        require(!api.visible(&root), "close button signal must reach its receiver");
        require(api.token(first) == token, "QVariant token must round-trip exactly");
        show(&root);
        require(addTab(&bar, &text) == 1, "create second test tab");
        setName(api.tabButton(&bar, 1, 1), &name);
        selectTab(&bar, 1);
        connect(&connection, &bar, "2tabCloseRequested(int)", &root, "1hide()", 1);
        connectionDestroy(&connection);
        require(actions->closeInactive(token, [] { return false; }) == ghost::TabCleanup::kept,
            "inactive tab cleanup must honor ownership guard");
        require(api.visible(&root), "guarded cleanup must leave the window alone");
        require(actions->closeInactive(token, [] { return true; }) == ghost::TabCleanup::closed,
            "close the marked inactive test tab");
        require(!api.visible(&root), "typed int signal must reach its receiver");
        require(actions->closeInactive(token + 1, [] { return true; }) == ghost::TabCleanup::missing,
            "unrelated tabs must not match another token");
        api.stringDestroy(&name); api.stringDestroy(&text);
        barDestroy(&bar); widgetDestroy(&root); applicationDestroy(&application);
        std::cout << "PASS vendor Qt bridge, ownership guards, token lifetime and typed queued/direct calls\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << std::endl;
        return 1;
    }
}
