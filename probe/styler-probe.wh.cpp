// ==WindhawkMod==
// @id              styler-probe
// @name            Styler conflict probe (temporary)
// @description     Finds out which part of a mod stops the Notification Center Styler theming the Control Center. Diagnostic only.
// @version         0.1
// @author          bardelyne
// @github          https://github.com/bardelyne
// @include         ShellHost.exe
// @architecture    x86-64
// @license         GPL-3.0
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lwbemuuid -ldxva2
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Styler conflict probe

Temporary diagnostic. The per-monitor brightness mod stops the Notification
Center Styler theming the Control Center, and that mod does too many things at
once to say which of them is responsible. This does one thing at a time.

Every capability is a setting, all off by default, so a run can turn on
exactly one and change nothing else.

It also removes the need to judge the answer by eye. The styler's theme is
translucent, so the flyout takes the colour of whatever is behind it -- which
is what made an earlier round of screenshot comparisons worthless. Instead
this reads the styler's effect straight out of the visual tree: the
`Background` brush and `CornerRadius` of `Border#RootGridBorder`, which is
what the theme changes. Those are facts, not impressions.

Output: `%TEMP%\styler-probe.log`.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- startThread: false
  $name: 1. Start a worker thread
  $description: A thread that does nothing at all, to test whether the mere presence of one matters.
- initCom: false
  $name: 2. Initialise COM on that thread
  $description: CoInitializeEx(COINIT_MULTITHREADED), which creates the process-wide MTA. Needs the thread.
- connectWmi: false
  $name: 3. Connect to WMI
  $description: The same WbemLocator/ConnectServer/CoSetProxyBlanket the brightness engine does. Needs COM.
- probeDdc: false
  $name: 4. Probe monitors over DDC/CI
  $description: Enumerate monitors and read VCP 0x10, as the brightness engine does at startup.
- taplessMode: false
  $name: 6. Find the Control Center without a TAP
  $description: >-
    Skip XAML diagnostics entirely. Watch for the Control Center window with
    an in-context WinEvent hook, which runs the callback on the thread that
    raised the event -- the XAML thread -- and walk the tree from
    Window::Current().Content() with the public VisualTreeHelper API. This is
    what explorer-command-bar does, and why it says it does not use the TAP.
- dumpSymbols: false
  $name: 7. List what can be hooked in ControlCenter.dll
  $description: >-
    Window::Current() is null on the Control Center thread because the flyout
    is a XAML island, so there is no ambient root to walk from. Enumerate the
    host DLL instead and log the functions that name the view, which is where
    an element can be taken from without diagnostics.
- injectElement: false
  $name: 5. Add an element to the Control Center
  $description: Appends one TextBlock to the sliders area, to test whether modifying the tree is what matters.
*/
// ==/WindhawkModSettings==

#include <initguid.h>  // must precede xamlom.h

#include <inspectable.h>
#include <xamlom.h>

#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#pragma pop_macro("GetCurrentTime")

#include <comdef.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <wbemidl.h>
#include <windhawk_utils.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;

static HRESULT InjectWindhawkTAP() noexcept;

namespace {

void Rec(const wchar_t* fmt, ...) {
    wchar_t body[1024] = {};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(body, ARRAYSIZE(body), _TRUNCATE, fmt, args);
    va_end(args);

    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n > MAX_PATH - 32) {
        return;
    }
    wcscat_s(path, MAX_PATH, L"styler-probe.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t kCrLf[] = {13, 10, 0};
    wchar_t line[1200];
    // The pid matters: several ShellHost processes are injected at once, and
    // without it a quiet hook cannot be told from a hook in the other one.
    int len = wsprintfW(line, L"%02d:%02d:%02d.%03d [%lu] %ls%ls", st.wHour,
                        st.wMinute, st.wSecond, st.wMilliseconds,
                        GetCurrentProcessId(), body, kCrLf);
    DWORD written = 0;
    WriteFile(h, line, static_cast<DWORD>(len * sizeof(wchar_t)), &written,
              nullptr);
    CloseHandle(h);
}

struct Settings {
    bool startThread = false;
    bool initCom = false;
    bool connectWmi = false;
    bool probeDdc = false;
    bool injectElement = false;
    bool taplessMode = false;
    bool dumpSymbols = false;
};
Settings g_settings;

void LoadSettings() {
    g_settings.startThread = Wh_GetIntSetting(L"startThread") != 0;
    g_settings.initCom = Wh_GetIntSetting(L"initCom") != 0;
    g_settings.connectWmi = Wh_GetIntSetting(L"connectWmi") != 0;
    g_settings.probeDdc = Wh_GetIntSetting(L"probeDdc") != 0;
    g_settings.injectElement = Wh_GetIntSetting(L"injectElement") != 0;
    g_settings.taplessMode = Wh_GetIntSetting(L"taplessMode") != 0;
    g_settings.dumpSymbols = Wh_GetIntSetting(L"dumpSymbols") != 0;
    Rec(L"=== settings: thread=%d com=%d wmi=%d ddc=%d inject=%d tapless=%d ===",
        g_settings.startThread, g_settings.initCom, g_settings.connectWmi,
        g_settings.probeDdc, g_settings.injectElement,
        g_settings.taplessMode);
}

std::atomic<DWORD> g_xamlThreadId{0};
[[clang::no_destroy]] std::thread g_worker;
std::atomic<bool> g_quit{false};

HMODULE GetCurrentModuleHandle() {
    HMODULE module;
    if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           L"", &module)) {
        return nullptr;
    }
    return module;
}

std::wstring ElementLabel(wux::DependencyObject const& obj) {
    std::wstring label;
    try {
        label = winrt::get_class_name(obj);
    } catch (...) {
        label = L"<unknown>";
    }
    if (auto fe = obj.try_as<wux::FrameworkElement>()) {
        std::wstring name{fe.Name()};
        if (!name.empty()) {
            label += L"#" + name;
        }
    }
    return label;
}

wux::DependencyObject FindByName(wux::DependencyObject const& root,
                                 std::wstring_view name, int maxDepth) {
    if (maxDepth < 0) {
        return nullptr;
    }
    if (auto fe = root.try_as<wux::FrameworkElement>()) {
        if (std::wstring_view{fe.Name()} == name) {
            return root;
        }
    }
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        if (auto found =
                FindByName(wuxm::VisualTreeHelper::GetChild(root, i), name,
                           maxDepth - 1)) {
            return found;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The measurement
//
// What the styler's theme actually changes, read as data. Judging this from a
// screenshot does not work: the theme is translucent, so the flyout shows
// whatever is behind it and its apparent colour says nothing.
// ---------------------------------------------------------------------------

// A brush described by its values, not just its class.
//
// Logging only the type was useless: the theme keeps using an AcrylicBrush
// and only changes its tint, so a styled and an unstyled run produced byte
// identical output and looked like proof that nothing had happened.
std::wstring DescribeBrush(wuxm::Brush const& brush) {
    if (!brush) {
        return L"(null)";
    }
    std::wstring out = ElementLabel(brush.as<wux::DependencyObject>());
    wchar_t buf[128];
    if (auto solid = brush.try_as<wuxm::SolidColorBrush>()) {
        auto c = solid.Color();
        swprintf(buf, 128, L"(#%02X%02X%02X%02X op=%.2f)", c.A, c.R, c.G, c.B,
                 solid.Opacity());
        out += buf;
    } else if (auto acrylic = brush.try_as<wuxm::AcrylicBrush>()) {
        auto t = acrylic.TintColor();
        auto f = acrylic.FallbackColor();
        swprintf(buf, 128,
                 L"(tint=#%02X%02X%02X%02X to=%.2f fallback=#%02X%02X%02X%02X)",
                 t.A, t.R, t.G, t.B, acrylic.TintOpacity(), f.A, f.R, f.G, f.B);
        out += buf;
    }
    return out;
}

// Walks up to the top of the tree, then logs every element that carries a
// background brush or a corner radius. Aiming at one named element did not
// work -- RootGridBorder turned out to have neither, with the theme on or
// off. Dumping all of them and diffing a styler-on run against a styler-off
// run says where the theme actually lands, instead of guessing again.
void DumpPaintedElements(wux::DependencyObject const& node, int depth,
                         int maxDepth) {
    if (depth > maxDepth) {
        return;
    }
    std::wstring paint;
    if (auto panel = node.try_as<wuxc::Panel>()) {
        if (auto b = panel.Background()) {
            paint += L" bg=" + DescribeBrush(b);
        }
    }
    if (auto border = node.try_as<wuxc::Border>()) {
        if (auto b = border.Background()) {
            paint += L" bg=" + DescribeBrush(b);
        }
        auto r = border.CornerRadius();
        if (r.TopLeft != 0 || r.BottomRight != 0) {
            wchar_t buf[64];
            swprintf(buf, 64, L" radius=%.0f/%.0f", r.TopLeft, r.BottomRight);
            paint += buf;
        }
    }
    if (auto control = node.try_as<wuxc::Control>()) {
        if (auto b = control.Background()) {
            paint += L" ctlbg=" + DescribeBrush(b);
        }
    }
    if (!paint.empty()) {
        Rec(L"PAINT %2d %ls%ls", depth, ElementLabel(node).c_str(),
            paint.c_str());
    }
    int count = wuxm::VisualTreeHelper::GetChildrenCount(node);
    for (int i = 0; i < count; ++i) {
        DumpPaintedElements(wuxm::VisualTreeHelper::GetChild(node, i),
                            depth + 1, maxDepth);
    }
}

void ReportStylerEffect(wux::FrameworkElement const& view) {
    // Climb to the top so the whole flyout is covered, not just the view.
    wux::DependencyObject top = view;
    for (int i = 0; i < 12; i++) {
        auto parent = wuxm::VisualTreeHelper::GetParent(top);
        if (!parent) {
            break;
        }
        top = parent;
    }
    Rec(L"MEASURE begins at %ls", ElementLabel(top).c_str());
    DumpPaintedElements(top, 0, 10);
    Rec(L"MEASURE ends");
}

// ---------------------------------------------------------------------------
// Finding the Control Center without XAML diagnostics
//
// Only one diagnostics consumer can exist per process, so taking that
// connection is what stops the Notification Center Styler theming anything.
// This is the alternative explorer-command-bar documents: watch for the window
// and walk the tree with the public API.
//
// The WinEvent hook is installed WINEVENT_INCONTEXT and scoped to this
// process, so the callback runs on the very thread that raised the event --
// the XAML thread. That removes the need to marshal onto it at all.
// ---------------------------------------------------------------------------

HWINEVENTHOOK g_winEvent = nullptr;
std::atomic<bool> g_foundTapless{false};
// Whether Windhawk will arm the hook registrations for us, which it does
// exactly once, when Wh_ModInit returns.
std::atomic<bool> g_inModInit{false};

wux::DependencyObject FindByPartialType(wux::DependencyObject const& root,
                                        std::wstring_view fragment,
                                        int maxDepth) {
    if (maxDepth < 0) {
        return nullptr;
    }
    try {
        std::wstring_view name{winrt::get_class_name(root)};
        if (name.find(fragment) != std::wstring_view::npos) {
            return root;
        }
    } catch (...) {
    }
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        if (auto found = FindByPartialType(
                wuxm::VisualTreeHelper::GetChild(root, i), fragment,
                maxDepth - 1)) {
            return found;
        }
    }
    return nullptr;
}

void DumpTopLevels(wux::DependencyObject const& root, int depth, int maxDepth) {
    if (depth > maxDepth) {
        return;
    }
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        auto child = wuxm::VisualTreeHelper::GetChild(root, i);
        Rec(L"tapless:   %*s%ls", depth * 2, L"", ElementLabel(child).c_str());
        DumpTopLevels(child, depth + 1, maxDepth);
    }
}

wux::DependencyObject FindByType(wux::DependencyObject const& root,
                                 std::wstring_view type, int maxDepth) {
    if (maxDepth < 0) {
        return nullptr;
    }
    try {
        if (std::wstring_view{winrt::get_class_name(root)} == type) {
            return root;
        }
    } catch (...) {
    }
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        if (auto found = FindByType(wuxm::VisualTreeHelper::GetChild(root, i),
                                    type, maxDepth - 1)) {
            return found;
        }
    }
    return nullptr;
}

// The window filter that was here looked for a class name and then called
// Window::Current(). The hook fires -- 'ControlCenterWindow' on the XAML
// thread -- but Window::Current() is null there, and the window shown beside
// it says why: Windows.UI.Composition.DesktopWindowContentBridge. The Control
// Center is a XAML island. Islands have no CoreWindow, so there is no ambient
// Window to read a root from, and that is the real reason the mod reached for
// diagnostics in the first place.
//
// What explorer-command-bar actually does, on rereading it, is hook a function
// in the host's own DLL and take the element out of it. The equivalent here is
// ControlCenter.dll's
//
//   winrt::impl::produce<ControlCenterView, IFrameworkElementOverrides>
//       ::OnApplyTemplate()
//
// whose `this` is a live COM interface pointer on the view itself. From there
// XamlRoot() gives the island's root and VisualTreeHelper walks it, all public
// API and no diagnostics connection.

// Which of these actually fires is a question, not something to assume: the
// OnApplyTemplate override alone was hooked first and never ran, which says
// only that the template was applied elsewhere or earlier. So hook the view's
// whole lifecycle at once and let the log say which entry points are live.
//
// For the override, `this` is the produce struct -- a COM interface pointer on
// the view. For the private members it is the implementation object, whose
// first vtable is a producer's, so QueryInterface reaches the same object.
// Either way this goes through QueryInterface rather than assuming a layout,
// and a wrong guess returns an error instead of corrupting anything.

// Only ever call this with a pointer that really is a COM interface on the
// object -- the `this` of a winrt::impl::produce<> override. The `this` of an
// implementation member is not one, and QueryInterface through it faults.
wux::FrameworkElement ElementFromThis(void* pThis) {
    wux::FrameworkElement element{nullptr};
    if (!pThis) {
        return element;
    }
    try {
        winrt::copy_from_abi(element, pThis);
    } catch (...) {
        element = nullptr;
    }
    return element;
}

std::atomic<bool> g_slidersSeen{false};

void ReportView(const wchar_t* via, void* pThis) {
    try {
        auto view = ElementFromThis(pThis);
        if (!view) {
            Rec(L"tapless: %ls fired, but `this` is not a XAML element", via);
            return;
        }
        auto root = view.XamlRoot();
        Rec(L"tapless: FOUND via %ls -- %ls on thread %lu, %.0fx%.0f, "
            L"XamlRoot %ls, no TAP involved",
            via, winrt::get_class_name(view).c_str(), GetCurrentThreadId(),
            view.ActualWidth(), view.ActualHeight(), root ? L"yes" : L"no");

        // The view is found before it has been laid out -- 0x0, and no slider
        // under it yet, because the sliders are delay-loaded content. So the
        // walk has to happen later, but not from LayoutUpdated: walking the
        // tree inside a layout pass took ShellHost down with a fastfail
        // (0xc0000409). A DispatcherTimer runs on this same XAML thread
        // between passes, which is where reading the tree is allowed.
        auto timer = wux::DispatcherTimer();
        timer.Interval(std::chrono::milliseconds(300));
        timer.Tick([weak = winrt::make_weak(view), timer,
                    tries = std::make_shared<int>(0)](
                       wf::IInspectable const&, wf::IInspectable const&) {
            try {
                if (++*tries > 20 || g_slidersSeen.load()) {
                    timer.Stop();
                    return;
                }
                auto element = weak.get();
                if (!element) {
                    timer.Stop();
                    return;
                }
                auto slider = FindByPartialType(element, L"Slider", 24);
                if (!slider) {
                    if (*tries == 5) {
                        Rec(L"tapless: nothing named Slider yet; the view's "
                            L"first two levels are:");
                        DumpTopLevels(element, 0, 1);
                    }
                    return;
                }
                timer.Stop();
                if (g_slidersSeen.exchange(true)) {
                    return;
                }
                Rec(L"tapless: slider reached from the view after %d ticks: "
                    L"%ls (view now %.0fx%.0f) -- the whole path works with "
                    L"no TAP",
                    *tries, ElementLabel(slider).c_str(),
                    element.ActualWidth(), element.ActualHeight());
            } catch (...) {
                timer.Stop();
                Rec(L"tapless: tick threw %08X",
                    static_cast<unsigned>(winrt::to_hresult()));
            }
        });
        timer.Start();
    } catch (...) {
        Rec(L"tapless: %ls threw %08X", via,
            static_cast<unsigned>(winrt::to_hresult()));
    }
}

// A canary with nothing to do with the view class: it runs when the Control
// Center becomes visible. It stays in because it is what separated 'the hook
// is on the wrong function' from 'the hooks were never armed', and keeping
// that distinction available costs one log line.
using Telemetry_t = void(WINAPI*)(void* a, void* b);
Telemetry_t g_telemetryOriginal;
void WINAPI ControlCenterViewVisible_Hook(void* a, void* b) {
    Rec(L"tapless: CANARY ControlCenterViewVisible fired on thread %lu",
        GetCurrentThreadId());
    g_telemetryOriginal(a, b);
}

// OnApplyTemplate never fired, and that was not a race: ControlCenterView is
// compiled XAML with an InitializeComponent, so no ControlTemplate is ever
// applied to it and the override is dead code. What does run, once per x:Name
// in the markup, is IComponentConnector::Connect -- and a produce<> override
// really does get a COM interface on the view as its `this`, which is the one
// kind of pointer an element can safely be taken from.
// Connect did hand over the real element -- "got ControlCenter.ControlCenterView
// from Connect" -- so the technique is sound. What it could not survive is
// being touched that early. Connect runs inside InitializeComponent, and taking
// a reference there (copy_from_abi addrefs, the scope releases) destroyed the
// half-constructed view: ShellHost then faulted in no module at all, calling
// through what had been freed.
//
// OnGotFocus is the same kind of pointer -- a produce<> override's `this`, a
// COM interface on the view -- but it runs once the flyout is up and taking
// focus, when the object is fully alive and the tree is complete. Nothing has
// to be deferred, and nothing is touched during construction.
using OnGotFocus_t = int(WINAPI*)(void* pThis, void* args);
OnGotFocus_t g_onGotFocusOriginal;
int WINAPI ControlCenterView_OnGotFocus_Hook(void* pThis, void* args) {
    int ret = g_onGotFocusOriginal(pThis, args);
    if (!g_foundTapless.exchange(true)) {
        ReportView(L"OnGotFocus", pThis);
    }
    return ret;
}

using Ctor_t = void*(WINAPI*)(void* pThis);
Ctor_t g_ctorOriginal;
void* WINAPI ControlCenterView_Ctor_Hook(void* pThis) {
    void* ret = g_ctorOriginal(pThis);
    // Nothing of the element is usable yet -- this only says when the view is
    // built, which is the thing the hooks kept losing the race to.
    Rec(L"tapless: ControlCenterView constructed on thread %lu",
        GetCurrentThreadId());
    return ret;
}

void InstallTaplessHooks(HMODULE module) {
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {
            {LR"(public: static void __cdecl QuickActionsTelemetry::ControlCenterViewVisible<unsigned long,bool>(unsigned long &&,bool &&))"},
            &g_telemetryOriginal,
            ControlCenterViewVisible_Hook,
        },
        {
            {LR"(public: __cdecl winrt::ControlCenter::implementation::ControlCenterView::ControlCenterView(void))"},
            &g_ctorOriginal,
            ControlCenterView_Ctor_Hook,
        },
        {
            {LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::ControlCenter::implementation::ControlCenterView,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnGotFocus(void *))"},
            &g_onGotFocusOriginal,
            ControlCenterView_OnGotFocus_Hook,
        },
    };
    bool ok = WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks));
    // Wh_SetFunctionHook only registers; Windhawk applies the registrations
    // itself, but only once, when Wh_ModInit returns. Hooks registered from a
    // thread after that sit there unapplied -- which is why five of them,
    // including a canary that had nothing to do with the view class, stayed
    // silent in the very process that showed the flyout. Nothing was wrong
    // with the target or the technique; the hooks were never armed.
    bool applied = true;
    if (!g_inModInit.load()) {
        applied = Wh_ApplyHookOperations();
    }
    Rec(L"tapless: symbol hooks %ls%ls", ok ? L"installed" : L"FAILED",
        g_inModInit.load()  ? L" (armed by Wh_ModInit)"
        : applied           ? L" and armed"
                            : L" but arming FAILED");
}

// Hooking from a polling thread after Wh_ModAfterInit was too late: none of
// the three fired. ControlCenter.dll is already loaded before ShellHost has
// done anything visible, and when it is not, a 250ms poll still leaves a
// window in which the host can build the view and apply its template. So take
// the library at the moment it is mapped and hook before it runs.
std::atomic<bool> g_taplessHooked{false};

void InstallTaplessHooksOnce(HMODULE module) {
    if (g_taplessHooked.exchange(true)) {
        return;
    }
    InstallTaplessHooks(module);
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR path, HANDLE file, DWORD flags) {
    HMODULE module = LoadLibraryExW_Original(path, file, flags);
    // Log the loads themselves for a while. Silence from this hook is
    // ambiguous on its own -- it means either that ControlCenter.dll arrives
    // by some other path, or that nothing in this process is being hooked at
    // all, which would equally explain the symbol hooks never firing. One of
    // those is a wrong guess and the other is a broken tool, so distinguish
    // them before changing anything else.
    static std::atomic<int> loads{0};
    if (path && wcsstr(path, L"api-ms-win-") == nullptr &&
        loads.fetch_add(1) < 40) {
        Rec(L"tapless: LoadLibraryExW('%ls')", path);
    }
    if (module && !g_taplessHooked.load() && path) {
        if (const wchar_t* name = wcsrchr(path, L'\\')) {
            path = name + 1;
        }
        if (_wcsicmp(path, L"ControlCenter.dll") == 0) {
            Rec(L"tapless: ControlCenter.dll just mapped, hooking now");
            InstallTaplessHooksOnce(module);
        }
    }
    return module;
}

void StartTaplessWatch() {
    if (HMODULE module = GetModuleHandleW(L"ControlCenter.dll")) {
        Rec(L"tapless: ControlCenter.dll already loaded at init");
        InstallTaplessHooksOnce(module);
        return;
    }
    Rec(L"tapless: ControlCenter.dll not loaded yet, waiting for its load");
    WindhawkUtils::SetFunctionHook(LoadLibraryExW, LoadLibraryExW_Hook,
                                   &LoadLibraryExW_Original);
    // It can also be mapped without a LoadLibraryExW call, which is what
    // happened the first time this was tried, so poll too and take whichever
    // notices first.
    std::thread([] {
        for (int i = 0; i < 400 && !g_taplessHooked.load(); ++i) {
            if (HMODULE module = GetModuleHandleW(L"ControlCenter.dll")) {
                Rec(L"tapless: ControlCenter.dll appeared after %d ms", i * 25);
                InstallTaplessHooksOnce(module);
                return;
            }
            Sleep(25);
        }
    }).detach();
}

void StopTaplessWatch() {
    // Nothing to undo: Windhawk removes the symbol hook on unload.
}

// ---------------------------------------------------------------------------
// The capabilities under test, each independent
// ---------------------------------------------------------------------------

void DoWmi() {
    IWbemLocator* locator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator,
                                  reinterpret_cast<void**>(&locator));
    if (FAILED(hr) || !locator) {
        Rec(L"wmi: CoCreateInstance failed %08X", static_cast<unsigned>(hr));
        return;
    }
    IWbemServices* services = nullptr;
    hr = locator->ConnectServer(_bstr_t(L"root\\WMI"), nullptr, nullptr, nullptr,
                                0, nullptr, nullptr, &services);
    if (FAILED(hr) || !services) {
        Rec(L"wmi: ConnectServer failed %08X", static_cast<unsigned>(hr));
        locator->Release();
        return;
    }
    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);
    Rec(L"wmi: connected to root\\WMI");
    services->Release();
    locator->Release();
}

void DoDdc() {
    struct Ctx {
        int monitors = 0;
        int physical = 0;
        int answered = 0;
    } ctx;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);
            c->monitors++;
            DWORD count = 0;
            if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &count) || !count) {
                return TRUE;
            }
            std::vector<PHYSICAL_MONITOR> mons(count);
            if (!GetPhysicalMonitorsFromHMONITOR(mon, count, mons.data())) {
                return TRUE;
            }
            c->physical += static_cast<int>(count);
            for (auto& pm : mons) {
                DWORD current = 0, maximum = 0;
                if (GetVCPFeatureAndVCPFeatureReply(pm.hPhysicalMonitor, 0x10,
                                                    nullptr, &current,
                                                    &maximum)) {
                    c->answered++;
                }
            }
            DestroyPhysicalMonitors(count, mons.data());
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    Rec(L"ddc: %d monitor(s), %d physical, %d answered VCP 0x10", ctx.monitors,
        ctx.physical, ctx.answered);
}

void WorkerMain() {
    Rec(L"thread: started");
    HRESULT comHr = S_FALSE;
    if (g_settings.initCom) {
        comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Rec(L"com: CoInitializeEx -> %08X", static_cast<unsigned>(comHr));
        if (g_settings.connectWmi) {
            DoWmi();
        }
    } else if (g_settings.connectWmi) {
        Rec(L"wmi: skipped, needs COM");
    }

    if (g_settings.probeDdc) {
        DoDdc();
    }

    // Idle until unload, so the thread's mere existence is part of the test.
    while (!g_quit.load()) {
        Sleep(100);
    }
    if (SUCCEEDED(comHr)) {
        CoUninitialize();
    }
    Rec(L"thread: stopped");
}

}  // namespace

// ===========================================================================
// XAML diagnostics plumbing
// ===========================================================================

class VisualTreeWatcher
    : public winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2,
                               winrt::non_agile> {
   public:
    explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> site)
        : m_XamlDiagnostics(site.as<IXamlDiagnostics>()) {
        HANDLE thread = CreateThread(
            nullptr, 0,
            [](LPVOID param) -> DWORD {
                auto* watcher = reinterpret_cast<VisualTreeWatcher*>(param);
                auto service =
                    watcher->m_XamlDiagnostics.as<IVisualTreeService3>();
                HRESULT hr = service->AdviseVisualTreeChange(watcher);
                watcher->Release();
                if (FAILED(hr)) {
                    Rec(L"AdviseVisualTreeChange failed: %08X",
                        static_cast<unsigned>(hr));
                }
                return 0;
            },
            this, 0, nullptr);
        if (thread) {
            AddRef();
            CloseHandle(thread);
        }
    }

    VisualTreeWatcher(const VisualTreeWatcher&) = delete;
    VisualTreeWatcher& operator=(const VisualTreeWatcher&) = delete;

    void UnadviseVisualTreeChange() {
        m_XamlDiagnostics.as<IVisualTreeService3>()->UnadviseVisualTreeChange(
            this);
    }

   private:
    HRESULT STDMETHODCALLTYPE
    OnVisualTreeChange(ParentChildRelation, VisualElement element,
                       VisualMutationType mutationType) override try {
        if (mutationType != Add || !element.Type) {
            return S_OK;
        }
        if (wcscmp(element.Type, L"ControlCenter.ControlCenterView") != 0) {
            return S_OK;
        }
        g_xamlThreadId.store(GetCurrentThreadId());

        wf::IInspectable obj;
        winrt::check_hresult(m_XamlDiagnostics->GetIInspectableFromHandle(
            element.Handle,
            reinterpret_cast<::IInspectable**>(winrt::put_abi(obj))));
        auto view = obj.try_as<wux::FrameworkElement>();
        if (!view) {
            return S_OK;
        }
        Rec(L"ControlCenterView added");

        // Measure after a layout pass: the styler applies on its own tree
        // callback and the value read too early is the stock one.
        auto viewRef = view;
        auto token =
            std::make_shared<wux::FrameworkElement::LayoutUpdated_revoker>();
        auto passes = std::make_shared<int>(0);
        *token = view.LayoutUpdated(
            winrt::auto_revoke,
            [viewRef, token, passes](wf::IInspectable const&,
                                     wf::IInspectable const&) {
                if (++*passes < 3) {
                    return;  // let the styler get its turn first
                }
                try {
                    ReportStylerEffect(viewRef);
                    if (g_settings.injectElement) {
                        auto slidersObj =
                            FindByName(viewRef, L"SlidersGroup", 12);
                        if (auto sliders =
                                slidersObj
                                    ? slidersObj.try_as<wuxc::ContentControl>()
                                    : nullptr) {
                            Rec(L"inject: found SlidersGroup, leaving a mark");
                            sliders.Tag(winrt::box_value(L"styler-probe"));
                        } else {
                            Rec(L"inject: SlidersGroup not found");
                        }
                    }
                } catch (...) {
                    Rec(L"measure threw %08X",
                        static_cast<unsigned>(winrt::to_hresult()));
                }
                token->revoke();
            });
        return S_OK;
    } catch (...) {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle,
                                                    VisualElementState,
                                                    LPCWSTR) noexcept override {
        return S_OK;
    }

    winrt::com_ptr<IXamlDiagnostics> m_XamlDiagnostics = nullptr;
};

namespace {
[[clang::no_destroy]] winrt::com_ptr<VisualTreeWatcher> g_visualTreeWatcher;
}

// {8A6F55E4-B87E-4C33-B432-2E2DA11241D3}
//
// Its own, not the one every styler mod uses. Sharing a class id between two
// mods in one process is wrong regardless of whether it is what breaks this.
static constexpr CLSID CLSID_WindhawkTAP = {
    0x8a6f55e4,
    0xb87e,
    0x4c33,
    {0xb4, 0x32, 0x2e, 0x2d, 0xa1, 0x12, 0x41, 0xd3}};

class WindhawkTAP : public winrt::implements<WindhawkTAP, IObjectWithSite,
                                             winrt::non_agile> {
   public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) override try {
        if (g_visualTreeWatcher) {
            g_visualTreeWatcher->UnadviseVisualTreeChange();
            g_visualTreeWatcher = nullptr;
        }
        site.copy_from(pUnkSite);
        if (site) {
            FreeLibrary(GetCurrentModuleHandle());
            g_visualTreeWatcher = winrt::make_self<VisualTreeWatcher>(site);
        }
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid,
                                      void** ppvSite) noexcept override {
        return site.as(riid, ppvSite);
    }

   private:
    winrt::com_ptr<IUnknown> site;
};

template <class T>
struct SimpleFactory
    : winrt::implements<SimpleFactory<T>, IClassFactory, winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid,
                                             void** ppvObject) override try {
        if (pUnkOuter) {
            return CLASS_E_NOAGGREGATION;
        }
        *ppvObject = nullptr;
        return winrt::make<T>().as(riid, ppvObject);
    } catch (...) {
        return winrt::to_hresult();
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override {
        return S_OK;
    }
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"

__declspec(dllexport) _Use_decl_annotations_ STDAPI
    DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) try {
    if (rclsid == CLSID_WindhawkTAP) {
        *ppv = nullptr;
        return winrt::make<SimpleFactory<WindhawkTAP>>().as(riid, ppv);
    }
    return CLASS_E_CLASSNOTAVAILABLE;
} catch (...) {
    return winrt::to_hresult();
}

__declspec(dllexport) _Use_decl_annotations_ STDAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

#pragma clang diagnostic pop

using PFN_INITIALIZE_XAML_DIAGNOSTICS_EX =
    decltype(&InitializeXamlDiagnosticsEx);

static HRESULT InjectWindhawkTAP() noexcept {
    HMODULE module = GetCurrentModuleHandle();
    if (!module) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    WCHAR location[MAX_PATH];
    switch (GetModuleFileName(module, location, ARRAYSIZE(location))) {
        case 0:
        case ARRAYSIZE(location):
            return HRESULT_FROM_WIN32(GetLastError());
    }
    const HMODULE wuxDll = LoadLibraryEx(L"Windows.UI.Xaml.dll", nullptr,
                                         LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!wuxDll) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const auto ixde = reinterpret_cast<PFN_INITIALIZE_XAML_DIAGNOSTICS_EX>(
        GetProcAddress(wuxDll, "InitializeXamlDiagnosticsEx"));
    if (!ixde) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT hr = E_FAIL;
    for (int i = 0; i < 64; i++) {
        WCHAR connectionName[256];
        wsprintf(connectionName, L"VisualDiagConnection%d", i + 1);
        hr = ixde(connectionName, GetCurrentProcessId(), L"", location,
                  CLSID_WindhawkTAP, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }
    return hr;
}

// ===========================================================================
// Entry points
// ===========================================================================

namespace {
[[clang::no_destroy]] std::thread g_tapThread;
std::atomic<bool> g_tapQuit{false};

bool XamlWindowExists() {
    bool found = false;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId()) {
                return TRUE;
            }
            wchar_t cls[128] = {};
            GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
            if (wcsstr(cls, L"Windows.UI.Core.CoreWindow")) {
                *reinterpret_cast<bool*>(param) = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}
}  // namespace

// ---------------------------------------------------------------------------
// What can be hooked in ControlCenter.dll
//
// Window::Current() is null on the Control Center's thread because the flyout
// is a XAML island, not a CoreWindow -- confirmed by the window that appears
// next to it, Windows.UI.Composition.DesktopWindowContentBridge. So there is
// no ambient root to walk from, and the tree has to be entered from an element
// the host hands us. That is what explorer-command-bar does: hook a function in
// the host's own DLL, take the element from its arguments, and walk from
// element.XamlRoot().
//
// This lists the candidates rather than guessing at one.
// ---------------------------------------------------------------------------

void DumpControlCenterSymbols() {
    HMODULE module = nullptr;
    for (int i = 0; i < 120; ++i) {
        module = GetModuleHandleW(L"ControlCenter.dll");
        if (module) {
            break;
        }
        Sleep(500);
    }
    if (!module) {
        Rec(L"symbols: ControlCenter.dll never loaded");
        return;
    }
    Rec(L"symbols: ControlCenter.dll at %p, enumerating", module);

    // Every symbol, to a file of its own, in UTF-8 so ordinary tools can search
    // it. Filtering here would mean guessing the answer before seeing the
    // inventory, which is how the earlier round of this went wrong.
    wchar_t path[MAX_PATH] = {};
    GetTempPathW(ARRAYSIZE(path), path);
    wcsncat_s(path, L"cc-symbols.txt", _TRUNCATE);
    HANDLE out = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    WH_FIND_SYMBOL_OPTIONS options{};
    options.optionsSize = sizeof(options);
    WH_FIND_SYMBOL found{};
    HANDLE search = Wh_FindFirstSymbol(module, &options, &found);
    if (!search) {
        Rec(L"symbols: enumeration failed");
        if (out != INVALID_HANDLE_VALUE) {
            CloseHandle(out);
        }
        return;
    }
    int total = 0;
    int written = 0;
    do {
        ++total;
        if (!found.symbol || out == INVALID_HANDLE_VALUE) {
            continue;
        }
        std::wstring line{found.symbol};
        line.push_back(L'\n');
        int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                        static_cast<int>(line.size()), nullptr,
                                        0, nullptr, nullptr);
        if (bytes <= 0) {
            continue;
        }
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                            static_cast<int>(line.size()), utf8.data(), bytes,
                            nullptr, nullptr);
        DWORD wrote = 0;
        WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote,
                  nullptr);
        ++written;
    } while (Wh_FindNextSymbol(search, &found));
    Wh_FindCloseSymbol(search);
    if (out != INVALID_HANDLE_VALUE) {
        CloseHandle(out);
    }
    Rec(L"symbols: %d enumerated, %d written to %ls", total, written, path);
}

BOOL Wh_ModInit() {
    LoadSettings();
    if (g_settings.taplessMode) {
        // Before the host gets a chance to build the view.
        g_inModInit.store(true);
        StartTaplessWatch();
        g_inModInit.store(false);
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    Rec(L"=== probe attached, pid=%lu ===", GetCurrentProcessId());

    if (g_settings.startThread) {
        g_quit.store(false);
        g_worker = std::thread([] {
            try {
                WorkerMain();
            } catch (...) {
                Rec(L"worker threw");
            }
        });
    } else {
        Rec(L"thread: not started");
    }

    if (g_settings.dumpSymbols) {
        std::thread(DumpControlCenterSymbols).detach();
    }

    if (g_settings.taplessMode) {
        // The whole point: no InitializeXamlDiagnosticsEx call at all. The
        // hooks went in during Wh_ModInit.
        return;
    }

    g_tapQuit.store(false);
    g_tapThread = std::thread([] {
        for (int attempt = 0; attempt < 120 && !g_tapQuit.load(); attempt++) {
            if (XamlWindowExists()) {
                HRESULT hr = InjectWindhawkTAP();
                Rec(L"tap: attempt %d -> %08X", attempt + 1,
                    static_cast<unsigned>(hr));
                if (SUCCEEDED(hr)) {
                    return;
                }
            }
            Sleep(500);
        }
        Rec(L"tap: gave up");
    });
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    *bReload = TRUE;
    return TRUE;
}

void Wh_ModUninit() {
    StopTaplessWatch();
    g_tapQuit.store(true);
    if (g_tapThread.joinable()) {
        g_tapThread.join();
    }
    g_quit.store(true);
    if (g_worker.joinable()) {
        g_worker.join();
    }
    if (g_visualTreeWatcher) {
        g_visualTreeWatcher->UnadviseVisualTreeChange();
        g_visualTreeWatcher = nullptr;
    }
    Rec(L"=== probe detached ===");
}
