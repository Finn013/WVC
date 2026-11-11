// WVC 0.11 — лёгкая виртуальная камера в стиле OBS: сцены, любое число источников, размещение мышью.
// Общие объявления для всех файлов приложения.
#pragma once

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
#include <mftransform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WVC_VERSION L"0.11"

template <class T> static inline void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ---------------------------------------------------------------------------
// util.cpp
// ---------------------------------------------------------------------------
std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);
std::wstring ExePath();
std::wstring ExeDir();
std::wstring IniPath();
std::wstring FileNameOf(const std::wstring& path);
std::wstring LowerExt(const std::wstring& f);
bool IsVideoFile(const std::wstring& f);
std::wstring Trim(const std::wstring& s);

// Settings file (UTF-16 INI), kept in memory and written in one go.
struct IniDoc {
    typedef std::vector<std::pair<std::wstring, std::wstring>> Keys;
    std::vector<std::pair<std::wstring, Keys>> secs;
    const std::wstring* Find(const std::wstring& sec, const std::wstring& key) const;
    bool Has(const std::wstring& sec, const std::wstring& key) const { return Find(sec, key) != nullptr; }
    bool HasSection(const std::wstring& sec) const;
    std::wstring Str(const std::wstring& sec, const std::wstring& key, const std::wstring& def = L"") const;
    int Int(const std::wstring& sec, const std::wstring& key, int def) const;
    double Dbl(const std::wstring& sec, const std::wstring& key, double def) const;
    void Set(const std::wstring& sec, const std::wstring& key, const std::wstring& v);
    void SetInt(const std::wstring& sec, const std::wstring& key, int v);
    void SetDbl(const std::wstring& sec, const std::wstring& key, double v);
    void Remove(const std::wstring& sec, const std::wstring& key);
    void RemoveSection(const std::wstring& sec);
    std::vector<std::wstring> SectionNames() const;
    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
    bool LoadText(const std::wstring& text);
};
std::wstring EscapeMultiline(const std::wstring& s);
std::wstring UnescapeMultiline(const std::wstring& s);

// JSON: {"app":"WVC","format":N,"version":"…","sections":{"sec":{"key":"value"}}}
bool IniToJsonFile(const IniDoc& doc, const std::wstring& path);
bool JsonFileToIni(const std::wstring& path, IniDoc* doc, std::wstring* err);

struct MonitorItem {
    RECT rc;
    bool primary;
    std::wstring device;
};
std::vector<MonitorItem> Monitors();
RECT PrimaryMonitorRect();
RECT VirtualScreen();
bool WindowRectOf(HWND hwnd, RECT* r);
struct WindowItem {
    HWND hwnd;
    std::wstring title;
};
std::vector<WindowItem> ListWindows();
HWND FindWindowByTitle(const std::wstring& title);
bool LoadImageScaled(const std::wstring& path, int maxW, int maxH, REFWICPixelFormatGUID fmt, std::vector<uint8_t>& out,
                     int* ow, int* oh);
bool ImageSize(const std::wstring& path, int* w, int* h);
struct CamDevice {
    std::wstring name, link;
};
std::vector<CamDevice> EnumWebcams();

// ---------------------------------------------------------------------------
// Model (model.cpp)
// ---------------------------------------------------------------------------
enum ItemType { IT_REGION = 0, IT_WINDOW, IT_MONITOR, IT_WEBCAM, IT_MEDIA, IT_PHONE, IT_TEXT, IT_IMAGE, IT_COUNT };
enum ShapeKind {
    SH_RECT = 0, SH_ROUND, SH_CIRCLE, SH_ELLIPSE, SH_HEX, SH_OCT, SH_DIAMOND, SH_STAR, SH_HEART, SH_BUBBLE, SH_MASK,
    SH_COUNT
};
extern const wchar_t* const kShapeNames[SH_COUNT];
extern const wchar_t* const kTypeNames[IT_COUNT];
extern const int kImageSecs[5];

struct ShapeDef {
    int kind = SH_RECT;
    int radius = 15;   // rounded rectangle: corner radius, % of the shorter side (0..50)
    int feather = 0;   // soft edge, % of the shorter side (0..30)
    std::wstring mask; // SH_MASK: PNG, white / opaque = visible
    bool Trivial() const { return kind == SH_RECT && feather == 0; }
    std::wstring Key() const;
};

struct Item {
    int uid = 0;
    int type = IT_REGION;
    std::wstring name;
    bool visible = true, locked = false;
    double x = 0, y = 0, w = 1, h = 1;  // box in the frame, fractions of the output size
    bool needFit = false;               // box not placed yet: fit to the frame once the size is known
    // frame
    int aspect = 0;          // box proportions: 0 as the source, 1 16:9, 2 4:3, 3 1:1, 4 9:16, 5 3:4
    int fit = 0;             // 0 fit (keep proportions), 1 fill (crop), 2 stretch
    int crop[4] = {0, 0, 0, 0};  // L T R B, per mille of the picture as shown (after rotation / mirror)
    int rot = 0;             // 0..3, x 90° clockwise
    bool mirror = false;
    int zoom = 100;          // %, centre zoom 100..400
    ShapeDef shape;
    int opacity = 100;
    bool border = false;     // thin white outline (the old small-window look)
    // screen sources
    RECT region{};
    std::wstring winTitle;
    HWND hwnd = nullptr;     // runtime
    int winCapture = 0;      // 0 = from the screen, 1 = straight from the window (PrintWindow)
    std::wstring monDevice;
    RECT monRect{};          // last known monitor rectangle
    bool cursor = false;
    // webcam
    std::wstring camLink, camName;
    // media files
    std::vector<std::wstring> media;
    int imageSec = 1;        // index into kImageSecs
    // phone
    int phoneConn = 0;       // 0 Wi-Fi, 1 USB
    std::wstring phoneIp, phoneSerial;
    int phoneSrc = 0;        // 0 back, 1 front, 2 phone screen
    int phoneRes = 0;        // 0 720, 1 1080
    int phoneCodec = 0;      // 0 auto (H.264, JPEG fallback), 1 JPEG, 2 camera through the screen
    bool autoRotate = true;
    int phoneOrient = 0;     // last orientation seen: 0 unknown, 1 portrait, 2 landscape
    // text
    std::wstring text;
    int textPx = 64;         // font height at 1080p
    int textColor = 0;       // index into kTextColors
    bool textBg = true, textBold = true;
    // picture
    std::wstring image;
};

struct Scene {
    std::wstring name;
    std::vector<Item> items;  // bottom -> top
};

struct OutputCfg {
    int quality = 1;    // 0 720p, 1 1080p
    int aspect = 0;     // 0 16:9, 1 4:3, 2 1:1, 3 9:16
    int fps = 30;
    bool native = false;  // «Как у источника»
    int nativeUid = 0;
    int nativeW = 1280, nativeH = 720;  // last known native size
    ShapeDef shape;
    int crop[4] = {0, 0, 0, 0};  // per mille
    int bgMode = 0;              // 0 colour, 1 blur, 2 picture
    COLORREF bgColor = RGB(0, 0, 0);
    std::wstring bgImage;
};

struct Project {
    std::vector<Scene> scenes;
    int cur = 0, next = -1;
    OutputCfg out;
    int transition = 1;   // 0 none, 1 dissolve, 2 through black, 3 wipe, 4 slide
    int transMs = 500;
    int pauseMode = 2;    // 0 last frame, 1 black, 2 text, 3 picture
    std::wstring pauseText = L"Скоро вернусь";
    std::wstring pauseImage;
    bool idleFrame = true, keepCamOn = false, liveWhenStopped = true, minToTray = true, closeToTray = false;
    int defPhoneConn = 0, defPhoneRes = 0, defPhoneCodec = 0;
    bool defAutoRotate = true;
    std::wstring lastPhoneIp;
    int nextUid = 1;
};

extern Project g_proj;
extern std::mutex g_projMx;
extern std::atomic<int> g_projVer;
extern std::atomic<bool> g_projDirty;
Project ProjSnapshot();
void ProjEdit(const std::function<void(Project&)>& f);  // locks, edits, bumps the version, marks dirty
Scene* CurScene(Project& p);
const Scene* CurSceneC(const Project& p);
Item* FindItem(Project& p, int uid, int* sceneIdx = nullptr);
const Item* FindItemC(const Project& p, int uid);
Item NewItem(Project& p, int type);  // defaults (uid taken)
std::wstring UniqueItemName(const Scene& sc, const std::wstring& base);
std::wstring UniqueSceneName(const Project& p, const std::wstring& base);
void OutSizeFor(const OutputCfg& o, int* w, int* h);
double AspectValue(int aspect);  // 0 = none
bool SaveProject();
bool LoadProjectFromIni(const IniDoc& doc, Project* p, std::wstring* note);  // migrates old formats
void ProjectToIni(const Project& p, IniDoc* doc);
extern IniDoc g_ini;  // what is on disk: app-wide keys + hotkeys + the project
void ItemFrameKeysToIni(const Item& it, IniDoc* doc, const std::wstring& sec);

// ---------------------------------------------------------------------------
// Inputs (inputs.cpp): webcams, media players and phones, shared by key
// ---------------------------------------------------------------------------
struct Input {
    virtual ~Input() {}
    std::wstring key;
    std::atomic<bool> stop{false};
    std::atomic<int> status{0};  // 0 off, 1 opening, 2 running, 3 failed
    std::atomic<uint64_t> seq{0};
    std::mutex mx;
    std::vector<uint8_t> buf;  // BGRA top-down
    int w = 0, h = 0;
    ULONGLONG failedAt = 0;
    std::thread th;
    ULONGLONG lastWanted = 0;
    void Publish(const uint8_t* bgra, int fw, int fh);
    void Clear();
    virtual void Start() = 0;
    virtual void Halt();
};

std::wstring InputKeyOf(const Item& it);
std::shared_ptr<Input> GetInput(const std::wstring& key);
void InputsSync();      // UI timer: start / stop what is needed
void InputsShutdown();
void MediaNextAll(const std::wstring& key);  // empty key = every player
std::wstring MediaNowName(const std::wstring& key);
std::wstring InputMessage(const Item& it);   // card text while there is no picture

// phone
struct PhoneFound {
    std::wstring name, ip;
};
struct PhoneInfo {
    bool exists = false;
    int status = 0;
    std::wstring name, state, err, usbModel, codec;
    int recvFpsX10 = 0, decFpsX10 = 0, kbps = 0, res = -1;
    int w = 0, h = 0;
};
PhoneInfo PhoneInfoFor(const std::wstring& key);
void PhoneUserConnect(const std::wstring& key, int v);  // 1 connect, -1 disconnect, 0 auto
int PhoneUserState(const std::wstring& key);
void PhoneDiscover(HWND notify);
void PhoneUsbList(HWND notify);
std::vector<PhoneFound> PhoneFoundList();
struct AdbDev {
    std::wstring serial, state, model;
};
std::vector<AdbDev> UsbDevices();
void AdbShutdown();
const wchar_t* PhoneStateName(const std::wstring& st);
extern bool g_wsaOk;
extern HWND g_notify;  // main window: WM_PHONE_* messages
static const UINT WM_PHONE_FOUND = WM_APP + 10;
static const UINT WM_PHONE_STATE = WM_APP + 11;
static const UINT WM_PHONE_USBLIST = WM_APP + 12;
extern std::atomic<bool> g_streaming, g_paused;
extern std::atomic<bool> g_uiVisible;  // main window shown (the preview wants live sources)
extern std::atomic<int> g_soloUid;     // item whose Properties are open (its source must run), 0 = none
extern std::atomic<bool> g_quitting;

// H.264 decoder (h264.cpp)
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    bool Init();  // false = no Media Foundation H.264 decoder
    // Feeds one access unit (Annex-B). Returns true when a new picture came out (BGRA top-down in *out).
    bool Decode(const uint8_t* data, size_t n, int64_t pts100ns, std::vector<uint8_t>* out, int* w, int* h,
                bool wantPicture);
    void Reset();
    int errors = 0;

private:
    struct Impl;
    Impl* d;
};

// ---------------------------------------------------------------------------
// Rendering (render.cpp)
// ---------------------------------------------------------------------------
struct Surface {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint8_t* bits = nullptr;
    int w = 0, h = 0;
    bool Ensure(HDC ref, int cw, int ch);
    void Clear(uint32_t v = 0);
    void Free();
    ~Surface();
};
void TextNativeSize(const Item& it, int* w, int* h);  // at 1080p
void BuildShapeMask(int w, int h, const ShapeDef& sd, std::vector<uint8_t>& m);
void ShapeOutline(double w, double h, const ShapeDef& sd, std::vector<std::vector<POINT>>* polys);  // for previews
void DrawCheckerboard(HDC dc, const RECT& r, int cell);

struct Published {
    std::mutex mx;
    std::vector<uint8_t> px;  // BGRA top-down
    int w = 0, h = 0;
    uint64_t seq = 0;
    std::atomic<int> wantW{0}, wantH{0};  // size the UI wants (0 = not wanted)
};
extern Published g_pubProgram;  // scene canvas with the output shape applied (main preview)
extern Published g_pubNext;     // «Следующий»
extern Published g_pubSolo;     // one source for its Properties (whole picture, rotation / mirror applied)
extern Published g_pubCanvas;   // scene canvas before the output shape (Settings → Видео)
extern std::mutex g_soloMx;
extern Item g_soloItem;         // the copy being edited in Properties
extern std::atomic<int> g_fpsX10;
extern std::atomic<bool> g_resetCapture, g_pauseDirty;
extern std::atomic<int> g_transGen;
struct NativeInfo {
    int w = 0, h = 0;  // as shown: after rotation, before crop
};
bool NativeSize(int uid, int* w, int* h);  // last native size seen by the renderer
void SetNativeSize(int uid, int w, int h);
void RenderStart();
void RenderStop();
extern int g_camW, g_camH, g_camFps;
extern bool g_camOk;
bool CameraDllLoaded();
bool LoadCameraDll();
struct RectD {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double W() const { return x1 - x0; }
    double H() const { return y1 - y0; }
};
// Item geometry. DW x DH = the source as shown (after rotation). D1 = after crop + zoom (display space).
RectD ItemSourceRect(const Item& it, double DW, double DH);
// C = where the picture lands in the frame (output px), D = the part of the source (display space) drawn there
void ItemLayout(const Item& it, double W, double H, double DW, double DH, RectD* C, RectD* D);
RectD ItemBox(const Item& it, double W, double H);
// display-space rect -> rect in the unrotated source (NW x NH)
RectD DispToSrc(const RectD& d, int rot, bool mirror, double NW, double NH);
// output crop: the part of the scene canvas the camera shows (aspect of the frame)
RectD OutputVisibleRect(const OutputCfg& o, double W, double H);

// ---------------------------------------------------------------------------
// UI (ui_*.cpp)
// ---------------------------------------------------------------------------
extern HINSTANCE g_hInst;
extern HWND g_main;
extern HFONT g_font, g_fontBold, g_fontSym, g_fontBig;
extern UINT g_dpi;
int S(int v);
HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
               DWORD ex = 0);
bool RunModal(HWND dlg, const bool* done);
bool InputBox(HWND owner, const wchar_t* title, const wchar_t* prompt, std::wstring* value);
void UiRefresh();          // lists, combos, status after a project change
int UiSelected();
void UiSelect(int uid);
void UiOutputSize(int* W, int* H);  // size of the frame the scene is composed in
void SetProgramScene(int idx);
void ShowMain();
extern COLORREF kDarkBg, kDarkPanel, kDarkList, kDarkText, kDarkDim, kDarkSel, kDarkBtn;
extern HBRUSH g_brDark, g_brPanel, g_brList;
void DarkTitle(HWND hwnd);
void PaintPublished(HDC dc, Published& pub, const RECT& area, RECT* drawn);  // fit, keep proportions
void ApplyItemTransform(Item& it, int cmd, int W, int H);  // menu commands of «Трансформация»
void UiStatus();
void SetNote(const std::wstring& n);
bool SelectRegion(HINSTANCE hInst, RECT* out);
bool PickFile(HWND owner, const wchar_t* filter, std::wstring* out, bool multi = false,
              std::vector<std::wstring>* many = nullptr);
const wchar_t* kImageFilter();
const wchar_t* kMediaFilter();

// properties (ui_props.cpp)
bool OpenProperties(int uid, bool isNew);
void AddSourceMenu(HWND owner, POINT pt);
void AddSourceOfType(int type);
void FitItemToScreen(Item& it, int W, int H, bool stretch);
void ToggleHideWindow(int uid);
void RestoreHiddenWindow();

// settings (ui_settings.cpp)
void OpenSettings(int page);
enum HotkeyAction {
    HA_TOGGLE = 0, HA_SELECT, HA_MONITOR, HA_CURSOR, HA_SWAPNEXT, HA_ITEMVIS, HA_TEXT, HA_SCENE1, HA_SCENE2, HA_SCENE3,
    HA_SCENE4, HA_SWAPTOP, HA_MEDIA_NEXT, HA_HIDE_WINDOW, HA_PAUSE, HA_COUNT
};
struct HotkeyDef {
    const wchar_t* name;
    UINT defMods, defVk;
};
extern const HotkeyDef kHotkeys[HA_COUNT];
extern UINT g_hk[HA_COUNT];
extern std::wstring g_hotkeyNote;
void LoadHotkeys();
void SaveHotkeysToIni();
void RegisterHotkeys(HWND hwnd);
void UnregisterHotkeys(HWND hwnd);
std::wstring HotkeyName(UINT v);
UINT HkPack(UINT mods, UINT vk);
bool AutostartOn();
bool SetAutostart(bool on);
void DoInstall(bool install);
int CameraInstallState();
void DoExportJson(HWND owner);
void DoImportJson(HWND owner);
void ReloadAfterImport();
void RestartCameraForFormat(const wchar_t* note);
