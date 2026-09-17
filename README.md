# Per-monitor brightness in Quick Settings

A Windhawk mod that adds a labelled brightness slider for every connected display
to the Windows 11 Quick Settings panel, each showing its live level and carrying
the shell's own animated brightness icon.

![Per-monitor brightness sliders in Quick Settings](screenshot.png)

Windows gives you exactly one brightness slider no matter how many monitors you
have, and on a desktop it gives you none at all. Each display here is driven over
whichever channel actually works for it: **DDC/CI** (VCP `0x10`) for external
monitors — the I2C side-channel in the video cable that the monitor's own
on-screen menu uses — and **WMI** for a laptop's internal panel, the same path
the stock slider takes. A display that answers neither is listed as
uncontrollable rather than silently dropped.

Built and verified on Windows 11 25H2 (26200.9457), Windhawk 1.7.3.

## What it does

- One titled slider per display, showing the live percentage.
- Ordered left to right to match how the monitors sit on your desk.
- Displays keyed by EDID device path, so the right slider follows the right
  monitor across hotplug, reordering and reboots.
- Each slider snaps to what its monitor can actually represent — panels do not
  all use a 0–100 scale.
- Function keys move the sliders live, and can optionally drive external
  monitors too, which they cannot do on their own.
- Monitors plugged in or unplugged are picked up immediately.
- Values re-read whenever the panel opens, catching changes made elsewhere.
- The stock brightness slider can be hidden, since it duplicates the internal
  panel's row.

## Settings

| Setting | Default | Effect |
|---|---|---|
| Hide the built-in brightness slider | on | Collapses the stock row, which only controls the internal panel |
| Laptop brightness keys control every monitor | `relative` | `relative` shifts others by the same delta (preserving offsets); `match` sets them equal; `off` disables |
| Verbose logging | off | Logs every brightness write; useful when a monitor will not respond |

## Layout

| File | Purpose |
|---|---|
| `per-monitor-brightness.wh.cpp` | **The deliverable.** Single-file mod; paste into Windhawk, or PR to `ramensoftware/windhawk-mods`. Generated — do not edit directly. |
| `mod_template.cpp` | The mod source: UI injection, settings, Win32 listeners. |
| `brightness_engine.h` | The brightness engine. Independently testable, no UI or Windhawk dependency. |
| `engine_test.cpp` | Standalone harness for the engine. |
| `build_mod.sh` | Splices the engine into the template to produce the `.wh.cpp`. |

Windhawk mods must be a single source file, hence the splice step. Keeping the
engine separate is what allowed it to be tested outside the shell — which is how
a COM-apartment crash was caught before it ever reached ShellHost.

## Building

Uses Windhawk's own bundled toolchain, so the mod compiles exactly as Windhawk
will compile it:

```sh
./build_mod.sh          # regenerate per-monitor-brightness.wh.cpp
```

```sh
CXX="/c/Program Files/Windhawk/Compiler/bin/clang++.exe"
INC="/c/Program Files/Windhawk/Compiler/include"

# mod (compile check only; Windhawk does the real build)
"$CXX" -x c++ -std=c++23 -target x86_64-w64-mingw32 \
  -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 \
  -D__USE_MINGW_ANSI_STDIO=0 -DWH_MOD -DWH_EDITING \
  -include windhawk_api.h -I"$INC" -Wall \
  -shared -o per-monitor-brightness.dll per-monitor-brightness.wh.cpp \
  -ldxva2 -lole32 -loleaut32 -lwbemuuid -luuid -lruntimeobject -lshlwapi

# engine harness (run it: prints displays, stress-tests coalescing, restores)
"$CXX" -x c++ -std=c++23 -target x86_64-w64-mingw32 \
  -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 \
  -D__USE_MINGW_ANSI_STDIO=0 -O2 -Wall \
  engine_test.cpp -o engine_test.exe \
  -ldxva2 -lole32 -loleaut32 -lwbemuuid -luuid -lgdi32 -luser32 -static
```

`__USE_MINGW_ANSI_STDIO=0` is not optional. Without it, `%s` in a **wide**
`printf` means a *narrow* string and passing `wchar_t*` walks off into garbage.

## Things that cost real time

Notes for anyone (including future me) touching this again. None of it is
documented anywhere obvious.

### The Control Center lives in `ShellHost.exe`, not `ShellExperienceHost.exe`

And it is **not** an AppContainer — medium integrity, so the mod can call
`dxva2` and WMI directly in-process. No helper process needed.

### Injection timing

The Control Center tree is built once at shell start and then merely shown and
hidden. There is no "opened" event on the element. Waiting on `LayoutUpdated`
means waiting until the panel is next *closed*. Inject as soon as the visual
tree reports `ControlCenterView`.

### `RowDefinition` needs a strong reference

XAML only keeps projection peers alive for elements in the live visual tree. A
`RowDefinition` is not a `UIElement`, so a `weak_ref` to one is always dead by
teardown even though the row is still sitting in the collection — the row then
leaks, one empty row per enable/disable cycle. `RowDefinitions().IndexOf()` is
also unreliable; scan by identity.

### Teardown is a crash risk, not a tidiness concern

The sliders' `ValueChanged` handlers are code inside the mod DLL. Leaving them
registered after unload means dragging a leftover slider calls into freed
memory. Detach handlers, remove the elements, and do it **on the XAML thread**,
blocking until done — then drain the dispatcher with a no-op posted at `Low`
priority, which only runs once everything queued ahead of it has.

### WinUI2 cannot be included as shipped

Windhawk ships the WinUI2 C++/WinRT projection under `winrt/winui2/`, but its
headers `#include "winrt/impl/..."`, which resolves to the **WinUI3** copies at
the default location. Declarations and definitions then disagree. Fixing it
needs machine-specific `-I` flags, which would make the mod non-portable — so
the three interfaces actually needed are declared by hand from that projection's
GUIDs and vtable order. Note `__declspec(uuid)` is an MSVC extension clang
ignores on mingw, so `__uuidof` does not work; use explicit `GUID` constants.

### XAML controls are composable

`RoActivateInstance` on `AnimatedIcon` returns `E_NOTIMPL`. The class *is*
registered — `RoGetActivationFactory` succeeds — but XAML controls are built
through `IAnimatedIconFactory::CreateInstance(outer, inner, instance)` with a
null outer, not through `IActivationFactory::ActivateInstance`.

### The brightness Lottie is progress-driven

Its markers are `Brightness_at_0 = 0.0` and `Brightness_at_100 = 1.0` — endpoint
labels, not a state machine. `AnimatedIcon` plays to a normalized progress when
the state string parses as a number, which is what makes a continuous sun.

### Monitors do not use a 0-100 scale

A Samsung G32 reports VCP `0x10` max as **50**. Map percentages onto each
monitor's own range, and snap the slider to that granularity — otherwise three
slider positions all write the same raw value.

### `GetMonitorCapabilities` lies

It fails with `0xC0262C07` on monitors that answer raw VCP reads and writes
perfectly well. Use the low-level `GetVCPFeatureAndVCPFeatureReply` /
`SetVCPFeature` and do not gate on the high-level API.

### DDC/CI is slow and one-way

~60 ms per write, and the bus saturates during a drag — writes land ~62 ms apart
while each *takes* ~62 ms. Coalescing (keep only the newest value per display)
is mandatory or the UI thread stalls. There is also no notification channel:
DDC/CI only ever answers what the host asks, so a change made on the monitor's
own OSD is unknowable until polled.

### Echo rejection must be temporal, not value-based

The internal panel raises `WmiMonitorBrightnessEvent` for *our* writes too.
Matching on the value fails: coalescing means the event for a value we wrote can
arrive after we have written a newer one, so the values disagree and the echo
looks like an external change — which then shoves the other monitors around at
random. Mute per display, by time, stamped when the change is *requested*.

Safe because following only ever writes to the *other* displays: a keypress
changes the internal panel and we respond on the externals, never back onto the
panel, so its mute window is only ever refreshed by the user's own dragging.

### `WM_DISPLAYCHANGE` needs a real top-level window

A message-only window never receives it. Park an invisible `WS_POPUP` on a
thread with a message loop. That same thread hosts the `WINEVENT_OUTOFCONTEXT`
hook used to notice the flyout opening — filter it to the `ControlCenterWindow`
class, or the slider's own thumb tooltip (`Xaml_WindowedPopupClass`) trips it on
every drag.

## Licence

GPLv3. The XAML-diagnostics plumbing (`VisualTreeWatcher` / `WindhawkTAP` /
`InjectWindhawkTAP`) is adapted from m417z's "Windows 11 Notification Center
Styler", which is GPLv3.

## Publishing to Windhawk

Rules are from the [windhawk-mods
README](https://github.com/ramensoftware/windhawk-mods#submitting-a-new-mod):

- The PR must consist of **exactly one file**, `mods/<mod-id>.wh.cpp`, where
  `<mod-id>` matches the `@id` in the header — so `mods/per-monitor-brightness.wh.cpp`.
- `@github` is **mandatory** and must match the PR author's GitHub profile.
- Mods with no `@license` are submitted under MIT. This one declares `GPL-3.0`,
  because the XAML-diagnostics plumbing is adapted from GPLv3 code.
- For later updates, bump `@version` and describe the change in the commit
  message — that text becomes the mod's changelog entry.

Before submitting, replace the `__GITHUB_USER__` placeholders (mod header and
the screenshot URL in the mod's readme block) and rebuild:

```sh
sed -i 's/__GITHUB_USER__/your-username/g' mod_template.cpp && ./build_mod.sh
```

The screenshot must be at a public absolute URL — published mods use either
`raw.githubusercontent.com` from their own repo or `i.imgur.com`. Relative paths
do not work in the mod's readme block.

As of this writing nothing in the catalogue does per-monitor brightness: a
search of all 608 mods for "quick settings" or "ddc" returns nothing.
