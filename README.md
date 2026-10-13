# mpc-vst-maze

💬 Questions or feedback? Join the [Open MPC Discord](https://discord.gg/sRRysZSgu3).

> **MPC OS 3.x and 2.x.** Releases from 1.0.3 are built for glibc 2.31 with the MPC OS 2.x skin shape, so the catalog labels them
> "MPC OS 2.x + 3.x". They are tested on a Force (3.x); a 2.x unit has not tried them yet (Maze Sequencer also needs
> `libasound.so.2`). See [MPC OS 2.x vs 3.x](https://github.com/sd88me/mpc-vst-plugins#mpc-os-2x-vs-3x) in the main repo.

Two native MPC OS VST2 plugins for Akai MPC standalone devices (Force, MPC Live/Live II, One, X, Key 61), both inspired
by the Moog Labyrinth. They load in MPC's own built-in plugin host and work on their own or together:

| Plugin | Type | What it does |
|---|---|---|
| **[Maze Voice](#maze-voice)** | instrument | A monophonic thru-zero oscillator / wavefolder / state-variable filter voice with two LFOs and a page randomiser. |
| **[Maze Sequencer](#maze-sequencer)** | MIDI generator | A dual 8-step generative sequencer. It makes no sound; it plays two random, scale-quantised step lines as MIDI into other tracks, such as Maze Voice. |

Each has its own MPC screen skin and Q-Link pages, and each is released separately (tags `maze-voice-vst-v*` and
`maze-sequencer-vst-v*`). Both are also in the plugin catalog.

A typical setup is Maze Sequencer on one track, driving Maze Voice on another: the sequencer writes the melody, and the
voice's LFOs and randomiser keep the sound moving. [How to set that up](#using-the-sequencer-with-maze-voice).

# Maze Voice

Maze Voice is a monophonic Moog Labyrinth-style thru-zero oscillator / wavefolder / state-variable filter voice with two
LFOs, its own MPC screen skin, and Q-Links. Add it to a track like any other instrument plugin and play it from pads,
keys or a MIDI clip.

## Voice features

A monophonic voice modeled on the Moog Labyrinth's signal path, with the
Force/Schwung LFO and randomiser layer built on top:

- **Thru-zero FM pair**: a sine VCO tracked by key, plus a triangle Mod
  oscillator with its own key-track and pitch EG, FM'd into the VCO
  (`fm_depth`/`fm_eg1`) for the Labyrinth's characteristic bell/clang tones.
  A ring-mod tap (VCO × Mod) and a variable-tone (dark↔bright) noise
  generator sit alongside them in the VCO/Mod/noise mixer.
- **Wavefolder**: drive + bias fold on the mixed signal, with its own pitch
  EG and key tracking, and a **Route** switch (`VCW>VCF`, `Parallel`,
  `VCF>VCW`) that decides whether the folder feeds the filter, the filter
  feeds the folder, or they run in parallel and get crossfaded with
  **Blend**.
- **State-variable filter**: a Cytomic/Simper TPT (zero-delay-feedback) SVF
  that morphs continuously lowpass → bandpass (no highpass tap — the real
  Labyrinth only sweeps LP↔BP), with resonance running from Butterworth-flat
  up to near self-oscillation, nonlinear saturation on the resonant feedback
  path so the peak blooms instead of ringing linearly, and its own Filt
  Drive stage (bypass-at-zero gain-into-tanh) feeding the filter input.
- **Output stage**: a Boss-style asymmetric-clip Tone/Sat saturator on the
  way out, plus warm per-channel mixer overdrive on the VCO/Mod/noise taps.
- **Two tempo-syncable LFOs**: 5 shapes (saw/tri/sine/square/S&H), free-run
  or clock-synced (1/16 to 8 bars), optional retrigger, each independently
  routed to 9 destinations (VCO/Mod pitch, FM depth, cutoff, both envelope
  decays, filter drive, fold amount, fold bias).
- **Page randomiser**: four latching per-page toggles (Voice / WaveFolder /
  Filter / Tone) plus a momentary Generate button that randomises every
  armed page's parameters at once, so you can lock in a section (say, the
  filter) while rolling the rest.
- **External Voice mode**: an alternate output tap (`out_mode`) that sends
  the raw, post-wavefolder oscillator mix out ungated and unfiltered (no
  blend, no VCA envelope, no filter) — for feeding an external filter or
  amp chain instead of the built-in filter/VCA path.
- **63 parameters**, all reachable from Q-Links across three MPC screen
  tabs — no menu-diving mid-performance.

## How the voice works

Signal flow, left to right:

```
VCO (sine) ──┐                         ┌─ Route: VCW>VCF  → fold → filter ─┐
Mod (tri) ───┼→ mixer → tone/drive →   ├─ Route: Parallel → fold + filter ─┼→ VCA → Tone/Sat → out
Noise ───────┤  (VCO·Mod ring tap)     └─ Route: VCF>VCW  → filter → fold ─┘   (Env 2)
Mod ─FM→ VCO ┘                           (Blend crossfades Parallel)
```

- **Two oscillators FM each other.** The Mod oscillator frequency-modulates the VCO. FM Depth sets how much, and FM EG1
  lets Env 1 sweep that depth on each note, which is where the bell and clang sounds come from. Both oscillators can
  follow the keyboard (VCO Key, Mod Key) or run at a fixed pitch, so you can go from tuned to inharmonic.
- **Two envelopes.** Env 1 (Env1 Dec) is a modulation envelope that can push the VCO pitch, Mod pitch, FM depth, fold
  amount and filter cutoff by the amounts of the `... EG1` knobs, positive or negative. Env 2 (VCA Decay) shapes the
  amplitude.
- **Fold, then filter, or the other way round.** Route picks the order, or runs them side by side and crossfades with
  Blend. The wavefolder adds harmonics, and the filter then removes or emphasises them, so the same settings sound very
  different in each order.
- **LFOs modulate the voice's own controls.** Each LFO has a depth per destination (bipolar, so it can push down as well
  as up). Synced to the project tempo, they give slow evolving movement that follows the song.
- **Randomise** is for finding sounds. Arm the pages you want changed (Voice, WaveFolder, Filter, Tone) and press
  Generate. Pages left off stay as they are.

## How the voice is built

- **Engine**: `src/maze_voice.c`, the same DSP core as the
  [`schwung-maze`](https://github.com/sd88me/schwung-maze) Maze Voice module
  and the Force addon in [`force-maze`](https://github.com/sd88me/force-maze),
  built with `-DMAZE_LFO=1` (the two LFOs) and `-DMAZE_VST=1`. `MAZE_VST`
  turns off the Move knob-touch filter that ignores notes 0–9, because MPC
  sends real notes there.
- **Parameters**: taken from `module.json`'s `chain_params`, so this plugin
  has the same 63 parameters as the other builds.
- **Skin**: `vst/layout.conf` is the Force Shadow page from `force-maze`
  (`maze-voice/addon/shadow_page.conf`), rebuilt as a native MPC skin. It has
  three tabs: VOICE, WAVEFOLDER / FILTER, and MOD / RANDOM. Each tab has its
  own Q-Link pages. DEST and GAIN are left out because on MPC those belong to
  the track mixer, not the plugin.
- **Wrapper**: [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)'
  generic port builder (`vst/vst.json`), with no hand-written VST shim.

## Layout

```
src/maze_voice.c        DSP core (keep in sync with force-maze / schwung-maze)
module.json             parameter table + hierarchy the builder reads
vst/vst.json            port config for mpc-vst-plugins' tools/build_port.sh
vst/layout.conf         MPC skin layout
vst/build.sh            build wrapper
.github/workflows/      draft-release workflow (mpc-vst-plugins' shared one)
```

## Build

You need an [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)
checkout, either next to this repo as `../mpc-vst` or pointed to with
`MPC_VST`, and Docker with armhf emulation:

```sh
MPC_VST=/path/to/mpc-vst-plugins ./vst/build.sh
```

The build writes these files to `vst/build/`:

- `maze_voice.so`: the plugin.
- `skin/sd88me - VST - Maze Voice/`: the skin. The plugin and its skin are one folder in `/sdcard/Synths/`; the release
  workflow (or `tools/release.py` in mpc-vst-plugins) packages them with an installer.
- `pluginlist-entry.xml`: the `<PLUGIN>` line for `pluginList-arm` in `MPC.settings`.

To test the build offline on x86 (ASan/UBSan, no device needed), run:

```sh
"$MPC_VST/tools/test_port.sh" vst/vst.json
```

## Installation

Download `Maze-Voice-<version>-mpc-armv7.zip` (Gen1: Force, MPC Live / Live II, One, X, Key 61) or `Maze-Voice-<version>-mpc-aarch64.zip` (Gen2, e.g. MPC Live III; MPC OS 3.x only; built and host-tested offline, not yet run on a Gen2 unit) from
[Releases](https://github.com/sd88me/mpc-vst-maze/releases) and unzip it.
Then copy the folder to the device and run its installer:

```sh
scp -r Maze-Voice-<version> root@<device-ip>:/tmp/
ssh root@<device-ip> sh /tmp/Maze-Voice-<version>/install.sh
```

The installer stops MPC, so save your project first. It also backs up
`MPC.settings`, adds the plugin to MPC's plugin list, and restarts MPC. The
zip's `INSTALL.md` has the manual steps and the uninstaller.

You need root shell access (SSH) to the device, which means a modded unit.
Installing plugins this way is unofficial, so back up first and use it at
your own risk.

## Releasing

Go to Actions → **VST release (draft)** → Run workflow, and enter a version.
The workflow builds the plugin, runs the host test, and creates a draft
release tagged `maze-voice-vst-v<version>`. Install that draft's zip on a
device, smoke-test it, then publish the draft. For details, see
mpc-vst-plugins' `docs/RELEASING.md`.

## History

This port started in `force-maze` as `maze-voice/vst/`. It was split into its
own repo so that `force-maze` holds only the Force (MockbaMod / Force Shadow)
version. The first VST release, `maze-voice-vst-v1.0.0`, is in
[force-maze's releases](https://github.com/sd88me/force-maze/releases/tag/maze-voice-vst-v1.0.0), so the next release from this repo should be 1.0.1 or later.

# Maze Sequencer

Maze Sequencer is a dual 8-step generative sequencer in the style of the Moog Labyrinth's two random step sequencers. It
makes no sound. It plays two lines of notes as MIDI, and you point other tracks (Maze Voice, or any instrument) at it.
The lines start with random gates and pitches, then you shape them with a few controls instead of entering notes:
the sequencer gives you pattern ideas, and you steer it with **Corrupt**, **CV Range**, scale and key.

## How it works

**Two lines, A and B.** Each line is 8 steps. Every step has a gate (on or off, the toggles under the line) and a hidden
random pitch value. When the line plays a step whose gate is on, it sends a MIDI note; a gate that is off is a rest.

**Where the pitch comes from.** Each step's random value is spread around a root note by **CV Range** (0 is every note at
the root, 100 is up to about 5 octaves each way), then snapped to the nearest note of the chosen **Scale** and **Key**.
The root is middle C plus Key plus Transpose. That is why a wide CV Range on a Pentatonic scale still sounds musical.
With the **UNQUANT** scale the notes are not snapped, so any semitone can appear.

**Clock.** The sequencer follows the MPC transport. It steps at the **Note Rate** (1/32 to a whole bar), starts from step 1
when you press play, and stops, releasing its notes, when you stop. No external clock is needed.

**Corrupt, the generative part.** Every time a step is played, Corrupt gives it a chance to change:
- From 0 to 50, the step's pitch value is sometimes replaced with a new random one. The gates stay as they are, so the
  rhythm holds while the melody drifts. At 50 each pass has about a 25% chance of a new pitch per step.
- Above 50 the step's gate can also flip (an off step turns on with a new pitch, an on step turns off). At 100 the pattern
  keeps rewriting itself.

Because Corrupt changes the pattern as it plays, a loop you like can wander off. Turn Corrupt back to 0 to freeze it. The
steps you hear are the steps shown, and the pattern is saved with the project.

**Length** (1 to 8) shortens a line, so A and B can run at different lengths and drift in and out of phase.
**Reset Both** sends both lines back to step 1 every 1, 2, 4 or 8 bars (OFF lets them drift), which gives long evolving
loops that still come back together.

**Trig Mix** crossfades the two lines' velocity. At the centre both play at similar velocity. Turning it left fades B out
(at full left B is silent), and turning it right fades A out. It works as a blend between two voices without any
level changes on the receiving tracks.

**Running light.** The red lamp above each line follows the play-head so you can see where each line is.

## Controls

The SEQUENCERS page has both lines on the left and timing and scale on the right.

| Control | What it does |
|---|---|
| **Steps 1-8** | The line's gates. Tap to switch a step on or off. |
| **CORRUPT** | How much the line rewrites itself as it plays (see above). 0 to 100. |
| **CV RANGE** | The pitch spread of the line's random values. 0 to 100, default 20. |
| **LENGTH** | Number of steps the line plays, 1 to 8. |
| **MIDI CH** | MIDI channel of this line, 1 to 16, set separately for A and B. |
| **ADVANCE** | Rotates the pattern one step forward each press (gates and pitches both move). It works while stopped, so you can try variations without running the transport. |
| **RESET** | Rolls a new random pattern for the line. Length, channel, Corrupt and CV Range stay as they are. |
| **TRIG MIX** | Velocity crossfade between A and B. |
| **NOTE RATE** | Step size: 1/32, 1/16, 1/8, 1/4, 1/2 or BAR. |
| **NOTE LEN** | Note length, 1/4 to 2 steps. Longer than 1 overlaps into the next step. |
| **TRANSPOSE** | Shift the whole sequence by -48 to +48 semitones. |
| **SCALE / KEY** | Quantise to Chromatic, Major, Minor, Pentatonic major or minor, Melodic or Harmonic minor, Whole tone, Hirajoshi, Major 7 or Minor 7 chords, or Unquantised, in any of the 12 keys. |
| **RESET BOTH - BARS** | Snap both lines back to step 1 every 1, 2, 4 or 8 bars, or OFF. |

### The LFO page

Two LFOs each modulate the sequencer's own controls, so the pattern evolves without touching the knobs. Each has a shape
(saw, triangle, sine, square or sample-and-hold), a free rate or a tempo-synced division (1/16 up to 8 bars), and eight
depth knobs, one per destination: A and B Corrupt, A and B Range, A and B Length, Trig Mix and Note Length. The depths are
bipolar (-100 to +100) and add to the knob's own value, so a destination with depth 0 is untouched.

Useful examples: a slow sine on A Length makes the line breathe between short and long. A synced saw on Corrupt ramps the
mutation up across 4 bars. A square on Trig Mix alternates between A and B.

### Q-Links

SEQ A, SEQ B and TIMING / SCALE pages cover the main controls, and LFO 1 and LFO 2 pages cover the modulation depths. The
touchscreen and the Q-Links change the same parameters.

## Using the sequencer with Maze Voice

1. Put **Maze Sequencer** on an instrument track. It produces no audio.
2. Put **Maze Voice** (or any instrument) on another track.
3. On that track, set the **MIDI input** to the port **Maze Sequencer**. Set its channel to the line's **MIDI CH**. Use
   one track for line A and another for line B, or the same channel if you want both to play one instrument.
4. Press play. The lines start from step 1 and follow the transport tempo.

The sequencer's notes go out over an ALSA MIDI port named after the plugin, because MPC OS ignores the MIDI output of a
plugin itself. If the receiving track doesn't hear anything, check that its MIDI input is set to the sequencer's port and
that the channel matches.

## Build and release

Build with `sequencer/vst/build.sh` and test offline with `sequencer/vst/test.sh`. Its core is vendored from
[force-maze](https://github.com/sd88me/force-maze) (see `sequencer/src/VENDORED.md`), and `sequencer/vst/maze_seq_vst.cpp`
is the plugin wrapper. The release workflow is **VST release - Maze Sequencer (draft)**, and installation works the same
way as Maze Voice, using `Maze-Sequencer-<version>-mpc-armv7.zip` (Gen1) or `Maze-Sequencer-<version>-mpc-aarch64.zip` (Gen2, untested on a device).

## License

MIT. See [LICENSE](LICENSE). Copyright © sd88me.
