# Maze Voice and Maze Sequencer 2

One engine family, two plugins. Each folder is a plugin project (`plugin.json`, `src/`, `skins/`).

Identity is frozen to the released builds (Maze Voice 1.0.5, Maze Sequencer 1.0.8): the same name, uid and
parameter order, so projects saved with them still load.

- `<plugin>/params.json`: the released 1.x parameter table, every field as that build compiled it, plus the CC map.
- `<plugin>/params.lock`: its VST order, one key per line. Append only. The `<key>__open` slots were the 1.x
  popup flags; they stay at their index.
- `tools/make_released_tables.sh` re-derives both from the release tags; `tests/make_legacy_chunks.sh` makes the
  old-format project chunks in `tests/data/` from the tags' own code.

Note: a Maze Voice 1.0.5 project's chunk holds the voice settings but not the LFO settings (the 1.x state text never
had them); the 2.x state saves everything and still reads the old text.
