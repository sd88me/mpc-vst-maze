#!/usr/bin/env bash
# Offline x86 tests under ASan/UBSan (no device). Each prints its OK line:
#   tests/voice_host_test.cpp  Maze Voice 2 front end + engine (local g++)
#   tests/seq_host_test.cpp    Maze Sequencer 2 front end + core (Docker gcc: it links ALSA)
#   sequencer/vst/grid_test.cpp  the song-position clock (transport_grid.h)
# MPC_SDK: the plugin SDK checkout (framework/mpc_plugin.h).
set -euo pipefail
cd "$(dirname "$0")/.."
SDK="${MPC_SDK:-$HOME/mpc-plugin-builder-sdk}"
python3 tools/gen_params.py plugins/voice plugins/sequencer >/dev/null
S="-fsanitize=address,undefined -fno-sanitize-recover=undefined -O1 -g -std=c++17 -DMAZE_TEST"
W="-Wall -Wextra -Wno-unused-parameter -Wno-format-truncation"
o=build/test/voice; rm -rf "$o"; mkdir -p "$o"
g++ $S $W -I"$SDK/framework" -Iplugins/voice/src -c plugins/voice/src/VoicePlugin.cpp -o "$o/plugin.o"
g++ $S -w -c plugins/voice/src/engine.cpp -o "$o/engine.o"
g++ $S $W -I"$SDK/framework" -Iplugins/voice/src -Itests -c tests/voice_host_test.cpp -o "$o/test.o"
g++ -fsanitize=address,undefined -o "$o/voice_test" "$o"/*.o -lpthread
"$o/voice_test" . | tail -n "${TAIL:-60}"

docker image inspect maze-sdk-x64 >/dev/null 2>&1 || docker build -q -t maze-sdk-x64 --build-arg BASE=gcc:12 scripts >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/b -v "$SDK":/sdk:ro -w /b -e ASAN_OPTIONS=detect_leaks=1 maze-sdk-x64 bash -euc '
  S="'"$S"'"; W="'"$W"'"; o=build/test/seq; rm -rf $o; mkdir -p $o
  gcc ${S/-std=c++17/-std=gnu11} -w -DMAZE_LFO=1 -DMAZE_VST=1 -Isequencer/src -c sequencer/src/maze_seq_core.c -o $o/core.o
  g++ $S $W -I/sdk/framework -Iplugins/sequencer/src -Isequencer/src -Isequencer/vst -c plugins/sequencer/src/SequencerPlugin.cpp -o $o/plugin.o
  g++ $S $W -I/sdk/framework -Iplugins/sequencer/src -Itests -c tests/seq_host_test.cpp -o $o/test.o
  g++ -fsanitize=address,undefined -o $o/seq_test $o/*.o -lasound -lpthread -lm
  $o/seq_test . 2>&1 | grep -v "^ALSA lib" | tail -n ${TAIL:-60}
  g++ -O1 -g -fsanitize=address,undefined -std=c++17 -Wall -Wextra -Isequencer/vst -Isequencer/src -o build/test/grid_test sequencer/vst/grid_test.cpp $o/core.o -lm
  build/test/grid_test | tail -n 2'
