// Cogitator Setup 2.1 — Windows 11 (Fluent) installer / uninstaller. Developer: Sallador.
// Per-user install (no admin): %LOCALAPPDATA%\Programs\Cogitator
//
//   Setup.exe                                  interactive install
//   Setup.exe /S [/D=<dir>] [/nodesktop] [/noautostart] [/nolaunch]   silent install
//   uninstall.exe /uninstall [/S]              remove (registered in "Installed apps")

#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <wchar.h>
#include "resource.h"
#include "fluent.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

#define APP_NAME     L"Cogitator"
#define APP_EXE      L"Cogitator.exe"
#define APP_VERSION  L"2.1.0"
#define PUBLISHER    L"Sallador"
#define APP_CLASS    L"CogitatorWidget"
#define OLD_CLASS    L"Cogitator40kWidget"                 // v2.0
#define LEGACY_CLASS L"CyberMon2077Widget"                 // v1.x
#define REG_APP      L"Software\\Sallador\\Cogitator"
#define REG_RUN      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_UNINST   L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Cogitator"
#define APP_EXIT_CMD 113
#define LNK_APP      L"\\Cogitator.lnk"

#define BASE_W 520
#define BASE_H 440
#define FOOTER_H 80
#define TITLE_H 32

enum { H_NONE = -1, H_MIN, H_CLOSEX, H_BROWSE, H_DESK, H_AUTO, H_LAUNCH, H_CANCEL, H_INSTALL, H_COUNT };
enum { SS_INFO, SS_OK, SS_ERR };

static HINSTANCE g_hInst;
static HWND   g_hwnd;
static int    g_w, g_h;
static Canvas g_cv;
static RECT   g_rc[H_COUNT];
static int    g_hover = H_NONE, g_press = H_NONE;
static BOOL   g_tracking;
static WCHAR  g_dir[MAX_PATH];
static BOOL   g_optDesk = TRUE, g_optAuto = TRUE, g_optLaunch = TRUE;
static int    g_state;                 // 0 = ready, 1 = installed
static WCHAR  g_status[256];
static int    g_statusKind = SS_INFO;

// ───────────────────────────── helpers ───────────────────────────────
static void KnownPath(REFKNOWNFOLDERID id, WCHAR* out, const WCHAR* tail) {
    PWSTR p = NULL; out[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p))) { swprintf_s(out, MAX_PATH, L"%s%s", p, tail); CoTaskMemFree(p); }
}
static void DelKnown(REFKNOWNFOLDERID id, const WCHAR* tail) {
    WCHAR p[MAX_PATH]; KnownPath(id, p, tail); if (p[0]) DeleteFileW(p);
}
static void JoinPath(WCHAR* out, const WCHAR* dir, const WCHAR* file) { swprintf_s(out, MAX_PATH, L"%s\\%s", dir, file); }

static void CloseWidget(const WCHAR* cls) {
    for (int guard = 0; guard < 4; guard++) {
        HWND w = FindWindowW(cls, NULL);
        if (!w) return;
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        HANDLE ph = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        PostMessageW(w, WM_COMMAND, APP_EXIT_CMD, 0);
        if (ph) {
            if (WaitForSingleObject(ph, 3000) == WAIT_TIMEOUT) TerminateProcess(ph, 0);
            WaitForSingleObject(ph, 2000);
            CloseHandle(ph);
        } else Sleep(800);
    }
}
static void CloseApp() { CloseWidget(APP_CLASS); CloseWidget(OLD_CLASS); CloseWidget(LEGACY_CLASS); }
static void RegDelValue(const WCHAR* key, const WCHAR* name) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, name); RegCloseKey(k); }
}
static void RegSetStr(HKEY k, const WCHAR* name, const WCHAR* v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v, (DWORD)((wcslen(v) + 1) * sizeof(WCHAR)));
}
static void RegSetDword(HKEY k, const WCHAR* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); }

static BOOL MakeShortcut(const WCHAR* lnk, const WCHAR* target, const WCHAR* args, const WCHAR* workdir, const WCHAR* desc) {
    IShellLinkW* sl = NULL; BOOL ok = FALSE;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) {
        sl->SetPath(target); if (args) sl->SetArguments(args);
        sl->SetWorkingDirectory(workdir); sl->SetDescription(desc); sl->SetIconLocation(target, 0);
        IPersistFile* pf = NULL;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { ok = SUCCEEDED(pf->Save(lnk, TRUE)); pf->Release(); }
        sl->Release();
    }
    return ok;
}
static BOOL ExtractPayload(const WCHAR* path) {
    HRSRC r = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDR_PAYLOAD), RT_RCDATA);
    if (!r) return FALSE;
    HGLOBAL g = LoadResource(g_hInst, r);
    DWORD size = SizeofResource(g_hInst, r);
    const void* data = g ? LockResource(g) : NULL;
    if (!data) return FALSE;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wr = 0; BOOL ok = WriteFile(f, data, size, &wr, NULL) && wr == size;
    CloseHandle(f);
    return ok;
}

// ───────────────────────────── install / uninstall ───────────────────
static BOOL DoInstall(WCHAR* err, int errN) {
    CloseApp();
    int rc = SHCreateDirectoryExW(NULL, g_dir, NULL);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) {
        swprintf_s(err, errN, L"Не удалось создать папку (код %d). Выберите другую папку.", rc); return FALSE;
    }
    WCHAR exe[MAX_PATH], unin[MAX_PATH], self[MAX_PATH];
    JoinPath(exe, g_dir, APP_EXE);
    JoinPath(unin, g_dir, L"uninstall.exe");
    if (!ExtractPayload(exe)) { swprintf_s(err, errN, L"Нет доступа к папке. Выберите другую папку."); return FALSE; }
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (_wcsicmp(self, unin) != 0) CopyFileW(self, unin, FALSE);

    RegDelValue(REG_RUN, L"CyberMon");                     // old CyberMon autostart entry
    WCHAR lnk[MAX_PATH];
    KnownPath(FOLDERID_Programs, lnk, LNK_APP);
    if (lnk[0]) MakeShortcut(lnk, exe, NULL, g_dir, L"Cogitator — монитор приложений (Sallador)");
    KnownPath(FOLDERID_Desktop, lnk, LNK_APP);
    if (lnk[0]) { if (g_optDesk) MakeShortcut(lnk, exe, NULL, g_dir, L"Cogitator — монитор приложений (Sallador)"); else DeleteFileW(lnk); }

    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (g_optAuto) { WCHAR cmd[MAX_PATH + 32]; swprintf_s(cmd, L"\"%s\"", exe); RegSetStr(k, APP_NAME, cmd); }
        else RegDeleteValueW(k, APP_NAME);
        RegCloseKey(k);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_APP, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetStr(k, L"InstallDir", g_dir); RegCloseKey(k);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_UNINST, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        WCHAR s[MAX_PATH + 32];
        RegSetStr(k, L"DisplayName", L"Cogitator");
        RegSetStr(k, L"DisplayVersion", APP_VERSION);
        RegSetStr(k, L"Publisher", PUBLISHER);
        RegSetStr(k, L"DisplayIcon", exe);
        RegSetStr(k, L"InstallLocation", g_dir);
        swprintf_s(s, L"\"%s\" /uninstall", unin);    RegSetStr(k, L"UninstallString", s);
        swprintf_s(s, L"\"%s\" /uninstall /S", unin); RegSetStr(k, L"QuietUninstallString", s);
        RegSetStr(k, L"Comments", L"Desktop widget: top applications by CPU / GPU / RAM. Developer: Sallador");
        RegSetDword(k, L"NoModify", 1); RegSetDword(k, L"NoRepair", 1); RegSetDword(k, L"EstimatedSize", 640);
        RegCloseKey(k);
    }
    if (g_optLaunch) ShellExecuteW(NULL, L"open", exe, NULL, g_dir, SW_SHOWNORMAL);
    return TRUE;
}

static void DoUninstall(BOOL silent) {
    if (!silent && MessageBoxW(NULL, L"Удалить Cogitator с этого компьютера?", L"Cogitator",
                               MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    WCHAR self[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wcscpy_s(dir, self); WCHAR* slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;

    CloseApp();
    RegDelValue(REG_RUN, APP_NAME);
    DelKnown(FOLDERID_Programs, LNK_APP);
    DelKnown(FOLDERID_Desktop, LNK_APP);
    RegDeleteTreeW(HKEY_CURRENT_USER, REG_APP);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Sallador");   // only succeeds if empty
    RegDeleteTreeW(HKEY_CURRENT_USER, REG_UNINST);
    JoinPath(path, dir, APP_EXE);
    for (int i = 0; i < 10 && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(path); i++) Sleep(300);

    WCHAR cmd[MAX_PATH * 3];
    swprintf_s(cmd, L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & del /f /q \"%s\" & rmdir \"%s\"", self, dir);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    WCHAR sysdir[MAX_PATH]; GetSystemDirectoryW(sysdir, MAX_PATH);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sysdir, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    if (!silent) MessageBoxW(NULL, L"Cogitator удалён.\n\nРазработчик: Sallador", L"Cogitator", MB_OK | MB_ICONINFORMATION);
}

// ───────────────────────────── UI ────────────────────────────────────
static void SetR(int id, int x, int y, int w, int h) { g_rc[id] = { S(x), S(y), S(x + w), S(y + h) }; }
static void Layout() {
    const int W = BASE_W, H = BASE_H;
    SetR(H_MIN,    W - 92, 0, 46, TITLE_H);
    SetR(H_CLOSEX, W - 46, 0, 46, TITLE_H);
    SetR(H_BROWSE, W - 24 - 110, 166, 110, 32);
    SetR(H_DESK,   24, 214, W - 48, 24);
    SetR(H_AUTO,   24, 248, W - 48, 24);
    SetR(H_LAUNCH, 24, 282, W - 48, 24);
    SetR(H_CANCEL,  W - 24 - 120 - 8 - 120, H - 56, 120, 32);
    SetR(H_INSTALL, W - 24 - 120, H - 56, 120, 32);
    g_w = S(BASE_W); g_h = S(BASE_H);
}
static BOOL Visible(int id) { return g_state == 0 || id == H_MIN || id == H_CLOSEX || id == H_INSTALL; }
static int HitTest(POINT p) {
    for (int i = 0; i < H_COUNT; i++) if (Visible(i) && PtInRect(&g_rc[i], p)) return i;
    return H_NONE;
}
static BOOL Hv(int id) { return g_hover == id && (g_press == H_NONE || g_press == id); }
static BOOL Pr(int id) { return g_press == id && g_hover == id; }

static void PaintTo(Graphics& g) {
    const int W = BASE_W, H = BASE_H;

    // ── title bar
    DrawAppMark(g, D(12), D(8), D(16));
    FText(g, L"Установка Cogitator", g_fCaption, T.text, RectF(D(40), 0, D(300), D(TITLE_H)));
    RectF rm = RF(g_rc[H_MIN]), rx = RF(g_rc[H_CLOSEX]);
    if (Hv(H_MIN)) FFillRect(g, rm.X, rm.Y, rm.Width, rm.Height, Pr(H_MIN) ? T.subtlePress : T.subtleHover);
    FGlyph(g, GL_MINIMIZE, D(10), T.text, rm);
    BOOL closeHot = Hv(H_CLOSEX);
    if (closeHot) FFillRect(g, rx.X, rx.Y, rx.Width, rx.Height, Pr(H_CLOSEX) ? Color(0xE6, 0xC4, 0x2B, 0x1C) : Color(255, 0xC4, 0x2B, 0x1C));
    FGlyph(g, GL_CLOSE, D(10), closeHot ? Color(255, 255, 255, 255) : T.text, rx);

    // ── hero
    DrawAppMark(g, D(24), D(52), D(56));
    FText(g, L"Cogitator", g_fSubtitle, T.text, RectF(D(96), D(50), D(380), D(30)));
    FText(g, L"Монитор приложений для рабочего стола", g_fBody, T.text2, RectF(D(96), D(80), D(380), D(22)));
    FText(g, L"Версия " APP_VERSION L"  ·  Разработчик: Sallador", g_fCaption, T.text3, RectF(D(96), D(102), D(380), D(20)));

    if (g_state == 0) {
        FText(g, L"Папка установки", g_fStrong, T.text, RectF(D(24), D(138), D(300), D(24)));
        RectF tb(D(24), D(166), D(W - 24 - 110 - 8 - 24), D(32));
        FillRR(g, tb, D(4), T.ctrl);
        StrokeRR(g, tb, D(4), T.ctrlStroke);
        FText(g, g_dir, g_fBody, T.text, RectF(tb.X + D(12), tb.Y, tb.Width - D(24), tb.Height), AL_LEFT, StringTrimmingEllipsisPath);
        FButton(g, RF(g_rc[H_BROWSE]), L"Обзор…", FALSE, Hv(H_BROWSE), Pr(H_BROWSE), GL_FOLDER);

        const WCHAR* labels[3] = { L"Ярлык на рабочем столе", L"Запускать вместе с Windows", L"Запустить после установки" };
        BOOL vals[3] = { g_optDesk, g_optAuto, g_optLaunch };
        for (int i = 0; i < 3; i++) {
            RectF r = RF(g_rc[H_DESK + i]);
            FCheck(g, r.X, r.Y + D(2), vals[i], Hv(H_DESK + i));
            FText(g, labels[i], g_fBody, T.text, RectF(r.X + D(32), r.Y, r.Width - D(32), r.Height));
        }
        Color sc = g_statusKind == SS_ERR ? T.critical : g_statusKind == SS_OK ? T.success : T.text3;
        FText(g, g_status, g_fCaption, sc, RectF(D(24), D(316), D(W - 48), D(22)), AL_LEFT, StringTrimmingEllipsisCharacter);
    } else {
        RectF ib(D(24), D(140), D(W - 48), D(64));
        FillRR(g, ib, D(4), T.successBg);
        StrokeRR(g, ib, D(4), T.cardStroke);
        FillCircle(g, ib.X + D(24), ib.Y + D(22), D(8), T.success);
        FGlyph(g, GL_CHECK, D(10), T.onAccent, RectF(ib.X + D(16), ib.Y + D(14), D(16), D(16)));
        FText(g, L"Установка завершена", g_fStrong, T.text, RectF(ib.X + D(44), ib.Y + D(10), ib.Width - D(56), D(24)));
        FText(g, g_dir, g_fBody, T.text2, RectF(ib.X + D(44), ib.Y + D(32), ib.Width - D(56), D(24)), AL_LEFT, StringTrimmingEllipsisPath);

        static const WCHAR glyphs[3] = { GL_PLAY, GL_MORE, GL_SETTINGS };
        static const WCHAR* hints[3] = {
            L"Виджет появится в правом верхнем углу экрана",
            L"Перетаскивайте его мышью; меню — правый клик или «…»",
            L"Удалить можно в Параметрах → Приложения" };
        for (int i = 0; i < 3; i++) {
            REAL y = D(224 + 32 * i);
            FGlyph(g, glyphs[i], D(16), T.text2, RectF(D(24), y, D(20), D(24)));
            FText(g, hints[i], g_fBody, T.text2, RectF(D(56), y, D(W - 80), D(24)), AL_LEFT, StringTrimmingEllipsisCharacter);
        }
    }

    // ── footer
    REAL fy = D(H - FOOTER_H);
    FFillRect(g, 0, fy, (REAL)g_w, (REAL)g_h - fy, T.footer);
    FFillRect(g, 0, fy, (REAL)g_w, 1.0f, T.divider);
    FText(g, L"© 2026 Sallador", g_fCaption, T.text3, RectF(D(24), fy, D(200), D(FOOTER_H)));
    if (g_state == 0) FButton(g, RF(g_rc[H_CANCEL]), L"Отмена", FALSE, Hv(H_CANCEL), Pr(H_CANCEL));
    FButton(g, RF(g_rc[H_INSTALL]), g_state == 1 ? L"Готово" : L"Установить", TRUE, Hv(H_INSTALL), Pr(H_INSTALL));
}

static void Paint(HDC hdc) {
    if (!CanvasEnsure(g_cv, g_w, g_h)) return;
    {
        Bitmap bm(g_w, g_h, g_w * 4, PixelFormat32bppPARGB, (BYTE*)g_cv.bits);
        Graphics g(&bm);
        Prep(g);
        g.Clear(T.backdrop ? Color(0, 0, 0, 0) : T.solidBg);
        PaintTo(g);
    }
    BitBlt(hdc, 0, 0, g_w, g_h, g_cv.dc, 0, 0, SRCCOPY);
}

static void Browse() {
    IFileOpenDialog* d = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0; d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Папка установки Cogitator");
    if (SUCCEEDED(d->Show(g_hwnd))) {
        IShellItem* it = NULL;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                size_t n = wcslen(p);
                if (n >= 9 && _wcsicmp(p + n - 9, L"Cogitator") == 0) wcscpy_s(g_dir, p);
                else swprintf_s(g_dir, L"%s%sCogitator", p, (n && p[n - 1] == L'\\') ? L"" : L"\\");
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
}

static void OnClick(int id) {
    switch (id) {
    case H_MIN:    ShowWindow(g_hwnd, SW_MINIMIZE); return;
    case H_CLOSEX: DestroyWindow(g_hwnd); return;
    case H_BROWSE: Browse(); break;
    case H_DESK:   g_optDesk = !g_optDesk; break;
    case H_AUTO:   g_optAuto = !g_optAuto; break;
    case H_LAUNCH: g_optLaunch = !g_optLaunch; break;
    case H_CANCEL: DestroyWindow(g_hwnd); return;
    case H_INSTALL:
        if (g_state == 1) { DestroyWindow(g_hwnd); return; }
        wcscpy_s(g_status, L"Установка…"); g_statusKind = SS_INFO;
        InvalidateRect(g_hwnd, NULL, FALSE); UpdateWindow(g_hwnd);
        {
            WCHAR err[256] = L"";
            SetCursor(LoadCursorW(NULL, IDC_WAIT));
            if (DoInstall(err, 256)) { g_state = 1; g_hover = H_NONE; }
            else { wcscpy_s(g_status, err); g_statusKind = SS_ERR; }
        }
        break;
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_NCCALCSIZE: if (w) return 0; break;
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        return HitTest(p) != H_NONE ? HTCLIENT : HTCAPTION;   // drag by any empty area
    }
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps); Paint(hdc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int hv = HitTest(p);
        if (hv != g_hover) { g_hover = hv; InvalidateRect(h, NULL, FALSE); }
        if (!g_tracking) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); g_tracking = TRUE; }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        if (g_hover != H_NONE) { g_hover = H_NONE; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_NCMOUSEMOVE:
        if (g_hover != H_NONE) { g_hover = H_NONE; InvalidateRect(h, NULL, FALSE); }
        break;
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        g_press = HitTest(p);
        if (g_press != H_NONE) { SetCapture(h); InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (GetCapture() == h) ReleaseCapture();
        int id = HitTest(p), pressed = g_press;
        g_press = H_NONE;
        InvalidateRect(h, NULL, FALSE);
        if (id != H_NONE && id == pressed) OnClick(id);
        return 0;
    }
    case WM_NCLBUTTONDBLCLK: return 0;
    case WM_KEYDOWN:
        if (w == VK_RETURN) OnClick(H_INSTALL);
        else if (w == VK_ESCAPE) DestroyWindow(h);
        return 0;
    case WM_SETTINGCHANGE:
        if (l && lstrcmpiW((LPCWSTR)l, L"ImmersiveColorSet") == 0) {
            LoadTheme(); ApplyFrame(h, BACKDROP_MICA); InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        FluentFonts(); Layout(); CanvasFree(g_cv);
        RECT* r = (RECT*)l;
        SetWindowPos(h, NULL, r->left, r->top, g_w, g_h, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    BOOL silent = FALSE, uninstall = FALSE;
    KnownPath(FOLDERID_UserProgramFiles, g_dir, L"\\Cogitator");
    if (!g_dir[0]) { WCHAR la[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH); swprintf_s(g_dir, L"%s\\Programs\\Cogitator", la); }

    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        const WCHAR* a = argv[i];
        if (!_wcsicmp(a, L"/S"))                silent = TRUE;
        else if (!_wcsicmp(a, L"/uninstall"))   uninstall = TRUE;
        else if (!_wcsicmp(a, L"/nodesktop"))   g_optDesk = FALSE;
        else if (!_wcsicmp(a, L"/noautostart")) g_optAuto = FALSE;
        else if (!_wcsicmp(a, L"/nolaunch"))    g_optLaunch = FALSE;
        else if (!_wcsnicmp(a, L"/D=", 3) && a[3]) wcscpy_s(g_dir, a + 3);
    }
    if (argv) LocalFree(argv);

    if (uninstall) { DoUninstall(silent); CoUninitialize(); return 0; }
    if (silent) {
        WCHAR err[256] = L"";
        int rc = DoInstall(err, 256) ? 0 : 1;
        CoUninitialize();
        return rc;
    }

    GdiplusStartupInput gsi; ULONG_PTR gtoken;
    if (GdiplusStartup(&gtoken, &gsi, NULL) != Ok) return 1;
    FluentInit();
    LoadTheme();

    HICON big = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON sm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = big; wc.hIconSm = sm; wc.lpszClassName = L"CogitatorSetup";
    RegisterClassExW(&wc);

    wcscpy_s(g_status, L"Права администратора не требуются — установка в профиль пользователя.");
    HWND h = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Установка Cogitator",
                             WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX,
                             0, 0, BASE_W, BASE_H, NULL, NULL, hInst, NULL);
    if (!h) return 1;
    g_hwnd = h;
    g_dpi = GetDpiForWindow(h); if (!g_dpi) g_dpi = 96;
    FluentFonts();
    Layout();
    ApplyFrame(h, BACKDROP_MICA);
    MONITORINFO mi = { sizeof(mi) }; POINT o = { 0, 0 };
    GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowPos(h, NULL, (mi.rcWork.left + mi.rcWork.right - g_w) / 2, (mi.rcWork.top + mi.rcWork.bottom - g_h) / 2,
                 g_w, g_h, SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    CanvasFree(g_cv);
    GdiplusShutdown(gtoken);
    CoUninitialize();
    return 0;
}
