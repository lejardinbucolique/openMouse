# openMouse

Multi-pointer input for Windows. Every connected mouse gets its own on-screen pointer on
one shared desktop, so two people can work at one PC at the same time — one per monitor,
each with their own mouse.

MIT licensed. No kernel driver, no second Windows session, no paid tier.

Named after Linux's Multi-Pointer X, which has offered this natively since 2008. Windows
never got an equivalent.

New to openMouse? Read the short user guide: [English](docs/HELP.md) ·
[Français](docs/AIDE.md) · [Deutsch](docs/HILFE.md) · [Español](docs/AYUDA.md) · [Русский](docs/SPRAVKA.md) · [简体中文](docs/BANGZHU.md) · [العربية](docs/DALIL.md). The rest of this README is
aimed at developers.

## Contents

- [Status](#status)
- [What it does](#what-it-does)
- [What it cannot do](#what-it-cannot-do)
- [Requirements](#requirements)
- [Installation](#installation)
- [Setup](#setup)
- [Configuration](#configuration)
- [Command reference](#command-reference)
- [Safety](#safety)
- [How it works](#how-it-works)
- [Findings](#findings)
- [Known limitations](#known-limitations)
- [Translations](#translations)
- [Building](#building)
- [Licence](#licence)

## Status

Working alpha, in daily use on the machine it was developed on (Windows 11 Pro 25H2,
build 26200; two Logitech mice and keyboards; two monitors).

The mouse half is complete and confirmed in real two-person use: independent pointers,
click-to-own, cross-window focus, per-seat scrolling and dragging. Keyboard support is
limited by an operating-system constraint described below, and is off by default.

It has been tested on exactly one machine. Several behaviours it depends on are
undocumented (see [Findings](#findings)), so treat other Windows versions as unverified
until you run `--probe` and `--probe-echo` there.

## What it does

- Each connected mouse drives its own pointer, drawn with the machine's real cursor
  artwork, tinted to the colour of its mouse (seat 0 starts bright yellow, seat 1 orange)
  and set off by a bright rim, or another set of pointers chosen per mouse.
- Exactly one seat owns the click stream at a time. Clicking transfers ownership, so
  clicks land where the clicker is pointing and hover states follow them. Clicking into
  a window that is not focused activates it and delivers the click in one press.
- Scrolling is per seat and requires no click: each wheel scrolls the window under its
  own pointer, and does not steal ownership.
- Dragging is protected. While one seat holds a button, the other seats' buttons are
  held off so the drag cannot be broken or hijacked; their pointers keep moving freely.
- Mouse back/forward side buttons work per seat.
- Runs from the system tray. Click the icon to open the settings window, stop,
  or open the log folder.
- A settings window sets each seat's speed and acceleration with sliders, applied at once
  while the engine runs, without editing any file.
- Each mouse can be set aside from the settings window with its **Disable** box, without
  unplugging it, and its section is titled with its make ("Logitech mouse"); see
  [Setting a mouse aside](#setting-a-mouse-aside).
- Any button of a mouse can be learned, even beyond the five Windows maps and without the
  manufacturer's driver, and given an action played at that mouse's own pointer; see
  [Learned buttons](#learned-buttons).
- Device bindings are stored by device interface path, so they survive reboots and
  replugging.

## What it cannot do

This section is the honest limit of a user-mode approach. Read it before relying on the
tool.

**One foreground window per desktop.** Windows delivers keystrokes only to the focused
window. There is no supported way to type into a control that does not have focus.
`PostMessage(WM_CHAR)` works for classic Win32 controls, where each control is a real
window, but Chromium (Edge, Chrome, Electron), WPF and UWP render every control inside a
single window handle. There is no handle to address.

**Per-seat keyboards cannot be built the way per-seat mice can.** The mouse works by
suppressing physical input in a low-level hook and re-injecting it per seat. That is
impossible for keyboards: measured on Windows 11 26200, blocking a key in a
`WH_KEYBOARD_LL` hook also suppresses that key's raw input delivery — and raw input is
the only place device identity exists, since `KBDLLHOOKSTRUCT` carries none. Suppress
the key and you go blind; stay sighted and you cannot suppress. The mouse is not subject
to this: blocking a button in `WH_MOUSE_LL` leaves its `WM_INPUT` intact.

Keyboard support is therefore **steering only**. Keystrokes are never suppressed and
never re-injected; the engine simply brings the typing seat's last-clicked window to the
front first. Consequences: no per-seat keyboard layouts, one shared modifier state, and
the first keystroke after a seat loses focus may still land in the previous window. Two
people typing simultaneously interleave into one window.

**One dragger at a time.** There is a single click stream, so while one seat drags, the
other seats' clicks are held until the drag ends.

**Shell surfaces.** The taskbar, Start menu and tray flyouts draw above every normal
window, including the pointer overlays. When the owning seat's pointer moves over shell
UI, the real cursor is shown there instead so those surfaces stay usable; non-owning
pointers are not visible over them.

Out of scope entirely: per-seat audio, per-seat clipboard, per-seat window z-order, and
anything needing real isolation. Isolation requires separate Windows sessions, which is
a different and much larger project.

## Requirements

| | |
|---|---|
| Operating system | Windows 10 or 11, x64. Windows 8 is the theoretical minimum (Magnification API) but is untested. |
| Architecture | x64 only. The Magnification API is not supported under WOW64, so a 32-bit build will not work correctly. |
| Privileges | No driver, no signing requirements. The engine **should run as administrator**: Windows keeps the input aimed at elevated windows (Task Manager, anything started "as administrator") away from a program at the ordinary level, so without it those windows escape the mice. The engine asks for the rights itself when you start it (one permission prompt, none if the logon task starts it). If you refuse, it still runs, says so in the log and in a tray notification, and those windows are not followed. |
| Hardware | Two or more mice. Any HID mouse should work; wireless receivers that expose several HID collections are handled by `--identify`. |
| Displays | Multi-monitor supported, including negative coordinates. Mixed-DPI setups are handled but lightly tested. |

Not portable to macOS or Linux, and not intended to be: the implementation is built on
Win32 raw input, low-level hooks and the Magnification API. Linux already has this
natively through X11's Multi-Pointer X.

## Installation

Download or build `openmouse.exe` (see [Building](#building)) and run it. There is no
installer and nothing is written outside `%APPDATA%\openMouse`, except the optional
logon task (see [Starting with Windows](#starting-with-windows)).

## Setup

Run these in order. Do not skip step 1.

**1. Check the machine is viable**

```
openmouse.exe --probe
openmouse.exe --probe-echo
```

`--probe` verifies that injected input still reaches the system while a raw-input
registration is active. `--probe-echo` reports whether injected input feeds back into the
raw input stream, which the engine must filter. If `--probe` fails, no user-mode
multi-pointer implementation is possible on that machine.

**2. Identify the devices**

```
openmouse.exe --identify
```

Wireless receivers routinely expose more HID interfaces than there are physical devices,
and enumeration order does not match physical order. On the development machine, two
mice and two keyboards enumerate as four and five interfaces respectively. `--identify`
binds each seat by observation: move only that seat's mouse, then type only on its
keyboard, when prompted. Interfaces it cannot attribute are left unbound rather than
guessed.

Nothing is suppressed during this step; the machine stays fully usable.

**3. Run**

```
openmouse.exe
```

A tray icon appears. You should see two pointers — a white arrow for seat 0 and a black
one for seat 1. Move both mice, click with each, scroll with each, drag with each.

Add `--seconds=60` for a first run that stops by itself.

**4. Optional: keyboard steering**

Set `capture_keyboards=1` in the configuration file and restart. This never suppresses or
injects keystrokes, so it cannot break typing; see
[What it cannot do](#what-it-cannot-do) for what it does and does not achieve.

## Configuration

The tray menu's **Settings…** entry (or `openmouse.exe --settings`) opens a window with,
for each seat, the colour and the set of pointers (with a preview on a white, a dark, a
green and a yellow background), the sensitivity and the acceleration sliders described
below. A change is
written to the configuration file and taken by the running engine at once; **Undo my
changes** puts back the mouse values the window opened with. The window has two tabs,
**General** (start with Windows, language, help, credits) and **Mice** (one section per
mouse), and runs as a process of its own, so it never shares a thread with the input path.
It speaks English, French, German, Spanish, Russian, Simplified Chinese or Arabic (the window is then mirrored, right to left): the language of Windows by default, or the one chosen in the
General tab (`language` in the ini), applied at once to the window, the tray menu and the
messages. Device bindings and the seat count are still set in the file for now.

`%APPDATA%\openMouse\openmouse.ini`

```ini
[general]
seats=2
touchpad=auto         ; auto | on | off, see below
capture_keyboards=0
mark_active_seat=1
language=auto         ; auto (language of Windows) | en | fr | de | es | ru | zh-CN | ar

[seat0]
color=FFDD00          ; FFFFFF renders the stock white arrow, 000000 the black scheme
cursor=bright         ; classic | bright | inverse | big | glow
sensitivity=1.00

[seat1]
color=E86A33
cursor=bright
sensitivity=1.00

[mouse]
\\?\HID#VID_046D&PID_C534&MI_01&Col01#7&393c14a9&0&0000#{...}=0
\\?\HID#VID_046D&PID_C534&MI_01&Col01#7&a9440b5&0&0000#{...}=1

[keyboard]
\\?\HID#VID_046D&PID_C534&MI_00#7&2bb71987&0&0000#{...}=0
\\?\HID#VID_046D&PID_C534&MI_00#7&dde68fc&0&0000#{...}=1
```

Any other `color` value tints the arrow body and keeps the dark outline. `cursor` picks
the set of pointers of that mouse; every set has all the shapes (arrow, hand, text bar,
resize arrows, hourglass, forbidden), drawn from the system pointers in the mouse colour:
`classic` (coloured body, black edge), `bright` (the same with a white rim, the default:
it shows on light, dark, green or yellow backgrounds), `inverse` (white body, coloured
edge, dark rim), `big` (one and a half times larger, bright rim) and `glow` (soft glow in
the mouse colour). Colour and set are taken by the running engine at once. `sensitivity`
is a per-seat pointer speed multiplier. Up to eight seats are supported; a
new mouse adds one by itself.

Optional pointer acceleration, per seat. As soon as any of these keys is present in a
`[seatN]` section, it replaces `sensitivity` for that seat; without them the fixed
multiplier applies as before:

```ini
accel_slow=1.00    ; gain for very slow motion: about 1 pixel per device count
accel_fast=8.00    ; gain ceiling for fast sweeps across the screen
accel_speed=2.00   ; hand speed (device counts per ms) where the gain is halfway up
```

The gain rises smoothly with speed (`slow + (fast - slow) * v² / (v² + speed²)`), with
no visible step, and pixel fractions carry over between moves so slow motion glides.
A USB mouse rarely exceeds 2 to 4 counts per ms, hence the low default threshold. A
touchpad reports a different step scale; tune each seat separately (`--perf` logs
each seat's speed and gain every two seconds of motion).

Once measured bindings exist, a mouse the ini has never listed is seated by itself, with
no setting and no command: it takes the first seat that has no mouse in the ini, or a new
seat (up to eight), with its own arrow, a colour that stands out from the other seats and
the speed settings of seat 0. The assignment is written to the ini, so the mouse finds the
same seat and colour the next time, on any USB port. The arrows follow the mice: plug one
in and its arrow appears; unplug it and its arrow disappears (the seat keeps its colour
and speed, and the arrow comes back where it stopped). The last arrow is never hidden.

The laptop touchpad (integrated ACPI/PS/2 device) completes the mice so that there are
always two arrows, re-evaluated at every plug and unplug, with no restart: no external
mouse, the touchpad alone; one external mouse, that mouse and the touchpad; two or more,
the mice only and the touchpad is cut off. `touchpad=on` keeps it seated whatever the
number of mice, `touchpad=off` cuts it off (except when no external mouse is left, so the
computer is never without a pointer). It gets a seat of its own, written to the ini like
any other.

Never seated on their own, and dropped as before: interfaces with no manufacturer id other
than the touchpad, other collections (`MI_`/`Col` suffix) of a
mouse that already has a seat, and every collection but one of a new product. List such a
device in `[mouse]` by hand to seat it anyway. On a first run with no bindings at all,
devices are auto-assigned round-robin so nothing is dead out of the box, but run
`--identify` before trusting the assignment.

### Setting a mouse aside

In the settings window, each mouse (not the touchpad, which has its own mode) has a
**Disable** box beside its title. Ticked, the mouse stays bound to its seat, with its colour,
speed, screen and learned buttons, but the engine ignores its input and hides its arrow; the
change applies at once, like the touchpad mode. Unticked, it comes back where it stopped. The
last working pointer cannot be set aside (the touchpad counts while it is seated, a mouse that
is unplugged does not): the window refuses, and the engine applies the same rule on its own,
so if the mice that worked are unplugged while others are set aside, those work again until
another pointer is back (the file is not changed). Setting a mouse aside does not bring the
touchpad back in `auto` mode, but with no working mouse left the touchpad is kept.

The state is the `[disabled]` section of the ini, keyed by the same interface path as
`[mouse]` and following the mouse to another USB port; entries that match no bound mouse are
dropped at the next write, and at most 64 are read.

```ini
[disabled]
\\?\HID#VID_046D&PID_C534&MI_01&Col01#7&393c14a9&0&0000#{...}=1
```

The title of each section shows the make of the mouse. It is read, in this order, from the
manufacturer string the mouse reports itself (`HidD_GetManufacturerString`, only while it is
plugged in), then from the USB vendor id in the path against a table of the makers that sell
mice under their own name (from the USB-IF list as kept in
[usb.ids](http://www.linux-usb.org/usb.ids); chip and contract makers whose id sits in many
unbranded mice are left out), then from the product string, then from the name Windows gives
it. A generic name ("HID-compliant mouse", "USB Optical Mouse") is never taken for a make:
the title then stays "Mouse n". Two mice of the same make carry their seat number too.
`openmouse.exe --list` prints the make it finds for each mouse.

### Learned buttons

A mouse often has more to say than the five buttons Windows maps: a button that sends
Windows+D through the mouse's own keyboard interface, media keys, messages only the
manufacturer's driver reads. In the settings window, each mouse has a **Learn a button**
button: press it, then press the button within eight seconds. The engine listens to that
one mouse and keeps the first thing it sends; you then choose what it does:

- a click (left, right, middle, back, forward), played at the position of **that mouse's
  own pointer**, with the same ownership and drag rules as the mouse's own buttons;
- a keyboard shortcut, sent to the whole system;
- nothing.

**Forget** removes the selected button. Up to 16 buttons per mouse. The table is stored in
the `[seatN]` section and taken by the running engine at once, like the speeds:

```ini
[seat0]
button=k:15B|6|1:54|Mouse key "Left Windows"      ; sig | action | mods:key | label
button=hFF00:0101A0|2|0:0|Manufacturer button 0101A0
```

`action` is 0 nothing, 1 left, 2 right, 3 middle, 4 back, 5 forward, 6 shortcut (`mods`
is a bit mask, 1 Ctrl, 2 Alt, 4 Shift, 8 Win, and `key` a virtual key code, both in hex).
The signature says what the button sends to Windows:

| Signature | What it is |
|---|---|
| `m:<hex>` | A bit of the raw mouse button flags (`40` is the "back" button). The chosen action **replaces** the engine's own relay of that button |
| `k:<hex>` | A key of the mouse's own keyboard interface: scan code, plus `100` for an extended key (`15B` is the Windows key) |
| `h<page>:<hex>` | A report of the mouse's media (`0C`), system (`01`) or manufacturer (`FF00`) collection, as its raw bytes |

What is listened to is the mouse's own hardware, nothing else. A keyboard or media
interface is looked at only when it has the same hardware id as a mouse bound to a seat
(`openmouse.exe --list` shows which interfaces that is) and is not a keyboard the ini
binds itself, so a real keyboard is never read. Keys typed on it are neither retained nor
logged. Even on a radio receiver shared by a mouse and a keyboard, only keys that do not
type (modifiers, F keys, navigation, media keys) can be learned.

Limits, said plainly:

- A button only the manufacturer's software can read (a message exclusive to its driver,
  or one the mouse keeps for itself) sends nothing to Windows and cannot be learned: the
  window then says so. The same goes for the sixth button and beyond of a mouse whose
  extra buttons Windows folds into the five it maps: the raw mouse input never shows them.
- For a key or a media key, Windows also carries out its own action (a button that sends
  Windows+D still shows the desktop): the keyboard stream cannot be intercepted per
  device without blinding the engine (see the keyboard gate note in `main.cpp`). The
  window says so under such a button. A manufacturer message, or a mouse button of the
  `m:` kind, has no such effect of its own.
- Two mice of the same model share their hardware id: the keys and reports of their
  interfaces then belong to no seat (their mouse buttons still do).

Each arrow that moves costs about 0.2 to 0.3 ms per flush (measured, 1 to 8 overlays).

## Command reference

| Command | Purpose |
|---|---|
| `openmouse.exe` | Run the engine |
| `--identify` | Bind each seat by observing its devices |
| `--list` | List every input device and its interface path |
| `--probe` | Check that injected input reaches the system |
| `--probe-echo` | Check whether injected input feeds back into raw input |
| `--watch` | Show which interface a given physical device emits on |
| `--receiver` | Open a window that logs the mouse messages it receives |
| `--monitor` | Log system-wide button events with provenance |
| `--restore-cursor` | Restore a system cursor left hidden |
| `--autostart=on` / `off` | Create or delete the logon task (needs administrator; the settings window runs it for you) |
| `--autostart=dry-run` | Say whether this copy is in a protected folder (and why not), print the task definition that would be registered and the task's current state; registers nothing |
| `--help` | Usage summary |

Run flags: `--verbose` enables per-event tracing to the log, `--seconds=N` exits after N
seconds, `--cage` forces the legacy cursor-clipping strategy, `--startup` is what the
logon task passes (see below), `--no-elevate` runs at your own level without asking for
administrator rights (the engine asks by default; the copy it starts carries `--elevated`).

The request is made by the program, not by its manifest, so that `--help`, `--list` and the
settings window never show a prompt. The cleaner Windows route for input tools, the
`uiAccess` manifest option, needs a signed executable installed under Program Files; it
comes with code signing and the installer, not before.

After Ctrl+Alt+Del or a locked session, the secure desktop takes the input away from every
ordinary program and gives you the normal Windows pointer. When you return, openMouse
registers its input again, hides the hardware cursor again and redraws the arrows.

Logs are written to `%APPDATA%\openMouse\openmouse.log` and rotate at 4 MB.

### Starting with Windows

The settings window has a **General** tab with the box "Start openMouse with
Windows". It is off unless you tick it; openMouse never installs itself otherwise.

Ticking it creates a Windows scheduled task named `openMouse`, triggered when you log on,
that runs `openmouse.exe --startup` with the highest run level available to you. A Run
registry key would not do: it starts without elevation, and an engine that is not elevated
cannot steer windows that are (Task Manager). Unticking the box deletes the task, and
nothing else is left behind. The box always shows the real state of the task.

Creating or deleting the task needs administrator rights. When the engine is already
running as administrator it does it silently; otherwise Windows asks for permission once
for that change. The task only ever starts this same executable, from the folder it was
ticked in: moving `openmouse.exe` afterwards means untick and tick again. You can see or
remove the task by hand in the Task Scheduler, or with `schtasks /Delete /TN openMouse /F`.

The task starts the program with full rights and no prompt, so it is only created for a copy
that nothing but an administrator can replace: on a fixed local disk, with no link or
junction in its path, and with neither the file nor any folder above it changeable by an
ordinary account (`Program Files` qualifies; your user profile, Downloads or Desktop do
not). From anywhere else the box is greyed out with an explanation, and the engine and
`--autostart=on` refuse as well, whatever the settings window said. An existing task that
points to a changeable folder is flagged in the settings window; unticking the box removes
it. `--autostart=dry-run` prints the verdict and the reason without changing anything.

At logon `--startup` waits (40 s at most) for the taskbar and for one mouse to exist before
hiding the hardware cursor, and does nothing if an engine is already running. If it fails
to start, the watchdog and `--restore-cursor` give the normal pointer back.

## Safety

While openMouse runs it owns all mouse input on the machine: a low-level mouse hook
swallows every physical button, wheel and move event, and the engine re-injects them per
seat. It also hides the hardware cursor through the Magnification API and positions it
itself.

| Risk | Mitigation |
|---|---|
| Process hangs, leaving no working input | A watchdog thread removes the hook, releases capture and restores the cursor after roughly three seconds of a stalled message loop |
| Need to stop immediately | `Ctrl+Alt+Shift+Q` releases everything and exits. It works from any keyboard, whether or not keyboard steering is on |
| Runaway injection loop | Injections are tagged and self-echoes discarded; a rate limiter of 300 injections per second releases everything and exits |
| Process crashes | An unhandled-exception filter releases everything. The hook and raw-input registration die with the process regardless |
| Process killed outright | A companion janitor process waits on the engine's handle and restores the cursor, and clears any cursor clip, however the engine died |
| Cursor left hidden | `openmouse.exe --restore-cursor` |
| Move suppression stops working after an OS update | Watched at runtime. If physical moves are being swallowed while no raw input arrives, the engine reverts to the cursor-cage strategy by itself within a second |

`Ctrl+Alt+Del` is handled by the kernel and is never captured.

## How it works

Windows has one hardware cursor and one foreground window per desktop. openMouse does
not try to change that. One seat owns the real cursor at any moment; every seat's pointer
is drawn as a layered, click-through, topmost window.

```
 physical mice and keyboards
          |
          v
   WH_MOUSE_LL gate ------> physical buttons, wheel and moves are blocked here
          |                 (injected events pass; this is the real suppression)
          v
   WM_INPUT (INPUTSINK | DEVNOTIFY)   <- used only to tell devices apart
          |   self-injected echoes (hDevice == NULL, or our tag) discarded
          |   ghost HID collections discarded
          v
   device handle ---> seat lookup
          |
          +-- mouse move  --> seat.pos += delta * sensitivity
          |                   owning seat -> SetCursorPos; the real cursor is
          |                   hidden and moves only where the engine puts it
          |                   every seat  -> layered-window overlay arrow
          |
          +-- mouse button -> drag guard; take ownership; if the target window
          |                   is not focused, activate it and let it settle;
          |                   preserve multi-click rhythm; inject the click
          |
          +-- wheel        -> park the cursor at this seat's position, then
          |                   inject (hover-scroll routes it to that window)
          |
          +-- key          -> panic-combo check; if steering is on, bring the
                              seat's last-clicked window to the front
```

| File | Responsibility |
|---|---|
| `src/main.cpp` | Raw input capture, routing, injection, suppression gate, watchdog, tray, janitor, command modes |
| `src/devices.cpp` | Device enumeration (mice, keyboards, HID collections), stable identity via the registry Enum key, make of a mouse |
| `src/overlay.cpp` | Per-seat pointer rendering derived from the real system cursor |
| `src/config.cpp` | Configuration load and save (seat values, learned buttons), logging |
| `src/lang.cpp` | Interface language: the one in use, the list of languages, the messages of the engine (tray) |
| `src/settings.cpp` | Settings window (`--settings`): per-seat speed, acceleration, screen, learned buttons, mouse set aside |

## Findings

These behaviours were measured on Windows 11 Pro build 26200 and are recorded because
they are either undocumented or contradict a reasonable reading of the documentation.
They shaped the design.

**`RIDEV_NOLEGACY` is per-application.** Its suppression applies to the registering
application only, so registering it from a background process does not stop physical
clicks reaching other applications. An early version relied on it, and every click was
consequently delivered twice — once physically and once injected — which appeared as
single clicks selecting text and double clicks selecting paragraphs. Suppression is now
performed by a low-level mouse hook. Raw input is used purely for device attribution.

**Blocking in a low-level mouse hook leaves raw input intact; the keyboard equivalent
does not.** In one measured session the engine still received 242 physical button events
while its hook blocked them, but zero keyboard events while its keyboard hook blocked
those. This asymmetry is the reason per-seat mice are possible and per-seat keyboards are
not.

**Injected input returns through raw input.** `SendInput` events arrive as `WM_INPUT`
with `hDevice == NULL`, carrying the `dwExtraInfo` tag intact. Unfiltered, the first
click becomes an infinite loop, and a watchdog cannot detect it because the storm keeps
the heartbeat healthy. Injections are tagged and discarded on arrival.

**`SetCursorPos` generates no low-level hook event at all.** This is what makes blocking
physical moves safe: the engine cannot block its own cursor positioning. Because it is
undocumented, it is verified at runtime and the engine falls back automatically.

**A `ClipCursor` rectangle survives the death of the process that set it.** A hard kill
can leave the pointer confined to a single pixel, with no way to click out of it. The
janitor process exists for this.

**`MagShowSystemCursor` has no documented restoration on process exit.** Same exposure,
same mitigation.

## Known limitations

- Keyboard support is steering only, and off by default.
- A drag begun as the very first action into an unfocused window is lost to the window
  activation delay. Click to focus, then drag.
- Simultaneous scrolling by two seats can occasionally cross-route a wheel tick, because
  one hardware cursor arbitrates hover-scroll routing.
- Applications that reposition the cursor themselves, such as the "snap to default
  button" accessibility setting, will move the owning seat's pointer.
- Non-owning pointers are not visible over the taskbar, Start menu or tray flyouts.
- No IME support. Non-Latin input under keyboard steering is untested and likely broken.
- Absolute-positioning devices such as tablets and touchscreens are handled in code but
  untested.
- Seat counts above two are supported in code but only lightly tested on real hardware.
- A new mouse that exposes several mouse collections can only be told from a ghost by its
  suffix; if the wrong one is seated, list the right interface in `[mouse]`.
- The settings window covers colour, pointer set, speed, acceleration, screen, learned
  buttons and setting a mouse aside; bindings are still edited in the file.

## Translations

The interface (settings window, tray menu and messages) and the user guide are available in:

| Language | Code (`language=`) | User guide |
|---|---|---|
| English | `en` | [HELP.md](docs/HELP.md) |
| Français | `fr` | [AIDE.md](docs/AIDE.md) |
| Deutsch | `de` | [HILFE.md](docs/HILFE.md) |
| Español | `es` | [AYUDA.md](docs/AYUDA.md) |
| Русский | `ru` | [SPRAVKA.md](docs/SPRAVKA.md) |
| 简体中文 | `zh-CN` | [BANGZHU.md](docs/BANGZHU.md) |
| العربية | `ar` | [DALIL.md](docs/DALIL.md) |

The translations other than the project's own languages are made with the help of an AI and
have not been proofread by a person who speaks the language. Corrections are welcome: report
them as an issue on the [GitHub repository of the fork](https://github.com/lejardinbucolique/openMouse/issues).
Adding a language is described next to the `Lang` enum in `src/openmouse.h`.

**Traductions.** L'interface (fenêtre des réglages, menu de l'icône et messages) et le mode
d'emploi existent en anglais, en français, en allemand, en espagnol, en russe, en chinois simplifié et en arabe (liste ci-dessus). Les traductions
autres que celles du projet sont faites avec l'aide d'une IA, sans relecture par une personne
qui parle la langue. Les corrections sont les bienvenues : à signaler par un ticket sur le
[dépôt GitHub du fork](https://github.com/lejardinbucolique/openMouse/issues).

## Building

Requires the Visual Studio Build Tools with the C++ workload, or any MSVC-compatible
toolchain, plus the Windows SDK.

```
build.cmd
```

The script locates the toolchain with `vswhere` and produces `openmouse.exe` in the
repository root.

Installer (needs Inno Setup 6): `build-installer.cmd [version]` builds the program, then
`installer/openmouse.iss`, and writes `dist\openMouse-setup-<version>.exe`. It installs into
Program Files only (a folder ordinary users cannot change, which the start-with-Windows task
requires); `dist/` is not tracked.

With CMake:

```
cmake -B build -A x64
cmake --build build --config Release
```

With MinGW, including cross-compiling from Linux:

```
x86_64-w64-mingw32-g++ -std=c++17 -municode -mwindows -O2 -Wall -static \
    src/main.cpp src/devices.cpp src/overlay.cpp src/config.cpp src/lang.cpp \
    src/settings.cpp src/autostart.cpp \
    -o openmouse.exe -luser32 -lgdi32 -lshell32 -lole32 -luuid -ladvapi32 \
    -lcomctl32 -loleaut32 -lwtsapi32 -static-libgcc -static-libstdc++
```

`/utf-8` is required under MSVC. The sources are UTF-8 without a byte-order mark, and
without that flag MSVC decodes them as the system ANSI codepage, corrupting non-ASCII
characters inside wide string literals as well as narrow ones.

## Licence

openMouse, créé par Alston Mendonca, amélioré et mis au point par [Calepitool](https://calepitool.fr/).

MIT. See [LICENSE](LICENSE).
