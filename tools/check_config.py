#!/usr/bin/env python3
"""Contract check: the generated movy_config.json + chain_params template
against Movy's ModuleConfig shape (reference/movy/src/types/param.ts).

Runs in the build container after gen_params.py — a violation fails the build.
"""
import json, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
fails = []

def check(cond, msg):
    if not cond:
        fails.append(msg)

# ---- movy_config.json shape -------------------------------------------
cfg = json.loads((ROOT / "src/movy_config.json").read_text())
check(cfg.get("id") == "tablor" and cfg.get("name"), "id/name present")
banks = cfg.get("banks", [])
check(len(banks) == 7, f"expected 7 banks, got {len(banks)}")
for b in banks:
    # Movy: a config bank is EXACTLY one page (bankGroups is per-bank but
    # indexed per-page; multi-row banks shift every later page label)
    check(len(b.get("rows", [])) == 1,
          f"bank {b.get('name')}: must be single-row (Movy bank == page)")

VALID_TYPES  = {"float", "int", "enum", "file"}
VALID_ENV    = {"a", "d", "s", "r"}
VALID_LFO    = {"shape", "phase", "mode", "retrig", "rate", "depth", "deform"}
VALID_FILTER = {"cutoff", "resonance", "mode", "slope"}

seen_keys = set()
for b in banks:
    check(isinstance(b.get("name"), str) and b["name"], "bank has a name")
    for r in b.get("rows", []):
        check(len(r) == 8, f"bank {b['name']}: row must have exactly 8 slots, got {len(r)}")
        for s in r:
            if s is None:
                continue
            k = s.get("key")
            check(isinstance(k, str) and k, f"slot missing key in bank {b['name']}")
            check(k not in seen_keys, f"duplicate key {k}")
            seen_keys.add(k)
            check(s.get("type") in VALID_TYPES, f"{k}: bad type {s.get('type')}")
            check(isinstance(s.get("short"), str) and len(s["short"]) <= 5,
                  f"{k}: short label must be <=5 chars ('{s.get('short')}')")
            check(isinstance(s.get("full"), str) and s["full"], f"{k}: full label")
            if s["type"] == "enum":
                check(isinstance(s.get("options"), list) and s["options"],
                      f"{k}: enum needs options")
            elif s["type"] == "file":
                check(isinstance(s.get("fileRoot"), str) and s["fileRoot"].startswith("/"),
                      f"{k}: file slot needs absolute fileRoot")
            else:
                check("min" in s and "max" in s and s["min"] < s["max"],
                      f"{k}: needs min < max")
            if "env" in s:    check(s["env"] in VALID_ENV, f"{k}: env hint {s['env']}")
            if "lfo" in s:    check(s["lfo"] in VALID_LFO, f"{k}: lfo hint {s['lfo']}")
            if "filter" in s: check(s["filter"] in VALID_FILTER, f"{k}: filter hint {s['filter']}")

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

    # every movy_config key must exist in chain_params, and vice versa
    # preset's options are published at runtime (the live preset list), so it
    # is deliberately absent from the static contract.
    DYNAMIC_KEYS = {"preset"}
    for k in seen_keys:
        check(k in chain_keys or k in DYNAMIC_KEYS,
              f"movy_config key {k} missing from chain_params")
    # Strings cannot be driven by a Movy encoder, so they are contract-only:
    # save_as is the keyboard-backed Save As cell on the device and web.
    MOVY_EXEMPT = {"save_as", "preset_name"}
    for k in chain_keys:
        check(k in seen_keys or k in MOVY_EXEMPT,
              f"chain_params key {k} not on any movy page")

    # types agree (movy 'file' pairs with schwung 'filepath')
    chain_types = {p["key"]: p["type"] for p in chain}
    pair = {"file": "filepath", "int": "int", "float": "float", "enum": "enum"}
    for b in banks:
        for r in b["rows"]:
            for s in r:
                if s and s["key"] in chain_types:
                    check(pair.get(s["type"]) == chain_types[s["key"]],
                          f"{s['key']}: movy type {s['type']} != chain {chain_types[s['key']]}")
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
    check(len(pages) == 7, f"7 pages expected, got {len(pages)}")
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
        check(k in page_keys, f"movy key {k} missing from ui_pages")
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
                  f"movy key {k} missing from hierarchy")

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
print(f"config contract OK: {len(banks)} banks, {len(seen_keys)} keys, "
      f"chain fmt splices to valid JSON, module.json {sz} B")
