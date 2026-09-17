#!/bin/sh
# Windhawk mods are a single source file, so splice the independently testable
# engine into the mod template at its marker.
set -e
OUT=per-monitor-brightness.wh.cpp
awk '
  /^\/\/__ENGINE_INLINE__$/ {
    while ((getline line < "brightness_engine.h") > 0) {
      if (line != "#pragma once") print line
    }
    close("brightness_engine.h")
    next
  }
  { print }
' mod_template.cpp > "$OUT"
echo "wrote $OUT ($(wc -l < "$OUT") lines)"
