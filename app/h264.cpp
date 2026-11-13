// WVC: H.264 (Annex-B) -> BGRA with the Media Foundation H.264 decoder MFT (software, low latency).
#include "common.h"

namespace {
// GUIDs written out here so no extra import library is needed
const GUID kCatVideoDecoder = {0xd6c02d4b, 0x6833, 0x45b4, {0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91}};
const GUID kLowLatency = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};
const GUID kMsH264Clsid = {0x62CE7E72, 0x4C71, 0x4d20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};
const GUID kSubH264 = {0x34363248, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubNV12 = {0x3231564E, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

inline uint8_t Clip(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// NV12 (BT.601, limited range) -> BGRA
[[maybe_unused]] void Nv12ToBgra(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int w, int h, uint8_t* out) {
    for (int r = 0; r < h; ++r) {
        const uint8_t* yr = y + static_cast<ptrdiff_t>(r) * yPitch;
        const uint8_t* cr = uv + static_cast<ptrdiff_t>(r / 2) * uvPitch;
        uint8_t* o = out + static_cast<size_t>(r) * w * 4;
        for (int c = 0; c < w; c += 2) {
            const int u = cr[c] - 128, v = cr[c + 1] - 128;
            const int ruv = 409 * v + 128, guv = -100 * u - 208 * v + 128, buv = 516 * u + 128;
            for (int k = 0; k < 2 && c + k < w; ++k) {
                const int yy = 298 * (yr[c + k] - 16);
                o[0] = Clip((yy + buv) >> 8);
                o[1] = Clip((yy + guv) >> 8);
                o[2] = Clip((yy + ruv) >> 8);
                o[3] = 255;
                o += 4;
            }
        }
    }
}
}  // namespace

struct H264Decoder::Impl {
    IMFTransform* t = nullptr;
    UINT32 fw = 0, fh = 0;      // coded frame size (e.g. 1920 x 1088)
    UINT32 vw = 0, vh = 0;      // visible size
    LONG stride = 0;
    DWORD outSize = 0;
    bool provides = false;
    bool outSet = false;

    bool SetOutput() {
        outSet = false;
        for (DWORD i = 0;; ++i) {
            IMFMediaType* mt = nullptr;
            if (FAILED(t->GetOutputAvailableType(0, i, &mt))) break;
            GUID sub{};
            mt->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == kSubNV12) {
                const HRESULT hr = t->SetOutputType(0, mt, 0);
                if (SUCCEEDED(hr)) {
                    MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &fw, &fh);
                    vw = fw;
                    vh = fh;
                    MFVideoArea area{};
                    UINT32 got = 0;
                    if (SUCCEEDED(mt->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area),
                                              sizeof(area), &got)) &&
                        got == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
                        vw = static_cast<UINT32>(area.Area.cx);
                        vh = static_cast<UINT32>(area.Area.cy);
                    }
                    UINT32 st = 0;
                    stride = SUCCEEDED(mt->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? static_cast<LONG>(st)
                                                                                 : static_cast<LONG>(fw);
                    outSet = true;
                }
                mt->Release();
                if (outSet) break;
                continue;
            }
            mt->Release();
        }
        MFT_OUTPUT_STREAM_INFO si{};
        if (SUCCEEDED(t->GetOutputStreamInfo(0, &si))) {
            outSize = si.cbSize;
            provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        }
        return outSet;
    }
};

H264Decoder::H264Decoder() : d(new Impl) {}

H264Decoder::~H264Decoder() {
    if (d->t) {
        d->t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        d->t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    SafeRelease(d->t);
    delete d;
}

bool H264Decoder::Init() {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, kSubH264};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, kSubNV12};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (SUCCEEDED(MFTEnumEx(kCatVideoDecoder, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                            &in, &out, &acts, &n))) {
        for (UINT32 i = 0; i < n; ++i) {
            if (!d->t) acts[i]->ActivateObject(IID_PPV_ARGS(&d->t));
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
    if (!d->t) CoCreateInstance(kMsH264Clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d->t));
    if (!d->t) return false;
    IMFAttributes* a = nullptr;
    if (SUCCEEDED(d->t->GetAttributes(&a)) && a) {
        a->SetUINT32(kLowLatency, TRUE);
        a->Release();
    }
    IMFMediaType* mt = nullptr;
    bool ok = false;
    if (SUCCEEDED(MFCreateMediaType(&mt))) {
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, kSubH264);
        mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ok = SUCCEEDED(d->t->SetInputType(0, mt, 0));
        mt->Release();
    }
    if (!ok) {
        SafeRelease(d->t);
        return false;
    }
    d->SetOutput();  // may fail until the first SPS arrives: done again on the stream change
    d->t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d->t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

void H264Decoder::Reset() {
    if (d->t) d->t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

bool H264Decoder::Decode(const uint8_t* data, size_t n, int64_t pts, std::vector<uint8_t>* outPx, int* ow, int* oh,
                         bool wantPicture) {
    if (!d->t || !data || !n) return false;
    IMFSample* in = nullptr;
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(MFCreateSample(&in)) || FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(n), &buf))) {
        SafeRelease(in);
        SafeRelease(buf);
        return false;
    }
    BYTE* p = nullptr;
    if (SUCCEEDED(buf->Lock(&p, nullptr, nullptr))) {
        std::memcpy(p, data, n);
        buf->Unlock();
    }
    buf->SetCurrentLength(static_cast<DWORD>(n));
    in->AddBuffer(buf);
    in->SetSampleTime(pts);
    in->SetSampleDuration(333333);
    buf->Release();

    bool got = false;
    auto drain = [&]() {
        for (int guard = 0; guard < 8; ++guard) {
            if (!d->outSet && !d->SetOutput()) return;
            MFT_OUTPUT_DATA_BUFFER ob{};
            IMFSample* os = nullptr;
            if (!d->provides) {
                IMFMediaBuffer* mb = nullptr;
                const DWORD sz = d->outSize ? d->outSize : d->fw * d->fh * 3 / 2 + 4096;
                if (FAILED(MFCreateSample(&os)) || FAILED(MFCreateMemoryBuffer(sz, &mb))) {
                    SafeRelease(os);
                    SafeRelease(mb);
                    return;
                }
                os->AddBuffer(mb);
                mb->Release();
                ob.pSample = os;
            }
            DWORD status = 0;
            const HRESULT hr = d->t->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                SafeRelease(os);
                if (ob.pSample && ob.pSample != os) ob.pSample->Release();
                return;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                SafeRelease(os);
                if (ob.pSample && ob.pSample != os) ob.pSample->Release();
                d->SetOutput();
                continue;
            }
            if (FAILED(hr)) {
                ++errors;
                SafeRelease(os);
                if (ob.pSample && ob.pSample != os) ob.pSample->Release();
                return;
            }
            errors = 0;
            IMFSample* res = ob.pSample;
            if (res && wantPicture && d->vw && d->vh) {
                IMFMediaBuffer* mb = nullptr;
                if (SUCCEEDED(res->ConvertToContiguousBuffer(&mb))) {
                    IMF2DBuffer* b2 = nullptr;
                    BYTE* scan0 = nullptr;
                    LONG pitch = 0;
                    BYTE* raw = nullptr;
                    DWORD len = 0;
                    bool l2 = false, l1 = false;
                    if (SUCCEEDED(mb->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
                        l2 = true;
                    } else if (SUCCEEDED(mb->Lock(&raw, nullptr, &len))) {
                        l1 = true;
                        scan0 = raw;
                        pitch = d->stride > 0 ? d->stride : static_cast<LONG>(d->fw);
                        if (len < static_cast<DWORD>(pitch) * d->fh * 3 / 2) scan0 = nullptr;
                    }
                    if (scan0 && pitch > 0) {
                        const int w = static_cast<int>(d->vw) & ~1, h = static_cast<int>(d->vh) & ~1;
                        outPx->resize(static_cast<size_t>(w) * h * 4);
                        Nv12ToBgraFast(scan0, pitch, scan0 + static_cast<size_t>(pitch) * d->fh, pitch, w, h, outPx->data(), w * 4, false,
                                       false);
                        *ow = w;
                        *oh = h;
                        got = true;
                    }
                    if (l2) b2->Unlock2D();
                    if (l1) mb->Unlock();
                    SafeRelease(b2);
                    mb->Release();
                }
            }
            if (res) res->Release();
        }
    };
    HRESULT hr = d->t->ProcessInput(0, in, 0);
    if (hr == MF_E_NOTACCEPTING) {
        drain();
        hr = d->t->ProcessInput(0, in, 0);
    }
    if (FAILED(hr)) ++errors;
    in->Release();
    drain();
    return got;
}
