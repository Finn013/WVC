// WVC 0.12: event log — recent lines in memory (for «Справка → Журнал») and wvc.log next to WVC.exe
// (UTF-8, rotated to wvc.log.1 at ~1 MB; when the folder is read-only: %LOCALAPPDATA%\WVC\wvc.log).
#include "common.h"

namespace {
std::mutex g_logMx;
std::deque<std::wstring> g_lines;
std::wstring g_logPath;
bool g_pathTried = false;
const size_t kMaxLines = 600;
const long long kMaxBytes = 1024 * 1024;

std::wstring ResolvePath() {
    std::wstring p = ExeDir() + L"\\wvc.log";
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return p;
    }
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
        std::wstring d = std::wstring(buf) + L"\\WVC";
        CreateDirectoryW(d.c_str(), nullptr);
        return d + L"\\wvc.log";
    }
    return L"";
}

void WriteLine(const std::wstring& line) {
    if (!g_pathTried) {
        g_pathTried = true;
        g_logPath = ResolvePath();
    }
    if (g_logPath.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fa)) {
        const long long size = (static_cast<long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        if (size > kMaxBytes) MoveFileExW(g_logPath.c_str(), (g_logPath + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const std::string u = ToUtf8(line) + "\r\n";
    DWORD wr = 0;
    WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &wr, nullptr);
    CloseHandle(h);
}

void Add(const wchar_t* level, const std::wstring& m) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[64];
    swprintf(ts, 64, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %ls ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds, level);
    const std::wstring line = ts + m;
    std::lock_guard<std::mutex> lk(g_logMx);
    g_lines.push_back(line);
    while (g_lines.size() > kMaxLines) g_lines.pop_front();
    WriteLine(line);
}
}  // namespace

void LogMsg(const std::wstring& m) { Add(L"   ", m); }
void LogErr(const std::wstring& m) { Add(L"ОШИБКА", m); }

std::vector<std::wstring> LogLines() {
    std::lock_guard<std::mutex> lk(g_logMx);
    return std::vector<std::wstring>(g_lines.begin(), g_lines.end());
}

std::wstring LogFilePath() {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_pathTried) {
        g_pathTried = true;
        g_logPath = ResolvePath();
    }
    return g_logPath;
}

// ---------------------------------------------------------------------------
// «Справка → Журнал»
// ---------------------------------------------------------------------------
namespace {
HWND g_logWin = nullptr;
size_t g_logShown = static_cast<size_t>(-1);
enum { IDL_TEXT = 10, IDL_COPY, IDL_FOLDER, IDL_CLOSE };

std::wstring LogText() {
    std::wstring t;
    for (const auto& l : LogLines()) t += l + L"\r\n";
    return t;
}

void CopyText(HWND owner, const std::wstring& t) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (t.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        void* p = GlobalLock(g);
        std::memcpy(p, t.c_str(), bytes);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

void LayoutLog(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int m = S(8), bh = S(28);
    MoveWindow(GetDlgItem(hwnd, IDL_TEXT), m, m, rc.right - 2 * m, rc.bottom - 3 * m - bh, TRUE);
    const int y = rc.bottom - m - bh;
    MoveWindow(GetDlgItem(hwnd, IDL_COPY), m, y, S(170), bh, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDL_FOLDER), m + S(178), y, S(170), bh, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDL_CLOSE), rc.right - m - S(110), y, S(110), bh, TRUE);
}

void RefreshLog(HWND hwnd) {
    const auto lines = LogLines();
    if (lines.size() == g_logShown && !lines.empty()) return;
    g_logShown = lines.size();
    HWND ed = GetDlgItem(hwnd, IDL_TEXT);
    SetWindowTextW(ed, LogText().c_str());
    SendMessageW(ed, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    SendMessageW(ed, EM_SCROLLCARET, 0, 0);
}

LRESULT CALLBACK LogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: LayoutLog(hwnd); return 0;
    case WM_TIMER: RefreshLog(hwnd); return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kDarkText);
        SetBkColor(dc, kDarkList);
        return reinterpret_cast<LRESULT>(g_brList);
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, g_brDark);
        return 1;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDL_COPY:
            CopyText(hwnd, L"WVC " WVC_VERSION L"\r\n" + LogText());
            SetWindowTextW(GetDlgItem(hwnd, IDL_COPY), L"Скопировано ✓");
            return 0;
        case IDL_FOLDER: {
            const std::wstring f = LogFilePath();
            if (!f.empty()) {
                const std::wstring args = L"/select,\"" + f + L"\"";
                ShellExecuteW(hwnd, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        case IDL_CLOSE:
        case IDCANCEL: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_logWin = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

void OpenLogWindow() {
    if (g_logWin) {
        ShowWindow(g_logWin, SW_SHOWNORMAL);
        SetForegroundWindow(g_logWin);
        return;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = LogProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"WVCLog";
        RegisterClassW(&wc);
        reg = true;
    }
    RECT mr;
    GetWindowRect(g_main, &mr);
    const int w = S(820), h = S(520);
    g_logWin = CreateWindowExW(0, L"WVCLog", L"WVC — журнал", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               mr.left + ((mr.right - mr.left) - w) / 2, mr.top + ((mr.bottom - mr.top) - h) / 2, w, h,
                               nullptr, nullptr, g_hInst, nullptr);
    if (!g_logWin) return;
    DarkTitle(g_logWin);
    HWND ed = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                                  ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                              0, 0, 10, 10, g_logWin, reinterpret_cast<HMENU>(IDL_TEXT), g_hInst, nullptr);
    HFONT mono = CreateFontW(-MulDiv(9, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH,
                             L"Consolas");
    SendMessageW(ed, WM_SETFONT, reinterpret_cast<WPARAM>(mono), FALSE);
    SendMessageW(ed, EM_SETLIMITTEXT, 0, 0);
    SetWindowTheme(ed, L"DarkMode_Explorer", nullptr);
    for (auto b : {std::make_pair(IDL_COPY, L"Скопировать журнал"), std::make_pair(IDL_FOLDER, L"Показать файл wvc.log"),
                   std::make_pair(IDL_CLOSE, L"Закрыть")}) {
        HWND h2 = CreateWindowExW(0, L"BUTTON", b.second, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                                  g_logWin, reinterpret_cast<HMENU>(static_cast<INT_PTR>(b.first)), g_hInst, nullptr);
        SendMessageW(h2, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
        SetWindowTheme(h2, L"DarkMode_Explorer", nullptr);
    }
    g_logShown = static_cast<size_t>(-1);
    LayoutLog(g_logWin);
    RefreshLog(g_logWin);
    SetTimer(g_logWin, 1, 1000, nullptr);
    ShowWindow(g_logWin, SW_SHOWNORMAL);
}
