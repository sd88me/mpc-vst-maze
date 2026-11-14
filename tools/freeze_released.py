#!/usr/bin/env python3
"""Freeze the parameter table a released 1.x build really compiled.

Usage: tools/freeze_released.py <released params.h> <out dir>

The released params.h is made by mpc-vst-plugins' tools/gen_vst.py at the tools commit the release workflow pinned,
run on the release tag (see tools/make_released_tables.sh). This writes, into <out dir>:
  params.json   every parameter in VST order with all of its 1.x behaviour fields, plus name/vendor/uid/CC map
  params.lock   one key per line, the VST index order (index = line number - 1). Append only from now on.
"""
import json, re, sys

FIELDS = ["key", "name", "unit", "min", "max", "def", "nopts", "opts", "momentary", "string_display", "int_display",
          "step_target", "step_delta", "popup_of", "hold_ms", "dynamic_name", "dynamic_display", "qlink_ticks", "no_poll",
          "nudge_pct", "nudge_gain", "refresh_display", "debounce_ms"]


def split_init(s):
    out, cur, q = [], "", False
    for ch in s:
        if ch == '"':
            q = not q
        if ch == "," and not q:
            out.append(cur.strip()); cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def value(tok):
    if tok.startswith('"'):
        return json.loads(tok)
    if tok in ("NULL", "0") and False:
        return None
    t = tok.rstrip("f")
    try:
        return int(t)
    except ValueError:
        return float(t)


def main():
    src, out = sys.argv[1], sys.argv[2]
    text = open(src).read()
    struct = re.search(r"typedef struct \{(.*?)\} param_t;", text, re.S).group(1)
    names = re.findall(r"(\w+)\s*(?:,|;)", re.sub(r"const char \*const \*", "", struct.replace("*", "")))
    opts = {m.group(1): [json.loads(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"', m.group(2))]
            for m in re.finditer(r"static const char \*const (OPTS_\d+)\[\] = \{(.*?)\};", text)}
    params = []
    for m in re.finditer(r'^\s*\{(".*)\},\s*$', text, re.M):
        toks = split_init(m.group(1))
        if len(toks) != len(names):
            sys.exit(f"field count {len(toks)} != {len(names)}: {m.group(0)[:80]}")
        p = {}
        for n, t in zip(names, toks):
            p[n] = opts.get(t) if n == "opts" else (None if t in ("NULL",) else value(t))
        params.append(p)
    n = int(re.search(r"#define NPARAMS (\d+)", text).group(1))
    assert n == len(params), (n, len(params))
    meta = {
        "name": json.loads(re.search(r"#define PLUG_NAME (\".*\")", text).group(1)),
        "vendor": json.loads(re.search(r"#define PLUG_VENDOR (\".*\")", text).group(1)),
        "uid": re.search(r"#define PLUG_UID (0x[0-9a-f]+) /\* '(.{4})' \*/", text).group(2),
        "uid_hex": re.search(r"#define PLUG_UID 0x([0-9a-f]+)", text).group(1),
        "cc": [int(x) for x in re.search(r"PLUG_CC\[\d+\] = \{(.*?)\}", text).group(1).split(",")]
              if "PLUG_CC" in text else [],
        "nrpn": "HAS_NRPN" in text,
        "source": "released 1.x params.h (gen_vst.py at the release's pinned tools commit)",
        "params": params,
    }
    with open(f"{out}/params.json", "w") as f:
        json.dump(meta, f, indent=1)
        f.write("\n")
    with open(f"{out}/params.lock", "w") as f:
        f.write("".join(p["key"] + "\n" for p in params))
    print(f"{out}: {len(params)} params, uid {meta['uid']} ({meta['uid_hex']}), name {meta['name']!r}")


if __name__ == "__main__":
    main()
