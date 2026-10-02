# Maze Combo (experiment)

Maze Sequencer and Maze Voice in one MPC plugin. The sequencer pages come first (SEQUENCERS, SEQ LFO), then the voice
pages (VOICE, WAVEFOLDER / FILTER, MOD / RANDOM).

Two controls on the SEQUENCERS page set the mode:

| SEQ ON | SEQ OUTPUT | What you get |
|---|---|---|
| OFF | (any) | **Voice only.** Notes play the voice like a normal synth. The sequencer is stopped. |
| ON | VOICE | **Seq + voice.** The sequencer, clocked by the transport, plays the voice. Nothing goes out on MIDI. |
| ON | VOICE+MIDI | **Seq + voice + MIDI out.** As above, and the same notes go out the MIDI port **Maze Combo** (A and B on their own MIDI CH) for other tracks. |
| ON | MIDI ONLY | **Seq only.** Notes go to the MIDI port only; the voice stays silent. |

With SEQ ON, notes you play do not sound. They transpose the sequence: middle C (60) is no shift, G (67) is +7 semitones,
and the last note played stays in effect. The sequencer's own TRANSPOSE knob adds to it. SEQ ON starts at the next
transport start (or at once if the transport is already running).

## Build

```sh
combo/build.sh          # gen.py (params, layout, images) -> params.h + skin -> armhf plugin in combo/build/
combo/test.sh           # x86 host test under ASan/UBSan: the four modes, transposition, chunk round-trip
```

`gen.py` builds `params.json` and `layout.conf` from the sequencer's and the voice's own files, so skin changes are
made in `sequencer/vst/layout.conf` and `vst/layout.conf` and show up here on the next build. Sequencer parameters are
prefixed `q_` (and named "Seq ..."), so they don't clash with the voice's LFO and key parameters. The plugin id is `MzCb`.
