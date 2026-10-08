/* transport_grid.h -- host transport (audioMasterGetTime) -> the MIDI real-time stream the core follows, anchored to the SONG
 * POSITION instead of to a pulse count.
 *
 * The core steps when its `pulse % RATE_PULSES == 0`, and `pulse` counted 0xF8 messages since Start, so a lost pulse, a start
 * in the middle of a 32nd or a loop wrap shifted every step for good. This tells the core WHERE it is (set_param "song_pulse" =
 * the index of the pulse about to arrive, ppq*24) at Start, after every loop wrap / locate and once per bar (heals a dropped
 * pulse). The smallest note rate is 3 pulses, so positions are announced on 3-pulse boundaries.
 *
 * Header-only and core-independent so grid_test.cpp can drive it with the real core. Emitter E must provide:
 *   void start();           // 0xFA
 *   void stop();            // 0xFC
 *   void songpos(long m);   // set_param song_pulse = m (the NEXT pulse is number m)
 *   void pulse(long m);     // 0xF8 (m = its song-position index, for tests)
 * Same scheme as mpc-vst-euclidier / mpc-vst-acid; guide: mpc-vst-plugins docs/MIDI_TIMING.md. */
#pragma once
#include <cmath>

struct TransportGrid {
    static const long kSync = 3;   /* pulses: the finest step the core has (32nd notes) */
    double last_ppq = 0.0;
    bool was_playing = false;
    bool need_sync = false;
    bool need_start = false;

    template <class E>
    void block(E &e, bool playing, double ppq, double tempo, double sample_rate, int frames) {
        if (playing && !was_playing) { last_ppq = ppq; need_sync = need_start = true; }
        else if (!playing && was_playing) { e.stop(); need_sync = need_start = false; }
        was_playing = playing;
        if (!playing) return;

        double blk = frames * (tempo > 0 ? tempo : 120.0) / (60.0 * (sample_rate > 0 ? sample_rate : 44100.0));
        double start = last_ppq, end = ppq;
        if (end < start) { start = end - blk; need_sync = true; }        /* loop wrap: re-cover the block holding the loop start */
        else if (end - start > 1.0) { start = end - blk; need_sync = true; }   /* locate forward: re-announce, no flood */
        last_ppq = ppq;

        for (long m = (long)std::ceil(start * 24.0 - 1e-9); m / 24.0 < end - 1e-9; m++) {
            if (m < 0) continue;                       /* count-in / pre-roll */
            if (need_sync) {
                if (m % kSync != 0) continue;
                if (need_start) { e.start(); need_start = false; }
                e.songpos(m);
                need_sync = false;
            } else if (m % 96 == 0) {
                e.songpos(m);                          /* once per bar */
            }
            e.pulse(m);
        }
    }
};
