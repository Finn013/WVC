// WVC: «Свойства» of a source, OBS-like: live preview of only this source on top (crop dimmed, shape outline,
// outside the shape as a checkerboard; crop edges can be dragged), all settings below, OK / Отмена.
// The phone source's window follows the same pattern (it replaces the old «Окно телефона»).
#include "ui_form.h"

namespace {
enum {
    P_FORM = 200, P_PREVIEW, P_OK, P_CANCEL, P_DEFAULTS,
    P_NAME = 300,
    P_REGION_LBL, P_REGION_SEL, P_REGION_MON, P_CURSOR,
    P_WIN, P_WINCAP, P_HIDEWIN,
    P_MON,
    P_CAM,
    P_MEDIA, P_MEDIA_ADD, P_MEDIA_DEL, P_MEDIA_UP, P_MEDIA_DOWN, P_MEDIA_SEC, P_MEDIA_NEXT, P_MEDIA_NOW,
    P_PH_CONN, P_PH_LIST, P_PH_FIND, P_PH_IP, P_PH_CONNECT, P_PH_SRC, P_PH_RES, P_PH_CODEC, P_PH_AUTOROT, P_PH_STATUS,
    P_TEXT, P_TEXTPX, P_TEXTPX_UD, P_TEXTCOLOR, P_TEXTBG, P_TEXTBOLD,
    P_IMAGE, P_IMAGE_BTN,
    P_ASPECT, P_FIT, P_CROP0, P_CROP0_UD, P_CROP1, P_CROP1_UD, P_CROP2, P_CROP2_UD, P_CROP3, P_CROP3_UD,
    P_ROT, P_MIRROR, P_ZOOM, P_ZOOM_UD, P_SHAPE, P_RADIUS, P_FEATHER, P_MASK, P_MASK_BTN, P_OPACITY, P_BORDER,
    P_WINFRAME, P_CC_EXP, P_CC_EXPV, P_CC_BR, P_CC_BRV, P_CC_WB, P_CC_WBV, P_CC_NOTE,
    P_LAST
};
const UINT_PTR kTimerProps = 7;

struct PropsState {
    HWND dlg = nullptr;
    HWND preview = nullptr;
    Form form;
    HWND ctl[P_LAST] = {};
    Item edit, orig;
    bool isNew = false, done = false, ok = false, syncing = false;
    std::vector<WindowItem> wins;
    std::vector<MonitorItem> mons;
    std::vector<CamDevice> cams;
    std::vector<std::wstring> phoneChoices;  // IP (Wi-Fi) or serial (USB) per list row
    std::wstring phoneNote;
    // preview geometry (for dragging the crop)
    RECT drawn{};
    int dragEdge = -1;
    POINT dragStart{};
    int dragCrop0 = 0;
};
PropsState* g_ps = nullptr;

HWND P(int id) { return g_ps ? g_ps->ctl[id] : nullptr; }

void SetCtlText(HWND h, const std::wstring& t) {
    if (!h) return;
    wchar_t cur[2048] = {};
    GetWindowTextW(h, cur, 2048);
    if (t != cur) SetWindowTextW(h, t.c_str());
}

std::wstring GetCtlText(HWND h) {
    if (!h) return L"";
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

int ComboGet(int id) { return static_cast<int>(SendMessageW(P(id), CB_GETCURSEL, 0, 0)); }
void ComboSet(int id, int v) { SendMessageW(P(id), CB_SETCURSEL, v, 0); }
bool CheckGet(int id) { return SendMessageW(P(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void CheckSet(int id, bool v) { SendMessageW(P(id), BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); }
int IntGet(int id, int def) {
    const std::wstring t = GetCtlText(P(id));
    return t.empty() ? def : _wtoi(t.c_str());
}

std::wstring RegionText(const RECT& r) {
    wchar_t b[96];
    swprintf(b, 96, L"%d, %d — %d × %d", static_cast<int>(r.left), static_cast<int>(r.top),
             static_cast<int>(r.right - r.left), static_cast<int>(r.bottom - r.top));
    return b;
}

// the working copy -> the project (live) and to the renderer's solo preview
void Push() {
    if (!g_ps) return;
    const Item e = g_ps->edit;
    {
        std::lock_guard<std::mutex> lk(g_soloMx);
        g_soloItem = e;
    }
    ProjEdit([&](Project& p) {
        if (Item* it = FindItem(p, e.uid)) {
            HWND h = it->hwnd;
            *it = e;
            if (e.type == IT_WINDOW && !e.hwnd) it->hwnd = h;
        }
        if (e.type == IT_PHONE) {
            p.lastPhoneIp = e.phoneIp.empty() ? p.lastPhoneIp : e.phoneIp;
        }
    });
}

void FillWindows() {
    g_ps->wins = ListWindows();
    HWND c = P(P_WIN);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < g_ps->wins.size(); ++i) {
        SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_ps->wins[i].title.c_str()));
        if (g_ps->wins[i].title == g_ps->edit.winTitle) sel = static_cast<int>(i);
    }
    if (sel < 0 && !g_ps->edit.winTitle.empty()) {
        sel = static_cast<int>(SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((g_ps->edit.winTitle + L" (не найдено)").c_str())));
        g_ps->wins.push_back(WindowItem{nullptr, g_ps->edit.winTitle});
    }
    SendMessageW(c, CB_SETCURSEL, sel, 0);
}

void FillMonitors() {
    g_ps->mons = Monitors();
    HWND c = P(P_MON);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < g_ps->mons.size(); ++i) {
        const auto& m = g_ps->mons[i];
        std::wstring t = L"Монитор " + std::to_wstring(i + 1) + (m.primary ? L" (основной)" : L"") + L" — " +
                         std::to_wstring(m.rc.right - m.rc.left) + L"×" + std::to_wstring(m.rc.bottom - m.rc.top);
        SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
        if (m.device == g_ps->edit.monDevice) sel = static_cast<int>(i);
    }
    SendMessageW(c, CB_SETCURSEL, sel, 0);
}

void FillCams() {
    g_ps->cams = EnumWebcams();
    HWND c = P(P_CAM);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < g_ps->cams.size(); ++i) {
        SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_ps->cams[i].name.c_str()));
        if (g_ps->cams[i].link == g_ps->edit.camLink) sel = static_cast<int>(i);
    }
    if (sel < 0 && !g_ps->edit.camLink.empty()) {
        sel = static_cast<int>(SendMessageW(c, CB_ADDSTRING, 0,
                                            reinterpret_cast<LPARAM>((g_ps->edit.camName + L" (не подключена)").c_str())));
        g_ps->cams.push_back(CamDevice{g_ps->edit.camName, g_ps->edit.camLink});
    }
    SendMessageW(c, CB_SETCURSEL, sel, 0);
}

void FillMedia() {
    HWND l = P(P_MEDIA);
    const int sel = static_cast<int>(SendMessageW(l, LB_GETCURSEL, 0, 0));
    SendMessageW(l, LB_RESETCONTENT, 0, 0);
    for (const auto& f : g_ps->edit.media) SendMessageW(l, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(FileNameOf(f).c_str()));
    SendMessageW(l, LB_SETCURSEL, sel, 0);
}

void FillPhones() {
    HWND c = P(P_PH_LIST);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    g_ps->phoneChoices.clear();
    int sel = -1;
    if (g_ps->edit.phoneConn == 1) {
        for (const auto& d : UsbDevices()) {
            std::wstring t = (d.model.empty() ? d.serial : d.model) + L" — USB";
            if (d.state == L"unauthorized") t += L" (подтвердите доступ на телефоне)";
            else if (d.state != L"device") t += L" (" + d.state + L")";
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
            g_ps->phoneChoices.push_back(d.serial);
            if (d.serial == g_ps->edit.phoneSerial) sel = static_cast<int>(g_ps->phoneChoices.size()) - 1;
        }
    } else {
        for (const auto& f : PhoneFoundList()) {
            const std::wstring t = f.name + L" — " + f.ip;
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
            g_ps->phoneChoices.push_back(f.ip);
            if (f.ip == Trim(g_ps->edit.phoneIp)) sel = static_cast<int>(g_ps->phoneChoices.size()) - 1;
        }
    }
    SendMessageW(c, CB_SETCURSEL, sel, 0);
    EnableWindow(P(P_PH_IP), g_ps->edit.phoneConn == 0);
}

void SyncControls() {
    PropsState& s = *g_ps;
    const Item& e = s.edit;
    s.syncing = true;
    SetCtlText(P(P_NAME), e.name);
    switch (e.type) {
    case IT_REGION:
        SetCtlText(P(P_REGION_LBL), RegionText(e.region));
        CheckSet(P_CURSOR, e.cursor);
        break;
    case IT_WINDOW:
        ComboSet(P_WINCAP, e.winCapture);
        CheckSet(P_CURSOR, e.cursor);
        CheckSet(P_WINFRAME, e.winFrame);
        break;
    case IT_WEBCAM: {
        const CamCtlCfg c = CamCtlGet(e.camLink);
        ComboSet(P_CC_EXP, c.expMode);
        ComboSet(P_CC_BR, c.brMode);
        ComboSet(P_CC_WB, c.wbMode);
        break;
    }
    case IT_MONITOR: CheckSet(P_CURSOR, e.cursor); break;
    case IT_MEDIA:
        FillMedia();
        ComboSet(P_MEDIA_SEC, e.imageSec);
        break;
    case IT_PHONE:
        ComboSet(P_PH_CONN, e.phoneConn);
        SetCtlText(P(P_PH_IP), e.phoneIp);
        ComboSet(P_PH_SRC, e.phoneSrc);
        ComboSet(P_PH_RES, e.phoneRes);
        ComboSet(P_PH_CODEC, e.phoneCodec);
        CheckSet(P_PH_AUTOROT, e.autoRotate);
        EnableWindow(P(P_PH_IP), e.phoneConn == 0);
        break;
    case IT_TEXT:
        SetCtlText(P(P_TEXT), e.text);
        SetCtlText(P(P_TEXTPX), std::to_wstring(e.textPx));
        ComboSet(P_TEXTCOLOR, e.textColor);
        CheckSet(P_TEXTBG, e.textBg);
        CheckSet(P_TEXTBOLD, e.textBold);
        break;
    case IT_IMAGE: SetCtlText(P(P_IMAGE), e.image); break;
    }
    ComboSet(P_ASPECT, e.aspect);
    ComboSet(P_FIT, e.fit);
    for (int i = 0; i < 4; ++i) {
        wchar_t b[16];
        swprintf(b, 16, L"%d", (e.crop[i] + 5) / 10);
        SetCtlText(P(P_CROP0 + 2 * i), b);
    }
    ComboSet(P_ROT, e.rot);
    CheckSet(P_MIRROR, e.mirror);
    SetCtlText(P(P_ZOOM), std::to_wstring(e.zoom));
    ComboSet(P_SHAPE, e.shape.kind);
    SendMessageW(P(P_RADIUS), TBM_SETPOS, TRUE, e.shape.radius);
    SendMessageW(P(P_FEATHER), TBM_SETPOS, TRUE, e.shape.feather);
    SetCtlText(P(P_MASK), e.shape.mask);
    EnableWindow(P(P_RADIUS), e.shape.kind == SH_ROUND);
    EnableWindow(P(P_MASK_BTN), e.shape.kind == SH_MASK);
    SendMessageW(P(P_OPACITY), TBM_SETPOS, TRUE, e.opacity);
    CheckSet(P_BORDER, e.border);
    s.syncing = false;
}

// controls -> working copy
void ReadControls(int id) {
    PropsState& s = *g_ps;
    Item& e = s.edit;
    switch (id) {
    case P_NAME: {
        std::wstring n = Trim(GetCtlText(P(P_NAME)));
        if (!n.empty()) e.name = n;
        break;
    }
    case P_CURSOR: e.cursor = CheckGet(P_CURSOR); break;
    case P_WINFRAME: e.winFrame = CheckGet(P_WINFRAME); break;
    case P_WIN: {
        const int k = ComboGet(P_WIN);
        if (k >= 0 && k < static_cast<int>(s.wins.size()) && s.wins[static_cast<size_t>(k)].hwnd) {
            e.winTitle = s.wins[static_cast<size_t>(k)].title;
            e.hwnd = s.wins[static_cast<size_t>(k)].hwnd;
            e.needFit = true;
        }
        break;
    }
    case P_WINCAP: e.winCapture = std::clamp(ComboGet(P_WINCAP), 0, 1); break;
    case P_MON: {
        const int k = ComboGet(P_MON);
        if (k >= 0 && k < static_cast<int>(s.mons.size())) {
            e.monDevice = s.mons[static_cast<size_t>(k)].device;
            e.monRect = s.mons[static_cast<size_t>(k)].rc;
        }
        break;
    }
    case P_CAM: {
        const int k = ComboGet(P_CAM);
        if (k >= 0 && k < static_cast<int>(s.cams.size())) {
            e.camLink = s.cams[static_cast<size_t>(k)].link;
            e.camName = s.cams[static_cast<size_t>(k)].name;
        }
        break;
    }
    case P_MEDIA_SEC: e.imageSec = std::clamp(ComboGet(P_MEDIA_SEC), 0, 4); break;
    case P_PH_CONN:
        e.phoneConn = std::clamp(ComboGet(P_PH_CONN), 0, 1);
        FillPhones();
        if (e.phoneConn == 1) PhoneUsbList(s.dlg);
        break;
    case P_PH_LIST: {
        const int k = ComboGet(P_PH_LIST);
        if (k >= 0 && k < static_cast<int>(s.phoneChoices.size())) {
            if (e.phoneConn == 1) e.phoneSerial = s.phoneChoices[static_cast<size_t>(k)];
            else {
                e.phoneIp = s.phoneChoices[static_cast<size_t>(k)];
                s.syncing = true;
                SetCtlText(P(P_PH_IP), e.phoneIp);
                s.syncing = false;
            }
        }
        break;
    }
    case P_PH_IP: e.phoneIp = Trim(GetCtlText(P(P_PH_IP))); break;
    case P_PH_SRC: e.phoneSrc = std::clamp(ComboGet(P_PH_SRC), 0, 2); break;
    case P_PH_RES: e.phoneRes = std::clamp(ComboGet(P_PH_RES), 0, 1); break;
    case P_PH_CODEC: e.phoneCodec = std::clamp(ComboGet(P_PH_CODEC), 0, 2); break;
    case P_PH_AUTOROT: e.autoRotate = CheckGet(P_PH_AUTOROT); break;
    case P_TEXT: e.text = GetCtlText(P(P_TEXT)); break;
    case P_TEXTPX: e.textPx = std::clamp(IntGet(P_TEXTPX, e.textPx), 8, 400); break;
    case P_TEXTCOLOR: e.textColor = std::clamp(ComboGet(P_TEXTCOLOR), 0, 7); break;
    case P_TEXTBG: e.textBg = CheckGet(P_TEXTBG); break;
    case P_TEXTBOLD: e.textBold = CheckGet(P_TEXTBOLD); break;
    case P_ASPECT: {
        e.aspect = std::clamp(ComboGet(P_ASPECT), 0, 5);
        const double a = AspectValue(e.aspect);
        if (a > 0) {  // the box takes these proportions (same width, same centre); the picture fills it
            int W, H;
            UiOutputSize(&W, &H);
            const double cy = e.y + e.h / 2;
            e.h = e.w * W / a / H;
            e.y = cy - e.h / 2;
            if (e.fit == 0) e.fit = 1;
            s.syncing = true;
            ComboSet(P_FIT, e.fit);
            s.syncing = false;
        }
        break;
    }
    case P_FIT: e.fit = std::clamp(ComboGet(P_FIT), 0, 2); break;
    case P_CROP0:
    case P_CROP1:
    case P_CROP2:
    case P_CROP3: {
        const int i = (id - P_CROP0) / 2;
        const int other = e.crop[i ^ 2];
        e.crop[i] = std::clamp(IntGet(id, 0) * 10, 0, 900 - other);
        break;
    }
    case P_ROT: e.rot = std::clamp(ComboGet(P_ROT), 0, 3); break;
    case P_MIRROR: e.mirror = CheckGet(P_MIRROR); break;
    case P_ZOOM: e.zoom = std::clamp(IntGet(P_ZOOM, 100), 100, 400); break;
    case P_SHAPE:
        e.shape.kind = std::clamp(ComboGet(P_SHAPE), 0, SH_COUNT - 1);
        EnableWindow(P(P_RADIUS), e.shape.kind == SH_ROUND);
        EnableWindow(P(P_MASK_BTN), e.shape.kind == SH_MASK);
        if (e.shape.kind == SH_MASK && e.shape.mask.empty()) {
            std::wstring f;
            if (PickFile(s.dlg, L"PNG-маска\0*.png\0Картинки\0*.png;*.bmp;*.jpg\0", &f)) e.shape.mask = f;
            SetCtlText(P(P_MASK), e.shape.mask);
        }
        break;
    case P_BORDER: e.border = CheckGet(P_BORDER); break;
    default: return;
    }
}

std::wstring PhoneStatus() {
    const Item& e = g_ps->edit;
    const std::wstring key = InputKeyOf(e);
    if (e.phoneConn == 0 && Trim(e.phoneIp).empty())
        return L"Укажите IP телефона (он крупно показан в WVC Phone) или нажмите «Найти». " + g_ps->phoneNote;
    const PhoneInfo pi = PhoneInfoFor(key);
    if (!pi.exists) {
        if (PhoneUserState(key) == -1) return L"Отключено. Нажмите «Подключить». " + g_ps->phoneNote;
        return L"Не подключено. " + g_ps->phoneNote;
    }
    if (pi.status == 1) return e.phoneConn == 1 ? L"Подключаюсь по USB…" : L"Подключаюсь…";
    if (pi.status == 3) return L"Нет связи: " + pi.err + L" Повтор через 3 с.";
    std::wstring t = L"Подключено";
    if (!pi.name.empty()) t += L": " + pi.name;
    else if (!pi.usbModel.empty()) t += L": " + pi.usbModel;
    if (e.phoneConn == 1) t += L" (USB)";
    t += L" — ";
    t += PhoneStateName(pi.state);
    wchar_t b[220];
    swprintf(b, 220, L"\r\nКодек: %ls · приём %.1f к/с · показ %.1f к/с · %.1f Мбит/с%ls", pi.codec.empty() ? L"—" : pi.codec.c_str(),
             pi.recvFpsX10 / 10.0, pi.decFpsX10 / 10.0, pi.kbps / 1000.0,
             pi.w ? (L" · " + std::to_wstring(pi.w) + L"×" + std::to_wstring(pi.h)).c_str() : L"");
    t += b;
    if (e.phoneCodec == 0 && pi.codec == L"JPEG")
        t += L"\r\nТелефон шлёт JPEG: обновите WVC Phone до 0.11, чтобы включился быстрый H.264.";
    return t;
}

// webcam picture controls: the sliders follow the camera's ranges once it is open
void CamCtlFromControls() {
    if (!g_ps || g_ps->edit.camLink.empty()) return;
    CamCtlCfg c;
    c.expMode = (std::max)(0, ComboGet(P_CC_EXP));
    c.brMode = (std::max)(0, ComboGet(P_CC_BR));
    c.wbMode = (std::max)(0, ComboGet(P_CC_WB));
    c.expVal = static_cast<int>(SendMessageW(P(P_CC_EXPV), TBM_GETPOS, 0, 0));
    c.brVal = static_cast<int>(SendMessageW(P(P_CC_BRV), TBM_GETPOS, 0, 0));
    c.wbVal = static_cast<int>(SendMessageW(P(P_CC_WBV), TBM_GETPOS, 0, 0));
    CamCtlSet(g_ps->edit.camLink, c);
}

void CamCtlLive() {
    const CamCtlInfo info = CamCtlInfoFor(g_ps->edit.camLink);
    const CamCtlCfg cfg = CamCtlGet(g_ps->edit.camLink);
    struct Row {
        int track, combo;
        const CamCtlRange* r;
        int mode, val;
    } rows[3] = {{P_CC_EXPV, P_CC_EXP, &info.exp, cfg.expMode, cfg.expVal},
                 {P_CC_BRV, P_CC_BR, &info.br, cfg.brMode, cfg.brVal},
                 {P_CC_WBV, P_CC_WB, &info.wb, cfg.wbMode, cfg.wbVal}};
    g_ps->syncing = true;
    for (const Row& r : rows) {
        HWND t = P(r.track);
        const bool has = info.open && r.r->has;
        EnableWindow(P(r.combo), !info.open || r.r->has);
        EnableWindow(t, has && r.mode == 1);
        if (!has) continue;
        if (SendMessageW(t, TBM_GETRANGEMIN, 0, 0) != r.r->lo || SendMessageW(t, TBM_GETRANGEMAX, 0, 0) != r.r->hi) {
            SendMessageW(t, TBM_SETRANGEMIN, FALSE, r.r->lo);
            SendMessageW(t, TBM_SETRANGEMAX, TRUE, r.r->hi);
            SendMessageW(t, TBM_SETLINESIZE, 0, r.r->step);
        }
        // manual: the remembered value; auto: what the camera chose itself
        if (GetCapture() != t) SendMessageW(t, TBM_SETPOS, TRUE, r.mode == 1 ? std::clamp<long>(r.val, r.r->lo, r.r->hi) : r.r->cur);
    }
    g_ps->syncing = false;
    SetCtlText(P(P_CC_NOTE), !info.open ? L"Камера сейчас выключена — настройки применятся, когда она включится."
                             : (!info.exp.has && !info.br.has && !info.wb.has)
                                 ? L"Эта камера не даёт менять экспозицию, яркость и баланс белого."
                                 : L"Меняется сразу и запоминается для этой камеры (применяется при каждом включении).");
}

void UpdateLive() {
    if (!g_ps) return;
    if (g_ps->edit.type == IT_WEBCAM && P(P_CC_EXP)) CamCtlLive();
    if (g_ps->edit.type == IT_PHONE) {
        SetCtlText(P(P_PH_STATUS), PhoneStatus());
        const int us = PhoneUserState(InputKeyOf(g_ps->edit));
        const PhoneInfo pi = PhoneInfoFor(InputKeyOf(g_ps->edit));
        SetCtlText(P(P_PH_CONNECT), (pi.exists && us != -1) ? L"Отключить" : L"Подключить");
        // the phone may have switched itself (camera / screen / quality)
        Project p = ProjSnapshot();
        if (const Item* it = FindItemC(p, g_ps->edit.uid)) {
            if (it->phoneSrc != g_ps->edit.phoneSrc || it->phoneRes != g_ps->edit.phoneRes) {
                g_ps->edit.phoneSrc = it->phoneSrc;
                g_ps->edit.phoneRes = it->phoneRes;
                g_ps->syncing = true;
                ComboSet(P_PH_SRC, g_ps->edit.phoneSrc);
                ComboSet(P_PH_RES, g_ps->edit.phoneRes);
                g_ps->syncing = false;
            }
        }
    }
    if (g_ps->edit.type == IT_MEDIA) {
        const std::wstring now = MediaNowName(InputKeyOf(g_ps->edit));
        SetCtlText(P(P_MEDIA_NOW), now.empty() ? L"Сейчас: —" : L"Сейчас: " + now);
    }
    InvalidateRect(g_ps->preview, nullptr, FALSE);
}

// --- preview of this source
void PaintPreview(HWND hwnd, HDC dc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(32, 33, 37));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    RECT area{S(8), S(8), rc.right - S(8), rc.bottom - S(26)};
    g_pubSolo.wantW = (std::max)(16, static_cast<int>(area.right - area.left));
    g_pubSolo.wantH = (std::max)(16, static_cast<int>(area.bottom - area.top));
    RECT d{};
    PaintPublished(dc, g_pubSolo, area, &d);
    g_ps->drawn = d;
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font);
    const Item& e = g_ps->edit;
    int nw = 0, nh = 0;
    const bool known = NativeSize(e.uid, &nw, &nh);
    RECT info{S(8), rc.bottom - S(24), rc.right - S(8), rc.bottom - S(2)};
    SetTextColor(dc, RGB(190, 190, 196));
    std::wstring it = known ? L"Источник: " + std::to_wstring(nw) + L"×" + std::to_wstring(nh) : L"Источник: нет картинки";
    it += L"   ·   Обрезка: тяните края рамки (или Alt + края в главном окне)";
    DrawTextW(dc, it.c_str(), -1, &info, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int dw = d.right - d.left, dh = d.bottom - d.top;
    if (dw < 4 || dh < 4) return;
    // crop + zoom in preview pixels
    const RectD crop{d.left + dw * e.crop[0] / 1000.0, d.top + dh * e.crop[1] / 1000.0, d.right - dw * e.crop[2] / 1000.0,
                     d.bottom - dh * e.crop[3] / 1000.0};
    const RectD eff = ItemSourceRect(e, dw, dh);
    const RECT E{static_cast<LONG>(d.left + eff.x0), static_cast<LONG>(d.top + eff.y0), static_cast<LONG>(d.left + eff.x1),
                 static_cast<LONG>(d.top + eff.y1)};
    // outside the shape: checkerboard
    const int ew = E.right - E.left, eh = E.bottom - E.top;
    if (!e.shape.Trivial() && ew > 2 && eh > 2) {
        std::vector<uint8_t> m;
        BuildShapeMask(ew, eh, e.shape, m);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = ew;
        bi.bmiHeader.biHeight = -eh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        uint8_t* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        if (bmp && bits) {
            HDC md = CreateCompatibleDC(dc);
            HGDIOBJ ob = SelectObject(md, bmp);
            BitBlt(md, 0, 0, ew, eh, dc, E.left, E.top, SRCCOPY);
            GdiFlush();
            const int cell = (std::max)(6, S(8));
            for (int y = 0; y < eh; ++y)
                for (int x = 0; x < ew; ++x) {
                    const int a = m[static_cast<size_t>(y) * ew + x];
                    if (a == 255) continue;
                    const int cv = (((x / cell) + (y / cell)) & 1) ? 150 : 204;
                    uint8_t* p = bits + (static_cast<size_t>(y) * ew + x) * 4;
                    for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>((p[c] * a + cv * (255 - a)) / 255);
                }
            BitBlt(dc, E.left, E.top, ew, eh, md, 0, 0, SRCCOPY);
            SelectObject(md, ob);
            DeleteDC(md);
        }
        if (bmp) DeleteObject(bmp);
        std::vector<std::vector<POINT>> polys;
        ShapeOutline(ew, eh, e.shape, &polys);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(240, 60, 60));
        HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        for (auto poly : polys) {
            for (auto& pt : poly) {
                pt.x += E.left;
                pt.y += E.top;
            }
            if (poly.size() >= 3) Polygon(dc, poly.data(), static_cast<int>(poly.size()));
        }
        SelectObject(dc, obr);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    // dim what is cut off
    {
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP one = CreateCompatibleBitmap(dc, 1, 1);
        HGDIOBJ ob = SelectObject(mdc, one);
        SetPixel(mdc, 0, 0, RGB(0, 0, 0));
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 160, 0};
        const RECT parts[4] = {{d.left, d.top, d.right, E.top},
                               {d.left, E.bottom, d.right, d.bottom},
                               {d.left, E.top, E.left, E.bottom},
                               {E.right, E.top, d.right, E.bottom}};
        for (const RECT& pr : parts)
            if (pr.right > pr.left && pr.bottom > pr.top)
                AlphaBlend(dc, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, mdc, 0, 0, 1, 1, bf);
        SelectObject(mdc, ob);
        DeleteObject(one);
        DeleteDC(mdc);
    }
    // crop frame with edge handles
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 210, 0));
    HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, static_cast<int>(crop.x0), static_cast<int>(crop.y0), static_cast<int>(crop.x1), static_cast<int>(crop.y1));
    SelectObject(dc, obr);
    SelectObject(dc, op);
    DeleteObject(pen);
    HBRUSH hb = CreateSolidBrush(RGB(255, 210, 0));
    const int hs = S(4);
    const int mx = static_cast<int>((crop.x0 + crop.x1) / 2), my = static_cast<int>((crop.y0 + crop.y1) / 2);
    const POINT hp[4] = {{static_cast<LONG>(crop.x0), my}, {mx, static_cast<LONG>(crop.y0)}, {static_cast<LONG>(crop.x1), my},
                         {mx, static_cast<LONG>(crop.y1)}};
    for (const POINT& pt : hp) {
        RECT r{pt.x - hs, pt.y - hs, pt.x + hs + 1, pt.y + hs + 1};
        FillRect(dc, &r, hb);
    }
    DeleteObject(hb);
}

int CropEdgeAt(int x, int y) {
    const RECT d = g_ps->drawn;
    const int dw = d.right - d.left, dh = d.bottom - d.top;
    if (dw < 4 || dh < 4) return -1;
    const Item& e = g_ps->edit;
    const double x0 = d.left + dw * e.crop[0] / 1000.0, y0 = d.top + dh * e.crop[1] / 1000.0;
    const double x1 = d.right - dw * e.crop[2] / 1000.0, y1 = d.bottom - dh * e.crop[3] / 1000.0;
    const int t = S(6);
    if (y >= y0 - t && y <= y1 + t) {
        if (std::fabs(x - x0) <= t) return 0;
        if (std::fabs(x - x1) <= t) return 2;
    }
    if (x >= x0 - t && x <= x1 + t) {
        if (std::fabs(y - y0) <= t) return 1;
        if (std::fabs(y - y1) <= t) return 3;
    }
    return -1;
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
        if (g_ps) PaintPreview(hwnd, m);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, old);
        DeleteObject(bmp);
        DeleteDC(m);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR: {
        if (!g_ps) break;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        const int edge = g_ps->dragEdge >= 0 ? g_ps->dragEdge : CropEdgeAt(pt.x, pt.y);
        if (edge >= 0) {
            SetCursor(LoadCursor(nullptr, (edge & 1) ? IDC_SIZENS : IDC_SIZEWE));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        if (!g_ps) break;
        const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
        const int edge = CropEdgeAt(x, y);
        if (edge >= 0) {
            g_ps->dragEdge = edge;
            g_ps->dragStart = POINT{x, y};
            g_ps->dragCrop0 = g_ps->edit.crop[edge];
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_ps && g_ps->dragEdge >= 0) {
            const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
            const RECT d = g_ps->drawn;
            const int edge = g_ps->dragEdge;
            const double full = (edge & 1) ? (d.bottom - d.top) : (d.right - d.left);
            double delta = (edge & 1) ? (y - g_ps->dragStart.y) : (x - g_ps->dragStart.x);
            if (edge >= 2) delta = -delta;
            Item& e = g_ps->edit;
            e.crop[edge] = std::clamp(g_ps->dragCrop0 + static_cast<int>(std::lround(delta / (std::max)(1.0, full) * 1000)), 0,
                                      900 - e.crop[edge ^ 2]);
            g_ps->syncing = true;
            SetCtlText(P(P_CROP0 + 2 * edge), std::to_wstring((e.crop[edge] + 5) / 10));
            g_ps->syncing = false;
            Push();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (g_ps && g_ps->dragEdge >= 0) {
            g_ps->dragEdge = -1;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void MediaAddFiles() {
    std::vector<std::wstring> files;
    std::wstring one;
    if (!PickFile(g_ps->dlg, kMediaFilter(), &one, true, &files)) return;
    for (auto& f : files) g_ps->edit.media.push_back(f);
    FillMedia();
}

void ShowMonitorMenuForRegion() {
    std::vector<MonitorItem> mons = Monitors();
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < mons.size(); ++i) {
        std::wstring t = L"Монитор " + std::to_wstring(i + 1) + (mons[i].primary ? L" (основной)" : L"") + L" — " +
                         std::to_wstring(mons[i].rc.right - mons[i].rc.left) + L"×" +
                         std::to_wstring(mons[i].rc.bottom - mons[i].rc.top);
        AppendMenuW(m, MF_STRING, 1 + i, t.c_str());
    }
    RECT r;
    GetWindowRect(P(P_REGION_MON), &r);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, r.left, r.bottom, 0, g_ps->dlg, nullptr);
    DestroyMenu(m);
    if (cmd >= 1 && cmd <= static_cast<int>(mons.size())) {
        g_ps->edit.region = mons[static_cast<size_t>(cmd - 1)].rc;
        g_ps->edit.needFit = true;
        SetCtlText(P(P_REGION_LBL), RegionText(g_ps->edit.region));
    }
}

void OnCommand(int id, int code) {
    PropsState& s = *g_ps;
    switch (id) {
    case P_OK:
        s.ok = true;
        s.done = true;
        return;
    case P_CANCEL:
        s.done = true;
        return;
    case P_DEFAULTS: {
        Item& e = s.edit;
        e.aspect = 0;
        e.fit = 0;
        for (int& c : e.crop) c = 0;
        e.rot = 0;
        e.mirror = e.type == IT_WEBCAM;
        e.zoom = 100;
        e.shape = ShapeDef();
        e.opacity = 100;
        e.border = false;
        SyncControls();
        Push();
        return;
    }
    case P_REGION_SEL: {
        RECT r;
        ShowWindow(s.dlg, SW_HIDE);
        const bool ok = SelectRegion(g_hInst, &r);
        ShowWindow(s.dlg, SW_SHOW);
        SetForegroundWindow(s.dlg);
        if (ok) {
            s.edit.region = r;
            s.edit.needFit = true;
            SetCtlText(P(P_REGION_LBL), RegionText(r));
            Push();
        }
        return;
    }
    case P_REGION_MON:
        ShowMonitorMenuForRegion();
        Push();
        return;
    case P_HIDEWIN:
        Push();
        ToggleHideWindow(s.edit.uid);
        {
            Project p = ProjSnapshot();
            if (const Item* it = FindItemC(p, s.edit.uid)) s.edit.winCapture = it->winCapture;
            s.syncing = true;
            ComboSet(P_WINCAP, s.edit.winCapture);
            s.syncing = false;
        }
        return;
    case P_WIN:
        if (code == CBN_DROPDOWN) {
            s.syncing = true;
            FillWindows();
            s.syncing = false;
            return;
        }
        break;
    case P_CAM:
        if (code == CBN_DROPDOWN) {
            s.syncing = true;
            FillCams();
            s.syncing = false;
            return;
        }
        break;
    case P_MEDIA_ADD:
        MediaAddFiles();
        Push();
        return;
    case P_MEDIA_DEL: {
        const int sel = static_cast<int>(SendMessageW(P(P_MEDIA), LB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(s.edit.media.size())) s.edit.media.erase(s.edit.media.begin() + sel);
        FillMedia();
        Push();
        return;
    }
    case P_MEDIA_UP:
    case P_MEDIA_DOWN: {
        const int sel = static_cast<int>(SendMessageW(P(P_MEDIA), LB_GETCURSEL, 0, 0));
        const int to = id == P_MEDIA_UP ? sel - 1 : sel + 1;
        if (sel >= 0 && to >= 0 && to < static_cast<int>(s.edit.media.size())) {
            std::swap(s.edit.media[static_cast<size_t>(sel)], s.edit.media[static_cast<size_t>(to)]);
            FillMedia();
            SendMessageW(P(P_MEDIA), LB_SETCURSEL, to, 0);
            Push();
        }
        return;
    }
    case P_MEDIA_NEXT: MediaNextAll(InputKeyOf(s.edit)); return;
    case P_PH_FIND:
        s.phoneNote = s.edit.phoneConn == 1 ? L"Ищу телефоны по USB…" : L"Ищу телефоны в сети…";
        if (s.edit.phoneConn == 1) PhoneUsbList(s.dlg);
        else PhoneDiscover(s.dlg);
        UpdateLive();
        return;
    case P_PH_CONNECT: {
        Push();
        const std::wstring key = InputKeyOf(s.edit);
        const PhoneInfo pi = PhoneInfoFor(key);
        PhoneUserConnect(key, (pi.exists && PhoneUserState(key) != -1) ? -1 : 1);
        InputsSync();
        UpdateLive();
        return;
    }
    case P_IMAGE_BTN: {
        std::wstring f;
        if (PickFile(s.dlg, kImageFilter(), &f)) {
            s.edit.image = f;
            s.edit.needFit = true;
            SetCtlText(P(P_IMAGE), f);
            Push();
        }
        return;
    }
    case P_MASK_BTN: {
        std::wstring f;
        if (PickFile(s.dlg, L"PNG-маска\0*.png\0Картинки\0*.png;*.bmp;*.jpg\0", &f)) {
            s.edit.shape.mask = f;
            SetCtlText(P(P_MASK), f);
            Push();
        }
        return;
    }
    }
    if (s.syncing) return;
    if ((id == P_CC_EXP || id == P_CC_BR || id == P_CC_WB) && code == CBN_SELCHANGE) {
        CamCtlFromControls();
        CamCtlLive();
        return;
    }
    const bool edit = id == P_NAME || id == P_PH_IP || id == P_TEXT || id == P_TEXTPX || id == P_ZOOM ||
                      (id >= P_CROP0 && id <= P_CROP3_UD);
    const bool combo = id == P_WIN || id == P_WINCAP || id == P_MON || id == P_CAM || id == P_MEDIA_SEC || id == P_PH_CONN ||
                       id == P_PH_LIST || id == P_PH_SRC || id == P_PH_RES || id == P_PH_CODEC || id == P_TEXTCOLOR ||
                       id == P_ASPECT || id == P_FIT || id == P_ROT || id == P_SHAPE;
    const bool check = id == P_CURSOR || id == P_WINFRAME || id == P_PH_AUTOROT || id == P_TEXTBG || id == P_TEXTBOLD || id == P_MIRROR ||
                       id == P_BORDER;
    if ((edit && code == EN_CHANGE) || (combo && code == CBN_SELCHANGE) || (check && code == BN_CLICKED)) {
        if (id == P_PH_IP) {  // the address is used when the field loses focus or OK is pressed
            s.edit.phoneIp = Trim(GetCtlText(P(P_PH_IP)));
            return;
        }
        ReadControls(id);
        Push();
    } else if (id == P_PH_IP && code == EN_KILLFOCUS) {
        Push();
    }
}

LRESULT CALLBACK PropsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (g_ps) {
            int id = LOWORD(wp);
            if (id == IDCANCEL) id = P_CANCEL;  // Esc
            OnCommand(id, HIWORD(wp));
        }
        return 0;
    case WM_HSCROLL:
        if (g_ps && !g_ps->syncing) {
            HWND t = reinterpret_cast<HWND>(lp);
            const int pos = static_cast<int>(SendMessageW(t, TBM_GETPOS, 0, 0));
            if (t == P(P_RADIUS)) g_ps->edit.shape.radius = pos;
            else if (t == P(P_FEATHER)) g_ps->edit.shape.feather = pos;
            else if (t == P(P_OPACITY)) g_ps->edit.opacity = pos;
            if (t == P(P_CC_EXPV) || t == P(P_CC_BRV) || t == P(P_CC_WBV)) {
                CamCtlFromControls();
                return 0;
            }
            Push();
        }
        return 0;
    case WM_TIMER:
        if (wp == kTimerProps) UpdateLive();
        return 0;
    case WM_PHONE_FOUND:
        if (g_ps) {
            g_ps->phoneNote = wp ? L"Найдено телефонов: " + std::to_wstring(wp) + L". Выберите в списке."
                                 : L"Телефон не найден: WVC Phone открыт? Одна сеть Wi-Fi? WVC разрешён в брандмауэре "
                                   L"(частная сеть)? IP можно ввести вручную.";
            g_ps->syncing = true;
            FillPhones();
            g_ps->syncing = false;
            if (wp == 1 && Trim(g_ps->edit.phoneIp).empty()) {
                const auto f = PhoneFoundList();
                if (!f.empty()) {
                    g_ps->edit.phoneIp = f[0].ip;
                    SetCtlText(P(P_PH_IP), f[0].ip);
                    ComboSet(P_PH_LIST, 0);
                    Push();
                }
            }
            UpdateLive();
        }
        return 0;
    case WM_PHONE_USBLIST:
        if (g_ps) {
            if (wp == 1) g_ps->phoneNote = UsbDevices().empty() ? L"Телефон по USB не найден — подключите кабель и включите "
                                                                   L"Отладку по USB."
                                                                 : L"";
            else if (wp == 2) g_ps->phoneNote = L"Не найден adb.exe рядом с WVC.exe — распакуйте WVC.zip целиком.";
            else if (wp == 3) g_ps->phoneNote = L"adb не ответил. Закройте другие программы, которые используют adb.";
            g_ps->syncing = true;
            FillPhones();
            g_ps->syncing = false;
            UpdateLive();
        }
        return 0;
    case WM_CLOSE:
        if (g_ps) g_ps->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void BuildForm(PropsState& s) {
    Form& f = s.form;
    Item& e = s.edit;
    auto keep = [&](int id, HWND h) {
        s.ctl[id] = h;
        return h;
    };
    keep(P_NAME, f.Row(L"Название:", L"EDIT", e.name.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, P_NAME, 24, -1, WS_EX_CLIENTEDGE));
    switch (e.type) {
    case IT_REGION: {
        f.Header(L"Область экрана");
        keep(P_REGION_LBL, f.Row(L"Сейчас:", L"STATIC", RegionText(e.region).c_str(), SS_LEFT, P_REGION_LBL, 20));
        const int y = f.y;
        keep(P_REGION_SEL, f.Make(L"BUTTON", L"Выделить область…", WS_TABSTOP | BS_PUSHBUTTON, P_REGION_SEL, f.cx, y, 160, 28));
        keep(P_REGION_MON, f.Make(L"BUTTON", L"Весь монитор ▾", WS_TABSTOP | BS_PUSHBUTTON, P_REGION_MON, f.cx + 168, y, 150, 28));
        f.y += 36;
        keep(P_CURSOR, f.Check(L"Показывать курсор мыши", P_CURSOR));
        f.Hint(L"На экране область обведена рамкой (зелёной без трансляции, красной в эфире). Рамку можно двигать за "
               L"ярлык и растягивать за края.", 2);
        break;
    }
    case IT_WINDOW:
        f.Header(L"Окно");
        keep(P_WIN, f.Combo(L"Окно программы:", P_WIN, {}));
        keep(P_WINCAP, f.Combo(L"Способ захвата:", P_WINCAP, {L"С экрана (окно должно быть видно)", L"Прямо из окна (можно перекрыть)"}));
        keep(P_HIDEWIN, f.Row(L"", L"BUTTON", L"Спрятать окно за экран / вернуть", WS_TABSTOP | BS_PUSHBUTTON, P_HIDEWIN, 28, 260));
        keep(P_CURSOR, f.Check(L"Показывать курсор мыши", P_CURSOR));
        keep(P_WINFRAME, f.Check(L"Тонкая цветная рамка вокруг окна на экране", P_WINFRAME));
        f.Hint(L"Окно ищется по заголовку и после перезапуска. Свёрнутое окно не показывается — «Спрятать окно за экран» "
               L"оставляет его работать.", 2);
        break;
    case IT_MONITOR:
        f.Header(L"Монитор");
        keep(P_MON, f.Combo(L"Монитор:", P_MON, {}));
        keep(P_CURSOR, f.Check(L"Показывать курсор мыши", P_CURSOR));
        break;
    case IT_WEBCAM:
        f.Header(L"Веб-камера");
        keep(P_CAM, f.Combo(L"Устройство:", P_CAM, {}));
        f.Hint(L"Камера включается, только пока она видна в сцене (или открыт предпросмотр). «Держать веб-камеру "
               L"включённой» — в «Настройки → Дополнительно».", 2);
        f.Header(L"Изображение камеры");
        keep(P_CC_EXP, f.Combo(L"Экспозиция:", P_CC_EXP, {L"Авто", L"Вручную"}, 140));
        keep(P_CC_EXPV, f.Track(L"", P_CC_EXPV, 0, 100));
        keep(P_CC_BR, f.Combo(L"Яркость:", P_CC_BR, {L"Как у камеры", L"Вручную"}, 140));
        keep(P_CC_BRV, f.Track(L"", P_CC_BRV, 0, 100));
        keep(P_CC_WB, f.Combo(L"Баланс белого:", P_CC_WB, {L"Авто", L"Вручную"}, 140));
        keep(P_CC_WBV, f.Track(L"", P_CC_WBV, 0, 100));
        keep(P_CC_NOTE, f.Hint(L"", 2));
        break;
    case IT_MEDIA: {
        f.Header(L"Медиафайлы (картинки, GIF, видео без звука)");
        keep(P_MEDIA, f.Make(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, P_MEDIA,
                             f.lx, f.y, f.cx + f.cw - f.lx, 110));
        f.y += 116;
        const int y = f.y;
        keep(P_MEDIA_ADD, f.Make(L"BUTTON", L"Добавить…", WS_TABSTOP | BS_PUSHBUTTON, P_MEDIA_ADD, f.lx, y, 100, 28));
        keep(P_MEDIA_DEL, f.Make(L"BUTTON", L"Убрать", WS_TABSTOP | BS_PUSHBUTTON, P_MEDIA_DEL, f.lx + 106, y, 80, 28));
        keep(P_MEDIA_UP, f.Make(L"BUTTON", L"▲", WS_TABSTOP | BS_PUSHBUTTON, P_MEDIA_UP, f.lx + 192, y, 36, 28));
        keep(P_MEDIA_DOWN, f.Make(L"BUTTON", L"▼", WS_TABSTOP | BS_PUSHBUTTON, P_MEDIA_DOWN, f.lx + 232, y, 36, 28));
        keep(P_MEDIA_NEXT, f.Make(L"BUTTON", L"Следующий файл ▶", WS_TABSTOP | BS_PUSHBUTTON, P_MEDIA_NEXT, f.lx + 280, y, 160, 28));
        f.y += 36;
        keep(P_MEDIA_SEC, f.Combo(L"Картинка показывается:", P_MEDIA_SEC, {L"3 с", L"5 с", L"10 с", L"30 с", L"60 с"}, 120));
        keep(P_MEDIA_NOW, f.Row(L"", L"STATIC", L"Сейчас: —", SS_LEFT | SS_ENDELLIPSIS, P_MEDIA_NOW, 20));
        break;
    }
    case IT_PHONE: {
        f.Header(L"Телефон (WVC Phone)");
        keep(P_PH_CONN, f.Combo(L"Подключение:", P_PH_CONN, {L"Wi-Fi", L"USB-кабель (adb)"}, 200));
        const int y = f.y;
        f.Make(L"STATIC", L"Телефон:", SS_LEFT, -1, f.lx, y + 4, f.lw, 20);
        keep(P_PH_LIST, f.Make(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, P_PH_LIST, f.cx, y, f.cw - 90, 300));
        keep(P_PH_FIND, f.Make(L"BUTTON", L"Найти", WS_TABSTOP | BS_PUSHBUTTON, P_PH_FIND, f.cx + f.cw - 84, y, 84, 26));
        f.y += 32;
        const int y2 = f.y;
        f.Make(L"STATIC", L"IP-адрес (Wi-Fi):", SS_LEFT, -1, f.lx, y2 + 4, f.lw, 20);
        keep(P_PH_IP, f.Make(L"EDIT", e.phoneIp.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, P_PH_IP, f.cx, y2, 170, 24, WS_EX_CLIENTEDGE));
        keep(P_PH_CONNECT, f.Make(L"BUTTON", L"Подключить", WS_TABSTOP | BS_PUSHBUTTON, P_PH_CONNECT, f.cx + 178, y2, 120, 26));
        f.y += 32;
        keep(P_PH_SRC, f.Combo(L"Что показывать:", P_PH_SRC, {L"Задняя камера", L"Передняя камера", L"Экран телефона"}, 200));
        keep(P_PH_RES, f.Combo(L"Качество:", P_PH_RES, {L"720p (быстрее)", L"1080p (чётче)"}, 200));
        keep(P_PH_CODEC, f.Combo(L"Передача:", P_PH_CODEC,
                                 {L"Авто: H.264 (быстро), иначе JPEG", L"JPEG (совместимость)",
                                  L"Камера через экран (запасной способ)"}));
        keep(P_PH_AUTOROT, f.Check(L"Автоориентация (повернули телефон — повернулась рамка)", P_PH_AUTOROT));
        keep(P_PH_STATUS, f.Row(L"Состояние:", L"STATIC", L"", SS_LEFT, P_PH_STATUS, 70));
        f.Hint(L"«Камера через экран»: WVC Phone показывает камеру во весь экран и передаёт её как экран телефона — "
               L"только если обычный способ не работает на этом телефоне. Без автоориентации размер и поворот рамки "
               L"задаёте сами (ниже).", 3);
        break;
    }
    case IT_TEXT:
        f.Header(L"Текст");
        keep(P_TEXT, f.Row(L"Текст:", L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                           P_TEXT, 66, -1, WS_EX_CLIENTEDGE));
        keep(P_TEXTPX, f.Spin(L"Размер (пикс. при 1080p):", P_TEXTPX, 8, 400));
        keep(P_TEXTCOLOR, f.Combo(L"Цвет:", P_TEXTCOLOR,
                                  {L"Белый", L"Жёлтый", L"Чёрный", L"Красный", L"Зелёный", L"Синий", L"Оранжевый", L"Голубой"}, 160));
        keep(P_TEXTBG, f.Check(L"Тёмная подложка", P_TEXTBG));
        keep(P_TEXTBOLD, f.Check(L"Жирный", P_TEXTBOLD));
        break;
    case IT_IMAGE: {
        f.Header(L"Логотип / картинка (PNG с прозрачностью — тоже)");
        keep(P_IMAGE, f.Row(L"Файл:", L"EDIT", e.image.c_str(), ES_READONLY | ES_AUTOHSCROLL, P_IMAGE, 24, -1, WS_EX_CLIENTEDGE));
        keep(P_IMAGE_BTN, f.Row(L"", L"BUTTON", L"Выбрать файл…", WS_TABSTOP | BS_PUSHBUTTON, P_IMAGE_BTN, 28, 150));
        break;
    }
    }
    f.Header(L"Кадр источника");
    keep(P_ASPECT, f.Combo(L"Пропорции рамки:", P_ASPECT, {L"Как у источника", L"16:9", L"4:3", L"1:1 (квадрат)", L"9:16", L"3:4"}, 200));
    keep(P_FIT, f.Combo(L"Если пропорции другие:", P_FIT, {L"Вписать целиком", L"Заполнить (обрезать края)", L"Растянуть"}, 220));
    {
        const wchar_t* names[4] = {L"Обрезка слева, %:", L"Обрезка сверху, %:", L"Обрезка справа, %:", L"Обрезка снизу, %:"};
        for (int i = 0; i < 4; ++i) keep(P_CROP0 + 2 * i, f.Spin(names[i], P_CROP0 + 2 * i, 0, 90, 80));
    }
    keep(P_ROT, f.Combo(L"Поворот:", P_ROT, {L"Без поворота", L"90° по часовой", L"180°", L"90° против часовой"}, 200));
    keep(P_MIRROR, f.Check(L"Отразить (зеркально)", P_MIRROR));
    keep(P_ZOOM, f.Spin(L"Увеличение центра, %:", P_ZOOM, 100, 400, 90));
    SendMessageW(GetDlgItem(f.panel, P_ZOOM + 1), UDM_SETACCEL, 0, 0);
    {
        HWND ud = GetDlgItem(f.panel, P_ZOOM + 1);
        UDACCEL acc{0, 5};
        SendMessageW(ud, UDM_SETACCEL, 1, reinterpret_cast<LPARAM>(&acc));
    }
    HWND sh = keep(P_SHAPE, f.Combo(L"Форма:", P_SHAPE, {}, 260));
    for (int k = 0; k < SH_COUNT; ++k) SendMessageW(sh, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kShapeNames[k]));
    keep(P_RADIUS, f.Track(L"Радиус скругления:", P_RADIUS, 0, 50));
    keep(P_FEATHER, f.Track(L"Мягкий край:", P_FEATHER, 0, 30));
    {
        const int y = f.y;
        f.Make(L"STATIC", L"PNG-маска:", SS_LEFT, -1, f.lx, y + 4, f.lw, 20);
        keep(P_MASK, f.Make(L"EDIT", e.shape.mask.c_str(), ES_READONLY | ES_AUTOHSCROLL, P_MASK, f.cx, y, f.cw - 96, 24, WS_EX_CLIENTEDGE));
        keep(P_MASK_BTN, f.Make(L"BUTTON", L"Выбрать…", WS_TABSTOP | BS_PUSHBUTTON, P_MASK_BTN, f.cx + f.cw - 90, y, 90, 26));
        f.y += 32;
        f.Hint(L"Маска: белое / непрозрачное — видно, чёрное / прозрачное — не видно. Вне формы в камере виден "
               L"фон сцены; в предпросмотре здесь — шахматка.", 2);
    }
    keep(P_OPACITY, f.Track(L"Непрозрачность:", P_OPACITY, 0, 100));
    keep(P_BORDER, f.Check(L"Белая обводка по форме", P_BORDER));
    f.Gap(6);
    f.Finish();
}
}  // namespace

bool OpenProperties(int uid, bool isNew) {
    if (g_ps) return false;  // one at a time
    Project p = ProjSnapshot();
    const Item* found = FindItemC(p, uid);
    if (!found) return false;
    PropsState st;
    st.edit = *found;
    st.orig = *found;
    st.isNew = isNew;
    g_ps = &st;
    {
        std::lock_guard<std::mutex> lk(g_soloMx);
        g_soloItem = st.edit;
    }
    g_soloUid = uid;
    {
        std::lock_guard<std::mutex> lk(g_pubSolo.mx);
        g_pubSolo.px.clear();
    }
    static bool reg = false;
    if (!reg) {
        reg = true;
        WNDCLASSW wc{};
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpfnWndProc = PropsProc;
        wc.lpszClassName = L"WVCProps";
        RegisterClassW(&wc);
        wc.lpfnWndProc = PreviewProc;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"WVCSolo";
        RegisterClassW(&wc);
    }
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int cw = S(600);
    const int ch = (std::min)(S(880), static_cast<int>(work.bottom - work.top) - S(60));
    RECT r{0, 0, cw, ch};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, g_dpi);
    RECT mr;
    GetWindowRect(g_main, &mr);
    const int w = r.right - r.left, h = r.bottom - r.top;
    int x = mr.left + ((mr.right - mr.left) - w) / 2, y = (std::max)(static_cast<int>(work.top), static_cast<int>(mr.top + ((mr.bottom - mr.top) - h) / 2));
    std::wstring title = L"Свойства — " + st.edit.name + L" (" + kTypeNames[st.edit.type] + L")";
    st.dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"WVCProps", title.c_str(), style, x, y, w, h,
                             g_main, nullptr, g_hInst, nullptr);
    if (!st.dlg) {
        g_ps = nullptr;
        g_soloUid = 0;
        return false;
    }
    const int prevH = (std::min)(S(300), ch * 36 / 100), btnH = S(48);
    st.preview = CreateWindowExW(0, L"WVCSolo", L"", WS_CHILD | WS_VISIBLE, 0, 0, cw, prevH, st.dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(P_PREVIEW)), g_hInst, nullptr);
    st.form.Create(st.dlg, P_FORM);
    MoveWindow(st.form.panel, 0, prevH, cw, ch - prevH - btnH, FALSE);
    st.form.cw = 600 - 190 - 34;
    BuildForm(st);
    HWND bDef = MakeChild(st.dlg, L"BUTTON", L"По умолчанию (кадр)", WS_TABSTOP | BS_PUSHBUTTON, P_DEFAULTS, 12, 0, 170, 32);
    HWND bOk = MakeChild(st.dlg, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, P_OK, 0, 0, 110, 32);
    HWND bCancel = MakeChild(st.dlg, L"BUTTON", L"Отмена", WS_TABSTOP | BS_PUSHBUTTON, P_CANCEL, 0, 0, 110, 32);
    const int by = ch - btnH + S(8);
    MoveWindow(bDef, S(12), by, S(170), S(32), FALSE);
    MoveWindow(bOk, cw - S(240), by, S(110), S(32), FALSE);
    MoveWindow(bCancel, cw - S(122), by, S(110), S(32), FALSE);
    switch (st.edit.type) {
    case IT_WINDOW: FillWindows(); break;
    case IT_MONITOR: FillMonitors(); break;
    case IT_WEBCAM: FillCams(); break;
    case IT_PHONE:
        FillPhones();
        if (st.edit.phoneConn == 1) PhoneUsbList(st.dlg);
        break;
    }
    SyncControls();
    SetTimer(st.dlg, kTimerProps, 300, nullptr);
    UpdateLive();
    RunModal(st.dlg, &st.done);
    // the IP typed last is used too
    if (st.ok) {
        Push();
        SaveProject();
    } else {
        // Отмена: everything back as it was (a just added source stays, with its first settings)
        const Item o = st.orig;
        ProjEdit([&](Project& pp) {
            if (Item* it = FindItem(pp, o.uid)) *it = o;
        });
    }
    g_soloUid = 0;
    g_pubSolo.wantW = 0;
    g_ps = nullptr;
    UiRefresh();
    return st.ok;
}

void AddSourceOfType(int type) {
    RECT region{};
    if (type == IT_REGION) {
        if (!SelectRegion(g_hInst, &region)) region = PrimaryMonitorRect();
    }
    std::vector<CamDevice> cams;
    if (type == IT_WEBCAM) cams = EnumWebcams();
    std::vector<MonitorItem> mons;
    if (type == IT_MONITOR) mons = Monitors();
    int uid = 0;
    ProjEdit([&](Project& p) {
        Scene* sc = UiEditScene(p);  // «Сейчас» or «Следующий» — the one being edited
        if (!sc) return;
        Item it = NewItem(p, type);
        it.name = UniqueItemName(*sc, it.name);
        if (type == IT_REGION) it.region = region;
        if (type == IT_WEBCAM && !cams.empty()) {
            // a webcam not used yet in this scene, else the first one
            const CamDevice* pick = &cams[0];
            for (const auto& c : cams) {
                bool used = false;
                for (const auto& o : sc->items) used = used || (o.type == IT_WEBCAM && o.camLink == c.link);
                if (!used) {
                    pick = &c;
                    break;
                }
            }
            it.camLink = pick->link;
            it.camName = pick->name;
        }
        if (type == IT_MONITOR) {
            for (const auto& m : mons)
                if (m.primary) {
                    it.monDevice = m.device;
                    it.monRect = m.rc;
                }
        }
        uid = it.uid;
        sc->items.push_back(it);
    });
    if (!uid) return;
    UiSelect(uid);
    OpenProperties(uid, true);
}

// --- "hide" a window off screen so it keeps playing while it is captured straight from the window
namespace {
HWND g_hiddenWin = nullptr;
RECT g_hiddenRect{};
bool g_hiddenMax = false;
}  // namespace

void RestoreHiddenWindow() {
    if (!g_hiddenWin) return;
    if (IsWindow(g_hiddenWin)) {
        SetWindowPos(g_hiddenWin, nullptr, g_hiddenRect.left, g_hiddenRect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_hiddenMax) ShowWindow(g_hiddenWin, SW_MAXIMIZE);
    }
    g_hiddenWin = nullptr;
}

void ToggleHideWindow(int uid) {
    if (g_hiddenWin) {
        RestoreHiddenWindow();
        SetNote(L"Окно возвращено на экран.");
        return;
    }
    Project p = ProjSnapshot();
    const Item* it = FindItemC(p, uid);
    if (!it || it->type != IT_WINDOW || !it->hwnd || !IsWindow(it->hwnd)) {
        MessageBoxW(g_main, L"Сначала выберите окно в свойствах источника «Окно».", L"WVC", MB_ICONINFORMATION);
        return;
    }
    HWND w = it->hwnd;
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    g_hiddenMax = IsZoomed(w) != FALSE;
    if (g_hiddenMax) ShowWindow(w, SW_RESTORE);
    GetWindowRect(w, &g_hiddenRect);
    RECT vs = VirtualScreen();
    SetWindowPos(w, nullptr, vs.right + 200, g_hiddenRect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_hiddenWin = w;
    ProjEdit([&](Project& pp) {
        if (Item* i = FindItem(pp, uid)) i->winCapture = 1;  // the screen no longer shows it
    });
    SetNote(L"Окно спрятано за край экрана и продолжает работать. Вернуть — той же кнопкой или клавишей.");
}
