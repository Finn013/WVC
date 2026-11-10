// WVC — лёгкая виртуальная камера, которая показывает выбранную область экрана.
// Камера: DirectShow-фильтр на базе tshino/softcam (MIT), лежит в WVCCam.dll / WVCCam32.dll.
// Это приложение: выбор области, захват экрана (GDI), отправка кадров в камеру.

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// softcam sender API (loaded dynamically from WVCCam.dll)
// ---------------------------------------------------------------------------
typedef void* scCamera;
typedef scCamera(__cdecl* PFN_scCreateCamera)(int, int, float);
typedef void(__cdecl* PFN_scDeleteCamera)(scCamera);
typedef void(__cdecl* PFN_scSendFrame)(scCamera, const void*);
typedef bool(__cdecl* PFN_scIsConnected)(scCamera);
typedef HRESULT(STDAPICALLTYPE* PFN_DllRegister)();

static HMODULE            g_camDll = nullptr;
static PFN_scCreateCamera pCreate = nullptr;
static PFN_scDeleteCamera pDelete = nullptr;
static PFN_scSendFrame    pSend = nullptr;
static PFN_scIsConnected  pIsConnected = nullptr;

static const wchar_t* kCamClsid = L"{32E48B39-D01F-4852-9568-C345CD9082BE}";
static const wchar_t* kDll64 = L"WVCCam.dll";
static const wchar_t* kDll32 = L"WVCCam32.dll";

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
struct Settings {
    int  res = 1;          // 0 = 1280x720, 1 = 1920x1080
    int  fps = 30;         // 30 or 60
    bool cursor = false;   // draw mouse cursor
    int  fit = 0;          // 0 = letterbox, 1 = crop
    bool hasRegion = false;
    RECT region{};         // virtual-screen coordinates, physical pixels
};

static const int kResW[] = {1280, 1920};
static const int kResH[] = {720, 1080};

static Settings    g_set;
static std::mutex  g_setMx;

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

static std::wstring IniPath() {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::wstring dir = std::wstring(buf) + L"\\WVC";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\settings.ini";
    }
    return ExeDir() + L"\\settings.ini";
}

static int IniInt(const wchar_t* key, int def) {
    wchar_t buf[64];
    wchar_t defs[32];
    swprintf(defs, 32, L"%d", def);
    GetPrivateProfileStringW(L"WVC", key, defs, buf, 64, IniPath().c_str());
    return _wtoi(buf);
}

static void IniSetInt(const wchar_t* key, int v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(L"WVC", key, buf, IniPath().c_str());
}

static void LoadSettings() {
    Settings s;
    s.res = std::clamp(IniInt(L"Resolution", 1), 0, 1);
    s.fps = IniInt(L"FPS", 30) == 60 ? 60 : 30;
    s.cursor = IniInt(L"Cursor", 0) != 0;
    s.fit = std::clamp(IniInt(L"Fit", 0), 0, 1);
    s.hasRegion = IniInt(L"HasRegion", 0) != 0;
    s.region.left = IniInt(L"Left", 0);
    s.region.top = IniInt(L"Top", 0);
    s.region.right = IniInt(L"Right", 0);
    s.region.bottom = IniInt(L"Bottom", 0);
    if (s.region.right - s.region.left < 16 || s.region.bottom - s.region.top < 16) s.hasRegion = false;
    std::lock_guard<std::mutex> lk(g_setMx);
    g_set = s;
}

static void SaveSettings() {
    Settings s;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        s = g_set;
    }
    IniSetInt(L"Resolution", s.res);
    IniSetInt(L"FPS", s.fps);
    IniSetInt(L"Cursor", s.cursor ? 1 : 0);
    IniSetInt(L"Fit", s.fit);
    IniSetInt(L"HasRegion", s.hasRegion ? 1 : 0);
    IniSetInt(L"Left", s.region.left);
    IniSetInt(L"Top", s.region.top);
    IniSetInt(L"Right", s.region.right);
    IniSetInt(L"Bottom", s.region.bottom);
}

static Settings GetSettings() {
    std::lock_guard<std::mutex> lk(g_setMx);
    return g_set;
}

// ---------------------------------------------------------------------------
// Screen capture (GDI)
// ---------------------------------------------------------------------------
class Capturer {
public:
    Capturer(int w, int h) : W(w), H(h) {
        screenDC = GetDC(nullptr);
        memDC = CreateCompatibleDC(screenDC);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        if (dib) oldBmp = SelectObject(memDC, dib);
        SetStretchBltMode(memDC, HALFTONE);
        SetBrushOrgEx(memDC, 0, 0, nullptr);
    }
    ~Capturer() {
        if (oldBmp) SelectObject(memDC, oldBmp);
        if (dib) DeleteObject(dib);
        if (memDC) DeleteDC(memDC);
        if (screenDC) ReleaseDC(nullptr, screenDC);
    }
    bool ok() const { return dib && bits; }

    // out: W*H*3 bytes, BGR, top-down (what softcam expects)
    void Grab(const RECT& r, int fit, bool cursor, uint8_t* out) {
        if (!ok()) return;
        const int sw = r.right - r.left, sh = r.bottom - r.top;
        if (sw <= 0 || sh <= 0) return;
        std::memset(bits, 0, static_cast<size_t>(W) * H * 4);

        double scale;
        int dx = 0, dy = 0, dw = W, dh = H;
        double ssx = r.left, ssy = r.top, ssw = sw, ssh = sh;
        if (fit == 0) {  // letterbox
            scale = (std::min)(static_cast<double>(W) / sw, static_cast<double>(H) / sh);
            dw = static_cast<int>(std::lround(sw * scale));
            dh = static_cast<int>(std::lround(sh * scale));
            dw = (std::min)(dw, W);
            dh = (std::min)(dh, H);
            dx = (W - dw) / 2;
            dy = (H - dh) / 2;
        } else {  // crop
            scale = (std::max)(static_cast<double>(W) / sw, static_cast<double>(H) / sh);
            ssw = W / scale;
            ssh = H / scale;
            ssx = r.left + (sw - ssw) / 2.0;
            ssy = r.top + (sh - ssh) / 2.0;
        }
        const int isx = static_cast<int>(std::lround(ssx));
        const int isy = static_cast<int>(std::lround(ssy));
        const int isw = (std::max)(1, static_cast<int>(std::lround(ssw)));
        const int ish = (std::max)(1, static_cast<int>(std::lround(ssh)));

        if (dw == isw && dh == ish)
            BitBlt(memDC, dx, dy, dw, dh, screenDC, isx, isy, SRCCOPY);
        else
            StretchBlt(memDC, dx, dy, dw, dh, screenDC, isx, isy, isw, ish, SRCCOPY);

        if (cursor) DrawCursor(isx, isy, scale, dx, dy, dw, dh);
        GdiFlush();

        const size_t n = static_cast<size_t>(W) * H;
        const uint8_t* src = bits;
        uint8_t* dst = out;
        for (size_t i = 0; i < n; ++i) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst += 3;
            src += 4;
        }
    }

private:
    void DrawCursor(int isx, int isy, double scale, int dx, int dy, int dw, int dh) {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
        ICONINFO ii{};
        if (!GetIconInfo(ci.hCursor, &ii)) return;
        int cw = GetSystemMetrics(SM_CXCURSOR), ch = GetSystemMetrics(SM_CYCURSOR);
        BITMAP bm{};
        if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm)) {
            cw = bm.bmWidth;
            ch = bm.bmHeight;
        } else if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(bm), &bm)) {
            cw = bm.bmWidth;
            ch = bm.bmHeight / 2;
        }
        const int x = static_cast<int>(std::lround((ci.ptScreenPos.x - static_cast<int>(ii.xHotspot) - isx) * scale)) + dx;
        const int y = static_cast<int>(std::lround((ci.ptScreenPos.y - static_cast<int>(ii.yHotspot) - isy) * scale)) + dy;
        const int sw = (std::max)(1, static_cast<int>(std::lround(cw * scale)));
        const int sh = (std::max)(1, static_cast<int>(std::lround(ch * scale)));
        int saved = SaveDC(memDC);
        IntersectClipRect(memDC, dx, dy, dx + dw, dy + dh);
        DrawIconEx(memDC, x, y, ci.hCursor, sw, sh, 0, nullptr, DI_NORMAL);
        RestoreDC(memDC, saved);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }

    int W, H;
    HDC screenDC = nullptr;
    HDC memDC = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBmp = nullptr;
    uint8_t* bits = nullptr;
};

// ---------------------------------------------------------------------------
// Camera + sender thread
// ---------------------------------------------------------------------------
static scCamera          g_cam = nullptr;
static std::thread       g_sender;
static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_streaming{false};

static void SenderLoop(int W, int H) {
    std::vector<uint8_t> frame(static_cast<size_t>(W) * H * 3, 0);
    Capturer cap(W, H);
    bool dirty = false;
    while (!g_quit) {
        if (g_streaming) {
            Settings s = GetSettings();
            if (s.hasRegion) {
                cap.Grab(s.region, s.fit, s.cursor, frame.data());
                dirty = true;
            }
        } else if (dirty) {
            std::fill(frame.begin(), frame.end(), static_cast<uint8_t>(0));
            dirty = false;
        }
        pSend(g_cam, frame.data());  // paces itself to the camera FPS
    }
}

static bool StartCamera() {
    if (!pCreate || g_cam) return g_cam != nullptr;
    Settings s = GetSettings();
    const int W = kResW[s.res], H = kResH[s.res];
    g_cam = pCreate(W, H, static_cast<float>(s.fps));
    if (!g_cam) return false;
    g_quit = false;
    g_sender = std::thread(SenderLoop, W, H);
    return true;
}

static void StopCamera() {
    if (g_sender.joinable()) {
        g_quit = true;
        g_sender.join();
    }
    if (g_cam) {
        pDelete(g_cam);
        g_cam = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Install / uninstall the camera (elevated helper mode)
// ---------------------------------------------------------------------------
static int RunWait(const std::wstring& cmdLine) {
    std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// exit codes: 0 ok, 2 = 64-bit ok but 32-bit failed, 10+ = 64-bit failed
static int DoRegister(bool reg) {
    const std::wstring dir = ExeDir();
    const std::wstring dll64 = dir + L"\\" + kDll64;
    const std::wstring dll32 = dir + L"\\" + kDll32;

    HMODULE h = LoadLibraryW(dll64.c_str());
    if (!h) return 10;
    auto fn = reinterpret_cast<PFN_DllRegister>(GetProcAddress(h, reg ? "DllRegisterServer" : "DllUnregisterServer"));
    if (!fn) {
        FreeLibrary(h);
        return 11;
    }
    HRESULT hr = fn();
    FreeLibrary(h);
    if (FAILED(hr) && reg) return 12;

    int result = 0;
    if (GetFileAttributesW(dll32.c_str()) != INVALID_FILE_ATTRIBUTES) {
        wchar_t wow[MAX_PATH] = {};
        if (GetSystemWow64DirectoryW(wow, MAX_PATH)) {
            std::wstring cmd = L"\"" + std::wstring(wow) + L"\\regsvr32.exe\" /s " + (reg ? L"" : L"/u ") + L"\"" + dll32 + L"\"";
            if (RunWait(cmd) != 0 && reg) result = 2;
        }
    }
    return result;
}

static int RunElevated(const wchar_t* args) {
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        return GetLastError() == ERROR_CANCELLED ? -2 : -1;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return static_cast<int>(code);
}

// 0 = not installed, 1 = installed from this folder, 2 = installed from another folder
static int CameraInstallState() {
    std::wstring key = std::wstring(L"CLSID\\") + kCamClsid + L"\\InprocServer32";
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS) return 0;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    LONG rc = RegQueryValueExW(hk, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS) return 0;
    std::wstring mine = ExeDir() + L"\\" + kDll64;
    return _wcsicmp(buf, mine.c_str()) == 0 ? 1 : 2;
}

// ---------------------------------------------------------------------------
// Region selection overlay
// ---------------------------------------------------------------------------
static const COLORREF kKey = RGB(255, 0, 255);

struct SelState {
    bool done = false;
    bool ok = false;
    bool dragging = false;
    POINT a{}, b{};
    RECT result{};
    int vx = 0, vy = 0;
    HFONT font = nullptr;
};
static SelState* g_sel = nullptr;

static RECT NormRect(POINT a, POINT b) {
    RECT r;
    r.left = (std::min)(a.x, b.x);
    r.top = (std::min)(a.y, b.y);
    r.right = (std::max)(a.x, b.x);
    r.bottom = (std::max)(a.y, b.y);
    return r;
}

static LRESULT CALLBACK SelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SelState* st = g_sel;
    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        return TRUE;
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
    case WM_ERASEBKGND:
        return 1;
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

        // hint on the primary monitor
        POINT zero{0, 0};
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY), &mi);
        RECT hint = mi.rcMonitor;
        if (st) OffsetRect(&hint, -st->vx, -st->vy);
        hint.top += 40;
        hint.bottom = hint.top + 60;
        DrawTextW(m, L"Выделите мышью область, которую покажет камера.   Esc или правая кнопка — отмена", -1, &hint,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        if (st && (st->dragging)) {
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

static bool SelectRegion(HINSTANCE hInst, RECT* out) {
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
    st.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    st.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    st.font = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_sel = &st;

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"WVCSelect", L"WVC",
                                WS_POPUP, st.vx, st.vy, vw, vh, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        g_sel = nullptr;
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
    g_sel = nullptr;
    DeleteObject(st.font);
    if (st.ok) *out = st.result;
    return st.ok;
}

// ---------------------------------------------------------------------------
// Red frame around the streamed region (click-through, outside the region)
// ---------------------------------------------------------------------------
static HWND g_frame = nullptr;
static const int kFrame = 3;

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH red = CreateSolidBrush(RGB(230, 40, 40));
        FillRect(dc, &rc, red);
        DeleteObject(red);
        InflateRect(&rc, -kFrame, -kFrame);
        HBRUSH key = CreateSolidBrush(kKey);
        FillRect(dc, &rc, key);
        DeleteObject(key);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void UpdateFrame() {
    if (!g_frame) return;
    Settings s = GetSettings();
    if (g_streaming && s.hasRegion) {
        RECT r = s.region;
        InflateRect(&r, kFrame, kFrame);
        SetWindowPos(g_frame, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(g_frame, nullptr, TRUE);
    } else {
        ShowWindow(g_frame, SW_HIDE);
    }
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------
enum {
    IDC_REGION_LABEL = 100,
    IDC_REGION_VALUE,
    IDC_SELECT,
    IDC_FULL,
    IDC_RES_LABEL,
    IDC_RES,
    IDC_FPS_LABEL,
    IDC_FPS,
    IDC_FIT_LABEL,
    IDC_FIT,
    IDC_CURSOR,
    IDC_START,
    IDC_STATUS,
    IDC_INSTALL,
    IDC_UNINSTALL,
};
enum { ID_TRAY_OPEN = 200, ID_TRAY_TOGGLE, ID_TRAY_EXIT };

static const UINT WM_TRAY = WM_APP + 1;
static const UINT_PTR kTimer = 1;

static HINSTANCE g_hInst;
static HWND g_main;
static HFONT g_font = nullptr;
static UINT g_dpi = 96;
static NOTIFYICONDATAW g_nid{};
static std::wstring g_note;  // one-off note in the status line

static int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }
static HWND Ctl(int id) { return GetDlgItem(g_main, id); }

static void MakeFont() {
    if (g_font) DeleteObject(g_font);
    g_font = CreateFontW(-MulDiv(9, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                         L"Segoe UI");
}

static const int kClientW = 380, kClientH = 352;

static void Layout() {
    struct L { int id, x, y, w, h; };
    const L items[] = {
        {IDC_REGION_LABEL, 12, 14, 150, 20},
        {IDC_REGION_VALUE, 165, 14, 203, 20},
        {IDC_SELECT, 12, 40, 175, 28},
        {IDC_FULL, 193, 40, 175, 28},
        {IDC_RES_LABEL, 12, 84, 150, 20},
        {IDC_RES, 165, 80, 203, 200},
        {IDC_FPS_LABEL, 12, 116, 150, 20},
        {IDC_FPS, 165, 112, 203, 200},
        {IDC_FIT_LABEL, 12, 148, 150, 20},
        {IDC_FIT, 165, 144, 203, 200},
        {IDC_CURSOR, 12, 178, 300, 22},
        {IDC_START, 12, 210, 356, 40},
        {IDC_STATUS, 12, 258, 356, 48},
        {IDC_INSTALL, 12, 312, 175, 28},
        {IDC_UNINSTALL, 193, 312, 175, 28},
    };
    for (const auto& it : items) {
        HWND h = Ctl(it.id);
        if (!h) continue;
        SetWindowPos(h, nullptr, S(it.x), S(it.y), S(it.w), S(it.h), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
}

static void UpdateRegionLabel() {
    Settings s = GetSettings();
    wchar_t buf[128];
    if (s.hasRegion)
        swprintf(buf, 128, L"%d × %d  (x %d, y %d)", static_cast<int>(s.region.right - s.region.left),
                 static_cast<int>(s.region.bottom - s.region.top), static_cast<int>(s.region.left),
                 static_cast<int>(s.region.top));
    else
        wcscpy(buf, L"не выбрана");
    SetWindowTextW(Ctl(IDC_REGION_VALUE), buf);
}

static void UpdateStartButton() {
    SetWindowTextW(Ctl(IDC_START), g_streaming ? L"■  Стоп" : L"▶  Старт");
    const wchar_t* tip = g_streaming ? L"WVC — трансляция идёт" : L"WVC — пауза";
    wcsncpy(g_nid.szTip, tip, ARRAYSIZE(g_nid.szTip) - 1);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void UpdateStatus() {
    std::wstring line1;
    if (!g_camDll) {
        line1 = L"Не найден WVCCam.dll рядом с программой.";
    } else {
        int st = CameraInstallState();
        if (st == 0)
            line1 = L"Камера не установлена: нажмите «Установить камеру» (один раз, нужны права администратора).";
        else if (st == 2)
            line1 = L"Камера установлена из другой папки: нажмите «Установить камеру» ещё раз.";
        else if (!g_cam)
            line1 = L"Не удалось запустить камеру.";
        else if (pIsConnected && pIsConnected(g_cam))
            line1 = L"«WVC Camera» сейчас используется приложением.";
        else
            line1 = L"«WVC Camera» готова: выберите её в Zoom, Telegram, Discord или браузере.";
    }
    std::wstring line2 = g_streaming ? L"Трансляция идёт." : L"Пауза: камера показывает чёрный экран.";
    if (!g_note.empty()) line2 = g_note;
    std::wstring text = line1 + L"\r\n" + line2;
    wchar_t cur[512] = {};
    GetWindowTextW(Ctl(IDC_STATUS), cur, 512);
    if (text != cur) SetWindowTextW(Ctl(IDC_STATUS), text.c_str());
}

static void ToggleStreaming() {
    if (!g_streaming) {
        Settings s = GetSettings();
        if (!s.hasRegion) {
            MessageBoxW(g_main, L"Сначала выберите область экрана.", L"WVC", MB_ICONINFORMATION);
            return;
        }
        if (!g_cam && !StartCamera()) {
            MessageBoxW(g_main, L"Не удалось запустить камеру.", L"WVC", MB_ICONERROR);
            return;
        }
    }
    g_streaming = !g_streaming;
    g_note.clear();
    UpdateStartButton();
    UpdateFrame();
    UpdateStatus();
}

static void RestartCameraForFormat() {
    StopCamera();
    if (!StartCamera()) {
        MessageBoxW(g_main, L"Не удалось перезапустить камеру.", L"WVC", MB_ICONERROR);
    }
    g_note = L"Формат изменён: переподключите камеру в Zoom/Telegram (выключите и включите видео).";
    UpdateStatus();
}

static void DoInstall(bool install) {
    if (!install) {
        if (MessageBoxW(g_main, L"Удалить камеру «WVC Camera» из системы?", L"WVC", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    int rc = RunElevated(install ? L"--register" : L"--unregister");
    if (rc == -2) return;  // UAC cancelled
    if (install) {
        if (rc == 0)
            MessageBoxW(g_main,
                        L"Камера установлена. Если Zoom, Telegram или браузер уже открыты, перезапустите их, чтобы "
                        L"они увидели «WVC Camera».",
                        L"WVC", MB_ICONINFORMATION);
        else if (rc == 2)
            MessageBoxW(g_main, L"Камера установлена для 64-битных программ, а 32-битную часть установить не удалось.",
                        L"WVC", MB_ICONWARNING);
        else {
            wchar_t buf[128];
            swprintf(buf, 128, L"Не удалось установить камеру (код %d).", rc);
            MessageBoxW(g_main, buf, L"WVC", MB_ICONERROR);
        }
    } else {
        if (rc == 0)
            MessageBoxW(g_main, L"Камера удалена.", L"WVC", MB_ICONINFORMATION);
        else {
            wchar_t buf[128];
            swprintf(buf, 128, L"Не удалось удалить камеру (код %d).", rc);
            MessageBoxW(g_main, buf, L"WVC", MB_ICONERROR);
        }
    }
    UpdateStatus();
}

static void ShowMain() {
    ShowWindow(g_main, SW_SHOW);
    ShowWindow(g_main, SW_RESTORE);
    SetForegroundWindow(g_main);
}

static void CreateControls(HWND hwnd) {
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    };
    mk(L"STATIC", L"Область экрана:", 0, IDC_REGION_LABEL);
    mk(L"STATIC", L"", SS_ENDELLIPSIS, IDC_REGION_VALUE);
    mk(L"BUTTON", L"Выбрать область…", WS_TABSTOP | BS_PUSHBUTTON, IDC_SELECT);
    mk(L"BUTTON", L"Весь основной экран", WS_TABSTOP | BS_PUSHBUTTON, IDC_FULL);
    mk(L"STATIC", L"Разрешение камеры:", 0, IDC_RES_LABEL);
    HWND res = mk(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_RES);
    mk(L"STATIC", L"Частота кадров:", 0, IDC_FPS_LABEL);
    HWND fps = mk(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_FPS);
    mk(L"STATIC", L"Другие пропорции:", 0, IDC_FIT_LABEL);
    HWND fit = mk(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_FIT);
    HWND cur = mk(L"BUTTON", L"Показывать курсор мыши", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_CURSOR);
    mk(L"BUTTON", L"▶  Старт", WS_TABSTOP | BS_PUSHBUTTON, IDC_START);
    mk(L"STATIC", L"", 0, IDC_STATUS);
    mk(L"BUTTON", L"Установить камеру", WS_TABSTOP | BS_PUSHBUTTON, IDC_INSTALL);
    mk(L"BUTTON", L"Удалить камеру", WS_TABSTOP | BS_PUSHBUTTON, IDC_UNINSTALL);

    SendMessageW(res, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1280 × 720"));
    SendMessageW(res, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1920 × 1080"));
    SendMessageW(fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"30 кадров/с"));
    SendMessageW(fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"60 кадров/с"));
    SendMessageW(fit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Вписать с полями"));
    SendMessageW(fit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Обрезать по краям"));

    Settings s = GetSettings();
    SendMessageW(res, CB_SETCURSEL, s.res, 0);
    SendMessageW(fps, CB_SETCURSEL, s.fps == 60 ? 1 : 0, 0);
    SendMessageW(fit, CB_SETCURSEL, s.fit, 0);
    SendMessageW(cur, BM_SETCHECK, s.cursor ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void ResizeForDpi(HWND hwnd) {
    RECT r{0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), g_dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        MakeFont();
        CreateControls(hwnd);
        Layout();
        ResizeForDpi(hwnd);
        UpdateRegionLabel();

        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAY;
        g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wcscpy(g_nid.szTip, L"WVC — пауза");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        SetTimer(hwnd, kTimer, 1000, nullptr);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        MakeFont();
        Layout();
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimer) UpdateStatus();
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) ShowWindow(hwnd, SW_HIDE);  // live in the tray
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowMain();
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Открыть WVC");
            AppendMenuW(m, MF_STRING, ID_TRAY_TOGGLE, g_streaming ? L"Стоп" : L"Старт");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Выход");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
        }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case ID_TRAY_OPEN:
            ShowMain();
            break;
        case ID_TRAY_TOGGLE:
        case IDC_START:
            ToggleStreaming();
            break;
        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            break;
        case IDC_SELECT: {
            bool wasVisible = IsWindowVisible(hwnd) != FALSE;
            ShowWindow(hwnd, SW_HIDE);
            if (g_frame) ShowWindow(g_frame, SW_HIDE);
            Sleep(150);  // let the window disappear before the overlay appears
            RECT r;
            if (SelectRegion(g_hInst, &r)) {
                {
                    std::lock_guard<std::mutex> lk(g_setMx);
                    g_set.region = r;
                    g_set.hasRegion = true;
                }
                SaveSettings();
                UpdateRegionLabel();
            }
            if (wasVisible) ShowMain();
            UpdateFrame();
            break;
        }
        case IDC_FULL: {
            POINT zero{0, 0};
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY), &mi);
            {
                std::lock_guard<std::mutex> lk(g_setMx);
                g_set.region = mi.rcMonitor;
                g_set.hasRegion = true;
            }
            SaveSettings();
            UpdateRegionLabel();
            UpdateFrame();
            break;
        }
        case IDC_RES:
        case IDC_FPS:
            if (code == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0));
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lk(g_setMx);
                    if (id == IDC_RES && sel != g_set.res && sel >= 0) {
                        g_set.res = sel;
                        changed = true;
                    }
                    int f = sel == 1 ? 60 : 30;
                    if (id == IDC_FPS && f != g_set.fps) {
                        g_set.fps = f;
                        changed = true;
                    }
                }
                if (changed) {
                    SaveSettings();
                    RestartCameraForFormat();
                }
            }
            break;
        case IDC_FIT:
            if (code == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0));
                {
                    std::lock_guard<std::mutex> lk(g_setMx);
                    g_set.fit = sel == 1 ? 1 : 0;
                }
                SaveSettings();
            }
            break;
        case IDC_CURSOR:
            if (code == BN_CLICKED) {
                bool on = SendMessageW(Ctl(IDC_CURSOR), BM_GETCHECK, 0, 0) == BST_CHECKED;
                {
                    std::lock_guard<std::mutex> lk(g_setMx);
                    g_set.cursor = on;
                }
                SaveSettings();
            }
            break;
        case IDC_INSTALL:
            DoInstall(true);
            break;
        case IDC_UNINSTALL:
            DoInstall(false);
            break;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimer);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int nCmdShow) {
    if (cmdLine && wcsstr(cmdLine, L"--register")) return DoRegister(true);
    if (cmdLine && wcsstr(cmdLine, L"--unregister")) return DoRegister(false);

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
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    LoadSettings();

    std::wstring dll = ExeDir() + L"\\" + kDll64;
    g_camDll = LoadLibraryW(dll.c_str());
    if (g_camDll) {
        pCreate = reinterpret_cast<PFN_scCreateCamera>(GetProcAddress(g_camDll, "scCreateCamera"));
        pDelete = reinterpret_cast<PFN_scDeleteCamera>(GetProcAddress(g_camDll, "scDeleteCamera"));
        pSend = reinterpret_cast<PFN_scSendFrame>(GetProcAddress(g_camDll, "scSendFrame"));
        pIsConnected = reinterpret_cast<PFN_scIsConnected>(GetProcAddress(g_camDll, "scIsConnected"));
        if (!pCreate || !pDelete || !pSend) {
            FreeLibrary(g_camDll);
            g_camDll = nullptr;
        }
    }
    if (g_camDll) StartCamera();  // the camera exists while WVC runs; Start/Stop only toggles the picture

    WNDCLASSW fc{};
    fc.lpfnWndProc = FrameProc;
    fc.hInstance = hInst;
    fc.lpszClassName = L"WVCFrame";
    RegisterClassW(&fc);
    g_frame = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              L"WVCFrame", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    if (g_frame) SetLayeredWindowAttributes(g_frame, kKey, 255, LWA_COLORKEY);

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WVCMain";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"WVCMain", L"WVC — камера из области экрана",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                400, 400, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    UpdateStatus();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_streaming = false;
    StopCamera();
    if (g_frame) DestroyWindow(g_frame);
    if (g_font) DeleteObject(g_font);
    if (single) CloseHandle(single);
    return 0;
}
