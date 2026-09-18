#!/bin/sh
# Compiles and installs per-monitor-brightness.wh.cpp the way Windhawk does,
# then signals the engine to hot-reload. Diagnostic tooling, not part of the mod.
set -e
cd "$(dirname "$0")"

CXX="/c/Program Files/Windhawk/Compiler/bin/clang++.exe"
INC="/c/Program Files/Windhawk/Compiler/include"
ENGDIR="/c/Program Files/Windhawk/Engine/1.7.3/64"
MODS="/c/ProgramData/Windhawk/Engine/Mods/64"
SRCDIR="/c/ProgramData/Windhawk/ModsSource"

NAME="local@per-monitor-brightness_1.7_9$(date +%H%M%S).dll"

"$CXX" --target=x86_64-w64-mingw32 -shared -O2 -std=c++23 \
  -DUNICODE -D_UNICODE -mwindows \
  -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 \
  -D__USE_MINGW_ANSI_STDIO=0 -DWH_MOD \
  '-DWH_MOD_ID=L"local@per-monitor-brightness"' '-DWH_MOD_VERSION=L"1.7"' \
  -include windhawk_api.h -I"$INC" \
  -L"$ENGDIR" \
  -Wl,--export-all-symbols \
  -o "$MODS/$NAME" \
  -x c++ per-monitor-brightness.wh.cpp -x none \
  -lwindhawk -ldxva2 -lole32 -loleaut32 -lwbemuuid -luuid -lruntimeobject

cp per-monitor-brightness.wh.cpp "$SRCDIR/local@per-monitor-brightness.wh.cpp"

powershell.exe -NoProfile -Command "
  \$k = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods\local@per-monitor-brightness';
  Set-ItemProperty \$k -Name 'LibraryFileName' -Value '$NAME';
  Set-ItemProperty \$k -Name 'Disabled' -Value 0 -Type DWord;
  Set-ItemProperty \$k -Name 'SettingsChangeTime' -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
" >/dev/null
echo "installed $NAME"
