// lang.h — Russian / English interface. Developer: Sallador.
// The language is chosen once per run: HKCU\<app key>\Language = 0 (follow Windows), 1 (Russian), 2 (English).
// "Follow Windows" picks Russian for Russian, Ukrainian, Belarusian and Kazakh Windows, English otherwise.
#pragma once
#include <windows.h>

static BOOL g_en;
static HANDLE g_mutexLang;            // single-instance mutex, released before a language restart
#define TR(ru, en) (g_en ? (en) : (ru))

enum { LANG_AUTO = 0, LANG_RU = 1, LANG_EN = 2 };
#define ID_LANG_AUTO 140
#define ID_LANG_RU   141
#define ID_LANG_EN   142

static int LangSetting(const WCHAR* regKey) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, regKey, L"Language", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS || v > 2) v = LANG_AUTO;
    return (int)v;
}
static void InitLang(const WCHAR* regKey) {
    int s = LangSetting(regKey);
    if (s == LANG_AUTO) {
        WORD p = PRIMARYLANGID(GetUserDefaultUILanguage());
        g_en = !(p == LANG_RUSSIAN || p == LANG_UKRAINIAN || p == LANG_BELARUSIAN || p == LANG_KAZAK);
    } else g_en = s == LANG_EN;
}
static void SaveLang(const WCHAR* regKey, int s) {
    HKEY k; DWORD v = (DWORD)s;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, regKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"Language", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}
// "Язык / Language" submenu; the item for the current setting is checked.
static void AppendLangMenu(HMENU m, const WCHAR* regKey) {
    int s = LangSetting(regKey);
    HMENU sub = CreatePopupMenu();
    AppendMenuW(sub, MF_STRING | (s == LANG_AUTO ? MF_CHECKED : 0), ID_LANG_AUTO, TR(L"Как в Windows", L"Same as Windows"));
    AppendMenuW(sub, MF_STRING | (s == LANG_RU ? MF_CHECKED : 0), ID_LANG_RU, L"Русский");
    AppendMenuW(sub, MF_STRING | (s == LANG_EN ? MF_CHECKED : 0), ID_LANG_EN, L"English");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, TR(L"Язык · Language", L"Language · Язык"));
}
// Save the new setting and start a fresh copy of the app (the caller then exits).
// The single-instance mutex must be released by the caller before calling this.
static void RestartForLang(const WCHAR* regKey, int s, const WCHAR* args) {
    SaveLang(regKey, s);
    if (g_mutexLang) { ReleaseMutex(g_mutexLang); CloseHandle(g_mutexLang); g_mutexLang = NULL; }
    WCHAR self[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf_s(cmd, L"\"%s\" %s", self, args ? args : L"");
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}
