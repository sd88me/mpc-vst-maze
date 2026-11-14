#!/usr/bin/env bash
# tests/data/legacy-voice-1.0.5.chunk and legacy-sequencer-1.0.8.chunk: the chunks projects saved with the released builds hold,
# made by each tag's own engine (and, for the sequencer, its own wrapper), never by the new code. Needs MPC_VST (an
# mpc-vst-plugins checkout, for the sequencer tag's params.h/popup.h at its pinned tools commit) and Docker (libasound).
set -euo pipefail
cd "$(dirname "$0")/.."
MPC_VST="${MPC_VST:?set MPC_VST to an mpc-vst-plugins checkout}"
w="$(mktemp -d)"; trap 'rm -rf "$w"' EXIT
mkdir -p "$w/v" "$w/s" "$w/mv" tests/data
git archive maze-voice-vst-v1.0.5 src | tar -x -C "$w/v"
git archive maze-sequencer-vst-v1.0.8 sequencer | tar -x -C "$w/s"
git -C "$MPC_VST" archive 8ee2ddb5a398c8c912560e906c1b4f051b6e49be tools wrapper | tar -x -C "$w/mv"
(cd "$w/s/sequencer/vst" && python3 "$w/mv/tools/gen_vst.py" vst.json --params-h >/dev/null && cp "$w/mv/wrapper/popup.h" build/)
cp tests/legacy_voice_dump.c tests/legacy_seq_dump.c "$w/"
gcc -O1 -w -DMAZE_LFO=1 -DMAZE_VST=1 -o "$w/vdump" "$w/legacy_voice_dump.c" "$w/v/src/maze_voice.c" -lm
(cd "$w" && ./vdump) > tests/data/legacy-voice-1.0.5.chunk
docker run --rm -v "$w":/w -w /w gcc:12 bash -ec '
  apt-get update -qq && apt-get install -y -qq libasound2-dev >/dev/null; S=s/sequencer
  gcc -O1 -w -std=gnu11 -DMAZE_LFO=1 -DMAZE_VST=1 -I$S/src -c $S/src/maze_seq_core.c -o core.o
  g++ -O1 -w -std=c++17 -I$S/src -I$S/vst/build -c $S/vst/maze_seq_vst.cpp -o vst.o
  gcc -O1 -w -std=gnu11 -I$S/vst/build -c legacy_seq_dump.c -o dump.o
  g++ -o sdump core.o vst.o dump.o -lasound -lpthread -lm
  ./sdump > seq.chunk; chown '"$(id -u):$(id -g)"' seq.chunk' 2>&1 | grep -v "^\[maze" || true
cp "$w/seq.chunk" tests/data/legacy-sequencer-1.0.8.chunk
for f in tests/data/legacy-*.chunk; do echo "$f: $(wc -c < "$f") bytes"; done
