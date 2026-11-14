# Maze Voice 2.0.0 and Maze Sequencer 2.0.0

New screens for both plugins, a new build underneath, and the same plugins: same names, same plugin IDs, same parameter order.
Your projects, presets and automation from 1.x load as before.

## Maze Voice 2.0.0
- **New screen** on the Moog-style panel: tabs **VOICE**, **FOLD/FILTER**, **TONE** and **LFO** (LFO 1 and LFO 2 are sub-tabs).
  Every Q-Link bank is four controls, in the order they sit on screen. Main knobs are larger than minor ones, as on the real panel.
- **FOLD/FILTER** carries the ROUTE section between VCW and VCF: the Blend knob and the three-way order switch.
- **LFO settings are now saved** with the project. 1.x did not store them, so they came back at their defaults on reload.
- Chrome toggle switches, full outlines round each section, larger value read-outs.

## Maze Sequencer 2.0.0
- **One screen for both sequencers**, as before, laid out with more room: Corrupt, CV Range and Length on the left of each line,
  then the eight step keys with a wider touch area, and Advance, Reset and the MIDI channel below them.
- **MIDI CH is a pop-up list (1 to 16)** instead of a knob. It is still on the Q-Link bank.
- The step keys and the play-head ring keep the 1.x look.
- **Starting values** for a new instance: Corrupt A at 1 o'clock, Corrupt B at 12 o'clock, Trig Mix -40, Scale MINOR, Key G.
  Saved projects keep their own values.
- LFO 1 and LFO 2 are sub-tabs of one tab.

## Upgrading
Install over 1.x: the plugin keeps its folder and file name, so the upgrade replaces it in place. Needs MPC OS 3.x, as 1.x did.
