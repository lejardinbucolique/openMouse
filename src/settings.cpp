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
//
// The sections of the mice sit in a panel of their own that scrolls: with a
// button section on top of the speed ones they no longer fit a small screen.
// The panel only hosts the controls and forwards their notifications.
#include "openmouse.h"
#include <commctrl.h>
#include <shellapi.h>   // ShellExecuteEx — not pulled in by WIN32_LEAN_AND_MEAN
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
constexpr wchar_t kPanelClass[]    = L"openMouse.Settings.Panel";
constexpr wchar_t kPreviewClass[]  = L"openMouse.Settings.Preview";
constexpr wchar_t kEngineClass[]   = L"openMouse.Engine";

// ---------------------------------------------------------------- strings

enum Str {
    sTitle, sSeat, sSensitivity, sAccel, sSlow, sFast, sSpeed, sScreen, sScreenNone,
    sScreenItem, sPrimary, sUnplugged, sDefaults, sUndo, sClose, sHint, sApplied, sNotRunning, sSaveFailed,
    sLearn, sForget, sListening, sNothing, sAbsent, sEngineOff, sLearned, sAlready, sFull,
    sNoButtons, sNative, sActNone, sActLeft, sActRight, sActMiddle, sActBack, sActForward, sActKeys,
    sKeysNone, sKeysCapture, sWin, sCtrlName, sAltName, sShiftName, sWinName,
    sGeneral, sAutostart, sAutostartOn, sAutostartOff, sAutostartWait, sAutostartFail,
    sAutostartDenied,
    sLook, sColorLabel, sStyleLabel, sColorCustom,
    sStyle0, sStyle1, sStyle2, sStyle3, sStyle4,
    sCol0, sCol1, sCol2, sCol3, sCol4, sCol5, sCol6, sCol7, sCol8, sCol9,
    sFixed, sExplainFixed, sExplainAccel, sSeatAbsent,
    sTouchpad, sPadCut, sPadMode, sPadAuto, sPadOn, sPadOff, sStrCount
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
    L"Every change applies at once. A mouse takes one of the two modes: the other one's "
    L"sliders are greyed out. A learned button acts at the position of the pointer of "
    L"its own mouse.",
    L"Applied and saved.",
    L"Saved. openMouse is not running: it will be used at next start.",
    L"Could not save %s",
    L"Learn a button",
    L"Forget",
    L"Press the button to learn (8 s)…",
    L"Nothing received: this button sends nothing to Windows (only the manufacturer's software "
    L"can read it), or it is not on this mouse.",
    L"This mouse is not plugged in.",
    L"openMouse is not running: there is nothing to listen to.",
    L"Button learned. Choose what it does.",
    L"This button is already learned.",
    L"This mouse already has %d learned buttons: forget one first.",
    L"No learned button. Learn one to give it an action.",
    L"Windows also keeps its own action for this button.",
    L"Do nothing",
    L"Left click",
    L"Right click",
    L"Middle click",
    L"Back button",
    L"Forward button",
    L"Keyboard shortcut",
    L"Choose the shortcut…",
    L"Press the shortcut…",
    L"Win +",
    L"Ctrl", L"Alt", L"Shift", L"Win",
    L"General",
    L"Start openMouse with Windows",
    L"openMouse will start by itself when you log on to Windows.",
    L"openMouse will no longer start with Windows.",
    L"Waiting for Windows to confirm…",
    L"Windows did not take the change. Run openMouse as administrator and try again.",
    L"Cancelled: administrator permission was refused, nothing changed.",
    L"Pointer look",
    L"Colour",
    L"Pointer set",
    L"Custom (%06X)",
    L"Classic (coloured body, black edge)",
    L"Bright rim (seen on any background)",
    L"White body, coloured edge",
    L"Large, bright rim",
    L"Soft glow",
    L"Yellow", L"Orange", L"Red", L"Pink", L"Purple", L"Blue", L"Cyan", L"Green", L"Lime", L"White",
    L"Fixed sensitivity",
    L"Every movement is multiplied by the same value.",
    L"Slow for small gestures, fast for wide ones. The threshold sets where it speeds up.",
    L" (unplugged)",
    L"Touchpad",
    L" (cut off)",
    L"Touchpad mode",
    L"Automatic (cut off from two mice on)",
    L"Always on",
    L"Cut off (kept when no other mouse)",
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
    L"Chaque changement s'applique tout de suite. Une souris suit l'un des deux modes : les "
    L"curseurs de l'autre sont grisés. Un bouton appris agit à la position de la flèche de "
    L"sa propre souris.",
    L"Appliqué et enregistré.",
    L"Enregistré. openMouse n'est pas lancé : ce réglage servira au prochain démarrage.",
    L"Impossible d'enregistrer %s",
    L"Apprendre un bouton",
    L"Oublier",
    L"Appuie sur le bouton à apprendre (8 s)…",
    L"Rien reçu : ce bouton n'envoie rien à Windows (seul le logiciel du fabricant sait le lire), "
    L"ou il n'est pas sur cette souris.",
    L"Cette souris n'est pas branchée.",
    L"openMouse n'est pas lancé : il n'y a rien à écouter.",
    L"Bouton appris. Choisis ce qu'il fait.",
    L"Ce bouton est déjà appris.",
    L"Cette souris a déjà %d boutons appris : oublie-en un d'abord.",
    L"Aucun bouton appris. Apprends-en un pour lui donner une action.",
    L"Windows garde aussi sa propre action pour ce bouton.",
    L"Ne rien faire",
    L"Clic gauche",
    L"Clic droit",
    L"Clic milieu",
    L"Bouton précédent",
    L"Bouton suivant",
    L"Raccourci clavier",
    L"Choisir le raccourci…",
    L"Appuie sur le raccourci…",
    L"Win +",
    L"Ctrl", L"Alt", L"Maj", L"Win",
    L"Général",
    L"Lancer openMouse au démarrage de Windows",
    L"openMouse se lancera tout seul à l'ouverture de votre session Windows.",
    L"openMouse ne se lancera plus avec Windows.",
    L"En attente de la confirmation de Windows…",
    L"Windows n'a pas pris le changement. Lancez openMouse en administrateur et recommencez.",
    L"Annulé : l'autorisation administrateur a été refusée, rien n'a changé.",
    L"Apparence de la flèche",
    L"Couleur",
    L"Jeu de curseurs",
    L"Personnalisée (%06X)",
    L"Classique (corps coloré, bord noir)",
    L"Liseré brillant (visible sur tous les fonds)",
    L"Corps blanc, bord coloré",
    L"Grand, liseré brillant",
    L"Halo lumineux",
    L"Jaune", L"Orange", L"Rouge", L"Rose", L"Violet", L"Bleu", L"Cyan", L"Vert", L"Citron vert", L"Blanc",
    L"Sensibilité fixe",
    L"Chaque mouvement est multiplié par la même valeur.",
    L"Lent pour les petits gestes, rapide pour les grands. Le seuil règle où ça accélère.",
    L" (débranchée)",
    L"Pavé tactile",
    L" (coupé)",
    L"Mode du pavé tactile",
    L"Automatique (coupé dès deux souris)",
    L"Toujours actif",
    L"Coupé (gardé s'il n'y a pas d'autre souris)",
};

// The colours offered for a mouse; the first one is the default of seat 0.
const COLORREF kColors[10] = {
    RGB(0xFF, 0xDD, 0x00), RGB(0xE8, 0x6A, 0x33), RGB(0xE0, 0x3C, 0x3C), RGB(0xF0, 0x6E, 0xAA),
    RGB(0xB0, 0x60, 0xC0), RGB(0x4A, 0x90, 0xD9), RGB(0x20, 0xC0, 0xD0), RGB(0x5C, 0xB8, 0x5C),
    RGB(0xB8, 0xD8, 0x2A), RGB(0xFF, 0xFF, 0xFF),
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

// Control ids: one block of 32 per seat.
constexpr int kPerSeat   = 32;
constexpr int kIdBase    = 1000;
constexpr int kIdUndo    = 900;
constexpr int kIdClose   = 901;
constexpr int kIdAutostart = 902;
enum SeatCtl { cLabel = 0, cValue = 4, cSlider = 8, cAccel = 12, cScreenLabel, cScreen, cDefaults,
               cLearn = 16, cNote, cList, cAction, cKeys, cWin, cForget,
               cColorLabel = 23, cColor, cStyleLabel, cStyle, cPreview, cFixed, cExplain,
               cPadLabel, cPad };
int CtlId(int seat, int ctl) { return kIdBase + seat * kPerSeat + ctl; }

constexpr UINT_PTR kTimerApply = 1, kTimerLearn = 2, kTimerAutostart = 3;
constexpr UINT_PTR kSubclassKeys = 1;

// ---------------------------------------------------------------- state

struct SeatUi {
    HWND label[fFieldCount]{}, value[fFieldCount]{}, slider[fFieldCount]{};
    HWND fixed = nullptr, explain = nullptr;   // the two speed modes, and a phrase on the one chosen
    HWND accel = nullptr, screenLabel = nullptr, screen = nullptr, defaults = nullptr;
    HWND learn = nullptr, note = nullptr, list = nullptr, action = nullptr, keys = nullptr,
         win = nullptr, forget = nullptr;
    HWND colorLabel = nullptr, color = nullptr, styleLabel = nullptr, style = nullptr, preview = nullptr;
    HWND padLabel = nullptr, pad = nullptr;   // the touchpad mode, shown in its section only
    RECT header{};   // swatch and title, painted by the panel (content coordinates)
};

struct State {
    std::wstring path;
    Config  opened;          // as the window found it, for "undo"
    Config  cur;             // what the window shows
    int     seats = 2;
    SeatUi  ui[kMaxSeats];
    HWND    panel = nullptr;
    HWND    hint = nullptr, status = nullptr, undo = nullptr, close = nullptr;
    HWND    autostart = nullptr;     // "start with Windows", in the general section
    RECT    general{};               // title of the general section, painted by the window
    bool    autoWait = false;        // a change is in flight, polled by kTimerAutostart
    bool    autoWant = false;
    int     autoTries = 0;
    HANDLE  autoHelper = nullptr;    // the elevated one-shot copy, when the engine could not do it
    HFONT   font = nullptr, bold = nullptr;
    UINT    dpi = 96;
    bool    pending = false;
    std::vector<ScreenInfo> screens;   // plugged in when the window opened

    int     scrollY = 0;                         // panel scroll offset, pixels
    int     sel[kMaxSeats] = {};                 // learned button selected in each seat's list
    std::wstring note[kMaxSeats];                // last thing said under a seat's learn button
    int     learning = -1;                       // seat the engine is listening to, or -1
    int     capturing = -1;                      // seat whose shortcut is being typed, or -1
    UINT    present = ~0u;                       // seats whose mouse is plugged in (bit mask), as the engine said
    int     padSeat = -1;                        // the seat of the laptop touchpad, or -1 without one
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

int ScrollBarWidth() {
    using Fn = int (WINAPI*)(int, UINT);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    return fn ? fn(SM_CXVSCROLL, g.dpi) : GetSystemMetrics(SM_CXVSCROLL);
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
    if ((int)c.cursorStyle.size() < n) c.cursorStyle.resize(n, (int)kStyleBright);
}

std::wstring Format(Field f, double v) {
    wchar_t buf[32];
    swprintf(buf, 32, kRange[f].fmt, v);
    if (UiFrench())
        for (wchar_t* p = buf; *p; ++p) if (*p == L'.') *p = L',';
    return buf;
}

// The seat the engine gave the laptop touchpad: it keeps it even while the
// touchpad is cut off, so its section is there to switch it back on.
int TouchpadSeat(const Config& c) {
    for (const auto& kv : c.mouseBinding)
        if (IsIntegratedPointer(kv.first) && kv.second < c.seatCount) return kv.second;
    return -1;
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
        disk.seatColors[i]  = g.cur.seatColors[i];
        disk.cursorStyle[i] = g.cur.cursorStyle[i];
        disk.buttons[i]     = g.cur.buttons[i];
    }
    disk.touchpad = g.cur.touchpad;
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
    SetTimer(hwnd, kTimerApply, 150, nullptr);
}

// ---------------------------------------------------------------- start with Windows

// The engine, when it is running elevated, is the one that owns the task: it
// can create and delete it without a prompt. Otherwise a one-shot elevated copy
// of this program does it, which costs one UAC prompt, once per change.
bool AskEngine(WPARAM request, DWORD_PTR& answer) {
    HWND engine = FindWindowW(kEngineClass, nullptr);
    answer = 0;
    return engine && SendMessageTimeoutW(engine, kAutostartMsg, request, 0, SMTO_ABORTIFHUNG,
                                         1000, &answer) != 0;
}

// The task as it is now: 1, 0, or -1 when nobody can tell.
int ReadAutostart() {
    DWORD_PTR r;
    if (AskEngine(kAutostartQuery, r) && (r & kAutoElevated) && (r & kAutoKnown))
        return (r & kAutoPresent) ? 1 : 0;
    return AutostartState();
}

void ShowAutostart(bool on) {
    SendMessageW(g.autostart, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

void FinishAutostart(HWND hwnd, bool ok, int state) {
    KillTimer(hwnd, kTimerAutostart);
    if (g.autoHelper) { CloseHandle(g.autoHelper); g.autoHelper = nullptr; }
    g.autoWait = false;
    EnableWindow(g.autostart, TRUE);
    ShowAutostart(state >= 0 ? state == 1 : !g.autoWant);   // what is real, not what was clicked
    SetStatus(ok ? S(g.autoWant ? sAutostartOn : sAutostartOff) : S(sAutostartFail));
}

void PollAutostart(HWND hwnd) {
    const int state = ReadAutostart();
    if (state == (g.autoWant ? 1 : 0)) { FinishAutostart(hwnd, true, state); return; }
    if (g.autoHelper) {
        // Waiting on the UAC prompt can take as long as the person likes.
        if (WaitForSingleObject(g.autoHelper, 0) == WAIT_OBJECT_0) FinishAutostart(hwnd, false, state);
    } else if (++g.autoTries > 40) {   // ten seconds
        FinishAutostart(hwnd, false, state);
    }
}

void OnAutostart(HWND hwnd) {
    if (g.autoWait) return;
    g.autoWant = SendMessageW(g.autostart, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g.autoTries = 0;

    DWORD_PTR r;
    const bool sent = AskEngine(kAutostartQuery, r) && (r & kAutoElevated) &&
                      AskEngine(g.autoWant ? kAutostartEnable : kAutostartDisable, r);
    if (!sent) {
        wchar_t exe[MAX_PATH];
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
        SHELLEXECUTEINFOW sei{};
        sei.cbSize       = sizeof(sei);
        sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd         = hwnd;
        sei.lpVerb       = L"runas";
        sei.lpFile       = exe;
        sei.lpParameters = g.autoWant ? L"--autostart=on" : L"--autostart=off";
        sei.nShow        = SW_HIDE;
        if (!ShellExecuteExW(&sei)) {   // the prompt was refused, or nothing could start
            ShowAutostart(ReadAutostart() == 1);
            SetStatus(S(sAutostartDenied));
            return;
        }
        g.autoHelper = sei.hProcess;
    }
    g.autoWait = true;
    EnableWindow(g.autostart, FALSE);
    SetStatus(S(sAutostartWait));
    SetTimer(hwnd, kTimerAutostart, 250, nullptr);
}

// ---------------------------------------------------------------- learned buttons

const wchar_t* ActionName(int action) {
    static const Str ids[kActCount] = { sActNone, sActLeft, sActRight, sActMiddle, sActBack, sActForward, sActKeys };
    return S(ids[action < 0 || action >= kActCount ? 0 : action]);
}

std::wstring KeyName(UINT vk) {
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t name[64] = L"";
    GetKeyNameTextW((LONG)((scan << 16) | (IsExtendedVk(vk) ? (1u << 24) : 0u)), name, 64);
    if (!*name) swprintf(name, 64, L"0x%X", vk);
    return name;
}

std::wstring ShortcutText(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & kModCtrl)  s += std::wstring(S(sCtrlName))  + L"+";
    if (mods & kModAlt)   s += std::wstring(S(sAltName))   + L"+";
    if (mods & kModShift) s += std::wstring(S(sShiftName)) + L"+";
    if (mods & kModWin)   s += std::wstring(S(sWinName))   + L"+";
    return vk ? s + KeyName(vk) : s;
}

// What the list of a seat shows for one learned button.
std::wstring ListText(const ButtonMap& b) {
    std::wstring s = b.label.empty() ? b.sig : b.label;
    s += L"  →  ";
    s += (b.action == kActKeys && b.vk) ? ShortcutText(b.mods, b.vk) : std::wstring(ActionName(b.action));
    return s;
}

// The action a standard mouse button has by itself, so that learning it does
// not take it away.
int NaturalAction(const ButtonMap& b) {
    if (b.kind != L'm') return kActNone;
    switch (b.code) {
    case RI_MOUSE_LEFT_BUTTON_DOWN:   return kActLeft;
    case RI_MOUSE_RIGHT_BUTTON_DOWN:  return kActRight;
    case RI_MOUSE_MIDDLE_BUTTON_DOWN: return kActMiddle;
    case RI_MOUSE_BUTTON_4_DOWN:      return kActBack;
    case RI_MOUSE_BUTTON_5_DOWN:      return kActForward;
    }
    return kActNone;
}

ButtonMap* Selected(int seat) {
    auto& list = g.cur.buttons[seat];
    if (list.empty()) return nullptr;
    if (g.sel[seat] < 0 || g.sel[seat] >= (int)list.size()) g.sel[seat] = 0;
    return &list[g.sel[seat]];
}

// The learned-button part of one seat's section.
void ShowButtons(int seat) {
    SeatUi& u = g.ui[seat];
    auto& list = g.cur.buttons[seat];
    ButtonMap* b = Selected(seat);

    SendMessageW(u.list, CB_RESETCONTENT, 0, 0);
    for (const ButtonMap& m : list) SendMessageW(u.list, CB_ADDSTRING, 0, (LPARAM)ListText(m).c_str());
    SendMessageW(u.list, CB_SETCURSEL, b ? g.sel[seat] : -1, 0);
    SendMessageW(u.action, CB_SETCURSEL, b ? b->action : -1, 0);

    const bool keys = b && b->action == kActKeys;
    std::wstring text = S(sKeysNone);
    if (g.capturing == seat)     text = S(sKeysCapture);
    else if (keys && b->vk)      text = ShortcutText(b->mods & ~kModWin, b->vk);
    SetWindowTextW(u.keys, text.c_str());
    SendMessageW(u.win, BM_SETCHECK, (keys && (b->mods & kModWin)) ? BST_CHECKED : BST_UNCHECKED, 0);

    EnableWindow(u.learn, g.learning < 0 && g.capturing < 0);
    EnableWindow(u.list, b != nullptr);
    EnableWindow(u.action, b != nullptr);
    EnableWindow(u.keys, keys);
    EnableWindow(u.win, keys);
    EnableWindow(u.forget, b != nullptr);

    // Under the buttons: what the engine is doing, else what just happened,
    // else what the selected button implies.
    std::wstring note;
    if (g.learning == seat)          note = S(sListening);
    else if (!g.note[seat].empty())  note = g.note[seat];
    else if (!b)                     note = S(sNoButtons);
    else if (b->WindowsAlsoActs())   note = S(sNative);
    SetWindowTextW(u.note, note.c_str());
}

void Note(int seat, const std::wstring& text) {
    g.note[seat] = text;
    ShowButtons(seat);
}

void OnLearnClick(HWND hwnd, int seat) {
    if (g.learning >= 0) return;
    if ((int)g.cur.buttons[seat].size() >= kMaxLearned) {
        wchar_t buf[160];
        swprintf(buf, 160, S(sFull), kMaxLearned);
        Note(seat, buf);
        return;
    }
    HWND engine = FindWindowW(kEngineClass, nullptr);
    if (!engine) { Note(seat, S(sEngineOff)); return; }
    if (g.pending) Apply();
    if (!PostMessageW(engine, kLearnMsg, (WPARAM)seat + 1, 0)) { Note(seat, S(sEngineOff)); return; }
    g.learning = seat;
    g.note[seat].clear();
    // The engine ends its own listening; this is only for the day it never answers.
    SetTimer(hwnd, kTimerLearn, kLearnListenMs + 4000, nullptr);
    for (int i = 0; i < g.seats; ++i) ShowButtons(i);
}

void EndLearning(HWND hwnd) {
    KillTimer(hwnd, kTimerLearn);
    const int seat = g.learning;
    g.learning = -1;
    if (seat >= 0 && seat < g.seats) for (int i = 0; i < g.seats; ++i) ShowButtons(i);
}

// The engine has finished listening to a seat's mouse.
void OnLearned(HWND hwnd, int seat, LPARAM result) {
    if (seat < 0 || seat >= g.seats) { EndLearning(hwnd); return; }
    g.learning = -1;
    KillTimer(hwnd, kTimerLearn);
    std::wstring note = S(sNothing);
    if (result == kLearnAbsent) note = S(sAbsent);
    if (result == kLearnHeard) {
        int heardSeat = -1;
        std::wstring sig, label;
        if (ReadLearnResult(heardSeat, sig, label) && heardSeat == seat) {
            auto& list = g.cur.buttons[seat];
            int found = -1;
            for (int i = 0; i < (int)list.size(); ++i) if (list[i].sig == sig) found = i;
            if (found >= 0) {
                g.sel[seat] = found;
                note = S(sAlready);
            } else if ((int)list.size() < kMaxLearned) {
                ButtonMap b;
                b.sig   = sig;
                b.label = label;
                if (b.Parse()) {
                    b.action = NaturalAction(b);
                    list.push_back(std::move(b));
                    g.sel[seat] = (int)list.size() - 1;
                    note = S(sLearned);
                    Apply();
                }
            }
        }
    }
    g.note[seat] = note;
    for (int i = 0; i < g.seats; ++i) ShowButtons(i);
}

void OnForget(int seat) {
    auto& list = g.cur.buttons[seat];
    if (list.empty()) return;
    const int at = g.sel[seat] < (int)list.size() ? g.sel[seat] : 0;
    list.erase(list.begin() + at);
    if (g.sel[seat] >= (int)list.size()) g.sel[seat] = (int)list.size() - 1;
    if (g.sel[seat] < 0) g.sel[seat] = 0;
    g.note[seat].clear();
    ShowButtons(seat);
    Apply();
}

void OnListSelect(int seat) {
    const int sel = (int)SendMessageW(g.ui[seat].list, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g.cur.buttons[seat].size()) return;
    g.sel[seat] = sel;
    g.note[seat].clear();
    ShowButtons(seat);
}

void OnActionSelect(int seat) {
    ButtonMap* b = Selected(seat);
    const int act = (int)SendMessageW(g.ui[seat].action, CB_GETCURSEL, 0, 0);
    if (!b || act < 0 || act >= kActCount) return;
    b->action = act;
    // A shortcut action with no shortcut yet does nothing; ask for it at once.
    g.note[seat].clear();
    ShowButtons(seat);
    Apply();
    if (act == kActKeys && !b->vk) SendMessageW(g.ui[seat].keys, BM_CLICK, 0, 0);
}

void StopCapture(int seat) {
    g.capturing = -1;
    if (seat >= 0 && seat < g.seats) ShowButtons(seat);
}

void OnKeysClick(int seat) {
    if (g.capturing == seat) { StopCapture(seat); return; }
    if (!Selected(seat)) return;
    g.capturing = seat;
    ShowButtons(seat);
    SetFocus(g.ui[seat].keys);
}

void OnWinToggle(int seat) {
    ButtonMap* b = Selected(seat);
    if (!b) return;
    if (SendMessageW(g.ui[seat].win, BM_GETCHECK, 0, 0) == BST_CHECKED) b->mods |= kModWin;
    else                                                                 b->mods &= ~kModWin;
    ShowButtons(seat);
    Apply();
}

bool SeatOf(int id, int& seat, int& ctl);

// The shortcut is typed on the button itself: its subclass sees the keys (and
// asks the dialog loop to leave them alone) while capturing.
LRESULT CALLBACK KeysProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    int seat = -1, ctl = 0;
    const bool mine = SeatOf(GetDlgCtrlID(hwnd), seat, ctl) && ctl == cKeys;
    if (mine && g.capturing == seat) {
        switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
        case WM_CHAR: case WM_SYSCHAR: case WM_KEYUP: case WM_SYSKEYUP:
            return 0;
        case WM_KILLFOCUS:
            StopCapture(seat);
            break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            const UINT vk = (UINT)wp;
            if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN)
                return 0;                          // a modifier alone: wait for the key
            if (vk == VK_ESCAPE) { StopCapture(seat); return 0; }
            if (ButtonMap* b = Selected(seat)) {
                UINT mods = b->mods & kModWin;
                if (GetKeyState(VK_CONTROL) & 0x8000) mods |= kModCtrl;
                if (GetKeyState(VK_MENU)    & 0x8000) mods |= kModAlt;
                if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= kModShift;
                b->vk   = vk;
                b->mods = mods;
                g.note[seat].clear();
                g.capturing = -1;
                ShowButtons(seat);
                Apply();
            }
            return 0;
        }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- look

// The two choices that make a pointer recognisable: its colour and its set of
// shapes, with a preview of the set on the backgrounds that hide pointers most.
void ShowLook(int seat) {
    SeatUi& u = g.ui[seat];
    const COLORREF cur = g.cur.seatColors[seat];
    SendMessageW(u.style, CB_SETCURSEL, g.cur.cursorStyle[seat], 0);
    SendMessageW(u.color, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (int k = 0; k < 10; ++k) {
        SendMessageW(u.color, CB_ADDSTRING, 0, (LPARAM)S((Str)(sCol0 + k)));
        if (kColors[k] == cur) sel = k;
    }
    if (sel < 0) {      // a colour typed in the ini: kept, and shown
        wchar_t buf[64];
        swprintf(buf, 64, S(sColorCustom), (GetRValue(cur) << 16) | (GetGValue(cur) << 8) | GetBValue(cur));
        SendMessageW(u.color, CB_ADDSTRING, 0, (LPARAM)buf);
        sel = 10;
    }
    SendMessageW(u.color, CB_SETCURSEL, sel, 0);
    InvalidateRect(u.preview, nullptr, FALSE);
}

void OnColor(int seat) {
    const int sel = (int)SendMessageW(g.ui[seat].color, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= 10) return;          // the custom entry, unchanged
    g.cur.seatColors[seat] = kColors[sel];
    ShowLook(seat);
    InvalidateRect(g.panel, nullptr, FALSE);   // the swatch in the header
    Apply();
}

void OnStyle(int seat) {
    const int sel = (int)SendMessageW(g.ui[seat].style, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= kStyleCount) return;
    g.cur.cursorStyle[seat] = sel;
    ShowLook(seat);
    Apply();
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
    SendMessageW(u.fixed, BM_SETCHECK, accel ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(u.accel, BM_SETCHECK, accel ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(u.explain, S(accel ? sExplainAccel : sExplainFixed));

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

    const bool pad = seat == g.padSeat;
    ShowWindow(u.padLabel, pad ? SW_SHOWNA : SW_HIDE);
    ShowWindow(u.pad, pad ? SW_SHOWNA : SW_HIDE);
    if (pad) SendMessageW(u.pad, CB_SETCURSEL, (WPARAM)g.cur.touchpad, 0);

    ShowLook(seat);
    ShowButtons(seat);
}

// Applied by the engine at once: it seats the touchpad or cuts it off.
void OnPadMode(int seat) {
    const int sel = (int)SendMessageW(g.ui[seat].pad, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 2) return;
    g.cur.touchpad = (Config::Touchpad)sel;
    Apply();
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

// The controls of one seat's section.
void CreateSeatControls(int i) {
    const wchar_t* names[fFieldCount] = { S(sSensitivity), S(sSlow), S(sFast), S(sSpeed) };
    HWND p = g.panel;
    SeatUi& u = g.ui[i];
    u.padLabel = Make(p, L"STATIC", S(sPadMode), SS_LEFT, CtlId(i, cPadLabel));
    u.pad = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cPad));
    for (Str k : { sPadAuto, sPadOn, sPadOff }) SendMessageW(u.pad, CB_ADDSTRING, 0, (LPARAM)S(k));
    u.colorLabel = Make(p, L"STATIC", S(sColorLabel), SS_LEFT, CtlId(i, cColorLabel));
    u.color = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cColor));
    u.styleLabel = Make(p, L"STATIC", S(sStyleLabel), SS_LEFT, CtlId(i, cStyleLabel));
    u.style = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cStyle));
    for (int k = 0; k < kStyleCount; ++k)
        SendMessageW(u.style, CB_ADDSTRING, 0, (LPARAM)S((Str)(sStyle0 + k)));
    u.preview = Make(p, kPreviewClass, L"", 0, CtlId(i, cPreview));
    // Two modes, one choice: the radio buttons first, the sliders of the mode below.
    u.fixed   = Make(p, L"BUTTON", S(sFixed), WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, CtlId(i, cFixed));
    u.accel   = Make(p, L"BUTTON", S(sAccel), WS_TABSTOP | BS_AUTORADIOBUTTON, CtlId(i, cAccel));
    u.explain = Make(p, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | WS_GROUP, CtlId(i, cExplain));
    for (int f = 0; f < fFieldCount; ++f) {
        u.label[f]  = Make(p, L"STATIC", names[f], SS_LEFT | SS_NOPREFIX, CtlId(i, cLabel + f));
        u.value[f]  = Make(p, L"STATIC", L"", SS_RIGHT, CtlId(i, cValue + f));
        u.slider[f] = Make(p, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                           CtlId(i, cSlider + f));
        const Range& r = kRange[f];
        SendMessageW(u.slider[f], TBM_SETRANGEMIN, FALSE, (LPARAM)std::lround(r.lo * r.scale));
        SendMessageW(u.slider[f], TBM_SETRANGEMAX, FALSE, (LPARAM)std::lround(r.hi * r.scale));
        SendMessageW(u.slider[f], TBM_SETLINESIZE, 0, 1);
        SendMessageW(u.slider[f], TBM_SETPAGESIZE, 0, (LPARAM)std::lround(r.scale / 2));
    }
    u.screenLabel = Make(p, L"STATIC", S(sScreen), SS_LEFT, CtlId(i, cScreenLabel));
    u.screen = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cScreen));
    u.defaults = Make(p, L"BUTTON", S(sDefaults), WS_TABSTOP | BS_PUSHBUTTON, CtlId(i, cDefaults));

    u.learn  = Make(p, L"BUTTON", S(sLearn), WS_TABSTOP | BS_PUSHBUTTON, CtlId(i, cLearn));
    u.forget = Make(p, L"BUTTON", S(sForget), WS_TABSTOP | BS_PUSHBUTTON, CtlId(i, cForget));
    u.note   = Make(p, L"STATIC", L"", SS_LEFT, CtlId(i, cNote));
    u.list   = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cList));
    u.action = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cAction));
    for (int a = 0; a < kActCount; ++a) SendMessageW(u.action, CB_ADDSTRING, 0, (LPARAM)ActionName(a));
    u.keys   = Make(p, L"BUTTON", S(sKeysNone), WS_TABSTOP | BS_PUSHBUTTON, CtlId(i, cKeys));
    u.win    = Make(p, L"BUTTON", S(sWin), WS_TABSTOP | BS_AUTOCHECKBOX, CtlId(i, cWin));
    SetWindowSubclass(u.keys, KeysProc, kSubclassKeys, 0);
    ShowSeat(i);
}

void CreateControls(HWND hwnd) {
    HWND p = g.panel;
    for (int i = 0; i < g.seats; ++i) CreateSeatControls(i);
    g.autostart = Make(p, L"BUTTON", S(sAutostart), WS_TABSTOP | BS_AUTOCHECKBOX, kIdAutostart);
    ShowAutostart(ReadAutostart() == 1);
    g.hint   = Make(hwnd, L"STATIC", S(sHint), SS_LEFT, 0);
    g.status = Make(hwnd, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0);
    g.undo   = Make(hwnd, L"BUTTON", S(sUndo), WS_TABSTOP | BS_PUSHBUTTON, kIdUndo);
    g.close  = Make(hwnd, L"BUTTON", S(sClose), WS_TABSTOP | BS_DEFPUSHBUTTON, kIdClose);
}

// ---------------------------------------------------------------- scrolling

void ScrollTo(int pos) {
    SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_PAGE };
    GetScrollInfo(g.panel, SB_VERT, &si);
    int most = si.nMax - (int)si.nPage + 1;
    if (most < 0) most = 0;
    if (pos > most) pos = most;
    if (pos < 0) pos = 0;
    if (pos == g.scrollY) return;
    ScrollWindowEx(g.panel, 0, g.scrollY - pos, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    g.scrollY = pos;
    si.fMask = SIF_POS;
    si.nPos  = pos;
    SetScrollInfo(g.panel, SB_VERT, &si, TRUE);
    UpdateWindow(g.panel);
}

void OnPanelScroll(WPARAM wp) {
    SCROLLINFO si{ sizeof(si), SIF_ALL };
    GetScrollInfo(g.panel, SB_VERT, &si);
    int pos = si.nPos;
    switch (LOWORD(wp)) {
    case SB_LINEUP:        pos -= Px(24); break;
    case SB_LINEDOWN:      pos += Px(24); break;
    case SB_PAGEUP:        pos -= (int)si.nPage; break;
    case SB_PAGEDOWN:      pos += (int)si.nPage; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP:           pos = 0; break;
    case SB_BOTTOM:        pos = si.nMax; break;
    default: return;
    }
    ScrollTo(pos);
}

// ---------------------------------------------------------------- layout

constexpr int kMargin = 16, kColW = 250, kGap = 28, kPerRow = 4;

int Columns() { return g.seats < kPerRow ? g.seats : kPerRow; }

// Lays out one seat's section from (x, y); returns where it ends.
int LayoutSeat(int i, int x, int y) {
    SeatUi& u = g.ui[i];
    const int colW = Px(kColW);
    auto place = [](HWND h, int px, int py, int w, int hgt) {
        SetWindowPos(h, nullptr, px, py, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    u.header = { x, y, x + colW, y + Px(24) };
    y += Px(34);
    if (i == g.padSeat) {
        place(u.padLabel, x, y, colW, Px(18));
        y += Px(20);
        place(u.pad, x, y, colW, Px(200));
        y += Px(32);
    }
    place(u.colorLabel, x, y, colW, Px(18));
    y += Px(20);
    place(u.color, x, y, colW, Px(200));
    y += Px(32);
    place(u.styleLabel, x, y, colW, Px(18));
    y += Px(20);
    place(u.style, x, y, colW, Px(200));
    y += Px(32);
    place(u.preview, x, y, colW, Px(112));
    y += Px(112) + Px(20);
    place(u.fixed, x, y, colW, Px(22));
    y += Px(24);
    place(u.accel, x, y, colW, Px(22));
    y += Px(26);
    place(u.explain, x, y, colW, Px(54));
    y += Px(58);
    for (int f = 0; f < fFieldCount; ++f) {
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
    y += Px(28) + Px(20);

    // Learned buttons.
    const int forgetW = Px(84);
    place(u.learn, x, y, colW - forgetW - Px(6), Px(28));
    place(u.forget, x + colW - forgetW, y, forgetW, Px(28));
    y += Px(34);
    place(u.note, x, y, colW, Px(50));
    y += Px(54);
    place(u.list, x, y, colW, Px(200));
    y += Px(30);
    place(u.action, x, y, colW, Px(200));
    y += Px(30);
    const int winW = Px(54);
    place(u.keys, x, y, colW - winW - Px(6), Px(26));
    place(u.win, x + colW - winW, y, winW, Px(26));
    y += Px(26);
    return y;
}

// Lays out every control for the current DPI; returns the client size.
SIZE Layout(HWND hwnd) {
    if (g.font) DeleteObject(g.font);
    if (g.bold) DeleteObject(g.bold);
    g.font = CreateFontW(-MulDiv(9, (int)g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.bold = CreateFontW(-MulDiv(11, (int)g.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                         0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    EnumChildWindows(hwnd, SetFontProc, (LPARAM)g.font);
    auto place = [](HWND h, int x, int y, int w, int hgt) {
        SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    // The panel is laid out unscrolled, then scrolled back to the top.
    g.scrollY = 0;
    const int colW = Px(kColW);

    // General settings first, across the full width. Later ones go under the
    // checkbox; the seats below follow whatever height this section takes.
    const int genW = Columns() * colW + (Columns() - 1) * Px(kGap);
    int gy = Px(kMargin);
    g.general = { Px(kMargin), gy, Px(kMargin) + genW, gy + Px(24) };
    gy += Px(34);
    place(g.autostart, Px(kMargin), gy, genW, Px(22));
    gy += Px(22);

    int bottom = gy, rowTop = gy + Px(30), rowEnd = 0;
    for (int i = 0; i < g.seats; ++i) {
        if (i > 0 && i % kPerRow == 0) { rowTop = rowEnd + Px(28); rowEnd = 0; }
        const int x = Px(kMargin) + (i % kPerRow) * (colW + Px(kGap));
        const int end = LayoutSeat(i, x, rowTop);
        if (end > rowEnd) rowEnd = end;
        if (end > bottom) bottom = end;
    }
    const int width    = Px(kMargin) * 2 + Columns() * colW + (Columns() - 1) * Px(kGap);
    const int contentH = bottom + Px(kMargin);
    const int footerH  = Px(12) + Px(86) + Px(28) + Px(30) + Px(kMargin);

    // The panel takes the height the screen allows; what does not fit scrolls.
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wr{}, cr{};
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    const int chrome    = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    const int maxClient = (mi.rcWork.bottom - mi.rcWork.top) - chrome - Px(8);
    int panelH = contentH;
    const bool scrolls = panelH + footerH > maxClient;
    if (scrolls) panelH = maxClient - footerH;
    if (panelH < Px(200)) panelH = Px(200);
    const int sbW = scrolls ? ScrollBarWidth() : 0;

    place(g.panel, 0, 0, width + sbW, panelH);
    SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMin  = 0;
    si.nMax  = contentH - 1;
    si.nPage = (UINT)(scrolls ? panelH : contentH + 1);
    si.nPos  = 0;
    SetScrollInfo(g.panel, SB_VERT, &si, TRUE);

    const int inner = width - Px(kMargin) * 2;
    int y = panelH + Px(12);
    place(g.hint, Px(kMargin), y, inner, Px(80));
    y += Px(86);
    place(g.status, Px(kMargin), y, inner, Px(18));
    y += Px(28);
    const int closeW = Px(100), undoW = Px(190), h = Px(30);
    place(g.close, width + sbW - Px(kMargin) - closeW, y, closeW, h);
    place(g.undo,  width + sbW - Px(kMargin) - closeW - Px(10) - undoW, y, undoW, h);
    y += h + Px(kMargin);
    InvalidateRect(g.panel, nullptr, TRUE);
    InvalidateRect(hwnd, nullptr, TRUE);
    return { width + sbW, y };
}

RECT WindowRectFor(SIZE client, DWORD style) {
    RECT r{ 0, 0, client.cx, client.cy };
    using Fn = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (fn) fn(&r, style, FALSE, 0, g.dpi);
    else    AdjustWindowRectEx(&r, style, FALSE, 0);
    return r;
}

void PaintPanel(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    SetBkMode(dc, TRANSPARENT);
    SetViewportOrgEx(dc, 0, -g.scrollY, nullptr);
    HGDIOBJ old = SelectObject(dc, g.bold);
    {
        const RECT& h = g.general;
        RECT tr = h;
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        DrawTextW(dc, S(sGeneral), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT line{ h.left, h.bottom + Px(4), h.right, h.bottom + Px(5) };
        FillRect(dc, &line, GetSysColorBrush(COLOR_3DLIGHT));
    }
    for (int i = 0; i < g.seats; ++i) {
        const RECT& h = g.ui[i].header;
        const int sw = Px(16), top = h.top + (h.bottom - h.top - sw) / 2;
        RECT swatch{ h.left, top, h.left + sw, top + sw };
        HBRUSH b = CreateSolidBrush(g.cur.seatColors[i]);
        FillRect(dc, &swatch, b);
        DeleteObject(b);
        FrameRect(dc, &swatch, GetSysColorBrush(COLOR_GRAYTEXT));
        wchar_t title[64];
        const bool pad = i == g.padSeat;
        if (pad) wcscpy_s(title, S(sTouchpad));
        else     swprintf(title, 64, S(sSeat), i + 1);
        const bool absent = !(g.present & (1u << i));
        if (absent) wcscat_s(title, S(pad ? sPadCut : sSeatAbsent));
        RECT tr{ h.left + sw + Px(8), h.top, h.right, h.bottom };
        SetTextColor(dc, GetSysColor(absent ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        DrawTextW(dc, title, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT line{ h.left, h.bottom + Px(4), h.right, h.bottom + Px(5) };
        FillRect(dc, &line, GetSysColorBrush(COLOR_3DLIGHT));
    }
    SelectObject(dc, old);
    EndPaint(hwnd, &ps);
}

// The preview of a seat: the arrow on four backgrounds that hide pointers (white,
// dark, green, yellow) and four other shapes of the same set underneath.
struct PreviewCache {
    COLORREF color = 0;
    int      style = -1;
    PointerImage img[5];     // arrow, hand, text bar, resize, forbidden
    bool     ok[5] = {};
} g_preview[kMaxSeats];

void PaintPreview(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    int seat = 0, ctl = 0;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    if (SeatOf(GetDlgCtrlID(hwnd), seat, ctl) && W > 0 && H > 0) {
        static const PointerShape kShapes[5] = { PointerShape::Arrow, PointerShape::Hand,
            PointerShape::IBeam, PointerShape::SizeNWSE, PointerShape::No };
        static const int kRow[2][4] = { { 0, 0, 0, 0 }, { 1, 2, 3, 4 } };
        static const COLORREF kBack[4] = { RGB(255, 255, 255), RGB(32, 32, 36),
                                           RGB(60, 170, 70),   RGB(250, 230, 60) };
        PreviewCache& pc = g_preview[seat];
        const COLORREF color = g.cur.seatColors[seat];
        const int style = g.cur.cursorStyle[seat];
        if (pc.style != style || pc.color != color) {
            pc.style = style; pc.color = color;
            for (int k = 0; k < 5; ++k) pc.ok[k] = RenderPointer(kShapes[k], color, style, pc.img[k]);
        }

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = W;
        bi.bmiHeader.biHeight      = -H;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) {
            BYTE* px = (BYTE*)bits;
            for (int row = 0; row < 2; ++row) {
                for (int col = 0; col < 4; ++col) {
                    const int x0 = W * col / 4, x1 = W * (col + 1) / 4;
                    const int y0 = H * row / 2, y1 = H * (row + 1) / 2;
                    const COLORREF back = kBack[col];
                    for (int y = y0; y < y1; ++y)
                        for (int x = x0; x < x1; ++x) {
                            BYTE* d = px + ((size_t)y * W + x) * 4;
                            d[0] = GetBValue(back); d[1] = GetGValue(back); d[2] = GetRValue(back); d[3] = 255;
                        }
                    const int k = kRow[row][col];
                    if (!pc.ok[k]) continue;
                    const PointerImage& im = pc.img[k];
                    const int ox = (x0 + x1 - im.w) / 2, oy = (y0 + y1 - im.h) / 2;
                    for (int y = 0; y < im.h; ++y) {
                        const int dy = oy + y;
                        if (dy < y0 || dy >= y1) continue;
                        for (int x = 0; x < im.w; ++x) {
                            const int dx = ox + x;
                            if (dx < x0 || dx >= x1) continue;
                            const BYTE* sp = im.bgra.data() + ((size_t)y * im.w + x) * 4;
                            BYTE* d = px + ((size_t)dy * W + dx) * 4;
                            const int inv = 255 - sp[3];       // premultiplied source over
                            for (int c = 0; c < 3; ++c) d[c] = (BYTE)(sp[c] + d[c] * inv / 255);
                        }
                    }
                }
            }
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, dib);
            BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        if (dib) DeleteObject(dib);
        FrameRect(dc, &rc, GetSysColorBrush(COLOR_GRAYTEXT));
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) { PaintPreview(hwnd); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The panel only hosts the controls: it hands their notifications to the
// window, scrolls, and paints the seat headers.
LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    case WM_VSCROLL:
        OnPanelScroll(wp);
        return 0;
    case WM_MOUSEWHEEL:
        ScrollTo(g.scrollY - (int)((short)HIWORD(wp) * Px(48) / WHEEL_DELTA));
        return 0;
    case WM_PAINT:
        PaintPanel(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- events

bool SeatOf(int id, int& seat, int& ctl) {
    if (id < kIdBase || id >= kIdBase + g.seats * kPerSeat) return false;
    seat = (id - kIdBase) / kPerSeat;
    ctl  = (id - kIdBase) % kPerSeat;
    return true;
}

// Lays the window out again for the seats it now has, keeping it on its screen.
void FitWindow(HWND hwnd) {
    const SIZE client = Layout(hwnd);
    const RECT wr = WindowRectFor(client, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE));
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    RECT cur{};
    GetWindowRect(hwnd, &cur);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    int x = cur.left, y = cur.top;
    if (x + w > wa.right)  x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top)  y = wa.top;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// The engine says which mice are plugged in. A mouse plugged in for the first
// time has a seat in the file by now: give it its section, without closing the
// window. A mouse unplugged keeps its section (its values are kept), marked.
void OnSeats(HWND hwnd, UINT present) {
    g.present = present;
    Config disk;
    if (!disk.Load(g.path)) { InvalidateRect(g.panel, nullptr, TRUE); return; }
    const int pad = TouchpadSeat(disk);
    const bool padMoved = pad != g.padSeat;
    g.padSeat = pad;
    if (disk.seatCount > g.seats) {
        const int old = g.seats;
        g.seats = disk.seatCount;
        EnsureSeats(g.opened, g.seats);
        EnsureSeats(g.cur, g.seats);
        for (int i = old; i < g.seats; ++i) {
            for (Config* c : { &g.opened, &g.cur }) {
                c->sensitivity[i] = disk.sensitivity[i];
                c->accel[i]       = disk.accel[i];
                c->screen[i]      = disk.screen[i];
                c->buttons[i]     = disk.buttons[i];
                c->seatColors[i]  = disk.seatColors[i];
                c->cursorStyle[i] = disk.cursorStyle[i];
            }
            CreateSeatControls(i);
        }
        FitWindow(hwnd);
    } else if (padMoved) {
        for (int i = 0; i < g.seats; ++i) ShowSeat(i);
        FitWindow(hwnd);
    }
    InvalidateRect(g.panel, nullptr, TRUE);
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
    if (id == kIdAutostart) { OnAutostart(hwnd); return; }
    if (id == kIdUndo) {
        for (int i = 0; i < g.seats; ++i) {
            g.cur.sensitivity[i] = g.opened.sensitivity[i];
            g.cur.accel[i]       = g.opened.accel[i];
            g.cur.screen[i]      = g.opened.screen[i];
            g.cur.seatColors[i]  = g.opened.seatColors[i];
            g.cur.cursorStyle[i] = g.opened.cursorStyle[i];
            g.cur.buttons[i]     = g.opened.buttons[i];
            g.cur.touchpad       = g.opened.touchpad;
            g.sel[i] = 0;
            g.note[i].clear();
            ShowSeat(i);
        }
        Apply();
        return;
    }
    int seat, ctl;
    if (!SeatOf(id, seat, ctl)) return;
    if (ctl == cAccel || ctl == cFixed) {
        const bool on = ctl == cAccel;
        if (g.cur.accel[seat].on != on) {
            g.cur.accel[seat].on = on;
            Apply();
        }
        ShowSeat(seat);
    } else if (ctl == cDefaults) {
        g.cur.sensitivity[seat] = 1.0;
        g.cur.accel[seat]       = Config::Accel{};
        g.cur.screen[seat].clear();
        g.cur.cursorStyle[seat] = (int)kStyleBright;
        if (seat == g.padSeat) g.cur.touchpad = Config::Touchpad::Auto;
        ShowSeat(seat);
        Apply();
    } else if (ctl == cLearn) {
        OnLearnClick(hwnd, seat);
    } else if (ctl == cForget) {
        OnForget(seat);
    } else if (ctl == cKeys) {
        OnKeysClick(seat);
    } else if (ctl == cWin) {
        OnWinToggle(seat);
    }
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HSCROLL:
        if (lp) OnSlider(hwnd, (HWND)lp);
        return 0;
    case WM_MOUSEWHEEL:
        return SendMessageW(g.panel, msg, wp, lp);
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) OnCommand(hwnd, LOWORD(wp));
        else if (HIWORD(wp) == CBN_SELCHANGE) {
            int seat, ctl;
            if (SeatOf(LOWORD(wp), seat, ctl)) {
                if (ctl == cScreen)      OnScreen(seat);
                else if (ctl == cColor)  OnColor(seat);
                else if (ctl == cStyle)  OnStyle(seat);
                else if (ctl == cList)   OnListSelect(seat);
                else if (ctl == cAction) OnActionSelect(seat);
                else if (ctl == cPad)    OnPadMode(seat);
            }
        }
        return 0;
    case kLearnedMsg:
        OnLearned(hwnd, (int)wp, lp);
        return 0;
    case kSeatsMsg:
        OnSeats(hwnd, (UINT)wp);
        return 0;
    case WM_TIMER:
        if (wp == kTimerAutostart) { PollAutostart(hwnd); return 0; }
        if (wp == kTimerLearn) {            // the engine never answered
            const int seat = g.learning;
            EndLearning(hwnd);
            if (seat >= 0 && seat < g.seats) Note(seat, S(sNothing));
            return 0;
        }
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
        if (g.autoHelper) { CloseHandle(g.autoHelper); g.autoHelper = nullptr; }
        if (g.learning >= 0) {              // stop the engine listening to a mouse nobody watches
            if (HWND engine = FindWindowW(kEngineClass, nullptr)) PostMessageW(engine, kLearnMsg, 0, 0);
            g.learning = -1;
        }
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
    g.padSeat = TouchpadSeat(g.opened);
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

    WNDCLASSEXW pc{};
    pc.cbSize        = sizeof(pc);
    pc.lpfnWndProc   = PanelProc;
    pc.hInstance     = inst;
    pc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    pc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    pc.lpszClassName = kPanelClass;
    RegisterClassExW(&pc);

    WNDCLASSEXW vc{};
    vc.cbSize        = sizeof(vc);
    vc.lpfnWndProc   = PreviewProc;
    vc.hInstance     = inst;
    vc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    vc.lpszClassName = kPreviewClass;
    RegisterClassExW(&vc);

    // Not resizable, never topmost: it must not stand in the way of anything.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kSettingsClass, S(sTitle), style,
                                CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    // The engine runs elevated and this window may not: let its answer through.
    ChangeWindowMessageFilterEx(hwnd, kLearnedMsg, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, kSeatsMsg, MSGFLT_ALLOW, nullptr);

    g.panel = CreateWindowExW(WS_EX_CONTROLPARENT, kPanelClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                              0, 0, 100, 100, hwnd, nullptr, inst, nullptr);
    if (!g.panel) return 1;

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
    // Ask which mice are plugged in; the answer comes back as kSeatsMsg.
    if (HWND engine = FindWindowW(kEngineClass, nullptr)) PostMessageW(engine, kSeatsMsg, 0, 0);
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
