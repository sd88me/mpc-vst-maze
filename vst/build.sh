#!/usr/bin/env bash
# Build Maze Voice as an MPC OS VST2 instrument with mpc-vst-plugins' generic port builder (vst.json).
#   vst/build/maze_voice.so               -> /sdcard/vst/ on the device
#   vst/build/skin/<folder>/              -> /sdcard/Synths/ on the device
#   vst/build/pluginlist-entry.xml        the <PLUGIN> line for MPC.settings' pluginList-arm
# Needs an mpc-vst-plugins checkout (MPC_VST) next to mpc-vst-maze.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
MPC_VST="${MPC_VST:-$here/../../mpc-vst}"
[ -x "$MPC_VST/tools/build_port.sh" ] || { echo "need an mpc-vst-plugins checkout (MPC_VST)" >&2; exit 1; }
exec "$MPC_VST/tools/build_port.sh" "$here/vst.json"
