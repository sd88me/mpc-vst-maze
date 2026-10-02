/* Offline x86 host test for maze_combo_vst (ALSA is optional: alsa_open() no-ops without /dev/snd, and note-ons are
 * counted by the MAZE_VST_TEST hooks). Checks the four states of the combined plugin:
 *   voice only            host notes play the voice, the sequencer stays stopped
 *   seq + voice           sequencer plays the voice, nothing on the MIDI port, host notes only transpose
 *   seq + voice + MIDI    both
 *   seq only (MIDI out)   notes on the port, voice silent
 * plus parameter routing to both cores and a chunk round-trip that carries both. Prints OK or FAILED. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "params.h"

typedef struct AEffect AEffect;
typedef intptr_t (*cb)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*d)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void *proc;
    void (*setP)(AEffect *, int32_t, float);
    float (*getP)(AEffect *, int32_t);
    int32_t np, npar, ni, no, flags;
    intptr_t r1, r2;
    int32_t a, b, c;
    float io;
    void *obj, *user;
    int32_t uid, ver;
    void (*pr)(AEffect *, float **, float **, int32_t);
    void *pdr;
    char f[56];
};
typedef struct { double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
                 int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags; } TI;
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4]; char d, n, r1, r2; } MEv;
typedef struct { int32_t numEvents; intptr_t reserved; void *events[2]; } Evs;
enum { kPlaying = 1 << 1, kPpq = 1 << 9, kTempo = 1 << 10 };

extern AEffect *VSTPluginMain(cb);
extern int maze_vst_note_ons(int ch);
extern int maze_vst_voice_note_ons(void);
extern int maze_vst_last_ext_note(void);

static TI g_ti;
static intptr_t host(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    (void)e; (void)idx; (void)v; (void)p; (void)o;
    if (op == 7) return (intptr_t)&g_ti;   /* audioMasterGetTime */
    return 0;
}
static int P(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!strcmp(PARAMS[i].key, key)) return i;
    printf("FAIL no param %s\n", key);
    return 0;
}
static int fails = 0;
#define CHECK(c, ...) do { int ok_ = (c); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

static float L[128], R[128];
static float *OUT[2] = { L, R };
static void set_opt(AEffect *a, const char *key, int opt) { a->setP(a, P(key), PARAMS[P(key)].nopts > 1 ? (float)opt / (PARAMS[P(key)].nopts - 1) : 0.0f); }
static void host_note(AEffect *a, int note, int vel) {
    MEv m = { 1, sizeof m, 0, 0, 0, 0, { 0x90, (unsigned char)note, (unsigned char)vel, 0 }, 0, 0, 0, 0 };
    Evs e = { 1, 0, { &m, 0 } };
    a->d(a, 25, 0, 0, &e, 0);
}
static float run_blocks(AEffect *a, int blocks, int playing) {   /* peak of the output over the run */
    float peak = 0;
    double ppq_per_block = (120.0 / 60.0) * (128.0 / 44100.0);
    for (int k = 0; k < blocks; k++) {
        g_ti.flags = (playing ? kPlaying : 0) | kPpq | kTempo;
        g_ti.tempo = 120.0;
        a->pr(a, 0, OUT, 128);
        if (playing) g_ti.ppqPos += ppq_per_block;
        for (int i = 0; i < 128; i++) { if (fabsf(L[i]) > peak) peak = fabsf(L[i]); }
    }
    return peak;
}
static void all_steps_on(AEffect *a) {   /* known pattern: every step on, both lines */
    for (int l = 1; l <= 2; l++) for (int i = 0; i < 8; i++) {
        char k[24]; snprintf(k, sizeof k, "q_s%d_step%d", l, i);
        a->setP(a, P(k), 0.0f); a->setP(a, P(k), 1.0f);
    }
}
static int ext_total(void) { int n = 0; for (int c = 0; c < 16; c++) n += maze_vst_note_ons(c); return n; }

int main(void) {
    AEffect *a = VSTPluginMain(host);
    char disp[64];
    CHECK(a && a->magic == 0x56737450 && a->npar == NPARAMS, "instance, %d params, uid %x", a ? a->npar : 0, a ? a->uid : 0);
    a->d(a, 7, P("q_on"), 0, disp, 0);  CHECK(!strcmp(disp, "OFF"), "seq starts OFF ('%s'): a normal synth", disp);
    a->d(a, 7, P("q_out"), 0, disp, 0); CHECK(!strcmp(disp, "VOICE"), "output starts VOICE ('%s')", disp);
    a->d(a, 8, P("q_s1_corrupt"), 0, disp, 0); CHECK(!strcmp(disp, "Seq A Corrupt"), "sequencer param name '%s'", disp);

    /* ---- voice only: host notes play the voice; the transport does not start the sequencer ---- */
    int v0 = maze_vst_voice_note_ons(), e0 = ext_total();
    all_steps_on(a);
    float quiet = run_blocks(a, 200, 1);
    CHECK(quiet < 0.001f && maze_vst_voice_note_ons() == v0 && ext_total() == e0, "voice only: transport running, sequencer silent (peak %.4f)", quiet);
    g_ti.ppqPos = 0;
    run_blocks(a, 1, 0);
    host_note(a, 60, 110);
    float pk = run_blocks(a, 40, 0);
    CHECK(pk > 0.01f, "voice only: a host note plays the voice (peak %.3f)", pk);

    /* ---- seq + voice (internal only) ---- */
    AEffect *b = VSTPluginMain(host);
    all_steps_on(b);
    set_opt(b, "q_on", 1);
    g_ti.ppqPos = 0;
    v0 = maze_vst_voice_note_ons(); e0 = ext_total();
    pk = run_blocks(b, 1100, 1);
    CHECK(maze_vst_voice_note_ons() > v0 + 8, "seq+voice: sequencer fed the voice (%d notes)", maze_vst_voice_note_ons() - v0);
    CHECK(ext_total() == e0, "seq+voice: nothing on the MIDI port");
    CHECK(pk > 0.01f, "seq+voice: audio comes out (peak %.3f)", pk);
    AEffect *f = VSTPluginMain(host);   /* fresh instance: no tail from earlier notes */
    set_opt(f, "q_on", 1);
    run_blocks(f, 1, 0);
    v0 = maze_vst_voice_note_ons();
    host_note(f, 72, 110);
    pk = run_blocks(f, 40, 0);
    CHECK(maze_vst_voice_note_ons() == v0 && pk < 0.001f, "seq on: a host note does not play the voice (peak %.4f)", pk);
    f->d(f, 1, 0, 0, 0, 0);

    /* ---- seq + voice + MIDI out ---- */
    AEffect *c = VSTPluginMain(host);
    all_steps_on(c);
    set_opt(c, "q_on", 1); set_opt(c, "q_out", 1);
    c->setP(c, P("q_s1_channel"), 2.0f / 15.0f);   /* line A on MIDI channel 3 */
    g_ti.ppqPos = 0;
    v0 = maze_vst_voice_note_ons(); e0 = maze_vst_note_ons(2);
    pk = run_blocks(c, 1100, 1);
    CHECK(maze_vst_voice_note_ons() > v0 + 8 && maze_vst_note_ons(2) > e0 + 8 && pk > 0.01f,
          "seq+voice+MIDI: voice %d notes, port CH3 %d notes, peak %.3f", maze_vst_voice_note_ons() - v0, maze_vst_note_ons(2) - e0, pk);

    /* ---- seq only (MIDI out): the voice stays silent ---- */
    AEffect *d = VSTPluginMain(host);
    all_steps_on(d);
    set_opt(d, "q_on", 1); set_opt(d, "q_out", 2);
    d->setP(d, P("q_s2_channel"), 4.0f / 15.0f);   /* line B on CH 5 */
    g_ti.ppqPos = 0;
    v0 = maze_vst_voice_note_ons(); e0 = maze_vst_note_ons(4);
    pk = run_blocks(d, 1100, 1);
    CHECK(maze_vst_voice_note_ons() == v0 && pk < 0.001f, "seq only: voice untouched, silent (peak %.4f)", pk);
    CHECK(maze_vst_note_ons(4) > e0 + 8, "seq only: line B sent %d notes on CH5", maze_vst_note_ons(4) - e0);

    /* ---- host notes transpose the sequence (CV range 0: every step is the root, so the note shows the shift) ---- */
    AEffect *g = VSTPluginMain(host);
    all_steps_on(g);
    set_opt(g, "q_on", 1); set_opt(g, "q_out", 2);
    g->setP(g, P("q_s1_cv_range"), 0.0f); g->setP(g, P("q_s2_cv_range"), 0.0f);
    g_ti.ppqPos = 0;
    run_blocks(g, 400, 1);
    int base = maze_vst_last_ext_note();
    host_note(g, 67, 100);   /* G: +7 from middle C */
    run_blocks(g, 400, 1);
    int shifted = maze_vst_last_ext_note();
    host_note(g, 60, 100);
    run_blocks(g, 400, 1);
    CHECK(base == 60 && shifted == 67 && maze_vst_last_ext_note() == 60, "seq on: host note G transposes the sequence %d -> %d, C brings it back to %d", base, shifted, maze_vst_last_ext_note());
    g->d(g, 1, 0, 0, 0, 0);

    /* ---- switching the sequencer off mid-play hands the notes back to the voice ---- */
    set_opt(b, "q_on", 0);
    g_ti.ppqPos = 10;
    v0 = maze_vst_voice_note_ons();
    run_blocks(b, 300, 1);
    CHECK(maze_vst_voice_note_ons() == v0, "seq switched off: the transport no longer plays notes");
    host_note(b, 60, 110);
    CHECK(run_blocks(b, 40, 0) > 0.01f, "seq switched off: host notes play the voice again");

    /* ---- parameters reach the right core ---- */
    a->setP(a, P("cutoff"), 0.25f);
    CHECK(fabsf(a->getP(a, P("cutoff")) - 0.25f) < 0.02f, "voice param cutoff round-trips (%.3f)", a->getP(a, P("cutoff")));
    a->setP(a, P("route"), 1.0f); a->d(a, 7, P("route"), 0, disp, 0);
    CHECK(!strcmp(disp, "VCF>VCW"), "voice enum route '%s'", disp);
    a->setP(a, P("q_scale"), 1.0f); a->d(a, 7, P("q_scale"), 0, disp, 0);
    CHECK(!strcmp(disp, "UNQUANT"), "sequencer enum scale '%s'", disp);
    a->setP(a, P("q_transpose"), 0.75f); a->d(a, 7, P("q_transpose"), 0, disp, 0);
    CHECK(!strcmp(disp, "24"), "sequencer transpose '%s'", disp);

    /* ---- chunk round-trip carries both cores ---- */
    set_opt(a, "q_on", 1); set_opt(a, "q_out", 1);
    void *chunk = 0;
    intptr_t n = a->d(a, 23, 0, 0, &chunk, 0);
    char saved[12288];
    strncpy(saved, (char *)chunk, sizeof saved - 1); saved[sizeof saved - 1] = 0;
    printf("     chunk %ld bytes\n", (long)n);
    CHECK(strstr(saved, "pattern=") && strstr(saved, "voice=") && strstr(saved, "q_on=1") && !strstr(saved, "__open") && !strstr(saved, "_run"),
          "chunk has pattern, voice state and the switches");
    AEffect *e = VSTPluginMain(host);
    e->d(e, 24, 0, n, saved, 0);
    void *chunk2 = 0;
    intptr_t n2 = e->d(e, 23, 0, 0, &chunk2, 0);
    CHECK(n2 == n && !strcmp(saved, (char *)chunk2), "a second instance's chunk equals the first after setChunk");
    CHECK(fabsf(e->getP(e, P("cutoff")) - a->getP(a, P("cutoff"))) < 1e-4f && e->getP(e, P("q_on")) > 0.5f, "voice param and switch carried over");

    a->d(a, 1, 0, 0, 0, 0); b->d(b, 1, 0, 0, 0, 0); c->d(c, 1, 0, 0, 0, 0); d->d(d, 1, 0, 0, 0, 0); e->d(e, 1, 0, 0, 0, 0);
    printf("%s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
