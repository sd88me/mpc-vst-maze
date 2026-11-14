#!/usr/bin/env bash
# Re-derive the frozen parameter tables from the released builds:
#   Maze Voice 1.0.5      tag maze-voice-vst-v1.0.5      tools b6e2197 (its release workflow's tools_ref)
#   Maze Sequencer 1.0.8  tag maze-sequencer-vst-v1.0.8  tools 8ee2ddb
# Needs an mpc-vst-plugins checkout (MPC_VST). Writes plugins/<voice|sequencer>/params.json + params.lock.
set -euo pipefail
cd "$(dirname "$0")/.."
MPC_VST="${MPC_VST:?set MPC_VST to an mpc-vst-plugins checkout}"
w="$(mktemp -d)"; trap 'rm -rf "$w"' EXIT
one() {   # <name> <tag> <tools ref> <vst.json dir in the tag>
  local d="$w/$1"; mkdir -p "$d/maze" "$d/mv"
  git archive "$2" | tar -x -C "$d/maze"
  git -C "$MPC_VST" archive "$3" | tar -x -C "$d/mv"
  (cd "$d/maze/$4" && python3 "$d/mv/tools/gen_vst.py" vst.json --params-h >/dev/null)
  mkdir -p "plugins/$1"
  python3 tools/freeze_released.py "$d/maze/$4/build/params.h" "plugins/$1"
}
one voice     maze-voice-vst-v1.0.5     b6e2197731fc7cc4dd491d34c4a83c4348c3babc vst
one sequencer maze-sequencer-vst-v1.0.8 8ee2ddb5a398c8c912560e906c1b4f051b6e49be sequencer/vst
