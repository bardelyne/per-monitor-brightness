#!/bin/sh
# Regenerates per-monitor-brightness.wh.cpp, compiles and installs it the way
# Windhawk does, then signals the engine to hot-reload. Diagnostic tooling, not
# part of the mod. Needs an elevated shell: the mod's keys live under HKLM.
set -e
cd "$(dirname "$0")"

CXX="/c/Program Files/Windhawk/Compiler/bin/clang++.exe"
INC="/c/Program Files/Windhawk/Compiler/include"
# The newest engine installed, rather than a version that goes stale on the
# next Windhawk update.
ENGDIR="$(ls -d "/c/Program Files/Windhawk/Engine/"*/64 | sort -V | tail -n 1)"
MODS="/c/ProgramData/Windhawk/Engine/Mods/64"
SRCDIR="/c/ProgramData/Windhawk/ModsSource"
KEY='HKLM\SOFTWARE\Windhawk\Engine\Mods\local@per-monitor-brightness'

# Installing a stale splice is an easy mistake to make and a confusing one to
# debug, so always build from the current template and engine.
sh ./build_mod.sh

VERSION="$(sed -n 's|^// @version *||p' per-monitor-brightness.wh.cpp | tr -d '\r')"
# A loaded DLL is memory-mapped and cannot be overwritten, so every build
# gets a new name.
NAME="local@per-monitor-brightness_${VERSION}_9$(date +%H%M%S).dll"

"$CXX" --target=x86_64-w64-mingw32 -shared -O2 -std=c++23 \
  -DUNICODE -D_UNICODE -mwindows \
  -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 \
  -D__USE_MINGW_ANSI_STDIO=0 -DWH_MOD \
  '-DWH_MOD_ID=L"local@per-monitor-brightness"' "-DWH_MOD_VERSION=L\"$VERSION\"" \
  -include windhawk_api.h -I"$INC" \
  -L"$ENGDIR" \
  -Wl,--export-all-symbols \
  -o "$MODS/$NAME" \
  per-monitor-brightness.wh.cpp \
  -lwindhawk -ldxva2 -lole32 -loleaut32 -lwbemuuid -luuid -lruntimeobject

cp per-monitor-brightness.wh.cpp "$SRCDIR/local@per-monitor-brightness.wh.cpp"

# reg.exe rather than PowerShell. MSYS would otherwise rewrite its /v-style
# switches into Windows paths.
export MSYS_NO_PATHCONV=1
reg add "$KEY" /v LibraryFileName /t REG_SZ /d "$NAME" /f >/dev/null
reg add "$KEY" /v Disabled /t REG_DWORD /d 0 /f >/dev/null
reg add "$KEY" /v SettingsChangeTime /t REG_DWORD /d "$(date +%s)" /f >/dev/null
echo "installed $NAME"
