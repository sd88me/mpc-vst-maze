# Vendored: maze_seq_core.c / maze_seq_host.h

Source: `sd88me/force-maze`, `maze-sequencer/src/maze_seq_core.c` + `maze_seq_host.h`
Vendored at commit: `abae5ea` (2026-10-02). License: MIT (same author, see `LICENSE`).

The core is `schwung-maze`'s `maze_seq.c` as already adapted for the Force (MockbaMod) build. It is vendored here so this
port stays self-contained. Local changes, all behind `#ifdef MAZE_VST` (the build defines it; the other builds are
unchanged) and marked `MPC-VST-ONLY`:

1. **No state file, no worker thread.** The Force build keeps one `maze_seq.bin` per process; with several plugin
   instances in several projects that would be shared and wrong. The project chunk carries the state instead.
2. **`pattern`** (`get_param` / `set_param`): the step bits and CV values of both lines as
   `<8 bits>:<8 cv>|<8 bits>:<8 cv>`, so a saved project replays the exact pattern (the core's `get_param` does not
   expose CV).
3. **`s1_regen` / `s2_regen`**: re-roll one line's pattern (keeps its length, channel, corrupt, range and reset).
4. **`host_bpm`**: tempo for LFO sync. The core estimates tempo from the wall-clock gap between clock pulses, which is
   meaningless in a plugin where pulses arrive in a burst per audio block.

5. **`s1_adv` / `s2_adv`**: rotate that line's pattern (gates and CV of its active steps) one step forward per press,
   also while stopped. The Force build's version only moves the play-head, which is inaudible when stopped.

6. **`song_pulse`**: anchor the pulse counter (and the reset-every-N-bars counter) to the song position, so steps follow the
   host's `ppqPos` instead of the number of pulses since Start. The wrapper sets it at Start, after a loop wrap / locate and once
   per bar (`vst/transport_grid.h`). A first Start at the top now lands the first step on the downbeat; counting from Start put it
   one pulse early relative to the song grid.

Re-vendor by diffing against force-maze's `src/` at a newer commit and re-applying these six.
