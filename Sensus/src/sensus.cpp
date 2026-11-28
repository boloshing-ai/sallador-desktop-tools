// Sensus 1.2 — desktop widget: hardware sensors (clocks, loads, memory, temperatures, fans, power).
// Windows 11 (Fluent) edition. Developer: Sallador.  Pure Win32 + GDI+, static CRT.
//
// Sources (no driver, no admin rights):
//   • CPU / memory / processes — GetSystemTimes, PDH, GetPerformanceInfo
//   • GPU — D3DKMT adapter / node perf data (temperature, fan, clocks), PDH (load, VRAM), NVML on NVIDIA (power, TDP)
//   • NVMe temperatures — IOCTL_STORAGE_QUERY_PROPERTY; free space; network adapters; battery; uptime
//   • AIDA64 shared memory (when enabled in AIDA64) — CPU / motherboard / SATA temperatures, fans, voltages, power
//
//   Sensus.exe          start (single instance; a second start shows the widget)
//   Sensus.exe /demo    sample data

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
#include <psapi.h>
#include <winioctl.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <d3dkmthk.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "resource.h"
#include "fluent.h"
#include "lang.h"
#include "appearance.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

// ───────────────────────────── constants ─────────────────────────────
#define APP_NAME     L"Sensus"
#define CLASS_NAME   L"SensusWidget"
#define SET_CLASS    L"SensusSettings"
#define REG_KEY      L"Software\\Sallador\\Sensus"
#define REG_SENS     L"Software\\Sallador\\Sensus\\Sensors"
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define WM_TRAY          (WM_APP + 1)
#define WM_APP_SHOW      (WM_APP + 2)
#define WM_APP_RECREATE  (WM_APP + 3)
#define TIMER_ID     1
#define MAX_SENS     320
#define MAX_GPU      4
#define MAX_DISK     16
#define POS_UNSET    0x80000000u

#define BASE_W   340
#define HDR_H    44
#define ROW_H    24
#define GRP_H    28
#define PAD_B    10

enum { ID_TOPMOST = 110, ID_AUTOSTART = 111, ID_SHOWHIDE = 112, ID_EXIT = 113 /*used by installer*/,
       ID_ACRYLIC = 115, ID_SETTINGS = 120, ID_EVERY_1 = 130, ID_EVERY_2, ID_EVERY_5, ID_COLS_AUTO = 135, ID_COLS_1, ID_COLS_2 };
enum { H_NONE = -1, H_MORE = 0, H_COUNT };

// groups, in display order
enum { G_CPU, G_MEM, G_GPU, G_TEMP, G_FAN, G_POWER, G_VOLT, G_DISK, G_NET, G_SYS, G_AIDA, G_COUNT };
// value kinds
enum { K_PCT, K_MHZ, K_MB, K_GB, K_TEMP, K_RPM, K_W, K_V, K_A, K_SPEED, K_COUNTV, K_TIME, K_TEXT };

struct Sensor {
    char id[48]; WCHAR label[72]; int group, kind;
    double val; WCHAR text[32];
    BOOL avail, shown, loaded, defOn;
    int order;
};

// ───────────────────────────── globals ───────────────────────────────
static HINSTANCE g_hInst;
static HWND      g_hwnd, g_set;
static int       g_w, g_h;
static Canvas    g_cv, g_cvSet;
static RECT      g_rc[H_COUNT];
static int       g_hover = H_NONE, g_press = H_NONE;
static BOOL      g_tracking;
static BOOL      g_drag; static POINT g_dragCursor; static RECT g_dragWin;
static HICON     g_iconSm, g_iconBig;
static NOTIFYICONDATAW g_nid;
static UINT      g_msgTaskbarCreated;
static BOOL      g_topmost, g_acrylic = TRUE, g_visible = TRUE, g_exiting, g_demo;
static int       g_every = 1;
static unsigned  g_tick;
static int       g_scroll, g_contentH, g_viewH;

static Sensor*   g_s; static int g_n; static int g_orderNext;
static BOOL      g_aidaOk;

// CPU / PDH
static ULONGLONG g_lastIdle, g_lastKern, g_lastUser; static BOOL g_haveCpu;
static double    g_cpuBaseMhz;
static PDH_HQUERY g_pdhQ; static PDH_HCOUNTER g_cPerf, g_cEng, g_cVram;
static BYTE*     g_pdhBuf; static DWORD g_pdhBufSize;

// GPUs
struct Gpu {
    D3DKMT_HANDLE h; LUID luid; WCHAR name[64]; BOOL perf; ULONGLONG vramTotal;
    void* nv; double load, vramUsed;
};
static Gpu g_gpu[MAX_GPU]; static int g_gpuN;
typedef int (*PFN_nvInit)(void); typedef int (*PFN_nvCount)(unsigned*); typedef int (*PFN_nvHandle)(unsigned, void**);
typedef int (*PFN_nvU32)(void*, unsigned*); typedef int (*PFN_nvTemp)(void*, int, unsigned*);
static PFN_nvU32 nvPower, nvLimit, nvFan;
static int g_nvCount; static void* g_nvDev[MAX_GPU];

// network
static ULONGLONG g_ifIn[64], g_ifOut[64]; static NET_LUID g_ifLuid[64]; static int g_ifN; static ULONGLONG g_lastNetTick;

// ───────────────────────────── helpers ───────────────────────────────
static void* Alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void  Free(void* p)   { if (p) HeapFree(GetProcessHeap(), 0, p); }
static ULONGLONG FT(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static DWORD RegGetDw(const WCHAR* name, DWORD def) { return ReadDw(REG_KEY, name, def); }
static void RegSetDwK(const WCHAR* key, const WCHAR* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}
static void RegSetDw(const WCHAR* name, DWORD v) { RegSetDwK(REG_KEY, name, v); }
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
    } else RegDeleteValueW(k, APP_NAME);
    RegCloseKey(k);
}

// ───────────────────────────── sensor catalog ────────────────────────
static Sensor* Find(const char* id) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_s[i].id, id)) return &g_s[i];
    return NULL;
}
static void LoadShown(Sensor* s) {
    WCHAR wid[48]; MultiByteToWideChar(CP_UTF8, 0, s->id, -1, wid, 48);
    s->shown = ReadDw(REG_SENS, wid, s->defOn) != 0;
    s->loaded = TRUE;
}
static void SaveShown(Sensor* s) {
    WCHAR wid[48]; MultiByteToWideChar(CP_UTF8, 0, s->id, -1, wid, 48);
    RegSetDwK(REG_SENS, wid, s->shown);
}
static Sensor* Put(const char* id, int group, int kind, const WCHAR* label, double val, BOOL defOn) {
    Sensor* s = Find(id);
    if (!s) {
        if (g_n >= MAX_SENS) return NULL;
        s = &g_s[g_n++];
        ZeroMemory(s, sizeof(*s));
        strcpy_s(s->id, id); s->group = group; s->kind = kind; s->defOn = defOn; s->order = g_orderNext++;
        LoadShown(s);
    }
    wcsncpy_s(s->label, label, _TRUNCATE);
    s->val = val; s->avail = TRUE;
    return s;
}

// ───────────────────────────── CPU / memory ──────────────────────────
static void InitCpu() {
    DWORD mhz = 0, sz = sizeof(mhz);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, NULL, &mhz, &sz) == ERROR_SUCCESS)
        g_cpuBaseMhz = mhz;
}
static void InitPdh() {
    if (PdhOpenQueryW(NULL, 0, &g_pdhQ) != ERROR_SUCCESS) { g_pdhQ = NULL; return; }
    if (PdhAddEnglishCounterW(g_pdhQ, L"\\Processor Information(_Total)\\% Processor Performance", 0, &g_cPerf) != ERROR_SUCCESS) g_cPerf = NULL;
    if (PdhAddEnglishCounterW(g_pdhQ, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_cEng) != ERROR_SUCCESS) g_cEng = NULL;
    if (PdhAddEnglishCounterW(g_pdhQ, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_cVram) != ERROR_SUCCESS) g_cVram = NULL;
    PdhCollectQueryData(g_pdhQ);
}
static PPDH_FMT_COUNTERVALUE_ITEM_W PdhArray(PDH_HCOUNTER c, DWORD* count) {
    *count = 0;
    if (!c) return NULL;
    for (int tries = 0; tries < 3; tries++) {
        DWORD size = g_pdhBufSize;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count, (PPDH_FMT_COUNTERVALUE_ITEM_W)g_pdhBuf);
        if (st == (PDH_STATUS)PDH_MORE_DATA) {
            Free(g_pdhBuf); g_pdhBufSize = size + 16384; g_pdhBuf = (BYTE*)Alloc(g_pdhBufSize);
            if (!g_pdhBuf) { g_pdhBufSize = 0; return NULL; }
            continue;
        }
        return st == ERROR_SUCCESS ? (PPDH_FMT_COUNTERVALUE_ITEM_W)g_pdhBuf : NULL;
    }
    return NULL;
}
static void LuidTag(const LUID& l, WCHAR* out, int n) { swprintf_s(out, n, L"luid_0x%08X_0x%08X", (unsigned)l.HighPart, (unsigned)l.LowPart); }

static void SampleCpuMem() {
    FILETIME fi, fk, fu;
    if (GetSystemTimes(&fi, &fk, &fu)) {
        ULONGLONG i = FT(fi), k = FT(fk), u = FT(fu);
        if (!g_haveCpu) Put("cpu.load", G_CPU, K_PCT, TR(L"Загрузка ЦП", L"CPU load"), 0, TRUE);   // fixes the row order
        if (g_haveCpu) {
            ULONGLONG di = i - g_lastIdle, dt = (k - g_lastKern) + (u - g_lastUser);
            double load = dt ? (double)(dt - di) * 100.0 / (double)dt : 0;
            if (load < 0) load = 0; if (load > 100) load = 100;
            Put("cpu.load", G_CPU, K_PCT, TR(L"Загрузка ЦП", L"CPU load"), load, TRUE);
        }
        g_lastIdle = i; g_lastKern = k; g_lastUser = u; g_haveCpu = TRUE;
    }
    if (g_pdhQ && g_cPerf && g_cpuBaseMhz > 0) {
        PDH_FMT_COUNTERVALUE v;
        if (PdhGetFormattedCounterValue(g_cPerf, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, NULL, &v) == ERROR_SUCCESS && v.CStatus == ERROR_SUCCESS)
            Put("cpu.clock", G_CPU, K_MHZ, TR(L"Частота ЦП", L"CPU clock"), g_cpuBaseMhz * v.doubleValue / 100.0, TRUE);
    }
    PERFORMANCE_INFORMATION pi = { sizeof(pi) };
    if (GetPerformanceInfo(&pi, sizeof(pi))) {
        double pg = (double)pi.PageSize / 1048576.0;
        Put("cpu.procs", G_CPU, K_COUNTV, TR(L"Процессов", L"Processes"), pi.ProcessCount, FALSE);
        Put("cpu.threads", G_CPU, K_COUNTV, TR(L"Потоков", L"Threads"), pi.ThreadCount, FALSE);
        double total = pi.PhysicalTotal * pg, avail = pi.PhysicalAvailable * pg;
        Put("mem.load", G_MEM, K_PCT, TR(L"Загрузка памяти", L"Memory load"), total > 0 ? (total - avail) * 100.0 / total : 0, TRUE);
        Put("mem.used", G_MEM, K_MB, TR(L"Занято памяти", L"Memory used"), total - avail, TRUE);
        Put("mem.free", G_MEM, K_MB, TR(L"Свободно памяти", L"Memory free"), avail, TRUE);
        Put("mem.commit", G_MEM, K_MB, TR(L"Занято виртуальной памяти", L"Virtual memory used"), pi.CommitTotal * pg, TRUE);
        Put("mem.commitpct", G_MEM, K_PCT, TR(L"Загрузка виртуальной памяти", L"Virtual memory load"),
            pi.CommitLimit ? pi.CommitTotal * 100.0 / pi.CommitLimit : 0, FALSE);
        Put("mem.cache", G_MEM, K_MB, TR(L"Системный кэш", L"System cache"), pi.SystemCache * pg, FALSE);
    }
    Put("sys.uptime", G_SYS, K_TIME, TR(L"Время работы", L"Uptime"), (double)(GetTickCount64() / 1000), TRUE);
}

// ───────────────────────────── GPU ───────────────────────────────────
static void InitGpu() {
    D3DKMT_ENUMADAPTERS2 ea = {};
    if (D3DKMTEnumAdapters2(&ea) != 0 || !ea.NumAdapters) return;
    D3DKMT_ADAPTERINFO* ai = (D3DKMT_ADAPTERINFO*)Alloc(sizeof(D3DKMT_ADAPTERINFO) * ea.NumAdapters);
    if (!ai) return;
    ea.pAdapters = ai;
    if (D3DKMTEnumAdapters2(&ea) == 0) {
        IDXGIFactory1* f = NULL; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f);
        for (UINT i = 0; i < ea.NumAdapters && g_gpuN < MAX_GPU; i++) {
            Gpu* gp = &g_gpu[g_gpuN];
            ZeroMemory(gp, sizeof(*gp));
            D3DKMT_QUERYADAPTERINFO q = {}; q.hAdapter = ai[i].hAdapter;
            D3DKMT_ADAPTERREGISTRYINFO ri = {};
            q.Type = KMTQAITYPE_ADAPTERREGISTRYINFO; q.pPrivateDriverData = &ri; q.PrivateDriverDataSize = sizeof(ri);
            if (D3DKMTQueryAdapterInfo(&q) != 0 || !ri.AdapterString[0]) continue;
            if (wcsstr(ri.AdapterString, L"Basic Render") || wcsstr(ri.AdapterString, L"Basic Display")) continue;
            D3DKMT_ADAPTER_PERFDATA pd = {};
            q.Type = KMTQAITYPE_ADAPTERPERFDATA; q.pPrivateDriverData = &pd; q.PrivateDriverDataSize = sizeof(pd);
            gp->perf = D3DKMTQueryAdapterInfo(&q) == 0;
            gp->h = ai[i].hAdapter; gp->luid = ai[i].AdapterLuid;
            wcsncpy_s(gp->name, ri.AdapterString, _TRUNCATE);
            if (f) {
                IDXGIAdapter1* a = NULL;
                for (UINT k = 0; f->EnumAdapters1(k, &a) != DXGI_ERROR_NOT_FOUND; k++) {
                    DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
                    if (d.AdapterLuid.LowPart == gp->luid.LowPart && d.AdapterLuid.HighPart == gp->luid.HighPart) gp->vramTotal = d.DedicatedVideoMemory;
                    a->Release();
                }
            }
            g_gpuN++;
        }
        if (f) f->Release();
    }
    Free(ai);

    HMODULE nv = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nv) nv = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (nv) {
        PFN_nvInit init = (PFN_nvInit)GetProcAddress(nv, "nvmlInit_v2");
        PFN_nvCount cnt = (PFN_nvCount)GetProcAddress(nv, "nvmlDeviceGetCount_v2");
        PFN_nvHandle hd = (PFN_nvHandle)GetProcAddress(nv, "nvmlDeviceGetHandleByIndex_v2");
        nvPower = (PFN_nvU32)GetProcAddress(nv, "nvmlDeviceGetPowerUsage");
        nvLimit = (PFN_nvU32)GetProcAddress(nv, "nvmlDeviceGetEnforcedPowerLimit");
        nvFan   = (PFN_nvU32)GetProcAddress(nv, "nvmlDeviceGetFanSpeed");
        unsigned n = 0;
        if (init && cnt && hd && init() == 0 && cnt(&n) == 0)
            for (unsigned i = 0; i < n && i < MAX_GPU; i++) if (hd(i, &g_nvDev[i]) == 0) g_nvCount++;
        int k = 0;                                           // NVML devices ↔ NVIDIA adapters, in order
        for (int i = 0; i < g_gpuN; i++) if (wcsstr(g_gpu[i].name, L"NVIDIA") && k < g_nvCount) g_gpu[i].nv = g_nvDev[k++];
    }
}
static void SampleGpu() {
    if (!g_gpuN) return;
    for (int i = 0; i < g_gpuN; i++) { g_gpu[i].load = 0; g_gpu[i].vramUsed = 0; }
    WCHAR tag[48];
    DWORD n = 0; PPDH_FMT_COUNTERVALUE_ITEM_W it = PdhArray(g_cEng, &n);
    if (it) {
        // per adapter: max over engines of (sum over processes of that engine)
        struct E { int g; WCHAR eng[48]; double v; };
        static E e[512]; int en = 0;
        for (DWORD k = 0; k < n; k++) {
            if (it[k].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[k].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            const WCHAR* nm = it[k].szName; const WCHAR* l = wcsstr(nm, L"luid_"); if (!l) continue;
            int gi = -1;
            for (int g = 0; g < g_gpuN; g++) { LuidTag(g_gpu[g].luid, tag, 48); if (!_wcsnicmp(l, tag, wcslen(tag))) { gi = g; break; } }
            if (gi < 0) continue;
            int j = 0;
            for (; j < en; j++) if (e[j].g == gi && !wcscmp(e[j].eng, l)) break;
            if (j == en) { if (en >= 512) continue; e[en].g = gi; wcsncpy_s(e[en].eng, l, _TRUNCATE); e[en].v = 0; en++; }
            e[j].v += it[k].FmtValue.doubleValue;
        }
        for (int j = 0; j < en; j++) if (e[j].v > g_gpu[e[j].g].load) g_gpu[e[j].g].load = e[j].v;
    }
    it = PdhArray(g_cVram, &n);
    if (it)
        for (DWORD k = 0; k < n; k++)
            for (int g = 0; g < g_gpuN; g++) {
                LuidTag(g_gpu[g].luid, tag, 48);
                if (!_wcsnicmp(it[k].szName, tag, wcslen(tag))) g_gpu[g].vramUsed += it[k].FmtValue.doubleValue;
            }

    for (int i = 0; i < g_gpuN; i++) {
        Gpu* gp = &g_gpu[i];
        char id[48]; WCHAR lb[72], suf[8] = L"";
        if (g_gpuN > 1) swprintf_s(suf, L" %d", i + 1);
        #define GID(x) (sprintf_s(id, "gpu%d." x, i), id)
        #define GLB(ru, en) (swprintf_s(lb, L"%s%s", TR(ru, en), suf), lb)
        Put(GID("load"), G_GPU, K_PCT, GLB(L"Загрузка ГП", L"GPU load"), gp->load > 100 ? 100 : gp->load, TRUE);
        if (gp->perf) {
            D3DKMT_QUERYADAPTERINFO q = {}; q.hAdapter = gp->h;
            D3DKMT_NODE_PERFDATA nd = {};
            q.Type = KMTQAITYPE_NODEPERFDATA; q.pPrivateDriverData = &nd; q.PrivateDriverDataSize = sizeof(nd);
            if (D3DKMTQueryAdapterInfo(&q) == 0 && nd.Frequency)
                Put(GID("clock"), G_GPU, K_MHZ, GLB(L"Частота ГП", L"GPU clock"), nd.Frequency / 1e6, TRUE);
            D3DKMT_ADAPTER_PERFDATA pd = {};
            q.Type = KMTQAITYPE_ADAPTERPERFDATA; q.pPrivateDriverData = &pd; q.PrivateDriverDataSize = sizeof(pd);
            if (D3DKMTQueryAdapterInfo(&q) == 0) {
                if (pd.Temperature) Put(GID("temp"), G_GPU, K_TEMP, GLB(L"Температура ГП", L"GPU temperature"), pd.Temperature / 10.0, TRUE);
                Put(GID("fan"), G_GPU, K_RPM, GLB(L"Вентилятор ГП", L"GPU fan"), pd.FanRPM, TRUE);
                if (pd.MemoryFrequency) Put(GID("memclk"), G_GPU, K_MHZ, GLB(L"Частота видеопамяти", L"GPU memory clock"), pd.MemoryFrequency / 1e6, FALSE);
            }

        }
        if (gp->vramTotal) Put(GID("vramload"), G_GPU, K_PCT, GLB(L"Загрузка видеопамяти", L"GPU memory load"), gp->vramUsed * 100.0 / gp->vramTotal, TRUE);
        Put(GID("vram"), G_GPU, K_MB, GLB(L"Занято видеопамяти", L"GPU memory used"), gp->vramUsed / 1048576.0, TRUE);
        if (gp->nv) {
            unsigned p = 0, lim = 0, fan = 0;
            if (nvPower && nvPower(gp->nv, &p) == 0) {
                Put(GID("power"), G_GPU, K_W, GLB(L"Мощность ГП", L"GPU power"), p / 1000.0, TRUE);
                if (nvLimit && nvLimit(gp->nv, &lim) == 0 && lim) Put(GID("tdp"), G_GPU, K_PCT, GLB(L"ГП TDP", L"GPU TDP"), p * 100.0 / lim, TRUE);
            }
            if (nvFan && nvFan(gp->nv, &fan) == 0) Put(GID("fanpct"), G_GPU, K_PCT, GLB(L"Вентилятор ГП, %", L"GPU fan, %"), fan, FALSE);
        }
        #undef GID
        #undef GLB
    }
}

// ───────────────────────────── disks / volumes ───────────────────────
static void SampleDisks() {
    char modelSeen[MAX_DISK][64]; int seen = 0;
    for (int i = 0; i < MAX_DISK; i++) {
        WCHAR path[32]; swprintf_s(path, L"\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_PROPERTY_QUERY q = {}; q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
        BYTE buf[2048] = {}; DWORD rd = 0; char model[64] = "";
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &rd, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR* d = (STORAGE_DEVICE_DESCRIPTOR*)buf;
            if (d->ProductIdOffset && d->ProductIdOffset < rd) strncpy_s(model, (char*)buf + d->ProductIdOffset, _TRUNCATE);
            for (int k = (int)strlen(model) - 1; k >= 0 && model[k] == ' '; k--) model[k] = 0;
        }
        q.PropertyId = StorageDeviceTemperatureProperty; ZeroMemory(buf, sizeof(buf));
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &rd, NULL)) {
            STORAGE_TEMPERATURE_DATA_DESCRIPTOR* t = (STORAGE_TEMPERATURE_DATA_DESCRIPTOR*)buf;
            if (t->InfoCount && t->TemperatureInfo[0].Temperature > 0) {
                int dup = 1;
                for (int k = 0; k < seen; k++) if (!strcmp(modelSeen[k], model)) dup++;
                if (seen < MAX_DISK) strcpy_s(modelSeen[seen++], model);
                char id[48]; sprintf_s(id, "disk%d.temp", i);
                WCHAR lb[72], wm[64]; MultiByteToWideChar(CP_ACP, 0, model[0] ? model : "Disk", -1, wm, 64);
                if (dup > 1) swprintf_s(lb, L"%s #%d", wm, dup); else wcscpy_s(lb, wm);
                Put(id, G_DISK, K_TEMP, lb, t->TemperatureInfo[0].Temperature, !g_aidaOk);
            }
        }
        CloseHandle(h);
    }
}
static void SampleVolumes() {
    WCHAR sysDrive = L'C'; WCHAR wd[MAX_PATH]; if (GetWindowsDirectoryW(wd, MAX_PATH)) sysDrive = wd[0];
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        WCHAR root[4] = { (WCHAR)(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER fr, tot;
        if (!GetDiskFreeSpaceExW(root, &fr, &tot, NULL)) continue;
        char id[16]; sprintf_s(id, "vol.%c", 'A' + i);
        WCHAR lb[72]; swprintf_s(lb, TR(L"Свободно на %c:", L"Free on %c:"), L'A' + i);
        Put(id, G_DISK, K_GB, lb, fr.QuadPart / 1073741824.0, root[0] == sysDrive);
    }
}

// ───────────────────────────── network ───────────────────────────────
static void SampleNet() {
    PMIB_IF_TABLE2 t = NULL;
    if (GetIfTable2(&t) != NO_ERROR || !t) return;
    ULONGLONG now = GetTickCount64();
    double dt = g_lastNetTick ? (now - g_lastNetTick) / 1000.0 : 0;
    g_lastNetTick = now;
    for (ULONG i = 0; i < t->NumEntries; i++) {
        MIB_IF_ROW2* r = &t->Table[i];
        if (r->Type == IF_TYPE_SOFTWARE_LOOPBACK || r->OperStatus != IfOperStatusUp) continue;
        if (!r->InterfaceAndOperStatusFlags.HardwareInterface || r->InterfaceAndOperStatusFlags.FilterInterface) continue;
        int k = 0;
        for (; k < g_ifN; k++) if (g_ifLuid[k].Value == r->InterfaceLuid.Value) break;
        if (k == g_ifN) { if (g_ifN >= 64) continue; g_ifLuid[g_ifN] = r->InterfaceLuid; g_ifIn[g_ifN] = r->InOctets; g_ifOut[g_ifN] = r->OutOctets; g_ifN++; continue; }
        double din = 0, dout = 0;
        if (dt > 0 && r->InOctets >= g_ifIn[k] && r->OutOctets >= g_ifOut[k]) { din = (r->InOctets - g_ifIn[k]) / dt; dout = (r->OutOctets - g_ifOut[k]) / dt; }
        g_ifIn[k] = r->InOctets; g_ifOut[k] = r->OutOctets;
        char id[48]; WCHAR lb[72];
        sprintf_s(id, "net.%llx.down", (unsigned long long)r->InterfaceLuid.Value);
        swprintf_s(lb, TR(L"Загрузка · %s", L"Download · %s"), r->Alias);
        Put(id, G_NET, K_SPEED, lb, din, TRUE);
        sprintf_s(id, "net.%llx.up", (unsigned long long)r->InterfaceLuid.Value);
        swprintf_s(lb, TR(L"Отдача · %s", L"Upload · %s"), r->Alias);
        Put(id, G_NET, K_SPEED, lb, dout, TRUE);
    }
    FreeMibTable(t);
}
static void SampleBattery() {
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps) || ps.BatteryFlag == 128 || ps.BatteryLifePercent > 100) return;
    Put("bat.pct", G_SYS, K_PCT, TR(L"Заряд батареи", L"Battery"), ps.BatteryLifePercent, TRUE);
}

// ───────────────────────────── AIDA64 shared memory ──────────────────
// "<temp><id>TCPU</id><label>CPU</label><value>42</value></temp><fan>…"  (ANSI)
static BOOL Tag(const char*& p, const char* name, char* out, int n) {
    char open[24], close[24]; sprintf_s(open, "<%s>", name); sprintf_s(close, "</%s>", name);
    const char* a = strstr(p, open); if (!a) return FALSE;
    a += strlen(open);
    const char* b = strstr(a, close); if (!b) return FALSE;
    int len = (int)(b - a); if (len >= n) len = n - 1;
    memcpy(out, a, len); out[len] = 0;
    p = b + strlen(close);
    return TRUE;
}
// AIDA64 shared memory always uses English labels; give the common ones Russian names.
static void LocalizeAida(const WCHAR* in, WCHAR* out, int n) {
    wcsncpy_s(out, n, in, _TRUNCATE);
    if (g_en) return;
    static const WCHAR* ex[][2] = {
        { L"Motherboard", L"Системная плата" }, { L"CPU", L"ЦП" }, { L"CPU Package", L"Корпус ЦП" },
        { L"CPU IA Cores", L"Ядра ЦП (IA)" }, { L"CPU GT Cores", L"Графические ядра ЦП" }, { L"PCH Diode", L"Диод PCH" },
        { L"GPU", L"Графический процессор" }, { L"GPU2", L"Графический процессор 2" }, { L"GPU Hotspot", L"Горячая точка ГП" },
        { L"GPU Memory", L"Память ГП" }, { L"GPU Core", L"Ядро ГП" }, { L"GPU TDP%", L"ГП TDP" }, { L"CPU Core", L"Ядро ЦП" },
        { L"VBAT Battery", L"Батарейка VBAT" }, { L"+3.3 V Standby", L"+3.3 В дежурное" }, { L"North Bridge", L"Северный мост" },
        { L"System", L"Система" }, { L"Chipset", L"Чипсет" }, { L"Water Pump", L"Помпа" }, { L"Power Supply", L"Блок питания" } };
    for (int i = 0; i < (int)(sizeof(ex) / sizeof(ex[0])); i++) if (!wcscmp(in, ex[i][0])) { wcsncpy_s(out, n, ex[i][1], _TRUNCATE); return; }
    static const WCHAR* px[][2] = {
        { L"CPU Core #", L"Ядро ЦП #" }, { L"Chassis #", L"Шасси #" }, { L"Chassis", L"Шасси" },
        { L"GPU 8-pin", L"ГП 8-pin" }, { L"GPU PCIe", L"ГП PCIe" }, { L"GPU ", L"ГП " }, { L"CPU ", L"ЦП " } };
    for (int i = 0; i < (int)(sizeof(px) / sizeof(px[0])); i++) {
        size_t k = wcslen(px[i][0]);
        if (!wcsncmp(in, px[i][0], k)) { swprintf_s(out, n, L"%s%s", px[i][1], in + k); return; }
    }
}
// Defaults: show what an AIDA64 sensor panel usually shows; hide what Sensus already reads itself.
static BOOL AidaDefault(const char* tag, const char* id) {
    if (!strcmp(tag, "temp")) return !strcmp(id, "TMOBO") || !strcmp(id, "TCPU") || !strcmp(id, "TGPU1HOT") || !strcmp(id, "TGPU1MEM") || !strncmp(id, "THDD", 4);
    if (!strcmp(tag, "fan"))  return strncmp(id, "FGPU", 4) != 0;
    if (!strcmp(tag, "pwr"))  return !strcmp(id, "PCPUPKG");
    return FALSE;
}
static void SampleAida() {
    g_aidaOk = FALSE;
    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, L"AIDA64_SensorValues");
    if (!m) return;
    const char* v = (const char*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (v) {
        MEMORY_BASIC_INFORMATION mbi; VirtualQuery(v, &mbi, sizeof(mbi));
        size_t cap = mbi.RegionSize;
        char* txt = (char*)Alloc(cap + 1);
        if (txt) {
            memcpy(txt, v, cap); txt[cap] = 0;
            const char* p = txt;
            while (*p) {
                const char* lt = strchr(p, '<'); if (!lt) break;
                char tag[16]; int k = 0; const char* q = lt + 1;
                while (*q && *q != '>' && k < 15) tag[k++] = *q++;
                tag[k] = 0; if (*q != '>') break;
                const char* body = q + 1;
                char closeT[24]; sprintf_s(closeT, "</%s>", tag);
                const char* end = strstr(body, closeT); if (!end) break;
                char item[512]; int il = (int)(end - body); if (il > 511) il = 511;
                memcpy(item, body, il); item[il] = 0;
                p = end + strlen(closeT);
                char idv[40], lab[128], val[48]; const char* ip = item;
                if (!Tag(ip, "id", idv, 40) || !Tag(ip, "label", lab, 128) || !Tag(ip, "value", val, 48)) continue;
                int group = G_AIDA, kind = K_TEXT; BOOL on = FALSE;
                if (!strcmp(tag, "temp"))      { group = G_TEMP;  kind = K_TEMP; }
                else if (!strcmp(tag, "fan"))  { group = G_FAN;   kind = K_RPM; }
                else if (!strcmp(tag, "duty")) { group = G_FAN;   kind = K_PCT; }
                else if (!strcmp(tag, "pwr"))  { group = G_POWER; kind = K_W; }
                else if (!strcmp(tag, "volt")) { group = G_VOLT;  kind = K_V; }
                else if (!strcmp(tag, "curr")) { group = G_VOLT;  kind = K_A; }
                on = AidaDefault(tag, idv);
                if (!strncmp(idv, "THDD", 4)) group = G_DISK;      // drive temperatures next to the drives
                if (!strcmp(tag, "pwr") && strstr(idv, "TDPP")) kind = K_PCT;
                static const char* skip[] = { "SDATE", "SYEAR", "SMONTH", "SDAYOFMONTH", "SDOW", "SWEEKOFYEAR", "STIME", "SHOUR", "SMIN", "SSEC", "SUPTIME" };
                BOOL junk = FALSE;
                for (int k2 = 0; k2 < (int)(sizeof(skip) / sizeof(skip[0])); k2++) if (!strncmp(idv, skip[k2], strlen(skip[k2]))) junk = TRUE;
                if (junk) continue;
                char sid[48]; sprintf_s(sid, "aida.%s", idv);
                WCHAR raw[72], wl[72]; MultiByteToWideChar(CP_ACP, 0, lab, -1, raw, 72); raw[71] = 0; LocalizeAida(raw, wl, 72);
                Sensor* s = Put(sid, group, kind, wl, atof(val), on);
                if (s && kind == K_TEXT) MultiByteToWideChar(CP_ACP, 0, val, -1, s->text, 32), s->text[31] = 0;
                g_aidaOk = TRUE;
            }
            Free(txt);
        }
        UnmapViewOfFile(v);
    }
    CloseHandle(m);
}

// ───────────────────────────── demo ──────────────────────────────────
static void DemoData() {
    struct D { const char* id; int g, k; const WCHAR *ru, *en; double v; };
    static const D d[] = {
        { "cpu.clock", G_CPU, K_MHZ, L"Частота ЦП", L"CPU clock", 4200 },
        { "cpu.load", G_CPU, K_PCT, L"Загрузка ЦП", L"CPU load", 51 },
        { "mem.load", G_MEM, K_PCT, L"Загрузка памяти", L"Memory load", 28 },
        { "mem.used", G_MEM, K_MB, L"Занято памяти", L"Memory used", 9112 },
        { "mem.free", G_MEM, K_MB, L"Свободно памяти", L"Memory free", 23478 },
        { "mem.commit", G_MEM, K_MB, L"Занято виртуальной памяти", L"Virtual memory used", 13077 },
        { "gpu0.clock", G_GPU, K_MHZ, L"Частота ГП", L"GPU clock", 1020 },
        { "gpu0.load", G_GPU, K_PCT, L"Загрузка ГП", L"GPU load", 21 },
        { "gpu0.vramload", G_GPU, K_PCT, L"Загрузка видеопамяти", L"GPU memory load", 21 },
        { "gpu0.vram", G_GPU, K_MB, L"Занято видеопамяти", L"GPU memory used", 2183 },
        { "gpu0.temp", G_GPU, K_TEMP, L"Температура ГП", L"GPU temperature", 44 },
        { "gpu0.fan", G_GPU, K_RPM, L"Вентилятор ГП", L"GPU fan", 1463 },
        { "gpu0.power", G_GPU, K_W, L"Мощность ГП", L"GPU power", 98.5 },
        { "gpu0.tdp", G_GPU, K_PCT, L"ГП TDP", L"GPU TDP", 31 },
        { "aida.TMOBO", G_TEMP, K_TEMP, L"Системная плата", L"Motherboard", 32 },
        { "aida.TCPU", G_TEMP, K_TEMP, L"ЦП", L"CPU", 42 },
        { "aida.TGPU1HOT", G_TEMP, K_TEMP, L"Горячая точка ГП", L"GPU hotspot", 61 },
        { "aida.TGPU1MEM", G_TEMP, K_TEMP, L"Память ГП", L"GPU memory", 58 },
        { "aida.FCPU", G_FAN, K_RPM, L"ЦП", L"CPU", 849 },
        { "aida.FCHA2", G_FAN, K_RPM, L"Шасси 2", L"Chassis 2", 1073 },
        { "aida.FPUMP", G_FAN, K_RPM, L"AIO Pump", L"AIO Pump", 2089 },
        { "aida.PCPUPKG", G_POWER, K_W, L"ЦП", L"CPU", 71.74 },
        { "disk3.temp", G_DISK, K_TEMP, L"Lexar SSD NM620 512GB", L"Lexar SSD NM620 512GB", 37 },
        { "disk4.temp", G_DISK, K_TEMP, L"Lexar SSD NM620 512GB #2", L"Lexar SSD NM620 512GB #2", 40 },
        { "vol.C", G_DISK, K_GB, L"Свободно на C:", L"Free on C:", 212.4 },
        { "net.1.down", G_NET, K_SPEED, L"Загрузка · Ethernet 2", L"Download · Ethernet 2", 3.3 * 1024 },
        { "net.1.up", G_NET, K_SPEED, L"Отдача · Ethernet 2", L"Upload · Ethernet 2", 0.8 * 1024 },
        { "sys.uptime", G_SYS, K_TIME, L"Время работы", L"Uptime", 3 * 3600 + 25 * 60 },
    };
    for (int i = 0; i < (int)(sizeof(d) / sizeof(d[0])); i++) {
        Sensor* s = Put(d[i].id, d[i].g, d[i].k, TR(d[i].ru, d[i].en), d[i].v, TRUE);
        if (s && !s->loaded) LoadShown(s);
    }
    g_aidaOk = TRUE;
}

static void Sample() {
    if (g_demo) return;
    for (int i = 0; i < g_n; i++) g_s[i].avail = FALSE;
    if (g_pdhQ) PdhCollectQueryData(g_pdhQ);
    SampleAida();                       // first: native duplicates default to off when AIDA64 is there
    SampleCpuMem();
    SampleGpu();
    SampleNet();
    SampleBattery();
    static ULONGLONG lastDisk, lastVol;
    ULONGLONG now = GetTickCount64();
    static BOOL diskInit;
    if (!diskInit || now - lastDisk >= 5000) { lastDisk = now; diskInit = TRUE; SampleDisks(); }
    else for (int i = 0; i < g_n; i++) if (!strncmp(g_s[i].id, "disk", 4)) g_s[i].avail = TRUE;   // keep last values
    if (!lastVol || now - lastVol >= 30000) { lastVol = now; SampleVolumes(); }
    else for (int i = 0; i < g_n; i++) if (!strncmp(g_s[i].id, "vol.", 4)) g_s[i].avail = TRUE;
}

// ───────────────────────────── formatting ────────────────────────────
static void Comma(WCHAR* s) { if (g_en) return; for (; *s; s++) if (*s == L'.') *s = L','; }
static void Format(const Sensor* s, WCHAR* b, int n) {
    double v = s->val;
    switch (s->kind) {
    case K_PCT:    swprintf_s(b, n, L"%.0f%%", v); break;
    case K_MHZ:    swprintf_s(b, n, TR(L"%.0f МГц", L"%.0f MHz"), v); break;
    case K_MB:     swprintf_s(b, n, TR(L"%.0f МБ", L"%.0f MB"), v); break;
    case K_GB:     swprintf_s(b, n, TR(L"%.1f ГБ", L"%.1f GB"), v); break;
    case K_TEMP:   swprintf_s(b, n, L"%.0f°C", v); break;
    case K_RPM:    swprintf_s(b, n, L"%.0f RPM", v); break;
    case K_W:      swprintf_s(b, n, TR(L"%.1f Вт", L"%.1f W"), v); break;
    case K_V:      swprintf_s(b, n, TR(L"%.3f В", L"%.3f V"), v); break;
    case K_A:      swprintf_s(b, n, TR(L"%.2f А", L"%.2f A"), v); break;
    case K_COUNTV: swprintf_s(b, n, L"%.0f", v); break;
    case K_SPEED:
        if (v < 1000 * 1024) swprintf_s(b, n, TR(L"%.1f КБ/с", L"%.1f KB/s"), v / 1024);
        else swprintf_s(b, n, TR(L"%.1f МБ/с", L"%.1f MB/s"), v / 1048576);
        break;
    case K_TIME: {
        unsigned long long t = (unsigned long long)v;
        unsigned d = (unsigned)(t / 86400), h = (unsigned)(t / 3600 % 24), m = (unsigned)(t / 60 % 60);
        if (d) swprintf_s(b, n, TR(L"%u д %u ч %02u мин", L"%u d %u h %02u min"), d, h, m);
        else swprintf_s(b, n, TR(L"%u ч %02u мин", L"%u h %02u min"), h, m);
        return;
    }
    default: wcsncpy_s(b, n, s->text, _TRUNCATE); return;
    }
    Comma(b);
}
static Color ValueColor(const Sensor* s) {
    if (s->kind == K_TEMP) return s->val >= 85 ? T.critical : s->val >= 70 ? T.caution : T.text;
    if (s->kind == K_PCT && s->group != G_FAN) return s->val >= 95 ? T.critical : T.text;
    return T.text;
}
static const WCHAR* GroupName(int g) {
    switch (g) {
    case G_CPU:   return TR(L"Процессор", L"Processor");
    case G_MEM:   return TR(L"Память", L"Memory");
    case G_GPU:   return TR(L"Видеокарта", L"Graphics");
    case G_TEMP:  return TR(L"Температуры", L"Temperatures");
    case G_FAN:   return TR(L"Вентиляторы", L"Fans");
    case G_POWER: return TR(L"Мощность", L"Power");
    case G_VOLT:  return TR(L"Напряжения и токи", L"Voltages and currents");
    case G_DISK:  return TR(L"Накопители", L"Drives");
    case G_NET:   return TR(L"Сеть", L"Network");
    case G_SYS:   return TR(L"Система", L"System");
    }
    return TR(L"AIDA64 · система", L"AIDA64 · system");
}

// ───────────────────────────── widget rendering ──────────────────────
// Layout: one or two columns of groups. Groups are never split; the split point keeps the two
// columns as even as possible. "Auto" uses two columns only when one would not fit on the screen.
#define COL_W   308
#define COL_GAP 24
static int g_colMode = 1;          // 0 = auto, 1 = one column, 2 = two columns
static int g_cols = 1, g_split = G_COUNT;

static int GroupH(int g) {
    int rows = 0;
    for (int i = 0; i < g_n; i++) if (g_s[i].group == g && g_s[i].avail && g_s[i].shown) rows++;
    return rows ? GRP_H + rows * ROW_H : 0;
}
static int ColumnsHeight(int cols, int* split) {
    int tot = 0, h[G_COUNT];
    for (int g = 0; g < G_COUNT; g++) { h[g] = GroupH(g); tot += h[g]; }
    *split = G_COUNT;
    if (cols == 1 || !tot) return tot ? tot : ROW_H * 2;
    int best = 0x7FFFFFFF, a = 0;
    for (int s = 0; s <= G_COUNT; s++) {
        int m = a > tot - a ? a : tot - a;
        if (m < best) { best = m; *split = s; }
        if (s < G_COUNT) a += h[s];
    }
    return best;
}
static int WinW(int cols) { return cols == 2 ? 16 + COL_W * 2 + COL_GAP + 16 : BASE_W; }
static void SetR(int id, int x, int y, int w, int h) { g_rc[id] = { S(x), S(y), S(x + w), S(y + h) }; }
static void Layout() {
    MONITORINFO mi = { sizeof(mi) };
    HMONITOR mon = g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    int top = mi.rcWork.top + 24;
    if (g_hwnd) { RECT wr; GetWindowRect(g_hwnd, &wr); if (wr.top > top) top = wr.top; }
    int maxH = MulDiv(mi.rcWork.bottom - top - 12, 96, (int)g_dpi);     // never run past the bottom of the screen
    if (maxH < 200) maxH = 200;

    int split1, split2;
    int h1 = ColumnsHeight(1, &split1), h2 = ColumnsHeight(2, &split2);
    g_cols = g_colMode == 2 ? 2 : g_colMode == 1 ? 1 : (HDR_H + h1 + PAD_B > maxH ? 2 : 1);
    g_contentH = g_cols == 2 ? h2 : h1;
    g_split = g_cols == 2 ? split2 : G_COUNT;

    SetR(H_MORE, WinW(g_cols) - 42, 8, 30, 30);
    int want = HDR_H + g_contentH + PAD_B;
    if (want > maxH) want = maxH;
    g_viewH = want - HDR_H - PAD_B;
    if (g_scroll > g_contentH - g_viewH) g_scroll = g_contentH - g_viewH;
    if (g_scroll < 0) g_scroll = 0;
    g_w = S(WinW(g_cols)); g_h = S(want);
}
static int HitTest(POINT p) { for (int i = 0; i < H_COUNT; i++) if (PtInRect(&g_rc[i], p)) return i; return H_NONE; }
static BOOL Hv(int id) { return g_hover == id && (g_press == H_NONE || g_press == id); }
static BOOL Pr(int id) { return g_press == id && g_hover == id; }

static void PaintTo(Graphics& g) {
    const int W = WinW(g_cols);
    WCHAR buf[64];
    DrawAppMark(g, D(14), D(14), D(16));
    FText(g, L"Sensus", g_fStrong, T.text, RectF(D(38), 0, D(120), D(HDR_H)));
    REAL tw = FTextW(g, L"Sensus", g_fStrong);
    RectF mr = RF(g_rc[H_MORE]);
    if (Hv(H_MORE)) FillRR(g, mr, D(4), Pr(H_MORE) ? T.subtlePress : T.subtleHover);
    FGlyph(g, GL_MORE, D(14), T.text2, mr);

    g.SetClip(RectF(0, D(HDR_H), D(W), D(g_viewH)));
    BOOL any = FALSE;
    for (int c = 0; c < g_cols; c++) {
        int g0 = c == 0 ? 0 : g_split, g1 = c == 0 ? g_split : G_COUNT;
        REAL x = D(16 + c * (COL_W + COL_GAP)), cw = g_cols == 2 ? D(COL_W) : D(W - 32);
        REAL y = D(HDR_H - g_scroll);
        BOOL first = TRUE;
        if (c == 1) FFillRect(g, x - D(COL_GAP / 2), D(HDR_H + 6), 1.0f, D(g_contentH < g_viewH ? g_contentH : g_viewH) - D(6), T.divider);
        for (int gi = g0; gi < g1; gi++) {
            if (!GroupH(gi)) continue;
            any = TRUE;
            if (!first) FFillRect(g, x, y + D(4), cw, 1.0f, T.divider);
            first = FALSE;
            FText(g, GroupName(gi), g_fCaption, T.accent, RectF(x, y + D(6), cw, D(GRP_H - 6)));
            y += D(GRP_H);
            for (int o = 0; o < g_orderNext; o++)                // keep catalog order
                for (int i = 0; i < g_n; i++) {
                    Sensor* s = &g_s[i];
                    if (s->order != o || s->group != gi || !s->avail || !s->shown) continue;
                    Format(s, buf, 64);
                    REAL vw = FTextW(g, buf, g_fStrong);
                    FText(g, s->label, g_fBody, T.text2, RectF(x, y, cw - vw - D(12), D(ROW_H)), AL_LEFT, StringTrimmingEllipsisCharacter);
                    FText(g, buf, g_fStrong, ValueColor(s), RectF(x, y, cw, D(ROW_H)), AL_RIGHT);
                    y += D(ROW_H);
                }
        }
    }
    if (!any) FText(g, TR(L"Выберите параметры в меню «…»", L"Choose sensors in the \"…\" menu"), g_fBody, T.text3, RectF(0, D(HDR_H), D(W), D(ROW_H * 2)), AL_CENTER);
    g.ResetClip();
    if (g_contentH > g_viewH) {                       // scroll indicator
        REAL track = D(g_viewH), th = track * g_viewH / g_contentH, ty = D(HDR_H) + track * g_scroll / g_contentH;
        FillRR(g, RectF(D(W - 8), ty + D(2), D(4), th - D(4)), D(2), T.text3);
    }
}
static void Render(HWND h, HDC hdc, Canvas& cv, int w, int hh, void (*paint)(Graphics&)) {
    if (!CanvasEnsure(cv, w, hh)) return;
    {
        Bitmap bm(w, hh, w * 4, PixelFormat32bppPARGB, (BYTE*)cv.bits);
        Graphics g(&bm);
        Prep(g);
        ClearBg(g, h, w, hh);
        paint(g);
    }
    PresentFrame(h, hdc, cv, w, hh);
}
static void Relayout() {
    int oldH = g_h, oldW = g_w;
    Layout();
    if (g_hwnd && (g_h != oldH || g_w != oldW)) {
        RECT wr; GetWindowRect(g_hwnd, &wr);
        int x = wr.left;
        if (g_w != oldW) {                            // keep the widget on its monitor when it gets wider
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            if (x + g_w > mi.rcWork.right - S(12)) x = mi.rcWork.right - S(12) - g_w;
            if (x < mi.rcWork.left) x = mi.rcWork.left;
        }
        SetWindowPos(g_hwnd, NULL, x, wr.top, g_w, g_h, SWP_NOZORDER | SWP_NOACTIVATE);
        CanvasFree(g_cv);
    }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}
static void ApplyBackdrop() {
    BOOL bd;
    if (g_acrylic) ApplyFrame(g_hwnd, BACKDROP_ACRYLIC);
    else { ApplyFrame(g_hwnd, 1); T.backdrop = FALSE; }
    bd = T.backdrop;
    SendMessageW(g_hwnd, WM_NCACTIVATE, TRUE, 0);
    (void)bd;
    ApplyLayered(g_hwnd);                           // see-through background mode (appearance.h)
}

// ───────────────────────────── settings window ───────────────────────
// A Fluent window with a toggle for every sensor, grouped like the widget. The list flows into as
// many columns as needed to fit the screen (balanced heights); only if even that is not enough it scrolls.
#define COLW   380
#define CGAP   16
#define ST_TTL 40
#define SROW   32
#define SHDR   34
static int g_sw, g_sh, g_sScroll, g_sContent, g_sHover = -1, g_sCols = 1;
static BOOL g_sColl[G_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, TRUE };   // AIDA64 system values start collapsed
struct SRowRef { int sensor; int group; int col; REAL y; BOOL cont; };
static SRowRef g_sRows[(MAX_SENS + G_COUNT) * 2]; static int g_sRowN;
static RECT g_sClose;
static BOOL g_sCloseHot;

// Flow rows into columns no taller than limit. Returns the number of columns used; *maxH = tallest column.
static int SettingsFlow(int limit, int* maxH) {
    g_sRowN = 0; int col = 0, y = 0, tall = 0;
    for (int gi = 0; gi < G_COUNT; gi++) {
        BOOL has = FALSE;
        for (int i = 0; i < g_n; i++) if (g_s[i].group == gi) { has = TRUE; break; }
        if (!has) continue;
        if (y > 0 && y + SHDR + SROW > limit) { col++; y = 0; }          // no orphan headers at the bottom
        g_sRows[g_sRowN] = { -1, gi, col, (REAL)y, FALSE }; g_sRowN++; y += SHDR;
        if (g_sColl[gi]) { if (y > tall) tall = y; continue; }
        for (int o = 0; o < g_orderNext; o++)
            for (int i = 0; i < g_n; i++) {
                if (g_s[i].order != o || g_s[i].group != gi) continue;
                if (y + SROW > limit && y > SHDR) {                       // continue the group in the next column
                    if (y > tall) tall = y;
                    col++; y = 0;
                    g_sRows[g_sRowN] = { -1, gi, col, (REAL)y, TRUE }; g_sRowN++; y += SHDR;
                }
                g_sRows[g_sRowN] = { i, gi, col, (REAL)y, FALSE }; g_sRowN++; y += SROW;
            }
        if (y > tall) tall = y;
    }
    *maxH = tall;
    return col + 1;
}
static int SettingsTop() { return ST_TTL + (g_aidaOk ? 8 : 84); }
static int ColX(int c) { return 16 + c * (COLW + CGAP); }
static void SettingsPaint(Graphics& g) {
    int W = 32 + g_sCols * COLW + (g_sCols - 1) * CGAP, top = SettingsTop();
    DrawAppMark(g, D(12), D(12), D(16));
    FText(g, TR(L"Параметры Sensus", L"Sensus settings"), g_fCaption, T.text, RectF(D(40), 0, D(300), D(ST_TTL)));
    RectF rx = RF(g_sClose);
    if (g_sCloseHot) FFillRect(g, rx.X, rx.Y, rx.Width, rx.Height, Color(255, 0xC4, 0x2B, 0x1C));
    FGlyph(g, GL_CLOSE, D(10), g_sCloseHot ? Color(255, 255, 255, 255) : T.text, rx);
    if (!g_aidaOk) {
        RectF ib(D(16), D(ST_TTL), D(W - 32), D(68));
        FillRR(g, ib, D(6), T.card); StrokeRR(g, ib, D(6), T.cardStroke);
        FGlyph(g, GL_INFO, D(16), T.accent, RectF(ib.X + D(12), ib.Y + D(10), D(20), D(20)));
        FText(g, TR(L"Больше датчиков — из AIDA64", L"More sensors from AIDA64"),
              g_fStrong, T.text, RectF(ib.X + D(42), ib.Y + D(8), ib.Width - D(52), D(22)), AL_LEFT, StringTrimmingEllipsisCharacter);
        FText(g, TR(L"Температуры ЦП и платы, вентиляторы. В AIDA64: Настройки →", L"CPU and board temps, fans. In AIDA64: Preferences →"),
              g_fCaption, T.text2, RectF(ib.X + D(42), ib.Y + D(28), ib.Width - D(52), D(18)), AL_LEFT, StringTrimmingEllipsisCharacter);
        FText(g, TR(L"Внешние приложения → включить общую память", L"External Applications → Enable shared memory"),
              g_fCaption, T.text2, RectF(ib.X + D(42), ib.Y + D(45), ib.Width - D(52), D(18)), AL_LEFT, StringTrimmingEllipsisCharacter);
    }
    g.SetClip(RectF(0, D(top), D(W), (REAL)g_sh - D(top)));
    for (int r = 0; r < g_sRowN; r++) {
        REAL x = D(ColX(g_sRows[r].col)), y = D(top + g_sRows[r].y - g_sScroll);
        if (y > g_sh || y + D(SROW) < D(top)) continue;
        if (g_sRows[r].sensor < 0) {
            WCHAR h[96];
            if (g_sRows[r].cont) swprintf_s(h, TR(L"%s (продолжение)", L"%s (continued)"), GroupName(g_sRows[r].group));
            else wcscpy_s(h, GroupName(g_sRows[r].group));
            if (!g_sRows[r].cont) {
                int cnt = 0; for (int i = 0; i < g_n; i++) if (g_s[i].group == g_sRows[r].group) cnt++;
                WCHAR c[48];
                swprintf_s(c, g_sColl[g_sRows[r].group] ? TR(L"показать (%d)", L"show (%d)") : TR(L"свернуть", L"collapse"), cnt);
                if (g_sHover == r) FillRR(g, RectF(x, y + D(4), D(COLW), D(SHDR - 6)), D(4), T.subtleHover);
                FText(g, c, g_fCaption, T.text3, RectF(x, y + D(8), D(COLW - 30), D(24)), AL_RIGHT);
                FGlyph(g, g_sColl[g_sRows[r].group] ? 0xE76C : GL_CHEVRON_D, D(9), T.text2, RectF(x + D(COLW - 24), y + D(8), D(16), D(24)));
            }
            FText(g, h, g_fStrong, T.text, RectF(x + D(4), y + D(8), D(COLW - 120), D(24)), AL_LEFT, StringTrimmingEllipsisCharacter);
            continue;
        }
        Sensor* s = &g_s[g_sRows[r].sensor];
        FillRR(g, RectF(x, y, D(COLW), D(SROW - 2)), D(4), g_sHover == r ? T.ctrlHover : T.card);
        WCHAR val[64] = L"";
        if (s->avail) Format(s, val, 64);
        REAL vw = val[0] ? FTextW(g, val, g_fCaption) + D(12) : 0;
        FText(g, s->label, g_fBody, s->avail ? T.text : T.text3, RectF(x + D(12), y, D(COLW - 12 - 64) - vw, D(SROW - 2)), AL_LEFT, StringTrimmingEllipsisCharacter);
        if (val[0]) FText(g, val, g_fCaption, T.text3, RectF(x + D(12), y, D(COLW - 12 - 64), D(SROW - 2)), AL_RIGHT);
        FToggle(g, x + D(COLW - 12 - 40), y + D((SROW - 2 - 20) / 2), s->shown, g_sHover == r);
    }
    g.ResetClip();
}
static void SettingsLayout() {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_hwnd ? g_hwnd : g_set, MONITOR_DEFAULTTOPRIMARY), &mi);
    int top = SettingsTop();
    int availH = MulDiv(mi.rcWork.bottom - mi.rcWork.top - 64, 96, (int)g_dpi) - top - 12;
    int availW = MulDiv(mi.rcWork.right - mi.rcWork.left - 64, 96, (int)g_dpi);
    if (availH < SHDR + SROW * 3) availH = SHDR + SROW * 3;
    int maxCols = (availW - 32 + CGAP) / (COLW + CGAP);
    if (maxCols < 1) maxCols = 1;
    if (maxCols > 6) maxCols = 6;

    int tall = 0, cols = 1;
    for (cols = 1; cols <= maxCols; cols++) if (SettingsFlow(availH, &tall) <= cols) break;
    if (cols > maxCols) cols = maxCols;
    // balance: the smallest column height that still fits into `cols` columns
    int lo = SHDR + SROW, hi = 1 << 20;
    while (lo < hi) { int mid = (lo + hi) / 2; if (SettingsFlow(mid, &tall) <= cols) hi = mid; else lo = mid + 1; }
    g_sCols = SettingsFlow(lo, &tall);
    g_sContent = tall + 12;

    int want = top + g_sContent;
    if (want > top + availH + 12) want = top + availH + 12;
    int W = 32 + g_sCols * COLW + (g_sCols - 1) * CGAP;
    g_sw = S(W); g_sh = S(want);
    g_sClose = { S(W - 46), 0, S(W), S(32) };
    int maxScroll = top + g_sContent - want; if (maxScroll < 0) maxScroll = 0;
    if (g_sScroll > maxScroll) g_sScroll = maxScroll;
}
static int SettingsHit(POINT p) {
    int top = S(SettingsTop());
    if (p.y < top) return -1;
    for (int r = 0; r < g_sRowN; r++) {
        if (g_sRows[r].sensor < 0 && g_sRows[r].cont) continue;
        int x = S(ColX(g_sRows[r].col)), y = S((int)g_sRows[r].y + SettingsTop() - g_sScroll);
        int hh = g_sRows[r].sensor < 0 ? S(SHDR) : S(SROW - 2);
        if (p.y >= y && p.y < y + hh && p.x >= x && p.x < x + S(COLW)) return r;
    }
    return -1;
}
static LRESULT CALLBACK SetProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_NCCALCSIZE: if (w) return 0; break;
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        if (PtInRect(&g_sClose, p)) return HTCLIENT;
        return p.y < S(ST_TTL) ? HTCAPTION : HTCLIENT;
    }
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps); Render(h, hdc, g_cvSet, g_sw, g_sh, SettingsPaint); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int hv = SettingsHit(p); BOOL ch = PtInRect(&g_sClose, p);
        if (hv != g_sHover || ch != g_sCloseHot) { g_sHover = hv; g_sCloseHot = ch; InvalidateRect(h, NULL, FALSE); }
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: g_sHover = -1; g_sCloseHot = FALSE; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONUP: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (PtInRect(&g_sClose, p)) { DestroyWindow(h); return 0; }
        int r = SettingsHit(p);
        if (r >= 0 && g_sRows[r].sensor < 0) {           // group header: collapse / expand
            g_sColl[g_sRows[r].group] = !g_sColl[g_sRows[r].group];
            RECT wr; GetWindowRect(h, &wr);
            SettingsLayout(); CanvasFree(g_cvSet); g_sHover = -1;
            MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
            int x = (mi.rcWork.left + mi.rcWork.right - g_sw) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - g_sh) / 2;
            SetWindowPos(h, NULL, x, y, g_sw, g_sh, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (r >= 0) {
            Sensor* s = &g_s[g_sRows[r].sensor];
            s->shown = !s->shown; SaveShown(s);
            InvalidateRect(h, NULL, FALSE);
            Relayout();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int maxScroll = SettingsTop() + g_sContent - MulDiv(g_sh, 96, (int)g_dpi);
        if (maxScroll < 0) maxScroll = 0;
        g_sScroll -= GET_WHEEL_DELTA_WPARAM(w) / 120 * SROW * 2;
        if (g_sScroll < 0) g_sScroll = 0; if (g_sScroll > maxScroll) g_sScroll = maxScroll;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN: if (w == VK_ESCAPE) DestroyWindow(h); return 0;
    case WM_TIMER: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w) * g_scale / 100; FluentFonts(); SettingsLayout(); CanvasFree(g_cvSet);
        RECT* r = (RECT*)l; SetWindowPos(h, NULL, r->left, r->top, g_sw, g_sh, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY: CanvasFree(g_cvSet); g_set = NULL; return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
static void OpenSettings() {
    if (g_set) { SetForegroundWindow(g_set); return; }
    g_sScroll = 0;
    HWND h = CreateWindowExW(WS_EX_APPWINDOW, SET_CLASS, TR(L"Параметры Sensus", L"Sensus settings"), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
                             0, 0, 440, 400, NULL, NULL, g_hInst, NULL);
    if (!h) return;
    g_set = h;
    SettingsLayout();
    ApplyFrame(h, BACKDROP_MICA);
    MONITORINFO mi = { sizeof(mi) };                 // centre on the widget's monitor
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowPos(h, NULL, (mi.rcWork.left + mi.rcWork.right - g_sw) / 2, (mi.rcWork.top + mi.rcWork.bottom - g_sh) / 2, g_sw, g_sh, SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(h, SW_SHOW); SetForegroundWindow(h);
    SetTimer(h, 1, 1000, NULL);
}

// ───────────────────────────── tray & menu ───────────────────────────
static void TrayAdd() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAY; g_nid.hIcon = g_iconSm;
    wcscpy_s(g_nid.szTip, L"Sensus");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void ApplyTopmost() {
    SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, g_topmost ? 0 : (LONG_PTR)FindWindowW(L"Progman", NULL));
    SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
static void SetVisible(BOOL v) {
    g_visible = v;
    if (v) { InvalidateRect(g_hwnd, NULL, FALSE); ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); }
    else   { ShowWindow(g_hwnd, SW_HIDE); CanvasFree(g_cv); }
}
static void ShowMenu(POINT pt) {
    HMENU m = CreatePopupMenu(), every = CreatePopupMenu();
    static const int secs[3] = { 1, 2, 5 };
    for (int i = 0; i < 3; i++) {
        WCHAR s[32]; swprintf_s(s, TR(L"Каждые %d с", L"Every %d s"), secs[i]);
        AppendMenuW(every, MF_STRING | (g_every == secs[i] ? MF_CHECKED : 0), ID_EVERY_1 + i, s);
    }
    AppendMenuW(m, MF_STRING, ID_SETTINGS, TR(L"Выбрать параметры…", L"Choose sensors…"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)every, TR(L"Частота обновления", L"Refresh interval"));
    HMENU cols = CreatePopupMenu();
    AppendMenuW(cols, MF_STRING | (g_colMode == 1 ? MF_CHECKED : 0), ID_COLS_1, TR(L"В один ряд", L"One column"));
    AppendMenuW(cols, MF_STRING | (g_colMode == 2 ? MF_CHECKED : 0), ID_COLS_2, TR(L"В два ряда", L"Two columns"));
    AppendMenuW(cols, MF_STRING | (g_colMode == 0 ? MF_CHECKED : 0), ID_COLS_AUTO, TR(L"Автоматически — два ряда, если не помещается", L"Automatic — two columns when it does not fit"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)cols, TR(L"Расположение", L"Layout"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_acrylic ? MF_CHECKED : 0), ID_ACRYLIC, TR(L"Прозрачный фон", L"Transparent background"));
    AppendAppearanceMenu(m);
    AppendMenuW(m, MF_STRING | (g_topmost ? MF_CHECKED : 0), ID_TOPMOST, TR(L"Поверх всех окон", L"Always on top"));
    AppendMenuW(m, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, TR(L"Запускать вместе с Windows", L"Start with Windows"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_SHOWHIDE, g_visible ? TR(L"Скрыть виджет", L"Hide widget") : TR(L"Показать виджет", L"Show widget"));
    AppendLangMenu(m, REG_KEY);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, TR(L"Sensus 1.2 · разработчик Sallador", L"Sensus 1.2 · by Sallador"));
    AppendMenuW(m, MF_STRING, ID_EXIT, TR(L"Выход", L"Exit"));
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}
static void SavePos() { RECT r; GetWindowRect(g_hwnd, &r); RegSetDw(L"PosX", (DWORD)r.left); RegSetDw(L"PosY", (DWORD)r.top); }
static void PaintWidget(Graphics& g) { PaintTo(g); }

// Widget size (appearance.h): the whole UI is drawn at DPI × scale.
static void Rescale() {
    if (!g_hwnd) return;
    UINT d = GetDpiForWindow(g_hwnd); if (!d) d = 96;
    g_dpi = d * g_scale / 100;
    FluentFonts(); g_h = 0;
    Relayout();
    if (g_set) { SettingsLayout(); CanvasFree(g_cvSet); SetWindowPos(g_set, NULL, 0, 0, g_sw, g_sh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); InvalidateRect(g_set, NULL, FALSE); }
    KeepOnScreen(g_hwnd);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ───────────────────────────── window proc ───────────────────────────
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) { TrayAdd(); if (!g_topmost) ApplyTopmost(); return 0; }
    switch (msg) {
    case WM_NCCALCSIZE: if (w) return 0; break;
    case WM_NCHITTEST:  return HTCLIENT;
    case WM_NCACTIVATE: return DefWindowProcW(h, msg, TRUE, l);
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps); Render(h, hdc, g_cv, g_w, g_h, PaintWidget); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        g_tick++;
        if (g_tick % g_every == 0) { Sample(); Relayout(); }
        if (g_tick == 3 || g_tick % 60 == 0) SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEWHEEL:
        if (ScaleWheel(w, REG_KEY)) return 0;
        if (g_contentH > g_viewH) {
            g_scroll -= GET_WHEEL_DELTA_WPARAM(w) / 120 * ROW_H * 3;
            Layout(); InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (g_drag) {
            POINT c; GetCursorPos(&c);
            SetWindowPos(h, NULL, g_dragWin.left + c.x - g_dragCursor.x, g_dragWin.top + c.y - g_dragCursor.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int hv = HitTest(p);
        if (hv != g_hover) { g_hover = hv; InvalidateRect(h, NULL, FALSE); }
        if (!g_tracking) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); g_tracking = TRUE; }
        return 0;
    }
    case WM_MOUSELEAVE: g_tracking = FALSE; if (g_hover != H_NONE) { g_hover = H_NONE; InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        g_press = HitTest(p);
        if (g_press == H_NONE) { g_drag = TRUE; GetCursorPos(&g_dragCursor); GetWindowRect(h, &g_dragWin); }
        SetCapture(h); InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        BOOL wasDrag = g_drag; int pressed = g_press;
        g_drag = FALSE; g_press = H_NONE;
        if (GetCapture() == h) ReleaseCapture();
        if (wasDrag) { SavePos(); Relayout(); return 0; }     // new spot may have less room below
        if (HitTest(p) == H_MORE && pressed == H_MORE) {
            RECT r = g_rc[H_MORE]; POINT pt = { r.left, r.bottom + S(4) };
            ClientToScreen(h, &pt); ShowMenu(pt);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: OpenSettings(); return 0;
    case WM_CAPTURECHANGED: if (g_drag) { g_drag = FALSE; SavePos(); } g_press = H_NONE; return 0;
    case WM_CONTEXTMENU: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (p.x == -1 && p.y == -1) GetCursorPos(&p);
        ShowMenu(p);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w) * g_scale / 100; FluentFonts(); Layout(); CanvasFree(g_cv);
        RECT* r = (RECT*)l; SetWindowPos(h, NULL, r->left, r->top, g_w, g_h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_drag) GetWindowRect(h, &g_dragWin), GetCursorPos(&g_dragCursor);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (l && lstrcmpiW((LPCWSTR)l, L"ImmersiveColorSet") == 0) { LoadTheme(); ApplyBackdrop(); ApplyMenuTheme(); InvalidateRect(h, NULL, FALSE); if (g_set) { ApplyFrame(g_set, BACKDROP_MICA); InvalidateRect(g_set, NULL, FALSE); } }
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED: LoadTheme(); ApplyBackdrop(); InvalidateRect(h, NULL, FALSE); break;
    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP) SetVisible(!g_visible);
        else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) { POINT p; GetCursorPos(&p); ShowMenu(p); }
        return 0;
    case WM_APP_SHOW: if (!g_visible) SetVisible(TRUE); return 0;
    case WM_CLOSE: g_exiting = TRUE; DestroyWindow(h); return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id >= ID_LANG_AUTO && id <= ID_LANG_EN) { RestartForLang(REG_KEY, id - ID_LANG_AUTO, g_demo ? L"/demo" : NULL); g_exiting = TRUE; DestroyWindow(h); return 0; }
        if (HandleAppearanceCmd(h, id, REG_KEY)) { ApplyBackdrop(); InvalidateRect(h, NULL, FALSE); if (g_set) InvalidateRect(g_set, NULL, FALSE); return 0; }
        if (id == ID_SETTINGS)       OpenSettings();
        else if (id >= ID_COLS_AUTO && id <= ID_COLS_2) { g_colMode = id - ID_COLS_AUTO; RegSetDw(L"Columns", g_colMode); g_scroll = 0; Relayout(); }
        else if (id >= ID_EVERY_1 && id <= ID_EVERY_5) { static const int secs[3] = { 1, 2, 5 }; g_every = secs[id - ID_EVERY_1]; RegSetDw(L"Every", g_every); }
        else if (id == ID_TOPMOST)   { g_topmost = !g_topmost; RegSetDw(L"TopMost", g_topmost); ApplyTopmost(); }
        else if (id == ID_AUTOSTART) SetAutostart(!IsAutostart());
        else if (id == ID_ACRYLIC)   { g_acrylic = !g_acrylic; RegSetDw(L"Acrylic", g_acrylic); ApplyBackdrop(); InvalidateRect(h, NULL, FALSE); }
        else if (id == ID_SHOWHIDE)  SetVisible(!g_visible);
        else if (id == ID_EXIT)      { g_exiting = TRUE; DestroyWindow(h); }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(h, TIMER_ID);
        if (g_set) DestroyWindow(g_set);
        g_nid.uFlags = 0; Shell_NotifyIconW(NIM_DELETE, &g_nid);
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
                             havePos ? x : 0, havePos ? y : 0, BASE_W, 400, owner, NULL, g_hInst, NULL);
    if (!h) return FALSE;
    g_hwnd = h;
    g_dpi = GetDpiForWindow(h); if (!g_dpi) g_dpi = 96;
    g_dpi = g_dpi * g_scale / 100;
    FluentFonts();
    Layout();
    ApplyBackdrop();
    if (!havePos) {
        MONITORINFO mi = { sizeof(mi) }; POINT o = { 0, 0 };
        GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi);
        x = mi.rcWork.right - g_w - S(24) - S(360 + 16); y = mi.rcWork.top + S(24);   // left of Cogitator's default spot
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
    LoadAppearance(REG_KEY); g_themeHook = ApplyTextColor; g_onScale = Rescale;
    g_demo = cmd && wcsstr(cmd, L"/demo") != NULL;
    g_mutexLang = CreateMutexW(NULL, TRUE, g_demo ? L"Local\\Sensus_Sallador_Demo" : L"Local\\Sensus_Sallador_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(CLASS_NAME, NULL);
        if (w) PostMessageW(w, WM_APP_SHOW, 0, 0);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    GdiplusStartupInput gsi; ULONG_PTR gtoken;
    if (GdiplusStartup(&gtoken, &gsi, NULL) != Ok) return 1;
    FluentInit(); LoadTheme(); ApplyMenuTheme();

    g_topmost = RegGetDw(L"TopMost", 0) != 0;
    g_acrylic = RegGetDw(L"Acrylic", 1) != 0;
    g_colMode = (int)RegGetDw(L"Columns", 1); if (g_colMode < 0 || g_colMode > 2) g_colMode = 1;
    g_every   = (int)RegGetDw(L"Every", 1); if (g_every != 1 && g_every != 2 && g_every != 5) g_every = 1;
    if (IsAutostart()) SetAutostart(TRUE);

    g_s = (Sensor*)Alloc(sizeof(Sensor) * MAX_SENS);
    if (!g_s) return 1;

    g_iconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = g_iconBig; wc.hIconSm = g_iconSm; wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    wc.style = 0; wc.lpfnWndProc = SetProc; wc.lpszClassName = SET_CLASS;
    RegisterClassExW(&wc);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (g_demo) DemoData();
    else { InitCpu(); InitPdh(); InitGpu(); Sample(); }
    if (!CreateMainWindow()) return 1;

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!m.hwnd && m.message == WM_APP_RECREATE) { Sleep(1500); if (!CreateMainWindow()) break; continue; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_pdhQ) PdhCloseQuery(g_pdhQ);
    GdiplusShutdown(gtoken);
    CoUninitialize();
    return 0;
}
