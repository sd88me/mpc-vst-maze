#!/usr/bin/env bash
# Build Maze Voice 2 and Maze Sequencer 2 into build/plugins/"sd88me - VST - <Name>"/ for arm32 (Force, MPC Live / One / X)
# and arm64 (MPC Live III).
#   ./build-plugins.sh          both plugins
#   ./build-plugins.sh beta     "Maze Voice 2 Beta" (uid MzVB) and "Maze Sequencer 2 Beta" (uid MzSB): install next to the release
#   ./build-plugins.sh test     the offline tests (tests/run.sh) and the plugin framework's host simulator on both
# Maze Voice is built with the plugin framework's own build (tools/build.py). Maze Sequencer links ALSA, so both its targets
# are compiled with gcc in Docker (scripts/Dockerfile, scripts/compile_seq.sh).
# MPC_SDK: the plugin framework checkout (tools/build.py, framework/mpc_plugin.h).
set -euo pipefail
cd "$(dirname "$0")"
SDK="${MPC_SDK:-$HOME/mpc-plugin-builder-sdk}"
[ -f "$SDK/framework/mpc_plugin.h" ] || { echo "need the plugin framework (MPC_SDK)" >&2; exit 1; }
python3 tools/gen_params.py plugins/voice plugins/sequencer
GLIBC_MAX=2.29   # the Force (MPC OS 3.9.x)

newest_glibc() { readelf -V "$1" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1; }
check_glibc() {   # <so>: refuse an arm32 build the Force cannot load
  local v; v="$(newest_glibc "$1")"
  if [ "$(printf '%s\n%s\n' "$v" "$GLIBC_MAX" | sort -V | tail -n 1)" != "$GLIBC_MAX" ]; then echo "$1 needs glibc $v (> $GLIBC_MAX)" >&2; exit 1; fi
  echo "  $(basename "$1"): newest glibc symbol $v (<= $GLIBC_MAX)"
}
skins() {   # <plugin dir> <folder>
  if [ -f tools/make_tui.py ]; then python3 tools/make_tui.py "$(basename "$1")" >/dev/null; fi
  if [ -d "$1/skins" ]; then mkdir -p "$2/Plugin Skins"; cp -r "$1/skins/." "$2/Plugin Skins/"; fi
}
manifest() {   # <plugin dir> <folder> <name> <uid> <slug>
  python3 - "$@" <<'PY'
import json, sys
d, folder, name, uid, slug = sys.argv[1:]
m = json.load(open(d + "/plugin.json"))
m.update(name=name, uid=uid, file32="VST/%s-arm32.so" % slug, file64="VST/%s-arm64.so" % slug)
json.dump(m, open(folder + "/plugin.json", "w"), indent=2)
open(folder + "/version.xml", "w").write('<?xml version="1.0" encoding="utf-8"?>\n<plugincontent version="1.0">\n    <version>%s</version>\n    <uiversion>0.1.0.0</uiversion>\n</plugincontent>\n' % m["version"])
PY
}

if [ "${1:-}" = test ]; then
  tests/run.sh
  python3 "$SDK/tools/build.py" plugins/voice --no-ui --target native --no-validate >/dev/null
  echo "== host_sim: Maze Voice"
  "$SDK/.sdk-tools/host_sim" plugins/voice/build/native/maze-voice.so test --notes 48,60 2>&1 | tail -n 3
  docker image inspect maze-sdk-run >/dev/null 2>&1 || docker build -q -t maze-sdk-run -f - scripts >/dev/null <<'EOF'
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq libasound2t64 && rm -rf /var/lib/apt/lists/*
EOF
  mkdir -p build/native
  docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/b -v "$SDK":/sdk:ro -w /b maze-sdk-x64 sh -c \
    'g++ -std=c++17 -O1 -w -fPIC -shared -fvisibility=hidden -DMAZE_LFO=1 -DMAZE_VST=1 -I/sdk/framework -Iplugins/sequencer/src -Isequencer/src -Isequencer/vst \
       -x c sequencer/src/maze_seq_core.c -x c++ plugins/sequencer/src/SequencerPlugin.cpp -o build/native/maze-seq.so -lasound -lpthread -lm 2>&1 | grep -v "^$" || true'
  echo "== host_sim: Maze Sequencer"
  docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/b -v "$SDK":/sdk:ro -w /b maze-sdk-run sh -c \
    '/sdk/.sdk-tools/host_sim build/native/maze-seq.so test --playing --notes 60 2>&1 | grep -v "ALSA lib" | tail -n 3'
  exit
fi

VN="Maze Voice"; VU=MzVc; VS=maze-voice; SN="Maze Sequencer"; SU=MzSq; SS=maze-sequencer
if [ "${1:-}" = beta ]; then VN="Maze Voice 2 Beta"; VU=MzVB; VS=mazevoice2beta; SN="Maze Sequencer 2 Beta"; SU=MzSB; SS=mazeseq2beta; fi
q() { printf "'%s','%s','%s','%s'" "${1:0:1}" "${1:1:1}" "${1:2:1}" "${1:3:1}"; }
OUTD=build/plugins; mkdir -p "$OUTD"

# ---- Maze Voice: the framework's build, from a staging project with this identity ----
stage=plugins/.stage-voice; rm -rf "$stage"; mkdir -p "$stage"   # beside plugins/voice: the same relative includes
cp -r plugins/voice/src "$stage/src"
manifest plugins/voice "$stage" "$VN" "$VU" "$VS"
python3 "$SDK/tools/build.py" "$stage" --no-ui --no-validate --define "MAZE_VOICE_NAME=\"$VN\"" --define "MAZE_VOICE_UID=$(q $VU)" | grep -E "ok|error|FAIL|warning: " || true
VF="$OUTD/sd88me - VST - $VN"; rm -rf "$VF"; mkdir -p "$VF/VST"
cp "$stage/build/sd88me - VST - $VN/VST/"*.so "$VF/VST/"
manifest plugins/voice "$VF" "$VN" "$VU" "$VS"; skins plugins/voice "$VF"
check_glibc "$VF/VST/$VS-arm32.so"

# ---- Maze Sequencer: gcc in Docker (ALSA) ----
SF="$OUTD/sd88me - VST - $SN"; rm -rf "$SF"; mkdir -p "$SF/VST"
build() {   # <docker platform> <image> <base image> <cpu>
  docker image inspect "$2" >/dev/null 2>&1 || docker build -q -t "$2" --platform "$1" --build-arg BASE="$3" scripts >/dev/null
  docker run --rm --platform "$1" -u "$(id -u):$(id -g)" -e SEQ_N="$SN" -e SEQ_U="$(q $SU)" -e TAG="$2" \
    -e OUT="$SF/VST/$SS-$4.so" -v "$PWD":/b -v "$SDK":/sdk:ro -w /b "$2" scripts/compile_seq.sh
}
build linux/arm/v7 maze-sdk-arm32 arm32v7/gcc:11-bullseye arm32 &
build linux/arm64 maze-sdk-arm64 arm64v8/gcc:12-bookworm arm64 &
wait
manifest plugins/sequencer "$SF" "$SN" "$SU" "$SS"; skins plugins/sequencer "$SF"
check_glibc "$SF/VST/$SS-arm32.so"
for f in "$VF" "$SF"; do python3 "$SDK/tools/validate.py" "$f" 2>&1 | tail -n 2 || true; echo "built: $f"; done
