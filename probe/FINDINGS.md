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

## Where a fix could go

The mod needs the TAP for one thing: to notice `ControlCenterView` appearing
and to get onto the XAML thread. Both may be reachable without diagnostics --
the mod already has a `WinEventProc` that sees the Control Center being shown,
and `RunOnXamlThread` to get onto the right thread, from where
`Window::Current().Content()` gives a tree that can be walked directly. If
that works, the TAP can go, and with it the conflict.

That is a hypothesis, not a result. It needs the same treatment as everything
above: build it in the probe first, measure it with the red-pixel signal, and
re-run the control every time.

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

That is the fix for this mod, and it needs no new machinery: `WinEventProc`
already sees `EVENT_OBJECT_SHOW` for the Control Center, and `RunOnXamlThread`
already gets onto the right thread, from where `Window::Current().Content()`
gives a tree to walk. Dropping the TAP removes the conflict by construction
rather than winning a fight over a single-consumer resource.

The alternative -- hook `InitializeXamlDiagnosticsEx` and arbitrate, as the two
explorer stylers do -- would also work, but it means this mod deciding whether
to break somebody else's, which is a worse position to put a user in when the
mod does not need diagnostics in the first place.
