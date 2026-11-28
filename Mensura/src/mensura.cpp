// Mensura 1.1 — desktop widget: remaining Claude plan usage limits (session / weekly).
// Windows 11 (Fluent) edition. Developer: Sallador.  Pure Win32 + GDI+ + WinHTTP, static CRT.
//
// Data source: the local Claude Code sign-in (%USERPROFILE%\.claude\.credentials.json) and the
// same usage endpoint Claude Code's /usage screen uses. The token is only read from disk and sent
// to api.anthropic.com; it is never stored or shown by the widget.
//
//   Mensura.exe          start (single instance; a second start shows the widget)
//   Mensura.exe /demo    sample data, no network (for screenshots)

#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <winhttp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "resource.h"
#include "fluent.h"
#include "lang.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")

// ───────────────────────────── constants ─────────────────────────────
#define APP_NAME     L"Mensura"
#define CLASS_NAME   L"MensuraWidget"
#define REG_KEY      L"Software\\Sallador\\Mensura"
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define WM_TRAY          (WM_APP + 1)
#define WM_APP_SHOW      (WM_APP + 2)
#define WM_APP_RECREATE  (WM_APP + 3)
#define WM_APP_DATA      (WM_APP + 4)
#define TIMER_ID     1
#define POS_UNSET    0x80000000u
#define MAX_LIM      8

#define BASE_W   320
#define HDR_H    44
#define CARD_Y   46
#define CARD_H   78
#define CARD_GAP 8
#define FOOT_H   36

enum {
    ID_TOPMOST = 110, ID_AUTOSTART = 111, ID_SHOWHIDE = 112, ID_EXIT = 113 /*used by installer*/,
    ID_ACRYLIC = 115, ID_MODE_LEFT = 120, ID_MODE_USED, ID_REFRESH, ID_WARN,
    ID_EVERY_1 = 130, ID_EVERY_2, ID_EVERY_5, ID_EVERY_10
};
enum { H_NONE = -1, H_MORE = 0, H_REFRESH, H_COUNT };
enum { ST_NONE = 0, ST_OK, ST_NOCREDS, ST_AUTH, ST_NET, ST_HTTP, ST_PARSE, ST_RATE };

struct Limit { char key[40]; double util; LONGLONG reset; /* UTC FILETIME, 0 = none */ int order; };
struct Usage { int status; DWORD http; int n; Limit lim[MAX_LIM]; WCHAR plan[24]; UINT32 tokHash; };
struct Warned { char key[40]; LONGLONG reset; int level; };

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

static BOOL g_topmost = FALSE, g_acrylic = TRUE, g_visible = TRUE, g_exiting = FALSE;
static BOOL g_showLeft = TRUE, g_warn = TRUE, g_demo = FALSE;
static int  g_everyMin = 5;

static Usage     g_data;                // last good data
static BOOL      g_haveData;
static SYSTEMTIME g_dataTime;           // local time of last good fetch
static int       g_status = ST_NONE;    // result of the latest attempt
static DWORD     g_httpCode;
static volatile LONG g_fetching;
static volatile UINT32 g_badTok;        // hash of a token the server rejected — don't retry it
static ULONGLONG g_nextFetch;
static Warned    g_warned[MAX_LIM]; static int g_warnedN; static BOOL g_warnInit;
static unsigned  g_tick;

// ───────────────────────────── helpers ───────────────────────────────
static void* Alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void  Free(void* p)   { if (p) HeapFree(GetProcessHeap(), 0, p); }

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
static LONGLONG NowUtc() { FILETIME f; GetSystemTimeAsFileTime(&f); return ((LONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static UINT32 Fnv(const char* s) { UINT32 h = 2166136261u; for (; *s; s++) { h ^= (BYTE)*s; h *= 16777619u; } return h ? h : 1; }

// ───────────────────────────── tiny JSON reader ──────────────────────
struct J { const char* p; const char* e; };
static void Ws(J& j) { while (j.p < j.e && (*j.p == ' ' || *j.p == '\n' || *j.p == '\r' || *j.p == '\t')) j.p++; }
static BOOL JStr(J& j, char* out, int n) {
    if (j.p >= j.e || *j.p != '"') return FALSE;
    j.p++; int k = 0;
    while (j.p < j.e && *j.p != '"') {
        char c = *j.p++;
        if (c == '\\' && j.p < j.e) { c = *j.p++; if (c == 'u') { j.p += (j.e - j.p >= 4) ? 4 : (j.e - j.p); c = '?'; } }
        if (out && k < n - 1) out[k++] = c;
    }
    if (j.p < j.e) j.p++;
    if (out && n) out[k] = 0;
    return TRUE;
}
static BOOL JSkip(J& j, int depth = 0) {
    Ws(j);
    if (j.p >= j.e || depth > 32) return FALSE;
    char c = *j.p;
    if (c == '"') return JStr(j, NULL, 0);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j.p++; Ws(j);
        if (j.p < j.e && *j.p == close) { j.p++; return TRUE; }
        for (;;) {
            if (c == '{') {
                Ws(j); if (!JStr(j, NULL, 0)) return FALSE;
                Ws(j); if (j.p >= j.e || *j.p != ':') return FALSE; j.p++;
            }
            if (!JSkip(j, depth + 1)) return FALSE;
            Ws(j); if (j.p >= j.e) return FALSE;
            if (*j.p == ',') { j.p++; continue; }
            if (*j.p == close) { j.p++; return TRUE; }
            return FALSE;
        }
    }
    while (j.p < j.e && !strchr(",}] \n\r\t", *j.p)) j.p++;
    return TRUE;
}
static BOOL IsNull(J& j) { return j.e - j.p >= 4 && !strncmp(j.p, "null", 4); }

// "2025-11-04T04:59:59.943648+00:00" -> UTC FILETIME
static LONGLONG ParseIso(const char* s) {
    int Y, M, D, h, m, sec;
    if (sscanf_s(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    SYSTEMTIME st = { (WORD)Y, (WORD)M, 0, (WORD)D, (WORD)h, (WORD)m, (WORD)sec, 0 };
    FILETIME f; if (!SystemTimeToFileTime(&st, &f)) return 0;
    LONGLONG t = ((LONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
    const char* p = strchr(s, 'T'); if (!p) return t;
    p += 9;                                              // past hh:mm:ss
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0; sscanf_s(p + 1, "%d:%d", &oh, &om);
        LONGLONG off = (LONGLONG)(oh * 60 + om) * 600000000LL;
        t += (*p == '+') ? -off : off;
    }
    return t;
}
static int KeyOrder(const char* k) {
    if (!strcmp(k, "five_hour")) return 0;
    if (!strcmp(k, "seven_day")) return 1;
    if (!strcmp(k, "seven_day_sonnet")) return 2;
    if (!strcmp(k, "seven_day_opus")) return 3;
    return 10;
}
// Every top-level member that is an object with a numeric "utilization" becomes a limit.
static BOOL ParseUsage(const char* body, int len, Usage* u) {
    J j = { body, body + len };
    Ws(j); if (j.p >= j.e || *j.p != '{') return FALSE; j.p++;
    BOOL any = FALSE;
    for (;;) {
        Ws(j); if (j.p < j.e && *j.p == '}') break;
        char key[40]; if (!JStr(j, key, sizeof(key))) return FALSE;
        Ws(j); if (j.p >= j.e || *j.p != ':') return FALSE; j.p++; Ws(j);
        if (j.p < j.e && *j.p == '{') {
            any = TRUE;
            j.p++;
            BOOL hasU = FALSE; double util = 0; LONGLONG reset = 0;
            for (;;) {
                Ws(j); if (j.p < j.e && *j.p == '}') { j.p++; break; }
                char ik[32]; if (!JStr(j, ik, sizeof(ik))) return FALSE;
                Ws(j); if (j.p >= j.e || *j.p != ':') return FALSE; j.p++; Ws(j);
                if (!strcmp(ik, "utilization") && !IsNull(j) && j.p < j.e && (*j.p == '-' || (*j.p >= '0' && *j.p <= '9'))) {
                    char num[32]; int k = 0;
                    while (j.p < j.e && k < 31 && strchr("0123456789.eE+-", *j.p)) num[k++] = *j.p++;
                    num[k] = 0; util = atof(num); hasU = TRUE;
                } else if (!strcmp(ik, "resets_at") && j.p < j.e && *j.p == '"') {
                    char ts[64]; JStr(j, ts, sizeof(ts)); reset = ParseIso(ts);
                } else if (!JSkip(j)) return FALSE;
                Ws(j); if (j.p < j.e && *j.p == ',') j.p++;
            }
            int ord = KeyOrder(key);
            BOOL keep = hasU && strcmp(key, "extra_usage") != 0 && (ord <= 1 || reset || util > 0);
            if (keep && u->n < MAX_LIM) {
                Limit* l = &u->lim[u->n++];
                strcpy_s(l->key, key); l->util = util < 0 ? 0 : util > 100 ? 100 : util; l->reset = reset; l->order = ord;
            }
        } else if (!JSkip(j)) return FALSE;
        Ws(j); if (j.p < j.e && *j.p == ',') j.p++;
        else { Ws(j); if (j.p < j.e && *j.p == '}') break; else if (j.p >= j.e) break; }
    }
    for (int a = 1; a < u->n; a++)                       // stable insertion sort by order
        for (int b = a; b > 0 && u->lim[b].order < u->lim[b - 1].order; b--) { Limit t = u->lim[b]; u->lim[b] = u->lim[b - 1]; u->lim[b - 1] = t; }
    return any;
}

// "key": "value" anywhere in the text (credential file fields are unique)
static BOOL FindStr(const char* txt, const char* key, char* out, int n) {
    char pat[48]; sprintf_s(pat, "\"%s\"", key);
    const char* p = strstr(txt, pat); if (!p) return FALSE;
    J j = { p + strlen(pat), txt + strlen(txt) };
    Ws(j); if (j.p >= j.e || *j.p != ':') return FALSE; j.p++; Ws(j);
    return JStr(j, out, n) && out[0];
}

// ───────────────────────────── fetching (worker thread) ──────────────
// Claude Code's public OAuth client id (same one the `claude` CLI uses for its own sign-in).
#define OAUTH_CLIENT_ID "9d1c250a-e61b-44d9-88ed-5944d1962f5e"

static void CredsPath(WCHAR* path, const WCHAR* name) {
    WCHAR dir[MAX_PATH];
    if (GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", dir, MAX_PATH) == 0) {
        if (!GetEnvironmentVariableW(L"USERPROFILE", dir, MAX_PATH)) { path[0] = 0; return; }
        wcscat_s(dir, L"\\.claude");
    }
    swprintf_s(path, MAX_PATH, L"%s\\%s", dir, name);
}
static char* ReadCreds() {
    WCHAR path[MAX_PATH]; CredsPath(path, L".credentials.json");
    if (!path[0]) return NULL;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), rd = 0;
    char* buf = NULL;
    if (size && size < (1u << 20) && (buf = (char*)Alloc(size + 1)) != NULL) {
        if (!ReadFile(f, buf, size, &rd, NULL)) rd = 0;
        buf[rd] = 0;
    }
    CloseHandle(f);
    return buf;
}
static void Wipe(char* s) { if (s) { SecureZeroMemory(s, strlen(s)); Free(s); } }

static void PlanLabel(const char* sub, const char* tier, WCHAR* out, int n) {
    out[0] = 0;
    if (strstr(tier, "20x")) { wcscpy_s(out, n, L"Max 20x"); return; }
    if (strstr(tier, "5x"))  { wcscpy_s(out, n, L"Max 5x");  return; }
    if (!sub[0]) return;
    MultiByteToWideChar(CP_UTF8, 0, sub, -1, out, n);
    out[n - 1] = 0;
    if (out[0] >= L'a' && out[0] <= L'z') out[0] -= 32;
}
static LONGLONG FindNum(const char* txt, const char* key) {
    char pat[48]; sprintf_s(pat, "\"%s\"", key);
    const char* p = strstr(txt, pat); if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return _atoi64(p);
}
static LONGLONG NowUnixMs() { return (NowUtc() - 116444736000000000LL) / 10000; }
static BOOL TokenChars(const char* s) {
    if (!*s) return FALSE;
    for (; *s; s++) if (!(isalnum((BYTE)*s) || *s == '-' || *s == '_' || *s == '.' || *s == '~' || *s == '+' || *s == '/' || *s == '=')) return FALSE;
    return TRUE;
}

// One HTTPS request. Returns the status code (0 = network failure); *outBody is heap, NUL-terminated.
static DWORD Https(const WCHAR* host, const WCHAR* verb, const WCHAR* path, const WCHAR* headers,
                   const char* body, char** outBody) {
    DWORD code = 0;
    *outBody = NULL;
    HINTERNET s = WinHttpOpen(L"Mensura/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, 15000);
    HINTERNET c = s ? WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    DWORD bl = body ? (DWORD)strlen(body) : 0;
    if (r && WinHttpSendRequest(r, headers, (DWORD)-1L, (LPVOID)body, bl, bl, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD sz = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
        const int CAP = 256 * 1024;
        char* buf = (char*)Alloc(CAP + 1); int len = 0;
        if (buf) {
            DWORD avail = 0, rd = 0;
            while (WinHttpQueryDataAvailable(r, &avail) && avail && len < CAP) {
                if (avail > (DWORD)(CAP - len)) avail = CAP - len;
                if (!WinHttpReadData(r, buf + len, avail, &rd) || !rd) break;
                len += rd;
            }
            buf[len] = 0;
        }
        *outBody = buf;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return code;
}

// Replace the value of "key" (string or number) in a JSON text; returns a new heap string or NULL.
static char* JsonSet(const char* txt, const char* key, const char* literal) {
    char pat[48]; sprintf_s(pat, "\"%s\"", key);
    const char* k = strstr(txt, pat); if (!k) return NULL;
    J j = { k + strlen(pat), txt + strlen(txt) };
    Ws(j); if (j.p >= j.e || *j.p != ':') return NULL; j.p++; Ws(j);
    const char* vs = j.p;
    if (!JSkip(j)) return NULL;
    const char* ve = j.p;
    size_t a = vs - txt, ln = strlen(literal), b = strlen(ve);
    char* out = (char*)Alloc(a + ln + b + 1); if (!out) return NULL;
    memcpy(out, txt, a); memcpy(out + a, literal, ln); memcpy(out + a + ln, ve, b + 1);
    return out;
}
// Atomically rewrite .credentials.json (temp file + rename) after keeping one backup copy.
static BOOL WriteCreds(const char* txt) {
    WCHAR path[MAX_PATH], tmp[MAX_PATH], bak[MAX_PATH];
    CredsPath(path, L".credentials.json"); CredsPath(tmp, L".credentials.json.mensura-tmp"); CredsPath(bak, L".credentials.json.mensura-backup");
    if (!path[0]) return FALSE;
    CopyFileW(path, bak, FALSE);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD n = (DWORD)strlen(txt), wr = 0;
    BOOL ok = WriteFile(f, txt, n, &wr, NULL) && wr == n && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp);
    return ok;
}
// Renew the sign-in with the refresh token and save it back, so the CLI keeps working too.
// On success copies the new access token into tok (cap 8192).
static BOOL RefreshLogin(char* tok) {
    char* creds = ReadCreds();
    if (!creds) return FALSE;
    char* rt = (char*)Alloc(8192);
    BOOL ok = FALSE;
    if (rt && FindStr(creds, "refreshToken", rt, 8192) && TokenChars(rt)) {
        char* body = (char*)Alloc(8192 + 256);
        if (body) {
            sprintf_s(body, 8192 + 256, "{\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\",\"client_id\":\"" OAUTH_CLIENT_ID "\"}", rt);
            static const WCHAR* hosts[2] = { L"platform.claude.com", L"console.anthropic.com" };
            char* resp = NULL; DWORD code = 0;
            for (int i = 0; i < 2 && code != 200; i++) {
                Wipe(resp); resp = NULL;
                code = Https(hosts[i], L"POST", L"/v1/oauth/token", L"Content-Type: application/json\r\nAccept: application/json\r\n", body, &resp);
                if (code == 400 || code == 401) break;            // refresh token rejected — no point trying the other host
            }
            SecureZeroMemory(body, 8192 + 256); Free(body);
            if (code == 200 && resp) {
                char* at = (char*)Alloc(8192); char* nrt = (char*)Alloc(8192);
                if (at && nrt && FindStr(resp, "access_token", at, 8192) && TokenChars(at)) {
                    BOOL haveNrt = FindStr(resp, "refresh_token", nrt, 8192) && TokenChars(nrt);
                    LONGLONG exp = FindNum(resp, "expires_in"); if (exp <= 0) exp = 3600;
                    char lit[8300], num[32];
                    sprintf_s(lit, "\"%s\"", at);
                    char* t1 = JsonSet(creds, "accessToken", lit);
                    char* t2 = NULL; char* t3 = NULL;
                    if (t1) { if (haveNrt) { sprintf_s(lit, "\"%s\"", nrt); t2 = JsonSet(t1, "refreshToken", lit); } else { t2 = (char*)Alloc(strlen(t1) + 1); if (t2) strcpy_s(t2, strlen(t1) + 1, t1); } }
                    if (t2) { sprintf_s(num, "%lld", NowUnixMs() + exp * 1000); t3 = JsonSet(t2, "expiresAt", num); }
                    if (t3 && WriteCreds(t3)) { strcpy_s(tok, 8192, at); ok = TRUE; }
                    SecureZeroMemory(lit, sizeof(lit));
                    Wipe(t1); Wipe(t2); Wipe(t3);
                }
                Wipe(at); Wipe(nrt);
            }
            Wipe(resp);
        }
    }
    Wipe(rt); Wipe(creds);
    return ok;
}

static DWORD UsageRequest(const char* tok, Usage* u) {
    WCHAR* hdr = (WCHAR*)Alloc((8192 + 256) * sizeof(WCHAR));
    if (!hdr) return 0;
    int k = swprintf_s(hdr, 256, L"Authorization: Bearer ");
    for (const char* p = tok; *p && k < 8192 + 200; p++) hdr[k++] = (WCHAR)(BYTE)*p;
    wcscpy_s(hdr + k, 8192 + 256 - k, L"\r\nanthropic-beta: oauth-2025-04-20\r\nAccept: application/json\r\n");
    char* body = NULL;
    DWORD code = Https(L"api.anthropic.com", L"GET", L"/api/oauth/usage", hdr, NULL, &body);
    SecureZeroMemory(hdr, (8192 + 256) * sizeof(WCHAR)); Free(hdr);
    u->http = code;
    if (code == 0) u->status = ST_NET;
    else if (code == 200) { u->n = 0; u->status = (body && ParseUsage(body, (int)strlen(body), u)) ? ST_OK : ST_PARSE; }
    else if (code == 401 || code == 403) u->status = ST_AUTH;
    else if (code == 429) u->status = ST_RATE;
    else u->status = ST_HTTP;
    Free(body);
    return code;
}

static void DoFetch(Usage* u) {
    char* creds = ReadCreds();
    if (!creds) { u->status = ST_NOCREDS; return; }
    char* tok = (char*)Alloc(8192);
    char sub[32] = "", tier[64] = "", rtProbe[16] = "";
    BOOL haveTok = tok && FindStr(creds, "accessToken", tok, 8192);
    BOOL canRefresh = FindStr(creds, "refreshToken", rtProbe, sizeof(rtProbe));
    LONGLONG expMs = FindNum(creds, "expiresAt");
    FindStr(creds, "subscriptionType", sub, sizeof(sub));
    FindStr(creds, "rateLimitTier", tier, sizeof(tier));
    SecureZeroMemory(rtProbe, sizeof(rtProbe));
    Wipe(creds);
    PlanLabel(sub, tier, u->plan, 24);
    if (!haveTok) { Wipe(tok); u->status = ST_NOCREDS; return; }

    BOOL refreshed = FALSE;
    if (canRefresh && expMs && expMs - NowUnixMs() < 5 * 60 * 1000)      // expired or about to expire
        refreshed = RefreshLogin(tok);
    u->tokHash = Fnv(tok);
    if (u->tokHash == g_badTok && !refreshed) {
        if (canRefresh && RefreshLogin(tok)) { refreshed = TRUE; u->tokHash = Fnv(tok); }
        else { Wipe(tok); u->status = ST_AUTH; return; }
    }
    DWORD code = UsageRequest(tok, u);
    if ((code == 401 || code == 403) && canRefresh && !refreshed && RefreshLogin(tok)) {
        u->tokHash = Fnv(tok);
        UsageRequest(tok, u);
    }
    Wipe(tok);
}
static DWORD WINAPI FetchThread(LPVOID) {
    Usage* u = (Usage*)Alloc(sizeof(Usage));
    if (u) DoFetch(u);
    if (!u || !g_hwnd || !PostMessageW(g_hwnd, WM_APP_DATA, 0, (LPARAM)u)) { Free(u); InterlockedExchange(&g_fetching, 0); }
    return 0;
}
static void DemoData(Usage* u) {
    LONGLONG now = NowUtc(), H = 36000000000LL;
    u->status = ST_OK; wcscpy_s(u->plan, L"Max 5x");
    u->n = 3;
    strcpy_s(u->lim[0].key, "five_hour");        u->lim[0].util = 37; u->lim[0].reset = now + H * 2 + H / 60 * 14;
    strcpy_s(u->lim[1].key, "seven_day");        u->lim[1].util = 64; u->lim[1].reset = now + H * 24 * 3 + H * 5;
    strcpy_s(u->lim[2].key, "seven_day_sonnet"); u->lim[2].util = 82; u->lim[2].reset = now + H * 24 * 3 + H * 5;
}
static void Fetch() {
    if (InterlockedCompareExchange(&g_fetching, 1, 0) != 0) return;
    if (g_demo) {
        Usage* u = (Usage*)Alloc(sizeof(Usage));
        if (u) { DemoData(u); PostMessageW(g_hwnd, WM_APP_DATA, 0, (LPARAM)u); } else InterlockedExchange(&g_fetching, 0);
        return;
    }
    HANDLE t = CreateThread(NULL, 64 * 1024, FetchThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_fetching, 0);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

// ───────────────────────────── text helpers ──────────────────────────
static void LimitTitle(const char* key, WCHAR* out, int n) {
    if (!strcmp(key, "five_hour"))             wcscpy_s(out, n, TR(L"Текущая сессия", L"Current session"));
    else if (!strcmp(key, "seven_day"))        wcscpy_s(out, n, TR(L"Неделя · все модели", L"Week · all models"));
    else if (!strcmp(key, "seven_day_sonnet")) wcscpy_s(out, n, TR(L"Неделя · Sonnet", L"Week · Sonnet"));
    else if (!strcmp(key, "seven_day_opus"))   wcscpy_s(out, n, TR(L"Неделя · Opus", L"Week · Opus"));
    else if (!strcmp(key, "seven_day_oauth_apps")) wcscpy_s(out, n, TR(L"Неделя · приложения", L"Week · apps"));
    else {
        MultiByteToWideChar(CP_UTF8, 0, key, -1, out, n); out[n - 1] = 0;
        for (WCHAR* p = out; *p; p++) if (*p == L'_') *p = L' ';
    }
}
static void ShortTitle(const char* key, WCHAR* out, int n) {
    if (!strcmp(key, "five_hour"))             wcscpy_s(out, n, TR(L"Сессия", L"Session"));
    else if (!strcmp(key, "seven_day"))        wcscpy_s(out, n, TR(L"Неделя", L"Week"));
    else if (!strcmp(key, "seven_day_sonnet")) wcscpy_s(out, n, L"Sonnet");
    else if (!strcmp(key, "seven_day_opus"))   wcscpy_s(out, n, L"Opus");
    else LimitTitle(key, out, n);
}
static void ResetText(LONGLONG reset, WCHAR* out, int n) {
    out[0] = 0;
    if (!reset) return;
    LONGLONG d = (reset - NowUtc()) / 10000000LL;     // seconds
    if (d <= 0) { wcscpy_s(out, n, TR(L"сброс сейчас", L"resets now")); return; }
    LONGLONG mins = (d + 59) / 60;
    if (mins < 60) { swprintf_s(out, n, TR(L"сброс через %lld мин", L"resets in %lld min"), mins); return; }
    if (mins < 24 * 60) { swprintf_s(out, n, TR(L"сброс через %lld ч %lld мин", L"resets in %lld h %lld min"), mins / 60, mins % 60); return; }
    FILETIME fu = { (DWORD)reset, (DWORD)(reset >> 32) }, fl; SYSTEMTIME st;
    FileTimeToLocalFileTime(&fu, &fl); FileTimeToSystemTime(&fl, &st);
    static const WCHAR* dn[7] = { TR(L"вс", L"Sun"), TR(L"пн", L"Mon"), TR(L"вт", L"Tue"), TR(L"ср", L"Wed"), TR(L"чт", L"Thu"), TR(L"пт", L"Fri"), TR(L"сб", L"Sat") };
    swprintf_s(out, n, TR(L"сброс %s, %02u:%02u", L"resets %s %02u:%02u"), dn[st.wDayOfWeek % 7], st.wHour, st.wMinute);
}
static int Used(const Limit* l) { int v = (int)(l->util + 0.5); return v < 0 ? 0 : v > 100 ? 100 : v; }
static int Left(const Limit* l) { return 100 - Used(l); }
static Color LevelColor(int left) { return left <= 10 ? T.critical : left <= 25 ? T.caution : T.accent; }

static const WCHAR* StatusShort(int st) {
    switch (st) {
    case ST_NOCREDS: return TR(L"Нет входа в Claude Code", L"Not signed in to Claude Code");
    case ST_AUTH:    return TR(L"Вход истёк", L"Sign-in expired");
    case ST_NET:     return TR(L"Нет соединения", L"No connection");
    case ST_RATE:    return TR(L"Сервер просит подождать", L"Server asks to wait");
    case ST_PARSE:   return TR(L"Неизвестный формат ответа", L"Unknown response format");
    case ST_HTTP:    return TR(L"Ошибка сервера", L"Server error");
    }
    return L"";
}

// ───────────────────────────── layout / rendering ────────────────────
static int Cards() { return g_haveData && g_data.n > 0 ? g_data.n : 1; }
static int BaseH() { return CARD_Y + Cards() * (CARD_H + CARD_GAP) - CARD_GAP + 6 + FOOT_H; }
static void SetR(int id, int x, int y, int w, int h) { g_rc[id] = { S(x), S(y), S(x + w), S(y + h) }; }
static void Layout() {
    int bh = BaseH();
    SetR(H_MORE, BASE_W - 42, 8, 30, 30);
    SetR(H_REFRESH, BASE_W - 42, bh - FOOT_H + 3, 30, 30);
    g_w = S(BASE_W); g_h = S(bh);
}
static int HitTest(POINT p) {
    for (int i = 0; i < H_COUNT; i++) if (PtInRect(&g_rc[i], p)) return i;
    return H_NONE;
}
static BOOL Hv(int id) { return g_hover == id && (g_press == H_NONE || g_press == id); }
static BOOL Pr(int id) { return g_press == id && g_hover == id; }

static void Bar(Graphics& g, REAL x, REAL y, REAL w, REAL frac, const Color& c) {
    REAL h = D(6);
    FillRR(g, RectF(x, y, w, h), h / 2, T.dark ? Color(0x26, 255, 255, 255) : Color(0x14, 0, 0, 0));
    if (frac > 0) {
        if (frac > 1) frac = 1;
        REAL fw = w * frac; if (fw < h) fw = h;
        FillRR(g, RectF(x, y, fw, h), h / 2, c);
    }
}

static void PaintTo(Graphics& g) {
    const int W = BASE_W, BH = BaseH();
    WCHAR buf[160];

    // ── header
    DrawAppMark(g, D(14), D(14), D(16));
    FText(g, L"Mensura", g_fStrong, T.text, RectF(D(38), 0, D(120), D(HDR_H)));
    REAL tw = FTextW(g, L"Mensura", g_fStrong);
    FText(g, TR(L"·  лимиты Claude", L"·  Claude limits"), g_fCaption, T.text3, RectF(D(38) + tw + D(8), 0, D(150), D(HDR_H)));
    const WCHAR* plan = g_data.plan[0] ? g_data.plan : NULL;
    if (plan) {
        REAL pw = FTextW(g, plan, g_fCaption) + D(14);
        RectF pr(D(W - 48) - pw, D(12), pw, D(22));
        FillRR(g, pr, D(11), T.ctrl);
        StrokeRR(g, pr, D(11), T.ctrlStroke);
        FText(g, plan, g_fCaption, T.text2, pr, AL_CENTER);
    }
    RectF mr = RF(g_rc[H_MORE]);
    if (Hv(H_MORE)) FillRR(g, mr, D(4), Pr(H_MORE) ? T.subtlePress : T.subtleHover);
    FGlyph(g, GL_MORE, D(14), T.text2, mr);

    // ── cards
    if (g_haveData && g_data.n > 0) {
        for (int i = 0; i < g_data.n; i++) {
            const Limit* l = &g_data.lim[i];
            REAL x = D(12), y = D(CARD_Y + i * (CARD_H + CARD_GAP)), w = D(W - 24);
            FCard(g, RectF(x, y, w, D(CARD_H)));
            LimitTitle(l->key, buf, 64);
            FText(g, buf, g_fStrong, T.text, RectF(x + D(14), y + D(8), w - D(28), D(22)), AL_LEFT, StringTrimmingEllipsisCharacter);
            ResetText(l->reset, buf, 64);
            FText(g, buf, g_fCaption, T.text3, RectF(x + D(14), y + D(8), w - D(28), D(22)), AL_RIGHT);

            int left = Left(l), used = Used(l);
            int big = g_showLeft ? left : used;
            swprintf_s(buf, L"%d %%", big);
            Color vc = left <= 10 ? T.critical : T.text;
            FText(g, buf, g_fSubtitle, vc, RectF(x + D(14), y + D(30), D(90), D(28)));
            REAL bw = FTextW(g, buf, g_fSubtitle);
            FText(g, g_showLeft ? TR(L"осталось", L"left") : TR(L"израсходовано", L"used"), g_fCaption, T.text2, RectF(x + D(14) + bw + D(6), y + D(33), D(120), D(26)));
            swprintf_s(buf, g_showLeft ? TR(L"израсходовано %d %%", L"used %d %%") : TR(L"осталось %d %%", L"left %d %%"), g_showLeft ? used : left);
            FText(g, buf, g_fCaption, T.text3, RectF(x + D(14), y + D(33), w - D(28), D(26)), AL_RIGHT);
            Bar(g, x + D(14), y + D(CARD_H - 16), w - D(28), (REAL)(g_showLeft ? left : used) / 100.0f, LevelColor(left));
        }
    } else {
        REAL x = D(12), y = D(CARD_Y), w = D(W - 24);
        FCard(g, RectF(x, y, w, D(CARD_H)));
        const WCHAR *t1 = TR(L"Загрузка…", L"Loading…"), *t2 = TR(L"Получаю данные о лимитах", L"Getting your usage limits");
        switch (g_status) {
        case ST_NOCREDS: t1 = TR(L"Не найден вход в Claude Code", L"No Claude Code sign-in found"); t2 = TR(L"Войдите в Claude Code командой /login", L"Sign in to Claude Code with /login"); break;
        case ST_AUTH:    t1 = TR(L"Срок входа истёк", L"Sign-in has expired");             t2 = TR(L"Откройте Claude Code — вход обновится сам", L"Open Claude Code to renew the sign-in"); break;
        case ST_NET:     t1 = TR(L"Нет соединения с сервером", L"Cannot reach the server");    t2 = TR(L"Повторю попытку через минуту", L"Will retry in a minute"); break;
        case ST_RATE:    t1 = TR(L"Сервер просит подождать", L"Server asks to wait");      t2 = TR(L"Повторю попытку через несколько минут", L"Will retry in a few minutes"); break;
        case ST_PARSE:   t1 = TR(L"Неизвестный формат ответа", L"Unknown response format");    t2 = TR(L"Возможно, Anthropic изменили API", L"Anthropic may have changed the API"); break;
        case ST_HTTP:    swprintf_s(buf, TR(L"Ошибка сервера (%lu)", L"Server error (%lu)"), g_httpCode); t1 = buf; t2 = TR(L"Повторю попытку позже", L"Will retry later"); break;
        }
        BOOL err = g_status != ST_NONE && g_status != ST_OK;
        FGlyph(g, GL_INFO, D(16), err ? T.caution : T.text2, RectF(x + D(12), y + D(14), D(20), D(22)));
        FText(g, t1, g_fStrong, T.text, RectF(x + D(42), y + D(14), w - D(54), D(22)), AL_LEFT, StringTrimmingEllipsisCharacter);
        FText(g, t2, g_fCaption, T.text2, RectF(x + D(42), y + D(38), w - D(54), D(20)), AL_LEFT, StringTrimmingEllipsisCharacter);
    }

    // ── footer
    REAL fy = D(BH - FOOT_H);
    FFillRect(g, D(12), fy, D(W - 24), 1.0f, T.divider);
    Color fc = T.text3;
    if (g_fetching) wcscpy_s(buf, TR(L"Обновление…", L"Updating…"));
    else if (g_haveData && g_status != ST_OK && g_status != ST_NONE) {
        swprintf_s(buf, TR(L"%s · данные от %02u:%02u", L"%s · data from %02u:%02u"), StatusShort(g_status), g_dataTime.wHour, g_dataTime.wMinute);
        fc = T.caution;
    } else if (g_haveData) swprintf_s(buf, TR(L"Обновлено в %02u:%02u%s", L"Updated at %02u:%02u%s"), g_dataTime.wHour, g_dataTime.wMinute, g_demo ? TR(L" · демо", L" · demo") : L"");
    else buf[0] = 0;
    FText(g, buf, g_fCaption, fc, RectF(D(20), fy + D(1), D(W - 70), D(FOOT_H - 1)), AL_LEFT, StringTrimmingEllipsisCharacter);
    RectF rr = RF(g_rc[H_REFRESH]);
    if (Hv(H_REFRESH)) FillRR(g, rr, D(4), Pr(H_REFRESH) ? T.subtlePress : T.subtleHover);
    FGlyph(g, GL_REFRESH, D(13), g_fetching ? T.text3 : T.text2, rr);
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
static void Relayout() {
    int oldH = g_h;
    Layout();
    if (g_hwnd && g_h != oldH) {
        SetWindowPos(g_hwnd, NULL, 0, 0, g_w, g_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        CanvasFree(g_cv);
    }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
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

// ───────────────────────────── tray, warnings, menu ──────────────────
static void TrayAdd() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_iconSm;
    wcscpy_s(g_nid.szTip, TR(L"Mensura — лимиты Claude", L"Mensura — Claude limits"));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayTip() {
    WCHAR tip[128] = L"Mensura", t[48], line[64];
    if (g_haveData) {
        for (int i = 0; i < g_data.n && i < 3; i++) {
            ShortTitle(g_data.lim[i].key, t, 48);
            swprintf_s(line, TR(L"\n%s: осталось %d %%", L"\n%s: %d %% left"), t, Left(&g_data.lim[i]));
            if (wcslen(tip) + wcslen(line) < 127) wcscat_s(tip, line);
        }
    } else if (g_status > ST_OK) { wcscat_s(tip, L"\n"); wcscat_s(tip, StatusShort(g_status)); }
    wcscpy_s(g_nid.szTip, tip);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void Notify(const WCHAR* title, const WCHAR* text) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcscpy_s(n.szInfoTitle, title);
    wcscpy_s(n.szInfo, text);
    n.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}
// One warning per limit window at 80 % and 95 % used. The first data after start only primes the state.
static void CheckWarnings() {
    for (int i = 0; i < g_data.n; i++) {
        const Limit* l = &g_data.lim[i];
        int lvl = l->util >= 95 ? 2 : l->util >= 80 ? 1 : 0;
        Warned* w = NULL;
        for (int k = 0; k < g_warnedN; k++) if (!strcmp(g_warned[k].key, l->key)) { w = &g_warned[k]; break; }
        if (!w) {
            if (g_warnedN >= MAX_LIM) continue;
            w = &g_warned[g_warnedN++]; strcpy_s(w->key, l->key); w->reset = l->reset; w->level = g_warnInit ? 0 : lvl;
        }
        if (w->reset != l->reset) { w->reset = l->reset; w->level = 0; }
        if (lvl > w->level) {
            w->level = lvl;
            if (g_warn && g_warnInit) {
                WCHAR t[64], r[64], msg[200];
                LimitTitle(l->key, t, 64); ResetText(l->reset, r, 64);
                swprintf_s(msg, TR(L"%s: осталось %d %%%s%s", L"%s: %d %% left%s%s"), t, Left(l), r[0] ? L"\n" : L"", r);
                Notify(lvl == 2 ? TR(L"Лимит Claude почти исчерпан", L"Claude limit almost used up") : TR(L"Лимит Claude заканчивается", L"Claude limit running low"), msg);
            }
        }
    }
    g_warnInit = TRUE;
}
static void OnData(Usage* u) {
    InterlockedExchange(&g_fetching, 0);
    g_status = u->status; g_httpCode = u->http;
    ULONGLONG now = GetTickCount64();
    if (u->status == ST_OK) {
        g_data = *u; g_haveData = TRUE; GetLocalTime(&g_dataTime);
        g_badTok = 0;
        g_nextFetch = now + (ULONGLONG)g_everyMin * 60000;
        CheckWarnings();
    } else {
        if (u->status == ST_AUTH && u->tokHash) g_badTok = u->tokHash;
        if (u->plan[0]) wcscpy_s(g_data.plan, u->plan);
        g_nextFetch = now + (u->status == ST_RATE ? 10 * 60000 : u->status == ST_HTTP || u->status == ST_PARSE || u->status == ST_AUTH ? 5 * 60000 : 60000);
    }
    Free(u);
    TrayTip();
    Relayout();
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
    static const int mins[4] = { 1, 2, 5, 10 };
    for (int i = 0; i < 4; i++) {
        WCHAR s[32]; swprintf_s(s, TR(L"Каждые %d мин", L"Every %d min"), mins[i]);
        AppendMenuW(every, MF_STRING | (g_everyMin == mins[i] ? MF_CHECKED : 0), ID_EVERY_1 + i, s);
    }
    AppendMenuW(m, MF_STRING, ID_REFRESH, TR(L"Обновить сейчас", L"Refresh now"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)every, TR(L"Частота обновления", L"Refresh interval"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_showLeft ? MF_CHECKED : 0), ID_MODE_LEFT, TR(L"Показывать остаток", L"Show remaining"));
    AppendMenuW(m, MF_STRING | (!g_showLeft ? MF_CHECKED : 0), ID_MODE_USED, TR(L"Показывать израсходованное", L"Show used"));
    AppendMenuW(m, MF_STRING | (g_warn ? MF_CHECKED : 0), ID_WARN, TR(L"Предупреждать при 80 % и 95 %", L"Warn at 80 % and 95 %"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_acrylic ? MF_CHECKED : 0), ID_ACRYLIC, TR(L"Прозрачный фон", L"Transparent background"));
    AppendMenuW(m, MF_STRING | (g_topmost ? MF_CHECKED : 0), ID_TOPMOST, TR(L"Поверх всех окон", L"Always on top"));
    AppendMenuW(m, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, TR(L"Запускать вместе с Windows", L"Start with Windows"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_SHOWHIDE, g_visible ? TR(L"Скрыть виджет", L"Hide widget") : TR(L"Показать виджет", L"Show widget"));
    AppendLangMenu(m, REG_KEY);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, TR(L"Mensura 1.1 · разработчик Sallador", L"Mensura 1.1 · by Sallador"));
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
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        TrayAdd(); TrayTip(); if (!g_topmost) ApplyTopmost();
        return 0;
    }
    switch (msg) {
    case WM_NCCALCSIZE: if (w) return 0; break;
    case WM_NCHITTEST:  return HTCLIENT;
    case WM_NCACTIVATE: return DefWindowProcW(h, msg, TRUE, l);
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
        Render(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        g_tick++;
        ULONGLONG now = GetTickCount64();
        BOOL resetPassed = FALSE;
        if (g_haveData) {
            LONGLONG u = NowUtc();
            for (int i = 0; i < g_data.n; i++)
                if (g_data.lim[i].reset && g_data.lim[i].reset + 30 * 10000000LL < u) resetPassed = TRUE;
        }
        if (now >= g_nextFetch || (resetPassed && g_status == ST_OK)) {
            g_nextFetch = now + 60000;                    // guard; OnData sets the real schedule
            Fetch();
        }
        if (g_visible) InvalidateRect(h, NULL, FALSE);
        if (g_tick % 20 == 2) SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        return 0;
    }
    case WM_APP_DATA: OnData((Usage*)l); return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMRESUMEAUTOMATIC) g_nextFetch = GetTickCount64() + 10000;
        break;
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
            } else if (id == H_REFRESH) { g_badTok = 0; Fetch(); }
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
        if (id >= ID_LANG_AUTO && id <= ID_LANG_EN) { RestartForLang(REG_KEY, id - ID_LANG_AUTO, g_demo ? L"/demo" : NULL); g_exiting = TRUE; DestroyWindow(h); return 0; }
        if (id == ID_TOPMOST)        { g_topmost = !g_topmost; RegSetDw(L"TopMost", g_topmost); ApplyTopmost(); }
        else if (id == ID_AUTOSTART) SetAutostart(!IsAutostart());
        else if (id == ID_ACRYLIC)   { g_acrylic = !g_acrylic; RegSetDw(L"Acrylic", g_acrylic); ApplyBackdrop(); InvalidateRect(h, NULL, FALSE); }
        else if (id == ID_MODE_LEFT || id == ID_MODE_USED) { g_showLeft = id == ID_MODE_LEFT; RegSetDw(L"ShowLeft", g_showLeft); InvalidateRect(h, NULL, FALSE); }
        else if (id == ID_WARN)      { g_warn = !g_warn; RegSetDw(L"Warn", g_warn); }
        else if (id == ID_REFRESH)   { g_badTok = 0; Fetch(); }
        else if (id >= ID_EVERY_1 && id <= ID_EVERY_10) {
            static const int mins[4] = { 1, 2, 5, 10 };
            g_everyMin = mins[id - ID_EVERY_1]; RegSetDw(L"EveryMin", g_everyMin);
            if (g_status == ST_OK) g_nextFetch = GetTickCount64() + (ULONGLONG)g_everyMin * 60000;
        }
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
        else PostThreadMessageW(GetCurrentThreadId(), WM_APP_RECREATE, 0, 0);   // owner (explorer) died — rebuild
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
                             havePos ? x : 0, havePos ? y : 0, BASE_W, 200, owner, NULL, g_hInst, NULL);
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
    TrayAdd(); TrayTip();
    if (g_visible) ShowWindow(h, SW_SHOWNOACTIVATE);
    SetTimer(h, TIMER_ID, 15000, NULL);
    Fetch();
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmd, int) {
    g_hInst = hInst;
    InitLang(REG_KEY);
    g_demo = cmd && wcsstr(cmd, L"/demo") != NULL;
    g_mutexLang = CreateMutexW(NULL, TRUE, g_demo ? L"Local\\Mensura_Sallador_Demo" : L"Local\\Mensura_Sallador_SingleInstance");
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

    g_topmost  = RegGetDw(L"TopMost", 0) != 0;
    g_acrylic  = RegGetDw(L"Acrylic", 1) != 0;
    g_showLeft = RegGetDw(L"ShowLeft", 1) != 0;
    g_warn     = RegGetDw(L"Warn", 1) != 0;
    g_everyMin = (int)RegGetDw(L"EveryMin", 5);
    if (g_everyMin != 1 && g_everyMin != 2 && g_everyMin != 5 && g_everyMin != 10) g_everyMin = 5;
    if (!g_demo && IsAutostart()) SetAutostart(TRUE);   // refresh path in case the exe was moved

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
    GdiplusShutdown(gtoken);
    return 0;
}
