// Noctis 1.1 — turns the monitor off after N minutes of user inactivity (like Windows' own
// display timeout, but ignoring apps that keep the screen awake). Wakes on mouse / keyboard.
// Windows 11 (Fluent) edition. Developer: Sallador.  Pure Win32 + GDI+, static CRT.
//
// Command line:  Noctis.exe            start (or open the settings flyout if already running)
//                Noctis.exe /off       turn the screen off now
//                Noctis.exe /autostart start silently (used by the Run key)

#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>
#include "resource.h"
#include "fluent.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

#define APP_NAME    L"Noctis"
#define CLASS_NAME  L"NoctisMain"
#define REG_KEY     L"Software\\Sallador\\Noctis"
#define RUN_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define WM_TRAY     (WM_APP + 1)
#define WM_APP_SHOW (WM_APP + 2)
#define WM_APP_OFF  (WM_APP + 3)
#define TIMER_MAIN  1
#define TIMER_FAST  2
#define TIMER_REOFF 3
#define HOTKEY_ID   1
#define BASE_W 360
#define BASE_H 544
#define GRACE_MS   1500     // after a manual "off", small mouse jitter puts the screen back to sleep
#define RELEASE_MS 400      // wait this long after hotkey / click is released before switching off

enum { ID_OFF = 101, ID_ENABLED = 110, ID_SKIPFS = 111, ID_PAUSE = 112, ID_EXIT = 113 /*used by installer*/,
       ID_AUTOSTART = 114, ID_SETTINGS = 115, ID_TIME_BASE = 130, ID_TIME_CUSTOM = 140 };
static const int kPresets[6] = { 1, 3, 5, 10, 15, 30 };
enum { H_NONE = -1, H_CLOSE = 0, H_CHIP0 = 1, H_MINUS = 7, H_PLUS, H_ROW_EN, H_ROW_FS, H_ROW_AUTO, H_PAUSE, H_OFF, H_COUNT };
enum { ST_NORMAL, ST_MUTED, ST_OK };

// GUID_CONSOLE_DISPLAY_STATE
static const GUID kDisplayState = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

static HINSTANCE g_hInst;
static HWND   g_hwnd;
static int    g_w, g_h;
static Canvas g_cv;
static RECT   g_rc[H_COUNT];
static int    g_hover = H_NONE, g_press = H_NONE;
static BOOL   g_tracking;
static HICON  g_icoOn, g_icoPaused, g_icoBig;
static NOTIFYICONDATAW g_nid;
static UINT   g_msgTaskbarCreated;
static WCHAR  g_lastTip[128];

// settings
static int  g_timeoutMin = 5;
static BOOL g_enabled = TRUE;
static BOOL g_skipFs = TRUE;
static BOOL g_hotkeyOk;

// state
static ULONGLONG g_pauseUntil;          // 0 = not paused
static BOOL      g_offActive;           // we switched the screen off and the user has not touched anything since
static DWORD     g_offInput;            // last-input tick at the moment of switching off
static ULONGLONG g_graceUntil;
static BOOL      g_pendingOff;          // manual off requested, waiting for keys/buttons to be released
static ULONGLONG g_pendStart, g_releaseTick;
static DWORD     g_displayState = 1;    // 0 off, 1 on, 2 dimmed
static int       g_reoffCount;
static SYSTEMTIME g_lastOff; static BOOL g_haveLastOff;
static ULONGLONG g_hideTick;
static unsigned  g_tick;

// ───────────────────────────── settings / registry ───────────────────
static DWORD RegGetDw(const WCHAR* name, DWORD def) { return ReadDw(REG_KEY, name, def); }
static void RegSetDw(const WCHAR* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}
static BOOL IsAutostart() {
    WCHAR buf[MAX_PATH * 2]; DWORD sz = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS;
}
static void SetAutostart(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        WCHAR path[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        swprintf_s(cmd, L"\"%s\" /autostart", path);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE*)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
    } else RegDeleteValueW(k, APP_NAME);
    RegCloseKey(k);
}
static void SetTimeoutMin(int m) {
    if (m < 1) m = 1;
    if (m > 240) m = 240;
    g_timeoutMin = m; RegSetDw(L"TimeoutMin", (DWORD)m);
}

// ───────────────────────────── core logic ────────────────────────────
static DWORD LastInputTick() {
    LASTINPUTINFO lii = { sizeof(lii) };
    GetLastInputInfo(&lii);
    return lii.dwTime;
}
static DWORD IdleMs() { return GetTickCount() - LastInputTick(); }

static BOOL FullscreenBusy() {
    QUERY_USER_NOTIFICATION_STATE st;
    if (FAILED(SHQueryUserNotificationState(&st))) return FALSE;
    return st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;
}
static BOOL Paused() { return g_pauseUntil != 0; }

static void DoOff() { DefWindowProcW(g_hwnd, WM_SYSCOMMAND, SC_MONITORPOWER, 2); }

static void MonitorOff(BOOL manual) {
    g_offActive = TRUE;
    g_offInput = LastInputTick();
    g_reoffCount = 0;
    g_graceUntil = manual ? GetTickCount64() + GRACE_MS : 0;
    if (manual) SetTimer(g_hwnd, TIMER_FAST, 50, NULL);
    DoOff();
}

static void HideFlyout() {
    if (IsWindowVisible(g_hwnd)) {
        ShowWindow(g_hwnd, SW_HIDE);
        g_hideTick = GetTickCount64();
        CanvasFree(g_cv);                       // give the back buffer back while hidden
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    }
    g_hover = g_press = H_NONE;
}
static void StartManualOff() {
    HideFlyout();
    g_pendingOff = TRUE; g_pendStart = GetTickCount64(); g_releaseTick = 0;
    SetTimer(g_hwnd, TIMER_FAST, 50, NULL);
}

static int StatusText(WCHAR* s, int n) {
    if (!g_enabled) { wcscpy_s(s, n, L"Автовыключение отключено"); return ST_MUTED; }
    if (Paused()) {
        ULONGLONG left = (g_pauseUntil - GetTickCount64()) / 60000 + 1;
        swprintf_s(s, n, L"Пауза ещё %llu мин", left); return ST_MUTED;
    }
    if (g_offActive) { wcscpy_s(s, n, L"Экран выключен"); return ST_OK; }
    DWORD idle = IdleMs(), lim = (DWORD)g_timeoutMin * 60000u;
    if (idle >= lim && g_skipFs && FullscreenBusy()) { wcscpy_s(s, n, L"Ожидание: игра или видео"); return ST_MUTED; }
    DWORD left = idle >= lim ? 0 : (lim - idle + 999) / 1000;
    swprintf_s(s, n, L"Выключение через %u:%02u", left / 60, left % 60);
    return ST_NORMAL;
}

// ───────────────────────────── tray ──────────────────────────────────
// Monochrome Fluent moon that matches the taskbar theme (white on dark, black on light).
static HICON MakeTrayIcon(BOOL paused) {
    int sz = GetSystemMetrics(SM_CXSMICON);
    Bitmap bmp(sz, sz, PixelFormat32bppARGB);
    {
        Graphics g(&bmp); Prep(g);
        g.Clear(Color(0, 0, 0, 0));
        BYTE v = ReadDw(PERSONALIZE_KEY, L"SystemUsesLightTheme", 0) ? 0 : 255;
        FGlyph(g, GL_MOON, (REAL)sz, Color(paused ? 0x6A : 0xFF, v, v, v), RectF(0, 0, (REAL)sz, (REAL)sz));
    }
    HICON h = NULL;
    bmp.GetHICON(&h);
    return h;
}
static void MakeTrayIcons() {
    if (g_icoOn) DestroyIcon(g_icoOn);
    if (g_icoPaused) DestroyIcon(g_icoPaused);
    g_icoOn = MakeTrayIcon(FALSE);
    g_icoPaused = MakeTrayIcon(TRUE);
}
static void TrayAdd() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (g_enabled && !Paused()) ? g_icoOn : g_icoPaused;
    wcscpy_s(g_nid.szTip, L"Noctis");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_lastTip[0] = 0;
}
static void TrayUpdate() {
    WCHAR st[96], tip[128];
    StatusText(st, 96);
    swprintf_s(tip, L"Noctis\n%s", st);
    HICON ico = (g_enabled && !Paused()) ? g_icoOn : g_icoPaused;
    if (wcscmp(tip, g_lastTip) == 0 && ico == g_nid.hIcon) return;
    wcscpy_s(g_lastTip, tip);
    wcscpy_s(g_nid.szTip, tip);
    g_nid.hIcon = ico;
    g_nid.uFlags = NIF_TIP | NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TogglePause() {
    g_pauseUntil = Paused() ? 0 : GetTickCount64() + 3600ull * 1000ull;
    TrayUpdate(); InvalidateRect(g_hwnd, NULL, FALSE);
}

static void ShowMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OFF, g_hotkeyOk ? L"Выключить экран сейчас\tCtrl+Alt+M" : L"Выключить экран сейчас");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    HMENU tm = CreatePopupMenu();
    BOOL preset = FALSE;
    for (int i = 0; i < 6; i++) {
        WCHAR t[32]; swprintf_s(t, L"%d мин", kPresets[i]);
        if (kPresets[i] == g_timeoutMin) preset = TRUE;
        AppendMenuW(tm, MF_STRING | (kPresets[i] == g_timeoutMin ? MF_CHECKED : 0), ID_TIME_BASE + i, t);
    }
    WCHAR ct[48];
    if (preset) wcscpy_s(ct, L"Другое…"); else swprintf_s(ct, L"Другое: %d мин…", g_timeoutMin);
    AppendMenuW(tm, MF_STRING | (preset ? 0 : MF_CHECKED), ID_TIME_CUSTOM, ct);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)tm, L"Выключать экран через");
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_ENABLED, L"Автоматическое выключение");
    AppendMenuW(m, MF_STRING | (g_skipFs ? MF_CHECKED : 0), ID_SKIPFS, L"Не выключать в играх и видео");
    WCHAR pt2[64];
    if (Paused()) swprintf_s(pt2, L"Снять паузу (ещё %llu мин)", (g_pauseUntil - GetTickCount64()) / 60000 + 1);
    else wcscpy_s(pt2, L"Пауза на 1 час");
    AppendMenuW(m, MF_STRING, ID_PAUSE, pt2);
    AppendMenuW(m, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, L"Запускать вместе с Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Параметры");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Noctis 1.1 · разработчик Sallador");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Выход");
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ───────────────────────────── flyout UI ─────────────────────────────
static void SetR(int id, int x, int y, int w, int h) { g_rc[id] = { S(x), S(y), S(x + w), S(y + h) }; }
static void Layout() {
    const int W = BASE_W;
    SetR(H_CLOSE, W - 44, 10, 32, 32);
    int cw = (W - 24 - 5 * 6) / 6;
    for (int i = 0; i < 6; i++) SetR(H_CHIP0 + i, 12 + i * (cw + 6), 188, cw, 32);
    SetR(H_MINUS, W - 162, 228, 36, 32);
    SetR(H_PLUS,  W - 48,  228, 36, 32);
    for (int i = 0; i < 3; i++) SetR(H_ROW_EN + i, 12, 272 + 48 * i, W - 24, 48);
    int bw = (W - 40) / 2;
    SetR(H_PAUSE, 16, 496, bw, 32);
    SetR(H_OFF, 24 + bw, 496, bw, 32);
    g_w = S(BASE_W); g_h = S(BASE_H);
}
static int HitTest(POINT p) {
    for (int i = 0; i < H_COUNT; i++) if (PtInRect(&g_rc[i], p)) return i;
    return H_NONE;
}
static BOOL Hv(int id) { return g_hover == id && (g_press == H_NONE || g_press == id); }
static BOOL Pr(int id) { return g_press == id && g_hover == id; }

static void PaintTo(Graphics& g) {
    const int W = BASE_W;
    WCHAR buf[128];

    // ── header
    FGlyph(g, GL_MOON, D(16), T.accent, RectF(D(16), D(16), D(20), D(20)));
    FText(g, L"Noctis", g_fStrong, T.text, RectF(D(46), D(10), D(120), D(32)));
    REAL tw = FTextW(g, L"Noctis", g_fStrong);
    FText(g, L"·  Sallador", g_fCaption, T.text3, RectF(D(46) + tw + D(8), D(10), D(160), D(32)));
    RectF rc = RF(g_rc[H_CLOSE]);
    if (Hv(H_CLOSE)) FillRR(g, rc, D(4), Pr(H_CLOSE) ? T.subtlePress : T.subtleHover);
    FGlyph(g, GL_CANCEL, D(12), T.text2, rc);

    // ── status card
    FCard(g, RectF(D(12), D(50), D(W - 24), D(98)));
    int st = StatusText(buf, 128);
    FText(g, buf, g_fSubtitle, st == ST_OK ? T.success : st == ST_MUTED ? T.text2 : T.text,
          RectF(D(28), D(60), D(W - 56), D(30)), AL_LEFT, StringTrimmingEllipsisCharacter);
    DWORD idle = IdleMs() / 1000;
    swprintf_s(buf, L"Бездействие %u:%02u", idle / 60, idle % 60);
    FText(g, buf, g_fBody, T.text2, RectF(D(28), D(92), D(170), D(22)));
    if (g_haveLastOff) swprintf_s(buf, L"Выключался в %02u:%02u", g_lastOff.wHour, g_lastOff.wMinute);
    else wcscpy_s(buf, L"Ещё не выключался");
    FText(g, buf, g_fCaption, T.text3, RectF(D(150), D(92), D(W - 178), D(22)), AL_RIGHT);
    REAL frac = 0;
    if (g_offActive) frac = 1;
    else if (g_enabled && !Paused()) frac = (REAL)IdleMs() / ((REAL)g_timeoutMin * 60000.0f);
    FProgress(g, D(28), D(126), D(W - 56), frac, st == ST_MUTED ? T.text3 : st == ST_OK ? T.success : T.accent);

    // ── timeout
    FText(g, L"Выключать экран через", g_fStrong, T.text, RectF(D(16), D(156), D(W - 32), D(24)));
    for (int i = 0; i < 6; i++) {
        int id = H_CHIP0 + i;
        RectF r = RF(g_rc[id]);
        swprintf_s(buf, L"%d мин", kPresets[i]);
        if (kPresets[i] == g_timeoutMin) {
            FillRR(g, r, D(4), Pr(id) ? T.accentPress : Hv(id) ? T.accentHover : T.accent);
            FText(g, buf, g_fBody, T.onAccent, r, AL_CENTER);
        } else {
            FillRR(g, r, D(4), Pr(id) ? T.ctrlPress : Hv(id) ? T.ctrlHover : T.ctrl);
            StrokeRR(g, r, D(4), T.ctrlStroke);
            FText(g, buf, g_fBody, Pr(id) ? T.text2 : T.text, r, AL_CENTER);
        }
    }
    FText(g, L"Своё значение", g_fBody, T.text2, RectF(D(16), D(228), D(150), D(32)));
    RectF box(D(W - 162), D(228), D(150), D(32));
    FillRR(g, box, D(4), T.ctrl);
    StrokeRR(g, box, D(4), T.ctrlStroke);
    for (int k = 0; k < 2; k++) {
        int id = k ? H_PLUS : H_MINUS;
        RectF b = RF(g_rc[id]);
        if (Hv(id)) FillRR(g, RectF(b.X + D(4), b.Y + D(4), b.Width - D(8), b.Height - D(8)), D(3), Pr(id) ? T.subtlePress : T.subtleHover);
        FGlyph(g, k ? GL_ADD : GL_REMOVE, D(12), T.text2, b);
    }
    swprintf_s(buf, L"%d мин", g_timeoutMin);
    FText(g, buf, g_fBody, T.text, RectF(box.X + D(36), box.Y, box.Width - D(72), box.Height), AL_CENTER);

    // ── settings group
    FCard(g, RectF(D(12), D(272), D(W - 24), D(192)));
    static const WCHAR* titles[4] = { L"Автоматическое выключение", L"Не выключать в играх и видео",
                                      L"Запускать вместе с Windows", L"Выключить сейчас" };
    static const WCHAR glyphs[4] = { GL_POWER, GL_GAME, GL_PLAY, GL_KEYBOARD };
    BOOL vals[3] = { g_enabled, g_skipFs, IsAutostart() };
    for (int i = 0; i < 4; i++) {
        REAL y = D(272 + 48 * i);
        if (i) FFillRect(g, D(13), y, D(W - 26), 1.0f, T.divider);
        int id = H_ROW_EN + i;
        if (i < 3 && Hv(id)) FillRR(g, RectF(D(15), y + D(3), D(W - 30), D(42)), D(5), Pr(id) ? T.subtlePress : T.subtleHover);
        FGlyph(g, glyphs[i], D(16), T.text, RectF(D(28), y, D(20), D(48)));
        if (i < 3) {
            FText(g, titles[i], g_fBody, T.text, RectF(D(60), y, D(W - 60 - 80), D(48)), AL_LEFT, StringTrimmingEllipsisCharacter);
            FToggle(g, D(W - 68), y + D(14), vals[i], Hv(id));
        } else {
            FText(g, titles[i], g_fBody, T.text, RectF(D(60), y, D(130), D(48)), AL_LEFT, StringTrimmingEllipsisCharacter);
            if (g_hotkeyOk) {
                REAL x = FKeycap(g, D(W - 28), y + D(12), L"M");
                x = FKeycap(g, x - D(4), y + D(12), L"Alt");
                FKeycap(g, x - D(4), y + D(12), L"Ctrl");
            } else {
                FText(g, L"Ctrl+Alt+M занято", g_fBody, T.caution, RectF(D(W - 200), y, D(172), D(48)), AL_RIGHT);
            }
        }
    }

    // ── footer
    REAL fy = D(480);
    FFillRect(g, 0, fy, (REAL)g_w, (REAL)g_h - fy, T.footer);
    FFillRect(g, 0, fy, (REAL)g_w, 1.0f, T.divider);
    FButton(g, RF(g_rc[H_PAUSE]), Paused() ? L"Снять паузу" : L"Пауза на 1 час", FALSE,
            Hv(H_PAUSE), Pr(H_PAUSE), Paused() ? GL_PLAY : GL_PAUSE);
    FButton(g, RF(g_rc[H_OFF]), L"Выключить экран", TRUE, Hv(H_OFF), Pr(H_OFF), GL_POWER);
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

static void ShowFlyout() {
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(g_hwnd, HWND_TOPMOST, wa.right - g_w - S(12), wa.bottom - g_h - S(12), g_w, g_h, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hwnd);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void ApplyThemeAll() {
    LoadTheme();
    ApplyFrame(g_hwnd, BACKDROP_ACRYLIC);
    ApplyMenuTheme();
    MakeTrayIcons();
    g_lastTip[0] = 0;
    TrayUpdate();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void OnClick(int id) {
    if (id >= H_CHIP0 && id < H_CHIP0 + 6) SetTimeoutMin(kPresets[id - H_CHIP0]);
    else switch (id) {
    case H_CLOSE:    HideFlyout(); return;
    case H_MINUS:    SetTimeoutMin(g_timeoutMin > 30 ? g_timeoutMin - 5 : g_timeoutMin - 1); break;
    case H_PLUS:     SetTimeoutMin(g_timeoutMin >= 30 ? g_timeoutMin + 5 : g_timeoutMin + 1); break;
    case H_ROW_EN:   g_enabled = !g_enabled; RegSetDw(L"Enabled", g_enabled); break;
    case H_ROW_FS:   g_skipFs = !g_skipFs; RegSetDw(L"SkipFullscreen", g_skipFs); break;
    case H_ROW_AUTO: SetAutostart(!IsAutostart()); break;
    case H_PAUSE:    TogglePause(); break;
    case H_OFF:      StartManualOff(); return;
    }
    TrayUpdate();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ───────────────────────────── window proc ───────────────────────────
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) { TrayAdd(); return 0; }
    switch (msg) {
    case WM_NCCALCSIZE: if (w) return 0; break;            // whole window is client area
    case WM_NCHITTEST:  return HTCLIENT;
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps); Paint(hdc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;

    case WM_TIMER:
        if (w == TIMER_MAIN) {
            ULONGLONG now = GetTickCount64();
            DWORD li = LastInputTick();
            g_tick++;
            if (g_offActive && li != g_offInput && now >= g_graceUntil) g_offActive = FALSE;   // user is back
            if (g_pauseUntil && now >= g_pauseUntil) g_pauseUntil = 0;
            if (g_enabled && !Paused() && !g_offActive && !g_pendingOff &&
                GetTickCount() - li >= (DWORD)g_timeoutMin * 60000u &&
                !(g_skipFs && FullscreenBusy()))
                MonitorOff(FALSE);
            TrayUpdate();
            if (IsWindowVisible(h)) InvalidateRect(h, NULL, FALSE);
            if (g_tick == 3 || g_tick % 60 == 0) SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        } else if (w == TIMER_FAST) {
            ULONGLONG now = GetTickCount64();
            if (g_pendingOff) {
                BOOL down = ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) | GetAsyncKeyState('M') |
                              GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RETURN)) & 0x8000) != 0;
                if (down) g_releaseTick = 0; else if (!g_releaseTick) g_releaseTick = now;
                if ((g_releaseTick && now - g_releaseTick >= RELEASE_MS) || now - g_pendStart > 3000) {
                    g_pendingOff = FALSE;
                    MonitorOff(TRUE);
                }
            } else if (g_offActive && now < g_graceUntil) {
                DWORD li = LastInputTick();
                if (li != g_offInput) { g_offInput = li; DoOff(); }   // jitter right after a manual off
            } else KillTimer(h, TIMER_FAST);
        } else if (w == TIMER_REOFF) {
            KillTimer(h, TIMER_REOFF);
            // something (a video tab, a driver) woke the screen without the user touching anything
            if (g_offActive && LastInputTick() == g_offInput && g_displayState != 0) { g_reoffCount++; DoOff(); }
        }
        return 0;

    case WM_POWERBROADCAST:
        if (w == PBT_POWERSETTINGCHANGE) {
            POWERBROADCAST_SETTING* ps = (POWERBROADCAST_SETTING*)l;
            if (IsEqualGUID(ps->PowerSetting, kDisplayState) && ps->DataLength >= sizeof(DWORD)) {
                DWORD st = *(DWORD*)ps->Data;
                if (st == 0 && g_displayState != 0) { GetLocalTime(&g_lastOff); g_haveLastOff = TRUE; }
                g_displayState = st;
                if (st == 1 && g_offActive && LastInputTick() == g_offInput && g_reoffCount < 10)
                    SetTimer(h, TIMER_REOFF, 500, NULL);
            }
        }
        return TRUE;

    case WM_HOTKEY:
        if (w == HOTKEY_ID) StartManualOff();
        return 0;
    case WM_APP_OFF:  StartManualOff(); return 0;
    case WM_APP_SHOW: ShowFlyout(); return 0;

    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP) {
            if (IsWindowVisible(h)) HideFlyout();
            else if (GetTickCount64() - g_hideTick > 300) ShowFlyout();
        } else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
            POINT p; GetCursorPos(&p); ShowMenu(p);
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) HideFlyout();
        return 0;
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
        if (id != H_NONE && id == pressed) OnClick(id);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN: if (w == VK_ESCAPE) HideFlyout(); return 0;

    case WM_SETTINGCHANGE:
        if (l && lstrcmpiW((LPCWSTR)l, L"ImmersiveColorSet") == 0) ApplyThemeAll();
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ApplyThemeAll();
        break;

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id >= ID_TIME_BASE && id < ID_TIME_BASE + 6) SetTimeoutMin(kPresets[id - ID_TIME_BASE]);
        else if (id == ID_TIME_CUSTOM || id == ID_SETTINGS) ShowFlyout();
        else if (id == ID_OFF)       StartManualOff();
        else if (id == ID_ENABLED)   { g_enabled = !g_enabled; RegSetDw(L"Enabled", g_enabled); }
        else if (id == ID_SKIPFS)    { g_skipFs = !g_skipFs; RegSetDw(L"SkipFullscreen", g_skipFs); }
        else if (id == ID_PAUSE)     TogglePause();
        else if (id == ID_AUTOSTART) SetAutostart(!IsAutostart());
        else if (id == ID_EXIT)      { DestroyWindow(h); return 0; }
        TrayUpdate();
        if (IsWindowVisible(h)) InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        FluentFonts(); Layout(); CanvasFree(g_cv);
        RECT* r = (RECT*)l;
        SetWindowPos(h, NULL, r->left, r->top, g_w, g_h, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE: HideFlyout(); return 0;
    case WM_DESTROY:
        UnregisterHotKey(h, HOTKEY_ID);
        g_nid.uFlags = 0; Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;
    BOOL argOff = FALSE, argAuto = FALSE;
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/off")) argOff = TRUE;
        else if (!_wcsicmp(argv[i], L"/autostart")) argAuto = TRUE;
    }
    if (argv) LocalFree(argv);

    CreateMutexW(NULL, TRUE, L"Local\\Noctis_Sallador_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(CLASS_NAME, NULL);
        if (w) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(w, argOff ? WM_APP_OFF : WM_APP_SHOW, 0, 0); }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    GdiplusStartupInput gsi; ULONG_PTR gtoken;
    if (GdiplusStartup(&gtoken, &gsi, NULL) != Ok) return 1;
    FluentInit();
    LoadTheme();
    ApplyMenuTheme();

    BOOL firstRun = RegGetDw(L"TimeoutMin", 0) == 0;
    g_timeoutMin = (int)RegGetDw(L"TimeoutMin", 5);
    if (g_timeoutMin < 1 || g_timeoutMin > 240) g_timeoutMin = 5;
    g_enabled = RegGetDw(L"Enabled", 1) != 0;
    g_skipFs  = RegGetDw(L"SkipFullscreen", 1) != 0;
    if (firstRun) RegSetDw(L"TimeoutMin", (DWORD)g_timeoutMin);
    if (IsAutostart()) SetAutostart(TRUE);   // refresh path in case the exe was moved

    MakeTrayIcons();
    g_icoBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON icoSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = g_icoBig; wc.hIconSm = icoSm; wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, CLASS_NAME, L"Noctis", WS_POPUP | WS_THICKFRAME,
                             0, 0, BASE_W, BASE_H, NULL, NULL, hInst, NULL);
    if (!h) return 1;
    g_hwnd = h;
    g_dpi = GetDpiForWindow(h); if (!g_dpi) g_dpi = 96;
    FluentFonts();
    Layout();
    ApplyFrame(h, BACKDROP_ACRYLIC);
    SetWindowPos(h, NULL, 0, 0, g_w, g_h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_FRAMECHANGED);

    RegisterPowerSettingNotification(h, &kDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    g_hotkeyOk = RegisterHotKey(h, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M');
    TrayAdd();
    TrayUpdate();
    SetTimer(h, TIMER_MAIN, 1000, NULL);

    if (argOff) StartManualOff();
    else if (!argAuto) ShowFlyout();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    GdiplusShutdown(gtoken);
    return 0;
}
