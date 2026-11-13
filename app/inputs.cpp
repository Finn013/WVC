// WVC: live inputs shared by key — webcams (Media Foundation), media players (pictures / GIF / video),
// phones (WVC Phone over Wi-Fi or a USB cable via adb; JPEG or H.264). See docs/PHONE-PROTOCOL.md.
#include "common.h"

bool g_wsaOk = false;
HWND g_notify = nullptr;
std::atomic<bool> g_streaming{false}, g_paused{false}, g_uiVisible{true}, g_quitting{false};
std::atomic<int> g_soloUid{0};
std::atomic<bool> g_hideGlare{true};

static double MeanLuma(const uint8_t* bgra, int w, int h) {
    // a sparse grid is enough for the average brightness
    const int sx = (std::max)(1, w / 64), sy = (std::max)(1, h / 36);
    double sum = 0;
    int n = 0;
    for (int y = sy / 2; y < h; y += sy) {
        const uint8_t* row = bgra + static_cast<size_t>(y) * w * 4;
        for (int x = sx / 2; x < w; x += sx) {
            const uint8_t* q = row + static_cast<size_t>(x) * 4;
            sum += (q[2] * 77 + q[1] * 150 + q[0] * 29) >> 8;
            ++n;
        }
    }
    return n ? sum / n : 0;
}

Input::Input() { newFrameEv = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
Input::~Input() {
    if (newFrameEv) CloseHandle(newFrameEv);
}

std::shared_ptr<VFrame> Input::NewFrame(int fw, int fh) {
    const size_t n = static_cast<size_t>(fw) * fh * 4;
    std::lock_guard<std::mutex> lk(mx);
    for (auto& f : pool)
        if (f.use_count() == 1 && f != frame) {  // only the pool holds it: nobody is reading
            f->px.resize(n);
            f->w = fw;
            f->h = fh;
            return f;
        }
    auto f = std::make_shared<VFrame>();
    f->px.resize(n);
    f->w = fw;
    f->h = fh;
    if (pool.size() < 4) pool.push_back(f);
    return f;
}

void Input::PublishFrame(const std::shared_ptr<VFrame>& f) {
    if (!f || f->w <= 0 || f->h <= 0) return;
    if (gateArm.exchange(false)) {
        gating = true;
        gateStable = 0;
        gateLuma = -1;
        gateStart = GetTickCount64();
    }
    if (gating) {
        const double l = MeanLuma(f->px.data(), f->w, f->h);
        const bool steady = gateLuma >= 0 && std::fabs(l - gateLuma) < 255 * 0.02;
        gateStable = steady ? gateStable + 1 : 0;
        gateLuma = l;
        if (gateStable >= 3 || GetTickCount64() - gateStart > 2000) {
            gating = false;
            LogMsg(L"Источник «" + key.substr(0, 60) + L"»: экспозиция установилась за " +
                   std::to_wstring(GetTickCount64() - gateStart) + L" мс");
        } else {
            return;  // still adjusting: keep showing the previous picture
        }
    }
    {
        std::lock_guard<std::mutex> lk(mx);
        frame = f;
    }
    ++seq;
    status = 2;
    const ULONGLONG now = GetTickCount64();
    lastFrameAt = now;
    if (fpsT0 == 0) fpsT0 = now;
    ++fpsCount;
    if (now - fpsT0 >= 1000) {
        fpsX10 = static_cast<int>(fpsCount * 10000 / (now - fpsT0));
        fpsCount = 0;
        fpsT0 = now;
    }
    if (newFrameEv) SetEvent(newFrameEv);
}

void Input::Publish(const uint8_t* bgra, int fw, int fh) {
    auto f = NewFrame(fw, fh);
    std::memcpy(f->px.data(), bgra, static_cast<size_t>(fw) * fh * 4);
    PublishFrame(f);
}

std::shared_ptr<VFrame> Input::Latest() {
    std::lock_guard<std::mutex> lk(mx);
    return frame;
}

int Input::FpsX10() const {
    const ULONGLONG last = lastFrameAt;
    if (!last || GetTickCount64() - last > 2000) return 0;
    return fpsX10;
}

void Input::Clear() {
    {
        std::lock_guard<std::mutex> lk(mx);
        frame.reset();
    }
    ++seq;
}

void Input::Halt() {
    stop = true;
    if (th.joinable()) th.join();
    status = 0;
}

// ---------------------------------------------------------------------------
// Webcam picture controls: exposure, brightness, white balance (remembered per device in [CamCtl <hash>])
// ---------------------------------------------------------------------------
namespace {
std::mutex g_ccMx;
struct CamCtlRt {
    int ver = 0, applied = -1;
    bool loaded = false;  // cfg read from settings.ini (on the UI thread only)
    CamCtlCfg cfg;
    CamCtlInfo info;
};
std::map<std::wstring, CamCtlRt> g_cc;  // by device link

std::wstring CamCtlSec(const std::wstring& link) {
    unsigned long long h = 1469598103934665603ull;
    for (wchar_t c : link) h = (h ^ static_cast<unsigned long long>(std::towlower(c))) * 1099511628211ull;
    wchar_t b[40];
    swprintf(b, 40, L"CamCtl %016llx", h);
    return b;
}

void ReadRange(IAMCameraControl* cc, IAMVideoProcAmp* va, bool camCtl, long prop, CamCtlRange* r) {
    long lo = 0, hi = 0, step = 0, def = 0, flags = 0, cur = 0;
    HRESULT hr = camCtl ? (cc ? cc->GetRange(prop, &lo, &hi, &step, &def, &flags) : E_FAIL)
                        : (va ? va->GetRange(prop, &lo, &hi, &step, &def, &flags) : E_FAIL);
    if (FAILED(hr) || hi <= lo) {
        r->has = false;
        return;
    }
    r->has = true;
    r->lo = lo;
    r->hi = hi;
    r->step = step > 0 ? step : 1;
    r->def = def;
    if (SUCCEEDED(camCtl ? cc->Get(prop, &cur, &flags) : va->Get(prop, &cur, &flags))) r->cur = cur;
    else r->cur = def;
}

void ApplyCtl(const std::wstring& link, IMFMediaSource* src) {
    CamCtlCfg c;
    {
        std::lock_guard<std::mutex> lk(g_ccMx);
        c = g_cc[link].cfg;  // the webcam thread never touches settings.ini itself
    }
    IAMCameraControl* cc = nullptr;
    IAMVideoProcAmp* va = nullptr;
    src->QueryInterface(IID_PPV_ARGS(&cc));
    src->QueryInterface(IID_PPV_ARGS(&va));
    CamCtlInfo info;
    info.open = true;
    ReadRange(cc, va, true, CameraControl_Exposure, &info.exp);
    ReadRange(cc, va, false, VideoProcAmp_Brightness, &info.br);
    ReadRange(cc, va, false, VideoProcAmp_WhiteBalance, &info.wb);
    auto clampTo = [](const CamCtlRange& r, int v) { return static_cast<long>(std::clamp<long>(v, r.lo, r.hi)); };
    if (cc && info.exp.has) {
        if (c.expMode == 1) cc->Set(CameraControl_Exposure, clampTo(info.exp, c.expVal), CameraControl_Flags_Manual);
        else cc->Set(CameraControl_Exposure, info.exp.cur, CameraControl_Flags_Auto);
    }
    if (va && info.br.has) {
        if (c.brMode == 1) va->Set(VideoProcAmp_Brightness, clampTo(info.br, c.brVal), VideoProcAmp_Flags_Manual);
        else va->Set(VideoProcAmp_Brightness, info.br.def, VideoProcAmp_Flags_Manual);
    }
    if (va && info.wb.has) {
        if (c.wbMode == 1) va->Set(VideoProcAmp_WhiteBalance, clampTo(info.wb, c.wbVal), VideoProcAmp_Flags_Manual);
        else va->Set(VideoProcAmp_WhiteBalance, info.wb.cur, VideoProcAmp_Flags_Auto);
    }
    // the values now (auto settings report what the camera chose)
    ReadRange(cc, va, true, CameraControl_Exposure, &info.exp);
    ReadRange(cc, va, false, VideoProcAmp_Brightness, &info.br);
    ReadRange(cc, va, false, VideoProcAmp_WhiteBalance, &info.wb);
    SafeRelease(cc);
    SafeRelease(va);
    std::lock_guard<std::mutex> lk(g_ccMx);
    g_cc[link].info = info;
}
}  // namespace

CamCtlCfg CamCtlGet(const std::wstring& link) {  // UI thread
    std::lock_guard<std::mutex> lk(g_ccMx);
    CamCtlRt& rt = g_cc[link];
    if (!rt.loaded) {
        const std::wstring sec = CamCtlSec(link);
        CamCtlCfg& c = rt.cfg;
        c.expMode = g_ini.Int(sec, L"ExpMode", 0);
        c.expVal = g_ini.Int(sec, L"ExpVal", 0);
        c.brMode = g_ini.Int(sec, L"BrMode", 0);
        c.brVal = g_ini.Int(sec, L"BrVal", 0);
        c.wbMode = g_ini.Int(sec, L"WbMode", 0);
        c.wbVal = g_ini.Int(sec, L"WbVal", 0);
        c.mode = g_ini.Str(sec, L"Mode");
        c.rgb32 = g_ini.Int(sec, L"Rgb32", 0) != 0;
        rt.loaded = true;
    }
    return rt.cfg;
}

void CamCtlSet(const std::wstring& link, const CamCtlCfg& c) {  // UI thread
    std::lock_guard<std::mutex> lk(g_ccMx);
    g_cc[link].cfg = c;
    g_cc[link].loaded = true;
    const std::wstring sec = CamCtlSec(link);
    g_ini.SetInt(sec, L"ExpMode", c.expMode);
    g_ini.SetInt(sec, L"ExpVal", c.expVal);
    g_ini.SetInt(sec, L"BrMode", c.brMode);
    g_ini.SetInt(sec, L"BrVal", c.brVal);
    g_ini.SetInt(sec, L"WbMode", c.wbMode);
    g_ini.SetInt(sec, L"WbVal", c.wbVal);
    g_ini.Set(sec, L"Mode", c.mode);
    g_ini.SetInt(sec, L"Rgb32", c.rgb32);
    ++g_cc[link].ver;
    g_projDirty = true;
}

CamCtlInfo CamCtlInfoFor(const std::wstring& link) {
    std::lock_guard<std::mutex> lk(g_ccMx);
    auto it = g_cc.find(link);
    return it == g_cc.end() ? CamCtlInfo() : it->second.info;
}

// webcam thread
static void CamCtlOpened(const std::wstring& link, IMFMediaSource* src) {
    int v;
    {
        std::lock_guard<std::mutex> lk(g_ccMx);
        v = g_cc[link].ver;
    }
    ApplyCtl(link, src);
    std::lock_guard<std::mutex> lk(g_ccMx);
    g_cc[link].applied = v;
}

static void CamCtlPoll(const std::wstring& link, IMFMediaSource* src) {
    int v, a;
    {
        std::lock_guard<std::mutex> lk(g_ccMx);
        v = g_cc[link].ver;
        a = g_cc[link].applied;
    }
    if (v == a) return;
    ApplyCtl(link, src);
    std::lock_guard<std::mutex> lk(g_ccMx);
    g_cc[link].applied = v;
}

static void CamCtlClosed(const std::wstring& link) {
    std::lock_guard<std::mutex> lk(g_ccMx);
    g_cc[link].info.open = false;
    g_cc[link].applied = -1;
}

// ---------------------------------------------------------------------------
// Webcam modes (native formats), cached per device when it is opened
// ---------------------------------------------------------------------------
static std::mutex g_modesMx;
static std::map<std::wstring, std::vector<CamMode>> g_modes;

static std::wstring FourccOf(const GUID& g) {
    if (g == MFVideoFormat_RGB32) return L"RGB32";
    if (g == MFVideoFormat_RGB24) return L"RGB24";
    // FOURCC subtypes: Data1 holds the four characters
    std::wstring t;
    for (int i = 0; i < 4; ++i) {
        const wchar_t c = static_cast<wchar_t>((g.Data1 >> (8 * i)) & 0xFF);
        if (c >= 32 && c < 127) t += c;
    }
    if (t == L"YUY2" || t == L"MJPG" || t == L"NV12" || t == L"H264" || t == L"I420" || t == L"YV12" || t == L"UYVY" ||
        t == L"HEVC" || t == L"Y800" || t == L"L8")
        return t;
    return t.empty() ? L"?" : t;
}

std::wstring CamMode::Key() const {
    wchar_t b[80];
    swprintf(b, 80, L"%ux%u@%.2f/%ls", w, h, Fps(), fourcc.c_str());
    return b;
}

std::wstring CamMode::Label() const {
    wchar_t b[100];
    const double f = Fps();
    if (std::fabs(f - std::round(f)) < 0.05)
        swprintf(b, 100, L"%u×%u, %d к/с, %ls", w, h, static_cast<int>(std::lround(f)), fourcc.c_str());
    else
        swprintf(b, 100, L"%u×%u, %.2f к/с, %ls", w, h, f, fourcc.c_str());
    return b;
}

std::vector<CamMode> CamModesFor(const std::wstring& link) {
    std::lock_guard<std::mutex> lk(g_modesMx);
    auto it = g_modes.find(link);
    return it == g_modes.end() ? std::vector<CamMode>() : it->second;
}

static void CamWanted(const std::wstring& link, std::wstring* mode, bool* rgb) {
    std::lock_guard<std::mutex> lk(g_ccMx);
    const CamCtlRt& rt = g_cc[link];
    *mode = rt.cfg.mode;
    *rgb = rt.cfg.rgb32;
}

// ---------------------------------------------------------------------------
// Webcam: native format with the needed frame rate, YUV converted by WVC (parallel) straight into a shared frame
// ---------------------------------------------------------------------------
namespace {
const GUID kLowLatencyAttr = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

struct WebcamInput : Input {
    std::wstring link;
    void Start() override {
        stop = false;
        status = 1;
        if (g_hideGlare) gateArm = true;
        th = std::thread([this] { Run(); });
    }
    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (int round = 0; !stop; ++round) {
            const int r = Session(round);  // 0 stopped / failed, 2 = open again with another format
            if (r != 2) break;
        }
        if (!stop) {
            failedAt = GetTickCount64();
            status = 3;
        }
        CoUninitialize();
    }

    // automatic choice: frame rate first (at least the camera's FPS), then size near the output height, then
    // formats that are cheap to convert (MJPG / NV12 before YUY2 at HD sizes; others last)
    static double Score(const CamMode& m, int wantFps, int targetH) {
        const double f = m.Fps();
        double s = 0;
        if (f + 0.5 < wantFps) s += (wantFps - f) * 1e6;
        else if (f > wantFps * 1.3) s += 2e3;  // faster than the output: wasted work, slightly worse
        if (m.h > 1080 || m.w > 1920) s += 5e5;
        s += std::fabs(static_cast<double>(m.h) - targetH) * 10;
        if (m.fourcc == L"YUY2" && m.h >= 720) s += 60;
        else if (m.fourcc != L"MJPG" && m.fourcc != L"NV12" && m.fourcc != L"YUY2") s += 3e5;
        return s;
    }

    int Session(int round) {
        IMFAttributes* a = nullptr;
        IMFMediaSource* src = nullptr;
        IMFAttributes* ra = nullptr;
        IMFSourceReader* rd = nullptr;
        IMFMediaType* outType = nullptr;
        IMFMediaType* cur = nullptr;
        const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        bool ok = false;
        UINT32 fw = 0, fh = 0;
        LONG defStride = 0;
        GUID outSub = GUID_NULL;
        bool bt709 = false, full = false;
        std::wstring wantMode, openedMode;
        bool wantRgb = false;
        CamWanted(link, &wantMode, &wantRgb);
        CamMode chosen;
        int result = 0;
        const int autoFps = g_camFps > 0 ? g_camFps : 30;
        do {
            if (FAILED(MFCreateAttributes(&a, 2))) break;
            a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
            a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
            if (FAILED(MFCreateDeviceSource(a, &src))) {
                if (round == 0) LogErr(L"Веб-камера не открылась: " + link.substr(0, 80));
                break;
            }
            CamCtlOpened(link, src);
            if (FAILED(MFCreateAttributes(&ra, 3))) break;
            ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            ra->SetUINT32(kLowLatencyAttr, TRUE);
            if (FAILED(MFCreateSourceReaderFromMediaSource(src, ra, &rd))) break;
            // native formats
            std::vector<CamMode> modes;
            std::vector<DWORD> idx;
            for (DWORD i = 0;; ++i) {
                IMFMediaType* t = nullptr;
                if (FAILED(rd->GetNativeMediaType(stream, i, &t))) break;
                CamMode m;
                GUID sub{};
                t->GetGUID(MF_MT_SUBTYPE, &sub);
                MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &m.w, &m.h);
                MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &m.num, &m.den);
                UINT32 mn = 0, md = 1;
                if (SUCCEEDED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE_RANGE_MAX, &mn, &md)) && md &&
                    static_cast<double>(mn) / md > m.Fps()) {
                    m.num = mn;
                    m.den = md;
                }
                m.fourcc = FourccOf(sub);
                t->Release();
                if (!m.w || !m.h) continue;
                modes.push_back(m);
                idx.push_back(i);
            }
            {
                // the list for «Свойства» (unique by key, sorted: bigger first, then faster)
                std::vector<CamMode> list;
                for (const auto& m : modes) {
                    bool dup = false;
                    for (const auto& o : list) dup = dup || o.Key() == m.Key();
                    if (!dup) list.push_back(m);
                }
                std::sort(list.begin(), list.end(), [](const CamMode& x, const CamMode& y) {
                    if (x.w * x.h != y.w * y.h) return x.w * x.h > y.w * y.h;
                    if (std::fabs(x.Fps() - y.Fps()) > 0.01) return x.Fps() > y.Fps();
                    return x.fourcc < y.fourcc;
                });
                std::lock_guard<std::mutex> lk(g_modesMx);
                g_modes[link] = list;
            }
            int best = -1;
            if (!wantMode.empty())
                for (size_t i = 0; i < modes.size() && best < 0; ++i)
                    if (modes[i].Key() == wantMode) best = static_cast<int>(i);
            if (best < 0) {
                const int wantFps = autoFps;
                const int targetH = std::clamp(g_camH > 0 ? (std::min)(g_camH, g_camW) : 1080, 480, 1080);
                double bs = 1e300;
                for (size_t i = 0; i < modes.size(); ++i) {
                    const double sc = Score(modes[i], wantFps, targetH);
                    if (sc < bs) {
                        bs = sc;
                        best = static_cast<int>(i);
                    }
                }
                if (!wantMode.empty() && round == 0) LogMsg(L"Веб-камера: выбранного режима " + wantMode + L" больше нет — выбираю сам");
            }
            if (best >= 0) {
                chosen = modes[static_cast<size_t>(best)];
                IMFMediaType* t = nullptr;
                if (SUCCEEDED(rd->GetNativeMediaType(stream, idx[static_cast<size_t>(best)], &t))) {
                    if (chosen.num && (chosen.num != 0)) MFSetAttributeRatio(t, MF_MT_FRAME_RATE, chosen.num, chosen.den);
                    if (FAILED(rd->SetCurrentMediaType(stream, nullptr, t))) {
                        LogErr(L"Веб-камера не приняла режим " + chosen.Label());
                    }
                    t->Release();
                }
            }
            // output: the native YUV as is (no conversion by Media Foundation) or NV12 from MJPG; RGB32 = old way
            std::vector<GUID> outs;
            if (wantRgb) outs = {MFVideoFormat_RGB32};
            else if (chosen.fourcc == L"YUY2") outs = {MFVideoFormat_YUY2, MFVideoFormat_NV12, MFVideoFormat_RGB32};
            else outs = {MFVideoFormat_NV12, MFVideoFormat_YUY2, MFVideoFormat_RGB32};
            if (FAILED(MFCreateMediaType(&outType))) break;
            bool set = false;
            for (const GUID& g : outs) {
                outType->DeleteAllItems();
                outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                outType->SetGUID(MF_MT_SUBTYPE, g);
                if (chosen.w && chosen.h) MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, chosen.w, chosen.h);
                if (SUCCEEDED(rd->SetCurrentMediaType(stream, nullptr, outType))) {
                    set = true;
                    break;
                }
                outType->DeleteItem(MF_MT_FRAME_SIZE);
                if (SUCCEEDED(rd->SetCurrentMediaType(stream, nullptr, outType))) {
                    set = true;
                    break;
                }
            }
            if (!set) break;
            if (FAILED(rd->GetCurrentMediaType(stream, &cur))) break;
            ok = ReadType(cur, &fw, &fh, &defStride, &outSub, &bt709, &full);
        } while (false);
        if (ok) {
            openedMode = wantMode;
            std::wstring outName = outSub == MFVideoFormat_NV12 ? L"NV12" : outSub == MFVideoFormat_YUY2 ? L"YUY2" : L"RGB32";
            {
                std::lock_guard<std::mutex> lk(mx);
                info = chosen.Label() + (chosen.fourcc == outName ? L"" : L" → " + outName) + (wantMode.empty() ? L" (авто)" : L"");
            }
            LogMsg(L"Веб-камера открыта: " + chosen.Label() + L" → " + outName + L", кадр " + std::to_wstring(fw) + L"×" +
                   std::to_wstring(fh) + (wantMode.empty() ? L" (режим выбран автоматически)" : L" (режим из свойств)"));
            ULONGLONG lastPoll = GetTickCount64();
            while (!stop) {
                const ULONGLONG now = GetTickCount64();
                if (now - lastPoll >= 250) {
                    lastPoll = now;
                    CamCtlPoll(link, src);
                    std::wstring m;
                    bool rgb;
                    CamWanted(link, &m, &rgb);
                    if (m != openedMode || rgb != wantRgb || (m.empty() && g_camFps > 0 && g_camFps != autoFps)) {
                        LogMsg(L"Веб-камера: меняю режим");
                        result = 2;
                        break;
                    }
                }
                DWORD si = 0, flags = 0;
                LONGLONG ts = 0;
                IMFSample* sample = nullptr;
                HRESULT hr = rd->ReadSample(stream, 0, &si, &flags, &ts, &sample);
                if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                    SafeRelease(sample);
                    LogErr(L"Веб-камера перестала отдавать кадры");
                    break;
                }
                if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                    SafeRelease(cur);
                    if (SUCCEEDED(rd->GetCurrentMediaType(stream, &cur))) ReadType(cur, &fw, &fh, &defStride, &outSub, &bt709, &full);
                }
                if (!sample) continue;
                Convert(sample, fw, fh, defStride, outSub, bt709, full);
                sample->Release();
            }
        }
        SafeRelease(cur);
        SafeRelease(outType);
        SafeRelease(rd);
        SafeRelease(ra);
        CamCtlClosed(link);
        if (src) src->Shutdown();
        SafeRelease(src);
        SafeRelease(a);
        return result;
    }

    static bool ReadType(IMFMediaType* t, UINT32* fw, UINT32* fh, LONG* stride, GUID* sub, bool* bt709, bool* full) {
        *fw = *fh = 0;
        MFGetAttributeSize(t, MF_MT_FRAME_SIZE, fw, fh);
        t->GetGUID(MF_MT_SUBTYPE, sub);
        UINT32 st = 0;
        *stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? static_cast<LONG>(st) : 0;
        // like the Media Foundation video processor: unknown matrix = BT.709 for HD, unknown range = 16..235
        UINT32 mtx = 0, rng = 0;
        if (SUCCEEDED(t->GetUINT32(MF_MT_YUV_MATRIX, &mtx)) && mtx != MFVideoTransferMatrix_Unknown)
            *bt709 = mtx == MFVideoTransferMatrix_BT709;
        else
            *bt709 = *fh >= 720;
        *full = SUCCEEDED(t->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &rng)) && rng == MFNominalRange_0_255;
        return *fw > 0 && *fh > 0;
    }

    void Convert(IMFSample* sample, UINT32 fw, UINT32 fh, LONG defStride, const GUID& sub, bool bt709, bool full) {
        IMFMediaBuffer* mb = nullptr;
        DWORD nb = 0;
        if (SUCCEEDED(sample->GetBufferCount(&nb)) && nb == 1) sample->GetBufferByIndex(0, &mb);  // no copy
        if (!mb && FAILED(sample->ConvertToContiguousBuffer(&mb))) return;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        IMF2DBuffer* b2 = nullptr;
        BYTE* raw = nullptr;
        DWORD len = 0;
        bool locked2d = false, locked = false;
        const bool nv12 = sub == MFVideoFormat_NV12, yuy2 = sub == MFVideoFormat_YUY2;
        const LONG minPitch = static_cast<LONG>(nv12 ? fw : yuy2 ? fw * 2 : fw * 4);
        if (SUCCEEDED(mb->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
            locked2d = true;
        } else if (SUCCEEDED(mb->Lock(&raw, nullptr, &len))) {
            locked = true;
            pitch = defStride ? defStride : minPitch;
            const size_t need = static_cast<size_t>(std::labs(pitch)) * fh * (nv12 ? 3 : 2) / 2;
            scan0 = pitch < 0 ? raw + static_cast<size_t>(fh - 1) * static_cast<size_t>(-pitch) : raw;
            if (len < (nv12 || yuy2 ? need : static_cast<size_t>(std::labs(pitch)) * fh)) scan0 = nullptr;
        }
        if (scan0 && (nv12 || yuy2 || sub == MFVideoFormat_RGB32) && std::labs(pitch) >= minPitch) {
            auto f = NewFrame(static_cast<int>(fw), static_cast<int>(fh));
            uint8_t* o = f->px.data();
            const int ow = static_cast<int>(fw) * 4;
            if (nv12)
                Nv12ToBgraFast(scan0, pitch, scan0 + static_cast<ptrdiff_t>(pitch) * fh, pitch, static_cast<int>(fw) & ~1,
                               static_cast<int>(fh), o, ow, bt709, full);
            else if (yuy2)
                Yuy2ToBgra(scan0, pitch, static_cast<int>(fw), static_cast<int>(fh), o, ow, bt709, full);
            else
                CopyBgrxOpaque(scan0, pitch, static_cast<int>(fw), static_cast<int>(fh), o, ow);
            if (nv12 && (fw & 1)) {
                for (UINT32 y = 0; y < fh; ++y) std::memcpy(o + y * ow + (fw - 1) * 4, o + y * ow + (fw - 2) * 4, 4);
            }
            PublishFrame(f);
        }
        if (locked2d) b2->Unlock2D();
        if (locked) mb->Unlock();
        SafeRelease(b2);
        mb->Release();
    }
};

// ---------------------------------------------------------------------------
// Media files player: pictures, animated GIF, video (no sound)
// ---------------------------------------------------------------------------
UINT PropUInt(IWICMetadataQueryReader* q, const wchar_t* name, UINT def) {
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

struct MediaInput : Input {
    std::vector<std::wstring> files;
    int seconds = 5;
    std::atomic<bool> skip{false};
    std::atomic<int> index{0};
    std::mutex nameMx;
    std::wstring now;

    void Start() override {
        stop = false;
        status = 1;
        th = std::thread([this] { Run(); });
    }
    bool Wait(ULONGLONG ms) {
        const ULONGLONG end = GetTickCount64() + ms;
        while (GetTickCount64() < end) {
            if (stop || skip) return false;
            Sleep(15);
        }
        return !(stop || skip);
    }
    void PublishOpaque(const uint8_t* px, int fw, int fh) {
        std::vector<uint8_t> t(px, px + static_cast<size_t>(fw) * fh * 4);
        for (size_t i = 3; i < t.size(); i += 4) t[i] = 255;
        Publish(t.data(), fw, fh);
    }
    bool PlayImage(const std::wstring& f) {
        std::vector<uint8_t> px;
        int fw = 0, fh = 0;
        if (!LoadImageScaled(f, 1920, 1920, GUID_WICPixelFormat32bppBGR, px, &fw, &fh)) return false;
        PublishOpaque(px.data(), fw, fh);
        Wait(static_cast<ULONGLONG>(seconds) * 1000);
        return true;
    }
    bool PlayGif(const std::wstring& f) {
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
            while (n > 0 && !stop && !skip) {
                for (UINT i = 0; i < n && !stop && !skip; ++i) {
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
                        PublishOpaque(canvas.data(), W, H);
                        ok = true;
                        if (disposal == 2) {
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
                    Wait(delay);
                }
                if (!ok || n == 1 || GetTickCount64() >= until) break;
            }
            if (n == 1 && ok) Wait(static_cast<ULONGLONG>(seconds) * 1000);
        }
        SafeRelease(dec);
        SafeRelease(fac);
        return ok;
    }
    bool PlayVideo(const std::wstring& f) {
        IMFAttributes* ra = nullptr;
        IMFSourceReader* rd = nullptr;
        IMFMediaType* outType = nullptr;
        IMFMediaType* cur = nullptr;
        const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        bool ok = false;
        UINT32 fw = 0, fh = 0;
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
            MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &fw, &fh);
            UINT32 st = 0;
            if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) defStride = static_cast<LONG>(st);
            ok = fw && fh;
        } while (false);
        bool played = false;
        if (ok) {
            std::vector<uint8_t> local(static_cast<size_t>(fw) * fh * 4);
            const ULONGLONG t0 = GetTickCount64();
            LONGLONG firstTs = -1;
            while (!stop && !skip) {
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
                        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &fw, &fh);
                        UINT32 st = 0;
                        defStride = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? static_cast<LONG>(st) : 0;
                        local.assign(static_cast<size_t>(fw) * fh * 4, 0);
                    }
                }
                if (!sample) continue;
                if (firstTs < 0) firstTs = ts;
                const LONGLONG dueMs = (ts - firstTs) / 10000;
                const LONGLONG nowMs = static_cast<LONGLONG>(GetTickCount64() - t0);
                if (dueMs > nowMs && !Wait(static_cast<ULONGLONG>(dueMs - nowMs))) {
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
                        pitch = defStride ? defStride : static_cast<LONG>(fw * 4);
                        scan0 = pitch < 0 ? raw + static_cast<size_t>(fh - 1) * static_cast<size_t>(-pitch) : raw;
                        if (len < static_cast<size_t>(std::labs(pitch)) * fh) scan0 = nullptr;
                    }
                    if (scan0) {
                        for (UINT32 y = 0; y < fh; ++y)
                            std::memcpy(local.data() + static_cast<size_t>(y) * fw * 4,
                                        scan0 + static_cast<ptrdiff_t>(y) * pitch, static_cast<size_t>(fw) * 4);
                        PublishOpaque(local.data(), static_cast<int>(fw), static_cast<int>(fh));
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
    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        int failedInRow = 0;
        while (!stop && !files.empty()) {
            const int n = static_cast<int>(files.size());
            const int idx = ((index % n) + n) % n;
            const std::wstring& f = files[static_cast<size_t>(idx)];
            {
                std::lock_guard<std::mutex> lk(nameMx);
                now = FileNameOf(f);
            }
            skip = false;
            bool ok;
            if (IsVideoFile(f)) ok = PlayVideo(f);
            else if (LowerExt(f) == L"gif") ok = PlayGif(f);
            else ok = PlayImage(f);
            if (stop) break;
            if (ok) {
                failedInRow = 0;
            } else if (++failedInRow >= n) {
                status = 3;
                failedInRow = 0;
                Wait(2000);
            }
            skip = false;
            index = idx + 1;
        }
        CoUninitialize();
    }
};

// ---------------------------------------------------------------------------
// Phone
// ---------------------------------------------------------------------------
const int kPhoneTcpPort = 47800;
const u_short kPhoneDiscPort = 47801;
const wchar_t* kPhoneActivity = L"com.wvc.phone/.MainActivity";

std::mutex g_adbMx;
std::wstring g_adbExe;
std::vector<AdbDev> g_usbDevs;
std::atomic<bool> g_adbStartedServer{false};
std::atomic<bool> g_usbListing{false};
std::map<std::wstring, int> g_adbForwards;  // serial -> local port WVC made
std::atomic<bool> g_phoneSearching{false};
std::mutex g_foundMx;
std::vector<PhoneFound> g_found;
std::atomic<int> g_h264Avail{-1};  // -1 unknown, 0 no decoder, 1 ok

std::string PhoneA(const std::wstring& w) { return ToUtf8(w); }
std::wstring PhoneW(const std::string& a) { return FromUtf8(a); }

bool RecvAll(SOCKET s, void* buf, int n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        const int r = recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

SOCKET TcpConnect(const std::wstring& ip, int port, std::wstring* err, const std::atomic<bool>& stop) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(PhoneA(ip).c_str(), portText.c_str(), &hints, &res) != 0 || !res) {
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
    for (int i = 0; i < 12 && !stop; ++i) {
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
    DWORD to = 7000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    int rb = 2 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rb), sizeof(rb));
    return s;
}

bool DecodeJpeg(IWICImagingFactory* f, const uint8_t* data, size_t n, int rot, std::vector<uint8_t>& out, int* ow,
                int* oh) {
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

// BGRA rotation, clockwise by rot * 90
void RotateBgra(const std::vector<uint8_t>& in, int w, int h, int rot, std::vector<uint8_t>& out, int* ow, int* oh) {
    rot &= 3;
    if (!rot) {
        out = in;
        *ow = w;
        *oh = h;
        return;
    }
    const int W = (rot & 1) ? h : w, H = (rot & 1) ? w : h;
    out.resize(static_cast<size_t>(W) * H * 4);
    const uint32_t* s = reinterpret_cast<const uint32_t*>(in.data());
    uint32_t* d = reinterpret_cast<uint32_t*>(out.data());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int sx, sy;
            if (rot == 1) {
                sx = y;
                sy = h - 1 - x;
            } else if (rot == 2) {
                sx = w - 1 - x;
                sy = h - 1 - y;
            } else {
                sx = w - 1 - y;
                sy = x;
            }
            d[static_cast<size_t>(y) * W + x] = s[static_cast<size_t>(sy) * w + sx];
        }
    *ow = W;
    *oh = H;
}

// ---- adb
std::wstring AdbExe() {
    std::lock_guard<std::mutex> lk(g_adbMx);
    if (g_adbExe.empty()) {
        const std::wstring dir = ExeDir();
        for (const wchar_t* rel : {L"\\adb\\adb.exe", L"\\adb.exe"}) {
            const std::wstring p = dir + rel;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_adbExe = p;
                break;
            }
        }
        if (g_adbExe.empty()) {
            wchar_t buf[MAX_PATH] = {};
            if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, buf, nullptr)) g_adbExe = buf;
        }
    }
    return g_adbExe;
}

std::wstring AdbSerialArg(const std::wstring& serial) {
    return serial.empty() ? std::wstring() : L"-s \"" + serial + L"\" ";
}

int AdbRun(const std::wstring& args, std::string* out, DWORD timeoutMs, const std::atomic<bool>* stop) {
    if (out) out->clear();
    const std::wstring exe = AdbExe();
    if (exe.empty()) return -1;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 64 * 1024)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;
    HANDLE inherit[2] = {wr, nul};
    const DWORD nInherit = nul ? 2 : 1;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize ? attrSize : 64);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    bool haveAttrs = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                     UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                               nInherit * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    if (haveAttrs) si.lpAttributeList = attrs;
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    const size_t slash = exe.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | (haveAttrs ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                                   dir.empty() ? nullptr : dir.c_str(), reinterpret_cast<STARTUPINFOW*>(&si), &pi);
    if (haveAttrs) DeleteProcThreadAttributeList(attrs);
    CloseHandle(wr);
    if (nul) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    std::string buf;
    auto drain = [&] {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            char tmp[4096];
            DWORD got = 0;
            if (!ReadFile(rd, tmp, (std::min)(avail, static_cast<DWORD>(sizeof(tmp))), &got, nullptr) || !got) break;
            if (buf.size() < (1u << 20)) buf.append(tmp, got);
        }
    };
    const ULONGLONG t0 = GetTickCount64();
    int code = -2;
    for (;;) {
        drain();
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            drain();
            DWORD ec = 0;
            GetExitCodeProcess(pi.hProcess, &ec);
            code = static_cast<int>(ec);
            break;
        }
        if ((stop && stop->load()) || GetTickCount64() - t0 > timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (buf.find("daemon started successfully") != std::string::npos) g_adbStartedServer = true;
    if (out) out->swap(buf);
    return code;
}

std::string AdbTrim(std::string t) {
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ' ' || t.back() == '\t')) t.pop_back();
    size_t b = 0;
    while (b < t.size() && (t[b] == ' ' || t[b] == '\t' || t[b] == '\r' || t[b] == '\n')) ++b;
    return t.substr(b);
}

std::vector<std::string> AdbLines(const std::string& out) {
    std::vector<std::string> v;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t e = out.find('\n', pos);
        if (e == std::string::npos) e = out.size();
        std::string line = AdbTrim(out.substr(pos, e - pos));
        if (!line.empty()) v.push_back(line);
        pos = e + 1;
    }
    return v;
}

std::vector<AdbDev> AdbDevices(const std::atomic<bool>* stop, int* rc) {
    std::string out;
    *rc = AdbRun(L"devices -l", &out, 15000, stop);
    std::vector<AdbDev> v;
    if (*rc != 0) return v;
    for (const std::string& line : AdbLines(out)) {
        if (line[0] == '*' || line.rfind("List of devices", 0) == 0 || line.rfind("adb ", 0) == 0) continue;
        const size_t a = line.find_first_of(" \t");
        if (a == std::string::npos) continue;
        const size_t b = line.find_first_not_of(" \t", a);
        if (b == std::string::npos) continue;
        const size_t c = line.find_first_of(" \t", b);
        std::string state = line.substr(b, c == std::string::npos ? std::string::npos : c - b);
        if (state == "no") state = "no permissions";
        AdbDev d;
        d.serial = PhoneW(line.substr(0, a));
        d.state = PhoneW(state);
        const size_t m = line.find(" model:");
        if (m != std::string::npos) {
            const size_t me = line.find_first_of(" \t", m + 7);
            std::string model = line.substr(m + 7, me == std::string::npos ? std::string::npos : me - m - 7);
            std::replace(model.begin(), model.end(), '_', ' ');
            d.model = PhoneW(model);
        }
        v.push_back(d);
    }
    return v;
}

std::wstring AdbModel(const std::wstring& serial, const std::atomic<bool>* stop) {
    std::string out;
    if (AdbRun(AdbSerialArg(serial) + L"shell getprop ro.product.model", &out, 5000, stop) != 0) return std::wstring();
    return PhoneW(AdbTrim(out));
}

bool LocalPortFree(int port) {
    SOCKET t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (t == INVALID_SOCKET) return false;
    BOOL ex = TRUE;
    setsockopt(t, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&ex), sizeof(ex));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = bind(t, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    closesocket(t);
    return ok;
}

void AdbRemoveForward(const std::wstring& serial, bool async) {
    int port = 0;
    {
        std::lock_guard<std::mutex> lk(g_adbMx);
        auto it = g_adbForwards.find(serial);
        if (it == g_adbForwards.end()) return;
        port = it->second;
        g_adbForwards.erase(it);
    }
    auto fn = [serial, port] {
        AdbRun(AdbSerialArg(serial) + L"forward --remove tcp:" + std::to_wstring(port), nullptr, 3000, nullptr);
    };
    if (async)
        std::thread(fn).detach();
    else
        fn();
}

struct PhoneInput;
std::map<std::wstring, int> g_phoneUser;  // key -> 1 connect / -1 disconnect
std::mutex g_phoneUserMx;

struct PhoneInput : Input {
    std::wstring link;  // IP, or "usb:<preferred serial>"
    // what the phone should send (set from the UI thread)
    std::atomic<int> wantSrc{0}, wantRes{0}, wantCodec{0}, wantFps{30};
    std::atomic<int> sentSrc{-1}, sentRes{-1}, sentFps{-1};
    std::mutex pmx;  // socket + strings
    SOCKET sock = INVALID_SOCKET;
    std::wstring name, state, err, usbModel, usbSerial, codec;
    std::atomic<int> recvFpsX10{0}, decFpsX10{0}, kbps{0}, resRep{-1};
    std::atomic<bool> forceJpeg{false};
    ULONGLONG launchAt = 0;

    void Start() override {
        stop = false;
        status = 1;
        th = std::thread([this] { Run(); });
    }
    void Halt() override {
        stop = true;
        {
            std::lock_guard<std::mutex> lk(pmx);
            if (sock != INVALID_SOCKET) {
                closesocket(sock);
                sock = INVALID_SOCKET;
            }
        }
        if (th.joinable()) th.join();
        status = 0;
        recvFpsX10 = decFpsX10 = kbps = 0;
        std::lock_guard<std::mutex> lk(pmx);
        state.clear();
        if (link.rfind(L"usb:", 0) == 0 && !usbSerial.empty()) AdbRemoveForward(usbSerial, true);
    }
    bool SendLine(const std::string& line) {
        std::lock_guard<std::mutex> lk(pmx);
        if (sock == INVALID_SOCKET) return false;
        const std::string l = line + "\n";
        return send(sock, l.data(), static_cast<int>(l.size()), 0) == static_cast<int>(l.size());
    }
    std::string SrcCmd(int src, int codecMode) {
        static const char* names[3] = {"back", "front", "screen"};
        src = std::clamp(src, 0, 2);
        if (codecMode == 2 && src < 2) return std::string("SRC ") + names[src] + "_screen";
        return std::string("SRC ") + names[src];
    }
    void SyncWanted() {  // UI thread: tell the phone what changed
        if (status != 2) return;
        const int s = wantSrc, r = wantRes;
        if (s != sentSrc && SendLine(SrcCmd(s, wantCodec))) {
            sentSrc = s;
            if (s < 2 && g_hideGlare) gateArm = true;  // a camera starts: hide its first bright frames
        }
        if (r != sentRes && SendLine(r == 1 ? "RES 1080" : "RES 720")) sentRes = r;
        const int f = wantFps;
        if (f != sentFps && SendLine("FPS " + std::to_string(f))) sentFps = f;  // WVC Phone 0.13+ (older ones ignore it)
    }
    void HandleText(const std::string& t) {
        if (t.rfind("STATE ", 0) == 0) {
            std::wstring st = PhoneW(t.substr(6));
            const size_t us = st.find(L"_screen");
            if (us != std::wstring::npos) st = st.substr(0, us);  // back_screen -> back
            bool changed;
            {
                std::lock_guard<std::mutex> lk(pmx);
                changed = st != state;
                state = st;
            }
            const int src = st == L"back" ? 0 : st == L"front" ? 1 : st == L"screen" ? 2 : -1;
            if (src >= 0) {  // also when switched on the phone itself: WVC follows it
                sentSrc = src;
                wantSrc = src;
            }
            if (src < 0) Clear();  // no picture now: show the card
            if (changed && g_notify) PostMessageW(g_notify, WM_PHONE_STATE, 0, 0);
        } else if (t.rfind("RES ", 0) == 0) {
            const int r = std::atoi(t.c_str() + 4) >= 1080 ? 1 : 0;
            sentRes = r;
            wantRes = r;
            if (resRep.exchange(r) != r && g_notify) PostMessageW(g_notify, WM_PHONE_STATE, 0, 0);
        } else if (t.rfind("NAME ", 0) == 0) {
            std::lock_guard<std::mutex> lk(pmx);
            name = PhoneW(t.substr(5));
        } else if (t.rfind("FPS ", 0) == 0) {
            std::lock_guard<std::mutex> lk(mx);
            info = L"телефон: " + PhoneW(t.substr(4)) + L" к/с";
        } else if (t.rfind("CODEC ", 0) == 0) {
            std::lock_guard<std::mutex> lk(pmx);
            codec = t.substr(6) == "h264" ? L"H.264" : L"JPEG";
        }
    }
    int UsbPrepare(const std::wstring& prefSerial, std::wstring* model, std::wstring* serialOut, std::wstring* e) {
        const std::atomic<bool>* st = &stop;
        if (AdbExe().empty()) {
            *e = L"Не найден adb.exe: он должен лежать в папке «adb» рядом с WVC.exe — распакуйте WVC.zip целиком.";
            return 0;
        }
        int rc = 0;
        std::vector<AdbDev> devs = AdbDevices(st, &rc);
        if (st->load()) return 0;
        if (rc != 0) {
            *e = rc == -2 ? L"adb не ответил вовремя. Закройте другие программы, которые используют adb (Android Studio, "
                            L"scrcpy), или переподключите кабель."
                          : L"adb не запустился (код " + std::to_wstring(rc) + L"). Переподключите кабель.";
            return 0;
        }
        for (auto& d : devs)
            if (d.state == L"device" && d.model.empty()) d.model = AdbModel(d.serial, st);
        bool changed;
        {
            std::lock_guard<std::mutex> lk(g_adbMx);
            changed = devs.size() != g_usbDevs.size();
            for (size_t i = 0; !changed && i < devs.size(); ++i)
                changed = devs[i].serial != g_usbDevs[i].serial || devs[i].state != g_usbDevs[i].state ||
                          devs[i].model != g_usbDevs[i].model;
            g_usbDevs = devs;
        }
        if (changed && g_notify) PostMessageW(g_notify, WM_PHONE_USBLIST, 0, 0);
        const AdbDev* pick = nullptr;
        for (const auto& d : devs)
            if (d.state == L"device" && d.serial == prefSerial) pick = &d;
        for (const auto& d : devs)
            if (!pick && d.state == L"device" && prefSerial.empty()) pick = &d;
        if (!pick && !prefSerial.empty())
            for (const auto& d : devs)
                if (!pick && d.state == L"device") pick = &d;
        if (!pick) {
            bool unauthorized = false, offline = false, noperm = false;
            for (const auto& d : devs) {
                unauthorized = unauthorized || d.state == L"unauthorized";
                offline = offline || d.state == L"offline";
                noperm = noperm || d.state.rfind(L"no", 0) == 0;
            }
            if (unauthorized)
                *e = L"Подтвердите доступ на телефоне (unauthorized): нажмите на телефоне «Разрешить» в запросе "
                     L"«Разрешить отладку по USB?» (можно отметить «Всегда разрешать с этого компьютера»).";
            else if (offline)
                *e = L"Телефон по USB в состоянии offline: переподключите кабель или выключите и включите Отладку по USB.";
            else if (noperm)
                *e = L"Нет доступа к телефону по USB: переподключите кабель и подтвердите запрос на телефоне.";
            else
                *e = L"Телефон по USB не найден — подключите кабель и включите Отладку по USB.";
            return 0;
        }
        const std::wstring serial = pick->serial;
        *serialOut = serial;
        *model = pick->model.empty() ? std::wstring(L"телефон") : pick->model;
        const std::wstring sa = AdbSerialArg(serial);
        int want = 0;
        std::string out;
        if (AdbRun(L"forward --list", &out, 5000, st) == 0) {
            const std::string ser = PhoneA(serial);
            for (const std::string& line : AdbLines(out)) {
                const std::string tail = " tcp:" + std::to_string(kPhoneTcpPort);
                if (line.rfind(ser + " tcp:", 0) == 0 && line.size() > tail.size() &&
                    line.compare(line.size() - tail.size(), tail.size(), tail) == 0) {
                    want = std::atoi(line.c_str() + ser.size() + 5);
                    break;
                }
            }
        }
        if (st->load()) return 0;
        if (!want) {
            std::lock_guard<std::mutex> lk(g_adbMx);
            auto it = g_adbForwards.find(serial);
            if (it != g_adbForwards.end()) want = it->second;
        }
        if (!want && LocalPortFree(kPhoneTcpPort)) want = kPhoneTcpPort;
        const std::wstring remote = L" tcp:" + std::to_wstring(kPhoneTcpPort);
        int port = 0;
        if (want && AdbRun(sa + L"forward tcp:" + std::to_wstring(want) + remote, &out, 5000, st) == 0) port = want;
        if (!port && !st->load() && AdbRun(sa + L"forward tcp:0" + remote, &out, 5000, st) == 0)
            port = std::atoi(AdbTrim(out).c_str());
        if (st->load()) return 0;
        if (port <= 0 || port > 65535) {
            std::string why = AdbTrim(out);
            if (why.size() > 160) why.resize(160);
            *e = L"adb не смог пробросить порт к телефону" + (why.empty() ? std::wstring() : L" (" + PhoneW(why) + L")") +
                 L". Переподключите кабель.";
            return 0;
        }
        {
            std::lock_guard<std::mutex> lk(g_adbMx);
            g_adbForwards[serial] = port;
        }
        return port;
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool usb = link.rfind(L"usb:", 0) == 0;
        std::wstring e, model, serial;
        SOCKET s = INVALID_SOCKET;
        if (usb) {
            const int port = UsbPrepare(link.substr(4), &model, &serial, &e);
            if (port) {
                {
                    std::lock_guard<std::mutex> lk(pmx);
                    usbModel = model;
                    usbSerial = serial;
                }
                s = TcpConnect(L"127.0.0.1", port, &e, stop);
                if (s == INVALID_SOCKET) e = L"Не удалось подключиться к порту adb (" + e + L"). Переподключите кабель.";
            }
        } else {
            s = TcpConnect(link, kPhoneTcpPort, &e, stop);
        }
        bool gotAny = false;
        if (s != INVALID_SOCKET) {
            std::lock_guard<std::mutex> lk(pmx);
            if (stop) {
                closesocket(s);
                s = INVALID_SOCKET;
            } else {
                sock = s;
                err.clear();
                codec = L"JPEG";
            }
        }
        if (s != INVALID_SOCKET) {
            status = 2;
            LogMsg(L"Телефон подключён: " + link);
            if (g_hideGlare && wantSrc < 2) gateArm = true;
            const int codecMode = wantCodec;
            if (g_h264Avail < 0) {
                H264Decoder probe;
                g_h264Avail = probe.Init() ? 1 : 0;
            }
            const bool tryH264 = codecMode != 1 && g_h264Avail == 1 && !forceJpeg;
            {  // first the codec, so the phone starts in the right mode
                std::string hello;
                if (tryH264) hello += "CODEC h264\n";
                hello += "FPS " + std::to_string(wantFps.load()) + "\n";  // before SRC: the camera starts at that rate
                hello += SrcCmd(wantSrc, codecMode) + "\n";
                hello += wantRes == 1 ? "RES 1080\n" : "RES 720\n";
                std::lock_guard<std::mutex> lk(pmx);
                send(s, hello.data(), static_cast<int>(hello.size()), 0);
            }
            sentSrc = wantSrc.load();
            sentRes = wantRes.load();
            sentFps = wantFps.load();
            // decoding on its own thread: JPEG = newest frame only; H.264 = every packet in order (drop to the next
            // key frame when it falls behind)
            struct Job {
                bool h264 = false, key = false;
                int rot = 0;
                int64_t pts = 0;
                std::vector<uint8_t> data;
            };
            std::mutex dmx;
            std::condition_variable dcv;
            std::deque<Job> hq;
            Job jpegJob;
            bool haveJpeg = false, decStop = false;
            std::atomic<int> decFrames{0};
            std::atomic<bool> needKey{false};
            std::thread decoder([&]() {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                IWICImagingFactory* df = nullptr;
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&df));
                std::unique_ptr<H264Decoder> hd;
                std::vector<uint8_t> px, rotated;
                int badRun = 0;
                for (;;) {
                    Job job;
                    bool more = false;
                    {
                        std::unique_lock<std::mutex> lk(dmx);
                        dcv.wait(lk, [&] { return decStop || haveJpeg || !hq.empty(); });
                        if (decStop) break;
                        if (!hq.empty()) {
                            job = std::move(hq.front());
                            hq.pop_front();
                            more = !hq.empty();
                        } else {
                            job = std::move(jpegJob);
                            haveJpeg = false;
                        }
                    }
                    int fw = 0, fh = 0;
                    if (!job.h264) {
                        if (DecodeJpeg(df, job.data.data(), job.data.size(), job.rot, px, &fw, &fh)) {
                            Publish(px.data(), fw, fh);
                            ++decFrames;
                        }
                        continue;
                    }
                    if (!hd) {
                        hd.reset(new H264Decoder());
                        if (!hd->Init()) {
                            forceJpeg = true;  // reconnect in JPEG
                            std::lock_guard<std::mutex> lk(pmx);
                            if (sock != INVALID_SOCKET) shutdown(sock, SD_BOTH);
                            break;
                        }
                    }
                    const bool pic = hd->Decode(job.data.data(), job.data.size(), job.pts, &px, &fw, &fh, !more);
                    if (pic) {
                        badRun = 0;
                        if (job.rot & 3) {
                            int rw = 0, rh = 0;
                            RotateBgra(px, fw, fh, job.rot, rotated, &rw, &rh);
                            Publish(rotated.data(), rw, rh);
                        } else {
                            Publish(px.data(), fw, fh);
                        }
                        ++decFrames;
                    } else if (hd->errors > 0 && ++badRun > 90) {  // ~3 s of nothing but errors: use JPEG
                        forceJpeg = true;
                        std::lock_guard<std::mutex> lk(pmx);
                        if (sock != INVALID_SOCKET) shutdown(sock, SD_BOTH);
                        break;
                    }
                }
                SafeRelease(df);
                CoUninitialize();
            });
            std::vector<uint8_t> payload;
            int frames = 0;
            uint64_t bytes = 0;
            ULONGLONG t0 = GetTickCount64();
            e = L"связь прервалась";
            while (!stop) {
                uint8_t hdr[8];
                if (!RecvAll(s, hdr, 8)) break;
                gotAny = true;
                const uint32_t len = static_cast<uint32_t>(hdr[4]) | (static_cast<uint32_t>(hdr[5]) << 8) |
                                     (static_cast<uint32_t>(hdr[6]) << 16) | (static_cast<uint32_t>(hdr[7]) << 24);
                bytes += 8 + len;
                if (std::memcmp(hdr, "WVCF", 4) == 0) {
                    uint8_t ext[5];
                    if (len > (16u << 20) || !RecvAll(s, ext, 5)) break;
                    payload.resize(len);
                    if (len && !RecvAll(s, payload.data(), static_cast<int>(len))) break;
                    {
                        std::lock_guard<std::mutex> lk(dmx);
                        jpegJob.h264 = false;
                        jpegJob.rot = ext[4] & 3;
                        jpegJob.data.swap(payload);
                        haveJpeg = true;
                    }
                    dcv.notify_one();
                    ++frames;
                    std::lock_guard<std::mutex> lk(pmx);
                    codec = L"JPEG";
                } else if (std::memcmp(hdr, "WVCH", 4) == 0) {
                    // flags u8 (1 config, 2 key), rot u8, width u16, height u16, pts u64 (µs), Annex-B data
                    uint8_t ext[14];
                    if (len > (16u << 20) || !RecvAll(s, ext, 14)) break;
                    Job j;
                    j.h264 = true;
                    j.key = (ext[0] & 3) != 0;
                    j.rot = ext[1] & 3;
                    uint64_t pts = 0;
                    for (int b = 0; b < 8; ++b) pts |= static_cast<uint64_t>(ext[6 + b]) << (8 * b);
                    j.pts = static_cast<int64_t>(pts) * 10;
                    j.data.resize(len);
                    if (len && !RecvAll(s, j.data.data(), static_cast<int>(len))) break;
                    bool askKey = false;
                    {
                        std::lock_guard<std::mutex> lk(dmx);
                        if (needKey && !j.key) {
                            // dropping until the next key frame
                        } else {
                            needKey = false;
                            if (hq.size() > 8) {  // the decoder is behind (~0.15–0.3 s): drop to the next key frame
                                hq.clear();
                                needKey = !j.key;
                                askKey = !j.key;
                                if (j.key) hq.push_back(std::move(j));
                            } else {
                                hq.push_back(std::move(j));
                            }
                        }
                    }
                    if (askKey) SendLine("KEY");
                    dcv.notify_one();
                    if (!(ext[0] & 1)) ++frames;
                    std::lock_guard<std::mutex> lk(pmx);
                    codec = L"H.264";
                } else if (std::memcmp(hdr, "WVCS", 4) == 0) {
                    if (len > 4096) break;
                    std::string t(len, '\0');
                    if (len && !RecvAll(s, &t[0], static_cast<int>(len))) break;
                    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
                    HandleText(t);
                } else {
                    e = L"неизвестный ответ (это точно WVC Phone?)";
                    break;
                }
                const ULONGLONG now = GetTickCount64();
                if (now - t0 >= 1000) {
                    recvFpsX10 = static_cast<int>(frames * 10000 / (now - t0));
                    decFpsX10 = static_cast<int>(decFrames.exchange(0) * 10000 / (now - t0));
                    kbps = static_cast<int>(bytes * 8 / (now - t0));
                    frames = 0;
                    bytes = 0;
                    t0 = now;
                }
            }
            {
                std::lock_guard<std::mutex> lk(dmx);
                decStop = true;
            }
            dcv.notify_one();
            decoder.join();
        }
        {
            std::lock_guard<std::mutex> lk(pmx);
            if (s != INVALID_SOCKET && sock == s) {
                closesocket(s);
                sock = INVALID_SOCKET;
            }
            state.clear();
        }
        if (usb && !stop && s != INVALID_SOCKET) {
            if (!gotAny) {
                e = L"Телефон «" + model + L"» подключён по USB, но WVC Phone на нём не запущен — открываю приложение. "
                    L"Если не открылось, откройте WVC Phone вручную.";
                if (launchAt == 0 || GetTickCount64() - launchAt > 15000) {
                    launchAt = GetTickCount64();
                    AdbRun(AdbSerialArg(serial) + L"shell am start -n " + kPhoneActivity, nullptr, 8000, &stop);
                }
            } else if (e == L"связь прервалась") {
                e = L"Связь с телефоном по USB прервалась (кабель отключён или WVC Phone закрыт).";
            } else {
                e = L"Ошибка связи по USB: " + e + L".";
            }
        }
        if (forceJpeg && !stop) e = L"H.264 не декодируется на этом компьютере — переключаюсь на JPEG.";
        {
            std::lock_guard<std::mutex> lk(pmx);
            if (!stop) err = e;
        }
        Clear();
        recvFpsX10 = 0;
        decFpsX10 = 0;
        kbps = 0;
        if (!stop) {
            failedAt = GetTickCount64();
            status = 3;
            if (g_notify) PostMessageW(g_notify, WM_PHONE_STATE, 0, 0);
        }
        CoUninitialize();
    }
};

// ---------------------------------------------------------------------------
// Pool
// ---------------------------------------------------------------------------
std::mutex g_inMx;
std::map<std::wstring, std::shared_ptr<Input>> g_inputs;

std::wstring PhoneLinkOf(const Item& it) {
    return it.phoneConn == 1 ? L"usb:" + it.phoneSerial : Trim(it.phoneIp);
}
}  // namespace

std::wstring InputKeyOf(const Item& it) {
    switch (it.type) {
    case IT_WEBCAM: return it.camLink.empty() ? L"" : L"cam|" + it.camLink;
    case IT_PHONE: {
        const std::wstring l = PhoneLinkOf(it);
        return l.empty() ? L"" : L"phone|" + l;
    }
    case IT_MEDIA: {
        if (it.media.empty()) return L"";
        std::wstring k = L"media|" + std::to_wstring(kImageSecs[std::clamp(it.imageSec, 0, 4)]);
        for (const auto& f : it.media) k += L"|" + f;
        return k;
    }
    default: return L"";
    }
}

std::shared_ptr<Input> GetInput(const std::wstring& key) {
    if (key.empty()) return nullptr;
    std::lock_guard<std::mutex> lk(g_inMx);
    auto it = g_inputs.find(key);
    return it == g_inputs.end() ? nullptr : it->second;
}

static std::shared_ptr<Input> CreateInput(const Item& it, const std::wstring& key) {
    std::shared_ptr<Input> in;
    if (it.type == IT_WEBCAM) {
        auto w = std::make_shared<WebcamInput>();
        w->link = it.camLink;
        CamCtlGet(it.camLink);  // load the remembered picture settings before the thread starts
        in = w;
    } else if (it.type == IT_MEDIA) {
        auto m = std::make_shared<MediaInput>();
        m->files = it.media;
        m->seconds = kImageSecs[std::clamp(it.imageSec, 0, 4)];
        in = m;
    } else if (it.type == IT_PHONE) {
        auto p = std::make_shared<PhoneInput>();
        p->link = PhoneLinkOf(it);
        p->wantSrc = it.phoneSrc;
        p->wantRes = it.phoneRes;
        p->wantCodec = it.phoneCodec;
        p->wantFps = g_camFps >= 50 ? 60 : 30;
        in = p;
    }
    if (in) in->key = key;
    return in;
}

int PhoneUserState(const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_phoneUserMx);
    auto it = g_phoneUser.find(key);
    return it == g_phoneUser.end() ? 0 : it->second;
}

void PhoneUserConnect(const std::wstring& key, int v) {
    std::lock_guard<std::mutex> lk(g_phoneUserMx);
    if (v == 0)
        g_phoneUser.erase(key);
    else
        g_phoneUser[key] = v;
}

void InputsSync() {
    if (g_quitting) return;
    Project p = ProjSnapshot();
    const bool live = g_streaming;
    const bool preview = g_uiVisible && p.liveWhenStopped;
    g_hideGlare = p.hideStartGlare;
    struct Want {
        const Item* item;
        int prio;
    };
    std::map<std::wstring, Want> want;
    auto add = [&](const Item& it, int prio) {
        const std::wstring k = InputKeyOf(it);
        if (k.empty()) return;
        auto f = want.find(k);
        if (f == want.end() || f->second.prio > prio) want[k] = Want{&it, prio};
    };
    Item solo;
    const int soloUid = g_soloUid;
    if (soloUid) {
        std::lock_guard<std::mutex> lk(g_soloMx);
        solo = g_soloItem;
    }
    if (soloUid) add(solo, 0);
    const int n = static_cast<int>(p.scenes.size());
    if (n > 0) {
        const Scene& cur = p.scenes[static_cast<size_t>(std::clamp(p.cur, 0, n - 1))];
        if (live || preview)
            for (const auto& it : cur.items)
                if (it.visible) add(it, 1);
        // «Следующий» is opened in advance (also while on air in the tray), so the switch has a picture at once
        if ((preview || live) && p.next >= 0 && p.next < n)
            for (const auto& it : p.scenes[static_cast<size_t>(p.next)].items)
                if (it.visible) add(it, 2);
        if (p.keepCamOn)
            for (const auto& sc : p.scenes)
                for (const auto& it : sc.items)
                    if (it.type == IT_WEBCAM) add(it, 3);
        // phones the user connected by hand
        for (const auto& sc : p.scenes)
            for (const auto& it : sc.items)
                if (it.type == IT_PHONE && PhoneUserState(InputKeyOf(it)) == 1) add(it, 4);
    }
    // a phone the user disconnected stays off until «Подключить» / «Старт»
    for (auto it = want.begin(); it != want.end();) {
        if (it->first.rfind(L"phone|", 0) == 0 && PhoneUserState(it->first) == -1)
            it = want.erase(it);
        else
            ++it;
    }
    const ULONGLONG now = GetTickCount64();
    std::vector<std::shared_ptr<Input>> toStop;
    std::vector<std::shared_ptr<Input>> toStart;
    {
        std::lock_guard<std::mutex> lk(g_inMx);
        for (auto& kv : want) {
            auto f = g_inputs.find(kv.first);
            if (f == g_inputs.end()) {
                auto in = CreateInput(*kv.second.item, kv.first);
                if (in) {
                    in->lastWanted = now;
                    g_inputs[kv.first] = in;
                    toStart.push_back(in);
                }
                continue;
            }
            auto& in = f->second;
            in->lastWanted = now;
            if (auto* ph = dynamic_cast<PhoneInput*>(in.get())) {
                const Item& it = *kv.second.item;
                ph->wantSrc = it.phoneSrc;
                ph->wantRes = it.phoneRes;
                ph->wantFps = p.out.fps >= 50 ? 60 : 30;
                if (ph->wantCodec != it.phoneCodec) {  // a new transfer mode needs a new connection
                    ph->wantCodec = it.phoneCodec;
                    ph->forceJpeg = false;
                    toStop.push_back(in);
                    in->status = 3;
                    in->failedAt = 0;
                    continue;
                }
            }
        }
        for (auto it = g_inputs.begin(); it != g_inputs.end();) {
            if (now - it->second->lastWanted > 1500) {  // a short grace period (transitions, scene switches)
                toStop.push_back(it->second);
                it = g_inputs.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& in : toStop) in->Halt();
    for (auto& in : toStart) in->Start();
    // retry failed inputs every 3 s; push phone settings
    std::vector<std::shared_ptr<Input>> all;
    {
        std::lock_guard<std::mutex> lk(g_inMx);
        for (auto& kv : g_inputs) all.push_back(kv.second);
    }
    for (auto& in : all) {
        if (in->status == 3 && GetTickCount64() - in->failedAt >= 3000) {
            in->Halt();
            in->Start();
        } else if (in->status == 0 && !in->th.joinable()) {
            in->Start();
        }
        if (auto* ph = dynamic_cast<PhoneInput*>(in.get())) ph->SyncWanted();
    }
}

void InputsShutdown() {
    std::vector<std::shared_ptr<Input>> all;
    {
        std::lock_guard<std::mutex> lk(g_inMx);
        for (auto& kv : g_inputs) all.push_back(kv.second);
        g_inputs.clear();
    }
    for (auto& in : all) in->stop = true;
    for (auto& in : all) in->Halt();
}

void MediaNextAll(const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_inMx);
    for (auto& kv : g_inputs)
        if (auto* m = dynamic_cast<MediaInput*>(kv.second.get()))
            if (key.empty() || kv.first == key) m->skip = true;
}

std::wstring MediaNowName(const std::wstring& key) {
    auto in = GetInput(key);
    if (auto* m = dynamic_cast<MediaInput*>(in.get())) {
        std::lock_guard<std::mutex> lk(m->nameMx);
        return m->now;
    }
    return L"";
}

PhoneInfo PhoneInfoFor(const std::wstring& key) {
    PhoneInfo pi;
    auto in = GetInput(key);
    auto* ph = dynamic_cast<PhoneInput*>(in.get());
    if (!ph) return pi;
    pi.exists = true;
    pi.status = ph->status;
    {
        std::lock_guard<std::mutex> lk(ph->pmx);
        pi.name = ph->name;
        pi.state = ph->state;
        pi.err = ph->err;
        pi.usbModel = ph->usbModel;
        pi.codec = ph->codec;
    }
    pi.recvFpsX10 = ph->recvFpsX10;
    pi.decFpsX10 = ph->decFpsX10;
    pi.kbps = ph->kbps;
    pi.res = ph->resRep;
    if (auto f = ph->Latest()) {
        pi.w = f->w;
        pi.h = f->h;
    }
    return pi;
}

InputStats InputStatsFor(const std::wstring& key) {
    InputStats st;
    auto in = GetInput(key);
    if (!in) return st;
    st.exists = true;
    st.status = in->status;
    st.fpsX10 = in->FpsX10();
    if (auto f = in->Latest()) {
        st.w = f->w;
        st.h = f->h;
    }
    std::lock_guard<std::mutex> lk(in->mx);
    st.info = in->info;
    return st;
}

std::wstring InputMessage(const Item& it) {
    const std::wstring key = InputKeyOf(it);
    auto in = GetInput(key);
    const int st = in ? in->status.load() : 0;
    if (in && in->gating && (it.type == IT_WEBCAM || it.type == IT_PHONE)) return L" ";  // exposure settling: dark
    switch (it.type) {
    case IT_WEBCAM:
        if (it.camLink.empty()) return L"Веб-камера не выбрана";
        return st == 3 ? L"Веб-камера недоступна" : L"Включаю веб-камеру…";
    case IT_MEDIA:
        if (it.media.empty()) return L"Список медиафайлов пуст";
        return st == 3 ? L"Не удалось открыть файлы" : L"Открываю файл…";
    case IT_PHONE: {
        const bool usb = it.phoneConn == 1;
        if (!usb && Trim(it.phoneIp).empty()) return L"Телефон не выбран";
        if (!in) return PhoneUserState(key) == -1 ? L"Телефон отключён" : usb ? L"Телефон по USB" : L"Телефон";
        if (st == 1) return usb ? L"Подключаюсь к телефону по USB…" : L"Подключаюсь к телефону…";
        if (st == 2) {
            const PhoneInfo pi = PhoneInfoFor(key);
            return pi.state == L"need_consent" ? L"Разрешите показ экрана на телефоне" : L"Телефон подключён, жду картинку…";
        }
        return usb ? L"Телефон по USB не подключён" : L"Телефон не подключён";
    }
    default: return L"Нет картинки";
    }
}

const wchar_t* PhoneStateName(const std::wstring& st) {
    if (st == L"back") return L"задняя камера";
    if (st == L"front") return L"передняя камера";
    if (st == L"screen") return L"экран телефона";
    if (st == L"need_consent") return L"ждёт разрешения на показ экрана";
    if (st == L"camera_error") return L"камера недоступна";
    if (st == L"no_camera_permission") return L"нет разрешения на камеру";
    if (st == L"need_activity") return L"откройте WVC Phone на телефоне";
    return L"…";
}

// ---------------------------------------------------------------------------
// Discovery (UDP) and the USB list
// ---------------------------------------------------------------------------
static void PhoneDiscoverThread(HWND notify) {
    std::vector<PhoneFound> found;
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u != INVALID_SOCKET) {
        BOOL yes = TRUE;
        setsockopt(u, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in me{};
        me.sin_family = AF_INET;
        bind(u, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        std::vector<uint32_t> targets{0xFFFFFFFFu};
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
                    const uint32_t mask = ~((1u << (32 - pl)) - 1u);
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
            for (const auto& x : found) dup = dup || x.ip == pf.ip;
            if (!dup) found.push_back(pf);
        }
        closesocket(u);
    }
    {
        std::lock_guard<std::mutex> lk(g_foundMx);
        g_found = found;
    }
    g_phoneSearching = false;
    if (notify) PostMessageW(notify, WM_PHONE_FOUND, found.size(), 0);
}

void PhoneDiscover(HWND notify) {
    if (!g_wsaOk || g_phoneSearching.exchange(true)) return;
    std::thread(PhoneDiscoverThread, notify).detach();
}

std::vector<PhoneFound> PhoneFoundList() {
    std::lock_guard<std::mutex> lk(g_foundMx);
    return g_found;
}

static void PhoneUsbListThread(HWND notify) {
    int rc = 0;
    std::vector<AdbDev> devs = AdbDevices(nullptr, &rc);
    for (auto& d : devs)
        if (d.state == L"device" && d.model.empty()) d.model = AdbModel(d.serial, nullptr);
    if (rc == 0) {
        std::lock_guard<std::mutex> lk(g_adbMx);
        g_usbDevs = devs;
    }
    g_usbListing = false;
    if (notify) PostMessageW(notify, WM_PHONE_USBLIST, rc == 0 ? 1 : rc == -1 ? 2 : 3, 0);
}

void PhoneUsbList(HWND notify) {
    if (g_usbListing.exchange(true)) return;
    std::thread(PhoneUsbListThread, notify).detach();
}

std::vector<AdbDev> UsbDevices() {
    std::lock_guard<std::mutex> lk(g_adbMx);
    return g_usbDevs;
}

void AdbShutdown() {
    std::vector<std::wstring> serials;
    {
        std::lock_guard<std::mutex> lk(g_adbMx);
        for (auto& kv : g_adbForwards) serials.push_back(kv.first);
    }
    for (auto& s : serials) AdbRemoveForward(s, false);
    if (g_adbStartedServer) AdbRun(L"kill-server", nullptr, 3000, nullptr);
}
