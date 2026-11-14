#!/usr/bin/env bash
# Runs INSIDE the build container (build-plugins.sh): Maze Sequencer 2 for this CPU, the front end + the sequencer core.
# It links ALSA (notes leave through an ALSA sequencer port), so it is built with gcc, not the zig build.
# env: SEQ_N (plugin name), SEQ_U (uid as 'M','z','S','q'), OUT (the .so), TAG
set -euo pipefail
O="build/obj-$TAG"; mkdir -p "$O"
F="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections"
gcc $F -std=gnu11 -w -DMAZE_LFO=1 -DMAZE_VST=1 -Isequencer/src -c sequencer/src/maze_seq_core.c -o "$O/core.o"
g++ $F -std=c++17 -fvisibility-inlines-hidden -fno-gnu-unique -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
  -I/sdk/framework -Iplugins/sequencer/src -Isequencer/src -Isequencer/vst \
  -DMAZE_SEQ_NAME="\"$SEQ_N\"" -DMAZE_SEQ_UID="$SEQ_U" -c plugins/sequencer/src/SequencerPlugin.cpp -o "$O/plugin.o"
g++ -shared -o "$OUT" "$O/plugin.o" "$O/core.o" -Wl,--gc-sections -Wl,--version-script=/sdk/tools/exports.map -Wl,--exclude-libs,ALL \
  -Wl,--no-undefined -static-libstdc++ -static-libgcc -lasound -lpthread -lm
strip "$OUT"
echo "-- $(basename "$OUT"): highest glibc $(readelf -V "$OUT" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1); needs: $(readelf -d "$OUT" | grep NEEDED | grep -o '\[.*\]' | tr '\n' ' ')"
