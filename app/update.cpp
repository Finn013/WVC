// WVC 0.12: «Проверить обновления» — reads the GitHub release `latest` (no token), whose description carries
// "version: X.Y" (written by CI), and offers the download when it is newer than this build.
#include "common.h"
#include <winhttp.h>

namespace {
struct UpdateResult {
    bool manual = false, ok = false, newer = false;
    std::wstring version, page, err;
};

bool HttpGet(const wchar_t* host, const wchar_t* path, std::string* body, std::wstring* err) {
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"WVC/" WVC_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"WVC/" WVC_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)
                    : nullptr;
    if (r) {
        WinHttpSetTimeouts(r, 8000, 8000, 10000, 15000);
        const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
        if (WinHttpSendRequest(r, hdr, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &sz, WINHTTP_NO_HEADER_INDEX);
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(r, &chunk[0], avail, &got) || !got) break;
                body->append(chunk.data(), got);
                if (body->size() > 4 * 1024 * 1024) break;
            }
            if (code == 200) ok = true;
            else *err = L"GitHub ответил кодом " + std::to_wstring(code);
        } else {
            *err = L"Нет связи с GitHub (ошибка " + std::to_wstring(GetLastError()) + L")";
        }
    } else {
        *err = L"Не удалось открыть соединение (ошибка " + std::to_wstring(GetLastError()) + L")";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

// "version: 0.12" -> 12 (minor), major in *maj
bool ParseVersion(const std::string& text, int* maj, int* min, std::string* str) {
    size_t p = 0;
    while ((p = text.find("version:", p)) != std::string::npos) {
        size_t q = p + 8;
        while (q < text.size() && text[q] == ' ') ++q;
        int a = 0, b = 0, n = 0;
        if (sscanf(text.c_str() + q, "%d.%d%n", &a, &b, &n) == 2) {
            *maj = a;
            *min = b;
            *str = text.substr(q, static_cast<size_t>(n));
            return true;
        }
        p = q;
    }
    return false;
}

std::string JsonStr(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p = j.find('"', p + k.size());
    if (p == std::string::npos) return "";
    std::string out;
    for (size_t i = p + 1; i < j.size() && j[i] != '"'; ++i) {
        if (j[i] == '\\' && i + 1 < j.size()) {
            ++i;
            out += j[i] == 'n' ? '\n' : j[i];
        } else {
            out += j[i];
        }
    }
    return out;
}
}  // namespace

void CheckUpdates(bool manual) {
    HWND notify = g_main;
    std::thread([manual, notify] {
        auto* r = new UpdateResult();
        r->manual = manual;
        std::string body;
        if (HttpGet(L"api.github.com", L"/repos/Finn013/WVC/releases/tags/latest", &body, &r->err)) {
            const std::string text = JsonStr(body, "body") + "\n" + JsonStr(body, "name");
            int ma = 0, mi = 0;
            std::string vs;
            r->page = FromUtf8(JsonStr(body, "html_url"));
            if (ParseVersion(text, &ma, &mi, &vs)) {
                r->ok = true;
                r->version = FromUtf8(vs);
                r->newer = ma > 0 || mi > WVC_VERSION_NUM;
            } else {
                r->err = L"В описании сборки нет номера версии";
            }
        }
        if (r->ok) LogMsg(L"Проверка обновлений: на GitHub версия " + r->version + (r->newer ? L" (новее)" : L""));
        else LogErr(L"Проверка обновлений: " + r->err);
        if (!notify || !PostMessageW(notify, WM_UPDATE_RESULT, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void OnUpdateResult(LPARAM lp) {
    std::unique_ptr<UpdateResult> r(reinterpret_cast<UpdateResult*>(lp));
    if (!r) return;
    if (r->ok && r->newer) {
        const std::wstring t = L"Вышла новая версия WVC " + r->version + L" (у вас " WVC_VERSION L").\n\n"
                               L"Да — скачать WVC.zip, Нет — открыть страницу сборки, Отмена — позже.\n"
                               L"Распакуйте архив поверх этой папки, когда WVC будет закрыт.\n"
                               L"Если камера в Zoom/Telegram пропадёт — «Настройки → Дополнительно → Установить».";
        const int a = MessageBoxW(g_main, t.c_str(), L"WVC — обновление", MB_YESNOCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON1);
        if (a == IDYES)
            ShellExecuteW(g_main, L"open", L"https://github.com/Finn013/WVC/releases/download/latest/WVC.zip", nullptr,
                          nullptr, SW_SHOWNORMAL);
        else if (a == IDNO && !r->page.empty())
            ShellExecuteW(g_main, L"open", r->page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (!r->manual) return;
    if (r->ok)
        MessageBoxW(g_main, (L"У вас последняя версия: " WVC_VERSION L" (на GitHub — " + r->version + L").").c_str(),
                    L"WVC — обновления", MB_ICONINFORMATION);
    else
        MessageBoxW(g_main, (L"Не удалось проверить обновления: " + r->err +
                             L".\n\nСтраница загрузки: https://github.com/Finn013/WVC/releases/tag/latest")
                                .c_str(),
                    L"WVC — обновления", MB_ICONWARNING);
}
