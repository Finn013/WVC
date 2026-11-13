// WVC 0.12: one transition state machine (used by the render thread only).
// Frames are BGRA top-down, W x H. Begin() takes a copy of what is on screen right now (also when a transition is
// still running: it restarts from the blended picture), Step() returns the picture to show for the new frame.
// While the target scene's live sources have no picture yet, the old picture is held (at most 1.5 s).
#include "common.h"

const wchar_t* const kTransNames[TR_COUNT] = {
    L"Без перехода (сразу)", L"Растворение", L"Через чёрный", L"Шторка", L"Сдвиг (толкает)",
    L"Мягкая шторка", L"Круг (диафрагма)", L"Часы", L"Масштаб", L"Размытие",
    L"Наплыв (поверх)", L"Жалюзи", L"Ромб", L"Вспышка (через белый)"};

bool TransUsesDir(int kind) {
    return kind == TR_WIPE || kind == TR_PUSH || kind == TR_SOFTWIPE || kind == TR_SLIDE || kind == TR_BLINDS;
}

namespace {
inline uint8_t Lerp8(int a, int b, int k) {  // k 0..256
    return static_cast<uint8_t>(a + (((b - a) * k) >> 8));
}

void MixAll(const uint8_t* a, const uint8_t* b, uint8_t* o, size_t n, int k) {
    for (size_t i = 0; i < n; ++i) o[i] = Lerp8(a[i], b[i], k);
}

double Clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
}  // namespace

void TransEngine::Reset() {
    active = false;
    started = false;
}

void TransEngine::Begin(const uint8_t* cur, int W, int H, int k, int durMs, int d, int e) {
    const size_t n = static_cast<size_t>(W) * H * 4;
    kind = k;
    ms = durMs;
    dir = d;
    ease = e;
    if (k == TR_NONE || durMs <= 0) {  // a cut: nothing to hold
        active = false;
        return;
    }
    if (cur == from.data() && from.size() == n) {
        // restarting while the old picture is still held: keep it
    } else if (cur)
        from.assign(cur, cur + n);
    else
        from.assign(n, 0);
    for (size_t i = 3; i < n; i += 4) from[i] = 255;
    fw = W;
    fh = H;
    active = true;
    started = false;
    begun = GetTickCount64();
    LogMsg(L"Переход: «" + std::wstring(kTransNames[std::clamp(k, 0, TR_COUNT - 1)]) + L"», " + std::to_wstring(durMs) +
           L" мс");
}

void TransEngine::MaskBlend(const uint8_t* a, const uint8_t* b, uint8_t* o, int W, int H,
                            const std::function<double(double, double)>& f) {
    // alpha of the new picture on a quarter-resolution grid (soft edges hide the steps)
    const int gw = (W + 3) / 4, gh = (H + 3) / 4;
    grid.resize(static_cast<size_t>(gw) * gh);
    for (int gy = 0; gy < gh; ++gy)
        for (int gx = 0; gx < gw; ++gx) {
            const double x = (gx * 4 + 2) / static_cast<double>(W), y = (gy * 4 + 2) / static_cast<double>(H);
            grid[static_cast<size_t>(gy) * gw + gx] = static_cast<uint16_t>(std::lround(Clamp01(f(x, y)) * 256));
        }
    for (int y = 0; y < H; ++y) {
        const uint16_t* gr = &grid[static_cast<size_t>(y >> 2) * gw];
        const size_t row = static_cast<size_t>(y) * W * 4;
        for (int x = 0; x < W; ++x) {
            const int k = gr[x >> 2];
            const size_t i = row + static_cast<size_t>(x) * 4;
            if (k == 0) {
                o[i] = a[i];
                o[i + 1] = a[i + 1];
                o[i + 2] = a[i + 2];
            } else if (k >= 256) {
                o[i] = b[i];
                o[i + 1] = b[i + 1];
                o[i + 2] = b[i + 2];
            } else {
                o[i] = Lerp8(a[i], b[i], k);
                o[i + 1] = Lerp8(a[i + 1], b[i + 1], k);
                o[i + 2] = Lerp8(a[i + 2], b[i + 2], k);
            }
            o[i + 3] = 255;
        }
    }
}

// picture `src` moved by (dx, dy) px into o (rows / columns outside are left as they are)
static void Shifted(const uint8_t* src, uint8_t* o, int W, int H, int dx, int dy) {
    const size_t row = static_cast<size_t>(W) * 4;
    for (int y = 0; y < H; ++y) {
        const int sy = y - dy;
        if (sy < 0 || sy >= H) continue;
        int x0 = dx, sx0 = 0, n = W;
        if (x0 < 0) {
            sx0 = -x0;
            n -= sx0;
            x0 = 0;
        }
        if (x0 + n > W) n = W - x0;
        if (n <= 0) continue;
        std::memcpy(o + static_cast<size_t>(y) * row + static_cast<size_t>(x0) * 4,
                    src + static_cast<size_t>(sy) * row + static_cast<size_t>(sx0) * 4, static_cast<size_t>(n) * 4);
    }
}

const uint8_t* TransEngine::Step(const uint8_t* to, int W, int H, bool ready, HDC ref) {
    if (!active) return to;
    if (W != fw || H != fh) {
        active = false;
        return to;
    }
    const ULONGLONG now = GetTickCount64();
    if (!started) {
        if (ready || now - begun > 1500) {
            started = true;
            t0 = now;
        } else {
            return from.data();  // the new scene's sources are still opening: hold the old picture
        }
    }
    const double t = static_cast<double>(now - t0) / (std::max)(1, ms);
    if (t >= 1.0) {
        active = false;
        return to;
    }
    const double e = ease == 0 ? t : t * t * (3 - 2 * t);
    const size_t n = static_cast<size_t>(W) * H * 4;
    if (!out.Ensure(ref, W, H)) {
        active = false;
        return to;
    }
    uint8_t* o = out.bits;
    const uint8_t* a = from.data();
    const uint8_t* b = to;
    // u: 0..1 along the direction of travel (the new picture appears where u < progress)
    auto along = [this](double x, double y) {
        switch (dir) {
        case 0: return 1 - x;  // ← the edge travels to the left: the new picture comes from the right
        case 1: return x;      // →
        case 2: return 1 - y;  // ↑
        default: return y;     // ↓
        }
    };
    switch (kind) {
    case TR_DISSOLVE: MixAll(a, b, o, n, static_cast<int>(e * 256)); break;
    case TR_BLACK:
    case TR_FLASH: {
        const uint8_t c = kind == TR_FLASH ? 255 : 0;
        std::vector<uint8_t>& tmp = scratch;
        tmp.assign(n, c);
        if (e < 0.5)
            MixAll(a, tmp.data(), o, n, static_cast<int>(e * 2 * 256));
        else
            MixAll(tmp.data(), b, o, n, static_cast<int>((e * 2 - 1) * 256));
        break;
    }
    case TR_WIPE:
        MaskBlend(a, b, o, W, H, [&](double x, double y) { return along(x, y) < e ? 1.0 : 0.0; });
        break;
    case TR_SOFTWIPE: {
        const double s = 0.18;
        MaskBlend(a, b, o, W, H, [&](double x, double y) { return (e * (1 + s) - along(x, y)) / s; });
        break;
    }
    case TR_PUSH:
    case TR_SLIDE: {
        int dx = 0, dy = 0;  // where the new picture's top-left lands
        switch (dir) {
        case 0: dx = static_cast<int>(std::lround(W * (1 - e))); break;
        case 1: dx = -static_cast<int>(std::lround(W * (1 - e))); break;
        case 2: dy = static_cast<int>(std::lround(H * (1 - e))); break;
        default: dy = -static_cast<int>(std::lround(H * (1 - e))); break;
        }
        if (kind == TR_PUSH) {
            const int ox = dx == 0 ? 0 : (dx > 0 ? dx - W : dx + W), oy = dy == 0 ? 0 : (dy > 0 ? dy - H : dy + H);
            Shifted(a, o, W, H, ox, oy);
        } else {
            std::memcpy(o, a, n);
        }
        Shifted(b, o, W, H, dx, dy);
        break;
    }
    case TR_CIRCLE: {
        const double ar = static_cast<double>(W) / H;
        const double R = std::sqrt(ar * ar / 4 + 0.25) * 1.02;
        const double s = 0.03;
        MaskBlend(a, b, o, W, H, [&](double x, double y) {
            const double dx = (x - 0.5) * ar, dy = y - 0.5;
            const double d = std::sqrt(dx * dx + dy * dy);
            return (e * (R + s) - d) / s;
        });
        break;
    }
    case TR_CLOCK: {
        const double ar = static_cast<double>(W) / H, s = 0.015;
        MaskBlend(a, b, o, W, H, [&](double x, double y) {
            double ang = std::atan2((x - 0.5) * ar, 0.5 - y) / (2 * 3.14159265358979);  // 0 at 12 o'clock, clockwise
            if (ang < 0) ang += 1;
            return (e * (1 + s) - ang) / s;
        });
        break;
    }
    case TR_DIAMOND: {
        const double s = 0.06;
        MaskBlend(a, b, o, W, H, [&](double x, double y) {
            const double d = std::fabs(x - 0.5) + std::fabs(y - 0.5);  // 0..1
            return (e * (1 + s) - d) / s;
        });
        break;
    }
    case TR_BLINDS: {
        const int slats = 10;
        const double s = 0.15;
        MaskBlend(a, b, o, W, H, [&](double x, double y) {
            double u = along(x, y) * slats;
            u -= std::floor(u);
            return (e * (1 + s) - u) / s;
        });
        break;
    }
    case TR_ZOOM: {
        // the old picture grows and fades, the new one settles from a little larger
        if (!sa.Ensure(ref, W, H) || !sb.Ensure(ref, W, H)) {
            MixAll(a, b, o, n, static_cast<int>(e * 256));
            break;
        }
        std::memcpy(sa.bits, a, n);
        const double za = 1 + e * 0.6;
        const int cw = static_cast<int>(W / za), ch = static_cast<int>(H / za);
        SetStretchBltMode(sb.dc, COLORONCOLOR);
        StretchBlt(sb.dc, 0, 0, W, H, sa.dc, (W - cw) / 2, (H - ch) / 2, cw, ch, SRCCOPY);
        std::memcpy(sa.bits, b, n);
        const double zb = 1 + (1 - e) * 0.25;
        const int bw = static_cast<int>(W / zb), bh = static_cast<int>(H / zb);
        SetStretchBltMode(out.dc, COLORONCOLOR);
        StretchBlt(out.dc, 0, 0, W, H, sa.dc, (W - bw) / 2, (H - bh) / 2, bw, bh, SRCCOPY);
        GdiFlush();
        // o currently = zoomed new; mix the zoomed old over it
        MixAll(sb.bits, o, o, n, static_cast<int>(e * 256));
        break;
    }
    case TR_BLUR: {
        MixAll(a, b, o, n, static_cast<int>(e * 256));
        const double amt = std::sin(e * 3.14159265358979);
        if (amt > 0.03) {
            const double f = 1 + amt * 23;
            const int sw = (std::max)(2, static_cast<int>(W / f)), sh = (std::max)(2, static_cast<int>(H / f));
            if (tiny.Ensure(ref, sw, sh)) {
                SetStretchBltMode(tiny.dc, HALFTONE);
                SetBrushOrgEx(tiny.dc, 0, 0, nullptr);
                StretchBlt(tiny.dc, 0, 0, sw, sh, out.dc, 0, 0, W, H, SRCCOPY);
                SetStretchBltMode(out.dc, HALFTONE);
                SetBrushOrgEx(out.dc, 0, 0, nullptr);
                StretchBlt(out.dc, 0, 0, W, H, tiny.dc, 0, 0, sw, sh, SRCCOPY);
                GdiFlush();
            }
        }
        break;
    }
    default: MixAll(a, b, o, n, static_cast<int>(e * 256)); break;
    }
    for (size_t i = 3; i < n; i += 4) o[i] = 255;
    return o;
}
