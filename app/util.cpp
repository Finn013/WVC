// WVC: strings, paths, the settings file (INI in memory), JSON, monitors / windows, WIC pictures, webcam list.
#include "common.h"

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

std::wstring IniPath() {
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

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring LowerExt(const std::wstring& f) {
    size_t d = f.find_last_of(L'.');
    std::wstring e = d == std::wstring::npos ? L"" : f.substr(d + 1);
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    return e;
}

bool IsVideoFile(const std::wstring& f) {
    static const wchar_t* v[] = {L"mp4", L"m4v", L"mov", L"avi", L"wmv", L"mkv", L"webm", L"mpg", L"mpeg", L"ts", L"3gp"};
    const std::wstring e = LowerExt(f);
    for (auto* x : v)
        if (e == x) return true;
    return false;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n' || s[a] == 0xFEFF)) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// IniDoc
// ---------------------------------------------------------------------------
static bool IEq(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

const std::wstring* IniDoc::Find(const std::wstring& sec, const std::wstring& key) const {
    for (const auto& s : secs) {
        if (!IEq(s.first, sec)) continue;
        for (const auto& kv : s.second)
            if (IEq(kv.first, key)) return &kv.second;
    }
    return nullptr;
}

bool IniDoc::HasSection(const std::wstring& sec) const {
    for (const auto& s : secs)
        if (IEq(s.first, sec)) return true;
    return false;
}

std::wstring IniDoc::Str(const std::wstring& sec, const std::wstring& key, const std::wstring& def) const {
    const std::wstring* v = Find(sec, key);
    return v ? *v : def;
}

int IniDoc::Int(const std::wstring& sec, const std::wstring& key, int def) const {
    const std::wstring* v = Find(sec, key);
    if (!v || v->empty()) return def;
    return _wtoi(v->c_str());
}

double IniDoc::Dbl(const std::wstring& sec, const std::wstring& key, double def) const {
    const std::wstring* v = Find(sec, key);
    if (!v || v->empty()) return def;
    return _wtof(v->c_str());
}

void IniDoc::Set(const std::wstring& sec, const std::wstring& key, const std::wstring& v) {
    for (auto& s : secs) {
        if (!IEq(s.first, sec)) continue;
        for (auto& kv : s.second)
            if (IEq(kv.first, key)) {
                kv.second = v;
                return;
            }
        s.second.push_back({key, v});
        return;
    }
    secs.push_back({sec, {{key, v}}});
}

void IniDoc::SetInt(const std::wstring& sec, const std::wstring& key, int v) { Set(sec, key, std::to_wstring(v)); }

void IniDoc::SetDbl(const std::wstring& sec, const std::wstring& key, double v) {
    wchar_t b[64];
    swprintf(b, 64, L"%.6f", v);
    Set(sec, key, b);
}

void IniDoc::Remove(const std::wstring& sec, const std::wstring& key) {
    for (auto& s : secs) {
        if (!IEq(s.first, sec)) continue;
        s.second.erase(std::remove_if(s.second.begin(), s.second.end(),
                                      [&](const std::pair<std::wstring, std::wstring>& kv) { return IEq(kv.first, key); }),
                       s.second.end());
    }
}

void IniDoc::RemoveSection(const std::wstring& sec) {
    secs.erase(std::remove_if(secs.begin(), secs.end(),
                              [&](const std::pair<std::wstring, Keys>& s) { return IEq(s.first, sec); }),
               secs.end());
}

std::vector<std::wstring> IniDoc::SectionNames() const {
    std::vector<std::wstring> v;
    for (const auto& s : secs) v.push_back(s.first);
    return v;
}

bool IniDoc::LoadText(const std::wstring& text) {
    secs.clear();
    std::wstring cur;
    bool haveSec = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find(L'\n', pos);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = Trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') {
            if (e >= text.size()) break;
            continue;
        }
        if (line[0] == L'[') {
            size_t c = line.find(L']');
            cur = line.substr(1, c == std::wstring::npos ? std::wstring::npos : c - 1);
            haveSec = true;
            bool found = false;
            for (const auto& s : secs) found = found || IEq(s.first, cur);
            if (!found) secs.push_back({cur, {}});
        } else if (haveSec) {
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
            if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
            Set(cur, k, v);
        }
        if (e >= text.size()) break;
    }
    return true;
}

bool IniDoc::Load(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        secs.clear();
        return false;
    }
    std::string bytes;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) bytes.append(buf, got);
    CloseHandle(f);
    std::wstring text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
    } else if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF) {
        text = FromUtf8(bytes.substr(3));
    } else if (!bytes.empty()) {
        int n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        text.resize(static_cast<size_t>(n));
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), &text[0], n);
    }
    return LoadText(text);
}

bool IniDoc::Save(const std::wstring& path) const {
    std::wstring t;
    for (const auto& s : secs) {
        t += L"[" + s.first + L"]\r\n";
        for (const auto& kv : s.second) {
            std::wstring v = kv.second;
            for (auto& c : v)
                if (c == L'\r' || c == L'\n') c = L' ';
            const bool quote = !v.empty() && (v.front() == L' ' || v.back() == L' ' || v.front() == L'"' ||
                                              v.front() == L'\t' || v.back() == L'\t');
            t += kv.first + L"=" + (quote ? L"\"" + v + L"\"" : v) + L"\r\n";
        }
        t += L"\r\n";
    }
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const unsigned char bom[2] = {0xFF, 0xFE};
    BOOL ok = WriteFile(f, bom, 2, &w, nullptr);
    if (ok && !t.empty()) ok = WriteFile(f, t.data(), static_cast<DWORD>(t.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

std::wstring EscapeMultiline(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\n') o += L"\\n";
        else if (c != L'\r') o += c;
    }
    return o;
}

std::wstring UnescapeMultiline(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            if (s[i + 1] == L'n') {
                o += L"\r\n";
                ++i;
                continue;
            }
            if (s[i + 1] == L'\\') {
                o += L'\\';
                ++i;
                continue;
            }
        }
        o += s[i];
    }
    return o;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
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

bool IniToJsonFile(const IniDoc& doc, const std::wstring& path) {
    std::wstring j = L"{\n  \"app\": \"WVC\",\n  \"format\": 2,\n  \"version\": \"" WVC_VERSION L"\",\n  \"sections\": {";
    bool firstSec = true;
    for (const auto& s : doc.secs) {
        j += firstSec ? L"\n    " : L",\n    ";
        firstSec = false;
        j += JsonStr(s.first) + L": {";
        bool firstKey = true;
        for (const auto& kv : s.second) {
            j += firstKey ? L"\n      " : L",\n      ";
            firstKey = false;
            j += JsonStr(kv.first) + L": " + JsonStr(kv.second);
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

namespace {
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
                    if (!Eat(L':')) {
                        bad = true;
                        return;
                    }
                }
                Skip();
            } while (!bad && Eat(L','));
            if (!Eat(close)) bad = true;
            return;
        }
        Scalar();
    }
};
}  // namespace

bool JsonFileToIni(const std::wstring& path, IniDoc* doc, std::wstring* err) {
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
    const std::wstring text = FromUtf8(bytes);
    JsonReader r(text);
    bool isWvc = false;
    IniDoc out;
    bool ok = r.Eat(L'{');
    if (ok && !r.Eat(L'}')) {
        do {
            std::wstring key = r.Str();
            if (r.bad || !r.Eat(L':')) {
                ok = false;
                break;
            }
            if (key == L"app") {
                isWvc = r.Scalar() == L"WVC";
            } else if (key == L"sections") {
                if (!r.Eat(L'{')) {
                    ok = false;
                    break;
                }
                if (!r.Eat(L'}')) {
                    do {
                        std::wstring sec = r.Str();
                        if (r.bad || !r.Eat(L':') || !r.Eat(L'{')) {
                            ok = false;
                            break;
                        }
                        out.secs.push_back({sec, {}});
                        if (!r.Eat(L'}')) {
                            do {
                                std::wstring k = r.Str();
                                if (r.bad || !r.Eat(L':')) {
                                    ok = false;
                                    break;
                                }
                                out.secs.back().second.push_back({k, r.Scalar()});
                            } while (ok && !r.bad && r.Eat(L','));
                            if (!ok || !r.Eat(L'}')) {
                                ok = false;
                                break;
                            }
                        }
                    } while (ok && !r.bad && r.Eat(L','));
                    if (!ok || !r.Eat(L'}')) {
                        ok = false;
                        break;
                    }
                }
            } else {
                r.Skip();
            }
        } while (ok && !r.bad && r.Eat(L','));
        ok = ok && !r.bad && r.Eat(L'}');
    }
    if (!ok || out.secs.empty()) {
        *err = L"Файл не похож на настройки WVC (ошибка в JSON).";
        return false;
    }
    if (!isWvc) {
        *err = L"В файле нет пометки \"app\": \"WVC\".";
        return false;
    }
    *doc = out;
    return true;
}

// ---------------------------------------------------------------------------
// Monitors and windows
// ---------------------------------------------------------------------------
static BOOL CALLBACK EnumMonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    auto* v = reinterpret_cast<std::vector<MonitorItem>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hm, &mi)) v->push_back({mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, mi.szDevice});
    return TRUE;
}

std::vector<MonitorItem> Monitors() {
    std::vector<MonitorItem> v;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&v));
    std::sort(v.begin(), v.end(), [](const MonitorItem& a, const MonitorItem& b) {
        return a.rc.left != b.rc.left ? a.rc.left < b.rc.left : a.rc.top < b.rc.top;
    });
    return v;
}

RECT PrimaryMonitorRect() {
    POINT zero{0, 0};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

RECT VirtualScreen() {
    RECT r;
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

bool WindowRectOf(HWND hwnd, RECT* r) {
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof(*r)))) {
        if (!GetWindowRect(hwnd, r)) return false;
    }
    return r->right - r->left >= 16 && r->bottom - r->top >= 16;
}

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
    return v->size() < 60;
}

std::vector<WindowItem> ListWindows() {
    std::vector<WindowItem> wins;
    EnumWindows(EnumWinProc, reinterpret_cast<LPARAM>(&wins));
    return wins;
}

HWND FindWindowByTitle(const std::wstring& title) {
    if (title.empty()) return nullptr;
    std::vector<WindowItem> wins = ListWindows();
    for (const auto& w : wins)
        if (w.title == title) return w.hwnd;
    std::wstring key = title;
    if (key.size() > 1 && key.back() == L'…') key.pop_back();
    for (const auto& w : wins)
        if (w.title.find(key) != std::wstring::npos || key.find(w.title) != std::wstring::npos) return w.hwnd;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Pictures (WIC)
// ---------------------------------------------------------------------------
bool LoadImageScaled(const std::wstring& path, int maxW, int maxH, REFWICPixelFormatGUID fmt, std::vector<uint8_t>& out,
                     int* ow, int* oh) {
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    IWICBitmapScaler* sc = nullptr;
    do {
        if (path.empty()) break;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) break;
        if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)))
            break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        if (FAILED(f->CreateFormatConverter(&cv))) break;
        if (FAILED(cv->Initialize(fr, fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) break;
        UINT iw = 0, ih = 0;
        cv->GetSize(&iw, &ih);
        if (!iw || !ih) break;
        // maxW <= 0: the picture's own size (at most 4096 on the long side); else fit into maxW x maxH (up or down)
        const double scale = maxW <= 0 || maxH <= 0
                                 ? (std::min)(1.0, 4096.0 / (std::max)(iw, ih))
                                 : (std::min)(static_cast<double>(maxW) / iw, static_cast<double>(maxH) / ih);
        const UINT dw = (std::max)(1u, static_cast<UINT>(std::lround(iw * scale)));
        const UINT dh = (std::max)(1u, static_cast<UINT>(std::lround(ih * scale)));
        out.assign(static_cast<size_t>(dw) * dh * 4, 0);
        if (dw == iw && dh == ih) {
            if (FAILED(cv->CopyPixels(nullptr, dw * 4, static_cast<UINT>(out.size()), out.data()))) break;
        } else {
            if (FAILED(f->CreateBitmapScaler(&sc))) break;
            if (FAILED(sc->Initialize(cv, dw, dh, WICBitmapInterpolationModeFant))) break;
            if (FAILED(sc->CopyPixels(nullptr, dw * 4, static_cast<UINT>(out.size()), out.data()))) break;
        }
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

bool ImageSize(const std::wstring& path, int* w, int* h) {
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    if (!path.empty() &&
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr))) {
        UINT iw = 0, ih = 0;
        fr->GetSize(&iw, &ih);
        *w = static_cast<int>(iw);
        *h = static_cast<int>(ih);
        ok = iw && ih;
    }
    SafeRelease(fr);
    SafeRelease(dec);
    SafeRelease(f);
    return ok;
}

std::vector<CamDevice> EnumWebcams() {
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
