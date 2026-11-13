// WVC 0.13: fast picture helpers used on every frame — a small thread pool, an antialiased separable resampler
// (triangle filter that widens when shrinking, so no aliasing; replaces GDI HALFTONE StretchBlt), rotate/mirror
// copies, masked blending with cached opaque spans, YUV (NV12 / YUY2) -> BGRA, BGRA -> BGR24 for the camera.
#include "common.h"

// ---------------------------------------------------------------------------
// Thread pool
// ---------------------------------------------------------------------------
namespace {
class Pool {
public:
    Pool() {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) hc = 2;
        nWorkers = static_cast<int>((std::min)(7u, hc > 1 ? hc - 1 : 0u));
        for (int i = 0; i < nWorkers; ++i) th.emplace_back([this] { Work(); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lk(mx);
            quit = true;
        }
        cv.notify_all();
        for (auto& t : th) t.join();
    }
    int Workers() const { return nWorkers; }
    // runs fn over [0, n) in chunks; the caller works too. Only one job at a time: a second caller runs inline.
    void Run(int n, int chunk, const std::function<void(int, int)>& fn) {
        std::unique_lock<std::mutex> job(jobMx, std::try_to_lock);
        if (!job.owns_lock() || nWorkers == 0 || n <= chunk) {
            fn(0, n);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mx);
            f = &fn;
            total = n;
            step = chunk;
            next = 0;
            pending = 0;
            ++gen;
        }
        cv.notify_all();
        Drain();
        std::unique_lock<std::mutex> lk(mx);
        doneCv.wait(lk, [&] { return next >= total && pending == 0; });
        f = nullptr;
    }

private:
    void Drain() {
        for (;;) {
            int a;
            const std::function<void(int, int)>* fn;
            {
                std::lock_guard<std::mutex> lk(mx);
                if (!f || next >= total) return;
                a = next;
                next += step;
                ++pending;
                fn = f;
            }
            (*fn)(a, (std::min)(a + step, total));
            {
                std::lock_guard<std::mutex> lk(mx);
                --pending;
                if (next >= total && pending == 0) doneCv.notify_all();
            }
        }
    }
    void Work() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mx);
                cv.wait(lk, [&] { return quit || (gen != seen && f && next < total); });
                if (quit) return;
                seen = gen;
            }
            Drain();
        }
    }
    std::vector<std::thread> th;
    std::mutex mx, jobMx;
    std::condition_variable cv, doneCv;
    const std::function<void(int, int)>* f = nullptr;
    int total = 0, step = 1, next = 0, pending = 0, nWorkers = 0;
    uint64_t gen = 0;
    bool quit = false;
};

Pool& ThePool() {
    static Pool p;
    return p;
}
}  // namespace

void ParallelFor(int n, const std::function<void(int, int)>& fn, int minChunk) {
    if (n <= 0) return;
    const int parts = (ThePool().Workers() + 1) * 3;
    const int chunk = (std::max)(minChunk, (n + parts - 1) / parts);
    ThePool().Run(n, chunk, fn);
}

// ---------------------------------------------------------------------------
// Resampler
// ---------------------------------------------------------------------------
namespace {
struct Taps {
    std::vector<int> start, count;
    std::vector<int16_t> w;
    int maxTaps = 1;
};

// out pixel u covers source [s0 + u*sc, s0 + (u+1)*sc); triangle filter of half-width max(1, sc)
void BuildTaps(double s0, double s1, int outN, int srcN, Taps& t) {
    const double sc = (s1 - s0) / outN;
    const double support = (std::max)(1.0, sc);
    t.maxTaps = static_cast<int>(std::ceil(support * 2)) + 2;
    t.start.assign(static_cast<size_t>(outN), 0);
    t.count.assign(static_cast<size_t>(outN), 1);
    t.w.assign(static_cast<size_t>(outN) * t.maxTaps, 0);
    double wf[64];
    const int cap = (std::min)(t.maxTaps, 64);
    for (int u = 0; u < outN; ++u) {
        const double c = s0 + (u + 0.5) * sc - 0.5;
        int lo = static_cast<int>(std::floor(c - support)) + 1;
        int hi = static_cast<int>(std::floor(c + support));
        lo = (std::max)(lo, 0);
        hi = (std::min)(hi, srcN - 1);
        if (hi - lo + 1 > cap) {
            const int mid = static_cast<int>(std::lround(c));
            lo = (std::max)(0, mid - cap / 2);
            hi = (std::min)(srcN - 1, lo + cap - 1);
        }
        double sum = 0;
        int n = 0;
        if (lo <= hi) {
            for (int i = lo; i <= hi; ++i) {
                const double v = (std::max)(0.0, 1.0 - std::fabs(c - i) / support);
                wf[n++] = v;
                sum += v;
            }
        }
        int16_t* w = &t.w[static_cast<size_t>(u) * t.maxTaps];
        if (n == 0 || sum <= 1e-9) {
            t.start[static_cast<size_t>(u)] = std::clamp(static_cast<int>(std::lround(c)), 0, srcN - 1);
            t.count[static_cast<size_t>(u)] = 1;
            w[0] = 16384;
            continue;
        }
        int acc = 0, big = 0;
        for (int k = 0; k < n; ++k) {
            w[k] = static_cast<int16_t>(std::lround(wf[k] / sum * 16384));
            acc += w[k];
            if (w[k] > w[big]) big = k;
        }
        w[big] = static_cast<int16_t>(w[big] + (16384 - acc));
        t.start[static_cast<size_t>(u)] = lo;
        t.count[static_cast<size_t>(u)] = n;
    }
}

inline uint8_t Sat14(int v) {
    v = (v + 8192) >> 14;
    return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

thread_local std::vector<uint8_t> tlTmp;
thread_local Taps tlTx, tlTy;
}  // namespace

void ResampleBgra(const uint8_t* src, int srcStride, int srcW, int srcH, double sx0, double sy0, double sx1, double sy1,
                  uint8_t* dst, int dstStride, int outW, int outH, bool mirror) {
    if (!src || !dst || outW < 1 || outH < 1 || srcW < 1 || srcH < 1) return;
    if (sx1 - sx0 < 1e-6) sx1 = sx0 + 1e-6;
    if (sy1 - sy0 < 1e-6) sy1 = sy0 + 1e-6;
    // exact 1:1 copy (no filtering needed)
    const double ix = std::round(sx0), iy = std::round(sy0);
    if (!mirror && std::fabs(sx0 - ix) < 1e-6 && std::fabs(sy0 - iy) < 1e-6 && std::fabs(sx1 - sx0 - outW) < 1e-6 &&
        std::fabs(sy1 - sy0 - outH) < 1e-6 && ix >= 0 && iy >= 0 && ix + outW <= srcW && iy + outH <= srcH) {
        const int x0 = static_cast<int>(ix), y0 = static_cast<int>(iy);
        ParallelFor(outH, [&](int a, int b) {
            for (int y = a; y < b; ++y)
                std::memcpy(dst + static_cast<ptrdiff_t>(y) * dstStride,
                            src + static_cast<ptrdiff_t>(y + y0) * srcStride + static_cast<ptrdiff_t>(x0) * 4,
                            static_cast<size_t>(outW) * 4);
        }, 32);
        return;
    }
    Taps& tx = tlTx;
    Taps& ty = tlTy;
    BuildTaps(sx0, sx1, outW, srcW, tx);
    BuildTaps(sy0, sy1, outH, srcH, ty);
    int ry0 = srcH, ry1 = 0;
    for (int v = 0; v < outH; ++v) {
        ry0 = (std::min)(ry0, ty.start[static_cast<size_t>(v)]);
        ry1 = (std::max)(ry1, ty.start[static_cast<size_t>(v)] + ty.count[static_cast<size_t>(v)]);
    }
    if (ry1 <= ry0) return;
    const int rows = ry1 - ry0;
    const size_t rowBytes = static_cast<size_t>(outW) * 4;
    std::vector<uint8_t>& tmp = tlTmp;
    if (tmp.size() < rowBytes * rows) tmp.resize(rowBytes * rows);
    uint8_t* tp = tmp.data();
    const Taps* TX = &tx;
    // horizontal pass: needed source rows -> tmp (outW wide)
    ParallelFor(rows, [&](int a, int b) {
        for (int r = a; r < b; ++r) {
            const uint8_t* s = src + static_cast<ptrdiff_t>(ry0 + r) * srcStride;
            uint8_t* o = tp + rowBytes * r;
            for (int u = 0; u < outW; ++u) {
                const int st = TX->start[static_cast<size_t>(u)], n = TX->count[static_cast<size_t>(u)];
                const int16_t* w = &TX->w[static_cast<size_t>(u) * TX->maxTaps];
                const uint8_t* q = s + static_cast<size_t>(st) * 4;
                int b0 = 0, g0 = 0, r0 = 0;
                for (int k = 0; k < n; ++k, q += 4) {
                    b0 += q[0] * w[k];
                    g0 += q[1] * w[k];
                    r0 += q[2] * w[k];
                }
                uint8_t* d = o + static_cast<size_t>(mirror ? outW - 1 - u : u) * 4;
                d[0] = Sat14(b0);
                d[1] = Sat14(g0);
                d[2] = Sat14(r0);
                d[3] = 255;
            }
        }
    }, 8);
    // vertical pass
    const Taps* TY = &ty;
    ParallelFor(outH, [&](int a, int b) {
        std::vector<int> acc(rowBytes);
        for (int v = a; v < b; ++v) {
            const int st = TY->start[static_cast<size_t>(v)], n = TY->count[static_cast<size_t>(v)];
            const int16_t* w = &TY->w[static_cast<size_t>(v) * TY->maxTaps];
            uint8_t* o = dst + static_cast<ptrdiff_t>(v) * dstStride;
            if (n == 1) {
                std::memcpy(o, tp + rowBytes * (st - ry0), rowBytes);
                continue;
            }
            std::fill(acc.begin(), acc.end(), 0);
            for (int k = 0; k < n; ++k) {
                const uint8_t* q = tp + rowBytes * (st - ry0 + k);
                const int wk = w[k];
                int* ap = acc.data();
                for (size_t x = 0; x < rowBytes; ++x) ap[x] += q[x] * wk;
            }
            for (size_t x = 0; x < rowBytes; ++x) o[x] = Sat14(acc[x]);
        }
    }, 8);
}

// canvas (CW x CH) -> TW x TH with clockwise rotation and horizontal mirror (as shown)
void TransformCopy(const uint8_t* cv, int CW, int CH, uint8_t* dst, int dstStride, int TW, int TH, int rot, bool mirror) {
    (void)CH;
    ParallelFor(TH, [&](int a, int b) {
        for (int y = a; y < b; ++y) {
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
            const uint32_t* p = reinterpret_cast<const uint32_t*>(cv) + static_cast<ptrdiff_t>(cy) * CW + cx;
            uint32_t* o = reinterpret_cast<uint32_t*>(dst + static_cast<ptrdiff_t>(y) * dstStride);
            if (step == 1)
                std::memcpy(o, p, static_cast<size_t>(TW) * 4);
            else
                for (int x = 0; x < TW; ++x, p += step) o[x] = *p;
        }
    }, 16);
}

// per mask row: [l, r) = the run of fully opaque (255) pixels that starts at the first opaque one
void BuildMaskSpans(const uint8_t* m, int w, int h, std::vector<int>& spans) {
    spans.assign(static_cast<size_t>(h) * 2, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = m + static_cast<size_t>(y) * w;
        int l = 0;
        while (l < w && row[l] != 255) ++l;
        int r = l;
        while (r < w && row[r] == 255) ++r;
        spans[static_cast<size_t>(y) * 2] = l;
        spans[static_cast<size_t>(y) * 2 + 1] = r;
    }
}

static inline uint8_t Mix(int s, int d, int a) {
    const int v = s * a + d * (255 - a) + 128;
    return static_cast<uint8_t>((v + (v >> 8)) >> 8);
}

void BlendMaskedRows(uint8_t* dst, int dstStride, const uint8_t* src, int srcStride, int w, int h, const uint8_t* mask,
                     int mstride, const int* spans, int spanX0, int opacity) {
    ParallelFor(h, [&](int a, int b) {
        for (int y = a; y < b; ++y) {
            const uint8_t* sp = src + static_cast<ptrdiff_t>(y) * srcStride;
            uint8_t* dp = dst + static_cast<ptrdiff_t>(y) * dstStride;
            const uint8_t* mp = mask ? mask + static_cast<ptrdiff_t>(y) * mstride : nullptr;
            int l = 0, r = 0;
            if (mp && spans && opacity >= 255) {
                l = std::clamp(spans[static_cast<size_t>(y) * 2] - spanX0, 0, w);
                r = std::clamp(spans[static_cast<size_t>(y) * 2 + 1] - spanX0, l, w);
            } else if (!mp && opacity >= 255) {
                l = 0;
                r = w;
            }
            if (r > l) std::memcpy(dp + static_cast<size_t>(l) * 4, sp + static_cast<size_t>(l) * 4, static_cast<size_t>(r - l) * 4);
            auto part = [&](int x0, int x1) {
                for (int x = x0; x < x1; ++x) {
                    int al = mp ? mp[x] : 255;
                    if (opacity < 255) al = al * opacity / 255;
                    if (al == 0) continue;
                    const uint8_t* s = sp + static_cast<size_t>(x) * 4;
                    uint8_t* d = dp + static_cast<size_t>(x) * 4;
                    if (al == 255) {
                        d[0] = s[0];
                        d[1] = s[1];
                        d[2] = s[2];
                    } else {
                        d[0] = Mix(s[0], d[0], al);
                        d[1] = Mix(s[1], d[1], al);
                        d[2] = Mix(s[2], d[2], al);
                    }
                }
            };
            part(0, l);
            part(r, w);
        }
    }, 16);
}

// ---------------------------------------------------------------------------
// YUV -> BGRA
// ---------------------------------------------------------------------------
namespace {
struct YuvK {
    int y, yoff, rv, gu, gv, bu;  // x 1024
};
YuvK KFor(bool bt709, bool full) {
    if (full) return bt709 ? YuvK{1024, 0, 1613, -192, -479, 1900} : YuvK{1024, 0, 1436, -352, -731, 1815};
    return bt709 ? YuvK{1192, 16, 1836, -218, -546, 2163} : YuvK{1192, 16, 1634, -401, -833, 2066};
}
inline uint8_t C10(int v) {
    v = (v + 512) >> 10;
    return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}
}  // namespace

void Nv12ToBgraFast(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int w, int h, uint8_t* out, int outStride,
                    bool bt709, bool fullRange) {
    const YuvK k = KFor(bt709, fullRange);
    ParallelFor(h, [&](int a, int b) {
        for (int r = a; r < b; ++r) {
            const uint8_t* yr = y + static_cast<ptrdiff_t>(r) * yPitch;
            const uint8_t* cr = uv + static_cast<ptrdiff_t>(r / 2) * uvPitch;
            uint8_t* o = out + static_cast<ptrdiff_t>(r) * outStride;
            for (int c = 0; c < w; c += 2) {
                const int u = cr[c] - 128, v = cr[c + 1] - 128;
                const int ro = k.rv * v, go = k.gu * u + k.gv * v, bo = k.bu * u;
                for (int q = 0; q < 2 && c + q < w; ++q) {
                    const int yy = k.y * (yr[c + q] - k.yoff);
                    o[0] = C10(yy + bo);
                    o[1] = C10(yy + go);
                    o[2] = C10(yy + ro);
                    o[3] = 255;
                    o += 4;
                }
            }
        }
    }, 16);
}

void Yuy2ToBgra(const uint8_t* src, int pitch, int w, int h, uint8_t* out, int outStride, bool bt709, bool fullRange) {
    const YuvK k = KFor(bt709, fullRange);
    ParallelFor(h, [&](int a, int b) {
        for (int r = a; r < b; ++r) {
            const uint8_t* s = src + static_cast<ptrdiff_t>(r) * pitch;
            uint8_t* o = out + static_cast<ptrdiff_t>(r) * outStride;
            for (int c = 0; c + 1 < w; c += 2, s += 4) {
                const int u = s[1] - 128, v = s[3] - 128;
                const int ro = k.rv * v, go = k.gu * u + k.gv * v, bo = k.bu * u;
                for (int q = 0; q < 2; ++q) {
                    const int yy = k.y * (s[q * 2] - k.yoff);
                    o[0] = C10(yy + bo);
                    o[1] = C10(yy + go);
                    o[2] = C10(yy + ro);
                    o[3] = 255;
                    o += 4;
                }
            }
        }
    }, 16);
}

void CopyBgrxOpaque(const uint8_t* src, int pitch, int w, int h, uint8_t* out, int outStride) {
    ParallelFor(h, [&](int a, int b) {
        for (int r = a; r < b; ++r) {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(src + static_cast<ptrdiff_t>(r) * pitch);
            uint32_t* o = reinterpret_cast<uint32_t*>(out + static_cast<ptrdiff_t>(r) * outStride);
            for (int x = 0; x < w; ++x) o[x] = s[x] | 0xFF000000u;
        }
    }, 32);
}

void PackBgraToBgr(const uint8_t* p, uint8_t* dst, int w, int h) {
    ParallelFor(h, [&](int a, int b) {
        for (int r = a; r < b; ++r) {
            const uint8_t* s = p + static_cast<size_t>(r) * w * 4;
            uint8_t* d = dst + static_cast<size_t>(r) * w * 3;
            int x = 0;
            for (; x + 4 <= w; x += 4, s += 16, d += 12) {
                uint32_t a0, a1, a2, a3;
                std::memcpy(&a0, s, 4);
                std::memcpy(&a1, s + 4, 4);
                std::memcpy(&a2, s + 8, 4);
                std::memcpy(&a3, s + 12, 4);
                const uint32_t o0 = (a0 & 0xFFFFFF) | (a1 << 24);
                const uint32_t o1 = ((a1 >> 8) & 0xFFFF) | (a2 << 16);
                const uint32_t o2 = ((a2 >> 16) & 0xFF) | (a3 << 8);
                std::memcpy(d, &o0, 4);
                std::memcpy(d + 4, &o1, 4);
                std::memcpy(d + 8, &o2, 4);
            }
            for (; x < w; ++x, s += 4, d += 3) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
        }
    }, 32);
}
