// SequencerPlugin.cpp - Maze Sequencer 2: the mpc::Plugin front end around the Maze sequencer core
// (sequencer/src/maze_seq_core.c, built with MAZE_VST as in 1.x).
//
// What carries over from the 1.0.8 wrapper (sequencer/vst/maze_seq_vst.cpp), unchanged in behaviour:
//   * the core is owned by the audio thread (maze_common.h), fed a 24-PPQN clock made from the host's song position
//     (sequencer/vst/transport_grid.h), and its notes leave through an ALSA sequencer port ("Maze Sequencer", "... 2", ...),
//     since the MPC ignores a plugin's VST MIDI output;
//   * a played note transposes the pattern (semitones from C4), except our own notes looped back by the input monitor;
//   * step bits and the play-head are the core's: the screen follows them as they change;
//   * a project holds "key=value;" for every saved parameter plus "pattern=..." (both lines' bits and CV). A 1.x project
//     chunk is exactly that text; a 2.x chunk is the SDK's "MPCP" chunk with the same text as its extra state.
// What is new: parameter values live in the SDK (the host reads them without calling in), a worker thread tells the host
// what changed on its own, parameter 0's prepare nudge is debounced, and the four 1.x popup flags are one "ui_open".
#include "../../common/maze_common.h"

#include <alsa/asoundlib.h>

extern "C" {
#include "maze_seq_host.h"
}
#include "transport_grid.h"

#ifndef MAZE_SEQ_NAME
#define MAZE_SEQ_NAME "Maze Sequencer"
#endif
#ifndef MAZE_SEQ_UID
#define MAZE_SEQ_UID 'M', 'z', 'S', 'q'
#endif
#define MAZE_FOURCC(...) MPC_FOURCC(__VA_ARGS__)
#ifndef MAZE_SEQ_VERSION
#define MAZE_SEQ_VERSION 2000
#endif

namespace {

using maze::nowMs;

// midi_send_internal has no instance argument: each call into the core runs with t_out pointing at the caller's outbox.
struct Outbox { uint8_t m[64][3]; int n = 0; };
thread_local Outbox* t_out = nullptr;
int hostMidiSend(const uint8_t* pkt, int len)   // USB-MIDI packet: [cin, status, d1, d2]
{
    if (!t_out || len < 4 || t_out->n >= 64) return 0;
    std::memcpy(t_out->m[t_out->n++], pkt + 1, 3);
    return len;
}
const host_api_v1_t g_host = { hostMidiSend };
plugin_api_v2_t* g_api = nullptr;
std::atomic<int> g_instances{0};

enum { kCmdSet = 1, kCmdFire, kCmdStep, kCmdPattern };

mpc::PluginInfo makeInfo()
{
    mpc::PluginInfo i;
    i.name = MAZE_SEQ_NAME;
    i.vendor = "sd88me";
    i.uniqueId = MAZE_FOURCC(MAZE_SEQ_UID);
    i.version = MAZE_SEQ_VERSION;
    i.isInstrument = true;
    i.numInputs = 0;
    i.numOutputs = 2;
    i.sendsMidi = true;
    return i;
}

bool isStep(int i, int* line, int* step)
{
    if (!(g_pspecs[i].flags & kStep)) return false;
    *line = g_pspecs[i].key[1] - '1';
    *step = g_pspecs[i].key[7] - '0';
    return true;
}

#ifdef MAZE_TEST
std::atomic<int> g_noteOns[16];
#endif

class SequencerPlugin : public mpc::Plugin
{
public:
    SequencerPlugin() : mpc::Plugin(makeInfo())
    {
        for (int i = 0; i < kNumParams; i++) addParam(maze::makeParam(g_pspecs[i]));
        for (int i = 0; i < kNumParams; i++)
        {
            held_[i].store(false); release_[i].store(false); told_[i].store(get(i)); lastPos_[i] = -1.0f;
            int l, s;
            if (isStep(i, &l, &s)) stepIdx_[l][s] = i;
        }
        runIdx_[0] = maze::indexOf("s1_run"); runIdx_[1] = maze::indexOf("s2_run");
        uiOpen_ = -1;
        for (int i = 0; i < kNumParams; i++) if (g_pspecs[i].flags & kUiOpen) uiOpen_ = i;
        if (!maze::logFile()) maze::logFile() = std::fopen("/tmp/maze_seq_vst.log", "a");
        if (!g_api) g_api = move_plugin_init_v2(&g_host);
        if (g_api && g_api->api_version == MOVE_PLUGIN_API_VERSION_2)
        {
            t_out = &out_; inst_ = g_api->create_instance("", nullptr); t_out = nullptr;
        }
        if (!inst_) MLOG("[maze_seq] core init failed\n");
        // The core starts with its own defaults; give it the table's (the host reads these from the first moment).
        for (int i = 0; i < kNumParams; i++) sendValue(i, get(i), true);
        ring_.drain([this](const maze::Cmd& c) { applyCmd(c); }, 100000);
        publishLines();
        for (int l = 0; l < 2; l++) for (int s = 0; s < 8; s++) store(stepIdx_[l][s], (float)sbits_[l][s].load());
        alsaOpen();
        worker_ = std::thread(&SequencerPlugin::workerMain, this);
        MLOG("[maze_seq] up, %d params\n", kNumParams);
    }

    ~SequencerPlugin() override
    {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        while (busy_.exchange(true)) sched_yield();   // never released: the plugin is going away
        if (inst_) { t_out = &out_; g_api->destroy_instance(inst_); t_out = nullptr; }
        flushOut();   // the core's final note-offs
        alsaClose();
    }

    void prepare(double, int) override {}
    void reset() override {}

    // ---- the audio callback: commands in, played notes, the block's clock from the song position, notes out. No audio. ----
    void process(mpc::ProcessContext& ctx) override
    {
        if (!inst_ || busy_.exchange(true)) return;   // the core is held for a moment: the grid sends its pulses with the next block
        drain();
        for (int i = 0; i < ctx.numMidiEvents; i++)
        {
            const mpc::MidiEvent& e = ctx.midi[i];
            if (e.size >= 3 && (e.data[0] & 0xF0) == 0x90 && e.data[2] > 0) playedNote(e.data[0], e.data[1]);
        }
        const mpc::Transport& t = ctx.transport;
        const bool playing = t.valid && t.playing;
        if (t.valid && t.tempoValid && t.bpm >= 20 && std::fabs((float)t.bpm - sentBpm_) > 0.01f)
        {
            char b[16];
            std::snprintf(b, sizeof b, "%.2f", t.bpm);
            sentBpm_ = (float)t.bpm;
            coreSet("host_bpm", b);   // LFO sync tempo
        }
        Clock clk{this};
        const bool cyc = t.valid && t.cycling && t.cycleEndPpq > t.cycleStartPpq;
        grid_.block(clk, playing, t.valid ? t.ppqPos : 0.0, t.valid ? t.bpm : 120.0, ctx.sampleRate, ctx.numFrames,
                    cyc ? t.cycleStartPpq : 0.0, cyc ? t.cycleEndPpq : 0.0);
        publishLines();
        playing_.store(grid_.was_playing);
        lastBlockMs_.store(nowMs(), std::memory_order_relaxed);
        busy_.store(false);
    }

    // ---- parameters: any thread. Nothing here touches the core. ----
    void parameterChanged(int i, float plain) override
    {
        if (i < 0 || i >= kNumParams) return;
        const PSpec& s = g_pspecs[i];
        if (s.flags & (kReserved | kRun)) { if (!loading_.load()) store(i, told_[i].load()); return; }   // display only
        if (s.flags & kUiOpen) { told_[i].store(plain); return; }   // UI only: never reaches the engine; a load restores it as saved
        if (s.flags & kMomentary)
        {
            if (loading_.load()) return;   // an action, not state
            if (plain < 0.5f) { held_[i].store(false); return; }
            if (held_[i].exchange(true)) return;   // our own release echo must not re-fire it
            push(kCmdFire, s.key, "");
            release_[i].store(true);
            return;
        }
        if (loading_.load()) { told_[i].store(plain); sendValue(i, plain, true); return; }
        float v = plain;
        bool nudge = false;
        if ((s.flags & (kChoice | kInt)) && s.max > s.min)
        {
            const float span = s.max - s.min, pos = getNormalised(i) * span, cur = told_[i].load() - s.min;
            nudge = std::fabs(pos - std::round(pos)) > 0.001f;
            float steps = maze::settle(pos, cur, lastPos_[i]);
            lastPos_[i] = pos;
            v = s.min + std::fmin(std::fmax(steps, 0.0f), span);
            if (v != plain) { store(i, v); notify_[i].store(true); }   // the worker tells the host where it settled
        }
        told_[i].store(v);
        if (i == 0) { p0Val_.store(v); p0At_.store(nowMs()); return; }   // the host nudges parameter 0 on every prepare: debounced
        sendValue(i, v, false);
        if (!nudge && uiOpen_ >= 0 && openListFor(i)) closeList_.store(true);   // a list pick closes it; a Q-Link nudge doesn't
    }

    bool formatValue(int index, float, std::string& out) override
    {
        if (index < 0 || index >= kNumParams || !(g_pspecs[index].flags & kMomentary)) return false;
        out = "";
        return true;
    }

    // ---- state ----
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const std::string s = stateText();
        out.assign(s.begin(), s.end());
    }
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        maze::forEachPair(reinterpret_cast<const char*>(data), size, ';', [this](const std::string& k, const std::string& v) {
            if (k == "pattern") push(kCmdPattern, "pattern", v.c_str());   // last: the bits and CV of both lines
        });
    }
    // A 1.x project: "key=value;..." in the core's own units, then the pattern. Applied the way the 1.x setChunk did.
    bool loadLegacy(const char* data, size_t size)
    {
        if (!data || !size || !std::memchr(data, '=', strnlen(data, size))) return false;
        std::string pattern;
        beginLoad();
        maze::forEachPair(data, size, ';', [&](const std::string& k, const std::string& v) {
            if (k == "pattern") { pattern = v; return; }
            int i = maze::indexOf(k.c_str());
            if (i < 0 || (g_pspecs[i].flags & (kMomentary | kRun | kUiOpen | kReserved | kStep))) return;
            const PSpec& s = g_pspecs[i];
            float x = std::fmin(std::fmax((float)std::atof(v.c_str()), s.min), s.max);
            store(i, x);
            told_[i].store(x);
            sendValue(i, x, true);
        });
        if (!pattern.empty()) push(kCmdPattern, "pattern", pattern.c_str());
        finishLoad();
        return true;
    }
    void beginLoad() { loading_.store(true); }
    void finishLoad()
    {
        loading_.store(false);
        refreshAll_.store(true);   // the worker re-reads the steps and tells the host
    }

    std::string stateText()   // any thread: values from the SDK, the pattern from the core
    {
        std::string s;
        for (int i = 0; i < kNumParams; i++)
        {
            if (g_pspecs[i].flags & (kMomentary | kRun | kUiOpen | kReserved | kStep)) continue;
            char b[48];
            std::snprintf(b, sizeof b, "%s=%ld;", g_pspecs[i].key, std::lround(told_[i].load()));
            s += b;
        }
        char pat[256];
        int n = 0;
        withCore([&] { n = g_api->get_param(inst_, "pattern", pat, sizeof pat); });
        if (n > 0) { s += "pattern="; s += pat; s += ';'; }
        return s;
    }

#ifdef MAZE_TEST
    int dbgStep(int line, int step) { return sbits_[line][step].load(); }
    int dbgPlay(int line) { return splay_[line].load(); }
    int dbgSemis()
    {
        char buf[1024]; int n = 0;
        withCore([&] { n = g_api->get_param(inst_, "state", buf, sizeof buf); });
        const char* k = n > 0 ? std::strstr(buf, "\"pad_semis\":\"") : nullptr;
        return k ? std::atoi(k + 13) : -999;
    }
    void dbgSync() { withCore([] {}); workerTick(); }
#endif

private:
    struct Clock   // TransportGrid's sink: the clock bytes the 1.x host shim fed the core from real MIDI clock
    {
        SequencerPlugin* p;
        void start() { p->coreMidi(0xFA); }
        void stop() { p->coreMidi(0xFC); }
        void songpos(long m) { char b[24]; std::snprintf(b, sizeof b, "%ld", m); p->coreSet("song_pulse", b); }
        void pulse(long) { p->coreMidi(0xF8); }
    };

    void store(int i, float plain) { values_[(size_t)i]->store(param(i).toNormalised(plain), std::memory_order_relaxed); }
    bool openListFor(int i) const   // is ui_open showing parameter i's list?
    {
        const int k = (int)std::lround(told_[uiOpen_].load());
        if (k <= 0) return false;
        int n = 0;
        for (int j = 0; j < kNumParams; j++)
            if (g_pspecs[j].popupOf >= 0 && ++n == k) return g_pspecs[j].popupOf == i;
        return false;
    }

    // a parameter value (plain) to the core, as the 1.x wrapper sent it
    void sendValue(int i, float v, bool force)
    {
        const PSpec& s = g_pspecs[i];
        int line, step;
        if (s.flags & (kMomentary | kRun | kUiOpen | kReserved)) return;
        if (isStep(i, &line, &step))
        {
            const int on = (int)std::lround(v) ? 1 : 0;
            sbits_[line][step].store(on);   // shown at once (as 1.x did); the owner applies it with its next block
            push(kCmdStep, "", "", line, step * 2 + on);
            return;
        }
        if (!force && sent_[i] == v) return;   // a drag that re-sends a value costs the audio thread nothing
        sent_[i] = v;
        int x = (int)std::lround(v);
        if (!std::strcmp(s.key, "s1_channel") || !std::strcmp(s.key, "s2_channel")) x -= 1;   // 1..16 -> 0..15
        char b[16];
        std::snprintf(b, sizeof b, "%d", x);
        push(kCmdSet, s.key, b);
    }

    // ---- the core: owner only (the audio thread, or whoever holds busy_) ----
    void coreMidi(uint8_t byte) { t_out = &out_; g_api->on_midi(inst_, &byte, 1, 2); t_out = nullptr; flushOut(); }
    void coreSet(const char* k, const char* v) { t_out = &out_; g_api->set_param(inst_, k, v); t_out = nullptr; flushOut(); }
    int coreBits(int line, int bits[8])   // "<len>|1,0,..|<play>": the bits; returns the play-head (-1 before the first step)
    {
        char buf[96], key[16];
        std::snprintf(key, sizeof key, "s%d_state", line + 1);
        int n = g_api->get_param(inst_, key, buf, sizeof buf);
        for (int i = 0; i < 8; i++) bits[i] = 0;
        if (n <= 0) return -1;
        const char* p = std::strchr(buf, '|');
        for (int i = 0; p && i < 8; i++) { p++; bits[i] = *p == '1'; p = std::strchr(p, ','); if (!p) break; }
        const char* q = std::strrchr(buf, '|');
        return q && q != std::strchr(buf, '|') ? std::atoi(q + 1) : -1;
    }
    void publishLines()
    {
        for (int l = 0; l < 2; l++)
        {
            int b[8];
            int pl = coreBits(l, b);
            for (int i = 0; i < 8; i++) sbits_[l][i].store(b[i], std::memory_order_relaxed);
            splay_[l].store(pl, std::memory_order_relaxed);
        }
    }
    void applyCmd(const maze::Cmd& c)
    {
        switch (c.kind)
        {
            case kCmdSet: case kCmdPattern: coreSet(c.key, c.val); break;
            case kCmdFire: coreSet(c.key, "1"); break;
            case kCmdStep:   // a = line, b = step * 2 + wanted
            {
                int bits[8];
                coreBits(c.a, bits);
                if (bits[c.b >> 1] != (c.b & 1)) { char n[8]; std::snprintf(n, sizeof n, "%d", c.b >> 1); coreSet(c.a ? "s2_flip" : "s1_flip", n); }
                break;
            }
        }
    }
    void drain() { ring_.drain([this](const maze::Cmd& c) { applyCmd(c); }); }
    void playedNote(uint8_t status, uint8_t note)   // a played note transposes, unless it is our own note echoed back
    {
        if (wasJustSent(status, note)) return;
        int semis = note - 60; semis = semis < -24 ? -24 : semis > 24 ? 24 : semis;
        char b[16];
        std::snprintf(b, sizeof b, "%d", semis);
        g_api->set_param(inst_, "pad_semis", b);
    }

    void push(int kind, const char* key, const char* val, int a = 0, int b = 0)   // any thread but the owner's loop
    {
        for (int tries = 0; ring_.full() && tries < 200; tries++) { idleSync(); sched_yield(); }
        ring_.put(kind, key, val, a, b);
        idleSync();
    }
    void idleSync(bool force = false)   // no block for 60 ms: take the core for a moment so edits apply and the screen follows
    {
        if (!inst_ || (!force && nowMs() - lastBlockMs_.load() < 60)) return;
        if (busy_.exchange(true)) return;
        drain();
        publishLines();
        busy_.store(false);
    }
    template <class F> void withCore(F fn)   // a non-audio thread needing the core now: waits for the audio thread, never the reverse
    {
        if (!inst_) return;
        while (busy_.exchange(true)) sched_yield();
        drain();
        fn();
        publishLines();
        busy_.store(false);
    }

    // ---- the worker: tells the host what changed without it (never from inside the host's own call) ----
    void workerMain()
    {
        while (!stop_.load())
        {
            usleep(10000);
            workerTick();
        }
    }
    void workerTick()
    {
        idleSync();
        if (p0At_.load() && nowMs() - p0At_.load() >= 50)   // parameter 0 settled
        {
            p0At_.store(0);
            sendValue(0, p0Val_.load(), false);
        }
        const bool all = refreshAll_.exchange(false);
        for (int l = 0; l < 2; l++)
        {
            for (int s = 0; s < 8; s++)
            {
                const int i = stepIdx_[l][s];
                const float b = (float)sbits_[l][s].load(std::memory_order_relaxed);
                if (i == 0 && p0At_.load()) continue;   // a nudge in flight
                if (all || std::fabs(get(i) - b) > 0.5f) { store(i, b); told_[i].store(b); notifyHostParameterChanged(i); }
            }
            const int ri = runIdx_[l];
            const int pl = splay_[l].load(std::memory_order_relaxed);
            const float v = playing_.load() && pl >= 0 && pl < 8 ? (float)(pl + 1) : 0.0f;
            if (ri >= 0 && (all || told_[ri].load() != v)) { told_[ri].store(v); store(ri, v); notifyHostParameterChanged(ri); }
        }
        for (int i = 0; i < kNumParams; i++)
        {
            if (release_[i].exchange(false)) { held_[i].store(false); store(i, 0); notifyHostParameterChanged(i); }
            if (notify_[i].exchange(false)) notifyHostParameterChanged(i);
        }
        if (closeList_.exchange(false) && uiOpen_ >= 0) { told_[uiOpen_].store(0); store(uiOpen_, 0); notifyHostParameterChanged(uiOpen_); }
    }

    // ---- ALSA sequencer output (the owner writes; one client per instance, lowest free name) ----
    void alsaOpen()
    {
        if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0) { seq_ = nullptr; return; }
        snd_seq_set_client_pool_output(seq_, 2048);
        int n = g_instances.fetch_add(1);
        char name[48];
        if (n == 0) std::snprintf(name, sizeof name, "%s", MAZE_SEQ_NAME);
        else std::snprintf(name, sizeof name, "%s %d", MAZE_SEQ_NAME, n + 1);
        snd_seq_set_client_name(seq_, name);
        port_ = snd_seq_create_simple_port(seq_, "MIDI Out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                           SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        MLOG("[maze_seq] ALSA client '%s' port %d\n", name, port_);
    }
    void alsaClose()
    {
        if (!seq_) return;
        for (int ch = 0; ch < 16; ch++)   // nothing left hanging on the track we fed
        {
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_source(&ev, port_);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            ev.type = SND_SEQ_EVENT_CONTROLLER;
            ev.data.control.channel = ch;
            ev.data.control.param = 123;   // all notes off
            snd_seq_event_output_direct(seq_, &ev);
        }
        snd_seq_close(seq_);
        seq_ = nullptr;
    }
    bool wasJustSent(uint8_t status, uint8_t note)   // status + note only: the loopback quantises velocity
    {
        for (int i = 0; i < sentLen_; i++)
        {
            int idx = (sentPos_ - 1 - i + 16) % 16;
            if (sentRing_[idx][0] == status && sentRing_[idx][1] == note) { sentRing_[idx][0] = 0xFF; return true; }
        }
        return false;
    }
    void flushOut()   // send and empty out_: owner only
    {
        const int n = out_.n;
        out_.n = 0;
        for (int i = 0; i < n; i++)
        {
            const uint8_t* m = out_.m[i];
            const uint8_t type = m[0] & 0xF0, ch = m[0] & 0x0F;
#ifdef MAZE_TEST
            if (type == 0x90 && m[2] > 0) g_noteOns[ch]++;
#endif
            if (type == 0x90 || type == 0x80)
            {
                sentRing_[sentPos_][0] = m[0]; sentRing_[sentPos_][1] = m[1];
                sentPos_ = (sentPos_ + 1) % 16; if (sentLen_ < 16) sentLen_++;
            }
            if (!seq_ || port_ < 0) continue;
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_source(&ev, port_);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            if (type == 0x90 && m[2] > 0) snd_seq_ev_set_noteon(&ev, ch, m[1], m[2]);
            else if (type == 0x80 || type == 0x90) snd_seq_ev_set_noteoff(&ev, ch, m[1], 0);
            else continue;
            snd_seq_event_output(seq_, &ev);   // buffered, non-blocking: drops instead of hanging
        }
        if (seq_ && n) snd_seq_drain_output(seq_);
    }

    void* inst_ = nullptr;
    maze::Ring ring_;
    Outbox out_;
    TransportGrid grid_;
    float sentBpm_ = 0.0f;
    std::atomic<bool> busy_{false}, playing_{false}, loading_{false}, stop_{false}, refreshAll_{false}, closeList_{false};
    std::atomic<long long> lastBlockMs_{0}, p0At_{0};
    std::atomic<float> p0Val_{0.0f};
    std::atomic<int> sbits_[2][8] = {}, splay_[2] = {};
    int stepIdx_[2][8] = {}, runIdx_[2] = {-1, -1}, uiOpen_ = -1;
    std::atomic<bool> held_[kNumParams], release_[kNumParams], notify_[kNumParams] = {};
    std::atomic<float> told_[kNumParams];   // the value the host last knew (plain)
    float lastPos_[kNumParams];             // settle(): where the host last pointed (UI thread)
    float sent_[kNumParams] = {};           // the last value sent to the core (UI thread)
    std::thread worker_;
    snd_seq_t* seq_ = nullptr;
    int port_ = -1;
    uint8_t sentRing_[16][2] = {{0}};   // our last note messages: the input monitor loops them back
    int sentLen_ = 0, sentPos_ = 0;
};

// The same entry as MPC_PLUGIN_ENTRY, with effSetChunk wrapped: an "MPCP" chunk loads with momentaries and display values
// kept out of the core; any other chunk is a 1.x project's text.
VstIntPtr (*g_origDispatcher)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float) = nullptr;
VstIntPtr seqDispatcher(AEffect* e, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
    if (opcode == effSetChunk && e->object && ptr && value > 0)
    {
        auto* p = static_cast<SequencerPlugin*>(e->object);
        if (value >= 4 && std::memcmp(ptr, "MPCP", 4) == 0)
        {
            p->beginLoad();
            const VstIntPtr r = g_origDispatcher(e, opcode, index, value, ptr, opt);
            p->finishLoad();
            return r;
        }
        return p->loadLegacy(static_cast<const char*>(ptr), (size_t)value) ? 1 : 0;
    }
    return g_origDispatcher(e, opcode, index, value, ptr, opt);
}

} // namespace

#ifdef MAZE_TEST   // offline test hooks (tests/seq_host_test.cpp)
extern "C" MPC_EXPORT int maze_seq_note_ons(int ch) { return g_noteOns[ch & 15].load(); }
extern "C" MPC_EXPORT int maze_seq_step(AEffect* e, int l, int s) { return static_cast<SequencerPlugin*>(e->object)->dbgStep(l, s); }
extern "C" MPC_EXPORT int maze_seq_play(AEffect* e, int l) { return static_cast<SequencerPlugin*>(e->object)->dbgPlay(l); }
extern "C" MPC_EXPORT int maze_seq_semis(AEffect* e) { return static_cast<SequencerPlugin*>(e->object)->dbgSemis(); }
extern "C" MPC_EXPORT void maze_seq_sync(AEffect* e) { static_cast<SequencerPlugin*>(e->object)->dbgSync(); }
#endif

extern "C" MPC_EXPORT AEffect* VSTPluginMain(audioMasterCallback host)
{
    if (host != nullptr && host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0) return nullptr;
    AEffect* e = mpc::detail::createEffect(new SequencerPlugin(), host);
    if (e && !g_origDispatcher) g_origDispatcher = e->dispatcher;
    if (e) e->dispatcher = seqDispatcher;
    return e;
}
