#!/usr/bin/env bash
# Build Maze Sequencer as a VST2 plugin for the MPC OS plugin host (armhf).
#   vst/build/maze_seq.so, vst/build/skin/  packaged together as ONE plugin folder in /sdcard/Synths (tools/release.py)
#   vst/build/pluginlist-entry.xml          the <PLUGIN> line for MPC.settings' pluginList-arm
# vst.json/params.json only feed mpc-vst-plugins' tools/gen_vst.py for params.h + the skin (MIDI generator:
# docs/PORTING.md classification 0). The plugin itself is maze_seq_vst.cpp, which links the vendored
# maze_seq_core.c and does its own ALSA seq MIDI output, so it is built here, not via tools/build_port.sh.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-../../../mpc-vst}"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. skin artwork renderer (the browser one: vst.json "art": "html")
docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art" >/dev/null

# 2. params.h, skin, pluginlist-entry.xml
docker run --rm -u "$U" -e HOME=/tmp ${SHADOW_SKIN_MPC_OS:+-e SHADOW_SKIN_MPC_OS=$SHADOW_SKIN_MPC_OS} -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w mpc-vst-html-art \
  python3 /mv/tools/gen_vst.py vst.json

cp "$MPC_VST/wrapper/popup.h" build/   # popup open-flag handling shared with mpc-vst's own wrapper

# 3. the plugin (armhf, glibc 2.31 (bullseye) so it loads on MPC OS 2.x (2.32) and 3.x (2.39))
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/vst arm32v7/gcc:11-bullseye bash -euxc '
  apt-get update -qq && apt-get install -y -qq -t bullseye libasound2-dev >/dev/null
  mkdir -p build/obj
  gcc -O2 -fPIC -fvisibility=hidden -std=gnu11 -DMAZE_LFO=1 -DMAZE_VST=1 -I../src -c ../src/maze_seq_core.c -o build/obj/core.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -I../src -Ibuild -c maze_seq_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/maze_seq.so build/obj/core.o build/obj/vst.o \
      -static-libstdc++ -static-libgcc -Wl,--no-undefined -lasound -lpthread -lm
  strip build/maze_seq.so
  echo "-- exported --"; readelf --dyn-syms -W build/maze_seq.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/maze_seq.so | grep NEEDED
  echo "-- highest glibc (MPC OS 2.x has 2.32) --"; readelf -V build/maze_seq.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/maze_seq.so

# 4. Gen2 (aarch64, MPC OS 3.x only, glibc 2.39): same sources, built in an arm64 container into build/aarch64/ (docs/GEN2.md)
docker run --rm --platform linux/arm64 -v "$PWD/..":/b -w /b/vst arm64v8/gcc:12-bookworm bash -euxc '
  apt-get update -qq && apt-get install -y -qq libasound2-dev >/dev/null
  mkdir -p build/obj64 build/aarch64
  gcc -O2 -fPIC -fvisibility=hidden -std=gnu11 -DMAZE_LFO=1 -DMAZE_VST=1 -I../src -c ../src/maze_seq_core.c -o build/obj64/core.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -I../src -Ibuild -c maze_seq_vst.cpp -o build/obj64/vst.o
  g++ -shared -o build/aarch64/maze_seq.so build/obj64/core.o build/obj64/vst.o \
      -static-libstdc++ -static-libgcc -Wl,--no-undefined -lasound -lpthread -lm
  strip build/aarch64/maze_seq.so
  readelf -h build/aarch64/maze_seq.so | grep Machine
  chown -R '"$U"' build
'
md5sum build/aarch64/maze_seq.so
