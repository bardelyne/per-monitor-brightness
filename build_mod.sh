#!/bin/sh
# Windhawk mods are a single source file, so splice the independently testable
# engine into the mod template at its marker.
set -e
OUT=per-monitor-brightness.wh.cpp
# Two things are dropped on the way in, and both exist because the header has
# a second life outside the mod:
#
#   * Lines tagged @harness-only exist for engine_test.cpp and are never read
#     by the mod. The mod is the artifact that gets reviewed and maintained,
#     so it should not carry them -- but deleting them from the header would
#     cost the standalone harness, which is where the engine is testable
#     without the shell. Tagging keeps one copy of the engine and lets the
#     splice decide.
#   * The header includes what it needs to compile on its own, so spliced in
#     it repeats <atomic>, <memory>, <string> and the rest that the template
#     has already included. Harmless to the compiler, but the output is what
#     reviewers read, and two include blocks disagreeing about what the file
#     needs is a question nobody should have to answer.
awk '
  # Everything the template itself includes, so the engine does not repeat it.
  /^#include / { seen[$0] = 1 }

  /^\/\/__ENGINE_INLINE__$/ {
    skipComment = 0
    while ((getline line < "brightness_engine.h") > 0) {
      if (line == "#pragma once") continue
      # The comment introducing the harness-only accessors goes with them.
      if (line ~ /Diagnostics for the standalone harness/) { skipComment = 1; continue }
      if (skipComment && line ~ /^[[:space:]]*\/\//) continue
      skipComment = 0
      if (line ~ /@harness-only/) continue
      if (line ~ /^#include /) {
        if (line in seen) continue
        seen[line] = 1
      }
      print line
    }
    close("brightness_engine.h")
    next
  }
  { print }
' mod_template.cpp > "$OUT"
echo "wrote $OUT ($(wc -l < "$OUT") lines)"
