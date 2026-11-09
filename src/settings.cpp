// openMouse — settings window
// SPDX-License-Identifier: MIT
//
// Runs as its own process (openmouse.exe --settings), started from the tray
// menu. Deliberately not a window of the engine: the engine's thread is the
// input path of every mouse on the machine, and a settings window has no
// business sharing it. A crash or a hang here costs a window, never the
// pointers.
//
// Every change is applied at once: the seat values are merged into
// openmouse.ini and the engine is told to re-read them (kReloadSettingsMsg).
// The file is re-read before each write, so device bindings the engine may
// have saved in the meantime are kept. "Undo" puts back the values the window
// opened with.
#include "openmouse.h"
#include <commctrl.h>
#include <cstdio>
#include <cmath>

#ifdef _MSC_VER
// Version 6 of the common controls, for themed trackbars and buttons. Without
// it the controls fall back to their Windows 95 look.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace om {

bool UiFrench() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH;
}

namespace {

constexpr wchar_t kSettingsClass[] = L"openMouse.Settings";
constexpr wchar_t kEngineClass[]   = L"openMouse.Engine";

// ---------------------------------------------------------------- strings

enum Str {
    sTitle, sSeat, sSensitivity, sAccel, sSlow, sFast, sSpeed, sScreen, sScreenNone,
    sScreenItem, sPrimary, sUnplugged, sDefaults, sUndo, sClose, sHint, sApplied, sNotRunning, sSaveFailed, sStrCount
};

const wchar_t* const kStrEn[sStrCount] = {
    L"openMouse settings",
    L"Mouse %d",
    L"Sensitivity",
    L"Progressive acceleration",
    L"Slow motion (precision)",
    L"Fast motion (crossing the screen)",
    L"Acceleration threshold",
    L"Keep inside screen",
    L"No, the whole desktop",
    L"Screen %d: %d × %d",
    L" (main)",
    L"%s (unplugged)",
    L"Default values",
    L"Undo my changes",
    L"Close",
    L"Sensitivity multiplies every movement. With acceleration on, it is replaced: the "
    L"pointer moves slowly for small gestures and fast for wide ones. The threshold sets "
    L"the hand speed where it speeds up (lower: sooner).",
    L"Applied and saved.",
    L"Saved. openMouse is not running: it will be used at next start.",
    L"Could not save %s",
};

const wchar_t* const kStrFr[sStrCount] = {
    L"Réglages d'openMouse",
    L"Souris %d",
    L"Sensibilité",
    L"Accélération progressive",
    L"Gestes lents (précision)",
    L"Gestes rapides (traverser l'écran)",
    L"Seuil d'accélération",
    L"Rester dans l'écran",
    L"Non, tout le bureau",
    L"Écran %d : %d × %d",
    L" (principal)",
    L"%s (débranché)",
    L"Valeurs par défaut",
    L"Annuler mes changements",
    L"Fermer",
    L"La sensibilité multiplie chaque mouvement. Avec l'accélération, elle est remplacée : "
    L"la flèche va lentement pour les petits gestes et vite pour les grands. Le seuil règle "
    L"la vitesse de la main où elle accélère (plus bas : plus tôt).",
    L"Appliqué et enregistré.",
    L"Enregistré. openMouse n'est pas lancé : ce réglage servira au prochain démarrage.",
    L"Impossible d'enregistrer %s",
};

const wchar_t* S(Str id) { return (UiFrench() ? kStrFr : kStrEn)[id]; }

// ---------------------------------------------------------------- sliders

// A trackbar holds integers; each value is stored as value * scale.
struct Range { double lo, hi, scale; const wchar_t* fmt; };

enum Field { fSens, fSlow, fFast, fSpeed, fFieldCount };

const Range kRange[fFieldCount] = {
    { 0.10, 10.0, 20, L"×%.2f" },   // steps of 0.05
    { 0.10,  5.0, 20, L"×%.2f" },
    { 1.0,  20.0, 10, L"×%.1f" },   // steps of 0.1
    { 0.2,  10.0, 10, L"%.1f"  },
};

// Control ids: one block of 16 per seat.
constexpr int kIdBase    = 1000;
constexpr int kIdUndo    = 900;
constexpr int kIdClose   = 901;
enum SeatCtl { cLabel = 0, cValue = 4, cSlider = 8, cAccel = 12, cScreenLabel, cScreen, cDefaults };
int CtlId(int seat, int ctl) { return kIdBase + seat * 16 + ctl; }

// ---------------------------------------------------------------- state

struct SeatUi {
    HWND label[fFieldCount]{}, value[fFieldCount]{}, slider[fFieldCount]{};
    HWND accel = nullptr, screenLabel = nullptr, screen = nullptr, defaults = nullptr;
    RECT header{};   // swatch and title, painted by the window
};

struct State {
    std::wstring path;
    Config  opened;          // as the window found it, for "undo"
    Config  cur;             // what the window shows
    int     seats = 2;
    SeatUi  ui[kMaxSeats];
    HWND    hint = nullptr, status = nullptr, undo = nullptr, close = nullptr;
    HFONT   font = nullptr, bold = nullptr;
    UINT    dpi = 96;
    bool    pending = false;
    std::vector<ScreenInfo> screens;   // plugged in when the window opened
} g;

int Px(int dip) { return MulDiv(dip, (int)g.dpi, 96); }

UINT DpiOf(HWND hwnd) {
    using Fn = UINT (WINAPI*)(HWND);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && hwnd) return fn(hwnd);
    HDC dc = GetDC(nullptr);
    const UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi;
}

double Get(const Config& c, int seat, Field f) {
    switch (f) {
    case fSens:  return c.sensitivity[seat];
    case fSlow:  return c.accel[seat].slow;
    case fFast:  return c.accel[seat].fast;
    default:     return c.accel[seat].speed;
    }
}

void Put(Config& c, int seat, Field f, double v) {
    switch (f) {
    case fSens:  c.sensitivity[seat] = v; break;
    case fSlow:  c.accel[seat].slow  = v; break;
    case fFast:  c.accel[seat].fast  = v; break;
    default:     c.accel[seat].speed = v; break;
    }
}

void EnsureSeats(Config& c, int n) {
    if ((int)c.sensitivity.size() < n) c.sensitivity.resize(n, 1.0);
    if ((int)c.accel.size()       < n) c.accel.resize(n);
    if ((int)c.screen.size()      < n) c.screen.resize(n);
    if ((int)c.seatColors.size()  < n) c.seatColors.resize(n, RGB(200, 200, 200));
}

std::wstring Format(Field f, double v) {
    wchar_t buf[32];
    swprintf(buf, 32, kRange[f].fmt, v);
    if (UiFrench())
        for (wchar_t* p = buf; *p; ++p) if (*p == L'.') *p = L',';
    return buf;
}

// ---------------------------------------------------------------- apply

void SetStatus(const std::wstring& text) { SetWindowTextW(g.status, text.c_str()); }

// Merge the seat values into the file as it is now, then wake the engine.
void Apply() {
    g.pending = false;
    Config disk;
    disk.Load(g.path);
    EnsureSeats(disk, g.seats);
    for (int i = 0; i < g.seats; ++i) {
        disk.sensitivity[i] = g.cur.sensitivity[i];
        disk.accel[i]       = g.cur.accel[i];
        disk.screen[i]      = g.cur.screen[i];
    }
    if (!disk.Save(g.path)) {
        wchar_t buf[MAX_PATH + 64];
        swprintf(buf, MAX_PATH + 64, S(sSaveFailed), g.path.c_str());
        SetStatus(buf);
        return;
    }
    HWND engine = FindWindowW(kEngineClass, nullptr);
    if (engine && PostMessageW(engine, kReloadSettingsMsg, 0, 0)) SetStatus(S(sApplied));
    else                                                          SetStatus(S(sNotRunning));
}

// Coalesce a slider drag into a few writes a second.
void ApplySoon(HWND hwnd) {
    g.pending = true;
    SetTimer(hwnd, 1, 150, nullptr);
}

// ---------------------------------------------------------------- controls

void ShowSeat(int seat) {
    SeatUi& u = g.ui[seat];
    const bool accel = g.cur.accel[seat].on;
    for (int f = 0; f < fFieldCount; ++f) {
        const double v = Get(g.cur, seat, (Field)f);
        const Range& r = kRange[f];
        const double clamped = v < r.lo ? r.lo : (v > r.hi ? r.hi : v);
        SendMessageW(u.slider[f], TBM_SETPOS, TRUE, (LPARAM)std::lround(clamped * r.scale));
        SetWindowTextW(u.value[f], Format((Field)f, v).c_str());
        // Acceleration replaces the fixed multiplier, so only one of the two
        // is ever live; grey out the other rather than hide it.
        const bool on = (f == fSens) ? !accel : accel;
        EnableWindow(u.label[f], on);
        EnableWindow(u.value[f], on);
        EnableWindow(u.slider[f], on);
    }
    SendMessageW(u.accel, BM_SETCHECK, accel ? BST_CHECKED : BST_UNCHECKED, 0);

    // Screen list: "none", every screen plugged in, and the chosen one if it
    // is unplugged, so opening the window never drops a choice by itself.
    SendMessageW(u.screen, CB_RESETCONTENT, 0, 0);
    SendMessageW(u.screen, CB_ADDSTRING, 0, (LPARAM)S(sScreenNone));
    const std::wstring& want = g.cur.screen[seat];
    int sel = 0;
    for (size_t k = 0; k < g.screens.size(); ++k) {
        const ScreenInfo& sc = g.screens[k];
        wchar_t buf[96];
        swprintf(buf, 96, S(sScreenItem), (int)k + 1,
                 (int)(sc.rc.right - sc.rc.left), (int)(sc.rc.bottom - sc.rc.top));
        std::wstring item = buf;
        if (sc.primary) item += S(sPrimary);
        SendMessageW(u.screen, CB_ADDSTRING, 0, (LPARAM)item.c_str());
        if (!want.empty() && _wcsicmp(sc.name.c_str(), want.c_str()) == 0) sel = (int)k + 1;
    }
    if (!want.empty() && sel == 0) {
        wchar_t buf[96];
        swprintf(buf, 96, S(sUnplugged), want.c_str());
        SendMessageW(u.screen, CB_ADDSTRING, 0, (LPARAM)buf);
        sel = (int)g.screens.size() + 1;
    }
    SendMessageW(u.screen, CB_SETCURSEL, sel, 0);
}

void OnScreen(int seat) {
    const int sel = (int)SendMessageW(g.ui[seat].screen, CB_GETCURSEL, 0, 0);
    if (sel == 0)                              g.cur.screen[seat].clear();
    else if (sel <= (int)g.screens.size())     g.cur.screen[seat] = g.screens[sel - 1].name;
    else                                       return;   // the unplugged one, unchanged
    ShowSeat(seat);
    Apply();
}

HWND Make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                           parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

BOOL CALLBACK SetFontProc(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

void CreateControls(HWND hwnd) {
    const wchar_t* names[fFieldCount] = { S(sSensitivity), S(sSlow), S(sFast), S(sSpeed) };
    for (int i = 0; i < g.seats; ++i) {
        SeatUi& u = g.ui[i];
        for (int f = 0; f < fFieldCount; ++f) {
            if (f == fSlow)   // the checkbox sits between sensitivity and the accel sliders
                u.accel = Make(hwnd, L"BUTTON", S(sAccel), WS_TABSTOP | BS_AUTOCHECKBOX, CtlId(i, cAccel));
            u.label[f]  = Make(hwnd, L"STATIC", names[f], SS_LEFT | SS_NOPREFIX, CtlId(i, cLabel + f));
            u.value[f]  = Make(hwnd, L"STATIC", L"", SS_RIGHT, CtlId(i, cValue + f));
            u.slider[f] = Make(hwnd, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                               CtlId(i, cSlider + f));
            const Range& r = kRange[f];
            SendMessageW(u.slider[f], TBM_SETRANGEMIN, FALSE, (LPARAM)std::lround(r.lo * r.scale));
            SendMessageW(u.slider[f], TBM_SETRANGEMAX, FALSE, (LPARAM)std::lround(r.hi * r.scale));
            SendMessageW(u.slider[f], TBM_SETLINESIZE, 0, 1);
            SendMessageW(u.slider[f], TBM_SETPAGESIZE, 0, (LPARAM)std::lround(r.scale / 2));
        }
        u.screenLabel = Make(hwnd, L"STATIC", S(sScreen), SS_LEFT, CtlId(i, cScreenLabel));
        u.screen = Make(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cScreen));
        u.defaults = Make(hwnd, L"BUTTON", S(sDefaults), WS_TABSTOP | BS_PUSHBUTTON, CtlId(i, cDefaults));
        ShowSeat(i);
    }
    g.hint   = Make(hwnd, L"STATIC", S(sHint), SS_LEFT, 0);
    g.status = Make(hwnd, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0);
    g.undo   = Make(hwnd, L"BUTTON", S(sUndo), WS_TABSTOP | BS_PUSHBUTTON, kIdUndo);
    g.close  = Make(hwnd, L"BUTTON", S(sClose), WS_TABSTOP | BS_DEFPUSHBUTTON, kIdClose);
}

// ---------------------------------------------------------------- layout

constexpr int kMargin = 16, kColW = 250, kGap = 28, kPerRow = 4;

int Columns() { return g.seats < kPerRow ? g.seats : kPerRow; }

// Lays out every control for the current DPI; returns the client size.
SIZE Layout(HWND hwnd) {
    if (g.font) DeleteObject(g.font);
    if (g.bold) DeleteObject(g.bold);
    g.font = CreateFontW(-MulDiv(9, (int)g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.bold = CreateFontW(-MulDiv(11, (int)g.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                         0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    EnumChildWindows(hwnd, SetFontProc, (LPARAM)g.font);

    const int colW = Px(kColW);
    auto place = [](HWND h, int x, int y, int w, int hgt) {
        SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    int bottom = 0;
    for (int i = 0; i < g.seats; ++i) {
        SeatUi& u = g.ui[i];
        const int x  = Px(kMargin) + (i % kPerRow) * (colW + Px(kGap));
        int y        = Px(kMargin) + (i / kPerRow) * Px(470);
        u.header = { x, y, x + colW, y + Px(24) };
        y += Px(34);
        for (int f = 0; f < fFieldCount; ++f) {
            if (f == fSlow) { place(u.accel, x, y, colW, Px(22)); y += Px(30); }
            place(u.label[f], x, y, colW - Px(64), Px(18));
            place(u.value[f], x + colW - Px(64), y, Px(64), Px(18));
            y += Px(20);
            place(u.slider[f], x - Px(4), y, colW + Px(8), Px(28));
            y += Px(f == fSens ? 40 : 34);
        }
        y += Px(4);
        place(u.screenLabel, x, y, colW, Px(18));
        y += Px(20);
        place(u.screen, x, y, colW, Px(200));
        y += Px(38);
        place(u.defaults, x, y, Px(150), Px(28));
        y += Px(28);
        if (y > bottom) bottom = y;
    }

    const int width = Px(kMargin) * 2 + Columns() * colW + (Columns() - 1) * Px(kGap);
    const int inner = width - Px(kMargin) * 2;
    int y = bottom + Px(20);
    place(g.hint, Px(kMargin), y, inner, Px(64));
    y += Px(70);
    place(g.status, Px(kMargin), y, inner, Px(18));
    y += Px(28);
    const int closeW = Px(100), undoW = Px(190), h = Px(30);
    place(g.close, width - Px(kMargin) - closeW, y, closeW, h);
    place(g.undo,  width - Px(kMargin) - closeW - Px(10) - undoW, y, undoW, h);
    y += h + Px(kMargin);
    InvalidateRect(hwnd, nullptr, TRUE);
    return { width, y };
}

RECT WindowRectFor(SIZE client, DWORD style) {
    RECT r{ 0, 0, client.cx, client.cy };
    using Fn = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (fn) fn(&r, style, FALSE, 0, g.dpi);
    else    AdjustWindowRectEx(&r, style, FALSE, 0);
    return r;
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g.bold);
    for (int i = 0; i < g.seats; ++i) {
        const RECT& h = g.ui[i].header;
        const int sw = Px(16), top = h.top + (h.bottom - h.top - sw) / 2;
        RECT swatch{ h.left, top, h.left + sw, top + sw };
        HBRUSH b = CreateSolidBrush(g.cur.seatColors[i]);
        FillRect(dc, &swatch, b);
        DeleteObject(b);
        FrameRect(dc, &swatch, GetSysColorBrush(COLOR_GRAYTEXT));
        wchar_t title[64];
        swprintf(title, 64, S(sSeat), i + 1);
        RECT tr{ h.left + sw + Px(8), h.top, h.right, h.bottom };
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        DrawTextW(dc, title, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT line{ h.left, h.bottom + Px(4), h.right, h.bottom + Px(5) };
        FillRect(dc, &line, GetSysColorBrush(COLOR_3DLIGHT));
    }
    SelectObject(dc, old);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- events

bool SeatOf(int id, int& seat, int& ctl) {
    if (id < kIdBase || id >= kIdBase + g.seats * 16) return false;
    seat = (id - kIdBase) / 16;
    ctl  = (id - kIdBase) % 16;
    return true;
}

void OnSlider(HWND hwnd, HWND slider) {
    int seat, ctl;
    if (!SeatOf(GetDlgCtrlID(slider), seat, ctl) || ctl < cSlider || ctl >= cSlider + fFieldCount)
        return;
    const Field f = (Field)(ctl - cSlider);
    const double v = (double)SendMessageW(slider, TBM_GETPOS, 0, 0) / kRange[f].scale;
    if (std::fabs(v - Get(g.cur, seat, f)) < 1e-9) return;
    Put(g.cur, seat, f, v);
    SetWindowTextW(g.ui[seat].value[f], Format(f, v).c_str());
    ApplySoon(hwnd);
}

void OnCommand(HWND hwnd, int id) {
    if (id == kIdClose || id == IDCANCEL) { SendMessageW(hwnd, WM_CLOSE, 0, 0); return; }
    if (id == kIdUndo) {
        for (int i = 0; i < g.seats; ++i) {
            g.cur.sensitivity[i] = g.opened.sensitivity[i];
            g.cur.accel[i]       = g.opened.accel[i];
            g.cur.screen[i]      = g.opened.screen[i];
            ShowSeat(i);
        }
        Apply();
        return;
    }
    int seat, ctl;
    if (!SeatOf(id, seat, ctl)) return;
    if (ctl == cAccel) {
        g.cur.accel[seat].on = SendMessageW(g.ui[seat].accel, BM_GETCHECK, 0, 0) == BST_CHECKED;
        ShowSeat(seat);
        Apply();
    } else if (ctl == cDefaults) {
        g.cur.sensitivity[seat] = 1.0;
        g.cur.accel[seat]       = Config::Accel{};
        g.cur.screen[seat].clear();
        ShowSeat(seat);
        Apply();
    }
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HSCROLL:
        if (lp) OnSlider(hwnd, (HWND)lp);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) OnCommand(hwnd, LOWORD(wp));
        else if (HIWORD(wp) == CBN_SELCHANGE) {
            int seat, ctl;
            if (SeatOf(LOWORD(wp), seat, ctl) && ctl == cScreen) OnScreen(seat);
        }
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, wp);
        if (g.pending) Apply();
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        // The window is white; the controls default to the dialog grey.
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor((HWND)lp == g.hint ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        const SIZE client = Layout(hwnd);
        const RECT* suggested = (const RECT*)lp;
        const RECT wr = WindowRectFor(client, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE));
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     wr.right - wr.left, wr.bottom - wr.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        if (g.pending) Apply();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int RunSettings(HINSTANCE inst) {
    // One window at a time: a second request brings the first one forward.
    if (HWND open = FindWindowW(kSettingsClass, nullptr)) {
        ShowWindow(open, SW_RESTORE);
        SetForegroundWindow(open);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    g.path = Config::DefaultPath();
    g.opened.Load(g.path);
    g.seats = g.opened.seatCount;
    EnsureSeats(g.opened, g.seats);
    g.cur = g.opened;
    g.screens = ListScreens();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SettingsProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(kAppIconResource), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                         LR_SHARED);
    wc.hIconSm       = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(kAppIconResource), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                         LR_SHARED);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kSettingsClass;
    RegisterClassExW(&wc);

    // Not resizable, never topmost: it must not stand in the way of anything.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kSettingsClass, S(sTitle), style,
                                CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    // Open on the screen where the pointer that asked for it is.
    POINT pt{};
    GetCursorPos(&pt);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(hwnd, nullptr, mi.rcWork.left, mi.rcWork.top, 100, 100,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    g.dpi = DpiOf(hwnd);

    CreateControls(hwnd);
    const SIZE client = Layout(hwnd);
    const RECT wr = WindowRectFor(client, style);
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    const RECT& wa = mi.rcWork;
    SetWindowPos(hwnd, nullptr, wa.left + ((wa.right - wa.left) - w) / 2,
                 wa.top + ((wa.bottom - wa.top) - h) / 2, w, h, SWP_NOZORDER);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    LogLine(L"[settings] window opened for %d seats (%s)", g.seats, g.path.c_str());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g.font) DeleteObject(g.font);
    if (g.bold) DeleteObject(g.bold);
    return 0;
}

} // namespace om
