#!/usr/bin/env bash
# Offline x86 host test under ASan/UBSan (no device needed). See host_test.c. Needs build/params.h + build/popup.h (build.sh).
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD/..":/w -w /w/vst gcc:12 bash -euxc '
  apt-get update -qq && apt-get install -y -qq libasound2-dev >/dev/null
  mkdir -p /tmp/t
  gcc -O0 -g -fsanitize=address,undefined -std=gnu11 -DMAZE_LFO=1 -DMAZE_VST=1 -I../src -c ../src/maze_seq_core.c -o /tmp/t/core.o
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -DMAZE_VST_TEST -I../src -Ibuild -c maze_seq_vst.cpp -o /tmp/t/vst.o
  gcc -O0 -g -fsanitize=address,undefined -std=gnu11 -Ibuild -c host_test.c -o /tmp/t/host_test.o
  g++ -fsanitize=address,undefined -o /tmp/t/mtest /tmp/t/core.o /tmp/t/vst.o /tmp/t/host_test.o -lasound -lpthread -lm
  /tmp/t/mtest
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -Wall -Wextra -I../src -c grid_test.cpp -o /tmp/t/grid_test.o
  g++ -fsanitize=address,undefined -o /tmp/t/gtest /tmp/t/core.o /tmp/t/grid_test.o -lm
  /tmp/t/gtest
'
echo "PASSED"
