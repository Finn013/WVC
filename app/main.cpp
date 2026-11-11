// WVC — лёгкая виртуальная камера, которая показывает выбранную область экрана.
// Камера: DirectShow-фильтр на базе tshino/softcam (MIT) в WVCCam.dll / WVCCam32.dll.
// Это приложение: источник (область / монитор / окно / веб-камера), захват GDI,
// поворот/зеркало/зум, надпись и логотип поверх кадра, картинка-в-картинке, телефон (WVC Phone по Wi-Fi),
// профили, настраиваемые горячие клавиши, предпросмотр, автовосстановление.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
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
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
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
    std::wstring pauseText = L"Скоро вернусь";  // text on the pause card
    // source
    int  srcMode = 0;      // 0 = region, 1 = window
    HWND srcWindow = nullptr;
    std::wstring srcTitle;
    bool hasRegion = false;
    RECT region{};         // virtual-screen coordinates, physical pixels
    int  winCapture = 0;   // window source: 0 = from the screen, 1 = straight from the window (PrintWindow)
    RECT region2{};        // "Экран 2": a second fixed area (virtual-screen coordinates)
    // what is shown
    int  mainSrc = 0;      // big picture: 0 = screen 1, 1 = webcam, 2 = media files, 3 = screen 2, 4 = phone
    int  pipSrc = 2;       // second source: 1 = screen 1, 2 = webcam, 3 = media, 4 = screen 2, 5 = phone (= kind + 1)
    int  layout = 0;       // 0 = only the main source, 1 = small window on top, 2 = side by side
    std::wstring camLink, camName;
    bool camMirror = true;
    int  pipCorner = 0;    // last preset: 0 = bottom right, 1 = bottom left, 2 = top right, 3 = top left
    int  pipSize = 1;      // last preset: index into kPipSize
    int  pipX = -1, pipY = -1, pipW = 2500;  // small window: top-left and width, 1/10000 of the frame
    int  pipShape = 0;     // kShapes
    int  frameShape = 0;   // kShapes, for the whole frame
    int  shapeColor = 0;   // colour outside a shape, index into kShapeColors
    // media files
    std::vector<std::wstring> media;
    int  imageSec = 1;     // index into kImageSec
    // overlays
    bool textOn = false;
    std::wstring text;
    int  textPos = 3;      // 0 TL, 1 TC, 2 TR, 3 BL, 4 BC, 5 BR
    int  textSize = 1;     // 0 small, 1 medium, 2 large
    std::wstring logo;     // empty = no logo
    int  logoCorner = 2;   // same as pipCorner
    int  logoSize = 1;     // index into kLogoSize
    // app-wide (not part of scenes)
    int  transition = 1;   // 0 none, 1 dissolve, 2 through black, 3 wipe, 4 slide
    int  transDur = 1;     // index into kTransMs
    bool idleFrame = true; // green frame around the source while not streaming
    bool keepCamOn = false; // keep the webcam open all the time (instant switching)
    std::wstring phoneIp;  // WVC Phone: last IP address
    int  phoneSrc = 0;     // what the phone sends: 0 back camera, 1 front camera, 2 its screen
    int  phoneRes = 0;     // 0 = 720p, 1 = 1080p (long side)
};

static const double kZoom[] = {1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
static const int    kZoomCount = 6;
static const double kPipSize[] = {0.20, 0.25, 0.33};
static const double kLogoSize[] = {0.10, 0.15, 0.20, 0.30};
static const int    kImageSec[] = {3, 5, 10, 30, 60};
static const int    kTransMs[] = {300, 500, 1000};
// 0 rectangle, 1 rounded, 2 circle, 3 hexagon, 4 octagon
static const int    kShapeCount = 5;
static const COLORREF kShapeColors[] = {RGB(0, 0, 0), RGB(255, 255, 255), RGB(0, 177, 64), RGB(40, 42, 48)};
static bool SquareShape(int shape) { return shape >= 2; }

// second source actually shown: 0 = none, else 1..4 (source kind + 1)
static int SecondSrc(const Settings& s) { return s.layout == 0 ? 0 : std::clamp(s.pipSrc, 1, 5); }
static bool ScreenNeeded(const Settings& s) { return s.mainSrc == 0 || SecondSrc(s) == 1; }
static bool Screen2Needed(const Settings& s) { return s.mainSrc == 3 || SecondSrc(s) == 4; }
static bool CamNeeded(const Settings& s) { return s.mainSrc == 1 || SecondSrc(s) == 2; }
static bool MediaNeeded(const Settings& s) { return s.mainSrc == 2 || SecondSrc(s) == 3; }
static bool PhoneNeeded(const Settings& s) { return s.mainSrc == 4 || SecondSrc(s) == 5; }

// Small window rectangle in output pixels (16:9, or square for circle/hexagon/octagon)
static RECT PipRect(const Settings& s, int W, int H) {
    int pw = (std::max)(32, MulDiv(W, std::clamp(s.pipW, 500, 9000), 10000));
    int ph = SquareShape(s.pipShape) ? pw : pw * 9 / 16;
    if (ph > H - 8) {
        ph = H - 8;
        pw = SquareShape(s.pipShape) ? ph : ph * 16 / 9;
    }
    const int m = (std::max)(8, H / 30);
    int x = s.pipX < 0 ? W - m - pw : MulDiv(W, s.pipX, 10000);
    int y = s.pipY < 0 ? H - m - ph : MulDiv(H, s.pipY, 10000);
    x = std::clamp(x, 0, W - pw);
    y = std::clamp(y, 0, H - ph);
    return RECT{x, y, x + pw, y + ph};
}

// Snap the small window to a corner, keeping its current width (old settings without a width get the preset)
static void PipPreset(Settings& s, int W, int H) {
    if (s.pipW < 500 || s.pipW > 9000) s.pipW = static_cast<int>(10000 * kPipSize[std::clamp(s.pipSize, 0, 2)]);
    s.pipX = s.pipY = 0;
    RECT r = PipRect(s, W, H);
    const int pw = r.right - r.left, ph = r.bottom - r.top, m = (std::max)(8, H / 30);
    const int x = (s.pipCorner == 1 || s.pipCorner == 3) ? m : W - m - pw;
    const int y = (s.pipCorner == 2 || s.pipCorner == 3) ? m : H - m - ph;
    s.pipX = MulDiv(x, 10000, W);
    s.pipY = MulDiv(y, 10000, H);
}

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

// def when the key is missing (an empty value that is present stays empty)
static std::wstring IniStrDef(const wchar_t* sec, const wchar_t* key, const std::wstring& def) {
    std::vector<wchar_t> buf(4096, 0);
    GetPrivateProfileStringW(sec, key, L"\x01", buf.data(), static_cast<DWORD>(buf.size()), IniPath().c_str());
    if (buf[0] == 1 && buf[1] == 0) return def;
    return buf.data();
}

static RECT PrimaryMonitorRect();
// "Экран 2" when nothing is saved yet: half of the primary monitor, centred
static RECT DefaultRegion2() {
    RECT p = PrimaryMonitorRect();
    const int w = (std::max)(64, static_cast<int>(p.right - p.left) / 2);
    const int h = (std::max)(64, static_cast<int>(p.bottom - p.top) / 2);
    const int x = p.left + (static_cast<int>(p.right - p.left) - w) / 2;
    const int y = p.top + (static_cast<int>(p.bottom - p.top) - h) / 2;
    return RECT{x, y, x + w, y + h};
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
        s.transition = std::clamp(IniInt(sec, L"Transition", s.transition), 0, 4);
        s.transDur = std::clamp(IniInt(sec, L"TransitionTime", s.transDur), 0, 2);
        s.idleFrame = IniInt(sec, L"IdleFrame", s.idleFrame) != 0;
        s.keepCamOn = IniInt(sec, L"KeepCamOn", s.keepCamOn) != 0;
        s.phoneIp = IniStr(sec, L"PhoneIp");
        s.phoneSrc = std::clamp(IniInt(sec, L"PhoneSrc", s.phoneSrc), 0, 2);
        s.phoneRes = std::clamp(IniInt(sec, L"PhoneRes", s.phoneRes), 0, 1);
    }
    s.cursor = IniInt(sec, L"Cursor", s.cursor) != 0;
    s.fit = std::clamp(IniInt(sec, L"Fit", s.fit), 0, 1);
    s.rotation = std::clamp(IniInt(sec, L"Rotation", s.rotation), 0, 3);
    s.mirror = IniInt(sec, L"Mirror", s.mirror) != 0;
    s.zoom = std::clamp(IniInt(sec, L"Zoom", s.zoom), 0, kZoomCount - 1);
    s.pauseMode = std::clamp(IniInt(sec, L"PauseMode", s.pauseMode), 0, 3);
    s.pauseImage = IniStr(sec, L"PauseImage");
    s.pauseText = IniStrDef(sec, L"PauseText", s.pauseText);
    s.srcMode = std::clamp(IniInt(sec, L"SourceMode", 0), 0, 1);
    s.srcTitle = IniStr(sec, L"WindowTitle");
    s.srcWindow = nullptr;
    s.hasRegion = IniInt(sec, L"HasRegion", s.hasRegion) != 0;
    s.region.left = IniInt(sec, L"Left", s.region.left);
    s.region.top = IniInt(sec, L"Top", s.region.top);
    s.region.right = IniInt(sec, L"Right", s.region.right);
    s.region.bottom = IniInt(sec, L"Bottom", s.region.bottom);
    if (s.region.right - s.region.left < 16 || s.region.bottom - s.region.top < 16) s.hasRegion = false;
    s.winCapture = std::clamp(IniInt(sec, L"WindowCapture", s.winCapture), 0, 1);
    {  // v0.7: Region2X/Y/W/H; missing (old settings / old scenes) -> keep what we have or the default
        const int w2 = IniInt(sec, L"Region2W", -1), h2 = IniInt(sec, L"Region2H", -1);
        if (w2 >= 16 && h2 >= 16) {
            const int x2 = IniInt(sec, L"Region2X", 0), y2 = IniInt(sec, L"Region2Y", 0);
            s.region2 = RECT{x2, y2, x2 + w2, y2 + h2};
        }
        if (s.region2.right - s.region2.left < 16 || s.region2.bottom - s.region2.top < 16) s.region2 = DefaultRegion2();
    }
    const int oldView = IniInt(sec, L"View", -1);  // v0.3
    s.mainSrc = IniInt(sec, L"MainSrc", -1);
    int rawPip = IniInt(sec, L"PipSrc", -1);       // v0.4-0.6: 0 = no small window
    if (s.mainSrc < 0 || rawPip < 0) {
        s.mainSrc = oldView == 1 ? 1 : 0;
        rawPip = oldView == 2 ? 2 : 0;
    }
    s.mainSrc = std::clamp(s.mainSrc, 0, 4);
    rawPip = std::clamp(rawPip, 0, 5);
    const int lay = IniInt(sec, L"Layout", -1);     // v0.7
    const int second = IniInt(sec, L"SecondSrc", -1);
    s.layout = lay < 0 ? (rawPip ? 1 : 0) : std::clamp(lay, 0, 2);
    s.pipSrc = second >= 1 ? std::clamp(second, 1, 5) : rawPip >= 1 ? rawPip : (s.mainSrc == 1 ? 1 : 2);
    s.camLink = IniStr(sec, L"CamLink");
    s.camName = IniStr(sec, L"CamName");
    s.camMirror = IniInt(sec, L"CamMirror", s.camMirror) != 0;
    s.pipCorner = std::clamp(IniInt(sec, L"PipCorner", s.pipCorner), 0, 3);
    s.pipSize = std::clamp(IniInt(sec, L"PipSize", s.pipSize), 0, 2);
    s.pipX = IniInt(sec, L"PipX", -1);
    s.pipY = IniInt(sec, L"PipY", -1);
    s.pipW = IniInt(sec, L"PipW", -1);
    if (s.pipX < 0 || s.pipY < 0 || s.pipW < 0) {  // v0.3 settings: start from the corner preset
        int W, H;
        OutSize(s.quality, s.aspect, &W, &H);
        PipPreset(s, W, H);
    }
    s.pipX = std::clamp(s.pipX, 0, 10000);
    s.pipY = std::clamp(s.pipY, 0, 10000);
    s.pipW = std::clamp(s.pipW, 500, 9000);
    s.pipShape = std::clamp(IniInt(sec, L"PipShape", s.pipShape), 0, kShapeCount - 1);
    s.frameShape = std::clamp(IniInt(sec, L"FrameShape", s.frameShape), 0, kShapeCount - 1);
    s.shapeColor = std::clamp(IniInt(sec, L"ShapeColor", s.shapeColor), 0, 3);
    s.imageSec = std::clamp(IniInt(sec, L"ImageSeconds", s.imageSec), 0, 4);
    s.media.clear();
    const int mc = std::clamp(IniInt(sec, L"MediaCount", 0), 0, 500);
    for (int i = 0; i < mc; ++i) {
        wchar_t k[24];
        swprintf(k, 24, L"Media%d", i);
        std::wstring f = IniStr(sec, k);
        if (!f.empty()) s.media.push_back(f);
    }
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
        IniSetInt(sec, L"Transition", s.transition);
        IniSetInt(sec, L"TransitionTime", s.transDur);
        IniSetInt(sec, L"IdleFrame", s.idleFrame ? 1 : 0);
        IniSetInt(sec, L"KeepCamOn", s.keepCamOn ? 1 : 0);
        IniSetStr(sec, L"PhoneIp", s.phoneIp);
        IniSetInt(sec, L"PhoneSrc", s.phoneSrc);
        IniSetInt(sec, L"PhoneRes", s.phoneRes);
    }
    IniSetInt(sec, L"Cursor", s.cursor ? 1 : 0);
    IniSetInt(sec, L"Fit", s.fit);
    IniSetInt(sec, L"Rotation", s.rotation);
    IniSetInt(sec, L"Mirror", s.mirror ? 1 : 0);
    IniSetInt(sec, L"Zoom", s.zoom);
    IniSetInt(sec, L"PauseMode", s.pauseMode);
    IniSetStr(sec, L"PauseImage", s.pauseImage);
    IniSetStr(sec, L"PauseText", s.pauseText);
    IniSetInt(sec, L"SourceMode", s.srcMode);
    IniSetStr(sec, L"WindowTitle", s.srcTitle);
    IniSetInt(sec, L"HasRegion", s.hasRegion ? 1 : 0);
    IniSetInt(sec, L"Left", s.region.left);
    IniSetInt(sec, L"Top", s.region.top);
    IniSetInt(sec, L"Right", s.region.right);
    IniSetInt(sec, L"Bottom", s.region.bottom);
    IniSetInt(sec, L"WindowCapture", s.winCapture);
    IniSetInt(sec, L"Region2X", s.region2.left);
    IniSetInt(sec, L"Region2Y", s.region2.top);
    IniSetInt(sec, L"Region2W", s.region2.right - s.region2.left);
    IniSetInt(sec, L"Region2H", s.region2.bottom - s.region2.top);
    IniSetInt(sec, L"MainSrc", s.mainSrc);
    IniSetInt(sec, L"PipSrc", SecondSrc(s));  // what v0.6 understood: 0 = none
    IniSetInt(sec, L"SecondSrc", std::clamp(s.pipSrc, 1, 5));
    IniSetInt(sec, L"Layout", s.layout);
    WritePrivateProfileStringW(sec, L"View", nullptr, IniPath().c_str());
    IniSetStr(sec, L"CamLink", s.camLink);
    IniSetStr(sec, L"CamName", s.camName);
    IniSetInt(sec, L"CamMirror", s.camMirror ? 1 : 0);
    IniSetInt(sec, L"PipCorner", s.pipCorner);
    IniSetInt(sec, L"PipSize", s.pipSize);
    IniSetInt(sec, L"PipX", s.pipX);
    IniSetInt(sec, L"PipY", s.pipY);
    IniSetInt(sec, L"PipW", s.pipW);
    IniSetInt(sec, L"PipShape", s.pipShape);
    IniSetInt(sec, L"FrameShape", s.frameShape);
    IniSetInt(sec, L"ShapeColor", s.shapeColor);
    IniSetInt(sec, L"ImageSeconds", s.imageSec);
    IniSetInt(sec, L"MediaCount", static_cast<int>(s.media.size()));
    for (size_t i = 0; i < s.media.size(); ++i) {
        wchar_t k[24];
        swprintf(k, 24, L"Media%d", static_cast<int>(i));
        IniSetStr(sec, k, s.media[i]);
    }
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

// "Экран 2": always a fixed area
static bool ResolveSource2(const Settings& s, RECT* r) {
    if (s.region2.right - s.region2.left < 16 || s.region2.bottom - s.region2.top < 16) return false;
    *r = s.region2;
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

struct FrameSource {
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
static FrameSource g_web;    // real webcam
static FrameSource g_media;  // media files player

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

// Called on a timer from the UI thread: opens the webcam only while it is shown (or always with KeepCamOn).
static std::atomic<bool> g_streaming{false};
static std::atomic<bool> g_paused{false};   // streaming, but the camera shows the pause picture
static std::atomic<int>  g_transGen{0};     // bumped by the UI: source / scene changed -> transition
static void WebcamSync() {
    Settings s = GetSettings();
    const bool want = ((g_streaming && CamNeeded(s)) || s.keepCamOn) && !s.camLink.empty();
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
// Media files player: pictures, animated GIF, video (Media Foundation, no sound)
// ---------------------------------------------------------------------------
static std::atomic<bool> g_mediaSkip{false};
static std::atomic<int>  g_mediaIndex{0};
static std::atomic<int>  g_mediaGen{0};     // bumped when the list changes
static int               g_mediaRunGen = -1;
static std::mutex        g_mediaNameMx;
static std::wstring      g_mediaNow;        // file name on screen now

static void MediaPublish(const uint8_t* bgra, int w, int h) {
    {
        std::lock_guard<std::mutex> lk(g_media.mx);
        g_media.buf.assign(bgra, bgra + static_cast<size_t>(w) * h * 4);
        g_media.w = w;
        g_media.h = h;
    }
    ++g_media.seq;
    g_media.status = 2;
}

static bool MediaWait(ULONGLONG ms) {  // false = stop or skip requested
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        if (g_media.stop || g_mediaSkip) return false;
        Sleep(15);
    }
    return !(g_media.stop || g_mediaSkip);
}

static bool LoadImageScaled(const std::wstring& path, int maxW, int maxH, REFWICPixelFormatGUID fmt,
                            std::vector<uint8_t>& out, int* ow, int* oh);

static std::wstring LowerExt(const std::wstring& f) {
    size_t d = f.find_last_of(L'.');
    std::wstring e = d == std::wstring::npos ? L"" : f.substr(d + 1);
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    return e;
}

static bool IsVideoFile(const std::wstring& f) {
    static const wchar_t* v[] = {L"mp4", L"m4v", L"mov", L"avi", L"wmv", L"mkv", L"webm", L"mpg", L"mpeg", L"ts", L"3gp"};
    const std::wstring e = LowerExt(f);
    for (auto* x : v)
        if (e == x) return true;
    return false;
}

static UINT PropUInt(IWICMetadataQueryReader* q, const wchar_t* name, UINT def) {
    if (!q) return def;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    UINT v = def;
    if (SUCCEEDED(q->GetMetadataByName(name, &pv))) {
        if (pv.vt == VT_UI2) v = pv.uiVal;
        else if (pv.vt == VT_UI1) v = pv.bVal;
        else if (pv.vt == VT_UI4) v = pv.ulVal;
    }
    PropVariantClear(&pv);
    return v;
}

static bool PlayImage(const std::wstring& f, int seconds) {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (!LoadImageScaled(f, 1920, 1920, GUID_WICPixelFormat32bppBGR, px, &w, &h)) return false;
    MediaPublish(px.data(), w, h);
    MediaWait(static_cast<ULONGLONG>(seconds) * 1000);
    return true;
}

static bool PlayGif(const std::wstring& f, int seconds) {
    IWICImagingFactory* fac = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
        SUCCEEDED(fac->CreateDecoderFromFilename(f.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec))) {
        UINT n = 0;
        dec->GetFrameCount(&n);
        IWICMetadataQueryReader* gq = nullptr;
        dec->GetMetadataQueryReader(&gq);
        int W = static_cast<int>(PropUInt(gq, L"/logscrdesc/Width", 0));
        int H = static_cast<int>(PropUInt(gq, L"/logscrdesc/Height", 0));
        SafeRelease(gq);
        std::vector<uint8_t> canvas;
        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
        bool first = true;
        while (n > 0 && !g_media.stop && !g_mediaSkip) {
            for (UINT i = 0; i < n && !g_media.stop && !g_mediaSkip; ++i) {
                IWICBitmapFrameDecode* fr = nullptr;
                IWICFormatConverter* cv = nullptr;
                IWICMetadataQueryReader* q = nullptr;
                if (FAILED(dec->GetFrame(i, &fr))) break;
                fr->GetMetadataQueryReader(&q);
                const int fx = static_cast<int>(PropUInt(q, L"/imgdesc/Left", 0));
                const int fy = static_cast<int>(PropUInt(q, L"/imgdesc/Top", 0));
                UINT delay = PropUInt(q, L"/grctlext/Delay", 10) * 10;
                const UINT disposal = PropUInt(q, L"/grctlext/Disposal", 0);
                if (delay < 20) delay = 100;
                UINT fw = 0, fh = 0;
                fr->GetSize(&fw, &fh);
                if (W <= 0 || H <= 0) {
                    W = static_cast<int>(fw);
                    H = static_cast<int>(fh);
                }
                if (canvas.size() != static_cast<size_t>(W) * H * 4) canvas.assign(static_cast<size_t>(W) * H * 4, 0);
                std::vector<uint8_t> px(static_cast<size_t>(fw) * fh * 4);
                if (SUCCEEDED(fac->CreateFormatConverter(&cv)) &&
                    SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                             WICBitmapPaletteTypeCustom)) &&
                    SUCCEEDED(cv->CopyPixels(nullptr, fw * 4, static_cast<UINT>(px.size()), px.data()))) {
                    std::vector<uint8_t> before;
                    if (disposal == 3) before = canvas;
                    for (UINT y = 0; y < fh; ++y) {
                        const int cy = fy + static_cast<int>(y);
                        if (cy < 0 || cy >= H) continue;
                        for (UINT x = 0; x < fw; ++x) {
                            const int cx = fx + static_cast<int>(x);
                            if (cx < 0 || cx >= W) continue;
                            const uint8_t* sp = px.data() + (static_cast<size_t>(y) * fw + x) * 4;
                            uint8_t* dp = canvas.data() + (static_cast<size_t>(cy) * W + cx) * 4;
                            const int inv = 255 - sp[3];
                            for (int c = 0; c < 3; ++c) dp[c] = static_cast<uint8_t>(sp[c] + dp[c] * inv / 255);
                            dp[3] = 255;
                        }
                    }
                    MediaPublish(canvas.data(), W, H);
                    ok = true;
                    if (disposal == 2) {  // restore to background
                        for (UINT y = 0; y < fh; ++y) {
                            const int cy = fy + static_cast<int>(y);
                            if (cy < 0 || cy >= H) continue;
                            for (UINT x = 0; x < fw; ++x) {
                                const int cx = fx + static_cast<int>(x);
                                if (cx >= 0 && cx < W)
                                    std::memset(canvas.data() + (static_cast<size_t>(cy) * W + cx) * 4, 0, 4);
                            }
                        }
                    } else if (disposal == 3) {
                        canvas.swap(before);
                    }
                }
                SafeRelease(q);
                SafeRelease(cv);
                SafeRelease(fr);
                if (!ok) break;
                MediaWait(delay);
            }
            if (!ok || n == 1 || GetTickCount64() >= until) break;
            if (first) first = false;
        }
        if (n == 1 && ok) MediaWait(static_cast<ULONGLONG>(seconds) * 1000);
    }
    SafeRelease(dec);
    SafeRelease(fac);
    return ok;
}

static bool PlayVideo(const std::wstring& f) {
    IMFAttributes* ra = nullptr;
    IMFSourceReader* rd = nullptr;
    IMFMediaType* outType = nullptr;
    IMFMediaType* cur = nullptr;
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    bool ok = false;
    UINT32 w = 0, h = 0;
    LONG defStride = 0;
    do {
        if (FAILED(MFCreateAttributes(&ra, 1))) break;
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (FAILED(MFCreateSourceReaderFromURL(f.c_str(), ra, &rd))) break;
        rd->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        rd->SetStreamSelection(stream, TRUE);
        if (FAILED(MFCreateMediaType(&outType))) break;
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED(rd->SetCurrentMediaType(stream, nullptr, outType))) break;
        if (FAILED(rd->GetCurrentMediaType(stream, &cur))) break;
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
        UINT32 st = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) defStride = static_cast<LONG>(st);
        ok = w && h;
    } while (false);
    bool played = false;
    if (ok) {
        std::vector<uint8_t> local(static_cast<size_t>(w) * h * 4);
        const ULONGLONG t0 = GetTickCount64();
        LONGLONG firstTs = -1;
        while (!g_media.stop && !g_mediaSkip) {
            DWORD si = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            HRESULT hr = rd->ReadSample(stream, 0, &si, &flags, &ts, &sample);
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                SafeRelease(sample);
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
            if (firstTs < 0) firstTs = ts;
            // pace by the timestamps
            const LONGLONG dueMs = (ts - firstTs) / 10000;
            const LONGLONG nowMs = static_cast<LONGLONG>(GetTickCount64() - t0);
            if (dueMs > nowMs && !MediaWait(static_cast<ULONGLONG>(dueMs - nowMs))) {
                sample->Release();
                break;
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
                    if (len < static_cast<size_t>(std::labs(pitch)) * h) scan0 = nullptr;
                }
                if (scan0) {
                    for (UINT32 y = 0; y < h; ++y)
                        std::memcpy(local.data() + static_cast<size_t>(y) * w * 4,
                                    scan0 + static_cast<ptrdiff_t>(y) * pitch, static_cast<size_t>(w) * 4);
                    MediaPublish(local.data(), static_cast<int>(w), static_cast<int>(h));
                    played = true;
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
    return played;
}

static void MediaThread(std::vector<std::wstring> files, int seconds) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int failedInRow = 0;
    while (!g_media.stop) {
        const int n = static_cast<int>(files.size());
        const int idx = ((g_mediaIndex % n) + n) % n;
        const std::wstring& f = files[idx];
        {
            std::lock_guard<std::mutex> lk(g_mediaNameMx);
            size_t p = f.find_last_of(L"\\/");
            g_mediaNow = p == std::wstring::npos ? f : f.substr(p + 1);
        }
        g_mediaSkip = false;
        bool ok;
        if (IsVideoFile(f)) ok = PlayVideo(f);
        else if (LowerExt(f) == L"gif") ok = PlayGif(f, seconds);
        else ok = PlayImage(f, seconds);
        if (g_media.stop) break;
        if (ok) {
            failedInRow = 0;
        } else if (++failedInRow >= n) {
            g_media.status = 3;  // nothing in the list can be shown
            failedInRow = 0;
            MediaWait(2000);
        }
        g_mediaSkip = false;
        g_mediaIndex = idx + 1;
    }
    CoUninitialize();
}

static void MediaStop() {
    if (g_media.th.joinable()) {
        g_media.stop = true;
        g_media.th.join();
    }
    g_media.status = 0;
}

static void MediaSync() {
    Settings s = GetSettings();
    const bool want = g_streaming && MediaNeeded(s) && !s.media.empty();
    if (g_media.th.joinable() && (!want || g_mediaRunGen != g_mediaGen)) MediaStop();
    if (want && !g_media.th.joinable()) {
        g_media.stop = false;
        g_media.status = 1;
        g_mediaRunGen = g_mediaGen;
        g_media.th = std::thread(MediaThread, s.media, kImageSec[std::clamp(s.imageSec, 0, 4)]);
    }
}

static void MediaNext() {
    g_mediaSkip = true;
}

// ---------------------------------------------------------------------------
// Phone: the WVC Phone app (Android) streams JPEG frames over Wi-Fi.
// The phone is the server (TCP 47800), discovery by UDP broadcast to 47801. See docs/PHONE-PROTOCOL.md.
// Connected only while the source is used during streaming, or after «Подключить».
// ---------------------------------------------------------------------------
static const char* kPhonePort = "47800";
static const u_short kPhoneDiscPort = 47801;
static const char* kPhoneSrcCmd[3] = {"back", "front", "screen"};
static const UINT WM_PHONE_FOUND = WM_APP + 10;  // lParam = std::vector<PhoneFound>*
static const UINT WM_PHONE_STATE = WM_APP + 11;  // the phone reported a new state

struct PhoneFound {
    std::wstring name, ip;
};

static FrameSource       g_phone;                    // link = IP address in use
static std::mutex        g_phoneMx;                  // guards the socket and the strings below
static SOCKET            g_phoneSock = INVALID_SOCKET;
static std::wstring      g_phoneName;                // device model reported by the phone
static std::wstring      g_phoneState;               // back / front / screen / need_consent / error ...
static std::wstring      g_phoneErr;                 // why the last connection failed
static std::atomic<int>  g_phoneFpsX10{0};
static std::atomic<int>  g_phoneUser{0};             // 1 = «Подключить», -1 = «Отключить», 0 = only while used
static std::atomic<bool> g_phoneSearching{false};
static std::atomic<int>  g_phoneResRep{-1};          // quality the phone reported: 0 = 720p, 1 = 1080p
static HWND              g_phoneNotify = nullptr;    // main window, for posted results
static bool              g_wsaOk = false;

static std::string PhoneA(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &r[0], n, nullptr, nullptr);
    return r;
}

static std::wstring PhoneW(const std::string& a) {
    if (a.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), nullptr, 0);
    std::wstring r(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), &r[0], n);
    return r;
}

static std::wstring PhoneStateNow() {
    std::lock_guard<std::mutex> lk(g_phoneMx);
    return g_phoneState;
}

static bool PhoneRecvAll(SOCKET s, void* buf, int n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        const int r = recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

// One command line to the phone ("SRC back", "RES 1080"); false when not connected
static bool PhoneSendLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_phoneMx);
    if (g_phoneSock == INVALID_SOCKET) return false;
    const std::string l = line + "\n";
    return send(g_phoneSock, l.data(), static_cast<int>(l.size()), 0) == static_cast<int>(l.size());
}

static bool PhoneSendSource(int src) { return PhoneSendLine(std::string("SRC ") + kPhoneSrcCmd[std::clamp(src, 0, 2)]); }
static bool PhoneSendRes(int res) { return PhoneSendLine(res == 1 ? "RES 1080" : "RES 720"); }

// TCP connect with a ~3 s timeout that still reacts to a stop request
static SOCKET PhoneConnect(const std::wstring& ip, std::wstring* err) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (getaddrinfo(PhoneA(ip).c_str(), kPhonePort, &hints, &res) != 0 || !res) {
        *err = L"неверный IP-адрес";
        return INVALID_SOCKET;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        *err = L"сеть недоступна";
        return INVALID_SOCKET;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    bool ok = false;
    for (int i = 0; i < 12 && !g_phone.stop; ++i) {  // 12 x 250 ms
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s, &w);
        FD_SET(s, &e);
        timeval tv{0, 250000};
        const int r = select(0, nullptr, &w, &e, &tv);
        if (r < 0 || FD_ISSET(s, &e)) {
            *err = L"телефон не принимает подключение";
            break;
        }
        if (r > 0 && FD_ISSET(s, &w)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        if (err->empty()) *err = L"телефон не отвечает";
        closesocket(s);
        return INVALID_SOCKET;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
    DWORD to = 7000;  // the phone sends its state every 2 s, so silence means the link is gone
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    int rb = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rb), sizeof(rb));
    return s;
}

// JPEG bytes -> BGRA top-down, rotated clockwise by rot * 90 degrees
static bool PhoneDecodeJpeg(IWICImagingFactory* f, const uint8_t* data, size_t n, int rot, std::vector<uint8_t>& out,
                            int* ow, int* oh) {
    if (!f || !data || n < 4) return false;
    bool ok = false;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapFlipRotator* fl = nullptr;
    IWICFormatConverter* cv = nullptr;
    do {
        if (FAILED(f->CreateStream(&st))) break;
        if (FAILED(st->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(n)))) break;
        if (FAILED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        IWICBitmapSource* src = fr;
        if (rot & 3) {
            const WICBitmapTransformOptions t = (rot & 3) == 1   ? WICBitmapTransformRotate90
                                                : (rot & 3) == 2 ? WICBitmapTransformRotate180
                                                                 : WICBitmapTransformRotate270;
            if (FAILED(f->CreateBitmapFlipRotator(&fl)) || FAILED(fl->Initialize(fr, t))) break;
            src = fl;
        }
        if (FAILED(f->CreateFormatConverter(&cv))) break;
        if (FAILED(cv->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                  WICBitmapPaletteTypeCustom)))
            break;
        UINT w = 0, h = 0;
        cv->GetSize(&w, &h);
        if (!w || !h || w > 8192 || h > 8192) break;
        out.resize(static_cast<size_t>(w) * h * 4);
        if (FAILED(cv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size()), out.data()))) break;
        *ow = static_cast<int>(w);
        *oh = static_cast<int>(h);
        ok = true;
    } while (false);
    SafeRelease(cv);
    SafeRelease(fl);
    SafeRelease(fr);
    SafeRelease(dec);
    SafeRelease(st);
    return ok;
}

static void PhoneHandleText(const std::string& t) {
    if (t.rfind("STATE ", 0) == 0) {
        const std::wstring st = PhoneW(t.substr(6));
        bool changed;
        {
            std::lock_guard<std::mutex> lk(g_phoneMx);
            changed = st != g_phoneState;
            g_phoneState = st;
        }
        if (st != L"back" && st != L"front" && st != L"screen") {  // no picture now: show the card, not an old frame
            std::lock_guard<std::mutex> lk(g_phone.mx);
            g_phone.buf.clear();
            ++g_phone.seq;
        }
        if (changed && g_phoneNotify) PostMessageW(g_phoneNotify, WM_PHONE_STATE, 0, 0);
    } else if (t.rfind("RES ", 0) == 0) {
        const int r = std::atoi(t.c_str() + 4) >= 1080 ? 1 : 0;
        if (g_phoneResRep.exchange(r) != r && g_phoneNotify) PostMessageW(g_phoneNotify, WM_PHONE_STATE, 0, 0);
    } else if (t.rfind("NAME ", 0) == 0) {
        std::lock_guard<std::mutex> lk(g_phoneMx);
        g_phoneName = PhoneW(t.substr(5));
    }
}

static void PhoneThread(std::wstring ip, int src, int res) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    std::wstring err;
    SOCKET s = PhoneConnect(ip, &err);
    if (s != INVALID_SOCKET) {
        std::lock_guard<std::mutex> lk(g_phoneMx);
        if (g_phone.stop) {
            closesocket(s);
            s = INVALID_SOCKET;
        } else {
            g_phoneSock = s;
            g_phoneErr.clear();
            g_phoneResRep = res;
        }
    }
    if (s != INVALID_SOCKET) {
        g_phone.status = 2;
        PhoneSendSource(src);
        PhoneSendRes(res);
        std::vector<uint8_t> jpg, px;
        int frames = 0;
        ULONGLONG t0 = GetTickCount64();
        err = L"связь прервалась";
        while (!g_phone.stop) {
            uint8_t hdr[8];
            if (!PhoneRecvAll(s, hdr, 8)) break;
            const uint32_t len = static_cast<uint32_t>(hdr[4]) | (static_cast<uint32_t>(hdr[5]) << 8) |
                                 (static_cast<uint32_t>(hdr[6]) << 16) | (static_cast<uint32_t>(hdr[7]) << 24);
            if (std::memcmp(hdr, "WVCF", 4) == 0) {
                uint8_t ext[5];  // width, height (uint16 LE, informational), rotation (0..3, clockwise x 90)
                if (len > (16u << 20) || !PhoneRecvAll(s, ext, 5)) break;
                jpg.resize(len);
                if (len && !PhoneRecvAll(s, jpg.data(), static_cast<int>(len))) break;
                int w = 0, h = 0;
                if (PhoneDecodeJpeg(f, jpg.data(), jpg.size(), ext[4] & 3, px, &w, &h)) {
                    {
                        std::lock_guard<std::mutex> lk(g_phone.mx);
                        g_phone.buf.swap(px);
                        g_phone.w = w;
                        g_phone.h = h;
                    }
                    ++g_phone.seq;
                    ++frames;
                }
            } else if (std::memcmp(hdr, "WVCS", 4) == 0) {
                if (len > 4096) break;
                std::string t(len, '\0');
                if (len && !PhoneRecvAll(s, &t[0], static_cast<int>(len))) break;
                while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
                PhoneHandleText(t);
            } else {
                err = L"неизвестный ответ (это точно WVC Phone?)";
                break;
            }
            const ULONGLONG now = GetTickCount64();
            if (now - t0 >= 1000) {
                g_phoneFpsX10 = static_cast<int>(frames * 10000 / (now - t0));
                frames = 0;
                t0 = now;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_phoneMx);
        if (s != INVALID_SOCKET && g_phoneSock == s) {
            closesocket(s);
            g_phoneSock = INVALID_SOCKET;
        }
        g_phoneState.clear();
        if (!g_phone.stop) g_phoneErr = err;
    }
    {
        std::lock_guard<std::mutex> lk(g_phone.mx);
        g_phone.buf.clear();
    }
    g_phoneFpsX10 = 0;
    SafeRelease(f);
    if (!g_phone.stop) {
        g_phone.failedAt = GetTickCount64();
        g_phone.status = 3;
        if (g_phoneNotify) PostMessageW(g_phoneNotify, WM_PHONE_STATE, 0, 0);
    }
    CoUninitialize();
}

static void PhoneStop() {
    if (g_phone.th.joinable()) {
        g_phone.stop = true;
        {
            std::lock_guard<std::mutex> lk(g_phoneMx);
            if (g_phoneSock != INVALID_SOCKET) {  // wakes the blocked recv()
                closesocket(g_phoneSock);
                g_phoneSock = INVALID_SOCKET;
            }
        }
        g_phone.th.join();
    }
    g_phone.status = 0;
    g_phone.link.clear();
    g_phoneFpsX10 = 0;
    std::lock_guard<std::mutex> lk(g_phoneMx);
    g_phoneState.clear();
}

// Called on the UI timer: connects while the phone is shown (or after «Подключить»), retries every 3 s.
static void PhoneSync() {
    if (!g_wsaOk) return;
    Settings s = GetSettings();
    const int user = g_phoneUser;
    const bool want = !s.phoneIp.empty() && (user == 1 || (user == 0 && g_streaming && PhoneNeeded(s)));
    if (g_phone.th.joinable()) {
        const bool dead = g_phone.status == 3;
        if (!want || g_phone.link != s.phoneIp || dead) {
            const ULONGLONG failedAt = g_phone.failedAt;
            const bool sameIp = g_phone.link == s.phoneIp;
            PhoneStop();
            if (dead && sameIp) {
                g_phone.status = 3;
                g_phone.failedAt = failedAt;
            }
        }
    }
    if (want && !g_phone.th.joinable()) {
        if (g_phone.status == 3 && GetTickCount64() - g_phone.failedAt < 3000) return;  // retry every 3 s
        g_phone.stop = false;
        g_phone.status = 1;
        g_phone.link = s.phoneIp;
        g_phone.th = std::thread(PhoneThread, s.phoneIp, s.phoneSrc, s.phoneRes);
    }
    if (!want && g_phone.status == 3) g_phone.status = 0;
}

// Broadcast "WVCP?" on every IPv4 network (and 255.255.255.255), collect "WVCP!<name>" for ~2 s
static void PhoneDiscoverThread(HWND notify) {
    auto* found = new std::vector<PhoneFound>();
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u != INVALID_SOCKET) {
        BOOL yes = TRUE;
        setsockopt(u, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in me{};
        me.sin_family = AF_INET;
        bind(u, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        std::vector<uint32_t> targets{0xFFFFFFFFu};  // host order
        ULONG size = 32 * 1024;
        std::vector<uint8_t> abuf(size);
        auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(abuf.data());
        ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                        nullptr, aa, &size);
        if (rc == ERROR_BUFFER_OVERFLOW) {
            abuf.resize(size);
            aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(abuf.data());
            rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, aa, &size);
        }
        if (rc == NO_ERROR) {
            for (auto* a = aa; a; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
                for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                    if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                    const UINT8 pl = ua->OnLinkPrefixLength;
                    if (pl == 0 || pl >= 31) continue;
                    const uint32_t ipv4 = ntohl(reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr)->sin_addr.s_addr);
                    const uint32_t mask = pl >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - pl)) - 1u);
                    const uint32_t bc = ipv4 | ~mask;
                    if (std::find(targets.begin(), targets.end(), bc) == targets.end()) targets.push_back(bc);
                }
            }
        }
        const ULONGLONG t0 = GetTickCount64();
        int sentRounds = 0;
        while (GetTickCount64() - t0 < 2000) {
            if (sentRounds < 3 && GetTickCount64() - t0 >= static_cast<ULONGLONG>(sentRounds) * 500) {
                for (uint32_t t : targets) {
                    sockaddr_in to{};
                    to.sin_family = AF_INET;
                    to.sin_port = htons(kPhoneDiscPort);
                    to.sin_addr.s_addr = htonl(t);
                    sendto(u, "WVCP?", 5, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
                }
                ++sentRounds;
            }
            fd_set r;
            FD_ZERO(&r);
            FD_SET(u, &r);
            timeval tv{0, 100000};
            if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
            char buf[300];
            sockaddr_in from{};
            int fl = sizeof(from);
            const int n = recvfrom(u, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n < 5 || std::memcmp(buf, "WVCP!", 5) != 0) continue;
            char ipText[64] = {};
            inet_ntop(AF_INET, &from.sin_addr, ipText, sizeof(ipText));
            PhoneFound pf;
            pf.name = PhoneW(std::string(buf + 5, buf + n));
            if (pf.name.empty()) pf.name = L"Телефон";
            pf.ip = PhoneW(ipText);
            bool dup = false;
            for (const auto& x : *found) dup = dup || x.ip == pf.ip;
            if (!dup) found->push_back(pf);
        }
        closesocket(u);
    }
    g_phoneSearching = false;
    if (!notify || !PostMessageW(notify, WM_PHONE_FOUND, 0, reinterpret_cast<LPARAM>(found))) delete found;
}

static void PhoneDiscover(HWND notify) {
    if (!g_wsaOk || g_phoneSearching.exchange(true)) return;
    std::thread(PhoneDiscoverThread, notify).detach();
}

static const wchar_t* PhoneStateName(const std::wstring& st) {
    if (st == L"back") return L"задняя камера";
    if (st == L"front") return L"передняя камера";
    if (st == L"screen") return L"экран телефона";
    if (st == L"need_consent") return L"ждёт разрешения на показ экрана";
    if (st == L"camera_error") return L"камера недоступна";
    if (st == L"no_camera_permission") return L"нет разрешения на камеру";
    return L"…";
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

// Shape coverage mask (0..255), 2x2 supersampled. Circle/polygons sit in the centred square.
static void BuildMask(int w, int h, int shape, std::vector<uint8_t>& m) {
    m.assign(static_cast<size_t>(w) * h, 255);
    if (shape <= 0) return;
    const double side = (std::min)(w, h);
    const double cx = w / 2.0, cy = h / 2.0;
    int n = 0;
    double rot = 0;
    if (shape == 3) { n = 6; rot = 0; }                  // hexagon, flat top and bottom
    if (shape == 4) { n = 8; rot = 3.14159265358979 / 8; }  // octagon, flat sides
    double nx[8], ny[8], ap = 0;
    if (n) {
        const double R = side / 2.0;
        ap = R * std::cos(3.14159265358979 / n);
        if (shape == 3) ap = (std::min)(ap, side / 2.0);
        for (int k = 0; k < n; ++k) {
            const double a = rot + (k + 0.5) * 2 * 3.14159265358979 / n;
            nx[k] = std::cos(a);
            ny[k] = std::sin(a);
        }
        if (shape == 3) {  // a hexagon is wider than tall: scale so it touches the square's sides
            ap = side / 2.0 * std::cos(3.14159265358979 / 6);
        }
    }
    const double rr = side * 0.14;  // rounded corner radius
    for (int y = 0; y < h; ++y) {
        uint8_t* row = m.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx) {
                    const double px = x + 0.25 + sx * 0.5, py = y + 0.25 + sy * 0.5;
                    bool in;
                    if (shape == 1) {
                        const double qx = (std::max)(0.0, std::fabs(px - cx) - (w / 2.0 - rr));
                        const double qy = (std::max)(0.0, std::fabs(py - cy) - (h / 2.0 - rr));
                        in = qx * qx + qy * qy <= rr * rr;
                    } else if (shape == 2) {
                        const double dx = px - cx, dy = py - cy, r = side / 2.0;
                        in = dx * dx + dy * dy <= r * r;
                    } else {
                        in = true;
                        for (int k = 0; k < n && in; ++k)
                            if ((px - cx) * nx[k] + (py - cy) * ny[k] > ap) in = false;
                    }
                    inside += in ? 1 : 0;
                }
            row[x] = static_cast<uint8_t>(inside * 255 / 4);
        }
    }
}

struct MaskCache {
    int w = -1, h = -1, shape = -1;
    std::vector<uint8_t> m;
    const std::vector<uint8_t>& Get(int cw, int ch, int sh) {
        if (cw != w || ch != h || sh != shape) {
            BuildMask(cw, ch, sh, m);
            w = cw;
            h = ch;
            shape = sh;
        }
        return m;
    }
};

class Renderer {
public:
    Renderer(int w, int h) : W(w), H(h) { screenDC = GetDC(nullptr); }
    ~Renderer() {
        if (textFont) DeleteObject(textFont);
        if (screenDC) ReleaseDC(nullptr, screenDC);
    }

    struct Inputs {
        const RECT* screen;      // screen/window rectangle, nullptr = not available now
        const RECT* screen2;     // "Экран 2" rectangle or nullptr
        const CamImage* cam;     // webcam frame or nullptr
        const CamImage* media;   // media frame or nullptr
        const CamImage* phone;   // phone frame or nullptr
        const wchar_t* camMsg;
        const wchar_t* mediaMsg;
        const wchar_t* phoneMsg;
    };

    // false = the screen grab for the big picture failed
    bool Frame(const Settings& s, const Inputs& in, uint8_t* outBGR) {
        if (!screenDC) return false;
        const bool swap = (s.rotation & 1) != 0;
        const bool side = s.layout == 2;          // side by side: main source in the left half
        const int TW = side ? W / 2 : W;
        const int cw = swap ? H : TW, ch = swap ? TW : H;
        if (!canvas.Ensure(screenDC, cw, ch) || !out.Ensure(screenDC, W, H)) return false;
        canvas.Clear();
        const double zoom = kZoom[std::clamp(s.zoom, 0, kZoomCount - 1)];
        if (!DrawSource(s.mainSrc, s, in, canvas.dc, cw, ch, zoom, s.fit, s.cursor)) return false;
        GdiFlush();
        Convert(s.rotation, s.mirror, TW);
        if (side)
            DrawSide(s, in, TW);
        else if (s.layout == 1)
            DrawPip(s, in);
        if (s.frameShape != 0) ApplyFrameShape(s);
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
            DrawCard(out.dc, W, H, s.pauseMode == 3 ? L"Не удалось открыть картинку" : s.pauseText.c_str());
        GdiFlush();
        Pack(outBGR);
    }

    void CamChanged() { camFresh = false; }
    void MediaChanged() { mediaFresh = false; }
    void PhoneChanged() { phoneFresh = false; }

private:
    bool Upload(Surface& surf, bool& fresh, const CamImage& img) {
        if (!img.bgra || img.w < 1 || img.h < 1) return false;
        if (fresh && img.w == surf.w && img.h == surf.h) return true;
        if (!surf.Ensure(screenDC, img.w, img.h)) return false;
        std::memcpy(surf.bits, img.bgra, static_cast<size_t>(img.w) * img.h * 4);
        fresh = true;
        return true;
    }

    // kind: 0 screen 1, 1 webcam, 2 media, 3 screen 2, 4 phone. Draws into dst (dw x dh) with zoom and fit.
    bool DrawSource(int kind, const Settings& s, const Inputs& in, HDC dst, int dw, int dh, double zoom, int fit,
                    bool cursor) {
        if (kind == 1 || kind == 2 || kind == 4) {
            const CamImage* img = kind == 1 ? in.cam : kind == 2 ? in.media : in.phone;
            Surface& surf = kind == 1 ? camSurf : kind == 2 ? mediaSurf : phoneSurf;
            bool& fresh = kind == 1 ? camFresh : kind == 2 ? mediaFresh : phoneFresh;
            if (img && Upload(surf, fresh, *img)) {
                FitRect f = ComputeFit(0, 0, img->w, img->h, zoom, fit, dw, dh);
                StretchBlt(dst, f.dx, f.dy, f.dw, f.dh, surf.dc, f.sx, f.sy, f.sw, f.sh, SRCCOPY);
            } else {
                const wchar_t* msg = kind == 1 ? in.camMsg : kind == 2 ? in.mediaMsg : in.phoneMsg;
                DrawCard(dst, dw, dh, msg ? msg : L"Нет картинки");
            }
            return true;
        }
        const RECT* rp = kind == 3 ? in.screen2 : in.screen;
        if (!rp) return false;
        const RECT& r = *rp;
        HDC srcDC = screenDC;
        int ox = 0, oy = 0;  // source DC origin in screen coordinates
        if (kind == 0 && s.srcMode == 1 && s.winCapture == 1) {
            if (!GrabWindow(s.srcWindow)) return false;
            srcDC = winSurf.dc;
            ox = winRect.left;
            oy = winRect.top;
        }
        FitRect f = ComputeFit(r.left - ox, r.top - oy, r.right - r.left, r.bottom - r.top, zoom, fit, dw, dh);
        BOOL ok;
        if (f.dw == f.sw && f.dh == f.sh)
            ok = BitBlt(dst, f.dx, f.dy, f.dw, f.dh, srcDC, f.sx, f.sy, SRCCOPY);
        else
            ok = StretchBlt(dst, f.dx, f.dy, f.dw, f.dh, srcDC, f.sx, f.sy, f.sw, f.sh, SRCCOPY);
        if (!ok) return false;
        if (cursor && dst == canvas.dc) DrawCursor(f, ox, oy);
        return true;
    }

    // Window straight from its own surface: works when it is covered or moved off screen
    bool GrabWindow(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;
        RECT wr;
        if (!GetWindowRect(hwnd, &wr)) return false;
        const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        if (ww < 16 || wh < 16 || !winSurf.Ensure(screenDC, ww, wh)) return false;
        winRect = wr;
        return PrintWindow(hwnd, winSurf.dc, 2 /* PW_RENDERFULLCONTENT */) != FALSE;
    }

    // canvas (BGRA) -> out (BGRA, columns 0..TW-1 of W x H) with clockwise rotation and horizontal mirror
    void Convert(int rot, bool mirror, int TW) {
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
                cx += (TW - 1) * ddx;
                cy += (TW - 1) * ddy;
                ddx = -ddx;
                ddy = -ddy;
            }
            const ptrdiff_t step = static_cast<ptrdiff_t>(ddx) + static_cast<ptrdiff_t>(ddy) * CW;
            const uint32_t* p = reinterpret_cast<const uint32_t*>(canvas.bits) + static_cast<ptrdiff_t>(cy) * CW + cx;
            uint32_t* o = reinterpret_cast<uint32_t*>(out.bits) + static_cast<size_t>(y) * W;
            if (step == 1) {
                std::memcpy(o, p, static_cast<size_t>(TW) * 4);
            } else {
                for (int x = 0; x < TW; ++x, p += step) o[x] = *p;
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

    // blend src (BGRA, w x h) into out at (x, y) through a coverage mask
    void BlendMasked(const uint8_t* src, int w, int h, const std::vector<uint8_t>& m, int x0, int y0) {
        for (int y = 0; y < h; ++y) {
            const int oy = y0 + y;
            if (oy < 0 || oy >= H) continue;
            const uint8_t* mp = m.data() + static_cast<size_t>(y) * w;
            const uint8_t* sp = src + static_cast<size_t>(y) * w * 4;
            uint8_t* dp = out.bits + static_cast<size_t>(oy) * W * 4;
            for (int x = 0; x < w; ++x) {
                const int ox = x0 + x;
                const int a = mp[x];
                if (ox < 0 || ox >= W || a == 0) continue;
                uint8_t* d = dp + static_cast<size_t>(ox) * 4;
                const uint8_t* q = sp + static_cast<size_t>(x) * 4;
                if (a == 255) {
                    d[0] = q[0]; d[1] = q[1]; d[2] = q[2];
                } else {
                    for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((q[c] * a + d[c] * (255 - a)) / 255);
                }
            }
        }
    }

    void FillMasked(COLORREF col, int w, int h, const std::vector<uint8_t>& m, int x0, int y0) {
        std::vector<uint8_t> solid(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < solid.size(); i += 4) {
            solid[i] = GetBValue(col);
            solid[i + 1] = GetGValue(col);
            solid[i + 2] = GetRValue(col);
            solid[i + 3] = 255;
        }
        BlendMasked(solid.data(), w, h, m, x0, y0);
    }

    // "Рядом": the second source fills the right half (webcam cropped to fill, others follow "fit")
    void DrawSide(const Settings& s, const Inputs& in, int TW) {
        const int pw = W - TW, ph = H;
        if (pw < 8 || !pipSurf.Ensure(screenDC, pw, ph)) return;
        pipSurf.Clear();
        const int kind = std::clamp(s.pipSrc, 1, 5) - 1;
        const int fit = (kind == 1 || kind == 4) ? 1 : s.fit;  // cameras fill their half
        if (!DrawSource(kind, s, in, pipSurf.dc, pw, ph, 1.0, fit, false)) DrawCard(pipSurf.dc, pw, ph, L"Нет картинки");
        GdiFlush();
        for (int y = 0; y < ph; ++y)
            std::memcpy(out.bits + (static_cast<size_t>(y) * W + TW) * 4, pipSurf.bits + static_cast<size_t>(y) * pw * 4,
                        static_cast<size_t>(pw) * 4);
        // thin divider
        const int dv = (std::max)(1, H / 540);
        for (int y = 0; y < H; ++y)
            for (int x = (std::max)(0, TW - dv); x < (std::min)(W, TW + dv); ++x) {
                uint8_t* d = out.bits + (static_cast<size_t>(y) * W + x) * 4;
                d[0] = d[1] = d[2] = 40;
            }
    }

    void DrawPip(const Settings& s, const Inputs& in) {
        RECT r = PipRect(s, W, H);
        const int pw = r.right - r.left, ph = r.bottom - r.top;
        if (!pipSurf.Ensure(screenDC, pw, ph)) return;
        pipSurf.Clear();
        // pip kinds 1 screen 1, 2 webcam, 3 media, 4 screen 2, 5 phone -> source kinds 0..4
        if (!DrawSource(std::clamp(s.pipSrc, 1, 5) - 1, s, in, pipSurf.dc, pw, ph, 1.0, 1, false))
            DrawCard(pipSurf.dc, pw, ph, L"Нет картинки");
        GdiFlush();
        const int b = (std::max)(2, H / 360);
        const auto& bm = pipBorderMask.Get(pw + 2 * b, ph + 2 * b, s.pipShape);
        FillMasked(RGB(255, 255, 255), pw + 2 * b, ph + 2 * b, bm, r.left - b, r.top - b);
        const auto& m = pipMask.Get(pw, ph, s.pipShape);
        BlendMasked(pipSurf.bits, pw, ph, m, r.left, r.top);
    }

    void ApplyFrameShape(const Settings& s) {
        const auto& m = frameMask.Get(W, H, s.frameShape);
        const COLORREF col = kShapeColors[std::clamp(s.shapeColor, 0, 3)];
        const int cb = GetBValue(col), cg = GetGValue(col), cr = GetRValue(col);
        uint8_t* p = out.bits;
        const size_t n = static_cast<size_t>(W) * H;
        for (size_t i = 0; i < n; ++i, p += 4) {
            const int a = m[i];
            if (a == 255) continue;
            p[0] = static_cast<uint8_t>((p[0] * a + cb * (255 - a)) / 255);
            p[1] = static_cast<uint8_t>((p[1] * a + cg * (255 - a)) / 255);
            p[2] = static_cast<uint8_t>((p[2] * a + cr * (255 - a)) / 255);
        }
    }

    RECT CornerRect(int corner, int w, int h) const {
        const int m = (std::max)(8, H / 30);
        const int x = (corner == 1 || corner == 3) ? m : W - m - w;
        const int y = (corner == 2 || corner == 3) ? m : H - m - h;
        return RECT{x, y, x + w, y + h};
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

    void DrawCursor(const FitRect& f, int ox, int oy) {
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
        const int x = static_cast<int>(std::lround((ci.ptScreenPos.x - ox - static_cast<int>(ii.xHotspot) - f.sx) * scale)) + f.dx;
        const int y = static_cast<int>(std::lround((ci.ptScreenPos.y - oy - static_cast<int>(ii.yHotspot) - f.sy) * scale)) + f.dy;
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
    Surface canvas, out, camSurf, mediaSurf, phoneSurf, pipSurf, winSurf;
    RECT winRect{};
    bool camFresh = false, mediaFresh = false, phoneFresh = false;
    MaskCache frameMask, pipMask, pipBorderMask;
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

static const wchar_t* PhoneMessage(const Settings& s) {
    if (s.phoneIp.empty()) return L"Телефон не выбран";
    switch (g_phone.status.load()) {
    case 1: return L"Подключаюсь к телефону…";
    case 2: return PhoneStateNow() == L"need_consent" ? L"Разрешите показ экрана на телефоне" : L"Телефон подключён, жду картинку…";
    default: return L"Телефон не подключён";
    }
}

static const wchar_t* MediaMessage(const Settings& s) {
    if (s.media.empty()) return L"Список медиафайлов пуст";
    switch (g_media.status.load()) {
    case 3: return L"Не удалось открыть файлы";
    default: return L"Открываю файл…";
    }
}

// What changes visibly: a change here starts a transition
static std::wstring TransitionKey(const Settings& s, bool live) {
    wchar_t buf[200];
    swprintf(buf, 200, L"%d|%d|%d|%d|%d|%d|%d|%d|%p|%d|%d", live ? 1 : 0, s.mainSrc, SecondSrc(s), s.textOn ? 1 : 0,
             s.logo.empty() ? 0 : 1, s.frameShape, s.pipShape, s.srcMode, static_cast<void*>(s.srcWindow),
             g_transGen.load(), s.layout);
    return buf + s.camLink;
}

// Blend two BGR frames: kind 1 dissolve, 2 through black, 3 wipe, 4 slide; e = 0..1 (eased)
static void BlendTransition(int kind, double e, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                            std::vector<uint8_t>& o, int W, int H) {
    o.resize(b.size());
    const size_t row = static_cast<size_t>(W) * 3, n = row * H;
    if (a.size() != b.size()) {
        o = b;
        return;
    }
    if (kind == 1 || kind == 2) {
        int ka = 0, kb = 0;  // weights out of 256
        if (kind == 1) {
            kb = static_cast<int>(e * 256);
            ka = 256 - kb;
        } else if (e < 0.5) {
            ka = static_cast<int>((1.0 - 2 * e) * 256);
        } else {
            kb = static_cast<int>((2 * e - 1.0) * 256);
        }
        for (size_t i = 0; i < n; ++i) o[i] = static_cast<uint8_t>((a[i] * ka + b[i] * kb) >> 8);
    } else if (kind == 3) {
        const size_t xs = static_cast<size_t>(e * W) * 3;
        for (int y = 0; y < H; ++y) {
            const size_t r = static_cast<size_t>(y) * row;
            std::memcpy(o.data() + r, b.data() + r, xs);
            std::memcpy(o.data() + r + xs, a.data() + r + xs, row - xs);
        }
    } else {
        const size_t off = static_cast<size_t>(e * W) * 3;  // old moves left, new comes in from the right
        for (int y = 0; y < H; ++y) {
            const size_t r = static_cast<size_t>(y) * row;
            std::memcpy(o.data() + r, a.data() + r + off, row - off);
            std::memcpy(o.data() + r + row - off, b.data() + r, off);
        }
    }
}

static void SenderLoop(int W, int H) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> frame(static_cast<size_t>(W) * H * 3, 0);
    std::unique_ptr<Renderer> rnd(new Renderer(W, H));
    std::vector<uint8_t> camBuf, mediaBuf, phoneBuf;
    int camW = 0, camH = 0, mediaW = 0, mediaH = 0, phoneW = 0, phoneH = 0;
    uint64_t camSeq = 0, mediaSeq = 0, phoneSeq = 0;
    std::wstring pauseKey;
    int fails = 0, frames = 0;
    ULONGLONG fpsT0 = GetTickCount64(), prevT = 0;
    std::vector<uint8_t> trFrom, blend;  // transition: old picture, mixed output
    std::wstring trKey;
    ULONGLONG trStart = 0;
    bool inTrans = false;

    while (!g_quit) {
        if (g_resetCapture.exchange(false)) {
            rnd.reset(new Renderer(W, H));
            pauseKey.clear();
            camSeq = 0;
            mediaSeq = 0;
            phoneSeq = 0;
            fails = 0;
        }
        Settings s = GetSettings();
        const bool live = g_streaming && !g_paused;
        {
            std::wstring key = TransitionKey(s, live);
            if (key != trKey) {
                if (!trKey.empty() && s.transition != 0) {
                    trFrom = inTrans ? blend : frame;  // start from what the camera shows right now
                    trStart = GetTickCount64();
                    inTrans = true;
                }
                trKey = key;
            }
        }
        if (live) {
            pauseKey.clear();
            auto fetch = [&](FrameSource& fs, bool need, std::vector<uint8_t>& buf, int& w, int& h, uint64_t& seq,
                             int which, CamImage& img) -> const CamImage* {
                if (!need || fs.status != 2) return nullptr;
                const uint64_t q = fs.seq;
                if (q != seq) {
                    std::lock_guard<std::mutex> lk(fs.mx);
                    buf = fs.buf;
                    w = fs.w;
                    h = fs.h;
                    seq = q;
                    if (which == 0) rnd->CamChanged(); else if (which == 1) rnd->MediaChanged(); else rnd->PhoneChanged();
                }
                if (buf.empty()) return nullptr;
                img = CamImage{buf.data(), w, h};
                return &img;
            };
            CamImage ci{}, mi{}, phi{};
            Renderer::Inputs in{};
            in.cam = fetch(g_web, CamNeeded(s), camBuf, camW, camH, camSeq, 0, ci);
            in.media = fetch(g_media, MediaNeeded(s), mediaBuf, mediaW, mediaH, mediaSeq, 1, mi);
            in.phone = fetch(g_phone, PhoneNeeded(s), phoneBuf, phoneW, phoneH, phoneSeq, 2, phi);
            in.camMsg = WebcamMessage(s);
            in.mediaMsg = MediaMessage(s);
            in.phoneMsg = PhoneMessage(s);
            RECT src, src2;
            const bool haveScreen = ScreenNeeded(s) && ResolveSource(s, &src);
            const bool haveScreen2 = Screen2Needed(s) && ResolveSource2(s, &src2);
            in.screen = haveScreen ? &src : nullptr;
            in.screen2 = haveScreen2 ? &src2 : nullptr;
            if ((s.mainSrc == 0 && !haveScreen) || (s.mainSrc == 3 && !haveScreen2)) {
                g_sourceLost = true;  // keep the last frame
            } else {
                g_sourceLost = ScreenNeeded(s) && !haveScreen;
                if (rnd->Frame(s, in, frame.data()))
                    fails = 0;
                else if (++fails >= 15)  // ~0.5 s of failures: rebuild capture
                    g_resetCapture = true;
            }
        } else if (s.pauseMode != 0) {
            std::wstring key = std::to_wstring(s.pauseMode) + L"|" + s.pauseImage + L"|" + s.pauseText;
            if (key != pauseKey || g_pauseDirty.exchange(false)) {
                if (s.pauseMode == 1)
                    std::fill(frame.begin(), frame.end(), static_cast<uint8_t>(0));
                else
                    rnd->Pause(s, frame.data());
                pauseKey = key;
            }
        }

        const ULONGLONG now = GetTickCount64();
        const std::vector<uint8_t>* outFrame = &frame;
        if (inTrans) {
            const int dur = kTransMs[std::clamp(s.transDur, 0, 2)];
            const double t = static_cast<double>(now - trStart) / dur;
            if (t >= 1.0 || s.transition == 0) {
                inTrans = false;
            } else {
                const double e = t * t * (3 - 2 * t);  // smoothstep
                BlendTransition(s.transition, e, trFrom, frame, blend, W, H);
                outFrame = &blend;
            }
        }
        if (now - prevT >= 50) {
            std::lock_guard<std::mutex> lk(g_prevMx);
            g_prev = *outFrame;
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

        pSend(g_cam, outFrame->data());  // paces itself to the camera FPS
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
// Two frames: [0] = "Экран 1" (region / window), [1] = "Экран 2" (dashed, labelled)
static HWND  g_frame[2] = {nullptr, nullptr};
static int   g_fb = 6;            // border thickness, px
static int   g_tabW = 150, g_tabH = 26;
static bool  g_tabBelow[2] = {false, false};  // tab under the region when there is no room above
static bool  g_frameMoving = false;
static int   g_movingIdx = -1;
static HFONT g_frameFont = nullptr;
static const UINT WM_FRAME_DONE = WM_APP + 7;

static int FrameIdx(HWND hwnd) { return hwnd && hwnd == g_frame[1] ? 1 : 0; }

static void FrameRects(int idx, int w, int h, RECT* border, RECT* hole, RECT* tab) {
    const bool below = g_tabBelow[idx & 1];
    const int top = below ? 0 : g_tabH;
    const int bottom = below ? h - g_tabH : h;
    *border = RECT{0, top, w, bottom};
    *hole = RECT{g_fb, top + g_fb, w - g_fb, bottom - g_fb};
    const int tw = (std::min)(g_tabW, w);
    *tab = below ? RECT{0, h - g_tabH, tw, h} : RECT{0, 0, tw, g_tabH};
}

// Frame colour: red = live, yellow = paused, green = not streaming
static COLORREF FrameColor() {
    if (!g_streaming) return RGB(40, 170, 70);
    return g_paused ? RGB(235, 170, 0) : RGB(230, 40, 40);
}

// The chosen shape (frame or small window) traced on the screen around the captured area.
// Points are pushed outward so the line lies in the part the shape cuts away: it never shows in the camera.
static bool ScreenShapeOutline(const Settings& s, const RECT& src, std::vector<POINT>* pts, int idx) {
    int W, H;
    OutSize(s.quality, s.aspect, &W, &H);
    int shape, areaW, areaH, rot = 0, fit = 1;
    bool mirror = false;
    double zoom = 1.0;
    const int mainKind = idx == 1 ? 3 : 0;
    if (s.layout == 2) return false;  // side by side: plain rectangle
    if (s.mainSrc == mainKind) {
        shape = s.frameShape;
        areaW = W;
        areaH = H;
        rot = s.rotation & 3;
        mirror = s.mirror;
        zoom = kZoom[std::clamp(s.zoom, 0, kZoomCount - 1)];
        fit = s.fit;
    } else if (SecondSrc(s) == mainKind + 1) {
        shape = s.pipShape;
        RECT r = PipRect(s, W, H);
        areaW = r.right - r.left;
        areaH = r.bottom - r.top;
    } else {
        return false;
    }
    if (shape == 0) return false;
    const double sw = src.right - src.left, sh = src.bottom - src.top;
    if (sw < 8 || sh < 8) return false;
    const int cw = (rot & 1) ? areaH : areaW, ch = (rot & 1) ? areaW : areaH;
    const FitRect f = ComputeFit(0, 0, sw, sh, zoom, fit, cw, ch);
    const double scx = static_cast<double>(f.sw) / f.dw, scy = static_cast<double>(f.sh) / f.dh;
    auto map = [&](double x, double y) {
        if (mirror) x = areaW - x;
        double cx = x, cy = y;
        switch (rot) {
        case 1: cx = y; cy = ch - x; break;
        case 2: cx = cw - x; cy = ch - y; break;
        case 3: cx = cw - y; cy = x; break;
        }
        return POINT{static_cast<LONG>(std::lround(src.left + f.sx + (cx - f.dx) * scx)),
                     static_cast<LONG>(std::lround(src.top + f.sy + (cy - f.dy) * scy))};
    };
    const double pi = 3.14159265358979;
    const double d = (g_fb / 2.0 + 1.0) / (std::min)(scx, scy) + 1.0;  // outward offset, output px
    const double side = (std::min)(areaW, areaH), x0 = areaW / 2.0, y0 = areaH / 2.0;
    pts->clear();
    if (shape == 1) {  // rounded rectangle
        const double rr = side * 0.14 + d;
        const double hx = areaW / 2.0 + d - rr, hy = areaH / 2.0 + d - rr;
        const double cxs[4] = {x0 + hx, x0 - hx, x0 - hx, x0 + hx}, cys[4] = {y0 + hy, y0 + hy, y0 - hy, y0 - hy};
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k <= 8; ++k) {
                const double a = (c * 90 + k * 90.0 / 8) * pi / 180;
                pts->push_back(map(cxs[c] + rr * std::cos(a), cys[c] + rr * std::sin(a)));
            }
    } else if (shape == 2) {  // circle
        const double r = side / 2.0 + d;
        for (int k = 0; k < 72; ++k) {
            const double a = k * 2 * pi / 72;
            pts->push_back(map(x0 + r * std::cos(a), y0 + r * std::sin(a)));
        }
    } else {  // hexagon (corners at 0°, 60°…) or octagon (corners at 22.5°, 67.5°…)
        const int n = shape == 3 ? 6 : 8;
        const double half = pi / n, start = shape == 3 ? 0.0 : pi / 8;
        const double ap = side / 2.0 * std::cos(half) + d;
        const double R = ap / std::cos(half);
        for (int k = 0; k < n; ++k) {
            const double a = start + k * 2 * pi / n;
            pts->push_back(map(x0 + R * std::cos(a), y0 + R * std::sin(a)));
        }
    }
    return true;
}

static void RegionFromFrame(HWND hwnd) {
    const int idx = FrameIdx(hwnd);
    RECT wr;
    GetWindowRect(hwnd, &wr);
    RECT r{wr.left + g_fb, wr.top + g_fb + (g_tabBelow[idx] ? 0 : g_tabH), wr.right - g_fb,
           wr.bottom - g_fb - (g_tabBelow[idx] ? g_tabH : 0)};
    std::lock_guard<std::mutex> lk(g_setMx);
    if (idx == 1) {
        g_set.region2 = r;
    } else {
        g_set.region = r;
        g_set.hasRegion = true;
    }
}

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        const int idx = FrameIdx(hwnd);
        RECT rc, border, hole, tab;
        GetClientRect(hwnd, &rc);
        FrameRects(idx, rc.right, rc.bottom, &border, &hole, &tab);
        const COLORREF col = FrameColor();
        const Settings fs = GetSettings();
        HBRUSH key = CreateSolidBrush(kKey);
        HBRUSH red = CreateSolidBrush(col);
        FillRect(dc, &rc, key);
        RECT wr;
        GetWindowRect(hwnd, &wr);
        const RECT holeScr{wr.left + hole.left, wr.top + hole.top, wr.left + hole.right, wr.top + hole.bottom};
        std::vector<POINT> pts;
        if (ScreenShapeOutline(fs, holeScr, &pts, idx) && pts.size() >= 3) {
            // corner marks (grab them to resize) + the shape's outline
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
            for (const RECT& m : marks) FillRect(dc, &m, red);
            for (POINT& p : pts) {
                p.x -= wr.left;
                p.y -= wr.top;
            }
            HPEN pen = CreatePen(PS_SOLID, (std::max)(2, g_fb * 2 / 3), col);
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            HRGN clip = CreateRectRgn(border.left, border.top, border.right, border.bottom);
            SelectClipRgn(dc, clip);
            Polygon(dc, pts.data(), static_cast<int>(pts.size()));
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
        } else {
            FillRect(dc, &border, red);
            FillRect(dc, &hole, key);
            if (idx == 1) {  // "Экран 2": dashed (colour / white) so the two frames never look alike
                HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
                const int seg = (std::max)(8, g_tabH / 2);
                for (int x = border.left + seg; x < border.right - seg; x += 2 * seg) {
                    const int x2 = (std::min)(x + seg, static_cast<int>(border.right) - seg);
                    RECT a{x, border.top, x2, border.top + g_fb}, b{x, border.bottom - g_fb, x2, border.bottom};
                    FillRect(dc, &a, wb);
                    FillRect(dc, &b, wb);
                }
                for (int y = border.top + seg; y < border.bottom - seg; y += 2 * seg) {
                    const int y2 = (std::min)(y + seg, static_cast<int>(border.bottom) - seg);
                    RECT a{border.left, y, border.left + g_fb, y2}, b{border.right - g_fb, y, border.right, y2};
                    FillRect(dc, &a, wb);
                    FillRect(dc, &b, wb);
                }
                DeleteObject(wb);
            }
        }
        FillRect(dc, &tab, red);
        DeleteObject(red);
        DeleteObject(key);
        HGDIOBJ old = g_frameFont ? SelectObject(dc, g_frameFont) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        const bool both = ScreenNeeded(fs) && Screen2Needed(fs);
        DrawTextW(dc, idx == 1 ? L"✥ Экран 2" : both ? L"✥ Экран 1" : L"✥ перетащить", -1, &tab,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
        FrameRects(FrameIdx(hwnd), rc.right, rc.bottom, &border, &hole, &tab);
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
        if (FrameIdx(hwnd) == 0 && s.srcMode == 1 && ResolveSource(s, &cur)) {
            std::lock_guard<std::mutex> lk(g_setMx);
            g_set.srcMode = 0;
            g_set.region = cur;
            g_set.hasRegion = true;
        }
        g_frameMoving = true;
        g_movingIdx = FrameIdx(hwnd);
        return 0;
    }
    case WM_MOVE:
    case WM_SIZE:
        if (msg == WM_SIZE) InvalidateRect(hwnd, nullptr, FALSE);
        if (g_frameMoving && g_movingIdx == FrameIdx(hwnd)) RegionFromFrame(hwnd);
        return 0;
    case WM_EXITSIZEMOVE:
        g_frameMoving = false;
        g_movingIdx = -1;
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

static void UpdateOneFrame(int idx, const Settings& s) {
    HWND f = g_frame[idx];
    if (!f) return;
    RECT r;
    const bool need = idx == 0 ? (ScreenNeeded(s) && ResolveSource(s, &r)) : (Screen2Needed(s) && ResolveSource2(s, &r));
    if ((g_streaming || s.idleFrame) && need) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
        g_tabBelow[idx] = (r.top - g_fb - g_tabH) < mi.rcMonitor.top;
        RECT w{r.left - g_fb, r.top - g_fb, r.right + g_fb, r.bottom + g_fb};
        if (g_tabBelow[idx])
            w.bottom += g_tabH;
        else
            w.top -= g_tabH;
        RECT cur;
        GetWindowRect(f, &cur);
        // repaint when the colour or anything that shapes the outline changes
        static std::wstring lastLook[2];
        wchar_t look[240];
        swprintf(look, 240, L"%u|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d", static_cast<unsigned>(FrameColor()),
                 s.mainSrc, SecondSrc(s), s.frameShape, s.pipShape, s.fit, s.zoom, s.rotation, s.mirror ? 1 : 0,
                 s.quality, s.aspect, s.pipX, s.pipY, s.pipW, s.layout, (ScreenNeeded(s) && Screen2Needed(s)) ? 1 : 0);
        if (!IsWindowVisible(f) || !EqualRect(&cur, &w)) {
            SetWindowPos(f, HWND_TOPMOST, w.left, w.top, w.right - w.left, w.bottom - w.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(f, nullptr, TRUE);
        } else if (lastLook[idx] != look) {
            InvalidateRect(f, nullptr, TRUE);
        }
        lastLook[idx] = look;
    } else {
        ShowWindow(f, SW_HIDE);
    }
}

static void UpdateFrame() {
    if (g_frameMoving) return;
    Settings s = GetSettings();
    UpdateOneFrame(0, s);
    UpdateOneFrame(1, s);
}

static void HideFrames() {
    for (HWND f : g_frame)
        if (f) ShowWindow(f, SW_HIDE);
}

// ---------------------------------------------------------------------------
// Preview control
// ---------------------------------------------------------------------------
// Preview geometry (frame -> preview control) from the last paint, for dragging the small window
static double g_pvScale = 0;
static int g_pvX = 0, g_pvY = 0, g_pvW = 0, g_pvH = 0;
static int g_pipDrag = 0;  // 0 none, 1 move, 2..5 resize by corner: 2 BR, 3 BL, 4 TR, 5 TL
static POINT g_pipDragStart{};
static Settings g_pipDragSet;
static void OnPipDragged();  // main window: save + refresh

static int PipHit(int mx, int my) {
    Settings s = GetSettings();
    if (s.layout != 1 || g_pvScale <= 0) return 0;
    RECT r = PipRect(s, g_pvW, g_pvH);
    const int l = g_pvX + static_cast<int>(r.left * g_pvScale), t = g_pvY + static_cast<int>(r.top * g_pvScale);
    const int rr = g_pvX + static_cast<int>(r.right * g_pvScale), b = g_pvY + static_cast<int>(r.bottom * g_pvScale);
    const int grip = 9;
    auto atPt = [&](int px, int py) { return std::abs(mx - px) <= grip && std::abs(my - py) <= grip; };
    if (atPt(rr, b)) return 2;
    if (atPt(l, b)) return 3;
    if (atPt(rr, t)) return 4;
    if (atPt(l, t)) return 5;
    if (mx >= l && mx < rr && my >= t && my < b) return 1;
    return 0;
}

// small window rectangle in preview pixels (for the corner handles)
static bool PipPreviewRect(RECT* out) {
    Settings s = GetSettings();
    if (s.layout != 1 || g_pvScale <= 0) return false;
    RECT r = PipRect(s, g_pvW, g_pvH);
    *out = RECT{g_pvX + static_cast<int>(r.left * g_pvScale), g_pvY + static_cast<int>(r.top * g_pvScale),
                g_pvX + static_cast<int>(r.right * g_pvScale), g_pvY + static_cast<int>(r.bottom * g_pvScale)};
    return true;
}

static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        const int hit = g_pipDrag ? g_pipDrag : PipHit(pt.x, pt.y);
        if (hit) {
            const wchar_t* cur = hit == 1 ? IDC_SIZEALL : (hit == 2 || hit == 5) ? IDC_SIZENWSE : IDC_SIZENESW;
            SetCursor(LoadCursor(nullptr, cur));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
        g_pipDrag = PipHit(x, y);
        if (g_pipDrag) {
            g_pipDragStart = POINT{x, y};
            g_pipDragSet = GetSettings();
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_pipDrag && g_pvScale > 0) {
            const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
            const double dx = (x - g_pipDragStart.x) / g_pvScale, dy = (y - g_pipDragStart.y) / g_pvScale;
            Settings base = g_pipDragSet;
            RECT r0 = PipRect(base, g_pvW, g_pvH);
            std::lock_guard<std::mutex> lk(g_setMx);
            if (g_pipDrag == 1) {
                const int nx = std::clamp(static_cast<int>(r0.left + dx), 0, g_pvW - static_cast<int>(r0.right - r0.left));
                const int ny = std::clamp(static_cast<int>(r0.top + dy), 0, g_pvH - static_cast<int>(r0.bottom - r0.top));
                g_set.pipX = MulDiv(nx, 10000, g_pvW);
                g_set.pipY = MulDiv(ny, 10000, g_pvH);
            } else {
                // the opposite corner stays put; the side that grew more wins, the shape keeps its proportions
                const bool right = g_pipDrag == 2 || g_pipDrag == 4, bottom = g_pipDrag == 2 || g_pipDrag == 3;
                const double w0 = r0.right - r0.left, h0 = r0.bottom - r0.top, ratio = w0 / (std::max)(1.0, h0);
                const double byX = w0 + (right ? dx : -dx), byY = (h0 + (bottom ? dy : -dy)) * ratio;
                double nw = (std::max)(byX, byY);
                const double maxW = (std::min)(right ? g_pvW - r0.left : static_cast<double>(r0.right),
                                               (bottom ? g_pvH - r0.top : static_cast<double>(r0.bottom)) * ratio);
                nw = std::clamp(nw, g_pvW * 0.05, (std::max)(g_pvW * 0.05, maxW));
                const double nh = nw / ratio;
                const double nx = right ? r0.left : r0.right - nw, ny = bottom ? r0.top : r0.bottom - nh;
                g_set.pipW = std::clamp(static_cast<int>(nw * 10000 / g_pvW), 500, 9000);
                g_set.pipX = std::clamp(static_cast<int>(nx * 10000 / g_pvW), 0, 10000);
                g_set.pipY = std::clamp(static_cast<int>(ny * 10000 / g_pvH), 0, 10000);
            }
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (g_pipDrag) {
            g_pipDrag = 0;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
            OnPipDragged();
        }
        return 0;
    }
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
                g_pvScale = sc;
                g_pvX = dx;
                g_pvY = dy;
                g_pvW = g_prevW;
                g_pvH = g_prevH;
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
        RECT pr;
        if (PipPreviewRect(&pr)) {  // corner handles: drag any of them to resize the small window
            const int hs = 4;
            HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
            HBRUSH dark = CreateSolidBrush(RGB(30, 30, 30));
            const POINT cs[4] = {{pr.left, pr.top}, {pr.right, pr.top}, {pr.left, pr.bottom}, {pr.right, pr.bottom}};
            for (const POINT& c : cs) {
                RECT o{c.x - hs - 1, c.y - hs - 1, c.x + hs + 1, c.y + hs + 1};
                RECT i{c.x - hs, c.y - hs, c.x + hs, c.y + hs};
                FillRect(m, &o, dark);
                FillRect(m, &i, white);
            }
            DeleteObject(white);
            DeleteObject(dark);
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
    HA_SWAP,
    HA_MEDIA_NEXT,
    HA_HIDE_WINDOW,
    HA_PAUSE,
    HA_SCREEN12,  // v0.7: main source Экран 1 <-> Экран 2
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
    {L"Главный источник: экран ↔ веб-камера", MOD_CONTROL | MOD_SHIFT, VK_F9},
    {L"Второй источник (окошко) вкл/выкл", MOD_CONTROL | MOD_SHIFT, VK_F10},
    {L"Надпись вкл/выкл", MOD_CONTROL | MOD_SHIFT, VK_F11},
    {L"Сцена 1", MOD_CONTROL | MOD_SHIFT, '1'},
    {L"Сцена 2", MOD_CONTROL | MOD_SHIFT, '2'},
    {L"Сцена 3", MOD_CONTROL | MOD_SHIFT, '3'},
    {L"Сцена 4", MOD_CONTROL | MOD_SHIFT, '4'},
    {L"Поменять главный и второй источник", MOD_CONTROL | MOD_SHIFT, VK_F12},
    {L"Следующий медиафайл", MOD_CONTROL | MOD_SHIFT, '5'},
    {L"Спрятать / вернуть окно", 0, 0},
    {L"Пауза / продолжить", MOD_CONTROL | MOD_SHIFT, '6'},
    {L"Главный источник: Экран 1 ⇄ Экран 2", MOD_CONTROL | MOD_SHIFT, '7'},
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
    // tab "Источники"
    IDC_SRC_HDR, IDC_SRC_VALUE, IDC_SELECT, IDC_MONITOR, IDC_WINDOW,
    IDC_SEC_WIN, IDC_WCAP_LABEL, IDC_WINCAP, IDC_HIDEWIN,
    IDC_SRC2_HDR, IDC_SRC2_VALUE, IDC_SELECT2, IDC_MONITOR2, IDC_SCR_TOGGLE,
    IDC_SHOW_HDR, IDC_VIEW_LABEL, IDC_VIEW, IDC_LAYOUT_LABEL, IDC_LAYOUT, IDC_PIPSRC_LABEL, IDC_PIPSRC, IDC_SWAP,
    IDC_CAM_HDR, IDC_CAM_LABEL, IDC_CAMDEV, IDC_CAMMIRROR,
    IDC_PHONE_HDR, IDC_PHONE_LABEL, IDC_PHONE_LIST, IDC_PHONE_FIND, IDC_PHONEIP_LABEL, IDC_PHONE_IP, IDC_PHONE_CONN,
    IDC_PHONE_BACK, IDC_PHONE_FRONT, IDC_PHONE_SCREEN, IDC_PHONERES_LABEL, IDC_PHONE_RES, IDC_PHONE_STATUS,
    IDC_MEDIA_HDR, IDC_MEDIA_LIST, IDC_MEDIA_ADD, IDC_MEDIA_DEL, IDC_MEDIA_UP, IDC_MEDIA_DOWN, IDC_IMGSEC_LABEL,
    IDC_IMGSEC, IDC_MEDIA_NEXT, IDC_MEDIA_NOW,
    // tab "Кадр"
    IDC_PIP_HDR, IDC_PIP_LABEL, IDC_PIPCORNER, IDC_PIPSIZE, IDC_PIPSIZE_UD, IDC_PIPSHAPE_LABEL, IDC_PIPSHAPE,
    IDC_FRAME_HDR, IDC_FIT_LABEL, IDC_FIT, IDC_SHAPE_LABEL, IDC_FRAMESHAPE,
    IDC_SEC_FRAME, IDC_ROT_LABEL, IDC_ROT, IDC_ZOOM_LABEL, IDC_ZOOM, IDC_MIRROR, IDC_CURSOR, IDC_SHAPECOLOR_LABEL,
    IDC_SHAPECOLOR,
    IDC_TEXT_HDR, IDC_TEXT_LABEL, IDC_TEXT, IDC_TEXTON, IDC_TPOS_LABEL, IDC_TEXTPOS, IDC_TEXTSIZE,
    IDC_LOGO_HDR, IDC_LOGO_LABEL, IDC_LOGO_PICK, IDC_LOGO_CLEAR, IDC_LPOS_LABEL, IDC_LOGOCORNER, IDC_LOGOSIZE,
    IDC_LOGO_NAME,
    IDC_TRANS_HDR, IDC_TRANS_LABEL, IDC_TRANS, IDC_TRANSDUR,
    // tab "Настройки"
    IDC_FMT_HDR, IDC_Q_LABEL, IDC_QUALITY, IDC_A_LABEL, IDC_ASPECT, IDC_FPS_LABEL, IDC_FPS,
    IDC_PAUSE_HDR, IDC_PAUSE_LABEL, IDC_PAUSE, IDC_PAUSETEXT_LABEL, IDC_PAUSETEXT, IDC_PAUSE_IMG,
    IDC_BEH_HDR, IDC_KEEPCAM, IDC_IDLEFRAME, IDC_AUTOSTART, IDC_AUTOSTREAM,
    IDC_HK_HDR, IDC_HK_EDIT, IDC_SEC_KEYS, IDC_HK_LIST,
    IDC_FILE_HDR, IDC_JSON_EXPORT, IDC_JSON_IMPORT,
    IDC_CAMSYS_HDR, IDC_INSTALL, IDC_UNINSTALL,
    // left column (always visible)
    IDC_PREV_LABEL, IDC_FPS_VALUE, IDC_PREVIEW, IDC_START, IDC_PAUSEBTN, IDC_STATUS,
    IDC_SCENE_HDR, IDC_SCENE1, IDC_SCENE2, IDC_SCENE3, IDC_SCENE4,
    IDC_PROF_LABEL, IDC_PROFILE, IDC_PROF_LOAD, IDC_PROF_SAVE, IDC_PROF_DEL, IDC_PROF_HINT,
    IDC_LAST_ID
};
static_assert(IDC_LAST_ID < 300, "control ids must stay below the tray ids");
enum { ID_TRAY_OPEN = 300, ID_TRAY_TOGGLE, ID_TRAY_PAUSE, ID_TRAY_EXIT };

static const UINT WM_TRAY = WM_APP + 1;
static const UINT_PTR kTimerStatus = 1, kTimerPreview = 2, kTimerFollow = 3;

static HINSTANCE g_hInst;
static HWND g_main = nullptr;
static HWND g_preview = nullptr;
static HFONT g_font = nullptr;
static HFONT g_fontBold = nullptr;
static UINT g_dpi = 96;
static NOTIFYICONDATAW g_nid{};
static std::wstring g_note;  // one-off note in the status line
static bool g_syncing = false;
static int g_page = 0;
static std::vector<CamDevice> g_camList;

// light theme
static const COLORREF kBgMain = RGB(243, 243, 243), kBgPage = RGB(255, 255, 255);
static const COLORREF kHdrText = RGB(0, 84, 160), kHintText = RGB(96, 96, 96);
static HBRUSH g_brMain = nullptr, g_brPage = nullptr;

// tab pages: scrollable child windows holding their controls
static const int kPages = 3;
static HWND g_pageWnd[kPages] = {};
static int g_scroll[kPages] = {};  // vertical scroll offset, px
static HWND g_ctl[IDC_LAST_ID + 1] = {};

// collapsible sections (bit per section, saved as UiExpanded)
enum { SEC_WIN = 1, SEC_FRAME = 2, SEC_KEYS = 4 };
static int g_expanded = 0;

static bool IsHeaderId(int id) {
    switch (id) {
    case IDC_SRC_HDR: case IDC_SRC2_HDR: case IDC_SHOW_HDR: case IDC_CAM_HDR: case IDC_MEDIA_HDR: case IDC_PHONE_HDR:
    case IDC_PIP_HDR: case IDC_FRAME_HDR: case IDC_TEXT_HDR: case IDC_LOGO_HDR: case IDC_TRANS_HDR:
    case IDC_FMT_HDR: case IDC_PAUSE_HDR: case IDC_BEH_HDR: case IDC_HK_HDR: case IDC_FILE_HDR:
    case IDC_CAMSYS_HDR: case IDC_SCENE_HDR:
        return true;
    }
    return false;
}

static int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }
static HWND Ctl(int id) {
    if (id > 0 && id <= IDC_LAST_ID && g_ctl[id]) return g_ctl[id];
    return GetDlgItem(g_main, id);
}

static void SetTextIfChanged(HWND h, const std::wstring& text) {
    if (!h) return;
    wchar_t cur[4096] = {};
    GetWindowTextW(h, cur, 4096);
    if (text != cur) SetWindowTextW(h, text.c_str());
}

static void MakeFont() {
    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    g_font = CreateFontW(-MulDiv(9, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                         L"Segoe UI");
    g_fontBold = CreateFontW(-MulDiv(9, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                             L"Segoe UI");
}

static const int kClientW = 900, kClientH = 600;
static const int kLeftX = 12, kLeftW = 400, kRightX = 424;

static void SetSectionText(int id, bool open, const wchar_t* title) {
    std::wstring t = (open ? L"▾  " : L"▸  ") + std::wstring(title);
    SetTextIfChanged(Ctl(id), t);
}

// One tab page laid out top to bottom; collapsed sections hide their controls and the rest moves up.
static void LayoutPage(int p) {
    HWND page = g_pageWnd[p];
    if (!page) return;
    RECT pc;
    GetClientRect(page, &pc);
    const int W = MulDiv(pc.right, 96, static_cast<int>(g_dpi));  // page width in 96-dpi units
    struct Pl { int id, x, y, w, h; bool vis; };
    std::vector<Pl> pl;
    const int pad = 14, lw = 148, cx = pad + lw + 6;
    const int fw = (std::max)(120, W - 2 * pad), cw = (std::max)(80, W - cx - pad);
    int y = 8;
    auto P = [&](int id, int x, int yy, int w, int h, bool vis = true) { pl.push_back({id, x, yy, w, h, vis}); };
    auto hdr = [&](int id) { y += 6; P(id, pad, y, fw, 20); y += 26; };
    auto row1 = [&](int lab, int ctl, int h = 200) { P(lab, pad, y + 4, lw, 20); P(ctl, cx, y, cw, h); y += 32; };
    auto row2 = [&](int lab, int a, int b, int bw = 76) {
        P(lab, pad, y + 4, lw, 20);
        P(a, cx, y, cw - bw - 6, 200);
        P(b, cx + cw - bw, y, bw, 200);
        y += 32;
    };
    auto chk = [&](int id) { P(id, pad, y, fw, 22); y += 26; };
    auto btns = [&](std::initializer_list<int> ids) {
        const int n = static_cast<int>(ids.size());
        const int bw = (fw - (n - 1) * 6) / n;
        int i = 0;
        for (int id : ids) {
            const int x = pad + i * (bw + 6);
            const int w = (i == n - 1) ? fw - (n - 1) * (bw + 6) : bw;
            P(id, x, y, w, 28);
            ++i;
        }
        y += 34;
    };
    auto full = [&](int id, int h) { P(id, pad, y, fw, h); y += h + 6; };
    auto hide = [&](std::initializer_list<int> ids) {
        for (int id : ids) P(id, 0, 0, 10, 10, false);
    };
    auto sec = [&](int id, int bit, const wchar_t* title) {
        const bool open = (g_expanded & bit) != 0;
        SetSectionText(id, open, title);
        P(id, pad, y, fw, 26);
        y += 32;
        return open;
    };

    if (p == 0) {  // Источники
        hdr(IDC_SRC_HDR);
        full(IDC_SRC_VALUE, 20);
        btns({IDC_SELECT, IDC_MONITOR, IDC_WINDOW});
        if (sec(IDC_SEC_WIN, SEC_WIN, L"Дополнительно: захват окна")) {
            row1(IDC_WCAP_LABEL, IDC_WINCAP);
            P(IDC_HIDEWIN, cx, y, cw, 28);
            y += 34;
        } else {
            hide({IDC_WCAP_LABEL, IDC_WINCAP, IDC_HIDEWIN});
        }
        hdr(IDC_SRC2_HDR);
        full(IDC_SRC2_VALUE, 20);
        btns({IDC_SELECT2, IDC_MONITOR2, IDC_SCR_TOGGLE});
        hdr(IDC_SHOW_HDR);
        row1(IDC_VIEW_LABEL, IDC_VIEW);
        row1(IDC_LAYOUT_LABEL, IDC_LAYOUT);
        P(IDC_PIPSRC_LABEL, pad, y + 4, lw, 20);
        P(IDC_PIPSRC, cx, y, cw - 46, 200);
        P(IDC_SWAP, cx + cw - 40, y - 1, 40, 26);
        y += 32;
        hdr(IDC_CAM_HDR);
        row1(IDC_CAM_LABEL, IDC_CAMDEV, 240);
        chk(IDC_CAMMIRROR);
        hdr(IDC_PHONE_HDR);
        row2(IDC_PHONE_LABEL, IDC_PHONE_LIST, IDC_PHONE_FIND, 76);
        P(IDC_PHONEIP_LABEL, pad, y + 4, lw, 20);
        P(IDC_PHONE_IP, cx, y, cw - 110 - 6, 24);
        P(IDC_PHONE_CONN, cx + cw - 110, y - 1, 110, 26);
        y += 32;
        btns({IDC_PHONE_BACK, IDC_PHONE_FRONT, IDC_PHONE_SCREEN});
        row1(IDC_PHONERES_LABEL, IDC_PHONE_RES);
        full(IDC_PHONE_STATUS, 54);
        hdr(IDC_MEDIA_HDR);
        full(IDC_MEDIA_LIST, 104);
        {
            const int bw = (fw - 3 * 6 - 2 * 44) / 2;
            P(IDC_MEDIA_ADD, pad, y, bw, 28);
            P(IDC_MEDIA_DEL, pad + bw + 6, y, bw, 28);
            P(IDC_MEDIA_UP, pad + 2 * (bw + 6), y, 44, 28);
            P(IDC_MEDIA_DOWN, pad + fw - 44, y, 44, 28);
            y += 34;
        }
        row1(IDC_IMGSEC_LABEL, IDC_IMGSEC);
        P(IDC_MEDIA_NEXT, pad, y, 180, 28);
        P(IDC_MEDIA_NOW, pad + 188, y + 5, fw - 188, 20);
        y += 34;
    } else if (p == 1) {  // Кадр
        hdr(IDC_PIP_HDR);
        P(IDC_PIP_LABEL, pad, y + 4, lw, 20);
        P(IDC_PIPCORNER, cx, y, cw - 84, 200);
        P(IDC_PIPSIZE, cx + cw - 78, y, 56, 24);
        P(IDC_PIPSIZE_UD, cx + cw - 22, y, 22, 24);
        y += 32;
        row1(IDC_PIPSHAPE_LABEL, IDC_PIPSHAPE);
        hdr(IDC_FRAME_HDR);
        row1(IDC_FIT_LABEL, IDC_FIT);
        row1(IDC_SHAPE_LABEL, IDC_FRAMESHAPE);
        if (sec(IDC_SEC_FRAME, SEC_FRAME, L"Дополнительно: поворот, зум, зеркало, курсор, фон")) {
            row1(IDC_ROT_LABEL, IDC_ROT);
            row1(IDC_ZOOM_LABEL, IDC_ZOOM);
            row1(IDC_SHAPECOLOR_LABEL, IDC_SHAPECOLOR);
            chk(IDC_MIRROR);
            chk(IDC_CURSOR);
        } else {
            hide({IDC_ROT_LABEL, IDC_ROT, IDC_ZOOM_LABEL, IDC_ZOOM, IDC_SHAPECOLOR_LABEL, IDC_SHAPECOLOR, IDC_MIRROR,
                  IDC_CURSOR});
        }
        hdr(IDC_TEXT_HDR);
        P(IDC_TEXT_LABEL, pad, y + 4, lw, 20);
        P(IDC_TEXT, cx, y, cw, 24);
        y += 32;
        chk(IDC_TEXTON);
        row2(IDC_TPOS_LABEL, IDC_TEXTPOS, IDC_TEXTSIZE, 96);
        hdr(IDC_LOGO_HDR);
        P(IDC_LOGO_LABEL, pad, y + 5, lw, 20);
        P(IDC_LOGO_PICK, cx, y, cw - 96 - 6, 28);
        P(IDC_LOGO_CLEAR, cx + cw - 96, y, 96, 28);
        y += 34;
        row2(IDC_LPOS_LABEL, IDC_LOGOCORNER, IDC_LOGOSIZE, 96);
        full(IDC_LOGO_NAME, 20);
        hdr(IDC_TRANS_HDR);
        row2(IDC_TRANS_LABEL, IDC_TRANS, IDC_TRANSDUR, 96);
    } else {  // Настройки
        hdr(IDC_FMT_HDR);
        row1(IDC_Q_LABEL, IDC_QUALITY);
        row1(IDC_A_LABEL, IDC_ASPECT);
        row1(IDC_FPS_LABEL, IDC_FPS);
        hdr(IDC_PAUSE_HDR);
        row1(IDC_PAUSE_LABEL, IDC_PAUSE);
        P(IDC_PAUSETEXT_LABEL, pad, y + 4, lw, 20);
        P(IDC_PAUSETEXT, cx, y, cw, 24);
        y += 32;
        P(IDC_PAUSE_IMG, cx, y, cw, 28);
        y += 34;
        hdr(IDC_BEH_HDR);
        chk(IDC_KEEPCAM);
        chk(IDC_IDLEFRAME);
        chk(IDC_AUTOSTART);
        chk(IDC_AUTOSTREAM);
        hdr(IDC_HK_HDR);
        P(IDC_HK_EDIT, pad, y, 240, 28);
        y += 34;
        if (sec(IDC_SEC_KEYS, SEC_KEYS, L"Список горячих клавиш")) {
            full(IDC_HK_LIST, 190);
        } else {
            hide({IDC_HK_LIST});
        }
        hdr(IDC_FILE_HDR);
        btns({IDC_JSON_EXPORT, IDC_JSON_IMPORT});
        hdr(IDC_CAMSYS_HDR);
        btns({IDC_INSTALL, IDC_UNINSTALL});
    }
    y += 8;

    // scroll range, then place everything shifted by the scroll offset
    const int content = S(y), view = pc.bottom;
    g_scroll[p] = std::clamp(g_scroll[p], 0, (std::max)(0, content - view));
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = (std::max)(0, content - 1);
    si.nPage = static_cast<UINT>((std::max)(1, view));
    si.nPos = g_scroll[p];
    SetScrollInfo(page, SB_VERT, &si, TRUE);
    HDWP dw = BeginDeferWindowPos(static_cast<int>(pl.size()));
    for (const Pl& it : pl) {
        HWND h = Ctl(it.id);
        if (!h) continue;
        const UINT fl = SWP_NOZORDER | SWP_NOACTIVATE | (it.vis ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
        if (dw)
            dw = DeferWindowPos(dw, h, nullptr, S(it.x), S(it.y) - g_scroll[p], S(it.w), S(it.h), fl);
        else
            SetWindowPos(h, nullptr, S(it.x), S(it.y) - g_scroll[p], S(it.w), S(it.h), fl);
    }
    if (dw) EndDeferWindowPos(dw);
    InvalidateRect(page, nullptr, TRUE);
}

static void LayoutPages() {
    for (int p = 0; p < kPages; ++p) LayoutPage(p);
}

static void Layout() {
    // fonts for every control (headers bold)
    for (int id = IDC_TAB; id < IDC_LAST_ID; ++id) {
        HWND h = Ctl(id);
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(IsHeaderId(id) ? g_fontBold : g_font), FALSE);
    }
    // left column: preview, start / pause, status, scenes
    struct L { int id, x, y, w, h; };
    const int lx = kLeftX, lw = kLeftW;
    const int sb = (lw - 3 * 6) / 4;
    const int pb = (lw - 2 * 6) / 3;
    const L items[] = {
        {IDC_PREV_LABEL, lx, 10, 260, 20},
        {IDC_FPS_VALUE, lx + 262, 10, lw - 262, 20},
        {IDC_PREVIEW, lx, 34, lw, 225},
        {IDC_START, lx, 269, lw - 138, 40},
        {IDC_PAUSEBTN, lx + lw - 132, 269, 132, 40},
        {IDC_STATUS, lx, 317, lw, 54},
        {IDC_SCENE_HDR, lx, 379, lw, 20},
        {IDC_SCENE1, lx, 403, sb, 30},
        {IDC_SCENE2, lx + (sb + 6), 403, sb, 30},
        {IDC_SCENE3, lx + 2 * (sb + 6), 403, sb, 30},
        {IDC_SCENE4, lx + 3 * (sb + 6), 403, lw - 3 * (sb + 6), 30},
        {IDC_PROF_LABEL, lx, 447, 56, 20},
        {IDC_PROFILE, lx + 58, 443, lw - 58, 240},
        {IDC_PROF_LOAD, lx, 475, pb, 28},
        {IDC_PROF_SAVE, lx + pb + 6, 475, pb, 28},
        {IDC_PROF_DEL, lx + 2 * (pb + 6), 475, lw - 2 * (pb + 6), 28},
        {IDC_PROF_HINT, lx, 511, lw, 80},
        {IDC_TAB, kRightX, 8, kClientW - kRightX - 12, kClientH - 16},
    };
    for (const auto& it : items) {
        HWND h = Ctl(it.id);
        if (h) SetWindowPos(h, nullptr, S(it.x), S(it.y), S(it.w), S(it.h), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // pages fill the tab's display area
    RECT tr{S(kRightX), S(8), S(kClientW - 12), S(kClientH - 8)};
    SendMessageW(Ctl(IDC_TAB), TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&tr));
    for (int p = 0; p < kPages; ++p)
        if (g_pageWnd[p])
            SetWindowPos(g_pageWnd[p], HWND_TOP, tr.left, tr.top, tr.right - tr.left, tr.bottom - tr.top,
                         SWP_NOACTIVATE);
    SetWindowPos(Ctl(IDC_TAB), HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    LayoutPages();
    InvalidateRect(g_main, nullptr, TRUE);
}

static void ShowPage(int page) {
    g_page = std::clamp(page, 0, kPages - 1);
    for (int p = 0; p < kPages; ++p)
        if (g_pageWnd[p]) ShowWindow(g_pageWnd[p], p == g_page ? SW_SHOW : SW_HIDE);
}

static void ToggleSection(int bit) {
    g_expanded ^= bit;
    IniSetInt(kMainSec, L"UiExpanded", g_expanded);
    LayoutPage(g_page);
}

static LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    int p = 0;
    for (int i = 0; i < kPages; ++i)
        if (g_pageWnd[i] == hwnd) p = i;
    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
        return g_main ? SendMessageW(g_main, msg, wp, lp) : 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        SetBkColor(dc, kBgPage);
        if (msg == WM_CTLCOLORSTATIC) {
            if (IsHeaderId(id))
                SetTextColor(dc, kHdrText);
            else if (id == IDC_MEDIA_NOW || id == IDC_LOGO_NAME || id == IDC_SRC_VALUE || id == IDC_SRC2_VALUE ||
                     id == IDC_PHONE_STATUS)
                SetTextColor(dc, kHintText);
        }
        return reinterpret_cast<LRESULT>(g_brPage);
    }
    case WM_VSCROLL: {
        if (lp) return 0;  // from a child (the % up-down): not the page scroll bar
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= S(30); break;
        case SB_LINEDOWN: pos += S(30); break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        default: return 0;
        }
        if (pos != g_scroll[p]) {
            g_scroll[p] = pos;
            LayoutPage(p);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int old = g_scroll[p];
        g_scroll[p] -= GET_WHEEL_DELTA_WPARAM(wp) * S(60) / WHEEL_DELTA;
        LayoutPage(p);
        (void)old;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
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
    // quick buttons 1-4 = the first four scenes in the list
    for (int i = 0; i < 4; ++i) {
        const bool have = i < static_cast<int>(names.size());
        std::wstring t = std::to_wstring(i + 1) + L"  " + (have ? names[i] : std::wstring(L"—"));
        SetTextIfChanged(Ctl(IDC_SCENE1 + i), t);
        EnableWindow(Ctl(IDC_SCENE1 + i), have);
    }
}

// settings -> controls (after hotkeys, profiles, startup)
// --- autostart with Windows (HKCU Run)
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool AutostartOn() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD sz = sizeof(buf), type = 0;
    const bool on = RegQueryValueExW(k, L"WVC", nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on && wcsstr(buf, ExePath().c_str()) != nullptr;
}

static bool SetAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --tray";
        rc = RegSetValueExW(k, L"WVC", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(k, L"WVC");
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

static void FillMediaList() {
    Settings s = GetSettings();
    HWND h = Ctl(IDC_MEDIA_LIST);
    const int sel = static_cast<int>(SendMessageW(h, LB_GETCURSEL, 0, 0));
    SendMessageW(h, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < s.media.size(); ++i) {
        std::wstring t = std::to_wstring(i + 1) + L". " + FileNameOf(s.media[i]);
        SendMessageW(h, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
    }
    if (sel >= 0 && sel < static_cast<int>(s.media.size())) SendMessageW(h, LB_SETCURSEL, sel, 0);
}

// settings -> controls (after hotkeys, scenes, startup)
static void SyncControls() {
    g_syncing = true;
    Settings s = GetSettings();
    ComboSet(IDC_QUALITY, s.quality);
    ComboSet(IDC_ASPECT, s.aspect);
    ComboSet(IDC_FPS, s.fps == 60 ? 1 : 0);
    ComboSet(IDC_FIT, s.fit);
    ComboSet(IDC_ROT, s.rotation);
    ComboSet(IDC_ZOOM, s.zoom);
    ComboSet(IDC_FRAMESHAPE, s.frameShape);
    ComboSet(IDC_SHAPECOLOR, s.shapeColor);
    CheckSet(IDC_MIRROR, s.mirror);
    CheckSet(IDC_CURSOR, s.cursor);
    ComboSet(IDC_WINCAP, s.winCapture);
    ComboSet(IDC_VIEW, s.mainSrc);
    ComboSet(IDC_LAYOUT, s.layout);
    ComboSet(IDC_PIPSRC, std::clamp(s.pipSrc, 1, 5) - 1);
    CheckSet(IDC_KEEPCAM, s.keepCamOn);
    SetTextIfChanged(Ctl(IDC_PHONE_IP), s.phoneIp);
    ComboSet(IDC_PHONE_RES, s.phoneRes);
    SetTextIfChanged(Ctl(IDC_PAUSETEXT), s.pauseText);
    ComboSet(IDC_PIPCORNER, s.pipCorner);
    SendMessageW(Ctl(IDC_PIPSIZE_UD), UDM_SETPOS32, 0, std::clamp((s.pipW + 50) / 100, 5, 90));
    ComboSet(IDC_TRANS, s.transition);
    ComboSet(IDC_TRANSDUR, s.transDur);
    CheckSet(IDC_IDLEFRAME, s.idleFrame);
    ComboSet(IDC_PIPSHAPE, s.pipShape);
    CheckSet(IDC_CAMMIRROR, s.camMirror);
    SetTextIfChanged(Ctl(IDC_TEXT), s.text);
    CheckSet(IDC_TEXTON, s.textOn);
    ComboSet(IDC_TEXTPOS, s.textPos);
    ComboSet(IDC_TEXTSIZE, s.textSize);
    ComboSet(IDC_LOGOCORNER, s.logoCorner);
    ComboSet(IDC_LOGOSIZE, s.logoSize);
    SetTextIfChanged(Ctl(IDC_LOGO_NAME), s.logo.empty() ? L"Логотип не выбран" : L"Логотип: " + FileNameOf(s.logo));
    ComboSet(IDC_PAUSE, s.pauseMode);
    ComboSet(IDC_IMGSEC, s.imageSec);
    CheckSet(IDC_AUTOSTART, AutostartOn());
    CheckSet(IDC_AUTOSTREAM, IniInt(kMainSec, L"AutoStream", 0) != 0);
    FillCamCombo();
    FillMediaList();
    g_syncing = false;
}

static const wchar_t* SrcKindName(int mainKind) {
    switch (mainKind) {
    case 1: return L"веб-камера";
    case 2: return L"медиафайлы";
    case 3: return L"Экран 2";
    case 4: return L"телефон";
    default: return L"Экран 1";
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
    swprintf(buf, 256, L"область %d × %d  (x %d, y %d)%ls", static_cast<int>(s.region2.right - s.region2.left),
             static_cast<int>(s.region2.bottom - s.region2.top), static_cast<int>(s.region2.left),
             static_cast<int>(s.region2.top), s.mainSrc == 3 ? L" — сейчас главный" : SecondSrc(s) == 4 ? L" — второй" : L"");
    SetTextIfChanged(Ctl(IDC_SRC2_VALUE), buf);
    std::wstring now;
    {
        std::lock_guard<std::mutex> lk(g_mediaNameMx);
        now = g_mediaNow;
    }
    SetTextIfChanged(Ctl(IDC_MEDIA_NOW), g_media.status == 2 && !now.empty() ? L"Сейчас: " + now : L"");
}
static void UpdateStartButton() {
    SetWindowTextW(Ctl(IDC_START), g_streaming ? L"■  Стоп" : L"▶  Старт");
    SetWindowTextW(Ctl(IDC_PAUSEBTN), g_streaming && g_paused ? L"▶  Продолжить" : L"⏸  Пауза");
    const wchar_t* tip = !g_streaming ? L"WVC — остановлено" : g_paused ? L"WVC — пауза" : L"WVC — трансляция идёт";
    wcsncpy(g_nid.szTip, tip, ARRAYSIZE(g_nid.szTip) - 1);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// --- phone section on «Источники»
static std::vector<PhoneFound> g_phoneList;  // what the combo shows
static std::wstring g_phoneNote;             // result of the last search / command while not connected

static bool PhoneWanted(const Settings& s) {
    const int user = g_phoneUser;
    return !s.phoneIp.empty() && (user == 1 || (user == 0 && g_streaming && PhoneNeeded(s)));
}

static void FillPhoneCombo() {
    Settings s = GetSettings();
    if (!s.phoneIp.empty()) {
        bool have = false;
        for (const auto& p : g_phoneList) have = have || p.ip == s.phoneIp;
        if (!have) g_phoneList.insert(g_phoneList.begin(), PhoneFound{L"Последний", s.phoneIp});
    }
    HWND h = Ctl(IDC_PHONE_LIST);
    const bool was = g_syncing;
    g_syncing = true;
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < g_phoneList.size(); ++i) {
        const std::wstring t = g_phoneList[i].name + L" — " + g_phoneList[i].ip;
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
        if (g_phoneList[i].ip == s.phoneIp) sel = static_cast<int>(i);
    }
    SendMessageW(h, CB_SETCURSEL, sel, 0);
    g_syncing = was;
}

static void UpdatePhoneUi() {
    if (!g_main) return;
    Settings s = GetSettings();
    std::wstring st, name, err;
    {
        std::lock_guard<std::mutex> lk(g_phoneMx);
        st = g_phoneState;
        name = g_phoneName;
        err = g_phoneErr;
    }
    const int status = g_phone.status;
    std::wstring t;
    if (!g_wsaOk) {
        t = L"Сеть Windows недоступна: телефон подключить нельзя.";
    } else if (g_phoneSearching) {
        t = L"Ищу телефоны в сети…";
    } else if (status == 2) {
        t = L"Подключено: " + (name.empty() ? std::wstring(L"телефон") : name) + L" (" + s.phoneIp + L") — " +
            PhoneStateName(st);
        if (st == L"need_consent") {
            t += L". Подтвердите запись экрана на телефоне.";
        } else {
            wchar_t b[64];
            swprintf(b, 64, L", %.1f кадров/с.", g_phoneFpsX10 / 10.0);
            t += b;
        }
    } else if (status == 1) {
        t = L"Подключаюсь к " + s.phoneIp + L"…";
    } else if (status == 3) {
        t = L"Нет связи с " + s.phoneIp + L" (" + err +
            L"). Пробую снова каждые 3 с. Проверьте: WVC Phone открыт, телефон в той же сети Wi-Fi, WVC разрешён в "
            L"брандмауэре Windows.";
    } else if (!g_phoneNote.empty()) {
        t = g_phoneNote;
    } else if (s.phoneIp.empty()) {
        t = L"Откройте WVC Phone на телефоне и нажмите «Найти» или введите IP-адрес с экрана телефона.";
    } else {
        t = L"Не подключено. WVC подключится сам, когда «Телефон» выбран источником и идёт трансляция; или нажмите "
            L"«Подключить».";
    }
    SetTextIfChanged(Ctl(IDC_PHONE_STATUS), t);
    SetTextIfChanged(Ctl(IDC_PHONE_CONN), PhoneWanted(s) ? L"Отключить" : L"Подключить");
    SetTextIfChanged(Ctl(IDC_PHONE_FIND), g_phoneSearching ? L"Ищу…" : L"Найти");
    static const wchar_t* names[3] = {L"Задняя камера", L"Передняя камера", L"Экран телефона"};
    for (int i = 0; i < 3; ++i)
        SetTextIfChanged(Ctl(IDC_PHONE_BACK + i), (s.phoneSrc == i ? L"● " : L"") + std::wstring(names[i]));
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
    else if (g_streaming && CamNeeded(s) && s.camLink.empty())
        line2 = L"Выберите веб-камеру на вкладке «Источники».";
    else if (g_streaming && CamNeeded(s) && g_web.status == 3)
        line2 = L"Веб-камера недоступна: занята другой программой или отключена. Пробую снова…";
    else if (g_streaming && MediaNeeded(s) && s.media.empty())
        line2 = L"Добавьте медиафайлы на вкладке «Источники».";
    else if (g_streaming && PhoneNeeded(s) && s.phoneIp.empty())
        line2 = L"Телефон: нажмите «Найти» или введите IP на вкладке «Источники».";
    else if (g_streaming && PhoneNeeded(s) && g_phone.status == 3)
        line2 = L"Телефон не отвечает: проверьте WVC Phone, Wi-Fi и брандмауэр. Пробую снова…";
    else if (g_streaming && g_paused)
        line2 = L"Пауза: в камере картинка паузы. «Продолжить» — вернуть трансляцию.";
    else if (g_streaming && g_sourceLost)
        line2 = L"Окно свёрнуто или закрыто: показываю последний кадр.";
    else if (g_streaming) {
        line2 = L"Трансляция идёт: ";
        line2 += SrcKindName(s.mainSrc);
        if (SecondSrc(s)) {
            line2 += s.layout == 2 ? L", рядом — " : L", в окошке — ";
            line2 += SrcKindName(SecondSrc(s) - 1);
        }
        line2 += L".";
    } else if (s.keepCamOn && !s.camLink.empty() && g_web.status == 3)
        line2 = L"Веб-камера не включилась (занята или отключена). Пробую снова…";
    else if (s.keepCamOn && !s.camLink.empty() && g_web.status == 2)
        line2 = L"Остановлено. Веб-камера держится включённой.";
    else
        line2 = L"Остановлено: в камере картинка паузы.";
    SetTextIfChanged(Ctl(IDC_STATUS), line1 + L"\r\n" + line2);

    wchar_t fps[64];
    swprintf(fps, 64, L"%.1f кадров/с", g_fpsX10 / 10.0);
    SetTextIfChanged(Ctl(IDC_FPS_VALUE), g_cam ? fps : L"");
    UpdateSourceLabel();
    UpdatePhoneUi();
}

static bool HaveSource() {
    Settings s = GetSettings();
    if (s.mainSrc != 0) return true;  // the card says what is missing
    return s.srcMode == 1 ? (s.srcWindow && IsWindow(s.srcWindow)) : s.hasRegion;
}

static void SyncSources() {
    WebcamSync();
    MediaSync();
    PhoneSync();
}

static void ApplyPhoneIp(std::wstring ip) {
    while (!ip.empty() && iswspace(ip.back())) ip.pop_back();
    while (!ip.empty() && iswspace(ip.front())) ip.erase(ip.begin());
    if (ip != GetSettings().phoneIp) {
        {
            std::lock_guard<std::mutex> lk(g_setMx);
            g_set.phoneIp = ip;
        }
        SaveSettings();
        g_phoneNote.clear();
    }
    const bool was = g_syncing;
    g_syncing = true;
    SetTextIfChanged(Ctl(IDC_PHONE_IP), ip);
    g_syncing = was;
    SyncSources();
    UpdateStatus();
}

static std::wstring PhoneIpFromEdit() {
    wchar_t buf[80] = {};
    GetWindowTextW(Ctl(IDC_PHONE_IP), buf, 80);
    return buf;
}

static void TogglePhoneConnection() {
    ApplyPhoneIp(PhoneIpFromEdit());
    Settings s = GetSettings();
    if (PhoneWanted(s)) {
        g_phoneUser = -1;  // stay disconnected until «Подключить» or the next «Старт»
        g_phoneNote = L"Отключено.";
    } else if (s.phoneIp.empty()) {
        g_phoneNote = L"Введите IP-адрес телефона или нажмите «Найти».";
    } else {
        g_phoneUser = 1;
        g_phoneNote.clear();
    }
    SyncSources();
    UpdateStatus();
}

static void SetPhoneSource(int src) {
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.phoneSrc = std::clamp(src, 0, 2);
    }
    SaveSettings();
    if (!PhoneSendSource(src)) g_phoneNote = L"Телефон не подключён: источник переключится при подключении.";
    UpdateStatus();
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
    g_paused = false;
    g_note.clear();
    if (g_streaming && g_phoneUser == -1) g_phoneUser = 0;  // «Старт» brings back auto-connect
    SyncSources();
    UpdateStartButton();
    UpdateFrame();
    UpdateStatus();
}
// Pause: the camera shows the pause picture, sources stay open so "Continue" is instant
static void TogglePause() {
    if (!g_streaming) {
        g_note = L"Пауза работает во время трансляции: сначала нажмите «Старт».";
        UpdateStatus();
        return;
    }
    g_paused = !g_paused;
    g_note.clear();
    UpdateStartButton();
    UpdateFrame();
    UpdateStatus();
}

// --- "hide" the source window off screen so it keeps playing while WVC captures it directly
static HWND g_hiddenWin = nullptr;
static RECT g_hiddenRect{};
static bool g_hiddenMax = false;

static void RestoreHiddenWindow() {
    if (!g_hiddenWin) return;
    if (IsWindow(g_hiddenWin)) {
        SetWindowPos(g_hiddenWin, nullptr, g_hiddenRect.left, g_hiddenRect.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_hiddenMax) ShowWindow(g_hiddenWin, SW_MAXIMIZE);
    }
    g_hiddenWin = nullptr;
    if (g_main) SetWindowTextW(Ctl(IDC_HIDEWIN), L"Спрятать окно за экран");
}

static void ToggleHideWindow() {
    if (g_hiddenWin) {
        RestoreHiddenWindow();
        return;
    }
    Settings s = GetSettings();
    if (s.srcMode != 1 || !s.srcWindow || !IsWindow(s.srcWindow)) {
        MessageBoxW(g_main, L"Сначала выберите окно как источник (кнопка «Окно ▾»).", L"WVC", MB_ICONINFORMATION);
        return;
    }
    HWND w = s.srcWindow;
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    g_hiddenMax = IsZoomed(w) != FALSE;
    if (g_hiddenMax) ShowWindow(w, SW_RESTORE);
    GetWindowRect(w, &g_hiddenRect);
    RECT vs = VirtualScreen();
    SetWindowPos(w, nullptr, vs.right + 200, g_hiddenRect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_hiddenWin = w;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.winCapture = 1;  // the screen no longer shows it
    }
    SaveSettings();
    ComboSet(IDC_WINCAP, 1);
    SetWindowTextW(Ctl(IDC_HIDEWIN), L"Вернуть окно на экран");
    g_note = L"Окно спрятано за край экрана и продолжает работать. Вернуть — той же кнопкой.";
    UpdateStatus();
}
static void RestartCameraForFormat() {
    StopCamera();
    if (!StartCamera()) MessageBoxW(g_main, L"Не удалось перезапустить камеру.", L"WVC", MB_ICONERROR);
    g_note = L"Формат изменён: переподключите камеру в Zoom/Telegram (выключите и включите видео).";
    UpdateStatus();
}

static void SetRegionSource(const RECT& r) {
    RestoreHiddenWindow();
    ++g_transGen;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.srcMode = 0;
        g_set.srcWindow = nullptr;
        g_set.region = r;
        g_set.hasRegion = true;
        if (g_set.mainSrc != 0 && SecondSrc(g_set) != 1) g_set.mainSrc = 0;  // picking a screen source = show it
    }
    SaveSettings();
    SyncControls();
    UpdateSourceLabel();
    UpdateFrame();
}

// "Экран 2": a fixed area. Picking it does not switch the camera to it: «Экран 1 ⇄ 2» does.
static void SetRegion2Source(const RECT& r) {
    bool shown;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.region2 = r;
        shown = Screen2Needed(g_set);
    }
    if (shown) ++g_transGen;
    SaveSettings();
    UpdateSourceLabel();
    UpdateFrame();
    if (!shown) {
        g_note = L"Область 2 сохранена. Показать её: «Экран 1 ⇄ 2»" +
                 (g_hk[HA_SCREEN12] ? L" или " + HotkeyName(g_hk[HA_SCREEN12]) : std::wstring()) + L".";
        UpdateStatus();
    }
}

static void DoSelectRegionN(int which) {
    bool wasVisible = IsWindowVisible(g_main) != FALSE;
    ShowWindow(g_main, SW_HIDE);
    HideFrames();
    Sleep(150);  // let the windows disappear before the overlay appears
    RECT r;
    if (SelectRegion(g_hInst, &r)) {
        if (which == 1)
            SetRegion2Source(r);
        else
            SetRegionSource(r);
    }
    if (wasVisible) {
        ShowWindow(g_main, SW_SHOW);
        SetForegroundWindow(g_main);
    }
    UpdateFrame();
}
static void DoSelectRegion() { DoSelectRegionN(0); }

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

static void ShowMonitorMenu(int which) {
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
    GetWindowRect(Ctl(which == 1 ? IDC_MONITOR2 : IDC_MONITOR), &br);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, br.left, br.bottom, 0, g_main, nullptr);
    DestroyMenu(m);
    if (cmd >= 1000 && cmd < 1000 + static_cast<int>(mons.size())) {
        if (which == 1)
            SetRegion2Source(mons[cmd - 1000].rc);
        else
            SetRegionSource(mons[cmd - 1000].rc);
    }
}

static void SetWindowSource(HWND hwnd, const std::wstring& title) {
    if (hwnd != g_hiddenWin) RestoreHiddenWindow();
    ++g_transGen;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set.srcMode = 1;
        g_set.srcWindow = hwnd;
        g_set.srcTitle = title;
        if (g_set.mainSrc != 0 && SecondSrc(g_set) != 1) g_set.mainSrc = 0;
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
    {
        RECT vs = VirtualScreen(), inter;
        if (!IntersectRect(&inter, &vs, &s.region2)) {
            SetRegion2Source(DefaultRegion2());
            g_note = L"Область 2 оказалась за пределами экрана и перенесена на основной монитор.";
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
    RestoreHiddenWindow();
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
    ++g_mediaGen;
    ++g_transGen;
    g_mediaIndex = 0;
    IniSetStr(kMainSec, L"LastScene", name);
    SaveSettings();
    SyncControls();
    SyncSources();
    UpdateFrame();
    g_note = note.empty() ? L"Сцена «" + name + L"» загружена." : note;
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
    IniSetStr(kMainSec, L"LastScene", name);
    g_note = L"Сцена «" + name + L"» сохранена.";
    UpdateStatus();
}

static void DeleteProfile(const std::wstring& name) {
    if (name.empty()) return;
    std::wstring q = L"Удалить сцену «" + name + L"»?";
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

// --- JSON export / import of everything in settings.ini (all sections: main, scenes, hotkeys)
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

static std::wstring JsonStr(const std::wstring& v) {
    std::wstring o = L"\"";
    for (wchar_t c : v) {
        switch (c) {
        case L'"': o += L"\\\""; break;
        case L'\\': o += L"\\\\"; break;
        case L'\n': o += L"\\n"; break;
        case L'\r': o += L"\\r"; break;
        case L'\t': o += L"\\t"; break;
        default:
            if (c < 0x20) {
                wchar_t b[8];
                swprintf(b, 8, L"\\u%04x", static_cast<unsigned>(c));
                o += b;
            } else {
                o += c;
            }
        }
    }
    return o + L"\"";
}

static std::vector<std::wstring> SplitZ(const std::vector<wchar_t>& buf, DWORD n) {
    std::vector<std::wstring> v;
    size_t i = 0;
    while (i < n && buf[i]) {
        std::wstring s(&buf[i]);
        i += s.size() + 1;
        v.push_back(s);
    }
    return v;
}

static bool ExportJson(const std::wstring& path) {
    const std::wstring ini = IniPath();
    std::vector<wchar_t> names(65536);
    DWORD n = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), ini.c_str());
    std::wstring j = L"{\n  \"app\": \"WVC\",\n  \"format\": 1,\n  \"sections\": {";
    bool firstSec = true;
    for (const auto& sec : SplitZ(names, n)) {
        std::vector<wchar_t> body(262144);
        DWORD m = GetPrivateProfileSectionW(sec.c_str(), body.data(), static_cast<DWORD>(body.size()), ini.c_str());
        j += firstSec ? L"\n    " : L",\n    ";
        firstSec = false;
        j += JsonStr(sec) + L": {";
        bool firstKey = true;
        for (const auto& line : SplitZ(body, m)) {
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            j += firstKey ? L"\n      " : L",\n      ";
            firstKey = false;
            j += JsonStr(line.substr(0, eq)) + L": " + JsonStr(line.substr(eq + 1));
        }
        j += L"\n    }";
    }
    j += L"\n  }\n}\n";
    const std::string u = ToUtf8(j);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL ok = WriteFile(f, u.data(), static_cast<DWORD>(u.size()), &w, nullptr);
    CloseHandle(f);
    return ok && w == u.size();
}

// minimal JSON reader for {"sections": {"name": {"key": "value", ...}, ...}}
struct JsonReader {
    const std::wstring& t;
    size_t i = 0;
    bool bad = false;
    explicit JsonReader(const std::wstring& text) : t(text) {}
    void Ws() {
        while (i < t.size() && (t[i] == L' ' || t[i] == L'\n' || t[i] == L'\r' || t[i] == L'\t' || t[i] == 0xFEFF)) ++i;
    }
    bool Eat(wchar_t c) {
        Ws();
        if (i < t.size() && t[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    std::wstring Str() {
        Ws();
        std::wstring o;
        if (i >= t.size() || t[i] != L'"') {
            bad = true;
            return o;
        }
        ++i;
        while (i < t.size() && t[i] != L'"') {
            wchar_t c = t[i++];
            if (c == L'\\' && i < t.size()) {
                wchar_t e = t[i++];
                switch (e) {
                case L'n': o += L'\n'; break;
                case L'r': o += L'\r'; break;
                case L't': o += L'\t'; break;
                case L'b': o += L'\b'; break;
                case L'f': o += L'\f'; break;
                case L'u':
                    if (i + 4 <= t.size()) {
                        o += static_cast<wchar_t>(wcstoul(t.substr(i, 4).c_str(), nullptr, 16));
                        i += 4;
                    }
                    break;
                default: o += e;
                }
            } else {
                o += c;
            }
        }
        if (i >= t.size()) bad = true;
        ++i;
        return o;
    }
    // a value: string, number, true/false/null -> text; objects/arrays skipped
    std::wstring Scalar() {
        Ws();
        if (i < t.size() && t[i] == L'"') return Str();
        if (i < t.size() && (t[i] == L'{' || t[i] == L'[')) {
            Skip();
            return L"";
        }
        size_t s0 = i;
        while (i < t.size() && t[i] != L',' && t[i] != L'}' && t[i] != L']' && t[i] > L' ') ++i;
        std::wstring v = t.substr(s0, i - s0);
        if (v == L"true") return L"1";
        if (v == L"false" || v == L"null") return L"0";
        return v;
    }
    void Skip() {
        Ws();
        if (i >= t.size()) return;
        if (t[i] == L'"') {
            Str();
            return;
        }
        if (t[i] == L'{' || t[i] == L'[') {
            const wchar_t close = t[i] == L'{' ? L'}' : L']';
            ++i;
            Ws();
            if (Eat(close)) return;
            do {
                if (close == L'}') {
                    Str();
                    if (!Eat(L':')) { bad = true; return; }
                }
                Skip();
            } while (!bad && Eat(L','));
            if (!Eat(close)) bad = true;
            return;
        }
        Scalar();
    }
};

typedef std::vector<std::pair<std::wstring, std::vector<std::pair<std::wstring, std::wstring>>>> IniDump;

static bool ParseJsonSections(const std::wstring& text, IniDump& out, bool* isWvc) {
    JsonReader r(text);
    *isWvc = false;
    if (!r.Eat(L'{')) return false;
    if (r.Eat(L'}')) return true;
    do {
        std::wstring key = r.Str();
        if (r.bad || !r.Eat(L':')) return false;
        if (key == L"app") {
            *isWvc = r.Scalar() == L"WVC";
        } else if (key == L"sections") {
            if (!r.Eat(L'{')) return false;
            if (!r.Eat(L'}')) {
                do {
                    std::wstring sec = r.Str();
                    if (r.bad || !r.Eat(L':') || !r.Eat(L'{')) return false;
                    out.push_back({sec, {}});
                    if (!r.Eat(L'}')) {
                        do {
                            std::wstring k = r.Str();
                            if (r.bad || !r.Eat(L':')) return false;
                            out.back().second.push_back({k, r.Scalar()});
                        } while (!r.bad && r.Eat(L','));
                        if (!r.Eat(L'}')) return false;
                    }
                } while (!r.bad && r.Eat(L','));
                if (!r.Eat(L'}')) return false;
            }
        } else {
            r.Skip();
        }
    } while (!r.bad && r.Eat(L','));
    return !r.bad && r.Eat(L'}');
}

static bool ImportJsonFile(const std::wstring& path, std::wstring* err) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *err = L"Не удалось открыть файл.";
        return false;
    }
    std::string bytes;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) bytes.append(buf, got);
    CloseHandle(f);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF) bytes.erase(0, 3);
    IniDump dump;
    bool isWvc = false;
    if (!ParseJsonSections(FromUtf8(bytes), dump, &isWvc) || dump.empty()) {
        *err = L"Файл не похож на настройки WVC (ошибка в JSON).";
        return false;
    }
    if (!isWvc) {
        *err = L"В файле нет пометки \"app\": \"WVC\".";
        return false;
    }
    // replace the whole ini
    const std::wstring ini = IniPath();
    HANDLE w = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (w != INVALID_HANDLE_VALUE) {
        const unsigned char bom[2] = {0xFF, 0xFE};
        DWORD wr = 0;
        WriteFile(w, bom, 2, &wr, nullptr);
        CloseHandle(w);
    }
    for (const auto& sec : dump)
        for (const auto& kv : sec.second)
            WritePrivateProfileStringW(sec.first.c_str(), kv.first.c_str(), kv.second.c_str(), ini.c_str());
    return true;
}

static bool PickJsonFile(bool save, std::wstring* out) {
    wchar_t file[MAX_PATH * 2] = L"WVC-настройки.json";
    if (!save) file[0] = 0;
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"Настройки WVC (*.json)\0*.json\0Все файлы\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
    *out = file;
    return true;
}
static void AfterLayersChanged() {
    ++g_transGen;
    SaveSettings();
    SyncControls();
    SyncSources();
    UpdateFrame();
    UpdateStatus();
}

static void SwapLayers() {
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        if (g_set.layout == 0) return;
        const int big = g_set.mainSrc;
        g_set.mainSrc = std::clamp(g_set.pipSrc, 1, 5) - 1;
        g_set.pipSrc = big + 1;
    }
    AfterLayersChanged();
}

// Main source Экран 1 <-> Экран 2. No frame moves. If the second source shows the target, they trade places.
static void ToggleScreens() {
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        const int old = g_set.mainSrc;
        const int now = old == 0 ? 3 : 0;
        if (g_set.layout != 0 && std::clamp(g_set.pipSrc, 1, 5) == now + 1) g_set.pipSrc = old + 1;
        g_set.mainSrc = now;
    }
    AfterLayersChanged();
}

static void OnPipDragged() {
    SaveSettings();
    g_syncing = true;
    SendMessageW(Ctl(IDC_PIPSIZE_UD), UDM_SETPOS32, 0, std::clamp((GetSettings().pipW + 50) / 100, 5, 90));
    g_syncing = false;
    UpdateFrame();
}

static void MediaListChanged() {
    ++g_mediaGen;
    g_mediaIndex = 0;
    SaveSettings();
    FillMediaList();
    MediaSync();
}

static void MediaAdd() {
    std::vector<wchar_t> buf(32768, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter =
        L"Картинки и видео\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.mp4;*.m4v;*.mov;*.avi;*.wmv;*.mkv;*.webm;*.mpg;*.mpeg;*.ts;*.3gp\0"
        L"Все файлы\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::wstring> files;
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) {
        files.push_back(first);  // one file: full path
    } else {
        while (*p) {  // directory, then names
            std::wstring name = p;
            files.push_back(first + L"\\" + name);
            p += name.size() + 1;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        for (auto& f : files) g_set.media.push_back(f);
        if (g_set.media.size() > 500) g_set.media.resize(500);
    }
    MediaListChanged();
}

static void MediaRemove() {
    const int sel = static_cast<int>(SendMessageW(Ctl(IDC_MEDIA_LIST), LB_GETCURSEL, 0, 0));
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        if (sel < 0 || sel >= static_cast<int>(g_set.media.size())) return;
        g_set.media.erase(g_set.media.begin() + sel);
    }
    MediaListChanged();
}

static void MediaMove(int dir) {
    const int sel = static_cast<int>(SendMessageW(Ctl(IDC_MEDIA_LIST), LB_GETCURSEL, 0, 0));
    int to = sel + dir;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        if (sel < 0 || to < 0 || to >= static_cast<int>(g_set.media.size())) return;
        std::swap(g_set.media[sel], g_set.media[to]);
    }
    MediaListChanged();
    SendMessageW(Ctl(IDC_MEDIA_LIST), LB_SETCURSEL, to, 0);
}

static void ReloadAllSettings() {
    Settings old = GetSettings();
    Settings s;
    ReadSettings(kMainSec, s, true);
    if (s.srcMode == 1) {
        s.srcWindow = FindWindowByTitle(s.srcTitle);
        if (!s.srcWindow) s.srcMode = 0;
    }
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        g_set = s;
    }
    LoadHotkeys();
    RegisterHotkeys(g_main);
    SetTextIfChanged(Ctl(IDC_HK_LIST), HotkeyListText());
    FillProfileCombo(IniStr(kMainSec, L"LastScene"));
    ++g_mediaGen;
    g_mediaIndex = 0;
    g_pauseDirty = true;
    SyncControls();
    if (old.quality != s.quality || old.aspect != s.aspect || old.fps != s.fps) RestartCameraForFormat();
    SyncSources();
    UpdateFrame();
    UpdateStatus();
}

static void DoExportJson() {
    std::wstring f;
    if (!PickJsonFile(true, &f)) return;
    SaveSettings();
    SaveHotkeys();
    if (ExportJson(f))
        g_note = L"Настройки сохранены в " + FileNameOf(f) + L".";
    else
        MessageBoxW(g_main, L"Не удалось записать файл.", L"WVC", MB_ICONERROR);
    UpdateStatus();
}

static void DoImportJson() {
    std::wstring f;
    if (!PickJsonFile(false, &f)) return;
    if (MessageBoxW(g_main, L"Заменить все текущие настройки, сцены и горячие клавиши настройками из файла?", L"WVC",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    std::wstring err;
    if (!ImportJsonFile(f, &err)) {
        MessageBoxW(g_main, err.c_str(), L"WVC", MB_ICONERROR);
        return;
    }
    ReloadAllSettings();
    g_note = L"Настройки загружены из " + FileNameOf(f) + L".";
    UpdateStatus();
}

static void OnHotkey(int action) {
    switch (action) {
    case HA_TOGGLE: ToggleStreaming(); return;
    case HA_SELECT: DoSelectRegion(); return;
    case HA_MONITOR: NextMonitor(); return;
    case HA_CURSOR: SetCursorOption(!GetSettings().cursor); return;
    case HA_SWAP: SwapLayers(); return;
    case HA_MEDIA_NEXT: MediaNext(); return;
    case HA_HIDE_WINDOW: ToggleHideWindow(); return;
    case HA_PAUSE: TogglePause(); return;
    case HA_SCREEN12: ToggleScreens(); return;
    case HA_SCREEN_CAM:
    case HA_PIP:
    case HA_TEXT: {
        {
            std::lock_guard<std::mutex> lk(g_setMx);
            if (action == HA_SCREEN_CAM) g_set.mainSrc = g_set.mainSrc == 1 ? 0 : 1;
            if (action == HA_PIP) {
                if (g_set.layout != 0) {
                    g_set.layout = 0;
                } else {
                    g_set.layout = 1;
                    if (std::clamp(g_set.pipSrc, 1, 5) == g_set.mainSrc + 1) g_set.pipSrc = g_set.mainSrc == 1 ? 1 : 2;
                }
            }
            if (action == HA_TEXT) g_set.textOn = !g_set.textOn;
        }
        AfterLayersChanged();
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

static HWND g_mkParent = nullptr;  // parent for the controls being created

static HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_mkParent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    if (h && id > 0 && id <= IDC_LAST_ID) g_ctl[id] = h;
    if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(IsHeaderId(id) ? g_fontBold : g_font), FALSE);
    return h;
}

static HWND AddCombo(int id, std::initializer_list<const wchar_t*> items) {
    HWND h = Mk(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id);
    for (auto* s : items) SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    return h;
}

static void CreateControls(HWND hwnd) {
    const DWORD btn = WS_TABSTOP | BS_PUSHBUTTON, chk = WS_TABSTOP | BS_AUTOCHECKBOX;
    g_mkParent = hwnd;
    HWND tab = Mk(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TAB);
    const wchar_t* tabs[] = {L"Источники", L"Кадр", L"Настройки"};
    for (int i = 0; i < kPages; ++i) {
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<wchar_t*>(tabs[i]);
        SendMessageW(tab, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&ti));
    }
    for (int p = 0; p < kPages; ++p)
        g_pageWnd[p] = CreateWindowExW(WS_EX_CONTROLPARENT, L"WVCPage", L"",
                                       WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 10, 10, hwnd,
                                       nullptr, g_hInst, nullptr);
    auto shapes = {L"Прямоугольник", L"Скруглённый", L"Круг", L"Шестиугольник", L"Восьмиугольник"};
    auto kinds = {L"Экран 1", L"Веб-камера", L"Медиафайлы", L"Экран 2", L"Телефон"};

    // ---- tab "Источники"
    g_mkParent = g_pageWnd[0];
    Mk(L"STATIC", L"Экран 1", 0, IDC_SRC_HDR);
    Mk(L"STATIC", L"", SS_ENDELLIPSIS, IDC_SRC_VALUE);
    Mk(L"BUTTON", L"Область…", btn, IDC_SELECT);
    Mk(L"BUTTON", L"Монитор ▾", btn, IDC_MONITOR);
    Mk(L"BUTTON", L"Окно ▾", btn, IDC_WINDOW);
    Mk(L"BUTTON", L"▸  Дополнительно", btn | BS_LEFT, IDC_SEC_WIN);
    Mk(L"STATIC", L"Окно захватывать:", 0, IDC_WCAP_LABEL);
    AddCombo(IDC_WINCAP, {L"С экрана (как видно)", L"Из самого окна"});
    Mk(L"BUTTON", L"Спрятать окно за экран", btn, IDC_HIDEWIN);
    Mk(L"STATIC", L"Экран 2", 0, IDC_SRC2_HDR);
    Mk(L"STATIC", L"", SS_ENDELLIPSIS, IDC_SRC2_VALUE);
    Mk(L"BUTTON", L"Выбрать область 2", btn, IDC_SELECT2);
    Mk(L"BUTTON", L"Монитор ▾", btn, IDC_MONITOR2);
    Mk(L"BUTTON", L"Экран 1 ⇄ 2", btn, IDC_SCR_TOGGLE);
    Mk(L"STATIC", L"Что показывать", 0, IDC_SHOW_HDR);
    Mk(L"STATIC", L"Главный источник:", 0, IDC_VIEW_LABEL);
    AddCombo(IDC_VIEW, kinds);
    Mk(L"STATIC", L"Раскладка:", 0, IDC_LAYOUT_LABEL);
    AddCombo(IDC_LAYOUT, {L"Только главный", L"Окошко поверх", L"Рядом (пополам)"});
    Mk(L"STATIC", L"Второй источник:", 0, IDC_PIPSRC_LABEL);
    AddCombo(IDC_PIPSRC, kinds);
    Mk(L"BUTTON", L"⇄", btn, IDC_SWAP);
    Mk(L"STATIC", L"Веб-камера", 0, IDC_CAM_HDR);
    Mk(L"STATIC", L"Устройство:", 0, IDC_CAM_LABEL);
    AddCombo(IDC_CAMDEV, {});
    Mk(L"BUTTON", L"Зеркалить веб-камеру (как в зеркале)", chk, IDC_CAMMIRROR);
    Mk(L"STATIC", L"Телефон (приложение WVC Phone)", 0, IDC_PHONE_HDR);
    Mk(L"STATIC", L"Найденные телефоны:", 0, IDC_PHONE_LABEL);
    AddCombo(IDC_PHONE_LIST, {});
    Mk(L"BUTTON", L"Найти", btn, IDC_PHONE_FIND);
    Mk(L"STATIC", L"IP-адрес телефона:", 0, IDC_PHONEIP_LABEL);
    {
        HWND pe = Mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_PHONE_IP, WS_EX_CLIENTEDGE);
        SendMessageW(pe, EM_LIMITTEXT, 64, 0);
    }
    Mk(L"BUTTON", L"Подключить", btn, IDC_PHONE_CONN);
    Mk(L"BUTTON", L"Задняя камера", btn, IDC_PHONE_BACK);
    Mk(L"BUTTON", L"Передняя камера", btn, IDC_PHONE_FRONT);
    Mk(L"BUTTON", L"Экран телефона", btn, IDC_PHONE_SCREEN);
    Mk(L"STATIC", L"Качество с телефона:", 0, IDC_PHONERES_LABEL);
    AddCombo(IDC_PHONE_RES, {L"720p (быстрее)", L"1080p (чётче)"});
    Mk(L"STATIC", L"", 0, IDC_PHONE_STATUS);
    Mk(L"STATIC", L"Медиафайлы", 0, IDC_MEDIA_HDR);
    Mk(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, IDC_MEDIA_LIST, WS_EX_CLIENTEDGE);
    Mk(L"BUTTON", L"Добавить…", btn, IDC_MEDIA_ADD);
    Mk(L"BUTTON", L"Убрать", btn, IDC_MEDIA_DEL);
    Mk(L"BUTTON", L"▲", btn, IDC_MEDIA_UP);
    Mk(L"BUTTON", L"▼", btn, IDC_MEDIA_DOWN);
    Mk(L"STATIC", L"Картинку показывать:", 0, IDC_IMGSEC_LABEL);
    AddCombo(IDC_IMGSEC, {L"3 секунды", L"5 секунд", L"10 секунд", L"30 секунд", L"1 минуту"});
    Mk(L"BUTTON", L"⏭  Следующий файл", btn, IDC_MEDIA_NEXT);
    Mk(L"STATIC", L"", SS_ENDELLIPSIS, IDC_MEDIA_NOW);

    // ---- tab "Кадр"
    g_mkParent = g_pageWnd[1];
    Mk(L"STATIC", L"Окошко (второй источник поверх)", 0, IDC_PIP_HDR);
    Mk(L"STATIC", L"Угол и ширина, %:", 0, IDC_PIP_LABEL);
    AddCombo(IDC_PIPCORNER, {L"Справа внизу", L"Слева внизу", L"Справа вверху", L"Слева вверху"});
    Mk(L"EDIT", L"25", WS_TABSTOP | ES_NUMBER | ES_CENTER, IDC_PIPSIZE, WS_EX_CLIENTEDGE);
    {
        HWND ud = Mk(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK,
                     IDC_PIPSIZE_UD);
        SendMessageW(ud, UDM_SETBUDDY, reinterpret_cast<WPARAM>(Ctl(IDC_PIPSIZE)), 0);
        SendMessageW(ud, UDM_SETRANGE32, 5, 90);
    }
    Mk(L"STATIC", L"Форма окошка:", 0, IDC_PIPSHAPE_LABEL);
    AddCombo(IDC_PIPSHAPE, shapes);
    Mk(L"STATIC", L"Кадр", 0, IDC_FRAME_HDR);
    Mk(L"STATIC", L"Если пропорции другие:", 0, IDC_FIT_LABEL);
    AddCombo(IDC_FIT, {L"Вписать с полями", L"Обрезать по краям"});
    Mk(L"STATIC", L"Форма кадра:", 0, IDC_SHAPE_LABEL);
    AddCombo(IDC_FRAMESHAPE, shapes);
    Mk(L"BUTTON", L"▸  Дополнительно", btn | BS_LEFT, IDC_SEC_FRAME);
    Mk(L"STATIC", L"Поворот:", 0, IDC_ROT_LABEL);
    AddCombo(IDC_ROT, {L"Без поворота", L"90° по часовой", L"180°", L"90° против часовой"});
    Mk(L"STATIC", L"Увеличение (центр):", 0, IDC_ZOOM_LABEL);
    AddCombo(IDC_ZOOM, {L"100%", L"125%", L"150%", L"200%", L"300%", L"400%"});
    Mk(L"BUTTON", L"Зеркально (весь главный источник)", chk, IDC_MIRROR);
    Mk(L"BUTTON", L"Показывать курсор", chk, IDC_CURSOR);
    Mk(L"STATIC", L"Фон вне формы:", 0, IDC_SHAPECOLOR_LABEL);
    AddCombo(IDC_SHAPECOLOR, {L"Чёрный", L"Белый", L"Зелёный", L"Серый"});
    Mk(L"STATIC", L"Надпись", 0, IDC_TEXT_HDR);
    Mk(L"STATIC", L"Текст:", 0, IDC_TEXT_LABEL);
    HWND ed = Mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_TEXT, WS_EX_CLIENTEDGE);
    SendMessageW(ed, EM_LIMITTEXT, 120, 0);
    Mk(L"BUTTON", L"Показывать надпись", chk, IDC_TEXTON);
    Mk(L"STATIC", L"Где и какого размера:", 0, IDC_TPOS_LABEL);
    AddCombo(IDC_TEXTPOS, {L"Слева вверху", L"По центру вверху", L"Справа вверху", L"Слева внизу", L"По центру внизу",
                           L"Справа внизу"});
    AddCombo(IDC_TEXTSIZE, {L"Мелкий", L"Средний", L"Крупный"});
    Mk(L"STATIC", L"Логотип", 0, IDC_LOGO_HDR);
    Mk(L"STATIC", L"Картинка (PNG):", 0, IDC_LOGO_LABEL);
    Mk(L"BUTTON", L"Выбрать…", btn, IDC_LOGO_PICK);
    Mk(L"BUTTON", L"Убрать", btn, IDC_LOGO_CLEAR);
    Mk(L"STATIC", L"Угол и размер:", 0, IDC_LPOS_LABEL);
    AddCombo(IDC_LOGOCORNER, {L"Справа внизу", L"Слева внизу", L"Справа вверху", L"Слева вверху"});
    AddCombo(IDC_LOGOSIZE, {L"10%", L"15%", L"20%", L"30%"});
    Mk(L"STATIC", L"", SS_ENDELLIPSIS, IDC_LOGO_NAME);
    Mk(L"STATIC", L"Переходы", 0, IDC_TRANS_HDR);
    Mk(L"STATIC", L"Плавный переход:", 0, IDC_TRANS_LABEL);
    AddCombo(IDC_TRANS, {L"Без перехода", L"Растворение", L"Через чёрный", L"Шторка", L"Сдвиг"});
    AddCombo(IDC_TRANSDUR, {L"0,3 с", L"0,5 с", L"1 с"});

    // ---- tab "Настройки"
    g_mkParent = g_pageWnd[2];
    Mk(L"STATIC", L"Формат камеры", 0, IDC_FMT_HDR);
    Mk(L"STATIC", L"Качество:", 0, IDC_Q_LABEL);
    AddCombo(IDC_QUALITY, {L"720p", L"1080p"});
    Mk(L"STATIC", L"Соотношение сторон:", 0, IDC_A_LABEL);
    AddCombo(IDC_ASPECT, {L"16:9 (обычное)", L"4:3", L"1:1 (квадрат)", L"9:16 (вертикальное)"});
    Mk(L"STATIC", L"Частота кадров:", 0, IDC_FPS_LABEL);
    AddCombo(IDC_FPS, {L"30 кадров/с", L"60 кадров/с"});
    Mk(L"STATIC", L"Пауза и стоп", 0, IDC_PAUSE_HDR);
    Mk(L"STATIC", L"Показывать в камере:", 0, IDC_PAUSE_LABEL);
    AddCombo(IDC_PAUSE, {L"Последний кадр", L"Чёрный экран", L"Текст (ниже)", L"Свою картинку"});
    Mk(L"STATIC", L"Текст паузы:", 0, IDC_PAUSETEXT_LABEL);
    {
        HWND pe = Mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_PAUSETEXT, WS_EX_CLIENTEDGE);
        SendMessageW(pe, EM_LIMITTEXT, 80, 0);
    }
    Mk(L"BUTTON", L"Выбрать картинку…", btn, IDC_PAUSE_IMG);
    Mk(L"STATIC", L"Поведение", 0, IDC_BEH_HDR);
    Mk(L"BUTTON", L"Держать веб-камеру включённой", chk, IDC_KEEPCAM);
    Mk(L"BUTTON", L"Зелёная рамка вокруг области, когда трансляция выключена", chk, IDC_IDLEFRAME);
    Mk(L"BUTTON", L"Запускать вместе с Windows (сразу в трей)", chk, IDC_AUTOSTART);
    Mk(L"BUTTON", L"Сразу включать трансляцию при запуске", chk, IDC_AUTOSTREAM);
    Mk(L"STATIC", L"Горячие клавиши", 0, IDC_HK_HDR);
    Mk(L"BUTTON", L"Настроить горячие клавиши…", btn, IDC_HK_EDIT);
    Mk(L"BUTTON", L"▸  Список горячих клавиш", btn | BS_LEFT, IDC_SEC_KEYS);
    Mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, IDC_HK_LIST, WS_EX_CLIENTEDGE);
    Mk(L"STATIC", L"Файл настроек", 0, IDC_FILE_HDR);
    Mk(L"BUTTON", L"Сохранить всё в JSON…", btn, IDC_JSON_EXPORT);
    Mk(L"BUTTON", L"Загрузить из JSON…", btn, IDC_JSON_IMPORT);
    Mk(L"STATIC", L"Камера в Windows", 0, IDC_CAMSYS_HDR);
    Mk(L"BUTTON", L"Установить камеру", btn, IDC_INSTALL);
    Mk(L"BUTTON", L"Удалить камеру", btn, IDC_UNINSTALL);

    // ---- left column (always visible)
    g_mkParent = hwnd;
    Mk(L"STATIC", L"Предпросмотр (то, что видят другие):", 0, IDC_PREV_LABEL);
    Mk(L"STATIC", L"", SS_RIGHT, IDC_FPS_VALUE);
    g_preview = Mk(L"WVCPreview", L"", 0, IDC_PREVIEW, WS_EX_CLIENTEDGE);
    Mk(L"BUTTON", L"▶  Старт", btn, IDC_START);
    Mk(L"BUTTON", L"⏸  Пауза", btn, IDC_PAUSEBTN);
    Mk(L"STATIC", L"", 0, IDC_STATUS);
    Mk(L"STATIC", L"Сцены", 0, IDC_SCENE_HDR);
    for (int i = 0; i < 4; ++i) Mk(L"BUTTON", L"", btn, IDC_SCENE1 + i);
    Mk(L"STATIC", L"Сцена:", 0, IDC_PROF_LABEL);
    AddCombo(IDC_PROFILE, {});
    Mk(L"BUTTON", L"Загрузить", btn, IDC_PROF_LOAD);
    Mk(L"BUTTON", L"Сохранить…", btn, IDC_PROF_SAVE);
    Mk(L"BUTTON", L"Удалить", btn, IDC_PROF_DEL);
    Mk(L"STATIC",
       L"Сцена запоминает обе области, окно, источники и раскладку, окошко, формы, надпись, логотип, медиафайлы и "
       L"паузу. Формат, переходы и «держать веб-камеру» — общие для всех сцен. Кнопки 1–4 — первые четыре сцены.",
       0, IDC_PROF_HINT);
}
// --- tooltips: what a control does + its hotkey (read live, so edited hotkeys show up at once)
struct TipDef {
    int id;
    const wchar_t* text;
    int hk;  // HotkeyAction, -1 none, -2 scenes 1-4
};
static const TipDef kTips[] = {
    {IDC_START, L"Начать или остановить трансляцию. Когда остановлено, в камере картинка паузы.", HA_TOGGLE},
    {IDC_PAUSEBTN, L"Пауза: в камере картинка паузы, всё остальное остаётся включённым. «Продолжить» — мгновенно.",
     HA_PAUSE},
    {IDC_PREVIEW, L"То, что видят другие. Окошко можно перетаскивать мышью, а за любой угол (белые квадратики) — "
                  L"менять размер.", -1},
    {IDC_SCENE1, L"Загрузить сцену 1 (первая в списке).", HA_PROFILE1},
    {IDC_SCENE2, L"Загрузить сцену 2 (вторая в списке).", HA_PROFILE2},
    {IDC_SCENE3, L"Загрузить сцену 3 (третья в списке).", HA_PROFILE3},
    {IDC_SCENE4, L"Загрузить сцену 4 (четвёртая в списке).", HA_PROFILE4},
    {IDC_PROFILE, L"Все сохранённые сцены. Первые четыре — на кнопках 1–4 и горячих клавишах.", -2},
    {IDC_PROF_LOAD, L"Загрузить выбранную сцену.", -2},
    {IDC_PROF_SAVE, L"Сохранить всё, что сейчас настроено, как сцену (то же название — перезаписать).", -1},
    {IDC_PROF_DEL, L"Удалить выбранную сцену.", -1},
    {IDC_SELECT, L"Экран 1: выделить мышью область экрана.", HA_SELECT},
    {IDC_MONITOR, L"Экран 1: показывать целый монитор. Клавиша переключает на следующий.", HA_MONITOR},
    {IDC_WINDOW, L"Экран 1: показывать окно программы — рамка следует за окном.", -1},
    {IDC_SEC_WIN, L"Показать или скрыть редкие настройки захвата окна.", -1},
    {IDC_WINCAP, L"«Из самого окна» — картинка берётся, даже если окно перекрыто другими.", -1},
    {IDC_HIDEWIN, L"Увести окно-источник за край экрана: оно продолжает работать, WVC берёт картинку из самого окна.",
     HA_HIDE_WINDOW},
    {IDC_SELECT2, L"Экран 2: выделить мышью вторую область. Камера на неё не переключается — для этого «Экран 1 ⇄ 2».",
     -1},
    {IDC_MONITOR2, L"Экран 2: взять целый монитор как вторую область.", -1},
    {IDC_SCR_TOGGLE, L"Главный источник: Экран 1 ⇄ Экран 2. Рамки не двигаются.", HA_SCREEN12},
    {IDC_VIEW, L"Что показывать главным (большим) кадром. Клавиша меняет экран ↔ веб-камера.", HA_SCREEN_CAM},
    {IDC_LAYOUT, L"Только главный; второй источник окошком поверх; или оба рядом, каждый на половине кадра. "
                 L"Клавиша включает и выключает второй источник.", HA_PIP},
    {IDC_PIPSRC, L"Второй источник — для окошка или для правой половины в раскладке «Рядом».", -1},
    {IDC_SWAP, L"Поменять местами главный и второй источник.", HA_SWAP},
    {IDC_CAMDEV, L"Какая веб-камера используется (WVC Camera в списке не показывается).", -1},
    {IDC_CAMMIRROR, L"Отразить веб-камеру по горизонтали, как в зеркале.", -1},
    {IDC_PHONE_LIST, L"Телефоны с открытым WVC Phone, найденные в вашей сети. Выберите, чтобы подставить IP-адрес.", -1},
    {IDC_PHONE_FIND, L"Поискать телефоны с WVC Phone в той же сети Wi-Fi (около 2 секунд). Если Windows спросит про "
                     L"брандмауэр — разрешите доступ для частных сетей.", -1},
    {IDC_PHONE_IP, L"IP-адрес телефона — он крупно написан в приложении WVC Phone. Можно ввести вручную.", -1},
    {IDC_PHONE_CONN, L"Подключиться к телефону сейчас или отключиться. Без этой кнопки WVC подключается сам, когда "
                     L"«Телефон» выбран источником и идёт трансляция.", -1},
    {IDC_PHONE_BACK, L"Телефон показывает заднюю камеру.", -1},
    {IDC_PHONE_FRONT, L"Телефон показывает переднюю камеру.", -1},
    {IDC_PHONE_SCREEN, L"Телефон показывает свой экран. На телефоне нужно один раз подтвердить запись экрана.", -1},
    {IDC_PHONE_RES, L"Размер картинки с телефона по длинной стороне: 720p — меньше задержка, 1080p — чётче.", -1},
    {IDC_MEDIA_LIST, L"Файлы идут по кругу. Картинки: PNG, JPG, BMP, GIF (анимация). Видео: MP4, MOV, AVI, WMV, MKV — "
                     L"если в Windows есть кодек. Звук не передаётся. Двойной щелчок — показать файл.", -1},
    {IDC_MEDIA_NEXT, L"Перейти к следующему файлу в списке.", HA_MEDIA_NEXT},
    {IDC_PIPCORNER, L"Поставить окошко в угол (ширина сохраняется).", -1},
    {IDC_PIPSIZE, L"Ширина окошка в процентах от кадра: стрелки ▲▼ или введите число. "
                  L"Ещё можно тянуть за угол окошка в предпросмотре.", -1},
    {IDC_PIPSIZE_UD, L"Больше / меньше на 1%.", -1},
    {IDC_PIPSHAPE, L"Форма окошка. Круг и многоугольники — в квадрате.", -1},
    {IDC_FIT, L"Как вписать картинку другой формы: с полями или обрезав края. В раскладке «Рядом» действует на обе "
              L"половины (веб-камера справа всегда заполняет половину).", -1},
    {IDC_FRAMESHAPE, L"Форма всего кадра. Рамка на экране повторяет форму.", -1},
    {IDC_SEC_FRAME, L"Показать или скрыть редкие настройки: поворот, увеличение, зеркало, курсор, фон формы.", -1},
    {IDC_ROT, L"Повернуть главный источник.", -1},
    {IDC_ZOOM, L"Увеличить центр главного источника.", -1},
    {IDC_MIRROR, L"Отразить главный источник по горизонтали.", -1},
    {IDC_CURSOR, L"Показывать указатель мыши в кадре.", HA_CURSOR},
    {IDC_SHAPECOLOR, L"Цвет вокруг формы кадра или окошка.", -1},
    {IDC_TEXT, L"Текст надписи поверх кадра.", -1},
    {IDC_TEXTON, L"Показывать надпись поверх кадра.", HA_TEXT},
    {IDC_LOGO_PICK, L"Выбрать логотип (PNG с прозрачностью).", -1},
    {IDC_LOGO_CLEAR, L"Убрать логотип.", -1},
    {IDC_TRANS, L"Плавный переход при смене источника, раскладки, окошка, сцены, а также при старте, стопе и паузе.",
     -1},
    {IDC_TRANSDUR, L"Длительность перехода.", -1},
    {IDC_QUALITY, L"Разрешение камеры. При смене переподключите камеру в Zoom/Telegram.", -1},
    {IDC_ASPECT, L"Форма кадра камеры. При смене переподключите камеру в Zoom/Telegram.", -1},
    {IDC_FPS, L"Частота кадров камеры.", -1},
    {IDC_PAUSE, L"Что видят другие на паузе и когда трансляция остановлена.", -1},
    {IDC_PAUSETEXT, L"Текст на картинке паузы (вариант «Текст»).", -1},
    {IDC_PAUSE_IMG, L"Выбрать свою картинку для паузы.", -1},
    {IDC_KEEPCAM, L"Веб-камера открывается при запуске WVC и не выключается, поэтому переключение на неё мгновенное. "
                  L"Минус: индикатор камеры горит всё время, а другие программы не смогут взять эту же камеру.", -1},
    {IDC_IDLEFRAME, L"Рамка: зелёная — трансляция выключена, красная — идёт, жёлтая — пауза. "
                    L"Рамка Экрана 2 — пунктирная.", -1},
    {IDC_AUTOSTART, L"Запускать WVC при входе в Windows, сразу свёрнутым в трей.", -1},
    {IDC_AUTOSTREAM, L"При запуске сразу включать трансляцию.", -1},
    {IDC_HK_EDIT, L"Назначить свои сочетания клавиш.", -1},
    {IDC_SEC_KEYS, L"Показать или скрыть список горячих клавиш.", -1},
    {IDC_JSON_EXPORT, L"Сохранить все настройки, сцены и горячие клавиши в файл.", -1},
    {IDC_JSON_IMPORT, L"Загрузить настройки, сцены и горячие клавиши из файла.", -1},
    {IDC_INSTALL, L"Зарегистрировать «WVC Camera» в Windows (один раз, нужны права администратора).", -1},
    {IDC_UNINSTALL, L"Убрать «WVC Camera» из Windows.", -1},
};
static HWND g_tips = nullptr;
static std::wstring g_tipText;

static void CreateTooltips(HWND hwnd) {
    g_tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, g_hInst,
                             nullptr);
    if (!g_tips) return;
    SendMessageW(g_tips, TTM_SETMAXTIPWIDTH, 0, S(340));
    SendMessageW(g_tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 15000);
    for (const TipDef& t : kTips) {
        HWND c = Ctl(t.id);
        if (!c) continue;
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd;
        ti.uId = reinterpret_cast<UINT_PTR>(c);
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(g_tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

static const wchar_t* TooltipText(HWND ctl) {
    const int id = GetDlgCtrlID(ctl);
    for (const TipDef& t : kTips) {
        if (t.id != id) continue;
        g_tipText = t.text;
        if (t.hk >= 0) {
            g_tipText += L"\nКлавиши: ";
            g_tipText += g_hk[t.hk] ? HotkeyName(g_hk[t.hk]) : L"не назначены (Настройки → Горячие клавиши)";
        } else if (t.hk == -2) {
            for (int i = 0; i < 4; ++i) {
                g_tipText += L"\nСцена " + std::to_wstring(i + 1) + L": ";
                g_tipText += g_hk[HA_PROFILE1 + i] ? HotkeyName(g_hk[HA_PROFILE1 + i]) : L"—";
            }
        }
        return g_tipText.c_str();
    }
    return L"";
}

static void ResizeForDpi(HWND hwnd) {
    RECT r{0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), g_dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// one combo / checkbox / edit changed -> settings
// one combo / checkbox / edit changed -> settings
static void OnControlChanged(int id) {
    if (g_syncing) return;
    bool format = false, pause = false, layers = false, media = false, phoneRes = false;
    int W, H;
    {
        Settings cur = GetSettings();
        OutSize(cur.quality, cur.aspect, &W, &H);
    }
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
        case IDC_FRAMESHAPE: s.frameShape = (std::min)(sel, kShapeCount - 1); break;
        case IDC_SHAPECOLOR: s.shapeColor = (std::min)(sel, 3); break;
        case IDC_MIRROR: s.mirror = CheckGet(id); break;
        case IDC_CURSOR: s.cursor = CheckGet(id); break;
        case IDC_WINCAP: s.winCapture = sel == 1 ? 1 : 0; break;
        case IDC_VIEW: s.mainSrc = (std::min)(sel, 4); layers = true; break;
        case IDC_LAYOUT: s.layout = (std::min)(sel, 2); layers = true; break;
        case IDC_PIPSRC:
            s.pipSrc = std::clamp(sel + 1, 1, 5);
            if (s.layout == 0) s.layout = 1;  // picking a second source = show it
            layers = true;
            break;
        case IDC_KEEPCAM: s.keepCamOn = CheckGet(id); layers = true; break;
        case IDC_PHONE_RES: s.phoneRes = (std::min)(sel, 1); phoneRes = true; break;
        case IDC_PAUSETEXT: {
            wchar_t buf[256] = {};
            GetWindowTextW(Ctl(IDC_PAUSETEXT), buf, 256);
            s.pauseText = buf;
            pause = true;
            break;
        }
        case IDC_CAMDEV:
            if (sel >= 1 && sel <= static_cast<int>(g_camList.size())) {
                s.camLink = g_camList[sel - 1].link;
                s.camName = g_camList[sel - 1].name;
            } else {
                s.camLink.clear();
                s.camName.clear();
            }
            layers = true;
            break;
        case IDC_PIPCORNER: s.pipCorner = (std::min)(sel, 3); PipPreset(s, W, H); break;
        case IDC_PIPSIZE: {
            BOOL ok = FALSE;
            const int pct = static_cast<int>(GetDlgItemInt(GetParent(Ctl(IDC_PIPSIZE)), IDC_PIPSIZE, &ok, FALSE));
            if (!ok || pct < 5 || pct > 90) break;  // still typing: nothing changes
            if (std::abs(pct * 100 - s.pipW) < 50) break;
            // grow / shrink around the nearest corner of the frame
            RECT r = PipRect(s, W, H);
            const bool right = (r.left + r.right) > W, bottom = (r.top + r.bottom) > H;
            s.pipW = pct * 100;
            RECT n = PipRect(s, W, H);  // new size (position not final yet)
            const int nw = n.right - n.left, nh = n.bottom - n.top;
            const int nx = std::clamp(right ? static_cast<int>(r.right) - nw : static_cast<int>(r.left), 0, W - nw);
            const int ny = std::clamp(bottom ? static_cast<int>(r.bottom) - nh : static_cast<int>(r.top), 0, H - nh);
            s.pipX = MulDiv(nx, 10000, W);
            s.pipY = MulDiv(ny, 10000, H);
            break;
        }
        case IDC_TRANS: s.transition = (std::min)(sel, 4); break;
        case IDC_TRANSDUR: s.transDur = (std::min)(sel, 2); break;
        case IDC_IDLEFRAME: s.idleFrame = CheckGet(id); break;
        case IDC_PIPSHAPE: s.pipShape = (std::min)(sel, kShapeCount - 1); break;
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
        case IDC_IMGSEC: s.imageSec = (std::min)(sel, 4); media = true; break;
        default: return;
        }
    }
    SaveSettings();
    if (format) RestartCameraForFormat();
    if (phoneRes) PhoneSendRes(GetSettings().phoneRes);
    if (pause) {
        g_pauseDirty = true;
        if (GetSettings().pauseMode == 3 && GetSettings().pauseImage.empty()) ChoosePauseImage();
    }
    if (media) {
        ++g_mediaGen;
        MediaSync();
    }
    if (layers) {
        if ((id == IDC_VIEW || id == IDC_PIPSRC) && PhoneNeeded(GetSettings()) && g_phoneUser == -1)
            g_phoneUser = 0;  // the phone was just picked as a source: connect again
        SyncSources();
        UpdateStatus();
        if (id == IDC_PIPSRC) {
            g_syncing = true;
            ComboSet(IDC_LAYOUT, GetSettings().layout);
            g_syncing = false;
        }
    }
    UpdateFrame();
}
static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        g_phoneNotify = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        RegisterHotkeys(hwnd);
        MakeFont();
        g_expanded = IniInt(kMainSec, L"UiExpanded", 0) & (SEC_WIN | SEC_FRAME | SEC_KEYS);
        CreateControls(hwnd);
        ResizeForDpi(hwnd);
        Layout();
        CreateTooltips(hwnd);
        SyncControls();
        FillProfileCombo(IniStr(kMainSec, L"LastScene"));
        SetTextIfChanged(Ctl(IDC_HK_LIST), HotkeyListText());
        FillPhoneCombo();
        {
            const int tabNo = std::clamp(IniInt(kMainSec, L"UiTab", 0), 0, kPages - 1);
            SendMessageW(Ctl(IDC_TAB), TCM_SETCURSEL, tabNo, 0);
            ShowPage(tabNo);
        }
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
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        // left column on the light-grey window background
        HDC dc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        SetBkColor(dc, kBgMain);
        if (msg == WM_CTLCOLORSTATIC) {
            if (IsHeaderId(id))
                SetTextColor(dc, kHdrText);
            else if (id == IDC_PROF_HINT || id == IDC_FPS_VALUE)
                SetTextColor(dc, kHintText);
        }
        return reinterpret_cast<LRESULT>(g_brMain);
    }
    case WM_MOUSEWHEEL: {
        // wheel over the tab page scrolls it even when the focus is elsewhere
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        RECT pr;
        if (g_pageWnd[g_page] && GetWindowRect(g_pageWnd[g_page], &pr) && PtInRect(&pr, pt))
            return SendMessageW(g_pageWnd[g_page], WM_MOUSEWHEEL, wp, lp);
        break;
    }
    case WM_NOTIFY: {
        const NMHDR* nh = reinterpret_cast<const NMHDR*>(lp);
        if (nh->code == TTN_GETDISPINFOW) {
            auto* di = reinterpret_cast<NMTTDISPINFOW*>(lp);
            di->lpszText = const_cast<wchar_t*>(TooltipText(reinterpret_cast<HWND>(nh->idFrom)));
            return 0;
        }
        if (nh->idFrom == IDC_TAB && nh->code == TCN_SELCHANGE) {
            ShowPage(static_cast<int>(SendMessageW(Ctl(IDC_TAB), TCM_GETCURSEL, 0, 0)));
            IniSetInt(kMainSec, L"UiTab", g_page);
        }
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
            SyncSources();
            UpdateFrame();  // follows a moving window, picks up colour / shape changes
        }
        return 0;
    case WM_HOTKEY:
        if (wp >= 1 && wp <= HA_COUNT) OnHotkey(static_cast<int>(wp) - 1);
        return 0;
    case WM_PHONE_FOUND: {
        std::unique_ptr<std::vector<PhoneFound>> v(reinterpret_cast<std::vector<PhoneFound>*>(lp));
        if (!v) return 0;
        g_phoneList = *v;
        if (v->empty()) {
            g_phoneNote = L"Телефон не найден. Проверьте: WVC Phone открыт, телефон и компьютер в одной сети Wi-Fi, WVC "
                          L"разрешён в брандмауэре Windows (частная сеть). IP можно ввести вручную.";
        } else {
            g_phoneNote = L"Найдено телефонов: " + std::to_wstring(v->size()) + L". Выберите в списке и нажмите «Подключить».";
            const std::wstring cur = GetSettings().phoneIp;
            bool listed = false;
            for (const auto& p : *v) listed = listed || p.ip == cur;
            if (!listed && (cur.empty() || v->size() == 1)) ApplyPhoneIp((*v)[0].ip);
        }
        FillPhoneCombo();
        UpdateStatus();
        return 0;
    }
    case WM_PHONE_STATE: {
        const std::wstring st = PhoneStateNow();
        const int src = st == L"back" ? 0 : st == L"front" ? 1 : st == L"screen" ? 2 : -1;
        if (src >= 0 && src != GetSettings().phoneSrc) {  // switched on the phone: remember it
            {
                std::lock_guard<std::mutex> lk(g_setMx);
                g_set.phoneSrc = src;
            }
            SaveSettings();
        }
        const int rr = g_phoneResRep;
        if (g_phone.status == 2 && rr >= 0 && rr != GetSettings().phoneRes) {  // quality changed on the phone
            {
                std::lock_guard<std::mutex> lk(g_setMx);
                g_set.phoneRes = rr;
            }
            SaveSettings();
            g_syncing = true;
            ComboSet(IDC_PHONE_RES, rr);
            g_syncing = false;
        }
        if (g_phone.status == 2) g_phoneNote.clear();
        UpdateStatus();
        return 0;
    }
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
            if (g_streaming) AppendMenuW(m, MF_STRING, ID_TRAY_PAUSE, g_paused ? L"Продолжить" : L"Пауза");
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
        case IDC_MONITOR: ShowMonitorMenu(0); break;
        case IDC_SELECT2: DoSelectRegionN(1); break;
        case IDC_MONITOR2: ShowMonitorMenu(1); break;
        case IDC_SCR_TOGGLE: ToggleScreens(); break;
        case IDC_SEC_WIN: ToggleSection(SEC_WIN); break;
        case IDC_SEC_FRAME: ToggleSection(SEC_FRAME); break;
        case IDC_SEC_KEYS: ToggleSection(SEC_KEYS); break;
        case IDC_SCENE1:
        case IDC_SCENE2:
        case IDC_SCENE3:
        case IDC_SCENE4: LoadProfileByIndex(id - IDC_SCENE1); break;
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
                MessageBoxW(hwnd, L"Сцен пока нет: настройте всё и нажмите «Сохранить…».", L"WVC",
                            MB_ICONINFORMATION);
            else
                LoadProfile(n);
            break;
        }
        case IDC_PROF_SAVE: {
            std::wstring n = SelectedProfile();
            if (n.empty()) n = L"Сцена " + std::to_wstring(ProfileNames().size() + 1);
            if (InputBox(L"Сохранить сцену", L"Название сцены (то же название — перезаписать):", &n))
                SaveProfile(n);
            break;
        }
        case IDC_PROF_DEL: DeleteProfile(SelectedProfile()); break;
        case IDC_HK_EDIT: EditHotkeys(); break;
        case IDC_SWAP: SwapLayers(); break;
        case IDC_HIDEWIN: ToggleHideWindow(); break;
        case IDC_MEDIA_ADD: MediaAdd(); break;
        case IDC_MEDIA_DEL: MediaRemove(); break;
        case IDC_MEDIA_UP: MediaMove(-1); break;
        case IDC_MEDIA_DOWN: MediaMove(1); break;
        case IDC_MEDIA_NEXT: MediaNext(); break;
        case IDC_MEDIA_LIST:
            if (code == LBN_DBLCLK) {
                const int sel = static_cast<int>(SendMessageW(Ctl(IDC_MEDIA_LIST), LB_GETCURSEL, 0, 0));
                if (sel >= 0) {
                    g_mediaIndex = sel - 1;  // the player moves on to sel
                    MediaNext();
                }
            }
            break;
        case IDC_JSON_EXPORT: DoExportJson(); break;
        case IDC_JSON_IMPORT: DoImportJson(); break;
        case IDC_AUTOSTART:
            if (code == BN_CLICKED && !g_syncing && !SetAutostart(CheckGet(IDC_AUTOSTART))) {
                MessageBoxW(hwnd, L"Не удалось изменить автозапуск.", L"WVC", MB_ICONERROR);
                CheckSet(IDC_AUTOSTART, AutostartOn());
            }
            break;
        case IDC_AUTOSTREAM:
            if (code == BN_CLICKED && !g_syncing) IniSetInt(kMainSec, L"AutoStream", CheckGet(IDC_AUTOSTREAM) ? 1 : 0);
            break;
        case IDC_PHONE_FIND:
            g_phoneNote.clear();
            PhoneDiscover(hwnd);
            UpdateStatus();
            break;
        case IDC_PHONE_CONN: TogglePhoneConnection(); break;
        case IDC_PHONE_BACK:
        case IDC_PHONE_FRONT:
        case IDC_PHONE_SCREEN: SetPhoneSource(id - IDC_PHONE_BACK); break;
        case IDC_PHONE_LIST:
            if (code == CBN_SELCHANGE && !g_syncing) {
                const int sel = ComboSel(IDC_PHONE_LIST);
                if (sel >= 0 && sel < static_cast<int>(g_phoneList.size())) ApplyPhoneIp(g_phoneList[sel].ip);
            }
            break;
        case IDC_PHONE_IP:
            if (code == EN_KILLFOCUS && !g_syncing) ApplyPhoneIp(PhoneIpFromEdit());
            break;
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
        case IDC_PAUSETEXT:
            if (code == EN_CHANGE) OnControlChanged(id);
            break;
        case IDC_KEEPCAM:
        case IDC_MIRROR:
        case IDC_CURSOR:
        case IDC_CAMMIRROR:
        case IDC_TEXTON:
        case IDC_IDLEFRAME:
            if (code == BN_CLICKED) OnControlChanged(id);
            break;
        case IDC_PIPSIZE:
            if (code == EN_CHANGE) OnControlChanged(id);
            break;
        case ID_TRAY_PAUSE:
        case IDC_PAUSEBTN: TogglePause(); break;
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
        g_phoneNotify = nullptr;
        RestoreHiddenWindow();
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
    {
        WSADATA wd;
        g_wsaOk = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_HOTKEY_CLASS |
                                                  ICC_UPDOWN_CLASS | ICC_BAR_CLASSES};
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
    for (HWND& f : g_frame) {
        f = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WVCFrame", L"",
                            WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
        if (f) SetLayeredWindowAttributes(f, kKey, 255, LWA_COLORKEY);
    }

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

    g_brMain = CreateSolidBrush(kBgMain);
    g_brPage = CreateSolidBrush(kBgPage);
    WNDCLASSW pg{};
    pg.lpfnWndProc = PageProc;
    pg.hInstance = hInst;
    pg.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pg.hbrBackground = g_brPage;
    pg.lpszClassName = L"WVCPage";
    RegisterClassW(&pg);

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = g_brMain;
    wc.lpszClassName = L"WVCMain";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"WVCMain", L"WVC 0.8 — виртуальная камера",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                940, 660, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    const bool tray = cmdLine && wcsstr(cmdLine, L"--tray");
    if (!tray) {
        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
    }
    UpdateStatus();
    if (IniInt(kMainSec, L"AutoStream", 0) && HaveSource() && g_cam) ToggleStreaming();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_FRAME_DONE && msg.hwnd && (msg.hwnd == g_frame[0] || msg.hwnd == g_frame[1])) {
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
    MediaStop();
    PhoneStop();
    StopCamera();
    for (HWND f : g_frame)
        if (f) DestroyWindow(f);
    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_frameFont) DeleteObject(g_frameFont);
    MFShutdown();
    if (g_wsaOk) WSACleanup();
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
