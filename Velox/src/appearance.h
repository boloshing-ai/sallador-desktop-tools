// appearance.h — background transparency, text shadow and text colour for the desktop widgets.
// Developer: Sallador. Include after fluent.h and lang.h. Settings live in the app's registry key:
//   BgTransparency = 0…90 (%)   TextShadow = 0/1   TextColor = 0 (follow the theme) or 0x01RRGGBB
//
// Background transparency makes only the background see-through; the text stays fully opaque.
// It uses a per-pixel-alpha layered window (UpdateLayeredWindow), so the Acrylic blur is off then.
#pragma once
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

#define ID_TRANSP_BASE  160          // 160…167: 0, 20, 40, 50, 60, 70, 80, 90 %
#define ID_SHADOW       168
#define ID_COLOR_BASE   170          // 170…178 presets, 179 = custom
#define ID_COLOR_CUSTOM 179

static int   g_bgTransp;             // 0 = normal window (Acrylic / solid)
static BOOL  g_shadowOpt = TRUE;
static DWORD g_textColor;            // 0 = theme colours
static HWND  g_layerHwnd;            // the widget window that uses the layered mode
static COLORREF g_customColors[16];

static const int kTransp[8] = { 0, 20, 40, 50, 60, 70, 80, 90 };
struct ColorPreset { const WCHAR *ru, *en; DWORD rgb; };
static const ColorPreset kColors[9] = {
    { L"Как в теме Windows", L"Same as Windows theme", 0 },
    { L"Белый",      L"White",  0xFFFFFF }, { L"Голубой",   L"Light blue", 0x60CDFF },
    { L"Зелёный",    L"Green",  0x6CCB5F }, { L"Жёлтый",    L"Yellow",     0xFCE100 },
    { L"Оранжевый",  L"Orange", 0xFF9A3D }, { L"Розовый",   L"Pink",       0xFF6FB5 },
    { L"Фиолетовый", L"Violet", 0xB79CFF }, { L"Чёрный",    L"Black",      0x000000 } };

static void LoadAppearance(const WCHAR* key) {
    g_bgTransp = (int)ReadDw(key, L"BgTransparency", 0);
    if (g_bgTransp < 0 || g_bgTransp > 90) g_bgTransp = 0;
    g_shadowOpt = ReadDw(key, L"TextShadow", 1) != 0;
    g_textColor = ReadDw(key, L"TextColor", 0);
}
static void SaveAppearanceDw(const WCHAR* key, const WCHAR* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); RegCloseKey(k);
    }
}
// Called from LoadTheme (via g_themeHook): replace the text colours with the chosen one.
static void ApplyTextColor() {
    if (!(g_textColor & 0x01000000)) return;
    BYTE r = (BYTE)(g_textColor >> 16), g = (BYTE)(g_textColor >> 8), b = (BYTE)g_textColor;
    T.text = Color(255, r, g, b); T.text2 = Color(0xD0, r, g, b); T.text3 = Color(0x99, r, g, b);
}
// Switch the widget between the normal DWM window and the layered (see-through background) mode.
// Call at the end of the app's ApplyBackdrop().
static void ApplyLayered(HWND h) {
    if (!h) return;
    g_layerHwnd = h;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (g_bgTransp > 0) {
        int none = 1; DwmSetWindowAttribute(h, 38, &none, sizeof(none));
        MARGINS m = { 0, 0, 0, 0 }; DwmExtendFrameIntoClientArea(h, &m);
        if (!(ex & WS_EX_LAYERED)) SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    } else if (ex & WS_EX_LAYERED) {
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
}
// Start a frame: background for the given window. Also decides whether text gets a shadow.
static void ClearBg(Graphics& g, HWND h, int w, int hh) {
    if (g_bgTransp > 0 && h == g_layerHwnd) {
        g.Clear(Color(0, 0, 0, 0));
        BYTE a = (BYTE)((100 - g_bgTransp) * 255 / 100);
        RectF r(0.5f, 0.5f, (REAL)w - 1, (REAL)hh - 1);
        FillRR(g, r, D(8), Color(a, T.solidBg.GetR(), T.solidBg.GetG(), T.solidBg.GetB()));
        BYTE sa = (BYTE)(a / 3 + 0x10);
        GraphicsPath p; RRPath(p, r, D(8));
        Pen pen(T.dark ? Color(sa, 255, 255, 255) : Color(sa, 0, 0, 0), 1.0f); g.DrawPath(&pen, &p);
        g_textShadow = g_shadowOpt;
    } else {
        g.Clear(T.backdrop ? Color(0, 0, 0, 0) : T.solidBg);
        g_textShadow = FALSE;
    }
}
// Finish a frame: BitBlt for a normal window, UpdateLayeredWindow in the layered mode.
static void PresentFrame(HWND h, HDC hdc, Canvas& cv, int w, int hh) {
    if (g_bgTransp > 0 && h == g_layerHwnd) {
        RECT r; GetWindowRect(h, &r);
        POINT dst = { r.left, r.top }, src = { 0, 0 }; SIZE sz = { w, hh };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(h, NULL, &dst, &sz, cv.dc, &src, 0, &bf, ULW_ALPHA);
    } else {
        BitBlt(hdc, 0, 0, w, hh, cv.dc, 0, 0, SRCCOPY);
    }
}
static void AppendAppearanceMenu(HMENU m) {
    HMENU op = CreatePopupMenu(), col = CreatePopupMenu();
    for (int i = 0; i < 8; i++) {
        WCHAR s[64];
        if (!kTransp[i]) wcscpy_s(s, TR(L"Без прозрачности", L"None"));
        else swprintf_s(s, L"%d %%", kTransp[i]);
        AppendMenuW(op, MF_STRING | (g_bgTransp == kTransp[i] ? MF_CHECKED : 0), ID_TRANSP_BASE + i, s);
        if (!i) AppendMenuW(op, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(op, MF_SEPARATOR, 0, NULL);
    AppendMenuW(op, MF_STRING | (g_shadowOpt ? MF_CHECKED : 0) | (g_bgTransp ? 0 : MF_GRAYED), ID_SHADOW,
                TR(L"Тень под текстом (читается на любых обоях)", L"Text shadow (readable on any wallpaper)"));
    BOOL preset = FALSE;
    for (int i = 0; i < 9; i++) {
        BOOL on = i == 0 ? !(g_textColor & 0x01000000) : g_textColor == (0x01000000 | kColors[i].rgb);
        if (on) preset = TRUE;
        AppendMenuW(col, MF_STRING | (on ? MF_CHECKED : 0), ID_COLOR_BASE + i, TR(kColors[i].ru, kColors[i].en));
        if (i == 0) AppendMenuW(col, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(col, MF_SEPARATOR, 0, NULL);
    AppendMenuW(col, MF_STRING | (!preset ? MF_CHECKED : 0), ID_COLOR_CUSTOM, TR(L"Другой цвет…", L"Other colour…"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)op, TR(L"Прозрачность фона (текст остаётся ярким)", L"Background transparency (text stays bright)"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)col, TR(L"Цвет текста", L"Text colour"));
}
// Handles the menu commands above. Returns TRUE when it did; the caller then calls its
// ApplyBackdrop() (which ends with ApplyLayered) and repaints.
static BOOL HandleAppearanceCmd(HWND h, int id, const WCHAR* key) {
    if (id >= ID_TRANSP_BASE && id < ID_TRANSP_BASE + 8) {
        g_bgTransp = kTransp[id - ID_TRANSP_BASE];
        SaveAppearanceDw(key, L"BgTransparency", g_bgTransp);
        return TRUE;
    }
    if (id == ID_SHADOW) { g_shadowOpt = !g_shadowOpt; SaveAppearanceDw(key, L"TextShadow", g_shadowOpt); return TRUE; }
    if (id >= ID_COLOR_BASE && id <= ID_COLOR_CUSTOM) {
        if (id == ID_COLOR_CUSTOM) {
            CHOOSECOLORW cc = { sizeof(cc) };
            cc.hwndOwner = h; cc.lpCustColors = g_customColors; cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            DWORD cur = g_textColor & 0xFFFFFF;
            cc.rgbResult = RGB(cur >> 16, (cur >> 8) & 0xFF, cur & 0xFF);
            if (!ChooseColorW(&cc)) return TRUE;
            g_textColor = 0x01000000 | (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
        } else {
            int i = id - ID_COLOR_BASE;
            g_textColor = i == 0 ? 0 : 0x01000000 | kColors[i].rgb;
        }
        SaveAppearanceDw(key, L"TextColor", g_textColor);
        LoadTheme();                                   // re-reads theme colours, then the hook applies ours
        return TRUE;
    }
    return FALSE;
}
