# Why this mod stops the Notification Center Styler

A user reported that enabling this mod stops the Windows 11 Notification
Center Styler theming the Control Center. This is what the probe mod found.

## The answer

**A second XAML diagnostics TAP in the process breaks the first one.** That is
all. It is nothing this mod does beyond injecting one.

The probe is a mod that does nothing except inject a TAP and read the tree.
Every capability of the real mod is behind a setting and all of them were off:

| probe state | styler |
|---|---|
| not loaded | **works** (8049 red pixels) |
| loaded, everything off | **broken** (0) |
| loaded, worker thread on | broken (0) |
| loaded, thread + COM on | broken (0) |

The control was re-run immediately afterwards with the probe disabled and gave
8049 again, so the difference is the probe and not drift.

It follows that this affects any pair of Windhawk mods that both use the TAP
in one process, not just this one. Relevant here: `everything-search` and
`windows-11-start-menu-styler` both target `SearchHost.exe`.

## How the signal was built, and why the first attempt was worthless

The first round of this compared screenshots of the Control Center and
concluded, in turn, that the cause was a duplicated CLSID, then the slider
hiding, then COM, then WMI, then DDC, then the worker thread. All of it was
wrong, because the measurement was:

- **The theme is translucent.** The flyout shows whatever is behind it, so its
  apparent colour tracked which window happened to be underneath, not whether
  the styler had run.
- **`TintedGlass` does not touch the Control Center at all.** Verified by
  dumping every element carrying a background brush or corner radius with the
  styler on and again with it off: byte for byte identical. There was never a
  signal in those screenshots to read.
- **Corner crops were positioned by eye**, so "square" versus "rounded" was
  comparing different parts of the curve.

The control that would have caught all of this -- disable the mod, confirm the
styler comes back -- was not re-run between tests. When it finally was, the
mod-off state looked exactly like the states that had been called broken.

What replaced it: give the styler a rule that paints something unmistakable,
`Grid#ControlCenterRegion` with
`Background:=<SolidColorBrush Color="Red"/>`, and count strongly red pixels.
8049 or 0, no judgement involved. Reaching that took two tries as well --
targeting `ControlCenter.ControlCenterView` with `Background=#FFFF0000`
applied nothing, so the first "styler is broken" reading from the loud rule
was also the measurement's fault rather than a finding.

Two earlier conclusions died with the old signal and should not be cited:

- "Two TAPs coexist fine, UWPSpy proves it." UWPSpy was attached while only
  the theme was configured, and the theme does not style the Control Center.
  Nothing was being measured.
- "The engine's COM/WMI/DDC work is the cause." Every one of those runs was
  read off a translucent surface.

## What survives

- The conflict is real and reproducible.
- It is not the CLSID. Tested directly with a fresh GUID; no change. The mod
  now has its own regardless, because two mods sharing one is wrong on its own
  merits.
- It is not this mod's engine, threads, COM, WMI, DDC, or tree injection. The
  probe does none of those and still breaks the styler.

## The fix, and what it took to land it

The TAP is gone. The mod now hooks a function in ControlCenter.dll and takes
the Control Center element out of it, then walks the tree with the public
`VisualTreeHelper` API -- what `explorer-command-bar` does, and for the reason
it gives.

The hook is:

    winrt::impl::produce<ControlCenterView, IControlOverrides>::OnGotFocus

Measured with the same red-pixel signal, control re-run each time:

| run | red | styler |
|---|---|---|
| nothing loaded | 8050 | works |
| probe with a TAP | 0 | **broken** |
| probe, tapless | 8050 | works |
| **real mod (ported) + styler** | **9470** | **works** |

The last row is the one that answers the bug report: the per-monitor sliders
and the styler's paint are both present in the same screenshot. Closing and
reopening the flyout re-injects. Disabling the mod restores the stock slider
and leaves ShellHost on the same pid.

### The first idea was wrong, and the way it was wrong was instructive

The plan recorded here was to reuse the existing `WinEventProc` and read the
tree from `Window::Current().Content()`. The WinEvent fires exactly as hoped --
`ControlCenterWindow`, on the XAML thread -- and `Window::Current()` is null
there anyway. The window shown beside it says why:
`Windows.UI.Composition.DesktopWindowContentBridge`. The Control Center is a
XAML island, islands have no `CoreWindow`, and there is therefore no ambient
`Window` to read a root from. That is the actual reason this mod reached for
diagnostics in the first place, and no amount of care with the WinEvent hook
would have changed it. The tree has to be entered from an element the host
hands over.

### Four things that each cost a measurement

**Hooks registered outside `Wh_ModInit` are never armed.** `Wh_SetFunctionHook`
only registers; Windhawk applies the registrations once, when `Wh_ModInit`
returns. Five hooks set from a polling thread stayed silent in the very process
that was showing the flyout, which looked exactly like "the class is not the
live code path". A canary on an unrelated telemetry function -- silent too --
is what separated a wrong target from an unarmed hook. Anything registered
later needs `Wh_ApplyHookOperations()`.

**Not every `this` is a COM pointer.** A `produce<>` override's `this` is an
interface on the object and `copy_from_abi` is safe. An implementation member's
`this` is not, and QueryInterface through it faulted ShellHost (0xc0000005),
repeatedly, until Windows stopped restarting it. A try/catch does not save a
call through a bad vtable.

**`IComponentConnector::Connect` is too early.** It hands over the real element
-- it was logged doing so -- but it runs inside `InitializeComponent`, and
merely taking a reference there (copy_from_abi addrefs, the scope releases)
destroyed the half-built view. ShellHost then faulted in no module at all,
calling through what had been freed.

**The tree cannot be walked from `LayoutUpdated`.** Doing it inside a layout
pass ends in a fastfail (0xc0000409). A `DispatcherTimer` runs on the same XAML
thread between passes and is fine.

> Later note, and worth reading before relying on this one. It was measured
> with the diagnostics connection open, and that connection's own retry loop
> produces the identical `0xc0000409` for a completely unrelated reason (see
> DEVELOPING.md, "Never run the TAP connection loop against a starting
> shell"). The shipped mod has no TAP and does walk the tree from
> `LayoutUpdated` on both of its retry paths, without reproducing this. Treat
> the attribution as unproven rather than the fact as established.

`OnGotFocus` avoids all four: it is a `produce<>` override, it runs after
construction, and it is not inside layout.

### Two smaller facts worth keeping

`OnApplyTemplate` is never called on this class and hooking it proves nothing.
`ControlCenterView` is compiled XAML with an `InitializeComponent`, so no
`ControlTemplate` is ever applied and the override is dead code.

The brightness slider's type is `ControlCenter.AsyncSlider`, not
`Windows.UI.Xaml.Controls.Slider`. Searching the tree for the latter returns
nothing, which reads identically to "the tree is not ready yet".

## This is a known limitation, and the ecosystem already documents it

Searching the whole mod catalogue (611 mods) for `InitializeXamlDiagnosticsEx`
returns seven, and reading them settles both the mechanism and the remedy.

`windows-11-taskbar-styler`, in its own README:

> This mod uses XAML diagnostics to inspect and customize the taskbar. However,
> there can only be one XAML diagnostics consumer at a time. If another program
> (such as ExplorerBlurMica or TranslucentTB) tries to use XAML diagnostics
> while this mod is running, there will be a conflict.

So the probe result is not a discovery about this mod -- it is the documented
behaviour of the platform.

### Who shares a process, and what they do about it

| mod | process | `InitializeXamlDiagnosticsEx` |
|---|---|---|
| taskbar-styler | explorer.exe | **hooks it**, setting `xamlDiagnosticsHandling` |
| file-explorer-styler | explorer.exe | **hooks it**, setting `xamlDiagnosticsHandling` |
| cjk-spacer | explorer.exe | only calls it (its XAML path is opt-in, and says why) |
| explorer-command-bar | explorer.exe | **does not use it at all** |
| notification-center-styler | ShellHost.exe | only calls it |
| start-menu-styler | StartMenuExperienceHost / SearchHost | only calls it |
| settings-styler | SystemSettings.exe | only calls it |

The two mods that had to share a process are exactly the two that hook the
function and offer alert / block / allow. Every other one sits in a process of
its own and never had to. The notification-center styler has no such defence,
because until now nothing else used diagnostics in `ShellHost.exe` -- this mod
is the first, and it takes the connection without asking.

The CLSID pattern falls out of the same thing: one id is reused across
styler mods that never meet, and `taskbar-styler` alone carries its own,
because it alone shares a process with another styler. That is hygiene, not
the mechanism -- the probe has a unique CLSID and still breaks the styler.

### The remedy the ecosystem chose

`explorer-command-bar` is the newest of the seven and deliberately refuses the
TAP:

> XAML Diagnostics (InitializeXamlDiagnosticsEx) would be an easier way to
> watch for the command bar, but only one XAML diagnostics consumer can be
> active per process, which makes it conflict with other tools and mods, such
> as Windows 11 File Explorer Styler. That's why it's not used here.

It hooks the host's own code to learn when the element appears and then walks
the tree with the public `VisualTreeHelper` API.

That is what this mod now does. The details of which function to hook, and the
several that cannot be, are above -- `Window::Current().Content()` is not among
the options, because the Control Center is a XAML island.

The alternative -- hook `InitializeXamlDiagnosticsEx` and arbitrate, as the two
explorer stylers do -- would also work, but it means this mod deciding whether
to break somebody else's, which is a worse position to put a user in when the
mod does not need diagnostics in the first place.
