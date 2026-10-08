/* =============================================================================
 * maze_seq_vst.cpp - Maze Sequencer as a VST2 MIDI generator for the MPC OS plugin host
 * (https://github.com/sd88me/mpc-vst-plugins). Links src/maze_seq_core.c (the same dual 8-step
 * generative sequencer the Force Shadow / MockbaMod build drives) and plays the role host_shim.cpp
 * plays there, but inside MPC's own JUCE plugin host:
 *
 *   host_shim.cpp (MockbaMod)              maze_seq_vst.cpp (MPC plugin)
 *   ------------------------------------   ---------------------------------
 *   RtMidi clock in from the transport     audioMasterGetTime (ppqPos/tempo) ->
 *                                           synthetic 24-PPQN 0xF8/0xFA/0xFC
 *   CC / control socket -> set_param       VST parameters (params.h) -> set_param
 *   host->midi_send_internal -> RtMidi     host->midi_send_internal -> an ALSA seq port
 *                                           (MPC OS ignores a plugin's VST MIDI output)
 *
 * The core's get_param only answers a few keys, so this wrapper keeps the value of every
 * parameter itself (cache[]); only the step bits are read back from the core, since "flip"
 * and "regen" change them. The pattern (bits + CV) travels in the project chunk as the
 * core's "pattern" string (MAZE_VST patch, see src/VENDORED.md).
 * ========================================================================== */
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include <alsa/asoundlib.h>

extern "C" {
#include "maze_seq_host.h"
}
#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "transport_grid.h"   /* song-position anchored clock: why and how in that header */

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

/* ---------------------------------------------------------------------------
 * Core output: midi_send_internal has no instance argument, so each call into the core runs with
 * t_out pointing at the calling instance's outbox; the wrapper drains it to ALSA afterwards.
 * ------------------------------------------------------------------------- */
struct Outbox { uint8_t m[64][3]; int n = 0; };
static thread_local Outbox *t_out = nullptr;
static int host_midi_send(const uint8_t *pkt, int len) {   /* USB-MIDI packet: [cin, status, d1, d2] */
    if (!t_out || len < 4 || t_out->n >= 64) return 0;
    std::memcpy(t_out->m[t_out->n++], pkt + 1, 3);
    return len;
}
static const host_api_v1_t g_host = { host_midi_send };
static plugin_api_v2_t *g_api = nullptr;
static std::mutex g_api_init_lock;
static std::atomic<int> g_instance_count{0};
static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

#ifdef MAZE_VST_TEST   /* host_test.c: what went out, without an ALSA device */
static std::atomic<int> g_note_ons[16];
extern "C" int maze_vst_note_ons(int ch) { return g_note_ons[ch & 15].load(); }
#endif

/* ---------------------------------------------------------------------------
 * Per-instance state
 * ------------------------------------------------------------------------- */
struct Plugin {
    AEffect fx;
    audioMasterCallback master;
    void *inst = nullptr;
    std::mutex lock;               /* serialises every call into the core */
    Outbox out;
    int run_idx[2] = {-1, -1};     /* param index of s1_run / s2_run */
    int step_idx[2][8];            /* param index of s?_step0..7 */
    int last_bits[2][8];           /* step bits the host was last told (-1: not yet read) */
    float cache[NPARAMS];          /* every parameter's value in its own domain (option index / number) */
    volatile int release[NPARAMS] = {0};
    bool held[NPARAMS] = {false};  /* momentary params: host currently reports them pressed */
    float open[NPARAMS] = {0};     /* popup "open" flags (popup.h): wrapper-only, not saved */
    TransportGrid grid;   /* song-position anchored clock (transport_grid.h) */
    float sent_bpm = 0.0f;
    snd_seq_t *seq = nullptr;
    int seq_port = -1;
    char chunk[4096] = {0};
};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void copy_str(void *dst, const std::string &s, size_t max) {
    std::strncpy((char *)dst, s.c_str(), max - 1);
    ((char *)dst)[max - 1] = 0;
}

/* ---- ALSA seq output ----------------------------------------------------- */
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0) { w->seq = nullptr; return; }
    snd_seq_set_client_pool_output(w->seq, 2048);
    int n = g_instance_count.fetch_add(1);
    char name[40];
    if (n == 0) std::snprintf(name, sizeof name, "Maze Sequencer");
    else std::snprintf(name, sizeof name, "Maze Sequencer %d", n + 1);
    snd_seq_set_client_name(w->seq, name);
    w->seq_port = snd_seq_create_simple_port(w->seq, "MIDI Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[maze_seq_vst] ALSA client '%s' port %d\n", name, w->seq_port);
}
static void alsa_flush(Plugin *w) {   /* send and empty w->out; called with w->lock held (UI and audio threads both get here) */
    int n = w->out.n;
    w->out.n = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *m = w->out.m[i];
        uint8_t type = m[0] & 0xF0, ch = m[0] & 0x0F;
#ifdef MAZE_VST_TEST
        if (type == 0x90 && m[2] > 0) g_note_ons[ch]++;
#endif
        if (!w->seq || w->seq_port < 0) continue;
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, w->seq_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        if (type == 0x90 && m[2] > 0) snd_seq_ev_set_noteon(&ev, ch, m[1], m[2]);
        else if (type == 0x80 || type == 0x90) snd_seq_ev_set_noteoff(&ev, ch, m[1], 0);
        else continue;
        snd_seq_event_output(w->seq, &ev);   /* buffered, non-blocking: drops instead of hanging */
    }
    if (w->seq && n) snd_seq_drain_output(w->seq);
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

/* ---- calls into the core --------------------------------------------------- */
static void core_midi(Plugin *w, uint8_t byte) {
    std::lock_guard<std::mutex> lk(w->lock);
    t_out = &w->out; g_api->on_midi(w->inst, &byte, 1, 2); t_out = nullptr;
    alsa_flush(w);
}
static void core_set(Plugin *w, const char *key, const char *val) {
    std::lock_guard<std::mutex> lk(w->lock);
    t_out = &w->out; g_api->set_param(w->inst, key, val); t_out = nullptr;
    alsa_flush(w);
}
static int core_bits(Plugin *w, int seq, int bits[8]) {   /* "<len>|1,0,..|<play>": the bits; returns the play-head (-1 before the first step) */
    char buf[96], key[16];
    std::snprintf(key, sizeof key, "s%d_state", seq + 1);
    int n;
    { std::lock_guard<std::mutex> lk(w->lock); n = g_api->get_param(w->inst, key, buf, sizeof buf); }
    for (int i = 0; i < 8; i++) bits[i] = 0;
    if (n <= 0) return -1;
    const char *p = std::strchr(buf, '|');
    for (int i = 0; p && i < 8; i++) { p++; bits[i] = *p == '1'; p = std::strchr(p, ','); if (!p) break; }
    const char *q = std::strrchr(buf, '|');
    return q && q != std::strchr(buf, '|') ? std::atoi(q + 1) : -1;
}

/* a parameter's step bit lives in the core: s1_step3 -> (0, 3) */
static bool is_step(const param_t *p, int *seq, int *idx) {
    if (std::strncmp(p->key, "s", 1) || (p->key[1] != '1' && p->key[1] != '2') || std::strncmp(p->key + 2, "_step", 5)) return false;
    *seq = p->key[1] - '1';
    *idx = p->key[7] - '0';
    return true;
}
static float to_norm(const param_t *p, float v) {
    if (p->nopts) return p->nopts > 1 ? clamp01(v / (p->nopts - 1)) : 0.0f;
    return p->max > p->min ? clamp01((v - p->min) / (p->max - p->min)) : 0.0f;
}
static float from_norm(const param_t *p, float n) {
    if (p->nopts) return (float)std::lround(clamp01(n) * (p->nopts - 1));
    return (float)std::lround(p->min + (p->max - p->min) * clamp01(n));   /* every core parameter is a whole number */
}
static bool is_run(const param_t *p) { return !std::strcmp(p->key, "s1_run") || !std::strcmp(p->key, "s2_run"); }
static float value_of(Plugin *w, int i) {   /* param domain */
    int seq, idx;
    if (is_step(&PARAMS[i], &seq, &idx)) { int b[8]; core_bits(w, seq, b); return (float)b[idx]; }
    return w->cache[i];
}

/* write a value (param domain) through to the core */
static void apply(Plugin *w, int i, float v) {
    const param_t *p = &PARAMS[i];
    char buf[32];
    int seq, idx;
    if (is_run(p)) return;   /* display only */
    if (is_step(p, &seq, &idx)) {
        int b[8];
        core_bits(w, seq, b);
        if (b[idx] != (int)v) { std::snprintf(buf, sizeof buf, "%d", idx); core_set(w, seq ? "s2_flip" : "s1_flip", buf); }
        return;
    }
    w->cache[i] = v;
    if (!std::strcmp(p->key, "s1_channel") || !std::strcmp(p->key, "s2_channel")) v -= 1;   /* 1..16 -> 0..15 */
    std::snprintf(buf, sizeof buf, "%d", (int)v);
    core_set(w, p->key, buf);
}

/* ---------------------------------------------------------------------------
 * Transport / clock synthesis: audioMasterGetTime -> a 24-PPQN clock, the same byte stream
 * host_shim.cpp fed the core from real MIDI clock. Once per audio block.
 * ------------------------------------------------------------------------- */
struct CoreClock {
    Plugin *w;
    void start() { core_midi(w, 0xFA); }
    void stop() { core_midi(w, 0xFC); }
    void songpos(long m) { char b[24]; std::snprintf(b, sizeof b, "%ld", m); core_set(w, "song_pulse", b); }
    void pulse(long) { core_midi(w, 0xF8); }
};
static void feed_transport(Plugin *w, int32_t frames) {
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstTempoValid | kVstPpqPosValid, 0, 0);
    bool playing = ti && (ti->flags & kVstTransportPlaying);
    if (ti && (ti->flags & kVstTempoValid) && ti->tempo >= 20 && std::fabs((float)ti->tempo - w->sent_bpm) > 0.01f) {
        char b[16];
        std::snprintf(b, sizeof b, "%.2f", ti->tempo);
        w->sent_bpm = (float)ti->tempo;
        core_set(w, "host_bpm", b);   /* LFO sync tempo (MAZE_VST patch) */
    }
    CoreClock clk{w};
    w->grid.block(clk, playing, ti ? ti->ppqPos : 0.0, ti ? ti->tempo : 120.0, ti ? ti->sampleRate : 44100.0, frames);
}

/* ---------------------------------------------------------------------------
 * VST callbacks
 * ------------------------------------------------------------------------- */
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    (void)in;
    Plugin *w = (Plugin *)e->object;
    feed_transport(w, n);
    for (int n2 = 0; n2 < 2; n2++) {
        /* The host does not re-read a toggle or ring by itself, so push what changed on its own (jv880's highlight
         * mechanism): the running light as the play-head moves, and step LEDs when the pattern changes (Advance,
         * Reset, Corrupt). Cheap: one state read per line per block. */
        int b[8], pl = core_bits(w, n2, b);
        for (int i = 0; i < 8; i++) {
            if (w->last_bits[n2][i] >= 0 && w->last_bits[n2][i] != b[i] && w->step_idx[n2][i] >= 0)
                w->master(&w->fx, audioMasterAutomate, w->step_idx[n2][i], 0, 0, (float)b[i]);
            w->last_bits[n2][i] = b[i];
        }
        int ri = w->run_idx[n2];
        if (ri < 0) continue;
        float v = w->grid.was_playing && pl >= 0 && pl < 8 ? (float)(pl + 1) : 0.0f;
        if (v != w->cache[ri]) {
            w->cache[ri] = v;
            w->master(&w->fx, audioMasterAutomate, ri, 0, 0, to_norm(&PARAMS[ri], v));
        }
    }
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; w->held[i] = false; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    for (int32_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;   /* MIDI generator: no audio */
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
        if (rising) { core_set(w, p->key, "1"); w->release[i] = 1; }
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {
        /* An exact option selects it; anything between options is a Q-Link/encoder nudge from the
         * current one: step one option that way (same convention as wrapper/vst2_wrap.c). */
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = value_of(w, i);
            int idx = (int)std::lround(cur) + (pos > cur ? 1 : -1);
            if (idx < 0) idx = 0;
            if (idx > p->nopts - 1) idx = p->nopts - 1;
            n = (float)idx / (p->nopts - 1);
            nudge = true;
        }
    }
    apply(w, i, from_norm(p, n));
    if (!nudge) popup_picked(w->open, w->release, i);   /* a list pick closes it; a Q-Link nudge doesn't */
}

static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    if (PARAMS[i].momentary) return 0.0f;   /* triggers always read released */
    return to_norm(&PARAMS[i], value_of(w, i));
}

static void reset_defaults(Plugin *w) {
    for (int i = 0; i < NPARAMS; i++) w->cache[i] = PARAMS[i].nopts ? (float)std::lround(PARAMS[i].def * (PARAMS[i].nopts - 1))
                                                                      : (float)std::lround(PARAMS[i].min + (PARAMS[i].max - PARAMS[i].min) * PARAMS[i].def);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    (void)o;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        { std::lock_guard<std::mutex> lk(w->lock);
          t_out = &w->out; g_api->destroy_instance(w->inst); t_out = nullptr;
          alsa_flush(w);   /* the core's final note-off */
          alsa_close(w); }
        delete w;
        return 1;
    case effGetPlugCategory: return 2; /* kPlugCategSynth */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && !is_run(&PARAMS[idx]);
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
    case effProcessEvents: return 1;   /* the sequencer takes no MIDI input (the core ignores notes) */
    case effCanDo:
        return (!std::strcmp((char *)p, "receiveVstTimeInfo")) ? 1 : -1;
    case effGetChunk: {
        std::string s;
        for (int i = 0; i < NPARAMS; i++) {
            int seq, st;
            if (PARAMS[i].momentary || popup_is(i) || is_run(&PARAMS[i]) || is_step(&PARAMS[i], &seq, &st)) continue;
            char b[32];
            std::snprintf(b, sizeof b, "%ld", std::lround(w->cache[i]));
            s += PARAMS[i].key; s += '='; s += b; s += ';';
        }
        char pat[256];
        int n;
        { std::lock_guard<std::mutex> lk(w->lock); n = g_api->get_param(w->inst, "pattern", pat, sizeof pat); }
        if (n > 0) { s += "pattern="; s += pat; s += ';'; }
        copy_str(w->chunk, s, sizeof w->chunk);
        *(void **)p = w->chunk;
        return (intptr_t)std::strlen(w->chunk) + 1;
    }
    case effSetChunk: {
        if (v <= 0 || (size_t)v > sizeof w->chunk) return 0;
        std::memcpy(w->chunk, p, (size_t)v);
        w->chunk[v - 1] = 0;
        char *save = nullptr, *pattern = nullptr;
        for (char *tok = strtok_r(w->chunk, ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
            char *eq = std::strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            if (!std::strcmp(tok, "pattern")) { pattern = eq + 1; continue; }
            for (int i = 0; i < NPARAMS; i++)
                if (!std::strcmp(PARAMS[i].key, tok)) { apply(w, i, (float)std::atof(eq + 1)); break; }
        }
        if (pattern) core_set(w, "pattern", pattern);   /* last: the bits and CV of both lines */
        return 1;
    }
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/maze_seq_vst.log", "a");
    {
        std::lock_guard<std::mutex> lk(g_api_init_lock);
        if (!g_api) {
            g_api = move_plugin_init_v2(&g_host);
            if (!g_api || g_api->api_version != MOVE_PLUGIN_API_VERSION_2) { LOG("[maze_seq_vst] core init failed\n"); g_api = nullptr; return nullptr; }
        }
    }
    Plugin *w = new Plugin();
    w->master = master;
    w->inst = g_api->create_instance("", nullptr);   /* "" : no state file (MAZE_VST) */
    if (!w->inst) { LOG("[maze_seq_vst] create_instance failed\n"); delete w; return nullptr; }
    reset_defaults(w);
    for (int n2 = 0; n2 < 2; n2++) for (int i = 0; i < 8; i++) { w->step_idx[n2][i] = -1; w->last_bits[n2][i] = -1; }
    for (int i = 0; i < NPARAMS; i++) { int sq, st; if (is_step(&PARAMS[i], &sq, &st)) w->step_idx[sq][st] = i; }
    for (int i = 0; i < NPARAMS; i++) { if (!std::strcmp(PARAMS[i].key, "s1_run")) w->run_idx[0] = i; if (!std::strcmp(PARAMS[i].key, "s2_run")) w->run_idx[1] = i; }
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
    LOG("[maze_seq_vst] up, %d params\n", NPARAMS);
    return e;
}
