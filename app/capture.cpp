// WVC 0.13: screen capture through DXGI Desktop Duplication. Each monitor is duplicated on the GPU that drives it;
// a new desktop picture is copied GPU->GPU at once, and only the parts the scene uses are read back to memory.
// Anything unusual (rotated monitor, a region across two monitors, secure desktop, no duplication allowed) returns
// null and the renderer falls back to GDI for that source. Used only by the render thread.
#include "common.h"

#include <d3d11.h>
#include <dxgi1_2.h>

namespace {
struct Dup {
    RECT rc{};                 // desktop coordinates
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput1* output = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    ID3D11Texture2D* gpu = nullptr;      // last desktop picture (GPU copy)
    ID3D11Texture2D* staging = nullptr;  // CPU-readable
    std::vector<uint8_t> buf;            // BGRA, monitor size
    int w = 0, h = 0;
    bool have = false;                   // gpu holds a picture
    uint64_t version = 0;                // bumps with every new desktop picture
    uint64_t checkedFrame = ~0ull;       // render frame in which AcquireNextFrame ran
    std::vector<RECT> valid;             // parts of buf current for `version` (monitor-local)
    uint64_t validVersion = ~0ull;
    ULONGLONG failedAt = 0;
    bool unsupported = false;
    std::wstring why;

    void ReleaseDup() {
        SafeRelease(dup);
        SafeRelease(gpu);
        SafeRelease(staging);
        have = false;
        valid.clear();
    }
    void ReleaseAll() {
        ReleaseDup();
        SafeRelease(ctx);
        SafeRelease(dev);
        SafeRelease(output);
        SafeRelease(adapter);
    }
};
}  // namespace

struct ScreenCapture::Impl {
    std::vector<Dup> mons;
    uint64_t frame = 0;
    ULONGLONG enumAt = 0;
    std::wstring lastStatus;

    void Enumerate() {
        for (auto& m : mons) m.ReleaseAll();
        mons.clear();
        enumAt = GetTickCount64();
        IDXGIFactory1* f = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return;
        for (UINT a = 0;; ++a) {
            IDXGIAdapter1* ad = nullptr;
            if (f->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT o = 0;; ++o) {
                IDXGIOutput* out = nullptr;
                if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC od{};
                out->GetDesc(&od);
                IDXGIOutput1* o1 = nullptr;
                out->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&o1));
                out->Release();
                if (!od.AttachedToDesktop || !o1) {
                    SafeRelease(o1);
                    continue;
                }
                Dup d;
                d.rc = od.DesktopCoordinates;
                d.output = o1;
                d.adapter = ad;
                ad->AddRef();
                if (od.Rotation != DXGI_MODE_ROTATION_IDENTITY && od.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
                    d.unsupported = true;
                    d.why = L"монитор повёрнут";
                }
                mons.push_back(d);
            }
            ad->Release();
        }
        f->Release();
    }

    bool Open(Dup& d) {
        if (d.dup) return true;
        if (d.unsupported) return false;
        if (d.failedAt && GetTickCount64() - d.failedAt < 2000) return false;
        if (!d.dev) {
            const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
            HRESULT hr = D3D11CreateDevice(d.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 3,
                                           D3D11_SDK_VERSION, &d.dev, nullptr, &d.ctx);
            if (FAILED(hr)) {
                d.failedAt = GetTickCount64();
                d.why = L"D3D11 недоступен";
                return false;
            }
        }
        HRESULT hr = d.output->DuplicateOutput(d.dev, &d.dup);
        if (FAILED(hr)) {
            d.failedAt = GetTickCount64();
            wchar_t b[64];
            swprintf(b, 64, L"DuplicateOutput 0x%08lX", static_cast<unsigned long>(hr));
            d.why = b;
            if (hr == DXGI_ERROR_UNSUPPORTED) d.unsupported = true;  // e.g. another GPU drives it: stay on GDI
            return false;
        }
        DXGI_OUTDUPL_DESC dd{};
        d.dup->GetDesc(&dd);
        d.w = static_cast<int>(dd.ModeDesc.Width);
        d.h = static_cast<int>(dd.ModeDesc.Height);
        if (dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && dd.ModeDesc.Format != DXGI_FORMAT_UNKNOWN) {
            d.ReleaseDup();
            d.unsupported = true;
            d.why = L"формат экрана не BGRA8";
            return false;
        }
        if (d.w != d.rc.right - d.rc.left || d.h != d.rc.bottom - d.rc.top) {
            d.ReleaseDup();
            d.failedAt = GetTickCount64();
            d.why = L"размер не совпал";
            return false;
        }
        d.buf.assign(static_cast<size_t>(d.w) * d.h * 4, 0);
        d.why.clear();
        return true;
    }

    // a new desktop picture (if any) -> d.gpu; once per render frame
    void Poll(Dup& d) {
        if (d.checkedFrame == frame) return;
        d.checkedFrame = frame;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        IDXGIResource* res = nullptr;
        HRESULT hr = d.dup->AcquireNextFrame(0, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
        if (FAILED(hr)) {
            d.ReleaseDup();
            d.failedAt = GetTickCount64();
            d.why = hr == DXGI_ERROR_ACCESS_LOST ? L"доступ потерян (смена режима, UAC)" : L"AcquireNextFrame";
            return;
        }
        if (fi.LastPresentTime.QuadPart != 0 || !d.have) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
                if (!d.gpu) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = 0;
                    td.CPUAccessFlags = 0;
                    td.MiscFlags = 0;
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    d.dev->CreateTexture2D(&td, nullptr, &d.gpu);
                    td.Usage = D3D11_USAGE_STAGING;
                    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    d.dev->CreateTexture2D(&td, nullptr, &d.staging);
                }
                if (d.gpu && d.staging) {
                    d.ctx->CopyResource(d.gpu, tex);
                    d.have = true;
                    ++d.version;
                }
                tex->Release();
            }
        }
        res->Release();
        d.dup->ReleaseFrame();
    }

    // monitor-local rect r: make buf current for it
    bool Fetch(Dup& d, const RECT& r) {
        if (d.validVersion != d.version) {
            d.valid.clear();
            d.validVersion = d.version;
        }
        for (const RECT& v : d.valid)
            if (r.left >= v.left && r.top >= v.top && r.right <= v.right && r.bottom <= v.bottom) return true;
        D3D11_BOX box{static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0, static_cast<UINT>(r.right),
                      static_cast<UINT>(r.bottom), 1};
        d.ctx->CopySubresourceRegion(d.staging, 0, r.left, r.top, 0, d.gpu, 0, &box);
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (FAILED(d.ctx->Map(d.staging, 0, D3D11_MAP_READ, 0, &ms))) return false;
        const size_t rowBytes = static_cast<size_t>(r.right - r.left) * 4;
        const uint8_t* srcBase = static_cast<const uint8_t*>(ms.pData);
        uint8_t* dstBase = d.buf.data();
        const int W = d.w;
        const UINT pitch = ms.RowPitch;
        ParallelFor(r.bottom - r.top, [&](int a, int b) {
            for (int y = a; y < b; ++y) {
                const int yy = r.top + y;
                std::memcpy(dstBase + (static_cast<size_t>(yy) * W + r.left) * 4,
                            srcBase + static_cast<size_t>(yy) * pitch + static_cast<size_t>(r.left) * 4, rowBytes);
            }
        }, 64);
        d.ctx->Unmap(d.staging, 0);
        d.valid.push_back(r);
        if (d.valid.size() > 8) d.valid.erase(d.valid.begin());
        return true;
    }
};

ScreenCapture::ScreenCapture() : d(new Impl) {}
ScreenCapture::~ScreenCapture() {
    for (auto& m : d->mons) m.ReleaseAll();
    delete d;
}

void ScreenCapture::NewFrame() {
    ++d->frame;
    // monitors changed (resolution, plugged in): enumerate again
    if (d->mons.empty() ? GetTickCount64() - d->enumAt > 5000 : false) d->Enumerate();
}

const uint8_t* ScreenCapture::View(const RECT& r, int* stride) {
    if (d->mons.empty() && d->enumAt == 0) d->Enumerate();
    for (auto& m : d->mons) {
        if (r.left < m.rc.left || r.top < m.rc.top || r.right > m.rc.right || r.bottom > m.rc.bottom) continue;
        if (!d->Open(m)) return nullptr;
        d->Poll(m);
        if (!m.dup || !m.have) return nullptr;
        const RECT local{r.left - m.rc.left, r.top - m.rc.top, r.right - m.rc.left, r.bottom - m.rc.top};
        if (local.right <= local.left || local.bottom <= local.top) return nullptr;
        if (!d->Fetch(m, local)) return nullptr;
        *stride = m.w * 4;
        return m.buf.data() + (static_cast<size_t>(local.top) * m.w + local.left) * 4;
    }
    return nullptr;
}

std::wstring ScreenCapture::Status() const {
    std::wstring t;
    for (const auto& m : d->mons) {
        if (!t.empty()) t += L"; ";
        wchar_t b[64];
        swprintf(b, 64, L"монитор %ldx%ld: ", m.rc.right - m.rc.left, m.rc.bottom - m.rc.top);
        t += b;
        t += m.dup ? L"DXGI" : (L"GDI" + (m.why.empty() ? std::wstring() : L" (" + m.why + L")"));
    }
    return t.empty() ? L"DXGI не используется" : t;
}
