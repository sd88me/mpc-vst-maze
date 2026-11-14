// Offline host test for Maze Sequencer 2 (ASan/UBSan, tests/run.sh; ALSA has no device here, so notes are counted by the
// MAZE_TEST hook). Prints OK or FAILED.
// Identity, step toggles, notes on the right MIDI channels from a song-position clock, Advance, run lights, option settle,
// the list flag, transpose from a played note (and not from our own echo), parameter 0's nudge, a busy screen thread, the
// SDK state round trip with the pattern, and a chunk saved by the released Maze Sequencer 1.0.8 (tests/data).
#include "fake_host.h"
#include "params_gen.h"

#include <thread>
#include <unistd.h>

extern "C" int maze_seq_note_ons(int ch);
extern "C" int maze_seq_step(AEffect* e, int l, int s);
extern "C" int maze_seq_play(AEffect* e, int l);
extern "C" int maze_seq_semis(AEffect* e);
extern "C" void maze_seq_sync(AEffect* e);

using fh::Plug;

static int P(const char* k) { for (int i = 0; i < kNumParams; i++) if (!std::strcmp(g_pspecs[i].key, k)) return i; std::printf("no param %s\n", k); return 0; }
static float norm(const char* k, float plain) { const PSpec& s = g_pspecs[P(k)]; return s.max > s.min ? (plain - s.min) / (s.max - s.min) : 0; }
static float plain(Plug& p, const char* k) { const PSpec& s = g_pspecs[P(k)]; return std::round(s.min + p.get(P(k)) * (s.max - s.min)); }
static void settle(Plug& p, int blocks = 3) { for (int i = 0; i < blocks; i++) p.block(128); maze_seq_sync(p.e); }
static void play(Plug& p, double seconds) { for (int b = 0; b < (int)(seconds * 44100 / 128); b++) p.block(128, true); }
static int bits(Plug& p, int line) { int v = 0; for (int s = 0; s < 8; s++) v |= maze_seq_step(p.e, line, s) << s; return v; }

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : ".";
    std::string lockPath = std::string(dir) + "/plugins/sequencer/params.lock", legacyPath = std::string(dir) + "/tests/data/legacy-sequencer-1.0.8.chunk";

    {
        Plug p;
        CHECK(p.effectName() == "Maze Sequencer", "name '%s'", p.effectName().c_str());
        CHECK(p.e->uniqueID == (VstInt32)0x4d7a5371, "uid MzSq (%08x)", (unsigned)p.e->uniqueID);
        auto lock = fh::readLines(lockPath.c_str());
        CHECK((int)lock.size() == 69 && p.n() >= 69, "%d params, lock %d", p.n(), (int)lock.size());
        bool order = true;
        for (size_t i = 0; i < lock.size() && i < (size_t)kNumParams; i++)
        {
            const bool popup = lock[i].find("__open") != std::string::npos;
            if (popup ? !(g_pspecs[i].flags & (kUiOpen | kReserved)) : lock[i] != g_pspecs[i].key) { order = false; std::printf("  index %zu: %s vs %s\n", i, lock[i].c_str(), g_pspecs[i].key); }
            if (p.name((int)i) != g_pspecs[i].name) order = false;
        }
        CHECK(order, "parameter order and names match params.lock");
        CHECK(plain(p, "s1_length") == 8 && plain(p, "s1_channel") == 1 && plain(p, "g_reset") == 4, "defaults are the 1.x table's");
        CHECK(plain(p, "s1_corrupt") == 61 && plain(p, "s2_corrupt") == 50 && plain(p, "trig_mix") == -40 && plain(p, "scale") == 2 && plain(p, "key") == 7,
              "2.x starting values: Corrupt A 1 o'clock (61), B 12 o'clock (50), Trig Mix -40, Scale MINOR, Key G");
    }

    // ---- steps, notes, channels, run lights, Advance ----
    {
        Plug p;
        settle(p);
        p.set(P("s1_corrupt"), 0); p.set(P("s2_corrupt"), 0);   // the 2.x starting Corrupt would flip gates under this count
        p.set(P("s1_step0"), 1); p.set(P("s1_step4"), 1); p.set(P("s2_step2"), 1); p.set(P("s2_channel"), norm("s2_channel", 3));
        usleep(80000); settle(p);   // s1_step0 is parameter 0: applied once it has settled
        CHECK(bits(p, 0) == 0x11 && bits(p, 1) == 0x04, "step toggles reach the core (A %02x B %02x)", bits(p, 0), bits(p, 1));
        int a0 = maze_seq_note_ons(0), b2 = maze_seq_note_ons(2);
        fh::ti.ppqPos = 0;
        play(p, 4.0);   // 8 beats at 120: 32 sixteenths, 4 cycles of 8 steps, 2 hits per cycle on A, 1 on B
        int a = maze_seq_note_ons(0) - a0, b = maze_seq_note_ons(2) - b2;
        CHECK(a >= 6 && a <= 10 && b >= 3 && b <= 5, "notes on A's channel 1 (%d) and B's channel 3 (%d)", a, b);
        maze_seq_sync(p.e);
        CHECK(plain(p, "s1_run") >= 1 && plain(p, "s1_run") <= 8, "run light shows the play-head while playing (%.0f)", plain(p, "s1_run"));
        for (int k = 0; k < 4; k++) p.block(128, false);
        settle(p);
        CHECK(plain(p, "s1_run") == 0, "run light off when stopped");
        const int adv = P("s1_adv");
        int was = bits(p, 0), told = fh::automated[adv].load();
        p.set(adv, 1); settle(p);
        CHECK(bits(p, 0) != was && fh::automated[adv].load() > told && p.get(adv) == 0.0f, "Advance rotates line A and drops back (%02x -> %02x)", was, bits(p, 0));
        CHECK(p.get(P("s1_step0")) == (float)maze_seq_step(p.e, 0, 0) && p.get(P("s1_step1")) == (float)maze_seq_step(p.e, 0, 1), "the host's step values follow the core");
        p.set(adv, 1); p.set(adv, 1); settle(p);   // a held press (repeat) fires once
        p.set(adv, 0);
    }

    // ---- settle, lists, transpose, parameter 0 ----
    {
        Plug p;
        settle(p);
        const int sc = P("scale");   // MINOR (2 of 0..11), the 2.x starting scale
        p.set(sc, (2 + 0.2f) / 11); settle(p);
        CHECK(plain(p, "scale") == 3, "option nudge up steps one option (%.0f)", plain(p, "scale"));
        const int uo = P("ui_open");
        p.set(uo, 2.0f / 6); settle(p);   // the Key list
        CHECK(std::fabs(p.get(uo) - 2.0f / 6) < 1e-4f, "ui_open shows the Key list");
        p.set(P("scale"), 4.0f / 11); settle(p);
        CHECK(std::fabs(p.get(uo) - 2.0f / 6) < 1e-4f, "a pick in another list leaves it open");
        p.set(P("key"), 5.0f / 11); settle(p);
        CHECK(p.get(uo) == 0.0f && plain(p, "key") == 5, "a pick in the Key list closes it");
        p.set(uo, 5.0f / 6); settle(p);   // the A MIDI CH list: a numeric 1-16 list, picked like any other
        CHECK(std::fabs(p.get(uo) - 5.0f / 6) < 1e-4f, "ui_open shows the A MIDI CH list");
        p.set(P("s1_channel"), 9.0f / 15); settle(p);
        CHECK(p.get(uo) == 0.0f && plain(p, "s1_channel") == 10, "a pick in the MIDI CH list sets the channel (%.0f) and closes it", plain(p, "s1_channel"));
        p.set(uo, 6.0f / 6); settle(p); p.set(P("s2_channel"), 15.0f / 15); settle(p);
        CHECK(p.get(uo) == 0.0f && plain(p, "s2_channel") == 16, "the B MIDI CH list likewise (%.0f)", plain(p, "s2_channel"));
        p.midi({{0x90, 67, 100}}); settle(p);
        CHECK(maze_seq_semis(p.e) == 7, "a played G4 transposes +7 (%d)", maze_seq_semis(p.e));
        p.set(0, 1.0f); p.set(0, 0.0f); usleep(80000); settle(p);
        CHECK(maze_seq_step(p.e, 0, 0) == 0, "parameter 0 nudge leaves step A1 alone");
    }

    // ---- a busy screen while playing ----
    {
        Plug p;
        settle(p);
        std::atomic<bool> stop{false};
        std::thread ui([&] {
            unsigned k = 0; char d[64];
            while (!stop.load())
            {
                const int i = (int)(k++ % (unsigned)kNumParams);
                if (!(g_pspecs[i].flags & (kUiOpen | kReserved | kRun))) p.set(i, (float)((k * 37) % 100) / 99.0f);
                p.get(i);
                p.e->dispatcher(p.e, effGetParamDisplay, i, 0, d, 0);
                if (k % 500 == 0) p.chunk();
            }
        });
        fh::ti.ppqPos = 0;
        play(p, 3.0);
        stop.store(true);
        ui.join();
        CHECK(true, "busy screen thread while playing: no crash, no sanitizer report");
    }

    // ---- state round trip ----
    {
        Plug a;
        settle(a);
        a.set(P("s1_step3"), 1); a.set(P("s2_step7"), 1); a.set(P("s1_corrupt"), norm("s1_corrupt", 55)); a.set(P("transpose"), norm("transpose", -5));
        a.set(P("lfo2_div"), 6.0f / 7); a.set(P("s1_channel"), norm("s1_channel", 10));
        settle(a);
        fh::ti.ppqPos = 0;
        play(a, 2.0);
        settle(a);
        const int ba = bits(a, 0), bb = bits(a, 1);
        auto c = a.chunk();
        CHECK(c.size() > 8 && !std::memcmp(c.data(), "MPCP", 4), "chunk is an SDK chunk (%zu bytes)", c.size());
        Plug b;
        settle(b);
        CHECK(b.load(c), "chunk loads");
        settle(b);
        CHECK(bits(b, 0) == ba && bits(b, 1) == bb, "pattern round-trips (A %02x/%02x B %02x/%02x)", ba, bits(b, 0), bb, bits(b, 1));
        bool same = true;
        for (int i = 0; i < kNumParams; i++)
            if (!(g_pspecs[i].flags & (kUiOpen | kRun | kMomentary)) && std::fabs(a.get(i) - b.get(i)) > 1e-5f) { same = false; std::printf("  %s %.4f vs %.4f\n", g_pspecs[i].key, a.get(i), b.get(i)); }
        CHECK(same, "every parameter round-trips");
        CHECK(a.chunk() == b.chunk(), "re-saved chunk is identical");
    }

    // ---- a Maze Sequencer 1.0.8 project ----
    {
        auto legacy = fh::readFile(legacyPath.c_str());
        CHECK(!legacy.empty() && legacy.back() == 0, "legacy chunk present (%zu bytes)", legacy.size());
        Plug p;
        settle(p);
        CHECK(p.load(legacy), "1.0.8 chunk loads");
        settle(p);
        CHECK(plain(p, "s1_corrupt") == 60 && plain(p, "s1_length") == 6 && plain(p, "s2_channel") == 3 && plain(p, "scale") == 2 &&
              plain(p, "key") == 5 && plain(p, "transpose") == 7 && plain(p, "lfo1_rate") == 40, "values from the 1.0.8 chunk");
        CHECK(bits(p, 0) == 0x34 && bits(p, 1) == 0xB5, "pattern from the 1.0.8 chunk (A %02x B %02x)", bits(p, 0), bits(p, 1));
        CHECK(p.get(P("s1_step2")) == 1.0f && p.get(P("s1_step0")) == 0.0f, "the host's steps show the loaded pattern");
        auto c = p.chunk();
        Plug q; settle(q); q.load(c); settle(q);
        CHECK(bits(q, 0) == 0x34 && plain(q, "s1_corrupt") == 60, "saved again as 2.x, reloads");
        int b2 = maze_seq_note_ons(2);
        fh::ti.ppqPos = 0; play(q, 2.0);
        CHECK(maze_seq_note_ons(2) > b2, "the loaded project plays B on channel 3");
    }

    std::printf(fh::fails ? "FAILED (%d)\n" : "OK\n", fh::fails);
    return fh::fails ? 1 : 0;
}
