# openMouse: user guide

*[Version française](AIDE.md)*

openMouse gives every connected mouse its own pointer on the same Windows desktop. Two
people can work at one PC at the same time, each with their own mouse: one clicks here
while the other scrolls a page over there.

## Before you start

- openMouse replaces the Windows arrow with one coloured pointer per mouse. By default the
  first is yellow, the second orange.
- Only one person types at a time: Windows sends keystrokes to the active window only.
  The mice are independent, the keyboards are not.
- You need 64-bit Windows 10 or 11 and at least two mice (a laptop touchpad counts as one).

## 1. Start

1. Run `openmouse.exe`. There is no installer.
2. Windows asks for administrator permission: accept. Without it openMouse still works,
   but windows started "as administrator" (Task Manager, for example) escape the mice.
3. An icon appears next to the clock and two pointers of different colours are on screen.

To start it with Windows: click the icon, **Settings…**, **General** section, tick
**Start openMouse with Windows**. This is only available once openMouse is installed in a
protected folder such as `Program Files`, because the program then starts with full
rights; from another folder (Downloads, Desktop…) the box is greyed out and says so.

## 2. Plug in two mice

Just plug in the second mouse: a new pointer appears in a different colour. Unplugged, its
pointer disappears; plugged back, it returns where it stopped. The last pointer is never
hidden, so you are never left without one.

Each mouse keeps its colour, even on another USB port.

If two pointers are swapped or a mouse drives the wrong pointer (this can happen with
wireless receivers), unplug and replug that mouse's receiver, then open **Settings…** to see
the recognised mice.

## 3. Using both mice

- **Moving**: each mouse moves its own pointer without disturbing the other.
- **Clicking**: the click lands where the clicking mouse is pointing. Clicking into a
  window that is not active activates it and clicks in one press.
- **Scrolling**: the wheel scrolls the window under its own pointer, no click needed.
- **Dragging** (moving a file, selecting): while one person holds a button, the other's
  clicks wait until the gesture ends. The pointers stay free.
- **Back and forward buttons** of the mouse work too, each for its own mouse.

## 4. Settings

Click the icon next to the clock, then **Settings…**. One area per mouse. Every change
applies and is saved at once; **Undo my changes** restores the values the window opened
with.

### Pointer look

- **Colour**: ten colours, to recognise your pointer at a glance.
- **Pointer set**: classic, bright rim (the most visible, the default), white body with a
  coloured edge, large, or soft glow. The preview shows the result on four backgrounds.

### Pointer speed

Each mouse follows one of two modes:

- **Fixed sensitivity**: every movement is multiplied by the same value. The simplest:
  raise the value if the pointer is too slow.
- **Progressive acceleration**: slow for small gestures (precision), fast for wide ones
  (crossing the screen). The **acceleration threshold** sets where it speeds up.

**Default values** restores the original settings.

### Keep inside screen

For each mouse you can confine the pointer to one screen (handy when each person works on
their own monitor) or let it roam the whole desktop.

### Learn a button

A mouse sometimes has buttons Windows does not know. Click **Learn a button**, then press
the button within eight seconds. Then choose what it does: a click (left, right, middle,
back, forward), a keyboard shortcut, or nothing. The click happens at the position of that
mouse's own pointer. **Forget** removes the selected button.

Some buttons cannot be learned: those only the manufacturer's software can read send
nothing to Windows. The window tells you so.

## 5. When something goes wrong

| Problem | What to do |
|---|---|
| No pointer, or the pointer stays hidden | Press **Ctrl + Alt + Shift + Q**: openMouse stops and gives the Windows pointer back. Otherwise run `openmouse.exe --restore-cursor`. |
| After Ctrl + Alt + Del or locking the session | Normal: Windows takes over. On return openMouse resumes by itself. |
| An administrator window ignores the mice | Restart openMouse and accept the administrator permission. |
| A pointer follows the wrong mouse | Replug the mouse, then check the recognised mice in **Settings…**. |
| Anything else | **Open log folder** in the icon menu: `openmouse.log` says what happened. |

## 6. Known limits

- **One active keyboard at a time.** Two people cannot type at once into two different
  windows.
- **Taskbar, Start menu and notifications.** Only the pointer of the mouse that currently
  "holds" the real Windows pointer is visible there. The other pointer passes behind them:
  to use them, first click with that mouse in an ordinary window, or let the other person
  handle it.
- **A drag started straight into an inactive window** is lost: click once to activate the
  window, then drag.
- **Simultaneous scrolling**: if both people scroll at the same instant, a wheel tick can
  occasionally go to the other window.
- Sound, clipboard and window order stay shared between both people.

For technical details (building, command-line options, configuration file), see the
[README](../README.md).
