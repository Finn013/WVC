// WVC 0.12: undo / redo (Ctrl+Z / Ctrl+Y). Snapshot based: the UI timer watches the project; when it has been
// steady for 0.4 s after a change (a whole drag, a whole Properties session, add + auto-fit), the state before the
// change becomes one undo step. Covers placement, sources added / removed / reordered / hidden, every property of
// sources and scenes, the camera frame settings. Switching scenes is not a step. At most 50 steps.
#include "common.h"

namespace {
struct Snap {
    std::vector<Scene> scenes;
    int cur = 0, next = -1, nextUid = 1;
    OutputCfg out;
    std::wstring key;
};

std::deque<Snap> g_undo, g_redo;
Snap g_committed;
bool g_haveCommitted = false;
int g_seenVer = -1, g_committedVer = -1;
ULONGLONG g_changedAt = 0;
const size_t kMaxSteps = 50;

std::wstring KeyOf(const Project& p) {
    IniDoc d;
    ProjectToIni(p, &d);
    std::wstring k;
    for (const auto& sec : d.secs) {
        const bool main = sec.first == L"WVC";
        for (const auto& kv : sec.second) {
            if (kv.first == L"NeedFit" || kv.first == L"PhoneOrient") continue;  // set by WVC itself
            if (main && (kv.first == L"CurScene" || kv.first == L"NextScene" || kv.first == L"NextUid")) continue;
            if (main && (kv.first.rfind(L"Out", 0) != 0 && kv.first != L"Resolution" && kv.first != L"Aspect" &&
                         kv.first != L"FPS"))
                continue;  // app-wide settings are not part of undo
            k += sec.first + L"|" + kv.first + L"=" + kv.second + L"\n";
        }
    }
    return k;
}

Snap SnapOf(const Project& p, const std::wstring& key) {
    Snap s;
    s.scenes = p.scenes;
    s.cur = p.cur;
    s.next = p.next;
    s.nextUid = p.nextUid;
    s.out = p.out;
    s.key = key;
    return s;
}

void Restore(const Snap& s) {
    ProjEdit([&](Project& p) {
        const bool sameCount = p.scenes.size() == s.scenes.size();
        std::vector<Scene> sc = s.scenes;
        for (auto& scene : sc)
            for (auto& it : scene.items)
                if (it.needFit)
                    if (const Item* now = FindItemC(p, it.uid))
                        if (!now->needFit) it.needFit = false;  // already fitted once: do not fit again
        p.scenes = sc;
        p.out = s.out;
        p.nextUid = (std::max)(p.nextUid, s.nextUid);
        if (!sameCount) {
            p.cur = s.cur;
            p.next = s.next;
        }
        const int n = static_cast<int>(p.scenes.size());
        p.cur = std::clamp(p.cur, 0, (std::max)(0, n - 1));
        if (p.next >= n || p.next == p.cur) p.next = -1;
    });
    g_committed = s;
    g_committedVer = g_projVer;
    g_seenVer = g_committedVer;
}

// commits a pending change now (before undo / redo)
void Flush(bool force) {
    const int v = g_projVer;
    const ULONGLONG now = GetTickCount64();
    if (v != g_seenVer) {
        g_seenVer = v;
        g_changedAt = now;
        if (!force) return;
    }
    if (!force && now - g_changedAt < 400) return;
    if (v == g_committedVer && g_haveCommitted) return;
    Project p = ProjSnapshot();
    const std::wstring k = KeyOf(p);
    if (!g_haveCommitted) {
        g_committed = SnapOf(p, k);
        g_haveCommitted = true;
        g_committedVer = v;
        return;
    }
    if (k != g_committed.key) {
        g_undo.push_back(g_committed);
        while (g_undo.size() > kMaxSteps) g_undo.pop_front();
        g_redo.clear();
        g_committed = SnapOf(p, k);
    }
    g_committedVer = v;
}
}  // namespace

void UndoPoll(bool userBusy) {
    if (userBusy) {  // a drag is in progress: wait for its end
        g_changedAt = GetTickCount64();
        g_seenVer = g_projVer;
        return;
    }
    Flush(false);
}

bool UndoStep(bool redo) {
    Flush(true);
    std::deque<Snap>& from = redo ? g_redo : g_undo;
    std::deque<Snap>& to = redo ? g_undo : g_redo;
    if (from.empty()) {
        SetNote(redo ? L"Нечего повторять." : L"Нечего отменять.");
        return false;
    }
    Snap s = from.back();
    from.pop_back();
    to.push_back(g_committed);
    while (to.size() > kMaxSteps) to.pop_front();
    Restore(s);
    LogMsg(redo ? L"Повтор (Ctrl+Y)" : L"Отмена (Ctrl+Z)");
    SetNote(std::wstring(redo ? L"Повторено." : L"Отменено.") + L" Ещё шагов: отменить " + std::to_wstring(g_undo.size()) +
            L", повторить " + std::to_wstring(g_redo.size()) + L".");
    UiRefresh();
    return true;
}

bool UndoAvailable(bool redo) { return redo ? !g_redo.empty() : !g_undo.empty(); }
