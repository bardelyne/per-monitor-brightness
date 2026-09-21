// ==WindhawkMod==
// @id              pmb-layout-probe
// @name            Control Center layout probe (temporary)
// @description     Dumps where the native slider group sits in the Control Center tree, so the brightness panel can be placed beside it instead of below everything. Diagnostic only.
// @version         0.1
// @author          bardelyne
// @github          https://github.com/bardelyne
// @include         ShellHost.exe
// @architecture    x86-64
// @license         GPL-3.0
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Control Center layout probe

Temporary diagnostic for per-monitor-brightness issue #5608, which asks for the
brightness sliders to sit with the native ones rather than below the settings
gear.

The question this answers is narrow: **what is `SlidersGroup`'s parent, and can
a sibling be inserted next to it?** If the parent is a `Panel`, insertion is a
`Children().InsertAt` and costs nothing. If it is a single-slot
`ContentControl`, the only way in is re-flowing `L1Grid`'s rows, which is a
different proposition and probably not worth it.

So it reports, for the chain from `SlidersGroup` up to `L1Grid`: each ancestor's
type, whether it is a `Panel` (and if so how many children and which index the
chain came through), and the `Grid.Row` each one is assigned. Then it dumps
`L1Grid`'s direct children with their rows, which is the map a position setting
would be built from.

Writes to `%TEMP%\pmb-layout-probe.log`. Reads only -- it changes nothing in
the tree.

Disable the real mod while running this: both hook the same symbol.
*/
// ==/WindhawkModReadme==

#include <inspectable.h>

// Same two traps as the mod proper, and for the same reasons: winbase.h
// defines GetCurrentTime as a macro, which collides with
// Windows.UI.Xaml.Media.Animation's method of that name; and Size/GetAt have
// deduced return types, so the .0.h forward declarations are not enough.
#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#pragma pop_macro("GetCurrentTime")

#include <windhawk_utils.h>

#include <atomic>
#include <cstdarg>
#include <string>
#include <string_view>
#include <vector>

namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;

namespace {

// A file, not Wh_Log. The Windhawk service owns the OutputDebugString channel
// for as long as it runs, so nothing else can read Wh_Log output -- see the
// debugging section of DEVELOPING.md.
void Rec(const wchar_t* format, ...) {
    wchar_t line[2048];
    va_list args;
    va_start(args, format);
    vswprintf(line, ARRAYSIZE(line), format, args);
    va_end(args);

    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path)) {
        return;
    }
    wcsncat_s(path, L"pmb-layout-probe.log", _TRUNCATE);

    HANDLE file =
        CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    std::wstring out{line};
    out += L"\r\n";
    DWORD written = 0;
    // UTF-16LE with a BOM on a fresh file, so the log opens correctly.
    if (GetFileSize(file, nullptr) == 0) {
        const WCHAR bom = 0xFEFF;
        WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    }
    WriteFile(file, out.c_str(),
              static_cast<DWORD>(out.size() * sizeof(wchar_t)), &written,
              nullptr);
    CloseHandle(file);
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
        if (auto found = FindDescendant(wuxm::VisualTreeHelper::GetChild(root, i),
                                        label, maxDepth - 1)) {
            return found;
        }
    }
    return nullptr;
}

// What a container can be asked to do, which is the whole point of the probe.
std::wstring Capability(wux::DependencyObject const& obj) {
    if (auto panel = obj.try_as<wuxc::Panel>()) {
        wchar_t buf[128];
        swprintf(buf, 128, L"Panel, %u child(ren) -- INSERTABLE",
                 panel.Children().Size());
        return buf;
    }
    if (obj.try_as<wuxc::ItemsControl>()) {
        return L"ItemsControl -- items come from a source we do not own";
    }
    if (obj.try_as<wuxc::ContentControl>()) {
        return L"ContentControl -- single slot, not insertable";
    }
    if (obj.try_as<wuxc::Border>()) {
        return L"Border -- single child, not insertable";
    }
    return L"(other)";
}

void DumpTree(wux::DependencyObject const& root, int depth, int maxDepth) {
    if (depth > maxDepth) {
        return;
    }
    std::wstring indent(static_cast<size_t>(depth) * 2, L' ');
    std::wstring extra;
    if (auto fe = root.try_as<wux::FrameworkElement>()) {
        wchar_t buf[256];
        // RowSpan matters as much as Row: the card behind the toggles and
        // sliders is a Border that spans them, so "which row" alone does not
        // say whether an inserted row lands inside the card or under it.
        swprintf(buf, 256,
                 L"  [row=%d span=%d col=%d w=%.0f h=%.0f Height=%.0f "
                 L"margin=%.0f,%.0f,%.0f,%.0f]",
                 wuxc::Grid::GetRow(fe), wuxc::Grid::GetRowSpan(fe),
                 wuxc::Grid::GetColumn(fe), fe.ActualWidth(),
                 fe.ActualHeight(), fe.Height(), fe.Margin().Left,
                 fe.Margin().Top, fe.Margin().Right, fe.Margin().Bottom);
        extra = buf;
    }
    Rec(L"%s%s%s", indent.c_str(), ElementLabel(root).c_str(), extra.c_str());

    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        DumpTree(wuxm::VisualTreeHelper::GetChild(root, i), depth + 1, maxDepth);
    }
}

std::atomic<bool> g_reported{false};

void Report(wux::DependencyObject const& view) try {
    if (g_reported.exchange(true)) {
        return;
    }

    Rec(L"=== Control Center layout, %s ===", L"first open after load");

    auto l1 = FindDescendant(view, L"Windows.UI.Xaml.Controls.Grid#L1Grid", 8);
    if (!l1) {
        Rec(L"L1Grid not found; dumping the view instead");
        DumpTree(view, 0, 6);
        return;
    }
    auto l1Grid = l1.as<wuxc::Grid>();

    Rec(L"");
    Rec(L"--- L1Grid: %u row(s), %u direct child(ren) ---",
        l1Grid.RowDefinitions().Size(), l1Grid.Children().Size());
    for (uint32_t i = 0; i < l1Grid.Children().Size(); ++i) {
        auto child = l1Grid.Children().GetAt(i);
        if (auto fe = child.try_as<wux::FrameworkElement>()) {
            Rec(L"  child[%u] row=%d span=%d  h=%.0f  %s  (%s)", i,
                wuxc::Grid::GetRow(fe), wuxc::Grid::GetRowSpan(fe),
                fe.ActualHeight(), ElementLabel(fe).c_str(),
                Capability(fe).c_str());
        }
    }
    Rec(L"  row heights:");
    for (uint32_t i = 0; i < l1Grid.RowDefinitions().Size(); ++i) {
        auto rd = l1Grid.RowDefinitions().GetAt(i);
        Rec(L"    row[%u] GridUnitType=%d value=%.0f actual=%.0f", i,
            static_cast<int>(rd.Height().GridUnitType), rd.Height().Value,
            rd.ActualHeight());
    }

    // The chain, which is the answer.
    auto group = FindDescendant(
        l1Grid, L"Windows.UI.Xaml.Controls.ContentControl#SlidersGroup", 8);
    Rec(L"");
    if (!group) {
        Rec(L"--- SlidersGroup NOT found under L1Grid ---");
        Rec(L"    (a desktop with no internal panel may not have one at all)");
    } else {
        Rec(L"--- SlidersGroup up to L1Grid ---");
        wux::DependencyObject node = group;
        wux::DependencyObject previous{nullptr};
        int level = 0;
        while (node && level < 24) {
            std::wstring where = L"";
            if (previous) {
                if (auto panel = node.try_as<wuxc::Panel>()) {
                    for (uint32_t i = 0; i < panel.Children().Size(); ++i) {
                        if (panel.Children().GetAt(i).try_as<wux::DependencyObject>() ==
                            previous) {
                            wchar_t buf[64];
                            swprintf(buf, 64, L"  <- came from Children[%u]", i);
                            where = buf;
                            break;
                        }
                    }
                }
            }
            int row = -1;
            if (auto fe = node.try_as<wux::FrameworkElement>()) {
                row = wuxc::Grid::GetRow(fe);
            }
            Rec(L"  [%d] row=%d  %s", level, row, ElementLabel(node).c_str());
            Rec(L"       %s%s", Capability(node).c_str(), where.c_str());

            if (ElementLabel(node) == L"Windows.UI.Xaml.Controls.Grid#L1Grid") {
                break;
            }
            previous = node;
            node = wuxm::VisualTreeHelper::GetParent(node);
            ++level;
        }
    }

    // Siblings of whatever holds the slider group, so a position setting knows
    // what it would be inserting between.
    Rec(L"");
    Rec(L"--- L1Grid subtree, 4 deep ---");
    DumpTree(l1Grid, 0, 4);

    // One native slider row in full, so an inserted row can be built to match
    // it rather than approximated by eye.
    // Anchored on the group, not on L1Grid. Both TogglesGroup and SlidersGroup
    // contain a "GridView#RootGridView", and a depth-first search from L1Grid
    // reaches the toggles' one first -- it dumps 96x88 tiles and looks like a
    // plausible answer, which is the worst kind of wrong.
    Rec(L"");
    if (group) {
        if (auto rootGridView = FindDescendant(
                group, L"Windows.UI.Xaml.Controls.GridView#RootGridView", 6)) {
            Rec(L"--- the native SLIDER rows, 10 deep ---");
            DumpTree(rootGridView, 0, 10);
        } else {
            Rec(L"--- no RootGridView under SlidersGroup ---");
            DumpTree(group, 0, 6);
        }
    }
    Rec(L"=== end ===");
} catch (...) {
    Rec(L"Report threw %08X", static_cast<unsigned>(winrt::to_hresult()));
}

using OnGotFocus_t = int(WINAPI*)(void* pThis, void* args);
OnGotFocus_t OnGotFocus_Original;

int WINAPI OnGotFocus_Hook(void* pThis, void* args) {
    int ret = OnGotFocus_Original(pThis, args);
    try {
        // QueryInterface, not copy_from_abi: pThis is the produce<> subobject,
        // and copy_from_abi would store it without converting.
        wux::FrameworkElement view{nullptr};
        static_cast<::IUnknown*>(pThis)->QueryInterface(
            winrt::guid_of<wux::FrameworkElement>(), winrt::put_abi(view));
        if (view) {
            Report(view);
        }
    } catch (...) {
    }
    return ret;
}

}  // namespace

BOOL Wh_ModInit() {
    HMODULE controlCenter = GetModuleHandleW(L"ControlCenter.dll");
    if (!controlCenter) {
        Rec(L"ControlCenter.dll not mapped yet; open Quick Settings once, then "
            L"reload this probe");
        return TRUE;
    }

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {
            {LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::ControlCenter::implementation::ControlCenterView,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnGotFocus(void *))"},
            &OnGotFocus_Original,
            OnGotFocus_Hook,
        },
    };
    if (!WindhawkUtils::HookSymbols(controlCenter, hooks, ARRAYSIZE(hooks))) {
        Rec(L"Could not hook OnGotFocus");
        return TRUE;
    }
    Rec(L"probe armed; open Quick Settings");
    return TRUE;
}

void Wh_ModUninit() {
    // Nothing to undo: the probe only reads.
}
