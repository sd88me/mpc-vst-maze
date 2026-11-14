#!/usr/bin/env bash
# The catalog-shaped release zips for Maze Voice and Maze Sequencer 2, from the release build (./build-plugins.sh):
#   dist/Maze-Voice-<ver>-mpc-armv7.zip, -mpc-aarch64.zip, dist/Maze-Sequencer-<ver>-mpc-armv7.zip, -mpc-aarch64.zip
# Same name, uid and folder as 1.x, and the .so under its 1.x name at the folder root (maze_voice.so, maze_seq.so), so the installer
# upgrades a 1.x install in place and saved presets and projects still find the plugin file.
# MPC_VST: an mpc-vst-plugins checkout (tools/release.py, tools/catalog_check.py).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
MPC_VST="${MPC_VST:?set MPC_VST to an mpc-vst-plugins checkout with tools/release.py}"
[ -f "$MPC_VST/tools/release.py" ] || { echo "need mpc-vst-plugins with tools/release.py (MPC_VST)" >&2; exit 1; }
out="$here/dist"; mkdir -p "$out"
pack() {   # <name> <asset stem> <so name> <so slug> <uid hex> <uid ascii> <id> <about>
  local NAME="$1" STEM="$2" SO="$3" SLUG="$4" UIDX="$5" UIDA="$6" ID="$7" ABOUT="$8"
  local DIR="sd88me - VST - $NAME" SRC="$here/build/plugins/sd88me - VST - $NAME"
  [ -f "$SRC/VST/$SLUG-arm32.so" ] || { echo "missing $SRC: run ./build-plugins.sh first" >&2; exit 1; }
  local VER; VER=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$SRC/plugin.json")
  python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); sys.exit(0 if m["uid"]==sys.argv[2] and "Beta" not in m["name"] else "not the release identity")' "$SRC/plugin.json" "$UIDA"
  for pair in armv7:arm32:file32 aarch64:arm64:file64; do
    IFS=: read -r cpu arch key <<<"$pair"
    local st="$here/build/catalog/$STEM/$cpu"; rm -rf "$st"; mkdir -p "$st/$DIR"
    cp -r "$SRC/Plugin Skins" "$SRC/version.xml" "$st/$DIR/"
    python3 - "$SRC/plugin.json" "$st/$DIR/plugin.json" "$key" "$SO" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
m.pop("file32", None); m.pop("file64", None)
m[sys.argv[3]] = sys.argv[4]
json.dump(m, open(sys.argv[2], "w"), indent=2)
PY
    cp "$SRC/VST/$SLUG-$arch.so" "$st/$SO"
    printf '<PLUGIN name="%s" descriptiveName="%s" format="VST" category="Synth" manufacturer="sd88me" version="%s" file="%s" uid="%s" isInstrument="1" fileTime="0" infoUpdateTime="0" numInputs="0" numOutputs="2" isShell="0"/>\n' \
      "$NAME" "$NAME" "$VER" "$SO" "$UIDX" > "$st/entry.xml"
    python3 "$MPC_VST/tools/release.py" --so "$st/$SO" --skin "$st/$DIR" --entry "$st/entry.xml" --version "$VER" \
      --repo sd88me/mpc-vst-maze --license MIT --id "$ID" --about "$ABOUT" -o "$st/dist"
    cp "$st"/dist/*.zip "$out/$STEM-$VER-mpc-$cpu.zip"
  done
}
pack "Maze Voice" Maze-Voice maze_voice.so maze-voice 4d7a5663 MzVc maze-voice \
  "Monophonic Moog Labyrinth-style thru-zero oscillator / wavefolder / state-variable filter voice, with two LFOs and a page randomiser."
pack "Maze Sequencer" Maze-Sequencer maze_seq.so maze-sequencer 4d7a5371 MzSq maze-sequencer \
  "Dual 8-step generative sequencer (Moog Labyrinth style) with corrupt, scale quantising, two LFOs and per-line MIDI channels, driving other tracks over MIDI."
python3 "$here/tools/scrub_zip.py" "$out"/Maze-*-2.*-mpc-armv7.zip "$out"/Maze-*-2.*-mpc-aarch64.zip
# the one-folder zip for every MPC (both CPUs in one folder), from the two scrubbed per-CPU zips
for stem in Maze-Voice Maze-Sequencer; do
  v=$(ls "$out"/$stem-*-mpc-armv7.zip | sed "s/.*$stem-\(.*\)-mpc-armv7.zip/\1/")
  python3 "$MPC_VST/tools/sdk_zip.py" "$out/$stem-$v-mpc-sdk.zip" "$out/$stem-$v-mpc-armv7.zip" "$out/$stem-$v-mpc-aarch64.zip"
done
for z in "$out"/Maze-*-2.*-mpc-armv7.zip "$out"/Maze-*-2.*-mpc-aarch64.zip; do python3 "$MPC_VST/tools/catalog_check.py" --catalog --require-portable "$z" | tail -n 3; done
ls -la "$out"/Maze-*-2.*-mpc-*.zip
