/* A dropdown whose list filters as you type: opening it puts the keyboard in a search box at its top (emptied each
 * time it opens), Enter picks the first match, and the current choice is scrolled into view.
 *
 *   FilterCombo fc;
 *   if (fc.begin("Species", preview)) {
 *       for (...) if (fc.item(name, selected)) choose(...);
 *       fc.end();
 *   } */
#pragma once
#include <ctype.h>
#include <imgui.h>

inline bool filter_match(const char *s, const char *f) {    /* case-insensitive substring; an empty filter matches */
    if (!*f) return true;
    for (; *s; s++) {
        size_t k = 0;
        while (f[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)f[k])) k++;
        if (!f[k]) return true;
    }
    return false;
}

struct FilterCombo {
    char filter[48] = "";
    bool enter = false, any = false, appearing = false;

    bool begin(const char *label, const char *preview, ImGuiComboFlags flags = ImGuiComboFlags_HeightLarge) {
        if (!ImGui::BeginCombo(label, preview, flags)) return false;
        appearing = ImGui::IsWindowAppearing();
        if (appearing) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(-1);
        enter = ImGui::InputTextWithHint("##filter", "type to filter", filter, sizeof filter,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
        any = false;
        return true;
    }
    /* one entry: true when chosen (clicked, or the first match when Enter was pressed) */
    bool item(const char *text, bool selected, const char *shown = nullptr) {
        if (!filter_match(text, filter)) return false;
        bool first = !any;
        any = true;
        bool hit = ImGui::Selectable(shown ? shown : text, selected);
        if (selected && appearing) ImGui::SetScrollHereY();
        if (!hit && enter && first) { hit = true; ImGui::CloseCurrentPopup(); }
        return hit;
    }
    void end() { ImGui::EndCombo(); }
};
