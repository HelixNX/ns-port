/* Item giver (F7), part of the raids mod's DLL (one ImGui context for both windows).
 *
 * The bag lives in the save's item block (PKHeX: KItem 0x21C9BD44), which the game keeps current in memory:
 * 3,000 entries of 0x10 bytes indexed by item id, each {u32 pocket, u32 count, u32 flags, u32 0}; flags bit 0 new,
 * bit 1 favourite, bit 2 obtained. The game writes it when items come and go and saves it as it is, but the bag
 * screen works from the script side's pocket lists, built when the save loads.
 *
 * Giving goes through the game's own bag code instead (host->run_script, the runner in violetrecomp/lua.c): the bag
 * singleton C14A8D35D72F56FE9.S90D73F1D715BCBF4() (main.blua, Haxe -> Lua 5.4, obfuscated names), whose
 * FB01A9B29CD4CC6CB(id, n) adds to the pocket list and the save entry (clamped to the item's maximum, new items
 * appended and flagged), FE027A297CCA14276(id, n) takes n away, F13D72EF77582090D(id) is the item's index in the
 * list (-1: not in it) and self[6][index].num its count. Shows at once. The direct write stays as the fallback (no
 * runner): that shows after saving and restarting.
 *
 * The block is found by scanning guest memory for an array of that shape that holds the Rotom Phone (every save has
 * it from the start) with every held item in its own pocket, as the raid blocks are. Item names and pockets:
 * <build>\mods\items.txt, lines of "id \t pocket \t name" (violetrecomp\tools\svedit `items`, PKHeX.Core, for now;
 * to come from the game's own item data). */
#include "items.h"
#include "modkit.h"
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

typedef uint32_t u32;
typedef uint64_t u64;

namespace {

enum { N_ITEMS = 3000, ROTOM_PHONE = 1829, POCKET_KEY = 8, MAX_COUNT = 999 };
const char *const pocket_names[] = {"Medicine", "Poke Balls", "Battle Items", "Berries", "Other Items",
                                    "TMs", "Treasures", "Picnic Items", "Key Items", "Materials"};
const int N_POCKETS = (int)(sizeof pocket_names / sizeof *pocket_names);

struct Entry { u32 pocket, count, flags, pad; };
static_assert(sizeof(Entry) == 0x10, "item entry");

struct Item { u32 id; int pocket; std::string name; };
std::vector<Item> g_items;
int g_pocket_of[N_ITEMS];                   /* from items.txt; -1: not listed */
std::string g_error;

std::mutex g_lock;
std::vector<u64> g_bags;                    /* where each copy of the item array starts */
std::atomic<bool> g_scanning;
std::atomic<u64> g_done, g_total;
std::string g_scan_msg;
std::mutex g_status_lock;                   /* g_status: also set from the game's thread (script results) */
std::string g_status;

void set_status(const std::string &s) {
    std::lock_guard<std::mutex> l(g_status_lock);
    g_status = s;
}

bool mem_read(u64 a, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, out, n, &got) && got == n;
}

bool mem_write(u64 a, const void *in, size_t n) {
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)a, in, n, &put) && put == n;
}

void load_items(const char *cache_dir) {
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\mods\\items.txt", cache_dir);
    for (int &p : g_pocket_of) p = -1;
    FILE *f = fopen(path, "rb");
    if (!f) { g_error = std::string("no ") + path; return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned id = 0;
        int pocket = 0, at = 0;
        if (sscanf(line, "%u\t%d\t%n", &id, &pocket, &at) < 2 || !at || id >= N_ITEMS || pocket < 0 || pocket >= N_POCKETS)
            continue;
        std::string name(line + at);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        g_items.push_back({id, pocket, name});
        g_pocket_of[id] = pocket;
    }
    fclose(f);
    if (g_items.empty()) g_error = std::string(path) + " has no items";
}

bool entry_shape(const Entry &e) {
    if (!(e.pocket | e.count | e.flags | e.pad)) return true;
    return e.pocket < (u32)N_POCKETS + 2 && e.count <= MAX_COUNT && e.flags < 8 && e.pad == 0;
}

/* a whole array at `a`: every entry item-shaped, the Rotom Phone held in the key pocket, and every held item in its
 * own pocket (without that, the array shifted by an entry or more passes too: Miraidon's ball and the Tera Orb sit a
 * few entries after the Rotom Phone and look like it) */
bool bag_at(u64 a, std::vector<Entry> &tmp) {
    tmp.resize(N_ITEMS);
    if (!mem_read(a, tmp.data(), N_ITEMS * sizeof(Entry))) return false;
    const Entry &r = tmp[ROTOM_PHONE];
    if (r.pocket != POCKET_KEY || r.count != 1) return false;
    for (u32 i = 0; i < N_ITEMS; i++) {
        const Entry &e = tmp[i];
        if (!entry_shape(e)) return false;
        if (e.count && g_pocket_of[i] >= 0 && e.pocket != (u32)g_pocket_of[i]) return false;
    }
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
    std::vector<u64> found;
    std::vector<Entry> tmp;
    const size_t CH = 16 << 20;
    std::vector<unsigned char> buf(CH);
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
                g_done += n;
                if (!mem_read(c, buf.data(), n)) continue;
                /* the Rotom Phone's entry {8, 1, flags, 0} at 16-byte alignment, then the array around it */
                for (size_t i = 0; i + sizeof(Entry) <= n; i += sizeof(Entry)) {
                    const Entry *e = (const Entry *)&buf[i];
                    if (e->pocket != POCKET_KEY || e->count != 1 || e->pad || e->flags >= 8) continue;
                    u64 base = c + i - (u64)ROTOM_PHONE * sizeof(Entry);
                    if (base >= lo && bag_at(base, tmp)) found.push_back(base);
                }
            }
        a = rend;
    }
    char msg[128];
    snprintf(msg, sizeof msg, "Found %zu bag%s (%.1f GB scanned)", found.size(), found.size() == 1 ? "" : "s",
             (double)g_done / (1 << 30));
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_bags = found;
        g_scan_msg = msg;
    }
    modkit::host()->log(0, "items: %s", msg);
    for (u64 b : found) modkit::host()->log(1, "items: bag at %llx", (unsigned long long)b);
    g_scanning = false;
}

/* always a whole scan (the window starts one each time it opens): a copy found earlier can be stale */
void scan_start() {
    if (g_scanning.exchange(true)) return;
    std::thread(scan_thread).detach();
}

/* the bag from its first copy still in place (false: none, scan again) */
bool read_bag(std::vector<Entry> &out) {
    std::lock_guard<std::mutex> l(g_lock);
    for (u64 b : g_bags) if (bag_at(b, out)) return true;
    return false;
}

/* one item's count into every copy still in place; returns the copies written */
int set_count(const Item &it, u32 count) {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<Entry> tmp;
    int n = 0;
    for (u64 b : g_bags) {
        if (!bag_at(b, tmp)) continue;
        Entry e = tmp[it.id];
        if (count) {
            e.pocket = (u32)it.pocket;
            e.flags |= e.count ? 0 : 1;     /* new to the bag: marked new */
            e.flags |= 4;                   /* obtained */
        }
        e.count = count;
        if (mem_write(b + (u64)it.id * sizeof(Entry), &e, sizeof e)) n++;
    }
    return n;
}

/* Give (add) / Set through the game's bag code: false when there is no script runner */
struct GiveJob { std::string name; };

void give_done(void *ud, bool ok, const char *result) {
    GiveJob *j = (GiveJob *)ud;
    set_status(ok ? j->name + ": " + result + " in the bag." : j->name + ": the game's bag code failed: " + result);
    delete j;
}

bool give_script(const Item &it, u32 amount, bool set) {
    if (modkit::host()->version < 3 || !modkit::host()->run_script) return false;
    char text[1024];
    snprintf(text, sizeof text,
             "local id, n, set = %u, %u, %s\n"
             "local cls = C14A8D35D72F56FE9\n"
             "if cls == nil then return '" NXMOD_SCRIPT_AGAIN "' end\n"
             "local b = cls.S90D73F1D715BCBF4()\n"
             "if b == nil or b[6] == nil then return '" NXMOD_SCRIPT_AGAIN "' end\n"
             "local function held() local i = b:F13D72EF77582090D(id) if i >= 0 then return b[6][i].num end return 0 end\n"
             "local h = held()\n"
             "local want = set and n or h + n\n"
             "if want > %d then want = %d end\n"
             "if want > h then b:FB01A9B29CD4CC6CB(id, want - h) elseif want < h then b:FE027A297CCA14276(id, h - want) end\n"
             "return tostring(held())\n",
             it.id, amount, set ? "true" : "false", (int)MAX_COUNT, (int)MAX_COUNT);
    GiveJob *j = new GiveJob{it.name};
    if (!modkit::host()->run_script(text, give_done, j)) { delete j; return false; }
    set_status(it.name + ": sent to the game...");
    return true;
}

/* the fallback: the save entry only */
void give_direct(const Item &it, u32 count) {
    int w = set_count(it, count);
    set_status(it.name + ": " + std::to_string(count) + " (" + std::to_string(w) + " cop" + (w == 1 ? "y" : "ies") +
               " written). Save, then restart to see it.");
}

bool contains_nocase(const char *s, const char *sub) {
    for (; *s; s++) {
        size_t k = 0;
        while (sub[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)sub[k])) k++;
        if (!sub[k]) return true;
    }
    return false;
}

std::atomic<bool> g_open;
char g_filter[64];
int g_pocket = -1, g_sel = -1, g_amount = 1;
bool g_owned_only;
std::vector<Entry> g_bag;
char g_lua[2048] = "return tostring(C14A8D35D72F56FE9 ~= nil)";

void lua_done(void *, bool ok, const char *result) { set_status(std::string(ok ? "Lua: " : "Lua error: ") + result); }

} // namespace

void items_init(const char *cache_dir) {
    load_items(cache_dir);
    if (!g_error.empty()) modkit::host()->log(0, "items: %s", g_error.c_str());
    else modkit::host()->log(1, "items: %zu items", g_items.size());
}

bool items_open() { return g_open; }
void items_toggle() {
    g_open = !g_open;
    if (g_open && g_error.empty()) scan_start();    /* every time it opens: the bag moves between loads */
}

void items_ui() {
    if (!g_open) return;
    ImGui::SetNextWindowSize(ImVec2(620, 620), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("Items (F7)", &open)) { ImGui::End(); if (!open) g_open = false; return; }
    if (!open) g_open = false;
    if (!g_error.empty()) { ImGui::TextWrapped("%s", g_error.c_str()); ImGui::End(); return; }

    if (g_scanning) ImGui::ProgressBar((float)((double)g_done / (double)g_total), ImVec2(240, 0), "scanning memory");
    else if (ImGui::Button("Scan memory for the bag")) scan_start();
    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> l(g_lock);
        ImGui::TextUnformatted(g_scan_msg.empty() ? "Load into the overworld, then scan." : g_scan_msg.c_str());
    }
    bool have = read_bag(g_bag);
    if (!have) ImGui::TextDisabled("No bag in memory: scan (again after loading a save).");
    else if (modkit::host()->version >= 2 && ImGui::SmallButton("Debug: log writes to the Poke Ball count")) {
        std::lock_guard<std::mutex> l(g_lock);
        u64 a = g_bags[0] + 4 * sizeof(Entry) + 4;
        modkit::host()->watch_mem(a, 4);
        set_status("Watching the Poke Ball count: pick up or use a Poke Ball (the log lists the code that writes it).");
    }
    bool script = modkit::host()->version >= 3 && modkit::host()->run_script;
    if (!script) ImGui::TextWrapped("Given items are written into the save's bag: save, then restart the game to see them.");

    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##filter", "search items", g_filter, sizeof g_filter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("##pocket", g_pocket < 0 ? "every pocket" : pocket_names[g_pocket])) {
        if (ImGui::Selectable("every pocket", g_pocket < 0)) g_pocket = -1;
        for (int p = 0; p < N_POCKETS; p++)
            if (ImGui::Selectable(pocket_names[p], g_pocket == p)) g_pocket = p;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Held only", &g_owned_only);

    if (ImGui::BeginTable("items", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, 380))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("Pocket");
        ImGui::TableSetupColumn("Held", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)g_items.size(); i++) {
            const Item &it = g_items[i];
            u32 held = have ? g_bag[it.id].count : 0;
            if (g_pocket >= 0 && it.pocket != g_pocket) continue;
            if (g_owned_only && !held) continue;
            if (g_filter[0] && !contains_nocase(it.name.c_str(), g_filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char lbl[160];
            snprintf(lbl, sizeof lbl, "%s##%u", it.name.c_str(), it.id);
            if (ImGui::Selectable(lbl, g_sel == i, ImGuiSelectableFlags_SpanAllColumns)) g_sel = i;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(pocket_names[it.pocket]);
            ImGui::TableNextColumn();
            if (held) ImGui::Text("%u", held);
        }
        ImGui::EndTable();
    }

    if (g_sel >= 0 && g_sel < (int)g_items.size()) {
        const Item &it = g_items[g_sel];
        u32 held = have ? g_bag[it.id].count : 0;
        ImGui::SeparatorText(it.name.c_str());
        ImGui::Text("Held: %u   (item %u, %s)", held, it.id, pocket_names[it.pocket]);
        if (it.pocket == POCKET_KEY)
            ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "A key item: giving one early can skip or confuse story events.");
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("Amount", &g_amount);
        if (g_amount < 0) g_amount = 0;
        if (g_amount > MAX_COUNT) g_amount = MAX_COUNT;
        if (have || script) {
            ImGui::SameLine();
            if (ImGui::Button("Give")) {
                u32 n = held + (u32)g_amount;
                if (!give_script(it, (u32)g_amount, false) && have) give_direct(it, n > MAX_COUNT ? MAX_COUNT : n);
            }
            ImGui::SameLine();
            if (ImGui::Button("Set to amount")) {
                if (!give_script(it, (u32)g_amount, true) && have) give_direct(it, (u32)g_amount);
            }
            ImGui::SameLine();
            if (ImGui::Button("Give 999")) {
                if (!give_script(it, MAX_COUNT, true) && have) give_direct(it, MAX_COUNT);
            }
        }
    }
    if (script && ImGui::CollapsingHeader("Lua (the game's VM)")) {
        ImGui::InputTextMultiline("##lua", g_lua, sizeof g_lua, ImVec2(-1, 90));
        if (ImGui::Button("Run")) modkit::host()->run_script(g_lua, lua_done, nullptr);
        ImGui::SameLine();
        ImGui::TextDisabled("its first result, as text, shows below (and in the log)");
    }
    {
        std::lock_guard<std::mutex> l(g_status_lock);
        if (!g_status.empty()) ImGui::TextWrapped("%s", g_status.c_str());
    }
    ImGui::End();
}
