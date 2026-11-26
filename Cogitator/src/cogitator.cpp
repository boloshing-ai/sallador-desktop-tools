// Cogitator 2.1 — desktop widget: top-10 applications by CPU / GPU / RAM.
// Windows 11 (Fluent) edition. Developer: Sallador.  Pure Win32 + GDI+, static CRT.

#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <winternl.h>
#include <shellapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "resource.h"
#include "fluent.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "pdh.lib")

// ───────────────────────────── constants ─────────────────────────────
#define APP_NAME     L"Cogitator"
#define CLASS_NAME   L"CogitatorWidget"
#define REG_KEY      L"Software\\Sallador\\Cogitator"
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define WM_TRAY          (WM_APP + 1)
#define WM_APP_SHOW      (WM_APP + 2)
#define WM_APP_RECREATE  (WM_APP + 3)
#define TIMER_ID     1
#define MAX_ROWS     10
#define MAX_PROC     4096
#define MAX_GROUPS   2048
#define MAX_GPUI     16384
#define MAX_APPPIDS  1024
#define POS_UNSET    0x80000000u

// base layout (DIPs)
#define BASE_W   360
#define BASE_H   432
#define HDR_H    44
#define TILE_Y   46
#define TILE_H   72
#define TH_Y     128
#define TH_H     28
#define ROW_Y    160
#define ROW_H    24
#define FOOT_Y   404

enum { SORT_CPU = 0, SORT_GPU = 1, SORT_RAM = 2 };
enum {
    ID_SORT_CPU = 101, ID_SORT_GPU, ID_SORT_RAM,
    ID_TOPMOST = 110, ID_AUTOSTART, ID_SHOWHIDE, ID_EXIT /*=113, used by installer*/, ID_SHOWALL, ID_ACRYLIC
};
enum { H_NONE = -1, H_MORE = 0, H_COL0 = 1, H_COUNT = 4 };

// ───────────────────────────── NT structures ─────────────────────────
typedef struct _SPI {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
} SPI;
typedef LONG (NTAPI *PFN_NtQSI)(ULONG, PVOID, ULONG, PULONG);

struct PrevCpu { ULONG_PTR pid; LONGLONG create; LONGLONG cpu; };
struct Group   { WCHAR name[48]; UINT32 hash; int count; int app; double cpu; double gpu; ULONGLONG ram; };
struct GpuItem { ULONG pid; UINT32 key; double v; };
struct PidGpu  { ULONG pid; double v; };

// ───────────────────────────── globals ───────────────────────────────
static HINSTANCE g_hInst;
static HWND      g_hwnd;
static int       g_w, g_h;
static Canvas    g_cv;
static RECT      g_rc[H_COUNT];
static int       g_hover = H_NONE, g_press = H_NONE;
static BOOL      g_tracking;
static BOOL      g_drag; static POINT g_dragCursor; static RECT g_dragWin;
static HICON     g_iconSm, g_iconBig;
static NOTIFYICONDATAW g_nid;
static UINT      g_msgTaskbarCreated;

static int  g_sort = SORT_CPU;
static BOOL g_topmost = FALSE;
static BOOL g_acrylic = TRUE;
static BOOL g_visible = TRUE;
static BOOL g_exiting = FALSE;
static BOOL g_showAll = FALSE;
static unsigned g_tick;

static DWORD g_appPids[MAX_APPPIDS]; static int g_appPidN;
static int   g_appCount;

static PFN_NtQSI g_NtQSI;
static BYTE*     g_spiBuf;  static ULONG g_spiSize;
static PrevCpu*  g_prev;    static PrevCpu* g_prevNew; static int g_prevN;
static Group*    g_groups;  static int g_groupN;
static Group     g_top[MAX_ROWS]; static int g_topN;
static LONGLONG  g_lastQpc, g_qpf;
static int       g_nCpu = 1;
static BOOL      g_havePrev;
static ULONGLONG g_lastIdle, g_lastKern, g_lastUser;
static double    g_cpuTotal, g_gpuTotal = -1;
static ULONGLONG g_ramUsed, g_ramTotal;
static int       g_procs;

static PDH_HQUERY   g_pdhQ;
static PDH_HCOUNTER g_pdhC;
static BOOL         g_gpuOk;
static BYTE*        g_pdhBuf; static DWORD g_pdhBufSize;
static GpuItem*     g_gi;
static PidGpu*      g_pg; static int g_pgN;

// ───────────────────────────── helpers ───────────────────────────────
static void* Alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void  Free(void* p)   { if (p) HeapFree(GetProcessHeap(), 0, p); }
static ULONGLONG FT(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static UINT32 HashName(const WCHAR* s, int n) {
    UINT32 h = 2166136261u;
    for (int i = 0; i < n; i++) {
        WCHAR c = s[i];
        if (c >= L'A' && c <= L'Z') c += 32; else if (c > 127) c = towlower(c);
        h ^= c; h *= 16777619u;
    }
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
static BOOL IsAutostart() {
    WCHAR buf[MAX_PATH * 2]; DWORD sz = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS;
}
static void SetAutostart(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        WCHAR path[MAX_PATH], cmd[MAX_PATH + 8];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        swprintf_s(cmd, L"\"%s\"", path);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE*)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

// ───────────────────────────── GPU (PDH) ─────────────────────────────
static void InitGpu() {
    if (PdhOpenQueryW(NULL, 0, &g_pdhQ) != ERROR_SUCCESS) { g_pdhQ = NULL; return; }
    if (PdhAddEnglishCounterW(g_pdhQ, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_pdhC) != ERROR_SUCCESS) {
        PdhCloseQuery(g_pdhQ); g_pdhQ = NULL; return;
    }
    PdhCollectQueryData(g_pdhQ);
    g_gpuOk = TRUE;
}
static int CmpGiKey(const void* a, const void* b) {
    UINT32 x = ((const GpuItem*)a)->key, y = ((const GpuItem*)b)->key;
    return x < y ? -1 : x > y ? 1 : 0;
}
static int CmpGiPid(const void* a, const void* b) {
    const GpuItem* x = (const GpuItem*)a; const GpuItem* y = (const GpuItem*)b;
    if (x->pid != y->pid) return x->pid < y->pid ? -1 : 1;
    return x->key < y->key ? -1 : x->key > y->key ? 1 : 0;
}
static void SampleGpu() {
    g_pgN = 0;
    if (!g_gpuOk) { g_gpuTotal = -1; return; }
    if (PdhCollectQueryData(g_pdhQ) != ERROR_SUCCESS) { g_gpuTotal = 0; return; }
    DWORD count = 0; PDH_STATUS st = ERROR_SUCCESS;
    for (int tries = 0; ; tries++) {
        DWORD size = g_pdhBufSize;
        st = PdhGetFormattedCounterArrayW(g_pdhC, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                          (PPDH_FMT_COUNTERVALUE_ITEM_W)g_pdhBuf);
        if (st == (PDH_STATUS)PDH_MORE_DATA && tries < 3) {
            Free(g_pdhBuf);
            g_pdhBufSize = size + 16384;
            g_pdhBuf = (BYTE*)Alloc(g_pdhBufSize);
            if (!g_pdhBuf) { g_pdhBufSize = 0; g_gpuTotal = 0; return; }
            continue;
        }
        break;
    }
    if (st != ERROR_SUCCESS) { g_gpuTotal = 0; return; }
    PPDH_FMT_COUNTERVALUE_ITEM_W it = (PPDH_FMT_COUNTERVALUE_ITEM_W)g_pdhBuf;
    int n = 0;
    for (DWORD i = 0; i < count && n < MAX_GPUI; i++) {
        DWORD cs = it[i].FmtValue.CStatus;
        if (cs != PDH_CSTATUS_VALID_DATA && cs != PDH_CSTATUS_NEW_DATA) continue;
        double v = it[i].FmtValue.doubleValue;
        if (v <= 0) continue;
        const WCHAR* nm = it[i].szName;           // pid_1234_luid_0x..._phys_0_eng_0_engtype_3D
        if (!nm || wcsncmp(nm, L"pid_", 4) != 0) continue;
        const WCHAR* q = nm + 4; ULONG pid = 0;
        while (*q >= L'0' && *q <= L'9') { pid = pid * 10 + (*q - L'0'); q++; }
        g_gi[n].pid = pid; g_gi[n].key = HashName(q, (int)wcslen(q)); g_gi[n].v = v; n++;
    }
    qsort(g_gi, n, sizeof(GpuItem), CmpGiKey);
    double best = 0;
    for (int i = 0; i < n;) {
        int j = i; double sum = 0;
        while (j < n && g_gi[j].key == g_gi[i].key) sum += g_gi[j++].v;
        if (sum > best) best = sum;
        i = j;
    }
    g_gpuTotal = best > 100 ? 100 : best;
    qsort(g_gi, n, sizeof(GpuItem), CmpGiPid);
    for (int i = 0; i < n;) {
        ULONG pid = g_gi[i].pid; double mx = 0; int j = i;
        while (j < n && g_gi[j].pid == pid) {
            int k = j; double s = 0;
            while (k < n && g_gi[k].pid == pid && g_gi[k].key == g_gi[j].key) s += g_gi[k++].v;
            if (s > mx) mx = s;
            j = k;
        }
        g_pg[g_pgN].pid = pid; g_pg[g_pgN].v = mx; g_pgN++;
        i = j;
    }
}
static double FindGpu(ULONG pid) {
    int lo = 0, hi = g_pgN - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_pg[m].pid == pid) return g_pg[m].v;
        if (g_pg[m].pid < pid) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

// ───────────────────────────── application detection ─────────────────
// A process is an "application" if it owns a visible, uncloaked top-level window that would
// appear on the taskbar / Alt+Tab (same idea as the "Apps" group in Task Manager).
static void AddAppPid(DWORD pid) {
    if (!pid || pid == GetCurrentProcessId()) return;
    for (int i = 0; i < g_appPidN; i++) if (g_appPids[i] == pid) return;
    if (g_appPidN < MAX_APPPIDS) g_appPids[g_appPidN++] = pid;
}
static BOOL CALLBACK UwpChildProc(HWND h, LPARAM l) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != (DWORD)l) { AddAppPid(pid); return FALSE; }
    return TRUE;
}
static BOOL CALLBACK AppWndProc(HWND h, LPARAM) {
    if (!IsWindowVisible(h)) return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    BOOL appWin = (ex & WS_EX_APPWINDOW) != 0;
    if (!appWin && ((ex & WS_EX_TOOLWINDOW) || GetWindow(h, GW_OWNER))) return TRUE;
    if ((ex & WS_EX_NOACTIVATE) && !appWin) return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    if (GetWindowTextLengthW(h) == 0) return TRUE;
    WCHAR cls[64]; GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (!wcscmp(cls, L"ApplicationFrameWindow")) { EnumChildWindows(h, UwpChildProc, (LPARAM)pid); return TRUE; }
    AddAppPid(pid);
    return TRUE;
}
static int CmpDw(const void* a, const void* b) {
    DWORD x = *(const DWORD*)a, y = *(const DWORD*)b; return x < y ? -1 : x > y ? 1 : 0;
}
static void CollectAppPids() {
    g_appPidN = 0;
    EnumWindows(AppWndProc, 0);
    qsort(g_appPids, g_appPidN, sizeof(DWORD), CmpDw);
}
static BOOL IsAppPid(DWORD pid) {
    return bsearch(&pid, g_appPids, g_appPidN, sizeof(DWORD), CmpDw) != NULL;
}

// ───────────────────────────── CPU / RAM sampling ────────────────────
static int CmpPrev(const void* a, const void* b) {
    ULONG_PTR x = ((const PrevCpu*)a)->pid, y = ((const PrevCpu*)b)->pid;
    return x < y ? -1 : x > y ? 1 : 0;
}
static PrevCpu* FindPrev(ULONG_PTR pid) {
    int lo = 0, hi = g_prevN - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_prev[m].pid == pid) return &g_prev[m];
        if (g_prev[m].pid < pid) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}
static double Metric(const Group* g, int s) {
    return s == SORT_CPU ? g->cpu : s == SORT_GPU ? g->gpu : (double)g->ram;
}
static int CmpGroup(const void* a, const void* b) {
    const Group* x = (const Group*)a; const Group* y = (const Group*)b;
    double mx = Metric(x, g_sort), my = Metric(y, g_sort);
    if (mx != my) return mx < my ? 1 : -1;
    if (x->ram != y->ram) return x->ram < y->ram ? 1 : -1;
    return 0;
}
static void BuildTop() {
    qsort(g_groups, g_groupN, sizeof(Group), CmpGroup);
    g_topN = 0; g_appCount = 0;
    for (int i = 0; i < g_groupN; i++) {
        if (g_groups[i].app) g_appCount++;
        if (!g_showAll && !g_groups[i].app) continue;
        if (g_topN < MAX_ROWS) g_top[g_topN++] = g_groups[i];
    }
}

static void Sample() {
    if (!g_NtQSI) return;
    ULONG ret = 0; LONG st = -1;
    for (int tries = 0; tries < 5; tries++) {
        st = g_NtQSI(5 /*SystemProcessInformation*/, g_spiBuf, g_spiSize, &ret);
        if (st == (LONG)0xC0000004 /*STATUS_INFO_LENGTH_MISMATCH*/) {
            Free(g_spiBuf);
            g_spiSize = ret + 128 * 1024;
            g_spiBuf = (BYTE*)Alloc(g_spiSize);
            if (!g_spiBuf) { g_spiSize = 0; return; }
            continue;
        }
        break;
    }
    if (st < 0) return;

    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    double denom = (double)(q.QuadPart - g_lastQpc) * 1e7 / (double)g_qpf * g_nCpu;  // 100ns units
    g_lastQpc = q.QuadPart;

    SampleGpu();
    CollectAppPids();

    int newN = 0; g_groupN = 0; g_procs = 0;
    BYTE* p = g_spiBuf;
    for (;;) {
        SPI* e = (SPI*)p;
        ULONG_PTR pid = (ULONG_PTR)e->UniqueProcessId;
        g_procs++;
        if (pid != 0) {
            LONGLONG cpu = e->KernelTime.QuadPart + e->UserTime.QuadPart;
            double pct = 0;
            if (g_havePrev && denom > 0) {
                PrevCpu* pr = FindPrev(pid);
                if (pr && pr->create == e->CreateTime.QuadPart) {
                    pct = (double)(cpu - pr->cpu) * 100.0 / denom;
                    if (pct < 0) pct = 0;
                }
            }
            if (newN < MAX_PROC) {
                g_prevNew[newN].pid = pid; g_prevNew[newN].create = e->CreateTime.QuadPart; g_prevNew[newN].cpu = cpu; newN++;
            }
            WCHAR nm[48]; int n = 0;
            if (e->ImageName.Buffer && e->ImageName.Length) {
                n = e->ImageName.Length / 2; if (n > 47) n = 47;
                memcpy(nm, e->ImageName.Buffer, n * sizeof(WCHAR));
            } else { wcscpy_s(nm, L"System"); n = 6; }
            nm[n] = 0;
            if (n > 4 && _wcsicmp(nm + n - 4, L".exe") == 0) { n -= 4; nm[n] = 0; }

            UINT32 h = HashName(nm, n);
            Group* g = NULL;
            for (int i = 0; i < g_groupN; i++)
                if (g_groups[i].hash == h && _wcsicmp(g_groups[i].name, nm) == 0) { g = &g_groups[i]; break; }
            if (!g && g_groupN < MAX_GROUPS) {
                g = &g_groups[g_groupN++];
                wcscpy_s(g->name, nm); g->hash = h; g->count = 0; g->app = 0; g->cpu = 0; g->gpu = 0; g->ram = 0;
            }
            if (g) {
                if (IsAppPid((DWORD)pid)) g->app = 1;
                g->cpu += pct;
                g->gpu += FindGpu((ULONG)pid);
                g->ram += (ULONGLONG)e->WorkingSetPrivateSize.QuadPart;
                g->count++;
            }
        }
        if (!e->NextEntryOffset) break;
        p += e->NextEntryOffset;
    }
    for (int i = 0; i < g_groupN; i++) {
        if (g_groups[i].cpu > 100) g_groups[i].cpu = 100;
        if (g_groups[i].gpu > 100) g_groups[i].gpu = 100;
    }
    qsort(g_prevNew, newN, sizeof(PrevCpu), CmpPrev);
    PrevCpu* t = g_prev; g_prev = g_prevNew; g_prevNew = t; g_prevN = newN;

    FILETIME fi, fk, fu;
    if (GetSystemTimes(&fi, &fk, &fu)) {
        ULONGLONG i = FT(fi), k = FT(fk), u = FT(fu);
        if (g_havePrev) {
            ULONGLONG di = i - g_lastIdle, dt = (k - g_lastKern) + (u - g_lastUser);
            g_cpuTotal = dt ? (double)(dt - di) * 100.0 / (double)dt : 0;
            if (g_cpuTotal < 0) g_cpuTotal = 0;
            if (g_cpuTotal > 100) g_cpuTotal = 100;
        }
        g_lastIdle = i; g_lastKern = k; g_lastUser = u;
    }
    MEMORYSTATUSEX ms = { sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) { g_ramTotal = ms.ullTotalPhys; g_ramUsed = ms.ullTotalPhys - ms.ullAvailPhys; }
    g_havePrev = TRUE;
    BuildTop();
}

// ───────────────────────────── rendering ─────────────────────────────
static void Comma(WCHAR* s) { for (; *s; s++) if (*s == L'.') *s = L','; }
static void FmtPct(WCHAR* b, int n, double v) {
    if (v < 0) wcscpy_s(b, n, L"—");
    else if (v < 9.95) swprintf_s(b, n, L"%.1f%%", v);
    else swprintf_s(b, n, L"%.0f%%", v);
    Comma(b);
}
static void FmtMem(WCHAR* b, int n, ULONGLONG bytes) {
    double mb = bytes / 1048576.0;
    if (mb >= 1024) swprintf_s(b, n, L"%.1f ГБ", mb / 1024.0);
    else swprintf_s(b, n, L"%.0f МБ", mb);
    Comma(b);
}
// value columns: x and width (DIPs)
static void ColDip(int col, int* x, int* w) {
    const int W = BASE_W;
    if (col == SORT_CPU)      { *x = W - 180; *w = 50; }
    else if (col == SORT_GPU) { *x = W - 126; *w = 48; }
    else                      { *x = W - 74;  *w = 58; }
}
static void SetR(int id, int x, int y, int w, int h) { g_rc[id] = { S(x), S(y), S(x + w), S(y + h) }; }
static void Layout() {
    SetR(H_MORE, BASE_W - 42, 8, 30, 30);
    for (int c = 0; c < 3; c++) { int x, w; ColDip(c, &x, &w); SetR(H_COL0 + c, x - 4, TH_Y, w + 8, TH_H); }
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
    WCHAR buf[160];

    // ── header
    DrawAppMark(g, D(14), D(14), D(16));
    FText(g, L"Cogitator", g_fStrong, T.text, RectF(D(38), 0, D(120), D(HDR_H)));
    REAL tw = FTextW(g, L"Cogitator", g_fStrong);
    FText(g, L"·  Sallador", g_fCaption, T.text3, RectF(D(38) + tw + D(8), 0, D(120), D(HDR_H)));
    FText(g, g_showAll ? L"Все процессы" : L"Приложения", g_fCaption, T.text3, RectF(D(W - 170), 0, D(124), D(HDR_H)), AL_RIGHT);
    RectF mr = RF(g_rc[H_MORE]);
    if (Hv(H_MORE)) FillRR(g, mr, D(4), Pr(H_MORE) ? T.subtlePress : T.subtleHover);
    FGlyph(g, GL_MORE, D(14), T.text2, mr);

    // ── summary tiles
    static const WCHAR* lbl[3] = { L"ЦП", L"ГП", L"Память" };
    double vals[3] = { g_cpuTotal, g_gpuTotal, g_ramTotal ? g_ramUsed * 100.0 / (double)g_ramTotal : 0 };
    int tileW = (W - 24 - 16) / 3;
    for (int i = 0; i < 3; i++) {
        int x = 12 + i * (tileW + 8);
        FCard(g, RectF(D(x), D(TILE_Y), D(tileW), D(TILE_H)));
        double v = vals[i];
        BOOL hot = v >= 85;
        FText(g, lbl[i], g_fCaption, T.text2, RectF(D(x + 12), D(TILE_Y + 8), D(tileW - 24), D(18)));
        if (i == 2) { swprintf_s(buf, L"%.1f ГБ", g_ramUsed / 1073741824.0); Comma(buf); }
        else if (v < 0) wcscpy_s(buf, L"—");
        else swprintf_s(buf, L"%.0f%%", v);
        FText(g, buf, g_fSubtitle, hot ? T.critical : T.text, RectF(D(x + 12), D(TILE_Y + 26), D(tileW - 24), D(28)));
        FProgress(g, D(x + 12), D(TILE_Y + TILE_H - 14), D(tileW - 24), v < 0 ? 0 : (REAL)(v / 100.0), hot ? T.critical : T.accent);
    }

    // ── column header
    FText(g, g_showAll ? L"Процесс" : L"Имя", g_fCaption, T.text2, RectF(D(20), D(TH_Y), D(150), D(TH_H)));
    static const WCHAR* hdr[3] = { L"ЦП", L"ГП", L"Память" };
    for (int c = 0; c < 3; c++) {
        int id = H_COL0 + c, x, w; ColDip(c, &x, &w);
        if (Hv(id)) FillRR(g, RF(g_rc[id]), D(4), Pr(id) ? T.subtlePress : T.subtleHover);
        BOOL act = g_sort == c;
        FText(g, hdr[c], act ? g_fStrong : g_fCaption, act ? T.text : T.text2, RectF(D(x), D(TH_Y), D(w), D(TH_H)), AL_RIGHT);
        if (act) {
            REAL hw = FTextW(g, hdr[c], g_fStrong);
            FGlyph(g, GL_CHEVRON_D, D(8), T.text2, RectF(D(x + w) - hw / 2 - D(5), D(TH_Y - 2), D(10), D(8)));   // above the label, like Task Manager
        }
    }
    FFillRect(g, D(12), D(TH_Y + TH_H + 1), D(W - 24), 1.0f, T.divider);

    // ── rows
    double maxm = 0;
    for (int i = 0; i < g_topN; i++) { double m = Metric(&g_top[i], g_sort); if (m > maxm) maxm = m; }
    Color heat(T.dark ? 0x30 : 0x26, T.accent.GetR(), T.accent.GetG(), T.accent.GetB());
    REAL nameX = D(20), nameMax = D(W - 200);
    for (int i = 0; i < g_topN; i++) {
        const Group* gr = &g_top[i];
        REAL y = D(ROW_Y + i * ROW_H);
        double m = Metric(gr, g_sort);
        if (maxm > 0 && m > 0) {
            REAL bw = D(W - 24) * (REAL)(m / maxm);
            if (bw < D(6)) bw = D(6);
            FillRR(g, RectF(D(12), y + D(2), bw, D(ROW_H - 4)), D(4), heat);
        }
        WCHAR cnt[16] = L"";
        if (gr->count > 1) swprintf_s(cnt, L"×%d", gr->count);
        REAL cw = cnt[0] ? FTextW(g, cnt, g_fCaption) + D(6) : 0;
        REAL avail = nameMax - cw, nw = FTextW(g, gr->name, g_fBody);
        if (nw > avail) nw = avail;
        FText(g, gr->name, g_fBody, T.text, RectF(nameX, y, avail, D(ROW_H)), AL_LEFT, StringTrimmingEllipsisCharacter);
        if (cnt[0]) FText(g, cnt, g_fCaption, T.text3, RectF(nameX + nw + D(6), y, cw + D(4), D(ROW_H)));
        for (int c = 0; c < 3; c++) {
            int x, w; ColDip(c, &x, &w);
            if (c == SORT_CPU) FmtPct(buf, 32, gr->cpu);
            else if (c == SORT_GPU) FmtPct(buf, 32, g_gpuOk ? gr->gpu : -1);
            else FmtMem(buf, 32, gr->ram);
            FText(g, buf, g_fBody, g_sort == c ? T.text : T.text2, RectF(D(x), y, D(w), D(ROW_H)), AL_RIGHT);
        }
    }
    if (g_topN == 0)
        FText(g, L"Нет открытых приложений", g_fBody, T.text3, RectF(0, D(ROW_Y), D(W), D(ROW_H * 2)), AL_CENTER);

    // ── footer
    FFillRect(g, D(12), D(FOOT_Y), D(W - 24), 1.0f, T.divider);
    ULONGLONG up = GetTickCount64() / 1000;
    unsigned d = (unsigned)(up / 86400), hr = (unsigned)(up / 3600 % 24), mi = (unsigned)(up / 60 % 60), se = (unsigned)(up % 60);
    WCHAR ups[32];
    if (d) swprintf_s(ups, L"%u д %02u:%02u", d, hr, mi); else swprintf_s(ups, L"%02u:%02u:%02u", hr, mi, se);
    swprintf_s(buf, L"Приложений %d  ·  Процессов %d  ·  Работает %s", g_appCount, g_procs, ups);
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
    SendMessageW(g_hwnd, WM_NCACTIVATE, TRUE, 0);   // the widget is never activated; render the backdrop as active
}
static void ApplyThemeAll() {
    LoadTheme();
    ApplyBackdrop();
    ApplyMenuTheme();
    InvalidateRect(g_hwnd, NULL, FALSE);
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
    wcscpy_s(g_nid.szTip, L"Cogitator");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayTip() {
    double ram = g_ramTotal ? g_ramUsed * 100.0 / (double)g_ramTotal : 0;
    if (g_gpuOk) swprintf_s(g_nid.szTip, L"Cogitator\nЦП %.0f%%   ГП %.0f%%   Память %.0f%%", g_cpuTotal, g_gpuTotal, ram);
    else         swprintf_s(g_nid.szTip, L"Cogitator\nЦП %.0f%%   Память %.0f%%", g_cpuTotal, ram);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void SetSort(int s) {
    g_sort = s; RegSetDw(L"Sort", (DWORD)s);
    BuildTop(); InvalidateRect(g_hwnd, NULL, FALSE);
}
static void ApplyTopmost() {
    // When not topmost the widget is owned by the desktop (Progman) so it survives "Show desktop"
    SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, g_topmost ? 0 : (LONG_PTR)FindWindowW(L"Progman", NULL));
    SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
static void SetVisible(BOOL v) {
    g_visible = v;
    if (v) {
        Sample(); InvalidateRect(g_hwnd, NULL, FALSE);
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        SetTimer(g_hwnd, TIMER_ID, 1000, NULL);
    } else {
        KillTimer(g_hwnd, TIMER_ID);
        ShowWindow(g_hwnd, SW_HIDE);
        CanvasFree(g_cv);
    }
}
static void ShowMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_sort == SORT_CPU ? MF_CHECKED : 0), ID_SORT_CPU, L"Сортировать по ЦП");
    AppendMenuW(m, MF_STRING | (g_sort == SORT_GPU ? MF_CHECKED : 0), ID_SORT_GPU, L"Сортировать по ГП");
    AppendMenuW(m, MF_STRING | (g_sort == SORT_RAM ? MF_CHECKED : 0), ID_SORT_RAM, L"Сортировать по памяти");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_showAll ? MF_CHECKED : 0), ID_SHOWALL, L"Показывать фоновые процессы");
    AppendMenuW(m, MF_STRING | (g_acrylic ? MF_CHECKED : 0), ID_ACRYLIC, L"Прозрачный фон");
    AppendMenuW(m, MF_STRING | (g_topmost ? MF_CHECKED : 0), ID_TOPMOST, L"Поверх всех окон");
    AppendMenuW(m, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, L"Запускать вместе с Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_SHOWHIDE, g_visible ? L"Скрыть виджет" : L"Показать виджет");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Cogitator 2.1 · разработчик Sallador");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Выход");
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
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        TrayAdd(); if (!g_topmost) ApplyTopmost();
        return 0;
    }
    switch (msg) {
    case WM_NCCALCSIZE: if (w) return 0; break;      // whole window is client area
    case WM_NCHITTEST:  return HTCLIENT;             // custom drag below — no resize, no snap
    case WM_NCACTIVATE: return DefWindowProcW(h, msg, TRUE, l);   // keep the Acrylic backdrop "active"
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
        Render(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        g_tick++;
        Sample(); TrayTip();
        InvalidateRect(h, NULL, FALSE);
        if (g_tick == 1) SetTimer(h, TIMER_ID, 1000, NULL);
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
        if (g_press == H_NONE) {                       // start dragging the widget
            g_drag = TRUE;
            GetCursorPos(&g_dragCursor);
            GetWindowRect(h, &g_dragWin);
        }
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        BOOL wasDrag = g_drag;
        int pressed = g_press;
        g_drag = FALSE; g_press = H_NONE;
        if (GetCapture() == h) ReleaseCapture();
        if (wasDrag) { SavePos(); return 0; }
        int id = HitTest(p);
        if (id != H_NONE && id == pressed) {
            if (id == H_MORE) {
                RECT r = g_rc[H_MORE]; POINT pt = { r.left, r.bottom + S(4) };
                ClientToScreen(h, &pt); ShowMenu(pt);
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
        FluentFonts(); Layout(); CanvasFree(g_cv);
        RECT* r = (RECT*)l;
        SetWindowPos(h, NULL, r->left, r->top, g_w, g_h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_drag) GetWindowRect(h, &g_dragWin), GetCursorPos(&g_dragCursor);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (l && lstrcmpiW((LPCWSTR)l, L"ImmersiveColorSet") == 0) ApplyThemeAll();
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ApplyThemeAll();
        break;
    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP) SetVisible(!g_visible);
        else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) { POINT p; GetCursorPos(&p); ShowMenu(p); }
        return 0;
    case WM_APP_SHOW:
        if (!g_visible) SetVisible(TRUE);
        return 0;
    case WM_CLOSE:
        g_exiting = TRUE; DestroyWindow(h);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id >= ID_SORT_CPU && id <= ID_SORT_RAM) SetSort(id - ID_SORT_CPU);
        else if (id == ID_TOPMOST)   { g_topmost = !g_topmost; RegSetDw(L"TopMost", g_topmost); ApplyTopmost(); }
        else if (id == ID_AUTOSTART) SetAutostart(!IsAutostart());
        else if (id == ID_SHOWALL)   { g_showAll = !g_showAll; RegSetDw(L"ShowAll", g_showAll); BuildTop(); InvalidateRect(h, NULL, FALSE); }
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
        if (g_exiting) PostQuitMessage(0);
        else PostThreadMessageW(GetCurrentThreadId(), WM_APP_RECREATE, 0, 0);  // owner (explorer) died — rebuild
        g_hwnd = NULL;
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
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | (g_topmost ? WS_EX_TOPMOST : 0),
                             CLASS_NAME, APP_NAME, WS_POPUP | WS_THICKFRAME,
                             havePos ? x : 0, havePos ? y : 0, BASE_W, BASE_H,
                             owner, NULL, g_hInst, NULL);
    if (!h) return FALSE;
    g_hwnd = h;
    g_dpi = GetDpiForWindow(h); if (!g_dpi) g_dpi = 96;
    FluentFonts();
    Layout();
    ApplyBackdrop();
    if (!havePos) {
        MONITORINFO mi = { sizeof(mi) }; POINT o = { 0, 0 };
        GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi);
        x = mi.rcWork.right - g_w - S(24); y = mi.rcWork.top + S(24);
    }
    SetWindowPos(h, NULL, x, y, g_w, g_h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    TrayAdd();
    if (g_visible) {
        ShowWindow(h, SW_SHOWNOACTIVATE);
        SetTimer(h, TIMER_ID, 400, NULL);
    }
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;
    CreateMutexW(NULL, TRUE, L"Local\\Cogitator_Sallador_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(CLASS_NAME, NULL);
        if (w) PostMessageW(w, WM_APP_SHOW, 0, 0);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                          PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
                                          PROCESS_POWER_THROTTLING_EXECUTION_SPEED };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));   // Efficiency mode

    GdiplusStartupInput gsi; ULONG_PTR gtoken;
    if (GdiplusStartup(&gtoken, &gsi, NULL) != Ok) return 1;
    FluentInit();
    LoadTheme();
    ApplyMenuTheme();

    g_NtQSI = (PFN_NtQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    g_spiSize = 1 << 20;
    g_spiBuf  = (BYTE*)Alloc(g_spiSize);
    g_prev    = (PrevCpu*)Alloc(sizeof(PrevCpu) * MAX_PROC);
    g_prevNew = (PrevCpu*)Alloc(sizeof(PrevCpu) * MAX_PROC);
    g_groups  = (Group*)Alloc(sizeof(Group) * MAX_GROUPS);
    g_gi      = (GpuItem*)Alloc(sizeof(GpuItem) * MAX_GPUI);
    g_pg      = (PidGpu*)Alloc(sizeof(PidGpu) * MAX_GPUI);
    if (!g_prev || !g_prevNew || !g_groups || !g_gi || !g_pg) return 1;

    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = f.QuadPart;
    LARGE_INTEGER q; QueryPerformanceCounter(&q); g_lastQpc = q.QuadPart;
    g_nCpu = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); if (g_nCpu < 1) g_nCpu = 1;

    g_sort    = (int)RegGetDw(L"Sort", SORT_CPU); if (g_sort < 0 || g_sort > 2) g_sort = SORT_CPU;
    g_topmost = RegGetDw(L"TopMost", 0) != 0;
    g_showAll = RegGetDw(L"ShowAll", 0) != 0;
    g_acrylic = RegGetDw(L"Acrylic", 1) != 0;
    if (IsAutostart()) SetAutostart(TRUE);   // refresh path in case the exe was moved

    g_iconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSm;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    InitGpu();
    Sample();
    if (!CreateMainWindow()) return 1;

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!m.hwnd && m.message == WM_APP_RECREATE) {
            Sleep(1500);
            if (!CreateMainWindow()) break;
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_pdhQ) PdhCloseQuery(g_pdhQ);
    GdiplusShutdown(gtoken);
    return 0;
}
