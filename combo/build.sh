#!/usr/bin/env bash
# Build Maze Combo (sequencer + voice in one plugin) as a VST2 plugin for the MPC OS plugin host (armhf).
#   combo/build/maze_combo.so, build/skin/, build/pluginlist-entry.xml
# gen.py writes params.json / layout.conf / images from the sequencer's and the voice's own sources; gen_vst.py turns
# them into params.h + the skin. The plugin is maze_combo_vst.cpp, linking both cores (each exports move_plugin_init_v2,
# so each is compiled with its own -Dmove_plugin_init_v2=<name>).
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-../../mpc-vst}"
U="$(id -u):$(id -g)"
mkdir -p build

MPC_VST="$MPC_VST" python3 gen.py

# 1. skin artwork renderer (the browser one: vst.json "art": "html")
docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art" >/dev/null

# 2. params.h, skin, pluginlist-entry.xml
docker run --rm -u "$U" -e HOME=/tmp -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w mpc-vst-html-art \
  python3 /mv/tools/gen_vst.py vst.json

cp "$MPC_VST/wrapper/popup.h" build/

# 3. the plugin (armhf, glibc 2.31 (bullseye) so it loads on MPC OS 2.x (2.32) and 3.x (2.39))
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/combo arm32v7/gcc:11-bullseye bash -euxc '
  apt-get update -qq && apt-get install -y -qq -t bullseye libasound2-dev >/dev/null
  mkdir -p build/obj
  gcc -O2 -fPIC -fvisibility=hidden -std=gnu11 -DMAZE_LFO=1 -DMAZE_VST=1 -Dmove_plugin_init_v2=maze_seq_plugin_init -I../sequencer/src -c ../sequencer/src/maze_seq_core.c -o build/obj/seq.o
  gcc -O2 -fPIC -fvisibility=hidden -std=gnu11 -DMAZE_LFO=1 -DMAZE_VST=1 -Dmove_plugin_init_v2=maze_voice_plugin_init -I../src -c ../src/maze_voice.c -o build/obj/voice.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -I../sequencer/src -Ibuild -c maze_combo_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/maze_combo.so build/obj/seq.o build/obj/voice.o build/obj/vst.o \
      -static-libstdc++ -static-libgcc -Wl,--no-undefined -lasound -lpthread -lm
  strip build/maze_combo.so
  echo "-- exported --"; readelf --dyn-syms -W build/maze_combo.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/maze_combo.so | grep NEEDED
  echo "-- highest glibc (MPC OS 2.x has 2.32) --"; readelf -V build/maze_combo.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/maze_combo.so
