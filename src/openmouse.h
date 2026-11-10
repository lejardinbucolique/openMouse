// openMouse — multi-pointer input for Windows
// SPDX-License-Identifier: MIT
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace om {

// ---------------------------------------------------------------- constants

constexpr int   kMaxSeats        = 8;
constexpr UINT  kTrayCallbackMsg = WM_APP + 1;
constexpr UINT  kTrayIconId      = 1;
// Id of the icon resource in res/openmouse.rc.
constexpr WORD  kAppIconResource = 1;
// Posted to the engine window by the settings window once it has written new
// seat values to the configuration file: re-read them and use them at once.
constexpr UINT  kReloadSettingsMsg = WM_APP + 2;
// Settings window -> engine: listen to the buttons of one mouse and keep the
// first one pressed (wParam = seat + 1), or stop listening (wParam = 0).
constexpr UINT  kLearnMsg = WM_APP + 3;
// Engine -> settings window, once the listening is over. wParam = seat,
// lParam = kLearnHeard (the signature is in the learn file, ReadLearnResult)
// or kLearnNothing (nothing came in time).
constexpr UINT  kLearnedMsg = WM_APP + 4;
constexpr LPARAM kLearnHeard = 1, kLearnNothing = 2, kLearnAbsent = 3;   // absent: that mouse is not plugged in
constexpr DWORD  kLearnListenMs = 8000;
// Engine <-> settings window, both ways. Settings -> engine (any wParam): tell
// me which mice are plugged in. Engine -> settings: wParam = bit mask of the
// seats whose mouse is plugged in; sent after every change of devices and when
// asked. The settings window then re-reads the ini for the seats it lacks.
constexpr UINT  kSeatsMsg = WM_APP + 6;

// ---------------------------------------------------------------- device id

// A raw-input device is identified by its device interface path, e.g.
//   \\?\HID#VID_046D&PID_C52B&MI_01&Col01#7&2f8e7b0d&0&0000#{...}
// This is stable across reboots for a given physical port+device, which is
// what we bind against. Device *handles* are not stable and must never be
// persisted.
struct DeviceInfo {
    HANDLE       handle = nullptr;   // volatile, valid for this session only
    std::wstring path;               // stable identity, used in config
    std::wstring friendlyName;       // best-effort, for the UI
    DWORD        type = 0;           // RIM_TYPEMOUSE | RIM_TYPEKEYBOARD | RIM_TYPEHID
    USHORT       page = 0;           // RIM_TYPEHID only: top-level usage page
};

std::vector<DeviceInfo> EnumerateDevices();
std::wstring            DevicePathFromHandle(HANDLE h);
std::wstring            FriendlyNameFromPath(const std::wstring& path);

// Identity that survives a change of USB port or of enumeration order. The
// interface path embeds an instance id (the "6&1b42cbdd&0&0000" part) that
// depends on both; the hardware id before it does not. Returns e.g.
// "hid#vid_046d&pid_c077" (lower case, MI_/Col suffixes kept so the
// collections of one receiver stay distinct), or "" when the path carries no
// VID/PID (ACPI touchpads, PS/2), which then only ever match exactly.
std::wstring StableKey(const std::wstring& path);

// Hardware identity of an interface, lower case: "class#hardware id", with the
// MI_/Col collection suffixes kept ("hid#vid_046d&pid_c534&mi_01&col01").
// Unlike StableKey it also exists for paths without "vid_" (Bluetooth).
std::wstring HardwareKey(const std::wstring& path);

// The same without the collection suffixes ("hid#vid_046d&pid_c534"): what
// all the interfaces of one product have in common, ghost collections included.
std::wstring HardwareBase(const std::wstring& path);

// Keys whose input needs KEYEVENTF_EXTENDEDKEY (arrows, Insert, right Ctrl, ...).
bool IsExtendedVk(UINT vk);

// Raw-input devices that are neither mice nor keyboards: the media-key,
// system-control and manufacturer collections a mouse exposes next to its
// pointer. The usage page of each is in DeviceInfo::page.
std::vector<DeviceInfo> EnumerateHidDevices();

// Whether an unknown mouse may be given a seat on its own: false for the
// integrated devices (ACPI touchpads, PS/2) and for any interface that carries
// no manufacturer id (I2C touchpads), which only an ini entry can seat.
bool IsAutoSeatCandidate(const std::wstring& path);

// An integrated pointing device (laptop touchpad, PS/2): the opposite of an
// auto-seat candidate. It is seated by the touchpad rule, never as a mouse.
bool IsIntegratedPointer(const std::wstring& path);

// ---------------------------------------------------------------- seats

struct Seat {
    int      index   = 0;
    COLORREF color   = RGB(255, 0, 0);
    bool     enabled = false;

    // Virtual-desktop coordinates of this seat's pointer.
    POINT    pos{0, 0};

    // The window this seat last clicked into. Keyboard events bound to this
    // seat are steered here. Null until the seat clicks something.
    HWND     focusTarget = nullptr;

    // Button state, so we can synthesise correct down/up pairs and detect
    // drags in progress. x1/x2 are the browser back/forward side buttons.
    bool     lDown = false, rDown = false, mDown = false;
    bool     x1Down = false, x2Down = false;

    // When the previous left press physically ARRIVED and when it was
    // actually INJECTED. The cross-window settle delays injection by ~250 ms,
    // which compresses the app-perceived gap between successive clicks — two
    // deliberate single clicks can land within the double-click window and
    // select a word, and a focus-click before a real double-click makes a
    // paragraph-selecting triple. These timestamps let injection restore the
    // hand's true rhythm.
    DWORD    lastPressArrive = 0, lastPressInject = 0;
};

// ---------------------------------------------------------------- cursor looks

// A set of pointers, all derived from the system cursors so that every shape
// (arrow, hand, text bar, resize arrows, hourglass, forbidden) exists in every
// set, in the colour of the mouse. Stored in the ini as the name in
// CursorStyleKey.
enum CursorStyle { kStyleClassic = 0,   // body in the mouse colour, black edge
                   kStyleBright,        // the same with a bright white rim: seen on any background
                   kStyleInverse,       // white body, edge in the mouse colour, dark rim
                   kStyleBig,           // bright, one and a half times larger
                   kStyleGlow,          // body in the mouse colour, soft glow of that colour
                   kStyleCount };

const wchar_t* CursorStyleKey(int style);       // "bright"...
int            CursorStyleFromKey(const std::wstring& key, int fallback);

// ---------------------------------------------------------------- config

// What a learned button does. The clicks are played at the position of the
// pointer of the mouse the button belongs to; the keys are a system-wide
// shortcut.
enum ButtonAction { kActNone = 0, kActLeft, kActRight, kActMiddle, kActBack, kActForward,
                    kActKeys, kActCount };
constexpr int kMaxLearned = 16;   // learned buttons per seat

// Modifier bits of a shortcut (ButtonMap::mods).
constexpr UINT kModCtrl = 1, kModAlt = 2, kModShift = 4, kModWin = 8;

// A button the engine was taught by listening to its mouse. The signature says
// what that button sends to Windows, as text, so it can live in the ini:
//   m:<hex>        a bit of the raw mouse button flags (0x1 left down, 0x40 the
//                  "previous" button down, ...). Replaces the engine's own relay
//                  of that button.
//   k:<hex>        a key of the mouse's own keyboard interface: scan code, plus
//                  0x100 for an extended key (Windows key = 15B).
//   h<page>:<hex>  a report of the mouse's media (0C), system (01) or
//                  manufacturer (FF00) collection, as its raw bytes.
// Ini line, in the section of the seat:  button=<sig>|<action>|<mods:vk hex>|<label>
struct ButtonMap {
    std::wstring sig;
    std::wstring label;               // for the settings window only
    int          action = kActNone;
    UINT         mods = 0, vk = 0;    // kActKeys: the shortcut

    // Read from sig by Parse().
    wchar_t           kind = 0;       // 'm', 'k' or 'h'
    UINT              code = 0;       // m: the flag bit; k: scan | 0x100 if extended
    USHORT            page = 0;       // h: the usage page
    std::vector<BYTE> report;         // h: the report that means "pressed"

    bool Parse();                     // false when sig is not one of the forms above
    // True when Windows keeps acting on this button by itself as well (a key or
    // a media key). A manufacturer message, or a mouse flag we relay, does not.
    bool WindowsAlsoActs() const { return kind == 'k' || (kind == 'h' && page != 0xFF00); }
};

struct Config {
    // Per-seat device bindings, keyed by device interface path.
    std::map<std::wstring, int> mouseBinding;
    std::map<std::wstring, int> keyboardBinding;

    std::vector<COLORREF> seatColors = {
        RGB(0xFF, 0xDD, 0x00),   // seat 0 — bright yellow (blue vanished on busy screens)
        RGB(0xE8, 0x6A, 0x33),   // seat 1 — orange
        RGB(0x5C, 0xB8, 0x5C),   // seat 2 — green
        RGB(0xB0, 0x60, 0xC0),   // seat 3 — purple
    };

    // Keyboard capture takes over ALL keyboards system-wide. If this process
    // stalls, so does typing. Off by default; see README "Safety".
    bool captureKeyboards = false;

    // Pointer speed multiplier per seat index.
    std::vector<double> sensitivity = {1.0, 1.0, 1.0, 1.0};

    // Optional per-seat pointer acceleration. Off unless one of the accel_*
    // keys appears in the seat's section; then the fixed sensitivity is
    // replaced by a gain that rises smoothly with hand speed:
    //   gain = slow + (fast - slow) * v^2 / (v^2 + speed^2)
    // v is the device speed in counts per millisecond, smoothed per seat.
    // A USB mouse and a touchpad report different step sizes, hence per seat.
    struct Accel {
        bool   on    = false;
        double slow  = 1.0;   // gain for very slow, precise motion
        double fast  = 8.0;   // ceiling for fast sweeps across the screen
        double speed = 2.0;   // counts/ms where the gain is halfway up (USB mouse: 2)
    };
    std::vector<Accel> accel = std::vector<Accel>(4);

    // Optional screen a seat's pointer is kept inside, by its Windows device
    // name (\\.\DISPLAY2). Empty: the pointer roams the whole desktop. A
    // screen that is not plugged in behaves as empty, so the pointer is never
    // shut in a rectangle nobody can see; the key is kept for when it returns.
    std::vector<std::wstring> screen = std::vector<std::wstring>(4);

    // How each seat's arrow is drawn (a CursorStyle, ini key "cursor").
    std::vector<int> cursorStyle = std::vector<int>(kMaxSeats, (int)kStyleBright);

    // Buttons learned for each seat's mouse (see ButtonMap).
    std::vector<std::vector<ButtonMap>> buttons = std::vector<std::vector<ButtonMap>>(kMaxSeats);

    // Draw a coloured ring around the seat that currently owns the real
    // system cursor.
    bool markActiveSeat = true;

    int  seatCount = 2;

    // What the laptop touchpad does. Auto: it completes the mice so that two
    // arrows always exist (alone with no external mouse, beside a single one,
    // cut off from two onwards). On: always seated. Off: never, unless no
    // external mouse is left, so the computer is never without a pointer.
    enum class Touchpad { Auto, On, Off };
    Touchpad touchpad = Touchpad::Auto;

    // Grows the per-seat tables to hold n seats. A slot that did not exist
    // gets a colour that stands out from the seats already there.
    void EnsureSeatSlots(int n);

    // Colour for a seat created at run time: the palette entry farthest from
    // every other seat's colour.
    COLORREF PickSeatColor(int seat) const;

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
    static std::wstring DefaultPath();
};

// The button the engine just heard, handed to the settings window (a process of
// its own) through a small file beside the ini: a message cannot carry text.
bool WriteLearnResult(int seat, const std::wstring& sig, const std::wstring& label);
bool ReadLearnResult(int& seat, std::wstring& sig, std::wstring& label);

// ---------------------------------------------------------------- overlay

// One transparent, click-through, always-on-top window per seat, used to draw
// that seat's pointer when it does not own the real system cursor.
// What the arrow looks like: the plain arrow, the system's resize arrows when
// the pointer rests on a window border or corner that can be grabbed, or the
// shape the window under the active seat asked for (hand on a link, text bar…).
enum class PointerShape { Arrow, SizeWE, SizeNS, SizeNWSE, SizeNESW,
                          Hand, IBeam, No, Wait, AppStarting, Cross, SizeAll, Count };

// One pointer image, premultiplied BGRA as UpdateLayeredWindow wants it.
struct PointerImage {
    int w = 0, h = 0, hotX = 0, hotY = 0;
    std::vector<BYTE> bgra;
};
// Draws a shape of a set in a colour. False when nothing can be drawn.
bool RenderPointer(PointerShape shape, COLORREF color, int style, PointerImage& out);

class CursorOverlay {
public:
    bool Create(HINSTANCE inst, int seatIndex, COLORREF color, int style);
    // Changes colour and set; the arrow is redrawn at once when it changed.
    void SetLook(COLORREF color, int style);
    void SetShape(PointerShape shape);
    void Destroy();
    void MoveTo(POINT ptVirtual);
    void Show(bool visible);
    bool IsVisible() const { return visible_; }
    // Brings the arrow back above other topmost windows, only when a visible
    // topmost window of another process sits above it. Returns true if
    // SetWindowPos was called; who then gets that window's class name.
    bool ReassertTopmost(wchar_t* who = nullptr, int whoLen = 0);

private:
    void Present(PointerShape shape);

    HWND     hwnd_    = nullptr;
    HBITMAP  bitmap_  = nullptr;
    int      w_ = 0, h_ = 0;
    int      hotX_ = 1, hotY_ = 1;   // from the system cursor when available
    bool     visible_ = false;

    // One rendered bitmap per shape, built on first use, so a shape change on
    // the move path is a bitmap swap and not a rebuild.
    struct ShapeBmp { HBITMAP bmp = nullptr; int w = 0, h = 0, hotX = 1, hotY = 1; };
    static constexpr int kShapes = (int)PointerShape::Count;
    ShapeBmp     shapes_[kShapes];
    PointerShape shape_ = PointerShape::Arrow;
    COLORREF     color_ = 0;
    int          style_ = kStyleBright;
    POINT        at_{ 0, 0 };        // last logical position, to re-place the arrow on a shape swap
};

// ---------------------------------------------------------------- settings

// The settings window, a process of its own (openmouse.exe --settings).
int  RunSettings(HINSTANCE inst);
// True when Windows shows its interface in French; the tray menu and the
// settings window then speak French, otherwise English.
bool UiFrench();

// ---------------------------------------------------------------- autostart

// "Start with Windows": a scheduled task, run at logon at the highest level.
// Posted or sent to the engine window by the settings window. wParam is the
// request (kAutostart*). The answer is the LRESULT of a SendMessage: a
// combination of kAuto* flags describing the engine and the task as it knows
// them. Enable and disable are only accepted by an elevated engine, which does
// the work on a thread of its own; the caller polls with kAutostartQuery.
constexpr UINT    kAutostartMsg     = WM_APP + 5;
constexpr WPARAM  kAutostartQuery   = 0;
constexpr WPARAM  kAutostartEnable  = 1;
constexpr WPARAM  kAutostartDisable = 2;
constexpr LRESULT kAutoKnown    = 1;   // the engine has read the task state
constexpr LRESULT kAutoPresent  = 2;   // the task exists
constexpr LRESULT kAutoElevated = 4;   // the engine may create or delete it

bool IsElevated();
// 1: the task exists, 0: it does not, -1: this process may not see it.
int  AutostartState();
// Both need administrator rights.
bool AutostartInstall();
bool AutostartRemove();
// openmouse.exe --autostart=on|off, started elevated by the settings window.
int  RunAutostartHelper(bool enable);
// openmouse.exe --autostart=dry-run: prints the task definition that would be
// registered and the state of the task as it is now. Registers nothing.
int  RunAutostartDryRun();

// ---------------------------------------------------------------- utilities

RECT VirtualScreenRect();
void ClampToVirtualScreen(POINT& p);
void ClampToRect(POINT& p, const RECT& r);
// The screens plugged in now, in Windows order (primary flagged).
struct ScreenInfo { std::wstring name; RECT rc; bool primary; };
std::vector<ScreenInfo> ListScreens();
bool IsShellWindowClass(const wchar_t* cls);
// Full path of this executable, or "" when it cannot be had whole (a path cut
// short by a fixed buffer would name another file).
std::wstring SelfPath();

// Opens one of the files kept in the user's configuration folder (ini, log,
// learn file...). That folder is writable by the user while the engine that
// writes there may be elevated, so a link planted in place of the file or of
// the folder must not be followed: the open is refused (INVALID_HANDLE_VALUE)
// when the folder or the file is a reparse point, or when the file has other
// names (hard links) and is being written.
//   Read:    existing file, shared for reading.
//   Rewrite: created if missing, emptied, held exclusively.
//   Append:  created if missing, writes go to the end, shared with other writers.
enum class SafeOpen { Read, Rewrite, Append };
HANDLE OpenSafeFile(const std::wstring& path, SafeOpen how);
void LogLine(const wchar_t* fmt, ...);
void OpenLogFile();
void CloseLogFile();

// Signature stamped into dwExtraInfo on every event this process injects, so
// that the injected event can be recognised and dropped when it comes back to
// us through the raw input stream. Without this the engine feeds on its own
// output — see RunProbeEcho().
constexpr ULONG_PTR kInjectTag = 0x4F4D5331;   // 'OMS1'

} // namespace om
