// maze_common.h - what Maze Voice 2 and Maze Sequencer 2 share around their engines.
//
// Threads (the house rule since Maze Sequencer 1.0.8): the audio thread owns the engine. The host/UI threads and the
// worker never call into it; they put commands on a lock-free ring (push) that the owner applies at the start of its
// next block (drain). When no block has run for 60 ms (transport stopped and the host not processing, plugin bypassed)
// the worker takes ownership for a moment (idleSync) so edits still apply; a block that finds the engine taken is
// skipped. Nothing here locks on the audio thread.
#pragma once

#include "mpc_plugin.h"
#include "params_gen.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sched.h>
#include <unistd.h>

namespace maze {

inline long long nowMs()
{
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The 1.x parameter table -> SDK parameters. Values are plain: an option index, a whole number, or the engine's own units.
inline mpc::Param makeParam(const PSpec& s)
{
    mpc::Param p;
    if (s.flags & kChoice)
        p = mpc::choiceParam(s.key, s.name, std::vector<std::string>(s.choices, s.choices + s.nchoices), (int)std::lround(s.def));
    else if (s.flags & (kInt | kMomentary))
        p = mpc::intParam(s.key, s.name, (int)s.min, (int)s.max, (int)std::lround(s.def), s.unit);
    else
        p = mpc::floatParam(s.key, s.name, s.min, s.max, s.def, s.unit, std::fabs(s.max - s.min) > 20.0f ? 0 : 1);
    if (s.flags & (kHidden | kRun | kReserved)) p.automatable = false;
    return p;
}

inline int indexOf(const char* key)
{
    for (int i = 0; i < kNumParams; i++) if (!std::strcmp(g_pspecs[i].key, key)) return i;
    return -1;
}

// Where a whole-step parameter (an option list, a whole number) lands, in steps from its minimum (1.x vst2_wrap.c settle()):
// a value on a step is taken as it is (a tap, automation, a preset); one between steps is a Q-Link / data wheel / drag move,
// rounded toward the way it is moving, so a turn smaller than one step still moves one step and a sweep moves steadily.
inline float settle(float pos, float cur, float last)
{
    if (std::fabs(pos - std::round(pos)) <= 0.001f) return std::round(pos);
    float dir = (last >= 0 && std::fabs(pos - last) < 0.5f) ? pos - last : pos - cur;
    if (dir == 0) return std::round(cur);
    return dir > 0 ? std::ceil(pos - 0.001f) : std::floor(pos + 0.001f);
}

struct Cmd { int kind = 0, a = 0, b = 0; char key[24] = {0}; char val[272] = {0}; };

// Commands from any thread to the engine's owner. Producers take turns on a flag; only the owner consumes.
struct Ring
{
    static const unsigned kSize = 512;
    Cmd cmd[kSize];
    std::atomic<unsigned> head{0}, tail{0};
    std::atomic_flag prod = ATOMIC_FLAG_INIT;

    bool full() const { return head.load() - tail.load() >= kSize; }
    bool put(int kind, const char* key, const char* val, int a, int b)
    {
        while (prod.test_and_set(std::memory_order_acquire)) sched_yield();
        unsigned h = head.load(std::memory_order_relaxed);
        bool ok = h - tail.load(std::memory_order_acquire) < kSize;
        if (ok)
        {
            Cmd& c = cmd[h % kSize];
            c.kind = kind; c.a = a; c.b = b;
            std::snprintf(c.key, sizeof c.key, "%s", key ? key : "");
            std::snprintf(c.val, sizeof c.val, "%s", val ? val : "");
            head.store(h + 1, std::memory_order_release);
        }
        prod.clear(std::memory_order_release);
        return ok;
    }
    template <class F> void drain(F apply, int budget = 128)   // owner only; a burst never holds one block for long
    {
        unsigned t = tail.load(std::memory_order_relaxed);
        while (budget-- > 0 && t != head.load(std::memory_order_acquire))
        {
            apply(cmd[t % kSize]);
            tail.store(++t, std::memory_order_release);
        }
    }
};

// "k=v<sep>k=v..." -> calls fn(key, value) for each pair.
template <class F> void forEachPair(const char* data, size_t size, char sep, F fn)
{
    std::string s(data, strnlen(data, size));
    size_t pos = 0;
    while (pos < s.size())
    {
        size_t end = s.find(sep, pos);
        if (end == std::string::npos) end = s.size();
        std::string tok = s.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        fn(tok.substr(0, eq), tok.substr(eq + 1));
    }
}

inline FILE*& logFile() { static FILE* f = nullptr; return f; }   // a pointer: no destructor at dlclose
#define MLOG(...) do { FILE* f_ = maze::logFile(); if (f_) { std::fprintf(f_, __VA_ARGS__); std::fflush(f_); } } while (0)

} // namespace maze
