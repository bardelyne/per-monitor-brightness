# Per-monitor brightness and more in Quick Settings

A [Windhawk](https://windhawk.net/) mod that adds a labelled brightness slider
for every connected display to the Windows 11 Quick Settings panel, each
carrying the shell's own animated brightness icon -- plus contrast, volume,
input and power controls where the monitor supports them, and sliders that set
every display at once.

![Per-monitor brightness sliders in Quick Settings](screenshot.png)

Windows gives you exactly one brightness slider no matter how many monitors you
have, and on a desktop it gives you none at all.

Each display is driven over whichever channel actually works for it:

- **External monitors** use **DDC/CI** (VCP code `0x10`), the I2C side-channel
  in the video cable that the monitor's own on-screen menu uses. Most monitors
  made in the last decade support it; some budget panels and some USB-C docks
  do not.
- **Laptop internal panels** use **WMI**, the same path the stock slider takes.

A display that answers neither is listed as uncontrollable rather than being
silently dropped.

**Contrast** (VCP `0x12`), **power** (`0xD6`), **volume** (`0x62`) and
**input** (`0x60`) are DDC/CI as well, and each is offered only for monitors
that answer it. Which inputs a monitor has comes from its capabilities string,
read once per session in the background.

## Features

- One titled brightness slider per display. The title shows the live
  percentage of every slider under it, in order -- `Samsung · 80% · 50% · 30%`
  for brightness, contrast and volume.
- A contrast slider for each monitor that supports it.
- "All displays" brightness and contrast sliders that set every display to the
  same level.
- A power button at the end of the brightness slider of monitors that support
  it, offered only while another display is connected.
- A volume slider for monitors with audio, with the shell's own animated
  speaker icon.
- Input buttons (HDMI 1, HDMI 2, DP 1...) that switch a monitor's input.
- Two layouts: each display's extra controls in a dropdown (the default), or
  everything visible.
- The mouse wheel moves whichever slider is under the pointer, by a step you
  choose.
- Optionally, clicking a slider's icon jumps to a level: left, middle and right
  button each have their own (0, 50 and 100 by default).
- Per-display settings: a name of your own, hiding a display altogether, or
  hiding just some of its controls. Two monitors of the same model are
  numbered left to right, and hovering a name shows its device id.
- Ordered left to right to match how the monitors sit on your desk.
- Displays keyed by EDID device path, so the right slider follows the right
  monitor across hotplug, reordering and reboots.
- Each slider snaps to what its monitor can actually represent. Panels do not
  all use a 0–100 scale — a Samsung G32 reports 0–50 — so offering 1% steps
  would just mean several slider positions that write the same value.
- Function keys move the sliders live, and can optionally drive external
  monitors too, which they cannot do on their own.
- Monitors plugged in or unplugged are picked up immediately.
- Values re-read whenever the panel opens, catching changes made elsewhere.
- The stock brightness slider can be hidden, since it duplicates the internal
  panel's row.
- The sliders sit with the Windows ones by default, and can be moved above them
  or down to the bottom of the flyout.
- Displays that cannot be controlled can be hidden.

## Settings

Everything the mod adds can be shown or hidden on its own, and each switch
covers only what it names.

| Setting | Default | Effect |
|---|---|---|
| Hide the built-in brightness slider | on | Collapses the stock row, which only controls the internal panel |
| Where to put the sliders | `belowSliders` | Joins the card holding the volume and stock brightness rows. `aboveSliders` puts it first in that card; `bottom` is under the settings button, outside the card, as before 2.1 |
| Hide displays that cannot be controlled | off | Leaves out displays that answer neither transport. They are still re-checked, so one that starts answering appears |
| Laptop brightness keys control every monitor | `off` | `relative` shifts others by the same delta, preserving their offsets; `match` sets them all equal |
| Layout | `dropdown` | `dropdown` shows each display's brightness slider and tucks the rest behind a chevron; `expanded` shows everything |
| Show brightness sliders | on | Each display's own **brightness** slider. Off, brightness is set with the all-displays slider alone; contrast, volume and input stay. The power button sits at the end of the brightness slider, so it goes with it |
| Show brightness for all displays | on | One slider setting every display's brightness. Appears with two or more displays, or when the brightness sliders are off |
| Show contrast sliders | on | Each monitor's own contrast slider, where supported |
| Show contrast for all displays | on | One slider setting every supporting monitor's contrast. Appears with two or more of them, or when the contrast sliders are off |
| Show power buttons | on | Turns a monitor off and back on over DDC/CI, where supported |
| Show volume sliders | on | Each monitor's speaker or headphone volume, where supported |
| Show input buttons | on | One button per input the monitor lists; pressing one switches to it |
| Mouse wheel step | 5 | Percentage points per notch; 0 leaves the wheel to the flyout |
| Click an icon to jump to a level | off | Left, middle and right click levels are settings of their own |
| Per-display settings | — | Text to look for in a display's name or device id, then a name to show and what to hide |

"Laptop brightness keys control every monitor" is off by default because
Windows raises the same signal for a brightness key, for the power plan's AC and
battery levels, and for idle dimming, with no way to tell them apart — so with
it on, unplugging the charger would also write to every external monitor.
Unlike everything else this mod does, that write is not undone by turning the
setting off or disabling the mod: the monitor stores the value itself.

## Limitations

- A DDC/CI write takes roughly 50–60 ms and the bus saturates while dragging, so
  an external monitor visibly steps rather than fades. The internal panel is
  around ten times faster and looks smooth. Nothing can be done about this from
  software — it is the cable.
- DDC/CI has no notification channel: a monitor only ever answers what the host
  asks it. Brightness changed using the monitor's own buttons cannot be
  detected, and only shows up the next time the panel is opened.
- Not every monitor implements DDC/CI correctly. One that does not answer at
  all is listed as uncontrollable and gets no slider; one that answers but
  misbehaves shows up as writes reporting `ok=0`, which you can see by enabling
  logging for the mod in Windhawk's settings. Some monitors acknowledge a
  power or input write and ignore it.
- The power button turns a monitor off over DDC/CI. Most monitors still listen
  in that state and come back when it is pressed again; one that does not has
  to be switched on with its own button.
- Switching a monitor's input hands it to whatever is on that input. Some
  monitors keep answering DDC/CI on the input they left, so you can switch back
  from here; others need their own buttons.
- A monitor that was asleep, switched to another input, or behind a dock still
  enumerating when you signed in will fail that first check. It is retried
  rather than written off: opening the panel again re-probes it, backing off
  from 30 seconds to at most 16 minutes between attempts, and unplugging and
  replugging it starts over immediately.

## Installing

Install [Windhawk](https://windhawk.net/), then either install this mod from the
[mods catalogue](https://windhawk.net/mods), or create a new mod and paste in
[`per-monitor-brightness.wh.cpp`](per-monitor-brightness.wh.cpp).

Requires Windows 11 with the redesigned Control Center.

Developed and tested on **25H2 (build 26200)**, where the Control Center is
hosted by `ShellHost.exe`. Earlier Windows 11 builds host it in
`ShellExperienceHost.exe` and are not supported -- see the compatibility note
in the mod's own readme for why that process is left out.

## Development

See [DEVELOPING.md](DEVELOPING.md) for the build process and notes on the
undocumented platform behaviour this relies on.

## Licence

GPLv3, and adapted in part from m417z's
[Windows 11 Notification Center Styler](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/windows-11-notification-center-styler.wh.cpp),
which is GPLv3.

What is borrowed has changed. This mod used to carry that mod's
XAML-diagnostics plumbing — `VisualTreeWatcher`, `WindhawkTAP`,
`InjectWindhawkTAP` — and none of it is here any more: XAML diagnostics allows
one consumer per process, so holding the slot stopped that very mod theming the
Control Center. Discovery is a symbol hook now.

What remains adapted is the synchronous hop onto the XAML thread in
`RunOnXamlThread`: a `WH_CALLWNDPROC` hook plus `SendMessage` of a registered
message, which is the approach behind its `RunFromWindowThread`.
