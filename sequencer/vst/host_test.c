/* Offline x86 host test for maze_seq_vst's VST2 wrapper (alsa_open()/alsa_flush() no-op gracefully when
 * snd_seq_open() fails, e.g. no /dev/snd here; note-ons are counted by the MAZE_VST_TEST hook instead).
 * Checks: two instances, option/number/channel round-trips, step toggles, notes out on the right MIDI
 * channels from a transport-driven clock, trigger spring-back, popups, and a chunk round-trip that
 * carries the pattern. Prints OK or FAILED. */
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
enum { kPlaying = 1 << 1, kPpq = 1 << 9, kTempo = 1 << 10 };

extern AEffect *VSTPluginMain(cb);
extern int maze_vst_note_ons(int ch);
extern int maze_vst_pad_semis(AEffect *e);

static TI g_ti;
static int automated[NPARAMS];
static intptr_t host(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    (void)e; (void)v; (void)p; (void)o;
    if (op == 0 && idx >= 0 && idx < NPARAMS) automated[idx]++;   /* audioMasterAutomate */
    if (op == 7) return (intptr_t)&g_ti;                          /* audioMasterGetTime */
    return 0;
}
static int P(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!strcmp(PARAMS[i].key, key)) return i;
    printf("FAIL no param %s\n", key);
    return 0;
}
static int fails = 0;
#define CHECK(c, ...) do { int ok_ = (c); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

int main(void) {
    AEffect *a = VSTPluginMain(host), *b = VSTPluginMain(host);
    char disp[64], name[64];
    CHECK(a && b && a != b && a->magic == 0x56737450, "two instances, magic %x, %d params, uid %x", a->magic, a->npar, a->uid);

    /* defaults match the core's own */
    a->d(a, 7, P("scale"), 0, disp, 0);   CHECK(!strcmp(disp, "MAJOR"), "default scale '%s'", disp);
    a->d(a, 7, P("s1_channel"), 0, disp, 0); CHECK(!strcmp(disp, "1"), "default MIDI CH '%s'", disp);
    a->d(a, 7, P("s1_length"), 0, disp, 0);  CHECK(!strcmp(disp, "8"), "default length '%s'", disp);
    a->d(a, 7, P("g_reset"), 0, disp, 0);    CHECK(!strcmp(disp, "OFF"), "default reset '%s'", disp);

    /* option, number and signed round-trips */
    a->setP(a, P("scale"), 1.0f);        a->d(a, 7, P("scale"), 0, disp, 0);     CHECK(!strcmp(disp, "UNQUANT"), "scale -> last option '%s'", disp);
    a->setP(a, P("transpose"), 0.75f);   a->d(a, 7, P("transpose"), 0, disp, 0); CHECK(!strcmp(disp, "24"), "transpose 0.75 -> '%s'", disp);
    a->setP(a, P("trig_mix"), 0.0f);     a->d(a, 7, P("trig_mix"), 0, disp, 0);  CHECK(!strcmp(disp, "-63"), "trig mix 0.0 -> '%s'", disp);
    /* Q-Link nudge between options steps one option */
    a->setP(a, P("note_rate"), 0.5f / 5.0f + 1.0f / 5.0f);   /* between 1/16 (1) and 1/8 (2) -> from 1/16 up one */
    a->d(a, 7, P("note_rate"), 0, disp, 0); CHECK(!strcmp(disp, "1/8"), "nudge steps one option: note rate '%s'", disp);
    a->d(a, 8, P("s1_step3"), 0, name, 0); CHECK(!strcmp(name, "A Step 4"), "step name '%s'", name);

    /* steps: all on for line A (MIDI CH 3), line B left to the random pattern but on CH 5 */
    a->setP(a, P("s1_channel"), 2.0f / 15.0f);   /* 1..16: 0.133 -> 3 */
    a->d(a, 7, P("s1_channel"), 0, disp, 0); CHECK(!strcmp(disp, "3"), "MIDI CH display '%s'", disp);
    a->setP(a, P("s2_channel"), 4.0f / 15.0f);
    for (int i = 0; i < 8; i++) {
        a->setP(a, P("s1_step0") + i, 0.0f);   /* known state: all off ... */
    }
    for (int i = 0; i < 8; i++) a->setP(a, P("s1_step0") + i, 1.0f);
    int allon = 1;
    for (int i = 0; i < 8; i++) allon &= a->getP(a, P("s1_step0") + i) > 0.5f;
    CHECK(allon, "all 8 steps of line A on");
    a->setP(a, P("s1_step5"), 0.0f);
    CHECK(a->getP(a, P("s1_step5")) < 0.5f && a->getP(a, P("s1_step4")) > 0.5f, "step 6 flips off alone");
    a->setP(a, P("s1_step5"), 1.0f);

    /* Advance rotates the active steps one step forward, also while stopped, and tells the host the LEDs changed */
    for (int i = 0; i < 8; i++) a->setP(a, P("s1_step0") + i, i == 0 ? 1.0f : 0.0f);
    a->setP(a, P("s1_length"), 7.0f / 7.0f);   /* 1..8 -> 8 */
    a->pr(a, 0, (float *[]){ (float[128]){0}, (float[128]){0} }, 128);
    int before = automated[P("s1_step1")];
    a->setP(a, P("s1_adv"), 1.0f);
    a->pr(a, 0, (float *[]){ (float[128]){0}, (float[128]){0} }, 128);
    CHECK(a->getP(a, P("s1_step1")) > 0.5f && a->getP(a, P("s1_step0")) < 0.5f, "advance moved the lit step 1 -> 2 while stopped");
    CHECK(automated[P("s1_step1")] == before + 1 && automated[P("s1_step0")] >= 1, "host told the LEDs changed (%d)", automated[P("s1_step1")] - before);
    a->setP(a, P("s1_adv"), 0.0f);
    for (int k = 0; k < 7; k++) { a->setP(a, P("s1_adv"), 1.0f); a->pr(a, 0, (float *[]){ (float[128]){0}, (float[128]){0} }, 128); a->setP(a, P("s1_adv"), 0.0f); }
    CHECK(a->getP(a, P("s1_step0")) > 0.5f && a->getP(a, P("s1_step1")) < 0.5f, "8 more presses wrap round to step 1");
    /* a Reset re-rolls the line and the LEDs follow */
    a->setP(a, P("s1_regen"), 1.0f); a->pr(a, 0, (float *[]){ (float[128]){0}, (float[128]){0} }, 128); a->setP(a, P("s1_regen"), 0.0f);
    a->d(a, 8, P("s1_regen"), 0, name, 0); CHECK(!strcmp(name, "A Reset"), "reset button name '%s'", name);
    /* back to a known all-on pattern for the transport checks below */
    for (int i = 0; i < 8; i++) a->setP(a, P("s1_step0") + i, 0.0f);
    for (int i = 0; i < 8; i++) a->setP(a, P("s1_step0") + i, 1.0f);

    /* triggers: fire and spring back (the host stub counts the automate) */
    a->setP(a, P("s2_regen"), 1.0f);
    a->pr(a, 0, (float *[]){ (float[128]){0}, (float[128]){0} }, 128);
    CHECK(automated[P("s2_regen")] == 1, "regen sprung back once via audioMasterAutomate");
    a->d(a, 7, P("s2_regen"), 0, disp, 0); CHECK(!disp[0], "trigger shows no value");

    a->setP(a, P("trig_mix"), 0.5f);   /* centred: both lines audible (-63 is line A only) */
    /* transport: 120 BPM, 3 bars: notes must come out on channels 2 (A) and 4 (B), none elsewhere */
    g_ti.flags = kPlaying | kPpq | kTempo;
    g_ti.tempo = 120.0;
    float L[128], R[128], *out[2] = { L, R };
    double ppq_per_block = (g_ti.tempo / 60.0) * (128.0 / 44100.0);
    int seen[9] = {0}, run = P("s1_run");
    for (int k = 0; k < 1100; k++) {
        a->pr(a, 0, out, 128);
        seen[(int)lroundf(a->getP(a, run) * 8)]++;
        g_ti.ppqPos += ppq_per_block;
    }
    int distinct = 0;
    for (int i = 1; i <= 8; i++) distinct += seen[i] > 0;
    CHECK(distinct >= 6, "running light visited %d of 8 steps while playing", distinct);
    CHECK(automated[run] >= 10, "play-head pushed to the host %d times", automated[run]);
    CHECK(automated[P("s2_run")] >= 10, "line B play-head pushed too (%d)", automated[P("s2_run")]);
    CHECK(maze_vst_note_ons(2) >= 8, "line A (CH 3) sent %d note-ons", maze_vst_note_ons(2));
    CHECK(maze_vst_note_ons(4) >= 1, "line B (CH 5) sent %d note-ons", maze_vst_note_ons(4));
    CHECK(maze_vst_note_ons(0) == 0 && maze_vst_note_ons(1) == 0, "nothing on the default channel");
    g_ti.flags = kTempo;   /* stop */
    a->pr(a, 0, out, 128);
    CHECK(a->getP(a, run) == 0.0f, "running light off after stop");
    a->setP(a, run, 1.0f);
    CHECK(a->getP(a, run) == 0.0f, "running light is display only (a host set is ignored)");
    a->d(a, 26, run, 0, 0, 0);

    /* popups (wrapper/popup.h): a tap opens it, a Q-Link nudge leaves it open, a pick closes it and tells the host once */
    for (int i = 0; i < NPARAMS; i++) {
        if (PARAMS[i].popup_of < 0) continue;
        int t = PARAMS[i].popup_of, no = PARAMS[t].nopts;
        a->setP(a, i, 1.0f);
        int opened = a->getP(a, i) > 0.5f;
        a->setP(a, t, 0.5f / (no - 1)); a->pr(a, 0, out, 128);
        int kept = a->getP(a, i) > 0.5f && !automated[i];
        a->setP(a, t, 1.0f); a->pr(a, 0, out, 128);
        int closed = a->getP(a, i) < 0.5f && automated[i] == 1;
        CHECK(opened && kept && closed, "popup %s: opens %d, nudge keeps it open %d, pick closes it %d", PARAMS[i].key, opened, kept, closed);
    }

    /* a played note transposes (semitones from C4); the plugin's own echoed note-ons must not */
    { typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; uint8_t midiData[4]; int8_t detune, noteOffVelocity, r1, r2; } ME;
      typedef struct { int32_t numEvents; intptr_t reserved; ME *events[1]; } EV;   /* same layout as the plugin's VstEvents */
      ME me = { 1, sizeof(ME), 0, 0, 0, 0, {0x90, 67, 100, 0}, 0, 0, 0, 0 }; EV ev = { 1, 0, { &me } };
      a->d(a, 25, 0, 0, &ev, 0);
      CHECK(maze_vst_pad_semis(a) == 7, "note 67 -> transpose %d (expect 7)", maze_vst_pad_semis(a));
      me.midiData[1] = 48; a->d(a, 25, 0, 0, &ev, 0);
      CHECK(maze_vst_pad_semis(a) == -12, "note 48 -> transpose %d (expect -12)", maze_vst_pad_semis(a));
      void *ci = 0; (void)ci; }
    /* chunk round-trip: parameters AND the pattern (bits + CV) */
    void *chunk = 0;
    intptr_t n = a->d(a, 23, 0, 0, &chunk, 0);
    char saved[4096];
    strncpy(saved, (char *)chunk, sizeof saved - 1); saved[sizeof saved - 1] = 0;
    printf("     chunk %ld bytes: %.100s...\n", (long)n, saved);
    CHECK(strstr(saved, "pattern=") && !strstr(saved, "__open") && !strstr(saved, "_step") && !strstr(saved, "_run"), "chunk has the pattern, no popup flags, no step or running-light keys");
    b->d(b, 24, 0, n, saved, 0);
    void *chunk2 = 0;
    intptr_t n2 = b->d(b, 23, 0, 0, &chunk2, 0);
    CHECK(n2 == n && !strcmp(saved, (char *)chunk2), "instance b's chunk equals a's after setChunk");
    CHECK(fabsf(b->getP(b, P("s1_channel")) - a->getP(a, P("s1_channel"))) < 1e-6f && b->getP(b, P("s1_step2")) == a->getP(a, P("s1_step2")),
          "channel and steps carried over");
    b->d(b, 7, P("transpose"), 0, disp, 0); CHECK(!strcmp(disp, "24"), "b transpose '%s'", disp);

    a->d(a, 1, 0, 0, 0, 0);
    b->d(b, 1, 0, 0, 0, 0);
    printf("%s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
