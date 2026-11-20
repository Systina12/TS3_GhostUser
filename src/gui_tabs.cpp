#include "gui_tabs.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <vector>

#ifdef _WIN32
#include <windows.h>

namespace ghost {
namespace {
// Qt 5 x64 ABI types used only as references/opaque return storage. All objects
// are constructed, read and destroyed by the Qt DLLs already loaded by TS3.
// QGenericArgument's public layout is data, then type name (qobjectdefs.h).
struct QtArgument { const void* data = nullptr; const char* name = nullptr; };
struct alignas(8) QtString { std::array<unsigned char, 16> bytes{}; };
struct alignas(8) QtVariant { std::array<unsigned char, 32> bytes{}; };

struct QtApi {
    using Invoke = bool (*)(void*, const char*, int, QtArgument, QtArgument, QtArgument,
        QtArgument, QtArgument, QtArgument, QtArgument, QtArgument, QtArgument, QtArgument, QtArgument);
    const char* (*version)() = nullptr;
    void (*stringConstruct)(QtString*, const wchar_t*, int) = nullptr;
    void (*stringDestroy)(QtString*) = nullptr;
    int (*stringSize)(const QtString*) = nullptr;
    const wchar_t* (*stringUtf16)(const QtString*) = nullptr;
    void (*translate)(QtString*, const char*, const char*, const char*, int) = nullptr;
    void* (*findWidget)(std::uint64_t) = nullptr;
    void* (*findChild)(const void*, const QtString*, const void*, int) = nullptr;
    bool (*inherits)(const void*, const char*) = nullptr;
    void* (*parent)(const void*) = nullptr;
    bool (*visible)(const void*) = nullptr;
    bool (*enabled)(const void*) = nullptr;
    int (*currentIndex)(const void*) = nullptr;
    int (*tabCount)(const void*) = nullptr;
    void* (*tabButton)(const void*, int, int) = nullptr;
    void (*variantConstruct)(QtVariant*, std::uint64_t) = nullptr;
    void (*variantDestroy)(QtVariant*) = nullptr;
    std::uint64_t (*variantNumber)(const QtVariant*, bool*) = nullptr;
    bool (*setProperty)(void*, const char*, const QtVariant*) = nullptr;
    void (*property)(const void*, QtVariant*, const char*) = nullptr;
    void* (*dialogButton)(const void*, int) = nullptr;
    Invoke invoke = nullptr;
    const void* objectMeta = nullptr;
    std::string failure;

    template<class Fn> bool bind(Fn& fn, HMODULE module, const char* name) {
        fn = reinterpret_cast<Fn>(GetProcAddress(module, name));
        return fn != nullptr;
    }
    bool load() {
        const auto core = GetModuleHandleW(L"Qt5Core.dll");
        const auto widgets = GetModuleHandleW(L"Qt5Widgets.dll");
        failure = "未找到受支持的 Qt 5.15 x64 界面，使用兼容重连";
        if (!core || !widgets || sizeof(void*) != 8 || !bind(version, core, "qVersion") ||
            std::strncmp(version(), "5.15.", 5) != 0) return false;
        const bool valid =
            bind(stringConstruct, core, "??0QString@@QEAA@PEBVQChar@@H@Z") &&
            bind(stringDestroy, core, "??1QString@@QEAA@XZ") &&
            bind(stringSize, core, "?size@QString@@QEBAHXZ") &&
            bind(stringUtf16, core, "?utf16@QString@@QEBAPEBGXZ") &&
            bind(translate, core, "?translate@QCoreApplication@@SA?AVQString@@PEBD00H@Z") &&
            bind(findWidget, widgets, "?find@QWidget@@SAPEAV1@_K@Z") &&
            bind(findChild, core, "?qt_qFindChild_helper@@YAPEAVQObject@@PEBV1@AEBVQString@@AEBUQMetaObject@@V?$QFlags@W4FindChildOption@Qt@@@@@Z") &&
            bind(inherits, core, "?inherits@QObject@@QEBA_NPEBD@Z") &&
            bind(parent, core, "?parent@QObject@@QEBAPEAV1@XZ") &&
            bind(visible, widgets, "?isVisible@QWidget@@QEBA_NXZ") &&
            bind(enabled, widgets, "?isEnabled@QWidget@@QEBA_NXZ") &&
            bind(currentIndex, widgets, "?currentIndex@QTabBar@@QEBAHXZ") &&
            bind(tabCount, widgets, "?count@QTabBar@@QEBAHXZ") &&
            bind(tabButton, widgets, "?tabButton@QTabBar@@QEBAPEAVQWidget@@HW4ButtonPosition@1@@Z") &&
            bind(variantConstruct, core, "??0QVariant@@QEAA@_K@Z") &&
            bind(variantDestroy, core, "??1QVariant@@QEAA@XZ") &&
            bind(variantNumber, core, "?toULongLong@QVariant@@QEBA_KPEA_N@Z") &&
            bind(setProperty, core, "?setProperty@QObject@@QEAA_NPEBDAEBVQVariant@@@Z") &&
            bind(property, core, "?property@QObject@@QEBA?AVQVariant@@PEBD@Z") &&
            bind(dialogButton, widgets, "?button@QDialogButtonBox@@QEBAPEAVQPushButton@@W4StandardButton@1@@Z") &&
            bind(invoke, core, "?invokeMethod@QMetaObject@@SA_NPEAVQObject@@PEBDW4ConnectionType@Qt@@VQGenericReturnArgument@@VQGenericArgument@@444444444@Z");
        objectMeta = reinterpret_cast<const void*>(GetProcAddress(core, "?staticMetaObject@QObject@@2UQMetaObject@@B"));
        if (!valid || !objectMeta) return false;
        failure.clear();
        return true;
    }
    void* child(void* object, const wchar_t* name) {
        QtString text;
        stringConstruct(&text, name, static_cast<int>(std::wcslen(name)));
        auto* result = findChild(object, &text, objectMeta, 1); // FindChildrenRecursively
        stringDestroy(&text);
        return result;
    }
    bool call(void* object, const char* method, int connection = 1, QtArgument argument = {}) {
        return invoke(object, method, connection, {}, argument, {}, {}, {}, {}, {}, {}, {}, {}, {});
    }
    void mark(void* button, std::uint64_t token) {
        QtVariant value;
        variantConstruct(&value, token);
        setProperty(button, "ts3GhostUserRetiringTab", &value);
        variantDestroy(&value);
    }
    std::uint64_t token(void* button) {
        QtVariant value;
        property(button, &value, "ts3GhostUserRetiringTab");
        bool valid = false;
        const auto result = variantNumber(&value, &valid);
        variantDestroy(&value);
        return valid ? result : 0;
    }
    std::wstring translated(const char* context, const char* source) {
        QtString value;
        translate(&value, context, source, nullptr, -1);
        const std::wstring text(stringUtf16(&value), static_cast<std::size_t>(stringSize(&value)));
        stringDestroy(&value);
        return text;
    }
};

struct ServerBar { HWND window = nullptr; void* bar = nullptr; };
std::vector<ServerBar> serverBars(QtApi& api) {
    struct Scan { QtApi& api; std::vector<ServerBar> results; } scan{api, {}};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
        auto& scan = *reinterpret_cast<Scan*>(data);
        if (GetWindow(window, GW_OWNER) || !IsWindowVisible(window)) return TRUE;
        auto* widget = scan.api.findWidget(reinterpret_cast<std::uint64_t>(window));
        if (!widget) return TRUE;
        auto* button = scan.api.child(widget, L"ServerTabCloseButton");
        if (!button) return TRUE;
        auto* bar = scan.api.parent(button);
        if (bar && scan.api.inherits(bar, "QTabBar")) scan.results.push_back({window, bar});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&scan));
    return scan.results;
}

struct Confirmation {
    QtApi& api;
    const GuiTabActions::Guard& guard;
    HWND owner;
    std::vector<std::wstring> titles;
    HWND handled = nullptr;
    bool cancelled = false;
};
thread_local Confirmation* activeConfirmation = nullptr;
void CALLBACK confirmClose(HWND, UINT, UINT_PTR, DWORD) {
    auto* attempt = activeConfirmation;
    if (!attempt) return;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
        auto& attempt = *reinterpret_cast<Confirmation*>(data);
        if (window == attempt.handled || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != attempt.owner)
            return TRUE;
        auto* dialog = attempt.api.findWidget(reinterpret_cast<std::uint64_t>(window));
        if (!dialog || (!attempt.api.inherits(dialog, "AreYouSureDialog") && !attempt.api.inherits(dialog, "QMessageBox")))
            return TRUE;
        auto* box = attempt.api.child(dialog, L"qt_msgbox_buttonbox");
        if (!box || !attempt.api.inherits(box, "QDialogButtonBox")) return TRUE;
        auto* yes = attempt.api.dialogButton(box, 0x4000);
        auto* no = attempt.api.dialogButton(box, 0x10000);
        if (!yes || !no) return TRUE;
        wchar_t title[512]{};
        GetWindowTextW(window, title, 512);
        const bool recognized = std::find(attempt.titles.begin(), attempt.titles.end(), title) != attempt.titles.end();
        const bool accept = recognized && attempt.guard();
        attempt.cancelled = !accept;
        // Invoke the standard Yes/No button without changing "Do not show again".
        attempt.api.call(accept ? yes : no, "click", 2); // QueuedConnection
        attempt.handled = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(attempt));
}

class QtGuiTabs final : public GuiTabActions {
public:
    bool closeCurrent(std::uint64_t token, const Guard& guard) override {
        if (!api_.load()) { reason_ = api_.failure; return false; }
        for (const auto& found : serverBars(api_)) {
            const int index = api_.currentIndex(found.bar);
            if (index < 0 || !guard()) continue;
            auto* button = api_.tabButton(found.bar, index, 1); // RightSide
            if (!button) button = api_.tabButton(found.bar, index, 0);
            if (!button || !api_.inherits(button, "QAbstractButton") || !api_.visible(button) || !api_.enabled(button)) continue;
            api_.mark(button, token);
            if (api_.token(button) != token) continue;
            const bool result = close(found.window, guard, [&] { return api_.call(button, "click"); });
            if (result) reason_ = "快速重连：旧测试标签页已请求关闭，新连接使用新标签页";
            return result;
        }
        reason_ = "未找到可确认的测试标签页关闭按钮，使用兼容重连";
        return false;
    }
    TabCleanup closeInactive(std::uint64_t token, const Guard& guard) override {
        if (!api_.load()) return TabCleanup::kept;
        for (const auto& found : serverBars(api_)) {
            const int count = api_.tabCount(found.bar);
            for (int index = 0; index < count; ++index) {
                auto* button = api_.tabButton(found.bar, index, 1);
                if (!button) button = api_.tabButton(found.bar, index, 0);
                if (!button || api_.token(button) != token) continue;
                if (index == api_.currentIndex(found.bar) || !guard()) return TabCleanup::kept;
                const bool result = close(found.window, guard, [&] {
                    return api_.call(found.bar, "tabCloseRequested", 1, {&index, "int"});
                });
                return result ? TabCleanup::closed : TabCleanup::kept;
            }
        }
        return TabCleanup::missing;
    }
    std::string description() const override { return reason_; }

private:
    bool close(HWND owner, const Guard& guard, const std::function<bool()>& action) {
        if (activeConfirmation || !guard()) return false;
        Confirmation attempt{api_, guard, owner, {L"Disconnect from Server", L"Close Connected Server Tab"}};
        for (const auto* context : {"MainWindow", "AreYouSureDialog", "ServerHandler", "ServerTabWidget", "ImprovedTabBar", "QObject"})
            for (const auto* title : {"Disconnect from Server", "Close Connected Server Tab"})
                attempt.titles.push_back(api_.translated(context, title));
        const auto timer = SetTimer(nullptr, 0, 10, confirmClose);
        if (!timer) { reason_ = "无法启动标签页关闭确认，使用兼容重连"; return false; }
        activeConfirmation = &attempt;
        const bool invoked = action();
        activeConfirmation = nullptr;
        KillTimer(nullptr, timer);
        if (attempt.cancelled) reason_ = "关闭确认未匹配或测试标签页已变化，使用兼容重连";
        return invoked && !attempt.cancelled;
    }
    QtApi api_;
    std::string reason_ = "快速重连尚未使用";
};
}
std::unique_ptr<GuiTabActions> platformGuiTabs() { return std::make_unique<QtGuiTabs>(); }
}
#else
namespace ghost {
namespace {
class UnsupportedGuiTabs final : public GuiTabActions {
public:
    bool closeCurrent(std::uint64_t, const Guard&) override { return false; }
    TabCleanup closeInactive(std::uint64_t, const Guard&) override { return TabCleanup::kept; }
    std::string description() const override { return "此平台未启用快速标签页重建"; }
};
}
std::unique_ptr<GuiTabActions> platformGuiTabs() { return std::make_unique<UnsupportedGuiTabs>(); }
}
#endif
