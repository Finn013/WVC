// WVC: the compositor. Scene = items bottom -> top, each: source -> rotation / mirror -> crop -> zoom -> fit into
// its box -> shape + soft edge -> opacity. Output: crop (cover) + shape with a background, pause picture,
// transitions; the camera thread and the preview pictures for the UI.
#include "common.h"

Published g_pubProgram, g_pubNext, g_pubSolo, g_pubCanvas;
std::mutex g_soloMx;
Item g_soloItem;
std::atomic<int> g_fpsX10{0};
std::atomic<bool> g_resetCapture{false}, g_pauseDirty{false};
std::atomic<int> g_transGen{0};
std::atomic<bool> g_editingNext{false};
int g_camW = 0, g_camH = 0, g_camFps = 0;
bool g_camOk = false;

static const COLORREF kTextColors[8] = {RGB(255, 255, 255), RGB(255, 220, 0), RGB(0, 0, 0),     RGB(230, 40, 40),
                                        RGB(40, 200, 80),   RGB(60, 120, 255), RGB(255, 140, 0), RGB(0, 220, 230)};

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
RectD ItemSourceRect(const Item& it, double DW, double DH) {
    RectD r;
    r.x0 = DW * it.crop[0] / 1000.0;
    r.y0 = DH * it.crop[1] / 1000.0;
    r.x1 = DW * (1.0 - it.crop[2] / 1000.0);
    r.y1 = DH * (1.0 - it.crop[3] / 1000.0);
    if (r.x1 - r.x0 < 1) r.x1 = r.x0 + 1;
    if (r.y1 - r.y0 < 1) r.y1 = r.y0 + 1;
    const double z = std::clamp(it.zoom, 100, 400) / 100.0;
    if (z > 1.0) {
        const double nw = r.W() / z, nh = r.H() / z;
        const double cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
        r = RectD{cx - nw / 2, cy - nh / 2, cx + nw / 2, cy + nh / 2};
    }
    return r;
}

RectD ItemBox(const Item& it, double W, double H) {
    return RectD{it.x * W, it.y * H, (it.x + it.w) * W, (it.y + it.h) * H};
}

void ItemLayout(const Item& it, double W, double H, double DW, double DH, RectD* C, RectD* D) {
    const RectD d1 = ItemSourceRect(it, DW, DH);
    const RectD b = ItemBox(it, W, H);
    const double sw = d1.W(), sh = d1.H();
    if (it.fit == 0) {
        const double s = (std::min)(b.W() / sw, b.H() / sh);
        const double cw = sw * s, ch = sh * s;
        const double cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
        *C = RectD{cx - cw / 2, cy - ch / 2, cx + cw / 2, cy + ch / 2};
        *D = d1;
    } else if (it.fit == 1) {
        const double s = (std::max)(b.W() / sw, b.H() / sh);
        const double nw = b.W() / s, nh = b.H() / s;
        const double cx = (d1.x0 + d1.x1) / 2, cy = (d1.y0 + d1.y1) / 2;
        *C = b;
        *D = RectD{cx - nw / 2, cy - nh / 2, cx + nw / 2, cy + nh / 2};
    } else {
        *C = b;
        *D = d1;
    }
}

RectD DispToSrc(const RectD& d, int rot, bool mirror, double NW, double NH) {
    const double DW = (rot & 1) ? NH : NW;
    auto inv = [&](double x, double y, double* sx, double* sy) {
        if (mirror) x = DW - x;
        switch (rot & 3) {
        case 1: *sx = y; *sy = NH - x; break;
        case 2: *sx = NW - x; *sy = NH - y; break;
        case 3: *sx = NW - y; *sy = x; break;
        default: *sx = x; *sy = y; break;
        }
    };
    double ax, ay, bx, by;
    inv(d.x0, d.y0, &ax, &ay);
    inv(d.x1, d.y1, &bx, &by);
    return RectD{(std::min)(ax, bx), (std::min)(ay, by), (std::max)(ax, bx), (std::max)(ay, by)};
}

RectD OutputVisibleRect(const OutputCfg& o, double W, double H) {
    RectD r{W * o.crop[0] / 1000.0, H * o.crop[1] / 1000.0, W * (1 - o.crop[2] / 1000.0), H * (1 - o.crop[3] / 1000.0)};
    if (r.W() < 16) r.x1 = r.x0 + 16;
    if (r.H() < 16) r.y1 = r.y0 + 16;
    const double a = W / H;
    double vw = r.W(), vh = r.H();
    if (vw / vh > a) vw = vh * a;
    else vh = vw / a;
    const double cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
    return RectD{cx - vw / 2, cy - vh / 2, cx + vw / 2, cy + vh / 2};
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------
bool Surface::Ensure(HDC ref, int cw, int ch) {
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (bmp && cw == w && ch == h) return true;
    if (old && dc) SelectObject(dc, old);
    if (bmp) DeleteObject(bmp);
    old = nullptr;
    bmp = nullptr;
    bits = nullptr;
    w = h = 0;
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

void Surface::Clear(uint32_t v) {
    if (!bits) return;
    if (v == 0) {
        std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
        return;
    }
    uint32_t* p = reinterpret_cast<uint32_t*>(bits);
    std::fill(p, p + static_cast<size_t>(w) * h, v);
}

void Surface::Free() {
    if (old && dc) SelectObject(dc, old);
    if (bmp) DeleteObject(bmp);
    old = nullptr;
    bmp = nullptr;
    bits = nullptr;
    w = h = 0;
}

Surface::~Surface() {
    Free();
    if (dc) DeleteDC(dc);
}

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------
namespace {
const double kPi = 3.14159265358979;

struct ShapeGeo {
    int kind;
    double w, h, cx, cy, side, rr;
    double nx[8], ny[8], ap;
    int n;
    double starX[10], starY[10];
};

void ShapePrep(ShapeGeo& g, double w, double h, const ShapeDef& sd) {
    g.kind = sd.kind;
    g.w = w;
    g.h = h;
    g.cx = w / 2;
    g.cy = h / 2;
    g.side = (std::min)(w, h);
    g.rr = g.side * std::clamp(sd.radius, 0, 50) / 100.0;
    g.n = 0;
    g.ap = 0;
    if (sd.kind == SH_HEX || sd.kind == SH_OCT) {
        g.n = sd.kind == SH_HEX ? 6 : 8;
        const double rot = sd.kind == SH_HEX ? 0 : kPi / 8;
        for (int k = 0; k < g.n; ++k) {
            const double a = rot + (k + 0.5) * 2 * kPi / g.n;
            g.nx[k] = std::cos(a);
            g.ny[k] = std::sin(a);
        }
        g.ap = g.side / 2.0 * std::cos(kPi / g.n);
    }
    if (sd.kind == SH_STAR) {
        const double R = g.side / 2.0, r = R * 0.45;
        for (int k = 0; k < 10; ++k) {
            const double a = -kPi / 2 + k * kPi / 5;
            const double rad = (k & 1) ? r : R;
            g.starX[k] = g.cx + rad * std::cos(a);
            g.starY[k] = g.cy + rad * std::sin(a) + R * 0.1;
        }
    }
}

bool InPoly(const double* xs, const double* ys, int n, double px, double py) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((ys[i] > py) != (ys[j] > py) && px < (xs[j] - xs[i]) * (py - ys[i]) / (ys[j] - ys[i]) + xs[i]) in = !in;
    }
    return in;
}

bool InRound(double px, double py, double x0, double y0, double x1, double y1, double rr) {
    if (px < x0 || px > x1 || py < y0 || py > y1) return false;
    rr = (std::min)(rr, (std::min)(x1 - x0, y1 - y0) / 2);
    const double cx = std::clamp(px, x0 + rr, x1 - rr), cy = std::clamp(py, y0 + rr, y1 - rr);
    const double dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= rr * rr;
}

bool HeartIn(double u, double v) {
    const double a = u * u + v * v - 1;
    return a * a * a - u * u * v * v * v <= 0;
}

bool InShape(const ShapeGeo& g, double px, double py) {
    switch (g.kind) {
    case SH_ROUND: return InRound(px, py, 0, 0, g.w, g.h, g.rr);
    case SH_CIRCLE: {
        const double dx = px - g.cx, dy = py - g.cy, r = g.side / 2;
        return dx * dx + dy * dy <= r * r;
    }
    case SH_ELLIPSE: {
        const double dx = (px - g.cx) / (g.w / 2), dy = (py - g.cy) / (g.h / 2);
        return dx * dx + dy * dy <= 1;
    }
    case SH_HEX:
    case SH_OCT:
        for (int k = 0; k < g.n; ++k)
            if ((px - g.cx) * g.nx[k] + (py - g.cy) * g.ny[k] > g.ap) return false;
        return true;
    case SH_DIAMOND: return std::fabs(px - g.cx) / (g.w / 2) + std::fabs(py - g.cy) / (g.h / 2) <= 1;
    case SH_STAR: return InPoly(g.starX, g.starY, 10, px, py);
    case SH_HEART: {
        const double u = (px - g.cx) / (g.side / 2) * 1.14, v = (g.cy - py) / (g.side / 2) * 1.118 + 0.118;
        return HeartIn(u, v);
    }
    case SH_BUBBLE: {
        const double bh = g.h * 0.80;
        if (InRound(px, py, 0, 0, g.w, bh, (std::min)(g.w, bh) * 0.22)) return true;
        const double tx[3] = {g.w * 0.16, g.w * 0.36, g.w * 0.10}, ty[3] = {bh - 2, bh - 2, g.h};
        return InPoly(tx, ty, 3, px, py);
    }
    default: return true;
    }
}

bool LoadMaskImage(const std::wstring& path, int w, int h, std::vector<uint8_t>& m) {
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    IWICBitmapScaler* sc = nullptr;
    bool ok = false;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    if (!path.empty() &&
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
        SUCCEEDED(sc->Initialize(cv, static_cast<UINT>(w), static_cast<UINT>(h), WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(sc->CopyPixels(nullptr, static_cast<UINT>(w) * 4, static_cast<UINT>(px.size()), px.data()))) {
        m.resize(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < m.size(); ++i) {
            const uint8_t* p = &px[i * 4];
            const int lum = (p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8;
            m[i] = static_cast<uint8_t>(lum * p[3] / 255);
        }
        ok = true;
    }
    SafeRelease(sc);
    SafeRelease(cv);
    SafeRelease(fr);
    SafeRelease(dec);
    SafeRelease(f);
    return ok;
}

// soft edge: fade over `f` px towards the outside (chamfer distance to the nearest outside pixel)
void Feather(std::vector<uint8_t>& m, int w, int h, double f) {
    if (f < 1) return;
    std::vector<float> d(static_cast<size_t>(w) * h);
    const float big = 1e9f;
    for (size_t i = 0; i < d.size(); ++i) d[i] = m[i] >= 128 ? big : 0.f;
    auto at = [&](int x, int y) -> float& { return d[static_cast<size_t>(y) * w + x]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float& v = at(x, y);
            if (v == 0) continue;
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                v = (std::min)(v, 1.f);  // the box edge counts as outside
            }
            if (x > 0) v = (std::min)(v, at(x - 1, y) + 1.f);
            if (y > 0) v = (std::min)(v, at(x, y - 1) + 1.f);
            if (x > 0 && y > 0) v = (std::min)(v, at(x - 1, y - 1) + 1.4142f);
            if (x < w - 1 && y > 0) v = (std::min)(v, at(x + 1, y - 1) + 1.4142f);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            float& v = at(x, y);
            if (v == 0) continue;
            if (x < w - 1) v = (std::min)(v, at(x + 1, y) + 1.f);
            if (y < h - 1) v = (std::min)(v, at(x, y + 1) + 1.f);
            if (x < w - 1 && y < h - 1) v = (std::min)(v, at(x + 1, y + 1) + 1.4142f);
            if (x > 0 && y < h - 1) v = (std::min)(v, at(x - 1, y + 1) + 1.4142f);
        }
    for (size_t i = 0; i < m.size(); ++i) {
        const double k = (std::min)(1.0, d[i] / f);
        const double s = k * k * (3 - 2 * k);
        m[i] = static_cast<uint8_t>(std::lround(m[i] * s));
    }
}
}  // namespace

// Coverage mask (0..255) of the shape in a w x h box; 2x2 supersampled.
void BuildShapeMask(int w, int h, const ShapeDef& sd, std::vector<uint8_t>& m) {
    w = (std::max)(1, w);
    h = (std::max)(1, h);
    m.assign(static_cast<size_t>(w) * h, 255);
    if (sd.kind == SH_MASK) {
        if (!LoadMaskImage(sd.mask, w, h, m)) m.assign(static_cast<size_t>(w) * h, 255);
    } else if (sd.kind != SH_RECT) {
        ShapeGeo g;
        ShapePrep(g, w, h, sd);
        for (int y = 0; y < h; ++y) {
            uint8_t* row = m.data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                int inside = 0;
                for (int sy = 0; sy < 2; ++sy)
                    for (int sx = 0; sx < 2; ++sx) inside += InShape(g, x + 0.25 + sx * 0.5, y + 0.25 + sy * 0.5);
                row[x] = static_cast<uint8_t>(inside * 255 / 4);
            }
        }
    }
    if (sd.feather > 0) Feather(m, w, h, (std::min)(w, h) * sd.feather / 100.0);
}

void ShapeOutline(double w, double h, const ShapeDef& sd, std::vector<std::vector<POINT>>* polys) {
    polys->clear();
    auto P = [](double x, double y) { return POINT{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))}; };
    std::vector<POINT> v;
    auto roundRect = [&](double x0, double y0, double x1, double y1, double rr) {
        std::vector<POINT> r;
        rr = (std::min)(rr, (std::min)(x1 - x0, y1 - y0) / 2);
        const double cxs[4] = {x1 - rr, x0 + rr, x0 + rr, x1 - rr}, cys[4] = {y1 - rr, y1 - rr, y0 + rr, y0 + rr};
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k <= 8; ++k) {
                const double a = (c * 90 + k * 90.0 / 8) * kPi / 180;
                r.push_back(P(cxs[c] + rr * std::cos(a), cys[c] + rr * std::sin(a)));
            }
        return r;
    };
    const double cx = w / 2, cy = h / 2, side = (std::min)(w, h);
    switch (sd.kind) {
    case SH_ROUND: polys->push_back(roundRect(0, 0, w, h, side * sd.radius / 100.0)); return;
    case SH_CIRCLE:
    case SH_ELLIPSE: {
        const double rx = sd.kind == SH_CIRCLE ? side / 2 : w / 2, ry = sd.kind == SH_CIRCLE ? side / 2 : h / 2;
        for (int k = 0; k < 72; ++k) v.push_back(P(cx + rx * std::cos(k * kPi / 36), cy + ry * std::sin(k * kPi / 36)));
        break;
    }
    case SH_HEX:
    case SH_OCT: {
        const int n = sd.kind == SH_HEX ? 6 : 8;
        const double start = sd.kind == SH_HEX ? 0.0 : kPi / 8;
        const double R = side / 2.0;
        for (int k = 0; k < n; ++k) v.push_back(P(cx + R * std::cos(start + k * 2 * kPi / n), cy + R * std::sin(start + k * 2 * kPi / n)));
        break;
    }
    case SH_DIAMOND: v = {P(cx, 0), P(w, cy), P(cx, h), P(0, cy)}; break;
    case SH_STAR: {
        ShapeGeo g;
        ShapePrep(g, w, h, sd);
        for (int k = 0; k < 10; ++k) v.push_back(P(g.starX[k], g.starY[k]));
        break;
    }
    case SH_HEART: {
        const double k = (side / 2);
        for (int i = 0; i < 90; ++i) {
            const double a = i * 2 * kPi / 90;
            double lo = 0, hi = 2.0;
            for (int it = 0; it < 30; ++it) {
                const double mid = (lo + hi) / 2;
                if (HeartIn(mid * std::cos(a), mid * std::sin(a))) lo = mid;
                else hi = mid;
            }
            const double u = lo * std::cos(a), vv = lo * std::sin(a);
            v.push_back(P(cx + u / 1.14 * k, cy - (vv - 0.118) / 1.118 * k));
        }
        break;
    }
    case SH_BUBBLE: {
        const double bh = h * 0.80;
        polys->push_back(roundRect(0, 0, w, bh, (std::min)(w, bh) * 0.22));
        polys->push_back({P(w * 0.16, bh), P(w * 0.36, bh), P(w * 0.10, h)});
        return;
    }
    default: v = {P(0, 0), P(w, 0), P(w, h), P(0, h)}; break;
    }
    polys->push_back(v);
}

void DrawCheckerboard(HDC dc, const RECT& r, int cell) {
    HBRUSH a = CreateSolidBrush(RGB(204, 204, 204)), b = CreateSolidBrush(RGB(150, 150, 150));
    cell = (std::max)(4, cell);
    for (int y = r.top; y < r.bottom; y += cell)
        for (int x = r.left; x < r.right; x += cell) {
            RECT c{x, y, (std::min)(x + cell, static_cast<int>(r.right)), (std::min)(y + cell, static_cast<int>(r.bottom))};
            FillRect(dc, &c, (((x - r.left) / cell + (y - r.top) / cell) & 1) ? b : a);
        }
    DeleteObject(a);
    DeleteObject(b);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
static HFONT MakeTextFont(int px, bool bold) {
    return CreateFontW(-(std::max)(4, px), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void MeasureTextPx(const Item& it, double scale, int* w, int* h, int* pad) {
    const int px = (std::max)(4, static_cast<int>(std::lround(it.textPx * scale)));
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT f = MakeTextFont(px, it.textBold);
    HGDIOBJ o = SelectObject(dc, f);
    RECT r{0, 0, 100000, 100000};
    const std::wstring t = it.text.empty() ? L" " : it.text;
    DrawTextW(dc, t.c_str(), -1, &r, DT_CALCRECT | DT_NOPREFIX | DT_CENTER);
    SelectObject(dc, o);
    DeleteObject(f);
    DeleteDC(dc);
    *pad = (std::max)(2, px / 3);
    *w = (std::max)(1, static_cast<int>(r.right - r.left) + 2 * *pad);
    *h = (std::max)(1, static_cast<int>(r.bottom - r.top) + *pad);
}

void TextNativeSize(const Item& it, int* w, int* h) {
    int pad;
    MeasureTextPx(it, 1.0, w, h, &pad);
}

// premultiplied BGRA picture of the text at `scale`
static void RasterText(const Item& it, double scale, std::vector<uint8_t>& out, int* ow, int* oh) {
    int w, h, pad;
    MeasureTextPx(it, scale, &w, &h, &pad);
    const int px = (std::max)(4, static_cast<int>(std::lround(it.textPx * scale)));
    HDC ref = GetDC(nullptr);
    Surface s;
    s.Ensure(ref, w, h);
    ReleaseDC(nullptr, ref);
    s.Clear();
    HFONT f = MakeTextFont(px, it.textBold);
    HGDIOBJ o = SelectObject(s.dc, f);
    SetBkMode(s.dc, TRANSPARENT);
    SetTextColor(s.dc, RGB(255, 255, 255));
    RECT r{pad, pad / 2, w - pad, h - pad / 2};
    const std::wstring t = it.text.empty() ? L" " : it.text;
    DrawTextW(s.dc, t.c_str(), -1, &r, DT_NOPREFIX | DT_CENTER);
    GdiFlush();
    SelectObject(s.dc, o);
    DeleteObject(f);
    const COLORREF c = kTextColors[std::clamp(it.textColor, 0, 7)];
    const int cb = GetBValue(c), cg = GetGValue(c), cr = GetRValue(c);
    out.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const uint8_t* p = s.bits + i * 4;
        const int a = (std::max)(p[0], (std::max)(p[1], p[2]));
        uint8_t* d = &out[i * 4];
        const int bgA = it.textBg ? 153 : 0;
        const int oa = a + bgA * (255 - a) / 255;
        d[0] = static_cast<uint8_t>(cb * a / 255);
        d[1] = static_cast<uint8_t>(cg * a / 255);
        d[2] = static_cast<uint8_t>(cr * a / 255);
        d[3] = static_cast<uint8_t>(oa);
    }
    *ow = w;
    *oh = h;
}

// ---------------------------------------------------------------------------
// Native sizes seen by the renderer (for the UI)
// ---------------------------------------------------------------------------
static std::mutex g_natMx;
static std::map<int, std::pair<int, int>> g_nat;

bool NativeSize(int uid, int* w, int* h) {
    std::lock_guard<std::mutex> lk(g_natMx);
    auto it = g_nat.find(uid);
    if (it == g_nat.end()) return false;
    *w = it->second.first;
    *h = it->second.second;
    return *w > 0 && *h > 0;
}

void SetNativeSize(int uid, int w, int h) {
    std::lock_guard<std::mutex> lk(g_natMx);
    g_nat[uid] = {w, h};
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
namespace {
void DrawCardRect(HDC dc, const RECT& rc, const wchar_t* text, std::map<int, HFONT>& fonts) {
    HBRUSH bg = CreateSolidBrush(RGB(30, 32, 38));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    const int px = (std::max)(10, (std::min)(W / 14, H / 6));
    HFONT& f = fonts[px];
    if (!f)
        f = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 235, 235));
    RECT r = rc;
    InflateRect(&r, -px / 2, 0);
    RECT m = r;
    DrawTextW(dc, text, -1, &m, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    const int th = m.bottom - m.top;
    r.top = rc.top + (H - th) / 2;
    r.bottom = r.top + th + 2;
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
}

struct MaskEntry {
    std::wstring key;
    std::vector<uint8_t> m;
};

struct AlphaPic {
    std::wstring key;
    std::vector<uint8_t> px;  // premultiplied BGRA, unrotated
    int w = 0, h = 0;
    double scale = 1;         // px per native unit (text is rastered at the size it is shown)
    bool ok = false;
};

struct AlphaOut {
    std::wstring key;
    std::vector<uint8_t> px;  // premultiplied BGRA of the visible part, mask and opacity applied
    int w = 0, h = 0;
};

struct InSurf {
    Surface s;
    uint64_t seq = ~0ull;
    int w = 0, h = 0;
};

inline uint8_t Mul255(int a, int b) { return static_cast<uint8_t>((a * b + 127) / 255); }
}  // namespace

class Renderer {
public:
    Renderer() { screenDC = GetDC(nullptr); }
    ~Renderer() {
        for (auto& kv : fonts) DeleteObject(kv.second);
        if (screenDC) ReleaseDC(nullptr, screenDC);
    }
    HDC screenDC = nullptr;
    int screenFails = 0;

    // Scene into dst (W x H). slot separates caches (0 program, 1 next, 2 solo)
    void RenderScene(const Scene* sc, Surface& dst, int slot, bool cards) {
        dst.Clear(0xFF000000);
        if (!sc) return;
        for (const auto& it : sc->items)
            if (it.visible) RenderItem(it, dst, slot, cards);
        GdiFlush();
    }

    // The whole source as shown (rotation / mirror, no crop) fitted into W x H on a checkerboard
    void RenderSolo(const Item& it0, Surface& dst) {
        Item it = it0;
        for (int& c : it.crop) c = 0;
        it.zoom = 100;
        it.fit = 0;
        it.shape = ShapeDef();
        it.opacity = 100;
        it.border = false;
        it.x = it.y = 0;
        it.w = it.h = 1;
        dst.Clear(0xFF000000);
        RECT r{0, 0, dst.w, dst.h};
        DrawCheckerboard(dst.dc, r, (std::max)(6, dst.h / 24));
        GdiFlush();
        RenderItem(it, dst, 2, true);
        GdiFlush();
    }

private:
    std::map<int, Surface> winSurf;
    std::map<std::wstring, InSurf> inSurf;
    std::map<int, MaskEntry> masks;
    std::map<int, AlphaPic> pics;
    std::map<int, AlphaOut> alphaOut;
    std::map<int, HFONT> fonts;
    Surface tmpT, tmpU;
    std::vector<uint8_t> scratch;

    struct View {
        HDC dc = nullptr;
        double ox = 0, oy = 0;  // source rect origin inside dc
        double NW = 0, NH = 0;  // native (unrotated) size
        bool screen = false;    // cursor can be drawn
        double scrX = 0, scrY = 0;  // screen coordinates of the source origin
        const AlphaPic* pic = nullptr;
        std::wstring card;
    };

    const std::vector<uint8_t>& Mask(int key, int w, int h, const ShapeDef& sd, int x0, int y0, int mw, int mh) {
        MaskEntry& e = masks[key];
        const std::wstring k = std::to_wstring(w) + L"x" + std::to_wstring(h) + L"|" + std::to_wstring(x0) + L"," +
                               std::to_wstring(y0) + L"," + std::to_wstring(mw) + L"," + std::to_wstring(mh) + L"|" + sd.Key();
        if (e.key != k) {
            e.key = k;
            if (mw == w && mh == h && x0 == 0 && y0 == 0) {
                BuildShapeMask(w, h, sd, e.m);
            } else {
                // huge box: only the visible window of the full mask
                std::vector<uint8_t> full;
                if (static_cast<double>(w) * h <= 16e6) {
                    BuildShapeMask(w, h, sd, full);
                    e.m.resize(static_cast<size_t>(mw) * mh);
                    for (int y = 0; y < mh; ++y)
                        std::memcpy(&e.m[static_cast<size_t>(y) * mw], &full[static_cast<size_t>(y + y0) * w + x0],
                                    static_cast<size_t>(mw));
                } else {
                    e.m.assign(static_cast<size_t>(mw) * mh, 255);
                }
            }
        }
        return e.m;
    }

    bool GetView(const Item& it, View* v, double scaleHint) {
        switch (it.type) {
        case IT_REGION:
        case IT_MONITOR: {
            const RECT r = it.type == IT_REGION ? it.region : it.monRect;
            if (r.right - r.left < 8 || r.bottom - r.top < 8) {
                v->card = L"Область не выбрана";
                return true;
            }
            v->dc = screenDC;
            v->ox = r.left;
            v->oy = r.top;
            v->NW = r.right - r.left;
            v->NH = r.bottom - r.top;
            v->screen = true;
            v->scrX = r.left;
            v->scrY = r.top;
            return true;
        }
        case IT_WINDOW: {
            RECT r;
            if (!WindowRectOf(it.hwnd, &r)) {
                v->card = it.winTitle.empty() ? L"Окно не выбрано" : L"Окно свёрнуто или закрыто";
                return true;
            }
            v->NW = r.right - r.left;
            v->NH = r.bottom - r.top;
            v->screen = true;
            v->scrX = r.left;
            v->scrY = r.top;
            if (it.winCapture == 1) {
                RECT wr;
                if (!GetWindowRect(it.hwnd, &wr)) return false;
                Surface& s = winSurf[it.uid];
                const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
                if (ww < 16 || wh < 16 || !s.Ensure(screenDC, ww, wh)) return false;
                if (!PrintWindow(it.hwnd, s.dc, 2 /* PW_RENDERFULLCONTENT */)) {
                    v->card = L"Окно не удаётся захватить";
                    return true;
                }
                v->dc = s.dc;
                v->ox = r.left - wr.left;
                v->oy = r.top - wr.top;
            } else {
                v->dc = screenDC;
                v->ox = r.left;
                v->oy = r.top;
            }
            return true;
        }
        case IT_WEBCAM:
        case IT_PHONE:
        case IT_MEDIA: {
            const std::wstring key = InputKeyOf(it);
            auto in = GetInput(key);
            if (!in || in->status != 2) {
                v->card = InputMessage(it);
                return true;
            }
            InSurf& is = inSurf[key];
            const uint64_t q = in->seq;
            if (q != is.seq) {
                std::lock_guard<std::mutex> lk(in->mx);
                if (in->buf.empty()) {
                    is.w = is.h = 0;
                } else if (is.s.Ensure(screenDC, in->w, in->h)) {
                    std::memcpy(is.s.bits, in->buf.data(), static_cast<size_t>(in->w) * in->h * 4);
                    is.w = in->w;
                    is.h = in->h;
                }
                is.seq = q;
            }
            if (!is.w) {
                v->card = InputMessage(it);
                return true;
            }
            v->dc = is.s.dc;
            v->NW = is.w;
            v->NH = is.h;
            return true;
        }
        case IT_IMAGE: {
            AlphaPic& p = pics[it.uid];
            if (p.key != it.image) {
                p.key = it.image;
                p.ok = LoadImageScaled(it.image, 0, 0, GUID_WICPixelFormat32bppPBGRA, p.px, &p.w, &p.h);
                p.scale = 1;
            }
            if (!p.ok) {
                v->card = it.image.empty() ? L"Картинка не выбрана" : L"Не удалось открыть картинку";
                return true;
            }
            v->pic = &p;
            v->NW = p.w;
            v->NH = p.h;
            return true;
        }
        case IT_TEXT: {
            AlphaPic& p = pics[it.uid];
            const double sc = std::clamp(std::round(scaleHint * 20) / 20, 0.05, 8.0);
            const std::wstring k = it.text + L"|" + std::to_wstring(it.textPx) + L"|" + std::to_wstring(it.textColor) +
                                   L"|" + std::to_wstring(it.textBg) + std::to_wstring(it.textBold) + L"|" +
                                   std::to_wstring(sc);
            if (p.key != k) {
                p.key = k;
                RasterText(it, sc, p.px, &p.w, &p.h);
                p.scale = sc;
                p.ok = true;
            }
            int nw, nh;
            TextNativeSize(it, &nw, &nh);
            v->pic = &p;
            v->NW = nw;
            v->NH = nh;
            return true;
        }
        default: return true;
        }
    }

    // canvas (BGRA) -> dst rectangle with clockwise rotation and horizontal mirror
    static void ConvertInto(const Surface& cv, Surface& dst, int ox, int oy, int TW, int TH, int rot, bool mirror) {
        const int CW = cv.w, CH = cv.h;
        if (!cv.bits || !dst.bits) return;
        for (int y = 0; y < TH; ++y) {
            if (oy + y < 0 || oy + y >= dst.h) continue;
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
            const uint32_t* p = reinterpret_cast<const uint32_t*>(cv.bits) + static_cast<ptrdiff_t>(cy) * CW + cx;
            uint32_t* o = reinterpret_cast<uint32_t*>(dst.bits) + static_cast<size_t>(oy + y) * dst.w + ox;
            const int n = (std::min)(TW, dst.w - ox);
            if (step == 1)
                std::memcpy(o, p, static_cast<size_t>(n) * 4);
            else
                for (int x = 0; x < n; ++x, p += step) o[x] = *p;
        }
    }

    void DrawCursorInto(Surface& t, const View& v, const RectD& S) {
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
        const double kx = t.w / S.W(), ky = t.h / S.H();
        const double sx = ci.ptScreenPos.x - static_cast<int>(ii.xHotspot) - v.scrX;  // native coords
        const double sy = ci.ptScreenPos.y - static_cast<int>(ii.yHotspot) - v.scrY;
        const int x = static_cast<int>(std::lround((sx - S.x0) * kx)), y = static_cast<int>(std::lround((sy - S.y0) * ky));
        DrawIconEx(t.dc, x, y, ci.hCursor, (std::max)(1, static_cast<int>(cw * kx)), (std::max)(1, static_cast<int>(ch * ky)),
                   0, nullptr, DI_NORMAL);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }

    // blend src (BGRA, opaque) through mask * opacity
    static void BlendOpaque(Surface& dst, int x0, int y0, const uint8_t* src, int w, int h, const uint8_t* mask, int mstride,
                            int opacity) {
        for (int y = 0; y < h; ++y) {
            const int oy = y0 + y;
            if (oy < 0 || oy >= dst.h) continue;
            const uint8_t* sp = src + static_cast<size_t>(y) * w * 4;
            const uint8_t* mp = mask ? mask + static_cast<size_t>(y) * mstride : nullptr;
            uint8_t* dp = dst.bits + (static_cast<size_t>(oy) * dst.w + x0) * 4;
            for (int x = 0; x < w; ++x, sp += 4, dp += 4) {
                int a = mp ? mp[x] : 255;
                if (opacity < 255) a = a * opacity / 255;
                if (a == 0) continue;
                if (a == 255) {
                    dp[0] = sp[0];
                    dp[1] = sp[1];
                    dp[2] = sp[2];
                } else {
                    const int ia = 255 - a;
                    dp[0] = static_cast<uint8_t>((sp[0] * a + dp[0] * ia) / 255);
                    dp[1] = static_cast<uint8_t>((sp[1] * a + dp[1] * ia) / 255);
                    dp[2] = static_cast<uint8_t>((sp[2] * a + dp[2] * ia) / 255);
                }
            }
        }
    }

    void FillMasked(Surface& dst, int x0, int y0, int w, int h, const uint8_t* mask, COLORREF col, int opacity) {
        const int cb = GetBValue(col), cg = GetGValue(col), cr = GetRValue(col);
        for (int y = 0; y < h; ++y) {
            const int oy = y0 + y;
            if (oy < 0 || oy >= dst.h) continue;
            for (int x = 0; x < w; ++x) {
                const int ox = x0 + x;
                if (ox < 0 || ox >= dst.w) continue;
                int a = mask[static_cast<size_t>(y) * w + x];
                if (opacity < 255) a = a * opacity / 255;
                if (!a) continue;
                uint8_t* d = dst.bits + (static_cast<size_t>(oy) * dst.w + ox) * 4;
                const int ia = 255 - a;
                d[0] = static_cast<uint8_t>((cb * a + d[0] * ia) / 255);
                d[1] = static_cast<uint8_t>((cg * a + d[1] * ia) / 255);
                d[2] = static_cast<uint8_t>((cr * a + d[2] * ia) / 255);
            }
        }
    }

public:
    void RenderItem(const Item& it, Surface& dst, int slot, bool cards) {
        const int W = dst.w, H = dst.h;
        const RectD box = ItemBox(it, W, H);
        if (box.W() < 1 || box.H() < 1) return;
        View v;
        double scaleHint = 1;
        if (it.type == IT_TEXT) {
            int nw, nh;
            TextNativeSize(it, &nw, &nh);
            const RectD d1 = ItemSourceRect(it, (it.rot & 1) ? nh : nw, (it.rot & 1) ? nw : nh);
            scaleHint = (std::max)(box.W() / d1.W(), box.H() / d1.H());
            if (it.fit == 0) scaleHint = (std::min)(box.W() / d1.W(), box.H() / d1.H());
        }
        if (!GetView(it, &v, scaleHint)) {
            ++screenFails;
            return;
        }
        if (!v.card.empty()) {
            if (it.type == IT_WEBCAM || it.type == IT_PHONE || it.type == IT_MEDIA || it.type == IT_WINDOW ||
                it.type == IT_IMAGE || it.type == IT_REGION) {
                const int x0 = (std::max)(0, static_cast<int>(std::lround(box.x0)));
                const int y0 = (std::max)(0, static_cast<int>(std::lround(box.y0)));
                const int x1 = (std::min)(W, static_cast<int>(std::lround(box.x1)));
                const int y1 = (std::min)(H, static_cast<int>(std::lround(box.y1)));
                if (cards && x1 - x0 > 4 && y1 - y0 > 4) {
                    RECT rc{x0, y0, x1, y1};
                    DrawCardRect(dst.dc, rc, v.card.c_str(), fonts);
                    GdiFlush();
                }
            }
            return;
        }
        const double DW = (it.rot & 1) ? v.NH : v.NW, DH = (it.rot & 1) ? v.NW : v.NH;
        SetNativeSize(it.uid, static_cast<int>(DW), static_cast<int>(DH));
        RectD C, D;
        ItemLayout(it, W, H, DW, DH, &C, &D);
        // integer content rect and its visible part
        const int cx0 = static_cast<int>(std::lround(C.x0)), cy0 = static_cast<int>(std::lround(C.y0));
        const int cx1 = static_cast<int>(std::lround(C.x1)), cy1 = static_cast<int>(std::lround(C.y1));
        const int cw = cx1 - cx0, ch = cy1 - cy0;
        if (cw < 1 || ch < 1) return;
        const int vx0 = (std::max)(0, cx0), vy0 = (std::max)(0, cy0), vx1 = (std::min)(W, cx1), vy1 = (std::min)(H, cy1);
        const int vw = vx1 - vx0, vh = vy1 - vy0;
        if (vw < 1 || vh < 1) return;
        const RectD Dv{D.x0 + (vx0 - cx0) * D.W() / cw, D.y0 + (vy0 - cy0) * D.H() / ch, D.x0 + (vx1 - cx0) * D.W() / cw,
                       D.y0 + (vy1 - cy0) * D.H() / ch};
        const RectD S = DispToSrc(Dv, it.rot, it.mirror, v.NW, v.NH);
        const bool trivialMask = it.shape.Trivial();
        const int opacity = std::clamp(it.opacity, 0, 100) * 255 / 100;
        const int mkey = it.uid * 8 + slot * 2;
        const bool hugeBox = static_cast<double>(cw) * ch > 6.0 * W * H;
        // border: the shape grown by b px, white, under the picture
        if (it.border) {
            const int b = (std::max)(2, H / 360);
            const int bw = cw + 2 * b, bh = ch + 2 * b;
            if (!hugeBox) {
                const auto& bm = Mask(mkey + 1, bw, bh, it.shape, 0, 0, bw, bh);
                FillMasked(dst, cx0 - b, cy0 - b, bw, bh, bm.data(), RGB(255, 255, 255), opacity);
            }
        }
        const uint8_t* mask = nullptr;
        int mstride = vw;
        if (!trivialMask) {
            const int mx0 = vx0 - cx0, my0 = vy0 - cy0;
            const auto& m = hugeBox ? Mask(mkey, cw, ch, it.shape, mx0, my0, vw, vh) : Mask(mkey, cw, ch, it.shape, 0, 0, cw, ch);
            if (hugeBox) {
                mask = m.data();
                mstride = vw;
            } else {
                mask = m.data() + static_cast<size_t>(my0) * cw + mx0;
                mstride = cw;
            }
        }
        if (v.pic) {
            RenderAlpha(it, v, S, dst, vx0, vy0, vw, vh, mask, mstride, opacity, slot);
            return;
        }
        // opaque GDI source
        const int sx = static_cast<int>(std::floor(v.ox + S.x0)), sy = static_cast<int>(std::floor(v.oy + S.y0));
        const int sw = (std::max)(1, static_cast<int>(std::lround(S.W()))), sh = (std::max)(1, static_cast<int>(std::lround(S.H())));
        const bool direct = (it.rot & 3) == 0 && !it.mirror && trivialMask && opacity == 255 && !(it.cursor && v.screen);
        if (direct) {
            BOOL ok = (sw == vw && sh == vh) ? BitBlt(dst.dc, vx0, vy0, vw, vh, v.dc, sx, sy, SRCCOPY)
                                             : StretchBlt(dst.dc, vx0, vy0, vw, vh, v.dc, sx, sy, sw, sh, SRCCOPY);
            if (!ok && v.screen) ++screenFails;
            GdiFlush();
            return;
        }
        const int tw = (it.rot & 1) ? vh : vw, th = (it.rot & 1) ? vw : vh;
        if (!tmpT.Ensure(screenDC, tw, th)) return;
        BOOL ok = (sw == tw && sh == th) ? BitBlt(tmpT.dc, 0, 0, tw, th, v.dc, sx, sy, SRCCOPY)
                                         : StretchBlt(tmpT.dc, 0, 0, tw, th, v.dc, sx, sy, sw, sh, SRCCOPY);
        if (!ok && v.screen) ++screenFails;
        if (it.cursor && v.screen) DrawCursorInto(tmpT, v, S);
        GdiFlush();
        if (trivialMask && opacity == 255) {
            ConvertInto(tmpT, dst, vx0, vy0, vw, vh, it.rot, it.mirror);
            return;
        }
        if (!tmpU.Ensure(screenDC, vw, vh)) return;
        ConvertInto(tmpT, tmpU, 0, 0, vw, vh, it.rot, it.mirror);
        BlendOpaque(dst, vx0, vy0, tmpU.bits, vw, vh, mask, mstride, opacity);
    }

private:
    // pictures with transparency (logo, text): sampled in software, cached while nothing changes
    void RenderAlpha(const Item& it, const View& v, const RectD& S, Surface& dst, int vx0, int vy0, int vw, int vh,
                     const uint8_t* mask, int mstride, int opacity, int slot) {
        const AlphaPic& p = *v.pic;
        AlphaOut& ao = alphaOut[it.uid * 4 + slot];
        wchar_t kb[200];
        swprintf(kb, 200, L"%d,%d,%d,%d|%.3f,%.3f,%.3f,%.3f|%d%d|%d|%p", vx0, vy0, vw, vh, S.x0, S.y0, S.x1, S.y1, it.rot,
                 it.mirror ? 1 : 0, opacity, static_cast<const void*>(mask));
        const std::wstring key = std::wstring(kb) + L"|" + p.key + L"|" + it.shape.Key() + std::to_wstring(mstride);
        if (ao.key != key) {
            ao.key = key;
            ao.w = vw;
            ao.h = vh;
            ao.px.assign(static_cast<size_t>(vw) * vh * 4, 0);
            const double ps = p.scale;
            // output pixel -> unrotated source coords: build the inverse map from the corners of S
            for (int y = 0; y < vh; ++y) {
                for (int x = 0; x < vw; ++x) {
                    // position inside the visible part, as fraction of the displayed rect
                    const double fx = (x + 0.5) / vw, fy = (y + 0.5) / vh;
                    double dx = fx, dy = fy;  // display fractions -> source fractions
                    if (it.mirror) dx = 1 - dx;
                    double sxf, syf;
                    switch (it.rot & 3) {
                    case 1: sxf = dy; syf = 1 - dx; break;
                    case 2: sxf = 1 - dx; syf = 1 - dy; break;
                    case 3: sxf = 1 - dy; syf = dx; break;
                    default: sxf = dx; syf = dy; break;
                    }
                    const double sxp = (S.x0 + sxf * S.W()) * ps - 0.5, syp = (S.y0 + syf * S.H()) * ps - 0.5;
                    const int ix = static_cast<int>(std::floor(sxp)), iy = static_cast<int>(std::floor(syp));
                    const double ax = sxp - ix, ay = syp - iy;
                    double acc[4] = {0, 0, 0, 0};
                    for (int k = 0; k < 4; ++k) {
                        const int qx = std::clamp(ix + (k & 1), 0, p.w - 1), qy = std::clamp(iy + (k >> 1), 0, p.h - 1);
                        const double wgt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
                        const uint8_t* q = &p.px[(static_cast<size_t>(qy) * p.w + qx) * 4];
                        for (int c = 0; c < 4; ++c) acc[c] += q[c] * wgt;
                    }
                    int k = mask ? mask[static_cast<size_t>(y) * mstride + x] : 255;
                    if (opacity < 255) k = k * opacity / 255;
                    uint8_t* o = &ao.px[(static_cast<size_t>(y) * vw + x) * 4];
                    for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>(std::lround(acc[c]) * k / 255);
                }
            }
        }
        for (int y = 0; y < vh; ++y) {
            const int oy = vy0 + y;
            if (oy < 0 || oy >= dst.h) continue;
            const uint8_t* sp = &ao.px[static_cast<size_t>(y) * vw * 4];
            uint8_t* dp = dst.bits + (static_cast<size_t>(oy) * dst.w + vx0) * 4;
            for (int x = 0; x < vw; ++x, sp += 4, dp += 4) {
                const int a = sp[3];
                if (!a) continue;
                const int ia = 255 - a;
                dp[0] = static_cast<uint8_t>((std::min)(255, sp[0] + dp[0] * ia / 255));
                dp[1] = static_cast<uint8_t>((std::min)(255, sp[1] + dp[1] * ia / 255));
                dp[2] = static_cast<uint8_t>((std::min)(255, sp[2] + dp[2] * ia / 255));
            }
        }
        (void)Mul255;
    }
};

// ---------------------------------------------------------------------------
// Output stage: crop (cover) + shape with background
// ---------------------------------------------------------------------------
namespace {
void BoxBlur(std::vector<uint8_t>& px, int w, int h, int r) {
    if (r < 1 || w < 2 || h < 2) return;
    std::vector<uint8_t> tmp(px.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y)
            for (int c = 0; c < 3; ++c) {
                int sum = 0, n = 0;
                for (int x = -r; x <= r; ++x)
                    if (x >= 0 && x < w) sum += px[(static_cast<size_t>(y) * w + x) * 4 + c], ++n;
                for (int x = 0; x < w; ++x) {
                    tmp[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>(sum / (std::max)(1, n));
                    const int a = x - r, b = x + r + 1;
                    if (a >= 0) sum -= px[(static_cast<size_t>(y) * w + a) * 4 + c], --n;
                    if (b < w) sum += px[(static_cast<size_t>(y) * w + b) * 4 + c], ++n;
                }
            }
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                int sum = 0, n = 0;
                for (int y = -r; y <= r; ++y)
                    if (y >= 0 && y < h) sum += tmp[(static_cast<size_t>(y) * w + x) * 4 + c], ++n;
                for (int y = 0; y < h; ++y) {
                    px[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>(sum / (std::max)(1, n));
                    const int a = y - r, b = y + r + 1;
                    if (a >= 0) sum -= tmp[(static_cast<size_t>(a) * w + x) * 4 + c], --n;
                    if (b < h) sum += tmp[(static_cast<size_t>(b) * w + x) * 4 + c], ++n;
                }
            }
    }
}
}  // namespace

struct OutputStage {
    std::wstring maskKey;
    std::vector<uint8_t> mask;
    std::wstring bgKey;
    Surface bg, smallS;
    std::vector<uint8_t> blurPx;

    // applies the shape inside V on the canvas (in place); returns V
    RECT Apply(const OutputCfg& o, Surface& canvas, HDC ref) {
        const RectD vd = OutputVisibleRect(o, canvas.w, canvas.h);
        RECT V{static_cast<LONG>(std::lround(vd.x0)), static_cast<LONG>(std::lround(vd.y0)),
               static_cast<LONG>(std::lround(vd.x1)), static_cast<LONG>(std::lround(vd.y1))};
        V.left = (std::max)(0L, V.left);
        V.top = (std::max)(0L, V.top);
        V.right = (std::min)(static_cast<LONG>(canvas.w), V.right);
        V.bottom = (std::min)(static_cast<LONG>(canvas.h), V.bottom);
        const int vw = V.right - V.left, vh = V.bottom - V.top;
        if (o.shape.Trivial() || vw < 2 || vh < 2) return V;
        const std::wstring mk = std::to_wstring(vw) + L"x" + std::to_wstring(vh) + L"|" + o.shape.Key();
        if (mk != maskKey) {
            BuildShapeMask(vw, vh, o.shape, mask);
            maskKey = mk;
        }
        // background picture for the area outside the shape
        const uint8_t* bgp = nullptr;
        if (o.bgMode == 1) {
            const int sw = (std::max)(4, vw / 8), sh = (std::max)(4, vh / 8);
            if (smallS.Ensure(ref, sw, sh) && bg.Ensure(ref, vw, vh)) {
                StretchBlt(smallS.dc, 0, 0, sw, sh, canvas.dc, V.left, V.top, vw, vh, SRCCOPY);
                GdiFlush();
                blurPx.assign(smallS.bits, smallS.bits + static_cast<size_t>(sw) * sh * 4);
                BoxBlur(blurPx, sw, sh, 3);
                std::memcpy(smallS.bits, blurPx.data(), blurPx.size());
                StretchBlt(bg.dc, 0, 0, vw, vh, smallS.dc, 0, 0, sw, sh, SRCCOPY);
                GdiFlush();
                bgp = bg.bits;
            }
        } else if (o.bgMode == 2 && !o.bgImage.empty()) {
            const std::wstring bk = o.bgImage + L"|" + std::to_wstring(vw) + L"x" + std::to_wstring(vh);
            if (bk != bgKey) {
                bgKey = bk;
                std::vector<uint8_t> img;
                int iw = 0, ih = 0;
                bg.Ensure(ref, vw, vh);
                bg.Clear(0xFF000000);
                if (LoadImageScaled(o.bgImage, 0, 0, GUID_WICPixelFormat32bppBGR, img, &iw, &ih)) {
                    Surface tmp;
                    if (tmp.Ensure(ref, iw, ih)) {
                        std::memcpy(tmp.bits, img.data(), img.size());
                        const double s = (std::max)(static_cast<double>(vw) / iw, static_cast<double>(vh) / ih);
                        const int cw = static_cast<int>(vw / s), ch = static_cast<int>(vh / s);
                        StretchBlt(bg.dc, 0, 0, vw, vh, tmp.dc, (iw - cw) / 2, (ih - ch) / 2, cw, ch, SRCCOPY);
                        GdiFlush();
                    }
                }
            }
            bgp = bg.bits;
        }
        const COLORREF c = o.bgColor;
        const int cb = GetBValue(c), cg = GetGValue(c), cr = GetRValue(c);
        for (int y = 0; y < vh; ++y) {
            uint8_t* p = canvas.bits + (static_cast<size_t>(V.top + y) * canvas.w + V.left) * 4;
            const uint8_t* m = &mask[static_cast<size_t>(y) * vw];
            const uint8_t* b = bgp ? bgp + static_cast<size_t>(y) * vw * 4 : nullptr;
            for (int x = 0; x < vw; ++x, p += 4) {
                const int a = m[x];
                if (a == 255) continue;
                const int ia = 255 - a;
                const int bb = b ? b[x * 4] : cb, bgc = b ? b[x * 4 + 1] : cg, br = b ? b[x * 4 + 2] : cr;
                p[0] = static_cast<uint8_t>((p[0] * a + bb * ia) / 255);
                p[1] = static_cast<uint8_t>((p[1] * a + bgc * ia) / 255);
                p[2] = static_cast<uint8_t>((p[2] * a + br * ia) / 255);
            }
        }
        return V;
    }
};

// ---------------------------------------------------------------------------
// Camera DLL + render thread
// ---------------------------------------------------------------------------
typedef void* scCamera;
typedef scCamera(__cdecl* PFN_scCreateCamera)(int, int, float);
typedef void(__cdecl* PFN_scDeleteCamera)(scCamera);
typedef void(__cdecl* PFN_scSendFrame)(scCamera, const void*);
static HMODULE g_camDll = nullptr;
static PFN_scCreateCamera pCreate = nullptr;
static PFN_scDeleteCamera pDelete = nullptr;
static PFN_scSendFrame pSend = nullptr;
static scCamera g_cam = nullptr;
static std::thread g_renderTh;
static std::atomic<bool> g_renderQuit{false};

bool CameraDllLoaded() { return g_camDll != nullptr; }

bool LoadCameraDll() {
    if (g_camDll) return true;
    std::wstring dll = ExeDir() + L"\\WVCCam.dll";
    g_camDll = LoadLibraryW(dll.c_str());
    if (!g_camDll) return false;
    pCreate = reinterpret_cast<PFN_scCreateCamera>(GetProcAddress(g_camDll, "scCreateCamera"));
    pDelete = reinterpret_cast<PFN_scDeleteCamera>(GetProcAddress(g_camDll, "scDeleteCamera"));
    pSend = reinterpret_cast<PFN_scSendFrame>(GetProcAddress(g_camDll, "scSendFrame"));
    if (!pCreate || !pDelete || !pSend) {
        FreeLibrary(g_camDll);
        g_camDll = nullptr;
        return false;
    }
    return true;
}

static void PublishScaled(Published& pub, Surface& src, const RECT* part, Surface& tmp, HDC ref) {
    int ww = pub.wantW, wh = pub.wantH;
    if (ww < 8 || wh < 8) return;
    const int sx = part ? part->left : 0, sy = part ? part->top : 0;
    const int sw = part ? part->right - part->left : src.w, sh = part ? part->bottom - part->top : src.h;
    // keep the picture's proportions inside the wanted size
    const double s = (std::min)(static_cast<double>(ww) / sw, static_cast<double>(wh) / sh);
    ww = (std::max)(1, static_cast<int>(sw * s));
    wh = (std::max)(1, static_cast<int>(sh * s));
    if (!tmp.Ensure(ref, ww, wh)) return;
    StretchBlt(tmp.dc, 0, 0, ww, wh, src.dc, sx, sy, sw, sh, SRCCOPY);
    GdiFlush();
    std::lock_guard<std::mutex> lk(pub.mx);
    pub.px.assign(tmp.bits, tmp.bits + static_cast<size_t>(ww) * wh * 4);
    pub.w = ww;
    pub.h = wh;
    ++pub.seq;
}

// pause / stopped picture (BGRA)
static void PauseFrame(const Project& p, Surface& out) {
    out.Clear(0xFF000000);
    bool drawn = false;
    if (p.pauseMode == 3 && !p.pauseImage.empty()) {
        std::vector<uint8_t> img;
        int iw = 0, ih = 0;
        if (LoadImageScaled(p.pauseImage, out.w, out.h, GUID_WICPixelFormat32bppBGR, img, &iw, &ih)) {
            const int dx = (out.w - iw) / 2, dy = (out.h - ih) / 2;
            for (int y = 0; y < ih; ++y)
                std::memcpy(out.bits + (static_cast<size_t>(dy + y) * out.w + dx) * 4,
                            img.data() + static_cast<size_t>(y) * iw * 4, static_cast<size_t>(iw) * 4);
            drawn = true;
        } else {
            LogErr(L"Не удалось открыть картинку паузы: " + p.pauseImage);
        }
    }
    if (p.pauseMode == 2 || (p.pauseMode == 3 && !drawn)) {
        std::map<int, HFONT> fonts;
        RECT rc{0, 0, out.w, out.h};
        DrawCardRect(out.dc, rc, p.pauseMode == 3 ? L"Не удалось открыть картинку" : p.pauseText.c_str(), fonts);
        for (auto& kv : fonts) DeleteObject(kv.second);
    }
    GdiFlush();
    uint8_t* q = out.bits;
    for (size_t i = 3, n = static_cast<size_t>(out.w) * out.h * 4; i < n; i += 4) q[i] = 255;
}

// true when every live source of the scene has a picture (or has failed: nothing to wait for)
static bool SceneReady(const Scene* sc) {
    if (!sc) return true;
    for (const auto& it : sc->items) {
        if (!it.visible || (it.type != IT_WEBCAM && it.type != IT_PHONE && it.type != IT_MEDIA)) continue;
        const std::wstring key = InputKeyOf(it);
        if (key.empty()) continue;
        auto in = GetInput(key);
        if (!in) return false;  // not started yet (the UI timer starts it within 0.1 s)
        const int st = in->status;
        if (st == 3) continue;
        if (st != 2) return false;
        std::lock_guard<std::mutex> lk(in->mx);
        if (in->buf.empty()) return false;
    }
    return true;
}

static void PublishBgra(Published& pub, const uint8_t* px, int W, int H, Surface& src, Surface& tmp, HDC ref) {
    // px -> src surface -> scaled publish
    if (!src.Ensure(ref, W, H)) return;
    if (src.bits != px) std::memcpy(src.bits, px, static_cast<size_t>(W) * H * 4);
    PublishScaled(pub, src, nullptr, tmp, ref);
}

static void PackBgra(const uint8_t* p, uint8_t* dst, size_t n) {
    for (size_t i = 0; i < n; ++i, p += 4, dst += 3) {
        dst[0] = p[0];
        dst[1] = p[1];
        dst[2] = p[2];
    }
}

// The render thread. Two transition channels:
//  A «программа»: scene switches (any path: list, «В эфир», hotkey, double click, tray) — preview and camera;
//  B «камера»: start / stop / pause — only what the camera gets.
static void RenderLoop(int W, int H, int fps) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_ptr<Renderer> rnd(new Renderer());
    OutputStage stage;
    Surface canvas, out, pubTmp, nextSurf, soloSurf, pauseSurf, trPub;
    const size_t npx = static_cast<size_t>(W) * H;
    std::vector<uint8_t> camBGR(npx * 3, 0), lastLive(npx * 4, 0), lastCam(npx * 4, 0);
    bool haveLastLive = false, haveLastCam = false, haveShownA = false;
    const uint8_t* shownA = nullptr;  // what channel A showed last frame (points into canvas / out / A's buffer)
    TransEngine trA, trB;
    std::wstring keyA, keyB, pauseKey;
    int frames = 0, tick = 0;
    ULONGLONG fpsT0 = GetTickCount64();
    int ver = -1;
    Project p;
    const DWORD period = static_cast<DWORD>(1000 / (std::max)(1, fps));
    LogMsg(L"Камера: " + std::to_wstring(W) + L"×" + std::to_wstring(H) + L", " + std::to_wstring(fps) + L" к/с");
    while (!g_renderQuit) {
        const ULONGLONG t0 = GetTickCount64();
        if (g_resetCapture.exchange(false) || rnd->screenFails > 15) {
            if (rnd->screenFails > 15) LogErr(L"Захват экрана не удался 15 раз подряд — пересоздаю");
            rnd.reset(new Renderer());
            pauseKey.clear();
        }
        const int v = g_projVer;
        if (v != ver) {
            p = ProjSnapshot();
            ver = v;
        }
        ++tick;
        const bool live = g_streaming && !g_paused;
        const Scene* cur = CurSceneC(p);
        const bool wantProg = live || g_pubProgram.wantW > 0 || g_pubCanvas.wantW > 0;
        // --- channel A trigger: checked BEFORE the new scene is drawn, so the old picture is still in the buffers
        {
            const std::wstring k = std::to_wstring(p.cur) + L"|" + (cur ? cur->name : std::wstring()) + L"|" +
                                   std::to_wstring(g_transGen.load());
            if (k != keyA) {
                if (!keyA.empty() && wantProg && haveShownA) trA.Begin(shownA, W, H, p.transition, p.transMs, p.transDir, p.transEase);
                keyA = k;
            }
        }
        const uint8_t* progFrame = nullptr;  // BGRA W x H: the scene as the camera sees it (shape + crop)
        if (wantProg && canvas.Ensure(rnd->screenDC, W, H)) {
            rnd->screenFails = 0;
            rnd->RenderScene(cur, canvas, 0, true);
            if (g_pubCanvas.wantW > 0) PublishScaled(g_pubCanvas, canvas, nullptr, pubTmp, rnd->screenDC);
            RECT V = stage.Apply(p.out, canvas, rnd->screenDC);
            const bool full = V.left == 0 && V.top == 0 && V.right == W && V.bottom == H;
            if (full) {
                progFrame = canvas.bits;
            } else if (out.Ensure(rnd->screenDC, W, H)) {
                StretchBlt(out.dc, 0, 0, W, H, canvas.dc, V.left, V.top, V.right - V.left, V.bottom - V.top, SRCCOPY);
                GdiFlush();
                progFrame = out.bits;
            }
            const uint8_t* shown = progFrame ? trA.Step(progFrame, W, H, SceneReady(cur), rnd->screenDC) : nullptr;
            shownA = shown;
            haveShownA = shown != nullptr;
            if (g_pubProgram.wantW > 0 && (tick % 2 == 0 || fps <= 30)) {
                if (shown && shown != progFrame)
                    PublishBgra(g_pubProgram, shown, W, H, trPub, pubTmp, rnd->screenDC);  // transition in the preview
                else
                    PublishScaled(g_pubProgram, canvas, nullptr, pubTmp, rnd->screenDC);
            }
            progFrame = shown;
        } else {
            trA.Reset();
            haveShownA = false;
        }
        // «Следующий» preview
        if (g_pubNext.wantW > 0 && (g_editingNext || tick % 3 == 0)) {
            const int n = static_cast<int>(p.scenes.size());
            const Scene* nx = (p.next >= 0 && p.next < n) ? &p.scenes[static_cast<size_t>(p.next)] : nullptr;
            int nw = g_pubNext.wantW, nh = g_pubNext.wantH;
            const double s = (std::min)(static_cast<double>(nw) / W, static_cast<double>(nh) / H);
            nw = (std::max)(8, static_cast<int>(W * s));
            nh = (std::max)(8, static_cast<int>(H * s));
            if (nextSurf.Ensure(rnd->screenDC, nw, nh)) {
                rnd->RenderScene(nx, nextSurf, 1, true);
                std::lock_guard<std::mutex> lk(g_pubNext.mx);
                g_pubNext.px.assign(nextSurf.bits, nextSurf.bits + static_cast<size_t>(nw) * nh * 4);
                g_pubNext.w = nw;
                g_pubNext.h = nh;
                ++g_pubNext.seq;
            }
        }
        // Properties preview of one source
        if (g_soloUid && g_pubSolo.wantW > 0 && tick % 2 == 0) {
            Item it;
            {
                std::lock_guard<std::mutex> lk(g_soloMx);
                it = g_soloItem;
            }
            if (soloSurf.Ensure(rnd->screenDC, g_pubSolo.wantW, g_pubSolo.wantH)) {
                rnd->RenderSolo(it, soloSurf);
                std::lock_guard<std::mutex> lk(g_pubSolo.mx);
                g_pubSolo.px.assign(soloSurf.bits, soloSurf.bits + static_cast<size_t>(soloSurf.w) * soloSurf.h * 4);
                g_pubSolo.w = soloSurf.w;
                g_pubSolo.h = soloSurf.h;
                ++g_pubSolo.seq;
            }
        }
        // --- channel B: what the camera gets
        const uint8_t* camIn = nullptr;
        if (live && progFrame) {
            camIn = progFrame;
            std::memcpy(lastLive.data(), progFrame, npx * 4);
            haveLastLive = true;
            pauseKey.clear();
        } else if (!live) {
            if (p.pauseMode == 0) {
                camIn = haveLastLive ? lastLive.data() : nullptr;
                if (!camIn && pauseSurf.Ensure(rnd->screenDC, W, H)) {
                    pauseSurf.Clear(0xFF000000);
                    camIn = pauseSurf.bits;
                }
            } else if (pauseSurf.Ensure(rnd->screenDC, W, H)) {
                const std::wstring key = std::to_wstring(p.pauseMode) + L"|" + p.pauseImage + L"|" + p.pauseText;
                if (key != pauseKey || g_pauseDirty.exchange(false)) {
                    if (p.pauseMode == 1) pauseSurf.Clear(0xFF000000);
                    else PauseFrame(p, pauseSurf);
                    pauseKey = key;
                }
                camIn = pauseSurf.bits;
            }
        }
        {
            const std::wstring k = std::wstring(live ? L"1" : L"0") + L"|" + std::to_wstring(p.pauseMode);
            if (k != keyB) {
                if (!keyB.empty()) trB.Begin(haveLastCam ? lastCam.data() : nullptr, W, H, p.transition, p.transMs, p.transDir, p.transEase);
                keyB = k;
            }
        }
        const uint8_t* send = camIn ? trB.Step(camIn, W, H, true, rnd->screenDC) : (haveLastCam ? lastCam.data() : nullptr);
        if (send) {
            if (send != lastCam.data()) std::memcpy(lastCam.data(), send, npx * 4);
            haveLastCam = true;
            PackBgra(lastCam.data(), camBGR.data(), npx);
        }
        const ULONGLONG now = GetTickCount64();
        ++frames;
        if (now - fpsT0 >= 1000) {
            g_fpsX10 = static_cast<int>(frames * 10000 / (now - fpsT0));
            frames = 0;
            fpsT0 = now;
        }
        if (g_cam && pSend) {
            pSend(g_cam, camBGR.data());  // paces itself to the camera FPS
        } else {
            const ULONGLONG spent = GetTickCount64() - t0;
            if (spent < period) Sleep(static_cast<DWORD>(period - spent));
        }
        if (!g_cam && !wantProg) Sleep(30);
    }
    CoUninitialize();
}

void RenderStart() {
    Project p = ProjSnapshot();
    int W, H;
    OutSizeFor(p.out, &W, &H);
    g_camOk = false;
    if (pCreate && !g_cam) {
        g_cam = pCreate(W, H, static_cast<float>(p.out.fps));
        g_camOk = g_cam != nullptr;
    }
    g_camW = W;
    g_camH = H;
    g_camFps = p.out.fps;
    // the camera DLL shows its placeholder at this size while WVC is closed
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\WVC", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        const DWORD vals[3] = {static_cast<DWORD>(W), static_cast<DWORD>(H), static_cast<DWORD>(p.out.fps)};
        const wchar_t* names[3] = {L"CamWidth", L"CamHeight", L"CamFps"};
        for (int i = 0; i < 3; ++i) RegSetValueExW(k, names[i], 0, REG_DWORD, reinterpret_cast<const BYTE*>(&vals[i]), sizeof(DWORD));
        RegCloseKey(k);
    }
    if (!g_camOk && pCreate) LogErr(L"Не удалось создать виртуальную камеру (scCreateCamera)");
    g_renderQuit = false;
    g_renderTh = std::thread(RenderLoop, W, H, p.out.fps);
}

void RenderStop() {
    if (g_renderTh.joinable()) {
        g_renderQuit = true;
        g_renderTh.join();
    }
    if (g_cam) {
        pDelete(g_cam);
        g_cam = nullptr;
    }
    g_camOk = false;
}
