#!/usr/bin/env python3
"""Contract check: the generated page layout + the chain_params template.

ui_pages.json is the page layout: it is what the web panel reads, and it
carries the same eight rows the device pages are built from.

Runs in the build container after gen_params.py — a violation fails the build.
"""
import json, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
fails = []

def check(cond, msg):
    if not cond:
        fails.append(msg)

# ---- page layout ------------------------------------------------------
cfg = json.loads((ROOT / "src/ui_pages.json").read_text())
pages = cfg.get("pages", [])
check(len(pages) == 8, f"expected 8 pages, got {len(pages)}")

VALID_TYPES = {"float", "int", "enum", "file"}

seen_keys = set()
for b in pages:
    check(isinstance(b.get("name"), str) and b["name"], "page has a name")
    r = b.get("slots", [])
    check(len(r) == 8, f"page {b['name']}: row must have exactly 8 slots, got {len(r)}")
    for s in r:
        if s is None:
            continue
        k = s.get("k")
        check(isinstance(k, str) and k, f"slot missing key on page {b['name']}")
        check(k not in seen_keys, f"duplicate key {k}")
        seen_keys.add(k)
        check(s.get("t") in VALID_TYPES, f"{k}: bad type {s.get('t')}")
        check(isinstance(s.get("n"), str) and len(s["n"]) <= 5,
              f"{k}: short label must be <=5 chars ('{s.get('n')}')")
        check(isinstance(s.get("full"), str) and s["full"], f"{k}: full label")
        if s["t"] == "enum":
            check(isinstance(s.get("options"), list) and s["options"],
                  f"{k}: enum needs options")
        elif s["t"] != "file":
            check("min" in s and "max" in s and s["min"] < s["max"],
                  f"{k}: needs min < max")

# ---- chain_params: static JSON, parse + cross-check -------------------
hdr = (ROOT / "src/dsp/params.h").read_text()
m = re.search(r'tb_chain_params_json =\n    "(.*)";', hdr)
check(m is not None, "chain_params found in params.h")
if m:
    raw = m.group(1).replace('\\"', '"').replace("\\\\", "\\")
    try:
        chain = json.loads(raw)
    except json.JSONDecodeError as e:
        fails.append(f"chain_params is not valid JSON: {e}")
        chain = []
    chain_keys = {p["key"] for p in chain}

    # every page key must exist in chain_params, and vice versa
    # preset's options are published at runtime (the live preset list), so it
    # is deliberately absent from the static contract.
    DYNAMIC_KEYS = {"preset"}
    for k in seen_keys:
        check(k in chain_keys or k in DYNAMIC_KEYS,
              f"page key {k} missing from chain_params")
    # Contract-only keys: reachable, but not as a turnable cell on a page.
    PAGE_EXEMPT = {"save_as", "preset_name"}
    for k in chain_keys:
        check(k in seen_keys or k in PAGE_EXEMPT,
              f"chain_params key {k} is on no page")

    # types agree (our 'file' pairs with schwung 'filepath')
    chain_types = {p["key"]: p["type"] for p in chain}
    pair = {"file": "filepath", "int": "int", "float": "float", "enum": "enum"}
    for b in pages:
        for s in b["slots"]:
            if s and s["k"] in chain_types:
                check(pair.get(s["t"]) == chain_types[s["k"]],
                      f"{s['k']}: page type {s['t']} != chain {chain_types[s['k']]}")
    # the wavetable cells are the stock file browser: bracketed, opened with
    # touch-pot + jog-click, with live preview while browsing
    for p in chain:
        if p["key"] in ("wt1_table", "wt2_table"):
            check(p["type"] == "filepath" and p.get("root", "").startswith("/")
                  and p.get("filter") and p.get("live_preview") is True,
                  f"{p['key']}: must be a filepath browser, got {p['type']}")
    # chain_params must stay printf-safe and STATIC: it is served from the SPI
    # callback, so building it per request jams the param bus
    check("%s" not in raw, "chain_params must be static (no runtime formatting)")

# ---- ui_pages.json (ui_chain.js data): parses, full key coverage ------
pages_file = ROOT / "src/ui_pages.json"
check(pages_file.exists(), "ui_pages.json generated")
if pages_file.exists():
    up = json.loads(pages_file.read_text())
    pages = up.get("pages", [])
    check(len(pages) == 8, f"8 pages expected, got {len(pages)}")
    page_keys = set()
    for pg in pages:
        check(isinstance(pg.get("name"), str) and pg.get("sec"), "page has name+sec")
        check(len(pg.get("slots", [])) == 8, f"page {pg.get('name')}: 8 slots")
        for s in pg["slots"]:
            if s is None:
                continue
            check(s.get("k") and s.get("n") and s.get("full") and s.get("t"),
                  f"slot needs k/n/full/t in {pg['name']}")
            if s["t"] == "enum":
                check(isinstance(s.get("options"), list) and s["options"],
                      f"{s['k']}: enum slot needs options")
            elif s["t"] == "int":
                check(s.get("min") is not None and s.get("max") is not None,
                      f"{s['k']}: int slot needs min/max")
            page_keys.add(s["k"])
    for k in seen_keys:
        check(k in page_keys, f"page key {k} missing from ui_pages")
    for k in page_keys:
        check(k in seen_keys, f"ui_pages key {k} not a real param")

# ---- ui_hierarchy (the STOCK Schwung 0.12+ editor is the UI) ----------
m3 = re.search(r'tb_ui_hierarchy_json =\n    "(.*)";', hdr)
check(m3 is not None, "ui_hierarchy emitted in params.h")
if m3:
    hs = m3.group(1).replace('\\"', '"').replace("\\\\", "\\")
    try:
        hier = json.loads(hs)
    except json.JSONDecodeError as e:
        fails.append(f"ui_hierarchy invalid JSON: {e}")
        hier = {"levels": {}}
    levels = hier.get("levels", {})
    check("root" in levels, "root level exists")
    # The preset browser lives on ROOT (obxd pattern): that puts [Presets]
    # BEFORE Main in jog order, so a jog off Main goes to the sections
    # instead of straight back into the preset list.
    # The fullscreen browser is deliberately GONE: loading lives on the
    # Preset page's enum cell (turn to step, dive for the list), which is
    # what the user asked for after the browser cost a whole page. Guard the
    # inverse now: nothing may quietly bring a browser page back.
    check(not any(l.get("list_param") for l in levels.values()),
          "no preset browser level (the Preset page's enum cell is the loader)")
    nav, hkeys = set(), set()
    for lid, lv in levels.items():
        for it in lv.get("params", []):
            if "level" in it:
                nav.add(it["level"])
            else:
                hkeys.add(it["key"])
        for kk in lv.get("knobs", []):
            check(any(it.get("key") == kk for it in lv.get("params", [])),
                  f"level {lid}: knob {kk} not among its params")
    for t in nav:
        check(t in levels, f"nav target {t} missing")
    for lid in levels:
        check(lid == "root" or lid in nav, f"level {lid} unreachable")
    # Params that exist and stream but deliberately have NO cell on the Move.
    # Listed rather than exempted by pattern so adding one is a decision:
    #   wt1_table / wt2_table  the filepath browsers -- opaque to a knob, and
    #                          the graphic beside them never read the file
    #   wt_pack                the pack chooser -- a whole page for a 3-row list
    # All three are reachable from the web UI, which is where they belong.
    DEVICE_HIDDEN = {"wt1_table", "wt2_table", "wt_pack"}
    for k in seen_keys:
        if k not in ("preset",):            # preset is the browser's list_param
            check(k in hkeys or k in DEVICE_HIDDEN
                  or k in ("save_preset", "preset_name"),
                  f"page key {k} missing from hierarchy")

    # viz contiguity: a group's members must sit adjacent within one 4-cell
    # row of some level's knobs (the 0.12 hard gate, checked at build time)
    viz_groups = {}
    for p in chain:
        v = p.get("viz")
        if isinstance(v, dict) and v.get("group"):
            viz_groups.setdefault(v["group"], []).append(p["key"])
    for g, keys in viz_groups.items():
        placed = False
        for lv in levels.values():
            knobs = lv.get("knobs", [])
            for rstart in (0, 4):
                rowk = knobs[rstart:rstart + 4]
                idxs = [rowk.index(k) for k in keys if k in rowk]
                if len(idxs) == len(keys):
                    idxs.sort()
                    if idxs == list(range(idxs[0], idxs[0] + len(idxs))):
                        placed = True
        check(placed, f"viz group '{g}' not contiguous in any 4-cell row")

# ---- module.json under the 8 KB loader cap ----------------------------
sz = (ROOT / "src/module.json").stat().st_size
check(sz < 8192, f"module.json {sz} bytes exceeds 8 KB cap")

if fails:
    print("CONFIG CONTRACT FAILED:")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print(f"config contract OK: {len(pages)} pages, {len(seen_keys)} keys, "
      f"chain fmt splices to valid JSON, module.json {sz} B")
