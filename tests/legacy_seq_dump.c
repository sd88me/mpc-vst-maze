/* The chunk a Maze Sequencer 1.0.8 project holds, made by the TAG's own wrapper (sequencer/vst/maze_seq_vst.cpp) and core,
 * built for x86 by tests/make_legacy_chunks.sh: "key=value;" for every saved parameter plus "pattern=..." (both lines' bits
 * and CV). A few values are moved off their defaults and the transport runs for a while with corrupt up, so the pattern is
 * not the start one. Writes the chunk (with its NUL) to stdout. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
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
static TI ti;
static intptr_t host(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    (void)e; (void)idx; (void)v; (void)p; (void)o;
    return op == 7 ? (intptr_t)&ti : 0;
}
extern AEffect *VSTPluginMain(cb);
static int P(const char *k) { for (int i = 0; i < NPARAMS; i++) if (!strcmp(PARAMS[i].key, k)) return i; fprintf(stderr, "no %s\n", k); return 0; }
static float norm(const char *k, float plain) {
    const param_t *p = &PARAMS[P(k)];
    if (p->nopts) return p->nopts > 1 ? plain / (p->nopts - 1) : 0;
    return (plain - p->min) / (p->max - p->min);
}
int main(void) {
    AEffect *e = VSTPluginMain(host);
    e->d(e, 10, 0, 0, 0, 44100.f); e->d(e, 11, 0, 128, 0, 0); e->d(e, 12, 0, 1, 0, 0);
    struct { const char *k; float v; } set[] = {{"s1_step2", 1}, {"s1_step5", 1}, {"s2_step0", 1}, {"s1_corrupt", 60},
        {"s2_corrupt", 35}, {"s1_length", 6}, {"s2_channel", 3}, {"scale", 2}, {"key", 5}, {"transpose", 7},
        {"note_rate", 1}, {"lfo1_rate", 40}};
    for (size_t i = 0; i < sizeof set / sizeof set[0]; i++) e->setP(e, P(set[i].k), norm(set[i].k, set[i].v));
    float l[128], r[128], *out[2] = {l, r};
    ti.sampleRate = 44100; ti.tempo = 120; ti.timeSigNumerator = ti.timeSigDenominator = 4;
    ti.flags = (1 << 1) | (1 << 9) | (1 << 10);
    for (int b = 0; b < 4000; b++) {   /* ~11.6 s at 120 BPM: many cycles of both lines */
        ti.samplePos = b * 128.0; ti.ppqPos = ti.samplePos / 44100.0 * 2.0;
        e->pr(e, out, out, 128);
    }
    ti.flags = 0; e->pr(e, out, out, 128);
    char *chunk = 0;
    intptr_t n = e->d(e, 23, 0, 0, &chunk, 0);
    if (n <= 0 || !chunk) return 1;
    fwrite(chunk, 1, (size_t)n, stdout);
    e->d(e, 1, 0, 0, 0, 0);   /* effClose */
    return 0;
}
