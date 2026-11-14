// fake_host.h - a minimal VST2 host for the offline tests: transport, MIDI in, parameters, chunks, and a record of what the
// plugin told the host (audioMasterAutomate per parameter, UpdateDisplay).
#pragma once
#include "mpc_vst2_abi.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback host);

namespace fh {

inline int fails = 0;
#define CHECK(c, ...) do { bool ok_ = (c); std::printf("%s ", ok_ ? "ok  " : "FAIL"); std::printf(__VA_ARGS__); std::printf("\n"); fh::fails += !ok_; } while (0)

inline VstTimeInfo ti;
inline std::atomic<int> automated[512];
inline std::atomic<float> automatedValue[512];
inline std::atomic<int> updateDisplay{0};

inline VstIntPtr host(AEffect*, VstInt32 op, VstInt32 idx, VstIntPtr, void*, float opt)
{
    if (op == audioMasterVersion) return 2400;
    if (op == audioMasterAutomate && idx >= 0 && idx < 512) { automatedValue[idx].store(opt); automated[idx]++; }
    if (op == audioMasterUpdateDisplay) updateDisplay++;
    if (op == audioMasterGetTime) return (VstIntPtr)&ti;
    return 0;
}

struct Plug
{
    AEffect* e = nullptr;
    double sample = 0;
    std::vector<float> l = std::vector<float>(4096), r = std::vector<float>(4096);

    Plug()
    {
        e = VSTPluginMain(host);
        e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
        e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, 44100.0f);
        e->dispatcher(e, effSetBlockSize, 0, 512, nullptr, 0);
        e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
    }
    ~Plug() { if (e) { e->dispatcher(e, effMainsChanged, 0, 0, nullptr, 0); e->dispatcher(e, effClose, 0, 0, nullptr, 0); } }

    int n() const { return e->numParams; }
    void set(int i, float norm) { e->setParameter(e, i, norm); }
    float get(int i) const { return e->getParameter(e, i); }
    std::string name(int i) const { char b[64] = {0}; e->dispatcher(e, effGetParamName, i, 0, b, 0); return b; }
    std::string display(int i) const { char b[64] = {0}; e->dispatcher(e, effGetParamDisplay, i, 0, b, 0); return b; }
    std::string effectName() const { char b[64] = {0}; e->dispatcher(e, effGetEffectName, 0, 0, b, 0); return b; }

    // one block; playing at `bpm` from song position ppq (advanced by the block)
    void block(int frames, bool playing = false, double bpm = 120.0)
    {
        ti.sampleRate = 44100; ti.tempo = bpm; ti.timeSigNumerator = ti.timeSigDenominator = 4;
        ti.samplePos = sample;
        ti.flags = kVstTempoValid | kVstPpqPosValid | (playing ? kVstTransportPlaying : 0);
        float* outs[2] = {l.data(), r.data()};
        e->processReplacing(e, outs, outs, frames);
        if (playing) ti.ppqPos += frames / 44100.0 * bpm / 60.0;
        sample += frames;
    }
    void midi(std::vector<std::vector<uint8_t>> msgs)
    {
        static VstMidiEvent ev[32];
        struct { VstInt32 numEvents; VstIntPtr reserved; VstEvent* events[32]; } list = {};
        for (size_t k = 0; k < msgs.size() && k < 32; k++)
        {
            std::memset(&ev[k], 0, sizeof ev[k]);
            ev[k].type = kVstMidiType; ev[k].byteSize = sizeof(VstMidiEvent);
            for (size_t j = 0; j < msgs[k].size() && j < 3; j++) ev[k].midiData[j] = (char)msgs[k][j];
            list.events[list.numEvents++] = (VstEvent*)&ev[k];
        }
        e->dispatcher(e, effProcessEvents, 0, 0, &list, 0);
    }
    std::vector<uint8_t> chunk() const
    {
        void* p = nullptr;
        VstIntPtr n = e->dispatcher(e, effGetChunk, 0, 0, &p, 0);
        return n > 0 && p ? std::vector<uint8_t>((uint8_t*)p, (uint8_t*)p + n) : std::vector<uint8_t>();
    }
    bool load(const std::vector<uint8_t>& c) { return e->dispatcher(e, effSetChunk, 0, (VstIntPtr)c.size(), (void*)c.data(), 0) == 1; }
};

inline std::vector<uint8_t> readFile(const char* path)
{
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    int c;
    while ((c = std::fgetc(f)) != EOF) v.push_back((uint8_t)c);
    std::fclose(f);
    return v;
}

inline std::vector<std::string> readLines(const char* path)
{
    std::vector<std::string> out;
    std::string cur;
    for (uint8_t c : readFile(path)) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur += (char)c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace fh
