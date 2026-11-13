// WVC 0.12: the layout of the panels under the preview (order, shown / collapsed, widths, height), kept in
// settings.ini [Layout]. The drawing and the mouse handling live in ui_main.cpp.
#include "common.h"

DockState g_docks;

static const int kDefWeight[DOCK_COUNT] = {300, 220, 260, 190, 180};

void DocksReset() {
    for (int i = 0; i < DOCK_COUNT; ++i) {
        g_docks.order[i] = i;
        g_docks.vis[i] = true;
        g_docks.coll[i] = false;
        g_docks.weight[i] = kDefWeight[i];
    }
    g_docks.height = 280;
}

static std::vector<int> Ints(const std::wstring& s) {
    std::vector<int> v;
    size_t p = 0;
    while (p <= s.size()) {
        const size_t q = s.find(L',', p);
        const std::wstring t = s.substr(p, q == std::wstring::npos ? std::wstring::npos : q - p);
        if (!t.empty()) v.push_back(_wtoi(t.c_str()));
        if (q == std::wstring::npos) break;
        p = q + 1;
    }
    return v;
}

static std::wstring Join(const int* a, int n) {
    std::wstring s;
    for (int i = 0; i < n; ++i) s += (i ? L"," : L"") + std::to_wstring(a[i]);
    return s;
}

void DocksLoad() {
    DocksReset();
    const std::wstring sec = L"Layout";
    if (!g_ini.HasSection(sec)) return;
    std::vector<int> ord = Ints(g_ini.Str(sec, L"Order")), vis = Ints(g_ini.Str(sec, L"Visible")),
                     col = Ints(g_ini.Str(sec, L"Collapsed")), w = Ints(g_ini.Str(sec, L"Widths"));
    // the order must be a permutation of 0..4
    bool seen[DOCK_COUNT] = {};
    bool okOrder = ord.size() == DOCK_COUNT;
    for (int v : ord) {
        if (v < 0 || v >= DOCK_COUNT || seen[v]) okOrder = false;
        else seen[v] = true;
    }
    if (okOrder)
        for (int i = 0; i < DOCK_COUNT; ++i) g_docks.order[i] = ord[static_cast<size_t>(i)];
    for (int i = 0; i < DOCK_COUNT; ++i) {
        if (i < static_cast<int>(vis.size())) g_docks.vis[i] = vis[static_cast<size_t>(i)] != 0;
        if (i < static_cast<int>(col.size())) g_docks.coll[i] = col[static_cast<size_t>(i)] != 0;
        if (i < static_cast<int>(w.size())) g_docks.weight[i] = std::clamp(w[static_cast<size_t>(i)], 60, 4000);
    }
    g_docks.height = std::clamp(g_ini.Int(sec, L"Height", 280), 120, 2000);
}

void DocksSave() {
    const std::wstring sec = L"Layout";
    int vis[DOCK_COUNT], col[DOCK_COUNT];
    for (int i = 0; i < DOCK_COUNT; ++i) {
        vis[i] = g_docks.vis[i] ? 1 : 0;
        col[i] = g_docks.coll[i] ? 1 : 0;
    }
    g_ini.Set(sec, L"Order", Join(g_docks.order, DOCK_COUNT));
    g_ini.Set(sec, L"Visible", Join(vis, DOCK_COUNT));
    g_ini.Set(sec, L"Collapsed", Join(col, DOCK_COUNT));
    g_ini.Set(sec, L"Widths", Join(g_docks.weight, DOCK_COUNT));
    g_ini.SetInt(sec, L"Height", g_docks.height);
    g_projDirty = true;  // written with the next save
}
