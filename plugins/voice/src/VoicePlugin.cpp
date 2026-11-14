// VoicePlugin.cpp - Maze Voice 2: the mpc::Plugin front end around the Maze voice engine (src/maze_voice.c, see engine.cpp).
//
// What carries over from the 1.x build (mpc-vst-plugins' generic wrapper, wrapper/vst2_wrap.c, as Maze Voice 1.0.5 used it):
//   * the engine renders 128-frame int16 stereo blocks, played out through a block buffer whatever the host's block size;
//     a block's MIDI goes in before it;
//   * MIDI CC 20-35 move the first page's controls (the 1.x CC map) and NRPN n sets parameter n (CC 99/98, 6/38); those
//     messages don't reach the engine;
//   * the host tempo drives the LFO sync ("lfo_bpm");
//   * an option list or whole-step value settles toward the way the Q-Link moves (maze::settle);
//   * Generate is a momentary that drops back; the values it rolls are read back so the screen follows.
// What is new: values live in the SDK, the engine is owned by the audio thread (maze_common.h), parameter 0's prepare nudge
// is debounced, the two 1.x popup flags are one "ui_open", and a project saves every parameter (1.0.5 saved the engine's
// state text, which left out both LFOs). A 1.0.5 project's text still loads.
#include "../../common/maze_common.h"

#ifndef MAZE_VOICE_NAME
#define MAZE_VOICE_NAME "Maze Voice"
#endif
#ifndef MAZE_VOICE_UID
#define MAZE_VOICE_UID 'M', 'z', 'V', 'c'
#endif
#define MAZE_FOURCC(...) MPC_FOURCC(__VA_ARGS__)
#ifndef MAZE_VOICE_VERSION
#define MAZE_VOICE_VERSION 2000
#endif

void* maze_voice_api();   // engine.cpp

namespace {

using maze::nowMs;

struct Api   // the engine's plugin_api_v2 (same layout as in src/maze_voice.c)
{
    uint32_t api_version;
    void* (*create_instance)(const char*, const char*);
    void (*destroy_instance)(void*);
    void (*on_midi)(void*, const uint8_t*, int, int);
    void (*set_param)(void*, const char*, const char*);
    int (*get_param)(void*, const char*, char*, int);
    int (*get_error)(void*, char*, int);
    void (*render_block)(void*, int16_t*, int);
};

constexpr int kBlock = 128;
enum { kCmdSet = 1, kCmdFire };

mpc::PluginInfo makeInfo()
{
    mpc::PluginInfo i;
    i.name = MAZE_VOICE_NAME;
    i.vendor = "sd88me";
    i.uniqueId = MAZE_FOURCC(MAZE_VOICE_UID);
    i.version = MAZE_VOICE_VERSION;
    i.isInstrument = true;
    i.numInputs = 0;
    i.numOutputs = 2;
    return i;
}

// a plain value -> the text the engine takes (1.x norm_to_str): an option index, a whole number, or "%g"
void valueText(const PSpec& s, float v, char* b, size_t n)
{
    if (s.flags & (kChoice | kInt | kMomentary)) std::snprintf(b, n, "%ld", std::lround(v));
    else std::snprintf(b, n, "%g", (double)v);
}

// the engine's text (a number or an option's label) -> a plain value (1.x str_to_norm)
float textValue(const PSpec& s, const char* t)
{
    if (s.flags & kChoice)
    {
        int idx = -1;
        if ((t[0] >= '0' && t[0] <= '9') || t[0] == '-') idx = std::atoi(t);
        else for (int i = 0; i < s.nchoices; i++) if (!strcasecmp(t, s.choices[i])) idx = i;
        return (float)(idx < 0 ? 0 : idx >= s.nchoices ? s.nchoices - 1 : idx);
    }
    return std::fmin(std::fmax((float)std::atof(t), s.min), s.max);
}

class VoicePlugin : public mpc::Plugin
{
public:
    VoicePlugin() : mpc::Plugin(makeInfo())
    {
        for (int i = 0; i < kNumParams; i++) addParam(maze::makeParam(g_pspecs[i]));
        for (int i = 0; i < kNumParams; i++)
        {
            held_[i].store(false); release_[i].store(false); notify_[i].store(false);
            told_[i].store(get(i)); snap_[i].store(get(i)); lastPos_[i] = -1.0f;
            if (g_pspecs[i].flags & kUiOpen) uiOpen_ = i;
        }
        api_ = static_cast<Api*>(maze_voice_api());
        if (api_ && api_->api_version == 2) inst_ = api_->create_instance("", nullptr);
        // the engine starts from its own defaults: give it the table's, which the host reads from the first moment
        for (int i = 0; i < kNumParams; i++) sendValue(i, get(i));
        ring_.drain([this](const maze::Cmd& c) { applyCmd(c); }, 100000);
        worker_ = std::thread(&VoicePlugin::workerMain, this);
    }

    ~VoicePlugin() override
    {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        while (busy_.exchange(true)) sched_yield();
        if (inst_) api_->destroy_instance(inst_);
    }

    void prepare(double, int) override {}
    void reset() override {}

    void process(mpc::ProcessContext& ctx) override
    {
        float* L = ctx.outputs[0];
        float* R = ctx.outputs[1];
        if (!inst_ || busy_.exchange(true))   // the engine is held for a moment: one block of silence
        {
            for (int i = 0; i < ctx.numFrames; i++) L[i] = R[i] = 0.0f;
            return;
        }
        drain();
        const mpc::Transport& t = ctx.transport;
        if (t.valid && t.tempoValid && t.bpm > 0 && std::fabs(t.bpm - bpm_) > 0.01)
        {
            char b[32];
            bpm_ = t.bpm;
            std::snprintf(b, sizeof b, "%.2f", bpm_);
            api_->set_param(inst_, "lfo_bpm", b);
        }
        for (int i = 0; i < ctx.numMidiEvents; i++)
        {
            const mpc::MidiEvent& e = ctx.midi[i];
            if (e.size < 1 || midiControl(e.data)) continue;
            if (e.size >= 2) api_->on_midi(inst_, e.data, 3, 2);
        }
        for (int i = 0; i < ctx.numFrames; i++)
        {
            if (pos_ >= kBlock) { api_->render_block(inst_, block_, kBlock); pos_ = 0; }
            L[i] = block_[pos_ * 2] * (1.0f / 32768.0f);
            R[i] = block_[pos_ * 2 + 1] * (1.0f / 32768.0f);
            pos_++;
        }
        lastBlockMs_.store(nowMs(), std::memory_order_relaxed);
        busy_.store(false);
    }

    void parameterChanged(int i, float plain) override
    {
        if (i < 0 || i >= kNumParams) return;
        const PSpec& s = g_pspecs[i];
        if (s.flags & kReserved) { if (!loading_.load()) store(i, 0); return; }
        if (s.flags & kUiOpen) { told_[i].store(plain); return; }   // UI only: never reaches the engine; a load restores it as saved
        if (s.flags & kMomentary)
        {
            if (loading_.load()) return;
            if (plain < 0.5f) { held_[i].store(false); return; }
            if (held_[i].exchange(true)) return;
            push(kCmdFire, s.key, "1");
            release_[i].store(true);
            return;
        }
        if (loading_.load()) { told_[i].store(plain); sendValue(i, plain); return; }
        float v = plain;
        bool nudge = false;
        if (s.flags & (kChoice | kInt))
        {
            const float span = s.max - s.min, pos = getNormalised(i) * span, cur = told_[i].load() - s.min;
            nudge = std::fabs(pos - std::round(pos)) > 0.001f;
            float steps = maze::settle(pos, cur, lastPos_[i]);
            lastPos_[i] = pos;
            v = s.min + std::fmin(std::fmax(steps, 0.0f), span);
            if (v != plain) { store(i, v); notify_[i].store(true); }
        }
        told_[i].store(v);
        if (i == 0) { p0Val_.store(v); p0At_.store(nowMs()); return; }   // the host nudges parameter 0 on every prepare
        sendValue(i, v);
        if (!nudge && uiOpen_ >= 0 && openListFor(i)) closeList_.store(true);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index < 0 || index >= kNumParams) return false;
        const PSpec& s = g_pspecs[index];
        if (s.flags & kMomentary) { out = ""; return true; }
        if (s.flags & kChoice) return false;
        char b[32];   // 1.x: whole numbers on wide ranges, one decimal on narrow ones
        std::snprintf(b, sizeof b, "%.*f", std::fabs(s.max - s.min) > 20 ? 0 : 1, (double)plain);
        if (!std::strcmp(b, "-0") || !std::strcmp(b, "-0.0")) std::memmove(b, b + 1, std::strlen(b));
        out = b;
        return true;
    }

    // ---- state: the SDK's chunk holds every parameter; a Maze Voice 1.x project holds the engine's text ----
    bool loadLegacy(const char* data, size_t size)
    {
        if (!data || !size || !std::memchr(data, '=', strnlen(data, size))) return false;
        beginLoad();
        maze::forEachPair(data, size, ',', [&](const std::string& k, const std::string& v) {
            const int i = maze::indexOf(k.c_str());
            if (i < 0 || (g_pspecs[i].flags & (kMomentary | kUiOpen | kReserved))) return;
            const float x = textValue(g_pspecs[i], v.c_str());
            store(i, x);
            told_[i].store(x);
            sendValue(i, x);
        });
        finishLoad();
        return true;
    }
    void beginLoad() { loading_.store(true); }
    void finishLoad()
    {
        loading_.store(false);
        p0At_.store(0);
    }

#ifdef MAZE_TEST
    float dbgEngine(const char* key)   // the engine's own value, read as the owner
    {
        const int i = maze::indexOf(key);
        char b[64]; int n = 0;
        withCore([&] { n = api_->get_param(inst_, key, b, sizeof b); });
        return n > 0 && i >= 0 ? textValue(g_pspecs[i], b) : -9999.0f;
    }
    void dbgSync() { withCore([] {}); workerTick(); }
#endif

private:
    void store(int i, float plain) { values_[(size_t)i]->store(param(i).toNormalised(plain), std::memory_order_relaxed); }
    bool openListFor(int i) const
    {
        const int k = (int)std::lround(told_[uiOpen_].load());
        if (k <= 0) return false;
        int n = 0;
        for (int j = 0; j < kNumParams; j++)
            if (g_pspecs[j].popupOf >= 0 && ++n == k) return g_pspecs[j].popupOf == i;
        return false;
    }
    void sendValue(int i, float v)
    {
        const PSpec& s = g_pspecs[i];
        if (s.flags & (kMomentary | kUiOpen | kReserved)) return;
        char b[32];
        valueText(s, v, b, sizeof b);
        push(kCmdSet, s.key, b);
    }

    // CC 20..35 (the 1.x map) and NRPN: set as a touch would, reported by the worker. Owner thread.
    bool midiControl(const uint8_t* m)
    {
        if ((m[0] & 0xF0) != 0xB0) return false;
        const int cc = m[1] & 127, v = m[2] & 127;
        if (cc >= 20 && cc <= 35 && kCcMap[cc - 20] >= 0) { ccSet(kCcMap[cc - 20], v / 127.0f); return true; }
#if PLUG_HAS_NRPN
        if (cc == 99) { nrpn_ = (v << 7) | (nrpn_ >= 0 ? nrpn_ & 127 : 0); return true; }
        if (cc == 98) { nrpn_ = (nrpn_ >= 0 ? nrpn_ & ~127 : 0) | v; return true; }
        if (cc == 101 || cc == 100) { nrpn_ = -1; return false; }   // an RPN: the engine's
        if (nrpn_ >= 0 && nrpn_ < kNumParams && cc == 6) { nrpnMsb_ = v; ccSet(nrpn_, v / 127.0f); return true; }
        if (nrpn_ >= 0 && nrpn_ < kNumParams && cc == 38) { ccSet(nrpn_, (nrpnMsb_ * 128 + v) / 16383.0f); return true; }
#endif
        return false;
    }
    void ccSet(int i, float n)
    {
        const PSpec& s = g_pspecs[i];
        if (s.flags & (kUiOpen | kReserved)) return;
        float v = s.min + (s.max - s.min) * std::fmin(std::fmax(n, 0.0f), 1.0f);
        if (s.flags & (kChoice | kInt)) v = std::round(v);
        if (s.flags & kMomentary)
        {
            if (v >= 0.5f) { api_->set_param(inst_, s.key, "1"); snapAll(); }
            return;
        }
        store(i, v);
        told_[i].store(v);
        char b[32];
        valueText(s, v, b, sizeof b);
        api_->set_param(inst_, s.key, b);
        cc_[i].store(true);
    }

    // ---- the engine: owner only ----
    void applyCmd(const maze::Cmd& c)
    {
        api_->set_param(inst_, c.key, c.val);
        if (c.kind == kCmdFire) snapAll();   // Generate rolled new values: read them back
    }
    void snapAll()
    {
        char b[64];
        for (int i = 0; i < kNumParams; i++)
        {
            const PSpec& s = g_pspecs[i];
            if ((s.flags & (kMomentary | kUiOpen | kReserved)) || api_->get_param(inst_, s.key, b, sizeof b) <= 0) continue;
            snap_[i].store(textValue(s, b));
        }
        snapRev_.fetch_add(1);
    }
    void drain() { ring_.drain([this](const maze::Cmd& c) { applyCmd(c); }); }
    void push(int kind, const char* key, const char* val)
    {
        for (int tries = 0; ring_.full() && tries < 200; tries++) { idleSync(); sched_yield(); }
        ring_.put(kind, key, val, 0, 0);
        idleSync();
    }
    void idleSync()
    {
        if (!inst_ || nowMs() - lastBlockMs_.load() < 60) return;
        if (busy_.exchange(true)) return;
        drain();
        busy_.store(false);
    }
    template <class F> void withCore(F fn)
    {
        if (!inst_) return;
        while (busy_.exchange(true)) sched_yield();
        drain();
        fn();
        busy_.store(false);
    }

    void workerMain()
    {
        while (!stop_.load()) { usleep(10000); workerTick(); }
    }
    void workerTick()
    {
        idleSync();
        if (p0At_.load() && nowMs() - p0At_.load() >= 50) { p0At_.store(0); sendValue(0, p0Val_.load()); }
        const unsigned rev = snapRev_.load();
        if (rev != seenRev_)
        {
            seenRev_ = rev;
            for (int i = 0; i < kNumParams; i++)
            {
                if (g_pspecs[i].flags & (kMomentary | kUiOpen | kReserved)) continue;
                const float v = snap_[i].load();
                if (std::fabs(v - told_[i].load()) > 1e-5f) { told_[i].store(v); store(i, v); notifyHostParameterChanged(i); }
            }
            requestDisplayUpdate();
        }
        const bool ccTurn = ++ccTick_ % 3 == 0;   // CC-driven changes: at most every ~30 ms per control
        for (int i = 0; i < kNumParams; i++)
        {
            if (release_[i].exchange(false)) { held_[i].store(false); store(i, 0); notifyHostParameterChanged(i); }
            if (notify_[i].exchange(false)) notifyHostParameterChanged(i);
            if (ccTurn && cc_[i].exchange(false)) notifyHostParameterChanged(i);
        }
        if (closeList_.exchange(false) && uiOpen_ >= 0) { told_[uiOpen_].store(0); store(uiOpen_, 0); notifyHostParameterChanged(uiOpen_); }
    }

    Api* api_ = nullptr;
    void* inst_ = nullptr;
    maze::Ring ring_;
    int16_t block_[kBlock * 2] = {};
    int pos_ = kBlock;
    double bpm_ = 0.0;
    int nrpn_ = -1, nrpnMsb_ = 0, uiOpen_ = -1;
    std::atomic<bool> busy_{false}, loading_{false}, stop_{false}, closeList_{false};
    std::atomic<long long> lastBlockMs_{0}, p0At_{0};
    std::atomic<float> p0Val_{0.0f};
    std::atomic<unsigned> snapRev_{0};
    unsigned seenRev_ = 0, ccTick_ = 0;
    std::atomic<bool> held_[kNumParams], release_[kNumParams], notify_[kNumParams], cc_[kNumParams] = {};
    std::atomic<float> told_[kNumParams], snap_[kNumParams];
    float lastPos_[kNumParams];
    std::thread worker_;
};

VstIntPtr (*g_origDispatcher)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float) = nullptr;
VstIntPtr voiceDispatcher(AEffect* e, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
    if (opcode == effSetChunk && e->object && ptr && value > 0)
    {
        auto* p = static_cast<VoicePlugin*>(e->object);
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

#ifdef MAZE_TEST
extern "C" MPC_EXPORT float maze_voice_engine(AEffect* e, const char* key) { return static_cast<VoicePlugin*>(e->object)->dbgEngine(key); }
extern "C" MPC_EXPORT void maze_voice_sync(AEffect* e) { static_cast<VoicePlugin*>(e->object)->dbgSync(); }
#endif

extern "C" MPC_EXPORT AEffect* VSTPluginMain(audioMasterCallback host)
{
    if (host != nullptr && host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0) return nullptr;
    AEffect* e = mpc::detail::createEffect(new VoicePlugin(), host);
    if (e && !g_origDispatcher) g_origDispatcher = e->dispatcher;
    if (e) e->dispatcher = voiceDispatcher;
    return e;
}
