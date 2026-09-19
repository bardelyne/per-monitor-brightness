// ==WindhawkMod==
// @id              per-monitor-brightness
// @name            Per-monitor brightness in Quick Settings
// @description     Adds a titled brightness slider for every connected monitor to the Windows 11 Quick Settings panel
// @version         2.0
// @author          bardelyne
// @github          https://github.com/bardelyne
// @include         ShellHost.exe
// @architecture    x86-64
// @license         GPL-3.0
// @compilerOptions -ldxva2 -lole32 -loleaut32 -lwbemuuid -luuid -lruntimeobject
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.
//
// This mod used to carry XAML-diagnostics plumbing adapted from m417z's
// "Windows 11 Notification Center Styler". That is gone -- diagnostics allows
// only one consumer per process, so holding the slot stopped that very mod
// theming the Control Center -- but the WH_CALLWNDPROC trick in
// RunOnXamlThread still follows its approach, and the mod remains GPLv3.

// ==WindhawkModReadme==
/*
# Per-monitor brightness in Quick Settings

Windows gives you exactly one brightness slider no matter how many monitors you
have, and on a desktop it gives you none at all. This mod adds a labelled slider
for every connected display, right in the Quick Settings panel, each showing its
current level and carrying the shell's own animated brightness icon.

![Per-monitor brightness sliders in Quick Settings](https://raw.githubusercontent.com/bardelyne/per-monitor-brightness/main/screenshot.png)

## How each display is driven

- **External monitors** use **DDC/CI** (VCP code `0x10`), the I2C side-channel
  in the video cable that the monitor's own on-screen menu uses. Most monitors
  made in the last decade support it; some budget panels and some USB-C docks
  do not.
- **Laptop internal panels** use **WMI**, the same path the stock slider takes.

A display that answers neither is listed as uncontrollable rather than being
silently dropped.

## Features

- One titled slider per display, showing the live percentage.
- Sliders ordered left to right to match how the monitors sit on your desk.
- Displays identified by EDID device path, so the right slider follows the right
  monitor across hotplug, reordering and reboots.
- Each slider snaps to what its monitor can actually represent. Panels do not
  all use a 0-100 scale -- a Samsung G32 reports 0-50 -- so offering 1% steps
  would just mean several slider positions that write the same value.
- Function keys move the sliders live, and can optionally drive external
  monitors too, which they cannot do on their own.
- Monitors plugged in or unplugged are picked up immediately.
- Values are re-read whenever the panel opens, so changes made elsewhere show up.
- The stock brightness slider can be hidden, since it duplicates the built-in
  panel's row.

## Settings

- **Hide the built-in brightness slider** -- on by default; the stock slider
  only controls the internal panel, which already has its own row here.
- **Laptop brightness keys control every monitor** -- `off` by default.
  `relative` shifts other monitors by the same amount, preserving their offset;
  `match` sets them all to the same percentage.

  It is off by default because Windows raises the same event for a brightness
  key, for the power plan's AC/battery levels, and for idle dimming, with no
  way to tell them apart -- so unplugging the charger or walking away would
  also write to every external monitor. Unlike everything else this mod does,
  that write survives turning the setting off: the monitor stores the value
  itself.

## Compatibility

Requires a Windows 11 build where the Control Center is hosted by
`ShellHost.exe` -- developed and tested on 25H2 (build 26200).

Earlier builds host it in `ShellExperienceHost.exe` and are **not supported**.
That process is not included, but the reason has changed and is now only that
it is untested. The original objection -- that a second XAML host would start a
second brightness engine, doubling the WMI connection, the DDC/CI probe and the
response to every brightness keypress -- no longer applies: the engine starts
only where `ControlCenter.dll` is loaded, so a process that never hosts the
Control Center never starts one.

What remains unverified is whether the hooked symbol and the `L1Grid` layout
this mod depends on are the same on those builds. Without one to test on, the
include stays off.

## Notes and limitations

- A DDC/CI write takes roughly 50-60 ms, and the bus saturates while dragging.
  All hardware access happens on a background thread and repeated values
  collapse into a single write, so dragging never stalls the shell -- but an
  external monitor will visibly step rather than fade. The internal panel is
  around ten times faster and looks smooth.
- DDC/CI has no notification channel: a monitor only ever answers what the host
  asks it. Brightness changed using the monitor's own buttons therefore cannot
  be detected, and only shows up the next time the panel is opened.
- Not every monitor implements DDC/CI correctly. If a display does not respond,
  turn on logging for this mod in Windhawk (the mod's **Advanced** settings ->
  **Logging**) and check whether its writes report `ok=0`.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hideStockBrightness: true
  $name: Hide the built-in brightness slider
  $description: >-
    The stock slider only controls the internal laptop panel, which this mod
    already gives its own labelled slider, so leaving both on shows the same
    display twice. Turn this off to keep the original slider as well.
- followInternalBrightness: "off"
  $name: Laptop brightness keys control every monitor
  $description: >-
    Function keys only reach the built-in panel -- that is a hardware limit, not
    a Windows one. This mirrors those keypresses onto external monitors over
    DDC/CI, so one keypress dims everything.

    Off by default, because Windows gives no way to tell a keypress apart from
    any other change to the built-in panel. The same signal is raised by the
    power plan's AC and battery brightness levels and by "dim the display
    after N minutes", so with this on, unplugging the charger or leaving the
    machine idle also writes to every external monitor. That write is not
    undone by turning this off again or by disabling the mod: a monitor keeps
    the brightness it was given in its own settings, so its previous value is
    gone unless you set it back by hand.

    Dragging the built-in display's own slider in this panel leaves the other
    monitors alone.
  $options:
  - "off": Leave other monitors alone
  - relative: Shift other monitors by the same amount (keeps their offset)
  - match: Set other monitors to the same percentage
*/
// ==/WindhawkModSettings==


#include <inspectable.h>

// winbase.h defines GetCurrentTime as a macro, which collides with
// Windows.UI.Xaml.Media.Animation's method of the same name.
#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
// Not just the .0.h forward declarations: Append/Size have deduced return
// types and must be defined before use.
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#pragma pop_macro("GetCurrentTime")

#include <roapi.h>
#include <windhawk_utils.h>

#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;

// WinUI2 ABI, declared by hand. Windhawk does ship the WinUI2 C++/WinRT
// projection, but its headers include "winrt/impl/..." which resolves to the
// WinUI3 copies sitting at the default location, so the declarations and
// definitions disagree and it cannot be included without machine-specific -I
// flags -- which would make this mod non-portable. These three interfaces are
// all we need, and their GUIDs and vtable order come straight from that
// projection. The shell's AnimatedIcon is WinUI2: it lives inside a
// Windows.UI.Xaml tree, which WinUI3 controls cannot.
// GUIDs as constants rather than __declspec(uuid): that is an MSVC extension
// which clang ignores in the mingw target, leaving __uuidof unresolvable.
constexpr GUID kIID_AnimatedIcon = {
    0xF705DFDA, 0x8196, 0x56D0, {0x8D, 0xCF, 0x2B, 0x66, 0xC2, 0xAE, 0xD7, 0x91}};
constexpr GUID kIID_AnimatedIconStatics = {
    0x381896A8, 0xEE9F, 0x5823, {0xAB, 0xF1, 0xB5, 0x96, 0xAD, 0xCC, 0x77, 0xD1}};
constexpr GUID kIID_AnimatedVisualSource2 = {
    0x1A3B53A7, 0xA8FE, 0x59A1, {0xB5, 0x44, 0x43, 0xA4, 0xD9, 0xC8, 0x1E, 0xF2}};

struct IAnimatedIconAbi : ::IInspectable {
    virtual HRESULT __stdcall get_Source(void**) = 0;
    virtual HRESULT __stdcall put_Source(void*) = 0;
    virtual HRESULT __stdcall get_FallbackIconSource(void**) = 0;
    virtual HRESULT __stdcall put_FallbackIconSource(void*) = 0;
    virtual HRESULT __stdcall get_MirroredWhenRightToLeft(bool*) = 0;
    virtual HRESULT __stdcall put_MirroredWhenRightToLeft(bool) = 0;
};

struct IAnimatedIconStaticsAbi : ::IInspectable {
    virtual HRESULT __stdcall get_StateProperty(void**) = 0;
    virtual HRESULT __stdcall SetState(void*, void*) = 0;
    virtual HRESULT __stdcall GetState(void*, void**) = 0;
    virtual HRESULT __stdcall get_SourceProperty(void**) = 0;
    virtual HRESULT __stdcall get_FallbackIconSourceProperty(void**) = 0;
    virtual HRESULT __stdcall get_MirroredWhenRightToLeftProperty(void**) = 0;
};

struct IAnimatedVisualSource2Abi : ::IInspectable {
    virtual HRESULT __stdcall get_Markers(void**) = 0;
    // SetColorProperty follows here; never called, so left undeclared.
};

// XAML controls are composable, so their activation factory does not implement
// IActivationFactory::ActivateInstance -- it answers E_NOTIMPL. Construction
// goes through this instead, passing a null outer for a plain instance.
constexpr GUID kIID_AnimatedIconFactory = {
    0x3356E0D1, 0xD82F, 0x5FC1, {0x81, 0x65, 0x9B, 0x9D, 0x1B, 0x9D, 0x95, 0x14}};

struct IAnimatedIconFactoryAbi : ::IInspectable {
    virtual HRESULT __stdcall CreateInstance(void* outer, void** inner,
                                             void** instance) = 0;
};

constexpr wchar_t kAnimatedIconClass[] = L"Microsoft.UI.Xaml.Controls.AnimatedIcon";

// ===========================================================================
// Brightness engine (spliced in from brightness_engine.h by build_mod.sh).
// ===========================================================================

//__ENGINE_INLINE__

// ===========================================================================
// Mod state
// ===========================================================================

namespace {

brightness::Engine* g_engine = nullptr;

std::atomic<bool> g_engineStarted{false};

// The engine may not touch COM until the host has finished starting.
//
// Connecting to WMI activates a COM object, and the first activation in a
// process implicitly initialises COM security process-wide. Done from
// Wh_ModInit -- which runs before ShellHost's own startup code -- we win that
// race, ShellHost's own CoInitializeSecurity then fails RPC_E_TOO_LATE, and it
// fast-fails. The host aborts, restarts, and does it again, so the shell never
// comes back. Not throwing our own CoInitializeSecurity call is not enough;
// any activation is sufficient.
//
// Waiting until a XAML window exists means the host is past that point.
// Joined in Wh_ModUninit, not detached: Start() is still inside the engine
// when it runs, and Windhawk frees this DLL the moment uninit returns.
[[clang::no_destroy]] std::optional<std::thread> g_engineStarter;
// The flag alone would leave a gap: a starter that had claimed the flag but
// not yet assigned the thread would be invisible to a join in uninit, and
// would then run on into a freed DLL. The mutex closes it.
std::mutex g_engineStarterMutex;
bool g_engineStarterSpawned = false;

void StartEngineIfNeeded() {
    if (!g_engine || g_engineStarted.exchange(true)) {
        return;
    }
    g_engine->Start();
    Wh_Log(L"engine started (%d display(s))",
           static_cast<int>(g_engine->GetDisplays().size()));
}

// Called from the XAML thread when the Control Center turns up, so it must not
// block there -- the first enumeration can take seconds.
void StartEngineAsync() {
    std::lock_guard<std::mutex> lock(g_engineStarterMutex);
    if (g_engineStarterSpawned) {
        return;
    }
    g_engineStarterSpawned = true;
    g_engineStarter.emplace([] { StartEngineIfNeeded(); });
}

// Guards against double-injection: the visual tree reports the Control Center
// being built every time it opens, and it is rebuilt on each open.
std::atomic<bool> g_injecting{false};

// Injection is retried on every layout pass until it takes, so the diagnostic
// tree dump has to be one-shot or it would flood the log.
std::atomic<bool> g_dumpedTree{false};

// Everything we added to somebody else's visual tree, so that disabling the
// mod can put that tree back the way we found it. This is not cosmetic: the
// slider's ValueChanged handler is code inside this DLL, so a slider left
// behind after unload would call into freed memory the moment it is dragged.
struct Injection {
    // Weak, so a closed Control Center can still be collected.
    winrt::weak_ref<wuxc::Grid> grid;
    winrt::weak_ref<wuxc::StackPanel> panel;
    // Strong, and deliberately so: XAML only keeps projection peers alive for
    // elements in the live visual tree. A RowDefinition is not a UIElement, so
    // a weak ref to it is always dead by teardown even though the row itself is
    // still sitting in the collection. Holding it does not root the grid.
    wuxc::RowDefinition row{nullptr};
    std::vector<wuxc::Primitives::RangeBase::ValueChanged_revoker> revokers;

    // Per-display controls, so a refresh can update values in place instead of
    // rebuilding the whole panel.
    struct Binding {
        std::wstring id;
        std::wstring name;
        winrt::weak_ref<wuxc::Slider> slider;
        winrt::weak_ref<wux::FrameworkElement> icon;
        winrt::weak_ref<wuxc::TextBlock> title;
    };
    std::vector<Binding> bindings;

};

bool g_hideStockBrightness = true;

// The stock brightness row we collapsed and the group we shrank to close the
// gap. Kept outside Injection because the sliders are virtualized: the row may
// not exist until the panel is first shown, long after we injected, and there
// is only ever one of them. Touched only on the XAML thread.
struct StockSliderState {
    winrt::weak_ref<wux::FrameworkElement> item;
    wux::Visibility visibility = wux::Visibility::Visible;
    winrt::weak_ref<wux::FrameworkElement> group;
    double groupHeight = std::numeric_limits<double>::quiet_NaN();
    bool hidden = false;
};

// No [[clang::no_destroy]]: this holds only weak_refs, a double, an enum and
// a bool. Destroying a weak_ref at process shutdown just decrements an
// in-process control block, which is safe from any thread.
StockSliderState g_stockSlider;
bool g_loggedHideMiss = false;

// Set while pushing refreshed values into sliders, so their ValueChanged
// handlers can tell our own writes apart from the user's. UI thread only.
bool g_suppressValueChanged = false;

// The shell's own brightness Lottie, borrowed off its AnimatedIcon so our
// sliders can show the real animated sun rather than an imitation. WinUI2
// exposes no brightness visual source publicly, so lifting the live one is the
// only way to get it.
[[clang::no_destroy]] winrt::com_ptr<::IInspectable> g_brightnessSource;
[[clang::no_destroy]] winrt::com_ptr<IAnimatedIconStaticsAbi> g_animatedIconStatics;
bool g_capturedSource = false;

// The shell's Lottie carries two markers, Brightness_at_0 and
// Brightness_at_100, sitting at progress 0 and 1. So it is progress-driven
// rather than a state machine, and the usable range comes from those markers.
double g_progressMin = 0.0;
double g_progressMax = 1.0;

std::mutex g_injectionsMutex;
// Wrapped rather than bare, per the Windhawk guidance on globals at process
// shutdown: engaged from static init so it is always safe to dereference, and
// explicitly reset at the very end of Wh_ModUninit so the XAML references go
// while the apartment is still alive. The attribute stops the destructor from
// running on the shutdown thread when the host exits instead.
[[clang::no_destroy]] std::optional<std::vector<Injection>> g_injections{
    std::in_place};

// The XAML thread to marshal onto, captured at injection time. A thread id
// rather than a CoreDispatcher: an id is a plain value with no destructor and
// no refcount, so it cannot be raced into a use-after-free the way a shared
// CoreDispatcher reference could, and it is what the synchronous SendMessage
// hop below needs anyway.
std::atomic<DWORD> g_xamlThreadId{0};

// The timer that injects before the first paint. Declared here because
// RemoveInjections, far above its use, is the one place allowed to stop it:
// a DispatcherTimer is UI-thread affine.
[[clang::no_destroy]] wux::DispatcherTimer g_earlyInject{nullptr};

// Retry subscriptions on the shell's own elements. They must live in mod-owned
// globals: a revoker owned only by the lambda it is captured in cannot be
// reached at unload time, and a handler left registered when Windhawk frees
// this DLL is a crash on the next layout pass.
struct RetryHandlers {
    wux::FrameworkElement::Loaded_revoker loaded;
    wux::FrameworkElement::LayoutUpdated_revoker layout;
    int attempts = 0;

    void Revoke() {
        loaded.revoke();
        layout.revoke();
        attempts = 0;
    }
};

// Bounded on purpose. ArmStockSliderHide looks for an element that only exists
// when there is an internal panel, so on a desktop it would otherwise retry
// forever -- re-walking the shell's visual tree on every single layout pass,
// and adding another permanent handler every time the panel is opened.
constexpr int kMaxRetryAttempts = 60;

[[clang::no_destroy]] RetryHandlers g_injectRetry;
[[clang::no_destroy]] RetryHandlers g_hideRetry;

void EngineLog(const wchar_t* msg) {
    Wh_Log(L"engine: %s", msg);
}

HMODULE GetCurrentModuleHandle() {
    HMODULE module;
    if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           L"", &module)) {
        return nullptr;
    }
    return module;
}

// ---------------------------------------------------------------------------
// Visual tree helpers
// ---------------------------------------------------------------------------

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

// One-shot structural dump. The Control Center layout is undocumented and
// moves between Windows builds, so when injection cannot find what it expects
// the log has to be enough to fix it without guessing.
void DumpTree(wux::DependencyObject const& root, int depth, int maxDepth) {
    if (depth > maxDepth) {
        return;
    }

    std::wstring indent(static_cast<size_t>(depth) * 2, L' ');
    std::wstring extra;
    if (auto fe = root.try_as<wux::FrameworkElement>()) {
        wchar_t buf[128];
        swprintf(buf, 128, L"  [row=%d col=%d w=%.0f h=%.0f]",
                 wuxc::Grid::GetRow(fe), wuxc::Grid::GetColumn(fe),
                 fe.ActualWidth(), fe.ActualHeight());
        extra = buf;
    }
    Wh_Log(L"%s%s%s", indent.c_str(), ElementLabel(root).c_str(), extra.c_str());

    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        DumpTree(wuxm::VisualTreeHelper::GetChild(root, i), depth + 1, maxDepth);
    }
}

wux::DependencyObject FindDescendant(wux::DependencyObject const& root,
                                     std::wstring_view label, int maxDepth) {
    if (maxDepth < 0) {
        return nullptr;
    }
    if (ElementLabel(root) == label) {
        return root;
    }
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        auto child = wuxm::VisualTreeHelper::GetChild(root, i);
        if (auto found = FindDescendant(child, label, maxDepth - 1)) {
            return found;
        }
    }
    return nullptr;
}

wux::DependencyObject FindDescendantByName(wux::DependencyObject const& root,
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
        auto child = wuxm::VisualTreeHelper::GetChild(root, i);
        if (auto found = FindDescendantByName(child, name, maxDepth - 1)) {
            return found;
        }
    }
    return nullptr;
}

wux::DependencyObject FindAncestorOfClass(wux::DependencyObject const& start,
                                          std::wstring_view className,
                                          int maxUp) {
    auto current = start;
    for (int i = 0; i < maxUp; ++i) {
        current = wuxm::VisualTreeHelper::GetParent(current);
        if (!current) {
            return nullptr;
        }
        try {
            if (std::wstring{winrt::get_class_name(current)} == className) {
                return current;
            }
        } catch (...) {
            // Keep walking; an element we cannot name is not a match.
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

// Lifts the shell's brightness Lottie off its own AnimatedIcon so our sliders
// can use the genuine article. The source stays usable after we collapse the
// stock row, because collapsing only hides the element -- it stays in the tree.
void TryCaptureBrightnessSource(wux::FrameworkElement const& l1Grid) {
    if (g_capturedSource) {
        return;
    }

    auto group = FindDescendant(
        l1Grid, L"Windows.UI.Xaml.Controls.ContentControl#SlidersGroup", 6);
    if (!group) {
        return;
    }
    auto iconObj = FindDescendantByName(group, L"BrightnessPlayer", 40);
    if (!iconObj) {
        return;  // virtualized away; a later attempt may find it
    }

    winrt::com_ptr<IAnimatedIconAbi> iconAbi;
    if (FAILED(winrt::get_unknown(iconObj)->QueryInterface(
            kIID_AnimatedIcon, iconAbi.put_void()))) {
        Wh_Log(L"BrightnessPlayer is not a WinUI2 AnimatedIcon (%s)",
               ElementLabel(iconObj).c_str());
        g_capturedSource = true;  // no point retrying
        return;
    }

    winrt::com_ptr<::IInspectable> source;
    if (FAILED(iconAbi->get_Source(source.put_void())) || !source) {
        Wh_Log(L"BrightnessPlayer has no Source yet");
        return;
    }

    g_brightnessSource = source;
    g_capturedSource = true;
    Wh_Log(L"Captured the shell's brightness source");

    winrt::com_ptr<IAnimatedVisualSource2Abi> source2;
    if (FAILED(source->QueryInterface(kIID_AnimatedVisualSource2,
                                      source2.put_void()))) {
        Wh_Log(L"Source is not IAnimatedVisualSource2; cannot read markers");
        return;
    }

    // The markers bound the animation rather than enumerating states, so all
    // we need from them is the progress range to drive between.
    double minProgress = 0.0;
    double maxProgress = 0.0;
    bool haveMarkers = false;
    try {
        void* markersAbi = nullptr;
        if (FAILED(source2->get_Markers(&markersAbi)) || !markersAbi) {
            Wh_Log(L"Source exposes no markers; assuming 0..1");
        } else {
            // Windows.Foundation is projected correctly, so the map itself can
            // be handled with the normal projection once detached from the ABI.
            wf::Collections::IMapView<winrt::hstring, double> markers{nullptr};
            winrt::attach_abi(markers, markersAbi);

            for (auto const& marker : markers) {
                double value = marker.Value();
                Wh_Log(L"  marker: %s = %.4f",
                       std::wstring{marker.Key()}.c_str(), value);
                if (!haveMarkers) {
                    minProgress = maxProgress = value;
                    haveMarkers = true;
                } else {
                    minProgress = std::min(minProgress, value);
                    maxProgress = std::max(maxProgress, value);
                }
            }
        }
    } catch (...) {
        Wh_Log(L"Reading markers failed: %08X", winrt::to_hresult());
    }

    if (maxProgress <= minProgress) {
        minProgress = 0.0;
        maxProgress = 1.0;
    }
    g_progressMin = minProgress;
    g_progressMax = maxProgress;

    // SetState lives on the activation factory, not the instance.
    winrt::hstring className{kAnimatedIconClass};
    HRESULT hr = RoGetActivationFactory(
        static_cast<HSTRING>(winrt::get_abi(className)),
        kIID_AnimatedIconStatics, g_animatedIconStatics.put_void());
    if (FAILED(hr)) {
        Wh_Log(L"No IAnimatedIconStatics (%08X); the icon will not animate", hr);
    }

    Wh_Log(L"Brightness icon progress range %.3f .. %.3f", g_progressMin,
           g_progressMax);
}

// Either the shell's real AnimatedIcon, or the Segoe Fluent brightness glyph
// when the Lottie could not be borrowed or cannot be driven by level.
wux::FrameworkElement MakeBrightnessIcon() {
    if (g_brightnessSource) {
        try {
            winrt::hstring className{kAnimatedIconClass};

            // Not RoActivateInstance: that routes through
            // IActivationFactory::ActivateInstance, which a composable XAML
            // control leaves unimplemented (E_NOTIMPL).
            winrt::com_ptr<IAnimatedIconFactoryAbi> factory;
            HRESULT hr = RoGetActivationFactory(
                static_cast<HSTRING>(winrt::get_abi(className)),
                kIID_AnimatedIconFactory, factory.put_void());

            winrt::com_ptr<::IInspectable> inner;
            winrt::com_ptr<::IInspectable> instance;
            if (SUCCEEDED(hr) && factory) {
                hr = factory->CreateInstance(nullptr, inner.put_void(),
                                             instance.put_void());
            }

            winrt::com_ptr<IAnimatedIconAbi> iconAbi;
            if (SUCCEEDED(hr) && instance &&
                SUCCEEDED(instance->QueryInterface(kIID_AnimatedIcon,
                                                   iconAbi.put_void())) &&
                SUCCEEDED(iconAbi->put_Source(g_brightnessSource.get()))) {
                wux::FrameworkElement element{nullptr};
                if (SUCCEEDED(instance->QueryInterface(
                        winrt::guid_of<wux::FrameworkElement>(),
                        winrt::put_abi(element)))) {
                    element.Width(20);
                    element.Height(20);
                    return element;
                }
            }
            Wh_Log(L"AnimatedIcon creation failed (%08X); using the glyph", hr);
        } catch (...) {
            Wh_Log(L"AnimatedIcon creation threw: %08X", winrt::to_hresult());
        }
    }

    wuxc::FontIcon font;
    font.FontFamily(wuxm::FontFamily(L"Segoe Fluent Icons"));
    font.Glyph(L"");  // Brightness
    return font;
}

void SetIconLevel(wux::FrameworkElement const& icon, double percent) {
    double t = std::clamp(percent, 0.0, 100.0) / 100.0;

    if (auto font = icon.try_as<wuxc::FontIcon>()) {
        // Opacity only. Changing FontSize changes the icon's measured width,
        // and it sits in an Auto-width column, so the whole row would reflow
        // and the slider shift sideways as the thumb moves.
        font.Opacity(0.40 + 0.60 * t);
        return;
    }

    // Otherwise it is the borrowed AnimatedIcon.
    if (!g_animatedIconStatics) {
        return;
    }

    // AnimatedIcon resolves a state name against the Lottie's markers, and
    // when the name is not a marker but parses as a number it plays to that
    // normalized progress instead. That numeric fallback is what turns two
    // endpoint markers into a continuous level.
    double progress = g_progressMin + t * (g_progressMax - g_progressMin);
    wchar_t buffer[32];
    swprintf(buffer, 32, L"%.4f", progress);

    try {
        auto depObj = icon.try_as<wux::DependencyObject>();
        if (depObj) {
            winrt::hstring state{buffer};
            // A rejected state must not take the slider down with it.
            g_animatedIconStatics->SetState(winrt::get_abi(depObj),
                                            winrt::get_abi(state));
        }
    } catch (...) {
    }
}

winrt::hstring FormatRowTitle(const std::wstring& name, int percent) {
    if (percent < 0) {
        return winrt::hstring{name};
    }
    return winrt::hstring{name + L"  ·  " + std::to_wstring(percent) + L"%"};
}

void PopulateSliderPanel(wuxc::StackPanel const& panel, Injection& injection) {
    std::vector<brightness::Display> displays = g_engine->GetDisplays();
    Wh_Log(L"Building panel for %zu display(s)", displays.size());

    for (const brightness::Display& d : displays) {
        std::wstring displayName = d.name;
        // One fallback for both halves of the row. Showing a thumb at 50%
        // while the title omits the percentage reads as a bug; agreeing on 50
        // is at least self-consistent, and the first refresh corrects it.
        const int shownPercent = d.percent < 0 ? 50 : d.percent;
        wuxc::TextBlock title;
        title.Text(FormatRowTitle(displayName, shownPercent));
        title.FontSize(12);
        title.Margin(wux::ThicknessHelper::FromLengths(0, 6, 0, 0));
        title.Opacity(0.85);
        panel.Children().Append(title);

        if (d.transport == brightness::Transport::None) {
            // Say so rather than showing a slider that does nothing.
            wuxc::TextBlock note;
            note.Text(L"Brightness control not supported");
            note.FontSize(11);
            note.Opacity(0.55);
            panel.Children().Append(note);
            continue;
        }

        // Icon column + stretching slider column, so each row reads like the
        // shell's own slider rows.
        wuxc::Grid sliderRow;
        wuxc::ColumnDefinition iconColumn;
        iconColumn.Width(
            wux::GridLengthHelper::FromValueAndType(0, wux::GridUnitType::Auto));
        wuxc::ColumnDefinition sliderColumn;
        sliderColumn.Width(
            wux::GridLengthHelper::FromValueAndType(1, wux::GridUnitType::Star));
        sliderRow.ColumnDefinitions().Append(iconColumn);
        sliderRow.ColumnDefinitions().Append(sliderColumn);

        wux::FrameworkElement icon = MakeBrightnessIcon();
        icon.VerticalAlignment(wux::VerticalAlignment::Center);
        icon.Margin(wux::ThicknessHelper::FromLengths(0, 0, 12, 0));
        wuxc::Grid::SetColumn(icon, 0);
        sliderRow.Children().Append(icon);

        wuxc::Slider slider;
        slider.Minimum(0);
        slider.Maximum(100);
        slider.Value(shownPercent);
        slider.IsThumbToolTipEnabled(true);
        slider.VerticalAlignment(wux::VerticalAlignment::Center);
        wuxc::Grid::SetColumn(slider, 1);

        SetIconLevel(icon, slider.Value());

        // Snap to what the hardware can actually represent. A monitor whose
        // VCP range is 0-50 has 2% granularity, so offering 1% steps just
        // means three slider positions that all round to the same raw value.
        double step = 1.0;
        if (d.transport == brightness::Transport::DdcCi && d.vcpMax > 0) {
            step = 100.0 / static_cast<double>(d.vcpMax);
        }
        if (step < 1.0) {
            step = 1.0;  // finer than the slider is worth showing
        }
        slider.StepFrequency(step);
        slider.SnapsTo(wuxc::Primitives::SliderSnapsTo::StepValues);

        std::wstring id = d.stableId;
        // auto_revoke so the handler can be detached on unload; a dangling
        // registration into an unloaded DLL is a crash, not a leak.
        injection.revokers.push_back(slider.ValueChanged(
            winrt::auto_revoke,
            [id, icon, title, displayName](
                wf::IInspectable const&,
                wuxc::Primitives::RangeBaseValueChangedEventArgs const& args) {
                SetIconLevel(icon, args.NewValue());
                title.Text(FormatRowTitle(displayName,
                                          static_cast<int>(std::lround(args.NewValue()))));
                if (g_suppressValueChanged) {
                    // Echo of a refresh we just wrote into the slider; writing
                    // it back to the hardware would be a pointless round trip.
                    return;
                }
                if (g_engine) {
                    // Returns immediately; the worker coalesces and writes.
                    g_engine->SetPercent(id, static_cast<int>(std::lround(args.NewValue())));
                }
            }));

        sliderRow.Children().Append(slider);
        panel.Children().Append(sliderRow);

        injection.bindings.push_back({id, displayName, winrt::make_weak(slider),
                                      winrt::make_weak(icon),
                                      winrt::make_weak(title)});
    }
}

wuxc::StackPanel BuildSliderPanel(Injection& injection) {
    wuxc::StackPanel panel;
    panel.Name(L"WindhawkPerMonitorBrightness");
    panel.Orientation(wuxc::Orientation::Vertical);
    panel.Margin(wux::ThicknessHelper::FromLengths(16, 4, 16, 8));
    PopulateSliderPanel(panel, injection);
    return panel;
}

// Collapses the stock brightness row, which duplicates the internal-panel
// slider this mod already provides. The sliders live in a virtualized GridView,
// so rather than guess at an index we find the row by the animated sun icon it
// carries -- the volume row has no such element -- and walk up to its item.
bool TryHideStockBrightness(wux::FrameworkElement const& l1Grid) {
    if (g_stockSlider.hidden) {
        if (g_stockSlider.item.get()) {
            return true;  // still the tree we hid
        }
        // The view was rebuilt: our weak refs point into the dead tree, so the
        // new one has an unhidden stock row and unload would restore nothing.
        g_stockSlider = StockSliderState{};
    }

    auto group = FindDescendant(
        l1Grid, L"Windows.UI.Xaml.Controls.ContentControl#SlidersGroup", 6);
    if (!group) {
        return false;
    }

    // Generous depth: the icon lives inside the slider's template, roughly 20
    // levels below the group once the item container and presenters are
    // counted. The first version used 16 and never reached it.
    auto icon = FindDescendantByName(group, L"BrightnessPlayer", 40);
    if (!icon) {
        return false;  // rows not realized yet, or renamed
    }

    auto item = FindAncestorOfClass(
        icon, L"Windows.UI.Xaml.Controls.GridViewItem", 24);
    if (!item) {
        if (!g_loggedHideMiss) {
            g_loggedHideMiss = true;
            Wh_Log(L"Found BrightnessPlayer but no GridViewItem above it");
        }
        return false;
    }

    auto itemFe = item.try_as<wux::FrameworkElement>();
    auto groupFe = group.try_as<wux::FrameworkElement>();
    if (!itemFe || !groupFe) {
        return false;
    }

    // The group is sized for a fixed number of sliders, so collapsing a row on
    // its own leaves a gap. If the height is fixed but the row has not been
    // measured yet, wait rather than collapse and leave a hole.
    double groupHeight = groupFe.Height();
    double itemHeight = itemFe.ActualHeight();
    bool fixedHeight = !std::isnan(groupHeight);
    if (fixedHeight && itemHeight <= 0) {
        return false;
    }

    g_stockSlider.item = winrt::make_weak(itemFe);
    g_stockSlider.visibility = itemFe.Visibility();
    g_stockSlider.group = winrt::make_weak(groupFe);
    g_stockSlider.groupHeight = groupHeight;

    itemFe.Visibility(wux::Visibility::Collapsed);

    if (fixedHeight && groupHeight > itemHeight) {
        groupFe.Height(groupHeight - itemHeight);
        Wh_Log(L"Hid stock brightness row; SlidersGroup %.0f -> %.0f (row %.0f)",
               groupHeight, groupHeight - itemHeight, itemHeight);
    } else {
        Wh_Log(L"Hid stock brightness row; SlidersGroup height Auto (row %.0f)",
               itemHeight);
    }

    g_stockSlider.hidden = true;
    return true;
}

// The sliders are virtualized, so the brightness row often does not exist at
// injection time -- it is created when the panel is first shown. Retry on each
// layout pass until it appears.
void ArmStockSliderHide(wux::FrameworkElement const& l1Grid) {
    if (TryHideStockBrightness(l1Grid)) {
        return;
    }

    g_hideRetry.Revoke();
    g_hideRetry.layout = l1Grid.LayoutUpdated(
        winrt::auto_revoke,
        [l1Grid](wf::IInspectable const&, wf::IInspectable const&) {
            if (TryHideStockBrightness(l1Grid) ||
                ++g_hideRetry.attempts >= kMaxRetryAttempts) {
                // On a desktop there is no internal panel and therefore no
                // BrightnessPlayer to find, so without this it would retry on
                // every layout pass forever.
                g_hideRetry.Revoke();
            }
        });
}

// Called when the Control Center is actually opened, which is the event that
// matters: the sliders are virtualized, so the stock brightness row is created
// when the panel is first shown, not when the view is built.
//
// Arming used to happen only at injection time and was capped at
// kMaxRetryAttempts layout passes. The whole budget went on guessing when the
// row would be realized, and if it ran out the default-on setting silently
// stopped working for the rest of the session with no second chance. Keyed off
// the open instead, the cap is harmless: every open brings a fresh attempt.
void ReArmStockSliderHide() {
    if (!g_hideStockBrightness) {
        return;
    }
    try {
        wux::FrameworkElement grid{nullptr};
        {
            std::lock_guard<std::mutex> lock(g_injectionsMutex);
            if (!g_injections) {
                return;
            }
            for (const Injection& i : *g_injections) {
                if (auto live = i.grid.get()) {
                    grid = live.try_as<wux::FrameworkElement>();
                    if (grid) {
                        break;
                    }
                }
            }
        }
        if (grid) {
            g_hideRetry.attempts = 0;
            ArmStockSliderHide(grid);
        }
    } catch (...) {
        Wh_Log(L"ReArmStockSliderHide threw: %08X", winrt::to_hresult());
    }
}

// The Control Center's L1Grid is a Grid whose row layout is not documented and
// has changed across builds. Appending a row is only safe when it already
// declares RowDefinitions; otherwise every existing child implicitly lives in
// row 0 and adding a definition would re-flow the whole panel. In that case we
// log the structure and leave the UI untouched.
bool InjectInto(wux::FrameworkElement const& l1Grid) {
    auto grid = l1Grid.try_as<wuxc::Grid>();
    if (!grid) {
        Wh_Log(L"L1Grid is not a Grid (%s) -- not injecting",
               ElementLabel(l1Grid).c_str());
        return false;
    }

    if (FindDescendant(grid, L"Windows.UI.Xaml.Controls.StackPanel#WindhawkPerMonitorBrightness", 6)) {
        Wh_Log(L"Panel already present, skipping");
        // Not a no-op: the panel surviving does not mean the stock row was
        // ever found. This path used to return before arming, so a hide that
        // had not managed to land yet never got another attempt.
        if (g_hideStockBrightness) {
            ArmStockSliderHide(l1Grid);
        }
        return true;
    }

    uint32_t rowCount = grid.RowDefinitions().Size();
    Wh_Log(L"L1Grid has %u RowDefinition(s), %u child(ren)", rowCount,
           grid.Children().Size());

    if (rowCount == 0) {
        // Either the grid genuinely has no rows (in which case appending one
        // would re-flow every existing child out of row 0), or it is not built
        // yet and a later retry will succeed.
        if (!g_dumpedTree.exchange(true)) {
            Wh_Log(L"L1Grid declares no rows; refusing to re-flow it. "
                   L"Tree follows:");
            DumpTree(grid, 0, 4);
        }
        return false;
    }

    // Before building the panel: the icons need the shell's Lottie, and it has
    // to be read while the stock row is still visible.
    TryCaptureBrightnessSource(l1Grid);

    Injection injection;
    wuxc::StackPanel panel = BuildSliderPanel(injection);

    wuxc::RowDefinition row;
    row.Height(wux::GridLengthHelper::FromValueAndType(0, wux::GridUnitType::Auto));
    grid.RowDefinitions().Append(row);

    wuxc::Grid::SetRow(panel, static_cast<int>(grid.RowDefinitions().Size()) - 1);
    grid.Children().Append(panel);

    injection.grid = winrt::make_weak(grid);
    injection.panel = winrt::make_weak(panel);
    injection.row = row;

    if (g_hideStockBrightness) {
        ArmStockSliderHide(l1Grid);
    }

    {
        std::lock_guard<std::mutex> lock(g_injectionsMutex);
        // The Control Center is rebuilt on every open, so prune the entries
        // whose tree has already been torn down by the shell.
        std::erase_if(*g_injections, [](const Injection& i) {
            return !i.grid.get() || !i.panel.get();
        });
        g_injections->push_back(std::move(injection));
    }

    // We are on the XAML thread here; this is how everything else gets back to
    // it. There was also a XamlRoot.Changed subscription here as a secondary
    // "flyout shown" signal, but it never fires for this host on 26200 and the
    // revoker was being assigned to a moved-from local anyway, so it was doing
    // nothing at all. The WinEvent hook in ShellEventWatcher is the real one.
    g_xamlThreadId.store(GetCurrentThreadId());

    Wh_Log(L"Injected per-monitor brightness panel into row %u", rowCount);
    return true;
}

// Undoes every injection. Must run on the XAML thread.
void RemoveInjections() {
    // First: these live on the shell's own elements and are owned by nothing
    // else. Left registered, they call into this DLL after Windhawk frees it.
    g_injectRetry.Revoke();

    // The early-inject timer belongs here too, and only here.
    //
    // A DispatcherTimer is UI-thread affine: stopping one from Windhawk's
    // thread fails with RPC_E_WRONG_THREAD, which C++/WinRT raises as an
    // exception -- and out of Wh_ModUninit that is a crash, not a log line.
    // RemoveInjections already runs on the XAML thread for the same reason
    // the revokers above do, and it is the one place that must also stop a
    // tick from re-injecting into the tree it has just restored.
    if (g_earlyInject) {
        g_earlyInject.Stop();
        g_earlyInject = nullptr;
    }
    g_hideRetry.Revoke();

    std::vector<Injection> injections;
    {
        std::lock_guard<std::mutex> lock(g_injectionsMutex);
        injections.swap(*g_injections);
    }

    // Unhide the stock slider first, independently of our own panel: it may
    // have been hidden by a retry long after the injection was recorded.
    try {
        if (auto item = g_stockSlider.item.get()) {
            item.Visibility(g_stockSlider.visibility);
            Wh_Log(L"Restored stock brightness row");
        }
        if (auto group = g_stockSlider.group.get()) {
            // NaN restores Auto, which is what it was if we never set it.
            group.Height(g_stockSlider.groupHeight);
        }
    } catch (...) {
        Wh_Log(L"Restoring stock slider failed: %08X", winrt::to_hresult());
    }
    g_stockSlider = StockSliderState{};

    // Release the borrowed Lottie here, on the XAML thread, rather than letting
    // a global destructor drop it after the DLL is gone.
    g_brightnessSource = nullptr;
    g_animatedIconStatics = nullptr;
    g_capturedSource = false;
    g_progressMin = 0.0;
    g_progressMax = 1.0;

    for (Injection& injection : injections) {
        try {
            // Detach handlers before anything else, so nothing can fire at a
            // half-removed panel.
            injection.revokers.clear();

            auto grid = injection.grid.get();
            auto panel = injection.panel.get();
            if (!grid) {
                continue;
            }

            if (panel) {
                uint32_t index = 0;
                if (grid.Children().IndexOf(panel, index)) {
                    grid.Children().RemoveAt(index);
                }
            }

            // Remove the row we appended, but only that one -- match by
            // identity rather than by index, which may have shifted.
            auto row = injection.row;
            if (!row) {
                Wh_Log(L"No RowDefinition recorded; %u row(s) left",
                       grid.RowDefinitions().Size());
                continue;
            }

            auto rows = grid.RowDefinitions();
            uint32_t index = 0;
            bool found = rows.IndexOf(row, index);
            if (!found) {
                // XAML's RowDefinitionCollection does not implement IndexOf
                // usefully, so fall back to an explicit identity scan.
                for (uint32_t i = 0; i < rows.Size(); ++i) {
                    if (rows.GetAt(i) == row) {
                        index = i;
                        found = true;
                        break;
                    }
                }
            }

            if (found) {
                rows.RemoveAt(index);
                Wh_Log(L"Removed appended RowDefinition at %u; %u row(s) left",
                       index, rows.Size());
            } else {
                // Loud on purpose: silently skipping this is what let empty
                // rows accumulate one per enable/disable cycle.
                Wh_Log(L"WARNING: appended RowDefinition not found among %u; "
                       L"leaking one empty row",
                       rows.Size());
            }
        } catch (...) {
            Wh_Log(L"RemoveInjections error: %08X", winrt::to_hresult());
        }
    }

    Wh_Log(L"Removed %zu injection(s)", injections.size());
}

// Runs fn on the XAML thread and does not return until it has finished.
//
// Deliberately not CoreDispatcher::RunAsync: that queues the work, so there is
// no point at which we can say nothing of ours is still scheduled -- and
// Wh_ModUninit must be able to say exactly that before Windhawk frees this
// DLL. SendMessage is synchronous by construction, so when it returns the
// callback has already run. This is the same WH_CALLWNDPROC trick the
// notification center styler uses for the same reason.
BOOL CALLBACK FindThreadWindow(HWND hwnd, LPARAM lParam) {
    *reinterpret_cast<HWND*>(lParam) = hwnd;
    return FALSE;  // first one will do; we only need somewhere to send to
}

bool RunOnXamlThread(std::function<void()> fn) {
    // The callback travels in the message's own lParam rather than a global,
    // which is how the styler mods do it. No shared pointer, so no mutex
    // serialising unrelated hops, and no way for the callee to still be
    // running after the caller has given up and destroyed the function.
    static const UINT kRunMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RunParam {
        std::function<void()>* fn;
        bool ran;
    };

    DWORD threadId = g_xamlThreadId.load();
    if (!threadId) {
        return false;  // nothing was ever injected
    }
    if (threadId == GetCurrentThreadId()) {
        try {
            fn();
        } catch (...) {
            Wh_Log(L"inline call threw: %08X", winrt::to_hresult());
            return false;
        }
        return true;
    }

    HWND target = nullptr;
    EnumThreadWindows(threadId, FindThreadWindow,
                      reinterpret_cast<LPARAM>(&target));
    if (!target) {
        Wh_Log(L"No window on the XAML thread; cannot marshal");
        return false;
    }
    if (!kRunMsg) {
        return false;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (code == HC_ACTION) {
                auto* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
                if (cwp->message == kRunMsg && cwp->lParam) {
                    auto* param = reinterpret_cast<RunParam*>(cwp->lParam);
                    // Claimed before running, not marked after.
                    //
                    // Every hook in the chain sees every message, so with two
                    // of these installed at once the callback ran twice. That
                    // happens in ordinary use: opening the Control Center has
                    // the watcher thread marshalling ReArmStockSliderHide
                    // while the worker marshals ApplyRefreshedValues. The
                    // callees are idempotent, so it cost duplicated work
                    // rather than correctness -- but the primitive should
                    // only run what it was asked to run, once.
                    if (!param->ran) {
                        param->ran = true;
                        try {
                            (*param->fn)();
                        } catch (...) {
                            // Runs inside the shell's own dispatch; letting
                            // anything escape here takes the process down.
                            Wh_Log(L"marshalled call threw: %08X",
                                   winrt::to_hresult());
                        }
                    }
                }
            }
            return CallNextHookEx(nullptr, code, wParam, lParam);
        },
        nullptr, threadId);
    if (!hook) {
        Wh_Log(L"SetWindowsHookEx failed: %u", GetLastError());
        return false;
    }

    // A plain SendMessage, not SendMessageTimeout: it cannot return while the
    // callee is still using `param`, which is what makes the stack-allocated
    // parameter safe. Only ever called once injection has succeeded, so the
    // target thread is known to be pumping.
    RunParam param{&fn, false};
    SendMessage(target, kRunMsg, 0, reinterpret_cast<LPARAM>(&param));
    UnhookWindowsHookEx(hook);

    if (!param.ran) {
        Wh_Log(L"Marshalled call did not run");
    }
    return param.ran;
}

// Pushes freshly read hardware values into the existing sliders. XAML thread.
void ApplyRefreshedValues() try {
    if (!g_engine) {
        return;
    }
    std::vector<brightness::Display> displays = g_engine->GetDisplays();

    std::lock_guard<std::mutex> lock(g_injectionsMutex);
    for (Injection& injection : *g_injections) {
        for (Injection::Binding& binding : injection.bindings) {
            auto slider = binding.slider.get();
            if (!slider) {
                continue;
            }
            for (const brightness::Display& d : displays) {
                if (d.stableId != binding.id || d.percent < 0) {
                    continue;
                }
                if (std::lround(slider.Value()) == d.percent) {
                    break;  // already correct, leave the thumb alone
                }
                // Scoped: if Value() throws, the outer catch would otherwise
                // swallow it with the flag stuck true, and from then on every
                // drag is silently ignored until the mod is reloaded.
                {
                    struct Suppress {
                        Suppress() { g_suppressValueChanged = true; }
                        ~Suppress() { g_suppressValueChanged = false; }
                    } guard;
                    slider.Value(d.percent);
                }
                if (auto icon = binding.icon.get()) {
                    SetIconLevel(icon, d.percent);
                }
                if (auto title = binding.title.get()) {
                    title.Text(FormatRowTitle(binding.name, d.percent));
                }
                break;
            }
        }
    }
} catch (...) {
    Wh_Log(L"ApplyRefreshedValues threw: %08X", winrt::to_hresult());
}

// A monitor appeared or vanished, so the rows themselves are wrong. XAML
// thread. The panel and its grid row are kept; only the contents are redone.
void RebuildInjectedPanels() try {
    std::lock_guard<std::mutex> lock(g_injectionsMutex);
    size_t rebuilt = 0;
    for (Injection& injection : *g_injections) {
        auto panel = injection.panel.get();
        if (!panel) {
            continue;
        }
        // Detach before discarding the controls the handlers point at.
        injection.revokers.clear();
        injection.bindings.clear();
        panel.Children().Clear();
        PopulateSliderPanel(panel, injection);
        ++rebuilt;
    }
    Wh_Log(L"Rebuilt %zu panel(s) after a display change", rebuilt);
} catch (...) {
    Wh_Log(L"RebuildInjectedPanels threw: %08X", winrt::to_hresult());
}

void OnEngineChanged(bool structural) try {
    if (structural && g_engine) {
        for (const brightness::Display& d : g_engine->GetDisplays()) {
            Wh_Log(L"display: %s  transport=%d  now=%d%%  vcpMax=%lu  id=%s",
                   d.name.c_str(), static_cast<int>(d.transport), d.percent,
                   static_cast<unsigned long>(d.vcpMax), d.stableId.c_str());
        }
    }

    // Called on an engine thread; XAML may only be touched on its own. This
    // blocks until the work has run, which is what lets Wh_ModUninit promise
    // that nothing of ours is still scheduled.
    RunOnXamlThread([structural]() {
        if (structural) {
            RebuildInjectedPanels();
        } else {
            ApplyRefreshedValues();
        }
    });
} catch (...) {
    Wh_Log(L"OnEngineChanged threw: %08X", winrt::to_hresult());
}

// Owns the mod's Win32 listening. Two jobs, both needing a message loop:
//   * WM_DISPLAYCHANGE, which is broadcast to top-level windows only, so a
//     message-only window would never see it -- hence a real invisible popup.
//   * a WinEvent hook for windows being shown, which is how we learn the
//     flyout was opened. XamlRoot.Changed looked like the right signal but
//     never fires for this host on 26200, so the Win32 side it is.
class ShellEventWatcher {
   public:
    void Start() {
        if (thread_.joinable()) {
            return;
        }
        ready_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        thread_ = std::thread([this] { ThreadMain(); });
        if (ready_) {
            WaitForSingleObject(ready_, 5000);
        }
    }

    void Stop() {
        // Set before anything is posted, so a stop that arrives while the
        // thread is still starting up is seen by the check before its message
        // loop rather than being lost.
        stopRequested_.store(true);

        if (thread_.joinable()) {
            // Both, and unconditionally. Posting only when hwnd_ is already
            // published meant that if Start()'s 5 s wait had timed out, or
            // CreateEvent had failed so Start() never waited at all, nothing
            // was posted and join() blocked forever on a thread sitting in
            // GetMessageW -- taking Wh_ModUninit with it, so the mod could be
            // neither disabled nor updated. A narrow window, but wedging
            // Windhawk is the worst outcome available here.
            if (HWND hwnd = hwnd_.load()) {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            if (DWORD threadId = threadId_.load()) {
                PostThreadMessage(threadId, WM_QUIT, 0, 0);
            }
            thread_.join();
        }
        if (ready_) {
            CloseHandle(ready_);
            ready_ = nullptr;
        }
        stopRequested_.store(false);
        threadId_.store(0);
    }

   private:
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                      LONG idObject, LONG idChild, DWORD,
                                      DWORD) {
        if (event != EVENT_OBJECT_SHOW || idObject != OBJID_WINDOW ||
            idChild != CHILDID_SELF || !hwnd) {
            return;
        }
        // Only top-level windows: the flyout appearing, not its contents.
        if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
            return;
        }

        // Only the Control Center itself. Without this the slider's own thumb
        // tooltip (Xaml_WindowedPopupClass) trips the hook, spending a DDC read
        // at the start of every drag and risking a refresh landing on the
        // slider just as the user starts moving it.
        wchar_t className[128] = {};
        GetClassNameW(hwnd, className, ARRAYSIZE(className));
        if (!wcsstr(className, L"ControlCenter")) {
            // Capped: if a future build renames the window, these lines say
            // what to match instead.
            static int ignored = 0;
            if (ignored < 5) {
                ++ignored;
                Wh_Log(L"ignoring shown window (%s)", className);
            }
            return;
        }

        // A burst of show events must not become a burst of I2C reads.
        static ULONGLONG lastTick = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastTick < 500) {
            return;
        }
        lastTick = now;

        Wh_Log(L"Control Center shown (%s); refreshing values", className);
        if (g_engine) {
            g_engine->RequestRefresh();
        }
        // Marshalled, because this runs on the watcher thread and the hide
        // touches XAML. Same hop OnEngineChanged uses.
        RunOnXamlThread(&ReArmStockSliderHide);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
        switch (msg) {
            case WM_DISPLAYCHANGE:
                Wh_Log(L"WM_DISPLAYCHANGE: rescanning displays");
                if (g_engine) {
                    g_engine->RequestRescan();
                }
                break;
            case WM_DESTROY:
                PostQuitMessage(0);
                break;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    void ThreadMain() {
        // Published first: it is the only way Stop() can reach this thread
        // before the window exists.
        threadId_.store(GetCurrentThreadId());

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &ShellEventWatcher::WndProc;
        wc.hInstance = GetCurrentModuleHandle();
        wc.lpszClassName = L"WindhawkPerMonitorBrightnessSink";

        ATOM atom = RegisterClassExW(&wc);
        if (atom) {
            hwnd_ = CreateWindowExW(0, MAKEINTATOM(atom), L"", WS_POPUP, 0, 0, 0,
                                    0, nullptr, nullptr, wc.hInstance, nullptr);
            if (!hwnd_) {
                Wh_Log(L"Display sink window failed: %u", GetLastError());
            }
        } else {
            Wh_Log(L"Display sink class failed: %u", GetLastError());
        }

        // Must be installed and removed on the thread that pumps messages.
        hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
                                &ShellEventWatcher::WinEventProc,
                                GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
        if (!hook_) {
            Wh_Log(L"SetWinEventHook failed: %u", GetLastError());
        }

        if (ready_) {
            SetEvent(ready_);
        }
        // A Stop() between the window being created and the loop starting
        // would otherwise leave this thread pumping messages with nothing left
        // to signal it.
        if (stopRequested_.load()) {
            Wh_Log(L"Stop requested during startup; not entering the loop");
            if (hook_) {
                UnhookWinEvent(hook_);
                hook_ = nullptr;
            }
            if (HWND hwnd = hwnd_.exchange(nullptr)) {
                DestroyWindow(hwnd);
            }
            if (atom) {
                UnregisterClassW(MAKEINTATOM(atom), wc.hInstance);
            }
            return;
        }
        if (!hwnd_) {
            if (hook_) {
                UnhookWinEvent(hook_);
                hook_ = nullptr;
            }
            if (atom) {
                UnregisterClassW(MAKEINTATOM(atom), wc.hInstance);
            }
            return;
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (hook_) {
            UnhookWinEvent(hook_);
            hook_ = nullptr;
        }
        hwnd_ = nullptr;
        UnregisterClassW(MAKEINTATOM(atom), wc.hInstance);
    }

    std::thread thread_;
    HANDLE ready_ = nullptr;
    std::atomic<HWND> hwnd_{nullptr};
    // So Stop() has something to post to before hwnd_ exists.
    std::atomic<DWORD> threadId_{0};
    std::atomic<bool> stopRequested_{false};
    HWINEVENTHOOK hook_ = nullptr;
};

[[clang::no_destroy]] std::optional<ShellEventWatcher> g_shellWatcher;

bool TryInject(wux::DependencyObject const& controlCenterView) {
    bool expected = false;
    if (!g_injecting.compare_exchange_strong(expected, true)) {
        return false;
    }
    struct Guard {
        ~Guard() { g_injecting.store(false); }
    } guard;

    try {
        auto l1 = FindDescendant(controlCenterView,
                                 L"Windows.UI.Xaml.Controls.Grid#L1Grid", 8);
        if (!l1) {
            if (!g_dumpedTree.exchange(true)) {
                Wh_Log(L"L1Grid not found under ControlCenterView. "
                       L"Tree follows:");
                DumpTree(controlCenterView, 0, 6);
            }
            return false;
        }
        return InjectInto(l1.as<wux::FrameworkElement>());
    } catch (...) {
        Wh_Log(L"Injection failed: %08X", winrt::to_hresult());
        return false;
    }
}

// The Control Center is built once at shell start and merely shown and hidden
// afterwards, so by the time the view is reported its tree is normally already
// complete -- inject straight away. Waiting on a layout pass instead meant
// waiting until the panel was next *closed*, which is the first layout it runs.
// Loaded/LayoutUpdated remain as retries for the case where it really is not
// ready yet; both are revoked as soon as one succeeds.
void AttachInjector(wux::FrameworkElement const& view) {
    // We are on the XAML thread, so record it here rather than only on a
    // successful injection. The retry handlers below are registered on the
    // shell's own element whether or not the tree was ready, and Wh_ModUninit
    // keys "is there anything to remove?" off this id -- so leaving it unset
    // in exactly the case the retries exist for would return from unload with
    // live revokers pointing into an image Windhawk is about to unmap.
    g_xamlThreadId.store(GetCurrentThreadId());

    if (TryInject(view)) {
        return;
    }

    Wh_Log(L"Tree not ready, arming retries");

    g_injectRetry.Revoke();  // replaces any earlier arming
    auto retry = [view]() {
        if (TryInject(view) || ++g_injectRetry.attempts >= kMaxRetryAttempts) {
            g_injectRetry.Revoke();
        }
    };

    g_injectRetry.loaded = view.Loaded(
        winrt::auto_revoke,
        [retry](wf::IInspectable const&, wux::RoutedEventArgs const&) {
            retry();
        });
    g_injectRetry.layout = view.LayoutUpdated(
        winrt::auto_revoke,
        [retry](wf::IInspectable const&, wf::IInspectable const&) { retry(); });
}

}  // namespace

// ===========================================================================
// Finding the Control Center without XAML diagnostics
// ===========================================================================
//
// This used to open an XAML diagnostics connection and watch the visual tree
// for ControlCenterView being added. It worked, but only one diagnostics
// consumer can exist per process -- the Windows 11 Taskbar Styler says so in
// its own README -- so taking that connection stopped the Windows 11
// Notification Center Styler theming the Control Center, which is what a user
// reported. Nothing this mod did was at fault beyond holding the slot.
//
// So it does what explorer-command-bar does instead, and for the reason that
// mod gives: hook a function in the host's own DLL, take the element from it,
// and walk the tree with the public VisualTreeHelper API. No diagnostics, no
// slot to contend for, and the conflict is gone by construction rather than by
// winning a fight over a single-consumer resource.
//
// The hook is on ControlCenter.dll's
//
//   winrt::impl::produce<ControlCenterView, IControlOverrides>::OnGotFocus
//
// Three things had to be true and each took a measurement to establish:
//
//   * The pointer must really be a COM interface on the view. A produce<>
//     override's `this` is one. An implementation member's `this` is not, and
//     calling QueryInterface through it faulted ShellHost outright.
//   * It must run after the view is fully constructed. IComponentConnector::
//     Connect hands over the same element but runs inside InitializeComponent,
//     and merely taking a reference there destroyed the half-built view.
//   * It must not be inside a layout pass. Walking the tree from LayoutUpdated
//     ended in a fastfail.
//
// OnGotFocus satisfies all three: the flyout has been built and is taking
// focus. OnApplyTemplate would have been the obvious choice and is never
// called at all -- ControlCenterView is compiled XAML with an
// InitializeComponent, so no ControlTemplate is ever applied to it.

namespace {

std::atomic<bool> g_discoveryHooked{false};

using ControlCenterView_OnGotFocus_t = int(WINAPI*)(void* pThis, void* args);
ControlCenterView_OnGotFocus_t ControlCenterView_OnGotFocus_Original;

// Defined below, next to the tree helpers it belongs with.
wux::FrameworkElement FindContainingView(wux::DependencyObject const& from);

// Injecting before the first paint, via IComponentConnector::Connect.
//
// PlayIntroAnimation was the previous attempt and it never fires -- it is
// hooked optional, so it failed silently and everything kept going through
// OnGotFocus, which is after the flyout has been drawn. That is the flicker.
//
// Connect does fire, once per x:Name, during InitializeComponent. Two things
// have to be right about how it is used:
//
//   * Not `this`. That is the view, half-constructed; taking a reference to
//     it there destroyed it and took ShellHost down with a call through freed
//     memory. Only the `target` argument is touched here, which is a child
//     element the host has already finished building.
//   * Not immediately. The tree is still being assembled, so walking it now
//     would find an incomplete one. The work is posted to the dispatcher and
//     runs once construction has unwound -- still well before the flyout is
//     shown.
using Connect_t = int(WINAPI*)(void* pThis, int connectionId, void* target);
Connect_t ControlCenterView_Connect_Original;


int WINAPI ControlCenterView_Connect_Hook(void* pThis, int connectionId,
                                          void* target) {
    int ret = ControlCenterView_Connect_Original(pThis, connectionId, target);

    if (!target || g_earlyInject) {
        return ret;  // once per view is enough
    }
    try {
        wux::FrameworkElement child{nullptr};
        static_cast<::IUnknown*>(target)->QueryInterface(
            winrt::guid_of<wux::FrameworkElement>(), winrt::put_abi(child));
        if (!child) {
            return ret;
        }

        // The XAML thread is recorded here, not only in AttachInjector.
        //
        // Otherwise the unload path cannot reach this timer: it decides
        // whether anything needs undoing from g_xamlThreadId, and between
        // Connect and the first tick nothing has been injected yet, so it
        // would conclude there was nothing to do and return -- leaving the
        // tick to fire into an unmapped image.
        g_xamlThreadId.store(GetCurrentThreadId());

        auto timer = wux::DispatcherTimer();
        timer.Interval(std::chrono::milliseconds(1));
        timer.Tick([weak = winrt::make_weak(child)](
                       wf::IInspectable const& sender,
                       wf::IInspectable const&) {
            // Stopped through the sender, not a captured copy. Capturing the
            // timer makes a cycle -- the timer owns the handler, the handler
            // owns the timer -- so clearing g_earlyInject would never release
            // it and every view construction would leak a timer and a
            // delegate whose code lives in this DLL.
            if (auto self = sender.try_as<wux::DispatcherTimer>()) {
                self.Stop();
            }
            g_earlyInject = nullptr;
            try {
                auto element = weak.get();
                if (!element) {
                    return;
                }
                auto view = FindContainingView(element);
                if (!view) {
                    return;
                }
                StartEngineAsync();
                // Injected whether or not the engine has enumerated yet.
                //
                // Bailing on an empty display list made this path useless on
                // the first open after sign-in -- the enumeration takes a WMI
                // connect plus a DDC round trip per monitor, so it is rarely
                // finished this early -- and that open fell through to
                // OnGotFocus, after the first paint, which is the flicker
                // this exists to remove. An empty panel is filled in by the
                // structural rebuild, exactly as the OnGotFocus path already
                // relies on.
                AttachInjector(view);
            } catch (...) {
            }
        });
        timer.Start();
        g_earlyInject = timer;
    } catch (...) {
    }
    return ret;
}

// Walks up from any element to the ControlCenterView that contains it.
//
// Used by the Connect hook, which is handed a child rather than the view --
// deliberately, because the view is half-built at that point and touching it
// crashed the host.
wux::FrameworkElement FindContainingView(wux::DependencyObject const& from) {
    wux::DependencyObject node = from;
    for (int up = 0; up < 12 && node; ++up) {
        try {
            if (std::wstring_view{winrt::get_class_name(node)} ==
                L"ControlCenter.ControlCenterView") {
                return node.try_as<wux::FrameworkElement>();
            }
        } catch (...) {
        }
        node = wuxm::VisualTreeHelper::GetParent(node);
    }
    return nullptr;
}

int WINAPI ControlCenterView_OnGotFocus_Hook(void* pThis, void* args) {
    int ret = ControlCenterView_OnGotFocus_Original(pThis, args);

    try {
        // QueryInterface, not copy_from_abi.
        //
        // copy_from_abi does not QI -- it AddRefs and stores the pointer as
        // it is. `pThis` is the produce<ControlCenterView, IControlOverrides>
        // subobject, so the result would be an IControlOverrides* being
        // treated as an IFrameworkElement*. The happy path hides that, since
        // passing it as a DependencyObject does a real conversion, but the
        // retry path calls view.Loaded() and view.LayoutUpdated() straight
        // through the assumed vtable -- slots far past the end of
        // IControlOverrides. That is a wild call, not an exception, so the
        // catch below would not have caught it, and it would have fired on
        // exactly the builds the retries exist for.
        wux::FrameworkElement view{nullptr};
        static_cast<::IUnknown*>(pThis)->QueryInterface(
            winrt::guid_of<wux::FrameworkElement>(), winrt::put_abi(view));
        if (!view) {
            return ret;
        }

        // Here, and nowhere earlier, is where the engine starts. It used to
        // start as soon as any XAML window existed in the process, which on a
        // build that hosts the Control Center elsewhere meant a WMI
        // connection, a DDC/CI probe of every monitor and two threads running
        // permanently for a UI that would never appear.
        StartEngineAsync();

        // Injected without waiting for the enumeration: an empty panel now is
        // fine, because the first enumeration ends in NotifyChanged(structural)
        // and RebuildInjectedPanels fills it in -- the same path hotplug
        // already uses.
        //
        // Cheap when there is nothing to do: InjectInto returns early if the
        // panel is already in the tree, which matters because focus can be
        // taken more than once per opening.
        AttachInjector(view);
    } catch (...) {
        Wh_Log(L"OnGotFocus hook error: %08X", winrt::to_hresult());
    }

    return ret;
}

void InstallDiscoveryHooks(HMODULE controlCenter, bool applyNow) {
    if (g_discoveryHooked.exchange(true)) {
        return;
    }

    WindhawkUtils::SYMBOL_HOOK controlCenterDllHooks[] = {
        {
            {LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::ControlCenter::implementation::ControlCenterView,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnGotFocus(void *))"},
            &ControlCenterView_OnGotFocus_Original,
            ControlCenterView_OnGotFocus_Hook,
        },
        {
            {LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::ControlCenter::implementation::ControlCenterView,struct winrt::Windows::UI::Xaml::Markup::IComponentConnector>::Connect(int,void *))"},
            &ControlCenterView_Connect_Original,
            ControlCenterView_Connect_Hook,
            // Optional: without it the panel still appears, on the OnGotFocus
            // path a frame later. That is the visible flicker when Quick
            // Settings has been closed for a while, not a loss of function.
            true,
        },
    };

    if (!WindhawkUtils::HookSymbols(controlCenter, controlCenterDllHooks,
                                    ARRAYSIZE(controlCenterDllHooks))) {
        Wh_Log(L"Could not hook ControlCenterView::OnGotFocus; the panel will "
               L"not be injected");
        return;
    }

    // Windhawk applies what Wh_ModInit registers, once, when it returns.
    // A hook registered later -- from inside the load hook -- has to be
    // applied by hand or it never fires.
    if (applyNow && !Wh_ApplyHookOperations()) {
        Wh_Log(L"Hook registered but could not be armed");
        return;
    }

    Wh_Log(L"ControlCenterView::OnGotFocus hooked");

    // Start the engine now, not when the Control Center is first opened.
    //
    // This is what the flicker was. Injecting early is no use if there is
    // nothing to inject: the panel is built from the engine's display list,
    // and the first enumeration blocks on WMI and an I2C round trip per
    // monitor. Opening Quick Settings cold meant the intro-animation hook
    // found no displays, gave up, and the panel arrived on the later
    // OnGotFocus path -- after the stock layout had already been drawn. Open
    // it twice in a row and the engine was warm, so it looked fine, which is
    // exactly the pattern that was reported.
    //
    // Doing it here still honours why the engine was moved out of
    // Wh_ModAfterInit in the first place. The objection was to starting on
    // "any XAML window exists", which is true in processes that never host a
    // Control Center. ControlCenter.dll being loaded is a far tighter gate:
    // the module is present precisely where the panel can appear, and it is
    // mapped long before the flyout is first opened.
    StartEngineAsync();
}

// ControlCenter.dll is usually mapped before Wh_ModInit runs. When it is not,
// it has to be caught at the moment it loads, before any of its code runs.
//
// The hook goes on kernelbase's LoadLibraryExW, not kernel32's.
//
// That distinction is the whole reason an earlier version needed a polling
// thread: kernel32's export is a forwarder, and the WinRT activation path
// that maps this DLL calls kernelbase directly, so a hook on kernel32 never
// saw the load at all. Polling papered over it, and brought a thread that
// Wh_ModUninit had no way to join -- if the mod were disabled mid-sleep,
// Windhawk would unmap the image and the thread's next instruction would be
// in freed memory. Hooking the right function removes the need for both.

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;

HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR path, HANDLE file, DWORD flags) {
    HMODULE result = LoadLibraryExW_Original(path, file, flags);
    if (result && path && !g_discoveryHooked.load()) {
        const wchar_t* name = wcsrchr(path, L'\\');
        if (_wcsicmp(name ? name + 1 : path, L"ControlCenter.dll") == 0) {
            // Resolved through the loader rather than trusting what came
            // back.
            //
            // LOAD_LIBRARY_AS_DATAFILE and friends -- which resource lookups
            // use -- return a mapping that is not executable code, with its
            // low bits set. Hooking that would patch a resource view, set the
            // guard, and leave the real load ignored and the mod silently
            // dead for the session. A data-file mapping is never in the
            // loader's module list, so asking for it by name cannot return
            // one.
            HMODULE module = GetModuleHandleW(L"ControlCenter.dll");
            if (!module) {
                return result;
            }
            // Applied immediately: this runs inside the load, before anything
            // in the DLL has executed, so there is no window in which the
            // host could build the view unhooked.
            InstallDiscoveryHooks(module, /*applyNow=*/true);
        }
    }
    return result;
}

void StartControlCenterWatch() {
    if (HMODULE module = GetModuleHandleW(L"ControlCenter.dll")) {
        // Windhawk arms what Wh_ModInit registers, so nothing to apply here.
        InstallDiscoveryHooks(module, /*applyNow=*/false);
        return;
    }

    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    auto target = kernelBase ? reinterpret_cast<LoadLibraryExW_t>(
                                   GetProcAddress(kernelBase, "LoadLibraryExW"))
                             : nullptr;
    if (!target) {
        Wh_Log(L"No kernelbase!LoadLibraryExW; the panel will not be injected");
        return;
    }
    WindhawkUtils::SetFunctionHook(target, LoadLibraryExW_Hook,
                                   &LoadLibraryExW_Original);
}

}  // namespace

// ===========================================================================
// Windhawk lifecycle
// ===========================================================================

void LoadSettings() {
    g_hideStockBrightness = Wh_GetIntSetting(L"hideStockBrightness") != 0;

    // Wh_GetStringSetting returns L"" rather than NULL on failure, so a
    // pointer check would be meaningless; the RAII wrapper also removes the
    // manual Wh_FreeStringSetting.
    // Off unless asked for: following writes to monitors that keep the value
    // in their own settings, and Windows cannot distinguish a brightness key
    // from power-plan or idle dimming.
    brightness::FollowMode followMode = brightness::FollowMode::Off;
    WindhawkUtils::StringSetting follow =
        WindhawkUtils::StringSetting::make(L"followInternalBrightness");
    if (wcscmp(follow.get(), L"relative") == 0) {
        followMode = brightness::FollowMode::Relative;
    } else if (wcscmp(follow.get(), L"match") == 0) {
        followMode = brightness::FollowMode::Match;
    }

    if (g_engine) {
        g_engine->SetFollowMode(followMode);
    }

    Wh_Log(L"hideStockBrightness=%d followInternalBrightness=%d",
           g_hideStockBrightness ? 1 : 0, static_cast<int>(followMode));
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    g_engine = new brightness::Engine();
    g_engine->SetLogger(&EngineLog);
    g_engine->SetOnChanged(&OnEngineChanged);

    LoadSettings();

    // In Wh_ModInit so that Windhawk arms the hook itself when it returns, and
    // so it is in place before the host can build the view.
    StartControlCenterWatch();

    // The engine is deliberately NOT started here; see StartEngineIfNeeded.
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");

    // Nothing is started here on purpose. The engine waits until the Control
    // Center is actually seen (see the OnGotFocus hook), so a process that
    // never hosts one never pays for it.
    g_shellWatcher.emplace();
    g_shellWatcher->Start();
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    Wh_Log(L">");

    bool previouslyHidden = g_hideStockBrightness;
    LoadSettings();

    // Follow mode is engine state and takes effect at once.
    // Hiding the stock slider changes what was injected into somebody else's
    // visual tree, so only that one needs a reload to rebuild it.
    *bReload = (g_hideStockBrightness != previouslyHidden);
    return TRUE;
}

// Removing the injected UI is not optional. RunOnXamlThread can fail for
// reasons that have nothing to do with whether we injected -- no window found
// yet, SetWindowsHookEx refused -- and returning anyway would leave the
// sliders' ValueChanged handlers and the retry subscriptions registered on the
// shell's own elements, pointing into an image Windhawk is about to unmap.
// That is a crash on the shell's next layout pass.
bool RemoveInjectionsWithRetry() {
    for (int attempt = 0; attempt < 25; attempt++) {
        if (RunOnXamlThread(&RemoveInjections)) {
            return true;
        }
        Wh_Log(L"XAML thread unreachable (attempt %d); retrying before unload",
               attempt + 1);
        Sleep(200);
    }
    return false;
}

void Wh_ModUninit() {
    Wh_Log(L">");

    // Order matters. Everything that could still call into this DLL has to be
    // stopped before the UI is dismantled, and all of it has to be finished
    // before we return -- Windhawk frees the module the moment we do.

    // 1. The watcher first: it owns the timer that can still start the engine.
    //
    //    Nothing has to be un-advised any more. The injection hook is a
    //    function hook, and Windhawk removes those itself as part of unloading
    //    the mod -- unlike a diagnostics connection, which had to be given
    //    back by hand and would otherwise have outlived the DLL.
    if (g_shellWatcher) {
        g_shellWatcher->Stop();
        // Explicit, rather than relying on the suppressed destructor: Stop()
        // is what joins the thread and closes the event, so this is the point
        // at which the watcher is provably finished with.
        g_shellWatcher.reset();
    }

    // 2. The starter thread may be inside Engine::Start() right now, so it has
    //    to be finished before anything below touches the engine.
    {
        std::lock_guard<std::mutex> lock(g_engineStarterMutex);
        if (g_engineStarter && g_engineStarter->joinable()) {
            g_engineStarter->join();
        }
        // reset() is the release: there is no assignment to a std::thread
        // that means "done with this", and move-assigning over a joinable
        // one terminates.
        g_engineStarter.reset();
    }

    // 3. No more engine callbacks, and join both engine threads so none can be
    //    in flight. After this nothing can ask to run on the XAML thread.
    if (g_engine) {
        g_engine->SetOnChanged(nullptr);
        g_engine->Stop();
    }

    // 4. Put the visual tree back, synchronously, on the thread that owns it.
    bool treeRestored = true;
    if (g_xamlThreadId.load() == 0) {
        Wh_Log(L"Nothing was injected; nothing to remove");
    } else if (!RemoveInjectionsWithRetry()) {
        Wh_Log(L"Could not reach the XAML thread after 5s; injected UI may "
               L"still be live");
        treeRestored = false;
    }

    // 5. Only now is it safe to drop the engine itself.
    if (g_engine) {
        delete g_engine;
        g_engine = nullptr;
    }

    g_xamlThreadId.store(0);

    // Last: release the injection bookkeeping while COM is still usable. On
    // the success path RemoveInjections has already swapped the vector empty,
    // so this just drops the container.
    //
    // If the tree could not be restored it is NOT empty, and resetting would
    // release live XAML references from this thread rather than the one that
    // owns them. Leaking the container is the lesser evil, and is what the
    // [[clang::no_destroy]] is there to make survivable.
    if (treeRestored) {
        g_injections.reset();
    } else {
        Wh_Log(L"Leaving injection bookkeeping alive; releasing it off the "
               L"XAML thread would be worse");
    }
}
