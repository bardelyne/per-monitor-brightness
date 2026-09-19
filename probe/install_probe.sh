#!/bin/sh
# Compiles and installs the probe the way Windhawk does. A fresh DLL name per
# build, because the engine will not reload a file it already has mapped.
set -e
cd "$(dirname "$0")"

CXX="/c/Program Files/Windhawk/Compiler/bin/clang++.exe"
INC="/c/Program Files/Windhawk/Compiler/include"
ENGDIR="/c/Program Files/Windhawk/Engine/1.7.3/64"
MODS="/c/ProgramData/Windhawk/Engine/Mods/64"
SRCDIR="/c/ProgramData/Windhawk/ModsSource"

NAME="local@styler-probe_1.0_9$(date +%H%M%S).dll"

"$CXX" --target=x86_64-w64-mingw32 -shared -O2 -std=c++23 \
  -DUNICODE -D_UNICODE -mwindows \
  -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 \
  -D__USE_MINGW_ANSI_STDIO=0 -DWH_MOD \
  '-DWH_MOD_ID=L"local@styler-probe"' '-DWH_MOD_VERSION=L"1.0"' \
  -include windhawk_api.h -I"$INC" \
  -L"$ENGDIR" \
  -Wl,--export-all-symbols \
  -o "$MODS/$NAME" \
  -x c++ styler-probe.wh.cpp -x none \
  -lwindhawk -ldxva2 -lole32 -loleaut32 -lwbemuuid -luuid -lruntimeobject

cp styler-probe.wh.cpp "$SRCDIR/local@styler-probe.wh.cpp"

powershell.exe -NoProfile -Command "
  \$k = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods\local@styler-probe';
  Set-ItemProperty \$k -Name 'LibraryFileName' -Value '$NAME';
  Set-ItemProperty \$k -Name 'SettingsChangeTime' -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
" >/dev/null
echo "installed $NAME"
