/* Tablor — 2-osc wavetable synth for Ableton Move (Schwung module).
 *
 * plugin_api_v2. Parameter surface + version-tagged state round-trip +
 * the phase 3 engine: 8 voices, poly/mono, glide, mod matrix.
 *
 * Realtime rules (docs/REALTIME_SAFETY.md): render_block never allocates,
 * never touches the filesystem, never logs.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <new>
#include <algorithm>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <atomic>
#include <memory>

#include "../host/plugin_api_v1.h"
#include "params.h"
#include "engine.h"
#include "wt/scanner.h"
#include "wt/loader.h"

static const host_api_v1_t *g_host = nullptr;

/* ------------------------------------------------------------------ */
/* Instance                                                            */
/* ------------------------------------------------------------------ */

struct tablor_instance {
    tb::Engine engine;
    tb::WtScanner scanner;                  /* used for first-run seeding only */
    tb::WtLoader loader { engine };
    char  module_dir[512] = {};

    /* wavetable selection = absolute file paths ("" = built-in Init) */
    char  wt_path[TB_WT_SLOTS][512] = {};

    /* presets: factory (from the module) + UNLIMITED user .tblr files in
     * the user library (filename = preset name, content = TBLR1 blob).
     *
     * The worker rebuilds this list whenever the folder changes while the
     * audio thread reads it in get_param, so it is published as an IMMUTABLE
     * snapshot and swapped atomically — never mutated in place. */
    struct preset { std::string name, blob, path; };   /* path "" = factory */
    struct PresetList { std::vector<preset> items; int factory_count = 0; };
    std::shared_ptr<const PresetList> presets { std::make_shared<PresetList>() };
    std::atomic<int> preset_index { 0 };

    std::shared_ptr<const PresetList> presetList() const
    { return std::atomic_load(&presets); }

    /* state blob scratch */
    char  state_buf[8 * 1024];

    /* ---- Turnable wavetable selection -------------------------------
     * The filepath control (wt1_table/wt2_table) is OPAQUE to the host:
     * it can be opened, never turned, because turning a path would write
     * nonsense into it. So a pot cannot change the wavetable, which is
     * what was reported.
     *
     * These add the turnable half WITHOUT touching the filepath control:
     * a pack chosen from a list, and one enum per oscillator whose
     * options are the entries in that pack. An enum is turnable AND
     * divable, so hold+click opens a scrolling picker for a long pack.
     *
     * Rebuilt only when the scan or the pack changes, on the worker and
     * NEVER on the SPI callback -- neither in get_param (see the note above
     * tb_chain_params_json) nor in set_param, which is the same thread.
     * Published as an immutable snapshot and swapped atomically, exactly
     * like the preset list: the audio thread only ever takes a pointer. */
    struct SelSnapshot {
        std::vector<std::string> packs;    /* "Pack" prefixes, [0] = All    */
        std::vector<std::string> paths;    /* the whole scan, by index      */
        std::vector<int>         filtered; /* indices into paths            */
        std::string chain_params;          /* static head + dynamic options */
        std::string pack_list;             /* wt_pack_list JSON             */
        std::string path_list;             /* wt_paths JSON, index-aligned
                                            * with the wtN_select options  */
    };
    std::shared_ptr<const SelSnapshot> sel { std::make_shared<SelSnapshot>() };
    std::atomic<int> wt_pack { 0 };

    std::shared_ptr<const SelSnapshot> selection() const
    { return std::atomic_load(&sel); }

    /* false until the worker has scanned, loaded presets and published the
     * Init table; the audio path renders silence in the meantime */
    std::atomic<bool> ready { false };

    /* A drawable digest of the table each oscillator actually has loaded, so
     * a UI can show the real waveform instead of a stand-in. Built on the
     * worker when a table lands and published as an immutable string; the
     * audio thread only ever hands out the pointer. */
    std::shared_ptr<const std::string> wt_shape[TB_WT_SLOTS];
    std::shared_ptr<const std::string> shape(int osc) const
    { return std::atomic_load(&wt_shape[osc & 1]); }

    float *params() { return engine.pots; }
};

/* User presets are plain files the user can copy, share and manage:
 *   /data/UserData/UserLibrary/Tablor Presets/<Name>.tblr
 * filename (minus extension) IS the preset name; content is one
 * TBLR1 state blob line. */
static constexpr const char *kPresetDir =
    "/data/UserData/UserLibrary/Tablor Presets";

static std::string sanitize_preset_name(const char *raw)
{
    std::string n;
    for (const char *s = raw; *s && n.size() < 40; s++) {
        char c = *s;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c == ';' ||
            c == '=' || c == '\n' || c == '\r')
            c = ' ';
        n += c;
    }
    while (!n.empty() && (n.front() == ' ' || n.front() == '.')) n.erase(0, 1);
    while (!n.empty() && n.back() == ' ') n.pop_back();
    if (n.empty()) n = "Untitled";
    return n;
}

static std::string preset_file_for(const std::string &name)
{
    return std::string(kPresetDir) + "/" + name + ".tblr";
}

/* A name nobody has yet: "Name", "Name 2", "Name 3", … */
static std::string unique_preset_name(const std::string &want)
{
    std::string name = want;
    for (int i = 2; i < 1000; i++) {
        struct stat st;
        if (::stat(preset_file_for(name).c_str(), &st) != 0)
            return name;
        name = want + " " + std::to_string(i);
    }
    return want;
}

static bool read_blob_file(const char *path, std::string &blob)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[8192];
    bool ok = false;
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        const char *b = line;
        const char *bar = strchr(line, '|');    /* legacy "Name|blob" lines */
        if (bar) b = bar + 1;
        if (!strncmp(b, "TBLR1;", 6) || !strncmp(b, "TBLR2;", 6)) { blob = b; ok = true; }
    }
    fclose(f);
    return ok;
}

static void write_preset_file(const std::string &path, const char *blob)
{
    FILE *f = fopen(path.c_str(), "w");
    if (f) {
        fputs(blob, f);
        fputc('\n', f);
        fclose(f);
    }
}

/* One-time migration of the old fixed-slot files (presets/user/uN.tbl). */
static void migrate_old_user_slots(tablor_instance *inst)
{
    for (int i = 0; i < 8; i++) {
        char old[600];
        snprintf(old, sizeof old, "%s/presets/user/u%d.tbl",
                 inst->module_dir, i + 1);
        FILE *f = fopen(old, "r");
        if (!f) continue;
        char line[8192];
        std::string name = "User " + std::to_string(i + 1), blob;
        if (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *bar = strchr(line, '|');
            const char *b = line;
            if (bar) { *bar = 0; if (line[0]) name = line; b = bar + 1; }
            if (!strncmp(b, "TBLR1;", 6) || !strncmp(b, "TBLR2;", 6)) blob = b;
        }
        fclose(f);
        if (!blob.empty() && blob != "TBLR1;" && blob != "TBLR2;")
            write_preset_file(preset_file_for(unique_preset_name(
                sanitize_preset_name(name.c_str()))), blob.c_str());
        ::remove(old);
    }
}

/* Re-list the user preset folder (sorted by name, case-insensitive).
 * WORKER THREAD ONLY: does directory I/O, then publishes a new snapshot. */
static void scan_user_presets(tablor_instance *inst)
{
    auto cur = inst->presetList();
    auto next = std::make_shared<tablor_instance::PresetList>();
    next->factory_count = cur->factory_count;
    next->items.assign(cur->items.begin(),
                       cur->items.begin() + cur->factory_count);

    std::vector<tablor_instance::preset> user;
    DIR *d = ::opendir(kPresetDir);
    if (d) {
        while (dirent *e = ::readdir(d)) {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcasecmp(dot, ".tblr") != 0) continue;
            std::string path = std::string(kPresetDir) + "/" + e->d_name;
            std::string blob;
            if (!read_blob_file(path.c_str(), blob)) continue;
            user.push_back({ std::string(e->d_name, (size_t) (dot - e->d_name)),
                             blob, path });
        }
        ::closedir(d);
    }
    std::sort(user.begin(), user.end(),
              [](const tablor_instance::preset &a,
                 const tablor_instance::preset &b) {
                  return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
              });
    for (auto &u : user)
        next->items.push_back(std::move(u));

    if (inst->preset_index.load() >= (int) next->items.size())
        inst->preset_index = 0;
    std::atomic_store(&inst->presets, std::shared_ptr<const tablor_instance::PresetList>(next));
}

static void select_preset_by_path(tablor_instance *inst, const std::string &path)
{
    auto ps = inst->presetList();
    for (size_t i = 0; i < ps->items.size(); i++)
        if (ps->items[i].path == path) {
            inst->preset_index = (int) i;
            return;
        }
}

/* WORKER THREAD ONLY. */
static void load_presets(tablor_instance *inst)
{
    auto boot = std::make_shared<tablor_instance::PresetList>();
    char path[600];
    snprintf(path, sizeof path, "%s/presets/factory.tbl", inst->module_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            char *bar = strchr(line, '|');
            if (!bar) continue;
            *bar = 0;
            char *blob = bar + 1;
            blob[strcspn(blob, "\r\n")] = 0;
            boot->items.push_back({ line, blob, "" });
        }
        fclose(f);
    }
    boot->factory_count = (int) boot->items.size();
    std::atomic_store(&inst->presets, std::shared_ptr<const tablor_instance::PresetList>(boot));

    ::mkdir(kPresetDir, 0755);
    migrate_old_user_slots(inst);
    scan_user_presets(inst);
}

static int param_index(const char *key)
{
    for (int i = 0; i < TB_PARAM_COUNT; i++)
        if (!strcmp(tb_params[i].key, key))
            return i;
    return -1;
}

/* Which wavetable slot a param drives: wt1_table -> 0, wt2_table -> 1. */
static int tb_path_slot(int param_idx)
{
    return param_idx == TB_P_WT1_TABLE ? 0 : 1;
}

/* Store a path and post the background load. "" or "Init" = built-in. */
/* The scanner list IS the enum: index 0 = "Init" (built-in), then every
 * file found. Keep the param's index and the loaded path in step. */
static void sync_table_index(tablor_instance *inst, int osc)
{
    auto snap = inst->selection();
    const auto &l = snap->paths;
    int idx = 0;
    for (size_t i = 0; i < l.size(); i++)
        if (l[i] == inst->wt_path[osc]) { idx = (int) i; break; }
    inst->params()[osc == 0 ? TB_P_WT1_TABLE : TB_P_WT2_TABLE] = (float) idx;
}

static void set_table_path(tablor_instance *inst, int osc, const char *path)
{
    if (!strcmp(path, "Init")) path = "";
    snprintf(inst->wt_path[osc], sizeof inst->wt_path[osc], "%s", path);

    tb::WtEntry e;
    e.path = path;
    if (const char *dot = strrchr(path, '.'); dot && !strncasecmp(dot, ".wt", 3)) {
        int fs = atoi(dot + 3);
        /* 32, not 256: the same floor wtBuild accepts. A .wt64 was rejected
         * here, fell through to the WAV reader, failed to parse as WAV and
         * loaded nothing at all. An unusable number leaves the size at 0 and
         * the loader infers it -- the file is still FLAC either way. */
        e.flac = true;
        if (fs >= 32 && fs <= 4096 && (fs & (fs - 1)) == 0)
            e.flacFrameSize = fs;
    }
    inst->loader.requestLoad(osc, e);
    sync_table_index(inst, osc);
}

/* ------------------------------------------------------------------ */
/* Pack list + per-pack enum options                                    */
/*                                                                      */
/* WtEntry.name is already "Pack/Name" or "Name", so the pack list costs */
/* no extra filesystem work -- it is a pass over a list we already hold. */
/* ------------------------------------------------------------------ */

static std::string tb_pack_of(const std::string &name)
{
    size_t slash = name.find('/');
    return slash == std::string::npos ? std::string() : name.substr(0, slash);
}

static std::string tb_leaf_of(const std::string &name)
{
    size_t slash = name.rfind('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

/* JSON-escape into a std::string. The option names are filenames, so a
 * quote or a backslash is unlikely but entirely legal on disk -- and one
 * unescaped byte turns the whole contract into a failed read. */
static void tb_json_append(std::string &out, const std::string &in)
{
    for (char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char) c < 0x20) continue;
        else out += c;
    }
}

/* Everything below runs ON THE WORKER: it walks the scan, copies it and
 * formats ~24 KB of JSON. The audio thread only ever reads the finished
 * snapshot through inst->selection(). */

/* The host's chain-host parser caps an enum at MAX_ENUM_OPTIONS (128).
 * Past that the knob grid still lists them (the JS parses the JSON
 * itself) but CC knob mapping and modulation targets truncate, which
 * fails silently -- so stop at the cap rather than publish a list only
 * half of the host can see. */
static const size_t TB_MAX_ENUM_OPTIONS = 128;

/* Rebuild the whole selection contract off the current scan and pack.
 * Worker thread only. Publishes atomically when done. */
static void tb_publish_selection(tablor_instance *inst)
{
    auto snap = std::make_shared<tablor_instance::SelSnapshot>();
    const auto &l = inst->scanner.list();

    /* the scan, copied — so the audio thread never reads the scanner while
     * a rescan is mutating it */
    snap->paths.reserve(l.size());
    for (const auto &e : l)
        snap->paths.push_back(e.path);

    /* packs: the distinct "Pack/" prefixes, All first */
    snap->packs.push_back("All");
    for (const auto &e : l) {
        std::string pk = tb_pack_of(e.name);
        if (pk.empty()) continue;
        bool seen = false;
        for (size_t i = 1; i < snap->packs.size(); i++)
            if (snap->packs[i] == pk) { seen = true; break; }
        if (!seen) snap->packs.push_back(pk);
    }

    int pack = inst->wt_pack.load(std::memory_order_relaxed);
    if (pack < 0 || pack >= (int) snap->packs.size()) {
        pack = 0;
        inst->wt_pack.store(0, std::memory_order_relaxed);
    }

    /* the entries this pack offers */
    const bool all = (pack <= 0);
    const std::string want = all ? std::string() : snap->packs[(size_t) pack];
    for (size_t i = 0; i < l.size(); i++) {
        if (i == 0) { snap->filtered.push_back(0); continue; }   /* Init */
        if (all || tb_pack_of(l[i].name) == want)
            snap->filtered.push_back((int) i);
        if (snap->filtered.size() >= TB_MAX_ENUM_OPTIONS) break;
    }

    /* wt_pack_list: the items-level rows */
    std::string &pl = snap->pack_list;
    pl = "[";
    for (size_t i = 0; i < snap->packs.size(); i++) {
        char head[32];
        snprintf(head, sizeof head, "%s{\"index\":%d,\"label\":\"",
                 i ? "," : "", (int) i);
        pl += head;
        tb_json_append(pl, snap->packs[i]);
        pl += "\"}";
    }
    pl += "]";

    /* wt_paths: the FILE for each wtN_select index. A browser cannot read a
     * value the device did not push, and nothing pushes when a pot changes a
     * table -- but the select INDEX does push, so with the paths in hand the
     * page can fetch and draw the file itself instead of waiting on us. */
    std::string &pl2 = snap->path_list;
    pl2 = "[";
    for (size_t i = 0; i < snap->filtered.size(); i++) {
        int g = snap->filtered[i];
        if (i) pl2 += ",";
        pl2 += "\"";
        if (g > 0 && g < (int) l.size()) tb_json_append(pl2, l[(size_t) g].path);
        pl2 += "\"";
    }
    pl2 += "]";

    /* chain_params: the static head plus the three dynamic entries */
    std::string &out = snap->chain_params;
    out.assign(tb_chain_params_json);
    if (out.size() < 2 || out.back() != ']')
        return;                              /* not the shape we expect */
    out.pop_back();                          /* drop the closing ] */

    out += ",{\"key\":\"wt_pack\",\"name\":\"WT Pack\",\"type\":\"enum\",\"options\":[";
    for (size_t i = 0; i < snap->packs.size(); i++) {
        if (i) out += ",";
        out += "\""; tb_json_append(out, snap->packs[i]); out += "\"";
    }
    out += "]}";

    for (int osc = 0; osc < 2; osc++) {
        char head[96];
        snprintf(head, sizeof head,
                 ",{\"key\":\"wt%d_select\",\"name\":\"WT%d Table\",\"type\":\"enum\",\"options\":[",
                 osc + 1, osc + 1);
        out += head;
        for (size_t i = 0; i < snap->filtered.size(); i++) {
            int g = snap->filtered[i];
            if (i) out += ",";
            out += "\"";
            tb_json_append(out, g == 0 ? std::string("Init")
                                       : tb_leaf_of(l[(size_t) g].name));
            out += "\"";
        }
        out += "]}";
    }

    /* The shape digests are DECLARED here so they reach a browser at all:
     * schwung-manager only streams values for keys chain_params declares
     * (its standalone getParam answers from that cache and never asks the
     * device), so an undeclared key reads as undefined forever.
     *
     * They are STRINGS -- a shape digest reads "24,64,Wjvx..." and wt_paths
     * is a path list -- and they are READ-ONLY, so that is what they now say.
     * They were declared as an int with min==max==0, which was an invented
     * way to mean "no knob can turn this" from before `access` existed. It
     * described them wrongly twice over: not integers, and an empty numeric
     * range is a defect in anything that really is a number, which is what
     * Schwung's contract checker correctly flagged (issue #2).
     *
     * They remain on no ui_hierarchy page deliberately: they are a data
     * channel for the web panel, not a control. */
    out += ",{\"key\":\"wt1_shape\",\"name\":\"WT1 Shape\",\"type\":\"string\",\"access\":\"read\"}"
           ",{\"key\":\"wt2_shape\",\"name\":\"WT2 Shape\",\"type\":\"string\",\"access\":\"read\"}"
           ",{\"key\":\"wt_paths\",\"name\":\"WT Paths\",\"type\":\"string\",\"access\":\"read\"}";
    /* The PRESET cell: its options ARE the preset list, factory + user, so
     * the page's enum can step and dive through everything. Republished from
     * the worker whenever a save or rename changes the list. */
    {
        auto ps = inst->presetList();
        out += ",{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"enum\","
               "\"automatable\":false,\"options\":[";
        size_t np = ps->items.size();
        if (np > 128) np = 128;          /* the host's MAX_ENUM_OPTIONS */
        for (size_t i = 0; i < np; i++) {
            if (i) out += ",";
            out += "\"";
            tb_json_append(out, ps->items[i].name);
            out += "\"";
        }
        out += "]}";
    }
    out += "]";

    std::atomic_store(&inst->sel,
                      std::shared_ptr<const tablor_instance::SelSnapshot>(snap));
}

/* Where the currently loaded table sits in the filtered list, or 0. */
static int tb_select_index(const tablor_instance::SelSnapshot &snap,
                           const char *path)
{
    for (size_t i = 0; i < snap.filtered.size(); i++) {
        size_t g = (size_t) snap.filtered[i];
        if (g < snap.paths.size() && snap.paths[g] == path)
            return (int) i;
    }
    return 0;
}

/* ---- drawable digest of a loaded wavetable ------------------------------
 * "<frames>,<samples>,<data>" with one character per sample: the amplitude
 * quantised to 64 levels over [-1,1] in a base64-ish alphabet. 24 frames of
 * 64 samples is ~1.5 KB -- enough to draw a waterfall, small enough to sit
 * in a param string, and cheap enough to build on the worker while the table
 * is already in cache. WORKER THREAD ONLY. */
static const char kQ[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_";

static std::string tb_wt_digest(const tb::Wavetable &wt)
{
    const int nf = wt.size();
    if (nf < 1) return std::string();
    const int OF = nf < 24 ? nf : 24;      /* frames we draw   */
    const int OS = 64;                     /* samples per frame */

    std::string out;
    char head[32];
    snprintf(head, sizeof head, "%d,%d,", OF, OS);
    out.reserve((size_t) OF * OS + 16);
    out.assign(head);

    for (int f = 0; f < OF; f++) {
        int src = (OF == 1) ? 0 : (int) ((long) f * (nf - 1) / (OF - 1));
        const tb::FrameTable *ft = wt.frame(src);
        if (!ft || ft->levels.empty()) { out.append(OS, kQ[32]); continue; }
        const tb::MipLevel &lv = ft->levels[0];   /* full band level */
        if (lv.size < 1 || lv.data.empty()) { out.append(OS, kQ[32]); continue; }
        for (int i = 0; i < OS; i++) {
            int j = (int) ((long) i * lv.size / OS);
            if (j >= (int) lv.data.size()) j = (int) lv.data.size() - 1;
            float v = lv.data[(size_t) j];
            if (v < -1.0f) v = -1.0f;
            if (v >  1.0f) v =  1.0f;
            int q = (int) ((v + 1.0f) * 0.5f * 63.0f + 0.5f);
            if (q < 0) q = 0;
            if (q > 63) q = 63;
            out += kQ[q];
        }
    }
    return out;
}

static void tb_publish_shape(tablor_instance *inst, int osc, const tb::Wavetable &wt)
{
    auto s = std::make_shared<const std::string>(tb_wt_digest(wt));
    std::atomic_store(&inst->wt_shape[osc & 1], s);
}

/* Select by index into the published snapshot. */
static void set_table_index(tablor_instance *inst, int osc, int idx)
{
    auto snap = inst->selection();
    if (idx < 0 || idx >= (int) snap->paths.size()) return;
    set_table_path(inst, osc, snap->paths[(size_t) idx].c_str());
}

static float clamp_param(const tb_param_t *p, float v)
{
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    return v;
}

/* Serialize the current sound into inst->state_buf (TBLR1 blob). */
static void build_state_blob(tablor_instance *inst)
{
    /* TBLR2, not TBLR1: v1.0.5 reordered the LFO shape options (the host
     * draws the glyph from the index), so the same lfoN_shape number means a
     * different waveform on either side of that release. The tag is how a
     * loader can tell which order a blob was saved under. */
    int o = snprintf(inst->state_buf, sizeof inst->state_buf, "TBLR2;");
    for (int i = 0; i < TB_PARAM_COUNT; i++) {
        if (tb_params[i].type == TB_PATH) {   /* wavetable: store the path */
            const char *path = inst->wt_path[tb_path_slot(i)];
            if (path[0])
                o += snprintf(inst->state_buf + o,
                              sizeof inst->state_buf - (size_t) o,
                              "%s=%s;", tb_params[i].key, path);
            continue;
        }
        /* preset machinery is not part of a sound */
        if (!strcmp(tb_params[i].key, "preset") ||
            !strncmp(tb_params[i].key, "save_", 5))
            continue;
        float v = inst->params()[i];
        if (v == tb_params[i].def) continue;            /* defaults are implicit */
        o += snprintf(inst->state_buf + o, sizeof inst->state_buf - (size_t) o,
                      "%s=%g;", tb_params[i].key, (double) v);
        if (o >= (int) sizeof inst->state_buf - 64) break;
    }
}

static void reset_to_defaults(tablor_instance *inst)
{
    for (int i = 0; i < TB_PARAM_COUNT; i++)
        if (tb_params[i].type != TB_PATH)
            inst->params()[i] = tb_params[i].def;
    set_table_path(inst, 0, "");
    set_table_path(inst, 1, "");
}

/* Version-tagged blob: "TBLR1;…" or "TBLR2;…". Anything without a known
 * tag is from another life — ignore it entirely rather than half-apply
 * it (the ER-99 total-silence lesson). */
static void apply_state_blob(tablor_instance *inst, const char *val)
{
    bool v1 = strncmp(val, "TBLR1;", 6) == 0;
    if (!v1 && strncmp(val, "TBLR2;", 6) != 0) return;
    const char *p = val + 6;
    char kbuf[64], vbuf[512];
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        const char *semi = strchr(eq, ';');
        if (!semi) semi = eq + strlen(eq);
        size_t kl = (size_t) (eq - p), vl = (size_t) (semi - eq - 1);
        if (kl < sizeof kbuf && vl < sizeof vbuf) {
            memcpy(kbuf, p, kl); kbuf[kl] = 0;
            memcpy(vbuf, eq + 1, vl); vbuf[vl] = 0;
            int idx = param_index(kbuf);
            if (idx >= 0) {
                if (tb_params[idx].type == TB_PATH)
                    set_table_path(inst, tb_path_slot(idx), vbuf);
                else
                    inst->params()[idx] = clamp_param(&tb_params[idx],
                                                      strtof(vbuf, nullptr));
            }
        }
        p = (*semi) ? semi + 1 : semi;
    }
}

/* ------------------------------------------------------------------ */
/* User macros: u{i} pots write through to their selected target;      */
/* picking a new target back-syncs the pot so nothing jumps.           */
/* ------------------------------------------------------------------ */

static int user_macro_target(tablor_instance *inst, int i)
{
    int opt = (int) inst->params()[tb_user_sels[i]];
    if (opt < 0 || opt >= (int) (sizeof tb_user_target_map / sizeof *tb_user_target_map))
        return -1;
    return tb_user_target_map[opt];
}

static void user_macro_apply(tablor_instance *inst, int i)
{
    int t = user_macro_target(inst, i);
    if (t < 0) return;
    const tb_param_t *p = &tb_params[t];
    float v01 = inst->params()[tb_user_pots[i]] / 127.0f;
    float span = p->max - p->min;
    inst->params()[t] = p->min + (float) (int) (v01 * span + 0.5f);
}

static void user_macro_backsync(tablor_instance *inst, int i)
{
    int t = user_macro_target(inst, i);
    if (t < 0) return;
    const tb_param_t *p = &tb_params[t];
    float span = p->max - p->min;
    float v01 = span > 0 ? (inst->params()[t] - p->min) / span : 0.0f;
    inst->params()[tb_user_pots[i]] = (float) (int) (v01 * 127.0f + 0.5f);
}

/* ------------------------------------------------------------------ */
/* set / get                                                           */
/* ------------------------------------------------------------------ */

static void tb_set_param(void *instance, const char *key, const char *val)
{
    auto *inst = (tablor_instance *) instance;
    if (!inst || !key || !val) return;

    /* state restore: host sends "<prefix>:state" resolved to "state" upstream
     * or the raw prefixed key — accept both spellings. */
    const char *k = key;
    const char *colon = strrchr(key, ':');
    if (colon) k = colon + 1;

    if (!strcmp(k, "state")) {
        apply_state_blob(inst, val);
        return;
    }

    if (!strcmp(k, "preset")) {
        /* accept a preset NAME (Movy/web may echo the enum option) or index */
        auto ps = inst->presetList();
        int pi = -1;
        for (size_t i = 0; i < ps->items.size(); i++)
            if (ps->items[i].name == val) { pi = (int) i; break; }
        if (pi < 0) {
            char *end = nullptr;
            long n2 = strtol(val, &end, 10);
            if (end == val) return;
            pi = (int) n2;
        }
        if (pi < 0 || pi >= (int) ps->items.size()) return;
        inst->preset_index = pi;
        /* a preset is a full sound: reset to defaults first so nothing
         * from the previous sound leaks through */
        reset_to_defaults(inst);
        apply_state_blob(inst, ps->items[(size_t) pi].blob.c_str());
        return;
    }

    /* The tap-buttons. A trigger fires on any value that is not its idle
     * first option -- the host sends option 1 through the enum wire, so both
     * the NAME and "1" must fire (MODULES.md, access:"write"). */
    /* Save As: the committed keyboard text IS the new preset's name. */
    if (!strcmp(k, "save_as")) {
        /* A blank commit means "never mind" -- sanitize would helpfully turn
         * it into "Untitled", which is exactly the file spam this replaces. */
        bool any = false;
        for (const char *c = val; *c; c++) if (*c != ' ') { any = true; break; }
        if (!any) return;
        std::string name = sanitize_preset_name(val);
        build_state_blob(inst);
        std::string blob = inst->state_buf;
        /* unique_preset_name stats the filesystem, so it runs on the worker --
         * this is the SPI callback */
        inst->loader.post([inst, name, blob] {
            /* The keyboard prefills the current preset's name, so committing
             * it unchanged means "save over mine" -- the path already exists
             * and is simply rewritten. A new name makes a new file. */
            std::string path = preset_file_for(name);
            write_preset_file(path, blob.c_str());
            scan_user_presets(inst);
            select_preset_by_path(inst, path);
            tb_publish_selection(inst);       /* the PRESET cell's options */
        });
        return;
    }
    if (!strcmp(k, "preset_rnd")) {
        if (!strcmp(val, "-") || !strcmp(val, "0")) return;
        auto ps = inst->presetList();
        int n = (int) ps->items.size();
        if (n < 2) return;
        /* never re-pick the current one: a button that sometimes does
         * nothing feels broken */
        int cur = inst->preset_index.load();
        int pick = rand() % (n - 1);
        if (pick >= cur) pick++;
        char buf2[16];
        snprintf(buf2, sizeof buf2, "%d", pick);
        tb_set_param(inst, "preset", buf2);
        return;
    }

    if (!strcmp(k, "save_preset")) {
        /* Trigger: never stores a value. Destination:
         *   "new"      -> a fresh .tblr file ("Untitled", "Untitled 2", …)
         *   "user:N"   -> overwrite the N-th user preset (0-based)
         *   "1" / "On" -> overwrite the current user preset, else new */
        build_state_blob(inst);
        std::string path;
        auto ps = inst->presetList();
        if (!strcmp(val, "new")) {
            path = preset_file_for(unique_preset_name("Untitled"));
        } else if (!strncmp(val, "user:", 5)) {
            int ui = ps->factory_count + atoi(val + 5);
            if (ui < ps->factory_count || ui >= (int) ps->items.size())
                return;
            path = ps->items[(size_t) ui].path;
        } else if (!strcmp(val, "1") || !strcmp(val, "On") || !strcmp(val, "Save")) {
            if (inst->preset_index.load() >= ps->factory_count)
                path = ps->items[(size_t) inst->preset_index.load()].path;
            else
                path = preset_file_for(unique_preset_name("Untitled"));
        } else {
            return;
        }
        if (path.empty()) return;
        /* file I/O belongs off the audio callback */
        std::string blob = inst->state_buf;
        inst->loader.post([inst, path, blob] {
            write_preset_file(path, blob.c_str());
            scan_user_presets(inst);
            select_preset_by_path(inst, path);
            tb_publish_selection(inst);       /* the PRESET cell's options */
        });
        return;
    }

    if (!strcmp(k, "preset_name")) {
        /* rename the CURRENT user preset = rename its file */
        auto ps = inst->presetList();
        if (inst->preset_index.load() < ps->factory_count) return;
        const auto &cur = ps->items[(size_t) inst->preset_index.load()];
        std::string want = sanitize_preset_name(val);
        if (want == cur.name) return;
        std::string blob = cur.blob, oldPath = cur.path;
        inst->loader.post([inst, want, blob, oldPath] {
            std::string newPath = preset_file_for(unique_preset_name(want));
            write_preset_file(newPath, blob.c_str());
            ::remove(oldPath.c_str());
            scan_user_presets(inst);
            select_preset_by_path(inst, newPath);
            tb_publish_selection(inst);
        });
        return;
    }

    /* Turnable wavetable selection. Indices only: get_param reports an
     * index for these, so the host learns WIRE_INDEX and never sends a
     * name. Nothing here touches the filesystem. */
    if (!strcmp(k, "wt_pack")) {
        auto snap = inst->selection();
        int n = atoi(val);
        if (n < 0) n = 0;
        if (n >= (int) snap->packs.size()) n = (int) snap->packs.size() - 1;
        if (n == inst->wt_pack.load(std::memory_order_relaxed)) return;
        inst->wt_pack.store(n, std::memory_order_relaxed);
        /* Republish: wt1_select / wt2_select now have different options.
         * The host re-reads the contract after an items selection settles
         * (page_controller commitItem -> armContractSettle), which is why
         * the pack is offered as an items level and not as a plain enum
         * knob -- a plain enum commit does NOT arm the re-read.
         *
         * The rebuild itself is ~24 KB of formatting: it goes to the worker,
         * because THIS is the SPI callback. The host waits 500 ms and then
         * wants two agreeing reads (CONTRACT_SETTLE_MS), and is_loading below
         * holds it off meanwhile, so the snapshot is always long since
         * published by the time anyone reads it. */
        inst->loader.post([inst] { tb_publish_selection(inst); });
        return;
    }
    if (!strcmp(k, "wt1_select") || !strcmp(k, "wt2_select")) {
        int osc = (k[2] == '2') ? 1 : 0;
        auto snap = inst->selection();
        int n = atoi(val);
        if (n < 0 || n >= (int) snap->filtered.size()) return;
        set_table_index(inst, osc, snap->filtered[(size_t) n]);
        return;
    }

    int idx = param_index(k);
    if (idx < 0) return;
    const tb_param_t *p = &tb_params[idx];

    if (p->type == TB_PATH) {
        set_table_path(inst, tb_path_slot(idx), val);
        return;
    }

    float v;
    if (p->type == TB_ENUM) {
        /* Accept an option NAME or an integer index — never trust one
         * spelling (the Forge atoi-collapse gotcha). */
        v = -1;
        for (int i = 0; i < p->n_options; i++)
            if (!strcmp(p->options[i], val)) { v = (float) i; break; }
        if (v < 0) {                            /* not a name — try a number */
            char *end = nullptr;
            float n = strtof(val, &end);
            if (end == val) return;             /* unknown name: ignore */
            v = (float) (int) n;
        }
    } else {
        v = strtof(val, nullptr);
    }

    inst->params()[idx] = clamp_param(p, v);

    /* keep the engine's mod-slot cache in step (cheap: 32 reads) */
    if (k[0] == 'm' && k[1] >= '1' && k[1] <= '8')

    /* user macros */
    for (int i = 0; i < TB_USER_MACROS; i++) {
        if (idx == tb_user_pots[i]) { user_macro_apply(inst, i); break; }
        if (idx == tb_user_sels[i]) { user_macro_backsync(inst, i); break; }
    }
}

static int write_str(char *buf, int buf_len, const char *s)
{
    int n = (int) strlen(s);
    if (n >= buf_len) n = buf_len - 1;
    memcpy(buf, s, (size_t) n);
    buf[n] = 0;
    return n;
}

static int tb_get_param(void *instance, const char *key, char *buf, int buf_len)
{
    auto *inst = (tablor_instance *) instance;
    if (!inst || !key || !buf || buf_len <= 1) return -1;

    /* Static string: a copy, nothing more. get_param is serviced from the
     * SPI callback (chain_internal.h) — anything that formats or reads the
     * filesystem here jams the param bus. */
    if (!strcmp(key, "chain_params")) {
        auto snap = inst->selection();
        return write_str(buf, buf_len,
                         snap->chain_params.empty() ? tb_chain_params_json
                                                    : snap->chain_params.c_str());
    }

    /* Turnable wavetable selection. All three are answered from memory --
     * no scan, no allocation beyond the small formatting buffer. */
    if (!strcmp(key, "wt_pack")) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", inst->wt_pack.load(std::memory_order_relaxed));
        return write_str(buf, buf_len, tmp);
    }
    if (!strcmp(key, "wt_paths")) {
        auto snap = inst->selection();
        return write_str(buf, buf_len,
                         snap->path_list.empty() ? "[]" : snap->path_list.c_str());
    }
    if (!strcmp(key, "wt_pack_list")) {
        auto snap = inst->selection();
        return write_str(buf, buf_len,
                         snap->pack_list.empty() ? "[]" : snap->pack_list.c_str());
    }
    if (!strcmp(key, "wt1_select") || !strcmp(key, "wt2_select")) {
        auto snap = inst->selection();
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d",
                 tb_select_index(*snap, inst->wt_path[key[2] == '2' ? 1 : 0]));
        return write_str(buf, buf_len, tmp);
    }

    /* The table each oscillator has loaded, drawable: "frames,samples,data".
     * Served from a string the worker built when the table landed -- no walk
     * of the wavetable here, this is the SPI callback. */
    if (!strcmp(key, "wt1_shape") || !strcmp(key, "wt2_shape")) {
        auto sp = inst->shape(key[2] == '2' ? 1 : 0);
        return write_str(buf, buf_len, sp ? sp->c_str() : "");
    }

    /* Triggers read as their idle option, so the host learns the NAME wire
     * and a page shows a quiet "-" rather than junk. */
    if (!strcmp(key, "save_preset") || !strcmp(key, "preset_rnd"))
        return write_str(buf, buf_len, "-");
    if (!strcmp(key, "save_as")) {
        /* Prefill the keyboard with the name of the preset being edited, so
         * confirm = overwrite. Factory presets prefill blank: they cannot be
         * overwritten, so the user is choosing a NEW name. */
        auto ps = inst->presetList();
        int pi = inst->preset_index.load();
        if (pi >= ps->factory_count && pi < (int) ps->items.size())
            return write_str(buf, buf_len, ps->items[(size_t) pi].name.c_str());
        return write_str(buf, buf_len, "");
    }

    /* The host asks this while a contract re-read is pending and holds off
     * for as long as it says "1" (page_controller isLoadingSays). Ours is
     * true while the worker still owes a scan, a table or a republish. */
    if (!strcmp(key, "is_loading"))
        return write_str(buf, buf_len,
                         (!inst->ready.load(std::memory_order_relaxed) ||
                          inst->loader.busy()) ? "1" : "0");

    /* Schwung 0.12+: the stock hierarchy editor (viz graphics, preset
     * browser, file browser, keyboard) IS Tablor's on-device UI. */
    if (!strcmp(key, "ui_hierarchy"))
        return write_str(buf, buf_len, tb_ui_hierarchy_json);

    /* Shadow UI preset browser convention: preset_count / preset /
     * preset_name (the "No presets" screen reads exactly these). */
    /* "1" while the worker still owes us file work (used by tests and by
     * anything that wants to read back a save it just asked for) */
    if (!strcmp(key, "busy"))
        return write_str(buf, buf_len, inst->loader.busy() ? "1" : "0");

    if (!strcmp(key, "ready"))
        return write_str(buf, buf_len,
                         inst->ready.load(std::memory_order_relaxed) ? "1" : "0");

    if (!strcmp(key, "preset_count")) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", (int) inst->presetList()->items.size());
        return write_str(buf, buf_len, tmp);
    }
    if (!strcmp(key, "preset_factory_count")) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", inst->presetList()->factory_count);
        return write_str(buf, buf_len, tmp);
    }
    if (!strcmp(key, "preset")) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", inst->preset_index.load());
        return write_str(buf, buf_len, tmp);
    }
    if (!strcmp(key, "preset_name")) {
        auto ps = inst->presetList();
        int pi = inst->preset_index.load();
        if (pi < 0 || pi >= (int) ps->items.size())
            return write_str(buf, buf_len, "");
        return write_str(buf, buf_len, ps->items[(size_t) pi].name.c_str());
    }
    if (!strcmp(key, "preset_names")) {
        /* JSON array — Movy's buildPresetParam convention, also used by
         * ui_chain's list overlay and the web panel. */
        int o = 0;
        buf[o++] = '[';
        auto ps = inst->presetList();
        for (size_t i = 0; i < ps->items.size() && o < buf_len - 8; i++) {
            if (i) buf[o++] = ',';
            buf[o++] = '"';
            for (const char *s = ps->items[i].name.c_str();
                 *s && o < buf_len - 8; s++) {
                if (*s == '"' || *s == '\\') buf[o++] = '\\';
                buf[o++] = *s;
            }
            buf[o++] = '"';
        }
        buf[o++] = ']';
        buf[o] = 0;
        return o;
    }

    if (!strcmp(key, "state")) {
        build_state_blob(inst);
        return write_str(buf, buf_len, inst->state_buf);
    }

    int idx = param_index(key);
    if (idx < 0) return -1;
    const tb_param_t *p = &tb_params[idx];

    if (p->type == TB_PATH)
        return write_str(buf, buf_len, inst->wt_path[tb_path_slot(idx)]);

    if (p->type == TB_ENUM) {
        const char *name = p->options[(int) inst->params()[idx]];
        return write_str(buf, buf_len, name ? name : "?");
    }
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%g", (double) inst->params()[idx]);
    return write_str(buf, buf_len, tmp);
}

static int tb_get_error(void *, char *, int) { return 0; }

/* ------------------------------------------------------------------ */
/* MIDI + audio                                                        */
/* ------------------------------------------------------------------ */

static void tb_on_midi(void *instance, const uint8_t *msg, int len, int source)
{
    auto *inst = (tablor_instance *) instance;
    if (!inst || !msg) return;
    inst->engine.onMidi(msg, len);
}

static void tb_render_block(void *instance, int16_t *out_lr, int frames)
{
    auto *inst = (tablor_instance *) instance;
    if (!inst || !inst->ready.load(std::memory_order_relaxed)) {
        memset(out_lr, 0, (size_t) frames * 2 * sizeof(int16_t));
        return;
    }
    if (g_host && g_host->get_bpm)
        inst->engine.setHostBpm(g_host->get_bpm());
    inst->engine.renderBlock(out_lr, frames);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void *tb_create_instance(const char *module_dir, const char *json_defaults)
{
    (void) json_defaults;
    auto *inst = new (std::nothrow) tablor_instance();
    if (!inst) return nullptr;

    if (module_dir)
        snprintf(inst->module_dir, sizeof inst->module_dir, "%s", module_dir);

    /* create_instance runs on the SPI callback too (MODULES.md, "Threading:
     * there is no control thread"). Everything below is file I/O, a directory
     * walk, a ~20 MB first-run copy and an FFT — none of which may happen
     * here. Start the worker and let it do all of it; the engine renders
     * silence until the Init table is published. */
    inst->loader.start();
    inst->loader.post([inst] {
        tb::WtScanner::seedUserFolder(inst->module_dir);   /* first-run copy */
        inst->scanner.scan();                              /* opendir walk   */
        /* Derive the pack list and the published option lists ONCE, off the
         * single scan — so get_param never has to. */
        tb_publish_selection(inst);
        load_presets(inst);                                /* fopen + scan   */
        /* AGAIN, now that the presets exist. The first call publishes the
         * wavetable lists, but the PRESET cell's options ARE the preset list,
         * and load_presets runs after it -- so a single early call left that
         * enum with no options at all: the cell read 0 and stepping it found
         * nothing. */
        tb_publish_selection(inst);
        /* The digest is built from the real samples, on this thread, as each
         * table lands -- including these built-in ones. */
        inst->loader.onTable = [inst](int osc, const tb::Wavetable &wt) {
            tb_publish_shape(inst, osc, wt);
        };
        auto init0 = tb::makeInitTable();                  /* FFT + ~13 MB */
        tb_publish_shape(inst, 0, *init0);
        tb_publish_shape(inst, 1, *init0);
        inst->engine.setTable(0, init0);
        inst->engine.setTable(1, tb::makeInitTable());
        /* a state restore may have landed while we were scanning: re-apply
         * whatever paths it stored, now that the list and loader are live */
        for (int osc = 0; osc < TB_WT_SLOTS; osc++)
            if (inst->wt_path[osc][0]) {
                char keep[512];
                snprintf(keep, sizeof keep, "%s", inst->wt_path[osc]);
                set_table_path(inst, osc, keep);
            }
        inst->ready = true;
        if (g_host && g_host->log) {
            char msg[160];
            snprintf(msg, sizeof msg,
                     "tablor: v" TB_VERSION " ready, %d wavetables, %d packs, %d presets",
                     (int) inst->scanner.list().size(),
                     (int) inst->selection()->packs.size(),
                     (int) inst->presetList()->items.size());
            g_host->log(msg);
        }
    });
    return inst;
}

static void tb_destroy_instance(void *instance)
{
    delete (tablor_instance *) instance;
}

static plugin_api_v2_t g_api = {
    /* .api_version      = */ 2,
    /* .create_instance  = */ tb_create_instance,
    /* .destroy_instance = */ tb_destroy_instance,
    /* .on_midi          = */ tb_on_midi,
    /* .set_param        = */ tb_set_param,
    /* .get_param        = */ tb_get_param,
    /* .get_error        = */ tb_get_error,
    /* .render_block     = */ tb_render_block,
};

extern "C" plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host)
{
    g_host = host;
    return &g_api;
}
