#include "DShowSoftcam.h"

#include <cstring>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <ctime>
#include <new>
#include <climits>


namespace {

//#define ENABLE_LOG
//#define LOG_FILE_PATH "C:\\my_temp\\debug_log.txt"

#if defined(ENABLE_LOG)

FILE* logfile = nullptr;
#define OPEN_LOGFILE() logfile = std::fopen(LOG_FILE_PATH, "a")
#define CLOSE_LOGFILE() [&]{ \
        if (logfile) std::fclose(logfile); \
        logfile = nullptr; \
    }()
#define NOW() []{ \
        auto tp = std::chrono::system_clock::now(); \
        auto t = std::chrono::system_clock::to_time_t(tp); \
        static char buff[128]; \
        std::strftime(buff, sizeof(buff), "%H:%M:%S", std::localtime(&t)); \
        return buff; \
    }()
std::string IID_TO_STR(REFIID riid)
{
    std::string s =
            riid == IID_IPin                ? "IPin" :
            riid == IID_IBaseFilter         ? "IBaseFilter" :
            riid == IID_IAMovieSetup        ? "IAMovieSetup" :
            riid == IID_IQualityControl     ? "IQualityControl" :
            riid == IID_IAMStreamConfig     ? "IAMStreamConfig" :
            riid == IID_IKsPropertySet      ? "IKsPropertySet" :
            riid == IID_IAMFilterMiscFlags  ? "IAMFilterMiscFlags" :
            riid == IID_IPersistPropertyBag ? "IPersistPropertyBag" :
            riid == IID_IReferenceClock     ? "IReferenceClock" :
            riid == IID_IMediaSeeking       ? "IMediaSeeking" :
            riid == IID_IAMDeviceRemoval    ? "IAMDeviceRemoval" :
            riid == IID_IAMOpenProgress     ? "IAMOpenProgress" :
            riid == IID_IMediaPosition      ? "IMediaPosition" :
            riid == IID_IMediaFilter        ? "IMediaFilter" :
            riid == IID_IBasicVideo         ? "IBasicVideo" :
            riid == IID_IBasicAudio         ? "IBasicAudio" :
            riid == IID_IVideoWindow        ? "IVideoWindow" :
            riid == IID_IUnknown            ? "IUnknown" :
            [&]() -> std::string
            {
                char buff[128];
                snprintf(buff, sizeof(buff),
                    "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
                    riid.Data1, riid.Data2, riid.Data3,
                    riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                    riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
                return buff;
            }();
    return s;
}
#define LOG(fmt, ...) [&,funcname=__func__]{ \
        if (logfile) std::fprintf(logfile, "%s: %s " fmt, NOW(), funcname, __VA_ARGS__); \
        std::printf("%s: %s " fmt, NOW(), funcname, __VA_ARGS__); \
    }()

#else // ENABLE_LOG

#define OPEN_LOGFILE()
#define CLOSE_LOGFILE()
#define LOG(...)

#endif // ENABLE_LOG


AM_MEDIA_TYPE* allocateMediaType()
{
    BYTE *pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!pbFormat)
    {
        return nullptr;
    }

    AM_MEDIA_TYPE *amt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!amt)
    {
        CoTaskMemFree(pbFormat);
        return nullptr;
    }
    std::memset(amt, 0, sizeof(*amt));
    amt->pbFormat = pbFormat;
    return amt;
}

std::size_t calcDIBSize(int width, int height)
{
    std::size_t stride = (static_cast<unsigned>(width) * 3 + 3) & ~3u;
    return stride * static_cast<unsigned>(height);
}

void fillMediaType(AM_MEDIA_TYPE* amt, int width, int height, float framerate)
{
    BYTE *pbFormat = amt->pbFormat;

    if (framerate <= 0.0f)
    {
        framerate = 60.0f;
    }
    const float bit_rate = (float)width * (float)height * 24 * framerate;
    const float period = 10 * 1000 * 1000 / framerate;

    VIDEOINFOHEADER* pFormat = (VIDEOINFOHEADER*)pbFormat;
    std::memset(pFormat, 0, sizeof(*pFormat));
    pFormat->dwBitRate = (uint32_t)(std::min)(bit_rate, (float)INT_MAX);
    pFormat->dwBitErrorRate = 0;
    pFormat->AvgTimePerFrame = (LONGLONG)std::round((std::min)(period, (float)LONG_MAX));
    pFormat->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pFormat->bmiHeader.biWidth = width;
    pFormat->bmiHeader.biHeight = height;
    pFormat->bmiHeader.biPlanes = 1;
    pFormat->bmiHeader.biBitCount = 24;
    pFormat->bmiHeader.biCompression = BI_RGB;
    pFormat->bmiHeader.biSizeImage = static_cast<uint32_t>(calcDIBSize(width, height));

    amt->majortype = MEDIATYPE_Video;
    amt->subtype = MEDIASUBTYPE_RGB24;
    amt->bFixedSizeSamples = TRUE;
    amt->bTemporalCompression = FALSE;
    amt->lSampleSize = static_cast<uint32_t>(calcDIBSize(width, height));
    amt->formattype = FORMAT_VideoInfo;
    amt->pUnk = nullptr;
    amt->cbFormat = sizeof(VIDEOINFOHEADER);
    amt->pbFormat = pbFormat;
}

AM_MEDIA_TYPE* makeMediaType(int width, int height, float framerate)
{
    AM_MEDIA_TYPE *amt = allocateMediaType();
    if (!amt)
    {
        return nullptr;
    }
    fillMediaType(amt, width, height, framerate);
    return amt;
}

// WVC: the format the camera offers while WVC is closed — the last one WVC used (HKCU\\Software\\WVC), else 1280x720@30
DWORD readRegDword(const wchar_t* name, DWORD def)
{
    DWORD v = 0, size = sizeof(v), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WVC", name, RRF_RT_REG_DWORD, &type, &v, &size) == ERROR_SUCCESS)
    {
        return v;
    }
    return def;
}

// WVC 0.14: the default format is the last one WVC used (HKCU\Software\WVC), else 1920x1080@30 (WVC's default).
// It never depends on whether WVC is running right now, so every instance of the filter offers the same list.
softcam::VideoFormat registryFormat()
{
    DWORD w = readRegDword(L"CamWidth", 1920);
    DWORD h = readRegDword(L"CamHeight", 1080);
    DWORD f = readRegDword(L"CamFps", 30);
    if (w < 64 || w > 4096 || h < 64 || h > 4096 || w % 4 || h % 4)
    {
        w = 1920;
        h = 1080;
    }
    if (f < 5 || f > 120) f = 30;
    return softcam::VideoFormat{(int)w, (int)h, (float)f};
}

std::vector<softcam::VideoFormat> buildFormatList()
{
    std::vector<softcam::VideoFormat> v;
    auto add = [&](int w, int h, float f)
    {
        for (auto& x : v)
        {
            if (x.width == w && x.height == h && std::fabs(x.framerate - f) < 0.01f) return;
        }
        v.push_back(softcam::VideoFormat{w, h, f});
    };
    const auto def = registryFormat();
    add(def.width, def.height, def.framerate);
    add(def.width, def.height, 30.0f);
    add(def.width, def.height, 60.0f);
    static const int sizes[][2] = {{1920, 1080}, {1280, 720}, {960, 540}, {848, 480}, {640, 360}, {640, 480}};
    for (auto& sz : sizes)
    {
        add(sz[0], sz[1], 30.0f);
        add(sz[0], sz[1], 60.0f);
    }
    return v;
}

// Any RGB24 VIDEOINFOHEADER type of a sane size is accepted; the frame rate and bitrate are not checked.
// out->framerate is 0 when the type does not say it. vis_w/vis_h: the visible size (rcTarget, else the bitmap).
bool acceptableType(const AM_MEDIA_TYPE* mt, softcam::VideoFormat* out, int* vis_w = nullptr, int* vis_h = nullptr)
{
    if (!mt || mt->majortype != MEDIATYPE_Video || mt->subtype != MEDIASUBTYPE_RGB24 ||
        mt->formattype != FORMAT_VideoInfo || !mt->pbFormat || mt->cbFormat < sizeof(VIDEOINFOHEADER))
    {
        return false;
    }
    const VIDEOINFOHEADER* v = (const VIDEOINFOHEADER*)mt->pbFormat;
    if (v->bmiHeader.biBitCount != 24 || v->bmiHeader.biCompression != BI_RGB)
    {
        return false;
    }
    const LONG w = v->bmiHeader.biWidth, h = v->bmiHeader.biHeight;
    if (w < 32 || w > 4096 || h < 32 || h > 4096)
    {
        return false;  // top-down (negative height) is not offered
    }
    if (out)
    {
        out->width = (int)w;
        out->height = (int)h;
        out->framerate = v->AvgTimePerFrame > 0 ? (float)(10000000.0 / (double)v->AvgTimePerFrame) : 0.0f;
        if (out->framerate > 0.0f) out->framerate = (std::min)((std::max)(out->framerate, 1.0f), 120.0f);
    }
    const RECT& t = v->rcTarget;
    const bool rect = t.right > t.left && t.bottom > t.top && t.left >= 0 && t.top >= 0 && t.right <= w && t.bottom <= h;
    if (vis_w) *vis_w = rect ? (int)(t.right - t.left) : (int)w;
    if (vis_h) *vis_h = rect ? (int)(t.bottom - t.top) : (int)h;
    return true;
}

// where the picture goes in a sample of this type
struct Layout
{
    int stride, rows, rx, ry, rw, rh;
    std::size_t size;
};

bool layoutOf(const AM_MEDIA_TYPE* mt, Layout* L)
{
    if (!acceptableType(mt, nullptr)) return false;
    const VIDEOINFOHEADER* v = (const VIDEOINFOHEADER*)mt->pbFormat;
    const int w = (int)v->bmiHeader.biWidth, h = (int)v->bmiHeader.biHeight;
    L->stride = (w * 3 + 3) & ~3;
    L->rows = h;
    L->size = (std::size_t)L->stride * (std::size_t)h;
    const RECT& t = v->rcTarget;
    if (t.right > t.left && t.bottom > t.top && t.left >= 0 && t.top >= 0 && t.right <= w && t.bottom <= h)
    {
        L->rx = t.left;
        L->ry = t.top;
        L->rw = t.right - t.left;
        L->rh = t.bottom - t.top;
    }
    else
    {
        L->rx = 0;
        L->ry = 0;
        L->rw = w;
        L->rh = h;
    }
    return true;
}

const int s_module_anchor = 0;

HMODULE thisModule()
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&s_module_anchor), &m);
    return m;
}

std::wstring placeholderPath()
{
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WVC", L"Placeholder", RRF_RT_REG_SZ, &type, buf, &size) == ERROR_SUCCESS &&
        buf[0])
    {
        return buf;
    }
    return L"";
}

} //namespace

namespace softcam {


CUnknown * Softcam::CreateInstance(
                    LPUNKNOWN   lpunk,
                    const GUID& clsid,
                    HRESULT*    phr)
{
    OPEN_LOGFILE();
    LOG("===== logging started =====\n");

    return new Softcam(lpunk, clsid, phr);
}

Softcam::Softcam(LPUNKNOWN lpunk, const GUID& clsid, HRESULT *phr) :
    CSource(NAME("WVC Camera"), lpunk, clsid),
    // WVC 0.14: no shared memory is held until the stream runs (an idle instance must not keep WVC's old block alive)
    m_frame_buffer(FrameBuffer::none()),
    m_formats(buildFormatList())
{
    // This code is okay though it may look strange as the return value is ignored.
    // Calling the SoftcamStream constructor results in calling the CBaseOutputPin
    // constructor which registers the instance to this Softcam instance by calling
    // CSource::AddPin().
    (void)new SoftcamStream(phr, this, L"WVC Camera Stream");
}


STDMETHODIMP Softcam::NonDelegatingQueryInterface(REFIID riid, __deref_out void **ppv)
{
    if (riid == IID_IAMStreamConfig)
    {
        LOG("(Softcam) IAMStreamConfig -> S_OK\n");
        return GetInterface(static_cast<IAMStreamConfig*>(this), ppv);
    }
    else
    {
        auto result = CSource::NonDelegatingQueryInterface(riid, ppv);
        LOG("(Softcam) %s -> %s\n", IID_TO_STR(riid).c_str(), result ? "(ERROR)" : "S_OK");
        return result;
    }
}

SoftcamStream* Softcam::stream()
{
    return m_iPins > 0 && m_paStreams && m_paStreams[0] ? static_cast<SoftcamStream*>(m_paStreams[0]) : nullptr;
}

VideoFormat Softcam::preferredFormat()
{
    CAutoLock lock(&m_fmt_lock);
    return m_has_set ? m_set : m_formats.front();
}

bool Softcam::setFormat(VideoFormat* out)
{
    CAutoLock lock(&m_fmt_lock);
    if (m_has_set && out) *out = m_set;
    return m_has_set;
}

HRESULT
Softcam::SetFormat(AM_MEDIA_TYPE *mt)
{
    if (!mt)
    {
        // back to the default
        CAutoLock lock(&m_fmt_lock);
        m_has_set = false;
        LOG("(reset) -> S_OK\n");
        return S_OK;
    }
    VideoFormat f{};
    if (!acceptableType(mt, &f))
    {
        LOG("-> VFW_E_INVALIDMEDIATYPE\n");
        return VFW_E_INVALIDMEDIATYPE;
    }
    if (f.framerate <= 0.0f)
    {
        f.framerate = defaultFormat().framerate;
    }
    {
        CAutoLock lock(&m_fmt_lock);
        m_has_set = true;
        m_set = f;
    }
    if (auto s = stream())
    {
        s->reconnectFor(f);
    }
    LOG("-> S_OK\n");
    return S_OK;
}

HRESULT
Softcam::GetFormat(AM_MEDIA_TYPE **out_pmt)
{
    if (!out_pmt)
    {
        LOG("-> E_POINTER\n");
        return E_POINTER;
    }
    if (auto s = stream())
    {
        if (s->copyConnectedType(out_pmt) == S_OK)
        {
            return S_OK;
        }
    }
    const VideoFormat f = preferredFormat();
    AM_MEDIA_TYPE* mt = makeMediaType(f.width, f.height, f.framerate);
    if (!mt)
    {
        LOG("-> E_OUTOFMEMORY\n");
        return E_OUTOFMEMORY;
    }
    *out_pmt = mt;
    LOG("-> S_OK\n");
    return S_OK;
}

HRESULT
Softcam::GetNumberOfCapabilities(int *out_count, int *out_size)
{
    if (!out_count || !out_size)
    {
        LOG("-> E_POINTER\n");
        return E_POINTER;
    }
    *out_count = (int)m_formats.size();
    *out_size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    LOG("-> S_OK\n");
    return S_OK;
}

HRESULT
Softcam::GetStreamCaps(int index, AM_MEDIA_TYPE **out_pmt, BYTE *out_scc)
{
    if (!out_pmt || !out_scc)
    {
        LOG("-> E_POINTER\n");
        return E_POINTER;
    }
    if (index < 0)
    {
        return E_INVALIDARG;
    }
    if (index >= (int)m_formats.size())
    {
        LOG("-> S_FALSE (invalid index)\n");
        return S_FALSE;
    }
    const VideoFormat f = m_formats[(std::size_t)index];
    AM_MEDIA_TYPE *mt = makeMediaType(f.width, f.height, f.framerate);
    if (!mt)
    {
        LOG("-> E_OUTOFMEMORY\n");
        return E_OUTOFMEMORY;
    }
    *out_pmt = mt;
    std::memset(out_scc, 0, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    VIDEO_STREAM_CONFIG_CAPS* scc = (VIDEO_STREAM_CONFIG_CAPS*)out_scc;
    const int w = f.width, h = f.height;
    scc->guid = FORMAT_VideoInfo;
    scc->VideoStandard = 0;
    scc->InputSize = SIZE{w, h};
    scc->MinCroppingSize = SIZE{w, h};
    scc->MaxCroppingSize = SIZE{w, h};
    scc->CropGranularityX = 1;
    scc->CropGranularityY = 1;
    scc->CropAlignX = 1;
    scc->CropAlignY = 1;
    scc->MinOutputSize = SIZE{w, h};
    scc->MaxOutputSize = SIZE{w, h};
    scc->OutputGranularityX = 1;
    scc->OutputGranularityY = 1;
    scc->StretchTapsX = 0;
    scc->StretchTapsY = 0;
    scc->ShrinkTapsX = 0;
    scc->ShrinkTapsY = 0;
    // any rate from 5 to 60 frames per second is fine (the frame rate is never a reason to refuse a format)
    scc->MinFrameInterval = 10000000LL / 60;
    scc->MaxFrameInterval = 10000000LL / 5;
    scc->MinBitsPerSecond = (LONG)(std::min)((double)w * h * 24 * 5, (double)LONG_MAX);
    scc->MaxBitsPerSecond = (LONG)(std::min)((double)w * h * 24 * 60, (double)LONG_MAX);
    LOG("-> S_OK\n");
    return S_OK;
}


FrameBuffer* Softcam::getFrameBuffer()
{
    CAutoLock lock(&m_critsec);
    if (!m_frame_buffer)
    {
        // WVC: look for the sender at most 5 times a second; any size is fine (the picture is scaled).
        // An inactive block (WVC closed or restarting its camera) is never kept.
        const unsigned long now = GetTickCount();
        if (now - m_last_open_try < 200)
        {
            return nullptr;
        }
        m_last_open_try = now;
        auto fb = FrameBuffer::open();
        if (fb && fb.active())
        {
            m_frame_buffer = fb;
        }
        else
        {
            fb.release();
        }
    }
    if (m_frame_buffer)
    {
        return &m_frame_buffer;
    }
    else
    {
        return nullptr;
    }
}

void
Softcam::releaseFrameBuffer()
{
    CAutoLock lock(&m_critsec);
    m_frame_buffer.release();
    m_last_open_try = 0;
}

SoftcamStream::SoftcamStream(HRESULT *phr,
                         Softcam *pParent,
                         LPCWSTR pPinName) :
    CSourceStream(NAME("DirectShow Softcam Stream"), phr, pParent, pPinName)
{
}


SoftcamStream::~SoftcamStream()
{
    LOG("===== logging finished =====\n");
    CLOSE_LOGFILE();
}


STDMETHODIMP SoftcamStream::NonDelegatingQueryInterface(REFIID riid, __deref_out void **ppv)
{
    if (riid == IID_IKsPropertySet )
    {
        LOG("(SoftcamStream) IKsPropertySet -> S_OK\n");
        return GetInterface(static_cast<IKsPropertySet*>(this), ppv);
    }
    else if(riid == IID_IAMStreamConfig)
    {
        LOG("(SoftcamStream) IAMStreamConfig -> S_OK\n");
        return GetInterface(static_cast<IAMStreamConfig*>(this), ppv);
    }
    else
    {
        auto result = CSourceStream::NonDelegatingQueryInterface(riid, ppv);
        LOG("(SoftcamStream) %s -> %s\n", IID_TO_STR(riid).c_str(), result ? "(ERROR)" : "S_OK");
        return result;
    }
}


// WVC: the frame shown while WVC is not running: a picture chosen in WVC (Настройки → Общие), or the WVC icon
// with «WVC не запущен» on dark blue. Drawn once per size with GDI; never blocks (only a local file is read).
void SoftcamStream::buildPlaceholder(int m_width, int m_height)
{
    const std::size_t size = calcDIBSize(m_width, m_height);
    if (!m_screenshot || m_ph_w != m_width || m_ph_h != m_height)
    {
        m_screenshot.reset(new (std::nothrow) uint8_t[size]);
        m_ph_w = m_width;
        m_ph_h = m_height;
    }
    m_placeholder_ready = true;
    if (!m_screenshot)
    {
        m_ph_w = m_ph_h = 0;
        return;
    }
    std::memset(m_screenshot.get(), 0, size);

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = m_width;
    bi.bmiHeader.biHeight = m_height;  // bottom-up, exactly the DirectShow DIB layout
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = dc ? CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (screen) ReleaseDC(nullptr, screen);
    if (!bmp || !bits)
    {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT rc{0, 0, m_width, m_height};
    HBRUSH bg = CreateSolidBrush(RGB(0x1a, 0x23, 0x40));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    bool custom = false;
    std::wstring path = placeholderPath();
    if (!path.empty())
    {
        HBITMAP pic = (HBITMAP)LoadImageW(nullptr, path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
        if (pic)
        {
            BITMAP bm{};
            GetObject(pic, sizeof(bm), &bm);
            HDC pdc = CreateCompatibleDC(dc);
            HGDIOBJ op = SelectObject(pdc, pic);
            if (bm.bmWidth > 0 && bm.bmHeight > 0)
            {
                double s = (std::min)((double)m_width / bm.bmWidth, (double)m_height / bm.bmHeight);
                int w = (int)(bm.bmWidth * s), h = (int)(bm.bmHeight * s);
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
                FillRect(dc, &rc, black);
                StretchBlt(dc, (m_width - w) / 2, (m_height - h) / 2, w, h, pdc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                custom = true;
            }
            SelectObject(pdc, op);
            DeleteDC(pdc);
            DeleteObject(pic);
        }
    }
    if (!custom)
    {
        const int icon = (std::max)(16, m_height / 4);
        HICON hi = (HICON)LoadImageW(thisModule(), MAKEINTRESOURCEW(1), IMAGE_ICON, icon, icon, 0);
        const int ty = m_height / 2 + m_height / 30;
        if (hi)
        {
            DrawIconEx(dc, (m_width - icon) / 2, ty - icon - m_height / 40, hi, icon, icon, 0, nullptr, DI_NORMAL);
            DestroyIcon(hi);
        }
        SetBkMode(dc, TRANSPARENT);
        HFONT big = CreateFontW(-(std::max)(10, m_height / 13), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT sm = CreateFontW(-(std::max)(8, m_height / 26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, big);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT t1{0, ty, m_width, ty + m_height / 9};
        DrawTextW(dc, L"WVC не запущен", -1, &t1, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, sm);
        SetTextColor(dc, RGB(170, 180, 205));
        RECT t2{0, t1.bottom, m_width, t1.bottom + m_height / 14};
        DrawTextW(dc, L"Запустите WVC", -1, &t2, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, of);
        DeleteObject(big);
        DeleteObject(sm);
    }
    GdiFlush();
    std::memcpy(m_screenshot.get(), bits, size);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// WVC 0.14: never fails the stream. The layout comes from the agreed media type (or a new one the renderer attached
// to the sample); WVC's frame is always fitted into it with black bars; if the sample is smaller than the frame,
// as much as fits is written.
HRESULT SoftcamStream::FillBuffer(IMediaSample *pms)
{
    CheckPointer(pms,E_POINTER);

    // a format change requested by the downstream filter (e.g. a renderer asking for another stride)
    {
        AM_MEDIA_TYPE* dyn = nullptr;
        if (pms->GetMediaType(&dyn) == S_OK && dyn)
        {
            if (acceptableType(dyn, nullptr))
            {
                CMediaType cm(*dyn);
                SetMediaType(&cm);
            }
            DeleteMediaType(dyn);
        }
    }

    long interval_ms = 33;
    {
        CAutoLock lock(&m_critsec);
        interval_ms = (std::min)((std::max)(m_interval_time_msec, 1L), 1000L);
    }
    // pace to the agreed frame rate: never faster than it (WVC may send more frames than the app asked for)
    if (!m_first_sample)
    {
        const float wait = (float)interval_ms * 0.8f / 1000.0f - m_pace_timer.get();
        if (wait > 0.001f)
        {
            Timer::sleep(wait);
        }
    }

    Layout L{};
    const bool have_layout = layoutOf(&m_mt, &L);
    BYTE *pData = nullptr;
    if (FAILED(pms->GetPointer(&pData)))
    {
        pData = nullptr;
    }
    const long lDataLen = pms->GetSize();
    long written = 0;
    if (have_layout && pData && lDataLen > 0)
    {
        uint8_t* target = pData;
        const bool use_scratch = (std::size_t)lDataLen < L.size;
        if (use_scratch)
        {
            m_scratch.resize(L.size);
            target = m_scratch.data();
        }
        bool sent = false;
        if (auto fb = getParent()->getFrameBuffer())
        {
            bool active = fb->waitForNewFrame(m_frame_counter);
            if (active)
            {
                fb->transferToDIBRect(target, L.stride, L.rows, L.rx, L.ry, L.rw, L.rh, &m_frame_counter);
                sent = true;
            }
            else
            {
                // WVC closed or restarted its camera: let the block go and show the placeholder until it is back
                getParent()->releaseFrameBuffer();
                m_frame_counter = 0;
            }
        }
        if (!sent)
        {
            if (!m_placeholder_ready || m_ph_w != L.rw || m_ph_h != L.rh)
            {
                buildPlaceholder(L.rw, L.rh);
            }
            const int pstride = (L.rw * 3 + 3) & ~3;
            for (int y = 0; y < L.rh; y++)
            {
                // both bottom-up: placeholder row (rh-1-y) -> sample row (rows-1-(ry+y))
                uint8_t* d = target + (std::size_t)L.stride * (std::size_t)(L.rows - 1 - (L.ry + y)) + (std::size_t)L.rx * 3;
                if (m_screenshot && m_ph_w == L.rw && m_ph_h == L.rh)
                    std::memcpy(d, m_screenshot.get() + (std::size_t)pstride * (std::size_t)(L.rh - 1 - y), (std::size_t)L.rw * 3);
                else
                    std::memset(d, 0, (std::size_t)L.rw * 3);
            }
            if (m_first_sample)
            {
                Timer::sleep((float)interval_ms / 1000.0f);
            }
        }
        if (use_scratch)
        {
            std::memcpy(pData, target, (std::size_t)lDataLen);
            written = lDataLen;
        }
        else
        {
            written = (long)L.size;
        }
    }
    else
    {
        // nothing we can draw into: keep the pace and deliver an empty sample
        Timer::sleep((float)interval_ms / 1000.0f);
    }
    pms->SetActualDataLength(written);

    {
        CAutoLock lock(&m_critsec);
        const LONGLONG interval = (LONGLONG)m_interval_time_msec * 10000;
        if (m_first_sample)
        {
            m_run_timer.reset();
            m_first_sample = false;
        }
        // stamps advance one frame each, but never lag behind real time (when the source is slower than agreed)
        const LONGLONG real = (LONGLONG)((double)m_run_timer.get() * 10000000.0);
        LONGLONG start = m_sample_time;
        if (real - start > interval)
        {
            start = real;
        }
        LONGLONG stop = start + interval;
        m_sample_time = stop;
        REFERENCE_TIME t0 = start, t1 = stop;
        pms->SetTime(&t0, &t1);
    }
    m_pace_timer.reset();
    pms->SetSyncPoint(TRUE);
    return NOERROR;
}


STDMETHODIMP SoftcamStream::Notify(IBaseFilter * pSender, Quality q)
{
    CAutoLock lock(&m_critsec);
    m_interval_time_msec = m_interval_time_msec * 1000 / (std::max)(q.Proportion, 1L);
    m_interval_time_msec = (std::min)((std::max)(m_interval_time_msec, 1L), 1000L);
    if (q.Late > 0) {
        m_sample_time += q.Late;
    }
    //LOG("-> NOERROR\n");
    return NOERROR;
}


HRESULT SoftcamStream::GetMediaType(CMediaType *pmt)
{
    return GetMediaType(0, pmt);
}

// position 0: the format set by SetFormat (else the default), then the whole fixed list
HRESULT SoftcamStream::GetMediaType(int iPosition, CMediaType *pmt)
{
    CheckPointer(pmt,E_POINTER);
    if (iPosition < 0)
    {
        return E_INVALIDARG;
    }
    VideoFormat f{};
    if (iPosition == 0)
    {
        f = getParent()->preferredFormat();
    }
    else if (iPosition - 1 < (int)getParent()->formats().size())
    {
        f = getParent()->formats()[(std::size_t)(iPosition - 1)];
    }
    else
    {
        return VFW_S_NO_MORE_ITEMS;
    }

    VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER*)pmt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER));
    if (pvi == nullptr)
    {
        LOG("-> E_OUTOFMEMORY\n");
        return E_OUTOFMEMORY;
    }
    fillMediaType(pmt, f.width, f.height, f.framerate);

    LOG("-> NOERROR\n");
    return NOERROR;
}

// WVC 0.14: any RGB24 size is fine (the picture is scaled); the frame rate and bitrate are ignored.
// After SetFormat only that size (visible size) is accepted, as IAMStreamConfig asks.
HRESULT SoftcamStream::CheckMediaType(const CMediaType *pmt)
{
    int vw = 0, vh = 0;
    if (!acceptableType(pmt, nullptr, &vw, &vh))
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    VideoFormat set{};
    if (getParent()->setFormat(&set))
    {
        if (vw != set.width || vh != set.height)
        {
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
    }
    return NOERROR;
}

HRESULT SoftcamStream::SetMediaType(const CMediaType *pmt)
{
    HRESULT hr = CSourceStream::SetMediaType(pmt);
    if (SUCCEEDED(hr))
    {
        VideoFormat f{};
        if (acceptableType(pmt, &f))
        {
            float fps = f.framerate > 0.0f ? f.framerate : getParent()->preferredFormat().framerate;
            fps = (std::min)((std::max)(fps, 1.0f), 120.0f);
            CAutoLock lock(&m_critsec);
            m_interval_time_msec = (long)std::round(1000.0f / fps);
        }
    }
    return hr;
}

void SoftcamStream::reconnectFor(const VideoFormat& f)
{
    if (!IsConnected())
    {
        // fps only matters for the pace
        return;
    }
    Layout L{};
    if (layoutOf(&m_mt, &L) && L.rw == f.width && L.rh == f.height)
    {
        CAutoLock lock(&m_critsec);
        m_interval_time_msec = (long)std::round(1000.0f / (std::min)((std::max)(f.framerate, 1.0f), 120.0f));
        return;
    }
    CMediaType cm;
    if (!cm.AllocFormatBuffer(sizeof(VIDEOINFOHEADER)))
    {
        return;
    }
    fillMediaType(&cm, f.width, f.height, f.framerate);
    if (GetConnected() && GetConnected()->QueryAccept(&cm) == S_OK)
    {
        (void)m_pFilter->ReconnectPin(this, &cm);
    }
}

HRESULT SoftcamStream::copyConnectedType(AM_MEDIA_TYPE **out_pmt)
{
    if (!IsConnected())
    {
        return S_FALSE;
    }
    AM_MEDIA_TYPE* mt = CreateMediaType(&m_mt);
    if (!mt)
    {
        return E_OUTOFMEMORY;
    }
    *out_pmt = mt;
    return S_OK;
}

HRESULT SoftcamStream::DecideBufferSize(IMemAllocator *pAlloc,
                                        ALLOCATOR_PROPERTIES *pProperties)
{
    CheckPointer(pAlloc,E_POINTER);
    CheckPointer(pProperties,E_POINTER);

    CAutoLock lock(m_pFilter->pStateLock());
    HRESULT hr = NOERROR;

    Layout L{};
    long size = 0;
    if (layoutOf(&m_mt, &L))
    {
        size = (long)L.size;
    }
    VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER *)m_mt.Format();
    if (pvi && m_mt.FormatLength() >= sizeof(VIDEOINFOHEADER))
    {
        size = (std::max)(size, (long)pvi->bmiHeader.biSizeImage);
    }
    if (size <= 0)
    {
        size = (long)calcDIBSize(1920, 1080);
    }
    pProperties->cBuffers = (std::max)(pProperties->cBuffers, 1L);
    pProperties->cbBuffer = (std::max)(pProperties->cbBuffer, size);

    ALLOCATOR_PROPERTIES actual;
    hr = pAlloc->SetProperties(pProperties, &actual);
    if (FAILED(hr))
    {
        LOG("-> (FAILED)\n");
        return hr;
    }
    // a smaller buffer than asked is not an error any more: FillBuffer writes as much as fits
    LOG("-> NOERROR\n");
    return NOERROR;
}

HRESULT SoftcamStream::OnThreadCreate()
{
    CAutoLock lock(&m_critsec);
    m_placeholder_ready = false;  // WVC: pick up a new placeholder picture each time the camera is opened
    m_sample_time = 0;
    m_first_sample = true;
    m_frame_counter = 0;
    float framerate = 0.0f;
    VideoFormat f{};
    if (acceptableType(&m_mt, &f) && f.framerate > 0.0f)
    {
        framerate = f.framerate;
    }
    else
    {
        framerate = getParent()->preferredFormat().framerate;
    }
    if (framerate <= 0.0f)
    {
        framerate = 30.0f;
    }
    framerate = (std::min)((std::max)(framerate, 1.0f), 120.0f);
    m_interval_time_msec = (long)std::round(1000.0f / framerate);

    LOG("-> NOERROR\n");
    return NOERROR;
}

HRESULT SoftcamStream::OnThreadDestroy()
{
    // WVC 0.14: the stream stopped — do not keep WVC's shared memory block
    getParent()->releaseFrameBuffer();
    m_frame_counter = 0;
    return NOERROR;
}

HRESULT SoftcamStream::Set(REFGUID guidPropSet, DWORD dwPropID,
                           LPVOID pInstanceData, DWORD cbInstanceData,
                           LPVOID pPropData, DWORD cbPropData)
{
    LOG("-> E_NOTIMPL\n");
    return E_NOTIMPL;
}

HRESULT SoftcamStream::Get(REFGUID guidPropSet, DWORD dwPropID,
                           LPVOID pInstanceData, DWORD cbInstanceData,
                           LPVOID pPropData, DWORD cbPropData, DWORD *pcbReturned)
{
    if (guidPropSet != AMPROPSETID_Pin)
    {
        LOG("-> E_PROP_SET_UNSUPPORTED\n");
        return E_PROP_SET_UNSUPPORTED;
    }
    if (dwPropID != AMPROPERTY_PIN_CATEGORY)
    {
        LOG("-> E_PROP_ID_UNSUPPORTED\n");
        return E_PROP_ID_UNSUPPORTED;
    }
    if (pPropData == nullptr && pcbReturned == nullptr)
    {
        LOG("-> E_POINTER\n");
        return E_POINTER;
    }
    if (pcbReturned)
    {
        *pcbReturned = sizeof(GUID);
    }
    if (pPropData)
    {
        if (cbPropData < sizeof(GUID))
        {
            LOG("-> E_UNEXPECTED\n");
            return E_UNEXPECTED;
        }
        *(GUID*)pPropData = PIN_CATEGORY_CAPTURE;
    }
    LOG("-> S_OK\n");
    return S_OK;
}

HRESULT SoftcamStream::QuerySupported(REFGUID guidPropSet, DWORD dwPropID,
                              DWORD *pTypeSupport)
{
    if (guidPropSet != AMPROPSETID_Pin)
    {
        LOG("-> E_PROP_SET_UNSUPPORTED\n");
        return E_PROP_SET_UNSUPPORTED;
    }
    if (dwPropID != AMPROPERTY_PIN_CATEGORY)
    {
        LOG("-> E_PROP_ID_UNSUPPORTED\n");
        return E_PROP_ID_UNSUPPORTED;
    }
    if (pTypeSupport)
    {
        *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    }
    LOG("-> S_OK\n");
    return S_OK;
}

HRESULT SoftcamStream::SetFormat(AM_MEDIA_TYPE *mt)
{
    return getParent()->SetFormat(mt);
}

HRESULT SoftcamStream::GetFormat(AM_MEDIA_TYPE **out_pmt)
{
    return getParent()->GetFormat(out_pmt);
}

HRESULT SoftcamStream::GetNumberOfCapabilities(int *out_count, int *out_size)
{
    return getParent()->GetNumberOfCapabilities(out_count, out_size);
}

HRESULT SoftcamStream::GetStreamCaps(int index, AM_MEDIA_TYPE **out_pmt, BYTE *out_scc)
{
    return getParent()->GetStreamCaps(index, out_pmt, out_scc);
}

Softcam* SoftcamStream::getParent()
{
    return static_cast<Softcam*>(m_pFilter);
}


} //namespace softcam
