// WVC — лёгкая виртуальная камера, которая показывает выбранную область экрана.
// Камера: DirectShow-фильтр на базе tshino/softcam (MIT) в WVCCam.dll / WVCCam32.dll.
// Это приложение: источник (область / монитор / окно), захват GDI, поворот/зеркало/зум,
// предпросмотр, картинка на паузе, горячие клавиши, автовосстановление.

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
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
    int  quality = 1;      // 0 = 720p, 1 = 1080p
    int  aspect = 0;       // 0 = 16:9, 1 = 4:3, 2 = 1:1, 3 = 9:16
    int  fps = 30;         // 30 or 60
    bool cursor = false;
    int  fit = 0;          // 0 = letterbox, 1 = crop
    int  rotation = 0;     // 0, 1, 2, 3 = 0/90/180/270 degrees clockwise
    bool mirror = false;
    int  zoom = 0;         // index into kZoom
    int  pauseMode = 2;    // 0 = freeze, 1 = black, 2 = text card, 3 = image
    std::wstring pauseImage;
    int  srcMode = 0;      // 0 = region, 1 = window
    HWND srcWindow = nullptr;
    std::wstring srcTitle;
    bool hasRegion = false;
    RECT region{};         // virtual-screen coordinates, physical pixels
};

static const double kZoom[] = {1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
static const int kZoomCount = 6;

static void OutSize(int quality, int aspect, int* w, int* h) {
    static const int sz[2][4][2] = {
        {{1280, 720}, {960, 720}, {720, 720}, {720, 1280}},
        {{1920, 1080}, {1440, 1080}, {1080, 1080}, {1080, 1920}},
    };
    *w = sz[quality][aspect][0];
    *h = sz[quality][aspect][1];
}

static Settings   g_set;
static std::mutex g_setMx;

static Settings GetSettings() {
    std::lock_guard<std::mutex> lk(g_setMx);
    return g_set;
}

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

static std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

static std::wstring IniPath() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::wstring dir = std::wstring(buf) + L"\\WVC";
        CreateDirectoryW(dir.c_str(), nullptr);
        cached = dir + L"\\settings.ini";
    } else {
        cached = ExeDir() + L"\\settings.ini";
    }
    return cached;
}

// The *PrivateProfile*W functions keep Unicode only if the file is UTF-16LE with a BOM.
static void EnsureUnicodeIni() {
    const std::wstring path = IniPath();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string bytes;
    if (f != INVALID_HANDLE_VALUE) {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) bytes.append(buf, got);
        CloseHandle(f);
        if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
            static_cast<unsigned char>(bytes[1]) == 0xFE)
            return;  // already UTF-16
    }
    std::wstring text;
    if (!bytes.empty()) {
        int n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        text.resize(n);
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), &text[0], n);
    }
    f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    const unsigned char bom[2] = {0xFF, 0xFE};
    WriteFile(f, bom, 2, &w, nullptr);
    if (!text.empty()) WriteFile(f, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(f);
}

static int IniInt(const wchar_t* key, int def) {
    wchar_t buf[64], defs[32];
    swprintf(defs, 32, L"%d", def);
    GetPrivateProfileStringW(L"WVC", key, defs, buf, 64, IniPath().c_str());
    return _wtoi(buf);
}

static void IniSetInt(const wchar_t* key, int v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(L"WVC", key, buf, IniPath().c_str());
}

static std::wstring IniStr(const wchar_t* key) {
    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"WVC", key, L"", buf, MAX_PATH * 2, IniPath().c_str());
    return buf;
}

static void LoadSettings() {
    EnsureUnicodeIni();
    Settings s;
    s.quality = std::clamp(IniInt(L"Resolution", 1), 0, 1);
    s.aspect = std::clamp(IniInt(L"Aspect", 0), 0, 3);
    s.fps = IniInt(L"FPS", 30) == 60 ? 60 : 30;
    s.cursor = IniInt(L"Cursor", 0) != 0;
    s.fit = std::clamp(IniInt(L"Fit", 0), 0, 1);
    s.rotation = std::clamp(IniInt(L"Rotation", 0), 0, 3);
    s.mirror = IniInt(L"Mirror", 0) != 0;
    s.zoom = std::clamp(IniInt(L"Zoom", 0), 0, kZoomCount - 1);
    s.pauseMode = std::clamp(IniInt(L"PauseMode", 2), 0, 3);
    s.pauseImage = IniStr(L"PauseImage");
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
    Settings s = GetSettings();
    IniSetInt(L"Resolution", s.quality);
    IniSetInt(L"Aspect", s.aspect);
    IniSetInt(L"FPS", s.fps);
    IniSetInt(L"Cursor", s.cursor ? 1 : 0);
    IniSetInt(L"Fit", s.fit);
    IniSetInt(L"Rotation", s.rotation);
    IniSetInt(L"Mirror", s.mirror ? 1 : 0);
    IniSetInt(L"Zoom", s.zoom);
    IniSetInt(L"PauseMode", s.pauseMode);
    WritePrivateProfileStringW(L"WVC", L"PauseImage", s.pauseImage.c_str(), IniPath().c_str());
    IniSetInt(L"HasRegion", s.hasRegion ? 1 : 0);
    IniSetInt(L"Left", s.region.left);
    IniSetInt(L"Top", s.region.top);
    IniSetInt(L"Right", s.region.right);
    IniSetInt(L"Bottom", s.region.bottom);
}

// ---------------------------------------------------------------------------
// Monitors and windows
// ---------------------------------------------------------------------------
struct MonitorItem {
    RECT rc;
    bool primary;
};

static BOOL CALLBACK EnumMonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    auto* v = reinterpret_cast<std::vector<MonitorItem>*>(lp);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hm, &mi)) v->push_back({mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

static std::vector<MonitorItem> Monitors() {
    std::vector<MonitorItem> v;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&v));
    std::sort(v.begin(), v.end(), [](const MonitorItem& a, const MonitorItem& b) {
        return a.rc.left != b.rc.left ? a.rc.left < b.rc.left : a.rc.top < b.rc.top;
    });
    return v;
}

static RECT PrimaryMonitorRect() {
    POINT zero{0, 0};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

static RECT VirtualScreen() {
    RECT r;
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

static bool WindowRectOf(HWND hwnd, RECT* r) {
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof(*r)))) {
        if (!GetWindowRect(hwnd, r)) return false;
    }
    return r->right - r->left >= 16 && r->bottom - r->top >= 16;
}

struct WindowItem {
    HWND hwnd;
    std::wstring title;
};

static BOOL CALLBACK EnumWinProc(HWND hwnd, LPARAM lp) {
    auto* v = reinterpret_cast<std::vector<WindowItem>*>(lp);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    wchar_t title[256] = {};
    if (GetWindowTextW(hwnd, title, 256) <= 0) return TRUE;
    RECT r;
    if (!WindowRectOf(hwnd, &r)) return TRUE;
    std::wstring t = title;
    if (t.size() > 60) t = t.substr(0, 57) + L"…";
    v->push_back({hwnd, t});
    return v->size() < 40;
}

// Source rectangle for this frame. false = nothing to capture right now.
static bool ResolveSource(const Settings& s, RECT* r) {
    if (s.srcMode == 1) return WindowRectOf(s.srcWindow, r);
    if (!s.hasRegion) return false;
    *r = s.region;
    return true;
}

// ---------------------------------------------------------------------------
// Screen capture (GDI) + transform
// ---------------------------------------------------------------------------
class Capturer {
public:
    Capturer(int w, int h) : W(w), H(h) {
        screenDC = GetDC(nullptr);
        memDC = CreateCompatibleDC(screenDC);
    }
    ~Capturer() {
        FreeCanvas();
        if (memDC) DeleteDC(memDC);
        if (screenDC) ReleaseDC(nullptr, screenDC);
    }

    // out: W*H*3 bytes, BGR, top-down (what softcam expects)
    bool Grab(const RECT& src, const Settings& s, uint8_t* out) {
        double sx = src.left, sy = src.top;
        double sw = src.right - src.left, sh = src.bottom - src.top;
        if (sw < 1 || sh < 1 || !screenDC || !memDC) return false;

        const double z = kZoom[std::clamp(s.zoom, 0, kZoomCount - 1)];
        if (z > 1.0) {
            const double nw = sw / z, nh = sh / z;
            sx += (sw - nw) / 2.0;
            sy += (sh - nh) / 2.0;
            sw = nw;
            sh = nh;
        }

        const bool swap = (s.rotation & 1) != 0;
        const int cw = swap ? H : W, ch = swap ? W : H;
        if (!EnsureCanvas(cw, ch)) return false;
        std::memset(bits, 0, static_cast<size_t>(CW) * CH * 4);

        double scale;
        int dx = 0, dy = 0, dw = CW, dh = CH;
        if (s.fit == 0) {  // letterbox
            scale = (std::min)(CW / sw, CH / sh);
            dw = (std::min)(CW, static_cast<int>(std::lround(sw * scale)));
            dh = (std::min)(CH, static_cast<int>(std::lround(sh * scale)));
            dx = (CW - dw) / 2;
            dy = (CH - dh) / 2;
        } else {  // crop
            scale = (std::max)(CW / sw, CH / sh);
            const double nw = CW / scale, nh = CH / scale;
            sx += (sw - nw) / 2.0;
            sy += (sh - nh) / 2.0;
            sw = nw;
            sh = nh;
        }
        const int isx = static_cast<int>(std::lround(sx));
        const int isy = static_cast<int>(std::lround(sy));
        const int isw = (std::max)(1, static_cast<int>(std::lround(sw)));
        const int ish = (std::max)(1, static_cast<int>(std::lround(sh)));

        BOOL ok;
        if (dw == isw && dh == ish)
            ok = BitBlt(memDC, dx, dy, dw, dh, screenDC, isx, isy, SRCCOPY);
        else
            ok = StretchBlt(memDC, dx, dy, dw, dh, screenDC, isx, isy, isw, ish, SRCCOPY);
        if (!ok) return false;

        if (s.cursor) DrawCursor(isx, isy, static_cast<double>(dw) / isw, dx, dy, dw, dh);
        GdiFlush();
        Convert(s.rotation, s.mirror, out);
        return true;
    }

    // Picture shown while paused (modes 1..3)
    void RenderPause(const Settings& s, uint8_t* out) {
        if (!EnsureCanvas(W, H)) return;
        std::memset(bits, 0, static_cast<size_t>(CW) * CH * 4);
        bool drawn = false;
        if (s.pauseMode == 3 && !s.pauseImage.empty()) drawn = DrawImageFile(s.pauseImage);
        if (s.pauseMode == 2 || (s.pauseMode == 3 && !drawn)) {
            RECT rc{0, 0, W, H};
            HBRUSH bg = CreateSolidBrush(RGB(30, 32, 38));
            FillRect(memDC, &rc, bg);
            DeleteObject(bg);
            const int px = (std::max)(24, (std::min)(W, H) / 9);
            HFONT f = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HGDIOBJ old = SelectObject(memDC, f);
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(240, 240, 240));
            const wchar_t* txt = (s.pauseMode == 3) ? L"Не удалось открыть картинку" : L"Скоро вернусь";
            DrawTextW(memDC, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(memDC, old);
            DeleteObject(f);
        }
        GdiFlush();
        Convert(0, false, out);
    }

private:
    bool EnsureCanvas(int cw, int ch) {
        if (dib && cw == CW && ch == CH) return true;
        FreeCanvas();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cw;
        bi.bmiHeader.biHeight = -ch;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        if (!dib || !bits) {
            dib = nullptr;
            bits = nullptr;
            return false;
        }
        oldBmp = SelectObject(memDC, dib);
        SetStretchBltMode(memDC, HALFTONE);
        SetBrushOrgEx(memDC, 0, 0, nullptr);
        CW = cw;
        CH = ch;
        return true;
    }

    void FreeCanvas() {
        if (oldBmp) SelectObject(memDC, oldBmp);
        if (dib) DeleteObject(dib);
        oldBmp = nullptr;
        dib = nullptr;
        bits = nullptr;
        CW = CH = 0;
    }

    // canvas (CW x CH, BGRA) -> out (W x H, BGR) with rotation (clockwise) and horizontal mirror
    void Convert(int rot, bool mirror, uint8_t* out) {
        for (int y = 0; y < H; ++y) {
            int cx = 0, cy = 0, ddx = 0, ddy = 0;
            switch (rot & 3) {
            case 0: cx = 0; cy = y; ddx = 1; ddy = 0; break;
            case 1: cx = y; cy = CH - 1; ddx = 0; ddy = -1; break;
            case 2: cx = CW - 1; cy = CH - 1 - y; ddx = -1; ddy = 0; break;
            case 3: cx = CW - 1 - y; cy = 0; ddx = 0; ddy = 1; break;
            }
            if (mirror) {
                cx += (W - 1) * ddx;
                cy += (W - 1) * ddy;
                ddx = -ddx;
                ddy = -ddy;
            }
            const ptrdiff_t step = static_cast<ptrdiff_t>(ddx) * 4 + static_cast<ptrdiff_t>(ddy) * CW * 4;
            const uint8_t* p = bits + (static_cast<ptrdiff_t>(cy) * CW + cx) * 4;
            uint8_t* o = out + static_cast<size_t>(y) * W * 3;
            for (int x = 0; x < W; ++x) {
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
                o += 3;
                p += step;
            }
        }
    }

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

    // Letterboxed image file into the canvas via WIC (needs COM on this thread)
    bool DrawImageFile(const std::wstring& path) {
        bool ok = false;
        IWICImagingFactory* f = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* fr = nullptr;
        IWICFormatConverter* cv = nullptr;
        IWICBitmapScaler* sc = nullptr;
        do {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&f))))
                break;
            if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnLoad, &dec)))
                break;
            if (FAILED(dec->GetFrame(0, &fr))) break;
            if (FAILED(f->CreateFormatConverter(&cv))) break;
            if (FAILED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                      WICBitmapPaletteTypeCustom)))
                break;
            UINT iw = 0, ih = 0;
            cv->GetSize(&iw, &ih);
            if (!iw || !ih) break;
            const double scale = (std::min)(static_cast<double>(CW) / iw, static_cast<double>(CH) / ih);
            const UINT dw = (std::max)(1u, (std::min)(static_cast<UINT>(CW), static_cast<UINT>(std::lround(iw * scale))));
            const UINT dh = (std::max)(1u, (std::min)(static_cast<UINT>(CH), static_cast<UINT>(std::lround(ih * scale))));
            if (FAILED(f->CreateBitmapScaler(&sc))) break;
            if (FAILED(sc->Initialize(cv, dw, dh, WICBitmapInterpolationModeFant))) break;
            const UINT dx = (CW - dw) / 2, dy = (CH - dh) / 2;
            const UINT stride = static_cast<UINT>(CW) * 4;
            BYTE* dst = bits + static_cast<size_t>(dy) * stride + static_cast<size_t>(dx) * 4;
            const UINT size = stride * (dh - 1) + dw * 4;
            if (FAILED(sc->CopyPixels(nullptr, stride, size, dst))) break;
            ok = true;
        } while (false);
        if (sc) sc->Release();
        if (cv) cv->Release();
        if (fr) fr->Release();
        if (dec) dec->Release();
        if (f) f->Release();
        return ok;
    }

    int W, H;
    int CW = 0, CH = 0;
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
static std::atomic<bool> g_resetCapture{false};   // recreate GDI objects (display change, resume)
static std::atomic<bool> g_pauseDirty{false};     // re-render pause picture
static std::atomic<bool> g_sourceLost{false};     // window closed / minimised
static std::atomic<int>  g_fpsX10{0};
static std::atomic<int>  g_recoveries{0};

static std::mutex           g_prevMx;
static std::vector<uint8_t> g_prev;
static int                  g_prevW = 0, g_prevH = 0;

static void SenderLoop(int W, int H) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> frame(static_cast<size_t>(W) * H * 3, 0);
    std::unique_ptr<Capturer> cap(new Capturer(W, H));
    std::wstring pauseKey;
    int fails = 0, frames = 0;
    ULONGLONG fpsT0 = GetTickCount64(), prevT = 0;

    while (!g_quit) {
        if (g_resetCapture.exchange(false)) {
            cap.reset(new Capturer(W, H));
            pauseKey.clear();
            fails = 0;
            ++g_recoveries;
        }
        Settings s = GetSettings();
        if (g_streaming) {
            pauseKey.clear();
            RECT src;
            if (ResolveSource(s, &src)) {
                g_sourceLost = false;
                if (cap->Grab(src, s, frame.data())) {
                    fails = 0;
                } else if (++fails >= 15) {  // ~0.5 s of failures: rebuild capture
                    g_resetCapture = true;
                }
            } else {
                g_sourceLost = true;  // keep the last frame
            }
        } else if (s.pauseMode != 0) {
            std::wstring key = std::to_wstring(s.pauseMode) + L"|" + s.pauseImage;
            if (key != pauseKey || g_pauseDirty.exchange(false)) {
                if (s.pauseMode == 1)
                    std::fill(frame.begin(), frame.end(), static_cast<uint8_t>(0));
                else
                    cap->RenderPause(s, frame.data());
                pauseKey = key;
            }
        }

        const ULONGLONG now = GetTickCount64();
        if (now - prevT >= 50) {
            std::lock_guard<std::mutex> lk(g_prevMx);
            g_prev = frame;
            g_prevW = W;
            g_prevH = H;
            prevT = now;
        }
        ++frames;
        if (now - fpsT0 >= 1000) {
            g_fpsX10 = static_cast<int>(frames * 10000 / (now - fpsT0));
            frames = 0;
            fpsT0 = now;
        }

        pSend(g_cam, frame.data());  // paces itself to the camera FPS
    }
    CoUninitialize();
}

static bool StartCamera() {
    if (!pCreate || g_cam) return g_cam != nullptr;
    Settings s = GetSettings();
    int W, H;
    OutSize(s.quality, s.aspect, &W, &H);
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
            std::wstring cmd = L"\"" + std::wstring(wow) + L"\\regsvr32.exe\" /s " + (reg ? L"" : L"/u ") + L"\"" +
                               dll32 + L"\"";
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
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return GetLastError() == ERROR_CANCELLED ? -2 : -1;
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
    const RECT vs = VirtualScreen();
    st.vx = vs.left;
    st.vy = vs.top;
    st.font = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_sel = &st;

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"WVCSelect", L"WVC", WS_POPUP,
                                vs.left, vs.top, vs.right - vs.left, vs.bottom - vs.top, nullptr, nullptr, hInst,
                                nullptr);
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
// Red frame around the streamed source, with a drag tab.
// Inside is colour-keyed (transparent, click-through). Tab = move, edges = resize.
// ---------------------------------------------------------------------------
static HWND  g_frame = nullptr;
static int   g_fb = 6;            // border thickness, px
static int   g_tabW = 150, g_tabH = 26;
static bool  g_tabBelow = false;  // tab under the region when there is no room above
static bool  g_frameMoving = false;
static HFONT g_frameFont = nullptr;
static const UINT WM_FRAME_DONE = WM_APP + 7;

static void FrameRects(int w, int h, RECT* border, RECT* hole, RECT* tab) {
    const int top = g_tabBelow ? 0 : g_tabH;
    const int bottom = g_tabBelow ? h - g_tabH : h;
    *border = RECT{0, top, w, bottom};
    *hole = RECT{g_fb, top + g_fb, w - g_fb, bottom - g_fb};
    const int tw = (std::min)(g_tabW, w);
    *tab = g_tabBelow ? RECT{0, h - g_tabH, tw, h} : RECT{0, 0, tw, g_tabH};
}

static void RegionFromFrame(HWND hwnd) {
    RECT wr;
    GetWindowRect(hwnd, &wr);
    std::lock_guard<std::mutex> lk(g_setMx);
    g_set.region.left = wr.left + g_fb;
    g_set.region.right = wr.right - g_fb;
    g_set.region.top = wr.top + g_fb + (g_tabBelow ? 0 : g_tabH);
    g_set.region.bottom = wr.bottom - g_fb - (g_tabBelow ? g_tabH : 0);
    g_set.hasRegion = true;
}

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, border, hole, tab;
        GetClientRect(hwnd, &rc);
        FrameRects(rc.right, rc.bottom, &border, &hole, &tab);
        HBRUSH key = CreateSolidBrush(kKey);
        HBRUSH red = CreateSolidBrush(RGB(230, 40, 40));
        FillRect(dc, &rc, key);
        FillRect(dc, &border, red);
        FillRect(dc, &hole, key);
        FillRect(dc, &tab, red);
        DeleteObject(red);
        DeleteObject(key);
        HGDIOBJ old = g_frameFont ? SelectObject(dc, g_frameFont) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, L"✥ перетащить", -1, &tab, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (old) SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCHITTEST: {
        // colour-keyed pixels never reach us: only the border and the tab do
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd, &pt);
        RECT rc, border, hole, tab;
        GetClientRect(hwnd, &rc);
        FrameRects(rc.right, rc.bottom, &border, &hole, &tab);
        if (PtInRect(&tab, pt)) return HTCAPTION;
        const int cs = g_tabH;  // corner grab size
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
    case WM_ENTERSIZEMOVE: {
        // dragging the frame always means "this exact area": a followed window becomes a fixed area
        RECT cur;
        Settings s = GetSettings();
        if (s.srcMode == 1 && ResolveSource(s, &cur)) {
            std::lock_guard<std::mutex> lk(g_setMx);
            g_set.srcMode = 0;
            g_set.region = cur;
            g_set.hasRegion = true;
        }
        g_frameMoving = true;
        return 0;
    }
    case WM_MOVE:
    case WM_SIZE:
        if (msg == WM_SIZE) InvalidateRect(hwnd, nullptr, FALSE);
        if (g_frameMoving) RegionFromFrame(hwnd);
        return 0;
    case WM_EXITSIZEMOVE:
        g_frameMoving = false;
        RegionFromFrame(hwnd);
        PostMessageW(hwnd, WM_FRAME_DONE, 0, 0);
        return 0;
    case WM_DPICHANGED:
        return 0;  // keep the size: it must match the region in physical pixels
    case WM_NCRBUTTONUP:
    case WM_NCLBUTTONDBLCLK:
        return 0;  // no system menu / maximise on the frame
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void UpdateFrame() {
    if (!g_frame || g_frameMoving) return;
    Settings s = GetSettings();
    RECT r;
    if (g_streaming && ResolveSource(s, &r)) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
        g_tabBelow = (r.top - g_fb - g_tabH) < mi.rcMonitor.top;
        RECT w{r.left - g_fb, r.top - g_fb, r.right + g_fb, r.bottom + g_fb};
        if (g_tabBelow)
            w.bottom += g_tabH;
        else
            w.top -= g_tabH;
        RECT cur;
        GetWindowRect(g_frame, &cur);
        if (!IsWindowVisible(g_frame) || !EqualRect(&cur, &w)) {
            SetWindowPos(g_frame, HWND_TOPMOST, w.left, w.top, w.right - w.left, w.bottom - w.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(g_frame, nullptr, TRUE);
        }
    } else {
        ShowWindow(g_frame, SW_HIDE);
    }
}

// ---------------------------------------------------------------------------
// Preview control
// ---------------------------------------------------------------------------
static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC m = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(m, bmp);
        HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(m, &rc, bg);
        DeleteObject(bg);
        {
            std::lock_guard<std::mutex> lk(g_prevMx);
            if (!g_prev.empty() && g_prevW > 0 && g_prevH > 0) {
                const double sc = (std::min)(static_cast<double>(rc.right) / g_prevW,
                                             static_cast<double>(rc.bottom) / g_prevH);
                const int dw = static_cast<int>(g_prevW * sc), dh = static_cast<int>(g_prevH * sc);
                const int dx = (rc.right - dw) / 2, dy = (rc.bottom - dh) / 2;
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = g_prevW;
                bi.bmiHeader.biHeight = -g_prevH;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 24;
                bi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(m, HALFTONE);
                SetBrushOrgEx(m, 0, 0, nullptr);
                StretchDIBits(m, dx, dy, dw, dh, 0, 0, g_prevW, g_prevH, g_prev.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            }
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, old);
        DeleteObject(bmp);
        DeleteDC(m);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------
enum {
    IDC_SRC_LABEL = 100,
    IDC_SRC_VALUE,
    IDC_SELECT,
    IDC_MONITOR,
    IDC_WINDOW,
    IDC_Q_LABEL,
    IDC_QUALITY,
    IDC_A_LABEL,
    IDC_ASPECT,
    IDC_FPS_LABEL,
    IDC_FPS,
    IDC_FIT_LABEL,
    IDC_FIT,
    IDC_ROT_LABEL,
    IDC_ROT,
    IDC_ZOOM_LABEL,
    IDC_ZOOM,
    IDC_MIRROR,
    IDC_CURSOR,
    IDC_START,
    IDC_STATUS,
    IDC_INSTALL,
    IDC_UNINSTALL,
    IDC_PREV_LABEL,
    IDC_FPS_VALUE,
    IDC_PREVIEW,
    IDC_PAUSE_LABEL,
    IDC_PAUSE,
    IDC_PAUSE_IMG,
    IDC_HOTKEYS,
};
enum { ID_TRAY_OPEN = 200, ID_TRAY_TOGGLE, ID_TRAY_EXIT };
enum { HK_TOGGLE = 1, HK_SELECT, HK_MONITOR, HK_CURSOR };

static const UINT WM_TRAY = WM_APP + 1;
static const UINT_PTR kTimerStatus = 1, kTimerPreview = 2, kTimerFollow = 3;

static HINSTANCE g_hInst;
static HWND g_main = nullptr;
static HWND g_preview = nullptr;
static HFONT g_font = nullptr;
static UINT g_dpi = 96;
static NOTIFYICONDATAW g_nid{};
static std::wstring g_note;  // one-off note in the status line
static std::wstring g_hotkeyNote;

static int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }
static HWND Ctl(int id) { return GetDlgItem(g_main, id); }

static void SetTextIfChanged(HWND h, const std::wstring& text) {
    wchar_t cur[1024] = {};
    GetWindowTextW(h, cur, 1024);
    if (text != cur) SetWindowTextW(h, text.c_str());
}

static void MakeFont() {
    if (g_font) DeleteObject(g_font);
    g_font = CreateFontW(-MulDiv(9, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                         L"Segoe UI");
}

static const int kClientW = 760, kClientH = 448;

static void Layout() {
    struct L { int id, x, y, w, h; };
    const L items[] = {
        // left column
        {IDC_SRC_LABEL, 12, 14, 90, 20},
        {IDC_SRC_VALUE, 102, 14, 266, 20},
        {IDC_SELECT, 12, 40, 116, 28},
        {IDC_MONITOR, 132, 40, 116, 28},
        {IDC_WINDOW, 252, 40, 116, 28},
        {IDC_Q_LABEL, 12, 84, 150, 20},
        {IDC_QUALITY, 165, 80, 203, 200},
        {IDC_A_LABEL, 12, 116, 150, 20},
        {IDC_ASPECT, 165, 112, 203, 200},
        {IDC_FPS_LABEL, 12, 148, 150, 20},
        {IDC_FPS, 165, 144, 203, 200},
        {IDC_FIT_LABEL, 12, 180, 150, 20},
        {IDC_FIT, 165, 176, 203, 200},
        {IDC_ROT_LABEL, 12, 212, 150, 20},
        {IDC_ROT, 165, 208, 203, 200},
        {IDC_ZOOM_LABEL, 12, 244, 150, 20},
        {IDC_ZOOM, 165, 240, 203, 200},
        {IDC_MIRROR, 12, 274, 170, 22},
        {IDC_CURSOR, 190, 274, 178, 22},
        {IDC_START, 12, 306, 356, 40},
        {IDC_STATUS, 12, 354, 356, 50},
        {IDC_INSTALL, 12, 408, 175, 28},
        {IDC_UNINSTALL, 193, 408, 175, 28},
        // right column
        {IDC_PREV_LABEL, 388, 14, 200, 20},
        {IDC_FPS_VALUE, 588, 14, 160, 20},
        {IDC_PREVIEW, 388, 40, 360, 203},
        {IDC_PAUSE_LABEL, 388, 256, 150, 20},
        {IDC_PAUSE, 545, 252, 203, 200},
        {IDC_PAUSE_IMG, 545, 284, 203, 28},
        {IDC_HOTKEYS, 388, 324, 360, 112},
    };
    for (const auto& it : items) {
        HWND h = Ctl(it.id);
        if (!h) continue;
        SetWindowPos(h, nullptr, S(it.x), S(it.y), S(it.w), S(it.h), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
}

static void UpdateSourceLabel() {
    if (!g_main) return;
    Settings s = GetSettings();
    wchar_t buf[256];
    if (s.srcMode == 1) {
        swprintf(buf, 256, L"окно «%ls»", s.srcTitle.c_str());
    } else if (s.hasRegion) {
        swprintf(buf, 256, L"область %d × %d  (x %d, y %d)", static_cast<int>(s.region.right - s.region.left),
                 static_cast<int>(s.region.bottom - s.region.top), static_cast<int>(s.region.left),
                 static_cast<int>(s.region.top));
    } else {
        wcscpy(buf, L"не выбран");
    }
    SetTextIfChanged(Ctl(IDC_SRC_VALUE), buf);
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
            line1 = L"Камера не запущена, пробую снова…";
        else if (pIsConnected && pIsConnected(g_cam))
            line1 = L"«WVC Camera» сейчас используется приложением.";
        else
            line1 = L"«WVC Camera» готова: выберите её в Zoom, Telegram, Discord или браузере.";
    }
    std::wstring line2;
    if (!g_note.empty())
        line2 = g_note;
    else if (g_streaming && g_sourceLost)
        line2 = L"Окно свёрнуто или закрыто: показываю последний кадр.";
    else if (g_streaming)
        line2 = L"Трансляция идёт.";
    else
        line2 = L"Пауза.";
    SetTextIfChanged(Ctl(IDC_STATUS), line1 + L"\r\n" + line2);

    wchar_t fps[64];
    swprintf(fps, 64, L"%.1f кадров/с", g_fpsX10 / 10.0);
    SetTextIfChanged(Ctl(IDC_FPS_VALUE), g_cam ? fps : L"");
    UpdateSourceLabel();
}

static bool HaveSource() {
    Settings s = GetSettings();
    return s.srcMode == 1 ? (s.srcWindow && IsWindow(s.srcWindow)) : s.hasRegion;
}

static void ToggleStreaming() {
    if (!g_streaming) {
        if (!HaveSource()) {
            MessageBoxW(g_main, L"Сначала выберите источник: область, монитор или окно.", L"WVC", MB_ICONINFORMATION);
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
    if (!StartCamera()) MessageBoxW(g_main, L"Не удалось перезапустить камеру.", L"WVC", MB_ICONERROR);
    g_note = L"Формат изменён: переподключите камеру в Zoom/Telegram (выключите и включите видео).";
    UpdateStatus();
}

static void SetRegionSource(const RECT& r) {
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.srcMode = 0;
        g_set.srcWindow = nullptr;
        g_set.region = r;
        g_set.hasRegion = true;
    }
    SaveSettings();
    UpdateSourceLabel();
    UpdateFrame();
}

static void DoSelectRegion() {
    bool wasVisible = IsWindowVisible(g_main) != FALSE;
    ShowWindow(g_main, SW_HIDE);
    if (g_frame) ShowWindow(g_frame, SW_HIDE);
    Sleep(150);  // let the windows disappear before the overlay appears
    RECT r;
    if (SelectRegion(g_hInst, &r)) SetRegionSource(r);
    if (wasVisible) {
        ShowWindow(g_main, SW_SHOW);
        SetForegroundWindow(g_main);
    }
    UpdateFrame();
}

static void NextMonitor() {
    auto mons = Monitors();
    if (mons.empty()) return;
    Settings s = GetSettings();
    RECT cur;
    int idx = -1;
    if (ResolveSource(s, &cur)) {
        POINT c{(cur.left + cur.right) / 2, (cur.top + cur.bottom) / 2};
        for (size_t i = 0; i < mons.size(); ++i)
            if (PtInRect(&mons[i].rc, c)) idx = static_cast<int>(i);
    }
    // already showing a whole monitor? go to the next one; otherwise take the monitor it is on
    if (idx >= 0 && s.srcMode == 0 && EqualRect(&s.region, &mons[idx].rc)) idx = (idx + 1) % static_cast<int>(mons.size());
    if (idx < 0) idx = 0;
    SetRegionSource(mons[idx].rc);
}

static void ShowMonitorMenu() {
    auto mons = Monitors();
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < mons.size(); ++i) {
        wchar_t buf[128];
        swprintf(buf, 128, L"Монитор %d — %d × %d%ls", static_cast<int>(i + 1),
                 static_cast<int>(mons[i].rc.right - mons[i].rc.left),
                 static_cast<int>(mons[i].rc.bottom - mons[i].rc.top), mons[i].primary ? L" (основной)" : L"");
        AppendMenuW(m, MF_STRING, 1000 + i, buf);
    }
    RECT br;
    GetWindowRect(Ctl(IDC_MONITOR), &br);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, br.left, br.bottom, 0, g_main, nullptr);
    DestroyMenu(m);
    if (cmd >= 1000 && cmd < 1000 + static_cast<int>(mons.size())) SetRegionSource(mons[cmd - 1000].rc);
}

static void ShowWindowMenu() {
    std::vector<WindowItem> wins;
    EnumWindows(EnumWinProc, reinterpret_cast<LPARAM>(&wins));
    HMENU m = CreatePopupMenu();
    if (wins.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Нет подходящих окон");
    for (size_t i = 0; i < wins.size(); ++i) AppendMenuW(m, MF_STRING, 2000 + i, wins[i].title.c_str());
    RECT br;
    GetWindowRect(Ctl(IDC_WINDOW), &br);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, br.left, br.bottom, 0, g_main, nullptr);
    DestroyMenu(m);
    if (cmd >= 2000 && cmd < 2000 + static_cast<int>(wins.size())) {
        {
            std::lock_guard<std::mutex> lk(g_setMx);
            g_set.srcMode = 1;
            g_set.srcWindow = wins[cmd - 2000].hwnd;
            g_set.srcTitle = wins[cmd - 2000].title;
        }
        UpdateSourceLabel();
        UpdateFrame();
    }
}

static void ChoosePauseImage() {
    wchar_t file[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"Картинки (PNG, JPG, BMP, GIF)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0Все файлы\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.pauseImage = file;
        g_set.pauseMode = 3;
    }
    SendMessageW(Ctl(IDC_PAUSE), CB_SETCURSEL, 3, 0);
    g_pauseDirty = true;
    SaveSettings();
}

// Display change / resume from sleep: rebuild capture and bring the source back on screen
static void RecoverDisplay() {
    g_resetCapture = true;
    Settings s = GetSettings();
    if (s.srcMode == 0 && s.hasRegion) {
        RECT vs = VirtualScreen(), inter;
        if (!IntersectRect(&inter, &vs, &s.region)) {
            RECT p = PrimaryMonitorRect();
            const int w = (std::min)(s.region.right - s.region.left, static_cast<LONG>(p.right - p.left));
            const int h = (std::min)(s.region.bottom - s.region.top, static_cast<LONG>(p.bottom - p.top));
            RECT r{p.left, p.top, p.left + w, p.top + h};
            SetRegionSource(r);
            g_note = L"Область оказалась за пределами экрана и перенесена на основной монитор.";
        }
    }
    UpdateFrame();
    UpdateStatus();
}

static void RegisterHotkeys(HWND hwnd) {
    struct HK { int id; UINT vk; const wchar_t* name; };
    const HK keys[] = {
        {HK_TOGGLE, VK_F5, L"Ctrl+Shift+F5"},
        {HK_SELECT, VK_F6, L"Ctrl+Shift+F6"},
        {HK_MONITOR, VK_F7, L"Ctrl+Shift+F7"},
        {HK_CURSOR, VK_F8, L"Ctrl+Shift+F8"},
    };
    std::wstring busy;
    for (const auto& k : keys) {
        if (!RegisterHotKey(hwnd, k.id, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, k.vk)) {
            if (!busy.empty()) busy += L", ";
            busy += k.name;
        }
    }
    if (!busy.empty()) g_hotkeyNote = L"\r\nЗаняты другой программой: " + busy;
}

static void SetCursorOption(bool on) {
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.cursor = on;
    }
    SendMessageW(Ctl(IDC_CURSOR), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    SaveSettings();
}

static void DoInstall(bool install) {
    if (!install &&
        MessageBoxW(g_main, L"Удалить камеру «WVC Camera» из системы?", L"WVC", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    int rc = RunElevated(install ? L"--register" : L"--unregister");
    if (rc == -2) return;  // UAC cancelled
    wchar_t buf[256];
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
            swprintf(buf, 256, L"Не удалось установить камеру (код %d).", rc);
            MessageBoxW(g_main, buf, L"WVC", MB_ICONERROR);
        }
    } else {
        if (rc == 0)
            MessageBoxW(g_main, L"Камера удалена.", L"WVC", MB_ICONINFORMATION);
        else {
            swprintf(buf, 256, L"Не удалось удалить камеру (код %d).", rc);
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

static HWND AddCombo(HWND hwnd, int id, std::initializer_list<const wchar_t*> items, int sel) {
    HWND h = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                             0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    for (auto* s : items) SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(h, CB_SETCURSEL, sel, 0);
    return h;
}

static void CreateControls(HWND hwnd) {
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    };
    Settings s = GetSettings();
    mk(L"STATIC", L"Источник:", 0, IDC_SRC_LABEL);
    mk(L"STATIC", L"", SS_ENDELLIPSIS, IDC_SRC_VALUE);
    mk(L"BUTTON", L"Область…", WS_TABSTOP | BS_PUSHBUTTON, IDC_SELECT);
    mk(L"BUTTON", L"Монитор ▾", WS_TABSTOP | BS_PUSHBUTTON, IDC_MONITOR);
    mk(L"BUTTON", L"Окно ▾", WS_TABSTOP | BS_PUSHBUTTON, IDC_WINDOW);

    mk(L"STATIC", L"Качество:", 0, IDC_Q_LABEL);
    AddCombo(hwnd, IDC_QUALITY, {L"720p", L"1080p"}, s.quality);
    mk(L"STATIC", L"Соотношение сторон:", 0, IDC_A_LABEL);
    AddCombo(hwnd, IDC_ASPECT, {L"16:9 (обычное)", L"4:3", L"1:1 (квадрат)", L"9:16 (вертикальное)"}, s.aspect);
    mk(L"STATIC", L"Частота кадров:", 0, IDC_FPS_LABEL);
    AddCombo(hwnd, IDC_FPS, {L"30 кадров/с", L"60 кадров/с"}, s.fps == 60 ? 1 : 0);
    mk(L"STATIC", L"Если пропорции другие:", 0, IDC_FIT_LABEL);
    AddCombo(hwnd, IDC_FIT, {L"Вписать с полями", L"Обрезать по краям"}, s.fit);
    mk(L"STATIC", L"Поворот:", 0, IDC_ROT_LABEL);
    AddCombo(hwnd, IDC_ROT, {L"Без поворота", L"90° по часовой", L"180°", L"90° против часовой"}, s.rotation);
    mk(L"STATIC", L"Увеличение (центр):", 0, IDC_ZOOM_LABEL);
    AddCombo(hwnd, IDC_ZOOM, {L"100%", L"125%", L"150%", L"200%", L"300%", L"400%"}, s.zoom);
    HWND mir = mk(L"BUTTON", L"Зеркально", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_MIRROR);
    HWND cur = mk(L"BUTTON", L"Показывать курсор", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_CURSOR);
    SendMessageW(mir, BM_SETCHECK, s.mirror ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(cur, BM_SETCHECK, s.cursor ? BST_CHECKED : BST_UNCHECKED, 0);

    mk(L"BUTTON", L"▶  Старт", WS_TABSTOP | BS_PUSHBUTTON, IDC_START);
    mk(L"STATIC", L"", 0, IDC_STATUS);
    mk(L"BUTTON", L"Установить камеру", WS_TABSTOP | BS_PUSHBUTTON, IDC_INSTALL);
    mk(L"BUTTON", L"Удалить камеру", WS_TABSTOP | BS_PUSHBUTTON, IDC_UNINSTALL);

    mk(L"STATIC", L"Предпросмотр (то, что видят другие):", 0, IDC_PREV_LABEL);
    mk(L"STATIC", L"", SS_RIGHT, IDC_FPS_VALUE);
    g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"WVCPreview", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PREVIEW)), g_hInst, nullptr);
    mk(L"STATIC", L"На паузе показывать:", 0, IDC_PAUSE_LABEL);
    AddCombo(hwnd, IDC_PAUSE, {L"Последний кадр", L"Чёрный экран", L"«Скоро вернусь»", L"Свою картинку"}, s.pauseMode);
    mk(L"BUTTON", L"Выбрать картинку…", WS_TABSTOP | BS_PUSHBUTTON, IDC_PAUSE_IMG);
    std::wstring hk = L"Горячие клавиши (работают везде):\r\n"
                      L"Ctrl+Shift+F5 — Старт / Стоп\r\n"
                      L"Ctrl+Shift+F6 — выбрать область\r\n"
                      L"Ctrl+Shift+F7 — следующий монитор\r\n"
                      L"Ctrl+Shift+F8 — курсор вкл/выкл";
    mk(L"STATIC", (hk + g_hotkeyNote).c_str(), 0, IDC_HOTKEYS);
}

static void ResizeForDpi(HWND hwnd) {
    RECT r{0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), g_dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static int ComboSel(int id) { return static_cast<int>(SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0)); }

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        RegisterHotkeys(hwnd);
        MakeFont();
        CreateControls(hwnd);
        Layout();
        ResizeForDpi(hwnd);
        UpdateSourceLabel();

        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAY;
        g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wcscpy(g_nid.szTip, L"WVC — пауза");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        SetTimer(hwnd, kTimerStatus, 1000, nullptr);
        SetTimer(hwnd, kTimerPreview, 66, nullptr);
        SetTimer(hwnd, kTimerFollow, 100, nullptr);
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
    case WM_DISPLAYCHANGE:
        RecoverDisplay();
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) RecoverDisplay();
        return TRUE;
    case WM_TIMER:
        if (wp == kTimerStatus) {
            if (g_camDll && !g_cam) StartCamera();  // auto-retry
            UpdateStatus();
        } else if (wp == kTimerPreview) {
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) InvalidateRect(g_preview, nullptr, FALSE);
        } else if (wp == kTimerFollow) {
            if (g_streaming && GetSettings().srcMode == 1) UpdateFrame();
        }
        return 0;
    case WM_HOTKEY:
        switch (wp) {
        case HK_TOGGLE: ToggleStreaming(); break;
        case HK_SELECT: DoSelectRegion(); break;
        case HK_MONITOR: NextMonitor(); break;
        case HK_CURSOR: SetCursorOption(!GetSettings().cursor); break;
        }
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
        case ID_TRAY_OPEN: ShowMain(); break;
        case ID_TRAY_TOGGLE:
        case IDC_START: ToggleStreaming(); break;
        case ID_TRAY_EXIT: DestroyWindow(hwnd); break;
        case IDC_SELECT: DoSelectRegion(); break;
        case IDC_MONITOR: ShowMonitorMenu(); break;
        case IDC_WINDOW: ShowWindowMenu(); break;
        case IDC_QUALITY:
        case IDC_ASPECT:
        case IDC_FPS:
            if (code == CBN_SELCHANGE) {
                const int sel = ComboSel(id);
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lk(g_setMx);
                    if (id == IDC_QUALITY && sel >= 0 && sel != g_set.quality) {
                        g_set.quality = sel;
                        changed = true;
                    }
                    if (id == IDC_ASPECT && sel >= 0 && sel != g_set.aspect) {
                        g_set.aspect = sel;
                        changed = true;
                    }
                    const int f = sel == 1 ? 60 : 30;
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
        case IDC_ROT:
        case IDC_ZOOM:
        case IDC_PAUSE:
            if (code == CBN_SELCHANGE) {
                const int sel = (std::max)(0, ComboSel(id));
                {
                    std::lock_guard<std::mutex> lk(g_setMx);
                    if (id == IDC_FIT) g_set.fit = sel == 1 ? 1 : 0;
                    if (id == IDC_ROT) g_set.rotation = sel & 3;
                    if (id == IDC_ZOOM) g_set.zoom = (std::min)(sel, kZoomCount - 1);
                    if (id == IDC_PAUSE) g_set.pauseMode = (std::min)(sel, 3);
                }
                if (id == IDC_PAUSE) {
                    g_pauseDirty = true;
                    if (sel == 3 && GetSettings().pauseImage.empty()) ChoosePauseImage();
                }
                SaveSettings();
            }
            break;
        case IDC_PAUSE_IMG: ChoosePauseImage(); break;
        case IDC_MIRROR:
            if (code == BN_CLICKED) {
                const bool on = SendMessageW(Ctl(IDC_MIRROR), BM_GETCHECK, 0, 0) == BST_CHECKED;
                {
                    std::lock_guard<std::mutex> lk(g_setMx);
                    g_set.mirror = on;
                }
                SaveSettings();
            }
            break;
        case IDC_CURSOR:
            if (code == BN_CLICKED) SetCursorOption(SendMessageW(Ctl(IDC_CURSOR), BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case IDC_INSTALL: DoInstall(true); break;
        case IDC_UNINSTALL: DoInstall(false); break;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        for (int i = HK_TOGGLE; i <= HK_CURSOR; ++i) UnregisterHotKey(hwnd, i);
        KillTimer(hwnd, kTimerStatus);
        KillTimer(hwnd, kTimerPreview);
        KillTimer(hwnd, kTimerFollow);
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // file dialog
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

    // frame window
    {
        const int sdpi = static_cast<int>(GetDpiForSystem());
        g_fb = MulDiv(6, sdpi, 96);
        g_tabW = MulDiv(150, sdpi, 96);
        g_tabH = MulDiv(26, sdpi, 96);
        g_frameFont = CreateFontW(-MulDiv(9, sdpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    WNDCLASSW fc{};
    fc.style = CS_HREDRAW | CS_VREDRAW;
    fc.lpfnWndProc = FrameProc;
    fc.hInstance = hInst;
    fc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    fc.lpszClassName = L"WVCFrame";
    RegisterClassW(&fc);
    g_frame = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WVCFrame", L"",
                              WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    if (g_frame) SetLayeredWindowAttributes(g_frame, kKey, 255, LWA_COLORKEY);

    WNDCLASSW pc{};
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.lpszClassName = L"WVCPreview";
    RegisterClassW(&pc);

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
                                800, 500, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    UpdateStatus();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == g_frame && msg.message == WM_FRAME_DONE) {
            SaveSettings();
            UpdateSourceLabel();
            UpdateFrame();
            continue;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_streaming = false;
    StopCamera();
    if (g_frame) DestroyWindow(g_frame);
    if (g_font) DeleteObject(g_font);
    if (g_frameFont) DeleteObject(g_frameFont);
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
