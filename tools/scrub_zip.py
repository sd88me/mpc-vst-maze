#!/usr/bin/env python3
"""Rewrite comment lines in a release zip's install.sh that name firmware versions (public text stays version-neutral), refresh
SHA256SUMS, keep every entry's mode bits. tools/scrub_zip.py dist/<zip> [...]"""
import hashlib, re, sys, zipfile

FIX = [(re.compile(r"^(\s*#.*?)\(the plugin loads either way; device test, MPC 3\.10\.0, (docs/NOTES\.md.*)$"), r"\1(the plugin loads either way; \2"),
       (re.compile(r"^(\s*#\s*)MPC OS 3\.10\.1\+ loads plugins"), r"\1Newer MPC OS versions load plugins")]
for path in sys.argv[1:]:
    zin = zipfile.ZipFile(path)
    items = [(i, zin.read(i)) for i in zin.infolist()]
    zin.close()
    data = {i.filename: d for i, d in items}
    for i, d in items:
        if i.filename.endswith("/install.sh"):
            lines = d.decode().split("\n")
            for k, l in enumerate(lines):
                for rx, rep in FIX:
                    lines[k] = rx.sub(rep, lines[k])
            data[i.filename] = "\n".join(lines).encode()
    sums = [n for n in data if n.endswith("/SHA256SUMS")]
    for sn in sums:
        root = sn[: -len("SHA256SUMS")]
        out = []
        for line in data[sn].decode().splitlines():
            h, _, name = line.partition("  ")
            full = root + name
            if full in data:
                h = hashlib.sha256(data[full]).hexdigest()
            out.append(h + "  " + name)
        data[sn] = ("\n".join(out) + "\n").encode()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zout:
        for i, _ in items:
            zi = zipfile.ZipInfo(i.filename, i.date_time)
            zi.external_attr, zi.compress_type = i.external_attr, zipfile.ZIP_DEFLATED
            zout.writestr(zi, data[i.filename])
    left = [l for l in data[[n for n in data if n.endswith("/install.sh")][0]].decode().splitlines() if re.search(r"3\.10", l)]
    print(path, "scrubbed" if not left else "STILL: %s" % left)
