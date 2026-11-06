/* Tera raid editor (a mod: nxrecomp/runtime/nxmod.h, ImGui through nxrecomp/modkit). F6 shows it.
 *
 * Raid dens live in the save's raid blocks, which the game keeps on its heap (PKHeX's RaidSpawnList9: Paldea
 * KTeraRaidPaldea 0xCAAC8800 = {u64 current seed, u64 tomorrow seed} + 72 dens; KTeraRaidDLC 0x100B93DA = 100
 * Kitakami dens + 100 Blueberry slots, 80 used). A den is 0x20 bytes of u32: enabled, area, lottery group, spawn
 * point, seed, 0, content (0 standard 1-5 stars, 1 black 6 stars, 2 event, 3 7-star event), league points
 * claimed. The blocks are found by scanning the heap for runs of entries of that shape (no pointer chains: they
 * change with every game version), read and written through Read/WriteProcessMemory (a page the game frees
 * fails the call instead of faulting).
 *
 * A den's raid follows from its 32-bit seed (behaviour as PKHeX's Tera9RNG / Encounter9RNG describe it, our own
 * implementation): Xoroshiro128+ (s0 = seed, s1 = 0x82A2B175229D6A5B); standard dens roll the star level
 * (NextInt(100) against the story progress' thresholds), black dens are 6 stars with no roll; then NextInt(total)
 * over that star table's rates for this game picks the species. From a fresh generator: EC, a fake trainer id and
 * the PID (shiny when their xor is below 16), the flawless IVs' positions then the other IVs, ability, gender,
 * nature. The Tera type comes from another fresh generator (NextInt(18), or NextInt(2) between the species' two
 * types). The tables come from the game's files (raiddata.py -> <build>\mods\raids.dat).
 *
 * The finder searches seeds for a species (and shiny / Tera type) and writes them into dens. The world's crystals
 * show a den's new raid once the area reloads. F7 opens the item giver (items.cpp) in the same DLL. */
#include "items.h"
#include "modkit.h"
#include <algorithm>
#include <atomic>
#include <ctype.h>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* ---------------------------------------------------------------------------------------------- data */
#pragma pack(push, 1)
struct Raid {                               /* raids.dat's 38-byte entry (raiddata.py) */
    u8 region, stars, rom, rate;
    u16 national, form;
    u8 level, gem, ability_kind, flawless;
    u8 shiny_kind, nature, type1, type2;
    u8 gender_group, gender_ratio, pad0, pad1;
    u16 abilities[3];
    u16 moves[4];
    u16 dev, pad2;
};
#pragma pack(pop)
static_assert(sizeof(Raid) == 38, "raids.dat entry");

enum { R_PALDEA, R_KITAKAMI, R_BLUEBERRY, N_REGIONS };
static const char *const region_names[N_REGIONS] = {"Paldea", "Kitakami", "Blueberry"};
static const u32 region_dens[N_REGIONS] = {72, 100, 80};

static struct {
    bool loaded;
    std::string error;
    std::vector<std::string> species, moves, abilities, natures, types;
    std::vector<Raid> raids;
    std::vector<u32> table[N_REGIONS][7];   /* indices into raids, per region and stars, in file order */
} D;

static u32 g_rom = 2;                       /* 1 Scarlet, 2 Violet */

static const char *name_of(const std::vector<std::string> &v, u32 i) { return i < v.size() ? v[i].c_str() : "?"; }

static void load_data(const char *cache_dir) {
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\mods\\raids.dat", cache_dir);
    FILE *f = fopen(path, "rb");
    if (!f) { D.error = std::string("no ") + path + ": run violetrecomp\\mods\\raids\\raiddata.py"; return; }
    std::vector<u8> b;
    fseek(f, 0, SEEK_END);
    b.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t got = fread(b.data(), 1, b.size(), f);
    fclose(f);
    size_t p = 8;
    auto u32at = [&](size_t o) { u32 v = 0; if (o + 4 <= b.size()) memcpy(&v, &b[o], 4); return v; };
    if (got != b.size() || b.size() < 8 || memcmp(b.data(), "NXRD", 4) || u32at(4) != 1) {
        D.error = std::string(path) + " is not raid data version 1: run raiddata.py again";
        return;
    }
    std::vector<std::string> *lists[] = {&D.species, &D.moves, &D.abilities, &D.natures, &D.types};
    for (auto *l : lists) {
        u32 n = u32at(p);
        p += 4;
        for (u32 i = 0; i < n && p + 2 <= b.size(); i++) {
            u16 len = (u16)(b[p] | b[p + 1] << 8);
            p += 2;
            l->emplace_back((const char *)&b[p], (size_t)len);
            p += len;
        }
    }
    u32 n = u32at(p);
    p += 4;
    if (p + (size_t)n * sizeof(Raid) > b.size()) { D.error = "raids.dat is cut short"; return; }
    D.raids.resize(n);
    memcpy(D.raids.data(), &b[p], (size_t)n * sizeof(Raid));
    for (u32 i = 0; i < n; i++)
        if (D.raids[i].region < N_REGIONS && D.raids[i].stars <= 6) D.table[D.raids[i].region][D.raids[i].stars].push_back(i);
    D.loaded = true;
}

/* ---------------------------------------------------------------------------------------------- the raid RNG */
struct Xoro {
    u64 s0, s1;
    explicit Xoro(u64 seed) : s0(seed), s1(0x82A2B175229D6A5Bull) {}
    static u64 rotl(u64 x, int k) { return x << k | x >> (64 - k); }
    u64 next() {
        u64 a = s0, b = s1, r = a + b;
        b ^= a;
        s0 = rotl(a, 24) ^ b ^ (b << 16);
        s1 = rotl(b, 37);
        return r;
    }
    u32 next_int(u64 max = 0xffffffffull) {      /* rejection over the smallest all-ones mask covering max - 1 */
        u64 m = max - 1;
        m |= m >> 1, m |= m >> 2, m |= m >> 4, m |= m >> 8, m |= m >> 16, m |= m >> 32;
        u64 r;
        do r = next() & m; while (r >= max);
        return (u32)r;
    }
};

/* story progress (0: start .. 4: 6-star raids unlocked) and the star roll (PKHeX Tera9RNG's ranges) */
static const char *const progress_names[] = {"Start (1-2 stars)", "3 stars unlocked", "4 stars unlocked",
                                             "5 stars unlocked", "6 stars unlocked (postgame)"};
static int stars_of_roll(u32 r, int progress) {
    switch (progress) {
    case 0: return r <= 80 ? 1 : 2;
    case 1: return r <= 30 ? 1 : r <= 70 ? 2 : 3;
    case 2: return r <= 20 ? 1 : r <= 40 ? 2 : r <= 70 ? 3 : 4;
    case 3: return r <= 40 ? 3 : r <= 75 ? 4 : 5;
    default: return r <= 30 ? 3 : r <= 70 ? 4 : 5;
    }
}

struct Result {
    const Raid *raid;                       /* NULL: content not covered (events) or no table */
    int stars, tera, ability, nature, gender;   /* gender 0 male, 1 female, 2 none */
    bool shiny;
    int ivs[6];                             /* HP Atk Def SpA SpD Spe */
    u32 ec, pid;
};

static const u8 amped[] = {0, 2, 3, 4, 6, 8, 9, 11, 13, 14, 19, 22, 24};
static const u8 lowkey[] = {1, 5, 7, 10, 12, 15, 16, 17, 18, 20, 21, 23};

/* the raid only (stars, species): the finder's fast path */
static const Raid *pick(u32 seed, int region, u32 content, int progress, int *stars_out, Xoro *rest = NULL) {
    if (region < 0 || region >= N_REGIONS || content > 1) return NULL;
    Xoro r(seed);
    int stars = content == 1 ? 6 : stars_of_roll(r.next_int(100), progress);
    const std::vector<u32> &t = D.table[region][stars];
    u32 total = 0;
    for (u32 i : t) if (D.raids[i].rom == 0 || D.raids[i].rom == g_rom) total += D.raids[i].rate;
    if (!total) return NULL;
    u32 roll = r.next_int(total), acc = 0;
    *stars_out = stars;
    for (u32 i : t) {
        const Raid &e = D.raids[i];
        if (e.rom != 0 && e.rom != g_rom) continue;
        acc += e.rate;
        if (roll < acc) return &e;
    }
    return NULL;
}

static int tera_of(u32 seed, const Raid &e) {
    if (e.gem >= 2) return e.gem - 2;
    Xoro r(seed);
    if (e.gem == 1) return (int)r.next_int(18);
    return r.next_int(2) == 0 ? e.type1 : e.type2;
}

static bool shiny_of(u32 seed, const Raid &e) {
    if (e.shiny_kind == 1) return false;
    if (e.shiny_kind == 2) return true;
    Xoro r(seed);
    r.next_int();                           /* EC */
    u32 tid = r.next_int(), pid = r.next_int();
    return ((pid >> 16) ^ (pid & 0xffff) ^ (tid >> 16) ^ (tid & 0xffff)) < 16;
}

static Result generate(u32 seed, int region, u32 content, int progress) {
    Result o = {};
    o.raid = pick(seed, region, content, progress, &o.stars);
    if (!o.raid) return o;
    const Raid &e = *o.raid;
    o.tera = tera_of(seed, e);
    Xoro r(seed);
    o.ec = r.next_int();
    u32 tid = r.next_int();
    o.pid = r.next_int();
    u32 x = (o.pid >> 16) ^ (o.pid & 0xffff) ^ (tid >> 16) ^ (tid & 0xffff);
    o.shiny = e.shiny_kind == 2 || (e.shiny_kind == 0 && x < 16);
    for (int &v : o.ivs) v = -1;
    for (int i = 0; i < e.flawless && i < 6; i++) {
        u32 k;
        do k = r.next_int(6); while (o.ivs[k] != -1);
        o.ivs[k] = 31;
    }
    for (int &v : o.ivs) if (v == -1) v = (int)r.next_int(32);
    o.ability = e.ability_kind == 0 ? (int)r.next_int(2) : e.ability_kind == 1 ? (int)r.next_int(3) : e.ability_kind - 2;
    if (e.gender_group == 1) o.gender = 0;
    else if (e.gender_group == 2) o.gender = 1;
    else if (e.gender_group == 3) o.gender = 2;
    else o.gender = (int)r.next_int(100) < e.gender_ratio ? 1 : 0;
    if (e.nature) o.nature = e.nature - 1;
    else if (e.national == 849) o.nature = e.form ? lowkey[r.next_int(sizeof lowkey)] : amped[r.next_int(sizeof amped)];
    else o.nature = (int)r.next_int(25);
    return o;
}

/* ---------------------------------------------------------------------------------------------- dens in memory */
struct Den { u32 enabled, area, group, point, seed, unused, content, claimed; };
static_assert(sizeof(Den) == 0x20, "den");

static bool den_shape(const u32 *w) {
    return w[0] <= 1 && w[1] >= 1 && w[1] <= 64 && w[2] < 256 && w[3] < 4096 && w[5] == 0 && w[6] <= 3 && w[7] <= 1;
}

/* a den with a raid: memory of zeros with a 1 every 0x20 bytes has the shape too (seed 0: every "den" the same raid) */
static bool den_live(const u32 *w) { return den_shape(w) && w[4] != 0; }

/* a slot of a table: a den, or unused (all zero: Paldea had 69 dens of 72 early in the story) */
static bool den_slot(const u32 *w) {
    return den_shape(w) || !(w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7]);
}

static bool mem_read(u64 addr, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, out, n, &got) && got == n;
}

static bool mem_write(u64 addr, const void *in, size_t n) {
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)addr, in, n, &put) && put == n;
}

static std::mutex g_blocks_lock;
static std::vector<u64> g_blocks[N_REGIONS];    /* where each region's dens start (copies: all written) */
static std::atomic<bool> g_scanning;
static std::atomic<u64> g_scan_done, g_scan_total;
static std::string g_scan_msg;

enum { MIN_LIVE = 20, MAX_GAP = 8 };
struct Run { u64 start; u32 live, gap; std::vector<u32> seeds; };

/* a run ends: kept when long enough and its seeds mostly distinct (real dens' are random) */
static void run_end(Run &r, std::vector<std::pair<u64, u32>> &runs) {
    if (r.live >= MIN_LIVE) {
        std::sort(r.seeds.begin(), r.seeds.end());
        size_t distinct = std::unique(r.seeds.begin(), r.seeds.end()) - r.seeds.begin();
        if (distinct * 10 >= (size_t)r.live * 9) runs.push_back({r.start, r.live});
    }
    r.live = r.gap = 0;
}

/* runs of dens in guest memory (live dens, up to MAX_GAP unused slots between them; mostly distinct seeds), classed by
 * live count: up to 72 Paldea, more Kitakami (+ Blueberry 100 slots on when over 100). Violet keeps
 * its data in the alias region (svcMapPhysicalMemory, its own allocator), not the svcSetHeapSize heap: both are
 * scanned, contiguous (alias then heap) */
static void scan_thread() {
    std::vector<u64> found[N_REGIONS];
    std::vector<std::pair<u64, u32>> runs;
    const u64 lo = NXMOD_ALIAS_BASE, hi = NXMOD_HEAP_BASE + NXMOD_HEAP_MAX;
    static_assert(NXMOD_ALIAS_BASE + NXMOD_ALIAS_SIZE == NXMOD_HEAP_BASE, "alias region ends where the heap starts");
    u64 total = 0;
    for (u64 a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi)) break;
        u64 end = (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT) total += (end < hi ? end : hi) - a;
        a = end;
    }
    g_scan_total = total ? total : 1;
    g_scan_done = 0;
    const size_t CH = 16 << 20;
    std::vector<u8> buf(CH + 64);
    for (u64 a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi)) break;
        u64 rend = (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (rend > hi) rend = hi;
        bool rw = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                  (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE));
        if (rw) {
            Run run[8] = {};
            for (u64 c = a; c < rend; c += CH) {
                size_t n = (size_t)(rend - c < CH + 32 ? rend - c : CH + 32);
                if (!mem_read(c, buf.data(), n)) {          /* freed under us: end the runs here */
                    for (int p = 0; p < 8; p++) run_end(run[p], runs);
                    g_scan_done += n < CH ? n : CH;
                    continue;
                }
                size_t words = (n < CH ? n : CH) / 4;
                const u32 *w = (const u32 *)buf.data();
                size_t avail = n / 4;
                for (size_t i = 0; i < words; i++) {
                    Run &r = run[i & 7];        /* c is page-aligned: the word index's low bits are the phase */
                    if (i + 8 > avail) { run_end(r, runs); continue; }
                    if (den_live(w + i)) {
                        if (!r.live) { r.start = c + i * 4; r.seeds.clear(); }
                        r.live++;
                        r.gap = 0;
                        r.seeds.push_back(w[i + 4]);
                    } else if (r.live && den_slot(w + i) && r.gap < MAX_GAP) {
                        r.gap++;
                    } else run_end(r, runs);
                }
                g_scan_done += words * 4;
            }
            for (int p = 0; p < 8; p++) run_end(run[p], runs);
        }
        a = rend;
    }
    for (auto &r : runs) {
        if (r.second <= 72) found[R_PALDEA].push_back(r.first);
        else {
            found[R_KITAKAMI].push_back(r.first);
            if (r.second > 100) found[R_BLUEBERRY].push_back(r.first + 100 * 0x20);
        }
    }
    char msg[256];
    snprintf(msg, sizeof msg, "Found: Paldea %zu, Kitakami %zu, Blueberry %zu (%zu runs, %.1f GB scanned)",
             found[0].size(), found[1].size(), found[2].size(), runs.size(), (double)g_scan_done / (1 << 30));
    {
        std::lock_guard<std::mutex> l(g_blocks_lock);
        for (int i = 0; i < N_REGIONS; i++) g_blocks[i] = found[i];
        g_scan_msg = msg;
    }
    modkit::host()->log(0, "raids: %s", msg);
    for (auto &r : runs) modkit::host()->log(1, "raids: run at %llx, %u dens", (unsigned long long)r.first, r.second);
    g_scanning = false;
}

static void scan_start() {
    if (g_scanning.exchange(true)) return;
    std::thread(scan_thread).detach();
}

/* the dens of a region from its first copy still in place (false: none, scan again) */
static bool read_dens(int region, Den *out) {
    std::lock_guard<std::mutex> l(g_blocks_lock);
    for (u64 a : g_blocks[region]) {
        if (!mem_read(a, out, region_dens[region] * sizeof(Den))) continue;
        bool ok = true;
        for (u32 i = 0; i < region_dens[region] && ok; i++) ok = den_slot(&out[i].enabled);
        if (ok) return true;
    }
    return false;
}

/* one den's seed and content into every copy of the region that is still in place */
static int write_den(int region, u32 index, u32 seed, u32 content, bool enable) {
    std::lock_guard<std::mutex> l(g_blocks_lock);
    int n = 0;
    for (u64 a : g_blocks[region]) {
        Den d;
        u64 at = a + (u64)index * sizeof(Den);
        if (!mem_read(at, &d, sizeof d) || !den_shape(&d.enabled)) continue;
        d.seed = seed;
        d.content = content;
        if (enable) d.enabled = 1;
        if (mem_write(at, &d, sizeof d)) n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------------------------- the finder */
struct Want { int region, progress, raid; bool shiny; int tera; };   /* raid: index into D.raids; tera -1: any */
static std::atomic<bool> g_finding, g_find_stop;
static std::atomic<u64> g_find_tried;
static std::mutex g_find_lock;
static std::vector<u32> g_found_seeds;
static std::string g_find_msg;

static bool matches(u32 seed, const Want &w, u32 content) {
    int stars;
    const Raid *e = pick(seed, w.region, content, w.progress, &stars);
    if (e != &D.raids[w.raid]) return false;
    if (w.tera >= 0 && tera_of(seed, *e) != w.tera) return false;
    return !w.shiny || shiny_of(seed, *e);
}

static void find_thread(Want w, u32 count) {
    const Raid &e = D.raids[w.raid];
    u32 content = e.stars == 6 ? 1 : 0;
    u32 start;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    start = (u32)(t.QuadPart * 2654435761u);
    std::vector<u32> out;
    u64 i = 0;
    for (; i < (1ull << 32) && out.size() < count && !g_find_stop; i++) {
        u32 s = start + (u32)i;
        if (matches(s, w, content)) out.push_back(s);
        if ((i & 0xffff) == 0) g_find_tried = i;
    }
    g_find_tried = i;
    char msg[160];
    snprintf(msg, sizeof msg, out.size() >= count ? "%zu seed(s) found (%llu tried)"
                                                  : "only %zu seed(s) in %llu tried",
             out.size(), (unsigned long long)i);
    std::lock_guard<std::mutex> l(g_find_lock);
    g_found_seeds = out;
    g_find_msg = msg;
    g_finding = false;
}

/* ---------------------------------------------------------------------------------------------- UI */
static std::atomic<bool> g_open;
static int g_region = R_PALDEA, g_progress = 4, g_sel = 0;
static Want g_want = {R_PALDEA, 4, -1, false, -1};
static char g_filter[64];
static char g_seed_text[16];
static bool g_enable = true;
static Den g_dens[100];
static bool g_have_dens;
static std::string g_status;

static bool contains_nocase(const char *s, const char *sub) {
    for (; *s; s++) {
        size_t k = 0;
        while (sub[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)sub[k])) k++;
        if (!sub[k]) return true;
    }
    return false;
}

static void stats_line(const Result &r) {
    const Raid &e = *r.raid;
    ImGui::Text("%s%s  Lv.%u  %d stars  Tera %s", name_of(D.species, e.dev), r.shiny ? "  (shiny)" : "",
                e.level, r.stars, name_of(D.types, (u32)r.tera));
    ImGui::Text("IVs %d/%d/%d/%d/%d/%d   %s   %s   %s", r.ivs[0], r.ivs[1], r.ivs[2], r.ivs[3], r.ivs[4], r.ivs[5],
                name_of(D.natures, (u32)r.nature), name_of(D.abilities, e.abilities[r.ability < 3 ? r.ability : 0]),
                r.gender == 0 ? "male" : r.gender == 1 ? "female" : "genderless");
    ImGui::Text("Moves: %s, %s, %s, %s", name_of(D.moves, e.moves[0]), name_of(D.moves, e.moves[1]),
                name_of(D.moves, e.moves[2]), name_of(D.moves, e.moves[3]));
}

static void ui() {
    ImGui::SetNextWindowSize(ImVec2(900, 640), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("Tera Raids (F6)", &open)) { ImGui::End(); if (!open) g_open = false; return; }
    if (!open) g_open = false;
    if (!D.loaded) { ImGui::TextWrapped("%s", D.error.c_str()); ImGui::End(); return; }

    if (g_scanning) {
        ImGui::ProgressBar((float)((double)g_scan_done / (double)g_scan_total), ImVec2(240, 0), "scanning the heap");
    } else if (ImGui::Button("Scan memory for raid dens")) scan_start();
    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> l(g_blocks_lock);
        ImGui::TextUnformatted(g_scan_msg.empty() ? "Load into the overworld, then scan." : g_scan_msg.c_str());
    }
    ImGui::SetNextItemWidth(280);
    ImGui::Combo("Story progress", &g_progress, progress_names, 5);
    for (int r = 0; r < N_REGIONS; r++) {
        if (r) ImGui::SameLine();
        if (ImGui::RadioButton(region_names[r], g_region == r)) { g_region = r; g_sel = 0; }
    }

    g_have_dens = read_dens(g_region, g_dens);
    u32 nd = region_dens[g_region];
    if (!g_have_dens) ImGui::TextDisabled("No %s dens in memory: scan (again after loading a save).", region_names[g_region]);

    if (g_have_dens && ImGui::BeginTable("dens", 8, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                                         ImVec2(0, 260))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        const char *heads[] = {"#", "On", "Area", "Seed", "Stars", "Species", "Tera", "Shiny"};
        for (const char *h : heads) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (u32 i = 0; i < nd; i++) {
            const Den &d = g_dens[i];
            Result r = generate(d.seed, g_region, d.content, g_progress);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char lbl[16];
            snprintf(lbl, sizeof lbl, "%u", i);
            if (ImGui::Selectable(lbl, g_sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) {
                g_sel = (int)i;
                snprintf(g_seed_text, sizeof g_seed_text, "%08X", d.seed);
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.enabled ? "yes" : "-");
            ImGui::TableNextColumn(); ImGui::Text("%u-%u-%u", d.area, d.group, d.point);
            ImGui::TableNextColumn(); ImGui::Text("%08X", d.seed);
            if (r.raid) {
                ImGui::TableNextColumn(); ImGui::Text("%d", r.stars);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(name_of(D.species, r.raid->dev));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(name_of(D.types, (u32)r.tera));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.shiny ? "SHINY" : "");
            } else {
                ImGui::TableNextColumn(); ImGui::TextUnformatted(d.content == 3 ? "7" : "-");
                ImGui::TableNextColumn(); ImGui::TextDisabled(d.content >= 2 ? "(event)" : "(none)");
                ImGui::TableNextColumn(); ImGui::TableNextColumn();
            }
        }
        ImGui::EndTable();
    }

    if (g_have_dens && g_sel < (int)nd) {
        const Den &d = g_dens[g_sel];
        ImGui::SeparatorText("Selected den");
        Result r = generate(d.seed, g_region, d.content, g_progress);
        if (r.raid) stats_line(r);
        else ImGui::TextDisabled("Den %d: %s", g_sel, d.content >= 2 ? "an event raid (not covered)" : "no raid");
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("Seed (hex)", g_seed_text, sizeof g_seed_text, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        static int content = 0;
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("##content", &content, "Standard (1-5)\0Black (6)\0");
        ImGui::SameLine();
        if (ImGui::Button("Write seed")) {
            u32 s = (u32)strtoul(g_seed_text, NULL, 16);
            int n = write_den(g_region, (u32)g_sel, s, (u32)content, g_enable);
            char m[96];
            snprintf(m, sizeof m, "den %d: seed %08X written (%d copies)", g_sel, s, n);
            g_status = m;
        }
    }

    ImGui::SeparatorText("Find a raid");
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##filter", "filter species", g_filter, sizeof g_filter);
    ImGui::SameLine();
    char preview[96] = "(choose)";
    if (g_want.raid >= 0 && g_want.raid < (int)D.raids.size()) {
        const Raid &e = D.raids[g_want.raid];
        snprintf(preview, sizeof preview, "%s  %d stars%s", name_of(D.species, e.dev), e.stars,
                 e.form ? " (form)" : "");
    }
    ImGui::SetNextItemWidth(320);
    if (ImGui::BeginCombo("Raid", preview, ImGuiComboFlags_HeightLarge)) {
        for (int s = 1; s <= 6; s++)
            for (u32 i : D.table[g_region][s]) {
                const Raid &e = D.raids[i];
                if (e.rom != 0 && e.rom != g_rom) continue;
                const char *nm = name_of(D.species, e.dev);
                if (g_filter[0] && !contains_nocase(nm, g_filter)) continue;
                char lbl[128];
                snprintf(lbl, sizeof lbl, "%s  %d stars%s  Lv.%u  (%u%%)##%u", nm, e.stars, e.form ? " (form)" : "",
                         e.level, e.rate / 100, i);
                if (ImGui::Selectable(lbl, g_want.raid == (int)i)) g_want.raid = (int)i;
            }
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Shiny", &g_want.shiny);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    {
        const char *tp = g_want.tera < 0 ? "any" : name_of(D.types, (u32)g_want.tera);
        if (ImGui::BeginCombo("Tera type", tp)) {
            if (ImGui::Selectable("any", g_want.tera < 0)) g_want.tera = -1;
            for (int t = 0; t < 18; t++)
                if (ImGui::Selectable(name_of(D.types, (u32)t), g_want.tera == t)) g_want.tera = t;
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Turn the den on", &g_enable);

    bool reachable = true;
    if (g_want.raid >= 0) {
        const Raid &e = D.raids[g_want.raid];
        if (e.stars < 6) {
            reachable = false;
            for (u32 roll = 0; roll < 100 && !reachable; roll++) reachable = stars_of_roll(roll, g_progress) == e.stars;
            if (!reachable) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%d-star raids do not appear at this story progress.", e.stars);
        }
    }
    if (g_finding) {
        ImGui::Text("Searching... %.1f M seeds", (double)g_find_tried / 1e6);
        ImGui::SameLine();
        if (ImGui::Button("Stop")) g_find_stop = true;
    } else if (g_want.raid >= 0 && reachable) {
        Want w = g_want;
        w.region = g_region;
        w.progress = g_progress;
        bool one = ImGui::Button("Find for the selected den");
        ImGui::SameLine();
        bool all = g_have_dens && ImGui::Button("Find for every den in this region");
        if (one || all) {
            g_find_stop = false;
            g_finding = true;
            std::thread(find_thread, w, all ? nd : 1u).detach();
        }
    }
    {
        std::lock_guard<std::mutex> l(g_find_lock);
        if (!g_finding && !g_found_seeds.empty()) {
            ImGui::Text("%s", g_find_msg.c_str());
            const Raid &e = D.raids[g_want.raid >= 0 ? g_want.raid : 0];
            u32 content = e.stars == 6 ? 1 : 0;
            ImGui::SameLine();
            if (g_have_dens && ImGui::Button(g_found_seeds.size() == 1 ? "Write into the selected den" : "Write into every den")) {
                int dens = 0, copies = 0;
                if (g_found_seeds.size() == 1) {
                    copies = write_den(g_region, (u32)g_sel, g_found_seeds[0], content, g_enable);
                    dens = 1;
                } else
                    for (u32 i = 0; i < nd && i < g_found_seeds.size(); i++) {
                        if (!g_dens[i].enabled && !g_enable) continue;
                        copies += write_den(g_region, i, g_found_seeds[i], content, g_enable);
                        dens++;
                    }
                char m[128];
                snprintf(m, sizeof m, "%d den(s) written (%d block copies). Fly somewhere or reload the area to see them.",
                         dens, copies);
                g_status = m;
                g_found_seeds.clear();
            }
            if (g_found_seeds.size() == 1) {
                Result r = generate(g_found_seeds[0], g_region, content, g_progress);
                ImGui::SameLine();
                ImGui::Text("seed %08X", g_found_seeds[0]);
                if (r.raid) stats_line(r);
            }
        } else if (!g_finding && !g_find_msg.empty())
            ImGui::TextUnformatted(g_find_msg.c_str());
    }
    if (!g_status.empty()) ImGui::TextWrapped("%s", g_status.c_str());
    ImGui::End();
}

/* ---------------------------------------------------------------------------------------------- the mod */
static bool any_open() { return g_open || items_open(); }

static bool mod_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_KEY_DOWN && (e->key.scancode == SDL_SCANCODE_F6 || e->key.scancode == SDL_SCANCODE_F7)) {
        if (!e->key.repeat) {
            if (e->key.scancode == SDL_SCANCODE_F6) g_open = !g_open;
            else items_toggle();
        }
        modkit::set_open(any_open());
        return true;
    }
    modkit::set_open(any_open());           /* after a window's close button (render thread): SDL calls here */
    return modkit::event(e, any_open());
}

static bool mod_wants_input(void) { return any_open(); }

static void mod_draw(const NxModFrame *f) {
    if (!any_open() || !modkit::begin(f)) return;
    if (g_open) ui();
    items_ui();
    modkit::end(f);
}

static const NxMod mod = {"Tera raids", mod_event, mod_wants_input, mod_draw};

extern "C" __declspec(dllexport) const NxMod *nx_mod_init(const NxModHost *host) {
    if (host->version != NXMOD_VERSION) return NULL;
    if (strcmp(host->title_id, "01008F6008C5E000") && strcmp(host->title_id, "0100A3D008C5C000")) return NULL;
    g_rom = strcmp(host->title_id, "0100A3D008C5C000") ? 2 : 1;
    modkit::init(host, "raids");
    load_data(host->cache_dir);
    items_init(host->cache_dir);
    if (!D.loaded) host->log(0, "raids: %s", D.error.c_str());
    else host->log(1, "raids: %zu raid table entries, %s", D.raids.size(), g_rom == 2 ? "Violet" : "Scarlet");
    return &mod;
}
