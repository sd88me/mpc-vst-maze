// Offline test of transport_grid.h driving the REAL maze_seq_core.c (built with -DMAZE_VST -DMAZE_LFO): notes must come out only
// on the song-position grid (pulse m with m % RATE_PULSES == 0, m = ppq*24), across loop wraps that are not block aligned, a start in
// the middle of a 32nd, a forward locate and a dropped pulse, and no note may be left hanging.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#include "maze_seq_host.h"
#include "transport_grid.h"

static long g_m = -1;                       // song-position index of the pulse being delivered
static std::vector<long> g_on;              // g_m at each note-on
static int g_off = 0, g_ons = 0;
static int midi_out(const uint8_t *msg, int len) {
    (void)len;
    uint8_t t = msg[1] & 0xF0;   // USB-MIDI packet: [cable|CIN, status, d1, d2]
    if (t == 0x90 && msg[3] > 0) { g_on.push_back(g_m); g_ons++; }
    else if (t == 0x80 || t == 0x90) g_off++;
    return len;
}

struct Clock {
    plugin_api_v2_t *api; void *inst; int drop = 0; int rate_pulses = 6;
    void start() { uint8_t b = 0xFA; api->on_midi(inst, &b, 1, 2); }
    void stop() { uint8_t b = 0xFC; api->on_midi(inst, &b, 1, 2); }
    void songpos(long m) { char b[24]; std::snprintf(b, sizeof b, "%ld", m); api->set_param(inst, "song_pulse", b); }
    void pulse(long m) { g_m = m; if (drop > 0) { drop--; return; } uint8_t b = 0xF8; api->on_midi(inst, &b, 1, 2); }
};

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static double sr = 44100.0, tempo = 128.0;
static double ppb() { return tempo / 60.0 * 128.0 / sr; }
static double loop4(long k) { return std::fmod(k * ppb(), 4.0); }
static double from33(long k) { return 3.3 + k * ppb(); }
static double locate(long k) { double p = k * ppb(); return p < 2.0 ? p : 40.0 + (p - 2.0); }
static double lin(long k) { return k * ppb(); }

static void run(plugin_api_v2_t *api, void *inst, double (*pos)(long), long blocks, int rate_idx, long drop_at = -1) {
    TransportGrid g; Clock c{api, inst};
    char b[8]; std::snprintf(b, sizeof b, "%d", rate_idx); api->set_param(inst, "note_rate", b);
    g_on.clear(); g_off = g_ons = 0; g_m = -1;
    for (long k = 0; k < blocks; k++) { if (k == drop_at) c.drop = 2; g.block(c, true, pos(k), tempo, sr, 128); }
    g.block(c, false, 0, tempo, sr, 128);   // stop -> all notes off
}

int main() {
    static const int RATE[] = {3, 6, 12, 24, 48, 96};
    host_api_v1_t host = { midi_out };
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    for (int ri = 0; ri < 6; ri++) {
        void *inst = api->create_instance(".", "{}");
        long blocks = (long)(16.0 * 8 / ppb());                 // 8 loops of 16 beats (every rate has a step in it)
        // trig_mix/length defaults give notes; use a 4-beat loop for rates up to a beat, 16 for slower
        double (*pos)(long) = loop4;
        if (ri >= 4) { pos = lin; }
        run(api, inst, pos, blocks, ri);
        int bad = 0; for (long m : g_on) if (m % RATE[ri] != 0) bad++;
        printf("rate %d pulses: %d note-ons, %d off the grid, %d note-offs\n", RATE[ri], g_ons, bad, g_off);
        CHECK(g_ons > 4, "rate %d produced %d notes", RATE[ri], g_ons);
        CHECK(bad == 0, "rate %d: %d note-ons off the song grid", RATE[ri], bad);
        CHECK(g_off >= g_ons, "rate %d: %d note-ons but only %d note-offs (hanging note)", RATE[ri], g_ons, g_off);
        api->destroy_instance(inst);
    }
    { void *inst = api->create_instance(".", "{}");   // start in the middle of a 16th: first note on the next 16th boundary
      run(api, inst, from33, (long)(4.0 / ppb()), 1);
      long first = g_on.empty() ? -1 : g_on[0];
      printf("start at ppq 3.3: first note-on at pulse %ld (>= %ld, on the 6-grid)\n", first, 84L);
      CHECK(first >= 84 && first % 6 == 0, "first note at pulse %ld", first);
      api->destroy_instance(inst); }
    { void *inst = api->create_instance(".", "{}");   // forward locate to 40.0: no flood, back on the grid
      run(api, inst, locate, (long)(4.0 / ppb()), 1);
      int bad = 0; for (long m : g_on) if (m % 6 != 0) bad++;
      printf("locate 2.0 -> 40.0: %d note-ons, %d off grid\n", g_ons, bad);
      CHECK(bad == 0, "locate left %d notes off the grid", bad);
      api->destroy_instance(inst); }
    { void *inst = api->create_instance(".", "{}");   // a dropped pulse is healed at the next bar: still on the grid afterwards
      long blocks = (long)(24.0 / ppb());
      run(api, inst, lin, blocks, 1, (long)(5.3 / ppb()));
      long last_bar_start = (long)(8.0 * 24);          // everything from ppq 8 on (>1 bar after the drop at 5.3) must be on the grid
      int bad = 0; for (long m : g_on) if (m >= last_bar_start && m % 6 != 0) bad++;
      printf("dropped pulse: %d note-ons after the next bar are off the grid\n", bad);
      CHECK(bad == 0, "not healed: %d", bad);
      api->destroy_instance(inst); }
    printf("%s\n", fails ? "GRID FAILED" : "GRID OK");
    return fails ? 1 : 0;
}
