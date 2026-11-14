// Offline host test for Maze Voice 2 (ASan/UBSan, tests/run.sh). Prints OK or FAILED.
// Identity (name, uid, parameter order = params.lock), engine sees parameter writes, option settle on a Q-Link nudge,
// Generate (momentary + readback), CC map and NRPN, the list flag, sound from a note, the SDK state round trip, and a
// chunk saved by the released Maze Voice 1.0.5 (tests/data, made from the tag's own engine).
#include "fake_host.h"
#include "params_gen.h"

#include <thread>
#include <unistd.h>

extern "C" float maze_voice_engine(AEffect* e, const char* key);
extern "C" void maze_voice_sync(AEffect* e);

using fh::Plug;

static int P(const char* k) { for (int i = 0; i < kNumParams; i++) if (!std::strcmp(g_pspecs[i].key, k)) return i; std::printf("no param %s\n", k); return 0; }
static float norm(const char* k, float plain) { const PSpec& s = g_pspecs[P(k)]; return (plain - s.min) / (s.max - s.min); }
static void settle(Plug& p, int blocks = 4) { for (int i = 0; i < blocks; i++) p.block(128); maze_voice_sync(p.e); }
static bool near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol * (1 + std::fabs(b)); }

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : ".";
    std::string lockPath = std::string(dir) + "/plugins/voice/params.lock", legacyPath = std::string(dir) + "/tests/data/legacy-voice-1.0.5.chunk";

    // ---- identity ----
    {
        Plug p;
        CHECK(p.effectName() == "Maze Voice", "name '%s'", p.effectName().c_str());
        CHECK(p.e->uniqueID == (VstInt32)0x4d7a5663, "uid MzVc (%08x)", (unsigned)p.e->uniqueID);
        auto lock = fh::readLines(lockPath.c_str());
        CHECK((int)lock.size() == 65 && p.n() >= (int)lock.size(), "%d params, lock %d", p.n(), (int)lock.size());
        bool order = true;
        for (size_t i = 0; i < lock.size() && i < (size_t)kNumParams; i++)
        {
            const bool popup = lock[i].find("__open") != std::string::npos;
            if (popup ? !(g_pspecs[i].flags & (kUiOpen | kReserved)) : lock[i] != g_pspecs[i].key) { order = false; std::printf("  index %zu: %s vs %s\n", i, lock[i].c_str(), g_pspecs[i].key); }
            if (p.name((int)i) != g_pspecs[i].name) { order = false; std::printf("  name %zu: %s\n", i, p.name((int)i).c_str()); }
        }
        CHECK(order, "parameter order and names match params.lock");
        CHECK(near(p.get(P("cutoff")), 1.0f) && near(p.get(P("env2_decay")), 0.7f), "defaults are the 1.x table's");
        settle(p);
        CHECK(near(maze_voice_engine(p.e, "cutoff"), 100.0f) && near(maze_voice_engine(p.e, "mod_freq"), 0.2f + 1299.8f * 0.04216033f, 1e-2f),
              "the engine holds the defaults (cutoff %.2f)", maze_voice_engine(p.e, "cutoff"));
    }

    // ---- parameter writes, settle, list flag ----
    {
        Plug p;
        settle(p);
        p.set(P("reso"), norm("reso", 42)); p.set(P("route"), 1.0f); p.set(P("lfo1_rate"), norm("lfo1_rate", 12.5f));
        settle(p);
        CHECK(near(maze_voice_engine(p.e, "reso"), 42) && near(maze_voice_engine(p.e, "route"), 2) && near(maze_voice_engine(p.e, "lfo1_rate"), 12.5f),
              "engine follows writes (reso %.1f route %.0f lfo1_rate %.2f)", maze_voice_engine(p.e, "reso"), maze_voice_engine(p.e, "route"), maze_voice_engine(p.e, "lfo1_rate"));
        CHECK(p.display(P("reso")) == "42" && p.display(P("vco_tune")) == "0" && p.display(P("route")) == "VCF>VCW",
              "display text (%s / %s / %s)", p.display(P("reso")).c_str(), p.display(P("vco_tune")).c_str(), p.display(P("route")).c_str());
        const int sh = P("lfo1_shape");   // Sine (2 of 0..4); a Q-Link nudge up lands between options
        p.set(sh, (2 + 0.1f) / 4); settle(p);
        CHECK(near(maze_voice_engine(p.e, "lfo1_shape"), 3) && near(p.get(sh), 0.75f), "option nudge up steps one option (%.0f, host %.2f)", maze_voice_engine(p.e, "lfo1_shape"), p.get(sh));
        p.set(sh, (3 - 0.1f) / 4); settle(p);
        CHECK(near(maze_voice_engine(p.e, "lfo1_shape"), 2), "option nudge down steps back (%.0f)", maze_voice_engine(p.e, "lfo1_shape"));
        const int uo = P("ui_open"), div = P("lfo1_div");
        p.set(uo, 0.5f); settle(p);   // 1 of 0..2: the LFO1 Div list
        CHECK(near(p.get(uo), 0.5f), "ui_open shows the LFO1 Div list");
        int before = fh::automated[uo].load();
        p.set(div, 5.0f / 7); settle(p);
        CHECK(near(p.get(uo), 0.0f) && fh::automated[uo].load() > before && near(maze_voice_engine(p.e, "lfo1_div"), 5), "a pick closes the list and sets the value");
        // parameter 0 (vco_tune): the host's prepare nudge (far end and straight back) never reaches the engine as a change
        p.set(0, 1.0f); p.set(0, 0.5f); usleep(80000); settle(p);
        CHECK(near(maze_voice_engine(p.e, "vco_tune"), 0), "parameter 0 nudge debounced");
        p.set(0, norm("vco_tune", 5)); usleep(80000); settle(p);
        CHECK(near(maze_voice_engine(p.e, "vco_tune"), 5), "parameter 0 change applied after it settles");
    }

    // ---- Generate, CC map, NRPN ----
    {
        Plug p;
        settle(p);
        const int go = P("rnd_go");
        int upd = fh::updateDisplay.load(), rel = fh::automated[go].load();
        p.set(go, 1.0f); settle(p); settle(p);
        bool same = true;
        for (const char* k : {"vco_tune", "mod_freq", "fold_drive", "cutoff", "reso", "route", "filter_mode", "level"})
            if (!near(maze_voice_engine(p.e, k), g_pspecs[P(k)].min + p.get(P(k)) * (g_pspecs[P(k)].max - g_pspecs[P(k)].min), 2e-3f)) { same = false; std::printf("  %s engine %.3f host %.3f\n", k, maze_voice_engine(p.e, k), p.get(P(k))); }
        CHECK(same, "Generate: the host reads back the rolled values");
        CHECK(fh::automated[go].load() > rel && p.get(go) == 0.0f && fh::updateDisplay.load() > upd, "Generate drops back and asks for a redraw");
        p.midi({{0xB0, 22, 127}}); settle(p); settle(p);   // CC 22 -> kCcMap[2] = vco_key
        CHECK(near(maze_voice_engine(p.e, g_pspecs[kCcMap[2]].key), g_pspecs[kCcMap[2]].max) && near(p.get(kCcMap[2]), 1.0f), "CC 22 moves %s", g_pspecs[kCcMap[2]].key);
        const int fd = P("fold_drive");
        p.midi({{0xB0, 99, 0}, {0xB0, 98, (uint8_t)fd}, {0xB0, 6, 64}, {0xB0, 38, 0}}); settle(p); settle(p);
        CHECK(near(maze_voice_engine(p.e, "fold_drive"), 100.0f * 8192 / 16383, 1e-2f), "NRPN %d sets Fold Drive (%.2f)", fd, maze_voice_engine(p.e, "fold_drive"));
    }

    // ---- sound ----
    {
        Plug p;
        settle(p);
        p.midi({{0x90, 48, 110}});
        float peak = 0;
        for (int b = 0; b < 40; b++) { p.block(256); for (int i = 0; i < 256; i++) peak = std::fmax(peak, std::fabs(p.l[i])); }
        CHECK(peak > 0.01f && peak < 1.0f, "a note sounds (peak %.3f)", peak);
        p.block(37); p.block(300); p.block(1);   // odd block sizes through the 128-frame buffer
    }

    // ---- state ----
    {
        Plug a;
        settle(a);
        a.set(P("cutoff"), norm("cutoff", 23)); a.set(P("lfo2_shape"), 1.0f); a.set(P("lfo1_cutoff"), norm("lfo1_cutoff", -40)); a.set(P("out_mode"), 1.0f);
        a.set(P("ui_open"), 1.0f);
        settle(a);
        auto c = a.chunk();
        CHECK(c.size() > 8 && !std::memcmp(c.data(), "MPCP", 4), "chunk is an SDK chunk (%zu bytes)", c.size());
        Plug b;
        settle(b);
        CHECK(b.load(c), "chunk loads");
        settle(b);
        bool same = true;
        for (int i = 0; i < kNumParams; i++)
            if (!near(a.get(i), b.get(i), 1e-5f)) { same = false; std::printf("  %s %.4f vs %.4f\n", g_pspecs[i].key, a.get(i), b.get(i)); }
        CHECK(same, "every parameter round-trips (LFOs included)");
        CHECK(b.get(P("ui_open")) == 1.0f, "a load restores the list flag as saved");
        CHECK(near(maze_voice_engine(b.e, "cutoff"), 23) && near(maze_voice_engine(b.e, "lfo2_shape"), 4) && near(maze_voice_engine(b.e, "lfo1_cutoff"), -40),
              "the engine got the loaded values");
        CHECK(a.chunk() == b.chunk(), "re-saved chunk is identical");
    }

    // ---- a Maze Voice 1.0.5 project ----
    {
        auto legacy = fh::readFile(legacyPath.c_str());
        CHECK(!legacy.empty() && legacy.back() == 0, "legacy chunk present (%zu bytes)", legacy.size());
        Plug p;
        settle(p);
        p.set(P("lfo1_rate"), norm("lfo1_rate", 77)); settle(p);   // 1.0.5 never saved the LFOs: a load leaves them as they are
        CHECK(p.load(legacy), "1.0.5 chunk loads");
        settle(p);
        CHECK(near(maze_voice_engine(p.e, "vco_tune"), 7) && near(maze_voice_engine(p.e, "cutoff"), 31) && near(maze_voice_engine(p.e, "reso"), 66) &&
              near(maze_voice_engine(p.e, "route"), 2) && near(maze_voice_engine(p.e, "out_mode"), 1) && near(maze_voice_engine(p.e, "level"), 77),
              "engine holds the 1.0.5 values");
        CHECK(near(p.get(P("vco_tune")), norm("vco_tune", 7)) && near(p.get(P("route")), 1.0f) && near(p.get(P("fold_drive")), 0.42f),
              "host sees the 1.0.5 values");
        CHECK(near(maze_voice_engine(p.e, "lfo1_rate"), 77), "LFO settings untouched by a 1.0.5 load");
        auto c = p.chunk();
        Plug q; settle(q); q.load(c); settle(q);
        CHECK(near(maze_voice_engine(q.e, "cutoff"), 31) && near(maze_voice_engine(q.e, "lfo1_rate"), 77), "saved again as 2.x, reloads");
        CHECK(!p.load(std::vector<uint8_t>{'x', 'y', 0}), "garbage chunk refused");
    }

    std::printf(fh::fails ? "FAILED (%d)\n" : "OK\n", fh::fails);
    return fh::fails ? 1 : 0;
}
