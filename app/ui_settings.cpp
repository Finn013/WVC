// WVC: «Настройки» in the OBS manner — sections on the left (Общие, Видео, Горячие клавиши, Телефон, Дополнительно),
// OK / Отмена / Применить. Also: hotkeys, autostart, camera install, JSON export / import.
#include "ui_form.h"

// ---------------------------------------------------------------------------
// Hotkeys
// ---------------------------------------------------------------------------
const HotkeyDef kHotkeys[HA_COUNT] = {
    {L"Старт / Стоп камеры", MOD_CONTROL | MOD_SHIFT, VK_F5},
    {L"Выделить область (выбранный источник «Экран»)", MOD_CONTROL | MOD_SHIFT, VK_F6},
    {L"Следующий монитор (для области / монитора)", MOD_CONTROL | MOD_SHIFT, VK_F7},
    {L"Курсор вкл/выкл (источники экрана)", MOD_CONTROL | MOD_SHIFT, VK_F8},
    {L"Сейчас ⇄ Следующий (с переходом)", MOD_CONTROL | MOD_SHIFT, VK_F9},
    {L"Показать/скрыть выбранный источник", MOD_CONTROL | MOD_SHIFT, VK_F10},
    {L"Текст вкл/выкл (все источники «Текст» сцены)", MOD_CONTROL | MOD_SHIFT, VK_F11},
    {L"Сцена 1", MOD_CONTROL | MOD_SHIFT, '1'},
    {L"Сцена 2", MOD_CONTROL | MOD_SHIFT, '2'},
    {L"Сцена 3", MOD_CONTROL | MOD_SHIFT, '3'},
    {L"Сцена 4", MOD_CONTROL | MOD_SHIFT, '4'},
    {L"Поменять местами два верхних источника", MOD_CONTROL | MOD_SHIFT, VK_F12},
    {L"Следующий медиафайл", MOD_CONTROL | MOD_SHIFT, '5'},
    {L"Спрятать / вернуть окно", 0, 0},
    {L"Пауза / продолжить", MOD_CONTROL | MOD_SHIFT, '6'},
};
UINT g_hk[HA_COUNT];
std::wstring g_hotkeyNote;

UINT HkPack(UINT mods, UINT vk) { return vk ? ((mods & 0xFF) << 8) | (vk & 0xFF) : 0; }
static UINT HkMods(UINT v) { return (v >> 8) & 0xFF; }
static UINT HkVk(UINT v) { return v & 0xFF; }

static bool IsExtendedVk(UINT vk) {
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK: return true;
    }
    return false;
}

std::wstring HotkeyName(UINT v) {
    if (!v) return L"—";
    std::wstring s;
    const UINT m = HkMods(v), vk = HkVk(v);
    if (m & MOD_CONTROL) s += L"Ctrl+";
    if (m & MOD_SHIFT) s += L"Shift+";
    if (m & MOD_ALT) s += L"Alt+";
    if (m & MOD_WIN) s += L"Win+";
    wchar_t name[64] = {};
    LONG lp = static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
    if (IsExtendedVk(vk)) lp |= 1 << 24;
    if (GetKeyNameTextW(lp, name, 64) > 0)
        s += name;
    else {
        wchar_t buf[16];
        swprintf(buf, 16, L"0x%02X", vk);
        s += buf;
    }
    return s;
}

void LoadHotkeys() {
    // HK0..HK14 keep their places from v0.10 (HK4 = Ctrl+Shift+F9 now swaps «Сейчас» and «Следующий»;
    // HK15 «Экран 1 ⇄ Экран 2» is gone: there are no fixed screens any more)
    for (int i = 0; i < HA_COUNT; ++i) {
        const int v = g_ini.Int(L"WVC", L"HK" + std::to_wstring(i), -1);
        g_hk[i] = v < 0 ? HkPack(kHotkeys[i].defMods, kHotkeys[i].defVk) : static_cast<UINT>(v);
    }
}

void SaveHotkeysToIni() {
    for (int i = 0; i < HA_COUNT; ++i) g_ini.SetInt(L"WVC", L"HK" + std::to_wstring(i), static_cast<int>(g_hk[i]));
    g_ini.Save(IniPath());
}

void UnregisterHotkeys(HWND hwnd) {
    for (int i = 0; i < HA_COUNT; ++i) UnregisterHotKey(hwnd, i + 1);
}

void RegisterHotkeys(HWND hwnd) {
    UnregisterHotkeys(hwnd);
    std::wstring busy;
    for (int i = 0; i < HA_COUNT; ++i) {
        if (!g_hk[i]) continue;
        if (!RegisterHotKey(hwnd, i + 1, HkMods(g_hk[i]) | MOD_NOREPEAT, HkVk(g_hk[i]))) {
            if (!busy.empty()) busy += L", ";
            busy += HotkeyName(g_hk[i]);
        }
    }
    g_hotkeyNote = busy.empty() ? L"" : L"Клавиши заняты другой программой: " + busy;
}

static WORD ToHotkeyCtl(UINT v) {
    if (!v) return 0;
    const UINT m = HkMods(v);
    BYTE f = 0;
    if (m & MOD_SHIFT) f |= HOTKEYF_SHIFT;
    if (m & MOD_CONTROL) f |= HOTKEYF_CONTROL;
    if (m & MOD_ALT) f |= HOTKEYF_ALT;
    if (IsExtendedVk(HkVk(v))) f |= HOTKEYF_EXT;
    return MAKEWORD(HkVk(v), f);
}

static UINT FromHotkeyCtl(WORD w) {
    const UINT vk = LOBYTE(w), f = HIBYTE(w);
    if (!vk) return 0;
    UINT m = 0;
    if (f & HOTKEYF_SHIFT) m |= MOD_SHIFT;
    if (f & HOTKEYF_CONTROL) m |= MOD_CONTROL;
    if (f & HOTKEYF_ALT) m |= MOD_ALT;
    return HkPack(m, vk);
}

// ---------------------------------------------------------------------------
// Autostart, camera install, JSON
// ---------------------------------------------------------------------------
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool AutostartOn() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    const bool on = RegQueryValueExW(k, L"WVC", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

bool SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    LONG rc;
    if (on) {
        const std::wstring v = L"\"" + ExePath() + L"\" --tray";
        rc = RegSetValueExW(k, L"WVC", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                            static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(k, L"WVC");
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

static const wchar_t* kCamClsid = L"{32E48B39-D01F-4852-9568-C345CD9082BE}";
typedef HRESULT(STDAPICALLTYPE* PFN_DllRegister)();

static int RunWait(const std::wstring& cmdLine) {
    std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// exit codes: 0 ok, 2 = 64-bit ok but 32-bit failed, 10+ = 64-bit failed
int DoRegisterCamera(bool reg) {
    const std::wstring dir = ExeDir();
    const std::wstring dll64 = dir + L"\\WVCCam.dll";
    const std::wstring dll32 = dir + L"\\WVCCam32.dll";
    HMODULE h = LoadLibraryW(dll64.c_str());
    if (!h) return 10;
    auto fn = reinterpret_cast<PFN_DllRegister>(GetProcAddress(h, reg ? "DllRegisterServer" : "DllUnregisterServer"));
    if (!fn) {
        FreeLibrary(h);
        return 11;
    }
    HRESULT hr = fn();
    FreeLibrary(h);
    if (FAILED(hr) && reg) return 12;
    int result = 0;
    if (GetFileAttributesW(dll32.c_str()) != INVALID_FILE_ATTRIBUTES) {
        wchar_t wow[MAX_PATH] = {};
        if (GetSystemWow64DirectoryW(wow, MAX_PATH)) {
            std::wstring cmd = L"\"" + std::wstring(wow) + L"\\regsvr32.exe\" /s " + (reg ? L"" : L"/u ") + L"\"" + dll32 + L"\"";
            if (RunWait(cmd) != 0 && reg) result = 2;
        }
    }
    return result;
}

static int RunElevated(const wchar_t* args) {
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return GetLastError() == ERROR_CANCELLED ? -2 : -1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return static_cast<int>(code);
}

int CameraInstallState() {
    std::wstring key = std::wstring(L"CLSID\\") + kCamClsid + L"\\InprocServer32";
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS) return 0;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    LONG rc = RegQueryValueExW(hk, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS) return 0;
    std::wstring mine = ExeDir() + L"\\WVCCam.dll";
    return _wcsicmp(buf, mine.c_str()) == 0 ? 1 : 2;
}

void DoInstall(bool install) {
    if (!install && MessageBoxW(g_main, L"Удалить камеру «WVC Camera» из системы?", L"WVC", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    const int rc = RunElevated(install ? L"--register" : L"--unregister");
    if (rc == -2) return;
    wchar_t buf[256];
    if (install) {
        if (rc == 0)
            MessageBoxW(g_main, L"Камера установлена. Если Zoom, Telegram или браузер уже открыты, перезапустите их, чтобы они "
                                L"увидели «WVC Camera».", L"WVC", MB_ICONINFORMATION);
        else if (rc == 2)
            MessageBoxW(g_main, L"Камера установлена для 64-битных программ, а 32-битную часть установить не удалось.", L"WVC",
                        MB_ICONWARNING);
        else {
            swprintf(buf, 256, L"Не удалось установить камеру (код %d).", rc);
            MessageBoxW(g_main, buf, L"WVC", MB_ICONERROR);
        }
    } else {
        if (rc == 0) MessageBoxW(g_main, L"Камера удалена.", L"WVC", MB_ICONINFORMATION);
        else {
            swprintf(buf, 256, L"Не удалось удалить камеру (код %d).", rc);
            MessageBoxW(g_main, buf, L"WVC", MB_ICONERROR);
        }
    }
    UiStatus();
}

static bool PickJson(HWND owner, bool save, std::wstring* out) {
    wchar_t file[MAX_PATH * 2] = L"WVC-настройки.json";
    if (!save) file[0] = 0;
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Настройки WVC (*.json)\0*.json\0Все файлы\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
    *out = file;
    return true;
}

void DoExportJson(HWND owner) {
    std::wstring path;
    if (!PickJson(owner, true, &path)) return;
    SaveProject();
    if (IniToJsonFile(g_ini, path))
        MessageBoxW(owner, L"Все настройки (сцены, источники, кадр, клавиши) сохранены в файл.", L"WVC", MB_ICONINFORMATION);
    else
        MessageBoxW(owner, L"Не удалось записать файл.", L"WVC", MB_ICONERROR);
}

void DoImportJson(HWND owner) {
    std::wstring path, err;
    if (!PickJson(owner, false, &path)) return;
    IniDoc doc;
    if (!JsonFileToIni(path, &doc, &err)) {
        MessageBoxW(owner, err.c_str(), L"WVC", MB_ICONERROR);
        return;
    }
    if (MessageBoxW(owner, L"Заменить все текущие настройки (сцены, источники, клавиши) настройками из файла?", L"WVC",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    Project p;
    std::wstring note;
    LoadProjectFromIni(doc, &p, &note);  // files of v0.5–v0.10 are converted here
    g_ini = doc;
    {
        std::lock_guard<std::mutex> lk(g_projMx);
        g_proj = p;
    }
    ++g_projVer;
    SaveProject();
    ReloadAfterImport();
    SetNote(note.empty() ? L"Настройки загружены из файла." : note);
}

void ReloadAfterImport() {
    LoadHotkeys();
    RegisterHotkeys(g_main);
    UiSelect(0);
    UiRefresh();
}

// ---------------------------------------------------------------------------
// The settings window
// ---------------------------------------------------------------------------
namespace {
enum {
    S_SIDEBAR = 100, S_BTNOK, S_BTNCANCEL, S_BTNAPPLY, S_FORM0,
    S_AUTOSTART = 200, S_AUTOSTREAM, S_MINTRAY, S_CLOSETRAY, S_IDLEFRAME, S_LIVE, S_PAUSEMODE, S_PAUSETEXT, S_PAUSEIMG,
    S_PAUSEIMG_BTN,
    S_QUALITY = 300, S_ASPECT, S_NATIVE, S_FPS, S_OUTPREV, S_OSHAPE, S_ORADIUS, S_OFEATHER, S_OMASK, S_OMASK_BTN, S_OCROP0,
    S_OCROP0_UD, S_OCROP1, S_OCROP1_UD, S_OCROP2, S_OCROP2_UD, S_OCROP3, S_OCROP3_UD, S_OBG, S_OCOLOR, S_OIMG, S_OIMG_BTN,
    S_HK0 = 400, S_HKCLR0 = 450, S_HKDEF = 499,
    S_PCONN = 500, S_PRES, S_PCODEC, S_PROT,
    S_KEEPCAM = 600, S_INSTALL, S_UNINSTALL, S_CAMSTATE, S_EXPORT, S_IMPORT, S_OPENDIR,
};
const int kPages = 5;
const wchar_t* kPageNames[kPages] = {L"Общие", L"Видео", L"Горячие клавиши", L"Телефон", L"Дополнительно"};

struct SetState {
    HWND dlg = nullptr, sidebar = nullptr;
    Form forms[kPages];
    std::map<int, HWND> ctl;
    Project work;  // the edited copy
    bool autostart = false, autostream = false;
    UINT hk[HA_COUNT] = {};
    std::vector<int> nativeUids;
    bool done = false, syncing = false;
    int page = 0;
};
SetState* g_ss = nullptr;

HWND Q(int id) {
    auto it = g_ss->ctl.find(id);
    return it == g_ss->ctl.end() ? nullptr : it->second;
}
int CGet(int id) { return static_cast<int>(SendMessageW(Q(id), CB_GETCURSEL, 0, 0)); }
void CSet(int id, int v) { SendMessageW(Q(id), CB_SETCURSEL, v, 0); }
bool BGet(int id) { return SendMessageW(Q(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void BSet(int id, bool v) { SendMessageW(Q(id), BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); }
std::wstring TGet(int id) {
    HWND h = Q(id);
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}
void TSet(int id, const std::wstring& t) { SetWindowTextW(Q(id), t.c_str()); }

const COLORREF kBgColors[5] = {RGB(0, 0, 0), RGB(255, 255, 255), RGB(0, 177, 64), RGB(0, 71, 187), RGB(40, 42, 48)};

void ShowPage(int p) {
    g_ss->page = p;
    for (int i = 0; i < kPages; ++i) ShowWindow(g_ss->forms[i].panel, i == p ? SW_SHOW : SW_HIDE);
    g_pubCanvas.wantW = p == 1 ? 1 : 0;
    if (p == 2) UnregisterHotkeys(g_main);  // so pressing a combo records it instead of firing it
    else RegisterHotkeys(g_main);
}

void SyncAll() {
    SetState& s = *g_ss;
    const Project& p = s.work;
    s.syncing = true;
    BSet(S_AUTOSTART, s.autostart);
    BSet(S_AUTOSTREAM, s.autostream);
    BSet(S_MINTRAY, p.minToTray);
    BSet(S_CLOSETRAY, p.closeToTray);
    BSet(S_IDLEFRAME, p.idleFrame);
    BSet(S_LIVE, p.liveWhenStopped);
    CSet(S_PAUSEMODE, p.pauseMode);
    TSet(S_PAUSETEXT, p.pauseText);
    TSet(S_PAUSEIMG, p.pauseImage);
    CSet(S_QUALITY, p.out.native ? 2 : p.out.quality);
    CSet(S_ASPECT, p.out.aspect);
    EnableWindow(Q(S_ASPECT), !p.out.native);
    // «Как у источника»: every source of every scene
    HWND nc = Q(S_NATIVE);
    SendMessageW(nc, CB_RESETCONTENT, 0, 0);
    s.nativeUids.clear();
    int sel = -1;
    for (const auto& sc : p.scenes)
        for (const auto& it : sc.items) {
            if (it.type == IT_TEXT) continue;
            const std::wstring t = sc.name + L" — " + it.name;
            SendMessageW(nc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
            s.nativeUids.push_back(it.uid);
            if (it.uid == p.out.nativeUid) sel = static_cast<int>(s.nativeUids.size()) - 1;
        }
    SendMessageW(nc, CB_SETCURSEL, sel, 0);
    EnableWindow(nc, p.out.native);
    CSet(S_FPS, p.out.fps == 60 ? 1 : 0);
    CSet(S_OSHAPE, p.out.shape.kind);
    SendMessageW(Q(S_ORADIUS), TBM_SETPOS, TRUE, p.out.shape.radius);
    SendMessageW(Q(S_OFEATHER), TBM_SETPOS, TRUE, p.out.shape.feather);
    TSet(S_OMASK, p.out.shape.mask);
    for (int i = 0; i < 4; ++i) TSet(S_OCROP0 + 2 * i, std::to_wstring((p.out.crop[i] + 5) / 10));
    CSet(S_OBG, p.out.bgMode);
    int ci = 5;
    for (int i = 0; i < 5; ++i)
        if (kBgColors[i] == p.out.bgColor) ci = i;
    CSet(S_OCOLOR, ci);
    TSet(S_OIMG, p.out.bgImage);
    for (int i = 0; i < HA_COUNT; ++i) SendMessageW(Q(S_HK0 + i), HKM_SETHOTKEY, ToHotkeyCtl(s.hk[i]), 0);
    CSet(S_PCONN, p.defPhoneConn);
    CSet(S_PRES, p.defPhoneRes);
    CSet(S_PCODEC, p.defPhoneCodec);
    BSet(S_PROT, p.defAutoRotate);
    BSet(S_KEEPCAM, p.keepCamOn);
    const int inst = CameraInstallState();
    TSet(S_CAMSTATE, inst == 1 ? L"«WVC Camera» установлена из этой папки." : inst == 2 ? L"«WVC Camera» установлена из другой папки — "
                                                                                          L"нажмите «Установить», чтобы использовать эту."
                                                                                        : L"«WVC Camera» не установлена.");
    s.syncing = false;
}

void ReadAll() {
    SetState& s = *g_ss;
    Project& p = s.work;
    s.autostart = BGet(S_AUTOSTART);
    s.autostream = BGet(S_AUTOSTREAM);
    p.minToTray = BGet(S_MINTRAY);
    p.closeToTray = BGet(S_CLOSETRAY);
    p.idleFrame = BGet(S_IDLEFRAME);
    p.liveWhenStopped = BGet(S_LIVE);
    p.pauseMode = std::clamp(CGet(S_PAUSEMODE), 0, 3);
    p.pauseText = TGet(S_PAUSETEXT);
    p.pauseImage = TGet(S_PAUSEIMG);
    const int q = CGet(S_QUALITY);
    p.out.native = q == 2;
    if (q >= 0 && q < 2) p.out.quality = q;
    p.out.aspect = std::clamp(CGet(S_ASPECT), 0, 3);
    const int ni = CGet(S_NATIVE);
    if (ni >= 0 && ni < static_cast<int>(s.nativeUids.size())) p.out.nativeUid = s.nativeUids[static_cast<size_t>(ni)];
    if (p.out.native && !FindItemC(p, p.out.nativeUid)) p.out.native = false;
    p.out.fps = CGet(S_FPS) == 1 ? 60 : 30;
    p.out.shape.kind = std::clamp(CGet(S_OSHAPE), 0, SH_COUNT - 1);
    p.out.shape.radius = static_cast<int>(SendMessageW(Q(S_ORADIUS), TBM_GETPOS, 0, 0));
    p.out.shape.feather = static_cast<int>(SendMessageW(Q(S_OFEATHER), TBM_GETPOS, 0, 0));
    p.out.shape.mask = TGet(S_OMASK);
    for (int i = 0; i < 4; ++i) p.out.crop[i] = std::clamp(_wtoi(TGet(S_OCROP0 + 2 * i).c_str()) * 10, 0, 450);
    p.out.bgMode = std::clamp(CGet(S_OBG), 0, 2);
    const int ci = CGet(S_OCOLOR);
    if (ci >= 0 && ci < 5) p.out.bgColor = kBgColors[ci];
    p.out.bgImage = TGet(S_OIMG);
    for (int i = 0; i < HA_COUNT; ++i)
        s.hk[i] = FromHotkeyCtl(static_cast<WORD>(SendMessageW(Q(S_HK0 + i), HKM_GETHOTKEY, 0, 0)));
    p.defPhoneConn = std::clamp(CGet(S_PCONN), 0, 1);
    p.defPhoneRes = std::clamp(CGet(S_PRES), 0, 1);
    p.defPhoneCodec = std::clamp(CGet(S_PCODEC), 0, 2);
    p.defAutoRotate = BGet(S_PROT);
    p.keepCamOn = BGet(S_KEEPCAM);
}

bool Apply() {
    SetState& s = *g_ss;
    ReadAll();
    for (int i = 0; i < HA_COUNT; ++i)
        for (int j = i + 1; j < HA_COUNT; ++j)
            if (s.hk[i] && s.hk[i] == s.hk[j]) {
                std::wstring t = L"Сочетание " + HotkeyName(s.hk[i]) + L" назначено дважды: «" + kHotkeys[i].name + L"» и «" +
                                 kHotkeys[j].name + L"».";
                MessageBoxW(s.dlg, t.c_str(), L"WVC", MB_ICONWARNING);
                return false;
            }
    const Project w = s.work;
    ProjEdit([&](Project& p) {
        p.out = w.out;
        p.pauseMode = w.pauseMode;
        p.pauseText = w.pauseText;
        p.pauseImage = w.pauseImage;
        p.idleFrame = w.idleFrame;
        p.keepCamOn = w.keepCamOn;
        p.liveWhenStopped = w.liveWhenStopped;
        p.minToTray = w.minToTray;
        p.closeToTray = w.closeToTray;
        p.defPhoneConn = w.defPhoneConn;
        p.defPhoneRes = w.defPhoneRes;
        p.defPhoneCodec = w.defPhoneCodec;
        p.defAutoRotate = w.defAutoRotate;
    });
    g_pauseDirty = true;
    for (int i = 0; i < HA_COUNT; ++i) g_hk[i] = s.hk[i];
    SaveHotkeysToIni();
    if (s.page != 2) RegisterHotkeys(g_main);
    if (s.autostart != AutostartOn() && !SetAutostart(s.autostart))
        MessageBoxW(s.dlg, L"Не удалось изменить автозапуск.", L"WVC", MB_ICONERROR);
    g_ini.SetInt(L"WVC", L"AutoStream", s.autostream);
    SaveProject();
    UiRefresh();
    return true;
}

// --- output frame preview: the whole scene, the part the camera does not show dimmed, the shape outline
void PaintOutPrev(HWND hwnd, HDC dc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(32, 33, 37));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    g_pubCanvas.wantW = (std::max)(16, static_cast<int>(rc.right - 8));
    g_pubCanvas.wantH = (std::max)(16, static_cast<int>(rc.bottom - 8));
    RECT area{4, 4, rc.right - 4, rc.bottom - 4};
    RECT d{};
    PaintPublished(dc, g_pubCanvas, area, &d);
    const int dw = d.right - d.left, dh = d.bottom - d.top;
    if (dw < 8 || dh < 8) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(180, 180, 180));
        DrawTextW(dc, L"Здесь будет кадр сцены", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (!g_ss) return;
    const OutputCfg& o = g_ss->work.out;
    const RectD V = OutputVisibleRect(o, dw, dh);
    const RECT vr{static_cast<LONG>(d.left + V.x0), static_cast<LONG>(d.top + V.y0), static_cast<LONG>(d.left + V.x1),
                  static_cast<LONG>(d.top + V.y1)};
    const int vw = vr.right - vr.left, vh = vr.bottom - vr.top;
    if (!o.shape.Trivial() && vw > 2 && vh > 2) {
        std::vector<uint8_t> m;
        BuildShapeMask(vw, vh, o.shape, m);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = vw;
        bi.bmiHeader.biHeight = -vh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        uint8_t* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        if (bmp && bits) {
            HDC md = CreateCompatibleDC(dc);
            HGDIOBJ ob = SelectObject(md, bmp);
            BitBlt(md, 0, 0, vw, vh, dc, vr.left, vr.top, SRCCOPY);
            GdiFlush();
            for (int y = 0; y < vh; ++y)
                for (int x = 0; x < vw; ++x) {
                    const int a = m[static_cast<size_t>(y) * vw + x];
                    if (a == 255) continue;
                    const int cv = (((x / 8) + (y / 8)) & 1) ? 150 : 204;
                    uint8_t* p = bits + (static_cast<size_t>(y) * vw + x) * 4;
                    for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>((p[c] * a + cv * (255 - a)) / 255);
                }
            BitBlt(dc, vr.left, vr.top, vw, vh, md, 0, 0, SRCCOPY);
            SelectObject(md, ob);
            DeleteDC(md);
        }
        if (bmp) DeleteObject(bmp);
        std::vector<std::vector<POINT>> polys;
        ShapeOutline(vw, vh, o.shape, &polys);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(240, 60, 60));
        HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        for (auto poly : polys) {
            for (auto& pt : poly) {
                pt.x += vr.left;
                pt.y += vr.top;
            }
            if (poly.size() >= 3) Polygon(dc, poly.data(), static_cast<int>(poly.size()));
        }
        SelectObject(dc, obr);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    HDC mdc = CreateCompatibleDC(dc);
    HBITMAP one = CreateCompatibleBitmap(dc, 1, 1);
    HGDIOBJ ob = SelectObject(mdc, one);
    SetPixel(mdc, 0, 0, RGB(0, 0, 0));
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 160, 0};
    const RECT parts[4] = {{d.left, d.top, d.right, vr.top},
                           {d.left, vr.bottom, d.right, d.bottom},
                           {d.left, vr.top, vr.left, vr.bottom},
                           {vr.right, vr.top, d.right, vr.bottom}};
    for (const RECT& pr : parts)
        if (pr.right > pr.left && pr.bottom > pr.top)
            AlphaBlend(dc, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, mdc, 0, 0, 1, 1, bf);
    SelectObject(mdc, ob);
    DeleteObject(one);
    DeleteDC(mdc);
    HPEN pen = CreatePen(PS_DOT, 1, RGB(255, 210, 0));
    HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, vr.left, vr.top, vr.right, vr.bottom);
    SelectObject(dc, obr);
    SelectObject(dc, op);
    DeleteObject(pen);
}

LRESULT CALLBACK OutPrevProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC m = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, (std::max)(1L, rc.right), (std::max)(1L, rc.bottom));
        HGDIOBJ old = SelectObject(m, bmp);
        SelectObject(m, g_font);
        PaintOutPrev(hwnd, m);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, old);
        DeleteObject(bmp);
        DeleteDC(m);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OnSetCommand(int id, int code) {
    SetState& s = *g_ss;
    switch (id) {
    case S_BTNOK:
        if (Apply()) s.done = true;
        return;
    case S_BTNCANCEL: s.done = true; return;
    case S_BTNAPPLY: Apply(); return;
    case S_SIDEBAR:
        if (code == LBN_SELCHANGE) ShowPage(static_cast<int>(SendMessageW(s.sidebar, LB_GETCURSEL, 0, 0)));
        return;
    case S_PAUSEIMG_BTN: {
        std::wstring f;
        if (PickFile(s.dlg, kImageFilter(), &f)) TSet(S_PAUSEIMG, f);
        return;
    }
    case S_OMASK_BTN: {
        std::wstring f;
        if (PickFile(s.dlg, L"PNG-маска\0*.png\0Картинки\0*.png;*.bmp;*.jpg\0", &f)) TSet(S_OMASK, f);
        break;
    }
    case S_OIMG_BTN: {
        std::wstring f;
        if (PickFile(s.dlg, kImageFilter(), &f)) {
            TSet(S_OIMG, f);
            CSet(S_OBG, 2);
        }
        break;
    }
    case S_HKDEF:
        for (int i = 0; i < HA_COUNT; ++i)
            SendMessageW(Q(S_HK0 + i), HKM_SETHOTKEY, ToHotkeyCtl(HkPack(kHotkeys[i].defMods, kHotkeys[i].defVk)), 0);
        return;
    case S_INSTALL: DoInstall(true); SyncAll(); return;
    case S_UNINSTALL: DoInstall(false); SyncAll(); return;
    case S_EXPORT: DoExportJson(s.dlg); return;
    case S_IMPORT:
        DoImportJson(s.dlg);
        s.work = ProjSnapshot();
        for (int i = 0; i < HA_COUNT; ++i) s.hk[i] = g_hk[i];
        SyncAll();
        return;
    case S_OPENDIR: {
        std::wstring dir = IniPath();
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        ShellExecuteW(s.dlg, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    default:
        if (id >= S_HKCLR0 && id < S_HKCLR0 + HA_COUNT) {
            SendMessageW(Q(S_HK0 + id - S_HKCLR0), HKM_SETHOTKEY, 0, 0);
            return;
        }
    }
    if (s.syncing) return;
    // live: what the output preview needs
    if (code == CBN_SELCHANGE || code == EN_CHANGE || code == BN_CLICKED) {
        ReadAll();
        if (id == S_QUALITY) {
            EnableWindow(Q(S_ASPECT), CGet(S_QUALITY) != 2);
            EnableWindow(Q(S_NATIVE), CGet(S_QUALITY) == 2);
        }
        if (id == S_OCOLOR && CGet(S_OCOLOR) == 5) {
            static COLORREF custom[16];
            CHOOSECOLORW cc{};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = s.dlg;
            cc.lpCustColors = custom;
            cc.rgbResult = s.work.out.bgColor;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) s.work.out.bgColor = cc.rgbResult;
        }
        InvalidateRect(Q(S_OUTPREV), nullptr, FALSE);
    }
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (g_ss) {
            int id = LOWORD(wp);
            if (id == IDCANCEL) id = S_BTNCANCEL;  // Esc
            OnSetCommand(id, HIWORD(wp));
        }
        return 0;
    case WM_HSCROLL:
        if (g_ss && !g_ss->syncing) {
            ReadAll();
            InvalidateRect(Q(S_OUTPREV), nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (g_ss && g_ss->page == 1) InvalidateRect(Q(S_OUTPREV), nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        if (g_ss) g_ss->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void BuildPages(SetState& s, int formW, int formH, int formX) {
    auto keep = [&](int id, HWND h) {
        s.ctl[id] = h;
        return h;
    };
    for (int i = 0; i < kPages; ++i) {
        s.forms[i].Create(s.dlg, S_FORM0 + i);
        MoveWindow(s.forms[i].panel, formX, 0, formW, formH, FALSE);
        s.forms[i].cw = MulDiv(formW, 96, static_cast<int>(g_dpi)) - 190 - 30;
    }
    {  // Общие
        Form& f = s.forms[0];
        f.Header(L"Общие");
        keep(S_AUTOSTART, f.Check(L"Запускать WVC вместе с Windows (в трее)", S_AUTOSTART));
        keep(S_AUTOSTREAM, f.Check(L"При запуске сразу включать камеру", S_AUTOSTREAM));
        keep(S_MINTRAY, f.Check(L"Сворачивать в трей", S_MINTRAY));
        keep(S_CLOSETRAY, f.Check(L"Крестик прячет окно в трей (выход — «Выход»)", S_CLOSETRAY));
        keep(S_IDLEFRAME, f.Check(L"Зелёная рамка вокруг области, пока камера остановлена", S_IDLEFRAME));
        keep(S_LIVE, f.Check(L"Живой предпросмотр, пока камера остановлена (источники включены)", S_LIVE));
        f.Hint(L"Без живого предпросмотра веб-камера и телефон включаются только во время трансляции, как раньше.", 2);
        f.Header(L"Пауза и остановленная камера");
        keep(S_PAUSEMODE, f.Combo(L"Что показывать:", S_PAUSEMODE, {L"Последний кадр", L"Чёрный экран", L"Текст (ниже)", L"Картинку"}, 220));
        keep(S_PAUSETEXT, f.Row(L"Текст паузы:", L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, S_PAUSETEXT, 24, -1, WS_EX_CLIENTEDGE));
        const int y = f.y;
        f.Make(L"STATIC", L"Картинка паузы:", SS_LEFT, -1, f.lx, y + 4, f.lw, 20);
        keep(S_PAUSEIMG, f.Make(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, S_PAUSEIMG, f.cx, y, f.cw - 96, 24, WS_EX_CLIENTEDGE));
        keep(S_PAUSEIMG_BTN, f.Make(L"BUTTON", L"Выбрать…", WS_TABSTOP | BS_PUSHBUTTON, S_PAUSEIMG_BTN, f.cx + f.cw - 90, y, 90, 26));
        f.y += 34;
        f.Finish();
    }
    {  // Видео
        Form& f = s.forms[1];
        f.Header(L"Формат камеры «WVC Camera»");
        keep(S_QUALITY, f.Combo(L"Размер:", S_QUALITY, {L"720p (1280×720 при 16:9)", L"1080p (1920×1080 при 16:9)", L"Как у источника"}, 240));
        keep(S_ASPECT, f.Combo(L"Пропорции:", S_ASPECT, {L"16:9", L"4:3", L"1:1", L"9:16 (вертикально)"}, 240));
        keep(S_NATIVE, f.Combo(L"Источник (для «как у»):", S_NATIVE, {}));
        keep(S_FPS, f.Combo(L"Кадров в секунду:", S_FPS, {L"30", L"60"}, 120));
        f.Hint(L"«Как у источника» — размер камеры = размер картинки выбранного источника (напр. веб-камера 1280×720, "
               L"телефон 1080×1920), не больше 1920 по длинной стороне. Смена формата перезапускает камеру: в "
               L"Zoom/Telegram выключите и включите видео.", 3);
        f.Header(L"Кадр камеры (вывод)");
        keep(S_OUTPREV, f.Make(L"WVCOutPrev", L"", 0, S_OUTPREV, f.lx, f.y, f.cx + f.cw - f.lx, 190));
        f.y += 196;
        f.Hint(L"Затемнено — что камера не покажет (обрезка). Шахматка — место вне формы: в камере там цвет, размытие "
               L"или картинка (прозрачности у камеры нет).", 2);
        HWND sh = keep(S_OSHAPE, f.Combo(L"Форма:", S_OSHAPE, {}, 260));
        for (int k = 0; k < SH_COUNT; ++k) SendMessageW(sh, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kShapeNames[k]));
        keep(S_ORADIUS, f.Track(L"Радиус скругления:", S_ORADIUS, 0, 50));
        keep(S_OFEATHER, f.Track(L"Мягкий край:", S_OFEATHER, 0, 30));
        {
            const int y = f.y;
            f.Make(L"STATIC", L"PNG-маска:", SS_LEFT, -1, f.lx, y + 4, f.lw, 20);
            keep(S_OMASK, f.Make(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, S_OMASK, f.cx, y, f.cw - 96, 24, WS_EX_CLIENTEDGE));
            keep(S_OMASK_BTN, f.Make(L"BUTTON", L"Выбрать…", WS_TABSTOP | BS_PUSHBUTTON, S_OMASK_BTN, f.cx + f.cw - 90, y, 90, 26));
            f.y += 32;
        }
        const wchar_t* names[4] = {L"Обрезка слева, %:", L"Обрезка сверху, %:", L"Обрезка справа, %:", L"Обрезка снизу, %:"};
        for (int i = 0; i < 4; ++i) keep(S_OCROP0 + 2 * i, f.Spin(names[i], S_OCROP0 + 2 * i, 0, 45, 80));
        keep(S_OBG, f.Combo(L"Вне формы:", S_OBG, {L"Цвет", L"Размытая сцена", L"Картинка"}, 220));
        keep(S_OCOLOR, f.Combo(L"Цвет:", S_OCOLOR,
                               {L"Чёрный", L"Белый", L"Зелёный (хромакей)", L"Синий (хромакей)", L"Тёмно-серый", L"Свой цвет…"}, 220));
        {
            const int y = f.y;
            f.Make(L"STATIC", L"Картинка фона:", SS_LEFT, -1, f.lx, y + 4, f.lw, 20);
            keep(S_OIMG, f.Make(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, S_OIMG, f.cx, y, f.cw - 96, 24, WS_EX_CLIENTEDGE));
            keep(S_OIMG_BTN, f.Make(L"BUTTON", L"Выбрать…", WS_TABSTOP | BS_PUSHBUTTON, S_OIMG_BTN, f.cx + f.cw - 90, y, 90, 26));
            f.y += 34;
        }
        f.Finish();
    }
    {  // Горячие клавиши
        Form& f = s.forms[2];
        f.Header(L"Горячие клавиши (работают везде, даже когда окно скрыто)");
        f.Hint(L"Щёлкните в поле и нажмите сочетание (Ctrl, Shift или Alt + клавиша). ✕ — убрать.", 1);
        for (int i = 0; i < HA_COUNT; ++i) {
            const int y = f.y;
            f.Make(L"STATIC", kHotkeys[i].name, SS_LEFT | SS_ENDELLIPSIS, -1, f.lx, y + 4, 250, 20);
            HWND hk = keep(S_HK0 + i, f.Make(HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, S_HK0 + i, 270, y, 170, 24));
            SendMessageW(hk, HKM_SETRULES, HKCOMB_NONE | HKCOMB_S, MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_SHIFT, 0));
            f.Make(L"BUTTON", L"✕", WS_TABSTOP | BS_PUSHBUTTON, S_HKCLR0 + i, 446, y, 30, 24);
            f.y += 30;
        }
        f.y += 6;
        f.Make(L"BUTTON", L"По умолчанию", WS_TABSTOP | BS_PUSHBUTTON, S_HKDEF, f.lx, f.y, 140, 28);
        f.y += 36;
        f.Finish();
    }
    {  // Телефон
        Form& f = s.forms[3];
        f.Header(L"Новый источник «Телефон» по умолчанию");
        keep(S_PCONN, f.Combo(L"Подключение:", S_PCONN, {L"Wi-Fi", L"USB-кабель (adb)"}, 200));
        keep(S_PRES, f.Combo(L"Качество:", S_PRES, {L"720p (быстрее)", L"1080p (чётче)"}, 200));
        keep(S_PCODEC, f.Combo(L"Передача:", S_PCODEC,
                               {L"Авто: H.264 (быстро), иначе JPEG", L"JPEG (совместимость)", L"Камера через экран (запасной способ)"}));
        keep(S_PROT, f.Check(L"Автоориентация", S_PROT));
        f.Hint(L"На телефон установите WVC-Phone.apk (из того же релиза). H.264 работает с WVC Phone 0.11 и новее; со "
               L"старым приложением WVC сам перейдёт на JPEG. По USB нужна «Отладка по USB» на телефоне.", 3);
        f.Finish();
    }
    {  // Дополнительно
        Form& f = s.forms[4];
        f.Header(L"Источники");
        keep(S_KEEPCAM, f.Check(L"Держать веб-камеры включёнными (мгновенное переключение)", S_KEEPCAM));
        f.Hint(L"Минус: индикатор камеры горит всегда, и другие программы не смогут её открыть.", 1);
        f.Header(L"Камера в Windows");
        keep(S_CAMSTATE, f.Label(L"", 20));
        {
            const int y = f.y;
            f.Make(L"BUTTON", L"Установить камеру", WS_TABSTOP | BS_PUSHBUTTON, S_INSTALL, f.lx, y, 170, 28);
            f.Make(L"BUTTON", L"Удалить камеру", WS_TABSTOP | BS_PUSHBUTTON, S_UNINSTALL, f.lx + 178, y, 150, 28);
            f.y += 36;
        }
        f.Hint(L"Установка один раз, нужны права администратора. Файлы остаются в папке WVC — не перемещайте её после "
               L"установки.", 2);
        f.Header(L"Файл настроек");
        {
            const int y = f.y;
            f.Make(L"BUTTON", L"Сохранить в JSON…", WS_TABSTOP | BS_PUSHBUTTON, S_EXPORT, f.lx, y, 170, 28);
            f.Make(L"BUTTON", L"Загрузить из JSON…", WS_TABSTOP | BS_PUSHBUTTON, S_IMPORT, f.lx + 178, y, 170, 28);
            f.Make(L"BUTTON", L"Открыть папку", WS_TABSTOP | BS_PUSHBUTTON, S_OPENDIR, f.lx + 356, y, 130, 28);
            f.y += 36;
        }
        f.Hint(L"JSON прежних версий (0.5–0.10) тоже загружается: сцены и источники переводятся в новый вид.", 1);
        f.Header(L"О программе");
        f.Label(L"WVC " WVC_VERSION L" — лёгкая виртуальная камера в стиле OBS. https://github.com/Finn013/WVC", 20);
        f.Finish();
    }
}
}  // namespace

void OpenSettings(int page) {
    if (g_ss) return;
    SetState st;
    st.work = ProjSnapshot();
    st.autostart = AutostartOn();
    st.autostream = g_ini.Int(L"WVC", L"AutoStream", 0) != 0;
    for (int i = 0; i < HA_COUNT; ++i) st.hk[i] = g_hk[i];
    g_ss = &st;
    static bool reg = false;
    if (!reg) {
        reg = true;
        WNDCLASSW wc{};
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpfnWndProc = SettingsProc;
        wc.lpszClassName = L"WVCSettings";
        RegisterClassW(&wc);
        wc.lpfnWndProc = OutPrevProc;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"WVCOutPrev";
        RegisterClassW(&wc);
    }
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int cw = S(780);
    const int ch = (std::min)(S(640), static_cast<int>(work.bottom - work.top) - S(60));
    RECT r{0, 0, cw, ch};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, g_dpi);
    RECT mr;
    GetWindowRect(g_main, &mr);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = mr.left + ((mr.right - mr.left) - w) / 2;
    const int y = (std::max)(static_cast<int>(work.top), static_cast<int>(mr.top + ((mr.bottom - mr.top) - h) / 2));
    st.dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"WVCSettings", L"Настройки", style, x, y, w, h,
                             g_main, nullptr, g_hInst, nullptr);
    if (!st.dlg) {
        g_ss = nullptr;
        return;
    }
    const int sideW = S(170), btnH = S(50);
    st.sidebar = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                 S(8), S(8), sideW - S(8), ch - btnH - S(8), st.dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(S_SIDEBAR)), g_hInst, nullptr);
    SendMessageW(st.sidebar, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(st.sidebar, LB_SETITEMHEIGHT, 0, S(30));
    for (const wchar_t* n : kPageNames) SendMessageW(st.sidebar, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
    BuildPages(st, cw - sideW - S(8), ch - btnH, sideW + S(8));
    MakeChild(st.dlg, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, S_BTNOK, 0, 0, 10, 10);
    MakeChild(st.dlg, L"BUTTON", L"Отмена", WS_TABSTOP | BS_PUSHBUTTON, S_BTNCANCEL, 0, 0, 10, 10);
    MakeChild(st.dlg, L"BUTTON", L"Применить", WS_TABSTOP | BS_PUSHBUTTON, S_BTNAPPLY, 0, 0, 10, 10);
    const int by = ch - btnH + S(9);
    MoveWindow(GetDlgItem(st.dlg, S_BTNOK), cw - S(366), by, S(110), S(32), FALSE);
    MoveWindow(GetDlgItem(st.dlg, S_BTNCANCEL), cw - S(248), by, S(110), S(32), FALSE);
    MoveWindow(GetDlgItem(st.dlg, S_BTNAPPLY), cw - S(130), by, S(118), S(32), FALSE);
    SyncAll();
    page = std::clamp(page, 0, kPages - 1);
    SendMessageW(st.sidebar, LB_SETCURSEL, page, 0);
    ShowPage(page);
    SetTimer(st.dlg, 1, 200, nullptr);
    RunModal(st.dlg, &st.done);
    g_pubCanvas.wantW = 0;
    RegisterHotkeys(g_main);
    g_ss = nullptr;
    UiRefresh();
}
