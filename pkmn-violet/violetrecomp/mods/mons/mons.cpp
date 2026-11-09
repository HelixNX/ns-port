/* Mon spawner: a mod of its own (violet_mons.dll, optional), window "Spawn a Pokemon" (F8 by default): a Pokemon made to
 * order, written into the first free box slot.
 *
 * The boxes are the save's box block (PKHeX: KBox 0x0D66012C), which the game keeps in memory and reads when the box
 * screen opens: 990 slots of 0x158 bytes, each an encrypted PK9 (empty slots too: an encrypted blank). We write our
 * own PK9 into a slot there, as live box editors and trade bots do on a console. The format, as PKHeX reads it
 * (offsets checked with violetrecomp\tools\svedit `layout`, our writer's output with `check`), decrypted:
 *   0 EC, 6 checksum (u16 sum of 0x08..0x148), 8 species (the game's internal index), 0xA item, 0xC TID16, 0xE SID16,
 *   0x10 EXP, 0x14 ability, 0x16 ability number (1, 2, 4), 0x1C PID, 0x20 nature, 0x21 stat nature, 0x22 fateful |
 *   gender << 1, 0x24 form, 0x26 EVs (hp atk def spe spa spd), 0x48 height, 0x49 weight, 0x4A scale, 0x58 nickname
 *   (13 UTF-16), 0x72 moves, 0x7A PP, 0x7E PP ups, 0x82 relearn moves, 0x8A current HP, 0x8C IVs (5 bits each, hp atk
 *   def spe spa spd; bit 30 egg, 31 nicknamed), 0x94 Tera type, 0x95 Tera override (0x13 none), 0xCE version, 0xD4
 *   affixed ribbon (0xFF none), 0xD5 language, 0xF8 OT name, 0x112 OT friendship, 0x11C met date (y-2000, m, d), 0x11F
 *   obedience level, 0x122 met location, 0x124 ball, 0x125 met level | OT gender << 7; party: 0x148 level, 0x14A max
 *   HP, then atk def spe spa spd.
 * Encrypted: the four 0x50 blocks from 0x08 shuffled by (EC >> 13) & 31, then 0x08..0x148 and 0x148..0x158 each XORed
 * with an LCRNG (x * 0x41C64E6D + 0x6073, high 16 bits) seeded with the EC.
 *
 * The block is found by scanning guest memory for 960+ consecutive slots whose checksums hold (the window scans each
 * time it opens). The trainer (OT name, ids, language) is copied from the most common one in the boxes.
 * Tables: <build>\mods\mons_*.txt (svedit `monsdata`, PKHeX.Core's personal / move / experience data). */
#include "filtercombo.h"
#include "modkit.h"
#include <algorithm>
#include <atomic>
#include <ctype.h>
#include <map>
#include <mutex>
#include <random>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <time.h>
#include <vector>
#include <windows.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

namespace {

enum { SLOT = 0x158, STORED = 0x148, MIN_RUN = 960, BOX_SLOTS = 30 };

/* ---------------------------------------------------------------------------------------------- PK9 */
const char *const shuffle[32] = {"ABCD", "ABDC", "ACBD", "ACDB", "ADBC", "ADCB", "BACD", "BADC", "BCAD", "BCDA", "BDAC",
                                 "BDCA", "CABD", "CADB", "CBAD", "CBDA", "CDAB", "CDBA", "DABC", "DACB", "DBAC", "DBCA",
                                 "DCAB", "DCBA", "ABCD", "ABDC", "ACBD", "ACDB", "ADBC", "ADCB", "BACD", "BADC"};

u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
void wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
void wr32(u8 *p, u32 v) { wr16(p, v); wr16(p + 2, v >> 16); }

void xor_stream(u8 *d, u32 seed, int from, int to) {
    for (int i = from; i < to; i += 2) {
        seed = seed * 0x41C64E6Du + 0x6073u;
        wr16(d + i, rd16(d + i) ^ (seed >> 16));
    }
}

u16 checksum(const u8 *dec) {
    u32 s = 0;
    for (int i = 8; i < STORED; i += 2) s += rd16(dec + i);
    return (u16)s;
}

void encrypt(const u8 *dec, u8 *enc) {
    u32 ec = rd32(dec);
    const char *order = shuffle[(ec >> 13) & 31];
    memcpy(enc, dec, SLOT);
    for (int pos = 0; pos < 4; pos++) memcpy(enc + 8 + 0x50 * pos, dec + 8 + 0x50 * (order[pos] - 'A'), 0x50);
    xor_stream(enc, ec, 8, STORED);
    xor_stream(enc, ec, STORED, SLOT);
}

void decrypt(const u8 *enc, u8 *dec) {
    u32 ec = rd32(enc);
    u8 t[SLOT];
    memcpy(t, enc, SLOT);
    xor_stream(t, ec, 8, STORED);
    xor_stream(t, ec, STORED, SLOT);
    const char *order = shuffle[(ec >> 13) & 31];
    memcpy(dec, t, SLOT);
    for (int pos = 0; pos < 4; pos++) memcpy(dec + 8 + 0x50 * (order[pos] - 'A'), t + 8 + 0x50 * pos, 0x50);
}

/* an encrypted slot's checksum holds (the block shuffle does not change the sum) */
bool slot_valid(const u8 *enc) {
    if (rd16(enc + 4)) return false;
    u32 seed = rd32(enc), s = 0;
    for (int i = 8; i < STORED; i += 2) {
        seed = seed * 0x41C64E6Du + 0x6073u;
        s += rd16(enc + i) ^ (seed >> 16);
    }
    return (u16)s == rd16(enc + 6);
}

/* ---------------------------------------------------------------------------------------------- tables */
struct Species {
    u16 national, form, internal;
    std::string name, form_name;
    u8 growth, gender;              /* gender: 0 male only, 254 female only, 255 genderless, else the female ratio */
    u16 ability[3];
    u8 type1, type2, friendship;
    u8 base[6];                     /* hp atk def spa spd spe */
    std::string label;
};
struct Move { u16 id; u8 pp; std::string name; };

std::vector<Species> g_species;
std::vector<Move> g_moves;
std::map<u16, std::string> g_ability_names;
std::vector<std::pair<u8, std::string>> g_balls;
std::vector<std::pair<u16, std::string>> g_held;   /* items a mon can hold (items.txt, by name) */
std::vector<u32> g_exp[6];
struct Trainer { u16 tid = 0, sid = 0; u8 name[26] = {}; u8 gender = 0, language = 2, version = 51; u16 location = 6; bool ok = false; };
Trainer g_save_trainer;                     /* mons_trainer.txt (svedit monsdata, from the save); else the boxes' */
void put_name(u8 *dst, const char *s);
std::string g_error;

std::vector<std::vector<std::string>> read_tsv(const std::string &path) {
    std::vector<std::vector<std::string>> rows;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { if (g_error.empty()) g_error = "no " + path + " (svedit monsdata)"; return rows; }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::vector<std::string> r;
        std::string cur;
        for (char *p = line; *p && *p != '\n' && *p != '\r'; p++) {
            if (*p == '\t') { r.push_back(cur); cur.clear(); }
            else cur += *p;
        }
        r.push_back(cur);
        rows.push_back(r);
    }
    fclose(f);
    return rows;
}

void load_tables(const char *cache_dir) {
    std::string d = std::string(cache_dir) + "\\mods\\";
    for (auto &r : read_tsv(d + "mons_species.txt")) {
        if (r.size() < 19) continue;
        Species s;
        s.national = (u16)atoi(r[0].c_str()); s.form = (u16)atoi(r[1].c_str()); s.internal = (u16)atoi(r[2].c_str());
        s.name = r[3]; s.form_name = r[4];
        s.growth = (u8)atoi(r[5].c_str()); s.gender = (u8)atoi(r[6].c_str());
        for (int i = 0; i < 3; i++) s.ability[i] = (u16)atoi(r[7 + i].c_str());
        s.type1 = (u8)atoi(r[10].c_str()); s.type2 = (u8)atoi(r[11].c_str()); s.friendship = (u8)atoi(r[12].c_str());
        for (int i = 0; i < 6; i++) s.base[i] = (u8)atoi(r[13 + i].c_str());   /* file: hp atk def spa spd spe */
        char lbl[128];
        snprintf(lbl, sizeof lbl, "%s%s%s%s (#%u)", s.name.c_str(), s.form_name.empty() || !s.form ? "" : " (",
                 s.form && !s.form_name.empty() ? s.form_name.c_str() : "", s.form && !s.form_name.empty() ? ")" : "", s.national);
        s.label = lbl;
        g_species.push_back(s);
    }
    for (auto &r : read_tsv(d + "mons_moves.txt"))
        if (r.size() >= 3) g_moves.push_back({(u16)atoi(r[0].c_str()), (u8)atoi(r[1].c_str()), r[2]});
    std::sort(g_moves.begin(), g_moves.end(), [](const Move &a, const Move &b) { return a.name < b.name; });
    for (auto &r : read_tsv(d + "mons_abilities.txt")) if (r.size() >= 2) g_ability_names[(u16)atoi(r[0].c_str())] = r[1];
    for (auto &r : read_tsv(d + "mons_balls.txt")) if (r.size() >= 2) g_balls.push_back({(u8)atoi(r[0].c_str()), r[1]});
    {   /* optional: no list, no held item choice */
        std::string keep = g_error;
        for (auto &r : read_tsv(d + "items.txt")) {
            if (r.size() < 3) continue;
            int pocket = atoi(r[1].c_str());     /* not TMs, picnic items, key items, materials */
            if (pocket == 5 || pocket == 7 || pocket == 8 || pocket == 9) continue;
            g_held.push_back({(u16)atoi(r[0].c_str()), r[2]});
        }
        std::sort(g_held.begin(), g_held.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
        g_error = keep;
    }
    int g = 0;
    for (auto &r : read_tsv(d + "mons_exp.txt")) {
        if (g >= 6) break;
        for (auto &v : r) g_exp[g].push_back((u32)strtoul(v.c_str(), nullptr, 10));
        g++;
    }
    for (auto &r : read_tsv(d + "mons_trainer.txt"))
        if (r.size() >= 6) {
            Trainer &t = g_save_trainer;
            t.tid = (u16)atoi(r[0].c_str()); t.sid = (u16)atoi(r[1].c_str()); t.gender = (u8)atoi(r[2].c_str());
            t.language = (u8)atoi(r[3].c_str()); t.version = (u8)atoi(r[4].c_str());
            put_name(t.name, r[5].c_str());
            t.ok = true;
        }
    if (g_error.find("mons_trainer") != std::string::npos) g_error.clear();     /* optional */
    if (g_error.empty() && (g_species.empty() || g_moves.empty() || g < 6)) g_error = "the mons_*.txt tables are incomplete";
}

const char *const natures[25] = {"Hardy", "Lonely", "Brave", "Adamant", "Naughty", "Bold", "Docile", "Relaxed", "Impish",
                                 "Lax", "Timid", "Hasty", "Serious", "Jolly", "Naive", "Modest", "Mild", "Quiet",
                                 "Bashful", "Rash", "Calm", "Gentle", "Sassy", "Careful", "Quirky"};
const char *const types[18] = {"Normal", "Fighting", "Flying", "Poison", "Ground", "Rock", "Bug", "Ghost", "Steel",
                               "Fire", "Water", "Grass", "Electric", "Psychic", "Ice", "Dragon", "Dark", "Fairy"};
const char *const stat_names[6] = {"HP", "Atk", "Def", "SpA", "SpD", "Spe"};   /* the UI's order */

/* ---------------------------------------------------------------------------------------------- what to make */
struct Spec {
    int species = -1;               /* index into g_species */
    int level = 50;
    bool shiny = false;
    int gender = 0;                 /* 0 male, 1 female (when the species has both) */
    int nature = 15;
    int ability_slot = 0;           /* 0, 1, 2 hidden */
    int iv[6] = {31, 31, 31, 31, 31, 31};       /* UI order hp atk def spa spd spe */
    int ev[6] = {};
    u16 moves[4] = {};
    int tera = -1;                  /* -1: the species' first type; 18: Stellar */
    u8 ball = 4;                    /* Poke Ball */
    u16 item = 0;                   /* held item (0 none) */
    char nickname[13] = "";
};

u32 stat(const Species &s, const Spec &p, int i) {    /* i: UI order */
    int b = s.base[i], iv = p.iv[i], ev = p.ev[i], l = p.level;
    if (i == 0) return s.national == 292 ? 1 : (u32)((2 * b + iv + ev / 4) * l / 100 + l + 10);
    u32 v = (u32)((2 * b + iv + ev / 4) * l / 100 + 5);
    int up = p.nature / 5, down = p.nature % 5;           /* the nature's stats: 0 atk 1 def 2 spe 3 spa 4 spd */
    int me = i == 1 ? 0 : i == 2 ? 1 : i == 3 ? 3 : i == 4 ? 4 : 2;
    if (up != down) { if (me == up) v = v * 110 / 100; else if (me == down) v = v * 90 / 100; }
    return v;
}

void put_name(u8 *dst, const char *s) {     /* 12 characters + terminator, UTF-16 (ASCII input) */
    memset(dst, 0, 26);
    for (int i = 0; i < 12 && s[i]; i++) wr16(dst + 2 * i, (u8)s[i]);
}

/* the decrypted PK9 */
void build(const Spec &p, const Trainer &t, u8 *d) {
    const Species &s = g_species[p.species];
    static std::mt19937 rng((u32)GetTickCount64() ^ (u32)time(nullptr));
    memset(d, 0, SLOT);
    u32 ec = rng(), pid = rng();
    u16 lo = (u16)pid;
    if (p.shiny) pid = (u32)lo | (u32)(u16)(lo ^ t.tid ^ t.sid) << 16;       /* square: the xor is 0 */
    else if (((pid >> 16) ^ lo ^ t.tid ^ t.sid) < 16) pid ^= 0x10000000;      /* not shiny by chance */
    wr32(d + 0x00, ec);
    wr16(d + 0x08, s.internal);
    wr16(d + 0x0A, p.item);
    wr16(d + 0x0C, t.tid);
    wr16(d + 0x0E, t.sid);
    int lv = p.level < 1 ? 1 : p.level > 100 ? 100 : p.level;
    wr32(d + 0x10, g_exp[s.growth < 6 ? s.growth : 0][lv - 1]);
    wr16(d + 0x14, s.ability[p.ability_slot]);
    d[0x16] = (u8)(1 << p.ability_slot);
    wr32(d + 0x1C, pid);
    d[0x20] = d[0x21] = (u8)p.nature;
    int gender = s.gender == 255 ? 2 : s.gender == 254 ? 1 : s.gender == 0 ? 0 : p.gender;
    d[0x22] = (u8)(gender << 1);
    wr16(d + 0x24, s.form);
    static const int ev_slot[6] = {0, 1, 2, 4, 5, 3};     /* UI hp atk def spa spd spe -> hp atk def spe spa spd */
    for (int i = 0; i < 6; i++) d[0x26 + ev_slot[i]] = (u8)p.ev[i];
    d[0x48] = (u8)rng(); d[0x49] = (u8)rng(); d[0x4A] = (u8)rng();
    put_name(d + 0x58, p.nickname[0] ? p.nickname : s.name.c_str());
    for (int i = 0; i < 4; i++) {
        wr16(d + 0x72 + 2 * i, p.moves[i]);
        u8 pp = 0;
        for (auto &m : g_moves) if (m.id == p.moves[i]) pp = m.pp;
        d[0x7A + i] = pp;
    }
    u32 iv32 = 0;
    for (int i = 0; i < 6; i++) iv32 |= (u32)(p.iv[i] & 31) << (5 * ev_slot[i]);
    if (p.nickname[0]) iv32 |= 1u << 31;
    wr32(d + 0x8C, iv32);
    d[0x94] = p.tera < 0 ? s.type1 : p.tera == 18 ? 99 : (u8)p.tera;
    d[0x95] = 0x13;
    d[0xCE] = t.version;
    d[0xD4] = 0xFF;
    d[0xD5] = t.language;
    memcpy(d + 0xF8, t.name, 26);
    d[0x112] = s.friendship;
    time_t now = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &now);
    d[0x11C] = (u8)(tm.tm_year - 100); d[0x11D] = (u8)(tm.tm_mon + 1); d[0x11E] = (u8)tm.tm_mday;
    d[0x11F] = (u8)lv;
    wr16(d + 0x122, t.location);
    d[0x124] = p.ball;
    d[0x125] = (u8)(lv | t.gender << 7);
    /* party part: level and stats */
    Spec q = p;
    q.level = lv;
    u32 hp = stat(s, q, 0);
    wr16(d + 0x8A, hp);
    d[0x148] = (u8)lv;
    wr16(d + 0x14A, hp);
    wr16(d + 0x14C, stat(s, q, 1)); wr16(d + 0x14E, stat(s, q, 2)); wr16(d + 0x150, stat(s, q, 5));
    wr16(d + 0x152, stat(s, q, 3)); wr16(d + 0x154, stat(s, q, 4));
    wr16(d + 0x06, checksum(d));
}

/* ---------------------------------------------------------------------------------------------- the box block */
std::mutex g_lock;
std::vector<u64> g_blocks;                  /* where each copy of the box block starts (all written) */
std::atomic<bool> g_scanning;
std::atomic<u64> g_done, g_total;
std::string g_scan_msg, g_status;
std::mutex g_status_lock;

void set_status(const std::string &s) { std::lock_guard<std::mutex> l(g_status_lock); g_status = s; }

bool mem_read(u64 a, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, out, n, &got) && got == n;
}
bool mem_write(u64 a, const void *in, size_t n) {
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)a, in, n, &put) && put == n;
}

/* ---------------------------------------------------------------------------------------------- wild spawns
 * The overworld spawner draws from the wild encounter tables, world/data/encount/pokedata/pokedata{,_su1,_su2}
 * (Paldea, Kitakami, Blueberry), FlatBuffers the game keeps in memory as loaded: {entries [entry]}, an entry's
 * fields (vtable slots): 0 DevID u16, 1 sex, 2 form u8, 3 min level u16, 4 max level u16, 5 weight, 6-13 biome /
 * weight pairs, 14 area, 15 location, 18-20 time / flags, 21-25 a companion (23 its DevID u16, 25 its form u8),
 * 26-29 the rest. Copies from the game's files: <build>\mods\encount_pokedata*.bin (raiddata.py).
 *
 * "Wild spawns" rewrites every entry in place: species, form, level range (and companions' species), so the game's
 * own spawner places that mon wherever it would place any (model, behaviour, battle, catching all the game's). Form
 * is absent in most entries (default 0): where the entry's vtable has no slot for it, slot 2 is pointed at a padding
 * byte of the table (bytes 4.. before its first field; entries sharing a vtable share the layout); an entry with
 * neither gets weights (5, 7, 9, 11, 13) 0 for a form other than 0, so it is never picked. A level range of
 * 1-100 leaves the level to the spawn point's own range (an entry outside a point's range does not spawn there).
 * Restore writes the original bytes back. Found by its first 28 bytes (root, entry count, first offset), then
 * checked byte for byte except where the patch writes. */
/* the log, or nothing offline (mons_wild_selftest) */
template <class... A> void enc_log(int level, const char *fmt, A... a) { if (modkit::host()) modkit::host()->log(level, fmt, a...); }
struct EncEntry { u32 dev, form, minl, maxl, band_dev, band_form, vt_slot2, weight[5]; };
struct EncTable {
    const char *file, *label;
    std::vector<u8> orig, touch;            /* touch: bytes a patch may change */
    std::vector<EncEntry> entries;          /* buffer offsets (0: none); form / vt_slot2: the padding byte route */
    std::vector<u64> found;                 /* copies in guest memory */
};
EncTable g_enc[3] = {{"encount_pokedata.bin", "Paldea"}, {"encount_pokedata_su1.bin", "Kitakami"},
                     {"encount_pokedata_su2.bin", "Blueberry"}};
enum { ENC_SIG = 28 };
std::string g_wild_status;
u32 g_enc_first;                            /* the tables' first word (their root offset), 0: none loaded */

bool enc_parse(EncTable &t) {
    const std::vector<u8> &b = t.orig;
    size_t n = b.size();
    auto u32at = [&](size_t o) { return o + 4 <= n ? rd32(&b[o]) : 0u; };
    auto u16at = [&](size_t o) { return o + 2 <= n ? rd16(&b[o]) : 0u; };
    auto field = [&](u32 tab, int k, u32 &vt, u32 &vtsize) -> u32 {
        vt = tab - (u32)(int32_t)u32at(tab);
        vtsize = u16at(vt);
        return (u32)(4 + 2 * k) < vtsize ? u16at(vt + 4 + 2 * k) : 0;
    };
    if (n < 64) return false;
    u32 root = u32at(0), vt, vs;
    u32 f0 = field(root, 0, vt, vs);
    if (!f0) return false;
    u32 vec = root + f0 + u32at(root + f0), cnt = u32at(vec);
    if (vec + 4 + 4ull * cnt > n) return false;
    t.touch.assign(n, 0);
    auto mark = [&](u32 o, int len) { for (int i = 0; o && i < len && o + i < n; i++) t.touch[o + i] = 1; };
    for (u32 k = 0; k < cnt; k++) {
        u32 e = vec + 4 + 4 * k;
        e += u32at(e);
        EncEntry x = {};
        u32 off;
        if ((off = field(e, 0, vt, vs))) x.dev = e + off;
        if ((off = field(e, 3, vt, vs))) x.minl = e + off;
        if ((off = field(e, 4, vt, vs))) x.maxl = e + off;
        if ((off = field(e, 23, vt, vs))) x.band_dev = e + off;
        if ((off = field(e, 25, vt, vs))) x.band_form = e + off;
        for (int w = 0; w < 5; w++) if ((off = field(e, 5 + 2 * w, vt, vs))) x.weight[w] = e + off;
        if ((off = field(e, 2, vt, vs))) x.form = e + off;
        else if (vs >= 4 + 2 * 3) {
            u32 tsize = u16at(vt + 2), lo = tsize;
            for (int f = 0; 4 + 2 * f < (int)vs; f++) { u32 o = u16at(vt + 4 + 2 * f); if (o && o < lo) lo = o; }
            if (lo > 4) { x.form = e + 4; x.vt_slot2 = vt + 4 + 2 * 2; }
        }
        if (!x.dev) continue;
        mark(x.dev, 2); mark(x.minl, 2); mark(x.maxl, 2); mark(x.band_dev, 2); mark(x.form, 1); mark(x.band_form, 1);
        if (x.vt_slot2) mark(x.vt_slot2, 2);
        if (!x.form) for (u32 w : x.weight) mark(w, 2);
        t.entries.push_back(x);
    }
    for (int i = 0; i < ENC_SIG; i++) if (t.touch[i]) return false;
    return !t.entries.empty();
}

void enc_load(const char *cache_dir) {
    for (auto &t : g_enc) {
        std::string p = std::string(cache_dir) + "\\mods\\" + t.file;
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) { enc_log(0, "mons: no %s (run mods\\raids\\raiddata.py): no wild spawns", p.c_str()); continue; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        t.orig.resize(sz > 0 ? (size_t)sz : 0);
        if (fread(t.orig.data(), 1, t.orig.size(), f) != t.orig.size()) t.orig.clear();
        fclose(f);
        if (!enc_parse(t)) { enc_log(0, "mons: %s: not an encounter table", t.file); t.orig.clear(); t.entries.clear(); continue; }
        g_enc_first = rd32(t.orig.data());
        enc_log(1, "mons: %s: %zu entries", t.label, t.entries.size());
    }
}

/* whether a copy of t starts at a: every byte the patch does not write as in the file */
bool enc_at(const EncTable &t, u64 a, std::vector<u8> &buf) {
    buf.resize(t.orig.size());
    if (!mem_read(a, buf.data(), buf.size())) return false;
    for (size_t i = 0; i < buf.size(); i++) if (!t.touch[i] && buf[i] != t.orig[i]) return false;
    return true;
}

/* species index sp (-1: restore), level 0: 1-100 (the spawn point decides) */
void enc_apply(int sp, int level, std::string &msg) {
    std::lock_guard<std::mutex> l(g_lock);
    int tables = 0, copies = 0, entries = 0, no_form = 0;
    std::vector<u8> cur;
    for (auto &t : g_enc) {
        if (t.orig.empty()) continue;
        std::vector<u8> img = t.orig;
        if (sp >= 0) {
            const Species &s = g_species[sp];
            int lo = level ? level : 1, hi = level ? level : 100;
            for (auto &e : t.entries) {
                wr16(&img[e.dev], s.internal);
                if (e.minl) wr16(&img[e.minl], lo);
                if (e.maxl) wr16(&img[e.maxl], hi);
                if (e.band_dev) wr16(&img[e.band_dev], s.internal);
                if (e.band_form) img[e.band_form] = (u8)s.form;
                if (e.form) {
                    img[e.form] = (u8)s.form;
                    if (e.vt_slot2 && s.form) wr16(&img[e.vt_slot2], 4);
                } else if (s.form) {        /* no way to give it the form: never picked (weights 0) */
                    for (u32 w : e.weight) if (w) wr16(&img[w], 0);
                    no_form++;
                }
                entries++;
            }
        }
        int w = 0;
        for (u64 a : t.found)
            if (enc_at(t, a, cur) && mem_write(a, img.data(), img.size())) w++;
        if (w) tables++;
        copies += w;
    }
    char m[256];
    if (sp < 0) snprintf(m, sizeof m, "Wild spawns restored (%d table cop%s).", copies, copies == 1 ? "y" : "ies");
    else if (!copies) snprintf(m, sizeof m, "No encounter table in memory: scan (load into the overworld first).");
    else snprintf(m, sizeof m, "Wild spawns: %s, level %s, in %d table%s (%d cop%s, %d entries%s). New spawns only: "
                  "walk away from the ones already out.", g_species[sp].label.c_str(),
                  level ? std::to_string(level).c_str() : "as the area's", tables, tables == 1 ? "" : "s", copies,
                  copies == 1 ? "y" : "ies", entries, no_form ? ", some left out: no form slot" : "");
    msg = m;
    enc_log(0, "mons: %s", m);
}

/* whether a block still starts at `a` (its first MIN_RUN slots hold) */
bool block_at(u64 a, std::vector<u8> &buf) {
    buf.resize((size_t)MIN_RUN * SLOT);
    if (!mem_read(a, buf.data(), buf.size())) return false;
    for (int i = 0; i < MIN_RUN; i++) if (!slot_valid(&buf[(size_t)i * SLOT])) return false;
    return true;
}

void scan_thread() {
    const u64 lo = NXMOD_ALIAS_BASE, hi = NXMOD_HEAP_BASE + NXMOD_HEAP_MAX;
    u64 total = 0;
    for (u64 a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi)) break;
        u64 end = (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT) total += (end < hi ? end : hi) - a;
        a = end;
    }
    g_total = total ? total : 1;
    g_done = 0;
    std::vector<u64> found, enc_cand[3];
    const size_t CH = 16 << 20;
    std::vector<u8> buf(CH + SLOT), tmp;
    u64 run_start = 0, run_next = 0;
    int run_len = 0;
    for (u64 a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi)) break;
        u64 rend = (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (rend > hi) rend = hi;
        bool rw = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                  (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE));
        if (rw)
            for (u64 c = a; c < rend; c += CH) {
                size_t n = (size_t)(rend - c < CH ? rend - c : CH);
                size_t rn = (size_t)(rend - c < CH + SLOT ? rend - c : CH + SLOT);   /* a slot across the chunk's end */
                g_done += n;
                if (!mem_read(c, buf.data(), rn)) continue;
                for (size_t i = 0; i < n && i + SLOT <= rn; i += 8) {
                    const u8 *e = &buf[i];
                    if (g_enc_first && rd32(e) == g_enc_first)
                        for (int k = 0; k < 3; k++)
                            if (g_enc[k].orig.size() && !memcmp(e, g_enc[k].orig.data(), ENC_SIG)) enc_cand[k].push_back(c + i);
                    if (rd16(e + 4) || (!rd32(e + 8) && !rd32(e + 12) && !rd32(e + 16))) continue;
                    if (!slot_valid(e)) continue;
                    u64 at = c + i;
                    if (run_len && at == run_next) { run_len++; run_next += SLOT; }
                    else if (!run_len || at > run_next) { run_start = at; run_next = at + SLOT; run_len = 1; }
                    else continue;                                  /* inside the current run, off its stride */
                    if (run_len == MIN_RUN) found.push_back(run_start);
                    i = (size_t)(run_next - c) - 8;                 /* to the next slot of this run */
                }
            }
        a = rend;
    }
    /* a run longer than one block (990 slots) starts where it starts; drop copies that do not hold any more */
    std::vector<u64> ok;
    for (u64 b : found) if (block_at(b, tmp)) ok.push_back(b);
    std::vector<u64> enc_ok[3];
    int enc_n = 0;
    for (int k = 0; k < 3; k++)
        for (u64 a : enc_cand[k]) if (enc_at(g_enc[k], a, tmp)) { enc_ok[k].push_back(a); enc_n++; }
    char msg[192];
    snprintf(msg, sizeof msg, "Found %zu box block%s, %d wild table cop%s (%.1f GB scanned)", ok.size(),
             ok.size() == 1 ? "" : "s", enc_n, enc_n == 1 ? "y" : "ies", (double)g_done / (1 << 30));
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_blocks = ok;
        for (int k = 0; k < 3; k++) g_enc[k].found = enc_ok[k];
        g_scan_msg = msg;
    }
    modkit::host()->log(0, "mons: %s", msg);
    for (int k = 0; k < 3; k++)
        for (u64 a : enc_ok[k]) modkit::host()->log(0, "mons: %s wild table at %llx", g_enc[k].label, (unsigned long long)a);
    for (u64 b : ok) modkit::host()->log(1, "mons: box block at %llx", (unsigned long long)b);
    g_scanning = false;
}

void scan_start() {
    if (g_scanning.exchange(true)) return;
    std::thread(scan_thread).detach();
}

/* the trainer most of the box's mons have (from this game: version 50 / 51) */
bool find_trainer(const std::vector<u8> &blk, Trainer &out) {
    std::map<std::string, int> count;
    std::map<std::string, Trainer> seen;
    u8 dec[SLOT];
    for (size_t i = 0; i + SLOT <= blk.size(); i += SLOT) {
        decrypt(&blk[i], dec);
        if (!rd16(dec + 8) || (dec[0xCE] != 50 && dec[0xCE] != 51)) continue;
        Trainer t;
        t.tid = rd16(dec + 0xC); t.sid = rd16(dec + 0xE);
        memcpy(t.name, dec + 0xF8, 26);
        t.gender = dec[0x125] >> 7; t.language = dec[0xD5]; t.version = dec[0xCE]; t.location = rd16(dec + 0x122);
        t.ok = true;
        std::string key((const char *)dec + 0xC, 4);
        key.append((const char *)dec + 0xF8, 26);
        if (!count[key]++) seen[key] = t;
    }
    int best = 0;
    for (auto &kv : count) if (kv.second > best) { best = kv.second; out = seen[kv.first]; }
    return best > 0;
}

/* the spec into the first empty slot of every copy; returns the slot (-1: none) and fills msg */
int spawn(const Spec &p, std::string &msg) {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<u8> blk;
    std::vector<u64> live;
    for (u64 b : g_blocks) if (block_at(b, blk)) live.push_back(b);
    if (live.empty()) { msg = "No box block in memory: scan (load into the overworld first)."; return -1; }
    if (!block_at(live[0], blk)) { msg = "The box block moved: scan again."; return -1; }
    Trainer t = g_save_trainer;
    if (!t.ok && !find_trainer(blk, t)) {
        msg = "No trainer: run svedit monsdata (writes mods/mons_trainer.txt), or keep one of your own mons in the boxes.";
        return -1;
    }
    int slot = -1;
    u8 dec[SLOT];
    for (int i = 0; i < MIN_RUN && slot < 0; i++) {
        decrypt(&blk[(size_t)i * SLOT], dec);
        if (!rd16(dec + 8)) slot = i;
    }
    if (slot < 0) { msg = "The boxes are full."; return -1; }
    u8 enc[SLOT];
    build(p, t, dec);
    encrypt(dec, enc);
    int w = 0;
    for (u64 b : live) if (mem_write(b + (u64)slot * SLOT, enc, SLOT)) w++;
    char m[256];
    snprintf(m, sizeof m, "%s: box %d, slot %d (%d cop%s written). Open the boxes to see it.",
             g_species[p.species].label.c_str(), slot / BOX_SLOTS + 1, slot % BOX_SLOTS + 1, w, w == 1 ? "y" : "ies");
    msg = m;
    modkit::host()->log(0, "mons: %s", m);
    return slot;
}

/* the rest of the spec for wild spawns: the game's hook on PokemonParam::Create (violetrecomp\wild.c, same layout),
 * reached through the exe's export violet_wild_set */
#pragma pack(push, 1)
struct VioletWild {
    u32 size;
    u16 dev, form, level;
    u8 shiny, sex;
    u16 nature;
    u8 ability;
    int8_t tera;
    u8 iv[6], ev[6];
    u16 moves[4];
    u32 swap_pct;
    u16 item;                       /* 0 as rolled */
    u16 pad;
};
#pragma pack(pop)
static_assert(sizeof(VioletWild) == 44, "wild.c's VioletWild");
typedef void (*WildSet)(const VioletWild *);

WildSet wild_set_fn() {
    static WildSet f = (WildSet)(void *)GetProcAddress(GetModuleHandleW(nullptr), "violet_wild_set");
    return f;
}

/* shiny: 0 as rolled, 1 never, 2 always; all: the IVs / EVs / nature / ability / gender / moves / Tera too */
bool wild_spec_apply(const Spec &p, int level, int shiny, bool all, int pct) {
    WildSet f = wild_set_fn();
    if (!f) return false;
    const Species &s = g_species[p.species];
    VioletWild w = {};
    w.size = sizeof w;
    w.dev = s.internal;
    w.form = s.form;
    w.level = (u16)level;
    w.shiny = (u8)shiny;
    w.swap_pct = (u32)pct;
    w.sex = 0xff; w.nature = 0xffff; w.ability = 0xff; w.tera = -1;
    memset(w.iv, 0xff, 6);
    if (all) {
        if (s.gender != 0 && s.gender != 254 && s.gender != 255) w.sex = (u8)p.gender;
        w.nature = (u16)p.nature;
        w.ability = (u8)p.ability_slot;
        if (p.tera >= 0 && p.tera < 18) w.tera = (int8_t)p.tera;
        for (int i = 0; i < 6; i++) { w.iv[i] = (u8)p.iv[i]; w.ev[i] = (u8)p.ev[i]; }
        for (int i = 0; i < 4; i++) w.moves[i] = p.moves[i];
        w.item = p.item;
    }
    f(&w);
    return true;
}

/* The species swap itself, in the game's scripts: the wild manager (class C4EE52E49562F8277, three call sites) makes
 * every overworld wild mon through C2B428A63695FD2C7.S988D7D0A4EEF693D(?, DevID, form, sex, level, ...), which fills a
 * spec (cC99399C3.f101D811F) and calls c113335A8.f7AC1FE6B. Wrapped through the Lua bridge (host->run_script): a
 * share of calls get our DevID / form (and level), then the original runs. Restore puts the original back. */
const char *const WILD_CLASS = "C2B428A63695FD2C7", *const WILD_FN = "S988D7D0A4EEF693D";

void wild_lua_done(void *, bool ok, const char *r) {
    std::lock_guard<std::mutex> l(g_status_lock);
    g_wild_status += ok ? std::string(" Script: ") + r : std::string(" Script error: ") + r;
}

bool wild_lua(int dev, int form, int level, int pct) {
    const NxModHost *h = modkit::host();
    if (h->version < 3 || !h->run_script) return false;
    char lua[1024];
    if (dev < 0)
        snprintf(lua, sizeof lua,
                 "local C = %s\n"
                 "if C == nil then return '" NXMOD_SCRIPT_AGAIN "' end\n"
                 "if C.__nx_orig then C.%s = C.__nx_orig; C.__nx_orig = nil; return 'wild maker restored' end\n"
                 "return 'wild maker was not wrapped'\n", WILD_CLASS, WILD_FN);
    else
        snprintf(lua, sizeof lua,
                 "local C = %s\n"
                 "if C == nil then return '" NXMOD_SCRIPT_AGAIN "' end\n"
                 "if not C.__nx_orig then C.__nx_orig = C.%s end\n"
                 "local orig = C.__nx_orig\n"
                 "C.__nx_count = 0\n"
                 "C.%s = function(a, dev, form, sex, lv, ...)\n"
                 "  if math.random(100) <= %d then\n"
                 "    C.__nx_count = C.__nx_count + 1\n"
                 "    C.__nx_last = tostring(dev) .. '/' .. tostring(form) .. ' lv ' .. tostring(lv)\n"
                 "    dev = %d; form = %d\n"
                 "    if %d > 0 then lv = %d end\n"
                 "  end\n"
                 "  return orig(a, dev, form, sex, lv, ...)\n"
                 "end\n"
                 "return 'wild maker wrapped'\n",
                 WILD_CLASS, WILD_FN, WILD_FN, pct, dev, form, level, level);
    return h->run_script(lua, wild_lua_done, nullptr);
}

/* ---------------------------------------------------------------------------------------------- around the player
 * The wild manager's instance is C4EE52E49562F8277.SF3075AB31C9E8AF4 (W). Its spawn step (prototype.FA9A2EC1E99B58D55,
 * called by its update F648B6B775BCC730B while W[17] == 0) takes the player's position (K.SBF5E0EFFB821427F:
 * f7360ED03() -> x, y, z), finds a spawn point near a random offset (cFB7F03A4.f42A4ADE3 / f35A39D75 -> a point with
 * a 'position'), reads it into an info table (W:F654580D678C85C0E(point): pos {x, y, z}, min_level, max_level,
 * biome, attribute, areano, areaname, enable_table), builds the group into W[23] (bosses / minions) with
 * W:F61D1ABB7697071EB(info), which picks the species with W[26]:FCEEA7F4134055567(info, rng, ctx, false), and puts
 * it in the world with W:F9B7FBB66FA56765E(W[23]) (field objects at each member's position; W[17] = 1 until loaded).
 * W[26] is the encounter table (class CE1BA7320ED1C2331): F2F6C7BBE115AFBCB(info, ctx, flag, list, weights) fills
 * the entries allowed at that point (rules in F67A9C2A9373A0AC6: area, location, ground ray, ...) and their weights
 * (Haxe arrays, 0-based) and returns the total; an entry has f2BF366A2() DevID, fE15E9D0A() form, f5B8A3FE2() sex,
 * f4646F546() / f61FD9277() level range. ctx = W[28]:FDCCDAF4C1D58AECA(pos) (ctx[4] / ctx[5] force a DevID / form).
 *
 * "Spawn on me" does the spawn step itself with the point's info moved to the player (plus an offset), only while
 * the manager is between spawns (W[17] == 0, else retried); a chosen row forces the pick once (an instance field
 * over the table's picker). Weights: the filter is wrapped on the instance (original kept in T.__nx_f2); overrides in
 * K.__nx_w ["DevID/form"] = weight, '*' for every other entry. A new table instance (save load) drops the wrap:
 * Apply again. */
const char *const FIELD_PRELUDE =
    "local K = C4EE52E49562F8277\n"
    "if K == nil then return '" NXMOD_SCRIPT_AGAIN "' end\n"
    "local W = K.SF3075AB31C9E8AF4\n"
    "if W == nil or W[26] == nil then return 'error|no wild manager: load into the overworld' end\n"
    "local function here()\n"
    "  local x, y, z = K.SBF5E0EFFB821427F:f7360ED03()\n"
    "  for _, r in ipairs({15, 40, 100, 250}) do\n"
    "    for t = 1, 6 do\n"
    "      local ok, pt = pcall(cFB7F03A4.f35A39D75, x, y, z, 0, r, 0)\n"
    "      if ok and pt ~= nil then\n"
    "        local ok2, has = pcall(function() return pt:fDC284F58('position') end)\n"
    "        if ok2 and has then return W:F654580D678C85C0E(pt), x, y, z end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return nil, x, y, z\n"
    "end\n"
    "local function newarr() return setmetatable({length = 0}, getmetatable(W[23].bosses)) end\n"
    "local function get(e, m) local ok, v = pcall(function() return e[m](e) end); if ok then return v end; return -1 end\n";

struct AreaRow { int dev = 0, form = 0, sex = -1, minl = 0, maxl = 0, weight = 0, set = -1; };
struct AreaList { bool ok = false; std::string msg, area, biome; int minl = 0, maxl = 0, total = 0; std::vector<AreaRow> rows; };
std::mutex g_area_lock;
AreaList g_area;
std::string g_field_status;
int g_weight_others = -1;                  /* the weight of every entry without its own (-1 as is) */

void field_status(const std::string &s) { std::lock_guard<std::mutex> l(g_area_lock); g_field_status = s; }

const char *species_label(int dev, int form) {
    for (auto &s : g_species) if (s.internal == dev && s.form == form) return s.label.c_str();
    for (auto &s : g_species) if (s.internal == dev) return s.name.c_str();
    return nullptr;
}

/* the reply: "ok|area|biome|minl|maxl|total" then one line per entry "dev|form|sex|minl|maxl|weight" */
void area_done(void *, bool ok, const char *r) {
    std::vector<std::string> lines;
    for (const char *p = r; *p;) {
        const char *e = strchr(p, '\n');
        lines.emplace_back(p, e ? (size_t)(e - p) : strlen(p));
        if (!e) break;
        p = e + 1;
    }
    AreaList a;
    std::vector<std::string> f;
    auto split = [&f](const std::string &s) {
        f.clear();
        size_t at = 0;
        for (;;) {
            size_t b = s.find('|', at);
            f.push_back(s.substr(at, b == std::string::npos ? std::string::npos : b - at));
            if (b == std::string::npos) break;
            at = b + 1;
        }
    };
    if (!ok || lines.empty()) a.msg = std::string("Script error: ") + r;
    else {
        split(lines[0]);
        if (f[0] != "ok" || f.size() < 6) a.msg = f.size() > 1 ? f[1] : lines[0];
        else {
            a.ok = true;
            a.area = f[1]; a.biome = f[2];
            a.minl = atoi(f[3].c_str()); a.maxl = atoi(f[4].c_str()); a.total = atoi(f[5].c_str());
            for (size_t i = 1; i < lines.size(); i++) {
                split(lines[i]);
                if (f.size() < 6) continue;
                AreaRow w;
                w.dev = atoi(f[0].c_str()); w.form = atoi(f[1].c_str()); w.sex = atoi(f[2].c_str());
                w.minl = atoi(f[3].c_str()); w.maxl = atoi(f[4].c_str()); w.weight = atoi(f[5].c_str());
                a.rows.push_back(w);
            }
        }
    }
    std::lock_guard<std::mutex> l(g_area_lock);
    /* keep the weight boxes the user filled for species still listed */
    for (auto &w : a.rows)
        for (auto &o : g_area.rows)
            if (o.dev == w.dev && o.form == w.form) w.set = o.set;
    g_area = a;
    g_field_status = a.ok ? "" : a.msg;
}

/* the entries allowed at a spawn point near the player, as the game's own filter lists them (unwrapped: the game's
 * weights) */
bool area_list() {
    const NxModHost *h = modkit::host();
    if (h->version < 3 || !h->run_script) return false;
    std::string lua = FIELD_PRELUDE;
    lua += "local info = here()\n"
           "if info == nil then return 'error|no spawn point within 250 m (towns and buildings have none)' end\n"
           "local T = W[26]\n"
           "local f = T.__nx_f2 or T.F2F6C7BBE115AFBCB\n"
           "local list, wts = newarr(), newarr()\n"
           "local ctx = W[28]:FDCCDAF4C1D58AECA(info.pos)\n"
           "local total = f(T, info, ctx, false, list, wts)\n"
           "local out = {'ok|' .. tostring(info.areaname or info.areano) .. '|' .. tostring(info.biome) .. '|' ..\n"
           "  tostring(info.min_level) .. '|' .. tostring(info.max_level) .. '|' .. tostring(total)}\n"
           "for i = 0, list.length - 1 do\n"
           "  local e = list[i]\n"
           "  out[#out + 1] = get(e, 'f2BF366A2') .. '|' .. get(e, 'fE15E9D0A') .. '|' .. get(e, 'f5B8A3FE2') .. '|' ..\n"
           "    get(e, 'f4646F546') .. '|' .. get(e, 'f61FD9277') .. '|' .. tostring(wts[i])\n"
           "end\n"
           "return table.concat(out, '\\n')\n";
    return h->run_script(lua.c_str(), area_done, nullptr);
}

void field_done(void *, bool ok, const char *r) {
    std::string s = r;
    if (s.rfind("error|", 0) == 0) s = s.substr(6);
    field_status(ok ? s : "Script error: " + s);
}

/* One group on the player (dist metres away). pick_dev >= 0: the area's entry with that DevID / form, found again
 * at spawn time (an entry kept from an earlier listing is a native object that may be gone by then: using one
 * crashed); if the point no longer allows it, the game picks. swap_dev >= 0: the members become that
 * DevID / form (and level, if > 0) through the script maker (C2B428A63695FD2C7.S988D7D0A4EEF693D, called by the
 * member maker F316E31E09886ADC9 inside the group maker), wrapped only for this one call. */
bool spawn_on_me(float dist, int pick_dev = -1, int pick_form = 0, int swap_dev = -1, int swap_form = 0, int swap_lv = 0) {
    const NxModHost *h = modkit::host();
    if (h->version < 3 || !h->run_script) return false;
    char head[200];
    snprintf(head, sizeof head, "local dist, pdev, pform, sdev, sform, slv = %.2f, %d, %d, %d, %d, %d\n", dist, pick_dev,
             pick_form, swap_dev, swap_form, swap_lv);
    std::string lua = FIELD_PRELUDE;
    lua += head;
    lua += "if W[17] ~= 0 then return '" NXMOD_SCRIPT_AGAIN "' end\n"
           "local g = W[23]\n"
           "g.bosses:resize(0); g.minions:resize(0)\n"
           "local info, x, y, z = here()\n"
           "if info == nil then return 'error|no spawn point within 250 m (towns and buildings have none)' end\n"
           "local a = math.random() * 6.283185\n"
           "info.pos = {x + dist * math.cos(a), y, z + dist * math.sin(a)}\n"
           "local T = W[26]\n"
           "local found = false\n"
           "if pdev >= 0 then\n"
           "  T.FCEEA7F4134055567 = function(self, inf, rng, ctx, flag)\n"
           "    T.FCEEA7F4134055567 = nil\n"
           "    local list, wts = newarr(), newarr()\n"
           "    local f = T.__nx_f2 or T.F2F6C7BBE115AFBCB\n"
           "    f(T, inf, ctx, flag, list, wts)\n"
           "    for i = 0, list.length - 1 do\n"
           "      local e = list[i]\n"
           "      if get(e, 'f2BF366A2') == pdev and get(e, 'fE15E9D0A') == pform then found = true; return e end\n"
           "    end\n"
           "    return T:FCEEA7F4134055567(inf, rng, ctx, flag)\n"
           "  end\n"
           "end\n"
           "local C = C2B428A63695FD2C7\n"
           "local maker = C and C.S988D7D0A4EEF693D\n"
           "local swapped = 0\n"
           "if sdev >= 0 and maker then\n"
           "  C.S988D7D0A4EEF693D = function(m, dev, form, sex, lv, ...)\n"
           "    swapped = swapped + 1\n"
           "    if slv > 0 then lv = slv end\n"
           "    return maker(m, sdev, sform, sex, lv, ...)\n"
           "  end\n"
           "end\n"
           "local ok, made = pcall(W.F61D1ABB7697071EB, W, info)\n"
           "T.FCEEA7F4134055567 = nil\n"
           "if sdev >= 0 and maker then C.S988D7D0A4EEF693D = maker end\n"
           "if not ok then g.bosses:resize(0); g.minions:resize(0); return 'error|the group maker failed: ' .. tostring(made) end\n"
           "if not made then g.bosses:resize(0); g.minions:resize(0); return 'error|the game refused this spot (no ground under the player, or no entry allowed here); try again or step aside' end\n"
           "local n = g.bosses.length + g.minions.length\n"
           "W:F9B7FBB66FA56765E(g)\n"
           "local how = ''\n"
           "if sdev >= 0 then how = ' (' .. swapped .. ' made as the chosen species)' end\n"
           "if pdev >= 0 and sdev < 0 and not found then how = ' (that entry is not allowed at this spot now: the game picked)' end\n"
           "return 'Spawned a group of ' .. n .. ' at ' .. string.format('%.1f', dist) .. ' m' .. how\n";
    return h->run_script(lua.c_str(), field_done, nullptr);
}

/* rows' weight boxes (-1 as is) and the weight for every other entry (-1 as is): wrap the filter on the instance */
bool weights_apply(bool clear) {
    const NxModHost *h = modkit::host();
    if (h->version < 3 || !h->run_script) return false;
    std::string lua = FIELD_PRELUDE;
    if (clear) lua += "K.__nx_w = nil\nreturn 'Spawn weights back to the game\\'s own'\n";
    else {
        lua += "local w = {}\n";
        std::lock_guard<std::mutex> l(g_area_lock);
        char ln[96];
        int n = 0;
        for (auto &r : g_area.rows)
            if (r.set >= 0) { snprintf(ln, sizeof ln, "w['%d/%d'] = %d\n", r.dev, r.form, r.set); lua += ln; n++; }
        if (g_weight_others >= 0) { snprintf(ln, sizeof ln, "w['*'] = %d\n", g_weight_others); lua += ln; n++; }
        if (!n) return false;
        lua += "K.__nx_w = w\n"
               "local T = W[26]\n"
               "if not T.__nx_f2 then\n"
               "  local orig = T.F2F6C7BBE115AFBCB\n"
               "  T.__nx_f2 = orig\n"
               "  T.F2F6C7BBE115AFBCB = function(self, info, ctx, flag, list, wts)\n"
               "    local total = orig(self, info, ctx, flag, list, wts)\n"
               "    local ov = K.__nx_w\n"
               "    if ov == nil or list == nil or wts == nil then return total end\n"
               "    local t = 0\n"
               "    for i = 0, list.length - 1 do\n"
               "      local e = list[i]\n"
               "      local v = ov[tostring(e:f2BF366A2()) .. '/' .. tostring(e:fE15E9D0A())]\n"
               "      if v == nil then v = ov['*'] end\n"
               "      if v ~= nil then wts[i] = v end\n"
               "      t = t + wts[i]\n"
               "    end\n"
               "    return t\n"
               "  end\n"
               "end\n"
               "return 'Spawn weights set (new spawns; every area until cleared or the save reloads)'\n";
    }
    return h->run_script(lua.c_str(), field_done, nullptr);
}

typedef void (*NoBattles)(int);
NoBattles no_battles_fn() {
    static NoBattles f = (NoBattles)(void *)GetProcAddress(GetModuleHandleW(nullptr), "violet_no_battles");
    return f;
}

/* ---------------------------------------------------------------------------------------------- UI */
std::atomic<bool> g_open;
Spec g_spec;
FilterCombo fc_species, fc_nature, fc_ability, fc_tera, fc_ball, fc_item, fc_moves[4];

const char *move_name(u16 id) {
    for (auto &m : g_moves) if (m.id == id) return m.name.c_str();
    return id ? "?" : "(none)";
}

/* NX_MONS_TEST="<national>[,<form>[,<level>]]": spawn one with default settings as soon as a box block is found
 * (scans every 20 s from 60 s after start), for an unattended check */
void test_thread(std::string arg) {
    int nat = 0, form = 0, level = 50;
    sscanf(arg.c_str(), "%d,%d,%d", &nat, &form, &level);
    Spec p;
    for (size_t i = 0; i < g_species.size(); i++)
        if (g_species[i].national == nat && g_species[i].form == form) p.species = (int)i;
    if (p.species < 0) { modkit::host()->log(0, "mons test: no species %d form %d", nat, form); return; }
    p.level = level;
    p.shiny = true;
    strcpy(p.nickname, "SpawnTest");
    Sleep(60000);
    for (int tries = 0; tries < 60; tries++) {
        scan_start();
        while (g_scanning) Sleep(500);
        std::string msg;
        bool have;
        { std::lock_guard<std::mutex> l(g_lock); have = !g_blocks.empty(); }
        int slot = have ? spawn(p, msg) : -1;
        if (slot >= 0) {
            modkit::host()->log(0, "mons test: %s", msg.c_str());
            /* read it back through the game's own box code: the mon's dev id (the internal index) and form */
            char lua[512];
            snprintf(lua, sizeof lua,
                     "if c1A1CBE3B == nil then return '" NXMOD_SCRIPT_AGAIN "' end\n"
                     "local m = c1A1CBE3B.f6AA17B5F(%d, %d)\n"
                     "if m == nil then return 'slot read: nil' end\n"
                     "return 'slot read: dev id ' .. tostring(m:f0BD5134F()) .. ' form ' .. tostring(m:f5D94E897()) .. ' (expected %u %u)'\n",
                     slot / BOX_SLOTS, slot % BOX_SLOTS, g_species[p.species].internal, g_species[p.species].form);
            if (modkit::host()->version >= 3 && modkit::host()->run_script)
                modkit::host()->run_script(lua, [](void *, bool ok, const char *r) {
                    modkit::host()->log(0, "mons test: %s %s", ok ? "game says" : "game error", r);
                }, nullptr);
            return;
        }
        modkit::host()->log(0, "mons test: not yet (%s)", have ? msg.c_str() : "no box block");
        Sleep(20000);
    }
}

} // namespace

void mons_init(const char *cache_dir) {
    load_tables(cache_dir);
    enc_load(cache_dir);
    if (!g_error.empty()) { modkit::host()->log(0, "mons: %s", g_error.c_str()); return; }
    modkit::host()->log(1, "mons: %zu species / forms, %zu moves", g_species.size(), g_moves.size());
    for (size_t i = 0; i < g_species.size(); i++) if (g_species[i].national == 25 && !g_species[i].form) g_spec.species = (int)i;
    const char *t = getenv("NX_MONS_TEST");
    if (t && *t) std::thread(test_thread, std::string(t)).detach();
}

bool mons_open() { return g_open; }
void mons_toggle() {
    g_open = !g_open;
    if (g_open && g_error.empty()) scan_start();    /* every time it opens: the boxes move between loads */
}

void mons_ui() {
    if (!g_open) return;
    ImGui::SetNextWindowSize(ImVec2(640, 700), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("Spawn a Pokemon", &open)) { ImGui::End(); if (!open) g_open = false; return; }
    if (!open) g_open = false;
    if (!g_error.empty()) { ImGui::TextWrapped("%s", g_error.c_str()); ImGui::End(); return; }

    if (g_scanning) ImGui::ProgressBar((float)((double)g_done / (double)g_total), ImVec2(240, 0), "scanning memory");
    else if (ImGui::Button("Scan memory for the boxes")) scan_start();
    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> l(g_lock);
        ImGui::TextUnformatted(g_scan_msg.empty() ? "Load a save, then scan." : g_scan_msg.c_str());
    }

    Spec &p = g_spec;
    const Species *s = p.species >= 0 ? &g_species[p.species] : nullptr;
    ImGui::SetNextItemWidth(320);
    if (fc_species.begin("Species", s ? s->label.c_str() : "(pick one)")) {
        for (int i = 0; i < (int)g_species.size(); i++)
            if (fc_species.item(g_species[i].label.c_str(), i == p.species)) {
                p.species = i;
                p.ability_slot = 0;
                p.tera = -1;
            }
        fc_species.end();
    }
    if (!s) { ImGui::End(); return; }
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Level", &p.level);
    p.level = p.level < 1 ? 1 : p.level > 100 ? 100 : p.level;
    ImGui::SameLine();
    ImGui::Checkbox("Shiny", &p.shiny);
    if (s->gender != 0 && s->gender != 254 && s->gender != 255) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::Combo("Gender", &p.gender, "Male\0Female\0");
    }
    ImGui::SetNextItemWidth(160);
    if (fc_nature.begin("Nature", natures[p.nature])) {
        for (int k = 0; k < 25; k++) if (fc_nature.item(natures[k], p.nature == k)) p.nature = k;
        fc_nature.end();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    {
        auto an = [&](int k) { auto it = g_ability_names.find(s->ability[k]); return it != g_ability_names.end() ? it->second : std::string("?"); };
        std::string cur = an(p.ability_slot) + (p.ability_slot == 2 ? " (H)" : "");
        if (fc_ability.begin("Ability", cur.c_str(), 0)) {
            for (int k = 0; k < 3; k++) {
                std::string t = an(k) + (k == 2 ? " (H)" : ""), l = t + "##" + std::to_string(k);
                if (fc_ability.item(t.c_str(), p.ability_slot == k, l.c_str())) p.ability_slot = k;
            }
            fc_ability.end();
        }
    }
    ImGui::SetNextItemWidth(160);
    {
        const char *tp = p.tera < 0 ? types[s->type1 < 18 ? s->type1 : 0] : p.tera == 18 ? "Stellar" : types[p.tera];
        if (fc_tera.begin("Tera type", tp)) {
            for (int k = 0; k < 19; k++)
                if (fc_tera.item(k == 18 ? "Stellar" : types[k], (p.tera < 0 ? s->type1 : p.tera) == k)) p.tera = k;
            fc_tera.end();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    {
        const char *bn = "?";
        for (auto &b : g_balls) if (b.first == p.ball) bn = b.second.c_str();
        if (fc_ball.begin("Ball", bn)) {
            for (auto &b : g_balls) if (fc_ball.item(b.second.c_str(), b.first == p.ball)) p.ball = b.first;
            fc_ball.end();
        }
    }
    if (!g_held.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        const char *in = "(none)";
        for (auto &it : g_held) if (it.first == p.item) in = it.second.c_str();
        if (fc_item.begin("Held item", in)) {
            if (fc_item.item("(none)", !p.item)) p.item = 0;
            for (auto &it : g_held) if (fc_item.item(it.second.c_str(), it.first == p.item)) p.item = it.first;
            fc_item.end();
        }
    }
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("Nickname", s->name.c_str(), p.nickname, sizeof p.nickname);

    ImGui::SeparatorText("IVs / EVs");
    int ev_total = 0;
    for (int i = 0; i < 6; i++) ev_total += p.ev[i];
    if (ImGui::BeginTable("stats", 4, ImGuiTableFlags_SizingFixedFit)) {
        Spec q = p;
        for (int i = 0; i < 6; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(stat_names[i]);
            /* typed boxes (no +/- buttons), clamped to the game's ranges */
            ImGui::TableNextColumn(); ImGui::TextDisabled("IV"); ImGui::SameLine(); ImGui::SetNextItemWidth(60);
            ImGui::InputInt((std::string("##iv") + stat_names[i]).c_str(), &p.iv[i], 0, 0);
            p.iv[i] = p.iv[i] < 0 ? 0 : p.iv[i] > 31 ? 31 : p.iv[i];
            ImGui::TableNextColumn(); ImGui::TextDisabled("EV"); ImGui::SameLine(); ImGui::SetNextItemWidth(60);
            ImGui::InputInt((std::string("##ev") + stat_names[i]).c_str(), &p.ev[i], 0, 0);
            p.ev[i] = p.ev[i] < 0 ? 0 : p.ev[i] > 252 ? 252 : p.ev[i];
            ImGui::TableNextColumn(); ImGui::Text("%u", stat(*s, q, i));
        }
        ImGui::EndTable();
    }
    if (ev_total > 510) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "EV total %d (max 510)", ev_total);
    else ImGui::TextDisabled("EV total %d / 510", ev_total);

    ImGui::SeparatorText("Moves");
    for (int k = 0; k < 4; k++) {
        ImGui::PushID(k);
        ImGui::SetNextItemWidth(260);
        FilterCombo &fm = fc_moves[k];
        if (fm.begin("##move", move_name(p.moves[k]))) {
            if (fm.item("(none)", !p.moves[k])) p.moves[k] = 0;
            for (auto &m : g_moves)
                if (fm.item(m.name.c_str(), m.id == p.moves[k])) p.moves[k] = m.id;
            fm.end();
        }
        ImGui::PopID();
        if (k % 2 == 0) ImGui::SameLine();
    }

    static int lv_mode = 0, shiny_mode = 2;   /* shared by the wild spawns and "Spawn <species> on me" */
    static bool all = true;
    ImGui::SeparatorText("Wild spawns");
    {
        static int pct = 100;
        ImGui::RadioButton("Level: the area's", &lv_mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("the level above", &lv_mode, 1);
        ImGui::SetNextItemWidth(140);
        ImGui::Combo("Shiny##wild", &shiny_mode, "As rolled\0Never\0Always\0");
        ImGui::SameLine();
        ImGui::Checkbox("Use the stats above", &all);
        ImGui::SetNextItemWidth(200);
        ImGui::SliderInt("of wild spawns", &pct, 0, 100, "%d%%");
        if (ImGui::Button("Apply to wild spawns")) {
            bool spec = wild_spec_apply(p, lv_mode ? p.level : 0, shiny_mode, all, pct);
            bool lua = wild_lua(s->internal, s->form, lv_mode ? p.level : 0, pct);
            std::string msg = !spec ? "No wild spawn hook in this build (violet_wild_set missing from the exe)."
                              : !lua ? "No script runner (the Lua bridge): only the spec, no species swap."
                              : "Wild spawns: " + s->label + ", " + std::to_string(pct) + "% of new spawns. Walk to fresh ground.";
            std::lock_guard<std::mutex> l(g_status_lock);
            g_wild_status = msg;
        }
        ImGui::SameLine();
        if (ImGui::Button("Restore")) {
            std::string msg;
            if (WildSet f = wild_set_fn()) f(nullptr);
            wild_lua(-1, 0, 0, 0);
            msg = "Wild spawns back to normal (new spawns).";
            std::lock_guard<std::mutex> l(g_status_lock);
            g_wild_status = msg;
        }
        ImGui::SameLine();
        if (ImGui::Button("Count")) {
            char lua[256];
            snprintf(lua, sizeof lua, "local C = %s\nif C == nil then return 'no class' end\n"
                     "return 'swapped ' .. tostring(C.__nx_count) .. ', last was ' .. tostring(C.__nx_last)\n", WILD_CLASS);
            if (modkit::host()->run_script) modkit::host()->run_script(lua, wild_lua_done, nullptr);
        }
        ImGui::TextDisabled("Until restored. Also affects other %s (raids, trades, eggs).", s->name.c_str());
        std::lock_guard<std::mutex> l(g_status_lock);
        if (!g_wild_status.empty()) ImGui::TextWrapped("%s", g_wild_status.c_str());
    }

    ImGui::SeparatorText("Around you");
    {
        static float dist = 3.0f;
        if (ImGui::Button("List spawns")) {
            if (!area_list()) field_status("No Lua bridge in this build.");
            else field_status("Listing...");
        }
        ImGui::SameLine();
        if (ImGui::Button("Spawn here")) {
            if (!spawn_on_me(dist)) field_status("No Lua bridge in this build.");
        }
        ImGui::SameLine();
        if (ImGui::Button(("Spawn " + s->name).c_str())) {
            /* the spec (shiny, IVs, ... as in Wild spawns above) stays on for this species until Restore wild spawns */
            wild_spec_apply(p, lv_mode ? p.level : 0, shiny_mode, all, 0);
            if (!spawn_on_me(dist, -1, 0, s->internal, s->form, lv_mode ? p.level : 0))
                field_status("No Lua bridge in this build.");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("Distance", &dist, 0.0f, 15.0f, "%.1f m");
        static bool no_battles = false;
        if (ImGui::Checkbox("No contact battles (dev)", &no_battles)) {
            if (NoBattles f = no_battles_fn()) f(no_battles ? 1 : 0);
            else field_status("No battle hook in this build.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(new spawns)");
        if (dist < 2.0f && !no_battles)
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Under 2 m may start a battle.");

        std::lock_guard<std::mutex> l(g_area_lock);
        if (g_area.ok) {
            ImGui::Text("%s, %s, Lv. %d-%d, %zu entries (weight %d)", g_area.area.c_str(),
                        g_area.biome.c_str(), g_area.minl, g_area.maxl, g_area.rows.size(), g_area.total);
            if (ImGui::BeginTable("##area", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                                  ImVec2(0, 260))) {
                ImGui::TableSetupColumn("Species", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Levels");
                ImGui::TableSetupColumn("Chance");
                ImGui::TableSetupColumn("Weight");
                ImGui::TableSetupColumn("");
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < g_area.rows.size(); i++) {
                    AreaRow &r = g_area.rows[i];
                    ImGui::PushID((int)i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const char *name = species_label(r.dev, r.form);
                    if (name) ImGui::TextUnformatted(name);
                    else ImGui::Text("DevID %d form %d", r.dev, r.form);
                    if (r.sex == 0 || r.sex == 1) { ImGui::SameLine(); ImGui::TextDisabled(r.sex ? "(F)" : "(M)"); }
                    ImGui::TableNextColumn();
                    ImGui::Text("%d-%d", r.minl, r.maxl);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f%%", g_area.total > 0 ? 100.0 * r.weight / g_area.total : 0.0);
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(90);
                    ImGui::InputInt("##w", &r.set, 1, 10);
                    if (r.set < -1) r.set = -1;
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Spawn")) spawn_on_me(dist, r.dev, r.form);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("Others' weight", &g_weight_others, 1, 10);
            if (g_weight_others < -1) g_weight_others = -1;
            if (ImGui::Button("Apply weights")) {
                g_area_lock.unlock();
                if (!weights_apply(false)) field_status("Set a weight on a row first (0 = never).");
                g_area_lock.lock();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear weights")) {
                for (auto &r : g_area.rows) r.set = -1;
                g_weight_others = -1;
                g_area_lock.unlock();
                weights_apply(true);
                g_area_lock.lock();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("-1 = unchanged, 0 = never");
        }
        if (!g_field_status.empty()) ImGui::TextWrapped("%s", g_field_status.c_str());
    }

    ImGui::Separator();
    if (ImGui::Button("Spawn into the boxes")) {
        std::string msg;
        spawn(p, msg);
        set_status(msg);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(first free box slot)");
    {
        std::lock_guard<std::mutex> l(g_status_lock);
        if (!g_status.empty()) ImGui::TextWrapped("%s", g_status.c_str());
    }
    ImGui::End();
}

/* for svedit `check`: the PK9 these settings make, encrypted, to a file (no game needed) */
extern "C" __declspec(dllexport) int mons_selftest(const char *cache_dir, const char *out, int national, int form, int level) {
    if (g_species.empty()) load_tables(cache_dir);
    Spec p;
    for (size_t i = 0; i < g_species.size(); i++)
        if (g_species[i].national == national && g_species[i].form == form) p.species = (int)i;
    if (p.species < 0) return 1;
    p.level = level; p.nature = 15; p.ability_slot = 2; p.shiny = true;
    p.ev[0] = 252; p.ev[3] = 252;
    for (auto &m : g_moves) {
        if (m.name == "Lumina Crash") p.moves[0] = m.id;
        if (m.name == "Calm Mind") p.moves[1] = m.id;
        if (m.name == "Feather Dance") p.moves[2] = m.id;
        if (m.name == "Roost") p.moves[3] = m.id;
    }
    strcpy(p.nickname, "SelfTest");
    Trainer t;
    put_name(t.name, "Tester");
    t.tid = 12345; t.sid = 54321;
    t.ok = true;
    u8 dec[SLOT], enc[SLOT], back[SLOT];
    build(p, t, dec);
    encrypt(dec, enc);
    decrypt(enc, back);
    if (memcmp(dec, back, SLOT) || !slot_valid(enc)) return 2;
    FILE *f = fopen(out, "wb");
    if (!f) return 3;
    fwrite(enc, 1, SLOT, f);
    fclose(f);
    return 0;
}

/* the Paldea wild table as "Wild spawns" would patch it, to a file (no game needed; read it back with a FlatBuffers
 * reader: every entry the species / form / level asked for) */
extern "C" __declspec(dllexport) int mons_wild_selftest(const char *cache_dir, const char *out, int national, int form, int level) {
    if (g_species.empty()) load_tables(cache_dir);
    if (g_enc[0].orig.empty()) enc_load(cache_dir);
    EncTable &t = g_enc[0];
    if (t.orig.empty()) return 1;
    int sp = -1;
    for (size_t i = 0; i < g_species.size(); i++)
        if (g_species[i].national == national && g_species[i].form == form) sp = (int)i;
    if (sp < 0) return 2;
    std::vector<u8> mem = t.orig;           /* a "guest" copy in our own memory */
    t.found = {(u64)(uintptr_t)mem.data()};
    std::string msg;
    enc_apply(sp, level, msg);
    std::vector<u8> chk;
    if (!enc_at(t, t.found[0], chk)) return 3;
    t.found.clear();
    FILE *f = fopen(out, "wb");
    if (!f) return 4;
    fwrite(mem.data(), 1, mem.size(), f);
    fclose(f);
    return 0;
}

/* ---------------------------------------------------------------------------------------------- the mod */
static void mod_ui() { mons_ui(); }

static const NxMod mod = {"Spawn a Pokemon", nullptr, nullptr, nullptr, "1", "Write a made-to-order Pokemon into the boxes, make wild spawns a chosen species, spawn groups on the player, list and weight the area's spawns. Needs build\\mods\\mons_*.txt (svedit monsdata) and the wild hooks in the exe.", mod_ui};

extern "C" __declspec(dllexport) const NxMod *nx_mod_init(const NxModHost *host) {
    if (host->version != NXMOD_VERSION) return NULL;
    if (strcmp(host->title_id, "01008F6008C5E000") && strcmp(host->title_id, "0100A3D008C5C000")) return NULL;
    if (!modkit::init(host, "mons")) return NULL;
    mons_init(host->cache_dir);
    modkit::window("Spawn a Pokemon", "F8", mons_open, mons_toggle);
    return &mod;
}
