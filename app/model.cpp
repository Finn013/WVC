// WVC: scenes, sources (items) and the output; saving to settings.ini; migration of v0.3–v0.10 settings.
#include "common.h"

const wchar_t* const kShapeNames[SH_COUNT] = {
    L"Прямоугольник", L"Скруглённый прямоугольник", L"Круг", L"Овал", L"Шестиугольник", L"Восьмиугольник",
    L"Ромб", L"Звезда", L"Сердце", L"Облачко (реплика)", L"Своя форма (PNG-маска)"};
const wchar_t* const kTypeNames[IT_COUNT] = {L"Экран/область", L"Окно", L"Монитор", L"Веб-камера",
                                             L"Медиафайл", L"Телефон", L"Текст", L"Логотип/картинка"};
const int kImageSecs[5] = {3, 5, 10, 30, 60};

Project g_proj;
std::mutex g_projMx;
std::atomic<int> g_projVer{1};
std::atomic<bool> g_projDirty{false};
IniDoc g_ini;

Project ProjSnapshot() {
    std::lock_guard<std::mutex> lk(g_projMx);
    return g_proj;
}

void ProjEdit(const std::function<void(Project&)>& f) {
    {
        std::lock_guard<std::mutex> lk(g_projMx);
        f(g_proj);
    }
    ++g_projVer;
    g_projDirty = true;
}

Scene* CurScene(Project& p) {
    if (p.scenes.empty()) return nullptr;
    p.cur = std::clamp(p.cur, 0, static_cast<int>(p.scenes.size()) - 1);
    return &p.scenes[static_cast<size_t>(p.cur)];
}

const Scene* CurSceneC(const Project& p) {
    if (p.scenes.empty()) return nullptr;
    return &p.scenes[static_cast<size_t>(std::clamp(p.cur, 0, static_cast<int>(p.scenes.size()) - 1))];
}

Item* FindItem(Project& p, int uid, int* sceneIdx) {
    if (!uid) return nullptr;
    for (size_t s = 0; s < p.scenes.size(); ++s)
        for (auto& it : p.scenes[s].items)
            if (it.uid == uid) {
                if (sceneIdx) *sceneIdx = static_cast<int>(s);
                return &it;
            }
    return nullptr;
}

const Item* FindItemC(const Project& p, int uid) {
    if (!uid) return nullptr;
    for (const auto& sc : p.scenes)
        for (const auto& it : sc.items)
            if (it.uid == uid) return &it;
    return nullptr;
}

std::wstring ShapeDef::Key() const {
    return std::to_wstring(kind) + L"," + std::to_wstring(radius) + L"," + std::to_wstring(feather) + L"," +
           (kind == SH_MASK ? mask : std::wstring());
}

static const wchar_t* DefaultName(int type) {
    switch (type) {
    case IT_REGION: return L"Экран";
    case IT_WINDOW: return L"Окно";
    case IT_MONITOR: return L"Монитор";
    case IT_WEBCAM: return L"Веб-камера";
    case IT_MEDIA: return L"Медиа";
    case IT_PHONE: return L"Телефон";
    case IT_TEXT: return L"Текст";
    default: return L"Картинка";
    }
}

std::wstring UniqueItemName(const Scene& sc, const std::wstring& base) {
    auto taken = [&](const std::wstring& n) {
        for (const auto& it : sc.items)
            if (it.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        std::wstring n = base + L" " + std::to_wstring(i);
        if (!taken(n)) return n;
    }
}

std::wstring UniqueSceneName(const Project& p, const std::wstring& base) {
    auto taken = [&](const std::wstring& n) {
        for (const auto& s : p.scenes)
            if (s.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        std::wstring n = base + L" " + std::to_wstring(i);
        if (!taken(n)) return n;
    }
}

Item NewItem(Project& p, int type) {
    Item it;
    it.uid = p.nextUid++;
    it.type = type;
    it.needFit = true;
    it.name = DefaultName(type);
    const RECT pr = PrimaryMonitorRect();
    it.region = pr;
    it.monRect = pr;
    it.phoneConn = p.defPhoneConn;
    it.phoneRes = p.defPhoneRes;
    it.phoneCodec = p.defPhoneCodec;
    it.autoRotate = p.defAutoRotate;
    it.phoneIp = p.lastPhoneIp;
    if (type == IT_TEXT) it.text = L"Текст";
    if (type == IT_WEBCAM) it.mirror = true;  // like a mirror, as Zoom / Telegram show yourself
    return it;
}

void OutSizeFor(const OutputCfg& o, int* w, int* h) {
    if (o.native) {
        int nw = o.nativeW, nh = o.nativeH;
        if (nw < 64 || nh < 64) {
            nw = 1280;
            nh = 720;
        }
        const double lim = 1920.0 / (std::max)(nw, nh);
        if (lim < 1.0) {
            nw = static_cast<int>(nw * lim);
            nh = static_cast<int>(nh * lim);
        }
        *w = (std::max)(64, nw & ~1);
        *h = (std::max)(64, nh & ~1);
        return;
    }
    static const int sz[2][4][2] = {
        {{1280, 720}, {960, 720}, {720, 720}, {720, 1280}},
        {{1920, 1080}, {1440, 1080}, {1080, 1080}, {1080, 1920}},
    };
    const int q = std::clamp(o.quality, 0, 1), a = std::clamp(o.aspect, 0, 3);
    *w = sz[q][a][0];
    *h = sz[q][a][1];
}

double AspectValue(int aspect) {
    switch (aspect) {
    case 1: return 16.0 / 9.0;
    case 2: return 4.0 / 3.0;
    case 3: return 1.0;
    case 4: return 9.0 / 16.0;
    case 5: return 3.0 / 4.0;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// v0.11 format
// ---------------------------------------------------------------------------
static const wchar_t* kMain = L"WVC";
static std::wstring SceneSec(int i) { return L"Scene " + std::to_wstring(i + 1); }
static std::wstring ItemSec(int s, int i) { return L"Scene " + std::to_wstring(s + 1) + L" Item " + std::to_wstring(i + 1); }
static const wchar_t* kCropK[4] = {L"CropL", L"CropT", L"CropR", L"CropB"};

static void WriteShape(IniDoc* d, const std::wstring& sec, const std::wstring& pfx, const ShapeDef& sh) {
    d->SetInt(sec, pfx + L"Shape", sh.kind);
    d->SetInt(sec, pfx + L"Radius", sh.radius);
    d->SetInt(sec, pfx + L"Feather", sh.feather);
    d->Set(sec, pfx + L"Mask", sh.mask);
}

static void ReadShape(const IniDoc& d, const std::wstring& sec, const std::wstring& pfx, ShapeDef* sh) {
    sh->kind = std::clamp(d.Int(sec, pfx + L"Shape", sh->kind), 0, SH_COUNT - 1);
    sh->radius = std::clamp(d.Int(sec, pfx + L"Radius", sh->radius), 0, 50);
    sh->feather = std::clamp(d.Int(sec, pfx + L"Feather", sh->feather), 0, 30);
    sh->mask = d.Str(sec, pfx + L"Mask");
}

static void WriteRect(IniDoc* d, const std::wstring& sec, const std::wstring& pfx, const RECT& r) {
    d->SetInt(sec, pfx + L"X", r.left);
    d->SetInt(sec, pfx + L"Y", r.top);
    d->SetInt(sec, pfx + L"W", r.right - r.left);
    d->SetInt(sec, pfx + L"H", r.bottom - r.top);
}

static RECT ReadRect(const IniDoc& d, const std::wstring& sec, const std::wstring& pfx, const RECT& def) {
    const int w = d.Int(sec, pfx + L"W", -1), h = d.Int(sec, pfx + L"H", -1);
    if (w < 16 || h < 16) return def;
    const int x = d.Int(sec, pfx + L"X", 0), y = d.Int(sec, pfx + L"Y", 0);
    return RECT{x, y, x + w, y + h};
}

void ItemFrameKeysToIni(const Item& it, IniDoc* d, const std::wstring& sec) {
    d->SetInt(sec, L"Uid", it.uid);
    d->SetInt(sec, L"Type", it.type);
    d->Set(sec, L"Name", it.name);
    d->SetInt(sec, L"Visible", it.visible);
    d->SetInt(sec, L"Locked", it.locked);
    d->SetDbl(sec, L"X", it.x);
    d->SetDbl(sec, L"Y", it.y);
    d->SetDbl(sec, L"W", it.w);
    d->SetDbl(sec, L"H", it.h);
    d->SetInt(sec, L"NeedFit", it.needFit);
    d->SetInt(sec, L"Aspect", it.aspect);
    d->SetInt(sec, L"Fit", it.fit);
    for (int e = 0; e < 4; ++e) d->SetInt(sec, kCropK[e], it.crop[e]);
    d->SetInt(sec, L"Rot", it.rot);
    d->SetInt(sec, L"Mirror", it.mirror);
    d->SetInt(sec, L"Zoom", it.zoom);
    WriteShape(d, sec, L"", it.shape);
    d->SetInt(sec, L"Opacity", it.opacity);
    d->SetInt(sec, L"Border", it.border);
}

static void ItemToIni(const Item& it, IniDoc* d, const std::wstring& sec) {
    ItemFrameKeysToIni(it, d, sec);
    switch (it.type) {
    case IT_REGION:
        WriteRect(d, sec, L"Region", it.region);
        d->SetInt(sec, L"Cursor", it.cursor);
        break;
    case IT_WINDOW:
        d->Set(sec, L"WinTitle", it.winTitle);
        d->SetInt(sec, L"WinCapture", it.winCapture);
        d->SetInt(sec, L"Cursor", it.cursor);
        break;
    case IT_MONITOR:
        d->Set(sec, L"MonDevice", it.monDevice);
        WriteRect(d, sec, L"Mon", it.monRect);
        d->SetInt(sec, L"Cursor", it.cursor);
        break;
    case IT_WEBCAM:
        d->Set(sec, L"CamLink", it.camLink);
        d->Set(sec, L"CamName", it.camName);
        break;
    case IT_MEDIA:
        d->SetInt(sec, L"MediaCount", static_cast<int>(it.media.size()));
        for (size_t i = 0; i < it.media.size(); ++i) d->Set(sec, L"Media" + std::to_wstring(i), it.media[i]);
        d->SetInt(sec, L"ImageSec", it.imageSec);
        break;
    case IT_PHONE:
        d->SetInt(sec, L"PhoneConn", it.phoneConn);
        d->Set(sec, L"PhoneIp", it.phoneIp);
        d->Set(sec, L"PhoneSerial", it.phoneSerial);
        d->SetInt(sec, L"PhoneSrc", it.phoneSrc);
        d->SetInt(sec, L"PhoneRes", it.phoneRes);
        d->SetInt(sec, L"PhoneCodec", it.phoneCodec);
        d->SetInt(sec, L"AutoRotate", it.autoRotate);
        d->SetInt(sec, L"PhoneOrient", it.phoneOrient);
        break;
    case IT_TEXT:
        d->Set(sec, L"Text", EscapeMultiline(it.text));
        d->SetInt(sec, L"TextPx", it.textPx);
        d->SetInt(sec, L"TextColor", it.textColor);
        d->SetInt(sec, L"TextBg", it.textBg);
        d->SetInt(sec, L"TextBold", it.textBold);
        break;
    case IT_IMAGE:
        d->Set(sec, L"Image", it.image);
        break;
    }
}

static Item ItemFromIni(const IniDoc& d, const std::wstring& sec) {
    Item it;
    it.uid = d.Int(sec, L"Uid", 0);
    it.type = std::clamp(d.Int(sec, L"Type", 0), 0, IT_COUNT - 1);
    it.name = d.Str(sec, L"Name", kTypeNames[it.type]);
    it.visible = d.Int(sec, L"Visible", 1) != 0;
    it.locked = d.Int(sec, L"Locked", 0) != 0;
    it.x = d.Dbl(sec, L"X", 0);
    it.y = d.Dbl(sec, L"Y", 0);
    it.w = std::clamp(d.Dbl(sec, L"W", 1), 0.002, 20.0);
    it.h = std::clamp(d.Dbl(sec, L"H", 1), 0.002, 20.0);
    it.needFit = d.Int(sec, L"NeedFit", 0) != 0;
    it.aspect = std::clamp(d.Int(sec, L"Aspect", 0), 0, 5);
    it.fit = std::clamp(d.Int(sec, L"Fit", 0), 0, 2);
    for (int e = 0; e < 4; ++e) it.crop[e] = std::clamp(d.Int(sec, kCropK[e], 0), 0, 900);
    it.rot = std::clamp(d.Int(sec, L"Rot", 0), 0, 3);
    it.mirror = d.Int(sec, L"Mirror", 0) != 0;
    it.zoom = std::clamp(d.Int(sec, L"Zoom", 100), 100, 400);
    ReadShape(d, sec, L"", &it.shape);
    it.opacity = std::clamp(d.Int(sec, L"Opacity", 100), 0, 100);
    it.border = d.Int(sec, L"Border", 0) != 0;
    const RECT pr = PrimaryMonitorRect();
    it.region = ReadRect(d, sec, L"Region", pr);
    it.winTitle = d.Str(sec, L"WinTitle");
    it.winCapture = std::clamp(d.Int(sec, L"WinCapture", 0), 0, 1);
    it.monDevice = d.Str(sec, L"MonDevice");
    it.monRect = ReadRect(d, sec, L"Mon", pr);
    it.cursor = d.Int(sec, L"Cursor", 0) != 0;
    it.camLink = d.Str(sec, L"CamLink");
    it.camName = d.Str(sec, L"CamName");
    const int mc = std::clamp(d.Int(sec, L"MediaCount", 0), 0, 500);
    for (int i = 0; i < mc; ++i) {
        std::wstring f = d.Str(sec, L"Media" + std::to_wstring(i));
        if (!f.empty()) it.media.push_back(f);
    }
    it.imageSec = std::clamp(d.Int(sec, L"ImageSec", 1), 0, 4);
    it.phoneConn = std::clamp(d.Int(sec, L"PhoneConn", 0), 0, 1);
    it.phoneIp = d.Str(sec, L"PhoneIp");
    it.phoneSerial = d.Str(sec, L"PhoneSerial");
    it.phoneSrc = std::clamp(d.Int(sec, L"PhoneSrc", 0), 0, 2);
    it.phoneRes = std::clamp(d.Int(sec, L"PhoneRes", 0), 0, 1);
    it.phoneCodec = std::clamp(d.Int(sec, L"PhoneCodec", 0), 0, 2);
    it.autoRotate = d.Int(sec, L"AutoRotate", 1) != 0;
    it.phoneOrient = std::clamp(d.Int(sec, L"PhoneOrient", 0), 0, 2);
    it.text = UnescapeMultiline(d.Str(sec, L"Text"));
    it.textPx = std::clamp(d.Int(sec, L"TextPx", 64), 8, 400);
    it.textColor = std::clamp(d.Int(sec, L"TextColor", 0), 0, 7);
    it.textBg = d.Int(sec, L"TextBg", 1) != 0;
    it.textBold = d.Int(sec, L"TextBold", 1) != 0;
    it.image = d.Str(sec, L"Image");
    return it;
}

void ProjectToIni(const Project& p, IniDoc* d) {
    // drop the previous project sections (old «Profile: …» sections stay for older versions)
    for (const auto& n : d->SectionNames())
        if (n.rfind(L"Scene ", 0) == 0) d->RemoveSection(n);
    d->SetInt(kMain, L"Format", 11);
    const OutputCfg& o = p.out;
    d->SetInt(kMain, L"Resolution", o.quality);
    d->SetInt(kMain, L"Aspect", o.aspect);
    d->SetInt(kMain, L"FPS", o.fps);
    d->SetInt(kMain, L"OutNative", o.native);
    d->SetInt(kMain, L"OutNativeUid", o.nativeUid);
    d->SetInt(kMain, L"OutNativeW", o.nativeW);
    d->SetInt(kMain, L"OutNativeH", o.nativeH);
    WriteShape(d, kMain, L"Out", o.shape);
    for (int e = 0; e < 4; ++e) d->SetInt(kMain, std::wstring(L"Out") + kCropK[e], o.crop[e]);
    d->SetInt(kMain, L"OutBg", o.bgMode);
    d->SetInt(kMain, L"OutBgColor", static_cast<int>(o.bgColor));
    d->Set(kMain, L"OutBgImage", o.bgImage);
    d->SetInt(kMain, L"Transition", p.transition);
    d->SetInt(kMain, L"TransitionMs", p.transMs);
    d->SetInt(kMain, L"PauseMode", p.pauseMode);
    d->Set(kMain, L"PauseText", p.pauseText);
    d->Set(kMain, L"PauseImage", p.pauseImage);
    d->SetInt(kMain, L"IdleFrame", p.idleFrame);
    d->SetInt(kMain, L"KeepCamOn", p.keepCamOn);
    d->SetInt(kMain, L"LiveWhenStopped", p.liveWhenStopped);
    d->SetInt(kMain, L"MinToTray", p.minToTray);
    d->SetInt(kMain, L"CloseToTray", p.closeToTray);
    d->SetInt(kMain, L"PhoneDefConn", p.defPhoneConn);
    d->SetInt(kMain, L"PhoneDefRes", p.defPhoneRes);
    d->SetInt(kMain, L"PhoneDefCodec", p.defPhoneCodec);
    d->SetInt(kMain, L"PhoneDefAutoRotate", p.defAutoRotate);
    d->Set(kMain, L"LastPhoneIp", p.lastPhoneIp);
    d->SetInt(kMain, L"NextUid", p.nextUid);
    d->SetInt(kMain, L"SceneCount", static_cast<int>(p.scenes.size()));
    d->SetInt(kMain, L"CurScene", p.cur);
    d->SetInt(kMain, L"NextScene", p.next);
    for (size_t s = 0; s < p.scenes.size(); ++s) {
        const Scene& sc = p.scenes[s];
        const std::wstring ss = SceneSec(static_cast<int>(s));
        d->Set(ss, L"Name", sc.name);
        d->SetInt(ss, L"ItemCount", static_cast<int>(sc.items.size()));
        for (size_t i = 0; i < sc.items.size(); ++i)
            ItemToIni(sc.items[i], d, ItemSec(static_cast<int>(s), static_cast<int>(i)));
    }
}

static void ProjectFromIni11(const IniDoc& d, Project* p) {
    OutputCfg& o = p->out;
    o.quality = std::clamp(d.Int(kMain, L"Resolution", 1), 0, 1);
    o.aspect = std::clamp(d.Int(kMain, L"Aspect", 0), 0, 3);
    o.fps = d.Int(kMain, L"FPS", 30) == 60 ? 60 : 30;
    o.native = d.Int(kMain, L"OutNative", 0) != 0;
    o.nativeUid = d.Int(kMain, L"OutNativeUid", 0);
    o.nativeW = std::clamp(d.Int(kMain, L"OutNativeW", 1280), 64, 8192);
    o.nativeH = std::clamp(d.Int(kMain, L"OutNativeH", 720), 64, 8192);
    ReadShape(d, kMain, L"Out", &o.shape);
    for (int e = 0; e < 4; ++e) o.crop[e] = std::clamp(d.Int(kMain, std::wstring(L"Out") + kCropK[e], 0), 0, 450);
    o.bgMode = std::clamp(d.Int(kMain, L"OutBg", 0), 0, 2);
    o.bgColor = static_cast<COLORREF>(d.Int(kMain, L"OutBgColor", 0)) & 0xFFFFFF;
    o.bgImage = d.Str(kMain, L"OutBgImage");
    p->transition = std::clamp(d.Int(kMain, L"Transition", 1), 0, 4);
    p->transMs = std::clamp(d.Int(kMain, L"TransitionMs", 500), 50, 5000);
    p->pauseMode = std::clamp(d.Int(kMain, L"PauseMode", 2), 0, 3);
    p->pauseText = d.Str(kMain, L"PauseText", L"Скоро вернусь");
    p->pauseImage = d.Str(kMain, L"PauseImage");
    p->idleFrame = d.Int(kMain, L"IdleFrame", 1) != 0;
    p->keepCamOn = d.Int(kMain, L"KeepCamOn", 0) != 0;
    p->liveWhenStopped = d.Int(kMain, L"LiveWhenStopped", 1) != 0;
    p->minToTray = d.Int(kMain, L"MinToTray", 1) != 0;
    p->closeToTray = d.Int(kMain, L"CloseToTray", 0) != 0;
    p->defPhoneConn = std::clamp(d.Int(kMain, L"PhoneDefConn", 0), 0, 1);
    p->defPhoneRes = std::clamp(d.Int(kMain, L"PhoneDefRes", 0), 0, 1);
    p->defPhoneCodec = std::clamp(d.Int(kMain, L"PhoneDefCodec", 0), 0, 2);
    p->defAutoRotate = d.Int(kMain, L"PhoneDefAutoRotate", 1) != 0;
    p->lastPhoneIp = d.Str(kMain, L"LastPhoneIp");
    p->nextUid = (std::max)(1, d.Int(kMain, L"NextUid", 1));
    const int n = std::clamp(d.Int(kMain, L"SceneCount", 0), 0, 200);
    for (int s = 0; s < n; ++s) {
        Scene sc;
        const std::wstring ss = SceneSec(s);
        sc.name = d.Str(ss, L"Name", L"Сцена " + std::to_wstring(s + 1));
        const int ic = std::clamp(d.Int(ss, L"ItemCount", 0), 0, 500);
        for (int i = 0; i < ic; ++i) {
            const std::wstring is = ItemSec(s, i);
            if (!d.HasSection(is)) continue;
            Item it = ItemFromIni(d, is);
            if (it.uid <= 0 || FindItem(*p, it.uid)) it.uid = p->nextUid++;
            p->nextUid = (std::max)(p->nextUid, it.uid + 1);
            sc.items.push_back(it);
        }
        p->scenes.push_back(sc);
    }
    p->cur = d.Int(kMain, L"CurScene", 0);
    p->next = d.Int(kMain, L"NextScene", -1);
}

// ---------------------------------------------------------------------------
// Migration of v0.3 – v0.10 settings (old keys in [WVC] and [Profile: name] sections)
// ---------------------------------------------------------------------------
namespace legacy {
static const int kSrcKinds = 5;
enum { KIND_SCREEN = 0, KIND_CAM = 1, KIND_MEDIA = 2, KIND_SCREEN2 = 3, KIND_PHONE = 4 };
static const double kZoom[] = {1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
static const double kPipSize[] = {0.20, 0.25, 0.33};
static const double kLogoSize[] = {0.10, 0.15, 0.20, 0.30};

struct SrcFrame {
    int rot = 0;
    bool mirror = false;
    int zoom = 0;
    int fit = 0;
    int crop[4] = {0, 0, 0, 0};
};

struct Settings {
    int quality = 1, aspect = 0, fps = 30;
    bool cursor = false;
    SrcFrame fr[kSrcKinds] = {SrcFrame{}, SrcFrame{0, false, 0, 1, {0, 0, 0, 0}}, SrcFrame{}, SrcFrame{}, SrcFrame{}};
    int pauseMode = 2;
    std::wstring pauseImage, pauseText = L"Скоро вернусь";
    int srcMode = 0;
    std::wstring srcTitle;
    bool hasRegion = false;
    RECT region{};
    int winCapture = 0;
    RECT region2{};
    int mainSrc = 0, pipSrc = 2, layout = 0;
    std::wstring camLink, camName;
    bool camMirror = true;
    int pipCorner = 0, pipSize = 1, pipX = -1, pipY = -1, pipW = 2500;
    int pipShape = 0, frameShape = 0, shapeColor = 0;
    std::vector<std::wstring> media;
    int imageSec = 1;
    bool textOn = false;
    std::wstring text;
    int textPos = 3, textSize = 1;
    std::wstring logo;
    int logoCorner = 2, logoSize = 1;
    int transition = 1, transDur = 1;
    bool idleFrame = true, keepCamOn = false;
    std::wstring phoneIp;
    int phoneSrc = 0, phoneRes = 0, phoneConn = 0;
    std::wstring phoneUsbSerial;
    bool phoneVertical = false;
    bool srcOn[kSrcKinds] = {true, true, true, false, false};
    int sideSplit = 5000;
    bool sideVertical = false;
    int sideX[2] = {0, 0}, sideY[2] = {0, 0}, sideZoom[2] = {10000, 10000};
};

static const IniDoc* D = nullptr;
static int IniInt(const std::wstring& sec, const wchar_t* key, int def) { return D->Int(sec, key, def); }
static std::wstring IniStr(const std::wstring& sec, const wchar_t* key) { return D->Str(sec, key); }
static std::wstring IniStrDef(const std::wstring& sec, const wchar_t* key, const std::wstring& def) {
    return D->Str(sec, key, def);
}

static std::vector<std::wstring> ProfileNames() {
    std::vector<std::wstring> v;
    std::wstring all = IniStr(kMain, L"Profiles");
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
static std::wstring ProfileSection(const std::wstring& name) { return L"Profile: " + name; }

static RECT DefaultRegion2() {
    RECT p = PrimaryMonitorRect();
    const int w = (std::max)(64, static_cast<int>(p.right - p.left) / 2);
    const int h = (std::max)(64, static_cast<int>(p.bottom - p.top) / 2);
    const int x = p.left + (static_cast<int>(p.right - p.left) - w) / 2;
    const int y = p.top + (static_cast<int>(p.bottom - p.top) - h) / 2;
    return RECT{x, y, x + w, y + h};
}

static void OutSize(int quality, int aspect, int* w, int* h) {
    OutputCfg o;
    o.quality = quality;
    o.aspect = aspect;
    OutSizeFor(o, w, h);
}

static bool SquareShape(int shape) { return shape >= 2; }

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

static void FrameKey(wchar_t* buf, int kind, const wchar_t* name) { swprintf(buf, 32, L"Fr%d%ls", kind, name); }
static const wchar_t* kCropKeys[4] = {L"CropL", L"CropT", L"CropR", L"CropB"};

static void ReadFrames(const std::wstring& sec, Settings& s) {
    wchar_t k[32];
    FrameKey(k, 0, L"Rot");
    if (IniInt(sec, k, -1) < 0) {
        const int lRot = IniInt(sec, L"Rotation", -1);
        if (lRot < 0) return;
        SrcFrame g;
        g.rot = std::clamp(lRot, 0, 3);
        g.mirror = IniInt(sec, L"Mirror", 0) != 0;
        g.zoom = std::clamp(IniInt(sec, L"Zoom", 0), 0, 5);
        g.fit = std::clamp(IniInt(sec, L"Fit", 0), 0, 1);
        const int oldMain = IniInt(sec, L"MainSrc", 0);
        s.fr[KIND_SCREEN] = g;
        s.fr[KIND_SCREEN2] = g;
        s.fr[KIND_MEDIA] = g;
        SrcFrame cam;
        cam.fit = 1;
        if (oldMain == KIND_CAM) {
            cam = g;
            cam.mirror = false;
            if (g.mirror) s.camMirror = !s.camMirror;
        }
        s.fr[KIND_CAM] = cam;
        s.fr[KIND_PHONE] = SrcFrame{};
        return;
    }
    for (int kind = 0; kind < kSrcKinds; ++kind) {
        SrcFrame& f = s.fr[kind];
        FrameKey(k, kind, L"Rot");
        f.rot = std::clamp(IniInt(sec, k, f.rot), 0, 3);
        FrameKey(k, kind, L"Mirror");
        f.mirror = IniInt(sec, k, f.mirror) != 0;
        FrameKey(k, kind, L"Zoom");
        f.zoom = std::clamp(IniInt(sec, k, f.zoom), 0, 5);
        FrameKey(k, kind, L"Fit");
        f.fit = std::clamp(IniInt(sec, k, f.fit), 0, 1);
        for (int e = 0; e < 4; ++e) {
            FrameKey(k, kind, kCropKeys[e]);
            f.crop[e] = std::clamp(IniInt(sec, k, f.crop[e]), 0, 45);
        }
    }
}

static void MarkUsedKinds(const std::wstring& sec, bool used[kSrcKinds]) {
    const int main = IniInt(sec, L"MainSrc", -1);
    const int view = IniInt(sec, L"View", -1);
    if (main >= 0 && main < kSrcKinds) used[main] = true;
    if (main < 0 && view == 1) used[KIND_CAM] = true;
    if (view == 2) used[KIND_CAM] = true;
    const int lay = IniInt(sec, L"Layout", -1);
    const int second = IniInt(sec, L"SecondSrc", -1), pip = IniInt(sec, L"PipSrc", -1);
    int k2 = -1;
    if (lay > 0 && second >= 1) k2 = second - 1;
    else if (lay < 0 && pip >= 1) k2 = pip - 1;
    if (k2 >= 0 && k2 < kSrcKinds) used[k2] = true;
}

static void ReadSourcesOn(const std::wstring& sec, Settings& s) {
    static const wchar_t* keys[kSrcKinds] = {nullptr, L"SrcWebcam", L"SrcMedia", L"SrcScreen2", L"SrcPhone"};
    bool need = false;
    for (int k = 1; k < kSrcKinds; ++k) need = need || IniInt(sec, keys[k], -1) < 0;
    bool used[kSrcKinds] = {};
    bool fresh = false;
    if (need) {
        fresh = IniInt(sec, L"MainSrc", -1) < 0 && IniInt(sec, L"View", -1) < 0 && IniInt(sec, L"HasRegion", -1) < 0;
        MarkUsedKinds(sec, used);
        for (const auto& n : ProfileNames()) MarkUsedKinds(ProfileSection(n), used);
    }
    for (int k = 1; k < kSrcKinds; ++k) {
        const int v = IniInt(sec, keys[k], -1);
        if (v >= 0) {
            s.srcOn[k] = v != 0;
            continue;
        }
        switch (k) {
        case KIND_CAM: s.srcOn[k] = fresh || used[k] || !s.camLink.empty(); break;
        case KIND_MEDIA: s.srcOn[k] = fresh || used[k] || !s.media.empty(); break;
        case KIND_SCREEN2: s.srcOn[k] = used[k]; break;
        default: s.srcOn[k] = used[k] || !s.phoneIp.empty() || s.phoneConn == 1; break;
        }
    }
    s.srcOn[0] = true;
}

static void ReadSettings(const std::wstring& sec, Settings& s, bool format) {
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
        s.phoneConn = std::clamp(IniInt(sec, L"PhoneConn", s.phoneConn), 0, 1);
        s.phoneUsbSerial = IniStr(sec, L"PhoneUsbSerial");
        s.phoneVertical = IniInt(sec, L"PhoneVertical", s.phoneVertical) != 0;
    }
    s.cursor = IniInt(sec, L"Cursor", s.cursor) != 0;
    s.pauseMode = std::clamp(IniInt(sec, L"PauseMode", s.pauseMode), 0, 3);
    s.pauseImage = IniStr(sec, L"PauseImage");
    s.pauseText = IniStrDef(sec, L"PauseText", s.pauseText);
    s.srcMode = std::clamp(IniInt(sec, L"SourceMode", 0), 0, 1);
    s.srcTitle = IniStr(sec, L"WindowTitle");
    s.hasRegion = IniInt(sec, L"HasRegion", s.hasRegion) != 0;
    s.region.left = IniInt(sec, L"Left", s.region.left);
    s.region.top = IniInt(sec, L"Top", s.region.top);
    s.region.right = IniInt(sec, L"Right", s.region.right);
    s.region.bottom = IniInt(sec, L"Bottom", s.region.bottom);
    if (s.region.right - s.region.left < 16 || s.region.bottom - s.region.top < 16) s.hasRegion = false;
    s.winCapture = std::clamp(IniInt(sec, L"WindowCapture", s.winCapture), 0, 1);
    {
        const int w2 = IniInt(sec, L"Region2W", -1), h2 = IniInt(sec, L"Region2H", -1);
        if (w2 >= 16 && h2 >= 16) {
            const int x2 = IniInt(sec, L"Region2X", 0), y2 = IniInt(sec, L"Region2Y", 0);
            s.region2 = RECT{x2, y2, x2 + w2, y2 + h2};
        }
        if (s.region2.right - s.region2.left < 16 || s.region2.bottom - s.region2.top < 16) s.region2 = DefaultRegion2();
    }
    const int oldView = IniInt(sec, L"View", -1);
    s.mainSrc = IniInt(sec, L"MainSrc", -1);
    int rawPip = IniInt(sec, L"PipSrc", -1);
    if (s.mainSrc < 0 || rawPip < 0) {
        s.mainSrc = oldView == 1 ? 1 : 0;
        rawPip = oldView == 2 ? 2 : 0;
    }
    s.mainSrc = std::clamp(s.mainSrc, 0, 4);
    rawPip = std::clamp(rawPip, 0, 5);
    const int lay = IniInt(sec, L"Layout", -1);
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
    if (s.pipX < 0 || s.pipY < 0 || s.pipW < 0) {
        int W, H;
        OutSize(s.quality, s.aspect, &W, &H);
        PipPreset(s, W, H);
    }
    s.pipX = std::clamp(s.pipX, 0, 10000);
    s.pipY = std::clamp(s.pipY, 0, 10000);
    s.pipW = std::clamp(s.pipW, 500, 9000);
    s.pipShape = std::clamp(IniInt(sec, L"PipShape", s.pipShape), 0, 4);
    s.frameShape = std::clamp(IniInt(sec, L"FrameShape", s.frameShape), 0, 4);
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
    ReadFrames(sec, s);
    s.sideSplit = std::clamp(IniInt(sec, L"SideSplit", s.sideSplit), 1000, 9000);
    s.sideVertical = IniInt(sec, L"SideVertical", s.sideVertical) != 0;
    for (int i = 0; i < 2; ++i) {
        wchar_t k[24];
        swprintf(k, 24, L"Side%dX", i + 1);
        s.sideX[i] = std::clamp(IniInt(sec, k, s.sideX[i]), -10000, 10000);
        swprintf(k, 24, L"Side%dY", i + 1);
        s.sideY[i] = std::clamp(IniInt(sec, k, s.sideY[i]), -10000, 10000);
        swprintf(k, 24, L"Side%dZoom", i + 1);
        s.sideZoom[i] = std::clamp(IniInt(sec, k, s.sideZoom[i]), 3000, 40000);
    }
    if (format) ReadSourcesOn(sec, s);
}

// old shapes: 0 rect, 1 rounded, 2 circle, 3 hexagon, 4 octagon
static ShapeDef MapShape(int old) {
    ShapeDef d;
    static const int m[5] = {SH_RECT, SH_ROUND, SH_CIRCLE, SH_HEX, SH_OCT};
    d.kind = m[std::clamp(old, 0, 4)];
    d.radius = 14;
    return d;
}

// source-space crop (%, old) -> crop as shown after rotation / mirror (per mille)
static void CropToDisplay(const int src[4], int rot, bool mirror, int out[4]) {
    double x0 = src[0] / 100.0, y0 = src[1] / 100.0, x1 = 1 - src[2] / 100.0, y1 = 1 - src[3] / 100.0;
    auto fwd = [&](double x, double y, double* ox, double* oy) {
        switch (rot & 3) {
        case 1: *ox = 1 - y; *oy = x; break;
        case 2: *ox = 1 - x; *oy = 1 - y; break;
        case 3: *ox = y; *oy = 1 - x; break;
        default: *ox = x; *oy = y; break;
        }
        if (mirror) *ox = 1 - *ox;
    };
    double ax, ay, bx, by;
    fwd(x0, y0, &ax, &ay);
    fwd(x1, y1, &bx, &by);
    const double l = (std::min)(ax, bx), t = (std::min)(ay, by), r = (std::max)(ax, bx), b = (std::max)(ay, by);
    out[0] = static_cast<int>(std::lround(l * 1000));
    out[1] = static_cast<int>(std::lround(t * 1000));
    out[2] = static_cast<int>(std::lround((1 - r) * 1000));
    out[3] = static_cast<int>(std::lround((1 - b) * 1000));
}

struct Ctx {
    Project* p;
    const Settings* s;
    int W, H;
};

static void ApplyFrame(Item& it, const SrcFrame& f, bool cam, bool camMirror) {
    it.rot = f.rot & 3;
    it.mirror = cam ? camMirror : f.mirror;
    it.zoom = static_cast<int>(std::lround(kZoom[std::clamp(f.zoom, 0, 5)] * 100));
    it.fit = f.fit;
    CropToDisplay(f.crop, it.rot, it.mirror, it.crop);
}

// a source of the old fixed set -> item (type settings + its frame settings)
static Item MakeKindItem(Project& p, const Settings& s, int kind) {
    Item it;
    switch (kind) {
    case KIND_SCREEN:
        if (s.srcMode == 1 && !s.srcTitle.empty()) {
            it = NewItem(p, IT_WINDOW);
            it.winTitle = s.srcTitle;
            it.winCapture = s.winCapture;
            it.name = L"Окно";
        } else {
            it = NewItem(p, IT_REGION);
            it.region = s.hasRegion ? s.region : PrimaryMonitorRect();
            it.name = L"Экран 1";
        }
        it.cursor = s.cursor;
        break;
    case KIND_SCREEN2:
        it = NewItem(p, IT_REGION);
        it.region = s.region2;
        it.name = L"Экран 2";
        break;
    case KIND_CAM:
        it = NewItem(p, IT_WEBCAM);
        it.camLink = s.camLink;
        it.camName = s.camName;
        break;
    case KIND_MEDIA:
        it = NewItem(p, IT_MEDIA);
        it.media = s.media;
        it.imageSec = s.imageSec;
        break;
    default:
        it = NewItem(p, IT_PHONE);
        it.phoneConn = s.phoneConn;
        it.phoneIp = s.phoneIp;
        it.phoneSerial = s.phoneUsbSerial;
        it.phoneSrc = s.phoneSrc;
        it.phoneRes = s.phoneRes;
        it.phoneCodec = 0;
        it.autoRotate = true;
        break;
    }
    it.needFit = false;
    ApplyFrame(it, s.fr[kind], kind == KIND_CAM, s.camMirror);
    return it;
}

// picture size as shown (after rotation), as far as it is known when migrating
static void GuessNative(const Settings& s, int kind, double* dw, double* dh) {
    double w = 1280, h = 720;
    if (kind == KIND_SCREEN && s.hasRegion && s.srcMode == 0) {
        w = s.region.right - s.region.left;
        h = s.region.bottom - s.region.top;
    } else if (kind == KIND_SCREEN2) {
        w = s.region2.right - s.region2.left;
        h = s.region2.bottom - s.region2.top;
    } else if (kind == KIND_PHONE && s.phoneSrc == 2) {
        w = 720;
        h = 1280;
    } else if (kind == KIND_SCREEN) {
        const RECT r = PrimaryMonitorRect();
        w = r.right - r.left;
        h = r.bottom - r.top;
    }
    if (s.fr[kind].rot & 1) std::swap(w, h);
    *dw = (std::max)(1.0, w);
    *dh = (std::max)(1.0, h);
}

static void SetBoxPx(Item& it, double x, double y, double w, double h, int W, int H) {
    it.x = x / W;
    it.y = y / H;
    it.w = (std::max)(1.0, w) / W;
    it.h = (std::max)(1.0, h) / H;
}

// «Рядом» half with its own pan / scale: express it as an exact box + crop (fit = stretch)
static void PlaceSide(Item& it, const Settings& s, int kind, const RECT& slot, int idx, int W, int H) {
    const double z = s.sideZoom[idx] / 10000.0;
    const double tw = slot.right - slot.left, th = slot.bottom - slot.top;
    const double panX = s.sideX[idx] * tw / 10000.0, panY = s.sideY[idx] * th / 10000.0;
    if (std::fabs(z - 1.0) < 1e-6 && std::fabs(panX) < 0.5 && std::fabs(panY) < 0.5) {
        SetBoxPx(it, slot.left, slot.top, tw, th, W, H);
        return;
    }
    double DW, DH;
    GuessNative(s, kind, &DW, &DH);
    // display-space source rect after crop + zoom
    double sx = it.crop[0] / 1000.0 * DW, sy = it.crop[1] / 1000.0 * DH;
    double sw = DW * (1 - (it.crop[0] + it.crop[2]) / 1000.0), sh = DH * (1 - (it.crop[1] + it.crop[3]) / 1000.0);
    sw = (std::max)(1.0, sw);
    sh = (std::max)(1.0, sh);
    const double zz = it.zoom / 100.0;
    if (zz > 1.0) {
        sx += (sw - sw / zz) / 2;
        sy += (sh - sh / zz) / 2;
        sw /= zz;
        sh /= zz;
    }
    double dx = 0, dy = 0, dw = tw, dh = th;
    if (it.fit == 0) {
        const double sc = (std::min)(tw / sw, th / sh);
        dw = sw * sc;
        dh = sh * sc;
        dx = (tw - dw) / 2;
        dy = (th - dh) / 2;
    } else {
        const double sc = (std::max)(tw / sw, th / sh);
        const double nw = tw / sc, nh = th / sc;
        sx += (sw - nw) / 2;
        sy += (sh - nh) / 2;
        sw = nw;
        sh = nh;
    }
    // slot scale around its centre + pan
    const double cx = tw / 2, cy = th / 2;
    double rx = cx + (dx - cx) * z + panX, ry = cy + (dy - cy) * z + panY, rw = dw * z, rh = dh * z;
    const double vx0 = (std::max)(0.0, rx), vy0 = (std::max)(0.0, ry);
    const double vx1 = (std::min)(tw, rx + rw), vy1 = (std::min)(th, ry + rh);
    if (vx1 - vx0 < 1 || vy1 - vy0 < 1) {
        it.visible = false;
        SetBoxPx(it, slot.left, slot.top, tw, th, W, H);
        return;
    }
    const double fx0 = (vx0 - rx) / rw, fy0 = (vy0 - ry) / rh, fx1 = (vx1 - rx) / rw, fy1 = (vy1 - ry) / rh;
    const double nsx0 = sx + fx0 * sw, nsy0 = sy + fy0 * sh, nsx1 = sx + fx1 * sw, nsy1 = sy + fy1 * sh;
    it.crop[0] = std::clamp(static_cast<int>(std::lround(nsx0 / DW * 1000)), 0, 900);
    it.crop[1] = std::clamp(static_cast<int>(std::lround(nsy0 / DH * 1000)), 0, 900);
    it.crop[2] = std::clamp(static_cast<int>(std::lround((1 - nsx1 / DW) * 1000)), 0, 900);
    it.crop[3] = std::clamp(static_cast<int>(std::lround((1 - nsy1 / DH) * 1000)), 0, 900);
    it.zoom = 100;
    it.fit = 2;
    SetBoxPx(it, slot.left + vx0, slot.top + vy0, vx1 - vx0, vy1 - vy0, W, H);
}

static Scene MakeScene(Project& p, const Settings& s, const std::wstring& name, const Settings& mainS) {
    Scene sc;
    sc.name = name;
    int W, H;
    OutSize(mainS.quality, mainS.aspect, &W, &H);
    const int second = s.layout == 0 ? -1 : std::clamp(s.pipSrc, 1, 5) - 1;
    bool shown[kSrcKinds] = {};
    shown[s.mainSrc] = true;
    if (second >= 0) shown[second] = true;
    // switched-on sources that this scene does not show: kept hidden, so nothing set up is lost
    for (int k = 0; k < kSrcKinds; ++k) {
        if (shown[k] || !mainS.srcOn[k]) continue;
        Item it = MakeKindItem(p, s, k);
        it.visible = false;
        it.needFit = true;
        sc.items.push_back(it);
    }
    Item main = MakeKindItem(p, s, s.mainSrc);
    if (s.layout == 2 && second >= 0) {
        RECT a, b;
        if (!s.sideVertical) {
            const int x = std::clamp(MulDiv(W, s.sideSplit, 10000), 8, W - 8);
            a = RECT{0, 0, x, H};
            b = RECT{x, 0, W, H};
        } else {
            const int y = std::clamp(MulDiv(H, s.sideSplit, 10000), 8, H - 8);
            a = RECT{0, 0, W, y};
            b = RECT{0, y, W, H};
        }
        PlaceSide(main, s, s.mainSrc, a, 0, W, H);
        sc.items.push_back(main);
        Item sec = MakeKindItem(p, s, second);
        PlaceSide(sec, s, second, b, 1, W, H);
        sc.items.push_back(sec);
    } else {
        SetBoxPx(main, 0, 0, W, H, W, H);
        if (s.frameShape != mainS.frameShape && s.frameShape != 0) main.shape = MapShape(s.frameShape);
        sc.items.push_back(main);
        if (s.layout == 1 && second >= 0) {
            Item pip = MakeKindItem(p, s, second);
            RECT r = PipRect(s, W, H);
            SetBoxPx(pip, r.left, r.top, r.right - r.left, r.bottom - r.top, W, H);
            pip.shape = MapShape(s.pipShape);
            pip.border = true;
            sc.items.push_back(pip);
        }
    }
    if (!s.logo.empty()) {
        Item lg = NewItem(p, IT_IMAGE);
        lg.name = L"Логотип";
        lg.image = s.logo;
        lg.needFit = false;
        int iw = 0, ih = 0;
        if (!ImageSize(s.logo, &iw, &ih)) {
            iw = 200;
            ih = 100;
        }
        const double maxW = W * kLogoSize[std::clamp(s.logoSize, 0, 3)], maxH = H / 2.0;
        const double sc2 = (std::min)(maxW / iw, maxH / ih);
        const double lw = iw * sc2, lh = ih * sc2, m = (std::max)(8, H / 30);
        const double x = (s.logoCorner == 1 || s.logoCorner == 3) ? m : W - m - lw;
        const double y = (s.logoCorner == 2 || s.logoCorner == 3) ? m : H - m - lh;
        SetBoxPx(lg, x, y, lw, lh, W, H);
        sc.items.push_back(lg);
    }
    if (!s.text.empty()) {
        Item tx = NewItem(p, IT_TEXT);
        tx.name = L"Надпись";
        tx.text = s.text;
        tx.visible = s.textOn;
        tx.needFit = false;
        static const int div[3] = {24, 16, 10};
        tx.textPx = (std::max)(12, 1080 / div[std::clamp(s.textSize, 0, 2)]);
        int nw = 0, nh = 0;
        TextNativeSize(tx, &nw, &nh);
        const double k = H / 1080.0;
        double bw = (std::min)(static_cast<double>(W - 2 * (std::max)(8, H / 30)), nw * k), bh = nh * k;
        const double m = (std::max)(8, H / 30);
        const int col = s.textPos % 3, row = s.textPos / 3;
        const double x = col == 0 ? m : col == 1 ? (W - bw) / 2 : W - m - bw;
        const double y = row == 0 ? m : H - m - bh;
        tx.fit = 2;
        SetBoxPx(tx, x, y, bw, bh, W, H);
        sc.items.push_back(tx);
    }
    return sc;
}

static bool Migrate(const IniDoc& d, Project* p, std::wstring* note) {
    D = &d;
    Settings mainS;
    ReadSettings(kMain, mainS, true);
    OutputCfg& o = p->out;
    o.quality = mainS.quality;
    o.aspect = mainS.aspect;
    o.fps = mainS.fps;
    o.shape = MapShape(mainS.frameShape);
    static const COLORREF cols[4] = {RGB(0, 0, 0), RGB(255, 255, 255), RGB(0, 177, 64), RGB(40, 42, 48)};
    o.bgColor = cols[std::clamp(mainS.shapeColor, 0, 3)];
    p->transition = mainS.transition;
    static const int ms[3] = {300, 500, 1000};
    p->transMs = ms[std::clamp(mainS.transDur, 0, 2)];
    p->pauseMode = mainS.pauseMode;
    p->pauseText = mainS.pauseText;
    p->pauseImage = mainS.pauseImage;
    p->idleFrame = mainS.idleFrame;
    p->keepCamOn = mainS.keepCamOn;
    p->defPhoneConn = mainS.phoneConn;
    p->defPhoneRes = mainS.phoneRes;
    p->lastPhoneIp = mainS.phoneIp;
    const std::vector<std::wstring> names = ProfileNames();
    const std::wstring last = IniStr(kMain, L"LastScene");
    int curIdx = -1;
    if (names.empty() || std::find(names.begin(), names.end(), last) == names.end()) {
        p->scenes.push_back(MakeScene(*p, mainS, names.empty() ? L"Сцена 1" : L"Текущая", mainS));
        curIdx = 0;
    }
    for (const auto& n : names) {
        Settings s = mainS;  // a scene is loaded on top of the current settings, as before
        ReadSettings(ProfileSection(n), s, false);
        p->scenes.push_back(MakeScene(*p, s, n, mainS));
        if (n == last && curIdx < 0) curIdx = static_cast<int>(p->scenes.size()) - 1;
    }
    p->cur = (std::max)(0, curIdx);
    p->next = p->scenes.size() > 1 ? (p->cur == 0 ? 1 : 0) : -1;
    if (mainS.phoneVertical && mainS.mainSrc == KIND_PHONE) {  // «вертикальная камера для телефона» -> «Как у источника»
        const Scene& sc = p->scenes[static_cast<size_t>(p->cur)];
        for (const auto& it : sc.items)
            if (it.type == IT_PHONE && it.visible) {
                o.native = true;
                o.nativeUid = it.uid;
                o.nativeW = mainS.phoneRes ? 1080 : 720;
                o.nativeH = mainS.phoneRes ? 1920 : 1280;
            }
    }
    *note = L"Настройки прежней версии перенесены: сцен — " + std::to_wstring(p->scenes.size()) +
            L". Проверьте расположение источников.";
    D = nullptr;
    return true;
}
}  // namespace legacy

bool LoadProjectFromIni(const IniDoc& d, Project* p, std::wstring* note) {
    *p = Project();
    note->clear();
    if (d.Int(kMain, L"Format", 0) >= 11) {
        ProjectFromIni11(d, p);
    } else if (d.Has(kMain, L"MainSrc") || d.Has(kMain, L"View") || d.Has(kMain, L"HasRegion") ||
               d.Has(kMain, L"Profiles")) {
        legacy::Migrate(d, p, note);
    }
    if (p->scenes.empty()) {
        Scene sc;
        sc.name = L"Сцена 1";
        p->scenes.push_back(sc);
    }
    const int n = static_cast<int>(p->scenes.size());
    p->cur = std::clamp(p->cur, 0, n - 1);
    if (p->next >= n || p->next == p->cur) p->next = -1;
    if (p->next < 0 && n > 1) p->next = p->cur == 0 ? 1 : 0;
    return true;
}

bool SaveProject() {
    Project p = ProjSnapshot();
    ProjectToIni(p, &g_ini);
    g_projDirty = false;
    return g_ini.Save(IniPath());
}
