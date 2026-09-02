#!/usr/bin/env python3
"""Tablor parameter generator — the single source of truth.

Emits, from one table:
  src/module.json        (< 8 KB — Schwung's loader cap; holds NO chain_params)
  src/movy_config.json   (the six Movy banks with render hints)
  src/dsp/params.h       (param table + chain_params JSON served via get_param)

Every continuous control is a 0..127 pot (the ER-99 doctrine): the DSP maps
each pot to its musical range internally; the screen never shows ms or Hz.
Musical integers (semitones, cents, voices) keep their real ranges.

Run: python3 tools/gen_params.py     (from the repo root)
"""
import json, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERSION = "1.2.1"

# ---------------------------------------------------------------- enums
FILTER_TYPES = ["LP 12", "LP 24", "HP 12", "HP 24", "BP 12", "BP 24", "Notch 12", "Notch 24"]
# Same rule (this list drives the `waveform` graphic): "Pulse 25"/"Pulse 12"
# were unrecognised and drew sines. "Pulse" and "Pulse Tr" are both in the set.
SUB_WAVES    = ["Sine", "Triangle", "Saw", "Square", "Pulse", "Pulse Tr"]
NOISE_TYPES  = ["White", "Pink"]
# Option NAMES are what the host draws a graphic from: Schwung matches them
# against a known set (viz_draw.mjs) and falls back to a SINE for
# anything it does not recognise. "Square+" -- our unipolar square -- was not
# in that set, so it drew a sine, which is a different waveform entirely.
# "Pulse" is recognised, and a 50% unipolar square has exactly the silhouette
# of the square glyph it selects. Renaming is safe for saved sounds: presets
# store the INDEX, and the order here is unchanged.
ONOFF        = ["Off", "On"]
RETRIG       = ["Free", "Retrig"]
VOICE_MODES  = ["Poly", "Mono"]
GLIDE_MODES  = ["Off", "Glissando", "Portamento"]

# Wavetable selection is a DYNAMIC ENUM: the options are whatever the scanner
# finds, served live from get_param("chain_params"). It has to be an enum —
# the stock UI cannot TURN a filepath/string/canvas param (param_meta.mjs:
# KIND_OPAQUE, isTurnable), it only opens them on click. An enum turns to the
# next wavetable, opens a full-screen list on click, and puts the full name in
# the header. Names come through without their extension.
WT_ROOT   = "/data/UserData/UserLibrary/Wavetables"
WT_FILTER = [".wav", ".wt2048", ".wt1024", ".wt512", ".wt256"]

def wtfile(key, short, full):
    """FILEPATH — the Schwung way: the cell shows the file in brackets, and
    touch-pot + jog-click opens the real browser (folders, live preview).
    A knob cannot TURN it (KIND_OPAQUE) and that is accepted: turning was
    tried as a dynamic enum and the option list had to be rebuilt on the SPI
    callback, which jammed the param bus."""
    return dict(key=key, short=short, full=full, type="file", default="")

# ---------------------------------------------------------------- helpers
def pot(key, short, full, default=64):
    """0..127 pot; DSP maps to the musical range."""
    return dict(key=key, short=short, full=full, type="int", min=0, max=127, default=default)

def num(key, short, full, lo, hi, default=0):
    """A musical integer shown as-is (semitones, cents, voices…)."""
    return dict(key=key, short=short, full=full, type="int", min=lo, max=hi, default=default)

def enum(key, short, full, options, default=0, **kw):
    d = dict(key=key, short=short, full=full, type="enum", options=options, default=default)
    d.update(kw)
    return d

def hint(p, **kw):
    """Attach movy-only render hints (env/filter/knobAcceleration…)."""
    q = dict(p); q["_movy"] = kw; return q

# ---------------------------------------------------------------- the surface
# Trimmed 2026-08-16 per Gus: no fine, no retrig anywhere, no sub/noise pan,
# no filter routing switches. Behaviors hardcode to the old defaults in the DSP.
#
# Trimmed again 2026-09-02: the two LFOs and the four mod slots are gone. Four
# pages of clutter for modulation Schwung already provides -- a slot LFO can
# target any Tablor parameter from the slot settings, which is where a user
# reaches for it first. Velocity and key tracking stay: they are envelope
# controls, not a matrix.
def osc(n):
    return dict(
        table  = wtfile(f"wt{n}_table", f"WT{n}", f"WT{n}"),
        pos    = pot(f"wt{n}_pos",     f"POS{n}",  f"WT{n} Pos", 0),
        level  = pot(f"wt{n}_level",   f"LVL{n}",  f"WT{n} Level", 100 if n == 1 else 0),
        tune   = num(f"wt{n}_tune",    f"TUN{n}",  f"WT{n} Tune", -24, 24, 0),
        uni    = num(f"wt{n}_uni",     f"UNI{n}",  f"WT{n} Unison", 1, 4, 1),
        detune = pot(f"wt{n}_detune",  f"DET{n}",  f"WT{n} Detune", 0),
        spread = pot(f"wt{n}_spread",  f"SPR{n}",  f"WT{n} Spread", 0),
        pan    = pot(f"wt{n}_pan",     f"PAN{n}",  f"WT{n} Pan", 64),
        bend   = pot(f"wt{n}_bend",    f"BND{n}",  f"WT{n} Bend", 64),
        formant= pot(f"wt{n}_formant", f"FRM{n}",  f"WT{n} Formant", 64),
    )

O1, O2 = osc(1), osc(2)

FILTER = dict(
    freq = hint(pot("flt_freq", "FREQ", "Filter Freq", 127), filter="cutoff"),
    res  = hint(pot("flt_res",  "RES",  "Filter Res", 0),    filter="resonance"),
    env  = pot("flt_env",  "FENV", "Filter EG Amt", 64),
    type = hint(enum("flt_type", "TYPE", "Filter Type", FILTER_TYPES, 1), filter="mode"),
    key  = pot("flt_key",  "KEYT", "Key Track", 0),
    vel  = pot("flt_vel",  "VELT", "Vel Track", 0),
)
SUB = dict(
    level = pot("sub_level", "SUB",  "Sub Level", 0),
    wave  = enum("sub_wave", "SUBW", "Sub Wave", SUB_WAVES, 0),
    tune  = num("sub_tune",  "SUBT", "Sub Tune", -24, 24, -12),
)
NOISE = dict(
    level = pot("noise_level", "NOIS", "Noise Level", 0),
    type  = enum("noise_type", "NTYP", "Noise Type", NOISE_TYPES, 0),
)
VCA = dict(
    # NO env hints: Movy's roleOf() gives every hinted stage qualifier "" —
    # two hinted foursomes then collapse into ONE group and the loser draws
    # as lone ramps. Unhinted, Movy parses the LABELS ("VCA Attack" -> Amp
    # group, "Flt Attack" -> Filter group) and stacks two named envelopes.
    # Defaults are the original's: attack 0.1 s, decay 0.1 s, sustain 80%,
    # release 0.1 s (PluginProcessor.cpp, both the amp and filter envelopes).
    # These had been ported as 0 / 100% / 5.7 ms, so a fresh patch had an
    # instant attack and an abrupt release -- the clicky first impression.
    a = pot("vca_a", "ATK", "VCA Attack", 51),
    d = pot("vca_d", "DEC", "VCA Decay", 64),
    s = pot("vca_s", "SUS", "VCA Sustain", 102),
    r = pot("vca_r", "REL", "VCA Release", 64),
    vel = pot("vca_vel", "VVEL", "VCA Velocity", 100),
)
FEG = dict(
    a = pot("flt_a", "FATK", "Flt Attack", 51),
    d = pot("flt_d", "FDEC", "Flt Decay", 64),
    s = pot("flt_s", "FSUS", "Flt Sustain", 102),
    r = pot("flt_r", "FREL", "Flt Release", 64),
)
# Presets are Schwung's, not ours. Its browser (shadow_ui_presets.mjs) is
# reached from the slot, works for every module, auditions as you scroll, and
# stores under /data/UserData/schwung/presets/<module-id>/ from the module's
# own `state` blob -- which is to say it needed nothing from Tablor but the
# blob Tablor already answers. Carrying a second preset system beside it was
# a page, four cells and a file format to keep honest, for less.
#
# The DSP still seeds the factory sounds into that store and copies anything
# a user had saved as .tblr (export_presets_to_schwung in tablor_plugin.cpp).

GLOBAL = dict(
    mode   = enum("voice_mode", "MODE", "Voice Mode", VOICE_MODES, 0),
    voices = num("voices", "VOIC", "Voices", 1, 8, 8),
    glide  = pot("glide", "GLID", "Glide", 0),
    gmode  = enum("glide_mode", "GMOD", "Glide Mode", GLIDE_MODES, 0),
    legato = enum("legato", "LEGA", "Legato", ONOFF, 0),
    pb     = num("pb_range", "BEND", "PB Range", 0, 24, 2),
    vol    = pot("volume", "VOL", "Volume", 100),
)

# ---------------------------------------------------------------- pages
def row(*slots):
    out = list(slots) + [None] * (8 - len(slots))
    assert len(out) == 8, f"row has {len(slots)} slots"
    return out

BANKS = [
    ("Osc", False, [
        row(O1["table"], O2["table"], O1["pos"], O2["pos"],
            O1["level"], O2["level"], O1["tune"], O2["tune"]),
        row(O1["uni"], O1["detune"], O1["spread"], O1["pan"],
            O2["uni"], O2["detune"], O2["spread"], O2["pan"]),
        row(O1["bend"], O1["formant"], O2["bend"], O2["formant"],
            SUB["tune"]),
    ]),
    # Stock pages render 2 rows of 4; a viz group must sit contiguously in
    # one row. freq/res/TYPE first so the filter-curve roles touch.
    ("Filter", False, [
        row(FILTER["freq"], FILTER["res"], FILTER["type"], FILTER["env"],
            SUB["level"], SUB["wave"], NOISE["level"], NOISE["type"]),
    ]),
    # Both envelopes on ONE page: a Movy envelope graphic spans one 4-cell
    # line and a page has two lines, so VCA (line 1) + Filter (line 2) draw
    # as two stacked ADSR graphics (the manual's env_dual). Each foursome
    # must own its own line — split across lines it degrades to lone ramps.
    ("Env", False, [
        row(VCA["a"], VCA["d"], VCA["s"], VCA["r"],
            FEG["a"], FEG["d"], FEG["s"], FEG["r"]),
        row(VCA["vel"], FILTER["key"], FILTER["vel"]),
    ]),
    ("Global", True, [
        row(GLOBAL["mode"], GLOBAL["voices"], GLOBAL["glide"], GLOBAL["gmode"],
            GLOBAL["legato"], GLOBAL["pb"], GLOBAL["vol"]),
    ]),
]

# ---------------------------------------------------------------- collect
def all_params():
    seen, out = set(), []
    for _, _, rows in BANKS:
        for r in rows:
            for s in r:
                if s and s["key"] not in seen:
                    seen.add(s["key"]); out.append(s)
    return out

# The 8 assignable macros were cut in 1.1.1, with the LFOs and the mod slots
# before them: two more pages, and a 46-entry target list to scroll, for
# indirection over knobs that are already one jog away. Schwung's own knob
# mapping reaches any Tablor parameter from the slot settings and is where a
# user looks for exactly this.
PARAMS = all_params()

PARAMS = all_params()

PARAMS = all_params()

# ---------------------------------------------------------------- emit: movy_config
def movy_slot(p):
    if p is None:
        return None
    s = {"key": p["key"], "short": p["short"], "full": p["full"], "type": p["type"]}
    if p["type"] == "file":
        s["fileRoot"] = WT_ROOT
        s["fileFilter"] = WT_FILTER
    elif p["type"] == "enum":
        s["options"] = p["options"]
    else:
        s["min"], s["max"] = p["min"], p["max"]
    if p.get("automatable") is False:
        s["automatable"] = False
    if p.get("behavior"):
        s["behavior"] = p["behavior"]
        if p["behavior"] == "trigger":
            s["access"] = "write"      # 0.12+ contract; behavior stays for Movy
    for k, v in p.get("_movy", {}).items():
        s[k] = v
    return s

# Movy constraint (config-pages.ts: "Each config bank is exactly one page"):
# bankGroups is one entry PER BANK but indexed PER PAGE, so a multi-row bank
# shifts every following page label. One bank per page, named like ui_chain.
MOVY_PAGE_NAMES = {
    ("Osc", 0): "Osc",    ("Osc", 1): "Unison",  ("Osc", 2): "Shape",
    ("Filter", 0): "Filter",
    ("Env", 0): "Env", ("Env", 1): "Env+",
    ("Global", 0): "Global",
}

movy_banks = []
for name, glob, rows in BANKS:
    for ri, r in enumerate(rows):
        movy_banks.append({
            "name": MOVY_PAGE_NAMES[(name, ri)],
            **({"global": True} if glob else {}),
            "rows": [[movy_slot(s) for s in r]],
        })

movy_config = {
    "id": "tablor",
    "name": "Tablor",
    "banks": movy_banks,
}

# ---------------------------------------------------------------- emit: chain_params
# Schwung 0.12+ parameter visualisations: DECLARED, not detected (the docs'
# own advice). Groups must sit contiguously within one 4-cell row — the page
# layouts above are ordered for exactly that.
VIZ = {
    "vca_a": {"group": "amp",  "role": "attack"},
    "vca_d": {"group": "amp",  "role": "decay"},
    "vca_s": {"group": "amp",  "role": "sustain"},
    "vca_r": {"group": "amp",  "role": "release"},
    "flt_a": {"group": "fenv", "role": "attack"},
    "flt_d": {"group": "fenv", "role": "decay"},
    "flt_s": {"group": "fenv", "role": "sustain"},
    "flt_r": {"group": "fenv", "role": "release"},
    "flt_freq": {"group": "flt", "role": "cutoff"},
    "flt_res":  {"group": "flt", "role": "resonance"},
    "flt_type": {"group": "flt", "role": "mode"},
    "sub_wave":  {"kind": "waveform"},
    "wt1_level": {"kind": "fader"},
    "wt2_level": {"kind": "fader"},
    "sub_level": {"kind": "fader"},
    "noise_level": {"kind": "fader"},
    "volume": {"kind": "fader"},
    "legato": {"kind": "switch"},
}
def chain_param(p):
    if p["type"] == "file":
        d = {"key": p["key"], "name": p["full"], "type": "filepath",
             "root": WT_ROOT, "start_path": WT_ROOT,
             "filter": WT_FILTER, "live_preview": True, "default": ""}
    elif p["type"] == "enum":
        d = {"key": p["key"], "name": p["full"], "type": "enum",
             "options": p["options"], "default": p["options"][p["default"]]}
    else:
        d = {"key": p["key"], "name": p["full"], "type": p["type"],
             "min": p["min"], "max": p["max"], "default": p["default"]}
    if p["key"] in VIZ:
        d["viz"] = VIZ[p["key"]]
    # access:"write" is what stops the host scrubbing a trigger with a knob.
    # It was being emitted into the wrong builder, so the host treated SAVE
    # as an ordinary enum and every detent of a turn fired it -- reported as
    # one turn of the pot creating 29 preset files.
    if p.get("behavior") == "trigger":
        d["access"] = "write"
    return d

# `preset` is dynamic: its options ARE the preset list, which changes on
# every save -- so the module publishes it into the contract tail at runtime,
# exactly like the wavetable enums. A static entry would duplicate the key.
chain = [chain_param(p) for p in PARAMS if p["key"] != "preset"]
chain_json = json.dumps(chain, separators=(",", ":"))
assert "%" not in chain_json, "chain_params must be printf-safe (served verbatim)"

# ---------------------------------------------------------------- emit: ui_pages.json
# Data for Tablor's OWN Shadow UI (ui_chain.js) — the 9W9 treatment: jog flips
# pages, Shift+jog jumps sections, 8 encoders edit the visible page.
# NOTE: the module must NOT serve ui_hierarchy — the Shadow UI's hierarchy
# editor takes precedence over ui_chain.js whenever a module offers one.
PAGE_MAP = [  # (bank name, row index, page title). Section = bank name.
    ("Osc",    0, "OSC"),
    ("Osc",    1, "UNISON"),
    ("Osc",    2, "SHAPE"),
    ("Filter", 0, "FILTER"),
    ("Env",    0, "ENV"),
    ("Env",    1, "ENV+"),
    ("Global", 0, "GLOBAL"),
]

def page_slot(p):
    if p is None:
        return None
    d = {"k": p["key"], "n": p["short"], "full": p["full"], "t": p["type"]}
    if p["type"] == "enum":
        d["options"] = p["options"]
    elif p["type"] == "int":
        d["min"], d["max"] = p["min"], p["max"]
    if p.get("behavior"):
        d["b"] = p["behavior"]
    return d

def build_pages():
    rows = {(name, i): r for name, _, rws in BANKS for i, r in enumerate(rws)}
    pages = []
    for bank, ri, title in PAGE_MAP:
        pages.append({
            "name": title,
            "sec": bank,
            "slots": [page_slot(s) for s in rows[(bank, ri)]],
        })
    return {"pages": pages}

ui_pages = build_pages()

# ---------------------------------------------------------------- ui_hierarchy
# Schwung 0.12+: the STOCK hierarchy editor draws knob pages with viz graphics,
# has a native preset-browser level (list_param/count_param/name_param), a
# file browser for filepath params, and an on-screen keyboard for `string`
# params. Tablor no longer ships its own editor — this hierarchy IS the UI.
def hier_param(p):
    d = {"key": p["key"], "name": p["full"]}
    if p["type"] == "file":
        d.update(type="filepath", root=WT_ROOT, start_path=WT_ROOT,
                 filter=WT_FILTER, live_preview=True)
    elif p["type"] == "enum":
        d.update(type="enum", options=p["options"])
    else:
        d.update(type="int", min=p["min"], max=p["max"])
    return d

def build_hierarchy():
    rows = {(name, i): r for name, _, rws in BANKS for i, r in enumerate(rws)}
    levels = {}

    level_ids = []
    for bank, ri, title in PAGE_MAP:
        if bank == "Preset":
            continue                      # the browser lives on root (below)
        if (bank, ri) == ("Osc", 0):
            continue                      # root IS the OSC headline page
        lid = title.lower().replace(" ", "").replace(".", "").replace("-", "")
        r = rows[(bank, ri)]
        levels[lid] = {
            "name": title.title() if title.isupper() else title,
            "params": [hier_param(s) for s in r if s],
            "knobs": [s["key"] for s in r if s],
        }
        level_ids.append((lid, levels[lid]["name"]))

    # ROOT declares the preset browser AND the OSC knobs — the obxd pattern
    # the planner is built for ("a level is routinely both the Main knob page
    # and the preset browser"). That yields, in jog order:
    #
    #     [Presets] > Main > Unison > Shape > Filter > ...
    #
    # so you land on the browser, pick a sound, and every jog from Main goes
    # forward through the SECTIONS. A separate presets level put the browser
    # *between* Main and the sections instead, which meant one jog off Main
    # dropped you straight back into the preset list.
    #
    # Rename is a params-only `string` entry (opens the stock keyboard; the
    # DSP renames the preset's file). Save Preset is a knob on Global.
    osc_row = rows[("Osc", 0)]

    # The wavetable pickers are the two knobs users reach for first, and they
    # were the two that could not move. The note at the top of this file
    # already says why -- "it HAS to be an enum, the stock UI cannot TURN a
    # filepath" -- but hier_param() emits filepath, so the intent never
    # reached the contract. Reported by a tester as "I want to change
    # wavetables with the pots... right now it's not possible".
    #
    # So the KNOB drives an enum (wt1_select / wt2_select), whose options the
    # DSP publishes at runtime from the scanner. The file browser is not lost:
    # its filepath entry stays in params[] as "WT1 File", one row down.
    file_keys = [s["key"] for s in osc_row if s and s["type"] == "file"]
    sel_of    = {k: k.split("_")[0] + "_select" for k in file_keys}

    root_params = []
    for s in osc_row:
        if not s:
            continue
        if s["type"] == "file":
            # Turnable first; options come from get_param("chain_params").
            root_params.append({"key": sel_of[s["key"]],
                                "name": s["full"] + " Table", "type": "enum"})
        else:
            root_params.append(hier_param(s))
    # NEITHER the pack chooser NOR the filepath browsers appear on the device.
    #
    # Both were opaque to a knob, so each claimed a whole cell and the pair of
    # them pushed extra MAIN pages: one showing two brackets under the stock
    # "sample" graphic -- a fixed shape that never reads the file
    # (viz_draw.mjs drawSample), so it said nothing about the loaded table --
    # and one that was just a three-row list of pack names. Reported as "what
    # is this shit page, we don't need this on the module".
    #
    # Nothing is lost. wt1_select / wt2_select are on knobs 1-2, and an enum
    # is divable, so hold+click opens a scrolling picker over every table:
    # with no pack chosen the list is "All", and the option cap (128) is
    # comfortably above the libraries this ships against. wt_pack and the
    # wtN_table keys still exist and still stream -- the web UI drives both --
    # they simply have no cell on the Move.
    # The save page: tap-buttons plus Rename, first section after Main so
    # saving is one jog away from the sound you just made.
    for lid, label in level_ids:
        root_params.append({"level": lid, "label": label})
    # Preset management is the LAST page: it is where you go when the sound is
    # finished, not something to page through on the way to the filter.
    # No list_param: the fullscreen browser page is gone. Loading lives on
    # the Preset page's PRESET cell -- turn to step, dive for the full list.
    levels["root"] = {
        "name": "Tablor",
        # Shift+click inside the preset browser saves the current sound as a
        # new preset (the host writes "new" here) and opens the keyboard on
        # name_param. Hosts without the gesture ignore this key.
        "save_param": "save_preset",
        "params": root_params,
        "knobs": [sel_of.get(s["key"], s["key"]) for s in osc_row if s],
    }

    # (The WT Pack items level lived here. It existed so the host would
    # re-read the contract after a pack commit -- armContractSettle -- which
    # only matters if the pack can be changed from the device, and it no
    # longer can. The web UI sets wt_pack directly.)

    # No list_param: a level that declares one renders as a scrolling browser
    # INSTEAD of its knobs (page_plan.mjs: "the browser takes priority"), and
    # this page needs its four cells. PRESET is an enum whose options are the
    # live preset list, so a turn steps presets and a dive opens the picker.
    return {"levels": levels}

hierarchy_json = json.dumps(build_hierarchy(), separators=(",", ":"))

# ---------------------------------------------------------------- emit: module.json
module_json = {
    "id": "tablor",
    "name": "Tablor",
    "abbrev": "TBL",
    "version": VERSION,
    "description": "2-oscillator wavetable synth - 8-voice poly, sub, noise, "
                   "multimode filter, two envelopes. Serum/Vital/Ableton "
                   "wavetables via Move Manager.",
    "author": "athousanddetails; after Wavetable (Roland Rabien / FigBug, BSD-3)",
    "dsp": "dsp.so",
    "api_version": 2,
    "capabilities": {
        "audio_out": True,
        "audio_in": False,
        "midi_in": True,
        "midi_out": False,
        "chainable": True,
        "component_type": "sound_generator",
    },
}

# ---------------------------------------------------------------- emit: params.h
def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')

lines = [
    "/* Generated by tools/gen_params.py — DO NOT EDIT.",
    " * The parameter surface: IDs, ranges, defaults, and the chain_params",
    " * JSON template (wavetable option lists are spliced in at runtime). */",
    "#ifndef TABLOR_PARAMS_H",
    "#define TABLOR_PARAMS_H",
    "",
    f'#define TB_VERSION "{VERSION}"',
    f"#define TB_PARAM_COUNT {len(PARAMS)}",
    "",
    "typedef enum { TB_INT = 0, TB_ENUM = 1, TB_PATH = 2 } tb_param_type_t;",
    "",
    "typedef struct {",
    "    const char *key;",
    "    tb_param_type_t type;",
    "    float min, max, def;              /* enums: def is the option index */",
    "    int n_options;                    /* 0 for non-enums                */",
    "    const char *const *options;       /* NULL for non-enums             */",
    "} tb_param_t;",
    "",
    "/* TB_PATH params (the wavetable files) store a string, not a float. */",
    "#define TB_WT_SLOTS 2",
    "",
]
enum_names = []
for i, p in enumerate(PARAMS):
    enum_names.append(f"    TB_P_{p['key'].upper()} = {i},")
lines += ["enum {"] + enum_names + ["};", ""]

# Option string tables (shared where identical)
opt_tables, opt_index = [], {}
for p in PARAMS:
    if p["type"] == "enum" and p["options"] is not None:
        key = tuple(p["options"])
        if key not in opt_index:
            name = f"tb_opts_{len(opt_tables)}"
            opt_index[key] = name
            opts = ", ".join(f'"{c_escape(o)}"' for o in p["options"])
            opt_tables.append(f"static const char *const {name}[] = {{ {opts} }};")
lines += opt_tables + [""]

rows = []
for p in PARAMS:
    if p["type"] == "file":
        rows.append(f'    {{ "{p["key"]}", TB_PATH, 0, 0, 0, 0, 0 }},')
    elif p["type"] == "enum":
        tbl = opt_index[tuple(p["options"])]
        rows.append(f'    {{ "{p["key"]}", TB_ENUM, 0, {len(p["options"]) - 1}, '
                    f'{p["default"]}, {len(p["options"])}, {tbl} }},')
    else:
        rows.append(f'    {{ "{p["key"]}", TB_INT, {p["min"]}, {p["max"]}, {p["default"]}, 0, 0 }},')
lines += ["static const tb_param_t tb_params[TB_PARAM_COUNT] = {"] + rows + ["};", ""]

lines += [
    "/* chain_params: fully static, served verbatim. It MUST stay cheap —",
    " * get_param runs on the SPI callback. */",
    f'static const char *tb_chain_params_json =\n    "{c_escape(chain_json)}";',
    "",
    "/* ui_hierarchy for the STOCK Shadow UI (Schwung 0.12+): knob pages",
    " * with declared viz graphics, native preset browser, file browser,",
    " * on-screen keyboard for renames. Tablor ships no custom editor. */",
    f'static const char *tb_ui_hierarchy_json =\n    "{c_escape(hierarchy_json)}";',
    "",
]

# User-macro wiring: pot index, selector index, and option->param-index map.
lines += [
    "#endif /* TABLOR_PARAMS_H */",
]

# ---------------------------------------------------------------- write
out_module = ROOT / "src" / "module.json"
out_movy   = ROOT / "src" / "movy_config.json"
out_header = ROOT / "src" / "dsp" / "params.h"

out_pages = ROOT / "src" / "ui_pages.json"

out_module.write_text(json.dumps(module_json, indent=2) + "\n")
out_movy.write_text(json.dumps(movy_config, indent=1) + "\n")
out_header.write_text("\n".join(lines) + "\n")
out_pages.write_text(json.dumps(ui_pages, separators=(",", ":")) + "\n")

msize = out_module.stat().st_size
print(f"params: {len(PARAMS)}")
print(f"module.json: {msize} bytes  ({'OK' if msize < 8192 else 'OVER 8KB CAP!'})")
print(f"movy_config.json: {out_movy.stat().st_size} bytes")
print(f"params.h: {out_header.stat().st_size} bytes  (chain fmt {len(chain_json)} bytes)")
assert msize < 8192, "module.json exceeds Schwung's 8 KB loader cap"
