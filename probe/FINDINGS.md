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
