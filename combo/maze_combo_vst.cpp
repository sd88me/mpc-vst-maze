/* =============================================================================
 * maze_combo_vst.cpp - Maze Sequencer + Maze Voice in one VST2 instrument for the MPC OS plugin host.
 *
 * Two cores run side by side in every instance:
 *   - the voice (src/maze_voice.c), rendered to the plugin's stereo output like the generic wrapper does, and
 *   - the sequencer (sequencer/src/maze_seq_core.c), clocked from the host transport like maze_seq_vst.cpp does.
 *
 * Two parameters decide who plays what:
 *   q_on   OFF  the sequencer is stopped; host notes play the voice like a normal synth.
 *          ON   the sequencer runs with the transport; host note-ons only TRANSPOSE it (note 60 = no shift,
 *               the core's "pad_semis"), and never reach the voice.
 *   q_out  VOICE        sequencer notes go to the voice only
 *          VOICE+MIDI   ... and also out the ALSA seq port "Maze Combo" (for other tracks)
 *          MIDI ONLY    sequencer notes go to the port only; the voice stays silent
 *
 * The cores each export move_plugin_init_v2, so they are built with -Dmove_plugin_init_v2=<own name>
 * (build.sh). Combined parameter keys: the sequencer's carry a "q_" prefix that is stripped before a key
 * reaches the sequencer core; the voice's keys are unchanged (gen.py).
 * ========================================================================== */
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include <alsa/asoundlib.h>

extern "C" {
#define move_plugin_init_v2 maze_seq_plugin_init   /* the sequencer core's exported name (same -D when compiling it) */
#include "maze_seq_host.h"
#undef move_plugin_init_v2
plugin_api_v2_t *maze_voice_plugin_init(const void *host);   /* same ABI; the voice core's exported name */
}
#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

#define DSP_BLOCK 128

/* ---------------------------------------------------------------------------
 * Sequencer core output: midi_send_internal has no instance argument, so each call into the sequencer core runs with
 * t_out pointing at the calling instance's outbox; route_out() then sends it to the voice and/or the ALSA port.
 * ------------------------------------------------------------------------- */
struct Outbox { uint8_t m[64][3]; int n = 0; };
static thread_local Outbox *t_out = nullptr;
static int host_midi_send(const uint8_t *pkt, int len) {   /* USB-MIDI packet: [cin, status, d1, d2] */
    if (!t_out || len < 4 || t_out->n >= 64) return 0;
    std::memcpy(t_out->m[t_out->n++], pkt + 1, 3);
    return len;
}
static const host_api_v1_t g_host = { host_midi_send };
static plugin_api_v2_t *g_seq = nullptr, *g_voice = nullptr;
static std::mutex g_api_init_lock;
static std::atomic<int> g_instance_count{0};
static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

#ifdef MAZE_VST_TEST   /* host_test.c: what went where, without an ALSA device */
static std::atomic<int> g_ext_note_ons[16], g_voice_note_ons{0}, g_ext_last_note{-1};
extern "C" int maze_vst_note_ons(int ch) { return g_ext_note_ons[ch & 15].load(); }
extern "C" int maze_vst_last_ext_note(void) { return g_ext_last_note.load(); }
extern "C" int maze_vst_voice_note_ons(void) { return g_voice_note_ons.load(); }
#endif

enum Kind { K_SEQ, K_VOICE, K_ON, K_OUT };
enum { OUT_VOICE = 0, OUT_BOTH = 1, OUT_MIDI = 2 };

/* ---------------------------------------------------------------------------
 * Per-instance state
 * ------------------------------------------------------------------------- */
struct Plugin {
    AEffect fx;
    audioMasterCallback master;
    void *seq_inst = nullptr, *voice_inst = nullptr;
    std::mutex lock;               /* serialises every call into either core */
    Outbox out;
    int16_t block[DSP_BLOCK * 2];  /* the voice's current 128-frame block */
    int pos = DSP_BLOCK;
    int run_idx[2] = {-1, -1};     /* param index of s1_run / s2_run */
    int step_idx[2][8];            /* param index of s?_step0..7 */
    int last_bits[2][8];           /* step bits the host was last told (-1: not yet read) */
    float cache[NPARAMS];          /* wrapper and sequencer parameters in their own domain (option index / number) */
    volatile int release[NPARAMS] = {0};
    bool held[NPARAMS] = {false};  /* momentary params: host currently reports them pressed */
    float open[NPARAMS] = {0};     /* popup "open" flags (popup.h): wrapper-only, not saved */
    double last_ppq = 0.0;
    bool seq_run = false;          /* the sequencer core has been started (transport playing and q_on) */
    float sent_bpm = 0.0f;
    snd_seq_t *seq = nullptr;
    int seq_port = -1;
    char chunk[12288] = {0};
};

static Kind kind_of[NPARAMS];
static int idx_on = -1, idx_out = -1;

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void copy_str(void *dst, const std::string &s, size_t max) {
    std::strncpy((char *)dst, s.c_str(), max - 1);
    ((char *)dst)[max - 1] = 0;
}
static const char *skey(int i) { return PARAMS[i].key + (kind_of[i] == K_SEQ ? 2 : 0); }   /* the key the owning core knows */
static bool seq_on(Plugin *w) { return w->cache[idx_on] > 0.5f; }
static int out_mode(Plugin *w) { return (int)std::lround(w->cache[idx_out]); }

/* ---- ALSA seq output ----------------------------------------------------- */
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0) { w->seq = nullptr; return; }
    snd_seq_set_client_pool_output(w->seq, 2048);
    int n = g_instance_count.fetch_add(1);
    char name[40];
    if (n == 0) std::snprintf(name, sizeof name, "Maze Combo");
    else std::snprintf(name, sizeof name, "Maze Combo %d", n + 1);
    snd_seq_set_client_name(w->seq, name);
    w->seq_port = snd_seq_create_simple_port(w->seq, "MIDI Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[maze_combo_vst] ALSA client '%s' port %d\n", name, w->seq_port);
}
static void alsa_note(Plugin *w, const uint8_t *m) {
    uint8_t type = m[0] & 0xF0, ch = m[0] & 0x0F;
#ifdef MAZE_VST_TEST
    if (type == 0x90 && m[2] > 0) { g_ext_note_ons[ch]++; g_ext_last_note = m[1]; }
#endif
    if (!w->seq || w->seq_port < 0) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, w->seq_port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    if (type == 0x90 && m[2] > 0) snd_seq_ev_set_noteon(&ev, ch, m[1], m[2]);
    else if (type == 0x80 || type == 0x90) snd_seq_ev_set_noteoff(&ev, ch, m[1], 0);
    else return;
    snd_seq_event_output(w->seq, &ev);   /* buffered, non-blocking: drops instead of hanging */
}
static void alsa_close(Plugin *w) {
    if (!w->seq) return;
    for (int ch = 0; ch < 16; ch++) {   /* nothing left hanging on the track we fed */
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, w->seq_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        ev.type = SND_SEQ_EVENT_CONTROLLER;
        ev.data.control.channel = ch;
        ev.data.control.param = 123;   /* all notes off */
        snd_seq_event_output_direct(w->seq, &ev);
    }
    snd_seq_close(w->seq);
    w->seq = nullptr;
}

/* Route what the sequencer core just produced. Called with w->lock held. A note-on is what the voice needs (its
 * envelopes are decay-only, so note-offs mean nothing to it); the ALSA port gets note-ons and note-offs.
 * Everything is dropped while q_on is OFF (a core's final note-off after "stop" still reaches the port). */
static void route_out(Plugin *w, bool force_ext = false) {
    int n = w->out.n;
    w->out.n = 0;
    int mode = out_mode(w);
    bool to_voice = seq_on(w) && mode != OUT_MIDI;
    bool to_ext = (seq_on(w) && mode != OUT_VOICE) || force_ext;
    bool sent = false;
    for (int i = 0; i < n; i++) {
        const uint8_t *m = w->out.m[i];
        if ((m[0] & 0xF0) == 0x90 && m[2] > 0 && to_voice) {
            uint8_t on[3] = { 0x90, m[1], m[2] };
#ifdef MAZE_VST_TEST
            g_voice_note_ons++;
#endif
            g_voice->on_midi(w->voice_inst, on, 3, 0);
        }
        if (to_ext) { alsa_note(w, m); sent = true; }
    }
    if (sent && w->seq) snd_seq_drain_output(w->seq);
}

/* ---- calls into the cores --------------------------------------------------- */
static void seq_midi(Plugin *w, uint8_t byte, bool force_ext = false) {
    std::lock_guard<std::mutex> lk(w->lock);
    t_out = &w->out; g_seq->on_midi(w->seq_inst, &byte, 1, 2); t_out = nullptr;
    route_out(w, force_ext);
}
static void seq_set(Plugin *w, const char *key, const char *val) {
    std::lock_guard<std::mutex> lk(w->lock);
    t_out = &w->out; g_seq->set_param(w->seq_inst, key, val); t_out = nullptr;
    route_out(w);
}
static void voice_set(Plugin *w, const char *key, const char *val) {
    std::lock_guard<std::mutex> lk(w->lock);
    g_voice->set_param(w->voice_inst, key, val);
}
static int voice_get(Plugin *w, const char *key, char *buf, int len) {
    std::lock_guard<std::mutex> lk(w->lock);
    return g_voice->get_param(w->voice_inst, key, buf, len);
}
static int core_bits(Plugin *w, int seq, int bits[8]) {   /* "<len>|1,0,..|<play>": the bits; returns the play-head (-1 before the first step) */
    char buf[96], key[16];
    std::snprintf(key, sizeof key, "s%d_state", seq + 1);
    int n;
    { std::lock_guard<std::mutex> lk(w->lock); n = g_seq->get_param(w->seq_inst, key, buf, sizeof buf); }
    for (int i = 0; i < 8; i++) bits[i] = 0;
    if (n <= 0) return -1;
    const char *p = std::strchr(buf, '|');
    for (int i = 0; i < 8 && p; i++) { p++; bits[i] = *p == '1'; p = std::strchr(p, ','); if (!p) break; }
    const char *q = std::strrchr(buf, '|');
    return q && q != std::strchr(buf, '|') ? std::atoi(q + 1) : -1;
}

/* a parameter's step bit lives in the sequencer core: q_s1_step3 -> (0, 3) */
static bool is_step(int i, int *seq, int *idx) {
    if (kind_of[i] != K_SEQ) return false;
    const char *k = skey(i);
    if (k[0] != 's' || (k[1] != '1' && k[1] != '2') || std::strncmp(k + 2, "_step", 5)) return false;
    *seq = k[1] - '1';
    *idx = k[7] - '0';
    return true;
}
static bool is_run(int i) { return kind_of[i] == K_SEQ && (!std::strcmp(skey(i), "s1_run") || !std::strcmp(skey(i), "s2_run")); }

static float to_norm(const param_t *p, float v) {
    if (p->nopts) return p->nopts > 1 ? clamp01(v / (p->nopts - 1)) : 0.0f;
    return p->max > p->min ? clamp01((v - p->min) / (p->max - p->min)) : 0.0f;
}
static float from_norm(const param_t *p, float n) {   /* sequencer and wrapper parameters are whole numbers */
    if (p->nopts) return (float)std::lround(clamp01(n) * (p->nopts - 1));
    return (float)std::lround(p->min + (p->max - p->min) * clamp01(n));
}

/* the voice's own parameters travel as the core's strings (the generic wrapper's convention) */
static void voice_norm_to_str(const param_t *p, float n, char *buf, int len) {
    if (p->nopts) std::snprintf(buf, len, "%d", (int)std::lround(clamp01(n) * (p->nopts - 1)));
    else std::snprintf(buf, len, "%g", p->min + (p->max - p->min) * clamp01(n));
}
static float voice_str_to_norm(const param_t *p, const char *s) {
    if (p->nopts) {
        int idx = -1;
        if (std::isdigit((unsigned char)s[0])) idx = std::atoi(s);
        else for (int i = 0; i < p->nopts; i++) if (!strcasecmp(s, p->opts[i])) idx = i;
        if (idx < 0) idx = 0;
        return p->nopts > 1 ? (float)idx / (p->nopts - 1) : 0;
    }
    return p->max > p->min ? clamp01((float)((std::atof(s) - p->min) / (p->max - p->min))) : 0;
}
static float voice_norm(Plugin *w, int i) {
    char buf[64];
    if (voice_get(w, PARAMS[i].key, buf, sizeof buf) <= 0) return PARAMS[i].def;
    return voice_str_to_norm(&PARAMS[i], buf);
}

static float value_of(Plugin *w, int i) {   /* sequencer / wrapper params, in the param's own domain */
    int seq, idx;
    if (is_step(i, &seq, &idx)) { int b[8]; core_bits(w, seq, b); return (float)b[idx]; }
    return w->cache[i];
}

/* the mode switches changed: start or stop the sequencer, and stop its notes hanging on the port */
static void transport_state(Plugin *w, bool playing, double ppq) {
    bool want = playing && seq_on(w);
    if (want && !w->seq_run) { w->seq_run = true; w->last_ppq = ppq; seq_midi(w, 0xFA); }
    else if (!want && w->seq_run) { w->seq_run = false; seq_midi(w, 0xFC, true); }
}
static bool g_playing_hint = false;   /* last transport state seen (per process is fine: one transport) */
static double g_ppq_hint = 0.0;

/* write a value (sequencer / wrapper param domain) through to its owner */
static void apply(Plugin *w, int i, float v) {
    const param_t *p = &PARAMS[i];
    char buf[32];
    int seq, idx;
    if (kind_of[i] == K_VOICE) { voice_norm_to_str(p, to_norm(p, v), buf, sizeof buf); voice_set(w, p->key, buf); return; }
    if (is_run(i)) return;   /* display only */
    if (is_step(i, &seq, &idx)) {
        int b[8];
        core_bits(w, seq, b);
        if (b[idx] != (int)v) { std::snprintf(buf, sizeof buf, "%d", idx); seq_set(w, seq ? "s2_flip" : "s1_flip", buf); }
        return;
    }
    w->cache[i] = v;
    if (kind_of[i] == K_ON || kind_of[i] == K_OUT) {
        if (kind_of[i] == K_ON) transport_state(w, g_playing_hint, g_ppq_hint);
        return;
    }
    const char *k = skey(i);
    if (!std::strcmp(k, "s1_channel") || !std::strcmp(k, "s2_channel")) v -= 1;   /* 1..16 -> 0..15 */
    std::snprintf(buf, sizeof buf, "%d", (int)v);
    seq_set(w, k, buf);
}

/* ---------------------------------------------------------------------------
 * Transport / clock synthesis: audioMasterGetTime -> a 24-PPQN clock for the sequencer, once per audio block,
 * and the tempo for both cores' synced LFOs.
 * ------------------------------------------------------------------------- */
static void feed_transport(Plugin *w) {
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstTempoValid | kVstPpqPosValid, 0, 0);
    bool playing = ti && (ti->flags & kVstTransportPlaying);
    g_playing_hint = playing;
    g_ppq_hint = ti ? ti->ppqPos : 0.0;
    if (ti && (ti->flags & kVstTempoValid) && ti->tempo >= 20 && std::fabs((float)ti->tempo - w->sent_bpm) > 0.01f) {
        char b[16];
        std::snprintf(b, sizeof b, "%.2f", ti->tempo);
        w->sent_bpm = (float)ti->tempo;
        seq_set(w, "host_bpm", b);     /* LFO sync tempo (MAZE_VST patch) */
        voice_set(w, "lfo_bpm", b);
    }
    transport_state(w, playing, ti ? ti->ppqPos : 0.0);
    if (w->seq_run && ti) {
        const double step = 1.0 / 24.0;   /* 24 PPQN, in quarter notes */
        double start = w->last_ppq, end = ti->ppqPos;
        if (end < start || end - start > 1.0) start = end;   /* loop/rewind/jump: resync, don't flood pulses */
        double next = std::ceil(start / step) * step;
        for (; next < end + 1e-9; next += step) seq_midi(w, 0xF8);
        w->last_ppq = ti->ppqPos;
    }
}

/* ---------------------------------------------------------------------------
 * VST callbacks
 * ------------------------------------------------------------------------- */
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    (void)in;
    Plugin *w = (Plugin *)e->object;
    feed_transport(w);
    for (int n2 = 0; n2 < 2; n2++) {
        /* The host does not re-read a toggle or ring by itself, so push what changed on its own: the running light as
         * the play-head moves, and step LEDs when the pattern changes (Advance, Reset, Corrupt). */
        int b[8], pl = core_bits(w, n2, b);
        for (int i = 0; i < 8; i++) {
            if (w->last_bits[n2][i] >= 0 && w->last_bits[n2][i] != b[i] && w->step_idx[n2][i] >= 0)
                w->master(&w->fx, audioMasterAutomate, w->step_idx[n2][i], 0, 0, to_norm(&PARAMS[w->step_idx[n2][i]], (float)b[i]));
            w->last_bits[n2][i] = b[i];
        }
        int ri = w->run_idx[n2];
        if (ri < 0) continue;
        float v = w->seq_run && pl >= 0 && pl < 8 ? (float)(pl + 1) : 0.0f;
        if (v != w->cache[ri]) {
            w->cache[ri] = v;
            w->master(&w->fx, audioMasterAutomate, ri, 0, 0, to_norm(&PARAMS[ri], v));
        }
    }
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; w->held[i] = false; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    for (int32_t i = 0; i < n; i++) {
        if (w->pos >= DSP_BLOCK) {
            { std::lock_guard<std::mutex> lk(w->lock); g_voice->render_block(w->voice_inst, w->block, DSP_BLOCK); }
            w->pos = 0;
        }
        out[0][i] = w->block[w->pos * 2] * (1.0f / 32768.0f);
        out[1][i] = w->block[w->pos * 2 + 1] * (1.0f / 32768.0f);
        w->pos++;
    }
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    if (p->momentary) {
        bool down = n > 0.5f;
        bool rising = down && !w->held[i];   /* an echo of our own automate must not re-fire the trigger */
        w->held[i] = down;
        if (rising) {
            if (kind_of[i] == K_VOICE) voice_set(w, p->key, "1"); else seq_set(w, skey(i), "1");
            w->release[i] = 1;
        }
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {
        /* An exact option selects it; anything between options is a Q-Link/encoder nudge from the
         * current one: step one option that way (same convention as wrapper/vst2_wrap.c). */
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = kind_of[i] == K_VOICE ? voice_norm(w, i) * (p->nopts - 1) : value_of(w, i);
            int idx = (int)std::lround(cur) + (pos > cur ? 1 : -1);
            if (idx < 0) idx = 0;
            if (idx > p->nopts - 1) idx = p->nopts - 1;
            n = (float)idx / (p->nopts - 1);
            nudge = true;
        }
    }
    if (kind_of[i] == K_VOICE) {
        char buf[32];
        voice_norm_to_str(p, n, buf, sizeof buf);
        voice_set(w, p->key, buf);
    } else apply(w, i, from_norm(p, n));
    if (!nudge) popup_picked(w->open, w->release, i);   /* a list pick closes it; a Q-Link nudge doesn't */
}

static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    if (PARAMS[i].momentary) return 0.0f;   /* triggers always read released */
    if (kind_of[i] == K_VOICE) return voice_norm(w, i);
    return to_norm(&PARAMS[i], value_of(w, i));
}

static void reset_defaults(Plugin *w) {
    for (int i = 0; i < NPARAMS; i++) w->cache[i] = PARAMS[i].nopts ? (float)std::lround(PARAMS[i].def * (PARAMS[i].nopts - 1))
                                                                      : (float)std::lround(PARAMS[i].min + (PARAMS[i].max - PARAMS[i].min) * PARAMS[i].def);
}

static void host_event(Plugin *w, const uint8_t *m) {
    if (seq_on(w)) {
        /* sequencer on: a played note only transposes the sequence (C = no shift); nothing reaches the voice */
        if ((m[0] & 0xF0) == 0x90 && m[2] > 0) {
            int semis = (int)m[1] - 60;
            if (semis < -60) semis = -60;
            if (semis > 60) semis = 60;
            char b[16];
            std::snprintf(b, sizeof b, "%d", semis);
            seq_set(w, "pad_semis", b);
        }
        return;
    }
    std::lock_guard<std::mutex> lk(w->lock);
    g_voice->on_midi(w->voice_inst, m, 3, 0);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    (void)o;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        { std::lock_guard<std::mutex> lk(w->lock);
          t_out = &w->out; g_seq->destroy_instance(w->seq_inst); t_out = nullptr;
          route_out(w, true);   /* the core's final note-off */
          g_voice->destroy_instance(w->voice_inst);
          alsa_close(w); }
        delete w;
        return 1;
    case effGetPlugCategory: return 2; /* kPlugCategSynth */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && !is_run(idx);
    case effGetParamName:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32);
        return 1;
    case effGetParamLabel:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8);
        return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        const param_t *pp = &PARAMS[idx];
        if (pp->momentary) { copy_str(p, "", 24); return 1; }
        if (kind_of[idx] == K_VOICE) {
            char buf[64];
            if (pp->nopts) {
                int k = (int)std::lround(voice_norm(w, idx) * (pp->nopts - 1));
                copy_str(p, pp->opts[k < 0 ? 0 : k >= pp->nopts ? pp->nopts - 1 : k], 24);
            } else if (voice_get(w, pp->key, buf, sizeof buf) > 0) {
                char disp[32];
                std::snprintf(disp, sizeof disp, "%.*f", std::fabs(pp->max - pp->min) > 20 ? 0 : 1, std::atof(buf));
                copy_str(p, disp, 24);
            }
            return 1;
        }
        float val = value_of(w, idx);
        if (pp->nopts) {
            int k = (int)std::lround(val);
            copy_str(p, pp->opts[k < 0 ? 0 : k >= pp->nopts ? pp->nopts - 1 : k], 24);
        } else {
            char disp[32];
            std::snprintf(disp, sizeof disp, "%ld", std::lround(val));
            copy_str(p, disp, 24);
        }
        return 1;
    }
    case effSetSampleRate: case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: {
        VstEvents *ev = (VstEvents *)p;
        for (int i = 0; i < ev->numEvents; i++)
            if (ev->events[i]->type == 1) host_event(w, ((VstMidiEvent *)ev->events[i])->midiData);
        return 1;
    }
    case effCanDo:
        return (!std::strcmp((char *)p, "receiveVstEvents") || !std::strcmp((char *)p, "receiveVstMidiEvent") ||
                !std::strcmp((char *)p, "receiveVstTimeInfo")) ? 1 : -1;
    case effGetChunk: {
        std::string s;
        for (int i = 0; i < NPARAMS; i++) {
            int seq, st;
            if (kind_of[i] == K_VOICE || PARAMS[i].momentary || popup_is(i) || is_run(i) || is_step(i, &seq, &st)) continue;
            char b[32];
            std::snprintf(b, sizeof b, "%ld", std::lround(w->cache[i]));
            s += PARAMS[i].key; s += '='; s += b; s += ';';
        }
        char buf[4096];
        int n;
        { std::lock_guard<std::mutex> lk(w->lock); n = g_seq->get_param(w->seq_inst, "pattern", buf, 256); }
        if (n > 0) { s += "pattern="; s += buf; s += ';'; }
        if (voice_get(w, "state", buf, sizeof buf) > 0) { s += "voice="; s += buf; s += ';'; }
        copy_str(w->chunk, s, sizeof w->chunk);
        *(void **)p = w->chunk;
        return (intptr_t)std::strlen(w->chunk) + 1;
    }
    case effSetChunk: {
        if (v <= 0 || (size_t)v > sizeof w->chunk) return 0;
        std::memcpy(w->chunk, p, (size_t)v);
        w->chunk[v - 1] = 0;
        char *save = nullptr, *pattern = nullptr, *voice = nullptr;
        for (char *tok = strtok_r(w->chunk, ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
            char *eq = std::strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            if (!std::strcmp(tok, "pattern")) { pattern = eq + 1; continue; }
            if (!std::strcmp(tok, "voice")) { voice = eq + 1; continue; }
            for (int i = 0; i < NPARAMS; i++)
                if (kind_of[i] != K_VOICE && !std::strcmp(PARAMS[i].key, tok)) { apply(w, i, (float)std::atof(eq + 1)); break; }
        }
        if (pattern) seq_set(w, "pattern", pattern);   /* after the sequencer parameters: the bits and CV of both lines */
        if (voice) voice_set(w, "state", voice);
        return 1;
    }
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/maze_combo_vst.log", "a");
    {
        std::lock_guard<std::mutex> lk(g_api_init_lock);
        if (!g_seq) {
            g_seq = maze_seq_plugin_init(&g_host);
            g_voice = maze_voice_plugin_init(nullptr);
            if (!g_seq || g_seq->api_version != MOVE_PLUGIN_API_VERSION_2 || !g_voice) { LOG("[maze_combo_vst] core init failed\n"); g_seq = nullptr; return nullptr; }
        }
    }
    for (int i = 0; i < NPARAMS; i++) {
        const char *k = PARAMS[i].key;
        kind_of[i] = !std::strcmp(k, "q_on") ? K_ON : !std::strcmp(k, "q_out") ? K_OUT : !std::strncmp(k, "q_", 2) ? K_SEQ : K_VOICE;
        if (kind_of[i] == K_ON) idx_on = i;
        if (kind_of[i] == K_OUT) idx_out = i;
    }
    Plugin *w = new Plugin();
    w->master = master;
    w->seq_inst = g_seq->create_instance("", nullptr);   /* "" : no state file (MAZE_VST) */
    w->voice_inst = g_voice->create_instance(nullptr, nullptr);
    if (!w->seq_inst || !w->voice_inst) { LOG("[maze_combo_vst] create_instance failed\n"); delete w; return nullptr; }
    reset_defaults(w);
    for (int n2 = 0; n2 < 2; n2++) for (int i = 0; i < 8; i++) { w->step_idx[n2][i] = -1; w->last_bits[n2][i] = -1; }
    for (int i = 0; i < NPARAMS; i++) {
        int sq, st;
        if (is_step(i, &sq, &st)) w->step_idx[sq][st] = i;
        if (kind_of[i] == K_SEQ && !std::strcmp(skey(i), "s1_run")) w->run_idx[0] = i;
        if (kind_of[i] == K_SEQ && !std::strcmp(skey(i), "s2_run")) w->run_idx[1] = i;
    }
    alsa_open(w);

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[maze_combo_vst] up, %d params\n", NPARAMS);
    return e;
}
