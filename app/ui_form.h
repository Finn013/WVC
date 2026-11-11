// WVC: a scrollable panel of label + control rows, used by «Свойства» and «Настройки».
#pragma once
#include "common.h"

struct Form {
    HWND panel = nullptr;
    int y = 10;               // next row, unscaled px
    int lx = 14, lw = 170;    // label column
    int cx = 190, cw = 330;   // control column
    std::vector<HWND> all;

    static void Register();
    void Create(HWND parent, int id);
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int yy, int w, int h, DWORD ex = 0,
              HFONT font = nullptr);
    void Header(const wchar_t* text);
    HWND Label(const wchar_t* text, int h = 20);
    HWND Row(const wchar_t* label, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int h = 24, int w = -1,
             DWORD ex = 0);
    HWND Combo(const wchar_t* label, int id, std::initializer_list<const wchar_t*> items, int w = -1);
    HWND Check(const wchar_t* text, int id);
    HWND Hint(const wchar_t* text, int lines = 2);
    HWND Spin(const wchar_t* label, int id, int lo, int hi, int w = 90);  // edit + up-down (the up-down is id + 1)
    HWND Track(const wchar_t* label, int id, int lo, int hi, int w = -1);
    void Gap(int g = 8) { y += g; }
    void Finish();  // sets the scroll range
};

inline LRESULT CALLBACK FormWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_MEASUREITEM: return SendMessageW(GetParent(hwnd), msg, wp, lp);
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        const LONG_PTR style = GetWindowLongPtrW(reinterpret_cast<HWND>(lp), GWL_STYLE);
        if ((style & SS_TYPEMASK) == SS_LEFT && GetWindowLongPtrW(reinterpret_cast<HWND>(lp), GWLP_USERDATA) == 1)
            SetTextColor(dc, RGB(0, 84, 160));
        else if (GetWindowLongPtrW(reinterpret_cast<HWND>(lp), GWLP_USERDATA) == 2)
            SetTextColor(dc, RGB(96, 96, 96));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_MOUSEWHEEL: {
        const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        for (int i = 0; i < std::abs(steps) * 3; ++i)
            SendMessageW(hwnd, WM_VSCROLL, steps > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
        return 0;
    }
    case WM_VSCROLL: {
        if (lp) return 0;  // an up-down control, not the panel's scroll bar
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= S(20); break;
        case SB_LINEDOWN: pos += S(20); break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        }
        pos = std::clamp(pos, 0, (std::max)(0, si.nMax - static_cast<int>(si.nPage) + 1));
        if (pos != si.nPos) {
            ScrollWindowEx(hwnd, 0, si.nPos - pos, nullptr, nullptr, nullptr, nullptr,
                           SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
            si.fMask = SIF_POS;
            si.nPos = pos;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        return 0;
    }
    case WM_SIZE: {
        SCROLLINFO si{sizeof(si), SIF_PAGE};
        RECT rc;
        GetClientRect(hwnd, &rc);
        si.nPage = static_cast<UINT>(rc.bottom);
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
    }

inline void Form::Register() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSW wc{};
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"WVCForm";
    wc.lpfnWndProc = FormWndProc;
    RegisterClassW(&wc);
}

inline void Form::Create(HWND parent, int id) {
    Register();
    panel = CreateWindowExW(WS_EX_CONTROLPARENT, L"WVCForm", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0,
                            10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
}

inline HWND Form::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int yy, int w, int h,
                       DWORD ex, HFONT font) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(yy), S(w), S(h), panel,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
    all.push_back(c);
    return c;
}

inline void Form::Header(const wchar_t* text) {
    y += 6;
    HWND h = Make(L"STATIC", text, SS_LEFT, -1, lx, y, cx + cw - lx, 20, 0, g_fontBold);
    SetWindowLongPtrW(h, GWLP_USERDATA, 1);
    y += 26;
}

inline HWND Form::Label(const wchar_t* text, int h) {
    HWND c = Make(L"STATIC", text, SS_LEFT, -1, lx, y, cx + cw - lx, h);
    y += h + 4;
    return c;
}

inline HWND Form::Row(const wchar_t* label, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int h, int w,
                      DWORD ex) {
    if (label && *label) Make(L"STATIC", label, SS_LEFT | SS_ENDELLIPSIS, -1, lx, y + 4, lw, 20);
    HWND c = Make(cls, text, style, id, cx, y, w < 0 ? cw : w, h, ex);
    y += (std::max)(h, 24) + 6;
    return c;
}

inline HWND Form::Combo(const wchar_t* label, int id, std::initializer_list<const wchar_t*> items, int w) {
    HWND c = Row(label, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id, 300, w);
    y -= 300 + 6 - 30;
    for (auto* s : items) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    return c;
}

inline HWND Form::Check(const wchar_t* text, int id) {
    HWND c = Make(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, id, cx, y, cw, 22);
    y += 28;
    return c;
}

inline HWND Form::Hint(const wchar_t* text, int lines) {
    HWND c = Make(L"STATIC", text, SS_LEFT, -1, lx, y, cx + cw - lx, 17 * lines + 2);
    SetWindowLongPtrW(c, GWLP_USERDATA, 2);
    y += 17 * lines + 8;
    return c;
}

inline HWND Form::Spin(const wchar_t* label, int id, int lo, int hi, int w) {
    HWND e = Row(label, L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_RIGHT, id, 24, w, WS_EX_CLIENTEDGE);
    HWND ud = Make(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, id + 1, 0, 0, 10,
                   10);
    SendMessageW(ud, UDM_SETBUDDY, reinterpret_cast<WPARAM>(e), 0);
    SendMessageW(ud, UDM_SETRANGE32, lo, hi);
    return e;
}

inline HWND Form::Track(const wchar_t* label, int id, int lo, int hi, int w) {
    HWND t = Row(label, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, id, 26, w);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    return t;
}

inline void Form::Finish() {
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
    RECT rc;
    GetClientRect(panel, &rc);
    si.nMin = 0;
    si.nMax = S(y + 12);
    si.nPage = static_cast<UINT>(rc.bottom);
    si.nPos = 0;
    SetScrollInfo(panel, SB_VERT, &si, TRUE);
}
