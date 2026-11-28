// Velox 1.1 — desktop widget: which applications use the network, their speed and downloaded volume.
// Windows 11 (Fluent) edition. Developer: Sallador.  Pure Win32 + GDI+ + ETW, static CRT.
//
// Per-process traffic comes from a real-time ETW session on the Microsoft-Windows-Kernel-Network
// provider (needs administrator rights). Total speed comes from the network adapters (GetIfTable2)
// and works without them.
//
//   Velox.exe              start (runs the elevated "\Sallador\Velox" task if it exists, else asks UAC)
//   Velox.exe /autostart   started by the logon task
//   Velox.exe /demo        sample data, no ETW (for screenshots)

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
#include <tlhelp32.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <evntrace.h>
#include <evntcons.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "resource.h"
#include "fluent.h"
#include "lang.h"
#include "tasks.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "version.lib")

// ───────────────────────────── constants ─────────────────────────────
#define APP_NAME     L"Velox"
#define CLASS_NAME   L"VeloxWidget"
#define REG_KEY      L"Software\\Sallador\\Velox"
#define SESSION_NAME L"Sallador Velox Network"
#define WM_TRAY          (WM_APP + 1)
#define WM_APP_SHOW      (WM_APP + 2)
#define WM_APP_RECREATE  (WM_APP + 3)
#define TIMER_ID     1
#define MAX_ROWS     10
#define MAX_GROUPS   512
#define ACC_SIZE     4096
#define PIDMAP_SIZE  8192
#define HIST         60
#define POS_UNSET    0x80000000u

#define BASE_W   400
#define BASE_H   462
#define HDR_H    44
#define TILE_Y   46
#define TILE_H   84
#define TH_Y     138
#define TH_H     28
#define ROW_Y    170
#define ROW_H    26
#define FOOT_Y   434

enum { SORT_DOWN = 0, SORT_UP = 1, SORT_TOTAL = 2 };
enum {
    ID_SORT_DOWN = 101, ID_SORT_UP, ID_SORT_TOTAL,
    ID_TOPMOST = 110, ID_AUTOSTART = 111, ID_SHOWHIDE = 112, ID_EXIT = 113 /*used by installer*/,
    ID_ACRYLIC = 115, ID_BITS = 120, ID_LAN, ID_RESET
};
enum { H_NONE = -1, H_MORE = 0, H_COL0, H_COL1, H_COL2, H_ELEVATE, H_COUNT };

static const GUID KERNEL_NETWORK = { 0x7dd42a49, 0x5329, 0x4832, { 0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88 } };

struct Acc   { ULONG key; ULONGLONG in, out; };            // key = pid + 1 (0 = empty)
struct Group {
    WCHAR exe[48], title[48]; UINT32 hash;
    Bitmap* icon; BOOL iconTried; WCHAR path[MAX_PATH];
    ULONGLONG totIn, totOut, tickIn, tickOut;
    double instIn, instOut, spdIn, spdOut;
};
struct PidEnt { ULONG key; int group; };

// ───────────────────────────── globals ───────────────────────────────
static HINSTANCE g_hInst;
static HWND      g_hwnd;
static HANDLE    g_mutex;
static int       g_w, g_h;
static Canvas    g_cv;
static RECT      g_rc[H_COUNT];
static int       g_hover = H_NONE, g_press = H_NONE;
static BOOL      g_tracking;
static BOOL      g_drag; static POINT g_dragCursor; static RECT g_dragWin;
static HICON     g_iconSm, g_iconBig;
static NOTIFYICONDATAW g_nid;
static UINT      g_msgTaskbarCreated;

static int  g_sort = SORT_DOWN;
static BOOL g_topmost, g_acrylic = TRUE, g_visible = TRUE, g_exiting, g_bits, g_demo, g_elevated;
static volatile BOOL g_countLan;
static unsigned g_tick;

// ETW
static TRACEHANDLE g_session;
static TRACEHANDLE g_trace = INVALID_PROCESSTRACE_HANDLE;
static BOOL        g_etwOk;
static CRITICAL_SECTION g_cs;
static Acc*        g_acc;      static Acc* g_accCopy;

// processes
static Group*  g_groups; static int g_groupN;
static PidEnt* g_pidMap;
static Group*  g_top[MAX_ROWS]; static int g_topN, g_active;
static ULONGLONG g_lastTick;

// adapters
static ULONGLONG g_ifIn, g_ifOut; static BOOL g_ifHave;
static double    g_spdIn, g_spdOut;
static ULONGLONG g_sesIn, g_sesOut;
static float     g_hIn[HIST], g_hOut[HIST];
static SYSTEMTIME g_since;

// ───────────────────────────── helpers ───────────────────────────────
static void* Alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static UINT32 HashName(const WCHAR* s) {
    UINT32 h = 2166136261u;
    for (; *s; s++) { WCHAR c = *s; if (c >= L'A' && c <= L'Z') c += 32; else if (c > 127) c = towlower(c); h ^= c; h *= 16777619u; }
    return h;
}
static DWORD RegGetDw(const WCHAR* name, DWORD def) { return ReadDw(REG_KEY, name, def); }
static void RegSetDw(const WCHAR* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}
static BOOL IsElevated() {
    HANDLE t; TOKEN_ELEVATION e = {}; DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return FALSE;
    GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n);
    CloseHandle(t);
    return e.TokenIsElevated != 0;
}
static void SelfPath(WCHAR* p) { GetModuleFileNameW(NULL, p, MAX_PATH); }

// ───────────────────────────── ETW (worker thread) ───────────────────
static BOOL Loop4(const BYTE* a) { return a[0] == 127; }
static BOOL Local4(const BYTE* a) {
    return a[0] == 127 || a[0] == 10 || a[0] == 0 || (a[0] == 172 && (a[1] & 0xF0) == 16) ||
           (a[0] == 192 && a[1] == 168) || (a[0] == 169 && a[1] == 254) || a[0] >= 224;
}
static BOOL Loop6(const BYTE* a) {
    for (int i = 0; i < 15; i++) if (a[i]) return FALSE;
    return a[15] == 1;
}
static BOOL Mapped4(const BYTE* a) {
    for (int i = 0; i < 10; i++) if (a[i]) return FALSE;
    return a[10] == 0xFF && a[11] == 0xFF;
}
static BOOL Local6(const BYTE* a) {
    if (Loop6(a)) return TRUE;
    if (Mapped4(a)) return Local4(a + 12);
    return (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) || (a[0] & 0xFE) == 0xFC || a[0] == 0xFF;
}
static void Accum(ULONG pid, ULONG size, BOOL recv) {
    ULONG key = pid + 1, i = (key * 2654435761u) & (ACC_SIZE - 1);
    EnterCriticalSection(&g_cs);
    for (int probe = 0; probe < 64; probe++, i = (i + 1) & (ACC_SIZE - 1)) {
        Acc* a = &g_acc[i];
        if (a->key == key || a->key == 0) {
            a->key = key;
            if (recv) a->in += size; else a->out += size;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
}
static void WINAPI OnEvent(PEVENT_RECORD e) {
    if (!IsEqualGUID(e->EventHeader.ProviderId, KERNEL_NETWORK)) return;
    BOOL recv, v6;
    switch (e->EventHeader.EventDescriptor.Id) {
    case 10: case 42: recv = FALSE; v6 = FALSE; break;   // TCP / UDP send, IPv4
    case 11: case 43: recv = TRUE;  v6 = FALSE; break;   // TCP / UDP receive, IPv4
    case 26: case 58: recv = FALSE; v6 = TRUE;  break;   // TCP / UDP send, IPv6
    case 27: case 59: recv = TRUE;  v6 = TRUE;  break;   // TCP / UDP receive, IPv6
    default: return;
    }
    const BYTE* d = (const BYTE*)e->UserData;
    if (!d || e->UserDataLength < (v6 ? 40u : 16u)) return;
    ULONG pid = *(const ULONG*)d, size = *(const ULONG*)(d + 4);
    const BYTE *da = d + 8, *sa = d + (v6 ? 24 : 12);
    if (v6) {
        if (Loop6(da) && Loop6(sa)) return;
        if (!g_countLan && Local6(da) && Local6(sa)) return;
    } else {
        if (Loop4(da) && Loop4(sa)) return;
        if (!g_countLan && Local4(da) && Local4(sa)) return;
    }
    Accum(pid, size, recv);
}
static DWORD WINAPI EtwThread(LPVOID) { ProcessTrace(&g_trace, 1, NULL, NULL); return 0; }

struct PropsBuf { EVENT_TRACE_PROPERTIES p; WCHAR name[128]; };
static void InitProps(PropsBuf& b) {
    ZeroMemory(&b, sizeof(b));
    b.p.Wnode.BufferSize = sizeof(b);
    b.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    b.p.Wnode.ClientContext = 1;                       // QPC timestamps
    b.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    b.p.LoggerNameOffset = offsetof(PropsBuf, name);
    b.p.BufferSize = 64;                               // KB
    b.p.MinimumBuffers = 4;
    b.p.MaximumBuffers = 64;
    b.p.FlushTimer = 1;                                // seconds
}
static void StopEtw() {
    PropsBuf b; InitProps(b);
    ControlTraceW(0, SESSION_NAME, &b.p, EVENT_TRACE_CONTROL_STOP);
    if (g_trace != INVALID_PROCESSTRACE_HANDLE) { CloseTrace(g_trace); g_trace = INVALID_PROCESSTRACE_HANDLE; }
    g_session = 0;
}
static BOOL StartEtw() {
    StopEtw();                                         // a session left over from a crash
    PropsBuf b; InitProps(b);
    if (StartTraceW(&g_session, SESSION_NAME, &b.p) != ERROR_SUCCESS) return FALSE;
    if (EnableTraceEx2(g_session, &KERNEL_NETWORK, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                       0, 0, 0, NULL) != ERROR_SUCCESS) { StopEtw(); return FALSE; }
    EVENT_TRACE_LOGFILEW lf = {};
    lf.LoggerName = (LPWSTR)SESSION_NAME;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = OnEvent;
    g_trace = OpenTraceW(&lf);
    if (g_trace == INVALID_PROCESSTRACE_HANDLE) { StopEtw(); return FALSE; }
    HANDLE t = CreateThread(NULL, 0, EtwThread, NULL, 0, NULL);
    if (!t) { StopEtw(); return FALSE; }
    CloseHandle(t);
    return TRUE;
}

// ───────────────────────────── process groups ────────────────────────
static Bitmap* IconToBitmap(HICON hi) {
    ICONINFO ii; if (!GetIconInfo(hi, &ii)) return NULL;
    Bitmap* out = NULL;
    BITMAP bm;
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm)) {
        int w = bm.bmWidth, h = bm.bmHeight;
        DWORD* px = (DWORD*)Alloc((SIZE_T)w * h * 4);
        DWORD* mk = (DWORD*)Alloc((SIZE_T)w * h * 4);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(NULL);
        if (px && mk && GetDIBits(dc, ii.hbmColor, 0, h, px, &bi, DIB_RGB_COLORS)) {
            BOOL alpha = FALSE;
            for (int i = 0; i < w * h; i++) if (px[i] >> 24) { alpha = TRUE; break; }
            if (!alpha && ii.hbmMask && GetDIBits(dc, ii.hbmMask, 0, h, mk, &bi, DIB_RGB_COLORS))
                for (int i = 0; i < w * h; i++) px[i] = (px[i] & 0xFFFFFF) | ((mk[i] & 0xFFFFFF) ? 0 : 0xFF000000u);
            out = new Bitmap(w, h, w * 4, PixelFormat32bppARGB, (BYTE*)px);
            Bitmap* copy = out->Clone(0, 0, w, h, PixelFormat32bppPARGB);   // own the pixels
            delete out; out = copy;
        }
        ReleaseDC(NULL, dc);
        HeapFree(GetProcessHeap(), 0, px); HeapFree(GetProcessHeap(), 0, mk);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return out;
}
static void LoadGroupIcon(Group* g) {
    g->iconTried = TRUE;
    if (!g->path[0]) return;
    int sz = S(16);
    HICON hsm = NULL;
    if (SHDefExtractIconW(g->path, 0, 0, NULL, &hsm, MAKELONG(sz, sz)) != S_OK || !hsm) {
        SHFILEINFOW fi = {};
        if (SHGetFileInfoW(g->path, 0, &fi, sizeof(fi), SHGFI_ICON | SHGFI_SMALLICON)) hsm = fi.hIcon;
    }
    if (hsm) { g->icon = IconToBitmap(hsm); DestroyIcon(hsm); }
}
static void FileDescription(const WCHAR* path, WCHAR* out, int n) {
    out[0] = 0;
    DWORD h = 0, sz = GetFileVersionInfoSizeW(path, &h);
    if (!sz || sz > 1 << 20) return;
    void* buf = Alloc(sz);
    if (buf && GetFileVersionInfoW(path, 0, sz, buf)) {
        struct LC { WORD lang, cp; } *tr = NULL; UINT tl = 0;
        if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void**)&tr, &tl) && tl >= sizeof(LC)) {
            WCHAR q[64]; WCHAR* v = NULL; UINT vl = 0;
            swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
            if (VerQueryValueW(buf, q, (void**)&v, &vl) && v && vl > 1) {
                wcsncpy_s(out, n, v, _TRUNCATE);
                for (int i = (int)wcslen(out) - 1; i >= 0 && out[i] == L' '; i--) out[i] = 0;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
}
static int FindOrAddGroup(const WCHAR* exe, ULONG pid) {
    UINT32 h = HashName(exe);
    for (int i = 0; i < g_groupN; i++) if (g_groups[i].hash == h && !_wcsicmp(g_groups[i].exe, exe)) return i;
    if (g_groupN >= MAX_GROUPS) return -1;
    Group* g = &g_groups[g_groupN];
    ZeroMemory(g, sizeof(*g));
    wcscpy_s(g->exe, exe); g->hash = h;
    if (pid > 4) {
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (ph) { DWORD n = MAX_PATH; if (!QueryFullProcessImageNameW(ph, 0, g->path, &n)) g->path[0] = 0; CloseHandle(ph); }
    }
    if (g->path[0]) FileDescription(g->path, g->title, 48);
    if (!g->title[0] || wcslen(g->title) > 40) wcscpy_s(g->title, g->exe);
    return g_groupN++;
}
// pid -> group, resolving new pids with one process snapshot per tick
static HANDLE g_snap;
static int ResolvePid(ULONG pid) {
    ULONG key = pid + 1, i = (key * 2654435761u) & (PIDMAP_SIZE - 1);
    for (int probe = 0; probe < 64; probe++, i = (i + 1) & (PIDMAP_SIZE - 1)) {
        if (g_pidMap[i].key == key) return g_pidMap[i].group;
        if (g_pidMap[i].key == 0) break;
    }
    WCHAR name[48] = L"";
    if (pid == 0 || pid == 4) wcscpy_s(name, TR(L"Система", L"System"));
    else {
        if (!g_snap) g_snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (g_snap != INVALID_HANDLE_VALUE && Process32FirstW(g_snap, &pe))
            do {
                if (pe.th32ProcessID == pid) {
                    wcsncpy_s(name, pe.szExeFile, _TRUNCATE);
                    size_t n = wcslen(name);
                    if (n > 4 && !_wcsicmp(name + n - 4, L".exe")) name[n - 4] = 0;
                    break;
                }
            } while (Process32NextW(g_snap, &pe));
        if (!name[0]) wcscpy_s(name, TR(L"Завершённые процессы", L"Exited processes"));
    }
    int gi = FindOrAddGroup(name, pid);
    if (pid == 0 || pid == 4) { if (gi >= 0) wcscpy_s(g_groups[gi].title, TR(L"Система", L"System")); }
    i = (key * 2654435761u) & (PIDMAP_SIZE - 1);
    for (int probe = 0; probe < 64; probe++, i = (i + 1) & (PIDMAP_SIZE - 1))
        if (g_pidMap[i].key == 0) { g_pidMap[i].key = key; g_pidMap[i].group = gi; break; }
    return gi;
}

// ───────────────────────────── sampling ──────────────────────────────
static void SampleAdapters(double dt) {
    PMIB_IF_TABLE2 t = NULL;
    if (GetIfTable2(&t) != NO_ERROR || !t) return;
    ULONGLONG in = 0, out = 0;
    for (ULONG i = 0; i < t->NumEntries; i++) {
        MIB_IF_ROW2* r = &t->Table[i];
        if (r->Type == IF_TYPE_SOFTWARE_LOOPBACK || r->OperStatus != IfOperStatusUp) continue;
        if (!r->InterfaceAndOperStatusFlags.HardwareInterface || r->InterfaceAndOperStatusFlags.FilterInterface) continue;
        in += r->InOctets; out += r->OutOctets;
    }
    FreeMibTable(t);
    if (g_ifHave && in >= g_ifIn && out >= g_ifOut && dt > 0) {
        ULONGLONG di = in - g_ifIn, dout = out - g_ifOut;
        g_spdIn = di / dt; g_spdOut = dout / dt;
        g_sesIn += di; g_sesOut += dout;
    }
    g_ifIn = in; g_ifOut = out; g_ifHave = TRUE;
    memmove(g_hIn, g_hIn + 1, sizeof(float) * (HIST - 1)); g_hIn[HIST - 1] = (float)g_spdIn;
    memmove(g_hOut, g_hOut + 1, sizeof(float) * (HIST - 1)); g_hOut[HIST - 1] = (float)g_spdOut;
}
static double Metric(const Group* g) {
    return g_sort == SORT_DOWN ? g->spdIn : g_sort == SORT_UP ? g->spdOut : (double)g->totIn;
}
static void BuildTop() {
    g_topN = 0; g_active = 0;
    Group* cand[MAX_GROUPS]; int n = 0;
    for (int i = 0; i < g_groupN; i++) {
        Group* g = &g_groups[i];
        if (g->spdIn >= 1 || g->spdOut >= 1) g_active++;
        if (g->totIn + g->totOut >= 1024) cand[n++] = g;
    }
    for (int k = 0; k < MAX_ROWS && k < n; k++) {                // partial selection sort
        int best = k;
        for (int i = k + 1; i < n; i++) {
            double a = Metric(cand[i]), b = Metric(cand[best]);
            if (a > b || (a == b && cand[i]->totIn + cand[i]->totOut > cand[best]->totIn + cand[best]->totOut)) best = i;
        }
        Group* t = cand[k]; cand[k] = cand[best]; cand[best] = t;
        g_top[g_topN++] = cand[k];
    }
}
static void Sample() {
    ULONGLONG now = GetTickCount64();
    double dt = g_lastTick ? (now - g_lastTick) / 1000.0 : 1.0;
    if (dt <= 0.05) dt = 1.0;
    g_lastTick = now;
    if (!g_demo) SampleAdapters(dt);

    if (g_etwOk) {
        EnterCriticalSection(&g_cs);
        memcpy(g_accCopy, g_acc, sizeof(Acc) * ACC_SIZE);
        ZeroMemory(g_acc, sizeof(Acc) * ACC_SIZE);
        LeaveCriticalSection(&g_cs);
        if (g_tick % 10 == 0) ZeroMemory(g_pidMap, sizeof(PidEnt) * PIDMAP_SIZE);   // pids get reused
        g_snap = NULL;
        for (int i = 0; i < ACC_SIZE; i++) {
            if (!g_accCopy[i].key) continue;
            int gi = ResolvePid(g_accCopy[i].key - 1);
            if (gi < 0) continue;
            g_groups[gi].tickIn += g_accCopy[i].in; g_groups[gi].tickOut += g_accCopy[i].out;
        }
        if (g_snap && g_snap != INVALID_HANDLE_VALUE) CloseHandle(g_snap);
        g_snap = NULL;
        for (int i = 0; i < g_groupN; i++) {
            Group* g = &g_groups[i];
            double ii = g->tickIn / dt, io = g->tickOut / dt;
            g->spdIn = (ii + g->instIn) / 2; g->spdOut = (io + g->instOut) / 2;   // ETW buffers flush ~1 s, smooth it
            g->instIn = ii; g->instOut = io;
            g->totIn += g->tickIn; g->totOut += g->tickOut;
            g->tickIn = g->tickOut = 0;
        }
    }
    BuildTop();
}
static void ResetCounters() {
    for (int i = 0; i < g_groupN; i++) { g_groups[i].totIn = g_groups[i].totOut = 0; }
    g_sesIn = g_sesOut = 0;
    GetLocalTime(&g_since);
    BuildTop();
}
static void DemoData() {
    static const WCHAR* t[7] = { L"qBittorrent", L"Google Chrome", L"Steam", L"Discord", L"Microsoft Edge", L"OneDrive", TR(L"Система", L"System") };
    static const WCHAR* e[7] = { L"qbittorrent", L"chrome", L"steam", L"Discord", L"msedge", L"OneDrive", L"System" };
    static const double di[7] = { 9.4e6, 1.25e6, 3.1e6, 52e3, 18e3, 0, 2e3 };
    static const double uo[7] = { 1.6e6, 96e3, 21e3, 38e3, 4e3, 410e3, 1e3 };
    static const double tot[7] = { 4.82e9, 812e6, 1.96e9, 74e6, 38e6, 12e6, 3e6 };
    g_groupN = 7;
    WCHAR sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    for (int i = 0; i < 7; i++) {
        Group* g = &g_groups[i]; ZeroMemory(g, sizeof(*g));
        wcscpy_s(g->exe, e[i]); wcscpy_s(g->title, t[i]); g->hash = HashName(e[i]);
        g->spdIn = di[i]; g->spdOut = uo[i]; g->totIn = (ULONGLONG)tot[i]; g->totOut = (ULONGLONG)(tot[i] / 9);
        g->iconTried = TRUE;
    }
    g_spdIn = 14.0e6; g_spdOut = 2.2e6; g_sesIn = 7900000000ULL; g_sesOut = 905000000ULL;
    for (int i = 0; i < HIST; i++) {
        g_hIn[i] = (float)(9e6 + 5e6 * sinf(i * 0.21f) + 2e6 * sinf(i * 0.77f) + (i > 40 ? 3e6 : 0));
        g_hOut[i] = (float)(1.6e6 + 0.7e6 * sinf(i * 0.33f + 1) + 0.4e6 * sinf(i * 1.1f));
    }
    g_etwOk = TRUE;
    BuildTop();
}

// ───────────────────────────── formatting ────────────────────────────
static void Comma(WCHAR* s) { if (g_en) return; for (; *s; s++) if (*s == L'.') *s = L','; }
static void FmtSpeed(WCHAR* b, int n, double bps, BOOL dashZero) {
    if (dashZero && bps < 1) { wcscpy_s(b, n, L"—"); return; }
    if (g_bits) {
        double v = bps * 8;
        if (v < 1e6)       swprintf_s(b, n, TR(L"%.0f Кбит/с", L"%.0f Kbps"), v / 1e3);
        else if (v < 1e8)  swprintf_s(b, n, TR(L"%.1f Мбит/с", L"%.1f Mbps"), v / 1e6);
        else if (v < 1e9)  swprintf_s(b, n, TR(L"%.0f Мбит/с", L"%.0f Mbps"), v / 1e6);
        else               swprintf_s(b, n, TR(L"%.2f Гбит/с", L"%.2f Gbps"), v / 1e9);
    } else {
        if (bps < 1000)                 swprintf_s(b, n, TR(L"%.0f Б/с", L"%.0f B/s"), bps);
        else if (bps < 1000.0 * 1024)   swprintf_s(b, n, TR(L"%.0f КБ/с", L"%.0f KB/s"), bps / 1024);
        else if (bps < 100.0 * 1048576) swprintf_s(b, n, TR(L"%.1f МБ/с", L"%.1f MB/s"), bps / 1048576);
        else if (bps < 1000.0 * 1048576) swprintf_s(b, n, TR(L"%.0f МБ/с", L"%.0f MB/s"), bps / 1048576);
        else                            swprintf_s(b, n, TR(L"%.2f ГБ/с", L"%.2f GB/s"), bps / 1073741824.0);
    }
    Comma(b);
}
static void FmtSize(WCHAR* b, int n, ULONGLONG v) {
    double d = (double)v;
    if (d < 1000)                    swprintf_s(b, n, TR(L"%.0f Б", L"%.0f B"), d);
    else if (d < 1000.0 * 1024)      swprintf_s(b, n, TR(L"%.0f КБ", L"%.0f KB"), d / 1024);
    else if (d < 100.0 * 1048576)    swprintf_s(b, n, TR(L"%.1f МБ", L"%.1f MB"), d / 1048576);
    else if (d < 1000.0 * 1048576)   swprintf_s(b, n, TR(L"%.0f МБ", L"%.0f MB"), d / 1048576);
    else                             swprintf_s(b, n, TR(L"%.2f ГБ", L"%.2f GB"), d / 1073741824.0);
    Comma(b);
}

// ───────────────────────────── rendering ─────────────────────────────
static Font* g_fValue;
static void ExtraFonts() { delete g_fValue; g_fValue = new Font(g_ffDisp, D(18), FontStyleRegular, UnitPixel); }

static void ColDip(int col, int* x, int* w) {
    if (col == SORT_DOWN)    { *x = 172; *w = 74; }
    else if (col == SORT_UP) { *x = 250; *w = 70; }
    else                     { *x = 324; *w = 60; }
}
static void SetR(int id, int x, int y, int w, int h) { g_rc[id] = { S(x), S(y), S(x + w), S(y + h) }; }
static void Layout() {
    SetR(H_MORE, BASE_W - 42, 8, 30, 30);
    for (int c = 0; c < 3; c++) { int x, w; ColDip(c, &x, &w); SetR(H_COL0 + c, x - 4, TH_Y, w + 8, TH_H); }
    SetR(H_ELEVATE, (BASE_W - 236) / 2, ROW_Y + 112, 236, 32);
    g_w = S(BASE_W); g_h = S(BASE_H);
}
static BOOL Shown(int id) { return id != H_ELEVATE || !g_etwOk; }
static int HitTest(POINT p) {
    for (int i = 0; i < H_COUNT; i++) if (Shown(i) && PtInRect(&g_rc[i], p)) return i;
    return H_NONE;
}
static BOOL Hv(int id) { return g_hover == id && (g_press == H_NONE || g_press == id); }
static BOOL Pr(int id) { return g_press == id && g_hover == id; }

static void Spark(Graphics& g, const RectF& r, const float* h, const Color& c) {
    float mx = 1024;
    for (int i = 0; i < HIST; i++) if (h[i] > mx) mx = h[i];
    PointF pts[HIST + 2];
    for (int i = 0; i < HIST; i++)
        pts[i] = PointF(r.X + r.Width * i / (HIST - 1), r.Y + r.Height - r.Height * (h[i] / mx));
    pts[HIST] = PointF(r.X + r.Width, r.Y + r.Height); pts[HIST + 1] = PointF(r.X, r.Y + r.Height);
    SolidBrush fill(Color(T.dark ? 0x38 : 0x2C, c.GetR(), c.GetG(), c.GetB()));
    g.FillPolygon(&fill, pts, HIST + 2);
    Pen pen(c, D(1.5)); pen.SetLineJoin(LineJoinRound);
    g.DrawLines(&pen, pts, HIST);
}

static void PaintTo(Graphics& g) {
    const int W = BASE_W;
    WCHAR buf[160];

    // ── header
    DrawAppMark(g, D(14), D(14), D(16));
    FText(g, L"Velox", g_fStrong, T.text, RectF(D(38), 0, D(120), D(HDR_H)));
    REAL tw = FTextW(g, L"Velox", g_fStrong);
    FText(g, L"·  Sallador", g_fCaption, T.text3, RectF(D(38) + tw + D(8), 0, D(120), D(HDR_H)));
    FText(g, g_etwOk ? (g_countLan ? TR(L"Интернет и локальная сеть", L"Internet and local network") : TR(L"Интернет", L"Internet")) : TR(L"Ограниченный режим", L"Limited mode"),
          g_fCaption, g_etwOk ? T.text3 : T.caution, RectF(D(W - 230), 0, D(184), D(HDR_H)), AL_RIGHT);
    RectF mr = RF(g_rc[H_MORE]);
    if (Hv(H_MORE)) FillRR(g, mr, D(4), Pr(H_MORE) ? T.subtlePress : T.subtleHover);
    FGlyph(g, GL_MORE, D(14), T.text2, mr);

    // ── tiles: download, upload, session volume
    int tileW = (W - 24 - 16) / 3;
    Color upColor = T.dark ? Color(255, 0xA7, 0x8B, 0xFA) : Color(255, 0x7C, 0x4D, 0xD8);
    for (int i = 0; i < 3; i++) {
        int x = 12 + i * (tileW + 8);
        RectF tr(D(x), D(TILE_Y), D(tileW), D(TILE_H));
        FCard(g, tr);
        if (i < 2) {
            BOOL dn = i == 0;
            FGlyph(g, dn ? GL_DOWN : GL_UP, D(11), dn ? T.accent : upColor, RectF(D(x + 10), D(TILE_Y + 8), D(14), D(18)));
            FText(g, dn ? TR(L"Загрузка", L"Download") : TR(L"Отдача", L"Upload"), g_fCaption, T.text2, RectF(D(x + 28), D(TILE_Y + 8), D(tileW - 36), D(18)));
            FmtSpeed(buf, 40, dn ? g_spdIn : g_spdOut, FALSE);
            FText(g, buf, g_fValue, T.text, RectF(D(x + 10), D(TILE_Y + 26), D(tileW - 16), D(26)), AL_LEFT, StringTrimmingEllipsisCharacter);
            Spark(g, RectF(D(x + 10), D(TILE_Y + 56), D(tileW - 20), D(20)), dn ? g_hIn : g_hOut, dn ? T.accent : upColor);
        } else {
            FText(g, TR(L"За сеанс", L"This session"), g_fCaption, T.text2, RectF(D(x + 10), D(TILE_Y + 8), D(tileW - 20), D(18)));
            FGlyph(g, GL_DOWN, D(11), T.accent, RectF(D(x + 10), D(TILE_Y + 30), D(14), D(22)));
            FmtSize(buf, 40, g_sesIn);
            FText(g, buf, g_fStrong, T.text, RectF(D(x + 28), D(TILE_Y + 30), D(tileW - 34), D(22)));
            FGlyph(g, GL_UP, D(11), upColor, RectF(D(x + 10), D(TILE_Y + 54), D(14), D(22)));
            FmtSize(buf, 40, g_sesOut);
            FText(g, buf, g_fStrong, T.text, RectF(D(x + 28), D(TILE_Y + 54), D(tileW - 34), D(22)));
        }
    }

    // ── column header
    FText(g, TR(L"Приложение", L"App"), g_fCaption, T.text2, RectF(D(20), D(TH_Y), D(140), D(TH_H)));
    static const WCHAR* hdr[3] = { TR(L"Загрузка", L"Down"), TR(L"Отдача", L"Up"), TR(L"Скачано", L"Total") };
    for (int c = 0; c < 3; c++) {
        int id = H_COL0 + c, x, w; ColDip(c, &x, &w);
        if (Hv(id)) FillRR(g, RF(g_rc[id]), D(4), Pr(id) ? T.subtlePress : T.subtleHover);
        BOOL act = g_sort == c;
        FText(g, hdr[c], act ? g_fStrong : g_fCaption, act ? T.text : T.text2, RectF(D(x), D(TH_Y), D(w), D(TH_H)), AL_RIGHT);
        if (act) {
            REAL hw = FTextW(g, hdr[c], g_fStrong);
            FGlyph(g, GL_CHEVRON_D, D(8), T.text2, RectF(D(x + w) - hw / 2 - D(5), D(TH_Y - 2), D(10), D(8)));
        }
    }
    FFillRect(g, D(12), D(TH_Y + TH_H + 1), D(W - 24), 1.0f, T.divider);

    // ── rows
    if (!g_etwOk) {
        FGlyph(g, GL_SHIELD, D(24), T.text2, RectF(0, D(ROW_Y + 22), D(W), D(30)));
        FText(g, TR(L"Чтобы видеть трафик каждого приложения,", L"To see the traffic of each app,"), g_fBody, T.text, RectF(0, D(ROW_Y + 58), D(W), D(22)), AL_CENTER);
        FText(g, TR(L"Velox нужны права администратора", L"Velox needs administrator rights"), g_fBody, T.text, RectF(0, D(ROW_Y + 80), D(W), D(22)), AL_CENTER);
        FButton(g, RF(g_rc[H_ELEVATE]), TR(L"Перезапустить с правами", L"Restart as administrator"), TRUE, Hv(H_ELEVATE), Pr(H_ELEVATE), GL_SHIELD);
        FText(g, TR(L"Общая скорость вверху работает и без них", L"The total speed above works without them"), g_fCaption, T.text3, RectF(0, D(ROW_Y + 156), D(W), D(20)), AL_CENTER);
    } else {
        double maxm = 0;
        for (int i = 0; i < g_topN; i++) { double m = Metric(g_top[i]); if (m > maxm) maxm = m; }
        Color heat(T.dark ? 0x30 : 0x26, T.accent.GetR(), T.accent.GetG(), T.accent.GetB());
        for (int i = 0; i < g_topN; i++) {
            Group* gr = g_top[i];
            REAL y = D(ROW_Y + i * ROW_H);
            double m = Metric(gr);
            if (maxm > 0 && m > 0) {
                REAL bw = D(W - 24) * (REAL)(m / maxm);
                if (bw < D(6)) bw = D(6);
                FillRR(g, RectF(D(12), y + D(2), bw, D(ROW_H - 4)), D(4), heat);
            }
            if (!gr->iconTried) LoadGroupIcon(gr);
            RectF ir(D(20), y + (D(ROW_H) - D(16)) / 2, D(16), D(16));
            if (gr->icon) {
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.DrawImage(gr->icon, ir);
            } else {
                FillRR(g, ir, D(4), T.ctrlStroke);
                WCHAR ch[2] = { (WCHAR)towupper(gr->title[0]), 0 };
                FText(g, ch, g_fCaption, T.text2, ir, AL_CENTER);
            }
            FText(g, gr->title, g_fBody, T.text, RectF(D(44), y, D(124), D(ROW_H)), AL_LEFT, StringTrimmingEllipsisCharacter);
            for (int c = 0; c < 3; c++) {
                int x, w; ColDip(c, &x, &w);
                if (c == SORT_DOWN)    FmtSpeed(buf, 40, gr->spdIn, TRUE);
                else if (c == SORT_UP) FmtSpeed(buf, 40, gr->spdOut, TRUE);
                else                   FmtSize(buf, 40, gr->totIn);
                FText(g, buf, g_fBody, g_sort == c ? T.text : T.text2, RectF(D(x - 6), y, D(w + 6), D(ROW_H)), AL_RIGHT);
            }
        }
        if (g_topN == 0)
            FText(g, TR(L"Пока ни одно приложение не обращалось к сети", L"No app has used the network yet"), g_fBody, T.text3, RectF(0, D(ROW_Y), D(W), D(ROW_H * 2)), AL_CENTER);
    }

    // ── footer
    FFillRect(g, D(12), D(FOOT_Y), D(W - 24), 1.0f, T.divider);
    if (g_etwOk) swprintf_s(buf, TR(L"Активных приложений: %d  ·  Счётчики с %02u:%02u", L"Active apps: %d  ·  Counting since %02u:%02u"), g_active, g_since.wHour, g_since.wMinute);
    else swprintf_s(buf, TR(L"Счётчики с %02u:%02u", L"Counting since %02u:%02u"), g_since.wHour, g_since.wMinute);
    FText(g, buf, g_fCaption, T.text3, RectF(D(20), D(FOOT_Y + 1), D(W - 40), D(BASE_H - FOOT_Y - 1)), AL_LEFT, StringTrimmingEllipsisCharacter);
}

static void Render(HDC hdc) {
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
static void ApplyBackdrop() {
    if (g_acrylic) ApplyFrame(g_hwnd, BACKDROP_ACRYLIC);
    else { ApplyFrame(g_hwnd, 1 /*DWMSBT_NONE*/); T.backdrop = FALSE; }
    SendMessageW(g_hwnd, WM_NCACTIVATE, TRUE, 0);
}
static void ApplyThemeAll() { LoadTheme(); ApplyBackdrop(); ApplyMenuTheme(); InvalidateRect(g_hwnd, NULL, FALSE); }

// ───────────────────────────── elevation ─────────────────────────────
// Start an elevated copy (via the installer's task, else UAC). Returns TRUE if one was started.
static BOOL LaunchElevated(BOOL allowUac, const WCHAR* extra) {
    WCHAR self[MAX_PATH]; SelfPath(self);
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = NULL; }
    if (TaskRunFor(TASK_RUN, self)) return TRUE;
    if (allowUac) {
        WCHAR args[64]; swprintf_s(args, L"/noelevate %s", extra ? extra : L"");
        SHELLEXECUTEINFOW se = { sizeof(se) };
        se.lpVerb = L"runas"; se.lpFile = self; se.lpParameters = args; se.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&se)) return TRUE;
    }
    g_mutex = CreateMutexW(NULL, TRUE, L"Local\\Velox_Sallador_SingleInstance");
    return FALSE;
}

// ───────────────────────────── tray & menu ───────────────────────────
static void TrayAdd() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_iconSm;
    wcscpy_s(g_nid.szTip, L"Velox");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayTip() {
    WCHAR a[40], b[40];
    FmtSpeed(a, 40, g_spdIn, FALSE); FmtSpeed(b, 40, g_spdOut, FALSE);
    swprintf_s(g_nid.szTip, TR(L"Velox\nЗагрузка %s\nОтдача %s", L"Velox\nDownload %s\nUpload %s"), a, b);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void SetSort(int s) { g_sort = s; RegSetDw(L"Sort", (DWORD)s); BuildTop(); InvalidateRect(g_hwnd, NULL, FALSE); }
static void ApplyTopmost() {
    SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, g_topmost ? 0 : (LONG_PTR)FindWindowW(L"Progman", NULL));
    SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
static void SetVisible(BOOL v) {
    g_visible = v;
    if (v) { InvalidateRect(g_hwnd, NULL, FALSE); ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); }
    else   { ShowWindow(g_hwnd, SW_HIDE); CanvasFree(g_cv); }
}
static int AutostartState() { return g_elevated ? TaskEnabled(TASK_AUTO) : -2; }
static void ToggleAutostart() {
    int st = TaskEnabled(TASK_AUTO);
    if (st == 1) TaskSetEnabled(TASK_AUTO, FALSE);
    else if (st == 0) TaskSetEnabled(TASK_AUTO, TRUE);
    else {
        WCHAR self[MAX_PATH]; SelfPath(self);
        TaskRegister(TASK_AUTO, self, L"/autostart", TRUE, TRUE);
        TaskRegister(TASK_RUN, self, L"", FALSE, TRUE);
    }
}
static void ShowMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_sort == SORT_DOWN ? MF_CHECKED : 0), ID_SORT_DOWN, TR(L"Сортировать по загрузке", L"Sort by download"));
    AppendMenuW(m, MF_STRING | (g_sort == SORT_UP ? MF_CHECKED : 0), ID_SORT_UP, TR(L"Сортировать по отдаче", L"Sort by upload"));
    AppendMenuW(m, MF_STRING | (g_sort == SORT_TOTAL ? MF_CHECKED : 0), ID_SORT_TOTAL, TR(L"Сортировать по объёму", L"Sort by volume"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_bits ? MF_CHECKED : 0), ID_BITS, TR(L"Скорость в мегабитах (Мбит/с)", L"Speed in megabits (Mbps)"));
    AppendMenuW(m, MF_STRING | (g_countLan ? MF_CHECKED : 0), ID_LAN, TR(L"Учитывать локальную сеть", L"Include local network"));
    AppendMenuW(m, MF_STRING, ID_RESET, TR(L"Сбросить счётчики", L"Reset counters"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_acrylic ? MF_CHECKED : 0), ID_ACRYLIC, TR(L"Прозрачный фон", L"Transparent background"));
    AppendMenuW(m, MF_STRING | (g_topmost ? MF_CHECKED : 0), ID_TOPMOST, TR(L"Поверх всех окон", L"Always on top"));
    int as = AutostartState();
    AppendMenuW(m, MF_STRING | (as == 1 ? MF_CHECKED : 0) | (as == -2 ? MF_GRAYED : 0), ID_AUTOSTART, TR(L"Запускать вместе с Windows", L"Start with Windows"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_SHOWHIDE, g_visible ? TR(L"Скрыть виджет", L"Hide widget") : TR(L"Показать виджет", L"Show widget"));
    AppendLangMenu(m, REG_KEY);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, TR(L"Velox 1.1 · разработчик Sallador", L"Velox 1.1 · by Sallador"));
    AppendMenuW(m, MF_STRING, ID_EXIT, TR(L"Выход", L"Exit"));
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}
static void SavePos() {
    RECT r; GetWindowRect(g_hwnd, &r);
    RegSetDw(L"PosX", (DWORD)r.left); RegSetDw(L"PosY", (DWORD)r.top);
}

// ───────────────────────────── window proc ───────────────────────────
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) { TrayAdd(); if (!g_topmost) ApplyTopmost(); return 0; }
    switch (msg) {
    case WM_NCCALCSIZE: if (w) return 0; break;
    case WM_NCHITTEST:  return HTCLIENT;
    case WM_NCACTIVATE: return DefWindowProcW(h, msg, TRUE, l);
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps); Render(hdc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        g_tick++;
        if (!g_demo) Sample();
        TrayTip();
        if (g_visible) InvalidateRect(h, NULL, FALSE);
        if (g_tick == 3 || g_tick % 30 == 0) SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        if (g_drag) {
            POINT c; GetCursorPos(&c);
            SetWindowPos(h, NULL, g_dragWin.left + c.x - g_dragCursor.x, g_dragWin.top + c.y - g_dragCursor.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
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
        if (g_press == H_NONE) { g_drag = TRUE; GetCursorPos(&g_dragCursor); GetWindowRect(h, &g_dragWin); }
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        BOOL wasDrag = g_drag; int pressed = g_press;
        g_drag = FALSE; g_press = H_NONE;
        if (GetCapture() == h) ReleaseCapture();
        if (wasDrag) { SavePos(); return 0; }
        int id = HitTest(p);
        if (id != H_NONE && id == pressed) {
            if (id == H_MORE) {
                RECT r = g_rc[H_MORE]; POINT pt = { r.left, r.bottom + S(4) };
                ClientToScreen(h, &pt); ShowMenu(pt);
            } else if (id == H_ELEVATE) {
                if (LaunchElevated(TRUE, NULL)) { g_exiting = TRUE; DestroyWindow(h); return 0; }
            } else SetSort(id - H_COL0);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_drag) { g_drag = FALSE; SavePos(); }
        g_press = H_NONE;
        return 0;
    case WM_CONTEXTMENU: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (p.x == -1 && p.y == -1) GetCursorPos(&p);
        ShowMenu(p);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        FluentFonts(); ExtraFonts(); Layout(); CanvasFree(g_cv);
        for (int i = 0; i < g_groupN; i++) { delete g_groups[i].icon; g_groups[i].icon = NULL; g_groups[i].iconTried = g_demo; }
        RECT* r = (RECT*)l;
        SetWindowPos(h, NULL, r->left, r->top, g_w, g_h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_drag) GetWindowRect(h, &g_dragWin), GetCursorPos(&g_dragCursor);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (l && lstrcmpiW((LPCWSTR)l, L"ImmersiveColorSet") == 0) ApplyThemeAll();
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED: ApplyThemeAll(); break;
    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP) SetVisible(!g_visible);
        else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) { POINT p; GetCursorPos(&p); ShowMenu(p); }
        return 0;
    case WM_APP_SHOW: if (!g_visible) SetVisible(TRUE); return 0;
    case WM_CLOSE: g_exiting = TRUE; DestroyWindow(h); return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id >= ID_LANG_AUTO && id <= ID_LANG_EN) { g_mutex = NULL; RestartForLang(REG_KEY, id - ID_LANG_AUTO, L"/noelevate"); g_exiting = TRUE; DestroyWindow(h); return 0; }
        if (id >= ID_SORT_DOWN && id <= ID_SORT_TOTAL) SetSort(id - ID_SORT_DOWN);
        else if (id == ID_BITS)      { g_bits = !g_bits; RegSetDw(L"Bits", g_bits); InvalidateRect(h, NULL, FALSE); }
        else if (id == ID_LAN)       { g_countLan = !g_countLan; RegSetDw(L"CountLan", g_countLan); InvalidateRect(h, NULL, FALSE); }
        else if (id == ID_RESET)     { ResetCounters(); InvalidateRect(h, NULL, FALSE); }
        else if (id == ID_TOPMOST)   { g_topmost = !g_topmost; RegSetDw(L"TopMost", g_topmost); ApplyTopmost(); }
        else if (id == ID_AUTOSTART) ToggleAutostart();
        else if (id == ID_ACRYLIC)   { g_acrylic = !g_acrylic; RegSetDw(L"Acrylic", g_acrylic); ApplyBackdrop(); InvalidateRect(h, NULL, FALSE); }
        else if (id == ID_SHOWHIDE)  SetVisible(!g_visible);
        else if (id == ID_EXIT)      { g_exiting = TRUE; DestroyWindow(h); }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(h, TIMER_ID);
        g_nid.uFlags = 0;
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        CanvasFree(g_cv);
        g_hwnd = NULL;
        if (g_exiting) PostQuitMessage(0);
        else PostThreadMessageW(GetCurrentThreadId(), WM_APP_RECREATE, 0, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

// ───────────────────────────── startup ───────────────────────────────
static BOOL CreateMainWindow() {
    int x = (int)RegGetDw(L"PosX", POS_UNSET), y = (int)RegGetDw(L"PosY", POS_UNSET);
    BOOL havePos = (DWORD)x != POS_UNSET && (DWORD)y != POS_UNSET;
    if (havePos) { POINT c = { x + 20, y + 20 }; if (!MonitorFromPoint(c, MONITOR_DEFAULTTONULL)) havePos = FALSE; }
    HWND owner = g_topmost ? NULL : FindWindowW(L"Progman", NULL);
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | (g_topmost ? WS_EX_TOPMOST : 0), CLASS_NAME, APP_NAME, WS_POPUP | WS_THICKFRAME,
                             havePos ? x : 0, havePos ? y : 0, BASE_W, BASE_H, owner, NULL, g_hInst, NULL);
    if (!h) return FALSE;
    g_hwnd = h;
    ChangeWindowMessageFilterEx(h, WM_APP_SHOW, MSGFLT_ALLOW, NULL);   // a non-elevated second start may poke us
    g_dpi = GetDpiForWindow(h); if (!g_dpi) g_dpi = 96;
    FluentFonts(); ExtraFonts();
    Layout();
    ApplyBackdrop();
    if (!havePos) {
        MONITORINFO mi = { sizeof(mi) }; POINT o = { 0, 0 };
        GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi);
        x = mi.rcWork.left + S(24); y = mi.rcWork.top + S(24);
    }
    SetWindowPos(h, NULL, x, y, g_w, g_h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    TrayAdd();
    if (g_visible) ShowWindow(h, SW_SHOWNOACTIVATE);
    SetTimer(h, TIMER_ID, 1000, NULL);
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmd, int) {
    g_hInst = hInst;
    InitLang(REG_KEY);
    g_demo = cmd && wcsstr(cmd, L"/demo") != NULL;
    BOOL noElevate = cmd && wcsstr(cmd, L"/noelevate") != NULL;
    BOOL autostart = cmd && wcsstr(cmd, L"/autostart") != NULL;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    g_mutex = g_mutexLang = CreateMutexW(NULL, TRUE, g_demo ? L"Local\\Velox_Sallador_Demo" : L"Local\\Velox_Sallador_SingleInstance");
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS || (!g_mutex && err == ERROR_ACCESS_DENIED)) {
        HWND w = FindWindowW(CLASS_NAME, NULL);
        if (w) PostMessageW(w, WM_APP_SHOW, 0, 0);
        return 0;
    }
    g_elevated = IsElevated();
    if (!g_demo && !g_elevated && !noElevate) {
        // autostart via the old Run key would have no rights: prefer the task, then UAC (not at logon)
        if (LaunchElevated(!autostart, autostart ? L"/autostart" : NULL)) return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    GdiplusStartupInput gsi; ULONG_PTR gtoken;
    if (GdiplusStartup(&gtoken, &gsi, NULL) != Ok) return 1;
    FluentInit(); LoadTheme(); ApplyMenuTheme();

    g_sort     = (int)RegGetDw(L"Sort", SORT_DOWN); if (g_sort < 0 || g_sort > 2) g_sort = SORT_DOWN;
    g_topmost  = RegGetDw(L"TopMost", 0) != 0;
    g_acrylic  = RegGetDw(L"Acrylic", 1) != 0;
    g_bits     = RegGetDw(L"Bits", 0) != 0;
    g_countLan = RegGetDw(L"CountLan", 0) != 0;
    GetLocalTime(&g_since);

    InitializeCriticalSection(&g_cs);
    g_acc     = (Acc*)Alloc(sizeof(Acc) * ACC_SIZE);
    g_accCopy = (Acc*)Alloc(sizeof(Acc) * ACC_SIZE);
    g_pidMap  = (PidEnt*)Alloc(sizeof(PidEnt) * PIDMAP_SIZE);
    g_groups  = (Group*)Alloc(sizeof(Group) * MAX_GROUPS);
    if (!g_acc || !g_accCopy || !g_pidMap || !g_groups) return 1;

    g_iconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = g_iconBig; wc.hIconSm = g_iconSm; wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilter(g_msgTaskbarCreated, MSGFLT_ADD);

    if (g_demo) DemoData();
    else {
        g_etwOk = g_elevated && StartEtw();
        Sample();
    }
    if (!CreateMainWindow()) { if (g_etwOk) StopEtw(); return 1; }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!m.hwnd && m.message == WM_APP_RECREATE) { Sleep(1500); if (!CreateMainWindow()) break; continue; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_etwOk && !g_demo) StopEtw();
    GdiplusShutdown(gtoken);
    CoUninitialize();
    return 0;
}
