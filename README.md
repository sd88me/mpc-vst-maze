# mpc-vst-maze

**Maze Voice** as a native MPC OS VST2 instrument for Akai MPC standalone
devices (Force, MPC Live/Live II, One, X, Key 61): a monophonic Moog
Labyrinth-style thru-zero oscillator / wavefolder / state-variable filter
voice with two LFOs, its own MPC screen skin, and Q-Links.

Loaded by MPC's own built-in plugin host. Add it to a track like any other
instrument plugin and play it from pads, keys or a MIDI clip.

## Features

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

## What it is

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

- `maze_voice.so`: the plugin, which goes in `/sdcard/vst/`.
- `skin/sd88me - VST - Maze Voice/`: the skin, which goes in `/sdcard/Synths/`.
- `pluginlist-entry.xml`: the `<PLUGIN>` line for `pluginList-arm` in `MPC.settings`.

To test the build offline on x86 (ASan/UBSan, no device needed), run:

```sh
"$MPC_VST/tools/test_port.sh" vst/vst.json
```

## Installation

Download `Maze-Voice-<version>-mpc-armv7.zip` from
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

## License

MIT. See [LICENSE](LICENSE). Copyright © sd88me.
