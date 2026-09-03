# Tablor — a wavetable synth for Ableton Move

Analysis and build plan. **No code written yet** — this document is the study.

---

## 0. What I studied

| Source | Where | Licence |
|---|---|---|
| FigBug/Wavetable (the synth to adapt) | `reference/wavetable` | BSD-3 |
| gin (its DSP library, a submodule) | `reference/wavetable/modules/gin` | BSD-3 |
| Schwung host + module API | `~/Developer/schwung-overtake` | — |
| 9W9 / ER-99 (a working Schwung module) | `~/Developer/909-schwung/schwung-er99` | — |
| Move itself | measured live over `ssh move.local` | — |

---

## 1. The original synth, anatomically

Wavetable is far smaller than it looks. The plugin is **3,532 lines**, and the
half that matters is only two files:

- `PluginProcessor.cpp/h` (1,725 lines) — parameter declarations, wavetable
  loading, FX chain, mod-matrix wiring.
- `WavetableVoice.cpp/h` (556 lines) — the entire per-voice signal path.

Everything else is JUCE GUI. The actual DSP lives in **gin**, and `WavetableVoice`
is little more than glue:

```
osc1 (WTVoicedStereoOscillator, ≤8 unison) ┐
osc2 (WTVoicedStereoOscillator, ≤8 unison) ├→ pre-filter bus → Filter → ┐
sub  (StereoOscillator)                    │                            ├→ AnalogADSR → out
noise(StereoOscillator)                    ┘→ post-filter bus ─────────→┘
```

Each of the four sources has its own **pre/post-filter routing switch**
(`filterParams.wt1/wt2/sub/noise`) — that is the "Per-source routing" feature,
and it is one boolean each. Worth keeping; it costs nothing.

The modulation side: a `ModMatrix` with poly and mono sources (3 LFOs, 3 mod
envelopes, filter env, step LFO, velocity, note, pressure, timbre, pitchbend,
128 CCs) feeding any parameter.

### What we cut, and what that removes

| Cut | Removes |
|---|---|
| Gate/gater, chorus, distortion (4 modes), delay, reverb | `FX/` dir, `gin::GateEffect`, `Modulation`, `StereoDelay`, `PlateReverb`, `DeRez2`, `FireAmp`, `GrindAmp` — and ~40% of the parameter surface |
| 32-step sequencer | `gin::StepLFO`, `StepLFOParams`, `modSrcStep`/`modSrcMonoStep` |
| 3 mod envelopes (you asked for VCA + Filter only) | `EnvParams[3]`, `modADSRs[3]` |
| MPE, MTS-ESP microtuning | `MTSClient`, MPE note handling |

That leaves a genuinely small synth — which is the point.

### The one non-obvious cost: memory

`gin::BandLimitedLookupTable::loadFromBuffer` builds a mip pyramid **per
wavetable frame** — one band-limited copy every 6 semitones from note 6.5 to
127, i.e. **21 levels**, each stored at *full* table size with no decimation.

```
21 levels × 2049 floats × 4 B ≈ 168 KB per frame
× 256 frames (a Serum table)  ≈  43 MB per oscillator  →  86 MB for two
```

I measured the device: **1849 MB RAM, ~1220 MB free**. So 86 MB fits and this is
not a blocker — but it is sloppy, and it makes wavetable switching slower than it
needs to be (~5,400 FFTs of size 2048 per load). Fix in §5.

---

## 2. What Schwung gives us

The module ABI is refreshingly plain — `docs/MODULES.md`, `plugin_api_v1.h`:

- A `dsp.so` `dlopen`'d into the Move process, exporting `move_plugin_init_v2`.
- `render_block(void*, int16_t *out_interleaved_lr, int frames)` — **44100 Hz,
  128 frames, stereo interleaved int16**.
- `on_midi`, `set_param(key, val)`, `get_param(key, buf, len)` — all stringly typed.
- `get_param("state")` / `set_param("synth:state")` round-trip → you get module
  presets and per-slot autosave for free, with no extra code.

No JUCE, no plugin framework, no GUI. This is the single most important fact for
the port: **we are not hosting a plugin, we are writing a `.so` that fills a
buffer.**

### Device and build constraints (measured / already paid for)

- aarch64 Cortex-A72, 4 cores, glibc 2.35 max, `libstdc++.so.6.0.29`.
- SPI audio runs FIFO 90 on **core 3**, ~900 µs/frame. Keep compute off core 3;
  **never** do file I/O or logging on the audio path.
- `module.json` is capped at **8 KB** — serve `chain_params` from `get_param()`.
- Build on the VPS in Docker `ubuntu:22.04` (glibc 2.35 matches the device).
  Nothing toolchain-related on the Mac. Link `-static-libstdc++ -static-libgcc`.
- Never `scp` over a live `dsp.so` — copy beside it, then `mv`.

---

## 3. The UI: Schwung's stock knob pages

Tablor declares `ui_hierarchy` and `chain_params`, and Schwung's own param
pages render them -- knob grids, graphics, the file browser, the keyboard.

An earlier draft of this plan targeted a third-party page renderer as well.
That is gone: the stock pages do the job, and there is no second layout to
keep in step with the first.

## 4. Decisions

### 4.1 De-JUCE the DSP rather than build JUCE headless — **recommended**

Wavetable is BSD-3 and gin is BSD-3. The *only* commercial-licence encumbrance
in the whole thing is JUCE. Schwung's ABI wants a plain `.so`, so hosting JUCE
would drag in an enormous dependency to satisfy an interface that is four
function pointers wide — and keep the licence problem.

The coupling is shallow. Counting `juce::` references in the files we need:
`gin_wtoscillators.h` 9, `gin_oscillators.h` 6, `gin_filter.h` 5,
`gin_analogadsr.h` 4, `gin_lfo.h` 12. In practice that is `AudioSampleBuffer`
(→ a plain `float**`), `jlimit`/`jmap` (three lines), `Array` (→ `std::vector`),
`Random`, and `juce::dsp::FFT` (→ a ~100-line radix-2).

**Port surface:**

| Component | Lines | Note |
|---|---|---|
| `gin_wtoscillators` .h/.cpp | 506 | the core |
| `gin_bandlimitedlookuptable` .h/.cpp | 619 | rework while porting (§5) |
| `gin_oscillators.h` | 323 | sub + noise + the unison wrapper |
| `gin_filter` .h/.cpp | 217 | |
| `gin_analogadsr` .h/.cpp | 373 | |
| `gin_lfo` .h/.cpp | ~510 | |
| smoothers, DC blocker, noise, fastmath | ~400 | |
| **Mod matrix** | — | **write fresh, ~200 lines** |

gin's `ModMatrix` (1,028 lines) is welded to `gin::Parameter` and
`juce::AudioProcessor`. Porting it would cost more than replacing it, and you
want a simple fixed-slot matrix anyway.

Total: roughly **3,000 lines ported + ~1,500 lines of new module shell**.

### 4.2 Voice architecture

8 voices, mono/poly switchable, as asked. The risk is unison: 8 voices × 2 osc ×
8 unison = **128 wavetable oscillators**. Default unison to 1, cap it at 4, and
**benchmark before committing** — the ER-99/gearmulator work established that CPU
is the gating risk on this box, and that measuring first saves weeks. A headless
`bench` binary is step 1 of the build plan for exactly this reason.

### 4.3 Parameter style: 0..127 pots

Following ER-99's `gen_params.py` — every continuous control is a 0..127 pot and
the DSP maps it to a musical range internally, so the screen never shows
milliseconds or hertz. A single Python generator emits `chain_params`,
the page layout and the C header from one source of truth.

---

## 5. Wavetables — the feature that has to be excellent

### 5.1 Formats — one code path, and format support is not the memory problem

Worth separating two things that look related but aren't:

**Memory has nothing to do with which synth exported the file.** A "Serum
wavetable" is a plain WAV holding N frames of 2048 samples. Dropping Serum
support would not save a single byte — it would only mean *guessing* the frame
size instead of reading it from the `clm ` chunk. What costs memory is
`frames × mip levels`, and that is fixed in §5.3 regardless of provenance.

So the simplification worth making is **not** dropping formats — it is having
exactly one internal representation and one loader:

> Everything on disk is normalised at load time to **N frames × 2048 samples,
> 44100 Hz, DC-removed, peak-normalised.** The rest of the engine knows nothing
> about where a table came from.

Layered frame-size detection, in order, all feeding that one path:

1. `clm ` RIFF chunk if present (Serum, and everything that copied Serum) — ~30
   lines of chunk walking, zero bytes of runtime cost.
2. Otherwise divisibility: 2048 → 1024 → 512 → 256, the same heuristic your OXI
   Coral converter already uses and which has been exercised on this library.
3. Otherwise a user-set frame size, defaulting to 2048.
4. Resample to 2048 / 44100 if needed.

I surveyed `~/Music/WaveTables`: **3,410 files, 100% `.wav`**. Serum, Vital and
Ableton all export plain WAV frame-stacks, so steps 1–3 *are* Serum/Vital/Ableton
support, and they cost one small function.

**Dropped:** the `.vitaltable` JSON parser. That one is a genuinely separate
parser (JSON + base64) for a format you own zero files in. Not worth the
maintenance. If a `.vitaltable` ever matters, Vital exports WAV.

### 5.2 Selection — the "on the fly" requirement

This is the headline feature, so it gets the design attention:

- On load, scan the wavetable directories and build a **name-indexed list**.
- Expose `wt1_table` / `wt2_table` as **enum params** whose `options` are those
  names, served through `get_param("chain_params")` (which is exactly why
  `chain_params` must be dynamic and not stuck in the 8 KB `module.json`).
- Turning the encoder steps tables; **touching it opens the full-screen
  scrollable list**. That is one gesture to browse hundreds of tables.
- Add `knobAcceleration: 'wide'` so a slow turn is single-step and a fast sweep
  travels.
- A `type: 'file'` browser lives on a secondary row for loading from anywhere.

**Directories scanned:** the module's own `wavetables/` (factory packs) plus
`/data/UserData/UserLibrary/Wavetables/` — Gus's choice; the module creates it
on first run, and both Move Manager and schwung-manager's file browser can drop
files there. A `Rescan Tables` trigger on the Global page re-reads it without a
reboot, and selections are stored by *name* so they survive the list shifting.

### 5.3 Factory tables — two of the seven packs are shippable

You said you don't need them but would take them if usable. I checked the
licensing on all seven packs the original bundles:

| Pack | Tables | Status |
|---|---|---|
| **Adventure Kid** | 65 | AKWF/AKWP by Kristoffer Ekstrand — **public domain**. Ship it. |
| **Neu KatalYst** | 50 | Bundled readme: *"feel free to use them in all your other synths… no credit necessary."* **Ship it.** |
| Analog, Distorted, FM, Growl, Hyper | 100 | No stated licence anywhere in the repo. **Skip.** |

So we can ship **115 tables, legitimately, for ~8 MB** — because FigBug also
bundles them FLAC-compressed as `.wt2048` (extension encodes the frame size;
Adventure Kid is 4.2 MB for 65 tables vs 278 MB for the uncompressed set).

That costs one dependency: a FLAC decoder. `dr_flac.h` is a single public-domain
header, so this is cheap — and it means the shipped module stays small while
your own library supplies the rest.

### 5.4 The memory fix, with real numbers

gin stores all 21 mip levels at **full 2048 samples**, which is simply wasteful:
the level that serves note 126 only needs to carry one harmonic. Decimating each
level to the smallest power of two that holds its harmonics — the band-limited
spectrum is already zeroed above its cutoff, so this is a *smaller* inverse FFT,
not extra work — gives:

```
level sizes (notesPerTable = 6, 21 levels):
2048 ×4, 1024 ×2, 512 ×2, 256 ×2, 128 ×2, 64 ×2, 32 ×7
= 12,384 samples/frame   vs   43,008 undecimated
```

| Config | Per osc, 256-frame table |
|---|---|
| gin as-is | **44.1 MB** |
| decimated mips (recommended) | **12.7 MB** |
| decimated + `notesPerTable = 12` | 6.3 MB — but audibly duller at the bottom of each octave; not worth it |
| + shared table cache when both oscs use the same table | 12.7 MB total instead of 25.4 |

**12.7 MB is the target.** It also cuts the FFT work ~3.5×, so a table switch
lands in tens of ms instead of hundreds. That is the whole fix, and it is the
same code we are writing anyway.

**Hard budget as the backstop.** Decimation handles normal tables; it does not
protect against something pathological (a 1024-frame table at 4096 samples would
still be ~100 MB). So the loader enforces a **24 MB per-oscillator ceiling** and
subsamples frames to fit if a table exceeds it. Bounded worst case, no
user-visible knob, and normal tables never touch it.

### 5.5 Making the switch instant and glitch-free

Non-negotiable, because loading a table means FFTs and allocation, and the audio
path allows neither:

- A **background loader thread** builds the new table set.
- **Double-buffer + atomic pointer swap**; voices hold a reference so an
  in-flight note never reads a freed table.
- The engine never allocates or touches the filesystem in `render_block`.

---

## 6. The pages, concretely

Six banks. Your five, plus a Global bank the synth needs (mono/poly lives
somewhere). Page 1 is exactly what you specified; the extras sit one jog-step
behind it rather than crowding it.

**Bank 1 — OSC**
- Row 1 *(your page 1, verbatim)*: `WT1 Table` · `WT2 Table` · `WT1 Pos` · `WT2 Pos` · `WT1 Level` · `WT2 Level` · `WT1 Tune` · `WT2 Tune`
- Row 2: `WT1 Uni` · `WT1 Detune` · `WT1 Spread` · `WT1 Pan` · then the same four for WT2
- Row 3: `WT1 Bend` · `WT1 Formant` · `WT1 Fine` · `WT1 Retrig` · then the same four for WT2

**Bank 2 — FILTER** (draws a live filter curve)
- Row 1: `Freq` · `Res` · `EG Amt` · `Type` · `Sub Level` · `Sub Wave` · `Noise Level` · `Noise Type`
- Row 2: `Key Trk` · `Vel Trk` · `Sub Tune` · `Sub Pan` · `Noise Pan` · `→WT1` · `→WT2` · `→Sub/Noise` (the pre/post routing switches)

**Bank 3 — ENV** (draws two envelope graphics)
- Row 1: `VCA A` · `VCA D` · `VCA S` · `VCA R` · `Flt A` · `Flt D` · `Flt S` · `Flt R`
- Row 2: `VCA Vel` · `VCA Retrig` · `Flt Retrig` · —

**Bank 4 — LFO** (one page each, live waveform preview)
- Row 1 = LFO1, Row 2 = LFO2, Row 3 = LFO3, each: `Shape` · `Rate` · `Sync` · `Beat` · `Depth` · `Phase` · `Offset` · `Retrig`

**Bank 5 — MOD** — 8 slots, 2 per row, 4 knobs each: `Src` · `Dst` · `Amount` · `On`.
Src and Dst are enums, so touching either opens the scrollable list.
- Sources: LFO 1–3, Filter EG, VCA EG, Velocity, Note, Modwheel, Aftertouch, Pitchbend, Random
- Destinations: ~20 — both WT positions/levels/tunes/bend/formant, filter freq/res, sub & noise level, pan, amp, LFO rates

**Bank 6 — GLOBAL** (`global: true`)
- `Poly/Mono` · `Voices` · `Glide` · `Glide Mode` · `Legato` · `PB Range` · `Level` · `Rescan Tables`

---

## 7. Risks, ranked

1. **CPU under unison.** The one thing that has killed a Move port before.
   Decision taken: **build it, measure it in phase 1, drop it if it isn't good.**
   To keep that a cheap decision rather than a refactor, unison is confined to
   one wrapper class (`VoicedStereoOscillator`) — dropping it means instantiating
   a bare `WTOscillator` per oscillator and deleting four params per osc from the
   config. No engine surgery. Default unison 1 either way.
2. **Wavetable switch glitching.** Mitigation: background loader + atomic swap,
   designed in from the start rather than retrofitted (§5.4).
4. **Ableton format claim unverified** (§5). Low stakes now that everything runs
   through one WAV path, but I'd still like one real file to confirm.

---

## 8. Build plan

Eight phases. Each ends in something **verified on the actual Move**, not on a
desktop. No phase starts before the previous one's gate passes. The order is
deliberate: the two things that can kill the project (CPU, and porting the DSP
wrong) are settled in phases 1 and 2, before any UI or feature work is built on
top of them.

### Repo layout (established in phase 0)

```
schwung-tablor/
├── src/
│   ├── module.json              # < 8 KB — generated
│   ├── dsp/
│   │   ├── tablor_plugin.cpp    # plugin_api_v2 entry point
│   │   ├── engine.{h,cpp}       # voice allocator, poly/mono, glide
│   │   ├── voice.{h,cpp}        # the per-voice signal path
│   │   ├── modmatrix.{h,cpp}    # written fresh, ~200 lines
│   │   ├── params.h             # generated
│   │   └── wt/                  # scanner, loaders, background swap
│   ├── ported/                  # de-JUCE'd gin — kept separate & attributed
│   │   ├── wt_oscillator.{h,cpp}
│   │   ├── bllt.{h,cpp}         # band-limited tables, decimated
│   │   ├── oscillators.h        # sub + noise
│   │   ├── filter.{h,cpp}  adsr.{h,cpp}  lfo.{h,cpp}
│   │   └── fft.h  smoothers.h  dcblocker.h  fastmath.h
│   ├── host/plugin_api_v1.h     # copied from Schwung
│   └── wavetables/              # AKWF + Neu KatalYst, .wt2048 FLAC
├── tools/
│   ├── bench.cpp                # phase 1 — the go/no-go
│   ├── render.cpp               # offline render for regression tests
│   └── gen_params.py            # single source of truth
├── tests/fixtures/              # golden WAVs from the JUCE original
├── scripts/{Dockerfile,build.sh,deploy.sh}
└── cmake/aarch64-toolchain.cmake
```

`gen_params.py` is the single source of truth (the ER-99 pattern): it emits
`params.h`, `module.json`, the `chain_params` JSON string, and
the page layout. A parameter is added in exactly one place.

### The phases

**Phase 0 — Scaffold and the build loop.**
`git init`. CMake + `aarch64-toolchain.cmake` + `Dockerfile` (ubuntu:22.04,
glibc 2.35), `-march=armv8-a -mtune=cortex-a72 -static-libstdc++ -static-libgcc`.
`build.sh` rsyncs to the VPS and builds in Docker; `deploy.sh` relays the
artifact to the Move and **`scp` beside, then `mv`** — never over a live
`dsp.so`. Ship a `dsp.so` that implements `plugin_api_v2` and renders silence.
→ **Gate:** module appears in Schwung, loads, renders silence, doesn't crash
MoveOriginal. The whole edit→device loop works and is timed.

**Phase 1 — Benchmark harness. The go/no-go.**
Port *only* `BandLimitedLookupTable` (with decimation), `WTOscillator`, a
minimal WAV loader, and a small radix-2 FFT. Build `tools/bench.cpp` as a
headless aarch64 binary, run on-device under `taskset 0x7`.
Measure: realtime factor at 8 voices × 2 osc × unison {1, 2, 4, 8}; wavetable
load time; RSS for a 256-frame table (verify the 12.7 MB prediction).
→ **Gate:** a number, not an opinion. It sets the unison cap — or removes unison
(§7.1). Nothing else is built until this passes. This is the step the
gearmulator work proved you cannot skip.

**Phase 2 — De-JUCE the remaining DSP, against golden references.**
Port filter, `AnalogADSR`, sub/noise oscillators, LFO, smoothers, DC blocker,
fastmath. Validate rather than hope: `plugin/Source/App.cpp` already contains a
headless render skeleton behind `#if 0`, so building the **original** once on
the VPS and dumping reference WAVs (osc sweeps across the note range, filter
sweeps per type/slope, envelope shapes) is cheap. Commit those as fixtures;
`tools/render.cpp` diffs the port against them.
→ **Gate:** every ported block matches the JUCE original within tolerance.
*Risk:* building JUCE on Linux needs ALSA/X11/freetype dev headers — routine but
a real one-time cost. If it turns painful, fall back to gin's own `*.test.h`
unit tests plus spectral comparison, and say so rather than skipping validation.

**Phase 3 — Voice and engine.**
Assemble the voice (2 WT osc → pre/post-filter routing → filter → VCA), the
8-voice allocator with poly/mono, glide, legato and voice stealing, MIDI
handling (note, pitchbend, modwheel, aftertouch, CC), and the fresh mod matrix
(8 slots, poly and mono sources).
→ **Gate:** plays polyphonically on the device through Schwung's own UI, with
sensible defaults, no clicks on note steal.

**Phase 4 — Wavetable subsystem.**
Scanner over the module's `wavetables/` and the Move Manager user directory;
frame-size detection (`clm ` → divisibility → default); `dr_flac` for `.wt2048`;
resample to 2048/44100; decimated mip build; 24 MB ceiling; shared table cache;
background loader thread with double-buffer and atomic swap.
→ **Gate:** switching tables *while a note is held* is silent; switch latency and
RSS measured on-device against phase 1's predictions.

**Phase 6 — State, presets, defaults.**
`get_param("state")` / `set_param("synth:state")` round-trip, **version-tagged**
(an untagged blob read under a new layout once produced total silence on ER-99).
A starter patch bank that shows off the wavetables.
→ **Gate:** presets save and recall; a deliberately corrupted or stale blob
degrades to defaults instead of silence.

**Phase 7 — Ship.**
`help.json`, README with attribution (BSD-3 for Wavetable and gin, wavetable
pack credits), `release.json`, GitHub Actions release workflow, module-catalog
entry.
→ **Gate:** installable from the module store on a clean device.

### Sequencing notes

- Phases 0–2 are prerequisites for everything. Phases 4 and 5 could overlap
  once 3 lands, but 5 depends on 4 for the table-name list.
- Every phase ends on hardware. "Works on the VPS" is not a gate.
- Phase 1's result is allowed to change the design. That is its purpose.

---

## 9. Decisions taken, and the one thing still open

**Settled:**

- **Memory** — decimated mips, 12.7 MB/osc, 24 MB hard ceiling, shared table
  cache. Format support is not what costs memory, so nothing is dropped for that
  reason. (§5.4)
- **Formats** — one internal representation (N × 2048 @ 44100). WAV + `clm `
  chunk covers Serum/Vital/Ableton. `.vitaltable` JSON parser dropped. (§5.1)
- **Unison** — built, measured in phase 1, dropped if it doesn't earn its keep.
  Confined to one wrapper class so dropping it is a deletion, not a refactor.
  (§7.1)
- **Factory tables** — ship Adventure Kid (public domain) + Neu KatalYst
  (explicitly free): 115 tables, ~8 MB via FLAC. Skip the five unlicensed packs.
  (§5.3)

**Still open:** one Ableton-exported wavetable file, to confirm the WAV path
handles it. Not a blocker — phase 0 and 1 don't touch it.
