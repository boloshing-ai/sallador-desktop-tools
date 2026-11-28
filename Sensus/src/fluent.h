// fluent.h — small Windows 11 (Fluent Design) drawing kit for plain Win32 apps, built on GDI+.
// Mica / Acrylic backdrop, rounded corners, system light/dark theme and accent colour,
// Segoe UI Variable text, Segoe Fluent Icons glyphs, Win11-style controls.
// Developer: Sallador.
#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <math.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
using namespace Gdiplus;

static UINT g_dpi = 96;
static inline int  S(int v)    { return MulDiv(v, (int)g_dpi, 96); }
static inline REAL D(double v) { return (REAL)(v * g_dpi / 96.0); }

#define PERSONALIZE_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define BACKDROP_MICA    2
#define BACKDROP_ACRYLIC 3

// Segoe Fluent Icons code points
#define GL_MOON      0xE708
#define GL_CLOSE     0xE8BB
#define GL_CANCEL    0xE711
#define GL_MINIMIZE  0xE921
#define GL_POWER     0xE7E8
#define GL_GAME      0xE7FC
#define GL_PLAY      0xE768
#define GL_PAUSE     0xE769
#define GL_KEYBOARD  0xE765
#define GL_ADD       0xE710
#define GL_REMOVE    0xE738
#define GL_CHECK     0xE73E
#define GL_FOLDER    0xE8B7
#define GL_SETTINGS  0xE713
#define GL_MORE      0xE712
#define GL_CHEVRON_D 0xE70D
#define GL_REFRESH   0xE72C
#define GL_INFO      0xE946
#define GL_DOWN      0xE896
#define GL_UP        0xE898
#define GL_SHIELD    0xEA18

// ───────────────────────────── theme ─────────────────────────────────
struct FluentTheme {
    BOOL  dark, backdrop;
    Color solidBg, text, text2, text3, onAccent;
    Color card, cardStroke, ctrl, ctrlHover, ctrlPress, ctrlStroke;
    Color subtleHover, subtlePress, strong, altFill, altFillHover, divider, footer;
    Color accent, accentHover, accentPress;
    Color success, successBg, caution, critical;
};
static FluentTheme T;
static void (*g_themeHook)(void);
static BOOL g_textShadow;                // soft shadow under text (see-through widget backgrounds)       // called at the end of LoadTheme (custom text colour)

static DWORD ReadDw(const WCHAR* key, const WCHAR* name, DWORD def) {
    DWORD v = 0, sz = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS ? v : def;
}

static void LoadTheme() {
    T.dark = ReadDw(PERSONALIZE_KEY, L"AppsUseLightTheme", 1) == 0;

    // Accent palette: Light3, Light2, Light1, Base, Dark1, Dark2, Dark3 (RGBA each).
    // Win11 uses Light2 for accent fills in dark mode and Dark1 in light mode.
    BYTE pal[32] = {}; DWORD sz = sizeof(pal);
    BOOL havePal = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                                L"AccentPalette", RRF_RT_REG_BINARY, NULL, pal, &sz) == ERROR_SUCCESS && sz >= 28;
    int i = T.dark ? 1 : 4;
    BYTE r = havePal ? pal[i * 4]     : (T.dark ? 0x60 : 0x00);
    BYTE g = havePal ? pal[i * 4 + 1] : (T.dark ? 0xCD : 0x5F);
    BYTE b = havePal ? pal[i * 4 + 2] : (T.dark ? 0xFF : 0xB8);
    T.accent = Color(255, r, g, b); T.accentHover = Color(0xE6, r, g, b); T.accentPress = Color(0xCC, r, g, b);

    if (T.dark) {
        T.solidBg = Color(255, 0x20, 0x20, 0x20);
        T.text = Color(255, 255, 255, 255); T.text2 = Color(0xC5, 255, 255, 255); T.text3 = Color(0x87, 255, 255, 255);
        T.onAccent = Color(255, 0, 0, 0);
        T.card = Color(0x0D, 255, 255, 255); T.cardStroke = Color(0x19, 0, 0, 0);
        T.ctrl = Color(0x0F, 255, 255, 255); T.ctrlHover = Color(0x15, 255, 255, 255); T.ctrlPress = Color(0x08, 255, 255, 255);
        T.ctrlStroke = Color(0x17, 255, 255, 255);
        T.subtleHover = Color(0x0F, 255, 255, 255); T.subtlePress = Color(0x0A, 255, 255, 255);
        T.strong = Color(0x8B, 255, 255, 255); T.altFill = Color(0x19, 0, 0, 0); T.altFillHover = Color(0x0F, 255, 255, 255);
        T.divider = Color(0x15, 255, 255, 255); T.footer = Color(0x33, 0, 0, 0);
        T.success = Color(255, 0x6C, 0xCB, 0x5F); T.successBg = Color(255, 0x39, 0x3D, 0x1B);
        T.caution = Color(255, 0xFC, 0xE1, 0x00); T.critical = Color(255, 0xFF, 0x99, 0xA4);
    } else {
        T.solidBg = Color(255, 0xF3, 0xF3, 0xF3);
        T.text = Color(0xE4, 0, 0, 0); T.text2 = Color(0x9E, 0, 0, 0); T.text3 = Color(0x72, 0, 0, 0);
        T.onAccent = Color(255, 255, 255, 255);
        T.card = Color(0xB3, 255, 255, 255); T.cardStroke = Color(0x0F, 0, 0, 0);
        T.ctrl = Color(0xB3, 255, 255, 255); T.ctrlHover = Color(0x80, 0xF9, 0xF9, 0xF9); T.ctrlPress = Color(0x4D, 0xF9, 0xF9, 0xF9);
        T.ctrlStroke = Color(0x17, 0, 0, 0);
        T.subtleHover = Color(0x09, 0, 0, 0); T.subtlePress = Color(0x06, 0, 0, 0);
        T.strong = Color(0x72, 0, 0, 0); T.altFill = Color(0x06, 0, 0, 0); T.altFillHover = Color(0x0F, 0, 0, 0);
        T.divider = Color(0x0F, 0, 0, 0); T.footer = Color(0x0A, 0, 0, 0);
        T.success = Color(255, 0x0F, 0x7B, 0x0F); T.successBg = Color(255, 0xDF, 0xF6, 0xDD);
        T.caution = Color(255, 0x9D, 0x5D, 0x00); T.critical = Color(255, 0xC4, 0x2B, 0x1C);
    }
    if (g_themeHook) g_themeHook();
}

// Rounded corners, dark/light frame and Mica / Acrylic backdrop. The window must strip its
// non-client area (WM_NCCALCSIZE -> 0). Returns TRUE when the backdrop is active.
static BOOL ApplyFrame(HWND h, int backdrop) {
    BOOL dark = T.dark;
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int corner = 2 /*DWMWCP_ROUND*/;
    DwmSetWindowAttribute(h, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    MARGINS m = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(h, &m);
    int bt = backdrop;
    T.backdrop = SUCCEEDED(DwmSetWindowAttribute(h, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &bt, sizeof(bt)));
    return T.backdrop;
}

// Context menus follow the app theme (same uxtheme calls Explorer / Notepad use).
static void ApplyMenuTheme() {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int  (WINAPI *PFN_SetMode)(int);
    typedef void (WINAPI *PFN_Flush)(void);
    PFN_SetMode setMode = (PFN_SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    PFN_Flush   flush   = (PFN_Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(T.dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
}

// ───────────────────────────── fonts ─────────────────────────────────
static FontFamily *g_ffText, *g_ffSemi, *g_ffDisp, *g_ffIcon;
static Font *g_fCaption, *g_fBody, *g_fStrong, *g_fSubtitle;

static FontFamily* PickFamily(const WCHAR* const* names, int n) {
    for (int i = 0; i < n; i++) {
        FontFamily* f = new FontFamily(names[i]);
        if (f->GetLastStatus() == Ok && f->IsAvailable()) return f;
        delete f;
    }
    return new FontFamily(L"Segoe UI");
}
static void FluentInit() {   // call after GdiplusStartup
    static const WCHAR* text[] = { L"Segoe UI Variable Text", L"Segoe UI" };
    static const WCHAR* semi[] = { L"Segoe UI Variable Text Semibold", L"Segoe UI Semibold" };
    static const WCHAR* disp[] = { L"Segoe UI Variable Display Semib", L"Segoe UI Variable Display Semibold", L"Segoe UI Semibold" };
    static const WCHAR* icon[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    g_ffText = PickFamily(text, 2); g_ffSemi = PickFamily(semi, 2);
    g_ffDisp = PickFamily(disp, 3); g_ffIcon = PickFamily(icon, 2);
}
static void FluentFonts() {  // call on start and on every DPI change
    delete g_fCaption; delete g_fBody; delete g_fStrong; delete g_fSubtitle;
    g_fCaption  = new Font(g_ffText, D(12), FontStyleRegular, UnitPixel);
    g_fBody     = new Font(g_ffText, D(14), FontStyleRegular, UnitPixel);
    g_fStrong   = new Font(g_ffSemi, D(14), FontStyleRegular, UnitPixel);
    g_fSubtitle = new Font(g_ffDisp, D(20), FontStyleRegular, UnitPixel);
}

// ───────────────────────────── canvas ────────────────────────────────
struct Canvas { HDC dc; HBITMAP bmp; HGDIOBJ old; void* bits; int w, h; };

static void CanvasFree(Canvas& c) {
    if (c.dc) { SelectObject(c.dc, c.old); DeleteObject(c.bmp); DeleteDC(c.dc); }
    ZeroMemory(&c, sizeof(c));
}
static BOOL CanvasEnsure(Canvas& c, int w, int h) {
    if (c.dc && c.w == w && c.h == h) return TRUE;
    CanvasFree(c);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    c.dc = CreateCompatibleDC(NULL);
    c.bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &c.bits, NULL, 0);
    if (!c.dc || !c.bmp) { if (c.bmp) DeleteObject(c.bmp); if (c.dc) DeleteDC(c.dc); ZeroMemory(&c, sizeof(c)); return FALSE; }
    c.old = SelectObject(c.dc, c.bmp); c.w = w; c.h = h;
    return TRUE;
}

static void Prep(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
}

// ───────────────────────────── primitives ────────────────────────────
static RectF RF(const RECT& r) { return RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)); }

static void RRPath(GraphicsPath& p, const RectF& r, REAL rad) {
    REAL d = rad * 2;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    if (d <= 0) { p.AddRectangle(r); return; }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
static void FillRR(Graphics& g, const RectF& r, REAL rad, const Color& c) {
    if (!c.GetA()) return;
    GraphicsPath p; RRPath(p, r, rad);
    SolidBrush b(c); g.FillPath(&b, &p);
}
static void StrokeRR(Graphics& g, const RectF& r, REAL rad, const Color& c) {
    RectF q(r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1);
    GraphicsPath p; RRPath(p, q, rad - 0.5f);
    Pen pen(c, 1.0f); g.DrawPath(&pen, &p);
}
static void FFillRect(Graphics& g, REAL x, REAL y, REAL w, REAL h, const Color& c) {
    SolidBrush b(c); g.FillRectangle(&b, x, y, w, h);
}
static void FillCircle(Graphics& g, REAL cx, REAL cy, REAL r, const Color& c) {
    SolidBrush b(c); g.FillEllipse(&b, cx - r, cy - r, r * 2, r * 2);
}

enum { AL_LEFT, AL_CENTER, AL_RIGHT };
static void FText(Graphics& g, const WCHAR* s, Font* f, const Color& c, const RectF& r,
                  int al = AL_LEFT, StringTrimming trim = StringTrimmingNone) {
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    sf.SetAlignment(al == AL_CENTER ? StringAlignmentCenter : al == AL_RIGHT ? StringAlignmentFar : StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(trim);
    if (g_textShadow && c.GetA() > 0x40) {
        BOOL light = c.GetR() * 299 + c.GetG() * 587 + c.GetB() * 114 > 128000;
        SolidBrush sh(light ? Color((BYTE)(c.GetA() * 0.75), 0, 0, 0) : Color((BYTE)(c.GetA() * 0.6), 255, 255, 255));
        RectF rs(r.X + 1, r.Y + 1, r.Width, r.Height);
        g.DrawString(s, -1, f, rs, &sf, &sh);
    }
    SolidBrush b(c);
    g.DrawString(s, -1, f, r, &sf, &b);
}
static REAL FTextW(Graphics& g, const WCHAR* s, Font* f) {
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    RectF out; g.MeasureString(s, -1, f, PointF(0, 0), &sf, &out);
    return out.Width;
}
// Icon glyph, centred by its ink bounds inside box.
static void FGlyph(Graphics& g, WCHAR ch, REAL em, const Color& c, const RectF& box) {
    WCHAR s[2] = { ch, 0 };
    GraphicsPath p;
    p.AddString(s, 1, g_ffIcon, FontStyleRegular, em, PointF(0, 0), StringFormat::GenericTypographic());
    RectF b; p.GetBounds(&b);
    if (b.Width <= 0) return;
    Matrix m;
    m.Translate(box.X + (box.Width - b.Width) / 2 - b.X, box.Y + (box.Height - b.Height) / 2 - b.Y);
    p.Transform(&m);
    SolidBrush br(c); g.FillPath(&br, &p);
}

// ───────────────────────────── controls ──────────────────────────────
static void FCard(Graphics& g, const RectF& r) {
    FillRR(g, r, D(8), T.card);
    StrokeRR(g, r, D(8), T.cardStroke);
}
static void FButton(Graphics& g, const RectF& r, const WCHAR* s, BOOL accent, BOOL hover, BOOL press, WCHAR glyph = 0) {
    REAL rad = D(4);
    Color tc;
    if (accent) {
        FillRR(g, r, rad, press ? T.accentPress : hover ? T.accentHover : T.accent);
        tc = T.onAccent;
    } else {
        FillRR(g, r, rad, press ? T.ctrlPress : hover ? T.ctrlHover : T.ctrl);
        StrokeRR(g, r, rad, T.ctrlStroke);
        tc = press ? T.text2 : T.text;
    }
    REAL tw = FTextW(g, s, g_fBody), gw = glyph ? D(16) + D(8) : 0;
    REAL x = r.X + (r.Width - tw - gw) / 2;
    if (glyph) FGlyph(g, glyph, D(16), tc, RectF(x, r.Y, D(16), r.Height));
    FText(g, s, g_fBody, tc, RectF(x + gw, r.Y, tw + D(4), r.Height));
}
static void FToggle(Graphics& g, REAL x, REAL y, BOOL on, BOOL hover) {
    RectF r(x, y, D(40), D(20));
    REAL rad = D(10), k = hover ? D(7) : D(6);
    if (on) {
        FillRR(g, r, rad, hover ? T.accentHover : T.accent);
        FillCircle(g, x + D(30), y + D(10), k, T.onAccent);
    } else {
        FillRR(g, r, rad, hover ? T.altFillHover : T.altFill);
        StrokeRR(g, r, rad, T.strong);
        FillCircle(g, x + D(10), y + D(10), k, T.strong);
    }
}
static void FCheck(Graphics& g, REAL x, REAL y, BOOL on, BOOL hover) {
    RectF r(x, y, D(20), D(20));
    if (on) {
        FillRR(g, r, D(4), hover ? T.accentHover : T.accent);
        FGlyph(g, GL_CHECK, D(12), T.onAccent, r);
    } else {
        FillRR(g, r, D(4), hover ? T.altFillHover : T.altFill);
        StrokeRR(g, r, D(4), T.strong);
    }
}
static void FProgress(Graphics& g, REAL x, REAL y, REAL w, REAL frac, const Color& c) {
    FFillRect(g, x, y + D(1), w, 1.0f, T.strong);
    if (frac > 0) {
        if (frac > 1) frac = 1;
        REAL fw = w * frac; if (fw < D(3)) fw = D(3);
        FillRR(g, RectF(x, y, fw, D(3)), D(1.5), c);
    }
}
// Keycap drawn leftwards from `right`; returns its left edge.
static REAL FKeycap(Graphics& g, REAL right, REAL y, const WCHAR* s) {
    REAL w = FTextW(g, s, g_fCaption) + D(16);
    if (w < D(26)) w = D(26);
    RectF r(right - w, y, w, D(24));
    FillRR(g, r, D(4), T.ctrl);
    StrokeRR(g, r, D(4), T.ctrlStroke);
    FText(g, s, g_fCaption, T.text2, r, AL_CENTER);
    return r.X;
}

// ───────────────────────────── app mark ──────────────────────────────
// Crescent = part of circle 1 outside circle 2, built from two arcs (antialiased).
static void CrescentPath(GraphicsPath& p, REAL x1, REAL y1, REAL r1, REAL x2, REAL y2, REAL r2) {
    REAL dx = x2 - x1, dy = y2 - y1, d = sqrtf(dx * dx + dy * dy);
    REAL a = (r1 * r1 - r2 * r2 + d * d) / (2 * d);
    REAL hh = sqrtf(fmaxf(r1 * r1 - a * a, 0.0f));
    REAL px = x1 + a * dx / d, py = y1 + a * dy / d;
    REAL ix1 = px - hh * dy / d, iy1 = py + hh * dx / d;
    REAL ix2 = px + hh * dy / d, iy2 = py - hh * dx / d;
    #define DEG(yy, xx) (atan2f((yy), (xx)) * 57.2957795f)
    #define NORM(v) (fmodf(fmodf((v), 360.0f) + 360.0f, 360.0f))
    REAL a1 = DEG(iy1 - y1, ix1 - x1), a2 = DEG(iy2 - y1, ix2 - x1), phi = DEG(dy, dx);
    REAL s1 = NORM(a2 - a1), t1 = NORM(phi - a1);
    REAL sweep1 = t1 < s1 ? s1 - 360 : s1;
    REAL b2 = DEG(iy2 - y2, ix2 - x2), b1 = DEG(iy1 - y2, ix1 - x2), psi = DEG(-dy, -dx);
    REAL s2 = NORM(b1 - b2), t2 = NORM(psi - b2);
    REAL sweep2 = t2 < s2 ? s2 : s2 - 360;
    #undef DEG
    #undef NORM
    p.AddArc(x1 - r1, y1 - r1, 2 * r1, 2 * r1, a1, sweep1);
    p.AddArc(x2 - r2, y2 - r2, 2 * r2, 2 * r2, b2, sweep2);
    p.CloseFigure();
}
// Sensus app tile: orange-to-red gradient rounded square with a white thermometer and scale ticks.
static void DrawAppMark(Graphics& g, REAL x, REAL y, REAL s) {
    GraphicsPath bg; RRPath(bg, RectF(x, y, s, s), s * 0.23f);
    LinearGradientBrush lg(PointF(x - 1, y - 1), PointF(x + s + 1, y + s + 1),
                           Color(255, 0xFF, 0x9A, 0x3D), Color(255, 0xE0, 0x3E, 0x52));
    g.FillPath(&lg, &bg);
    Color w(255, 255, 255, 255);
    GraphicsPath stem; RRPath(stem, RectF(x + s * 0.40f, y + s * 0.17f, s * 0.14f, s * 0.52f), s * 0.07f);
    SolidBrush wb(w); g.FillPath(&wb, &stem);
    FillCircle(g, x + s * 0.47f, y + s * 0.70f, s * 0.14f, w);
    Pen pen(w, s * 0.07f); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
    for (int i = 0; i < 3; i++) { REAL ty = y + s * (0.27f + 0.11f * i); g.DrawLine(&pen, x + s * 0.64f, ty, x + s * (i == 1 ? 0.76f : 0.71f), ty); }
}
