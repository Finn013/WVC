// WVC — лёгкая виртуальная камера, которая показывает выбранную область экрана.
// Камера: DirectShow-фильтр на базе tshino/softcam (MIT) в WVCCam.dll / WVCCam32.dll.
// Это приложение: источник (область / монитор / окно / веб-камера), захват GDI,
// поворот/зеркало/зум, надпись и логотип поверх кадра, картинка-в-картинке,
// профили, настраиваемые горячие клавиши, предпросмотр, автовосстановление.

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <objbase.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <initializer_list>
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
    // format (changing it restarts the camera; not part of profiles)
    int  quality = 1;      // 0 = 720p, 1 = 1080p
    int  aspect = 0;       // 0 = 16:9, 1 = 4:3, 2 = 1:1, 3 = 9:16
    int  fps = 30;         // 30 or 60
    // picture
    bool cursor = false;
    int  fit = 0;          // 0 = letterbox, 1 = crop
    int  rotation = 0;     // 0..3 = 0/90/180/270 degrees clockwise
    bool mirror = false;
    int  zoom = 0;         // index into kZoom
    int  pauseMode = 2;    // 0 = freeze, 1 = black, 2 = text card, 3 = image
    std::wstring pauseImage;
    // source
    int  srcMode = 0;      // 0 = region, 1 = window
    HWND srcWindow = nullptr;
    std::wstring srcTitle;
    bool hasRegion = false;
    RECT region{};         // virtual-screen coordinates, physical pixels
    // webcam
    int  view = 0;         // 0 = screen, 1 = webcam, 2 = screen + webcam in a corner
    std::wstring camLink, camName;
    bool camMirror = true;
    int  pipCorner = 0;    // 0 = bottom right, 1 = bottom left, 2 = top right, 3 = top left
    int  pipSize = 1;      // index into kPipSize
    // overlays
    bool textOn = false;
    std::wstring text;
    int  textPos = 3;      // 0 TL, 1 TC, 2 TR, 3 BL, 4 BC, 5 BR
    int  textSize = 1;     // 0 small, 1 medium, 2 large
    std::wstring logo;     // empty = no logo
    int  logoCorner = 2;   // same as pipCorner
    int  logoSize = 1;     // index into kLogoSize
};

static const double kZoom[] = {1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
static const int    kZoomCount = 6;
static const double kPipSize[] = {0.20, 0.25, 0.33};
static const double kLogoSize[] = {0.10, 0.15, 0.20, 0.30};

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

static const wchar_t* kMainSec = L"WVC";

static int IniInt(const wchar_t* sec, const wchar_t* key, int def) {
    wchar_t buf[64], defs[32];
    swprintf(defs, 32, L"%d", def);
    GetPrivateProfileStringW(sec, key, defs, buf, 64, IniPath().c_str());
    return _wtoi(buf);
}

static void IniSetInt(const wchar_t* sec, const wchar_t* key, int v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(sec, key, buf, IniPath().c_str());
}

static std::wstring IniStr(const wchar_t* sec, const wchar_t* key) {
    std::vector<wchar_t> buf(4096, 0);
    GetPrivateProfileStringW(sec, key, L"", buf.data(), static_cast<DWORD>(buf.size()), IniPath().c_str());
    return buf.data();
}

static void IniSetStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    // GetPrivateProfileString strips surrounding quotes, so quote values that need protecting
    std::wstring q = L"\"" + v + L"\"";
    WritePrivateProfileStringW(sec, key, q.c_str(), IniPath().c_str());
}

// format = also read/write quality, aspect and fps (main section only)
static void ReadSettings(const wchar_t* sec, Settings& s, bool format) {
    if (format) {
        s.quality = std::clamp(IniInt(sec, L"Resolution", s.quality), 0, 1);
        s.aspect = std::clamp(IniInt(sec, L"Aspect", s.aspect), 0, 3);
        s.fps = IniInt(sec, L"FPS", s.fps) == 60 ? 60 : 30;
    }
    s.cursor = IniInt(sec, L"Cursor", s.cursor) != 0;
    s.fit = std::clamp(IniInt(sec, L"Fit", s.fit), 0, 1);
    s.rotation = std::clamp(IniInt(sec, L"Rotation", s.rotation), 0, 3);
    s.mirror = IniInt(sec, L"Mirror", s.mirror) != 0;
    s.zoom = std::clamp(IniInt(sec, L"Zoom", s.zoom), 0, kZoomCount - 1);
    s.pauseMode = std::clamp(IniInt(sec, L"PauseMode", s.pauseMode), 0, 3);
    s.pauseImage = IniStr(sec, L"PauseImage");
    s.srcMode = std::clamp(IniInt(sec, L"SourceMode", 0), 0, 1);
    s.srcTitle = IniStr(sec, L"WindowTitle");
    s.srcWindow = nullptr;
    s.hasRegion = IniInt(sec, L"HasRegion", s.hasRegion) != 0;
    s.region.left = IniInt(sec, L"Left", s.region.left);
    s.region.top = IniInt(sec, L"Top", s.region.top);
    s.region.right = IniInt(sec, L"Right", s.region.right);
    s.region.bottom = IniInt(sec, L"Bottom", s.region.bottom);
    if (s.region.right - s.region.left < 16 || s.region.bottom - s.region.top < 16) s.hasRegion = false;
    s.view = std::clamp(IniInt(sec, L"View", s.view), 0, 2);
    s.camLink = IniStr(sec, L"CamLink");
    s.camName = IniStr(sec, L"CamName");
    s.camMirror = IniInt(sec, L"CamMirror", s.camMirror) != 0;
    s.pipCorner = std::clamp(IniInt(sec, L"PipCorner", s.pipCorner), 0, 3);
    s.pipSize = std::clamp(IniInt(sec, L"PipSize", s.pipSize), 0, 2);
    s.textOn = IniInt(sec, L"TextOn", s.textOn) != 0;
    s.text = IniStr(sec, L"Text");
    s.textPos = std::clamp(IniInt(sec, L"TextPos", s.textPos), 0, 5);
    s.textSize = std::clamp(IniInt(sec, L"TextSize", s.textSize), 0, 2);
    s.logo = IniStr(sec, L"Logo");
    s.logoCorner = std::clamp(IniInt(sec, L"LogoCorner", s.logoCorner), 0, 3);
    s.logoSize = std::clamp(IniInt(sec, L"LogoSize", s.logoSize), 0, 3);
}

static void WriteSettings(const wchar_t* sec, const Settings& s, bool format) {
    if (format) {
        IniSetInt(sec, L"Resolution", s.quality);
        IniSetInt(sec, L"Aspect", s.aspect);
        IniSetInt(sec, L"FPS", s.fps);
    }
    IniSetInt(sec, L"Cursor", s.cursor ? 1 : 0);
    IniSetInt(sec, L"Fit", s.fit);
    IniSetInt(sec, L"Rotation", s.rotation);
    IniSetInt(sec, L"Mirror", s.mirror ? 1 : 0);
    IniSetInt(sec, L"Zoom", s.zoom);
    IniSetInt(sec, L"PauseMode", s.pauseMode);
    IniSetStr(sec, L"PauseImage", s.pauseImage);
    IniSetInt(sec, L"SourceMode", s.srcMode);
    IniSetStr(sec, L"WindowTitle", s.srcTitle);
    IniSetInt(sec, L"HasRegion", s.hasRegion ? 1 : 0);
    IniSetInt(sec, L"Left", s.region.left);
    IniSetInt(sec, L"Top", s.region.top);
    IniSetInt(sec, L"Right", s.region.right);
    IniSetInt(sec, L"Bottom", s.region.bottom);
    IniSetInt(sec, L"View", s.view);
    IniSetStr(sec, L"CamLink", s.camLink);
    IniSetStr(sec, L"CamName", s.camName);
    IniSetInt(sec, L"CamMirror", s.camMirror ? 1 : 0);
    IniSetInt(sec, L"PipCorner", s.pipCorner);
    IniSetInt(sec, L"PipSize", s.pipSize);
    IniSetInt(sec, L"TextOn", s.textOn ? 1 : 0);
    IniSetStr(sec, L"Text", s.text);
    IniSetInt(sec, L"TextPos", s.textPos);
    IniSetInt(sec, L"TextSize", s.textSize);
    IniSetStr(sec, L"Logo", s.logo);
    IniSetInt(sec, L"LogoCorner", s.logoCorner);
    IniSetInt(sec, L"LogoSize", s.logoSize);
}

static void SaveSettings() { WriteSettings(kMainSec, GetSettings(), true); }

// --- profiles: names in [WVC] Profiles="a|b|c", each in its own section [Profile: a]
static std::vector<std::wstring> ProfileNames() {
    std::vector<std::wstring> v;
    std::wstring all = IniStr(kMainSec, L"Profiles");
    size_t start = 0;
    while (start <= all.size()) {
        size_t bar = all.find(L'|', start);
        std::wstring n = all.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start);
        if (!n.empty()) v.push_back(n);
        if (bar == std::wstring::npos) break;
        start = bar + 1;
    }
    return v;
}

static void SetProfileNames(const std::vector<std::wstring>& v) {
    std::wstring all;
    for (const auto& n : v) {
        if (!all.empty()) all += L"|";
        all += n;
    }
    IniSetStr(kMainSec, L"Profiles", all);
}

static std::wstring ProfileSection(const std::wstring& name) { return L"Profile: " + name; }
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


static HWND FindWindowByTitle(const std::wstring& title) {
    if (title.empty()) return nullptr;
    std::vector<WindowItem> wins;
    EnumWindows(EnumWinProc, reinterpret_cast<LPARAM>(&wins));
    for (const auto& w : wins)
        if (w.title == title) return w.hwnd;
    std::wstring key = title;
    if (key.size() > 1 && key.back() == L'…') key.pop_back();
    for (const auto& w : wins)
        if (w.title.find(key) != std::wstring::npos || key.find(w.title) != std::wstring::npos) return w.hwnd;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Real webcam via Media Foundation (opened only while it is shown)
// ---------------------------------------------------------------------------
struct CamDevice {
    std::wstring name, link;
};

template <class T> static void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

static std::vector<CamDevice> EnumWebcams() {
    std::vector<CamDevice> v;
    IMFAttributes* a = nullptr;
    if (FAILED(MFCreateAttributes(&a, 1))) return v;
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    if (SUCCEEDED(MFEnumDeviceSources(a, &devs, &n))) {
        for (UINT32 i = 0; i < n; ++i) {
            WCHAR* name = nullptr;
            WCHAR* link = nullptr;
            UINT32 len = 0;
            devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
            devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
            if (name && link && !wcsstr(name, L"WVC")) v.push_back({name, link});
            if (name) CoTaskMemFree(name);
            if (link) CoTaskMemFree(link);
            devs[i]->Release();
        }
        CoTaskMemFree(devs);
    }
    a->Release();
    return v;
}

struct Webcam {
    std::thread           th;
    std::atomic<bool>     stop{false};
    std::atomic<int>      status{0};  // 0 off, 1 opening, 2 running, 3 failed
    std::atomic<uint64_t> seq{0};
    std::mutex            mx;
    std::vector<uint8_t>  buf;        // BGRA, top-down
    int                   w = 0, h = 0;
    std::wstring          link;
    ULONGLONG             failedAt = 0;
};
static Webcam g_web;

static void WebcamThread(std::wstring link) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMFAttributes* a = nullptr;
    IMFMediaSource* src = nullptr;
    IMFAttributes* ra = nullptr;
    IMFSourceReader* rd = nullptr;
    IMFMediaType* outType = nullptr;
    IMFMediaType* cur = nullptr;
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    bool ok = false;
    UINT32 w = 0, h = 0;
    LONG defStride = 0;
    do {
        if (FAILED(MFCreateAttributes(&a, 2))) break;
        a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
        if (FAILED(MFCreateDeviceSource(a, &src))) break;
        if (FAILED(MFCreateAttributes(&ra, 1))) break;
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (FAILED(MFCreateSourceReaderFromMediaSource(src, ra, &rd))) break;

        // pick the native mode closest to 1280x720 at >= 24 fps
        DWORD best = MAXDWORD;
        double bestScore = 1e300;
        UINT32 bw = 0, bh = 0;
        for (DWORD i = 0;; ++i) {
            IMFMediaType* t = nullptr;
            if (FAILED(rd->GetNativeMediaType(stream, i, &t))) break;
            UINT32 tw = 0, tth = 0, num = 0, den = 1;
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &tw, &tth);
            MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den);
            const double fps = den ? static_cast<double>(num) / den : 0;
            const double px = static_cast<double>(tw) * tth;
            double score = std::fabs(px - 1280.0 * 720.0);
            if (fps < 24) score += 1e12;
            if (px > 1920.0 * 1080.0) score += 1e11;
            if (tw && tth && score < bestScore) {
                bestScore = score;
                best = i;
                bw = tw;
                bh = tth;
            }
            t->Release();
        }
        if (best != MAXDWORD) {
            IMFMediaType* t = nullptr;
            if (SUCCEEDED(rd->GetNativeMediaType(stream, best, &t))) {
                rd->SetCurrentMediaType(stream, nullptr, t);
                t->Release();
            }
        }
        if (FAILED(MFCreateMediaType(&outType))) break;
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (bw && bh) MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, bw, bh);
        if (FAILED(rd->SetCurrentMediaType(stream, nullptr, outType))) {
            outType->DeleteItem(MF_MT_FRAME_SIZE);
            if (FAILED(rd->SetCurrentMediaType(stream, nullptr, outType))) break;
        }
        if (FAILED(rd->GetCurrentMediaType(stream, &cur))) break;
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
        UINT32 st = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) defStride = static_cast<LONG>(st);
        if (!w || !h) break;
        ok = true;
    } while (false);

    if (ok) {
        std::vector<uint8_t> local(static_cast<size_t>(w) * h * 4);
        bool first = true;
        while (!g_web.stop) {
            DWORD si = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            HRESULT hr = rd->ReadSample(stream, 0, &si, &flags, &ts, &sample);
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                SafeRelease(sample);
                ok = false;
                break;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                SafeRelease(cur);
                if (SUCCEEDED(rd->GetCurrentMediaType(stream, &cur))) {
                    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
                    UINT32 st = 0;
                    defStride = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? static_cast<LONG>(st) : 0;
                    local.assign(static_cast<size_t>(w) * h * 4, 0);
                }
            }
            if (!sample) continue;
            bool mirror;
            {
                std::lock_guard<std::mutex> lk(g_setMx);
                mirror = g_set.camMirror;
            }
            IMFMediaBuffer* mb = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&mb))) {
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                IMF2DBuffer* b2 = nullptr;
                BYTE* raw = nullptr;
                DWORD len = 0;
                bool locked2d = false, locked = false;
                if (SUCCEEDED(mb->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
                    locked2d = true;
                } else if (SUCCEEDED(mb->Lock(&raw, nullptr, &len))) {
                    locked = true;
                    pitch = defStride ? defStride : static_cast<LONG>(w * 4);
                    scan0 = pitch < 0 ? raw + static_cast<size_t>(h - 1) * static_cast<size_t>(-pitch) : raw;
                    const size_t need = static_cast<size_t>(std::labs(pitch)) * h;
                    if (len < need) scan0 = nullptr;
                }
                if (scan0) {
                    for (UINT32 y = 0; y < h; ++y) {
                        const uint32_t* s = reinterpret_cast<const uint32_t*>(scan0 + static_cast<ptrdiff_t>(y) * pitch);
                        uint32_t* d = reinterpret_cast<uint32_t*>(local.data() + static_cast<size_t>(y) * w * 4);
                        if (mirror)
                            for (UINT32 x = 0; x < w; ++x) d[x] = s[w - 1 - x];
                        else
                            std::memcpy(d, s, static_cast<size_t>(w) * 4);
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_web.mx);
                        g_web.buf = local;
                        g_web.w = static_cast<int>(w);
                        g_web.h = static_cast<int>(h);
                    }
                    ++g_web.seq;
                    if (first) {
                        g_web.status = 2;
                        first = false;
                    }
                }
                if (locked2d) b2->Unlock2D();
                if (locked) mb->Unlock();
                SafeRelease(b2);
                mb->Release();
            }
            sample->Release();
        }
    }
    SafeRelease(cur);
    SafeRelease(outType);
    SafeRelease(rd);
    SafeRelease(ra);
    if (src) src->Shutdown();
    SafeRelease(src);
    SafeRelease(a);
    if (!g_web.stop) {
        g_web.failedAt = GetTickCount64();
        g_web.status = 3;
    }
    CoUninitialize();
}

static void WebcamStop() {
    if (g_web.th.joinable()) {
        g_web.stop = true;
        g_web.th.join();
    }
    g_web.status = 0;
    g_web.link.clear();
}

// Called on a timer from the UI thread: opens the webcam only while it is shown.
static std::atomic<bool> g_streaming{false};
static void WebcamSync() {
    Settings s = GetSettings();
    const bool want = g_streaming && s.view != 0 && !s.camLink.empty();
    if (g_web.th.joinable()) {
        const bool dead = g_web.status == 3;
        if (!want || g_web.link != s.camLink || dead) {
            const ULONGLONG failedAt = g_web.failedAt;
            WebcamStop();
            if (dead) {
                g_web.status = 3;
                g_web.failedAt = failedAt;
            }
        }
    }
    if (want && !g_web.th.joinable()) {
        if (g_web.status == 3 && GetTickCount64() - g_web.failedAt < 3000) return;  // retry every 3 s
        g_web.stop = false;
        g_web.status = 1;
        g_web.link = s.camLink;
        g_web.th = std::thread(WebcamThread, s.camLink);
    }
    if (!want && g_web.status == 3) g_web.status = 0;
}

// ---------------------------------------------------------------------------
// Rendering: screen/webcam -> canvas -> rotate/mirror -> overlays -> BGR frame
// ---------------------------------------------------------------------------
struct CamImage {
    const uint8_t* bgra;
    int w, h;
};

struct FitRect {
    int sx, sy, sw, sh;  // source
    int dx, dy, dw, dh;  // destination
};

// zoom into the centre of the source, then letterbox (fit=0) or crop (fit=1) into CW x CH
static FitRect ComputeFit(double sx, double sy, double sw, double sh, double zoom, int fit, int CW, int CH) {
    if (zoom > 1.0) {
        const double nw = sw / zoom, nh = sh / zoom;
        sx += (sw - nw) / 2.0;
        sy += (sh - nh) / 2.0;
        sw = nw;
        sh = nh;
    }
    FitRect f{};
    f.dx = 0;
    f.dy = 0;
    f.dw = CW;
    f.dh = CH;
    if (fit == 0) {
        const double scale = (std::min)(CW / sw, CH / sh);
        f.dw = (std::max)(1, (std::min)(CW, static_cast<int>(std::lround(sw * scale))));
        f.dh = (std::max)(1, (std::min)(CH, static_cast<int>(std::lround(sh * scale))));
        f.dx = (CW - f.dw) / 2;
        f.dy = (CH - f.dh) / 2;
    } else {
        const double scale = (std::max)(CW / sw, CH / sh);
        const double nw = CW / scale, nh = CH / scale;
        sx += (sw - nw) / 2.0;
        sy += (sh - nh) / 2.0;
        sw = nw;
        sh = nh;
    }
    f.sx = static_cast<int>(std::lround(sx));
    f.sy = static_cast<int>(std::lround(sy));
    f.sw = (std::max)(1, static_cast<int>(std::lround(sw)));
    f.sh = (std::max)(1, static_cast<int>(std::lround(sh)));
    return f;
}

// Load an image file scaled to fit inside maxW x maxH. fmt: GUID_WICPixelFormat32bppBGR or ...PBGRA
static bool LoadImageScaled(const std::wstring& path, int maxW, int maxH, REFWICPixelFormatGUID fmt,
                            std::vector<uint8_t>& out, int* ow, int* oh) {
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    IWICBitmapScaler* sc = nullptr;
    do {
        if (maxW < 1 || maxH < 1) break;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) break;
        if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                &dec)))
            break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        if (FAILED(f->CreateFormatConverter(&cv))) break;
        if (FAILED(cv->Initialize(fr, fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) break;
        UINT iw = 0, ih = 0;
        cv->GetSize(&iw, &ih);
        if (!iw || !ih) break;
        const double scale = (std::min)(static_cast<double>(maxW) / iw, static_cast<double>(maxH) / ih);
        const UINT dw = (std::max)(1u, (std::min)(static_cast<UINT>(maxW), static_cast<UINT>(std::lround(iw * scale))));
        const UINT dh = (std::max)(1u, (std::min)(static_cast<UINT>(maxH), static_cast<UINT>(std::lround(ih * scale))));
        if (FAILED(f->CreateBitmapScaler(&sc))) break;
        if (FAILED(sc->Initialize(cv, dw, dh, WICBitmapInterpolationModeFant))) break;
        out.assign(static_cast<size_t>(dw) * dh * 4, 0);
        if (FAILED(sc->CopyPixels(nullptr, dw * 4, static_cast<UINT>(out.size()), out.data()))) break;
        *ow = static_cast<int>(dw);
        *oh = static_cast<int>(dh);
        ok = true;
    } while (false);
    SafeRelease(sc);
    SafeRelease(cv);
    SafeRelease(fr);
    SafeRelease(dec);
    SafeRelease(f);
    return ok;
}

// A 32-bit top-down DIB selected into its own memory DC
struct Surface {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint8_t* bits = nullptr;
    int w = 0, h = 0;

    bool Ensure(HDC ref, int cw, int ch) {
        if (bmp && cw == w && ch == h) return true;
        Free();
        if (!dc) dc = CreateCompatibleDC(ref);
        if (!dc) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cw;
        bi.bmiHeader.biHeight = -ch;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        if (!bmp || !bits) {
            bmp = nullptr;
            bits = nullptr;
            return false;
        }
        old = SelectObject(dc, bmp);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        w = cw;
        h = ch;
        return true;
    }
    void Clear() {
        if (bits) std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
    }
    void Free() {
        if (old && dc) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        old = nullptr;
        bmp = nullptr;
        bits = nullptr;
        w = h = 0;
    }
    ~Surface() {
        Free();
        if (dc) DeleteDC(dc);
    }
};

static void DrawCard(HDC dc, int W, int H, const wchar_t* text) {
    RECT rc{0, 0, W, H};
    HBRUSH bg = CreateSolidBrush(RGB(30, 32, 38));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    const int px = (std::max)(20, (std::min)(W, H) / 11);
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 240));
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(f);
}

class Renderer {
public:
    Renderer(int w, int h) : W(w), H(h) { screenDC = GetDC(nullptr); }
    ~Renderer() {
        if (textFont) DeleteObject(textFont);
        if (screenDC) ReleaseDC(nullptr, screenDC);
    }

    // src: screen rectangle (views 0 and 2), cam: latest webcam frame or nullptr, camMsg: shown when cam is missing.
    // false = the screen grab failed.
    bool Frame(const Settings& s, const RECT* src, const CamImage* cam, const wchar_t* camMsg, uint8_t* outBGR) {
        if (!screenDC) return false;
        const bool swap = (s.rotation & 1) != 0;
        const int cw = swap ? H : W, ch = swap ? W : H;
        if (!canvas.Ensure(screenDC, cw, ch) || !out.Ensure(screenDC, W, H)) return false;
        canvas.Clear();
        const double zoom = kZoom[std::clamp(s.zoom, 0, kZoomCount - 1)];

        if (s.view == 1) {
            if (cam && UploadCam(*cam)) {
                FitRect f = ComputeFit(0, 0, cam->w, cam->h, zoom, s.fit, cw, ch);
                StretchBlt(canvas.dc, f.dx, f.dy, f.dw, f.dh, camSurf.dc, f.sx, f.sy, f.sw, f.sh, SRCCOPY);
            } else {
                DrawCard(canvas.dc, cw, ch, camMsg);
            }
        } else {
            if (!src) return false;
            FitRect f = ComputeFit(src->left, src->top, src->right - src->left, src->bottom - src->top, zoom, s.fit,
                                   cw, ch);
            BOOL ok;
            if (f.dw == f.sw && f.dh == f.sh)
                ok = BitBlt(canvas.dc, f.dx, f.dy, f.dw, f.dh, screenDC, f.sx, f.sy, SRCCOPY);
            else
                ok = StretchBlt(canvas.dc, f.dx, f.dy, f.dw, f.dh, screenDC, f.sx, f.sy, f.sw, f.sh, SRCCOPY);
            if (!ok) return false;
            if (s.cursor) DrawCursor(f);
        }
        GdiFlush();
        Convert(s.rotation, s.mirror);

        if (s.view == 2) DrawPip(s, cam, camMsg);
        if (!s.logo.empty()) DrawLogo(s);
        if (s.textOn && !s.text.empty()) DrawTextOverlay(s);
        GdiFlush();
        Pack(outBGR);
        return true;
    }

    // Picture shown while paused (modes 1..3)
    void Pause(const Settings& s, uint8_t* outBGR) {
        if (!out.Ensure(screenDC, W, H)) return;
        out.Clear();
        bool drawn = false;
        if (s.pauseMode == 3 && !s.pauseImage.empty()) {
            std::vector<uint8_t> img;
            int iw = 0, ih = 0;
            if (LoadImageScaled(s.pauseImage, W, H, GUID_WICPixelFormat32bppBGR, img, &iw, &ih)) {
                const int dx = (W - iw) / 2, dy = (H - ih) / 2;
                for (int y = 0; y < ih; ++y)
                    std::memcpy(out.bits + (static_cast<size_t>(dy + y) * W + dx) * 4,
                                img.data() + static_cast<size_t>(y) * iw * 4, static_cast<size_t>(iw) * 4);
                drawn = true;
            }
        }
        if (s.pauseMode == 2 || (s.pauseMode == 3 && !drawn))
            DrawCard(out.dc, W, H, s.pauseMode == 3 ? L"Не удалось открыть картинку" : L"Скоро вернусь");
        GdiFlush();
        Pack(outBGR);
    }

private:
    bool UploadCam(const CamImage& cam) {
        if (!cam.bgra || cam.w < 1 || cam.h < 1) return false;
        if (cam.bgra == camSrc && cam.w == camSurf.w && cam.h == camSurf.h && camSeqUploaded) return true;
        if (!camSurf.Ensure(screenDC, cam.w, cam.h)) return false;
        std::memcpy(camSurf.bits, cam.bgra, static_cast<size_t>(cam.w) * cam.h * 4);
        camSrc = cam.bgra;
        camSeqUploaded = true;
        return true;
    }

public:
    void CamChanged() { camSeqUploaded = false; }

private:
    // canvas (BGRA) -> out (BGRA, W x H) with clockwise rotation and horizontal mirror
    void Convert(int rot, bool mirror) {
        const int CW = canvas.w, CH = canvas.h;
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
            const ptrdiff_t step = static_cast<ptrdiff_t>(ddx) + static_cast<ptrdiff_t>(ddy) * CW;
            const uint32_t* p = reinterpret_cast<const uint32_t*>(canvas.bits) + static_cast<ptrdiff_t>(cy) * CW + cx;
            uint32_t* o = reinterpret_cast<uint32_t*>(out.bits) + static_cast<size_t>(y) * W;
            if (step == 1) {
                std::memcpy(o, p, static_cast<size_t>(W) * 4);
            } else {
                for (int x = 0; x < W; ++x, p += step) o[x] = *p;
            }
        }
    }

    void Pack(uint8_t* dst) {
        const uint8_t* p = out.bits;
        const size_t n = static_cast<size_t>(W) * H;
        for (size_t i = 0; i < n; ++i, p += 4, dst += 3) {
            dst[0] = p[0];
            dst[1] = p[1];
            dst[2] = p[2];
        }
    }

    RECT CornerRect(int corner, int w, int h) const {
        const int m = (std::max)(8, H / 30);
        const int x = (corner == 1 || corner == 3) ? m : W - m - w;
        const int y = (corner == 2 || corner == 3) ? m : H - m - h;
        return RECT{x, y, x + w, y + h};
    }

    void DrawPip(const Settings& s, const CamImage* cam, const wchar_t* camMsg) {
        const int pw = static_cast<int>(W * kPipSize[std::clamp(s.pipSize, 0, 2)]);
        int ph = pw * 9 / 16;
        if (cam && cam->w > 0) ph = pw * cam->h / cam->w;
        ph = (std::min)(ph, H / 2);
        RECT r = CornerRect(s.pipCorner, pw, ph);
        RECT border = r;
        const int b = (std::max)(2, H / 360);
        InflateRect(&border, b, b);
        FillRect(out.dc, &border, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        if (cam && UploadCam(*cam)) {
            StretchBlt(out.dc, r.left, r.top, pw, ph, camSurf.dc, 0, 0, cam->w, cam->h, SRCCOPY);
        } else {
            HBRUSH bg = CreateSolidBrush(RGB(30, 32, 38));
            FillRect(out.dc, &r, bg);
            DeleteObject(bg);
            HFONT f = CreateFontW(-(std::max)(12, ph / 8), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                                  L"Segoe UI");
            HGDIOBJ old = SelectObject(out.dc, f);
            SetBkMode(out.dc, TRANSPARENT);
            SetTextColor(out.dc, RGB(220, 220, 220));
            DrawTextW(out.dc, camMsg, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(out.dc, old);
            DeleteObject(f);
        }
    }

    void DrawLogo(const Settings& s) {
        const int maxW = static_cast<int>(W * kLogoSize[std::clamp(s.logoSize, 0, 3)]);
        const int maxH = H / 2;
        if (s.logo != logoPath || maxW != logoMaxW || maxH != logoMaxH) {
            logoPath = s.logo;
            logoMaxW = maxW;
            logoMaxH = maxH;
            logoOk = LoadImageScaled(s.logo, maxW, maxH, GUID_WICPixelFormat32bppPBGRA, logoPx, &logoW, &logoH);
        }
        if (!logoOk) return;
        GdiFlush();
        RECT r = CornerRect(s.logoCorner, logoW, logoH);
        for (int y = 0; y < logoH; ++y) {
            const int oy = r.top + y;
            if (oy < 0 || oy >= H) continue;
            const uint8_t* sp = logoPx.data() + static_cast<size_t>(y) * logoW * 4;
            for (int x = 0; x < logoW; ++x, sp += 4) {
                const int ox = r.left + x;
                if (ox < 0 || ox >= W) continue;
                uint8_t* dp = out.bits + (static_cast<size_t>(oy) * W + ox) * 4;
                const int inv = 255 - sp[3];
                dp[0] = static_cast<uint8_t>(sp[0] + dp[0] * inv / 255);
                dp[1] = static_cast<uint8_t>(sp[1] + dp[1] * inv / 255);
                dp[2] = static_cast<uint8_t>(sp[2] + dp[2] * inv / 255);
            }
        }
    }

    void DrawTextOverlay(const Settings& s) {
        static const int div[3] = {24, 16, 10};
        const int px = (std::max)(12, H / div[std::clamp(s.textSize, 0, 2)]);
        if (px != textPx || !textFont) {
            if (textFont) DeleteObject(textFont);
            textFont = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            textPx = px;
        }
        HGDIOBJ old = SelectObject(out.dc, textFont);
        RECT tr{0, 0, W, H};
        DrawTextW(out.dc, s.text.c_str(), -1, &tr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        const int m = (std::max)(8, H / 30), pad = px / 3;
        const int bw = (std::min)(W - 2 * m, static_cast<int>(tr.right - tr.left) + 2 * pad);
        const int bh = static_cast<int>(tr.bottom - tr.top) + pad;
        const int col = s.textPos % 3, row = s.textPos / 3;
        const int x = col == 0 ? m : col == 1 ? (W - bw) / 2 : W - m - bw;
        const int y = row == 0 ? m : H - m - bh;
        // darken the box behind the text
        GdiFlush();
        for (int yy = (std::max)(0, y); yy < (std::min)(H, y + bh); ++yy) {
            uint8_t* p = out.bits + (static_cast<size_t>(yy) * W + (std::max)(0, x)) * 4;
            for (int xx = (std::max)(0, x); xx < (std::min)(W, x + bw); ++xx, p += 4) {
                p[0] = static_cast<uint8_t>(p[0] * 2 / 5);
                p[1] = static_cast<uint8_t>(p[1] * 2 / 5);
                p[2] = static_cast<uint8_t>(p[2] * 2 / 5);
            }
        }
        RECT box{x + pad, y, x + bw - pad, y + bh};
        SetBkMode(out.dc, TRANSPARENT);
        SetTextColor(out.dc, RGB(255, 255, 255));
        DrawTextW(out.dc, s.text.c_str(), -1, &box, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
        SelectObject(out.dc, old);
    }

    void DrawCursor(const FitRect& f) {
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
        const double scale = static_cast<double>(f.dw) / f.sw;
        const int x = static_cast<int>(std::lround((ci.ptScreenPos.x - static_cast<int>(ii.xHotspot) - f.sx) * scale)) + f.dx;
        const int y = static_cast<int>(std::lround((ci.ptScreenPos.y - static_cast<int>(ii.yHotspot) - f.sy) * scale)) + f.dy;
        const int sw = (std::max)(1, static_cast<int>(std::lround(cw * scale)));
        const int sh = (std::max)(1, static_cast<int>(std::lround(ch * scale)));
        int saved = SaveDC(canvas.dc);
        IntersectClipRect(canvas.dc, f.dx, f.dy, f.dx + f.dw, f.dy + f.dh);
        DrawIconEx(canvas.dc, x, y, ci.hCursor, sw, sh, 0, nullptr, DI_NORMAL);
        RestoreDC(canvas.dc, saved);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }

    int W, H;
    HDC screenDC = nullptr;
    Surface canvas, out, camSurf;
    const uint8_t* camSrc = nullptr;
    bool camSeqUploaded = false;
    std::wstring logoPath;
    int logoMaxW = 0, logoMaxH = 0, logoW = 0, logoH = 0;
    bool logoOk = false;
    std::vector<uint8_t> logoPx;
    HFONT textFont = nullptr;
    int textPx = 0;
};

// ---------------------------------------------------------------------------
// Camera + sender thread
// ---------------------------------------------------------------------------
static scCamera          g_cam = nullptr;
static std::thread       g_sender;
static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_resetCapture{false};   // recreate GDI objects (display change, resume)
static std::atomic<bool> g_pauseDirty{false};     // re-render pause picture
static std::atomic<bool> g_sourceLost{false};     // window closed / minimised
static std::atomic<int>  g_fpsX10{0};

static std::mutex           g_prevMx;
static std::vector<uint8_t> g_prev;
static int                  g_prevW = 0, g_prevH = 0;

static const wchar_t* WebcamMessage(const Settings& s) {
    if (s.camLink.empty()) return L"Веб-камера не выбрана";
    switch (g_web.status.load()) {
    case 3: return L"Веб-камера недоступна";
    default: return L"Включаю веб-камеру…";
    }
}

static void SenderLoop(int W, int H) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> frame(static_cast<size_t>(W) * H * 3, 0);
    std::unique_ptr<Renderer> rnd(new Renderer(W, H));
    std::vector<uint8_t> camBuf;
    int camW = 0, camH = 0;
    uint64_t camSeq = 0;
    std::wstring pauseKey;
    int fails = 0, frames = 0;
    ULONGLONG fpsT0 = GetTickCount64(), prevT = 0;

    while (!g_quit) {
        if (g_resetCapture.exchange(false)) {
            rnd.reset(new Renderer(W, H));
            pauseKey.clear();
            camSeq = 0;
            fails = 0;
        }
        Settings s = GetSettings();
        if (g_streaming) {
            pauseKey.clear();
            const CamImage* cam = nullptr;
            CamImage ci{};
            if (s.view != 0 && g_web.status == 2) {
                const uint64_t q = g_web.seq;
                if (q != camSeq) {
                    std::lock_guard<std::mutex> lk(g_web.mx);
                    camBuf = g_web.buf;
                    camW = g_web.w;
                    camH = g_web.h;
                    camSeq = q;
                    rnd->CamChanged();
                }
                if (!camBuf.empty()) {
                    ci = CamImage{camBuf.data(), camW, camH};
                    cam = &ci;
                }
            }
            const wchar_t* msg = WebcamMessage(s);
            if (s.view == 1) {
                g_sourceLost = false;
                rnd->Frame(s, nullptr, cam, msg, frame.data());
            } else {
                RECT src;
                if (ResolveSource(s, &src)) {
                    g_sourceLost = false;
                    if (rnd->Frame(s, &src, cam, msg, frame.data()))
                        fails = 0;
                    else if (++fails >= 15)  // ~0.5 s of failures: rebuild capture
                        g_resetCapture = true;
                } else {
                    g_sourceLost = true;  // keep the last frame
                }
            }
        } else if (s.pauseMode != 0) {
            std::wstring key = std::to_wstring(s.pauseMode) + L"|" + s.pauseImage;
            if (key != pauseKey || g_pauseDirty.exchange(false)) {
                if (s.pauseMode == 1)
                    std::fill(frame.begin(), frame.end(), static_cast<uint8_t>(0));
                else
                    rnd->Pause(s, frame.data());
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
    if (g_streaming && s.view != 1 && ResolveSource(s, &r)) {
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
// Hotkeys (user-configurable)
// ---------------------------------------------------------------------------
enum HotkeyAction {
    HA_TOGGLE = 0,
    HA_SELECT,
    HA_MONITOR,
    HA_CURSOR,
    HA_SCREEN_CAM,
    HA_PIP,
    HA_TEXT,
    HA_PROFILE1,
    HA_PROFILE2,
    HA_PROFILE3,
    HA_PROFILE4,
    HA_COUNT
};

struct HotkeyDef {
    const wchar_t* name;
    UINT defMods;  // MOD_* flags
    UINT defVk;
};

static const HotkeyDef kHotkeys[HA_COUNT] = {
    {L"Старт / Стоп", MOD_CONTROL | MOD_SHIFT, VK_F5},
    {L"Выбрать область", MOD_CONTROL | MOD_SHIFT, VK_F6},
    {L"Следующий монитор", MOD_CONTROL | MOD_SHIFT, VK_F7},
    {L"Курсор вкл/выкл", MOD_CONTROL | MOD_SHIFT, VK_F8},
    {L"Экран ↔ веб-камера", MOD_CONTROL | MOD_SHIFT, VK_F9},
    {L"Камера в углу вкл/выкл", MOD_CONTROL | MOD_SHIFT, VK_F10},
    {L"Надпись вкл/выкл", MOD_CONTROL | MOD_SHIFT, VK_F11},
    {L"Профиль 1", MOD_CONTROL | MOD_SHIFT, '1'},
    {L"Профиль 2", MOD_CONTROL | MOD_SHIFT, '2'},
    {L"Профиль 3", MOD_CONTROL | MOD_SHIFT, '3'},
    {L"Профиль 4", MOD_CONTROL | MOD_SHIFT, '4'},
};

static UINT g_hk[HA_COUNT];  // (MOD_* << 8) | vk, 0 = none
static std::wstring g_hotkeyNote;

static UINT HkPack(UINT mods, UINT vk) { return vk ? ((mods & 0xFF) << 8) | (vk & 0xFF) : 0; }
static UINT HkMods(UINT v) { return (v >> 8) & 0xFF; }
static UINT HkVk(UINT v) { return v & 0xFF; }

static bool IsExtendedVk(UINT vk) {
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        return true;
    }
    return false;
}

static std::wstring HotkeyName(UINT v) {
    if (!v) return L"—";
    std::wstring s;
    const UINT m = HkMods(v), vk = HkVk(v);
    if (m & MOD_CONTROL) s += L"Ctrl+";
    if (m & MOD_SHIFT) s += L"Shift+";
    if (m & MOD_ALT) s += L"Alt+";
    if (m & MOD_WIN) s += L"Win+";
    wchar_t name[64] = {};
    LONG lp = static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
    if (IsExtendedVk(vk)) lp |= 1 << 24;
    if (GetKeyNameTextW(lp, name, 64) > 0)
        s += name;
    else {
        wchar_t buf[16];
        swprintf(buf, 16, L"0x%02X", vk);
        s += buf;
    }
    return s;
}

static void LoadHotkeys() {
    for (int i = 0; i < HA_COUNT; ++i) {
        wchar_t key[16];
        swprintf(key, 16, L"HK%d", i);
        const int v = IniInt(kMainSec, key, -1);
        g_hk[i] = v < 0 ? HkPack(kHotkeys[i].defMods, kHotkeys[i].defVk) : static_cast<UINT>(v);
    }
}

static void SaveHotkeys() {
    for (int i = 0; i < HA_COUNT; ++i) {
        wchar_t key[16];
        swprintf(key, 16, L"HK%d", i);
        IniSetInt(kMainSec, key, static_cast<int>(g_hk[i]));
    }
}

static void UnregisterHotkeys(HWND hwnd) {
    for (int i = 0; i < HA_COUNT; ++i) UnregisterHotKey(hwnd, i + 1);
}

static void RegisterHotkeys(HWND hwnd) {
    UnregisterHotkeys(hwnd);
    std::wstring busy;
    for (int i = 0; i < HA_COUNT; ++i) {
        if (!g_hk[i]) continue;
        if (!RegisterHotKey(hwnd, i + 1, HkMods(g_hk[i]) | MOD_NOREPEAT, HkVk(g_hk[i]))) {
            if (!busy.empty()) busy += L", ";
            busy += HotkeyName(g_hk[i]);
        }
    }
    g_hotkeyNote = busy.empty() ? L"" : L"Заняты другой программой: " + busy;
}

static std::wstring HotkeyListText() {
    std::wstring t = L"Горячие клавиши (работают везде):\r\n";
    for (int i = 0; i < HA_COUNT; ++i) {
        if (!g_hk[i]) continue;
        t += HotkeyName(g_hk[i]) + L" — " + kHotkeys[i].name + L"\r\n";
    }
    if (!g_hotkeyNote.empty()) t += L"\r\n" + g_hotkeyNote;
    return t;
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------
enum {
    IDC_TAB = 100,
    // page 0: source and picture
    IDC_SRC_LABEL, IDC_SRC_VALUE, IDC_SELECT, IDC_MONITOR, IDC_WINDOW,
    IDC_Q_LABEL, IDC_QUALITY, IDC_A_LABEL, IDC_ASPECT, IDC_FPS_LABEL, IDC_FPS,
    IDC_FIT_LABEL, IDC_FIT, IDC_ROT_LABEL, IDC_ROT, IDC_ZOOM_LABEL, IDC_ZOOM,
    IDC_MIRROR, IDC_CURSOR,
    // page 1: webcam and overlays
    IDC_VIEW_LABEL, IDC_VIEW, IDC_CAM_LABEL, IDC_CAMDEV, IDC_PIP_LABEL, IDC_PIPCORNER, IDC_PIPSIZE, IDC_CAMMIRROR,
    IDC_TEXT_LABEL, IDC_TEXT, IDC_TEXTON, IDC_TPOS_LABEL, IDC_TEXTPOS, IDC_TEXTSIZE,
    IDC_LOGO_LABEL, IDC_LOGO_PICK, IDC_LOGO_CLEAR, IDC_LPOS_LABEL, IDC_LOGOCORNER, IDC_LOGOSIZE, IDC_LOGO_NAME,
    // page 2: profiles and hotkeys
    IDC_PROF_LABEL, IDC_PROFILE, IDC_PROF_LOAD, IDC_PROF_SAVE, IDC_PROF_DEL, IDC_PROF_HINT,
    IDC_HK_EDIT, IDC_HK_LIST,
    // right column
    IDC_PREV_LABEL, IDC_FPS_VALUE, IDC_PREVIEW, IDC_START, IDC_STATUS,
    IDC_PAUSE_LABEL, IDC_PAUSE, IDC_PAUSE_IMG, IDC_INSTALL, IDC_UNINSTALL,
};
enum { ID_TRAY_OPEN = 300, ID_TRAY_TOGGLE, ID_TRAY_EXIT };

static const UINT WM_TRAY = WM_APP + 1;
static const UINT_PTR kTimerStatus = 1, kTimerPreview = 2, kTimerFollow = 3;

static HINSTANCE g_hInst;
static HWND g_main = nullptr;
static HWND g_preview = nullptr;
static HFONT g_font = nullptr;
static UINT g_dpi = 96;
static NOTIFYICONDATAW g_nid{};
static std::wstring g_note;  // one-off note in the status line
static bool g_syncing = false;
static int g_page = 0;
static std::vector<CamDevice> g_camList;

static const int kPage0[] = {IDC_SRC_LABEL, IDC_SRC_VALUE, IDC_SELECT, IDC_MONITOR, IDC_WINDOW, IDC_Q_LABEL,
                             IDC_QUALITY, IDC_A_LABEL, IDC_ASPECT, IDC_FPS_LABEL, IDC_FPS, IDC_FIT_LABEL, IDC_FIT,
                             IDC_ROT_LABEL, IDC_ROT, IDC_ZOOM_LABEL, IDC_ZOOM, IDC_MIRROR, IDC_CURSOR};
static const int kPage1[] = {IDC_VIEW_LABEL, IDC_VIEW, IDC_CAM_LABEL, IDC_CAMDEV, IDC_PIP_LABEL, IDC_PIPCORNER,
                             IDC_PIPSIZE, IDC_CAMMIRROR, IDC_TEXT_LABEL, IDC_TEXT, IDC_TEXTON, IDC_TPOS_LABEL,
                             IDC_TEXTPOS, IDC_TEXTSIZE, IDC_LOGO_LABEL, IDC_LOGO_PICK, IDC_LOGO_CLEAR, IDC_LPOS_LABEL,
                             IDC_LOGOCORNER, IDC_LOGOSIZE, IDC_LOGO_NAME};
static const int kPage2[] = {IDC_PROF_LABEL, IDC_PROFILE, IDC_PROF_LOAD, IDC_PROF_SAVE, IDC_PROF_DEL,
                             IDC_PROF_HINT, IDC_HK_EDIT, IDC_HK_LIST};

static bool OnTabPage(int id) { return id >= IDC_SRC_LABEL && id <= IDC_HK_LIST; }

static int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }
static HWND Ctl(int id) { return GetDlgItem(g_main, id); }

static void SetTextIfChanged(HWND h, const std::wstring& text) {
    wchar_t cur[2048] = {};
    GetWindowTextW(h, cur, 2048);
    if (text != cur) SetWindowTextW(h, text.c_str());
}

static void MakeFont() {
    if (g_font) DeleteObject(g_font);
    g_font = CreateFontW(-MulDiv(9, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                         L"Segoe UI");
}

static const int kClientW = 800, kClientH = 484;

static void Layout() {
    struct L { int id, x, y, w, h; };
    const int lx = 24, cx = 172, cw = 210;  // label x, control x, control width on the tab pages
    const L items[] = {
        {IDC_TAB, 10, 8, 384, 468},
        // page 0
        {IDC_SRC_LABEL, lx, 46, 70, 20},
        {IDC_SRC_VALUE, lx + 72, 46, 286, 20},
        {IDC_SELECT, lx, 70, 116, 28},
        {IDC_MONITOR, lx + 121, 70, 116, 28},
        {IDC_WINDOW, lx + 242, 70, 116, 28},
        {IDC_Q_LABEL, lx, 116, 145, 20}, {IDC_QUALITY, cx, 112, cw, 200},
        {IDC_A_LABEL, lx, 148, 145, 20}, {IDC_ASPECT, cx, 144, cw, 200},
        {IDC_FPS_LABEL, lx, 180, 145, 20}, {IDC_FPS, cx, 176, cw, 200},
        {IDC_FIT_LABEL, lx, 212, 145, 20}, {IDC_FIT, cx, 208, cw, 200},
        {IDC_ROT_LABEL, lx, 244, 145, 20}, {IDC_ROT, cx, 240, cw, 200},
        {IDC_ZOOM_LABEL, lx, 276, 145, 20}, {IDC_ZOOM, cx, 272, cw, 200},
        {IDC_MIRROR, lx, 308, 150, 22}, {IDC_CURSOR, cx, 308, cw, 22},
        // page 1
        {IDC_VIEW_LABEL, lx, 50, 145, 20}, {IDC_VIEW, cx, 46, cw, 200},
        {IDC_CAM_LABEL, lx, 82, 145, 20}, {IDC_CAMDEV, cx, 78, cw, 240},
        {IDC_PIP_LABEL, lx, 114, 145, 20}, {IDC_PIPCORNER, cx, 110, 128, 200}, {IDC_PIPSIZE, cx + 134, 110, 76, 200},
        {IDC_CAMMIRROR, lx, 144, 340, 22},
        {IDC_TEXT_LABEL, lx, 186, 145, 20}, {IDC_TEXT, cx, 182, cw, 24},
        {IDC_TEXTON, lx, 214, 340, 22},
        {IDC_TPOS_LABEL, lx, 248, 145, 20}, {IDC_TEXTPOS, cx, 244, 128, 200}, {IDC_TEXTSIZE, cx + 134, 244, 76, 200},
        {IDC_LOGO_LABEL, lx, 292, 145, 20}, {IDC_LOGO_PICK, cx, 288, 128, 28}, {IDC_LOGO_CLEAR, cx + 134, 288, 76, 28},
        {IDC_LPOS_LABEL, lx, 328, 145, 20}, {IDC_LOGOCORNER, cx, 324, 128, 200}, {IDC_LOGOSIZE, cx + 134, 324, 76, 200},
        {IDC_LOGO_NAME, lx, 358, 358, 20},
        // page 2
        {IDC_PROF_LABEL, lx, 50, 90, 20}, {IDC_PROFILE, lx + 92, 46, 266, 240},
        {IDC_PROF_LOAD, lx, 82, 116, 28}, {IDC_PROF_SAVE, lx + 121, 82, 116, 28}, {IDC_PROF_DEL, lx + 242, 82, 116, 28},
        {IDC_PROF_HINT, lx, 120, 358, 56},
        {IDC_HK_EDIT, lx, 184, 220, 30},
        {IDC_HK_LIST, lx, 224, 358, 240},
        // right column
        {IDC_PREV_LABEL, 406, 14, 220, 20},
        {IDC_FPS_VALUE, 630, 14, 160, 20},
        {IDC_PREVIEW, 406, 40, 384, 216},
        {IDC_START, 406, 266, 384, 40},
        {IDC_STATUS, 406, 314, 384, 56},
        {IDC_PAUSE_LABEL, 406, 382, 150, 20},
        {IDC_PAUSE, 560, 378, 230, 200},
        {IDC_PAUSE_IMG, 560, 410, 230, 28},
        {IDC_INSTALL, 406, 448, 189, 28},
        {IDC_UNINSTALL, 601, 448, 189, 28},
    };
    for (const auto& it : items) {
        HWND h = Ctl(it.id);
        if (!h) continue;
        SetWindowPos(h, nullptr, S(it.x), S(it.y), S(it.w), S(it.h), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
    SetWindowPos(Ctl(IDC_TAB), HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void ShowPage(int page) {
    g_page = page;
    auto show = [](const int* ids, size_t n, bool on) {
        for (size_t i = 0; i < n; ++i) ShowWindow(Ctl(ids[i]), on ? SW_SHOW : SW_HIDE);
    };
    show(kPage0, ARRAYSIZE(kPage0), page == 0);
    show(kPage1, ARRAYSIZE(kPage1), page == 1);
    show(kPage2, ARRAYSIZE(kPage2), page == 2);
}

static int ComboSel(int id) { return static_cast<int>(SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0)); }
static void ComboSet(int id, int sel) { SendMessageW(Ctl(id), CB_SETCURSEL, sel, 0); }
static void CheckSet(int id, bool on) { SendMessageW(Ctl(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
static bool CheckGet(int id) { return SendMessageW(Ctl(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }

static std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

static void FillCamCombo() {
    Settings s = GetSettings();
    g_camList = EnumWebcams();
    HWND h = Ctl(IDC_CAMDEV);
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"— не выбрана —"));
    int sel = 0;
    for (size_t i = 0; i < g_camList.size(); ++i) {
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_camList[i].name.c_str()));
        if (g_camList[i].link == s.camLink) sel = static_cast<int>(i + 1);
    }
    if (sel == 0 && !s.camLink.empty()) {  // remembered camera that is not plugged in now
        std::wstring n = (s.camName.empty() ? L"Камера" : s.camName) + L" (не подключена)";
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n.c_str()));
        g_camList.push_back({s.camName, s.camLink});
        sel = static_cast<int>(g_camList.size());
    }
    SendMessageW(h, CB_SETCURSEL, sel, 0);
}

static void FillProfileCombo(const std::wstring& select) {
    HWND h = Ctl(IDC_PROFILE);
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    auto names = ProfileNames();
    int sel = names.empty() ? -1 : 0;
    for (size_t i = 0; i < names.size(); ++i) {
        std::wstring label = std::to_wstring(i + 1) + L". " + names[i];
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (names[i] == select) sel = static_cast<int>(i);
    }
    SendMessageW(h, CB_SETCURSEL, sel, 0);
}

// settings -> controls (after hotkeys, profiles, startup)
static void SyncControls() {
    g_syncing = true;
    Settings s = GetSettings();
    ComboSet(IDC_QUALITY, s.quality);
    ComboSet(IDC_ASPECT, s.aspect);
    ComboSet(IDC_FPS, s.fps == 60 ? 1 : 0);
    ComboSet(IDC_FIT, s.fit);
    ComboSet(IDC_ROT, s.rotation);
    ComboSet(IDC_ZOOM, s.zoom);
    CheckSet(IDC_MIRROR, s.mirror);
    CheckSet(IDC_CURSOR, s.cursor);
    ComboSet(IDC_VIEW, s.view);
    ComboSet(IDC_PIPCORNER, s.pipCorner);
    ComboSet(IDC_PIPSIZE, s.pipSize);
    CheckSet(IDC_CAMMIRROR, s.camMirror);
    SetTextIfChanged(Ctl(IDC_TEXT), s.text);
    CheckSet(IDC_TEXTON, s.textOn);
    ComboSet(IDC_TEXTPOS, s.textPos);
    ComboSet(IDC_TEXTSIZE, s.textSize);
    ComboSet(IDC_LOGOCORNER, s.logoCorner);
    ComboSet(IDC_LOGOSIZE, s.logoSize);
    SetTextIfChanged(Ctl(IDC_LOGO_NAME), s.logo.empty() ? L"Логотип не выбран" : L"Логотип: " + FileNameOf(s.logo));
    ComboSet(IDC_PAUSE, s.pauseMode);
    FillCamCombo();
    g_syncing = false;
}

static void UpdateSourceLabel() {
    if (!g_main) return;
    Settings s = GetSettings();
    wchar_t buf[256];
    if (s.view == 1) {
        wcscpy(buf, L"веб-камера");
    } else if (s.srcMode == 1) {
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
    Settings s = GetSettings();
    std::wstring line2;
    if (!g_note.empty())
        line2 = g_note;
    else if (g_streaming && s.view != 0 && s.camLink.empty())
        line2 = L"Выберите веб-камеру на вкладке «Камера и наложения».";
    else if (g_streaming && s.view != 0 && g_web.status == 3)
        line2 = L"Веб-камера недоступна: занята другой программой или отключена. Пробую снова…";
    else if (g_streaming && s.view != 1 && g_sourceLost)
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
    if (s.view == 1) return true;  // the card says what is missing
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
    WebcamSync();
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
        if (g_set.view == 1) g_set.view = 0;  // picking a screen source means "show the screen"
    }
    SaveSettings();
    SyncControls();
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
    if (idx >= 0 && s.srcMode == 0 && EqualRect(&s.region, &mons[idx].rc))
        idx = (idx + 1) % static_cast<int>(mons.size());
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

static void SetWindowSource(HWND hwnd, const std::wstring& title) {
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.srcMode = 1;
        g_set.srcWindow = hwnd;
        g_set.srcTitle = title;
        if (g_set.view == 1) g_set.view = 0;
    }
    SaveSettings();
    SyncControls();
    UpdateSourceLabel();
    UpdateFrame();
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
    if (cmd >= 2000 && cmd < 2000 + static_cast<int>(wins.size()))
        SetWindowSource(wins[cmd - 2000].hwnd, wins[cmd - 2000].title);
}

static bool PickImageFile(std::wstring* out) {
    wchar_t file[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"Картинки (PNG, JPG, BMP, GIF)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0Все файлы\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    *out = file;
    return true;
}

static void ChoosePauseImage() {
    std::wstring f;
    if (!PickImageFile(&f)) return;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.pauseImage = f;
        g_set.pauseMode = 3;
    }
    ComboSet(IDC_PAUSE, 3);
    g_pauseDirty = true;
    SaveSettings();
}

static void ChooseLogo() {
    std::wstring f;
    if (!PickImageFile(&f)) return;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.logo = f;
    }
    SaveSettings();
    SyncControls();
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

static void SetCursorOption(bool on) {
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.cursor = on;
    }
    CheckSet(IDC_CURSOR, on);
    SaveSettings();
}

// --- profiles
static void LoadProfile(const std::wstring& name) {
    Settings s = GetSettings();
    ReadSettings(ProfileSection(name).c_str(), s, false);
    std::wstring note;
    if (s.srcMode == 1) {
        s.srcWindow = FindWindowByTitle(s.srcTitle);
        if (!s.srcWindow) {
            s.srcMode = 0;
            note = L"Окно «" + s.srcTitle + L"» не найдено: показываю сохранённую область.";
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set = s;
    }
    g_pauseDirty = true;
    SaveSettings();
    SyncControls();
    UpdateFrame();
    g_note = note.empty() ? L"Профиль «" + name + L"» загружен." : note;
    UpdateStatus();
}

static void LoadProfileByIndex(int i) {
    auto names = ProfileNames();
    if (i >= 0 && i < static_cast<int>(names.size())) {
        LoadProfile(names[i]);
        FillProfileCombo(names[i]);
    }
}

static std::wstring SelectedProfile() {
    auto names = ProfileNames();
    int sel = ComboSel(IDC_PROFILE);
    return sel >= 0 && sel < static_cast<int>(names.size()) ? names[sel] : L"";
}

static void SaveProfile(std::wstring name) {
    for (auto& ch : name)
        if (ch == L'|' || ch == L'[' || ch == L']' || ch == L'"') ch = L' ';
    while (!name.empty() && name.back() == L' ') name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    if (name.empty()) return;
    auto names = ProfileNames();
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    SetProfileNames(names);
    const std::wstring sec = ProfileSection(name);
    WritePrivateProfileStringW(sec.c_str(), nullptr, nullptr, IniPath().c_str());  // drop old values
    WriteSettings(sec.c_str(), GetSettings(), false);
    FillProfileCombo(name);
    g_note = L"Профиль «" + name + L"» сохранён.";
    UpdateStatus();
}

static void DeleteProfile(const std::wstring& name) {
    if (name.empty()) return;
    std::wstring q = L"Удалить профиль «" + name + L"»?";
    if (MessageBoxW(g_main, q.c_str(), L"WVC", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    auto names = ProfileNames();
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
    SetProfileNames(names);
    WritePrivateProfileStringW(ProfileSection(name).c_str(), nullptr, nullptr, IniPath().c_str());
    FillProfileCombo(L"");
}

// --- small modal windows (input box, hotkey editor)
static bool RunModal(HWND dlg, const bool* done) {
    EnableWindow(g_main, FALSE);
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
    EnableWindow(g_main, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(g_main);
    if (quit) PostQuitMessage(0);
    return !quit;
}

static HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w,
                      int h, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return c;
}

static HWND MakeDialog(const wchar_t* cls, const wchar_t* title, int cw, int ch) {
    RECT r{0, 0, S(cw), S(ch)};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, g_dpi);
    RECT mr;
    GetWindowRect(g_main, &mr);
    const int w = r.right - r.left, h = r.bottom - r.top;
    return CreateWindowExW(WS_EX_DLGMODALFRAME, cls, title, style, mr.left + ((mr.right - mr.left) - w) / 2,
                           mr.top + ((mr.bottom - mr.top) - h) / 2, w, h, g_main, nullptr, g_hInst, nullptr);
}

struct InputState {
    bool done = false, ok = false;
    std::wstring value;
};
static InputState* g_input = nullptr;

static LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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

static bool InputBox(const wchar_t* title, const wchar_t* prompt, std::wstring* value) {
    InputState st;
    g_input = &st;
    HWND dlg = MakeDialog(L"WVCInput", title, 360, 128);
    if (!dlg) {
        g_input = nullptr;
        return false;
    }
    MakeChild(dlg, L"STATIC", prompt, 0, 11, 14, 12, 332, 20);
    HWND ed = MakeChild(dlg, L"EDIT", value->c_str(), WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 10, 14, 38, 332, 26);
    SendMessageW(ed, EM_LIMITTEXT, 60, 0);
    MakeChild(dlg, L"BUTTON", L"Сохранить", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 140, 82, 100, 30);
    MakeChild(dlg, L"BUTTON", L"Отмена", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 246, 82, 100, 30);
    SetFocus(ed);
    SendMessageW(ed, EM_SETSEL, 0, -1);
    RunModal(dlg, &st.done);
    g_input = nullptr;
    if (st.ok) *value = st.value;
    return st.ok;
}

// Hotkey editor: one msctls_hotkey32 per action
struct HotkeyDlgState {
    bool done = false, ok = false;
    HWND ctl[HA_COUNT] = {};
};
static HotkeyDlgState* g_hkDlg = nullptr;

static WORD ToHotkeyCtl(UINT v) {
    if (!v) return 0;
    const UINT m = HkMods(v);
    BYTE f = 0;
    if (m & MOD_SHIFT) f |= HOTKEYF_SHIFT;
    if (m & MOD_CONTROL) f |= HOTKEYF_CONTROL;
    if (m & MOD_ALT) f |= HOTKEYF_ALT;
    if (IsExtendedVk(HkVk(v))) f |= HOTKEYF_EXT;
    return MAKEWORD(HkVk(v), f);
}

static UINT FromHotkeyCtl(WORD w) {
    const UINT vk = LOBYTE(w), f = HIBYTE(w);
    if (!vk) return 0;
    UINT m = 0;
    if (f & HOTKEYF_SHIFT) m |= MOD_SHIFT;
    if (f & HOTKEYF_CONTROL) m |= MOD_CONTROL;
    if (f & HOTKEYF_ALT) m |= MOD_ALT;
    return HkPack(m, vk);
}

static LRESULT CALLBACK HotkeyDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HotkeyDlgState* st = g_hkDlg;
    if (msg == WM_COMMAND && st) {
        const int id = LOWORD(wp);
        if (id >= 500 && id < 500 + HA_COUNT) {  // clear
            SendMessageW(st->ctl[id - 500], HKM_SETHOTKEY, 0, 0);
        } else if (id == 600) {  // defaults
            for (int i = 0; i < HA_COUNT; ++i)
                SendMessageW(st->ctl[i], HKM_SETHOTKEY, ToHotkeyCtl(HkPack(kHotkeys[i].defMods, kHotkeys[i].defVk)), 0);
        } else if (id == IDOK) {
            UINT v[HA_COUNT];
            for (int i = 0; i < HA_COUNT; ++i)
                v[i] = FromHotkeyCtl(static_cast<WORD>(SendMessageW(st->ctl[i], HKM_GETHOTKEY, 0, 0)));
            for (int i = 0; i < HA_COUNT; ++i)
                for (int j = i + 1; j < HA_COUNT; ++j)
                    if (v[i] && v[i] == v[j]) {
                        std::wstring t = L"Сочетание " + HotkeyName(v[i]) + L" назначено дважды: «" + kHotkeys[i].name +
                                         L"» и «" + kHotkeys[j].name + L"».";
                        MessageBoxW(hwnd, t.c_str(), L"WVC", MB_ICONWARNING);
                        return 0;
                    }
            for (int i = 0; i < HA_COUNT; ++i) g_hk[i] = v[i];
            st->ok = true;
            st->done = true;
        } else if (id == IDCANCEL) {
            st->done = true;
        }
        return 0;
    }
    if (msg == WM_CLOSE) {
        if (st) st->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EditHotkeys() {
    HotkeyDlgState st;
    g_hkDlg = &st;
    const int rowH = 30, top = 44;
    const int ch = top + HA_COUNT * rowH + 56;
    HWND dlg = MakeDialog(L"WVCHotkeys", L"Горячие клавиши", 470, ch);
    if (!dlg) {
        g_hkDlg = nullptr;
        return;
    }
    UnregisterHotkeys(g_main);  // otherwise pressing a current combo would fire it instead of being recorded
    MakeChild(dlg, L"STATIC",
              L"Щёлкните в поле и нажмите сочетание (Ctrl, Shift или Alt + клавиша). ✕ — убрать.", 0, 1, 14, 10,
              442, 30);
    for (int i = 0; i < HA_COUNT; ++i) {
        const int y = top + i * rowH;
        MakeChild(dlg, L"STATIC", kHotkeys[i].name, 0, 2, 14, y + 4, 210, 20);
        st.ctl[i] = MakeChild(dlg, HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, 400 + i, 228, y, 190, 24);
        SendMessageW(st.ctl[i], HKM_SETRULES, HKCOMB_NONE | HKCOMB_S, MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_SHIFT, 0));
        SendMessageW(st.ctl[i], HKM_SETHOTKEY, ToHotkeyCtl(g_hk[i]), 0);
        MakeChild(dlg, L"BUTTON", L"✕", WS_TABSTOP | BS_PUSHBUTTON, 500 + i, 424, y, 32, 24);
    }
    const int by = top + HA_COUNT * rowH + 14;
    MakeChild(dlg, L"BUTTON", L"По умолчанию", WS_TABSTOP | BS_PUSHBUTTON, 600, 14, by, 130, 30);
    MakeChild(dlg, L"BUTTON", L"Сохранить", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 250, by, 100, 30);
    MakeChild(dlg, L"BUTTON", L"Отмена", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 356, by, 100, 30);
    RunModal(dlg, &st.done);
    g_hkDlg = nullptr;
    if (st.ok) SaveHotkeys();
    RegisterHotkeys(g_main);
    SetTextIfChanged(Ctl(IDC_HK_LIST), HotkeyListText());
    if (!g_hotkeyNote.empty()) MessageBoxW(g_main, g_hotkeyNote.c_str(), L"WVC", MB_ICONWARNING);
}

static void OnHotkey(int action) {
    switch (action) {
    case HA_TOGGLE: ToggleStreaming(); return;
    case HA_SELECT: DoSelectRegion(); return;
    case HA_MONITOR: NextMonitor(); return;
    case HA_CURSOR: SetCursorOption(!GetSettings().cursor); return;
    case HA_SCREEN_CAM:
    case HA_PIP:
    case HA_TEXT: {
        {
            std::lock_guard<std::mutex> lk(g_setMx);
            if (action == HA_SCREEN_CAM) g_set.view = g_set.view == 1 ? 0 : 1;
            if (action == HA_PIP) g_set.view = g_set.view == 2 ? 0 : 2;
            if (action == HA_TEXT) g_set.textOn = !g_set.textOn;
        }
        SaveSettings();
        SyncControls();
        WebcamSync();
        UpdateFrame();
        UpdateStatus();
        return;
    }
    default:
        if (action >= HA_PROFILE1 && action <= HA_PROFILE4) LoadProfileByIndex(action - HA_PROFILE1);
    }
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

static HWND AddCombo(HWND hwnd, int id, std::initializer_list<const wchar_t*> items) {
    HWND h = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                             0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    for (auto* s : items) SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    return h;
}

static void CreateControls(HWND hwnd) {
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    };
    HWND tab = mk(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TAB);
    const wchar_t* tabs[] = {L"Источник и картинка", L"Камера и наложения", L"Профили и клавиши"};
    for (int i = 0; i < 3; ++i) {
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<wchar_t*>(tabs[i]);
        SendMessageW(tab, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&ti));
    }

    // page 0
    mk(L"STATIC", L"Источник:", 0, IDC_SRC_LABEL);
    mk(L"STATIC", L"", SS_ENDELLIPSIS, IDC_SRC_VALUE);
    mk(L"BUTTON", L"Область…", WS_TABSTOP | BS_PUSHBUTTON, IDC_SELECT);
    mk(L"BUTTON", L"Монитор ▾", WS_TABSTOP | BS_PUSHBUTTON, IDC_MONITOR);
    mk(L"BUTTON", L"Окно ▾", WS_TABSTOP | BS_PUSHBUTTON, IDC_WINDOW);
    mk(L"STATIC", L"Качество:", 0, IDC_Q_LABEL);
    AddCombo(hwnd, IDC_QUALITY, {L"720p", L"1080p"});
    mk(L"STATIC", L"Соотношение сторон:", 0, IDC_A_LABEL);
    AddCombo(hwnd, IDC_ASPECT, {L"16:9 (обычное)", L"4:3", L"1:1 (квадрат)", L"9:16 (вертикальное)"});
    mk(L"STATIC", L"Частота кадров:", 0, IDC_FPS_LABEL);
    AddCombo(hwnd, IDC_FPS, {L"30 кадров/с", L"60 кадров/с"});
    mk(L"STATIC", L"Если пропорции другие:", 0, IDC_FIT_LABEL);
    AddCombo(hwnd, IDC_FIT, {L"Вписать с полями", L"Обрезать по краям"});
    mk(L"STATIC", L"Поворот:", 0, IDC_ROT_LABEL);
    AddCombo(hwnd, IDC_ROT, {L"Без поворота", L"90° по часовой", L"180°", L"90° против часовой"});
    mk(L"STATIC", L"Увеличение (центр):", 0, IDC_ZOOM_LABEL);
    AddCombo(hwnd, IDC_ZOOM, {L"100%", L"125%", L"150%", L"200%", L"300%", L"400%"});
    mk(L"BUTTON", L"Зеркально", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_MIRROR);
    mk(L"BUTTON", L"Показывать курсор", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_CURSOR);

    // page 1
    mk(L"STATIC", L"Показывать:", 0, IDC_VIEW_LABEL);
    AddCombo(hwnd, IDC_VIEW, {L"Экран", L"Веб-камеру", L"Экран + камера в углу"});
    mk(L"STATIC", L"Веб-камера:", 0, IDC_CAM_LABEL);
    AddCombo(hwnd, IDC_CAMDEV, {});
    mk(L"STATIC", L"Окошко камеры:", 0, IDC_PIP_LABEL);
    AddCombo(hwnd, IDC_PIPCORNER, {L"Справа внизу", L"Слева внизу", L"Справа вверху", L"Слева вверху"});
    AddCombo(hwnd, IDC_PIPSIZE, {L"20%", L"25%", L"33%"});
    mk(L"BUTTON", L"Зеркалить веб-камеру (как в зеркале)", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_CAMMIRROR);
    mk(L"STATIC", L"Надпись:", 0, IDC_TEXT_LABEL);
    HWND ed = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_TEXT, WS_EX_CLIENTEDGE);
    SendMessageW(ed, EM_LIMITTEXT, 120, 0);
    mk(L"BUTTON", L"Показывать надпись", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_TEXTON);
    mk(L"STATIC", L"Где и какого размера:", 0, IDC_TPOS_LABEL);
    AddCombo(hwnd, IDC_TEXTPOS, {L"Слева вверху", L"По центру вверху", L"Справа вверху", L"Слева внизу",
                                 L"По центру внизу", L"Справа внизу"});
    AddCombo(hwnd, IDC_TEXTSIZE, {L"Мелкий", L"Средний", L"Крупный"});
    mk(L"STATIC", L"Логотип (PNG):", 0, IDC_LOGO_LABEL);
    mk(L"BUTTON", L"Выбрать…", WS_TABSTOP | BS_PUSHBUTTON, IDC_LOGO_PICK);
    mk(L"BUTTON", L"Убрать", WS_TABSTOP | BS_PUSHBUTTON, IDC_LOGO_CLEAR);
    mk(L"STATIC", L"Угол и размер логотипа:", 0, IDC_LPOS_LABEL);
    AddCombo(hwnd, IDC_LOGOCORNER, {L"Справа внизу", L"Слева внизу", L"Справа вверху", L"Слева вверху"});
    AddCombo(hwnd, IDC_LOGOSIZE, {L"10%", L"15%", L"20%", L"30%"});
    mk(L"STATIC", L"", SS_ENDELLIPSIS, IDC_LOGO_NAME);

    // page 2
    mk(L"STATIC", L"Профиль:", 0, IDC_PROF_LABEL);
    AddCombo(hwnd, IDC_PROFILE, {});
    mk(L"BUTTON", L"Загрузить", WS_TABSTOP | BS_PUSHBUTTON, IDC_PROF_LOAD);
    mk(L"BUTTON", L"Сохранить…", WS_TABSTOP | BS_PUSHBUTTON, IDC_PROF_SAVE);
    mk(L"BUTTON", L"Удалить", WS_TABSTOP | BS_PUSHBUTTON, IDC_PROF_DEL);
    mk(L"STATIC",
       L"Профиль запоминает источник, поворот, масштаб, веб-камеру, надпись, логотип и паузу. Качество и частота "
       L"кадров в профиль не входят: их смена требует переподключения камеры.",
       0, IDC_PROF_HINT);
    mk(L"BUTTON", L"Настроить горячие клавиши…", WS_TABSTOP | BS_PUSHBUTTON, IDC_HK_EDIT);
    mk(L"STATIC", L"", 0, IDC_HK_LIST);

    // right column
    mk(L"STATIC", L"Предпросмотр (то, что видят другие):", 0, IDC_PREV_LABEL);
    mk(L"STATIC", L"", SS_RIGHT, IDC_FPS_VALUE);
    g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"WVCPreview", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PREVIEW)), g_hInst, nullptr);
    mk(L"BUTTON", L"▶  Старт", WS_TABSTOP | BS_PUSHBUTTON, IDC_START);
    mk(L"STATIC", L"", 0, IDC_STATUS);
    mk(L"STATIC", L"На паузе показывать:", 0, IDC_PAUSE_LABEL);
    AddCombo(hwnd, IDC_PAUSE, {L"Последний кадр", L"Чёрный экран", L"«Скоро вернусь»", L"Свою картинку"});
    mk(L"BUTTON", L"Выбрать картинку…", WS_TABSTOP | BS_PUSHBUTTON, IDC_PAUSE_IMG);
    mk(L"BUTTON", L"Установить камеру", WS_TABSTOP | BS_PUSHBUTTON, IDC_INSTALL);
    mk(L"BUTTON", L"Удалить камеру", WS_TABSTOP | BS_PUSHBUTTON, IDC_UNINSTALL);
}

static void ResizeForDpi(HWND hwnd) {
    RECT r{0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), g_dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// one combo / checkbox / edit changed -> settings
static void OnControlChanged(int id) {
    if (g_syncing) return;
    bool format = false, pause = false, webcam = false;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        Settings& s = g_set;
        const int sel = (std::max)(0, static_cast<int>(SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0)));
        switch (id) {
        case IDC_QUALITY: format = sel != s.quality; s.quality = (std::min)(sel, 1); break;
        case IDC_ASPECT: format = sel != s.aspect; s.aspect = (std::min)(sel, 3); break;
        case IDC_FPS: {
            const int f = sel == 1 ? 60 : 30;
            format = f != s.fps;
            s.fps = f;
            break;
        }
        case IDC_FIT: s.fit = sel == 1 ? 1 : 0; break;
        case IDC_ROT: s.rotation = sel & 3; break;
        case IDC_ZOOM: s.zoom = (std::min)(sel, kZoomCount - 1); break;
        case IDC_MIRROR: s.mirror = CheckGet(id); break;
        case IDC_CURSOR: s.cursor = CheckGet(id); break;
        case IDC_VIEW: s.view = (std::min)(sel, 2); webcam = true; break;
        case IDC_CAMDEV:
            if (sel >= 1 && sel <= static_cast<int>(g_camList.size())) {
                s.camLink = g_camList[sel - 1].link;
                s.camName = g_camList[sel - 1].name;
            } else {
                s.camLink.clear();
                s.camName.clear();
            }
            webcam = true;
            break;
        case IDC_PIPCORNER: s.pipCorner = (std::min)(sel, 3); break;
        case IDC_PIPSIZE: s.pipSize = (std::min)(sel, 2); break;
        case IDC_CAMMIRROR: s.camMirror = CheckGet(id); break;
        case IDC_TEXT: {
            wchar_t buf[256] = {};
            GetWindowTextW(Ctl(IDC_TEXT), buf, 256);
            s.text = buf;
            break;
        }
        case IDC_TEXTON: s.textOn = CheckGet(id); break;
        case IDC_TEXTPOS: s.textPos = (std::min)(sel, 5); break;
        case IDC_TEXTSIZE: s.textSize = (std::min)(sel, 2); break;
        case IDC_LOGOCORNER: s.logoCorner = (std::min)(sel, 3); break;
        case IDC_LOGOSIZE: s.logoSize = (std::min)(sel, 3); break;
        case IDC_PAUSE: s.pauseMode = (std::min)(sel, 3); pause = true; break;
        default: return;
        }
    }
    SaveSettings();
    if (format) RestartCameraForFormat();
    if (pause) {
        g_pauseDirty = true;
        if (GetSettings().pauseMode == 3 && GetSettings().pauseImage.empty()) ChoosePauseImage();
    }
    if (webcam) {
        WebcamSync();
        UpdateFrame();
        UpdateStatus();
    }
}

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
        SyncControls();
        FillProfileCombo(L"");
        SetTextIfChanged(Ctl(IDC_HK_LIST), HotkeyListText());
        ShowPage(0);
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
    case WM_CTLCOLORSTATIC: {
        // controls sitting on the (white, themed) tab page get a matching background
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (OnTabPage(id) && IsAppThemed()) {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    }
    case WM_NOTIFY: {
        const NMHDR* nh = reinterpret_cast<const NMHDR*>(lp);
        if (nh->idFrom == IDC_TAB && nh->code == TCN_SELCHANGE)
            ShowPage(static_cast<int>(SendMessageW(Ctl(IDC_TAB), TCM_GETCURSEL, 0, 0)));
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
            WebcamSync();
            if (g_streaming && GetSettings().srcMode == 1) UpdateFrame();
        }
        return 0;
    case WM_HOTKEY:
        if (wp >= 1 && wp <= HA_COUNT) OnHotkey(static_cast<int>(wp) - 1);
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
        case IDC_PAUSE_IMG: ChoosePauseImage(); break;
        case IDC_LOGO_PICK: ChooseLogo(); break;
        case IDC_LOGO_CLEAR: {
            {
                std::lock_guard<std::mutex> lk(g_setMx);
                g_set.logo.clear();
            }
            SaveSettings();
            SyncControls();
            break;
        }
        case IDC_PROF_LOAD: {
            std::wstring n = SelectedProfile();
            if (n.empty())
                MessageBoxW(hwnd, L"Профилей пока нет: настройте всё и нажмите «Сохранить…».", L"WVC",
                            MB_ICONINFORMATION);
            else
                LoadProfile(n);
            break;
        }
        case IDC_PROF_SAVE: {
            std::wstring n = SelectedProfile();
            if (n.empty()) n = L"Профиль " + std::to_wstring(ProfileNames().size() + 1);
            if (InputBox(L"Сохранить профиль", L"Название профиля (то же название — перезаписать):", &n))
                SaveProfile(n);
            break;
        }
        case IDC_PROF_DEL: DeleteProfile(SelectedProfile()); break;
        case IDC_HK_EDIT: EditHotkeys(); break;
        case IDC_INSTALL: DoInstall(true); break;
        case IDC_UNINSTALL: DoInstall(false); break;
        case IDC_CAMDEV:
            if (code == CBN_DROPDOWN) {
                g_syncing = true;
                FillCamCombo();
                g_syncing = false;
            } else if (code == CBN_SELCHANGE) {
                OnControlChanged(id);
            }
            break;
        case IDC_TEXT:
            if (code == EN_CHANGE) OnControlChanged(id);
            break;
        case IDC_MIRROR:
        case IDC_CURSOR:
        case IDC_CAMMIRROR:
        case IDC_TEXTON:
            if (code == BN_CLICKED) OnControlChanged(id);
            break;
        default:
            if (code == CBN_SELCHANGE) OnControlChanged(id);
            break;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        UnregisterHotkeys(hwnd);
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // file dialog, WIC
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&icc);

    EnsureUnicodeIni();
    {
        Settings s;
        ReadSettings(kMainSec, s, true);
        if (s.srcMode == 1) {  // window handles do not survive a restart: find it again by title
            s.srcWindow = FindWindowByTitle(s.srcTitle);
            if (!s.srcWindow) s.srcMode = 0;
        }
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set = s;
    }
    LoadHotkeys();

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

    WNDCLASSW dc{};
    dc.hInstance = hInst;
    dc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    dc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    dc.lpfnWndProc = InputProc;
    dc.lpszClassName = L"WVCInput";
    RegisterClassW(&dc);
    dc.lpfnWndProc = HotkeyDlgProc;
    dc.lpszClassName = L"WVCHotkeys";
    RegisterClassW(&dc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WVCMain";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"WVCMain", L"WVC — камера из области экрана",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                840, 520, nullptr, nullptr, hInst, nullptr);
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
    WebcamStop();
    StopCamera();
    if (g_frame) DestroyWindow(g_frame);
    if (g_font) DeleteObject(g_font);
    if (g_frameFont) DeleteObject(g_frameFont);
    MFShutdown();
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
