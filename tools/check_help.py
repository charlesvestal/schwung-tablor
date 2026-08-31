#!/usr/bin/env python3
"""Every help line must fit the screen and be drawable.

The Help viewer draws a line with print(4, y, line) and walks it one glyph at
a time; anything past x=127 is dropped by set_pixel. There is no wrapping, no
truncation marker and nothing in the log, so an over-long line silently loses
its tail. A character with no glyph is dropped just as quietly -- an em dash
in "Lush Pad — wide" simply vanishes, leaving a double space.

Reported by Schwung upstream (issue #3) after auditing 124 modules, 27 of
which were shipping lines that run off screen. This is the gate that keeps
this module out of that list.

The font is PROPORTIONAL at runtime: load_font trims each glyph to its inked
extent, so "." advances 3px and "W" 6px. That makes it a pixel budget, not a
character count, so the widths are measured rather than guessed.
"""
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
FONT = json.loads((HERE / "font5x7_widths.json").read_text())
WIDTHS, SPACING = FONT["widths"], FONT["charSpacing"]
LEFT_MARGIN = 4          # print(4, y, line)
SCREEN_W = 128
LIMIT = SCREEN_W - LEFT_MARGIN - 1

def width(s):
    return sum(WIDTHS.get(c, 0) + SPACING for c in s)

def undrawable(s):
    return sorted({c for c in s if c not in WIDTHS})

def walk(node, path=""):
    title = node.get("title", "")
    here = f"{path}/{title}" if title else path
    for i, line in enumerate(node.get("lines", [])):
        yield here, i, line
    for child in node.get("children", []):
        yield from walk(child, here)

def main():
    path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else HERE.parent / "src/help.json")
    doc = json.loads(path.read_text())
    fails = []
    n = 0
    for where, i, line in walk(doc):
        n += 1
        w = width(line)
        if w > LIMIT:
            fails.append(f"{where} line {i}: {w}px > {LIMIT}px, the tail is dropped: {line!r}")
        bad = undrawable(line)
        if bad:
            fails.append(f"{where} line {i}: no glyph for {bad}, they vanish: {line!r}")
    if fails:
        print("HELP CONTRACT FAILED:")
        for f in fails:
            print("  -", f)
        return 1
    widest = max((width(l) for _, _, l in walk(doc)), default=0)
    print(f"help OK: {n} lines, widest {widest}px of {LIMIT}, all glyphs drawable")
    return 0

if __name__ == "__main__":
    sys.exit(main())
