// WVC: the main window in the OBS manner — «Сейчас» (program) preview with mouse placement, «Следующий»,
// docks «Сцены», «Источники», «Переходы», «Управление»; tray, hotkeys, frames around captured screen areas.
#include "common.h"

HINSTANCE g_hInst;
HWND g_main = nullptr;
HFONT g_font = nullptr, g_fontBold = nullptr, g_fontSym = nullptr, g_fontBig = nullptr;
UINT g_dpi = 96;
COLORREF kDarkBg = RGB(37, 38, 43), kDarkPanel = RGB(46, 47, 53), kDarkList = RGB(29, 30, 34),
         kDarkText = RGB(228, 228, 228), kDarkDim = RGB(150, 152, 160), kDarkSel = RGB(52, 86, 160),
         kDarkBtn = RGB(64, 66, 74);
HBRUSH g_brDark = nullptr, g_brPanel = nullptr, g_brList = nullptr;

int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }

namespace {
enum {
    IDC_PROGRAM = 100, IDC_NEXTPV, IDC_NEXT_COMBO, IDC_SWAP, IDC_SCENES, IDC_SC_ADD, IDC_SC_DEL, IDC_SC_REN, IDC_SC_UP,
    IDC_SC_DOWN, IDC_SOURCES, IDC_SR_ADD, IDC_SR_DEL, IDC_SR_PROPS, IDC_SR_UP, IDC_SR_DOWN, IDC_TR_TYPE, IDC_TR_MS,
    IDC_TR_SPIN, IDC_START, IDC_PAUSE, IDC_SETTINGS, IDC_EXIT, IDC_TR_DIR, IDC_TR_EASE, IDC_LAST
};
enum {
    ID_TRAY_OPEN = 900, ID_TRAY_TOGGLE, ID_TRAY_PAUSE, ID_TRAY_SWAP, ID_TRAY_EXIT, ID_TRAY_SCENE0 = 920,  // .. 999
    ID_MENU_SETTINGS = 1200, ID_MENU_EXPORT, ID_MENU_IMPORT, ID_MENU_EXIT, ID_MENU_UNDO, ID_MENU_REDO, ID_MENU_DOCK0,
    ID_MENU_DOCKRESET = ID_MENU_DOCK0 + 8, ID_MENU_LOG, ID_MENU_UPDATES, ID_MENU_ABOUT, ID_MENU_EDITNEXT, ID_MENU_README,
    ID_M_PROPS = 1000, ID_M_RESET, ID_M_FIT, ID_M_STRETCH, ID_M_CENTER, ID_M_CENTERH, ID_M_CENTERV, ID_M_ROTCW,
    ID_M_ROTCCW, ID_M_ROT180, ID_M_FLIPH, ID_M_FLIPV, ID_M_TOP, ID_M_UP, ID_M_DOWN, ID_M_BOTTOM, ID_M_VIS, ID_M_LOCK,
    ID_M_RENAME, ID_M_DUP, ID_M_DEL, ID_M_OUTPUT, ID_M_SELECTREGION,
    ID_S_REN = 1100, ID_S_DUP, ID_S_DEL, ID_S_NEXT, ID_S_UP, ID_S_DOWN, ID_S_ADD,
    ID_ADD_BASE = 2000, ID_COPY_BASE = 3000
};
const UINT WM_TRAY = WM_APP + 1;
const UINT WM_FRAME_DONE = WM_APP + 7;
const UINT_PTR kTimerStatus = 1, kTimerPreview = 2, kTimerFollow = 3;

HWND g_c[IDC_LAST] = {};
NOTIFYICONDATAW g_nid{};
std::wstring g_note, g_statusText;
ULONGLONG g_noteAt = 0;
int g_selUid = 0;
bool g_syncing = false;
RECT g_dock[DOCK_COUNT] = {};  // by dock id (empty when hidden): next, scenes, sources, transitions, controls
bool g_editNext = false;      // studio mode: the sources list and the mouse edit «Следующий»
struct DockGeo {
    RECT title{}, btnColl{}, btnClose{};
    bool strip = false;  // collapsed while others are open: a narrow vertical strip
};
DockGeo g_dg[DOCK_COUNT];

RECT g_statusRect{};
int g_titleH = 22;

HWND C(int id) { return g_c[id]; }

void SetTextIfChanged(HWND h, const std::wstring& text) {
    if (!h) return;
    wchar_t cur[1024] = {};
    GetWindowTextW(h, cur, 1024);
    if (text != cur) SetWindowTextW(h, text.c_str());
}
}  // namespace

int UiSelected() { return g_selUid; }

static int EditIdx(const Project& p) {
    const int n = static_cast<int>(p.scenes.size());
    if (n == 0) return -1;
    const int cur = std::clamp(p.cur, 0, n - 1);
    if (g_editNext && p.next >= 0 && p.next < n && p.next != cur) return p.next;
    return cur;
}

Scene* UiEditScene(Project& p) {
    const int i = EditIdx(p);
    return i < 0 ? nullptr : &p.scenes[static_cast<size_t>(i)];
}

const Scene* UiEditSceneC(const Project& p) {
    const int i = EditIdx(p);
    return i < 0 ? nullptr : &p.scenes[static_cast<size_t>(i)];
}

static bool EditingNext(const Project& p) { return EditIdx(p) >= 0 && EditIdx(p) != std::clamp(p.cur, 0, (std::max)(0, static_cast<int>(p.scenes.size()) - 1)); }

static void SetEditNext(bool on) {
    if (g_editNext == on) return;
    g_editNext = on;
    g_editingNext = on;
    g_selUid = 0;
    UiRefresh();
}

void UiOutputSize(int* W, int* H) {
    if (g_camW > 0 && g_camH > 0) {
        *W = g_camW;
        *H = g_camH;
        return;
    }
    Project p = ProjSnapshot();
    OutSizeFor(p.out, W, H);
}

void SetNote(const std::wstring& n) {
    g_note = n;
    g_noteAt = GetTickCount64();
    UiStatus();
}

void DarkTitle(HWND hwnd) {
    BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof(on));
}

HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
               DWORD ex) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return c;
}

bool RunModal(HWND dlg, const bool* done) {
    HWND owner = GetWindow(dlg, GW_OWNER);
    if (!owner) owner = g_main;
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    MSG msg;
    bool quit = false;
    while (!*done) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            quit = true;
            break;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(owner);
    if (quit) PostQuitMessage(0);
    return !quit;
}

// --- input box
namespace {
struct InputState {
    bool done = false, ok = false;
    std::wstring value;
};
InputState* g_input = nullptr;

LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && g_input) {
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[256] = {};
            GetDlgItemTextW(hwnd, 10, buf, 256);
            g_input->value = buf;
            g_input->ok = true;
            g_input->done = true;
        } else if (LOWORD(wp) == IDCANCEL) {
            g_input->done = true;
        }
        return 0;
    }
    if (msg == WM_CLOSE) {
        if (g_input) g_input->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

bool InputBox(HWND owner, const wchar_t* title, const wchar_t* prompt, std::wstring* value) {
    InputState st;
    InputState* prev = g_input;
    g_input = &st;
    RECT r{0, 0, S(360), S(128)};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, g_dpi);
    RECT mr;
    GetWindowRect(owner ? owner : g_main, &mr);
    const int w = r.right - r.left, h = r.bottom - r.top;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"WVCInput", title, style, mr.left + ((mr.right - mr.left) - w) / 2,
                               mr.top + ((mr.bottom - mr.top) - h) / 2, w, h, owner ? owner : g_main, nullptr, g_hInst,
                               nullptr);
    if (!dlg) {
        g_input = prev;
        return false;
    }
    MakeChild(dlg, L"STATIC", prompt, 0, 11, 14, 12, 332, 20);
    HWND ed = MakeChild(dlg, L"EDIT", value->c_str(), WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 10, 14, 38, 332, 26);
    SendMessageW(ed, EM_LIMITTEXT, 80, 0);
    MakeChild(dlg, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 140, 82, 100, 30);
    MakeChild(dlg, L"BUTTON", L"Отмена", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 246, 82, 100, 30);
    SetFocus(ed);
    SendMessageW(ed, EM_SETSEL, 0, -1);
    RunModal(dlg, &st.done);
    g_input = prev;
    if (st.ok) *value = Trim(st.value);
    return st.ok && !value->empty();
}

bool PickFile(HWND owner, const wchar_t* filter, std::wstring* out, bool multi, std::vector<std::wstring>* many) {
    std::vector<wchar_t> file(32768, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | (multi ? OFN_ALLOWMULTISELECT : 0);
    if (!GetOpenFileNameW(&ofn)) return false;
    if (!multi) {
        *out = file.data();
        return true;
    }
    std::wstring first = file.data();
    const wchar_t* p = file.data() + first.size() + 1;
    if (!*p) {
        many->push_back(first);
    } else {
        while (*p) {
            std::wstring f = p;
            many->push_back(first + L"\\" + f);
            p += f.size() + 1;
        }
    }
    return !many->empty();
}

const wchar_t* kImageFilter() {
    return L"Картинки\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico\0Все файлы\0*.*\0";
}

const wchar_t* kMediaFilter() {
    return L"Картинки и видео\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.mp4;*.m4v;*.mov;*.avi;*.wmv;*.mkv;*.webm;*.mpg;"
           L"*.mpeg;*.ts;*.3gp\0Все файлы\0*.*\0";
}

void PaintPublished(HDC dc, Published& pub, const RECT& area, RECT* drawn) {
    std::lock_guard<std::mutex> lk(pub.mx);
    const int aw = area.right - area.left, ah = area.bottom - area.top;
    if (pub.px.empty() || pub.w < 1 || pub.h < 1 || aw < 2 || ah < 2) {
        if (drawn) *drawn = RECT{0, 0, 0, 0};
        return;
    }
    const double s = (std::min)(static_cast<double>(aw) / pub.w, static_cast<double>(ah) / pub.h);
    const int dw = (std::max)(1, static_cast<int>(pub.w * s)), dh = (std::max)(1, static_cast<int>(pub.h * s));
    const int dx = area.left + (aw - dw) / 2, dy = area.top + (ah - dh) / 2;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = pub.w;
    bi.bmiHeader.biHeight = -pub.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, pub.w, pub.h, pub.px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    if (drawn) *drawn = RECT{dx, dy, dx + dw, dy + dh};
}

// ---------------------------------------------------------------------------
// Scenes and items: actions
// ---------------------------------------------------------------------------
void SetProgramScene(int idx) {
    ProjEdit([&](Project& p) {
        const int n = static_cast<int>(p.scenes.size());
        if (idx < 0 || idx >= n || idx == p.cur) return;
        const int old = p.cur;
        p.cur = idx;
        if (p.next == idx) p.next = old;
    });
    g_selUid = 0;
    UiRefresh();
}

static void SwapNext() {
    bool ok = false;
    LogMsg(L"Сейчас ⇄ Следующий");
    ProjEdit([&](Project& p) {
        const int n = static_cast<int>(p.scenes.size());
        if (p.next < 0 || p.next >= n || p.next == p.cur) return;
        std::swap(p.cur, p.next);
        ok = true;
    });
    if (!ok) {
        SetNote(L"«Следующий» не выбран: выберите сцену в списке под предпросмотром.");
        return;
    }
    g_selUid = 0;
    UiRefresh();
}

void UiSelect(int uid) {
    g_selUid = uid;
    UiRefresh();
}

void ShowMain() {
    ShowWindow(g_main, SW_SHOW);
    ShowWindow(g_main, SW_RESTORE);
    SetForegroundWindow(g_main);
    RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static bool CurItemC(const Project& p, int uid, const Item** out) {
    const Scene* sc = UiEditSceneC(p);
    if (!sc) return false;
    for (const auto& it : sc->items)
        if (it.uid == uid) {
            *out = &it;
            return true;
        }
    return false;
}

void FitItemToScreen(Item& it, int W, int H, bool stretch) {
    if (stretch) {
        it.x = it.y = 0;
        it.w = it.h = 1;
        it.fit = 2;
        return;
    }
    int nw = 0, nh = 0;
    if (!NativeSize(it.uid, &nw, &nh)) {
        RECT r{};
        if (it.type == IT_REGION) r = it.region;
        if (it.type == IT_MONITOR) r = it.monRect;
        nw = r.right - r.left;
        nh = r.bottom - r.top;
        if (it.type == IT_TEXT) TextNativeSize(it, &nw, &nh);
        if (it.type == IT_IMAGE && !ImageSize(it.image, &nw, &nh)) nw = nh = 0;
        if ((it.rot & 1) && nw > 0) std::swap(nw, nh);
    }
    if (nw <= 0 || nh <= 0) {
        nw = 16;
        nh = 9;
    }
    const RectD d1 = ItemSourceRect(it, nw, nh);
    double aw = d1.W(), ah = d1.H();
    const double fixed = AspectValue(it.aspect);
    if (fixed > 0) {
        aw = fixed;
        ah = 1;
    }
    if (it.type == IT_TEXT || it.type == IT_IMAGE) {  // at its own size (at most the frame)
        const double k = H / 1080.0;
        double w = aw * (it.type == IT_TEXT ? k : 1), h = ah * (it.type == IT_TEXT ? k : 1);
        if (it.type == IT_IMAGE) {
            w = aw * k;
            h = ah * k;
        }
        const double s = (std::min)(1.0, (std::min)(W / w, H / h));
        w *= s;
        h *= s;
        it.w = w / W;
        it.h = h / H;
        it.x = (1 - it.w) / 2;
        it.y = (1 - it.h) / 2;
        return;
    }
    const double s = (std::min)(W / aw, H / ah);
    const double w = aw * s, h = ah * s;
    it.w = w / W;
    it.h = h / H;
    it.x = (W - w) / 2 / W;
    it.y = (H - h) / 2 / H;
}

void ApplyItemTransform(Item& it, int cmd, int W, int H) {
    const double cx = it.x + it.w / 2, cy = it.y + it.h / 2;
    auto swapBox = [&]() {  // turn the box around its centre, keeping its size in pixels
        const double wpx = it.w * W, hpx = it.h * H;
        it.w = hpx / W;
        it.h = wpx / H;
        it.x = cx - it.w / 2;
        it.y = cy - it.h / 2;
    };
    switch (cmd) {
    case ID_M_RESET:
        it.rot = 0;
        it.mirror = it.type == IT_WEBCAM;
        for (int& c : it.crop) c = 0;
        it.zoom = 100;
        it.fit = 0;
        FitItemToScreen(it, W, H, false);
        break;
    case ID_M_FIT: FitItemToScreen(it, W, H, false); break;
    case ID_M_STRETCH: FitItemToScreen(it, W, H, true); break;
    case ID_M_CENTER:
        it.x = (1 - it.w) / 2;
        it.y = (1 - it.h) / 2;
        break;
    case ID_M_CENTERH: it.x = (1 - it.w) / 2; break;
    case ID_M_CENTERV: it.y = (1 - it.h) / 2; break;
    case ID_M_ROTCW: {
        it.rot = (it.rot + 1) & 3;
        const int c[4] = {it.crop[0], it.crop[1], it.crop[2], it.crop[3]};
        it.crop[0] = c[3];
        it.crop[1] = c[0];
        it.crop[2] = c[1];
        it.crop[3] = c[2];
        swapBox();
        break;
    }
    case ID_M_ROTCCW: {
        it.rot = (it.rot + 3) & 3;
        const int c[4] = {it.crop[0], it.crop[1], it.crop[2], it.crop[3]};
        it.crop[0] = c[1];
        it.crop[1] = c[2];
        it.crop[2] = c[3];
        it.crop[3] = c[0];
        swapBox();
        break;
    }
    case ID_M_ROT180:
        it.rot = (it.rot + 2) & 3;
        std::swap(it.crop[0], it.crop[2]);
        std::swap(it.crop[1], it.crop[3]);
        break;
    case ID_M_FLIPH:
        it.mirror = !it.mirror;
        std::swap(it.crop[0], it.crop[2]);
        break;
    case ID_M_FLIPV:
        it.mirror = !it.mirror;
        it.rot = (it.rot + 2) & 3;
        std::swap(it.crop[1], it.crop[3]);
        break;
    }
}

static void ItemCommand(int uid, int cmd) {
    int W, H;
    UiOutputSize(&W, &H);
    if (cmd == ID_M_PROPS) {
        OpenProperties(uid, false);
        return;
    }
    if (cmd == ID_M_DEL) {
        std::wstring name;
        {
            Project p = ProjSnapshot();
            const Item* it = nullptr;
            if (!CurItemC(p, uid, &it)) return;
            name = it->name;
        }
        if (MessageBoxW(g_main, (L"Удалить источник «" + name + L"»?").c_str(), L"WVC", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        ProjEdit([&](Project& p) {
            int si = -1;
            if (!FindItem(p, uid, &si) || si < 0) return;
            Scene* sc = &p.scenes[static_cast<size_t>(si)];
            sc->items.erase(std::remove_if(sc->items.begin(), sc->items.end(), [&](const Item& i) { return i.uid == uid; }),
                            sc->items.end());
            if (p.out.nativeUid == uid && !FindItem(p, uid)) p.out.native = false;
        });
        if (g_selUid == uid) g_selUid = 0;
        UiRefresh();
        return;
    }
    if (cmd == ID_M_RENAME) {
        Project p = ProjSnapshot();
        const Item* it = nullptr;
        if (!CurItemC(p, uid, &it)) return;
        std::wstring n = it->name;
        if (!InputBox(g_main, L"Переименовать источник", L"Название источника:", &n)) return;
        ProjEdit([&](Project& pp) {
            if (Item* i = FindItem(pp, uid)) i->name = n;
        });
        UiRefresh();
        return;
    }
    if (cmd == ID_M_SELECTREGION) {
        RECT r;
        if (!SelectRegion(g_hInst, &r)) return;
        ProjEdit([&](Project& pp) {
            if (Item* i = FindItem(pp, uid)) i->region = r;
        });
        UiRefresh();
        return;
    }
    if (cmd >= ID_COPY_BASE && cmd < ID_COPY_BASE + 200) {
        const int target = cmd - ID_COPY_BASE;
        ProjEdit([&](Project& p) {
            Item* it = FindItem(p, uid);
            if (!it || target < 0 || target >= static_cast<int>(p.scenes.size())) return;
            Item copy = *it;
            copy.uid = p.nextUid++;
            copy.name = UniqueItemName(p.scenes[static_cast<size_t>(target)], copy.name);
            p.scenes[static_cast<size_t>(target)].items.push_back(copy);
        });
        SetNote(L"Источник скопирован в другую сцену.");
        UiRefresh();
        return;
    }
    ProjEdit([&](Project& p) {
        int si = -1;
        if (!FindItem(p, uid, &si) || si < 0) return;
        Scene* sc = &p.scenes[static_cast<size_t>(si)];
        auto& v = sc->items;
        int i = -1;
        for (size_t k = 0; k < v.size(); ++k)
            if (v[k].uid == uid) i = static_cast<int>(k);
        if (i < 0) return;
        const int n = static_cast<int>(v.size());
        switch (cmd) {
        case ID_M_TOP: {
            Item t = v[static_cast<size_t>(i)];
            v.erase(v.begin() + i);
            v.push_back(t);
            break;
        }
        case ID_M_BOTTOM: {
            Item t = v[static_cast<size_t>(i)];
            v.erase(v.begin() + i);
            v.insert(v.begin(), t);
            break;
        }
        case ID_M_UP:
            if (i < n - 1) std::swap(v[static_cast<size_t>(i)], v[static_cast<size_t>(i + 1)]);
            break;
        case ID_M_DOWN:
            if (i > 0) std::swap(v[static_cast<size_t>(i)], v[static_cast<size_t>(i - 1)]);
            break;
        case ID_M_VIS: v[static_cast<size_t>(i)].visible = !v[static_cast<size_t>(i)].visible; break;
        case ID_M_LOCK: v[static_cast<size_t>(i)].locked = !v[static_cast<size_t>(i)].locked; break;
        case ID_M_DUP: {
            Item c = v[static_cast<size_t>(i)];
            c.uid = p.nextUid++;
            c.name = UniqueItemName(*sc, c.name);
            c.x += 0.03;
            c.y += 0.03;
            v.insert(v.begin() + i + 1, c);
            break;
        }
        default: ApplyItemTransform(v[static_cast<size_t>(i)], cmd, W, H); break;
        }
    });
    UiRefresh();
}

static HMENU AddSourceSubmenu() {
    HMENU m = CreatePopupMenu();
    for (int t = 0; t < IT_COUNT; ++t) AppendMenuW(m, MF_STRING, ID_ADD_BASE + t, kTypeNames[t]);
    return m;
}

void AddSourceMenu(HWND owner, POINT pt) {
    HMENU m = AddSourceSubmenu();
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(m);
    if (cmd >= ID_ADD_BASE && cmd < ID_ADD_BASE + IT_COUNT) AddSourceOfType(cmd - ID_ADD_BASE);
}

static void ItemContextMenu(int uid, POINT pt) {
    Project p = ProjSnapshot();
    const Item* it = nullptr;
    HMENU m = CreatePopupMenu();
    if (uid && CurItemC(p, uid, &it)) {
        AppendMenuW(m, MF_STRING, ID_M_PROPS, L"Свойства…");
        if (it->type == IT_REGION) AppendMenuW(m, MF_STRING, ID_M_SELECTREGION, L"Выделить область заново…");
        HMENU tr = CreatePopupMenu();
        AppendMenuW(tr, MF_STRING, ID_M_RESET, L"Сбросить трансформацию");
        AppendMenuW(tr, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(tr, MF_STRING, ID_M_FIT, L"По размеру экрана");
        AppendMenuW(tr, MF_STRING, ID_M_STRETCH, L"Растянуть на весь экран");
        AppendMenuW(tr, MF_STRING, ID_M_CENTER, L"По центру");
        AppendMenuW(tr, MF_STRING, ID_M_CENTERH, L"По центру по горизонтали");
        AppendMenuW(tr, MF_STRING, ID_M_CENTERV, L"По центру по вертикали");
        AppendMenuW(tr, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(tr, MF_STRING, ID_M_ROTCW, L"Повернуть на 90° по часовой");
        AppendMenuW(tr, MF_STRING, ID_M_ROTCCW, L"Повернуть на 90° против часовой");
        AppendMenuW(tr, MF_STRING, ID_M_ROT180, L"Повернуть на 180°");
        AppendMenuW(tr, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(tr, MF_STRING, ID_M_FLIPH, L"Отразить по горизонтали");
        AppendMenuW(tr, MF_STRING, ID_M_FLIPV, L"Отразить по вертикали");
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(tr), L"Трансформация");
        HMENU ord = CreatePopupMenu();
        AppendMenuW(ord, MF_STRING, ID_M_TOP, L"Наверх");
        AppendMenuW(ord, MF_STRING, ID_M_UP, L"Выше");
        AppendMenuW(ord, MF_STRING, ID_M_DOWN, L"Ниже");
        AppendMenuW(ord, MF_STRING, ID_M_BOTTOM, L"В самый низ");
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(ord), L"Порядок");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_M_VIS, it->visible ? L"Скрыть" : L"Показать");
        AppendMenuW(m, MF_STRING, ID_M_LOCK, it->locked ? L"Разблокировать" : L"Заблокировать");
        AppendMenuW(m, MF_STRING, ID_M_RENAME, L"Переименовать…");
        AppendMenuW(m, MF_STRING, ID_M_DUP, L"Дублировать");
        if (p.scenes.size() > 1) {
            HMENU cp = CreatePopupMenu();
            for (size_t s = 0; s < p.scenes.size(); ++s)
                if (static_cast<int>(s) != EditIdx(p))
                    AppendMenuW(cp, MF_STRING, ID_COPY_BASE + static_cast<UINT>(s), p.scenes[s].name.c_str());
            AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(cp), L"Копировать в сцену");
        }
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_M_DEL, L"Удалить");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(AddSourceSubmenu()), L"Добавить источник");
    AppendMenuW(m, MF_STRING, ID_M_OUTPUT, L"Кадр камеры (вывод)…");
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    if (!cmd) return;
    if (cmd >= ID_ADD_BASE && cmd < ID_ADD_BASE + IT_COUNT) {
        AddSourceOfType(cmd - ID_ADD_BASE);
        return;
    }
    if (cmd == ID_M_OUTPUT) {
        OpenSettings(1);
        return;
    }
    ItemCommand(uid, cmd);
}

// ---------------------------------------------------------------------------
// Streaming control
// ---------------------------------------------------------------------------
static void ToggleStreaming() {
    if (!g_streaming && !g_camOk) {
        MessageBoxW(g_main,
                    CameraDllLoaded() ? L"Не удалось запустить виртуальную камеру." :
                                        L"Не найден WVCCam.dll рядом с WVC.exe — распакуйте WVC.zip целиком.",
                    L"WVC", MB_ICONERROR);
        return;
    }
    g_streaming = !g_streaming;
    g_paused = false;
    if (g_streaming) {
        Project p = ProjSnapshot();
        for (const auto& sc : p.scenes)
            for (const auto& it : sc.items)
                if (it.type == IT_PHONE && PhoneUserState(InputKeyOf(it)) == -1) PhoneUserConnect(InputKeyOf(it), 0);
    }
    g_note.clear();
    UiStatus();
}

static void TogglePause() {
    if (!g_streaming) {
        SetNote(L"Пауза работает во время трансляции: сначала нажмите «Старт камеры».");
        return;
    }
    g_paused = !g_paused;
    UiStatus();
}

// ---------------------------------------------------------------------------
// Red frames around captured screen areas
// ---------------------------------------------------------------------------
namespace {
const COLORREF kKey = RGB(255, 0, 255);
struct FrameWin {
    HWND hwnd = nullptr;
    int uid = 0;
    bool tabBelow = false;
    bool movable = true;
    bool thin = false;  // a captured window: a thin line, no label, clicks go through
    bool dashed = false;   // 0.13: what «Следующий» will capture (blue dashed line)
    bool createdThin = false;
    std::wstring label, look;
    std::vector<std::vector<POINT>> outline;  // screen coordinates
};
std::map<int, FrameWin> g_frames;
int g_fb = 6, g_tabW = 150, g_tabH = 26;
HFONT g_frameFont = nullptr;
bool g_frameMoving = false;

FrameWin* FrameOf(HWND h) {
    for (auto& kv : g_frames)
        if (kv.second.hwnd == h) return &kv.second;
    return nullptr;
}

COLORREF FrameColor() {
    if (!g_streaming) return RGB(40, 170, 70);
    return g_paused ? RGB(235, 170, 0) : RGB(230, 40, 40);
}
const COLORREF kNextColor = RGB(40, 140, 255);
COLORREF ColorOf(const FrameWin& f) { return f.dashed ? kNextColor : FrameColor(); }

int FrameB(const FrameWin& f) { return f.thin ? (std::max)(2, g_fb / 2) : g_fb; }
int FrameTab(const FrameWin& f) { return f.thin ? 0 : g_tabH; }

void FrameRects(const FrameWin& f, int w, int h, RECT* border, RECT* hole, RECT* tab) {
    const int fb = FrameB(f), tabH = FrameTab(f);
    const int top = f.tabBelow ? 0 : tabH;
    const int bottom = f.tabBelow ? h - tabH : h;
    *border = RECT{0, top, w, bottom};
    *hole = RECT{fb, top + fb, w - fb, bottom - fb};
    const int tw = (std::min)(g_tabW + (f.dashed ? g_tabW / 3 : 0), w);
    const int tx = f.dashed ? w - tw : 0;  // the «Следующий» label sits on the right, so it never hides the on-air one
    *tab = f.tabBelow ? RECT{tx, h - tabH, tx + tw, h} : RECT{tx, 0, tx + tw, tabH};
}

void RegionFromFrame(HWND hwnd) {
    FrameWin* f = FrameOf(hwnd);
    if (!f || !f->movable) return;
    RECT wr;
    GetWindowRect(hwnd, &wr);
    RECT r{wr.left + g_fb, wr.top + g_fb + (f->tabBelow ? 0 : g_tabH), wr.right - g_fb,
           wr.bottom - g_fb - (f->tabBelow ? g_tabH : 0)};
    const int uid = f->uid;
    std::lock_guard<std::mutex> lk(g_projMx);
    if (Item* it = FindItem(g_proj, uid)) it->region = r;
    ++g_projVer;
    g_projDirty = true;
}

LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FrameWin* f = FrameOf(hwnd);
        RECT rc, border, hole, tab;
        GetClientRect(hwnd, &rc);
        if (f) {
            FrameRects(*f, rc.right, rc.bottom, &border, &hole, &tab);
            const COLORREF col = ColorOf(*f);
            HBRUSH key = CreateSolidBrush(kKey), red = CreateSolidBrush(col);
            FillRect(dc, &rc, key);
            HPEN dash = nullptr;
            const int pw = (std::max)(2, g_fb * 2 / 3);
            if (f->dashed) {
                LOGBRUSH lb{BS_SOLID, col, 0};
                dash = ExtCreatePen(PS_GEOMETRIC | PS_DASH | PS_ENDCAP_FLAT | PS_JOIN_MITER, static_cast<DWORD>(pw), &lb, 0,
                                    nullptr);
            }
            if (f->dashed && f->outline.empty()) {
                // dashed rectangle in the middle of the band (around the area, outside the picture)
                HGDIOBJ op = SelectObject(dc, dash ? static_cast<HGDIOBJ>(dash) : GetStockObject(BLACK_PEN));
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                const int fbw = f->thin ? 0 : FrameB(*f) / 2;
                Rectangle(dc, border.left + fbw + pw / 2, border.top + fbw + pw / 2, border.right - fbw - pw / 2 + 1,
                          border.bottom - fbw - pw / 2 + 1);
                SelectObject(dc, ob);
                SelectObject(dc, op);
            } else if (!f->outline.empty()) {
                const int L = (std::min)(g_tabH * 3 / 2, (std::min)(static_cast<int>(border.right - border.left),
                                                                    static_cast<int>(border.bottom - border.top)) / 3);
                const RECT marks[8] = {
                    {border.left, border.top, border.left + L, border.top + g_fb},
                    {border.left, border.top, border.left + g_fb, border.top + L},
                    {border.right - L, border.top, border.right, border.top + g_fb},
                    {border.right - g_fb, border.top, border.right, border.top + L},
                    {border.left, border.bottom - g_fb, border.left + L, border.bottom},
                    {border.left, border.bottom - L, border.left + g_fb, border.bottom},
                    {border.right - L, border.bottom - g_fb, border.right, border.bottom},
                    {border.right - g_fb, border.bottom - L, border.right, border.bottom},
                };
                if (!f->thin)
                    for (const RECT& m : marks) FillRect(dc, &m, red);
                RECT wr;
                GetWindowRect(hwnd, &wr);
                HPEN pen = dash ? nullptr : CreatePen(PS_SOLID, (std::max)(2, g_fb * 2 / 3), col);
                HGDIOBJ op = SelectObject(dc, dash ? dash : pen);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                for (auto poly : f->outline) {
                    for (POINT& pt : poly) {
                        pt.x -= wr.left;
                        pt.y -= wr.top;
                    }
                    if (poly.size() >= 3) Polygon(dc, poly.data(), static_cast<int>(poly.size()));
                }
                SelectObject(dc, ob);
                SelectObject(dc, op);
                if (pen) DeleteObject(pen);
            } else {
                FillRect(dc, &border, red);
                FillRect(dc, &hole, key);
            }
            if (dash) DeleteObject(dash);
            if (FrameTab(*f) > 0) FillRect(dc, &tab, red);
            DeleteObject(red);
            DeleteObject(key);
            HGDIOBJ old = g_frameFont ? SelectObject(dc, g_frameFont) : nullptr;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            RECT tr = tab;
            InflateRect(&tr, -4, 0);
            if (FrameTab(*f) > 0) DrawTextW(dc, f->label.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (old) SelectObject(dc, old);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCHITTEST: {
        FrameWin* f = FrameOf(hwnd);
        if (!f) return HTTRANSPARENT;
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd, &pt);
        RECT rc, border, hole, tab;
        GetClientRect(hwnd, &rc);
        FrameRects(*f, rc.right, rc.bottom, &border, &hole, &tab);
        if (!f->movable) return HTTRANSPARENT;
        if (PtInRect(&tab, pt)) return HTCAPTION;
        const int cs = g_tabH;
        const bool L = pt.x < border.left + cs, R = pt.x >= border.right - cs;
        const bool T = pt.y < border.top + cs, B = pt.y >= border.bottom - cs;
        if (T && L) return HTTOPLEFT;
        if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && R) return HTBOTTOMRIGHT;
        if (pt.x < hole.left) return HTLEFT;
        if (pt.x >= hole.right) return HTRIGHT;
        if (pt.y < hole.top) return HTTOP;
        if (pt.y >= hole.bottom) return HTBOTTOM;
        return HTCAPTION;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCAPTION) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = g_tabW + 2 * g_fb;
        mm->ptMinTrackSize.y = g_tabH + 2 * g_fb + 48;
        return 0;
    }
    case WM_ENTERSIZEMOVE: g_frameMoving = true; return 0;
    case WM_MOVE:
    case WM_SIZE:
        if (msg == WM_SIZE) InvalidateRect(hwnd, nullptr, FALSE);
        if (g_frameMoving) RegionFromFrame(hwnd);
        return 0;
    case WM_EXITSIZEMOVE:
        g_frameMoving = false;
        RegionFromFrame(hwnd);
        PostMessageW(g_main, WM_FRAME_DONE, 0, 0);
        return 0;
    case WM_DPICHANGED: return 0;
    case WM_NCRBUTTONUP:
    case WM_NCLBUTTONDBLCLK: return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// shape outline of the item mapped back onto the screen
void ScreenOutline(const Item& it, const RECT& src, int W, int H, std::vector<std::vector<POINT>>* out, bool rectToo = false) {
    out->clear();
    const double NW = src.right - src.left, NH = src.bottom - src.top;
    const double DW = (it.rot & 1) ? NH : NW, DH = (it.rot & 1) ? NW : NH;
    RectD Cr, D;
    ItemLayout(it, W, H, DW, DH, &Cr, &D);
    if (Cr.W() < 2 || Cr.H() < 2) return;
    if (it.shape.kind == SH_RECT || it.shape.kind == SH_MASK) {
        if (!rectToo) return;
        // «Следующий»: the part of the area that is really shown (crop / zoom / fill), when it is smaller
        const RectD r = DispToSrc(D, it.rot, it.mirror, NW, NH);
        if (r.x0 < 2 && r.y0 < 2 && r.x1 > NW - 2 && r.y1 > NH - 2) return;
        const LONG x0 = static_cast<LONG>(std::lround(src.left + r.x0)), y0 = static_cast<LONG>(std::lround(src.top + r.y0));
        const LONG x1 = static_cast<LONG>(std::lround(src.left + r.x1)), y1 = static_cast<LONG>(std::lround(src.top + r.y1));
        out->push_back({POINT{x0, y0}, POINT{x1, y0}, POINT{x1, y1}, POINT{x0, y1}});
        return;
    }
    std::vector<std::vector<POINT>> polys;
    // a little outside the shape, so the line never shows in the picture
    const double grow = 1.0 + (g_fb + 2.0) * 2 / (std::min)(Cr.W(), Cr.H()) * (std::min)(D.W() / Cr.W(), 1.0);
    ShapeOutline(Cr.W(), Cr.H(), it.shape, &polys);
    for (auto& poly : polys) {
        std::vector<POINT> o;
        for (const POINT& pt : poly) {
            double x = Cr.W() / 2 + (pt.x - Cr.W() / 2) * grow, y = Cr.H() / 2 + (pt.y - Cr.H() / 2) * grow;
            // content -> display -> unrotated source
            const double dx = D.x0 + x / Cr.W() * D.W(), dy = D.y0 + y / Cr.H() * D.H();
            const RectD r = DispToSrc(RectD{dx, dy, dx, dy}, it.rot, it.mirror, NW, NH);
            o.push_back(POINT{static_cast<LONG>(std::lround(src.left + r.x0)), static_cast<LONG>(std::lround(src.top + r.y0))});
        }
        out->push_back(o);
    }
}

// Windows 10 2004+: the frames never get into a screen capture (DXGI / GDI), whatever they cover
void ExcludeFromCapture(HWND h) {
    typedef BOOL(WINAPI * PFN)(HWND, DWORD);
    static PFN f = reinterpret_cast<PFN>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowDisplayAffinity"));
    if (f && h) f(h, 0x11 /* WDA_EXCLUDEFROMCAPTURE */);
}

// one frame window around r (screen coordinates); returns false when it could not be made
bool PlaceFrame(FrameWin& f, const Item& it, const RECT& r, int W, int H, std::map<int, bool>& keep, bool monitor) {
    keep[it.uid] = true;
    f.uid = it.uid;
    if (f.hwnd && f.createdThin != f.thin) {
        DestroyWindow(f.hwnd);
        f.hwnd = nullptr;
    }
    if (!f.hwnd) {
        const DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (f.thin ? WS_EX_TRANSPARENT : 0);
        f.hwnd = CreateWindowExW(ex, L"WVCFrame", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_hInst, nullptr);
        if (f.hwnd) {
            SetLayeredWindowAttributes(f.hwnd, kKey, 255, LWA_COLORKEY);
            ExcludeFromCapture(f.hwnd);
        }
        f.createdThin = f.thin;
        f.look.clear();
    }
    if (!f.hwnd) return false;
    const int fb = FrameB(f), tabH = FrameTab(f);
    RECT w;
    if (monitor) {
        f.tabBelow = false;
        w = r;  // a whole monitor: the dashed line runs along its edge, inside
    } else {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
        f.tabBelow = (r.top - fb - tabH) < mi.rcMonitor.top;
        w = RECT{r.left - fb, r.top - fb, r.right + fb, r.bottom + fb};
        if (f.tabBelow)
            w.bottom += tabH;
        else
            w.top -= tabH;
    }
    if (it.type == IT_REGION || (f.dashed && it.type == IT_WINDOW)) ScreenOutline(it, r, W, H, &f.outline, f.dashed);
    else f.outline.clear();
    if (f.thin && !f.dashed) f.outline.clear();
    wchar_t look[300];
    swprintf(look, 300, L"%u|%ls|%d|%.4f,%.4f,%.4f,%.4f|%d,%d,%d,%d|%d%d%d|%ls|%d%d", static_cast<unsigned>(ColorOf(f)),
             it.shape.Key().c_str(), it.fit, it.x, it.y, it.w, it.h, it.crop[0], it.crop[1], it.crop[2], it.crop[3], it.rot,
             it.mirror ? 1 : 0, it.zoom, f.label.c_str(), f.dashed ? 1 : 0, f.tabBelow ? 1 : 0);
    RECT cur;
    GetWindowRect(f.hwnd, &cur);
    if (!IsWindowVisible(f.hwnd) || !EqualRect(&cur, &w)) {
        SetWindowPos(f.hwnd, HWND_TOPMOST, w.left, w.top, w.right - w.left, w.bottom - w.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(f.hwnd, nullptr, TRUE);
    } else if (f.look != look) {
        InvalidateRect(f.hwnd, nullptr, TRUE);
    }
    f.look = look;
    return true;
}

void UpdateFrames() {
    if (g_frameMoving) return;
    Project p = ProjSnapshot();
    const Scene* sc = CurSceneC(p);
    int W, H;
    UiOutputSize(&W, &H);
    std::map<int, bool> keep;
    // the scene on air (or the current one while stopped): green / red / yellow, as before
    if (sc && (g_streaming || p.idleFrame)) {
        for (const auto& it : sc->items) {
            if (!it.visible || (it.type != IT_REGION && it.type != IT_WINDOW)) continue;
            RECT r;
            if (it.type == IT_REGION) {
                r = it.region;
                if (r.right - r.left < 16 || r.bottom - r.top < 16) continue;
            } else if (!it.winFrame || !WindowRectOf(it.hwnd, &r) || IsIconic(it.hwnd)) {
                continue;  // «Окно»: the frame is optional (per source)
            }
            FrameWin& f = g_frames[it.uid];
            f.dashed = false;
            f.movable = it.type == IT_REGION && !it.locked;
            f.thin = it.type == IT_WINDOW;
            f.label = (f.movable ? L"✥ " : L"") + it.name;
            PlaceFrame(f, it, r, W, H, keep, false);
        }
    }
    // 0.13: «Следующий» — a blue dashed outline of what it will capture, on air or not; areas can be moved / resized
    const int n = static_cast<int>(p.scenes.size());
    if (p.nextFrame && p.next >= 0 && p.next < n && p.next != p.cur) {
        const Scene& nx = p.scenes[static_cast<size_t>(p.next)];
        for (const auto& it : nx.items) {
            if (!it.visible || keep.count(it.uid)) continue;
            RECT r;
            bool monitor = false;
            if (it.type == IT_REGION) {
                r = it.region;
                if (r.right - r.left < 16 || r.bottom - r.top < 16) continue;
            } else if (it.type == IT_WINDOW) {
                if (!WindowRectOf(it.hwnd, &r) || IsIconic(it.hwnd)) continue;
            } else if (it.type == IT_MONITOR) {
                r = it.monRect;
                for (const auto& m : Monitors())
                    if (!it.monDevice.empty() && m.device == it.monDevice) r = m.rc;
                if (r.right - r.left < 16 || r.bottom - r.top < 16) continue;
                monitor = true;
            } else {
                continue;
            }
            FrameWin& f = g_frames[it.uid];
            f.dashed = true;
            f.movable = it.type == IT_REGION && !it.locked;
            f.thin = !f.movable;
            f.label = std::wstring(f.movable ? L"✥ " : L"") + L"Следующий: " + it.name;
            PlaceFrame(f, it, r, W, H, keep, monitor);
        }
    }
    for (auto it = g_frames.begin(); it != g_frames.end();) {
        if (!keep.count(it->first)) {
            if (it->second.hwnd) DestroyWindow(it->second.hwnd);
            it = g_frames.erase(it);
        } else {
            ++it;
        }
    }
}

void DestroyFrames() {
    for (auto& kv : g_frames)
        if (kv.second.hwnd) DestroyWindow(kv.second.hwnd);
    g_frames.clear();
}
}  // namespace

// ---------------------------------------------------------------------------
// Region selection overlay
// ---------------------------------------------------------------------------
namespace {
struct SelState {
    bool done = false, ok = false, dragging = false;
    POINT a{}, b{};
    RECT result{};
    int vx = 0, vy = 0;
    HFONT font = nullptr;
};
SelState* g_sel = nullptr;

RECT NormRect(POINT a, POINT b) {
    return RECT{(std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::max)(a.x, b.x), (std::max)(a.y, b.y)};
}

LRESULT CALLBACK SelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SelState* st = g_sel;
    switch (msg) {
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, IDC_CROSS)); return TRUE;
    case WM_LBUTTONDOWN:
        if (st) {
            SetCapture(hwnd);
            st->dragging = true;
            st->a.x = st->b.x = static_cast<short>(LOWORD(lp));
            st->a.y = st->b.y = static_cast<short>(HIWORD(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (st && st->dragging) {
            st->b.x = static_cast<short>(LOWORD(lp));
            st->b.y = static_cast<short>(HIWORD(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (st && st->dragging) {
            ReleaseCapture();
            st->dragging = false;
            st->b.x = static_cast<short>(LOWORD(lp));
            st->b.y = static_cast<short>(HIWORD(lp));
            RECT r = NormRect(st->a, st->b);
            if (r.right - r.left >= 16 && r.bottom - r.top >= 16) {
                OffsetRect(&r, st->vx, st->vy);
                st->result = r;
                st->ok = true;
                st->done = true;
            } else {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (st) st->done = true;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && st) st->done = true;
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC m = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(m, bmp);
        HBRUSH dark = CreateSolidBrush(RGB(16, 16, 16));
        FillRect(m, &rc, dark);
        DeleteObject(dark);
        HGDIOBJ oldFont = st && st->font ? SelectObject(m, st->font) : nullptr;
        SetBkMode(m, TRANSPARENT);
        SetTextColor(m, RGB(255, 255, 255));
        RECT hint = PrimaryMonitorRect();
        if (st) OffsetRect(&hint, -st->vx, -st->vy);
        hint.top += 40;
        hint.bottom = hint.top + 60;
        DrawTextW(m, L"Выделите мышью область, которую покажет камера.   Esc или правая кнопка — отмена", -1, &hint,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        if (st && st->dragging) {
            RECT s = NormRect(st->a, st->b);
            RECT frame = s;
            InflateRect(&frame, 2, 2);
            HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(m, &frame, white);
            DeleteObject(white);
            HBRUSH key = CreateSolidBrush(kKey);
            FillRect(m, &s, key);
            DeleteObject(key);
            wchar_t txt[64];
            swprintf(txt, 64, L"%d × %d", static_cast<int>(s.right - s.left), static_cast<int>(s.bottom - s.top));
            RECT tr{s.left, s.bottom + 6, s.left + 400, s.bottom + 40};
            if (tr.bottom > rc.bottom) OffsetRect(&tr, 0, -(s.bottom - s.top) - 46);
            DrawTextW(m, txt, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_TOP);
        }
        if (oldFont) SelectObject(m, oldFont);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, old);
        DeleteObject(bmp);
        DeleteDC(m);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

bool SelectRegion(HINSTANCE hInst, RECT* out) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = SelProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
        wc.lpszClassName = L"WVCSelect";
        RegisterClassW(&wc);
        registered = true;
    }
    SelState st;
    const RECT vs = VirtualScreen();
    st.vx = vs.left;
    st.vy = vs.top;
    st.font = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SelState* prev = g_sel;
    g_sel = &st;
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"WVCSelect", L"WVC", WS_POPUP,
                                vs.left, vs.top, vs.right - vs.left, vs.bottom - vs.top, nullptr, nullptr, hInst, nullptr);
    ExcludeFromCapture(hwnd);
    if (!hwnd) {
        g_sel = prev;
        DeleteObject(st.font);
        return false;
    }
    SetLayeredWindowAttributes(hwnd, kKey, 120, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(hwnd);
    g_sel = prev;
    DeleteObject(st.font);
    if (st.ok) *out = st.result;
    return st.ok;
}

// ---------------------------------------------------------------------------
// «Сейчас» preview: placement with the mouse
// ---------------------------------------------------------------------------
namespace {
enum { HL = 1, HT = 2, HR = 4, HB = 8 };
struct PvGeo {
    RECT frame{};     // where the frame is drawn (client px)
    double scale = 0; // client px per output px
    int W = 0, H = 0;
};
PvGeo g_pvs[2];
int g_pvWhich = 0;  // 0 «Сейчас», 1 «Следующий»: the same code draws and edits both
PvGeo& PV() { return g_pvs[g_pvWhich]; }
const Scene* PvScene(const Project& p) {
    if (g_pvWhich == 0) return CurSceneC(p);
    const int n = static_cast<int>(p.scenes.size());
    return (p.next >= 0 && p.next < n && p.next != p.cur) ? &p.scenes[static_cast<size_t>(p.next)] : nullptr;
}
RECT g_emptyAdd{}, g_emptyJson{};
bool g_empty = false;
int g_hoverUid = 0;
struct Drag {
    int mode = 0;  // 1 move, 2 resize, 3 crop (Alt)
    int handle = 0;
    POINT start{};
    Item orig;
    int nw = 0, nh = 0;
} g_drag;

POINT ToOut(int x, int y) {
    if (PV().scale <= 0) return POINT{0, 0};
    return POINT{static_cast<LONG>((x - PV().frame.left) / PV().scale), static_cast<LONG>((y - PV().frame.top) / PV().scale)};
}

RECT BoxClient(const Item& it) {
    const RectD b = ItemBox(it, PV().W, PV().H);
    return RECT{static_cast<LONG>(PV().frame.left + b.x0 * PV().scale), static_cast<LONG>(PV().frame.top + b.y0 * PV().scale),
                static_cast<LONG>(PV().frame.left + b.x1 * PV().scale), static_cast<LONG>(PV().frame.top + b.y1 * PV().scale)};
}

int HitItem(const Project& p, int x, int y) {
    const Scene* sc = PvScene(p);
    if (!sc || PV().scale <= 0) return 0;
    for (auto it = sc->items.rbegin(); it != sc->items.rend(); ++it) {
        if (!it->visible) continue;
        RECT r = BoxClient(*it);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return it->uid;
    }
    return 0;
}

int HitHandle(const Item& it, int x, int y) {
    const RECT r = BoxClient(it);
    const int hs = S(6);
    const int xs[3] = {r.left, (r.left + r.right) / 2, r.right}, ys[3] = {r.top, (r.top + r.bottom) / 2, r.bottom};
    const int fl[3][3] = {{HL | HT, HT, HR | HT}, {HL, 0, HR}, {HL | HB, HB, HR | HB}};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            if (!fl[j][i]) continue;
            if (std::abs(x - xs[i]) <= hs && std::abs(y - ys[j]) <= hs) return fl[j][i];
        }
    return 0;
}

double Snap(double v, const std::vector<double>& targets, double thr, bool* snapped) {
    double best = v, bd = thr;
    for (double t : targets)
        if (std::fabs(v - t) < bd) {
            bd = std::fabs(v - t);
            best = t;
        }
    if (snapped) *snapped = best != v;
    return best;
}

void DragTo(int mx, int my, bool shift, bool ctrl) {
    if (!g_drag.mode || PV().scale <= 0) return;
    const double W = PV().W, H = PV().H;
    const double dx = (mx - g_drag.start.x) / PV().scale, dy = (my - g_drag.start.y) / PV().scale;
    const Item& o = g_drag.orig;
    double L = o.x * W, T = o.y * H, R = (o.x + o.w) * W, B = (o.y + o.h) * H;
    const double thr = S(8) / PV().scale;
    std::vector<double> tx{0, W / 2, W}, ty{0, H / 2, H};
    if (!ctrl) {
        Project p = ProjSnapshot();
        if (const Scene* sc = PvScene(p))
            for (const auto& it : sc->items)
                if (it.visible && it.uid != o.uid) {
                    tx.push_back(it.x * W);
                    tx.push_back((it.x + it.w) * W);
                    ty.push_back(it.y * H);
                    ty.push_back((it.y + it.h) * H);
                }
    } else {
        tx.clear();
        ty.clear();
    }
    Item n = o;
    if (g_drag.mode == 1) {
        double nl = L + dx, nt = T + dy;
        const double w = R - L, h = B - T;
        bool s1 = false;
        double best = nl;
        // snap left, centre or right edge
        double v = Snap(nl, tx, thr, &s1);
        if (s1) best = v;
        else {
            v = Snap(nl + w / 2, tx, thr, &s1);
            if (s1) best = v - w / 2;
            else {
                v = Snap(nl + w, tx, thr, &s1);
                if (s1) best = v - w;
            }
        }
        nl = best;
        best = nt;
        v = Snap(nt, ty, thr, &s1);
        if (s1) best = v;
        else {
            v = Snap(nt + h / 2, ty, thr, &s1);
            if (s1) best = v - h / 2;
            else {
                v = Snap(nt + h, ty, thr, &s1);
                if (s1) best = v - h;
            }
        }
        nt = best;
        n.x = nl / W;
        n.y = nt / H;
    } else if (g_drag.mode == 2) {
        const int hd = g_drag.handle;
        if (hd & HL) L = Snap(L + dx, tx, thr, nullptr);
        if (hd & HR) R = Snap(R + dx, tx, thr, nullptr);
        if (hd & HT) T = Snap(T + dy, ty, thr, nullptr);
        if (hd & HB) B = Snap(B + dy, ty, thr, nullptr);
        const double minS = 8;
        if (R - L < minS) {
            if (hd & HL) L = R - minS;
            else R = L + minS;
        }
        if (B - T < minS) {
            if (hd & HT) T = B - minS;
            else B = T + minS;
        }
        const double fixed = AspectValue(o.aspect);
        const bool keep = fixed > 0 || !shift;
        if (keep) {
            const double ar = fixed > 0 ? fixed : (o.w * W) / (o.h * H);
            double w = R - L, h = B - T;
            const bool corner = (hd & (HL | HR)) && (hd & (HT | HB));
            if (corner) {
                if (std::fabs(w / (o.w * W) - 1) >= std::fabs(h / (o.h * H) - 1)) h = w / ar;
                else w = h * ar;
                if (hd & HL) L = R - w;
                else R = L + w;
                if (hd & HT) T = B - h;
                else B = T + h;
            } else if (hd & (HL | HR)) {
                h = w / ar;
                const double cy = (o.y + o.h / 2) * H;
                T = cy - h / 2;
                B = cy + h / 2;
            } else {
                w = h * ar;
                const double cx = (o.x + o.w / 2) * W;
                L = cx - w / 2;
                R = cx + w / 2;
            }
        }
        n.x = L / W;
        n.y = T / H;
        n.w = (R - L) / W;
        n.h = (B - T) / H;
    } else if (g_drag.mode == 3 && g_drag.nw > 0) {
        // Alt: crop the edge, the rest of the picture stays where it is
        const double DW = g_drag.nw, DH = g_drag.nh;
        RectD Cc, D;
        ItemLayout(o, W, H, DW, DH, &Cc, &D);
        const double kx = Cc.W() / D.W(), ky = Cc.H() / D.H();
        const int hd = g_drag.handle;
        auto pm = [](double disp, double full) { return disp / full * 1000.0; };
        if (hd & HL) {
            const double c = std::clamp(o.crop[0] + pm(dx / kx, DW), 0.0, 900.0 - o.crop[2]);
            n.crop[0] = static_cast<int>(std::lround(c));
            const double real = (n.crop[0] - o.crop[0]) / 1000.0 * DW * kx;
            n.x = (L + real) / W;
            n.w = (R - L - real) / W;
        }
        if (hd & HR) {
            const double c = std::clamp(o.crop[2] - pm(dx / kx, DW), 0.0, 900.0 - o.crop[0]);
            n.crop[2] = static_cast<int>(std::lround(c));
            const double real = (n.crop[2] - o.crop[2]) / 1000.0 * DW * kx;
            n.w = (R - L - real) / W;
        }
        if (hd & HT) {
            const double c = std::clamp(o.crop[1] + pm(dy / ky, DH), 0.0, 900.0 - o.crop[3]);
            n.crop[1] = static_cast<int>(std::lround(c));
            const double real = (n.crop[1] - o.crop[1]) / 1000.0 * DH * ky;
            n.y = (T + real) / H;
            n.h = (B - T - real) / H;
        }
        if (hd & HB) {
            const double c = std::clamp(o.crop[3] - pm(dy / ky, DH), 0.0, 900.0 - o.crop[1]);
            n.crop[3] = static_cast<int>(std::lround(c));
            const double real = (n.crop[3] - o.crop[3]) / 1000.0 * DH * ky;
            n.h = (B - T - real) / H;
        }
        n.w = (std::max)(n.w, 4.0 / W);
        n.h = (std::max)(n.h, 4.0 / H);
    }
    std::lock_guard<std::mutex> lk(g_projMx);
    if (Item* it = FindItem(g_proj, o.uid)) {
        it->x = n.x;
        it->y = n.y;
        it->w = n.w;
        it->h = n.h;
        for (int e = 0; e < 4; ++e) it->crop[e] = n.crop[e];
        it->needFit = false;
    }
    ++g_projVer;
    g_projDirty = true;
}

void DrawHandle(HDC dc, int x, int y, HBRUSH br) {
    const int hs = S(4);
    RECT r{x - hs, y - hs, x + hs + 1, y + hs + 1};
    FillRect(dc, &r, br);
}

void PaintPreview(HWND hwnd, HDC dc) {
    const int which = g_pvWhich;
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, g_brList);
    Project p = ProjSnapshot();
    const Scene* sc = PvScene(p);
    int W, H;
    UiOutputSize(&W, &H);
    Published& pub = which == 0 ? g_pubProgram : g_pubNext;
    const bool editing = (which == 1) == EditingNext(p);
    if (which == 1 && !sc) {
        pub.wantW = 0;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kDarkDim);
        DrawTextW(dc, L"Выберите сцену выше —\nона станет «Следующей»", -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        return;
    }
    // header (only «Сейчас»)
    const int hh = which == 0 ? S(24) : 0;
    if (which == 0) {
    RECT hdr{0, 0, rc.right, hh};
    HBRUSH hb = CreateSolidBrush(kDarkPanel);
    FillRect(dc, &hdr, hb);
    DeleteObject(hb);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g_fontBold);
    SetTextColor(dc, kDarkText);
    RECT ht{S(8), 0, rc.right / 2, hh};
    std::wstring title = L"СЕЙЧАС:  " + (sc ? sc->name : std::wstring());
    DrawTextW(dc, title.c_str(), -1, &ht, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    std::wstring badge;
    COLORREF bc;
    if (!g_streaming) {
        badge = L"■ КАМЕРА ОСТАНОВЛЕНА";
        bc = kDarkDim;
    } else if (g_paused) {
        badge = L"❚❚ ПАУЗА";
        bc = RGB(235, 170, 0);
    } else {
        badge = L"● В ЭФИРЕ";
        bc = RGB(240, 70, 70);
    }
    wchar_t fmt[64];
    swprintf(fmt, 64, L"   %d×%d  %.1f к/с · %.1f мс", W, H, g_fpsX10.load() / 10.0, g_composeAvgX10.load() / 10.0);
    SetTextColor(dc, bc);
    RECT bt{rc.right / 2, 0, rc.right - S(8), hh};
    DrawTextW(dc, (badge + fmt).c_str(), -1, &bt, DT_VCENTER | DT_SINGLELINE | DT_RIGHT);
    SelectObject(dc, of);
    }
    // frame area
    const int pad = which == 0 ? S(4) : S(2);
    RECT area{pad, hh + pad, rc.right - pad, rc.bottom - pad};
    const int aw = area.right - area.left, ah = area.bottom - area.top;
    const double s = (std::min)(static_cast<double>(aw) / W, static_cast<double>(ah) / H);
    const int fw = (std::max)(1, static_cast<int>(W * s)), fh = (std::max)(1, static_cast<int>(H * s));
    RECT fr{area.left + (aw - fw) / 2, area.top + (ah - fh) / 2, 0, 0};
    fr.right = fr.left + fw;
    fr.bottom = fr.top + fh;
    PV().frame = fr;
    PV().scale = s;
    PV().W = W;
    PV().H = H;
    if (which == 0)
        pub.wantW = (g_streaming || p.liveWhenStopped) && g_uiVisible ? fw : 0;
    else
        pub.wantW = g_uiVisible ? fw : 0;
    pub.wantH = fh;
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &fr, black);
    DeleteObject(black);
    if (pub.wantW > 0) {
        PaintPublished(dc, pub, fr, nullptr);
    } else {
        SetTextColor(dc, kDarkDim);
        DrawTextW(dc, L"Камера остановлена. Предпросмотр включается в «Настройки → Общие».", -1, &fr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // output crop: dim what the camera does not show
    const RectD V = OutputVisibleRect(p.out, W, H);
    if (V.x0 > 0.5 || V.y0 > 0.5 || V.x1 < W - 0.5 || V.y1 < H - 0.5) {
        const RECT vr{static_cast<LONG>(fr.left + V.x0 * s), static_cast<LONG>(fr.top + V.y0 * s),
                      static_cast<LONG>(fr.left + V.x1 * s), static_cast<LONG>(fr.top + V.y1 * s)};
        const RECT parts[4] = {{fr.left, fr.top, fr.right, vr.top},
                               {fr.left, vr.bottom, fr.right, fr.bottom},
                               {fr.left, vr.top, vr.left, vr.bottom},
                               {vr.right, vr.top, fr.right, vr.bottom}};
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP one = CreateCompatibleBitmap(dc, 1, 1);
        HGDIOBJ ob = SelectObject(mdc, one);
        SetPixel(mdc, 0, 0, RGB(0, 0, 0));
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 150, 0};
        for (const RECT& pr : parts)
            if (pr.right > pr.left && pr.bottom > pr.top)
                AlphaBlend(dc, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, mdc, 0, 0, 1, 1, bf);
        SelectObject(mdc, ob);
        DeleteObject(one);
        DeleteDC(mdc);
        HPEN pen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, vr.left, vr.top, vr.right, vr.bottom);
        SelectObject(dc, obr);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    // the scene being edited: a thin blue outline
    if (editing && p.next >= 0 && p.next < static_cast<int>(p.scenes.size()) && p.next != p.cur) {
        HPEN pen = CreatePen(PS_SOLID, (std::max)(2, S(2)), RGB(70, 140, 255));
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, fr.left - 1, fr.top - 1, fr.right + 1, fr.bottom + 1);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    if (which == 1 && sc && sc->items.empty()) {
        SetTextColor(dc, kDarkDim);
        DrawTextW(dc, L"Пусто: щёлкните здесь и добавьте источник (＋ в «Источниках»)", -1, &fr,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        return;
    }
    // empty scene: big buttons
    if (which == 0) g_empty = !sc || sc->items.empty();
    if (which == 0 && g_empty) {
        const int bw = (std::min)(fw - S(40), S(340)), bh = S(52);
        const int cx = (fr.left + fr.right) / 2, cy = (fr.top + fr.bottom) / 2;
        g_emptyAdd = RECT{cx - bw / 2, cy - bh - S(8), cx + bw / 2, cy - S(8)};
        g_emptyJson = RECT{cx - bw / 2, cy + S(8), cx + bw / 2, cy + S(8) + bh * 3 / 4};
        HBRUSH b1 = CreateSolidBrush(RGB(52, 96, 190)), b2 = CreateSolidBrush(kDarkBtn);
        HGDIOBJ ob = SelectObject(dc, b1);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, g_emptyAdd.left, g_emptyAdd.top, g_emptyAdd.right, g_emptyAdd.bottom, S(10), S(10));
        SelectObject(dc, b2);
        RoundRect(dc, g_emptyJson.left, g_emptyJson.top, g_emptyJson.right, g_emptyJson.bottom, S(10), S(10));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b1);
        DeleteObject(b2);
        SetTextColor(dc, RGB(255, 255, 255));
        HGDIOBJ f2 = SelectObject(dc, g_fontBig);
        DrawTextW(dc, L"＋  Добавить источник", -1, &g_emptyAdd, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, g_font);
        DrawTextW(dc, L"Открыть JSON с настройками", -1, &g_emptyJson, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT hint{fr.left, g_emptyJson.bottom + S(10), fr.right, g_emptyJson.bottom + S(40)};
        SetTextColor(dc, kDarkDim);
        DrawTextW(dc, L"или правая кнопка мыши здесь → «Добавить источник»", -1, &hint, DT_CENTER | DT_SINGLELINE);
        SelectObject(dc, f2);
        return;
    }
    // hover + selection
    const Item* hov = nullptr;
    const Item* sel = nullptr;
    for (const auto& it : sc->items) {
        if (it.uid == g_hoverUid && it.visible) hov = &it;
        if (it.uid == g_selUid) sel = &it;
    }
    HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (hov && hov != sel) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 120, 120));
        HGDIOBJ op = SelectObject(dc, pen);
        const RECT r = BoxClient(*hov);
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    if (sel) {
        const COLORREF col = sel->locked ? RGB(160, 160, 160) : RGB(240, 40, 40);
        HPEN pen = CreatePen(PS_SOLID, (std::max)(1, S(2) / 2 + 1), col);
        HGDIOBJ op = SelectObject(dc, pen);
        const RECT r = BoxClient(*sel);
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, op);
        DeleteObject(pen);
        if (!sel->locked) {
            HBRUSH hbB = CreateSolidBrush(col);
            const int xs[3] = {r.left, (r.left + r.right) / 2, r.right}, ys[3] = {r.top, (r.top + r.bottom) / 2, r.bottom};
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i)
                    if (i != 1 || j != 1) DrawHandle(dc, xs[i], ys[j], hbB);
            DeleteObject(hbB);
        }
        // name tag
        SetTextColor(dc, RGB(255, 255, 255));
        RECT tag{r.left, r.top - S(18), r.left + S(260), r.top};
        if (tag.top < fr.top) OffsetRect(&tag, 0, S(20));
        DrawTextW(dc, sel->name.c_str(), -1, &tag, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, obr);
}

LRESULT PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const int which = g_pvWhich;
    // a click in a preview makes its scene the one being edited (studio mode)
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_LBUTTONDBLCLK) {
        Project p0 = ProjSnapshot();
        if (which == 1 && !PvScene(p0)) return 0;
        if (which == 1 && msg == WM_LBUTTONDBLCLK && !HitItem(p0, static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)))) {
            SwapNext();  // double click on an empty spot of «Следующий»: put it on air
            return 0;
        }
        SetEditNext(which == 1);
    }
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC m = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, (std::max)(1L, rc.right), (std::max)(1L, rc.bottom));
        HGDIOBJ old = SelectObject(m, bmp);
        SelectObject(m, g_font);
        PaintPreview(hwnd, m);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, old);
        DeleteObject(bmp);
        DeleteDC(m);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (which == 0 && g_empty) {
            if (PtInRect(&g_emptyAdd, pt) || PtInRect(&g_emptyJson, pt)) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        Project p = ProjSnapshot();
        const Item* sel = nullptr;
        int hd = g_drag.mode == 2 || g_drag.mode == 3 ? g_drag.handle : 0;
        if (!hd && CurItemC(p, g_selUid, &sel) && !sel->locked) hd = HitHandle(*sel, pt.x, pt.y);
        if (hd) {
            const wchar_t* c = ((hd & (HL | HT)) == (HL | HT) || (hd & (HR | HB)) == (HR | HB)) ? IDC_SIZENWSE
                               : ((hd & (HR | HT)) == (HR | HT) || (hd & (HL | HB)) == (HL | HB)) ? IDC_SIZENESW
                               : (hd & (HL | HR)) ? IDC_SIZEWE
                                                  : IDC_SIZENS;
            SetCursor(LoadCursor(nullptr, c));
            return TRUE;
        }
        if (g_drag.mode == 1) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
        if (g_drag.mode) {
            DragTo(x, y, (wp & MK_SHIFT) != 0, (wp & MK_CONTROL) != 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        Project p = ProjSnapshot();
        const int h = HitItem(p, x, y);
        if (h != g_hoverUid) {
            g_hoverUid = h;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hoverUid) {
            g_hoverUid = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
        SetFocus(hwnd);
        POINT pt{x, y};
        if (which == 0 && g_empty) {
            if (PtInRect(&g_emptyAdd, pt)) {
                POINT sp{g_emptyAdd.left, g_emptyAdd.bottom};
                ClientToScreen(hwnd, &sp);
                AddSourceMenu(g_main, sp);
            } else if (PtInRect(&g_emptyJson, pt)) {
                DoImportJson(g_main);
            }
            return 0;
        }
        Project p = ProjSnapshot();
        const Item* sel = nullptr;
        const bool alt = GetKeyState(VK_MENU) < 0;
        if (CurItemC(p, g_selUid, &sel) && !sel->locked && sel->visible) {
            const int hd = HitHandle(*sel, x, y);
            if (hd) {
                g_drag.mode = alt ? 3 : 2;
                g_drag.handle = hd;
                g_drag.start = pt;
                g_drag.orig = *sel;
                if (!NativeSize(sel->uid, &g_drag.nw, &g_drag.nh)) g_drag.nw = g_drag.nh = 0;
                SetCapture(hwnd);
                return 0;
            }
        }
        const int h = HitItem(p, x, y);
        if (h != g_selUid) UiSelect(h);
        const Item* it = nullptr;
        if (h && CurItemC(p, h, &it) && !it->locked) {
            g_drag.mode = 1;
            g_drag.start = pt;
            g_drag.orig = *it;
            SetCapture(hwnd);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag.mode) {
            g_drag.mode = 0;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED: g_drag.mode = 0; return 0;
    case WM_LBUTTONDBLCLK: {
        Project p = ProjSnapshot();
        const int h = HitItem(p, static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        if (h) OpenProperties(h, false);
        return 0;
    }
    case WM_RBUTTONUP: {
        const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
        Project p = ProjSnapshot();
        const int h = HitItem(p, x, y);
        if (h) UiSelect(h);
        POINT sp{x, y};
        ClientToScreen(hwnd, &sp);
        ItemContextMenu(h, sp);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_KEYDOWN: {
        if (!g_selUid) break;
        if (wp == VK_DELETE) {
            ItemCommand(g_selUid, ID_M_DEL);
            return 0;
        }
        int dx = 0, dy = 0;
        if (wp == VK_LEFT) dx = -1;
        if (wp == VK_RIGHT) dx = 1;
        if (wp == VK_UP) dy = -1;
        if (wp == VK_DOWN) dy = 1;
        if (dx || dy) {
            const int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
            const int uid = g_selUid;
            ProjEdit([&](Project& p) {
                if (Item* it = FindItem(p, uid)) {
                    if (it->locked) return;
                    it->x += dx * step / static_cast<double>((std::max)(1, PV().W));
                    it->y += dy * step / static_cast<double>((std::max)(1, PV().H));
                }
            });
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ProgramProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const int prev = g_pvWhich;
    g_pvWhich = 0;
    const LRESULT r = PreviewProc(hwnd, msg, wp, lp);
    g_pvWhich = prev;
    return r;
}

LRESULT CALLBACK NextProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const int prev = g_pvWhich;
    g_pvWhich = 1;
    const LRESULT r = PreviewProc(hwnd, msg, wp, lp);
    g_pvWhich = prev;
    return r;
}
}  // namespace

// ---------------------------------------------------------------------------
// Lists (owner-drawn, dark)
// ---------------------------------------------------------------------------
namespace {
std::vector<int> g_rowUid;  // sources list row -> item uid (top row = front)

void IconEye(HDC dc, int cx, int cy, bool on) {
    const COLORREF col = on ? kDarkText : RGB(90, 92, 100);
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int w = S(7), h = S(4);
    Ellipse(dc, cx - w, cy - h, cx + w + 1, cy + h + 1);
    if (on) {
        HBRUSH b = CreateSolidBrush(col);
        SelectObject(dc, b);
        Ellipse(dc, cx - S(2), cy - S(2), cx + S(2) + 1, cy + S(2) + 1);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        DeleteObject(b);
    } else {
        MoveToEx(dc, cx - w, cy + h + 1, nullptr);
        LineTo(dc, cx + w, cy - h - 1);
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

void IconLock(HDC dc, int cx, int cy, bool on) {
    const COLORREF col = on ? RGB(240, 190, 60) : RGB(90, 92, 100);
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int w = S(5), h = S(4);
    Rectangle(dc, cx - w, cy - 1, cx + w + 1, cy + h * 2);
    if (on)
        Arc(dc, cx - S(3), cy - S(7), cx + S(4), cy + S(1), cx + S(3), cy - 1, cx - S(3), cy - 1);
    else
        Arc(dc, cx - S(1), cy - S(7), cx + S(6), cy + S(1), cx + S(6), cy - 1, cx, cy - 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

void DrawSceneRow(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    Project p = ProjSnapshot();
    const int i = static_cast<int>(di->itemID);
    const bool cur = i == p.cur, nxt = i == p.next;
    HBRUSH bg = CreateSolidBrush(cur ? kDarkSel : kDarkList);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    if (i < 0 || i >= static_cast<int>(p.scenes.size())) return;
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, cur ? g_fontBold : g_font);
    SetTextColor(dc, kDarkText);
    RECT t{r.left + S(8), r.top, r.right - S(80), r.bottom};
    std::wstring name = p.scenes[static_cast<size_t>(i)].name;
    if (i < 4) name = std::to_wstring(i + 1) + L".  " + name;
    DrawTextW(dc, name.c_str(), -1, &t, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, g_font);
    RECT tag{r.right - S(84), r.top, r.right - S(6), r.bottom};
    if (cur) {
        SetTextColor(dc, RGB(255, 140, 140));
        DrawTextW(dc, L"сейчас", -1, &tag, DT_VCENTER | DT_SINGLELINE | DT_RIGHT);
    } else if (nxt) {
        SetTextColor(dc, RGB(140, 200, 255));
        DrawTextW(dc, L"следующий", -1, &tag, DT_VCENTER | DT_SINGLELINE | DT_RIGHT);
    }
}

void DrawSourceRow(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    const int row = static_cast<int>(di->itemID);
    const bool sel = row >= 0 && row < static_cast<int>(g_rowUid.size()) && g_rowUid[static_cast<size_t>(row)] == g_selUid;
    HBRUSH bg = CreateSolidBrush(sel ? kDarkSel : kDarkList);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    if (row < 0 || row >= static_cast<int>(g_rowUid.size())) return;
    Project p = ProjSnapshot();
    const Item* it = FindItemC(p, g_rowUid[static_cast<size_t>(row)]);
    if (!it) return;
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font);
    SetTextColor(dc, it->visible ? kDarkText : kDarkDim);
    RECT t{r.left + S(8), r.top, r.right - S(56), r.bottom};
    std::wstring label = it->name;
    DrawTextW(dc, label.c_str(), -1, &t, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    const int cy = (r.top + r.bottom) / 2;
    IconEye(dc, r.right - S(40), cy, it->visible);
    IconLock(dc, r.right - S(16), cy, it->locked);
}

LRESULT CALLBACK SourcesSub(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONUP) {
        const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
        const LRESULT r = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
        const int row = HIWORD(r) ? -1 : LOWORD(r);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (row >= 0 && row < static_cast<int>(g_rowUid.size())) {
            const int uid = g_rowUid[static_cast<size_t>(row)];
            if (msg == WM_LBUTTONDOWN && x >= rc.right - S(52)) {
                ItemCommand(uid, x >= rc.right - S(28) ? ID_M_LOCK : ID_M_VIS);
                return 0;
            }
            if (msg == WM_RBUTTONUP) {
                UiSelect(uid);
                POINT sp{x, y};
                ClientToScreen(hwnd, &sp);
                ItemContextMenu(uid, sp);
                return 0;
            }
        } else if (msg == WM_RBUTTONUP) {
            POINT sp{x, y};
            ClientToScreen(hwnd, &sp);
            ItemContextMenu(0, sp);
            return 0;
        }
    }
    if (msg == WM_KEYDOWN && wp == VK_DELETE && g_selUid) {
        ItemCommand(g_selUid, ID_M_DEL);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ScenesSub(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_RBUTTONUP) {
        const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
        const LRESULT r = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
        const int row = HIWORD(r) ? -1 : LOWORD(r);
        HMENU m = CreatePopupMenu();
        Project p = ProjSnapshot();
        const bool valid = row >= 0 && row < static_cast<int>(p.scenes.size());
        AppendMenuW(m, MF_STRING, ID_S_ADD, L"Добавить сцену");
        if (valid) {
            if (row != p.cur) AppendMenuW(m, MF_STRING, ID_S_NEXT, L"Сделать «Следующей»");
            AppendMenuW(m, MF_STRING, ID_S_REN, L"Переименовать…");
            AppendMenuW(m, MF_STRING, ID_S_DUP, L"Дублировать");
            AppendMenuW(m, MF_STRING, ID_S_UP, L"Выше");
            AppendMenuW(m, MF_STRING, ID_S_DOWN, L"Ниже");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, ID_S_DEL, L"Удалить");
        }
        POINT sp{x, y};
        ClientToScreen(hwnd, &sp);
        const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, g_main, nullptr);
        DestroyMenu(m);
        if (cmd) SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(cmd, 0), static_cast<LPARAM>(valid ? row : -1) + 0x10000);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void SceneCommand(int cmd, int row) {
    Project p = ProjSnapshot();
    const int n = static_cast<int>(p.scenes.size());
    if (row < 0) row = p.cur;
    switch (cmd) {
    case ID_S_ADD: {
        std::wstring name = UniqueSceneName(p, L"Сцена " + std::to_wstring(n + 1));
        if (!InputBox(g_main, L"Новая сцена", L"Название сцены:", &name)) return;
        ProjEdit([&](Project& pp) {
            Scene sc;
            sc.name = UniqueSceneName(pp, name);
            pp.scenes.push_back(sc);
            if (pp.next < 0) pp.next = static_cast<int>(pp.scenes.size()) - 1;
        });
        break;
    }
    case ID_S_REN: {
        if (row >= n) return;
        std::wstring name = p.scenes[static_cast<size_t>(row)].name;
        if (!InputBox(g_main, L"Переименовать сцену", L"Название сцены:", &name)) return;
        ProjEdit([&](Project& pp) {
            if (row < static_cast<int>(pp.scenes.size())) pp.scenes[static_cast<size_t>(row)].name = name;
        });
        break;
    }
    case ID_S_DUP:
        if (row >= n) return;
        ProjEdit([&](Project& pp) {
            Scene sc = pp.scenes[static_cast<size_t>(row)];
            sc.name = UniqueSceneName(pp, sc.name);
            for (auto& it : sc.items) it.uid = pp.nextUid++;
            pp.scenes.insert(pp.scenes.begin() + row + 1, sc);
            if (pp.cur > row) ++pp.cur;
            if (pp.next > row) ++pp.next;
        });
        break;
    case ID_S_DEL: {
        if (n <= 1) {
            SetNote(L"Последнюю сцену удалить нельзя.");
            return;
        }
        if (MessageBoxW(g_main, (L"Удалить сцену «" + p.scenes[static_cast<size_t>(row)].name + L"» со всеми источниками?").c_str(),
                        L"WVC", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        ProjEdit([&](Project& pp) {
            pp.scenes.erase(pp.scenes.begin() + row);
            const int m = static_cast<int>(pp.scenes.size());
            auto fix = [&](int& v) {
                if (v == row) v = -1;
                else if (v > row) --v;
            };
            fix(pp.cur);
            fix(pp.next);
            if (pp.cur < 0) pp.cur = (std::min)(row, m - 1);
            if (pp.next == pp.cur) pp.next = -1;
            if (pp.out.native && !FindItem(pp, pp.out.nativeUid)) pp.out.native = false;
        });
        g_selUid = 0;
        break;
    }
    case ID_S_NEXT:
        ProjEdit([&](Project& pp) {
            if (row != pp.cur) pp.next = row;
        });
        break;
    case ID_S_UP:
    case ID_S_DOWN: {
        const int to = cmd == ID_S_UP ? row - 1 : row + 1;
        if (to < 0 || to >= n) return;
        ProjEdit([&](Project& pp) {
            std::swap(pp.scenes[static_cast<size_t>(row)], pp.scenes[static_cast<size_t>(to)]);
            auto fix = [&](int& v) {
                if (v == row) v = to;
                else if (v == to) v = row;
            };
            fix(pp.cur);
            fix(pp.next);
        });
        break;
    }
    }
    UiRefresh();
}
}  // namespace

// ---------------------------------------------------------------------------
// Refresh / status / layout
// ---------------------------------------------------------------------------
void UiRefresh() {
    if (!g_main) return;
    g_syncing = true;
    Project p = ProjSnapshot();
    // scenes
    HWND sl = C(IDC_SCENES);
    SendMessageW(sl, WM_SETREDRAW, FALSE, 0);
    const int top = static_cast<int>(SendMessageW(sl, LB_GETTOPINDEX, 0, 0));
    SendMessageW(sl, LB_RESETCONTENT, 0, 0);
    for (const auto& sc : p.scenes) SendMessageW(sl, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sc.name.c_str()));
    SendMessageW(sl, LB_SETCURSEL, p.cur, 0);
    SendMessageW(sl, LB_SETTOPINDEX, top, 0);
    SendMessageW(sl, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(sl, nullptr, TRUE);
    // next combo: every scene but the current one
    HWND nc = C(IDC_NEXT_COMBO);
    SendMessageW(nc, CB_RESETCONTENT, 0, 0);
    SendMessageW(nc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"— не выбрана —"));
    SendMessageW(nc, CB_SETITEMDATA, 0, static_cast<LPARAM>(-1));
    int selIdx = 0;
    for (size_t i = 0; i < p.scenes.size(); ++i) {
        if (static_cast<int>(i) == p.cur) continue;
        const int k = static_cast<int>(SendMessageW(nc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.scenes[i].name.c_str())));
        SendMessageW(nc, CB_SETITEMDATA, k, static_cast<LPARAM>(i));
        if (static_cast<int>(i) == p.next) selIdx = k;
    }
    SendMessageW(nc, CB_SETCURSEL, selIdx, 0);
    // sources of the scene being edited («Сейчас» or «Следующий»), front first
    if (g_editNext && !EditingNext(p)) {
        g_editNext = false;  // «Следующий» is gone
        g_editingNext = false;
    }
    HWND src = C(IDC_SOURCES);
    SendMessageW(src, WM_SETREDRAW, FALSE, 0);
    const int stop = static_cast<int>(SendMessageW(src, LB_GETTOPINDEX, 0, 0));
    SendMessageW(src, LB_RESETCONTENT, 0, 0);
    g_rowUid.clear();
    const Scene* sc = UiEditSceneC(p);
    bool selFound = false;
    if (sc) {
        for (auto it = sc->items.rbegin(); it != sc->items.rend(); ++it) {
            g_rowUid.push_back(it->uid);
            SendMessageW(src, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(it->name.c_str()));
            if (it->uid == g_selUid) {
                SendMessageW(src, LB_SETCURSEL, g_rowUid.size() - 1, 0);
                selFound = true;
            }
        }
    }
    if (!selFound) {
        g_selUid = 0;
        SendMessageW(src, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    SendMessageW(src, LB_SETTOPINDEX, stop, 0);
    SendMessageW(src, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(src, nullptr, TRUE);
    // transitions
    SendMessageW(C(IDC_TR_TYPE), CB_SETCURSEL, std::clamp(p.transition, 0, TR_COUNT - 1), 0);
    SendMessageW(C(IDC_TR_DIR), CB_SETCURSEL, std::clamp(p.transDir, 0, 3), 0);
    SendMessageW(C(IDC_TR_EASE), CB_SETCURSEL, std::clamp(p.transEase, 0, 1), 0);
    EnableWindow(C(IDC_TR_DIR), TransUsesDir(p.transition));
    SetTextIfChanged(C(IDC_TR_MS), std::to_wstring(p.transMs));
    g_syncing = false;
    for (int d : {DOCK_SOURCES, DOCK_NEXT}) {
        RECT dr = g_dg[d].title;
        InvalidateRect(g_main, &dr, FALSE);
    }
    InvalidateRect(C(IDC_PROGRAM), nullptr, FALSE);
    InvalidateRect(C(IDC_NEXTPV), nullptr, FALSE);
    UiStatus();
}

void UiStatus() {
    if (!g_main) return;
    SetTextIfChanged(C(IDC_START), g_streaming ? L"■  Стоп камеры" : L"▶  Старт камеры");
    SetTextIfChanged(C(IDC_PAUSE), g_paused ? L"▶  Продолжить" : L"❚❚  Пауза");
    EnableWindow(C(IDC_PAUSE), g_streaming);
    InvalidateRect(C(IDC_START), nullptr, FALSE);
    Project p = ProjSnapshot();
    std::wstring s;
    if (!CameraDllLoaded())
        s = L"Нет WVCCam.dll рядом с WVC.exe — камера недоступна. ";
    else if (!g_camOk)
        s = L"Камера не запустилась. ";
    else {
        const int inst = CameraInstallState();
        if (inst == 0) s = L"Камера «WVC Camera» не установлена в Windows: «Настройки → Дополнительно → Установить». ";
        else if (inst == 2) s = L"«WVC Camera» установлена из другой папки: переустановите в «Настройки → Дополнительно». ";
    }
    s += g_streaming ? (g_paused ? L"Пауза — камера показывает картинку паузы." : L"Камера показывает сцену «" +
                                                                                    (CurSceneC(p) ? CurSceneC(p)->name : L"") + L"».")
                     : L"Камера остановлена (показывает картинку паузы).";
    {
        const std::wstring st = RenderStatsLine();
        if (!st.empty()) s += L"  " + st + L".";
    }
    // phones in the current scene
    if (const Scene* sc = CurSceneC(p)) {
        for (const auto& it : sc->items) {
            if (it.type != IT_PHONE || !it.visible) continue;
            const PhoneInfo pi = PhoneInfoFor(InputKeyOf(it));
            if (!pi.exists || pi.status != 2) continue;
            wchar_t b[200];
            swprintf(b, 200, L"  %ls: %ls, приём %.0f к/с, показ %.0f к/с, %.1f Мбит/с.", it.name.c_str(),
                     pi.codec.empty() ? L"JPEG" : pi.codec.c_str(), pi.recvFpsX10 / 10.0, pi.decFpsX10 / 10.0,
                     pi.kbps / 1000.0);
            s += b;
        }
    }
    if (!g_note.empty() && GetTickCount64() < g_noteAt + 15000) s = g_note + L"   " + s;
    if (!g_hotkeyNote.empty()) s += L"  " + g_hotkeyNote;
    if (s != g_statusText) {
        g_statusText = s;
        InvalidateRect(g_main, &g_statusRect, FALSE);
    }
    const wchar_t* tip = g_streaming ? (g_paused ? L"WVC — пауза" : L"WVC — в эфире") : L"WVC — камера остановлена";
    const int state = g_streaming ? (g_paused ? 1 : 0) : 2;  // tray icon: red dot / yellow / grey
    static int shownState = -1;
    if (g_nid.hWnd && (wcscmp(g_nid.szTip, tip) != 0 || state != shownState)) {
        static HICON icons[3] = {};
        if (!icons[state])
            icons[state] = static_cast<HICON>(LoadImageW(g_hInst, MAKEINTRESOURCEW(2 + state), IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        wcsncpy(g_nid.szTip, tip, 127);
        g_nid.uFlags = NIF_TIP | NIF_ICON;
        if (icons[state]) g_nid.hIcon = icons[state];
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        shownState = state;
    }
}

namespace {
// --- panels under the preview: order, width (splitters), collapse (title only), close; drag a title to reorder
struct Splitter {
    int left = -1, right = -1;  // dock ids
    RECT r{};
};
std::vector<Splitter> g_splits;
RECT g_hsplit{};      // between the preview and the panels
int g_availW = 1, g_sumW = 1;
struct DockDrag {
    int mode = 0;     // 1 splitter, 2 height, 3 move a panel (title)
    int split = -1, dock = -1;
    POINT start{};
    int w0[DOCK_COUNT] = {}, h0 = 0;
    bool moved = false;
    int dropIdx = -1, dropX = 0;
} g_dd;
const wchar_t* const kDockTitles[DOCK_COUNT] = {L"Следующий", L"Сцены", L"Источники", L"Переходы", L"Управление"};

std::vector<int> DockCtls(int d) {
    switch (d) {
    case DOCK_NEXT: return {IDC_NEXT_COMBO, IDC_SWAP, IDC_NEXTPV};
    case DOCK_SCENES: return {IDC_SCENES, IDC_SC_ADD, IDC_SC_DEL, IDC_SC_REN, IDC_SC_UP, IDC_SC_DOWN};
    case DOCK_SOURCES: return {IDC_SOURCES, IDC_SR_ADD, IDC_SR_DEL, IDC_SR_PROPS, IDC_SR_UP, IDC_SR_DOWN};
    case DOCK_TRANS: return {IDC_TR_TYPE, IDC_TR_DIR, IDC_TR_EASE, IDC_TR_MS, IDC_TR_SPIN};
    default: return {IDC_START, IDC_PAUSE, IDC_SETTINGS, IDC_EXIT};
    }
}

int g_trLabelY[3] = {};  // transitions dock: label rows (direction, smoothness, duration)

void Layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom, m = S(6);
    const int statusH = S(26);
    g_titleH = S(24);
    const int th = g_titleH, pad = S(4), bh = S(26);
    std::vector<int> vis;
    bool anyOpen = false;
    for (int k = 0; k < DOCK_COUNT; ++k) {
        const int d = g_docks.order[k];
        if (!g_docks.vis[d]) continue;
        vis.push_back(d);
        if (!g_docks.coll[d]) anyOpen = true;
    }
    int dockH = 0;
    if (!vis.empty()) dockH = anyOpen ? std::clamp(S(g_docks.height), S(210), (std::max)(S(210), h - statusH - S(180))) : th;
    const int y1 = h - statusH;
    const int y0 = vis.empty() ? y1 : y1 - dockH;
    const int progBottom = vis.empty() ? y1 - m : y0 - m;
    MoveWindow(C(IDC_PROGRAM), m, m, w - 2 * m, (std::max)(S(60), progBottom - m), TRUE);
    g_statusRect = RECT{0, y1, w, h};
    g_hsplit = anyOpen ? RECT{0, progBottom, w, y0} : RECT{0, 0, 0, 0};
    for (int d = 0; d < DOCK_COUNT; ++d) {
        g_dock[d] = RECT{0, 0, 0, 0};
        g_dg[d] = DockGeo();
    }
    g_splits.clear();
    // widths: collapsed panels are narrow strips (or title tabs when every panel is collapsed)
    HDC dc = GetDC(hwnd);
    HGDIOBJ of = SelectObject(dc, g_fontBold);
    int fixedW = 0, sumW = 0;
    std::vector<int> wid(DOCK_COUNT, 0);
    for (int d : vis) {
        if (g_docks.coll[d]) {
            if (anyOpen) {
                wid[d] = S(26);
            } else {
                SIZE sz{};
                GetTextExtentPoint32W(dc, kDockTitles[d], static_cast<int>(wcslen(kDockTitles[d])), &sz);
                wid[d] = sz.cx + S(64);
            }
            fixedW += wid[d];
        } else {
            sumW += g_docks.weight[d];
        }
    }
    SelectObject(dc, of);
    ReleaseDC(hwnd, dc);
    const int gaps = vis.empty() ? 0 : static_cast<int>(vis.size() - 1) * m;
    const int avail = (std::max)(1, w - 2 * m - gaps - fixedW);
    g_availW = avail;
    g_sumW = (std::max)(1, sumW);
    int x = m;
    int prevOpen = -1;
    for (size_t k = 0; k < vis.size(); ++k) {
        const int d = vis[k];
        int ww = wid[d];
        if (!g_docks.coll[d]) ww = (std::max)(S(60), static_cast<int>(static_cast<long long>(avail) * g_docks.weight[d] / g_sumW));
        if (k + 1 == vis.size() && !g_docks.coll[d]) ww = (std::max)(S(60), w - m - x);
        g_dock[d] = RECT{x, y0, x + ww, y1};
        DockGeo& g = g_dg[d];
        g.strip = g_docks.coll[d] && anyOpen;
        const int bs = S(20);
        if (g.strip) {
            g.title = g_dock[d];
            g.btnColl = RECT{x + (ww - bs) / 2, y0 + S(3), x + (ww + bs) / 2, y0 + S(3) + bs};
            g.btnClose = RECT{g.btnColl.left, g.btnColl.bottom + S(2), g.btnColl.right, g.btnColl.bottom + S(2) + bs};
        } else {
            g.title = RECT{x, y0, x + ww, y0 + th};
            g.btnClose = RECT{x + ww - bs - S(2), y0 + (th - bs) / 2, x + ww - S(2), y0 + (th - bs) / 2 + bs};
            g.btnColl = RECT{g.btnClose.left - bs - S(2), g.btnClose.top, g.btnClose.left - S(2), g.btnClose.bottom};
        }
        if (k > 0 && prevOpen >= 0 && !g_docks.coll[d] && vis[k - 1] == prevOpen) {
            Splitter sp;
            sp.left = prevOpen;
            sp.right = d;
            sp.r = RECT{x - m, y0, x, y1};
            g_splits.push_back(sp);
        }
        prevOpen = g_docks.coll[d] ? -1 : d;
        x += ww + m;
    }
    // controls
    for (int d = 0; d < DOCK_COUNT; ++d) {
        const bool open = g_docks.vis[d] && !g_docks.coll[d];
        for (int id : DockCtls(d)) ShowWindow(C(id), open ? SW_SHOWNA : SW_HIDE);
    }
    if (g_docks.vis[DOCK_NEXT] && !g_docks.coll[DOCK_NEXT]) {
        const RECT d = g_dock[DOCK_NEXT];
        const int cy = d.top + th + pad;
        const int swapW = S(96);
        MoveWindow(C(IDC_NEXT_COMBO), d.left + pad, cy, d.right - d.left - 3 * pad - swapW, S(300), TRUE);
        MoveWindow(C(IDC_SWAP), d.right - pad - swapW, cy, swapW, bh, TRUE);
        MoveWindow(C(IDC_NEXTPV), d.left + pad, cy + bh + pad, d.right - d.left - 2 * pad,
                   (std::max)(S(20), static_cast<int>(d.bottom - pad - (cy + bh + pad))), TRUE);
    }
    auto listDock = [&](const RECT& d, int list, int b0, int nb) {
        const int lh = d.bottom - d.top - th - bh - 3 * pad;
        MoveWindow(C(list), d.left + pad, d.top + th + pad, d.right - d.left - 2 * pad, (std::max)(S(30), lh), TRUE);
        const int by = d.bottom - pad - bh;
        const int bw = S(34);
        for (int i = 0; i < nb; ++i) MoveWindow(C(b0 + i), d.left + pad + i * (bw + S(2)), by, bw, bh, TRUE);
    };
    if (g_docks.vis[DOCK_SCENES] && !g_docks.coll[DOCK_SCENES]) listDock(g_dock[DOCK_SCENES], IDC_SCENES, IDC_SC_ADD, 5);
    if (g_docks.vis[DOCK_SOURCES] && !g_docks.coll[DOCK_SOURCES]) listDock(g_dock[DOCK_SOURCES], IDC_SOURCES, IDC_SR_ADD, 5);
    if (g_docks.vis[DOCK_TRANS] && !g_docks.coll[DOCK_TRANS]) {
        const RECT d = g_dock[DOCK_TRANS];
        const int cw = d.right - d.left - 2 * pad;
        int y = d.top + th + pad;
        MoveWindow(C(IDC_TR_TYPE), d.left + pad, y, cw, S(400), TRUE);
        y += bh + S(4);
        g_trLabelY[0] = y;
        y += S(18);
        MoveWindow(C(IDC_TR_DIR), d.left + pad, y, cw, S(300), TRUE);
        y += bh + S(4);
        g_trLabelY[1] = y;
        y += S(18);
        MoveWindow(C(IDC_TR_EASE), d.left + pad, y, cw, S(300), TRUE);
        y += bh + S(4);
        g_trLabelY[2] = y;
        y += S(18);
        MoveWindow(C(IDC_TR_MS), d.left + pad, y, cw - S(60), bh - 2, TRUE);
        SendMessageW(C(IDC_TR_SPIN), UDM_SETBUDDY, reinterpret_cast<WPARAM>(C(IDC_TR_MS)), 0);
    }
    if (g_docks.vis[DOCK_CONTROLS] && !g_docks.coll[DOCK_CONTROLS]) {
        const RECT d = g_dock[DOCK_CONTROLS];
        int y = d.top + th + pad;
        const int bw = d.right - d.left - 2 * pad, big = S(36);
        MoveWindow(C(IDC_START), d.left + pad, y, bw, big, TRUE);
        y += big + pad;
        MoveWindow(C(IDC_PAUSE), d.left + pad, y, bw, bh + 2, TRUE);
        y += bh + 2 + pad;
        MoveWindow(C(IDC_SETTINGS), d.left + pad, y, bw, bh + 2, TRUE);
        y += bh + 2 + pad;
        MoveWindow(C(IDC_EXIT), d.left + pad, y, bw, bh + 2, TRUE);
    }
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void DockChanged() {
    DocksSave();
    if (g_main) {
        Layout(g_main);
        DrawMenuBar(g_main);
    }
}

void DrawGlyph(HDC dc, const RECT& r, const wchar_t* t, bool hot) {
    if (hot) {
        HBRUSH b = CreateSolidBrush(RGB(70, 72, 82));
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    SetTextColor(dc, kDarkText);
    RECT rr = r;
    DrawTextW(dc, t, -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void PaintMain(HWND hwnd, HDC dc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, g_brDark);
    SetBkMode(dc, TRANSPARENT);
    POINT mp;
    GetCursorPos(&mp);
    ScreenToClient(hwnd, &mp);
    Project p = ProjSnapshot();
    const bool editNext = EditingNext(p);
    for (int d = 0; d < DOCK_COUNT; ++d) {
        if (!g_docks.vis[d]) continue;
        const RECT r = g_dock[d];
        const DockGeo& g = g_dg[d];
        HBRUSH pb = CreateSolidBrush(g_dd.mode == 3 && g_dd.dock == d && g_dd.moved ? RGB(58, 62, 80) : kDarkPanel);
        FillRect(dc, &r, pb);
        DeleteObject(pb);
        std::wstring title = kDockTitles[d];
        if (d == DOCK_SOURCES) {
            const Scene* sc = UiEditSceneC(p);
            title += editNext ? L"  —  ✎ Следующий" : L"  —  Сейчас";
            if (sc) title += L": " + sc->name;
        }
        if (d == DOCK_NEXT && editNext) title += L"  ✎";
        HGDIOBJ of = SelectObject(dc, g_fontBold);
        SetTextColor(dc, d == DOCK_SOURCES && editNext ? RGB(130, 180, 255) : kDarkText);
        if (g.strip) {
            LOGFONTW lf{};
            GetObjectW(g_fontBold, sizeof(lf), &lf);
            lf.lfEscapement = lf.lfOrientation = 2700;  // top to bottom
            HFONT vf = CreateFontIndirectW(&lf);
            SelectObject(dc, vf);
            TextOutW(dc, r.left + (r.right - r.left) / 2 + S(7), g.btnClose.bottom + S(8), kDockTitles[d],
                     static_cast<int>(wcslen(kDockTitles[d])));
            SelectObject(dc, g_fontBold);
            DeleteObject(vf);
        } else {
            RECT t{g.title.left + S(8), g.title.top, g.btnColl.left - S(4), g.title.bottom};
            DrawTextW(dc, title.c_str(), -1, &t, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (!g_docks.coll[d]) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 72, 80));
                HGDIOBJ op = SelectObject(dc, pen);
                MoveToEx(dc, r.left, r.top + g_titleH - 1, nullptr);
                LineTo(dc, r.right, r.top + g_titleH - 1);
                SelectObject(dc, op);
                DeleteObject(pen);
            }
        }
        SelectObject(dc, g_fontSym);
        DrawGlyph(dc, g.btnColl, g_docks.coll[d] ? L"▸" : L"▾", PtInRect(&g.btnColl, mp) != 0);
        DrawGlyph(dc, g.btnClose, L"×", PtInRect(&g.btnClose, mp) != 0);
        SelectObject(dc, of);
    }
    if (g_docks.vis[DOCK_TRANS] && !g_docks.coll[DOCK_TRANS]) {
        const RECT d = g_dock[DOCK_TRANS];
        SelectObject(dc, g_font);
        SetTextColor(dc, kDarkDim);
        const wchar_t* labels[3] = {L"Направление:", L"Плавность:", L"Длительность, мс (0 — сразу):"};
        for (int i = 0; i < 3; ++i) {
            RECT t{d.left + S(6), g_trLabelY[i], d.right - S(4), g_trLabelY[i] + S(18)};
            DrawTextW(dc, labels[i], -1, &t, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    // where a dragged panel would land
    if (g_dd.mode == 3 && g_dd.moved && g_dd.dropX > 0) {
        RECT mk{g_dd.dropX - S(2), g_dock[g_dd.dock].top, g_dd.dropX + S(2), g_statusRect.top};
        HBRUSH b = CreateSolidBrush(RGB(70, 140, 255));
        FillRect(dc, &mk, b);
        DeleteObject(b);
    }
    RECT st = g_statusRect;
    HBRUSH sb = CreateSolidBrush(RGB(30, 31, 35));
    FillRect(dc, &st, sb);
    DeleteObject(sb);
    InflateRect(&st, -S(8), 0);
    SelectObject(dc, g_font);
    SetTextColor(dc, kDarkDim);
    DrawTextW(dc, g_statusText.c_str(), -1, &st, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// mouse on the main window itself (titles, buttons, splitters)
int SplitAt(POINT pt) {
    for (size_t i = 0; i < g_splits.size(); ++i)
        if (PtInRect(&g_splits[i].r, pt)) return static_cast<int>(i);
    return -1;
}

int DockTitleAt(POINT pt) {
    for (int d = 0; d < DOCK_COUNT; ++d)
        if (g_docks.vis[d] && PtInRect(&g_dg[d].title, pt)) return d;
    return -1;
}

bool DockMouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
    switch (msg) {
    case WM_SETCURSOR: {
        POINT c;
        GetCursorPos(&c);
        ScreenToClient(hwnd, &c);
        if (LOWORD(lp) != HTCLIENT) return false;
        if (g_dd.mode == 1 || SplitAt(c) >= 0) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return true;
        }
        if (g_dd.mode == 2 || PtInRect(&g_hsplit, c)) {
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            return true;
        }
        if (g_dd.mode == 3 && g_dd.moved) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            return true;
        }
        return false;
    }
    case WM_LBUTTONDBLCLK: {
        const int d = DockTitleAt(pt);
        if (d >= 0 && !PtInRect(&g_dg[d].btnClose, pt) && !PtInRect(&g_dg[d].btnColl, pt)) {
            g_docks.coll[d] = !g_docks.coll[d];
            DockChanged();
            return true;
        }
        return false;
    }
    case WM_LBUTTONDOWN: {
        const int sp = SplitAt(pt);
        if (sp >= 0) {
            g_dd = DockDrag();
            g_dd.mode = 1;
            g_dd.split = sp;
            g_dd.start = pt;
            for (int d = 0; d < DOCK_COUNT; ++d) g_dd.w0[d] = g_docks.weight[d];
            SetCapture(hwnd);
            return true;
        }
        if (PtInRect(&g_hsplit, pt)) {
            g_dd = DockDrag();
            g_dd.mode = 2;
            g_dd.start = pt;
            g_dd.h0 = g_docks.height;
            SetCapture(hwnd);
            return true;
        }
        const int d = DockTitleAt(pt);
        if (d < 0) return false;
        if (PtInRect(&g_dg[d].btnClose, pt)) {
            g_docks.vis[d] = false;
            DockChanged();
            SetNote(std::wstring(L"Панель «") + kDockTitles[d] + L"» скрыта — вернуть: «Вид → Панели».");
            return true;
        }
        if (PtInRect(&g_dg[d].btnColl, pt)) {
            g_docks.coll[d] = !g_docks.coll[d];
            DockChanged();
            return true;
        }
        g_dd = DockDrag();
        g_dd.mode = 3;
        g_dd.dock = d;
        g_dd.start = pt;
        SetCapture(hwnd);
        return true;
    }
    case WM_MOUSEMOVE: {
        if (!g_dd.mode) {
            // hover highlight of the title buttons
            static int lastHot = -1;
            int hot = -1;
            for (int d = 0; d < DOCK_COUNT; ++d)
                if (g_docks.vis[d] && (PtInRect(&g_dg[d].btnClose, pt) || PtInRect(&g_dg[d].btnColl, pt))) hot = d;
            if (hot != lastHot) {
                if (lastHot >= 0) InvalidateRect(hwnd, &g_dg[lastHot].title, FALSE);
                if (hot >= 0) InvalidateRect(hwnd, &g_dg[hot].title, FALSE);
                lastHot = hot;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return false;
        }
        if (g_dd.mode == 1 && g_dd.split >= 0 && g_dd.split < static_cast<int>(g_splits.size())) {
            const Splitter& s = g_splits[static_cast<size_t>(g_dd.split)];
            const int dxw = static_cast<int>(static_cast<long long>(pt.x - g_dd.start.x) * g_sumW / g_availW);
            const int minW = static_cast<int>(static_cast<long long>(S(120)) * g_sumW / g_availW);
            int a = g_dd.w0[s.left] + dxw, b = g_dd.w0[s.right] - dxw;
            const int total = g_dd.w0[s.left] + g_dd.w0[s.right];
            a = std::clamp(a, minW, (std::max)(minW, total - minW));
            b = total - a;
            g_docks.weight[s.left] = a;
            g_docks.weight[s.right] = b;
            Layout(hwnd);
            return true;
        }
        if (g_dd.mode == 2) {
            const int dy = MulDiv(pt.y - g_dd.start.y, 96, static_cast<int>(g_dpi));
            g_docks.height = std::clamp(g_dd.h0 - dy, 150, 2000);
            Layout(hwnd);
            return true;
        }
        if (g_dd.mode == 3) {
            if (!g_dd.moved && std::abs(pt.x - g_dd.start.x) + std::abs(pt.y - g_dd.start.y) < S(6)) return true;
            g_dd.moved = true;
            // insertion point among the visible panels (order index)
            int idx = 0, best = INT_MAX;
            g_dd.dropX = 0;
            std::vector<int> vis;
            for (int k = 0; k < DOCK_COUNT; ++k)
                if (g_docks.vis[g_docks.order[k]]) vis.push_back(g_docks.order[k]);
            for (size_t k = 0; k <= vis.size(); ++k) {
                const int x = k < vis.size() ? g_dock[vis[k]].left - S(3) : g_dock[vis.back()].right + S(3);
                if (std::abs(pt.x - x) < best) {
                    best = std::abs(pt.x - x);
                    idx = static_cast<int>(k);
                    g_dd.dropX = x;
                }
            }
            g_dd.dropIdx = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
        return false;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED: {
        if (!g_dd.mode) return false;
        const DockDrag dd = g_dd;
        g_dd = DockDrag();
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        if (dd.mode == 3 && dd.moved && dd.dropIdx >= 0) {
            std::vector<int> vis, all(g_docks.order, g_docks.order + DOCK_COUNT);
            for (int d : all)
                if (g_docks.vis[d]) vis.push_back(d);
            const int from = static_cast<int>(std::find(vis.begin(), vis.end(), dd.dock) - vis.begin());
            int to = dd.dropIdx;
            if (from < static_cast<int>(vis.size())) {
                vis.erase(vis.begin() + from);
                if (to > from) --to;
                to = std::clamp(to, 0, static_cast<int>(vis.size()));
                vis.insert(vis.begin() + to, dd.dock);
            }
            // hidden panels keep their place at the end
            std::vector<int> order = vis;
            for (int d : all)
                if (!g_docks.vis[d]) order.push_back(d);
            for (int k = 0; k < DOCK_COUNT; ++k) g_docks.order[k] = order[static_cast<size_t>(k)];
        }
        if (dd.mode) DockChanged();
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    }
    return false;
}

void DrawButton(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    const int id = static_cast<int>(di->CtlID);
    const bool pressed = (di->itemState & ODS_SELECTED) != 0, disabled = (di->itemState & ODS_DISABLED) != 0;
    COLORREF bg = kDarkBtn;
    if (id == IDC_START) bg = g_streaming ? RGB(170, 45, 45) : RGB(40, 120, 70);
    if (id == IDC_SWAP) bg = RGB(52, 86, 160);
    if (pressed) bg = RGB(std::min(255, GetRValue(bg) + 25), std::min(255, GetGValue(bg) + 25), std::min(255, GetBValue(bg) + 25));
    HBRUSH b = CreateSolidBrush(disabled ? RGB(50, 51, 57) : bg);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(85, 88, 98));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    HBRUSH back = g_brPanel;
    FillRect(dc, &r, back);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(6), S(6));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(pen);
    wchar_t text[128] = {};
    GetWindowTextW(di->hwndItem, text, 128);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? RGB(110, 110, 118) : RGB(240, 240, 240));
    const bool sym = (id >= IDC_SC_ADD && id <= IDC_SC_DOWN) || (id >= IDC_SR_ADD && id <= IDC_SR_DOWN);
    HGDIOBJ of = SelectObject(dc, sym ? g_fontSym : (id == IDC_START ? g_fontBold : g_font));
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

// tooltips for the small buttons
HWND g_tips = nullptr;
void AddTip(int id, const wchar_t* text) {
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_main;
    ti.uId = reinterpret_cast<UINT_PTR>(C(id));
    ti.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(g_tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

void MakeFonts() {
    for (HFONT* f : {&g_font, &g_fontBold, &g_fontSym, &g_fontBig})
        if (*f) DeleteObject(*f);
    const int pt9 = -MulDiv(9, static_cast<int>(g_dpi), 72);
    g_font = CreateFontW(pt9, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontBold = CreateFontW(pt9, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontSym = CreateFontW(-MulDiv(11, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            L"Segoe UI Symbol");
    g_fontBig = CreateFontW(-MulDiv(13, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            L"Segoe UI");
}

HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    if (h && id > 0 && id < IDC_LAST) g_c[id] = h;
    if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    return h;
}

void CreateControls(HWND hwnd) {
    const DWORD od = WS_TABSTOP | BS_OWNERDRAW;
    Mk(hwnd, L"WVCProgram", L"", WS_TABSTOP, IDC_PROGRAM);
    Mk(hwnd, L"WVCNext", L"", 0, IDC_NEXTPV);
    HWND nc = Mk(hwnd, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_NEXT_COMBO);
    SetWindowTheme(nc, L"DarkMode_CFD", nullptr);
    Mk(hwnd, L"BUTTON", L"В эфир ⇅", od, IDC_SWAP);
    HWND sl = Mk(hwnd, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                                          LBS_NOINTEGRALHEIGHT, IDC_SCENES);
    SetWindowTheme(sl, L"DarkMode_Explorer", nullptr);
    SetWindowSubclass(sl, ScenesSub, 1, 0);
    const wchar_t* sb[5] = {L"＋", L"－", L"✎", L"▲", L"▼"};
    for (int i = 0; i < 5; ++i) Mk(hwnd, L"BUTTON", sb[i], od, IDC_SC_ADD + i);
    HWND srl = Mk(hwnd, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                                           LBS_NOINTEGRALHEIGHT, IDC_SOURCES);
    SetWindowTheme(srl, L"DarkMode_Explorer", nullptr);
    SetWindowSubclass(srl, SourcesSub, 2, 0);
    const wchar_t* rb[5] = {L"＋", L"－", L"⚙", L"▲", L"▼"};
    for (int i = 0; i < 5; ++i) Mk(hwnd, L"BUTTON", rb[i], od, IDC_SR_ADD + i);
    HWND tt = Mk(hwnd, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_TR_TYPE);
    SetWindowTheme(tt, L"DarkMode_CFD", nullptr);
    for (int k = 0; k < TR_COUNT; ++k) SendMessageW(tt, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kTransNames[k]));
    HWND td = Mk(hwnd, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_TR_DIR);
    SetWindowTheme(td, L"DarkMode_CFD", nullptr);
    for (const wchar_t* s : {L"← влево", L"→ вправо", L"↑ вверх", L"↓ вниз"}) SendMessageW(td, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    HWND te = Mk(hwnd, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_TR_EASE);
    SetWindowTheme(te, L"DarkMode_CFD", nullptr);
    for (const wchar_t* s : {L"Равномерно", L"Плавно (разгон и торможение)"}) SendMessageW(te, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    Mk(hwnd, L"EDIT", L"500", WS_TABSTOP | ES_NUMBER | ES_RIGHT, IDC_TR_MS, WS_EX_CLIENTEDGE);
    HWND spin = Mk(hwnd, UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, IDC_TR_SPIN);
    SendMessageW(spin, UDM_SETRANGE32, 0, 5000);
    UDACCEL acc{0, 50};
    SendMessageW(spin, UDM_SETACCEL, 1, reinterpret_cast<LPARAM>(&acc));
    Mk(hwnd, L"BUTTON", L"▶  Старт камеры", od, IDC_START);
    Mk(hwnd, L"BUTTON", L"❚❚  Пауза", od, IDC_PAUSE);
    Mk(hwnd, L"BUTTON", L"⚙  Настройки", od, IDC_SETTINGS);
    Mk(hwnd, L"BUTTON", L"Выход", od, IDC_EXIT);
    g_tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                             hwnd, nullptr, g_hInst, nullptr);
    SendMessageW(g_tips, TTM_SETMAXTIPWIDTH, 0, 420);
    AddTip(IDC_SC_ADD, L"Добавить сцену");
    AddTip(IDC_SC_DEL, L"Удалить сцену");
    AddTip(IDC_SC_REN, L"Переименовать сцену");
    AddTip(IDC_SC_UP, L"Сцену выше (первые четыре — Ctrl+Shift+1…4)");
    AddTip(IDC_SC_DOWN, L"Сцену ниже");
    AddTip(IDC_SR_ADD, L"Добавить источник в сцену");
    AddTip(IDC_SR_DEL, L"Удалить выбранный источник");
    AddTip(IDC_SR_PROPS, L"Свойства источника (двойной щелчок — то же)");
    AddTip(IDC_SR_UP, L"Выше (поверх других)");
    AddTip(IDC_SR_DOWN, L"Ниже");
    AddTip(IDC_SWAP, L"В эфир: «Следующий» становится «Сейчас» с переходом (Ctrl+Shift+F9 по умолчанию). "
                     L"Щёлкните по «Следующему», чтобы править его источники мышью.");
    AddTip(IDC_TR_DIR, L"Направление для шторки, сдвига, наплыва и жалюзи");
    AddTip(IDC_TR_MS, L"0 — смена без перехода");
    AddTip(IDC_START, L"Камера «WVC Camera» начинает показывать сцену (Ctrl+Shift+F5). Остановлена — картинка паузы.");
    AddTip(IDC_PAUSE, L"Пауза: камера показывает картинку паузы, источники остаются включены (Ctrl+Shift+6)");
}

// ---------------------------------------------------------------------------
// Timers: inputs, windows / monitors, fitting new sources, phone rotation, output size
// ---------------------------------------------------------------------------
ULONGLONG g_lastSlow = 0, g_lastSave = 0;
std::pair<int, int> g_nativeCand{0, 0};
ULONGLONG g_nativeSince = 0;

void FollowTick() {
    InputsSync();
    const ULONGLONG now = GetTickCount64();
    int W, H;
    UiOutputSize(&W, &H);
    bool changed = false, structural = false;
    const bool slow = now - g_lastSlow >= 1000;
    if (slow) g_lastSlow = now;
    std::vector<MonitorItem> mons;
    if (slow) mons = Monitors();
    {
        std::lock_guard<std::mutex> lk(g_projMx);
        for (auto& sc : g_proj.scenes)
            for (auto& it : sc.items) {
                if (slow && it.type == IT_WINDOW && (!it.hwnd || !IsWindow(it.hwnd)) && !it.winTitle.empty()) {
                    HWND h = FindWindowByTitle(it.winTitle);
                    if (h != it.hwnd) {
                        it.hwnd = h;
                        changed = true;
                    }
                }
                if (slow && it.type == IT_MONITOR) {
                    for (const auto& m : mons)
                        if (m.device == it.monDevice && !EqualRect(&m.rc, &it.monRect)) {
                            it.monRect = m.rc;
                            changed = true;
                        }
                }
                int nw = 0, nh = 0;
                const bool known = NativeSize(it.uid, &nw, &nh);
                if (it.needFit) {
                    const bool can = known || it.type == IT_REGION || it.type == IT_MONITOR || it.type == IT_TEXT ||
                                     (it.type == IT_IMAGE && !it.image.empty());
                    if (can) {
                        FitItemToScreen(it, W, H, false);
                        it.needFit = false;
                        changed = true;
                        if (it.type == IT_PHONE && known) it.phoneOrient = nw > nh ? 2 : 1;
                    }
                } else if (it.type == IT_PHONE && it.autoRotate && known) {
                    const int o = nw > nh ? 2 : 1;
                    if (it.phoneOrient != o) {
                        if (it.phoneOrient != 0) {  // the phone turned: so does its box (and shape)
                            const double cx = it.x + it.w / 2, cy = it.y + it.h / 2;
                            const double wpx = it.w * W, hpx = it.h * H;
                            it.w = hpx / W;
                            it.h = wpx / H;
                            it.x = cx - it.w / 2;
                            it.y = cy - it.h / 2;
                        }
                        it.phoneOrient = o;
                        changed = true;
                    }
                }
            }
    }
    if (changed) {
        ++g_projVer;
        g_projDirty = true;
    }
    // «Как у источника»: follow the source's size (stable for 1.5 s), then restart the camera
    Project p = ProjSnapshot();
    if (p.out.native) {
        const Item* it = FindItemC(p, p.out.nativeUid);
        int nw = 0, nh = 0;
        if (it && NativeSize(it->uid, &nw, &nh)) {
            const RectD d1 = ItemSourceRect(*it, nw, nh);
            int w = static_cast<int>(d1.W()) & ~1, h = static_cast<int>(d1.H()) & ~1;
            if (w >= 64 && h >= 64 && (w != p.out.nativeW || h != p.out.nativeH)) {
                if (g_nativeCand != std::make_pair(w, h)) {
                    g_nativeCand = {w, h};
                    g_nativeSince = now;
                } else if (now - g_nativeSince > 1500) {
                    ProjEdit([&](Project& pp) {
                        pp.out.nativeW = w;
                        pp.out.nativeH = h;
                    });
                    p = ProjSnapshot();
                }
            }
        }
    }
    int ow, oh;
    OutSizeFor(p.out, &ow, &oh);
    if (ow != g_camW || oh != g_camH || p.out.fps != g_camFps) {
        RestartCameraForFormat(p.out.native ? L"Формат камеры теперь как у источника: " : nullptr);
        structural = true;
    }
    UpdateFrames();
    UndoPoll(g_drag.mode != 0 || g_frameMoving || GetKeyState(VK_LBUTTON) < 0);
    if (g_projDirty && now - g_lastSave > 1000) {
        SaveProject();
        g_lastSave = now;
    }
    if (structural) UiRefresh();
}

void OnHotkey(int a) {
    Project p = ProjSnapshot();
    const Scene* sc = CurSceneC(p);
    auto firstOf = [&](std::initializer_list<int> types) -> int {
        const Item* sel = nullptr;
        if (CurItemC(p, g_selUid, &sel))
            for (int t : types)
                if (sel->type == t) return sel->uid;
        if (sc)
            for (auto it = sc->items.rbegin(); it != sc->items.rend(); ++it)
                for (int t : types)
                    if (it->type == t && it->visible) return it->uid;
        return 0;
    };
    switch (a) {
    case HA_TOGGLE: ToggleStreaming(); return;
    case HA_PAUSE: TogglePause(); return;
    case HA_SWAPNEXT: SwapNext(); return;
    case HA_SELECT: {
        const int uid = firstOf({IT_REGION});
        if (uid)
            ItemCommand(uid, ID_M_SELECTREGION);
        else
            AddSourceOfType(IT_REGION);
        return;
    }
    case HA_MONITOR: {
        const int uid = firstOf({IT_REGION, IT_MONITOR});
        if (!uid) return;
        std::vector<MonitorItem> mons = Monitors();
        if (mons.empty()) return;
        ProjEdit([&](Project& pp) {
            Item* it = FindItem(pp, uid);
            if (!it) return;
            const RECT cur = it->type == IT_MONITOR ? it->monRect : it->region;
            size_t idx = 0;
            for (size_t i = 0; i < mons.size(); ++i)
                if (EqualRect(&mons[i].rc, &cur)) idx = i + 1;
            const MonitorItem& m = mons[idx % mons.size()];
            if (it->type == IT_MONITOR) {
                it->monDevice = m.device;
                it->monRect = m.rc;
            } else {
                it->region = m.rc;
            }
        });
        return;
    }
    case HA_CURSOR: {
        bool on = false;
        if (sc)
            for (const auto& it : sc->items)
                if ((it.type == IT_REGION || it.type == IT_WINDOW || it.type == IT_MONITOR) && it.cursor) on = true;
        ProjEdit([&](Project& pp) {
            if (Scene* s = CurScene(pp))
                for (auto& it : s->items)
                    if (it.type == IT_REGION || it.type == IT_WINDOW || it.type == IT_MONITOR) it.cursor = !on;
        });
        SetNote(on ? L"Курсор скрыт." : L"Курсор показан.");
        return;
    }
    case HA_ITEMVIS: {
        int uid = g_selUid;
        if (!uid && sc && !sc->items.empty()) uid = sc->items.back().uid;
        if (uid) ItemCommand(uid, ID_M_VIS);
        return;
    }
    case HA_TEXT:
        ProjEdit([&](Project& pp) {
            if (Scene* s = CurScene(pp)) {
                bool anyOn = false;
                for (auto& it : s->items)
                    if (it.type == IT_TEXT && it.visible) anyOn = true;
                for (auto& it : s->items)
                    if (it.type == IT_TEXT) it.visible = !anyOn;
            }
        });
        UiRefresh();
        return;
    case HA_SWAPTOP: {
        // the two top visible picture sources trade places (box, shape, outline, order) — the old «⇄»
        ProjEdit([&](Project& pp) {
            Scene* s = CurScene(pp);
            if (!s) return;
            std::vector<size_t> idx;
            for (size_t i = s->items.size(); i-- > 0;)
                if (s->items[i].visible && s->items[i].type != IT_TEXT && s->items[i].type != IT_IMAGE) idx.push_back(i);
            if (idx.size() < 2) return;
            Item& a = s->items[idx[0]];
            Item& b = s->items[idx[1]];
            std::swap(a.x, b.x);
            std::swap(a.y, b.y);
            std::swap(a.w, b.w);
            std::swap(a.h, b.h);
            std::swap(a.shape, b.shape);
            std::swap(a.border, b.border);
            std::swap(s->items[idx[0]], s->items[idx[1]]);
        });
        UiRefresh();
        return;
    }
    case HA_MEDIA_NEXT:
        if (sc)
            for (const auto& it : sc->items)
                if (it.type == IT_MEDIA && it.visible) MediaNextAll(InputKeyOf(it));
        return;
    case HA_HIDE_WINDOW: {
        const int uid = firstOf({IT_WINDOW});
        if (uid) ToggleHideWindow(uid);
        else SetNote(L"В сцене нет источника «Окно».");
        return;
    }
    default:
        if (a >= HA_SCENE1 && a <= HA_SCENE4) {
            const int i = a - HA_SCENE1;
            if (i < static_cast<int>(p.scenes.size())) SetProgramScene(i);
            else SetNote(L"Сцены " + std::to_wstring(i + 1) + L" нет.");
        }
    }
}

void OnPhoneState() {
    // the phone was switched on the phone itself (camera / screen / quality): the sources follow
    bool changed = false;
    std::lock_guard<std::mutex> lk(g_projMx);
    for (auto& sc : g_proj.scenes)
        for (auto& it : sc.items) {
            if (it.type != IT_PHONE) continue;
            const PhoneInfo pi = PhoneInfoFor(InputKeyOf(it));
            if (!pi.exists || pi.status != 2) continue;
            const int src = pi.state == L"back" ? 0 : pi.state == L"front" ? 1 : pi.state == L"screen" ? 2 : -1;
            if (src >= 0 && src != it.phoneSrc) {
                it.phoneSrc = src;
                changed = true;
            }
            if (pi.res >= 0 && pi.res != it.phoneRes) {
                it.phoneRes = pi.res;
                changed = true;
            }
        }
    if (changed) {
        ++g_projVer;
        g_projDirty = true;
    }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        g_notify = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        DarkTitle(hwnd);
        MakeFonts();
        CreateControls(hwnd);
        RegisterHotkeys(hwnd);
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAY;
        g_nid.hIcon = static_cast<HICON>(LoadImageW(g_hInst, MAKEINTRESOURCEW(4), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                    GetSystemMetrics(SM_CYSMICON), 0));
        wcscpy(g_nid.szTip, L"WVC");
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        {
            HICON big = static_cast<HICON>(LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                                      GetSystemMetrics(SM_CYICON), 0));
            HICON sm = static_cast<HICON>(LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                     GetSystemMetrics(SM_CYSMICON), 0));
            if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
            if (sm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(sm));
        }
        SetTimer(hwnd, kTimerStatus, 1000, nullptr);
        SetTimer(hwnd, kTimerPreview, 50, nullptr);
        SetTimer(hwnd, kTimerFollow, 100, nullptr);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        MakeFonts();
        for (int i = IDC_PROGRAM; i < IDC_LAST; ++i)
            if (C(i)) SendMessageW(C(i), WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = S(980);
        mm->ptMinTrackSize.y = S(640);
        return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            Project p = ProjSnapshot();
            if (p.minToTray) ShowWindow(hwnd, SW_HIDE);
            g_uiVisible = false;
        } else {
            g_uiVisible = true;
            Layout(hwnd);
        }
        return 0;
    case WM_SHOWWINDOW:
        g_uiVisible = wp != 0;
        break;
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wp) == hwnd && DockMouse(hwnd, msg, wp, lp)) return TRUE;
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
        if (DockMouse(hwnd, msg, wp, lp)) return 0;
        break;
    case WM_MOUSELEAVE:
        for (int d = 0; d < DOCK_COUNT; ++d) InvalidateRect(hwnd, &g_dg[d].title, FALSE);
        return 0;
    case WM_UPDATE_RESULT: OnUpdateResult(lp); return 0;
    case WM_INITMENUPOPUP: {
        HMENU m = reinterpret_cast<HMENU>(wp);
        for (int d = 0; d < DOCK_COUNT; ++d)
            CheckMenuItem(m, ID_MENU_DOCK0 + d, MF_BYCOMMAND | (g_docks.vis[d] ? MF_CHECKED : MF_UNCHECKED));
        EnableMenuItem(m, ID_MENU_UNDO, MF_BYCOMMAND | (UndoAvailable(false) ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(m, ID_MENU_REDO, MF_BYCOMMAND | (UndoAvailable(true) ? MF_ENABLED : MF_GRAYED));
        CheckMenuItem(m, ID_MENU_EDITNEXT, MF_BYCOMMAND | (g_editNext ? MF_CHECKED : MF_UNCHECKED));
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC m = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, (std::max)(1L, rc.right), (std::max)(1L, rc.bottom));
        HGDIOBJ old = SelectObject(m, bmp);
        PaintMain(hwnd, m);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, old);
        DeleteObject(bmp);
        DeleteDC(m);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kDarkText);
        SetBkColor(dc, kDarkList);
        return reinterpret_cast<LRESULT>(g_brList);
    }
    case WM_MEASUREITEM: {
        auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mi->CtlType == ODT_LISTBOX) mi->itemHeight = S(26);
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (di->CtlType == ODT_BUTTON) DrawButton(di);
        else if (di->CtlID == IDC_SCENES) DrawSceneRow(di);
        else if (di->CtlID == IDC_SOURCES) DrawSourceRow(di);
        return TRUE;
    }
    case WM_TIMER:
        if (wp == kTimerStatus) {
            UiStatus();
        } else if (wp == kTimerPreview) {
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                InvalidateRect(C(IDC_PROGRAM), nullptr, FALSE);
                static int k = 0;
                if (!g_docks.vis[DOCK_NEXT] || g_docks.coll[DOCK_NEXT]) g_pubNext.wantW = 0;  // panel closed: not drawn
                else if (g_editNext || ++k % 3 == 0) InvalidateRect(C(IDC_NEXTPV), nullptr, FALSE);
            } else {
                g_pubProgram.wantW = 0;
                g_pubNext.wantW = 0;
            }
        } else if (wp == kTimerFollow) {
            FollowTick();
        }
        return 0;
    case WM_HOTKEY:
        if (wp >= 1 && wp <= HA_COUNT) OnHotkey(static_cast<int>(wp) - 1);
        return 0;
    case WM_PHONE_STATE:
        OnPhoneState();
        UiStatus();
        return 0;
    case WM_PHONE_FOUND:
    case WM_PHONE_USBLIST: return 0;  // the Properties window asks for these itself
    case WM_DISPLAYCHANGE:
        g_resetCapture = true;
        g_lastSlow = 0;
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) g_resetCapture = true;
        return TRUE;
    case WM_FRAME_DONE:
        SaveProject();
        UiRefresh();
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowMain();
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, ID_TRAY_TOGGLE, g_streaming ? L"■  Стоп камеры" : L"▶  Старт камеры");
            AppendMenuW(m, MF_STRING | (g_streaming ? 0 : MF_GRAYED), ID_TRAY_PAUSE, g_paused ? L"Продолжить" : L"Пауза");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            {
                Project p = ProjSnapshot();
                HMENU sm = CreatePopupMenu();
                for (size_t i = 0; i < p.scenes.size() && i < 80; ++i)
                    AppendMenuW(sm, MF_STRING | (static_cast<int>(i) == p.cur ? MF_CHECKED : 0), ID_TRAY_SCENE0 + static_cast<UINT>(i),
                                p.scenes[i].name.c_str());
                AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(sm), L"Сцены");
                const bool hasNext = p.next >= 0 && p.next < static_cast<int>(p.scenes.size()) && p.next != p.cur;
                AppendMenuW(m, MF_STRING | (hasNext ? 0 : MF_GRAYED), ID_TRAY_SWAP, L"«Следующий» в эфир ⇅");
            }
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Показать окно");
            AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Выход");
            SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
        }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= ID_TRAY_SCENE0 && id < ID_TRAY_SCENE0 + 80) {
            SetProgramScene(id - ID_TRAY_SCENE0);
            return 0;
        }
        if (id >= ID_MENU_DOCK0 && id < ID_MENU_DOCK0 + DOCK_COUNT) {
            const int d = id - ID_MENU_DOCK0;
            g_docks.vis[d] = !g_docks.vis[d];
            if (g_docks.vis[d]) g_docks.coll[d] = false;
            DockChanged();
            return 0;
        }
        switch (id) {
        case ID_MENU_DOCKRESET: DocksReset(); DockChanged(); return 0;
        case ID_MENU_SETTINGS: OpenSettings(0); return 0;
        case ID_MENU_EXPORT: DoExportJson(hwnd); return 0;
        case ID_MENU_IMPORT: DoImportJson(hwnd); return 0;
        case ID_MENU_EXIT: DestroyWindow(hwnd); return 0;
        case ID_MENU_UNDO: UndoStep(false); return 0;
        case ID_MENU_REDO: UndoStep(true); return 0;
        case ID_MENU_EDITNEXT: SetEditNext(!g_editNext); return 0;
        case ID_MENU_LOG: OpenLogWindow(); return 0;
        case ID_MENU_UPDATES: CheckUpdates(true); return 0;
        case ID_MENU_README: {
            const std::wstring f = ExeDir() + L"\\README.txt";
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                ShellExecuteW(hwnd, L"open", L"https://github.com/Finn013/WVC", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case ID_MENU_ABOUT: {
            MSGBOXPARAMSW mb{};
            mb.cbSize = sizeof(mb);
            mb.hwndOwner = hwnd;
            mb.hInstance = g_hInst;
            mb.lpszText = L"WVC " WVC_VERSION L" — лёгкая виртуальная камера в стиле OBS.\n\n"
                          L"Камера «WVC Camera» для Zoom, Telegram, Discord и браузеров; телефон как камера (WVC Phone).\n"
                          L"https://github.com/Finn013/WVC\n\nКамера основана на softcam (MIT).";
            mb.lpszCaption = L"О программе WVC";
            mb.dwStyle = MB_USERICON | MB_OK;
            mb.lpszIcon = MAKEINTRESOURCEW(1);
            MessageBoxIndirectW(&mb);
            return 0;
        }
        }
        if (id >= ID_S_REN && id <= ID_S_ADD) {
            SceneCommand(id, lp >= 0x10000 ? static_cast<int>(lp - 0x10000) : -1);
            return 0;
        }
        switch (id) {
        case ID_TRAY_OPEN: ShowMain(); break;
        case ID_TRAY_TOGGLE:
        case IDC_START: ToggleStreaming(); break;
        case ID_TRAY_PAUSE:
        case IDC_PAUSE: TogglePause(); break;
        case ID_TRAY_SWAP:
        case IDC_SWAP: SwapNext(); break;
        case ID_TRAY_EXIT:
        case IDC_EXIT: DestroyWindow(hwnd); break;
        case IDC_SETTINGS: OpenSettings(0); break;
        case IDC_NEXT_COMBO:
            if (code == CBN_SELCHANGE && !g_syncing) {
                const int k = static_cast<int>(SendMessageW(C(IDC_NEXT_COMBO), CB_GETCURSEL, 0, 0));
                const int idx = static_cast<int>(SendMessageW(C(IDC_NEXT_COMBO), CB_GETITEMDATA, k, 0));
                ProjEdit([&](Project& p) { p.next = idx; });
                UiRefresh();
            }
            break;
        case IDC_SCENES:
            if (code == LBN_SELCHANGE && !g_syncing) {
                SetProgramScene(static_cast<int>(SendMessageW(C(IDC_SCENES), LB_GETCURSEL, 0, 0)));
            } else if (code == LBN_DBLCLK) {
                SceneCommand(ID_S_REN, static_cast<int>(SendMessageW(C(IDC_SCENES), LB_GETCURSEL, 0, 0)));
            }
            break;
        case IDC_SC_ADD: SceneCommand(ID_S_ADD, -1); break;
        case IDC_SC_DEL: SceneCommand(ID_S_DEL, -1); break;
        case IDC_SC_REN: SceneCommand(ID_S_REN, -1); break;
        case IDC_SC_UP: SceneCommand(ID_S_UP, -1); break;
        case IDC_SC_DOWN: SceneCommand(ID_S_DOWN, -1); break;
        case IDC_SOURCES:
            if (code == LBN_SELCHANGE && !g_syncing) {
                const int row = static_cast<int>(SendMessageW(C(IDC_SOURCES), LB_GETCURSEL, 0, 0));
                if (row >= 0 && row < static_cast<int>(g_rowUid.size())) UiSelect(g_rowUid[static_cast<size_t>(row)]);
            } else if (code == LBN_DBLCLK && g_selUid) {
                OpenProperties(g_selUid, false);
            }
            break;
        case IDC_SR_ADD: {
            RECT r;
            GetWindowRect(C(IDC_SR_ADD), &r);
            AddSourceMenu(hwnd, POINT{r.left, r.bottom});
            break;
        }
        case IDC_SR_DEL:
            if (g_selUid) ItemCommand(g_selUid, ID_M_DEL);
            break;
        case IDC_SR_PROPS:
            if (g_selUid) OpenProperties(g_selUid, false);
            else SetNote(L"Выберите источник в списке.");
            break;
        case IDC_SR_UP:
            if (g_selUid) ItemCommand(g_selUid, ID_M_UP);
            break;
        case IDC_SR_DOWN:
            if (g_selUid) ItemCommand(g_selUid, ID_M_DOWN);
            break;
        case IDC_TR_TYPE:
            if (code == CBN_SELCHANGE && !g_syncing) {
                const int t = static_cast<int>(SendMessageW(C(IDC_TR_TYPE), CB_GETCURSEL, 0, 0));
                ProjEdit([&](Project& p) { p.transition = std::clamp(t, 0, TR_COUNT - 1); });
                EnableWindow(C(IDC_TR_DIR), TransUsesDir(t));
            }
            break;
        case IDC_TR_DIR:
            if (code == CBN_SELCHANGE && !g_syncing) {
                const int t = static_cast<int>(SendMessageW(C(IDC_TR_DIR), CB_GETCURSEL, 0, 0));
                ProjEdit([&](Project& p) { p.transDir = std::clamp(t, 0, 3); });
            }
            break;
        case IDC_TR_EASE:
            if (code == CBN_SELCHANGE && !g_syncing) {
                const int t = static_cast<int>(SendMessageW(C(IDC_TR_EASE), CB_GETCURSEL, 0, 0));
                ProjEdit([&](Project& p) { p.transEase = std::clamp(t, 0, 1); });
            }
            break;
        case IDC_TR_MS:
            if (code == EN_CHANGE && !g_syncing) {
                wchar_t b[16] = {};
                GetWindowTextW(C(IDC_TR_MS), b, 16);
                const int v = _wtoi(b);
                if (b[0] && v >= 0 && v <= 5000) ProjEdit([&](Project& p) { p.transMs = v; });
            }
            break;
        }
        return 0;
    }
    case WM_CLOSE: {
        Project p = ProjSnapshot();
        if (p.closeToTray) {
            ShowWindow(hwnd, SW_HIDE);
            g_uiVisible = false;
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY: {
        WINDOWPLACEMENT wpl{sizeof(wpl)};
        if (GetWindowPlacement(hwnd, &wpl)) {
            const RECT& r = wpl.rcNormalPosition;
            g_ini.SetInt(L"WVC", L"WinX", r.left);
            g_ini.SetInt(L"WVC", L"WinY", r.top);
            g_ini.SetInt(L"WVC", L"WinW", r.right - r.left);
            g_ini.SetInt(L"WVC", L"WinH", r.bottom - r.top);
            g_ini.SetInt(L"WVC", L"WinMax", wpl.showCmd == SW_SHOWMAXIMIZED);
        }
        RestoreHiddenWindow();
        UnregisterHotkeys(hwnd);
        KillTimer(hwnd, kTimerStatus);
        KillTimer(hwnd, kTimerPreview);
        KillTimer(hwnd, kTimerFollow);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DestroyFrames();
        g_notify = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

void RestartCameraForFormat(const wchar_t* note) {
    RenderStop();
    RenderStart();
    int W, H;
    UiOutputSize(&W, &H);
    std::wstring n = note ? note : L"Формат камеры изменён: ";
    n += std::to_wstring(W) + L"×" + std::to_wstring(H) + L". В Zoom/Telegram может понадобиться выключить и включить видео.";
    if (!g_camOk && CameraDllLoaded()) n = L"Не удалось перезапустить камеру.";
    SetNote(n);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int DoRegisterCamera(bool reg);  // ui_settings.cpp

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int nCmdShow) {
    if (cmdLine && wcsstr(cmdLine, L"--register")) return DoRegisterCamera(true);
    if (cmdLine && wcsstr(cmdLine, L"--unregister")) return DoRegisterCamera(false);

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\WVC.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"WVCMain", nullptr);
        if (other) {
            ShowWindow(other, SW_SHOW);
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    g_hInst = hInst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    {
        WSADATA wd;
        g_wsaOk = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_HOTKEY_CLASS | ICC_UPDOWN_CLASS |
                                              ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    // settings: load, migrate older versions (a copy of the old file is kept next to it)
    {
        g_ini.Load(IniPath());
        Project p;
        std::wstring note;
        LoadProjectFromIni(g_ini, &p, &note);
        if (!note.empty()) {
            CopyFileW(IniPath().c_str(), (IniPath() + L".v010-backup").c_str(), FALSE);
            g_note = note;
            g_noteAt = GetTickCount64() + 30000;
        }
        {
            std::lock_guard<std::mutex> lk(g_projMx);
            g_proj = p;
        }
        ++g_projVer;
        SaveProject();
    }
    LogMsg(L"WVC " WVC_VERSION L" запущен: " + ExePath());
    DocksLoad();
    LoadHotkeys();
    if (!LoadCameraDll()) LogErr(L"Не загрузился WVCCam.dll рядом с WVC.exe");
    RenderStart();  // the camera exists while WVC runs; Start / Stop only switch the picture

    {
        const int sdpi = static_cast<int>(GetDpiForSystem());
        g_fb = MulDiv(6, sdpi, 96);
        g_tabW = MulDiv(170, sdpi, 96);
        g_tabH = MulDiv(26, sdpi, 96);
        g_frameFont = CreateFontW(-MulDiv(9, sdpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    g_brDark = CreateSolidBrush(kDarkBg);
    g_brPanel = CreateSolidBrush(kDarkPanel);
    g_brList = CreateSolidBrush(kDarkList);

    WNDCLASSW fc{};
    fc.style = CS_HREDRAW | CS_VREDRAW;
    fc.lpfnWndProc = FrameProc;
    fc.hInstance = hInst;
    fc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    fc.lpszClassName = L"WVCFrame";
    RegisterClassW(&fc);

    WNDCLASSW pc{};
    pc.style = CS_DBLCLKS;
    pc.lpfnWndProc = ProgramProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.lpszClassName = L"WVCProgram";
    RegisterClassW(&pc);
    pc.lpfnWndProc = NextProc;
    pc.lpszClassName = L"WVCNext";
    RegisterClassW(&pc);

    WNDCLASSW dc{};
    dc.hInstance = hInst;
    dc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    dc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    dc.lpfnWndProc = InputProc;
    dc.lpszClassName = L"WVCInput";
    RegisterClassW(&dc);

    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"WVCMain";
    RegisterClassW(&wc);

    int wx = g_ini.Int(L"WVC", L"WinX", CW_USEDEFAULT), wy = g_ini.Int(L"WVC", L"WinY", CW_USEDEFAULT);
    int ww = g_ini.Int(L"WVC", L"WinW", 0), wh = g_ini.Int(L"WVC", L"WinH", 0);
    {
        const UINT sdpi = GetDpiForSystem();
        if (ww < 600 || wh < 400) {
            ww = MulDiv(1280, static_cast<int>(sdpi), 96);
            wh = MulDiv(860, static_cast<int>(sdpi), 96);
            wx = wy = CW_USEDEFAULT;
        }
        RECT test{wx, wy, wx + ww, wy + wh};
        if (wx != CW_USEDEFAULT && !MonitorFromRect(&test, MONITOR_DEFAULTTONULL)) wx = wy = CW_USEDEFAULT;
    }
    HMENU bar = CreateMenu();
    {
        HMENU f = CreatePopupMenu();
        AppendMenuW(f, MF_STRING, ID_MENU_SETTINGS, L"Настройки…");
        AppendMenuW(f, MF_STRING, ID_MENU_EXPORT, L"Сохранить настройки в JSON…");
        AppendMenuW(f, MF_STRING, ID_MENU_IMPORT, L"Загрузить настройки из JSON…");
        AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(f, MF_STRING, ID_MENU_EXIT, L"Выход");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(f), L"Файл");
        HMENU e = CreatePopupMenu();
        AppendMenuW(e, MF_STRING, ID_MENU_UNDO, L"Отменить\tCtrl+Z");
        AppendMenuW(e, MF_STRING, ID_MENU_REDO, L"Повторить\tCtrl+Y");
        AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(e, MF_STRING, ID_MENU_EDITNEXT, L"Править «Следующий» (студийный режим)");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(e), L"Правка");
        HMENU v = CreatePopupMenu();
        HMENU pn = CreatePopupMenu();
        static const wchar_t* names[DOCK_COUNT] = {L"Следующий", L"Сцены", L"Источники", L"Переходы", L"Управление"};
        for (int d = 0; d < DOCK_COUNT; ++d) AppendMenuW(pn, MF_STRING, ID_MENU_DOCK0 + d, names[d]);
        AppendMenuW(v, MF_POPUP, reinterpret_cast<UINT_PTR>(pn), L"Панели");
        AppendMenuW(v, MF_STRING, ID_MENU_DOCKRESET, L"Сбросить раскладку");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(v), L"Вид");
        HMENU h = CreatePopupMenu();
        AppendMenuW(h, MF_STRING, ID_MENU_LOG, L"Журнал");
        AppendMenuW(h, MF_STRING, ID_MENU_UPDATES, L"Проверить обновления");
        AppendMenuW(h, MF_STRING, ID_MENU_README, L"Инструкция (README)");
        AppendMenuW(h, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(h, MF_STRING, ID_MENU_ABOUT, L"О программе");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(h), L"Справка");
    }
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"WVCMain", L"WVC " WVC_VERSION L" — виртуальная камера",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, wx, wy, ww, wh, nullptr, bar, hInst, nullptr);
    if (!hwnd) return 1;
    UiRefresh();
    const bool tray = cmdLine && wcsstr(cmdLine, L"--tray");
    if (!tray) {
        ShowWindow(hwnd, g_ini.Int(L"WVC", L"WinMax", 0) ? SW_SHOWMAXIMIZED : nCmdShow);
        UpdateWindow(hwnd);
        g_uiVisible = true;
    } else {
        g_uiVisible = false;
    }
    UiStatus();
    if (g_ini.Int(L"WVC", L"AutoStream", 0) && g_camOk) ToggleStreaming();
    if (ProjSnapshot().checkUpdates) CheckUpdates(false);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) in the main window, but not while typing in a text field
        if (msg.message == WM_KEYDOWN && (msg.wParam == 'Z' || msg.wParam == 'Y') && GetKeyState(VK_CONTROL) < 0 &&
            GetKeyState(VK_MENU) >= 0 && GetAncestor(msg.hwnd, GA_ROOT) == hwnd) {
            wchar_t cls[32] = {};
            GetClassNameW(msg.hwnd, cls, 32);
            if (_wcsicmp(cls, L"Edit") != 0) {
                const bool redo = msg.wParam == 'Y' || GetKeyState(VK_SHIFT) < 0;
                UndoStep(redo);
                continue;
            }
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    g_quitting = true;
    g_streaming = false;
    LogMsg(L"WVC закрыт");
    SaveProject();
    RenderStop();
    InputsShutdown();
    AdbShutdown();
    if (g_frameFont) DeleteObject(g_frameFont);
    MFShutdown();
    if (g_wsaOk) WSACleanup();
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
