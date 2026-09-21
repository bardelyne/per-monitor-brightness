# Developing

## Layout

| File | Purpose |
|---|---|
| `per-monitor-brightness.wh.cpp` | **The deliverable.** Single-file mod; paste into Windhawk, or PR to `ramensoftware/windhawk-mods`. Generated — do not edit directly. |
| `mod_template.cpp` | The mod source: UI injection, settings, Win32 listeners. |
| `brightness_engine.h` | The brightness engine. Independently testable, no UI or Windhawk dependency. |
| `engine_test.cpp` | Standalone harness for the engine. |
| `probe/layout-probe.wh.cpp` | Read-only dump of where the native slider group sits in `L1Grid`. What the panel-position setting was designed against. |
| `stage_test.cpp` | Single-threaded bisect of the engine's dependencies: COM, WMI and DDC/CI in order, each step printed unbuffered. What told the startup crash apart from a threading bug. |
| `build_mod.sh` | Splices the engine into the template to produce the `.wh.cpp`. |
| `install_mod.sh` | Compiles and installs the mod without the Windhawk GUI, then hot-reloads it. Needs an elevated shell. |

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

`stage_test.cpp` builds with that same line. Reach for it when the
engine takes a process down and the stack says nothing useful: it makes
the same COM, WMI and DDC/CI calls the engine does, one at a time on one
thread, printing before each one with the buffer off. The last line
printed is the call that did it, and building it at both `-O0` and `-O2`
separates a real fault from an optimiser artefact.

`__USE_MINGW_ANSI_STDIO=0` is not optional. Without it, `%s` in a **wide**
`printf` means a *narrow* string and passing `wchar_t*` walks off into garbage.

## Building and installing without the GUI

`install_mod.sh` compiles with Windhawk's own clang against
`Engine\<ver>\64\windhawk.lib`, drops the DLL into `Engine\Mods\64` under a
fresh name, points the registry at it and bumps `SettingsChangeTime` so the
service hot-reloads. It needs an elevated shell, since the mod's registry keys
live under HKLM.

Two things it must get right, both of which fail silently otherwise:

- **`-Wl,--export-all-symbols`.** Without it nothing is exported, Windhawk
  loads the DLL, finds no `Wh_ModInit`, and the mod simply never runs. It looks
  exactly like a mod that loads fine and does nothing.
- **A new DLL filename each build.** A loaded DLL is memory-mapped and cannot
  be overwritten.

Always verify the mod actually *ran* -- a breadcrumb it writes, or observable
behaviour -- rather than trusting that the DLL appears in the process's module
list.

## Debugging

Turn logging on for the mod (Windhawk's mod settings, or `LoggingEnabled=1`
under `HKLM\SOFTWARE\Windhawk\Engine\Mods\<id>` plus a `SettingsChangeTime`
bump) and read the output **in Windhawk's own log window**.

DebugView will not work, and this is worth knowing before spending an hour on
it. `Wh_Log` is `OutputDebugString` underneath, and the Windhawk *service* owns
`Global\DBWIN_BUFFER_READY` for as long as it runs — that is how the log
window gets its content. A second listener creates the objects with
`ERROR_ALREADY_EXISTS`, reports itself as listening, and then receives nothing,
because the events are auto-reset and the service is always waiting on them.
Stopping the tray process does not help; the service is the owner. Verified by
emitting a canary from another process and watching both listeners.

If you need log output somewhere a script can read it, write to a file from the
mod rather than trying to tap the debug channel.

Every hardware write is logged with its transport, value, success flag and
duration, which is usually enough to tell a misbehaving monitor from a mistake
in the mod.

## Things that cost real time

Notes for anyone (including future me) touching this again. None of it is
documented anywhere obvious.

### The Control Center lives in `ShellHost.exe`, not `ShellExperienceHost.exe`

And it is **not** an AppContainer — medium integrity, so the mod can call
`dxva2` and WMI directly in-process. No helper process needed.

### How long a `ControlCenterView` actually lives

Neither "built once at shell start" nor "rebuilt on every open", and this mod
has at different times believed both.

It is built the first time Quick Settings is shown and then kept, shown and
hidden, across subsequent opens — which is why the second open is instant, and
why a view that is being kept runs no layout while it sits open, so waiting on
`LayoutUpdated` means waiting until the panel is next *closed*. Leave it closed
for a while and the shell discards it; the next open constructs a new one.

That last part is the whole of the "flicker if I have not opened it in a while,
none if I just did" report. It also means injection bookkeeping has to prune
trees the shell has torn down rather than assume one view forever.

There is no "opened" event on the element, so inject as soon as the view is
reported and treat `Loaded`/`LayoutUpdated` as retries.

### Only one XAML diagnostics consumer per process

This is the single most important thing to know before changing how the mod
finds the Control Center, and it is documented upstream in the Windows 11
Taskbar Styler's own readme rather than by Microsoft.

The mod used to find `ControlCenterView` through an XAML diagnostics TAP. It
worked, and it quietly broke the Windows 11 Notification Center Styler, which
wants the same slot in the same process: whichever mod connects second gets
nothing and says nothing. A user reported it as "your mod stops my theming",
which is exactly right.

So discovery is a symbol hook now, the way `explorer-command-bar` does it: hook
a function in the host's own DLL, take the element from the arguments, and walk
the tree with the public `VisualTreeHelper`. No slot to contend for. The
conflict is gone by construction rather than by cooperation, and the `probe/`
directory has the experiment that proved both halves of it.

Two things the replacement needs that the TAP did not:

- **The pointer must really be a COM interface on the view.** A `produce<>`
  override's `this` is. An implementation member's `this` is not, and
  `QueryInterface` through it faults the host. `copy_from_abi` does **not** QI
  -- it addrefs and stores the pointer as-is -- so it is the wrong tool here
  even though the happy path hides that.
- **Symbol hooks registered outside `Wh_ModInit` are not armed.** Windhawk
  applies what `Wh_ModInit` registers, once, when it returns. Anything
  registered later -- from inside a `LoadLibraryExW` hook, say -- sits there
  doing nothing until `Wh_ApplyHookOperations()` is called by hand. Five hooks,
  including a canary that had nothing to do with the view, stayed silent in the
  very process that was showing the flyout.

### Hook kernelbase's `LoadLibraryExW`, not kernel32's

kernel32's export is a forwarder. The WinRT activation path that maps
`ControlCenter.dll` calls kernelbase directly, so a hook on kernel32 never sees
the load — which is the kind of thing that sends you off building a polling
thread to work around it.

Also: resolve the module with `GetModuleHandleW` afterwards rather than
trusting the returned handle. `LOAD_LIBRARY_AS_DATAFILE` and friends, which
resource lookups use, return a mapping that is not executable code and is never
in the loader's module list.

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
blocking until done.

The contract is that when `Wh_ModUninit` returns, no code from the mod image may
still be running *or scheduled to run*. `CoreDispatcher::RunAsync` cannot satisfy
that — it queues, so the best you get is a wait with a timeout, and on timeout
you return anyway with your code still queued. Use a synchronous hop instead: a
`WH_CALLWNDPROC` hook on the target thread plus `SendMessage` of a registered
message, which has already run by the time the call returns. Same reason the
notification center styler has `RunFromWindowThread`.

That also means handlers must be reachable at teardown. A revoker owned only by
the lambda it was captured in cannot be revoked from anywhere else — keep them
in mod-owned globals.

### Globals with destructors will crash the host at process exit

`Wh_ModUninit` runs on unload, but **not** when the host process exits. There,
the OS terminates every other thread first and then runs global destructors
alone on the shutdown thread under the loader lock.

A global holding a `std::thread` is the worst case: nothing called `Stop()`, the
thread is still `joinable()`, and `~thread()` calls `std::terminate()` — the host
aborts on every sign-out. Globals holding XAML objects or COM references are the
same class of problem, releasing them after their apartment is gone.

Mark them `[[clang::no_destroy]]` and clean up explicitly in `Wh_ModUninit`. See
[the wiki page](https://github.com/ramensoftware/windhawk/wiki/Global-objects-and-process-shutdown).

### A mod must not touch COM before the host has started

This one cost a day, and every intuition about it was wrong.

Connecting to WMI activates a COM object, and **the first COM activation in a
process implicitly initialises COM security process-wide**. `Wh_ModInit` runs
before ShellHost's own startup code, so a mod that talks to WMI there wins that
race. ShellHost's own `CoInitializeSecurity` then fails `RPC_E_TOO_LATE`, and it
fast-fails: the host aborts, restarts, does the same thing again, and the shell
never comes back. Recovery is disabling the mod.

Not making your own `CoInitializeSecurity` call is **not** sufficient. Any
activation is enough. See the section below, which is still true and still not
enough on its own.

What makes this hard to find:

- Enabling the mod into an already-running shell works perfectly, every time.
  Only a *freshly starting* ShellHost breaks -- sign-out, sign-in, or
  `taskkill /f /im ShellHost.exe`. Test that path explicitly.
- The fault looks like memory corruption and is not. Event Viewer reports
  `0xc0000409` in `ucrtbase.dll`, which reads as a stack buffer overrun, but
  the subcode (`Exception Data` = `7`) is `FAST_FAIL_FATAL_APP_EXIT` -- plain
  `abort()`. Always read the subcode.
- Nothing throws, so exception guards catch nothing and prove nothing.

The fix is to do no COM until the host is up. The gate used to be "a XAML
window exists in the process", which is the condition the TAP injection already
waited for. It is now "a Control Center exists in this process" -- the engine
starts from the `ControlCenter.dll` discovery hook, or from the view's
`Connect`/`OnGotFocus`, whichever fires first. Tighter in both directions: a
process that hosts XAML but never a Control Center pays nothing, and the engine
is warm before the first open instead of enumerating while the flyout is
already on screen.

One consequence worth keeping in mind if the host ever changes. "ControlCenter.dll
is mapped at `Wh_ModInit`" means "the mod was enabled into a running shell"
only because ShellHost does not statically import it. If a future build does,
that would be true on every cold start and the old race is back, so the
already-mapped path checks whether the process yet owns a top-level window
before warming the engine.

If you need to find something like this again, bisect rather than theorise: put
a temporary bitmask setting in the mod that switches subsystems off, then drive
it from the registry (`HKLM\SOFTWARE\Windhawk\Engine\Mods\<id>\Settings`,
then bump `SettingsChangeTime` to hot-reload) and count crash events in the
Application log. That located it in four rounds after three wrong guesses.

### Never call `CoInitializeSecurity` from a mod

It is process-wide, and a mod's thread starting from `Wh_ModInit` will usually
win the race against the host's own startup — permanently imposing your settings
on the host and making its own call fail `RPC_E_TOO_LATE`. `CoSetProxyBlanket` on
the specific proxy is what actually governs your calls.

### Never run the TAP connection loop against a starting shell

This mod no longer opens a diagnostics connection at all (see above), so none
of this is in the source any more. It is kept because the failure is generic to
mods that do, and because its fault code is easy to misattribute -- including
by this project, which recorded an unrelated "cannot walk the tree from
`LayoutUpdated`" constraint that was probably this.

`InitializeXamlDiagnosticsEx` is called in a loop over `VisualDiagConnection1`,
`2`, ... because there is no way to ask which diagnostics slot is free. That is
borrowed from mods that inject into an already-running shell, where the first or
second name takes.

On a *starting* shell the failure is not "name in use", it is `ERROR_NOT_FOUND`
(`0x80070490`) — XAML is not up yet, and no connection name will change that.
Running the loop to its limit then makes ten thousand diagnostics registrations
in a process whose XAML has not initialised, and the host `abort()`s about a
second later, when it does. In Event Viewer that is `0xc0000409` in
`ucrtbase.dll` with subcode `7` (`FAST_FAIL_FATAL_APP_EXIT`), which reads like a
stack overrun and is not one.

Break out of the loop on `ERROR_NOT_FOUND`, and do not enter it at all until a
XAML window exists in the process.

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

### A DDC/CI write stalls the shell's UI thread

Coalescing keeps the queue short, but it does not stop the worker writing
continuously: as soon as one write finishes the next newest value goes out, so
a slider drag puts a write on the wire roughly every 60 ms for its whole
duration. An I2C transaction serialises against the display driver, and while
one is in flight the shell's UI thread stalls.

The result is that the slider stops tracking the pointer. Drag quickly to the
right-hand end and the thumb falls behind and settles around 60-something
percent, which looks exactly like the value drifting on its own afterwards --
it is not drifting, it never got there.

Measured on this machine, fast drag from ~50% to past the end:

| hardware writes | where the thumb lands |
|---|---|
| on, unthrottled | 64%, six times out of six |
| suppressed entirely | 100%, every time |
| throttled to one per 140 ms | 100%, every time |

The internal panel never showed it, because a WMI write is ~5 ms rather than
~60 ms. That contrast is the cleanest way to test it: drag both sliders the
same way and compare.

So cap the DDC write rate (`kDdcCooldown`) and let the queue coalesce in the
gap. The newest value always wins, so the value the user released on is still
the one that lands, just slightly later.

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

## Submitting an update to Windhawk

Rules are from the [windhawk-mods
README](https://github.com/ramensoftware/windhawk-mods#submitting-a-mod-update):

- The PR must consist of changes to **exactly one file**,
  `mods/per-monitor-brightness.wh.cpp`.
- `@version` must be bumped.
- `@github` must match the PR author, so only the original submitter can update
  the mod.
- The commit message becomes the changelog entry shown in the catalogue, so
  describe what changed in the new version.
