// openMouse — multi-pointer input for Windows
// SPDX-License-Identifier: MIT
//
// Gives each physical mouse its own pointer on one shared Windows desktop, and
// optionally binds a keyboard to each pointer so that keystrokes are steered to
// the window that pointer last clicked.
//
// Design notes worth knowing before reading:
//
//  * Windows has ONE hardware cursor and ONE foreground window per desktop.
//    We do not change that. Exactly one seat "owns" the real cursor at any
//    moment; every other seat's pointer is drawn by us as a layered window.
//    Clicking transfers ownership. This is a deliberate turn-taking model —
//    see README, "What this cannot do".
//
//  * RIDEV_NOLEGACY suppresses OS-generated legacy messages for a whole device
//    CLASS, never a single device. So capturing one mouse means capturing all
//    of them and re-injecting for the active seat. Same for keyboards. This is
//    the reason a naive per-device implementation is impossible in user mode.
//
//  * Because we take over input system-wide, a hang in this process is a hang
//    in the user's input. A watchdog thread unregisters us if the message loop
//    stalls. Do not remove it.

#include "openmouse.h"
#include <shellapi.h>   // CommandLineToArgvW — not pulled in by WIN32_LEAN_AND_MEAN
#include <wtsapi32.h>   // lock/unlock notifications
#include <atomic>
#include <set>
#include <thread>
#include <cstdio>
#include <cmath>

namespace om {
namespace {

// ---------------------------------------------------------------- state

Config                    g_cfg;
Seat                      g_seats[kMaxSeats];
CursorOverlay             g_overlay[kMaxSeats];
std::map<HANDLE, int>     g_mouseSeat;      // device handle -> seat
std::map<HANDLE, int>     g_keyboardSeat;

// Interfaces deliberately left dead. This machine's receivers expose extra
// HID collections beyond the ones --identify measured; letting those default
// to seat 0 means a ghost interface can double-drive a seat. Once measured
// bindings exist, unmatched interfaces land here and their events are
// dropped. A device whose handle is in NEITHER map (hotplugged after the
// last rescan) still falls through to seat 0 so new hardware is never dead.
std::set<HANDLE>          g_deadMice;
std::set<HANDLE>          g_deadKeyboards;
int                       g_activeSeat = 0;  // owns the real system cursor
HWND                      g_hwnd = nullptr;
HINSTANCE                 g_inst = nullptr;  // for the overlays created at run time

// Seats are created while the engine runs, one per newly seen mouse. Ready:
// state and overlay exist. Present: a bound mouse is plugged in right now.
// A seat whose mouse is unplugged hides its arrow, kept for its return.
bool                      g_seatReady[kMaxSeats]   = {};
bool                      g_seatPresent[kMaxSeats] = {};
bool                      g_captureActive = false;

std::atomic<uint64_t>     g_heartbeat{0};
std::atomic<bool>         g_running{true};

// Runtime telemetry. Counters only in the hot path — the periodic timer does
// the formatting and the file write. Without this there is no way to tell
// "mouse 2 was routed to the wrong seat" apart from "mouse 2 was routed
// correctly and something else moved the cursor".
struct SeatStats { uint64_t moves = 0, buttons = 0, cursorSets = 0, overlaySets = 0; };
SeatStats                 g_stats[kMaxSeats];
uint64_t                  g_unboundEvents = 0;   // events that fell through to seat 0
std::map<HANDLE, uint64_t> g_perDevice;
bool                      g_overlayOk[kMaxSeats] = {};

// Counted at the point of arrival, before any filtering, so that "no WM_INPUT
// at all" can be told apart from "WM_INPUT arrived and was discarded".
uint64_t g_wmInputTotal = 0, g_rawMouse = 0, g_rawKbd = 0;
uint64_t g_dropInjected = 0, g_dropNullDev = 0;

// Per-event tracing. Off unless --verbose: these fire on every click, every
// ownership change and twice a second, and each line is a synchronous file
// write on the input thread. Invaluable while diagnosing, pure latency and
// unbounded log growth in normal use.
bool g_verbose = false;

// Latency measurement (--perf). Accumulated always — a few QPC reads per
// event — but only written once every 2 s, and only with --perf, so it can be
// run without --verbose (which changes nothing here, but floods the log).
// "lag" is the age of a WM_INPUT when we dequeue it: if processing falls
// behind the mouse, it climbs, and that IS the cursor trailing the hand.
bool g_perfLog = false;
struct PerfStats {
    uint64_t events = 0, flushes = 0;
    int64_t  tRead = 0, tPin = 0, tOverlay = 0, tShell = 0, tHit = 0, tClass = 0, tSettle = 0, tTotal = 0, tMax = 0;
    // Largest single-event value of each step, reset with the rest every 2 s.
    int64_t  mRead = 0, mPin = 0, mOverlay = 0, mShell = 0, mHit = 0, mClass = 0;
    DWORD    lagMax = 0;
    uint64_t lagSum = 0;
} g_perf;
int64_t g_qpcFreq = 0;
inline int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// Scancodes currently held down, per seat, for the panic combo and to keep one
// seat's modifiers out of the other seat's typing. Tracked manually because
// with NOLEGACY the async key-state table only reflects what we re-inject.
bool                      g_keyDown[kMaxSeats][512] = {};

// True only when keyboards are captured with RIDEV_NOLEGACY and we are
// responsible for re-injecting them. When false we still receive keyboard raw
// input — that is what keeps the panic key alive — but Windows is delivering
// the keystrokes itself and we must not inject.
bool                      g_keyboardRouting = false;

constexpr USHORT SC_LCTRL = 0x1D, SC_LALT = 0x38, SC_LSHIFT = 0x2A, SC_Q = 0x10;

// ---------------------------------------------------------------- learned buttons
//
// A mouse often has more to say than the five buttons Windows maps: its own
// keyboard interface (a button that sends Windows+D), media and system keys,
// messages only the manufacturer's driver reads. The settings window asks the
// engine to listen to ONE mouse and keep the first thing it sends (kLearnMsg);
// the signatures kept are then matched here (ButtonMap).
//
// What is listened to is the mouse's own hardware, nothing else: a keyboard or
// media interface is looked at only when it shares its hardware id with a mouse
// bound to a seat (g_ownerSeat) and is not a keyboard the ini binds itself. A
// real keyboard is never in that table, so its keys are neither retained nor
// logged; and even on a receiver shared by a mouse and a keyboard, only keys
// that do not type (modifiers, F keys, navigation, media) can be learned.
int                   g_learnSeat = -1;          // the seat being listened to, or -1
DWORD                 g_learnArmAt = 0;          // events before this are the click that asked
std::map<HANDLE, int>    g_ownerSeat;            // keyboard / HID interface -> seat of its mouse
std::map<HANDLE, USHORT> g_hidPage;              // HID interface -> its usage page
bool                  g_haveKeyMaps = false;     // some seat has a learned key or HID button
bool                  g_held[kMaxSeats][kMaxLearned] = {};   // pressed and not yet released
constexpr UINT_PTR    kLearnTimer = 5;
constexpr wchar_t     kSettingsClassName[] = L"openMouse.Settings";

// ---------------------------------------------------------------- cursor
//
// Windows keeps moving the single hardware cursor for every physical mouse
// even under RIDEV_NOLEGACY — measured, see the note in OnRawMouse. Snapping
// it back after each stray event does restore the position, but the correction
// races the OS at mouse polling rates and reads as violent jitter.
//
// So stop fighting it. Hide the hardware cursor and draw every seat, including
// the one that owns it. The real cursor still drifts, but invisibly, and the
// only moment its position matters is the instant of a click — which we set
// explicitly anyway.
//
// Loaded dynamically so the binary still starts if magnification.dll is
// missing; the engine then falls back to the snap-back correction.
struct MagApi {
    HMODULE lib = nullptr;
    BOOL (WINAPI *Initialize)()            = nullptr;
    BOOL (WINAPI *Uninitialize)()          = nullptr;
    BOOL (WINAPI *ShowSystemCursor)(BOOL)  = nullptr;

    bool Load() {
        if (ShowSystemCursor) return true;
        // From System32 only: the engine runs elevated, and the folder it sits
        // in is not necessarily one only an administrator can write to.
        lib = LoadLibraryExW(L"magnification.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib) return false;
        Initialize       = (BOOL (WINAPI*)())        (void*)GetProcAddress(lib, "MagInitialize");
        Uninitialize     = (BOOL (WINAPI*)())        (void*)GetProcAddress(lib, "MagUninitialize");
        ShowSystemCursor = (BOOL (WINAPI*)(BOOL))    (void*)GetProcAddress(lib, "MagShowSystemCursor");
        return Initialize && ShowSystemCursor;
    }
};

MagApi g_mag;
bool   g_magReady     = false;
bool   g_cursorHidden = false;

bool HideSystemCursor() {
    if (g_cursorHidden) return true;
    if (!g_mag.Load()) { LogLine(L"[cursor] magnification.dll unavailable"); return false; }
    if (!g_magReady) {
        if (!g_mag.Initialize()) { LogLine(L"[cursor] MagInitialize failed: %lu", GetLastError()); return false; }
        g_magReady = true;
    }
    if (!g_mag.ShowSystemCursor(FALSE)) {
        LogLine(L"[cursor] MagShowSystemCursor(FALSE) failed: %lu", GetLastError());
        return false;
    }
    g_cursorHidden = true;
    LogLine(L"[cursor] hardware cursor hidden — all seats drawn as overlays");
    return true;
}

// Must run on every exit path. If this process dies without restoring, the
// desktop is left with no visible pointer at all.
void RestoreSystemCursor() {
    ClipCursor(nullptr);   // release the 1px cage before anything else
    if (g_magReady && g_mag.ShowSystemCursor) g_mag.ShowSystemCursor(TRUE);
    if (g_magReady && g_mag.Uninitialize)     g_mag.Uninitialize();
    if (g_cursorHidden) LogLine(L"[cursor] hardware cursor restored");
    g_cursorHidden = false;
    g_magReady     = false;
}

// ---------------------------------------------------------------- injection

void RequestExit();

// Last-resort brake against a runaway injection loop. The watchdog cannot
// catch one, because a storm arrives as WM_INPUT and keeps the heartbeat
// climbing — the process looks perfectly healthy while it floods the desktop
// with clicks. Injection is only ever driven by buttons and wheel, so honest
// traffic is nowhere near this rate; anything above it means we are feeding on
// our own output and the only safe move is to let go of every device.
bool InjectAllowed() {
    constexpr DWORD kWindowMs      = 1000;
    constexpr int   kMaxPerWindow  = 300;

    static DWORD windowStart = 0;
    static int   count       = 0;
    static bool  tripped     = false;

    if (tripped) return false;

    const DWORD now = GetTickCount();
    if (windowStart == 0 || (int)(now - windowStart) >= (int)kWindowMs) {
        windowStart = now;
        count       = 0;
    }
    if (++count > kMaxPerWindow) {
        tripped = true;
        LogLine(L"[emergency] %d injections in under %lu ms — runaway loop.",
                count, kWindowMs);
        LogLine(L"[emergency] releasing all input capture and exiting.");
        RequestExit();
        return false;
    }
    return true;
}

// SendInput absolute coordinates are normalised 0..65535. With
// MOUSEEVENTF_VIRTUALDESK that range spans the whole virtual desktop rather
// than just the primary monitor, which is what we want for multi-monitor.
void SendMouseAt(POINT pt, DWORD flags, DWORD mouseData = 0) {
    if (!InjectAllowed()) return;
    const RECT v = VirtualScreenRect();
    const int vw = (v.right - v.left) > 1 ? (v.right - v.left) : 2;
    const int vh = (v.bottom - v.top) > 1 ? (v.bottom - v.top) : 2;

    INPUT in{};
    in.type            = INPUT_MOUSE;
    in.mi.dx           = (LONG)(((int64_t)(pt.x - v.left) * 65535) / (vw - 1));
    in.mi.dy           = (LONG)(((int64_t)(pt.y - v.top)  * 65535) / (vh - 1));
    in.mi.mouseData    = mouseData;
    in.mi.dwFlags      = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dwExtraInfo  = kInjectTag;
    SendInput(1, &in, sizeof(INPUT));
}

void SendKeyScan(USHORT scan, bool up, bool e0) {
    if (!InjectAllowed()) return;
    INPUT in{};
    in.type           = INPUT_KEYBOARD;
    in.ki.wVk         = 0;
    in.ki.wScan       = scan;
    in.ki.dwFlags     = KEYEVENTF_SCANCODE
                      | (up ? KEYEVENTF_KEYUP : 0)
                      | (e0 ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = kInjectTag;
    SendInput(1, &in, sizeof(INPUT));
}

// SetForegroundWindow is rate-limited by the foreground lock. Attaching our
// input queue to the current foreground thread lifts that restriction, which
// is the documented workaround and what every focus-follows tool uses.
bool ClaimForeground(HWND target) {
    if (!target || !IsWindow(target)) return false;
    HWND root = GetAncestor(target, GA_ROOT);
    if (!root) root = target;
    if (GetForegroundWindow() == root) return true;

    // Windows 11 restricts SetForegroundWindow heavily, and it returns TRUE in
    // cases where it merely flashed the taskbar button and left the foreground
    // alone. Attaching to the outgoing foreground thread is the documented
    // workaround, but attaching to the TARGET thread as well is what makes it
    // stick when the two windows belong to different processes — which is the
    // whole point here, one seat in Edge and the other somewhere else.
    const DWORD me  = GetCurrentThreadId();
    const DWORD fg  = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD tgt = GetWindowThreadProcessId(root, nullptr);

    if (fg  && fg  != me)              AttachThreadInput(me, fg,  TRUE);
    if (tgt && tgt != me && tgt != fg) AttachThreadInput(me, tgt, TRUE);

    BringWindowToTop(root);
    SetForegroundWindow(root);
    SetActiveWindow(root);

    if (tgt && tgt != me && tgt != fg) AttachThreadInput(me, tgt, FALSE);
    if (fg  && fg  != me)              AttachThreadInput(me, fg,  FALSE);

    // Trust the observed state, not the return value.
    return GetForegroundWindow() == root;
}

// Class name of a window, for diagnostics only.
const wchar_t* ClassOf(HWND h) {
    static wchar_t buf[64];
    buf[0] = 0;
    if (h) GetClassNameW(h, buf, 63);
    return buf;
}

// ---------------------------------------------------------------- seats

// The OS keeps applying every physical mouse's motion to the one hardware
// cursor even under NOLEGACY — the drift measured at the start of this
// project. Most of the time drift is invisible, and the cursor is re-parked
// before anything reads it. But during a drag the cursor position IS the
// drag: seat B physically steering the hidden cursor steered seat A's grab,
// and B moving during A's click smeared it into a text selection.
//
// The cage fixes it at the source: a one-pixel ClipCursor rect pinned to the
// active seat's position. The OS clamps every other seat's deltas against
// the cage, so only the seat that owns the cursor can move it. The cage is
// moved on every active-seat event, reasserted by the timer (the system
// drops clip rects on some foreground changes), and released on EVERY exit
// path — a leaked cage would trap the user's only pointer in one pixel.
POINT g_pinnedAt{ 0, 0 };
bool  g_pinned = false;

extern bool g_eatMoves;   // defined with the gate, further down

void PinCursor(POINT p) {
    if (g_eatMoves) {
        // Physical moves never reach the cursor, so there is nothing to fence
        // it against — a plain warp is enough. No clip rect means external
        // automation, accessibility tools and anything else that positions
        // the pointer keep working normally while openMouse runs.
        SetCursorPos(p.x, p.y);
        if (g_pinned) { ClipCursor(nullptr); g_pinned = false; }
        g_pinnedAt = p;
        return;
    }

    // Fallback when move-eating had to be switched off. Clip FIRST: measured
    // on this machine, SetCursorPos to a point outside the active clip rect
    // is silently clamped (does nothing), while moving the clip rect drags
    // the cursor into it. So the rect move does the positioning and the
    // SetCursorPos afterwards is just exactness.
    const RECT r{ p.x, p.y, p.x + 1, p.y + 1 };
    ClipCursor(&r);
    SetCursorPos(p.x, p.y);
    g_pinnedAt = p;
    g_pinned   = true;
}

void UnpinCursor() {
    ClipCursor(nullptr);
    g_pinned = false;
}


// The taskbar, Start menu and tray flyouts render in shell z-bands ABOVE the
// topmost band; no normal window — including our overlays — can draw over
// them. The real hardware cursor CAN. So while the active seat's pointer is
// over shell UI, the hidden real cursor (already pinned to that seat) is made
// visible and the seat's overlay hidden, restoring a usable pointer over the
// taskbar, Start and the tray. Reverts the moment the pointer leaves.
bool g_overShell = false;
DWORD g_lastShellFlip = 0;                       // tick of the last accepted flip, 0 = none yet
constexpr DWORD      kShellMinGapMs = 120;       // OP16: minimum time between two flips
constexpr UINT_PTR   kShellRecheckTimer = 6;

bool IsShellWindowAt(POINT p) {
    int64_t t0 = Qpc();
    HWND h = WindowFromPoint(p);
    g_perf.tHit += Qpc() - t0;
    if (!h) return false;
    HWND root = GetAncestor(h, GA_ROOT);
    wchar_t cls[64] = {};
    t0 = Qpc();
    GetClassNameW(root ? root : h, cls, 63);
    g_perf.tClass += Qpc() - t0;
    return IsShellWindowClass(cls);
}

void UpdateShellCursorFallback(int seat) {
    if (!g_cursorHidden || !g_magReady || !g_mag.ShowSystemCursor) return;
    const int64_t t0 = Qpc();
    const bool over = IsShellWindowAt(g_seats[seat].pos);
    g_perf.tShell += Qpc() - t0;
    if (over == g_overShell) return;

    // OP16: a pointer resting on the edge of the taskbar made this detector
    // change its mind hundreds of times a second (725 flips in one log), each
    // flip costing a ShowSystemCursor and an overlay show/hide that starved the
    // arrows. Two flips are never closer than kShellMinGapMs; a refused flip is
    // re-read by a one-shot timer, so a pointer that has really moved on still
    // ends up in the right state when it stops.
    const DWORD now = GetTickCount();
    if (g_lastShellFlip && (now - g_lastShellFlip) < kShellMinGapMs) {
        if (g_hwnd)
            SetTimer(g_hwnd, kShellRecheckTimer,
                     kShellMinGapMs - (now - g_lastShellFlip) + 5, nullptr);
        return;
    }
    g_lastShellFlip = now;
    g_overShell = over;
    g_mag.ShowSystemCursor(over ? TRUE : FALSE);
    g_overlay[seat].Show(!over);
    LogLine(over ? L"[shell] pointer over shell UI — real cursor shown"
                 : L"[shell] left shell UI — overlay resumed");
}

void SetActiveSeat(int seat) {
    if (seat == g_activeSeat || seat < 0 || seat >= g_cfg.seatCount) return;

    const int prev = g_activeSeat;
    g_activeSeat = seat;
    if (g_verbose) LogLine(L"[owner] seat %d -> seat %d", prev, seat);

    // With the hardware cursor hidden every seat keeps its own overlay at all
    // times, so there is nothing to show or hide on handover — only the real
    // cursor to reposition, so the click lands where this seat is pointing.
    if (!g_cursorHidden) {
        if (prev >= 0 && prev < g_cfg.seatCount) {
            g_overlay[prev].MoveTo(g_seats[prev].pos);
            g_overlay[prev].Show(true);
        }
        g_overlay[seat].Show(false);
    } else if (g_overShell) {
        // The outgoing owner was in shell-fallback mode (real cursor shown,
        // its overlay hidden). It is no longer the cursor's seat: give it its
        // overlay back, re-hide the real cursor, and re-evaluate for the new
        // owner's position.
        g_overlay[prev].Show(true);
        if (g_magReady && g_mag.ShowSystemCursor) g_mag.ShowSystemCursor(FALSE);
        g_overShell = false;
    }
    PinCursor(g_seats[seat].pos);
    if (g_cursorHidden) UpdateShellCursorFallback(seat);
}

// Moves only update the logical position; the costly part — SetCursorPos,
// SetWindowPos on the overlay, the shell hit-test — runs in FlushPointers,
// once per drained burst of raw input instead of once per mouse report. At
// 1000 Hz the per-report cost exceeded the report interval, so moves queued
// up and the pointer kept travelling for a second or two after the hand
// stopped.
bool  g_dirty[kMaxSeats] = {};
DWORD g_lastFlush = 0;

void LogDragState(int seat, POINT p);   // OP23 diagnostics, defined with HandleButton
bool OwnerDragging();                   // defined with HandleButton

// True when a non-owner seat at p should take the real cursor without a
// click: on the outermost pixel of its monitor (where an auto-hidden taskbar
// waits for the cursor) or over shell UI (taskbar, tray, notifications).
bool WantsShellHandover(POINT p) {
    if (OwnerDragging()) return false;
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT& r = mi.rcMonitor;
        if (p.x <= r.left || p.y <= r.top || p.x >= r.right - 1 || p.y >= r.bottom - 1)
            return true;
    }
    const int64_t t0 = Qpc();
    const bool over = IsShellWindowAt(p);
    g_perf.tShell += Qpc() - t0;
    return over;
}

void FlushSeat(int seat) {
    if (!g_dirty[seat]) return;
    g_dirty[seat] = false;
    Seat& s = g_seats[seat];

    // The owning seat still drives the real cursor — that is what keeps hover
    // states and click targeting following it — but when the cursor is hidden
    // it also draws its own arrow, because there is no longer a visible system
    // pointer to represent it.
    if (seat == g_activeSeat) {
        if (g_verbose && s.lDown) LogDragState(seat, s.pos);
        g_stats[seat].cursorSets++;
        int64_t t0 = Qpc();
        PinCursor(s.pos);
        g_perf.tPin += Qpc() - t0;
        if (g_cursorHidden) {
            g_stats[seat].overlaySets++;
            t0 = Qpc();
            g_overlay[seat].MoveTo(s.pos);
            g_perf.tOverlay += Qpc() - t0;
            UpdateShellCursorFallback(seat);
        }
    } else if (WantsShellHandover(s.pos)) {
        // The shell only reacts to the real cursor: an auto-hidden taskbar
        // rises when IT reaches the screen edge, and toasts and tray flyouts
        // sit above every overlay. A seat that never clicked does not hold
        // that cursor, so hovering the edge or shell UI hands it over — no
        // click needed. Never during the owner's drag: that would yank the
        // cursor out of the drag.
        if (g_verbose) LogLine(L"[owner] seat %d takes the cursor on shell hover", seat);
        g_overlay[seat].MoveTo(s.pos);
        SetActiveSeat(seat);
    } else {
        g_stats[seat].overlaySets++;
        const int64_t t0 = Qpc();
        g_overlay[seat].MoveTo(s.pos);
        g_perf.tOverlay += Qpc() - t0;
    }
}

// The hidden hardware cursor can no longer tell the user that a window border
// or corner is grabbable, so each seat's arrow does it: when the pointer rests
// on the sizing border of a resizable window, the arrow becomes the matching
// system resize arrow. The answer is the window's own WM_NCHITTEST, the same
// question the system asks when a press lands, so what is shown is what a
// press would grab.
PointerShape ResizeShapeAt(POINT p) {
    HWND under = WindowFromPoint(p);
    if (!under) return PointerShape::Arrow;
    DWORD pid = 0;
    GetWindowThreadProcessId(under, &pid);
    if (pid == GetCurrentProcessId()) return PointerShape::Arrow;
    // The sizing border belongs to the top-level window: a child is client area.
    if (GetAncestor(under, GA_ROOT) != under) return PointerShape::Arrow;
    const LONG style = GetWindowLongW(under, GWL_STYLE);
    if (!(style & WS_THICKFRAME) || (style & WS_MAXIMIZE) || (style & WS_MINIMIZE))
        return PointerShape::Arrow;

    DWORD_PTR hit = 0;
    if (!SendMessageTimeoutW(under, WM_NCHITTEST, 0, MAKELPARAM((SHORT)p.x, (SHORT)p.y),
                             SMTO_ABORTIFHUNG, 25, &hit))
        return PointerShape::Arrow;
    switch ((LRESULT)hit) {
    case HTLEFT: case HTRIGHT:           return PointerShape::SizeWE;
    case HTTOP:  case HTBOTTOM:          return PointerShape::SizeNS;
    case HTTOPLEFT: case HTBOTTOMRIGHT:  return PointerShape::SizeNWSE;
    case HTTOPRIGHT: case HTBOTTOMLEFT:  return PointerShape::SizeNESW;
    default:                             return PointerShape::Arrow;
    }
}

// The shape a window picks for its client area (hand on a link, text bar,
// hourglass…) is set by that window on WM_SETCURSOR and lands on the real
// cursor, which is hidden but still pinned under the active seat. Reading it
// back is one GetCursorInfo; system cursors are shared handles, so matching
// against LoadCursor of each standard id is an exact comparison. Only the
// active seat has the real cursor under it: the others show the arrow, or a
// resize arrow on a window border.
PointerShape ClientShapeOfRealCursor(POINT p) {
    struct Std { LPCWSTR id; PointerShape shape; HCURSOR h; };
    static Std known[] = {
        { IDC_HAND, PointerShape::Hand, nullptr },       { IDC_IBEAM, PointerShape::IBeam, nullptr },
        { IDC_NO, PointerShape::No, nullptr },           { IDC_WAIT, PointerShape::Wait, nullptr },
        { IDC_APPSTARTING, PointerShape::AppStarting, nullptr },
        { IDC_CROSS, PointerShape::Cross, nullptr },     { IDC_SIZEALL, PointerShape::SizeAll, nullptr },
        { IDC_SIZEWE, PointerShape::SizeWE, nullptr },   { IDC_SIZENS, PointerShape::SizeNS, nullptr },
        { IDC_SIZENWSE, PointerShape::SizeNWSE, nullptr }, { IDC_SIZENESW, PointerShape::SizeNESW, nullptr },
    };
    static bool loaded = false;
    if (!loaded) { for (auto& k : known) k.h = LoadCursorW(nullptr, k.id); loaded = true; }
    CURSORINFO ci{ sizeof(ci) };
    if (!GetCursorInfo(&ci) || !ci.hCursor) return PointerShape::Arrow;
    // The real cursor must actually sit where the seat points, or its shape
    // belongs to somewhere else.
    if (abs(ci.ptScreenPos.x - p.x) > 1 || abs(ci.ptScreenPos.y - p.y) > 1) return PointerShape::Arrow;
    for (const auto& k : known) if (k.h == ci.hCursor) return k.shape;
    return PointerShape::Arrow;
}

// The border hit-test is re-asked only when the pointer has moved, and at most
// every 30 ms per seat; the timer flush (250 ms) catches the position the
// throttle skipped. The client shape of the active seat is re-read on every
// refresh, moved or not, because a window sets it a moment after the move
// (and changes it on its own, the hourglass of a busy program). While the
// seat holds a button the shape is left alone, so a resize in progress keeps
// its arrow however far the pointer runs ahead of the window edge.
void RefreshSeatShape(int seat) {
    if (!g_cursorHidden) return;           // the real cursor shows its own shape
    static POINT        askedAt[kMaxSeats];
    static bool         asked[kMaxSeats] = {};
    static DWORD        askedTick[kMaxSeats] = {};
    static PointerShape border[kMaxSeats] = {};
    Seat& s = g_seats[seat];
    if (s.lDown || s.rDown || s.mDown) return;
    if (g_overShell && seat == g_activeSeat) return;
    const bool moved = !asked[seat] || askedAt[seat].x != s.pos.x || askedAt[seat].y != s.pos.y;
    const DWORD now = GetTickCount();
    if (moved && (!asked[seat] || now - askedTick[seat] >= 30)) {
        asked[seat] = true; askedAt[seat] = s.pos; askedTick[seat] = now;
        border[seat] = ResizeShapeAt(s.pos);
    } else if (moved) {
        return;                            // throttled: the flush will come back
    }
    PointerShape shape = border[seat];
    if (shape == PointerShape::Arrow && seat == g_activeSeat)
        shape = ClientShapeOfRealCursor(s.pos);
    g_overlay[seat].SetShape(shape);
}

void FlushPointers() {
    for (int i = 0; i < g_cfg.seatCount; ++i) FlushSeat(i);
    for (int i = 0; i < g_cfg.seatCount; ++i) RefreshSeatShape(i);
    g_lastFlush = GetTickCount();
    ++g_perf.flushes;
}

// Flush when the raw-input queue is drained, or at least every 8 ms while a
// continuous stream keeps it full, so the pointer never waits on a burst.
void MaybeFlushPointers() {
    bool any = false;
    for (int i = 0; i < g_cfg.seatCount; ++i) any |= g_dirty[i];
    if (!any) return;
    const bool pending = (HIWORD(GetQueueStatus(QS_RAWINPUT)) & QS_RAWINPUT) != 0;
    if (!pending || (GetTickCount() - g_lastFlush) >= 8) FlushPointers();
}

// Per-seat acceleration state: hand speed and the sub-pixel remainder
// carried from one move to the next, so slow motion is not lost to
// truncation and glides instead of stepping.
//
// Speed is measured on the time each WM_INPUT was POSTED (GetMessageTime),
// not when we get round to processing it: the UI thread can stall for
// 50-190 ms, and timing on processing would read a backlog as a pause and
// then as a burst. Posting time has the tick's ~16 ms granularity, so the
// speed is the distance covered over windows a few ticks wide.
constexpr DWORD kSpeedWindowMs = 48;
constexpr DWORD kAttackWindowMs = 32;   // two ticks: shortest window that is not noise
constexpr DWORD kIdleResetMs   = 100;
struct AccelState {
    struct Step { DWORD t; double d; };
    Step     ring[128]{};
    int      head = 0, count = 0;
    DWORD    lastT = 0;
    double   v = 0, rx = 0, ry = 0;
    // --perf: speed and gain seen since the last report.
    uint64_t n = 0; double vSum = 0, vMax = 0, gSum = 0, gMax = 0;
};
AccelState g_accel[kMaxSeats];

// The rectangle each seat is kept inside: its assigned screen when one is set
// and plugged in, otherwise the whole desktop. Resolved once per settings load
// or display change, never per mouse event.
struct SeatBounds { bool screen = false; RECT rc{}; };
SeatBounds g_bounds[kMaxSeats];

void ResolveSeatBounds() {
    const std::vector<ScreenInfo> screens = ListScreens();
    for (int i = 0; i < kMaxSeats; ++i) {
        g_bounds[i] = SeatBounds{};
        const std::wstring want = i < (int)g_cfg.screen.size() ? g_cfg.screen[i] : L"";
        if (want.empty()) continue;
        for (const ScreenInfo& sc : screens)
            if (_wcsicmp(sc.name.c_str(), want.c_str()) == 0) { g_bounds[i] = { true, sc.rc }; break; }
        LogLine(L"[screen] seat %d kept on %s: %s", i, want.c_str(),
                g_bounds[i].screen ? L"yes" : L"not plugged in, desktop instead");
    }
}

void ClampSeat(int seat, POINT& p) {
    ClampToVirtualScreen(p);
    if (seat >= 0 && seat < kMaxSeats && g_bounds[seat].screen) ClampToRect(p, g_bounds[seat].rc);
}

// Brings every seat back inside its bounds, after they changed.
void ReclampSeats();

void MovePointer(int seat, int dx, int dy) {
    Seat& s = g_seats[seat];
    const double k = (seat < (int)g_cfg.sensitivity.size()) ? g_cfg.sensitivity[seat] : 1.0;
    if (seat >= (int)g_cfg.accel.size() || !g_cfg.accel[seat].on) {
        s.pos.x += (LONG)(dx * k);
        s.pos.y += (LONG)(dy * k);
    } else {
        const Config::Accel& c = g_cfg.accel[seat];
        AccelState& a = g_accel[seat];
        const DWORD now = (DWORD)GetMessageTime();
        // A real pause of the hand (by posting time) starts afresh.
        if (a.count && now - a.lastT > kIdleResetMs) { a.count = 0; a.rx = a.ry = 0; }
        a.lastT = now;
        const int cap = (int)(sizeof(a.ring) / sizeof(a.ring[0]));
        a.ring[a.head] = { now, std::sqrt((double)dx * dx + (double)dy * dy) };
        a.head = (a.head + 1) % cap;
        if (a.count < cap) ++a.count;
        // Fast attack, slow decay: the short window catches a hand speeding
        // up (or leaving a turn) within a couple of ticks, the long one
        // keeps the gain from collapsing on a brief slowdown.
        double dist = 0, distShort = 0;
        for (int i = 0; i < a.count; ++i) {
            const AccelState::Step& st = a.ring[(a.head - 1 - i + cap) % cap];
            if (now - st.t > kSpeedWindowMs) break;
            dist += st.d;
            if (now - st.t <= kAttackWindowMs) distShort += st.d;
        }
        const double vLong = dist / (double)kSpeedWindowMs;          // counts per ms
        const double vShort = distShort / (double)kAttackWindowMs;
        a.v = vShort > vLong ? vShort : vLong;
        const double v2 = a.v * a.v, s2 = c.speed * c.speed;
        const double g = c.slow + (c.fast - c.slow) * (s2 > 0 ? v2 / (v2 + s2) : 1.0);
        ++a.n; a.vSum += a.v; a.gSum += g;
        if (a.v > a.vMax) a.vMax = a.v;
        if (g > a.gMax) a.gMax = g;
        a.rx += dx * g;
        a.ry += dy * g;
        const LONG mx = (LONG)a.rx, my = (LONG)a.ry;
        a.rx -= mx; a.ry -= my;
        s.pos.x += mx;
        s.pos.y += my;
    }
    ClampSeat(seat, s.pos);
    g_dirty[seat] = true;
}

void RememberClickTarget(int seat) {
    HWND w = WindowFromPoint(g_seats[seat].pos);
    if (w) g_seats[seat].focusTarget = w;
}

// True while the seat that owns the click stream holds any button down —
// i.e. a drag is in progress. Self-heals from the async key-state table,
// which under NOLEGACY reflects exactly what we injected: if a release was
// lost (device unplugged mid-drag, event dropped), the stale flag would
// otherwise lock every other seat out of clicking forever.
bool OwnerDragging() {
    Seat& o = g_seats[g_activeSeat];
    if (o.lDown  && !(GetAsyncKeyState(VK_LBUTTON)  & 0x8000)) o.lDown  = false;
    if (o.rDown  && !(GetAsyncKeyState(VK_RBUTTON)  & 0x8000)) o.rDown  = false;
    if (o.mDown  && !(GetAsyncKeyState(VK_MBUTTON)  & 0x8000)) o.mDown  = false;
    if (o.x1Down && !(GetAsyncKeyState(VK_XBUTTON1) & 0x8000)) o.x1Down = false;
    if (o.x2Down && !(GetAsyncKeyState(VK_XBUTTON2) & 0x8000)) o.x2Down = false;
    return o.lDown || o.rDown || o.mDown || o.x1Down || o.x2Down;
}

// True when the press targets a menu or a popup of the foreground window.
// Menus (class #32768, Chrome's own popups, tray menus) are never the
// foreground window: their owner is. Taking the foreground for them dismisses
// the menu before the click lands, so the entry never fires — and the click,
// arriving after the menu closed, falls through to the window underneath.
bool IsMenuTarget(HWND under, HWND root, const wchar_t** why) {
    GUITHREADINFO gti{ sizeof(gti) };
    if (GetGUIThreadInfo(0, &gti) && (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE))) {
        *why = L"menu mode";
        return true;
    }
    if (!wcscmp(ClassOf(root), L"#32768")) { *why = L"menu class"; return true; }
    if (GetWindowLongW(root, GWL_EXSTYLE) & WS_EX_NOACTIVATE) { *why = L"no-activate"; return true; }
    HWND fg = GetForegroundWindow();
    if (fg && GetAncestor(under, GA_ROOTOWNER) == fg) { *why = L"popup of foreground"; return true; }
    return false;
}

// OP23 diagnostics (--verbose only): what the system makes of a left press,
// and whether its move/size loop is running while the seat drags.
const wchar_t* HitName(LRESULT h) {
    switch (h) {
    case HTCLIENT: return L"CLIENT";   case HTCAPTION: return L"CAPTION";
    case HTLEFT: return L"LEFT";       case HTRIGHT: return L"RIGHT";
    case HTTOP: return L"TOP";         case HTBOTTOM: return L"BOTTOM";
    case HTTOPLEFT: return L"TOPLEFT"; case HTTOPRIGHT: return L"TOPRIGHT";
    case HTBOTTOMLEFT: return L"BOTTOMLEFT"; case HTBOTTOMRIGHT: return L"BOTTOMRIGHT";
    case HTBORDER: return L"BORDER";   case HTNOWHERE: return L"NOWHERE";
    case HTTRANSPARENT: return L"TRANSPARENT"; case HTSYSMENU: return L"SYSMENU";
    default: return L"other";
    }
}

void LogPressHit(int seat, POINT p, HWND under) {
    DWORD_PTR hit = 0;
    const LRESULT ok = SendMessageTimeoutW(under, WM_NCHITTEST, 0, MAKELPARAM((SHORT)p.x, (SHORT)p.y),
                                           SMTO_ABORTIFHUNG, 200, &hit);
    HWND root = GetAncestor(under, GA_ROOT);
    RECT wr{}; if (root) GetWindowRect(root, &wr);
    POINT cur{}; GetCursorPos(&cur);
    LogLine(L"[hit] seat %d press %ld,%ld real cursor %ld,%ld under %p [%s] hit=%s(%ld)%s  root %p [%s] rect %ld,%ld-%ld,%ld style %08lX",
            seat, p.x, p.y, cur.x, cur.y, under, ClassOf(under), HitName((LRESULT)hit), (long)hit,
            ok ? L"" : L" (no answer)", root, ClassOf(root), wr.left, wr.top, wr.right, wr.bottom,
            root ? (unsigned long)GetWindowLongW(root, GWL_STYLE) : 0UL);
}

// Sampled while the owner holds the left button: the move/size loop shows as
// GUI_INMOVESIZE with the target window holding the capture.
void LogDragState(int seat, POINT p) {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 100) return;
    last = now;
    GUITHREADINFO gti{ sizeof(gti) };
    const BOOL ok = GetGUIThreadInfo(0, &gti);
    POINT cur{}; GetCursorPos(&cur);
    LogLine(L"[drag] seat %d pos %ld,%ld real %ld,%ld gti=%d flags=%lX capture=%p [%s] active=%p",
            seat, p.x, p.y, cur.x, cur.y, (int)ok, ok ? gti.flags : 0UL,
            ok ? gti.hwndCapture : nullptr, ok ? ClassOf(gti.hwndCapture) : L"", ok ? gti.hwndActive : nullptr);
}

// Waiting inside a press used to be a plain Sleep on this, the input thread:
// every pointer, the keyboard steering and the other seat froze for up to a
// second. Now the wait keeps draining WM_INPUT, so movement flows on. Button
// events that arrive meanwhile (any seat) are parked in arrival order and
// replayed once the press that is waiting has been injected — that keeps the
// press/release order the inline sleep guaranteed. Only WM_INPUT is pumped:
// other messages wait, as they did behind the Sleep.
struct DeferredButton { int seat; USHORT flags; USHORT data; POINT pos; };
static std::vector<DeferredButton> g_deferredButtons;
static int g_waitDepth = 0;

void OnRawInput(HRAWINPUT h);

// Waits up to `ms`, returning early once `done()` holds (checked every 10 ms
// or so, and after each input). Returns the time actually waited.
template <class Done>
DWORD WaitPumpingInput(DWORD ms, Done done) {
    const DWORD start = GetTickCount();
    ++g_waitDepth;
    for (;;) {
        if (done()) break;
        const DWORD spent = GetTickCount() - start;
        if (spent >= ms) break;
        MSG msg;
        if (PeekMessageW(&msg, nullptr, WM_INPUT, WM_INPUT, PM_REMOVE)) {
            g_heartbeat.fetch_add(1);
            OnRawInput(reinterpret_cast<HRAWINPUT>(msg.lParam));
            MaybeFlushPointers();
            continue;
        }
        const DWORD left = ms - spent;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, left < 10 ? left : 10, QS_RAWINPUT);
    }
    --g_waitDepth;
    return GetTickCount() - start;
}

void HandleButton(int seat, USHORT flags, USHORT data);

// Entry point for every relayed button event: parked while a press waits,
// handled now otherwise, then whatever got parked meanwhile is replayed.
void RelayButton(int seat, USHORT flags, USHORT data) {
    if (g_waitDepth > 0) {
        g_deferredButtons.push_back({ seat, flags, data, g_seats[seat].pos });
        return;
    }
    HandleButton(seat, flags, data);
    while (!g_deferredButtons.empty()) {
        const DeferredButton d = g_deferredButtons.front();
        g_deferredButtons.erase(g_deferredButtons.begin());
        // Act where the pointer was when the button moved, not where the
        // hand has carried it since.
        const POINT now = g_seats[d.seat].pos;
        g_seats[d.seat].pos = d.pos;
        HandleButton(d.seat, d.flags, d.data);
        g_seats[d.seat].pos = now;
    }
}

void HandleButton(int seat, USHORT flags, USHORT data) {
    // Physical arrival time, captured before any settle sleeps distort it —
    // the anti-compression pacing below compares against this.
    const DWORD pressArrive = GetTickCount();

    // Any button press takes ownership of the real cursor, so the click lands
    // where this seat's pointer is and hover states follow the clicker.
    const bool isPress =
        (flags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
                  RI_MOUSE_MIDDLE_BUTTON_DOWN |
                  RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN)) != 0;

    // Drag guard. There is one hardware cursor and one logical button state;
    // injecting another seat's buttons or wheel into the middle of a drag
    // teleports the drag point, breaks the grab, and drops the payload
    // somewhere neither person intended. So while the owner holds a button,
    // every other seat's buttons and wheel are swallowed whole. Movement is
    // untouched — the other pointers keep gliding on their overlays. The
    // swallowed click simply never happened: click again once the drag ends.
    if (seat != g_activeSeat && OwnerDragging()) {
        if (isPress)
            LogLine(L"[drag-guard] seat %d press swallowed — seat %d is mid-drag",
                    seat, g_activeSeat);
        return;
    }

    if (isPress && seat != g_activeSeat) SetActiveSeat(seat);

    const POINT p = g_seats[seat].pos;

    // Applications route a click using the pointer position they last saw, not
    // the coordinates carried on the button event — Chromium and Electron in
    // particular. Two things conspire here: the hardware cursor drifts
    // invisibly under us, and a button injected with no preceding move leaves
    // the target still believing the pointer is somewhere else. The result was
    // that focusing a text box took two clicks, the first only serving to tell
    // the app where the pointer had got to.
    //
    // So before any press: put the real cursor where this seat is pointing and
    // deliver an explicit move, which queues ahead of the button and makes the
    // target agree with us before the click lands.
    if (isPress) {
        // Cross-window clicks need help. A click-through injection — letting
        // the click itself activate the window like real hardware — works when
        // the foreground is an app that yields (verified against Notepad,
        // 8/8), but an Electron foreground window does not let go: the click
        // dies mid-activation with focus never moving (verified against a
        // live Claude Code window, 0/3). So when the press targets a window
        // that is not foreground:
        //
        //   1. take the foreground explicitly (attach-dance, synchronous),
        //   2. poll until the switch is actually observable,
        //   3. wait out Chromium's post-activation mouse-ignore window —
        //      150 ms is measurably flaky, 250 ms passed 10 of 11 trials,
        //
        // and only then inject. The residual failure degrades to "click
        // again", never to a double action. The inline wait also keeps the
        // press/release order intact: the physical release queues behind us
        // and is injected after the DOWN no matter how long we sleep here.
        const int64_t tSettle = Qpc();
        HWND under = WindowFromPoint(p);
        if (under) {
            DWORD pidUnder = 0;
            GetWindowThreadProcessId(under, &pidUnder);
            if (pidUnder != GetCurrentProcessId()) {
                g_seats[seat].focusTarget = under;

                HWND root = GetAncestor(under, GA_ROOT);
                const wchar_t* menuWhy = nullptr;
                if (root && root != GetForegroundWindow() && IsMenuTarget(under, root, &menuWhy)) {
                    if (g_verbose)
                        LogLine(L"[click] seat %d press on %p [%s] pid %lu (%s) — foreground left alone",
                                seat, root, ClassOf(root), pidUnder, menuWhy);
                } else if (root && root != GetForegroundWindow()) {
                    ClaimForeground(under);
                    const DWORD fgMs = WaitPumpingInput(200, [root] { return GetForegroundWindow() == root; });
                    if (g_verbose)
                        LogLine(L"[click] seat %d cross-window -> %p [%s]  fg settled in %lu ms",
                                seat, root, ClassOf(root), fgMs);

                    // The settle waits inline, deliberately. A deferred press
                    // flushed from WM_TIMER was tried and reverted: timers
                    // only fire on an empty queue, and raw input keeps the
                    // queue full whenever any mouse moves, so deferred clicks
                    // starved. The wait pumps raw input instead of sleeping,
                    // so pointers keep moving. Known cost: a drag BEGUN
                    // cross-window during these 250 ms is time-compressed and
                    // will not register — start cross-window drags with a
                    // separate click first, or accept one dragger at a time.
                    WaitPumpingInput(250, [] { return false; });
                }
            }
        }
        g_perf.tSettle += Qpc() - tSettle;

        PinCursor(p);
        SendMouseAt(p, MOUSEEVENTF_MOVE);
        if (g_verbose && (flags & RI_MOUSE_LEFT_BUTTON_DOWN)) {
            HWND hit = WindowFromPoint(p);
            if (hit) LogPressHit(seat, p, hit);
        }
    }

    // Releases inject only if we injected the matching press. A press
    // swallowed by the drag guard leaves the seat's flag clear, so its
    // release is swallowed with it instead of firing a stray UP.
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN)   {
        // Anti-compression: the cross-window settle delays injection, so two
        // presses the hand made comfortably apart can inject close enough to
        // read as a double- or triple-click. If the PHYSICAL gap since the
        // previous press was outside the double-click window but the INJECTED
        // gap would be inside it, pad the difference so apps see the rhythm
        // the hand actually produced. Genuine double-clicks (physical gap
        // inside the window) are untouched.
        Seat& s = g_seats[seat];
        const DWORD dct = GetDoubleClickTime();
        DWORD now = GetTickCount();
        if (s.lastPressInject != 0) {
            const DWORD physGap = pressArrive - s.lastPressArrive;
            const DWORD injGap  = now - s.lastPressInject;
            if (physGap > dct && injGap < dct) {
                const DWORD pad = (dct - injGap) < 300 ? (dct - injGap) : 300;
                if (g_verbose)
                    LogLine(L"[pace] seat %d padding %lu ms (physGap=%lu injGap=%lu)",
                            seat, pad, physGap, injGap);
                WaitPumpingInput(pad, [] { return false; });
                now = GetTickCount();
            }
        }
        s.lastPressArrive = pressArrive;
        s.lastPressInject = now;
        if (g_verbose) LogLine(L"[inj] seat %d Ldown %ld,%ld t=%lu", seat, p.x, p.y, now);
        SendMouseAt(p, MOUSEEVENTF_LEFTDOWN);
        s.lDown = true;
        RememberClickTarget(seat);
    }
    if (flags & RI_MOUSE_LEFT_BUTTON_UP)     { if (g_seats[seat].lDown) {
                                                   if (g_verbose) LogLine(L"[inj] seat %d Lup   %ld,%ld t=%lu", seat, p.x, p.y, GetTickCount());
                                                   SendMouseAt(p, MOUSEEVENTF_LEFTUP); g_seats[seat].lDown = false; } }
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN)  { SendMouseAt(p, MOUSEEVENTF_RIGHTDOWN);  g_seats[seat].rDown = true;  RememberClickTarget(seat); }
    if (flags & RI_MOUSE_RIGHT_BUTTON_UP)    { if (g_seats[seat].rDown) {
                                                   SendMouseAt(p, MOUSEEVENTF_RIGHTUP); g_seats[seat].rDown = false; } }
    if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) { SendMouseAt(p, MOUSEEVENTF_MIDDLEDOWN); g_seats[seat].mDown = true; }
    if (flags & RI_MOUSE_MIDDLE_BUTTON_UP)   { if (g_seats[seat].mDown) {
                                                   SendMouseAt(p, MOUSEEVENTF_MIDDLEUP); g_seats[seat].mDown = false; } }
    // Side buttons — browser back/forward. Dropped entirely before this.
    if (flags & RI_MOUSE_BUTTON_4_DOWN)      { SendMouseAt(p, MOUSEEVENTF_XDOWN, XBUTTON1); g_seats[seat].x1Down = true; }
    if (flags & RI_MOUSE_BUTTON_4_UP)        { if (g_seats[seat].x1Down) {
                                                   SendMouseAt(p, MOUSEEVENTF_XUP, XBUTTON1); g_seats[seat].x1Down = false; } }
    if (flags & RI_MOUSE_BUTTON_5_DOWN)      { SendMouseAt(p, MOUSEEVENTF_XDOWN, XBUTTON2); g_seats[seat].x2Down = true; }
    if (flags & RI_MOUSE_BUTTON_5_UP)        { if (g_seats[seat].x2Down) {
                                                   SendMouseAt(p, MOUSEEVENTF_XUP, XBUTTON2); g_seats[seat].x2Down = false; } }
    // Wheel routing follows the REAL cursor, not the coordinates on the event:
    // with hover-scroll routing (MouseWheelRouting=2, this machine's setting,
    // and the Windows 11 default) the system scrolls whatever window lies
    // under the hardware cursor. That cursor is invisible and parked at the
    // OWNER seat's position — so a non-owner's scroll went to the owner's
    // window. Park it on the scrolling seat first; the owner's next move puts
    // it back. No ownership transfer: scrolling is a hover action, not a
    // focus action, same as Windows treats it.
    if (flags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL))
        PinCursor(p);
    if (flags & RI_MOUSE_WHEEL)              { SendMouseAt(p, MOUSEEVENTF_WHEEL, (DWORD)(SHORT)data); }
    if (flags & RI_MOUSE_HWHEEL)             { SendMouseAt(p, MOUSEEVENTF_HWHEEL, (DWORD)(SHORT)data); }
}

// ---------------------------------------------------------------- learn / play

constexpr USHORT kStdDownBits = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
                                RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN |
                                RI_MOUSE_BUTTON_5_DOWN;
constexpr USHORT kWheelBits   = RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL;
// Everything the raw mouse flags define; a bit outside it is a button Windows
// has no name for.
constexpr USHORT kKnownBits   = 0x03FF | kWheelBits;

// A shortcut as one SendInput call: the modifiers down, the key, then all up.
void SendHotkey(UINT mods, UINT vk) {
    if (!vk || !InjectAllowed()) return;
    INPUT in[12]{};
    int n = 0;
    auto add = [&](UINT v, bool up) {
        in[n].type           = INPUT_KEYBOARD;
        in[n].ki.wVk         = (WORD)v;
        in[n].ki.wScan       = (WORD)MapVirtualKeyW(v, MAPVK_VK_TO_VSC);
        in[n].ki.dwFlags     = (up ? KEYEVENTF_KEYUP : 0) | (IsExtendedVk(v) ? KEYEVENTF_EXTENDEDKEY : 0);
        in[n].ki.dwExtraInfo = kInjectTag;
        ++n;
    };
    const UINT modVk[4] = { VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN };
    const UINT bits[4]  = { kModCtrl, kModAlt, kModShift, kModWin };
    for (int i = 0; i < 4; ++i) if (mods & bits[i]) add(modVk[i], false);
    add(vk, false);
    add(vk, true);
    for (int i = 3; i >= 0; --i) if (mods & bits[i]) add(modVk[i], true);
    SendInput((UINT)n, in, sizeof(INPUT));
}

USHORT ActionFlags(int action, bool down) {
    switch (action) {
    case kActLeft:    return down ? RI_MOUSE_LEFT_BUTTON_DOWN   : RI_MOUSE_LEFT_BUTTON_UP;
    case kActRight:   return down ? RI_MOUSE_RIGHT_BUTTON_DOWN  : RI_MOUSE_RIGHT_BUTTON_UP;
    case kActMiddle:  return down ? RI_MOUSE_MIDDLE_BUTTON_DOWN : RI_MOUSE_MIDDLE_BUTTON_UP;
    case kActBack:    return down ? RI_MOUSE_BUTTON_4_DOWN      : RI_MOUSE_BUTTON_4_UP;
    case kActForward: return down ? RI_MOUSE_BUTTON_5_DOWN      : RI_MOUSE_BUTTON_5_UP;
    }
    return 0;
}

// Plays the action of learned button idx of a seat, as a press or a release.
// Presses and releases come in pairs: a release without its press (a reload
// between the two, a repeat) is dropped, so no button is left held down.
// Clicks go through HandleButton, as if the seat's own mouse had made them: at
// the seat's pointer, with the ownership and drag rules that come with it.
void RunMapped(int seat, int idx, bool down) {
    if (seat < 0 || seat >= kMaxSeats || idx < 0 || idx >= kMaxLearned) return;
    if (idx >= (int)g_cfg.buttons[seat].size()) return;
    bool& held = g_held[seat][idx];
    if (down == held) return;
    held = down;

    const ButtonMap& b = g_cfg.buttons[seat][idx];
    if (b.action == kActKeys) {
        if (down) SendHotkey(b.mods, b.vk);
        return;
    }
    const USHORT f = ActionFlags(b.action, down);
    if (!f) return;
    if (g_verbose) LogLine(L"[button] seat %d %s %s", seat, b.sig.c_str(), down ? L"down" : L"up");
    FlushPointers();
    RelayButton(seat, f, 0);
}

void ReleaseHeldButtons() {
    for (int s = 0; s < kMaxSeats; ++s)
        for (int i = 0; i < kMaxLearned; ++i)
            if (g_held[s][i]) RunMapped(s, i, false);
}

void RefreshMapFlags() {
    g_haveKeyMaps = false;
    for (int s = 0; s < kMaxSeats; ++s)
        for (const ButtonMap& b : g_cfg.buttons[s])
            if (b.kind == L'k' || b.kind == L'h') g_haveKeyMaps = true;
}

// ---- listening

void NotifyLearn(int seat, LPARAM result) {
    if (HWND w = FindWindowW(kSettingsClassName, nullptr))
        PostMessageW(w, kLearnedMsg, (WPARAM)seat, result);
}

void StopLearn() {
    g_learnSeat = -1;
    KillTimer(g_hwnd, kLearnTimer);
}

void StartLearn(int seat) {
    StopLearn();
    if (seat < 0 || seat >= g_cfg.seatCount || seat >= kMaxSeats) return;
    if (!g_seatPresent[seat]) {
        LogLine(L"[learn] seat %d : mouse not plugged in", seat);
        NotifyLearn(seat, kLearnAbsent);
        return;
    }
    g_learnSeat  = seat;
    g_learnArmAt = GetTickCount() + 500;   // the click on "learn" is still being released
    SetTimer(g_hwnd, kLearnTimer, kLearnListenMs, nullptr);
    LogLine(L"[learn] seat %d : listening for %lu ms", seat, kLearnListenMs);
}

void FinishLearn(int seat, const std::wstring& sig, const std::wstring& label) {
    StopLearn();
    if (!WriteLearnResult(seat, sig, label)) {
        LogLine(L"[learn] seat %d : could not write the result", seat);
        NotifyLearn(seat, kLearnNothing);
        return;
    }
    LogLine(L"[learn] seat %d heard %s", seat, sig.c_str());
    NotifyLearn(seat, kLearnHeard);
}

bool LearnArmed() { return (int)(GetTickCount() - g_learnArmAt) >= 0; }

std::wstring HexString(const BYTE* p, size_t n) {
    std::wstring s;
    wchar_t two[4];
    for (size_t i = 0; i < n; ++i) { swprintf(two, 4, L"%02X", p[i]); s += two; }
    return s;
}

std::wstring MouseBitLabel(USHORT bit) {
    const bool fr = UiFrench();
    switch (bit) {
    case RI_MOUSE_LEFT_BUTTON_DOWN:   return fr ? L"Bouton gauche"        : L"Left button";
    case RI_MOUSE_RIGHT_BUTTON_DOWN:  return fr ? L"Bouton droit"         : L"Right button";
    case RI_MOUSE_MIDDLE_BUTTON_DOWN: return fr ? L"Bouton du milieu"     : L"Middle button";
    case RI_MOUSE_BUTTON_4_DOWN:      return fr ? L"Bouton précédent (4)" : L"Back button (4)";
    case RI_MOUSE_BUTTON_5_DOWN:      return fr ? L"Bouton suivant (5)"   : L"Forward button (5)";
    }
    wchar_t buf[64];
    swprintf(buf, 64, fr ? L"Bouton spécial (0x%X)" : L"Special button (0x%X)", bit);
    return buf;
}

// Keys that do not type. A keyboard interface of a mouse may be shared with a
// real keyboard (one radio receiver for both): learning only these keeps what
// is typed out of anything this program retains.
bool LearnableKey(USHORT scan, bool e0) {
    if (scan == 0 || scan >= 0xFF) return false;
    if (e0) return scan != 0x1C && scan != 0x35 && scan != 0x2A && scan != 0x36;   // pad Enter, pad /, fake shifts
    if (scan == 0x01 || scan == 0x1D || scan == 0x2A || scan == 0x36 || scan == 0x38) return true;   // Esc, Ctrl, Shift, Alt
    if (scan >= 0x3B && scan <= 0x44) return true;   // F1-F10
    if (scan == 0x57 || scan == 0x58) return true;   // F11, F12
    if (scan >= 0x64 && scan <= 0x6E) return true;   // F13-F23
    return scan == 0x76;                             // F24
}

// A press of the mouse being listened to. Returns the flags without the
// presses: while listening they act on nothing, they are only heard.
USHORT LearnFromMouse(int seat, USHORT flags) {
    const USHORT presses = flags & (kStdDownBits | (USHORT)~kKnownBits);
    if (!presses) return flags;
    if (LearnArmed()) {
        const USHORT bit = presses & (USHORT)(0 - presses);   // the lowest
        wchar_t sig[16];
        swprintf(sig, 16, L"m:%X", bit);
        FinishLearn(seat, sig, MouseBitLabel(bit));
    }
    return flags & (USHORT)~presses;
}

// A key of a mouse's own keyboard interface.
void OnMouseKeyboardInterface(HANDLE dev, USHORT scan, bool e0, bool up) {
    if (g_learnSeat < 0 && !g_haveKeyMaps) return;
    const auto owner = g_ownerSeat.find(dev);
    if (owner == g_ownerSeat.end()) return;       // not a mouse's own interface: never looked at
    const int seat = owner->second;
    const UINT code = scan | (e0 ? 0x100u : 0u);

    if (g_learnSeat == seat) {
        if (up || !LearnArmed() || !LearnableKey(scan, e0)) return;
        wchar_t sig[16], name[64] = L"";
        swprintf(sig, 16, L"k:%X", code);
        GetKeyNameTextW((LONG)(((UINT)scan << 16) | (e0 ? (1u << 24) : 0u)), name, 64);
        wchar_t label[128];
        swprintf(label, 128, UiFrench() ? L"Touche « %s » de la souris" : L"Mouse key \"%s\"", name);
        FinishLearn(seat, sig, label);
        return;
    }
    if (g_learnSeat >= 0) return;
    for (int i = 0; i < (int)g_cfg.buttons[seat].size(); ++i) {
        const ButtonMap& b = g_cfg.buttons[seat][i];
        if (b.kind == L'k' && b.code == code) RunMapped(seat, i, !up);
    }
}

bool AllZero(const BYTE* p, size_t n) {
    for (size_t i = 0; i < n; ++i) if (p[i]) return false;
    return true;
}

// Whether a report says "something is pressed". The first byte of a report is
// its id when the collection has several (always so for the media and
// manufacturer collections of a mouse, which share one interface), so it says
// nothing: a report of an id and zeros is a release. Only a one-byte report is
// all data.
bool IsPressReport(const BYTE* p, size_t n) {
    return n == 1 ? p[0] != 0 : !AllZero(p + 1, n - 1);
}

// One report of a mouse's media, system or manufacturer collection.
void OnMouseHidReport(HANDLE dev, int seat, const BYTE* p, size_t n) {
    const auto pg = g_hidPage.find(dev);
    if (pg == g_hidPage.end() || n == 0 || n > 64) return;
    const USHORT page = pg->second;

    if (g_learnSeat == seat) {
        if (!LearnArmed() || !IsPressReport(p, n)) return;
        wchar_t head[16];
        swprintf(head, 16, L"h%X:", page);
        const std::wstring hex = HexString(p, n);
        const bool fr = UiFrench();
        const wchar_t* what = page == 0x0C   ? (fr ? L"Touche multimédia" : L"Media key")
                            : page == 0xFF00 ? (fr ? L"Bouton du fabricant" : L"Manufacturer button")
                                             : (fr ? L"Commande système" : L"System control");
        FinishLearn(seat, std::wstring(head) + hex, std::wstring(what) + L" " + hex.substr(0, 16));
        return;
    }
    if (g_learnSeat >= 0) return;
    for (int i = 0; i < (int)g_cfg.buttons[seat].size(); ++i) {
        const ButtonMap& b = g_cfg.buttons[seat][i];
        if (b.kind != L'h' || b.page != page || b.report.size() != n) continue;
        if (!memcmp(b.report.data(), p, n))
            RunMapped(seat, i, true);
        // Released: the same kind of report with nothing pressed. Its first byte
        // is the report id (kept) or the only data byte (then zero).
        else if (g_held[seat][i] && (p[0] == 0 || p[0] == b.report[0]) && AllZero(p + 1, n - 1))
            RunMapped(seat, i, false);
    }
}

void OnRawHid(HANDLE dev, const RAWHID& h, size_t avail) {
    if (g_learnSeat < 0 && !g_haveKeyMaps) return;
    const auto owner = g_ownerSeat.find(dev);
    if (owner == g_ownerSeat.end() || h.dwSizeHid == 0) return;
    // The reports must lie inside what Windows handed over: the counts come
    // from the device's side of the driver.
    if ((uint64_t)h.dwSizeHid * h.dwCount > avail) return;
    const BYTE* p = h.bRawData;
    for (DWORD c = 0; c < h.dwCount; ++c, p += h.dwSizeHid)
        OnMouseHidReport(dev, owner->second, p, h.dwSizeHid);
}

// Replaces the relay of the mouse buttons that have been learned: the action
// chosen for them is played instead. Returns the flags left for the usual relay.
USHORT RemapMouseButtons(int seat, USHORT flags) {
    const auto& list = g_cfg.buttons[seat];
    for (int i = 0; i < (int)list.size(); ++i) {
        const ButtonMap& b = list[i];
        if (b.kind != L'm') continue;
        const USHORT down = (USHORT)b.code;
        if (flags & down) {
            RunMapped(seat, i, true);
            if (!(down & kStdDownBits)) RunMapped(seat, i, false);   // no release exists: a click
            flags &= (USHORT)~down;
        }
        const USHORT up = (down & kStdDownBits) ? (USHORT)(down << 1) : 0;
        // A release whose press was not ours (made before the button was
        // learned) goes on to the usual relay, which knows how to end it.
        if (up && (flags & up) && g_held[seat][i]) {
            RunMapped(seat, i, false);
            flags &= (USHORT)~up;
        }
    }
    return flags;
}

// ---------------------------------------------------------------- raw input

void OnRawMouse(HANDLE dev, const RAWMOUSE& m) {
    // Our own injected input returns through this very path, with a NULL
    // device handle and our signature in ulExtraInformation. Verified by
    // --probe-echo on this machine. Without this guard the first click is an
    // infinite loop — inject, receive, route to a seat, inject again — and the
    // watchdog cannot break it because the storm keeps the heartbeat climbing.
    // Dropping every NULL-handle event also ignores injection from other
    // processes, which is correct: those already reach the OS on their own.
    if (dev == nullptr)                       { ++g_dropNullDev;  return; }
    if (m.ulExtraInformation == kInjectTag)   { ++g_dropInjected; return; }
    if (g_deadMice.count(dev))                { return; }   // ghost interface

    auto it = g_mouseSeat.find(dev);
    // A device not yet bound (plugged in within the rescan window) must not
    // drive seat 0 once measured bindings exist; it is ignored until bound.
    if (it == g_mouseSeat.end() && !g_cfg.mouseBinding.empty()) {
        ++g_unboundEvents;
        if (g_verbose) LogLine(L"[ignored] unbound mouse %p, event dropped", dev);
        return;
    }
    // With no measured binding at all, unbound devices drive seat 0 so a
    // first run never leaves the user with dead hardware.
    const int seat = (it != g_mouseSeat.end()) ? it->second : 0;
    if (seat < 0 || seat >= g_cfg.seatCount) return;

    g_perDevice[dev]++;
    if (it == g_mouseSeat.end()) ++g_unboundEvents;
    if (m.usButtonFlags) g_stats[seat].buttons++; else g_stats[seat].moves++;

    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
        const RECT v = VirtualScreenRect();
        POINT p;
        p.x = v.left + (LONG)(((int64_t)m.lLastX * (v.right  - v.left)) / 65535);
        p.y = v.top  + (LONG)(((int64_t)m.lLastY * (v.bottom - v.top))  / 65535);
        g_seats[seat].pos = p;
        ClampSeat(seat, g_seats[seat].pos);
        g_dirty[seat] = true;
    } else if (m.lLastX || m.lLastY) {
        MovePointer(seat, m.lLastX, m.lLastY);
    }

    if (m.usButtonFlags) {
        // One line per PHYSICAL button event, before any routing. If a single
        // physical press ever shows up here twice from two device handles,
        // that is a duplicate HID collection double-driving a seat.
        if (g_verbose)
            LogLine(L"[phys] dev=%p seat=%d flags=%04X pos=%ld,%ld t=%lu",
                    dev, seat, m.usButtonFlags, g_seats[seat].pos.x, g_seats[seat].pos.y,
                    GetTickCount());
        // Buttons act on positions: bring every pointer up to date first.
        FlushPointers();

        // Learned buttons: listened to while the settings window asks for it,
        // replaced by their chosen action otherwise. What is left goes on to
        // the usual relay.
        USHORT flags = m.usButtonFlags;
        if (g_learnSeat == seat) flags = LearnFromMouse(seat, flags);
        flags = RemapMouseButtons(seat, flags);
        if (flags) RelayButton(seat, flags, m.usButtonData);
    }

    // RIDEV_NOLEGACY does NOT stop Windows moving the single hardware cursor
    // in response to physical mouse motion. It suppresses the legacy *message*
    // stream only. Measured on this machine: with a non-active seat's mouse
    // moving and SetCursorPos never once called, the real cursor still tracked
    // it across 300+ pixels.
    //
    // With move-eating on, physical motion never reaches the cursor at all
    // and there is nothing to correct. Otherwise a non-owning seat's motion
    // has already dragged the cursor off target: snapping it back works but
    // races the OS at polling rates and shows as harsh jitter, so it is a
    // last resort — only when the cursor is visible and move-eating is off.
    if (seat != g_activeSeat && !g_cursorHidden && !g_eatMoves) {
        PinCursor(g_seats[g_activeSeat].pos);
        g_stats[g_activeSeat].cursorSets++;
    }
}

void RequestExit();

void OnRawKeyboard(HANDLE dev, const RAWKEYBOARD& k) {
    // Same self-injection guard as the mouse path. Dead keyboard interfaces
    // are NOT dropped here: with routing off Windows delivers their keys
    // itself anyway, and the panic combo must work from every keyboard.
    if (dev == nullptr || k.ExtraInformation == kInjectTag) return;

    const USHORT scan = k.MakeCode;
    const bool   up   = (k.Flags & RI_KEY_BREAK) != 0;
    if (scan >= 512) return;

    // A key of a mouse's own keyboard interface may be a learned button. A real
    // keyboard is not in the table that call looks in: it returns at once.
    OnMouseKeyboardInterface(dev, scan, (k.Flags & RI_KEY_E0) != 0, up);

    auto it = g_keyboardSeat.find(dev);
    int seat = (it != g_keyboardSeat.end()) ? it->second : 0;
    if (seat < 0 || seat >= g_cfg.seatCount) seat = 0;

    // Held-key state is per seat. Tracking it globally lets one person's Shift
    // or Ctrl apply to the other person's typing once two keyboards are live.
    g_keyDown[seat][scan] = !up;

    // Panic combo: Ctrl+Alt+Shift+Q releases every captured device. Checked
    // before any routing so it works even if routing is what went wrong.
    if (!up && scan == SC_Q &&
        g_keyDown[seat][SC_LCTRL] && g_keyDown[seat][SC_LALT] && g_keyDown[seat][SC_LSHIFT]) {
        LogLine(L"[panic] Ctrl+Alt+Shift+Q — releasing input capture");
        RequestExit();
        return;
    }

    if (!g_keyboardRouting) return;              // steering disabled
    if (g_deadKeyboards.count(dev)) return;      // ghost interface

    // STEERING ONLY — the physical keystroke is never suppressed and never
    // re-injected. Windows delivers it normally; all we do is make sure the
    // right window is in front first.
    //
    // The eat-and-re-inject design that per-seat layouts would need is
    // impossible here: eating a key in a low-level keyboard hook also kills
    // its raw input, which is the only source of device identity. So instead
    // of owning the keystroke we merely aim it.
    //
    // Consequence to expect: the very first keystroke after a seat's window
    // loses focus may still land in the previous window, because it can
    // already be queued there by the time we claim. Subsequent keys land
    // correctly. This is a real limitation, but a benign one — worst case a
    // stray character, never a lost keyboard. NO sleeps here: this runs on
    // the input thread, and stalling it delays the other seat's mouse too.
    if (!up) {
        HWND target = g_seats[seat].focusTarget;
        if (target && IsWindow(target)) {
            HWND root = GetAncestor(target, GA_ROOT);
            if (root && root != GetForegroundWindow()) {
                ClaimForeground(target);
                if (g_verbose)
                    LogLine(L"[key] seat %d steered typing to %p [%s]", seat, root, ClassOf(root));
            }
        }
    }
}

void OnRawInput(HRAWINPUT h) {
    UINT size = 0;
    if (GetRawInputData(h, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0) return;
    if (size == 0 || size > 4096) return;

    BYTE stack[4096];
    const int64_t t0 = Qpc();
    if (GetRawInputData(h, RID_INPUT, stack, &size, sizeof(RAWINPUTHEADER)) != size) return;
    g_perf.tRead += Qpc() - t0;

    ++g_wmInputTotal;
    auto* ri = reinterpret_cast<RAWINPUT*>(stack);
    if (ri->header.dwType == RIM_TYPEMOUSE)         { ++g_rawMouse; OnRawMouse(ri->header.hDevice, ri->data.mouse); }
    else if (ri->header.dwType == RIM_TYPEKEYBOARD) { ++g_rawKbd;   OnRawKeyboard(ri->header.hDevice, ri->data.keyboard); }
    else if (ri->header.dwType == RIM_TYPEHID)      { OnRawHid(ri->header.hDevice, ri->data.hid, size > offsetof(RAWINPUT, data.hid.bRawData) ? size - offsetof(RAWINPUT, data.hid.bRawData) : 0); }
}

// ---------------------------------------------------------------- gate
//
// The load-bearing suppression. RIDEV_NOLEGACY was assumed to silence
// physical mouse clicks system-wide; measured with the --monitor hook, it
// does not — every physical click was reaching applications through the
// legacy pipeline IN ADDITION to our per-seat injection. Single clicks
// arrived as doubles, double clicks as quadruples (paragraph select), drags
// as two interleaved button streams. This lived in the engine from the first
// build, disguised as a dozen smaller symptoms.
//
// A WH_MOUSE_LL hook can EAT events by returning nonzero. Physical (non-
// injected) button and wheel events are swallowed here; injected events —
// ours and other software's — pass. Movement passes untouched: the cursor
// cage already owns motion, and eating moves would risk eating our own
// SetCursorPos stream.
//
// Safety: the hook dies with the process; the watchdog and every exit path
// unhook explicitly; if the engine stops injecting while the hook eats,
// keyboards still work (panic key) because this gate touches only the mouse.

HHOOK g_mouseGate = nullptr;

// Eat physical MOVES too, not just buttons. This is what replaces the 1px
// ClipCursor cage: drift is stopped at the source instead of being corrected
// afterwards, so the hardware cursor only ever moves where we put it.
//
// Safe because SetCursorPos generates no low-level hook event at all
// (measured on this machine — a warp cannot be eaten by our own hook), and
// because eating in the MOUSE hook leaves raw input intact (measured: 242
// physical button events still delivered while the gate ate them). Both were
// verified before enabling; the keyboard equivalent is impossible for exactly
// the opposite reason. Undocumented behaviour either way, so it is watched at
// runtime — see the self-blinding guard in WndProc.
bool                  g_eatMoves = true;
std::atomic<uint64_t> g_gateEatenMoves{0};

// Logs any timer tick that runs long under --perf: the timers share the thread
// that reads the mice, so a slow tick shows up as a pointer hitch.
struct TimerProbe {
    WPARAM  id;
    int64_t t0 = Qpc();
    explicit TimerProbe(WPARAM i) : id(i) {}
    ~TimerProbe() {
        if (!g_perfLog || !g_qpcFreq) return;
        const double us = (Qpc() - t0) * 1e6 / (double)g_qpcFreq;
        if (us >= 5000) LogLine(L"[perf] timer %u took %.0f us", (unsigned)id, us);
    }
};
LRESULT CALLBACK MouseGate(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && lp) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (!(m->flags & LLMHF_INJECTED)) {
            switch (wp) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
                return 1;                      // physical button/wheel: eaten
            case WM_MOUSEMOVE:
                if (g_eatMoves) { g_gateEatenMoves.fetch_add(1); return 1; }
                break;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// There is deliberately NO keyboard gate. One was tried and it broke typing
// completely, in a way worth recording: eating a physical key in a
// WH_KEYBOARD_LL hook ALSO suppresses that key's raw input delivery, so the
// engine went blind (measured: kbd=0 raw events across a whole session while
// the user typed) and never re-injected anything. Keystrokes vanished.
//
// The mouse does not behave this way — eating a physical button in
// WH_MOUSE_LL leaves its WM_INPUT intact (measured: 242 physical button
// events still seen by the engine in the same session). That asymmetry is
// why the mouse gate stays and the keyboard one cannot exist.
//
// Without raw input there is no device identity — KBDLLHOOKSTRUCT carries
// none — so eat-and-re-inject is impossible for keyboards. Keyboard support
// is therefore steering-only; see OnRawKeyboard.

void RemoveMouseGate() {
    if (g_mouseGate) { UnhookWindowsHookEx(g_mouseGate); g_mouseGate = nullptr; }
}

// The hook runs on its installing thread's message pump. The engine's input
// thread sleeps up to ~550 ms inside cross-window click handling, and a
// low-level hook that cannot be dispatched within the system timeout is
// silently BYPASSED — physical clicks would leak through at exactly the
// moments the engine is busy. So the gate lives on a dedicated thread whose
// only job is pumping, which nothing else can block.
DWORD g_gateThreadId = 0;

void GateThreadProc() {
    g_gateThreadId = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // OP16
    g_mouseGate = SetWindowsHookExW(WH_MOUSE_LL, MouseGate, GetModuleHandleW(nullptr), 0);
    if (!g_mouseGate) {
        LogLine(L"[gate] SetWindowsHookEx failed: %lu — physical clicks are NOT suppressed", GetLastError());
        return;
    }
    LogLine(L"[gate] physical mouse suppression active — buttons, wheel%s",
            g_eatMoves ? L" and moves (no cursor cage)" : L" only (cursor cage in use)");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RemoveMouseGate();
    LogLine(L"[gate] removed");
}

void StopGateThread() {
    RemoveMouseGate();   // hook first, so nothing is eaten while the thread winds down
    if (g_gateThreadId) PostThreadMessageW(g_gateThreadId, WM_QUIT, 0, 0);
}

// ---------------------------------------------------------------- capture

bool RegisterCapture(HWND hwnd, bool keyboards) {
    RAWINPUTDEVICE rid[2];
    int n = 0;

    // RIDEV_DEVNOTIFY is what actually delivers WM_INPUT_DEVICE_CHANGE. Without
    // it that handler never runs and a mouse plugged in later is never bound.
    //
    // No RIDEV_NOLEGACY, deliberately. Measured on this machine: a background
    // INPUTSINK registration's NOLEGACY does NOT suppress physical clicks
    // system-wide — apps kept receiving the legacy stream alongside our
    // injections, doubling every click. Raw input here is used purely as the
    // per-device attribution tap; the actual suppression is the WH_MOUSE_LL
    // gate (see MouseGate), which provably eats the physical events.
    rid[n].usUsagePage = 0x01;
    rid[n].usUsage     = 0x02;                       // generic mouse
    rid[n].dwFlags     = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid[n].hwndTarget  = hwnd;
    ++n;

    // Keyboards are registered either way, observe-only at this layer: the
    // raw stream is what detects the panic combo and (with routing on) feeds
    // per-seat re-injection. No NOLEGACY — same per-application scoping as
    // the mouse, it would suppress nothing for other apps. The actual
    // suppression while routing is live is the WH_KEYBOARD_LL gate.
    rid[n].usUsagePage = 0x01;
    rid[n].usUsage     = 0x06;                       // generic keyboard
    rid[n].dwFlags     = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid[n].hwndTarget  = hwnd;
    ++n;

    if (!RegisterRawInputDevices(rid, n, sizeof(RAWINPUTDEVICE))) {
        LogLine(L"[error] RegisterRawInputDevices failed: %lu", GetLastError());
        return false;
    }

    // The other collections a mouse exposes (media keys, system controls, the
    // manufacturer's own page), observe-only, for learned buttons. They are only
    // ever looked at when the device is a mouse's own interface (g_ownerSeat).
    // Failing here costs the learning of those buttons, not the pointers.
    RAWINPUTDEVICE hid[3] = {};
    hid[0] = { 0x0C,   0,    RIDEV_INPUTSINK | RIDEV_PAGEONLY, hwnd };   // consumer / media
    hid[1] = { 0x01,   0x80, RIDEV_INPUTSINK,                  hwnd };   // system control
    hid[2] = { 0xFF00, 0,    RIDEV_INPUTSINK | RIDEV_PAGEONLY, hwnd };   // manufacturer
    if (!RegisterRawInputDevices(hid, 3, sizeof(RAWINPUTDEVICE)))
        LogLine(L"[warn] media / manufacturer collections not registered: %lu — those buttons cannot be learned",
                GetLastError());
    g_keyboardRouting = keyboards;
    g_captureActive   = true;
    return true;
}

void UnregisterCapture() {
    if (!g_captureActive) return;
    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = nullptr;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_REMOVE; rid[1].hwndTarget = nullptr;
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
    RAWINPUTDEVICE hid[3] = {};
    hid[0] = { 0x0C,   0,    RIDEV_REMOVE | RIDEV_PAGEONLY, nullptr };
    hid[1] = { 0x01,   0x80, RIDEV_REMOVE,                  nullptr };
    hid[2] = { 0xFF00, 0,    RIDEV_REMOVE | RIDEV_PAGEONLY, nullptr };
    for (const RAWINPUTDEVICE& r : hid) RegisterRawInputDevices(const_cast<RAWINPUTDEVICE*>(&r), 1, sizeof(RAWINPUTDEVICE));
    g_keyboardRouting = false;
    g_captureActive   = false;
    // Every exit path funnels through here; a cage left behind would trap
    // the machine's only pointer in one pixel.
    UnpinCursor();
}

void RequestExit() {
    g_running = false;
    StopGateThread();
    UnregisterCapture();
    RestoreSystemCursor();
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// If the message loop stalls while we hold a system-wide NOLEGACY
// registration, the user has no working mouse or keyboard. Release capture
// from a separate thread so the machine stays usable.
void WatchdogThread() {
    uint64_t last = g_heartbeat.load();
    int stalled = 0;
    while (g_running.load()) {
        Sleep(500);
        const uint64_t now = g_heartbeat.load();
        if (now == last) {
            if (++stalled >= 6) {           // ~3 seconds with no progress
                LogLine(L"[watchdog] message loop stalled — releasing capture");
                StopGateThread();           // physical clicks must flow again
                UnregisterCapture();
                // Releasing capture gives the mice back, but a hidden caged
                // cursor would leave the user with working input and no
                // pointer to see. Restore visibility too.
                RestoreSystemCursor();
                stalled = 0;
            }
        } else {
            stalled = 0;
            last = now;
        }
    }
}

// ---------------------------------------------------------------- binding

int SeatCursorStyle(int i) {
    return i < (int)g_cfg.cursorStyle.size() ? g_cfg.cursorStyle[i] : (int)kStyleBright;
}

// A seat's pointer: its state, its overlay and the place it starts from.
void InitSeat(int i, POINT at) {
    g_seats[i]       = Seat{};
    g_seats[i].index = i;
    g_seats[i].pos   = at;
    // A seat kept on a screen starts in its middle rather than at an edge.
    if (g_bounds[i].screen) {
        const RECT& r = g_bounds[i].rc;
        g_seats[i].pos = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    }
    ClampSeat(i, g_seats[i].pos);
    const COLORREF c = (i < (int)g_cfg.seatColors.size())
                     ? g_cfg.seatColors[i] : RGB(200, 200, 200);
    // Every seat gets an overlay, including seat 0. Seat 0 starts out
    // owning the hardware cursor so its overlay starts hidden, but it
    // needs to exist: the moment another seat clicks, seat 0 loses the
    // cursor and SetActiveSeat asks for that overlay. Creating it only for
    // i != 0 made seat 0's pointer vanish permanently on the first
    // handover.
    g_overlayOk[i] = g_overlay[i].Create(g_inst, i, c, SeatCursorStyle(i));
    g_overlay[i].MoveTo(g_seats[i].pos);
    g_seatReady[i] = true;
    LogLine(L"[seat %d] colour %02X%02X%02X  overlay %s", i,
            GetRValue(c), GetGValue(c), GetBValue(c),
            g_overlayOk[i] ? L"created" : L"FAILED TO CREATE");
}

// Seats a rescan has added to the configuration get their pointer here, beside
// the one that owns the cursor so the new arrow is easy to find.
void CreateNewSeats() {
    for (int i = 0; i < g_cfg.seatCount; ++i) {
        if (g_seatReady[i]) continue;
        POINT at = g_seats[g_activeSeat].pos;
        at.x += 40; at.y += 24;
        InitSeat(i, at);
    }
}

int FirstPresentSeat() {
    for (int i = 0; i < g_cfg.seatCount; ++i)
        if (g_seatReady[i] && g_seatPresent[i]) return i;
    return -1;
}

// A seat's arrow follows its mouse: shown while it is plugged in, hidden (not
// destroyed, so the seat keeps its colour and speed) once it is unplugged, and
// shown again where it stopped. Never hides the last arrow: with no seat
// present at all, the one left stays, so the screen is not left without one.
void SyncSeatArrows() {
    const int first = FirstPresentSeat();
    if (first < 0) return;
    // The seat that owns the real cursor is gone: a present one takes it over.
    if (g_activeSeat < 0 || g_activeSeat >= g_cfg.seatCount || !g_seatPresent[g_activeSeat])
        SetActiveSeat(first);
    for (int i = 0; i < g_cfg.seatCount; ++i) {
        if (!g_seatReady[i]) continue;
        if (g_seatPresent[i]) {
            if (g_overlay[i].IsVisible()) continue;
            // The owner draws through the real cursor unless that one is hidden.
            if (i == g_activeSeat && (!g_cursorHidden || g_overShell)) continue;
            g_overlay[i].MoveTo(g_seats[i].pos);
            g_overlay[i].Show(true);
        } else {
            // A button still held by the unplugged mouse would stay pressed for good.
            Seat& st = g_seats[i];
            if (st.lDown)  { SendMouseAt(st.pos, MOUSEEVENTF_LEFTUP);   st.lDown = false; }
            if (st.rDown)  { SendMouseAt(st.pos, MOUSEEVENTF_RIGHTUP);  st.rDown = false; }
            if (st.mDown)  { SendMouseAt(st.pos, MOUSEEVENTF_MIDDLEUP); st.mDown = false; }
            if (st.x1Down) { SendMouseAt(st.pos, MOUSEEVENTF_XUP, XBUTTON1); st.x1Down = false; }
            if (st.x2Down) { SendMouseAt(st.pos, MOUSEEVENTF_XUP, XBUTTON2); st.x2Down = false; }
            if (g_overlay[i].IsVisible()) g_overlay[i].Show(false);
        }
    }
}

// A mouse openMouse has never seen: a seat of its own, taking the speed of
// seat 0 until it is tuned. A seat past the end of the table is a new one.
void AddSeatConfig(int seat) {
    g_cfg.seatCount = seat + 1;
    g_cfg.EnsureSeatSlots(g_cfg.seatCount);
    g_cfg.seatColors[seat]  = g_cfg.PickSeatColor(seat);
    g_cfg.sensitivity[seat] = g_cfg.sensitivity[0];
    g_cfg.accel[seat]       = g_cfg.accel[0];
}

// Binds the mouse interfaces the ini does not know to a seat, and writes the
// assignment back so the mouse finds the same seat and colour next time.
// Everything that is not a mouse of its own stays dead:
//   - integrated devices (ACPI touchpads, PS/2) and anything without a
//     manufacturer id;
//   - another collection of hardware that is already bound (same product,
//     different MI_/Col suffix): a ghost of a mouse that has its seat;
//   - of a new product, every collection but one.
// A second mouse of a bound model is told from a ghost by having the SAME
// suffix as the bound one: a second Col01 is a second mouse.
void SeatNewMice(const std::vector<DeviceInfo>& devices,
                 std::map<std::wstring, const wchar_t*>& deadWhy) {
    std::set<std::wstring> boundKeys, boundBases;
    std::set<int> usedSeats;
    for (const auto& kv : g_cfg.mouseBinding) {
        boundKeys.insert(HardwareKey(kv.first));
        boundBases.insert(HardwareBase(kv.first));
        usedSeats.insert(kv.second);
    }

    struct Fresh { const DeviceInfo* d; std::wstring key, base; };
    std::vector<Fresh> fresh;
    for (const auto& d : devices) {
        if (d.type != RIM_TYPEMOUSE || g_cfg.mouseBinding.count(d.path)) continue;
        if (!IsAutoSeatCandidate(d.path)) {
            deadWhy[d.path] = L"integrated device or no manufacturer id";
            continue;
        }
        Fresh f{ &d, HardwareKey(d.path), HardwareBase(d.path) };
        if (boundBases.count(f.base) && !boundKeys.count(f.key)) {
            deadWhy[d.path] = L"other collection of a bound mouse";
            continue;
        }
        fresh.push_back(std::move(f));
    }

    // One collection per new product: the one most interfaces share, the
    // lowest suffix on a tie (Col01 before Col02).
    std::map<std::wstring, std::map<std::wstring, int>> familyKeys;
    for (const auto& f : fresh)
        if (!boundBases.count(f.base)) familyKeys[f.base][f.key]++;
    std::map<std::wstring, std::wstring> primary;
    for (const auto& fam : familyKeys) {
        int best = 0;
        for (const auto& k : fam.second)
            if (k.second > best) { best = k.second; primary[fam.first] = k.first; }
    }

    bool added = false;
    for (const auto& f : fresh) {
        if (!boundBases.count(f.base) && f.key != primary[f.base]) {
            deadWhy[f.d->path] = L"other collection of a new mouse";
            continue;
        }
        // The first seat no mouse holds in the ini; past the end of the table it is a new seat.
        int seat = -1;
        for (int s = 0; s < kMaxSeats; ++s)
            if (!usedSeats.count(s)) { seat = s; break; }
        if (seat < 0) {
            deadWhy[f.d->path] = L"all seats are taken";
            LogLine(L"[warn] mouse %s : no seat left (%d seats)",
                    f.d->friendlyName.c_str(), kMaxSeats);
            continue;
        }
        if (seat >= g_cfg.seatCount) AddSeatConfig(seat);
        usedSeats.insert(seat);
        g_cfg.mouseBinding[f.d->path] = seat;
        added = true;
        const COLORREF c = g_cfg.seatColors[seat];
        LogLine(L"[new] mouse    seat %d  %s  colour %02X%02X%02X  (%d seats)", seat,
                f.d->friendlyName.c_str(), GetRValue(c), GetGValue(c), GetBValue(c),
                g_cfg.seatCount);
    }
    if (added && !g_cfg.Save(Config::DefaultPath()))
        LogLine(L"[error] could not write %s", Config::DefaultPath().c_str());
}

// The seat whose mouse a keyboard or HID interface belongs to: the one whose
// bound mouse has the same hardware id. -1 when there is none, when two seats'
// mice share that id (which of the two pressed cannot be told), or when the ini
// binds the interface itself to a seat as a keyboard (a real one).
int OwnerSeatOf(const std::wstring& path) {
    if (g_cfg.keyboardBinding.count(path)) return -1;
    const std::wstring base = HardwareBase(path);
    if (base.empty()) return -1;
    int seat = -1;
    for (const auto& kv : g_cfg.mouseBinding) {
        if (HardwareBase(kv.first) != base) continue;
        if (seat >= 0 && seat != kv.second) return -1;
        seat = kv.second;
    }
    return seat;
}

// The laptop touchpad is seated by rule, not as a mouse, so that there are
// always two arrows to work with:
//   - no external mouse : the touchpad alone;
//   - one external mouse: that mouse and the touchpad;
//   - two or more       : the mice only, the touchpad is cut off.
// touchpad=on / off in the ini overrides this (off still yields to the safety
// rule: with no external mouse the touchpad is kept, never leave no pointer).
// A touchpad has its seat in the ini even while it is cut off, so it comes
// back with the same colour and speed, and the settings window can show it.
// Only one interface of it is ever seated.
void SeatTouchpad(const std::vector<DeviceInfo>& devices,
                  std::map<std::wstring, const wchar_t*>& deadWhy,
                  std::set<std::wstring>& cutOff) {
    std::set<int> externalSeats;
    for (const auto& d : devices) {
        if (d.type != RIM_TYPEMOUSE || IsIntegratedPointer(d.path)) continue;
        const auto it = g_cfg.mouseBinding.find(d.path);
        if (it != g_cfg.mouseBinding.end()) externalSeats.insert(it->second);
    }
    const size_t externals = externalSeats.size();
    const bool active = g_cfg.touchpad == Config::Touchpad::On ||
                        (g_cfg.touchpad == Config::Touchpad::Auto && externals <= 1) ||
                        externals == 0;

    static int lastState = -1;   // -1 unknown, else 0 cut off / 1 seated; logged on change only
    std::set<std::wstring> seen;
    bool changed = false;
    for (const auto& d : devices) {
        if (d.type != RIM_TYPEMOUSE || !IsIntegratedPointer(d.path)) continue;
        // A second interface of the same touchpad is a ghost of the first.
        std::wstring base = HardwareBase(d.path);
        if (base.empty()) base = d.path;
        if (!seen.insert(base).second) {
            deadWhy[d.path] = L"other interface of the touchpad";
            cutOff.insert(d.path);
            continue;
        }
        // Seated even while cut off, so that the settings window always has
        // its section to switch it back on from.
        if (!active) {
            deadWhy[d.path] = L"touchpad cut off";
            cutOff.insert(d.path);
        }
        if (g_cfg.mouseBinding.count(d.path)) continue;
        // Same touchpad under a new instance id: take over its seat.
        bool taken = false;
        for (auto it = g_cfg.mouseBinding.begin(); it != g_cfg.mouseBinding.end(); ++it) {
            if (!IsIntegratedPointer(it->first) || HardwareBase(it->first) != base) continue;
            const int seat = it->second;
            g_cfg.mouseBinding.erase(it);
            g_cfg.mouseBinding[d.path] = seat;
            taken = changed = true;
            break;
        }
        if (taken) continue;
        std::set<int> usedSeats;
        for (const auto& kv : g_cfg.mouseBinding) usedSeats.insert(kv.second);
        int seat = -1;
        for (int k = 0; k < kMaxSeats; ++k)
            if (!usedSeats.count(k)) { seat = k; break; }
        if (seat < 0) {
            deadWhy[d.path] = L"all seats are taken";
            cutOff.insert(d.path);
            LogLine(L"[warn] touchpad %s : no seat left (%d seats)", d.friendlyName.c_str(), kMaxSeats);
            continue;
        }
        if (seat >= g_cfg.seatCount) AddSeatConfig(seat);
        g_cfg.mouseBinding[d.path] = seat;
        changed = true;
        LogLine(L"[new] touchpad seat %d  %s  (%d seats)", seat, d.friendlyName.c_str(),
                g_cfg.seatCount);
    }
    if (seen.empty()) return;
    if (lastState != (active ? 1 : 0)) {
        lastState = active ? 1 : 0;
        LogLine(L"[touchpad] %s  (%u external mice, mode %s)",
                active ? L"seated" : L"cut off", (unsigned)externals,
                g_cfg.touchpad == Config::Touchpad::On ? L"on"
              : g_cfg.touchpad == Config::Touchpad::Off ? L"off" : L"auto");
    }
    if (changed && !g_cfg.Save(Config::DefaultPath()))
        LogLine(L"[error] could not write %s", Config::DefaultPath().c_str());
}

// Without any measured binding the assignment is a runtime guess, never
// written back: this machine exposed four HID mouse interfaces for two
// physical mice, and persisting a guess would be indistinguishable from what
// --identify measured. Once the ini holds measured mice, a mouse it does not
// know is given a seat (SeatNewMice) and that seat IS written back.
void ApplyBindings() {
    g_mouseSeat.clear();
    g_keyboardSeat.clear();
    g_deadMice.clear();
    g_deadKeyboards.clear();

    const auto devices = EnumerateDevices();
    const bool haveMouseCfg = !g_cfg.mouseBinding.empty();
    const bool haveKbdCfg   = !g_cfg.keyboardBinding.empty();
    int autoMouse = 0, autoKeyboard = 0;

    // A mouse moved to another USB port, or renumbered by a reboot, comes
    // back with a new instance id in its path and would no longer match.
    // Exact paths are claimed first, so of two identical mice each keeps its
    // own entry when it can; the entries left over then go, in order, to the
    // present mice of the same hardware id, and the ini follows.
    std::set<std::wstring> claimed;
    for (const auto& d : devices)
        if (d.type == RIM_TYPEMOUSE && g_cfg.mouseBinding.count(d.path)) claimed.insert(d.path);
    std::map<std::wstring, int> moved;   // new path -> seat, taken over from a stale entry
    for (const auto& d : devices) {
        if (d.type != RIM_TYPEMOUSE || g_cfg.mouseBinding.count(d.path)) continue;
        const std::wstring key = StableKey(d.path);
        if (key.empty()) continue;
        for (auto it = g_cfg.mouseBinding.begin(); it != g_cfg.mouseBinding.end(); ++it) {
            if (claimed.count(it->first) || StableKey(it->first) != key) continue;
            moved[d.path] = it->second;
            g_cfg.mouseBinding.erase(it);
            break;
        }
    }
    for (const auto& kv : moved) {
        g_cfg.mouseBinding[kv.first] = kv.second;
        LogLine(L"[rebind] mouse seat %d : chemin mis à jour  %s", kv.second, kv.first.c_str());
    }
    if (!moved.empty() && !g_cfg.Save(Config::DefaultPath()))
        LogLine(L"[error] could not write %s", Config::DefaultPath().c_str());

    std::map<std::wstring, const wchar_t*> deadWhy;
    std::set<std::wstring> cutOff;   // touchpad interfaces kept dead whatever the ini says
    if (haveMouseCfg) {
        SeatNewMice(devices, deadWhy);
        SeatTouchpad(devices, deadWhy, cutOff);
    }

    for (const auto& d : devices) {
        if (d.type == RIM_TYPEMOUSE) {
            auto it = g_cfg.mouseBinding.find(d.path);
            if (it != g_cfg.mouseBinding.end() && !cutOff.count(d.path)) {
                g_mouseSeat[d.handle] = it->second;
                LogLine(L"[bind] mouse    seat %d  %s", it->second, d.friendlyName.c_str());
            } else if (haveMouseCfg) {
                // Measured bindings exist, so an unmatched interface is a
                // ghost collection, an integrated device or a mouse past the
                // last seat. Kill it rather than let it double-drive seat 0.
                g_deadMice.insert(d.handle);
                const auto why = deadWhy.find(d.path);
                LogLine(L"[dead] mouse    %s  (%s, dropped)", d.friendlyName.c_str(),
                        why != deadWhy.end() ? why->second : L"unidentified interface");
            } else {
                // No measured config at all: auto-bind so first run works.
                const int seat = (autoMouse < g_cfg.seatCount) ? autoMouse : 0;
                g_mouseSeat[d.handle] = seat;
                LogLine(L"[auto] mouse    seat %d  %s  (not identified)",
                        seat, d.friendlyName.c_str());
            }
            ++autoMouse;
        } else {
            auto it = g_cfg.keyboardBinding.find(d.path);
            if (it != g_cfg.keyboardBinding.end()) {
                g_keyboardSeat[d.handle] = it->second;
                LogLine(L"[bind] keyboard seat %d  %s", it->second, d.friendlyName.c_str());
            } else if (haveKbdCfg) {
                g_deadKeyboards.insert(d.handle);
                LogLine(L"[dead] keyboard %s  (unidentified interface, dropped)",
                        d.friendlyName.c_str());
            } else {
                const int seat = (autoKeyboard < g_cfg.seatCount) ? autoKeyboard : 0;
                g_keyboardSeat[d.handle] = seat;
                LogLine(L"[auto] keyboard seat %d  %s  (not identified)",
                        seat, d.friendlyName.c_str());
            }
            ++autoKeyboard;
        }
    }
    if (!haveMouseCfg)
        LogLine(L"[warn] no measured bindings — run openmouse.exe --identify");

    for (bool& p : g_seatPresent) p = false;
    for (const auto& kv : g_mouseSeat)
        if (kv.second >= 0 && kv.second < kMaxSeats) g_seatPresent[kv.second] = true;

    // The keyboard and HID interfaces that belong to a seat's mouse: same
    // hardware id as a mouse bound to the seat. Two mice of one model share it,
    // and then which of them pressed a media key cannot be told: those
    // interfaces belong to no seat. A keyboard the ini binds to a seat is a
    // real one and is never taken for a mouse's.
    g_ownerSeat.clear();
    g_hidPage.clear();
    auto own = [&](const DeviceInfo& d) {
        const int seat = OwnerSeatOf(d.path);
        if (seat < 0) return;
        g_ownerSeat[d.handle] = seat;
        if (d.type == RIM_TYPEHID) g_hidPage[d.handle] = d.page;
    };
    for (const auto& d : devices)
        if (d.type == RIM_TYPEKEYBOARD) own(d);
    for (const auto& d : EnumerateHidDevices()) own(d);
    RefreshMapFlags();
}

// Tells the settings window, when it is open, which mice are plugged in.
void NotifySeats() {
    HWND w = FindWindowW(kSettingsClassName, nullptr);
    if (!w) return;
    WPARAM mask = 0;
    for (int i = 0; i < kMaxSeats; ++i)
        if (g_seatPresent[i]) mask |= (WPARAM)1 << i;
    PostMessageW(w, kSeatsMsg, mask, 0);
}

// The mice that are plugged in, and the touchpad rule, applied to the seats.
void Rescan() {
    ApplyBindings();
    CreateNewSeats();      // a mouse seen for the first time: its pointer
    SyncSeatArrows();      // a mouse back or gone: its arrow shows or hides
    NotifySeats();         // the settings window follows, if it is open
}

// ---------------------------------------------------------------- window

void TrayMenu(HWND hwnd);   // defined with the other tray plumbing below

// The settings window has written new seat values: take the speed ones at
// once. Speed, buttons, colours and pointer sets are live; the rest of the file
// (bindings, seat count) still needs a restart.
void ReclampSeats() {
    for (int i = 0; i < g_cfg.seatCount && i < kMaxSeats; ++i) {
        const POINT before = g_seats[i].pos;
        ClampSeat(i, g_seats[i].pos);
        if (before.x != g_seats[i].pos.x || before.y != g_seats[i].pos.y) {
            g_dirty[i] = true;
            FlushSeat(i);
        }
    }
}

void ReloadSettings() {
    Config fresh;
    if (!fresh.Load(Config::DefaultPath())) {
        LogLine(L"[settings] reload asked, but the configuration could not be read");
        return;
    }
    g_cfg.sensitivity = fresh.sensitivity;
    g_cfg.accel       = fresh.accel;
    const bool padChanged = g_cfg.touchpad != fresh.touchpad;
    g_cfg.touchpad    = fresh.touchpad;
    g_cfg.screen      = fresh.screen;
    // Colour and set of pointers: redrawn at once.
    g_cfg.cursorStyle = fresh.cursorStyle;
    for (int i = 0; i < g_cfg.seatCount && i < kMaxSeats; ++i) {
        if (i < (int)fresh.seatColors.size()) g_cfg.seatColors[i] = fresh.seatColors[i];
        if (g_seatReady[i])
            g_overlay[i].SetLook(g_cfg.seatColors[i], SeatCursorStyle(i));
    }
    // Learned buttons: end what the old table holds down, then swap.
    ReleaseHeldButtons();
    g_cfg.buttons     = fresh.buttons;
    RefreshMapFlags();
    ResolveSeatBounds();
    ReclampSeats();
    for (auto& a : g_accel) a = AccelState{};
    for (int i = 0; i < g_cfg.seatCount; ++i) {
        const double k = i < (int)g_cfg.sensitivity.size() ? g_cfg.sensitivity[i] : 1.0;
        if (i < (int)g_cfg.accel.size() && g_cfg.accel[i].on)
            LogLine(L"[settings] seat %d accel slow=%.2f fast=%.2f speed=%.2f", i,
                    g_cfg.accel[i].slow, g_cfg.accel[i].fast, g_cfg.accel[i].speed);
        else
            LogLine(L"[settings] seat %d sensitivity %.2f", i, k);
    }
    if (padChanged) Rescan();   // the touchpad is seated or cut off at once
}

// Peak of each step within one event, and a per-step breakdown of any event
// over 20 ms, so a hitch names the step that took the time.
void NoteEventSteps(const PerfStats& before, int64_t dt, DWORD lag, const wchar_t* src) {
    const int64_t read  = g_perf.tRead    - before.tRead;
    const int64_t pin   = g_perf.tPin     - before.tPin;
    const int64_t over  = g_perf.tOverlay - before.tOverlay;
    const int64_t shell = g_perf.tShell   - before.tShell;
    const int64_t hit   = g_perf.tHit     - before.tHit;
    const int64_t cls   = g_perf.tClass   - before.tClass;
    const int64_t set   = g_perf.tSettle  - before.tSettle;
    if (read  > g_perf.mRead)    g_perf.mRead    = read;
    if (pin   > g_perf.mPin)     g_perf.mPin     = pin;
    if (over  > g_perf.mOverlay) g_perf.mOverlay = over;
    if (shell > g_perf.mShell)   g_perf.mShell   = shell;
    if (hit   > g_perf.mHit)     g_perf.mHit     = hit;
    if (cls   > g_perf.mClass)   g_perf.mClass   = cls;
    if (!g_perfLog || !g_qpcFreq) return;
    const double us = 1e6 / (double)g_qpcFreq;
    if (dt * us < 20000) return;
    const int64_t other = dt - read - pin - over - shell;
    LogLine(L"[perf] slow %s event %.0f us: read=%.0f pin=%.0f overlay=%.0f shell=%.0f"
            L" (hit=%.0f class=%.0f) settle=%.0f other=%.0f lag=%lu ms",
            src, dt * us, read * us, pin * us, over * us, shell * us,
            hit * us, cls * us, set * us, other * us, (unsigned long)lag);
}

// Puts every arrow back above whatever topmost window now covers it. Cheap
// when nothing does: the z-order walk only costs a SetWindowPos on a hit.
void ReassertArrows() {
    for (int i = 0; i < kMaxSeats; ++i) {
        const int64_t t0 = Qpc();
        wchar_t who[64] = L"";
        if (g_overlay[i].ReassertTopmost(who, 64) && g_perfLog) {
            const double us = g_qpcFreq ? 1e6 / (double)g_qpcFreq : 0;
            LogLine(L"[perf] topmost seat %d reasserted in %.0f us (under %s)", i, (Qpc() - t0) * us, who);
        }
    }
}

// ---------------------------------------------------------------- start with Windows

// Set by --startup, which only the logon task passes.
bool g_startup = false;

std::atomic<int>  g_autoState{-2};   // -2 not read yet, -1 unknown, 0 no task, 1 task
std::atomic<bool> g_autoBusy{false};

// The Task Scheduler is asked on threads of their own: this thread carries
// every mouse of the machine and must not wait on an RPC.
void AutostartWork(WPARAM request) {
    if (request == kAutostartEnable)       AutostartInstall();
    else if (request == kAutostartDisable) AutostartRemove();
    g_autoState = AutostartState();
    g_autoBusy  = false;
}

LRESULT OnAutostart(WPARAM request) {
    static const bool elevated = IsElevated();
    // A change is only taken by an elevated engine, one at a time. The
    // settings window sees the outcome by asking again.
    if (request != kAutostartQuery && elevated && !g_autoBusy.exchange(true))
        std::thread(AutostartWork, request).detach();
    const int st = g_autoState;
    LRESULT r = 0;
    if (st >= 0 && !g_autoBusy) r |= kAutoKnown;
    if (st == 1)                r |= kAutoPresent;
    if (elevated)               r |= kAutoElevated;
    return r;
}

// At logon the task fires before the shell has drawn the desktop and before
// every mouse is enumerated. Hiding the hardware cursor then could leave a
// screen with no pointer at all, so wait for the taskbar and for one mouse to
// exist. After 40 s start anyway: the rescan on device change picks up the
// late ones, and the watchdog and --restore-cursor still give the pointer back.
void WaitForDesktop() {
    const DWORD t0 = GetTickCount();
    bool shell = false, mouse = false;
    while (GetTickCount() - t0 < 40000) {
        if (!shell) shell = FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr;
        if (!mouse)
            for (const auto& d : EnumerateDevices())
                if (d.type == RIM_TYPEMOUSE) { mouse = true; break; }
        if (shell && mouse) break;
        Sleep(500);
    }
    LogLine(L"[startup] desktop %s, mouse %s after %lu ms", shell ? L"ready" : L"NOT ready",
            mouse ? L"found" : L"NOT found", GetTickCount() - t0);
    Sleep(1500);   // the shell is still settling when its window first exists
}

constexpr UINT_PTR kMenuSettleTimer = 4;
HWND  g_menuTimerWnd = nullptr;
DWORD g_trayClickTick = 0;            // when the tray icon was clicked, to time the menu
DWORD g_menuEventTick = 0;            // when the last menu event fired, to time the passes
int   g_menuStage = 0;

// Delay before each follow-up pass, so the passes land at 30, 80 and 200 ms
// after the menu event: the first catches a menu already on screen, the later
// ones a menu the system shows a few frames after announcing it.
constexpr UINT kMenuStageDelay[] = {30, 50, 120};
constexpr int  kMenuStages = (int)(sizeof(kMenuStageDelay) / sizeof(kMenuStageDelay[0]));

// Brings the arrows back in line with what is on screen now, without waiting
// for a mouse move. The shell fallback is the part that needs it: with the
// pointer over the taskbar the arrow is hidden and the real cursor shown, and
// that state is only re-read on a move. Right after a click on the tray icon
// the menu is up over the pointer, nothing moves, and the arrow stayed hidden
// under it until the first twitch of the mouse.
void RefreshArrowsNow() {
    if (g_cursorHidden && g_activeSeat >= 0 && g_activeSeat < g_cfg.seatCount) {
        g_overlay[g_activeSeat].MoveTo(g_seats[g_activeSeat].pos);   // forces a redraw
        UpdateShellCursorFallback(g_activeSeat);
    }
    for (int i = 0; i < g_cfg.seatCount; ++i)
        if (i != g_activeSeat) g_overlay[i].MoveTo(g_seats[i].pos);
    ReassertArrows();
}

// Back on the normal desktop after Ctrl+Alt+Del or a locked session. The
// secure desktop takes the input, the drawing and the cursor away from every
// ordinary program while it is up; what we registered may not have survived
// it. Registering again is harmless when it did, so put back, in order: the raw
// input registration, the hidden hardware cursor, the arrows. (The secure
// desktop itself stays out of reach: there Windows gives the normal pointer.)
void ResumeAfterSecureDesktop(const wchar_t* why) {
    LogLine(L"[session] %s — registering input again", why);
    if (!RegisterCapture(g_hwnd, g_cfg.captureKeyboards))
        LogLine(L"[session] raw input could not be registered again");
    if (g_cursorHidden && g_magReady && g_mag.ShowSystemCursor && !g_mag.ShowSystemCursor(FALSE))
        LogLine(L"[session] hardware cursor could not be hidden again: %lu", GetLastError());
    if (!g_mouseGate) LogLine(L"[session] the mouse gate is gone — restart openMouse");
    RefreshArrowsNow();
}

// A menu opens (or closes) over the arrows: re-read the state at once, then
// again at 30, 80 and 200 ms. Bounded, so nothing is reasserted permanently
// (the once-a-second pass of OP8 stays the only steady one).
void CALLBACK MenuPopupEvent(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
    const DWORD now = GetTickCount();
    if (g_verbose || g_perfLog)
        LogLine(L"[menu] popup %s, %lu ms after the tray click",
                event == EVENT_SYSTEM_MENUPOPUPSTART ? L"start" : L"end",
                (unsigned long)(g_trayClickTick ? now - g_trayClickTick : 0));
    g_menuEventTick = now;
    RefreshArrowsNow();
    g_menuStage = 0;
    if (g_menuTimerWnd) SetTimer(g_menuTimerWnd, kMenuSettleTimer, kMenuStageDelay[0], nullptr);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kTrayCallbackMsg:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU || lp == WM_LBUTTONUP) {
            g_trayClickTick = GetTickCount();
            TrayMenu(hwnd);
        }
        return 0;
    case WM_INPUT: {
        g_heartbeat.fetch_add(1);
        const DWORD lag = GetTickCount() - (DWORD)GetMessageTime();
        const PerfStats before = g_perf;
        const int64_t t0 = Qpc();
        OnRawInput(reinterpret_cast<HRAWINPUT>(lp));
        MaybeFlushPointers();
        const int64_t dt = Qpc() - t0;
        NoteEventSteps(before, dt, lag, L"input");
        ++g_perf.events;
        g_perf.tTotal += dt;
        if (dt > g_perf.tMax) g_perf.tMax = dt;
        g_perf.lagSum += lag;
        if (lag > g_perf.lagMax) g_perf.lagMax = lag;
        return 0;
    }

    case kReloadSettingsMsg:
        ReloadSettings();
        return 0;

    case kAutostartMsg:
        return OnAutostart(wp);
    case kSeatsMsg:
        NotifySeats();
        return 0;
    case kLearnMsg:
        // The settings window asks to listen to one mouse (wParam = seat + 1),
        // or to stop (0).
        if (wp == 0) StopLearn();
        else         StartLearn((int)wp - 1);
        return 0;

    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_UNLOCK)         ResumeAfterSecureDesktop(L"session unlocked");
        else if (wp == WTS_CONSOLE_CONNECT)   ResumeAfterSecureDesktop(L"console reconnected");
        else if (wp == WTS_SESSION_LOCK)      LogLine(L"[session] locked — the secure desktop has the input");
        return 0;

    case WM_DISPLAYCHANGE:
        // A screen came or went: an enclosed seat follows it, or roams free.
        ResolveSeatBounds();
        ReclampSeats();
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_INPUT_DEVICE_CHANGE:
        // One of these arrives per device the moment we register, so a machine
        // with nine HID interfaces would re-enumerate nine times at startup —
        // each pass hitting the registry for every device. Coalesce the burst
        // onto a single deferred pass.
        SetTimer(hwnd, 3, 500, nullptr);
        return 0;

    case WM_TIMER: {
        TimerProbe probe(wp);
        if (wp == kShellRecheckTimer) {     // a shell flip was held back (OP16)
            KillTimer(hwnd, kShellRecheckTimer);
            if (g_activeSeat >= 0 && g_activeSeat < g_cfg.seatCount)
                UpdateShellCursorFallback(g_activeSeat);
            return 0;
        }
        if (wp == 3) {                      // coalesced device-change rescan
            KillTimer(hwnd, 3);
            Rescan();
            return 0;
        }
        if (wp == kMenuSettleTimer) {       // follow-up pass after a menu event
            RefreshArrowsNow();
            if (g_verbose || g_perfLog)
                LogLine(L"[menu] pass %d, %lu ms after the menu event", g_menuStage + 1,
                        (unsigned long)(GetTickCount() - g_menuEventTick));
            if (++g_menuStage < kMenuStages)
                SetTimer(hwnd, kMenuSettleTimer, kMenuStageDelay[g_menuStage], nullptr);
            else
                KillTimer(hwnd, kMenuSettleTimer);
            return 0;
        }
        if (wp == kLearnTimer) {            // nothing was heard in time
            const int seat = g_learnSeat;
            StopLearn();
            if (seat >= 0) {
                LogLine(L"[learn] seat %d : nothing heard", seat);
                NotifyLearn(seat, kLearnNothing);
            }
            return 0;
        }
        if (wp == 2) {                      // auto-exit timer
            LogLine(L"[exit] run time limit reached — releasing input capture");
            RequestExit();
            return 0;
        }
        g_heartbeat.fetch_add(1);
        if (wp == 1) {                      // nothing left pending once the queue is idle
            const PerfStats before = g_perf;
            const int64_t t0 = Qpc();
            FlushPointers();
            NoteEventSteps(before, Qpc() - t0, 0, L"timer");
        }
        if (wp == 1 && g_perfLog) {
            static int perfTick = 0;
            if (++perfTick >= 8 && g_perf.events) {   // every 2 s, only when moving
                perfTick = 0;
                const double us = g_qpcFreq ? 1e6 / (double)g_qpcFreq : 0;
                const double n  = (double)g_perf.events;
                const double f  = g_perf.flushes ? (double)g_perf.flushes : 1;
                LogLine(L"[perf] %.0f ev/s  flush/s=%.0f  per-event us: total=%.1f max=%.0f read=%.1f"
                        L"  per-flush us: pin=%.1f overlay=%.1f shell=%.1f  lag ms: avg=%.1f max=%lu",
                        n / 2.0, f / 2.0, g_perf.tTotal * us / n, g_perf.tMax * us,
                        g_perf.tRead * us / n, g_perf.tPin * us / f,
                        g_perf.tOverlay * us / f, g_perf.tShell * us / f,
                        g_perf.lagSum / n, g_perf.lagMax);
                LogLine(L"[perf] peak per event us: read=%.0f pin=%.0f overlay=%.0f shell=%.0f"
                        L" (hit=%.0f class=%.0f)",
                        g_perf.mRead * us, g_perf.mPin * us, g_perf.mOverlay * us,
                        g_perf.mShell * us, g_perf.mHit * us, g_perf.mClass * us);
                // Cost of one more arrow: the overlay step is per flush, and a
                // flush moves every seat that has moved, so read it with the count.
                int arrows = 0;
                for (int i = 0; i < g_cfg.seatCount; ++i) arrows += g_overlay[i].IsVisible() ? 1 : 0;
                LogLine(L"[perf] %d arrows shown (%d seats)  overlay per flush=%.1f us",
                        arrows, g_cfg.seatCount, g_perf.tOverlay * us / f);
                g_perf = PerfStats{};
                for (int i = 0; i < g_cfg.seatCount; ++i) {
                    AccelState& a = g_accel[i];
                    if (!a.n) continue;
                    LogLine(L"[perf] seat%d accel: moves=%llu  speed counts/ms: avg=%.2f max=%.2f"
                            L"  gain: avg=%.2f max=%.2f",
                            i, a.n, a.vSum / a.n, a.vMax, a.gSum / a.n, a.gMax);
                    a.n = 0; a.vSum = a.vMax = a.gSum = a.gMax = 0;
                }
            } else if (perfTick >= 8) {
                perfTick = 0;
                g_perf = PerfStats{};
            }
        }
        if (wp == 1) {
            // The system silently drops clip rects on some foreground
            // changes; reassert the cage wherever it was last placed. Placed,
            // not the active seat's pos — a scrolling seat legitimately holds
            // the cage between wheel ticks. Only relevant in cage mode; with
            // move-eating there is no clip rect to lose.
            if (g_captureActive && g_pinned) PinCursor(g_pinnedAt);

            // Self-blinding guard. Eating physical moves relies on
            // undocumented behaviour: for the mouse it leaves raw input
            // intact, but the keyboard equivalent silently suppresses raw
            // input and swallows the input stream whole. If that ever
            // happened here — a Windows update, a filter driver — the gate
            // would eat every move while the engine saw none, and both
            // pointers would freeze with no way to recover from inside.
            // So: many moves eaten and zero raw input in the same second
            // means we have blinded ourselves. Fall back to the cage.
            static uint64_t lastRaw = 0, lastEaten = 0;
            static int guardTick = 0;
            if (++guardTick >= 4) {                 // once a second
                guardTick = 0;
                const uint64_t raw   = g_rawMouse;
                const uint64_t eaten = g_gateEatenMoves.load();
                if (g_eatMoves && (eaten - lastEaten) > 20 && raw == lastRaw) {
                    g_eatMoves = false;
                    LogLine(L"[gate] move-eating suppressed raw input (%llu eaten, 0 received)"
                            L" — disabled, reverting to cursor cage",
                            (unsigned long long)(eaten - lastEaten));
                }
                lastRaw   = raw;
                lastEaten = eaten;

                // Other topmost windows can come above the arrows. Reassert
                // here rather than on the move path, and only when covered.
                ReassertArrows();
            }
        }
        if (wp == 1 && g_verbose) {
            // Logged unconditionally while verbose, even when idle: a silent
            // log cannot distinguish "no input arrived" from "instrumentation
            // is broken" — that ambiguity cost a whole debugging round.
            static int tick = 0;
            if (++tick >= 8) {              // every 2 seconds
                tick = 0;
                LogLine(L"[stat] active=%d  wm_input=%llu (mouse=%llu kbd=%llu)  dropped: null=%llu injected=%llu  unbound=%llu",
                        g_activeSeat,
                        (unsigned long long)g_wmInputTotal,
                        (unsigned long long)g_rawMouse,
                        (unsigned long long)g_rawKbd,
                        (unsigned long long)g_dropNullDev,
                        (unsigned long long)g_dropInjected,
                        (unsigned long long)g_unboundEvents);
                for (int i = 0; i < g_cfg.seatCount; ++i)
                    LogLine(L"       seat %d  mv=%llu btn=%llu setcursor=%llu overlay=%llu pos=%ld,%ld",
                            i, (unsigned long long)g_stats[i].moves,
                            (unsigned long long)g_stats[i].buttons,
                            (unsigned long long)g_stats[i].cursorSets,
                            (unsigned long long)g_stats[i].overlaySets,
                            g_seats[i].pos.x, g_seats[i].pos.y);
                POINT real{};
                GetCursorPos(&real);
                LogLine(L"       system cursor actually at %ld,%ld", real.x, real.y);
                for (const auto& kv : g_perDevice) {
                    auto s = g_mouseSeat.find(kv.first);
                    LogLine(L"       device %p -> seat %d  events=%llu", kv.first,
                            s != g_mouseSeat.end() ? s->second : -1,
                            (unsigned long long)kv.second);
                }
            }
        }
        return 0;
    }

    case WM_CLOSE:
        UnregisterCapture();
        RestoreSystemCursor();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- modes

int RunList() {
    const auto devices = EnumerateDevices();
    g_cfg.Load(Config::DefaultPath());
    LogLine(L"openMouse — input devices");
    LogLine(L"");
    for (const auto& d : devices) {
        LogLine(L"  %s  %s", d.type == RIM_TYPEMOUSE ? L"[mouse]   " : L"[keyboard]",
                d.friendlyName.c_str());
        LogLine(L"      %s", d.path.c_str());
        if (d.type == RIM_TYPEKEYBOARD && OwnerSeatOf(d.path) >= 0)
            LogLine(L"      interface of the mouse of seat %d (its buttons can be learned)", OwnerSeatOf(d.path));
    }
    // The media, system and manufacturer collections of the mice: what a
    // learned button can come from besides the five buttons of the mouse itself.
    for (const auto& d : EnumerateHidDevices()) {
        if (OwnerSeatOf(d.path) < 0) continue;
        LogLine(L"  [hid]       usage page %04X  %s", d.page, d.friendlyName.c_str());
        LogLine(L"      %s", d.path.c_str());
        LogLine(L"      interface of the mouse of seat %d (its buttons can be learned)", OwnerSeatOf(d.path));
    }
    LogLine(L"");
    LogLine(L"Config file: %s", Config::DefaultPath().c_str());
    return 0;
}

// The single most important thing to verify on a new machine: does SendInput
// still reach applications while a RIDEV_NOLEGACY registration is active?
// The whole design rests on the answer being yes. If this prints FAIL, no
// user-mode multi-pointer implementation is possible on this system.
int RunProbe() {
    LogLine(L"openMouse — SendInput / RIDEV_NOLEGACY compatibility probe");
    LogLine(L"");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"openMouse.Probe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LogLine(L"FAIL: could not create message window"); return 1; }

    POINT before{};
    GetCursorPos(&before);
    LogLine(L"  cursor before        : %ld,%ld", before.x, before.y);

    if (!RegisterCapture(hwnd, false)) {
        LogLine(L"FAIL: RIDEV_NOLEGACY registration rejected.");
        LogLine(L"      Check App Control / WDAC policy and any input filter drivers.");
        return 1;
    }
    LogLine(L"  NOLEGACY registered  : yes");

    Sleep(300);
    POINT target{ before.x + 120, before.y + 60 };
    ClampToVirtualScreen(target);
    SendMouseAt(target, MOUSEEVENTF_MOVE);
    Sleep(300);

    POINT after{};
    GetCursorPos(&after);
    LogLine(L"  cursor after SendInput: %ld,%ld  (wanted %ld,%ld)",
            after.x, after.y, target.x, target.y);

    const bool moved = (labs(after.x - target.x) <= 2 && labs(after.y - target.y) <= 2);
    UnregisterCapture();
    SetCursorPos(before.x, before.y);

    LogLine(L"");
    if (moved) {
        LogLine(L"  PASS — SendInput reaches the system while NOLEGACY is active.");
        LogLine(L"         The multi-pointer design is viable on this machine.");
        return 0;
    }
    LogLine(L"  FAIL — SendInput did not move the cursor under NOLEGACY.");
    LogLine(L"         User-mode multi-pointer is not possible here; a kernel");
    LogLine(L"         filter driver would be required instead.");
    return 2;
}

// ---------------------------------------------------------------- echo probe
//
// RunProbe answers "does SendInput still reach the system under NOLEGACY".
// It does NOT answer the question that actually decides whether the engine can
// run safely: does our own injected input come BACK to us as WM_INPUT?
//
// If it does, the engine feeds on its own output. A single click becomes
// inject -> WM_INPUT -> route to a seat -> inject -> ... forever, and the
// watchdog cannot save us because a storm keeps the heartbeat climbing. So
// this probe injects exactly once, never in response to input, and reports
// what came back and how it can be told apart from real hardware.

struct EchoStats {
    int total = 0, nullDev = 0, tagged = 0, moves = 0, buttons = 0;
    std::map<HANDLE, int> byDevice;
    ULONG_PTR sampleExtra = 0;
    bool sawExtra = false;
};

EchoStats g_echo;
bool      g_echoRecording = false;

LRESULT CALLBACK EchoWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT && g_echoRecording) {
        UINT size = 0;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER)) == 0 && size && size <= 4096) {
            BYTE buf[4096];
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size,
                                sizeof(RAWINPUTHEADER)) == size) {
                auto* ri = reinterpret_cast<RAWINPUT*>(buf);
                if (ri->header.dwType == RIM_TYPEMOUSE) {
                    const RAWMOUSE& rm = ri->data.mouse;
                    ++g_echo.total;
                    g_echo.byDevice[ri->header.hDevice]++;
                    if (ri->header.hDevice == nullptr)          ++g_echo.nullDev;
                    if (rm.ulExtraInformation == kInjectTag)    ++g_echo.tagged;
                    if (rm.usButtonFlags)                       ++g_echo.buttons;
                    else                                        ++g_echo.moves;
                    if (!g_echo.sawExtra) { g_echo.sampleExtra = rm.ulExtraInformation; g_echo.sawExtra = true; }
                }
            }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void PumpFor(DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    MSG msg;
    while ((int)(GetTickCount() - end) < 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(2);
    }
}

int RunProbeEcho() {
    LogLine(L"openMouse — injection feedback probe");
    LogLine(L"");
    LogLine(L"  Question: does input this process injects come back to it as WM_INPUT?");
    LogLine(L"  If yes, the engine must filter its own injections or it will loop.");
    LogLine(L"");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = EchoWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"openMouse.EchoProbe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LogLine(L"FAIL: could not create message window"); return 1; }

    POINT before{};
    GetCursorPos(&before);

    if (!RegisterCapture(hwnd, false)) {
        LogLine(L"FAIL: RIDEV_NOLEGACY registration rejected.");
        return 1;
    }

    // Step 1 — the decisive test. Inject one move and see whether it returns.
    // Deliberately a pure MOUSEEVENTF_MOVE and no buttons: a synthetic click
    // would land on whatever window happens to be under the cursor.
    LogLine(L"  [1/3] Injecting ONE SendInput mouse move (tagged %08llX)...",
            (unsigned long long)kInjectTag);
    g_echo = EchoStats{};
    g_echoRecording = true;
    POINT target{ before.x + 40, before.y + 20 };
    ClampToVirtualScreen(target);
    SendMouseAt(target, MOUSEEVENTF_MOVE);
    PumpFor(700);
    g_echoRecording = false;
    const int  afterMove   = g_echo.total;
    const int  moveTagged  = g_echo.tagged;
    const int  moveNullDev = g_echo.nullDev;
    LogLine(L"        echoed events: %d   hDevice==NULL: %d   tag matched: %d",
            afterMove, moveNullDev, moveTagged);
    if (g_echo.sawExtra)
        LogLine(L"        first ulExtraInformation seen: %08llX",
                (unsigned long long)g_echo.sampleExtra);
    for (const auto& kv : g_echo.byDevice)
        LogLine(L"          hDevice=%p  events=%d", kv.first, kv.second);

    // Step 2 — SetCursorPos runs on every move of the active seat, so whether
    // it echoes matters as much as SendInput.
    LogLine(L"  [2/3] SetCursorPos (runs on every active-seat move)...");
    g_echo = EchoStats{};
    g_echoRecording = true;
    SetCursorPos(before.x, before.y);
    PumpFor(700);
    g_echoRecording = false;
    const int afterSetPos = g_echo.total;
    LogLine(L"        echoed events: %d   tag matched: %d", afterSetPos, g_echo.tagged);

    // Step 3 — only needed to disambiguate a negative result. If nothing
    // echoed, we must rule out "this window receives no raw input at all"
    // before concluding "injection does not feed back".
    int baseline = -1;
    if (afterMove == 0 && afterSetPos == 0) {
        LogLine(L"  [3/3] Nothing echoed. Confirming the window is not simply deaf —");
        LogLine(L"        MOVE A MOUSE NOW, 4 seconds...");
        g_echo = EchoStats{};
        g_echoRecording = true;
        PumpFor(4000);
        g_echoRecording = false;
        baseline = g_echo.total;
        LogLine(L"        real hardware events: %d   distinct devices: %llu",
                baseline, (unsigned long long)g_echo.byDevice.size());
        for (const auto& kv : g_echo.byDevice)
            LogLine(L"          hDevice=%p  events=%d", kv.first, kv.second);
    } else {
        LogLine(L"  [3/3] Skipped — injection already echoed, window is clearly live.");
    }

    UnregisterCapture();
    SetCursorPos(before.x, before.y);

    LogLine(L"");
    LogLine(L"  ---------------- verdict ----------------");

    if (afterMove > 0 || afterSetPos > 0) {
        LogLine(L"  FEEDBACK CONFIRMED — this process sees its own injected input.");
        LogLine(L"    SendInput echo=%d   SetCursorPos echo=%d", afterMove, afterSetPos);
        LogLine(L"    Unfiltered, the engine loops on the first click: inject ->");
        LogLine(L"    WM_INPUT -> route to seat -> inject -> ... The watchdog cannot");
        LogLine(L"    break it, because the storm keeps the heartbeat climbing.");
        LogLine(L"");
        if (afterMove > 0 && moveTagged == afterMove) {
            LogLine(L"    dwExtraInfo tagging is RELIABLE (%d/%d) — filter on",
                    moveTagged, afterMove);
            LogLine(L"    RAWMOUSE.ulExtraInformation == kInjectTag.");
            return 10;
        }
        LogLine(L"    dwExtraInfo tagging covered only %d of %d echoed events.",
                moveTagged, afterMove);
        LogLine(L"    Filter on hDevice==NULL as well (%d/%d were NULL).",
                moveNullDev, afterMove);
        return 11;
    }

    if (baseline == 0) {
        LogLine(L"  INCONCLUSIVE — no injected echo AND no hardware events.");
        LogLine(L"    Cannot tell 'injection does not feed back' apart from");
        LogLine(L"    'this message-only window receives no raw input'. Re-run");
        LogLine(L"    and move a mouse when prompted.");
        return 3;
    }

    LogLine(L"  NO FEEDBACK — the window receives real hardware input (%d events)", baseline);
    LogLine(L"    but not its own injections. No injection filter is required.");
    return 0;
}

// ---------------------------------------------------------------- identify
//
// This machine enumerates four HID mouse interfaces and five keyboard
// interfaces for two physical mice and two physical keyboards — Logitech
// receivers expose several collections each. ApplyBindings assigns seats in
// raw-enumeration order, so it binds seat 0 and seat 1 to whichever interfaces
// happen to come first, and silently drops the rest onto seat 0. Two physical
// mice can therefore end up driving the same pointer.
//
// The only reliable way to map a physical device to its interface path is to
// ask which handle produces events when that device alone is moved. Runs
// WITHOUT RIDEV_NOLEGACY, so the machine stays completely usable throughout.

std::map<HANDLE, int> g_idMouse, g_idKeyboard;
bool                  g_idRecording = false;

LRESULT CALLBACK IdentifyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT && g_idRecording) {
        UINT size = 0;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER)) == 0 && size && size <= 4096) {
            BYTE buf[4096];
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size,
                                sizeof(RAWINPUTHEADER)) == size) {
                auto* ri = reinterpret_cast<RAWINPUT*>(buf);
                if (ri->header.hDevice) {
                    if (ri->header.dwType == RIM_TYPEMOUSE) {
                        const RAWMOUSE& rm = ri->data.mouse;
                        if (rm.lLastX || rm.lLastY || rm.usButtonFlags)
                            g_idMouse[ri->header.hDevice]++;
                    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                        if (!(ri->data.keyboard.Flags & RI_KEY_BREAK))
                            g_idKeyboard[ri->header.hDevice]++;
                    }
                }
            }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HANDLE g_conIn = INVALID_HANDLE_VALUE;

// Pump the message loop while waiting for Enter, so raw input keeps arriving.
// A blocking console read would starve WM_INPUT and record nothing.
bool PumpUntilEnter(DWORD timeoutMs) {
    const DWORD end = GetTickCount() + timeoutMs;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_conIn != INVALID_HANDLE_VALUE) {
            DWORD pending = 0;
            if (GetNumberOfConsoleInputEvents(g_conIn, &pending) && pending) {
                INPUT_RECORD rec[64];
                DWORD got = 0;
                if (ReadConsoleInputW(g_conIn, rec, pending < 64 ? pending : 64, &got)) {
                    for (DWORD i = 0; i < got; ++i)
                        if (rec[i].EventType == KEY_EVENT &&
                            rec[i].Event.KeyEvent.bKeyDown &&
                            rec[i].Event.KeyEvent.wVirtualKeyCode == VK_RETURN)
                            return true;
                }
            }
        }
        if ((int)(GetTickCount() - end) >= 0) return false;
        Sleep(5);
    }
}

// Returns the handle with the most events, provided it is a clear winner:
// at least kMinEvents total and at least twice the runner-up. Anything less
// means the user moved more than one device and we must not guess.
HANDLE DominantDevice(const std::map<HANDLE, int>& tally, int& outCount, int& outRunnerUp) {
    HANDLE best = nullptr;
    int bestN = 0, secondN = 0;
    for (const auto& kv : tally) {
        if (kv.second > bestN)      { secondN = bestN; bestN = kv.second; best = kv.first; }
        else if (kv.second > secondN) secondN = kv.second;
    }
    outCount = bestN;
    outRunnerUp = secondN;
    return best;
}

// Reports every interface that produces events, not just the dominant one.
// --identify only names its winner, which hides the case that matters here: a
// single physical mouse emitting on two HID collections at once. The second
// collection is not bound to any seat, so it falls through to seat 0 and drags
// the system cursor along with the seat's own pointer.
int RunWatch() {
    LogLine(L"openMouse — raw device watch");
    LogLine(L"");
    LogLine(L"  Observe-only: nothing is suppressed, input works normally.");
    LogLine(L"");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = IdentifyWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"openMouse.Watch";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LogLine(L"FAIL: could not create message window"); return 1; }

    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_INPUTSINK; rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        LogLine(L"FAIL: RegisterRawInputDevices failed: %lu", GetLastError());
        return 1;
    }

    g_cfg.Load(Config::DefaultPath());

    LogLine(L"  Move ONLY mouse 2 — the one whose pointer drags the other —");
    LogLine(L"  in wide circles for the next 12 seconds. Starting now.");
    g_idMouse.clear();
    g_idKeyboard.clear();
    g_idRecording = true;
    PumpFor(12000);
    g_idRecording = false;

    rid[0].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = nullptr;
    rid[1].dwFlags = RIDEV_REMOVE; rid[1].hwndTarget = nullptr;
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));

    LogLine(L"");
    LogLine(L"  ---- mouse interfaces that produced events ----");
    if (g_idMouse.empty()) LogLine(L"  (none — was the mouse moved?)");
    for (const auto& kv : g_idMouse) {
        const std::wstring path = DevicePathFromHandle(kv.first);
        auto bound = g_cfg.mouseBinding.find(path);
        LogLine(L"  %6d events   seat %s", kv.second,
                bound != g_cfg.mouseBinding.end()
                    ? std::to_wstring(bound->second).c_str()
                    : L"UNBOUND -> falls through to seat 0");
        LogLine(L"                 %s", path.c_str());
    }
    LogLine(L"");
    if (g_idMouse.size() > 1) {
        LogLine(L"  DIAGNOSIS: one physical mouse is emitting on %llu interfaces.",
                (unsigned long long)g_idMouse.size());
        LogLine(L"  Any of them not bound to this seat drives seat 0 as well,");
        LogLine(L"  which is why moving it drags the other pointer along.");
    } else {
        LogLine(L"  Only one interface emitted — the drag has another cause.");
    }
    LogLine(L"");
    LogLine(L"  This window closes in 20 seconds.");
    Sleep(20000);
    return 0;
}

int RunIdentify() {
    constexpr int kMinEvents = 15;

    g_cfg.Load(Config::DefaultPath());

    LogLine(L"openMouse — device identification");
    LogLine(L"");
    LogLine(L"  This machine reports more HID interfaces than it has physical");
    LogLine(L"  devices, so seats must be bound by observation rather than by");
    LogLine(L"  enumeration order.");
    LogLine(L"");
    LogLine(L"  Normal input is NOT suppressed during this step — the mouse and");
    LogLine(L"  keyboard keep working as usual.");
    LogLine(L"");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = IdentifyWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"openMouse.Identify";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LogLine(L"FAIL: could not create message window"); return 1; }

    // Observe only: INPUTSINK without NOLEGACY. Nothing is suppressed.
    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_INPUTSINK; rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        LogLine(L"FAIL: RegisterRawInputDevices failed: %lu", GetLastError());
        return 1;
    }

    g_conIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    if (g_conIn != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_conIn, ENABLE_PROCESSED_INPUT);
        FlushConsoleInputBuffer(g_conIn);
    }

    std::map<std::wstring, int> newMouse, newKeyboard;
    bool ok = true;

    for (int seat = 0; seat < g_cfg.seatCount && ok; ++seat) {
        // ---- mouse
        LogLine(L"  ---- SEAT %d ----", seat);
        LogLine(L"  Move ONLY the mouse for seat %d in circles, then press Enter.", seat);
        g_idMouse.clear();
        g_idRecording = true;
        const bool gotEnterM = PumpUntilEnter(120000);
        g_idRecording = false;
        if (!gotEnterM) { LogLine(L"  timed out waiting for Enter"); ok = false; break; }

        int n = 0, runner = 0;
        HANDLE dev = DominantDevice(g_idMouse, n, runner);
        if (!dev || n < kMinEvents) {
            LogLine(L"  Not enough movement recorded (%d events). Move the mouse more.", n);
            ok = false; break;
        }
        if (runner * 2 > n) {
            LogLine(L"  Ambiguous: top device %d events, runner-up %d. Move ONE mouse only.",
                    n, runner);
            ok = false; break;
        }
        std::wstring path = DevicePathFromHandle(dev);
        LogLine(L"  -> mouse    %d events  %s", n, FriendlyNameFromPath(path).c_str());
        LogLine(L"     %s", path.c_str());
        if (newMouse.count(path)) {
            LogLine(L"  That is the same mouse already bound to another seat.");
            ok = false; break;
        }
        newMouse[path] = seat;

        // ---- keyboard
        LogLine(L"  Now type on ONLY the keyboard for seat %d, then press Enter.", seat);
        g_idKeyboard.clear();
        g_idRecording = true;
        const bool gotEnterK = PumpUntilEnter(120000);
        g_idRecording = false;
        if (!gotEnterK) { LogLine(L"  timed out waiting for Enter"); ok = false; break; }

        // The Enter keypress itself lands on this keyboard, which is fine —
        // it is one more vote for the right device.
        int kn = 0, krunner = 0;
        HANDLE kdev = DominantDevice(g_idKeyboard, kn, krunner);
        if (!kdev || kn < 3) {
            LogLine(L"  Not enough typing recorded (%d events). Skipping keyboard for this seat.", kn);
        } else if (krunner * 2 > kn) {
            LogLine(L"  Ambiguous keyboard: top %d, runner-up %d. Skipping.", kn, krunner);
        } else {
            std::wstring kpath = DevicePathFromHandle(kdev);
            LogLine(L"  -> keyboard %d events  %s", kn, FriendlyNameFromPath(kpath).c_str());
            LogLine(L"     %s", kpath.c_str());
            if (newKeyboard.count(kpath)) LogLine(L"  Already bound to another seat — skipping.");
            else newKeyboard[kpath] = seat;
        }
        LogLine(L"");
    }

    rid[0].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = nullptr;
    rid[1].dwFlags = RIDEV_REMOVE; rid[1].hwndTarget = nullptr;
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
    if (g_conIn != INVALID_HANDLE_VALUE) CloseHandle(g_conIn);
    g_conIn = INVALID_HANDLE_VALUE;

    if (!ok) {
        LogLine(L"  Aborted — configuration NOT changed.");
        return 2;
    }

    // Every interface we did not positively identify is left unbound rather
    // than defaulted to seat 0, so a stray collection cannot drive a pointer.
    g_cfg.mouseBinding    = newMouse;
    g_cfg.keyboardBinding = newKeyboard;
    if (!g_cfg.Save(Config::DefaultPath())) {
        LogLine(L"  FAIL: could not write %s", Config::DefaultPath().c_str());
        return 1;
    }
    LogLine(L"  Saved %llu mouse and %llu keyboard bindings to",
            (unsigned long long)newMouse.size(), (unsigned long long)newKeyboard.size());
    LogLine(L"  %s", Config::DefaultPath().c_str());
    LogLine(L"");
    LogLine(L"  Done. This window closes in 15 seconds.");
    Sleep(15000);
    return 0;
}

// Seconds after which the engine shuts itself down, or 0 for no limit. A
// bounded run is the difference between "try it and see" and "try it and hope"
// when the thing under test owns every mouse on the machine.
int g_runSeconds = 0;

// If the process dies with an unhandled exception it takes a system-wide
// input capture, a hidden cursor and a 1px cursor cage down with it. Undo
// all three before Windows Error Reporting gets the corpse.
LONG WINAPI EmergencyRestore(EXCEPTION_POINTERS*) {
    StopGateThread();          // physical clicks must flow again
    UnregisterCapture();       // also releases the cage
    RestoreSystemCursor();
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- receiver
//
// A visible topmost window that logs every mouse message it receives — the
// app's-eye view of what the engine delivers. Run alongside the engine and
// click into it; the diff between the engine's [inj] lines and this log
// localises any fault to a layer.

HANDLE g_recvLog = INVALID_HANDLE_VALUE;

void RecvLine(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    if (g_recvLog != INVALID_HANDLE_VALUE) {
        char utf8[1024];
        const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, 1020, nullptr, nullptr);
        if (n > 1) {
            DWORD w = 0;
            WriteFile(g_recvLog, utf8, n - 1, &w, nullptr);
            WriteFile(g_recvLog, "\r\n", 2, &w, nullptr);
        }
    }
}

LRESULT CALLBACK ReceiverWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static POINT lastMove{ -32768, -32768 };
    const int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    switch (msg) {
    case WM_LBUTTONDOWN:   RecvLine(L"LDOWN    t=%lu  %d,%d", GetTickCount(), x, y); return 0;
    case WM_LBUTTONUP:     RecvLine(L"LUP      t=%lu  %d,%d", GetTickCount(), x, y); return 0;
    case WM_LBUTTONDBLCLK: RecvLine(L"LDBLCLK  t=%lu  %d,%d", GetTickCount(), x, y); return 0;
    case WM_RBUTTONDOWN:   RecvLine(L"RDOWN    t=%lu  %d,%d", GetTickCount(), x, y); return 0;
    case WM_RBUTTONUP:     RecvLine(L"RUP      t=%lu  %d,%d", GetTickCount(), x, y); return 0;
    case WM_MOUSEWHEEL:    RecvLine(L"WHEEL    t=%lu  delta=%d", GetTickCount(), GET_WHEEL_DELTA_WPARAM(wp)); return 0;
    case WM_MOUSEACTIVATE: RecvLine(L"MOUSEACTIVATE t=%lu", GetTickCount()); break;
    case WM_SETFOCUS:      RecvLine(L"SETFOCUS t=%lu", GetTickCount()); break;
    case WM_KILLFOCUS:     RecvLine(L"KILLFOCUS t=%lu", GetTickCount()); break;
    case WM_MOUSEMOVE:
        if (x != lastMove.x || y != lastMove.y) {
            RecvLine(L"MOVE     t=%lu  %d,%d", GetTickCount(), x, y);
            lastMove = { x, y };
        }
        return 0;
    case WM_DESTROY:       PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int RunReceiver() {
    std::wstring path = Config::DefaultPath();
    const size_t slash = path.rfind(L'\\');
    path = (slash == std::wstring::npos) ? std::wstring(L"receiver.log")
                                         : path.substr(0, slash + 1) + L"receiver.log";
    g_recvLog = OpenSafeFile(path, SafeOpen::Rewrite);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ReceiverWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"openMouse.ClickReceiver";     // deliberately no CS_DBLCLKS
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName,
                                L"openMouse click test — click in here",
                                WS_OVERLAPPEDWINDOW, 600, 250, 500, 400,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { RecvLine(L"FAIL CreateWindowEx err=%lu", GetLastError()); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    RecvLine(L"receiver up t=%lu", GetTickCount());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RecvLine(L"receiver closed t=%lu", GetTickCount());
    CloseHandle(g_recvLog);
    return 0;
}

// ---------------------------------------------------------------- monitor
//
// WH_MOUSE_LL sees every button event entering the legacy pointer pipeline,
// system-wide, with provenance: LLMHF_INJECTED plus our kInjectTag in
// dwExtraInfo separate hardware events, engine injections, and foreign
// injections. If a physical click ever reaches this hook UN-injected while
// the engine holds NOLEGACY, legacy suppression is leaking and every click
// doubles — the prime suspect for "double click selects a paragraph".

HANDLE g_monLog = INVALID_HANDLE_VALUE;

LRESULT CALLBACK MonitorHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && lp) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        const wchar_t* name = nullptr;
        switch (wp) {
        case WM_LBUTTONDOWN: name = L"LDOWN"; break;
        case WM_LBUTTONUP:   name = L"LUP  "; break;
        case WM_RBUTTONDOWN: name = L"RDOWN"; break;
        case WM_RBUTTONUP:   name = L"RUP  "; break;
        case WM_MOUSEWHEEL:  name = L"WHEEL"; break;
        }
        if (name && g_monLog != INVALID_HANDLE_VALUE) {
            wchar_t buf[256];
            _snwprintf(buf, 255, L"%s t=%lu pos=%ld,%ld injected=%d lowerIL=%d tag=%s\r\n",
                       name, m->time, m->pt.x, m->pt.y,
                       (m->flags & LLMHF_INJECTED) ? 1 : 0,
                       (m->flags & LLMHF_LOWER_IL_INJECTED) ? 1 : 0,
                       m->dwExtraInfo == kInjectTag ? L"ENGINE" : L"none");
            char utf8[512];
            const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, 508, nullptr, nullptr);
            if (n > 1) { DWORD w = 0; WriteFile(g_monLog, utf8, n - 1, &w, nullptr); }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

int RunMonitor() {
    std::wstring path = Config::DefaultPath();
    const size_t slash = path.rfind(L'\\');
    path = (slash == std::wstring::npos) ? std::wstring(L"monitor.log")
                                         : path.substr(0, slash + 1) + L"monitor.log";
    g_monLog = OpenSafeFile(path, SafeOpen::Rewrite);

    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MonitorHook, GetModuleHandleW(nullptr), 0);
    if (!hook) { LogLine(L"[monitor] SetWindowsHookEx failed: %lu", GetLastError()); return 1; }
    LogLine(L"[monitor] running — logging all system button events");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(hook);
    CloseHandle(g_monLog);
    return 0;
}

// ---------------------------------------------------------------- janitor
//
// Documentation research turned up two undoing-gaps that in-process cleanup
// cannot close: a ClipCursor rect SURVIVES the death of the process that set
// it (measured: pointer stays welded to the 1px cage after a hard kill until
// some focus change clears it — which the user cannot click to cause), and
// MagShowSystemCursor has no documented crash restoration either. Exit paths
// and exception filters never run under TerminateProcess / Task Manager.
//
// So the engine spawns a janitor of itself: a tiny process that just waits on
// the engine's handle and, when the engine dies FOR ANY REASON, releases the
// cage and re-shows the cursor. Belt for every suspender.

int RunJanitor(DWORD parentPid) {
    OpenLogFile();
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!h) return 1;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);

    ClipCursor(nullptr);
    if (g_mag.Load() && g_mag.Initialize && g_mag.Initialize()) {
        if (g_mag.ShowSystemCursor) g_mag.ShowSystemCursor(TRUE);
        if (g_mag.Uninitialize)     g_mag.Uninitialize();
    }
    LogLine(L"[janitor] engine pid %lu died — cage released, cursor restored", parentPid);
    return 0;
}

void SpawnJanitor() {
    const std::wstring exe = SelfPath();
    if (exe.empty()) return;
    std::wstring cmd = L"\"" + exe + L"\" --janitor=" + std::to_wstring(GetCurrentProcessId());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LogLine(L"[janitor] spawned (pid %lu watches this engine)", pi.dwProcessId);
    } else {
        LogLine(L"[janitor] spawn failed: %lu — hard kills will leave the cage behind", GetLastError());
    }
}

// ---------------------------------------------------------------- tray

constexpr UINT kTrayCmdStop     = 100;
constexpr UINT kTrayCmdLog      = 102;
constexpr UINT kTrayCmdAbout    = 103;
constexpr UINT kTrayCmdSettings = 104;

// Relaunch ourselves in another mode. Used by the tray menu.
void RelaunchSelf(const wchar_t* args) {
    const std::wstring exe = SelfPath();
    if (exe.empty()) return;
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void TrayAdd(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = kTrayIconId;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    // The tray draws at the small-icon size; asking for exactly that picks the
    // matching frame of the .ico instead of a blurry rescale.
    nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(kAppIconResource), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                  LR_SHARED);
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, UiFrench()
        ? L"openMouse : clic pour les réglages ou l'arrêt (urgence : Ctrl+Alt+Maj+Q)"
        : L"openMouse — click for settings or to stop (panic: Ctrl+Alt+Shift+Q)");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

// Without administrator rights Windows keeps the input aimed at elevated
// windows (Task Manager...) away from us: say so, once, where the user looks.
void TrayWarnNotElevated(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize     = sizeof(nid);
    nid.hWnd       = hwnd;
    nid.uID        = kTrayIconId;
    nid.uFlags     = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    const bool fr = UiFrench();
    wcscpy_s(nid.szInfoTitle, fr ? L"openMouse sans droits d'administrateur"
                                 : L"openMouse without administrator rights");
    wcscpy_s(nid.szInfo, fr
        ? L"Le Gestionnaire des tâches et les fenêtres lancées en administrateur échapperont aux souris. Relancez openMouse en acceptant la demande d'autorisation."
        : L"Task Manager and windows started as administrator will escape the mice. Start openMouse again and accept the permission prompt.");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayRemove(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void TrayMenu(HWND hwnd) {
    const bool fr = UiFrench();
    wchar_t header[64];
    if (fr) swprintf(header, 64, L"openMouse : %d souris%s", g_cfg.seatCount,
                     g_cfg.captureKeyboards ? L", claviers dirigés" : L"");
    else    swprintf(header, 64, L"openMouse — %d seats%s", g_cfg.seatCount,
                     g_cfg.captureKeyboards ? L", keyboards steered" : L"");

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DISABLED, kTrayCmdAbout, header);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCmdSettings, fr ? L"Réglages…" : L"Settings…");
    AppendMenuW(menu, MF_STRING, kTrayCmdLog,      fr ? L"Ouvrir le dossier du journal" : L"Open log folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCmdStop,     fr ? L"Arrêter openMouse\tCtrl+Alt+Maj+Q"
                                                      : L"Stop openMouse\tCtrl+Alt+Shift+Q");
    SetMenuDefaultItem(menu, kTrayCmdSettings, FALSE);

    POINT pt{};
    GetCursorPos(&pt);
    // TrackPopupMenu on a tray icon needs the window foreground, or the menu
    // refuses to dismiss when the user clicks away. Documented quirk.
    SetForegroundWindow(hwnd);
    const UINT cmd = (UINT)TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
    case kTrayCmdSettings:
        // A process of its own, so the window never shares this thread with
        // the input path. Let it come to the front: we hold the foreground.
        LogLine(L"[tray] settings requested");
        AllowSetForegroundWindow(ASFW_ANY);
        RelaunchSelf(L"--settings");
        break;
    case kTrayCmdStop:
        LogLine(L"[tray] stop requested");
        RequestExit();
        break;
    case kTrayCmdLog: {
        std::wstring dir = Config::DefaultPath();
        const size_t slash = dir.rfind(L'\\');
        if (slash != std::wstring::npos) dir = dir.substr(0, slash);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    }
}

int RunMain(HINSTANCE inst) {
    SetUnhandledExceptionFilter(EmergencyRestore);

    // OP16: the system cursor is serviced ahead of ordinary scheduling, while
    // the arrows are drawn by this process and lose the CPU when other
    // processes saturate it (the arrows froze for seconds under load). Run
    // above normal, and the input thread (this one) above the rest of the
    // process. HIGH, not REALTIME: it never starves the system.
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        LogLine(L"[prio] SetPriorityClass failed: %lu", GetLastError());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // A clip rect survives the death of the process that set it, so a
    // previous instance killed hard enough to outrun its janitor can leave
    // the pointer fenced into one pixel. Clear anything stale before doing
    // anything else — this build normally holds no clip at all.
    ClipCursor(nullptr);

    // The logon task must never leave two engines on the machine, e.g. when
    // one was started by hand before the task fired.
    if (g_startup && FindWindowW(L"openMouse.Engine", nullptr)) {
        LogLine(L"[startup] an engine is already running — nothing to do");
        return 0;
    }

    g_inst = inst;
    g_cfg.Load(Config::DefaultPath());
    if (g_startup) WaitForDesktop();
    std::thread([] { g_autoState = AutostartState(); }).detach();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = L"openMouse.Engine";
    RegisterClassExW(&wc);

    // A hidden NORMAL window, not a message-only one: the tray icon needs a
    // window that can receive shell callbacks, and message-only windows
    // cannot. Never shown; WS_EX_TOOLWINDOW keeps it out of Alt-Tab.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"openMouse",
                             WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) { LogLine(L"[fatal] could not create engine window"); return 1; }
    // The settings window may run without elevation while the engine is
    // elevated; let its reload request through the privilege filter. All it
    // does is make us re-read a file that window could write anyway.
    ChangeWindowMessageFilterEx(g_hwnd, kReloadSettingsMsg, MSGFLT_ALLOW, nullptr);
    // Same for the start-with-Windows request. It can only make this program
    // create or delete the one task that starts this same executable.
    ChangeWindowMessageFilterEx(g_hwnd, kAutostartMsg, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, kLearnMsg, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, kSeatsMsg, MSGFLT_ALLOW, nullptr);   // only makes us answer

    // Told when the session locks or unlocks, to take the input back on return.
    if (!WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION))
        LogLine(L"[session] no lock/unlock notifications: %lu", GetLastError());

    ApplyBindings();
    ResolveSeatBounds();

    // Seed every seat at the current cursor, then fan them out so they are not
    // stacked on top of each other at startup.
    POINT start{};
    GetCursorPos(&start);
    for (int i = 0; i < g_cfg.seatCount; ++i)
        InitSeat(i, { start.x + i * 40, start.y + i * 24 });
    g_activeSeat = FirstPresentSeat();   // the arrows of absent mice start hidden
    if (g_activeSeat < 0) g_activeSeat = 0;

    // Decide this before showing the overlays: if the hardware cursor can be
    // hidden, every seat draws itself; if not, seat 0 keeps the system cursor
    // and we fall back to snapping it back after stray movement.
    const bool hidden = HideSystemCursor();
    if (!hidden)
        LogLine(L"[cursor] could not hide — falling back to snap-back correction (expect jitter)");
    // A seat whose mouse is not plugged in has no arrow until it is. With no
    // seat present at all, the first one keeps its arrow.
    const bool anyPresent = FirstPresentSeat() >= 0;
    for (int i = 0; i < g_cfg.seatCount; ++i)
        g_overlay[i].Show((hidden || i != g_activeSeat) &&
                          (anyPresent ? g_seatPresent[i] : i == 0));

    if (!RegisterCapture(g_hwnd, g_cfg.captureKeyboards)) {
        RestoreSystemCursor();   // never leave a desktop without a pointer
        return 1;
    }

    // The gate is what stops physical clicks reaching apps alongside our
    // injections. Without it every click is delivered twice, so its failure
    // is fatal, not degraded.
    std::thread gate(GateThreadProc);
    Sleep(100);
    if (!g_mouseGate) {
        LogLine(L"[fatal] mouse gate failed — refusing to run with doubled clicks");
        UnregisterCapture();
        if (g_gateThreadId) PostThreadMessageW(g_gateThreadId, WM_QUIT, 0, 0);
        gate.join();
        RestoreSystemCursor();
        return 1;
    }

    TrayAdd(g_hwnd);
    SpawnJanitor();
    if (!IsElevated()) {
        LogLine(L"[privileges] running WITHOUT administrator rights: Task Manager and any window "
                L"started as administrator will not follow the mice (their input is filtered out)");
        TrayWarnNotElevated(g_hwnd);
    } else {
        LogLine(L"[privileges] running with administrator rights");
    }

    LogLine(L"openMouse running — %d seats, keyboard capture %s",
            g_cfg.seatCount, g_cfg.captureKeyboards ? L"ON" : L"off");
    LogLine(L"Panic key: Ctrl+Alt+Shift+Q");
    for (int i = 0; i < g_cfg.seatCount; ++i)
        LogLine(L"  seat %d start pos %ld,%ld", i, g_seats[i].pos.x, g_seats[i].pos.y);

    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart; }
    SetTimer(g_hwnd, 1, 250, nullptr);

    if (g_runSeconds > 0) {
        LogLine(L"Auto-exit in %d seconds.", g_runSeconds);
        SetTimer(g_hwnd, 2, (UINT)g_runSeconds * 1000, nullptr);
    }

    std::thread watchdog(WatchdogThread);

    g_menuTimerWnd = g_hwnd;
    HWINEVENTHOOK menuHook = SetWinEventHook(EVENT_SYSTEM_MENUPOPUPSTART, EVENT_SYSTEM_MENUPOPUPEND,
                                             nullptr, MenuPopupEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!menuHook) LogLine(L"[menu] SetWinEventHook failed: %lu", GetLastError());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (menuHook) UnhookWinEvent(menuHook);
    g_menuTimerWnd = nullptr;
    g_running = false;
    watchdog.join();
    StopGateThread();
    gate.join();
    WTSUnRegisterSessionNotification(g_hwnd);
    TrayRemove(g_hwnd);
    UnregisterCapture();
    RestoreSystemCursor();
    for (int i = 0; i < kMaxSeats; ++i) g_overlay[i].Destroy();
    return 0;
}

// Recovery hatch. If the engine is ever killed hard enough to skip its exit
// path, the desktop is left with an invisible pointer and this is the way back.
int RunRestoreCursor() {
    LogLine(L"openMouse — restoring the system cursor");
    if (!g_mag.Load())       { LogLine(L"  magnification.dll unavailable"); return 1; }
    if (!g_mag.Initialize()) { LogLine(L"  MagInitialize failed: %lu", GetLastError()); return 1; }
    g_magReady = true;
    const BOOL ok = g_mag.ShowSystemCursor(TRUE);
    if (g_mag.Uninitialize) g_mag.Uninitialize();
    LogLine(ok ? L"  cursor restored." : L"  MagShowSystemCursor(TRUE) failed.");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------- elevation
//
// Windows isolates integrity levels (UIPI): an engine at the ordinary level
// receives no input aimed at an elevated window (Task Manager, anything started
// "as administrator") and cannot draw over or steer it. The arrows then freeze
// while the hidden real cursor keeps moving. So the engine wants to run
// elevated, and asks for it itself rather than through the manifest: a
// requireAdministrator manifest would also put a prompt in front of --help,
// --list and the settings window, which have no need of it.

// Start this same program again through the "runas" verb, with the same
// arguments plus --elevated so the copy never asks a second time. True when
// the elevated copy is on its way and this one should simply leave.
bool RelaunchElevated(const std::wstring& args) {
    const std::wstring exe = SelfPath();
    if (exe.empty()) return false;
    const std::wstring params = args.empty() ? L"--elevated" : args + L" --elevated";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOASYNC;
    sei.lpVerb       = L"runas";
    sei.lpFile       = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow        = SW_HIDE;
    if (ShellExecuteExW(&sei)) {
        LogLine(L"[privileges] not elevated — started an administrator copy, this one leaves");
        return true;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_CANCELLED)
        LogLine(L"[privileges] administrator rights were refused — running without them");
    else
        LogLine(L"[privileges] could not start an administrator copy (%lu) — running without rights", err);
    return false;
}

} // namespace
} // namespace om

// ---------------------------------------------------------------- entry

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // Per-monitor DPI awareness keeps pointer coordinates honest on mixed-DPI
    // setups. Resolved dynamically so the binary still runs on older builds.
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtxFn = BOOL (WINAPI*)(HANDLE);
        if (auto fn = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
            fn((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    std::wstring passArgs;   // the command line again, to hand to an elevated copy
    bool noElevate = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a != L"--no-elevate" && a != L"--elevated")
            passArgs += (passArgs.empty() ? L"\"" : L" \"") + a + L"\"";
        if (a == L"--list" || a == L"--probe" || a == L"--probe-echo" ||
            a == L"--identify" || a == L"--watch" || a == L"--restore-cursor" ||
            a == L"--receiver" || a == L"--monitor" || a == L"--settings" ||
            a == L"--help") mode = a;
        else if (a == L"--verbose")
            om::g_verbose = true;
        else if (a == L"--perf")
            om::g_perfLog = true;
        else if (a == L"--cage")
            om::g_eatMoves = false;   // force the old ClipCursor behaviour
        else if (a == L"--startup")
            om::g_startup = true;     // started by the logon task
        else if (a == L"--no-elevate")
            noElevate = true;         // run at the user's own level, never ask for more
        else if (a == L"--elevated")
            noElevate = true;         // this is already the administrator copy
        else if (a == L"--autostart=on" || a == L"--autostart=off" || a == L"--autostart=dry-run")
            mode = a;
        else if (a.rfind(L"--seconds=", 0) == 0)
            om::g_runSeconds = _wtoi(a.c_str() + 10);
        else if (a.rfind(L"--janitor=", 0) == 0)
            return om::RunJanitor((DWORD)_wtoi(a.c_str() + 10));
    }
    LocalFree(argv);

    om::OpenLogFile();

    // A window, not a console tool: no console to attach to or allocate.
    if (mode == L"--settings") return om::RunSettings(inst);
    if (mode == L"--autostart=on" || mode == L"--autostart=off")
        return om::RunAutostartHelper(mode == L"--autostart=on");

    if (mode == L"--identify" || mode == L"--watch") {
        // Needs its own visible window: it is a dialogue with the person at
        // the keyboard, not something to be captured through a pipe.
        AllocConsole();
        SetConsoleTitleW(mode == L"--watch" ? L"openMouse — device watch"
                                            : L"openMouse — device identification");
        if (HWND c = GetConsoleWindow()) { ShowWindow(c, SW_SHOW); SetForegroundWindow(c); }
    } else if (!mode.empty()) {
        // Only steal a console when stdout is not already going somewhere. If
        // the caller redirected our output to a pipe or file, AttachConsole
        // would replace that handle and the captured output would vanish.
        const HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!existing || existing == INVALID_HANDLE_VALUE) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        }
    }

    if (mode == L"--autostart=dry-run") return om::RunAutostartDryRun();
    if (mode == L"--list")       return om::RunList();
    if (mode == L"--probe")      return om::RunProbe();
    if (mode == L"--probe-echo") return om::RunProbeEcho();
    if (mode == L"--identify")   return om::RunIdentify();
    if (mode == L"--watch")      return om::RunWatch();
    if (mode == L"--restore-cursor") return om::RunRestoreCursor();
    if (mode == L"--receiver")   return om::RunReceiver();
    if (mode == L"--monitor")    return om::RunMonitor();
    if (mode == L"--help") {
        om::LogLine(L"openMouse — multi-pointer input for Windows");
        om::LogLine(L"");
        om::LogLine(L"  openmouse.exe               run the engine");
        om::LogLine(L"  openmouse.exe --settings    window to set each seat's speed and acceleration");
        om::LogLine(L"  openmouse.exe --list        list input devices and their paths");
        om::LogLine(L"  openmouse.exe --identify    bind each seat by moving its mouse");
        om::LogLine(L"  openmouse.exe --probe       check SendInput works under NOLEGACY");
        om::LogLine(L"  openmouse.exe --probe-echo  check whether injection feeds back");
        om::LogLine(L"  openmouse.exe --watch       show which interface a device emits on");
        om::LogLine(L"  openmouse.exe --receiver    window that logs the mouse messages it gets");
        om::LogLine(L"  openmouse.exe --monitor     system-wide button log with provenance");
        om::LogLine(L"  openmouse.exe --restore-cursor  bring back a cursor left hidden");
        om::LogLine(L"  openmouse.exe --help        this text");
        om::LogLine(L"");
        om::LogLine(L"  Run flags:  --verbose        per-event tracing to the log");
        om::LogLine(L"              --seconds=N     auto-exit after N seconds");
        om::LogLine(L"              --perf          input latency figures every 2 s");
        om::LogLine(L"              --startup       what the logon task passes: wait for the desktop first");
        om::LogLine(L"              --no-elevate    do not ask for administrator rights (the engine asks by default)");
        om::LogLine(L"  openmouse.exe --autostart=on|off  create or delete the logon task (needs administrator)");
        om::LogLine(L"  openmouse.exe --autostart=dry-run  print the task that would be created, change nothing");
        om::LogLine(L"");
        om::LogLine(L"  Panic key while running: Ctrl+Alt+Shift+Q");
        om::LogLine(L"  Tray icon: click for settings, to stop, or open the log folder");
        // %% — this string goes through a printf-style formatter, where a bare
        // %A is a hex-float specifier and eats the path.
        om::LogLine(L"  Log file: %%APPDATA%%\\openMouse\\openmouse.log");
        return 0;
    }
    // The engine itself wants administrator rights (see the elevation note
    // above). The logon task already starts it elevated; a copy started by hand
    // asks once. A refusal is not fatal: it runs, says so, and some windows
    // will escape the mice.
    if (!noElevate && !om::IsElevated() && om::RelaunchElevated(passArgs))
        return 0;
    return om::RunMain(inst);
}
