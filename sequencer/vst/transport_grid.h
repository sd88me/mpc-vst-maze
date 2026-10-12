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
#include <algorithm>
#include <cmath>

struct TransportGrid {
    static const long kSync = 3;   /* pulses: the finest step the core has (32nd notes) */
    double sent_to = 0.0;          /* high-water mark: pulses before this song position have been sent */
    bool was_playing = false;
    bool need_sync = false;
    bool need_start = false;

    /* Once per audio block. ppq = ppqPos at the start of this block. The pulses of THIS block, [ppq, ppq + blk), go to the
     * core now, at its start (as StepRunner-8 does), so its notes leave up to a block early instead of a block late; MPC's MIDI
     * input comes after the plugin, so earlier is closer to the grid (Euclidier user report: track delay needed to line up).
     * Looking ahead runs into the loop end: on a Force (MPC OS 3.9.1) the block before a wrap covers the loop end and the next
     * reports a position just after the loop start. MPC reports the loop (cycleStartPos/cycleEndPos, cycle flags), so
     * loop_start/loop_end (loop_end <= loop_start: no loop) cut the block there: up to the loop end, then the loop start is
     * announced and the rest goes from there. */
    template <class E>
    void block(E &e, bool playing, double ppq, double tempo, double sample_rate, int frames,
               double loop_start = 0.0, double loop_end = 0.0) {
        if (playing && !was_playing) { sent_to = ppq; need_sync = need_start = true; }
        else if (!playing && was_playing) { e.stop(); need_sync = need_start = false; }
        was_playing = playing;
        if (!playing) return;

        double blk = frames * (tempo > 0 ? tempo : 120.0) / (60.0 * (sample_rate > 0 ? sample_rate : 44100.0));
        double start = sent_to, end = ppq + blk;
        /* ppq behind what was sent by less than a beat: block jitter (MPC's ppq wobbles up to 0.0009 beat) or a tempo change (the
         * host's ppq steps back a little then crawls on); hold the high-water mark, so nothing plays twice. By more: a wrap the
         * loop range did not announce, or a locate. The block before was covered by the look-ahead, so carry on from here, less
         * a little more than the jitter (a wrap reported at 0.0001 still plays the loop start's pulse) -- except after a wrap the
         * host did not report: the look-ahead may already have sent that pulse (under the pre-wrap position). */
        if (std::fabs(ppq - sent_to) > 1.0) {
            bool in_loop = loop_end > loop_start && ppq >= loop_start - 0.002 && ppq < loop_end;
            start = in_loop ? std::max(ppq - 0.002, loop_start) : ppq < sent_to ? ppq : ppq - 0.002;
            need_sync = true;
        }
        if (end <= start) return;

        if (loop_end > loop_start + 0.25 && ppq >= loop_start && ppq < loop_end && end > loop_end) {
            pulses(e, start, loop_end);
            need_sync = true;                          /* the loop start is announced before its first pulse */
            sent_to = loop_start + (end - loop_end);
            pulses(e, loop_start, sent_to);
        } else {
            pulses(e, start, end);
            sent_to = end;
        }
    }

private:
    template <class E>
    void pulses(E &e, double start, double end) {
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
