#!/usr/bin/env python3
"""Writes a JavaScript file as a C string constant, for code PXJS compiles
on first use (src/js/*.js -> src/px_*_js.h).

    python3 tools/embed_js.py src/js/url.js k_url_js > src/px_url_js.h

Whole-line // comments and blank lines are dropped (smaller binary, less to
lex); nothing else changes, so line numbers in errors are not the file's.
tools/verify.sh checks that every header matches its source."""

import sys


def main():
    path, name = sys.argv[1], sys.argv[2]
    lines = []
    for line in open(path, encoding="utf-8"):
        t = line.strip()
        if not t or t.startswith("//"):
            continue
        lines.append(line.rstrip("\n"))
    out = [f"/* Generated from {path} by tools/embed_js.py: do not edit. */", f"static const char {name}[] ="]
    for line in lines:
        esc = ""
        for ch in line + "\n":
            o = ord(ch)
            if ch == "\\":
                esc += "\\\\"
            elif ch == '"':
                esc += '\\"'
            elif ch == "\n":
                esc += "\\n"
            elif o < 0x20 or o > 0x7E:
                # UTF-8 bytes as octal escapes (hex escapes would swallow following hex digits)
                esc += "".join(f"\\{b:03o}" for b in ch.encode("utf-8"))
            else:
                esc += ch
        out.append(f'    "{esc}"')
    out[-1] += ";"
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
