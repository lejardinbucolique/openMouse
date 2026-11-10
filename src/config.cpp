// openMouse — configuration
// SPDX-License-Identifier: MIT
//
// A hand-rolled INI reader rather than GetPrivateProfileString, because device
// interface paths are long and contain '#', '&' and '{}' — characters the Win32
// profile APIs handle poorly. Paths never contain '=', so splitting on the
// first '=' is unambiguous.
#include "openmouse.h"
#include <cstdio>
#include <cstdarg>
#include <shlobj.h>
#include <algorithm>
#include <cmath>

namespace om {

HANDLE OpenSafeFile(const std::wstring& path, SafeOpen how) {
    // The folder is held open, without the right to delete or rename it, until
    // the file is open and checked: it cannot be swapped for a link in between.
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? std::wstring(L".") : path.substr(0, slash);
    HANDLE hd = CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (hd == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION di{};
    HANDLE h = INVALID_HANDLE_VALUE;
    if (GetFileInformationByHandle(hd, &di) && !(di.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        DWORD access = GENERIC_READ, share = FILE_SHARE_READ, disposition = OPEN_EXISTING;
        if (how == SafeOpen::Rewrite) { access = GENERIC_WRITE; share = 0; disposition = OPEN_ALWAYS; }
        if (how == SafeOpen::Append)  { access = FILE_APPEND_DATA; share = FILE_SHARE_READ | FILE_SHARE_WRITE; disposition = OPEN_ALWAYS; }
        h = CreateFileW(path.c_str(), access, share, nullptr, disposition,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION fi{};
            bool ok = GetFileInformationByHandle(h, &fi) &&
                      !(fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
                      (how == SafeOpen::Read || fi.nNumberOfLinks <= 1);
            if (ok && how == SafeOpen::Rewrite) ok = SetEndOfFile(h) != 0;   // the pointer is at 0
            if (!ok) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
        }
    }
    CloseHandle(hd);
    return h;
}

namespace {

// A configuration file is a few kilobytes. Anything past this is not one, and
// the engine that reads it may be elevated.
constexpr LONGLONG kMaxConfigBytes = 1024 * 1024;

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring ReadFileUtf16(const std::wstring& path) {
    HANDLE h = OpenSafeFile(path, SafeOpen::Read);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > kMaxConfigBytes) {
        CloseHandle(h);
        LogLine(L"[config] %s is not read: not a plausible size", path.c_str());
        return L"";
    }
    std::string raw((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    if (!raw.empty() && !ReadFile(h, raw.data(), (DWORD)raw.size(), &read, nullptr)) read = 0;
    CloseHandle(h);
    raw.resize(read);

    // Accept UTF-8 with or without BOM; that is what an editor will produce.
    const char* p = raw.c_str();
    int len = (int)raw.size();
    if (len >= 3 && (BYTE)p[0] == 0xEF && (BYTE)p[1] == 0xBB && (BYTE)p[2] == 0xBF) {
        p += 3; len -= 3;
    }
    if (len <= 0) return L"";
    int need = MultiByteToWideChar(CP_UTF8, 0, p, len, nullptr, 0);
    if (need <= 0) return L"";
    std::wstring out(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, len, &out[0], need);
    return out;
}

bool WriteFileUtf8(const std::wstring& path, const std::wstring& text) {
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
    std::string raw(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        &raw[0], need, nullptr, nullptr);

    HANDLE h = OpenSafeFile(path, SafeOpen::Rewrite);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, raw.data(), (DWORD)raw.size(), &written, nullptr);
    CloseHandle(h);
    return written == raw.size();
}

// A number read from the file, held to a range the program can work with: the
// text may say "nan", "1e999" or anything else.
double Sane(const std::wstring& val, double lo, double hi, double fallback) {
    const double v = _wtof(val.c_str());
    if (!std::isfinite(v)) return fallback;
    return std::min(hi, std::max(lo, v));
}

unsigned long HexOf(const std::wstring& s, bool* ok) {
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(s.c_str(), &end, 16);
    if (ok) *ok = !s.empty() && end && *end == L'\0';
    return v;
}

} // namespace

bool ButtonMap::Parse() {
    kind = 0; code = 0; page = 0; report.clear();
    if (sig.size() < 3) return false;
    bool ok = false;
    if (sig[0] == L'm' && sig[1] == L':') {
        code = (UINT)HexOf(sig.substr(2), &ok);
        if (!ok || code == 0 || (code & (code - 1))) return false;   // one bit
        kind = L'm';
        return true;
    }
    if (sig[0] == L'k' && sig[1] == L':') {
        code = (UINT)HexOf(sig.substr(2), &ok);
        if (!ok || (code & 0xFF) == 0 || code > 0x1FF) return false;
        kind = L'k';
        return true;
    }
    if (sig[0] == L'h') {
        const size_t colon = sig.find(L':');
        if (colon == std::wstring::npos || colon < 2) return false;
        const unsigned long p = HexOf(sig.substr(1, colon - 1), &ok);
        if (!ok || p == 0 || p > 0xFFFF) return false;
        const std::wstring bytes = sig.substr(colon + 1);
        if (bytes.empty() || bytes.size() % 2 || bytes.size() > 128) return false;
        for (size_t i = 0; i < bytes.size(); i += 2) {
            const unsigned long b = HexOf(bytes.substr(i, 2), &ok);
            if (!ok) return false;
            report.push_back((BYTE)b);
        }
        page = (USHORT)p;
        kind = L'h';
        return true;
    }
    return false;
}

std::wstring Config::DefaultPath() {
    wchar_t* appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        const std::wstring roaming = appdata;
        CoTaskMemFree(appdata);
        dir = roaming + L"\\openMouse";
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring path = dir + L"\\openmouse.ini";

        // Carry over a configuration written under the project's former name.
        // Device bindings are measured by hand with --identify; silently
        // losing them on an upgrade would be a poor trade for a rename.
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const std::wstring legacy = roaming + L"\\OpenMPX\\openmpx.ini";
            // Read and written back through the checked open, not CopyFile,
            // which would follow a link planted at either end.
            const std::wstring old = ReadFileUtf16(legacy);
            if (!old.empty()) WriteFileUtf8(path, old);
        }
        return path;
    }
    return L"openmouse.ini";
}

namespace {

// Colours handed to seats created at run time, picked for how well they tell
// apart from each other on a white or dark background.
const COLORREF kPalette[] = {
    RGB(0x4A, 0x90, 0xD9),   // blue
    RGB(0x5C, 0xB8, 0x5C),   // green
    RGB(0xB0, 0x60, 0xC0),   // purple
    RGB(0xE0, 0x3C, 0x3C),   // red
    RGB(0x20, 0xC0, 0xD0),   // cyan
    RGB(0xF0, 0x6E, 0xAA),   // pink
    RGB(0xF5, 0xD0, 0x00),   // yellow
    RGB(0xE8, 0x6A, 0x33),   // orange
    RGB(0xB8, 0xD8, 0x2A),   // lime
    RGB(0x8B, 0x5A, 0x2B),   // brown
};

long ColorDistance2(COLORREF a, COLORREF b) {
    const long dr = (long)GetRValue(a) - GetRValue(b);
    const long dg = (long)GetGValue(a) - GetGValue(b);
    const long db = (long)GetBValue(a) - GetBValue(b);
    return dr * dr + dg * dg + db * db;
}

} // namespace

COLORREF Config::PickSeatColor(int seat) const {
    COLORREF best = kPalette[0];
    long bestDist = -1;
    for (const COLORREF c : kPalette) {
        long nearest = 0x7FFFFFFF;
        for (int i = 0; i < seatCount && i < (int)seatColors.size(); ++i)
            if (i != seat) nearest = std::min(nearest, ColorDistance2(c, seatColors[i]));
        if (nearest > bestDist) { bestDist = nearest; best = c; }
    }
    return best;
}

void Config::EnsureSeatSlots(int n) {
    if (n > kMaxSeats) n = kMaxSeats;
    if ((int)sensitivity.size() < n) sensitivity.resize(n, 1.0);
    if ((int)accel.size()       < n) accel.resize(n);
    if ((int)screen.size()      < n) screen.resize(n);
    if ((int)cursorStyle.size() < n) cursorStyle.resize(n, (int)kStyleBright);
    while ((int)seatColors.size() < n) {
        seatColors.push_back(RGB(200, 200, 200));
        seatColors.back() = PickSeatColor((int)seatColors.size() - 1);
    }
}

bool Config::Load(const std::wstring& path) {
    const std::wstring text = ReadFileUtf16(path);
    if (text.empty()) return false;

    std::wstring section;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;

        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, eq));
        std::wstring val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (section == L"general") {
            if      (key == L"seats")             seatCount        = _wtoi(val.c_str());
            else if (key == L"capture_keyboards") captureKeyboards = (val == L"1" || val == L"true");
            else if (key == L"mark_active_seat")  markActiveSeat   = (val == L"1" || val == L"true");
            else if (key == L"touchpad")
                touchpad = (val == L"on")  ? Touchpad::On
                         : (val == L"off") ? Touchpad::Off : Touchpad::Auto;
        } else if (section == L"mouse") {
            mouseBinding[key] = _wtoi(val.c_str());
        } else if (section == L"keyboard") {
            keyboardBinding[key] = _wtoi(val.c_str());
        } else if (section.rfind(L"seat", 0) == 0) {
            const int idx = _wtoi(section.substr(4).c_str());
            if (idx >= 0 && idx < kMaxSeats) {
                if ((int)sensitivity.size() <= idx) sensitivity.resize(idx + 1, 1.0);
                if ((int)seatColors.size()  <= idx) seatColors.resize(idx + 1, RGB(200,200,200));
                if ((int)accel.size()       <= idx) accel.resize(idx + 1);
                if ((int)screen.size()      <= idx) screen.resize(idx + 1);
                if (key == L"sensitivity") sensitivity[idx] = Sane(val, 0.01, 100.0, 1.0);
                else if (key == L"accel_slow")  { accel[idx].on = true; accel[idx].slow  = Sane(val, 0.01, 100.0, 1.0); }
                else if (key == L"accel_fast")  { accel[idx].on = true; accel[idx].fast  = Sane(val, 0.01, 100.0, 8.0); }
                else if (key == L"accel_speed") { accel[idx].on = true; accel[idx].speed = Sane(val, 0.01, 1000.0, 2.0); }
                else if (key == L"screen") screen[idx] = val;
                else if (key == L"cursor") {
                    if ((int)cursorStyle.size() <= idx) cursorStyle.resize(idx + 1, (int)kStyleBright);
                    cursorStyle[idx] = CursorStyleFromKey(val, (int)kStyleBright);
                }
                else if (key == L"button") {
                    // sig|action|mods:vk|label  (the label may be missing)
                    std::vector<std::wstring> f;
                    size_t from = 0;
                    for (int n = 0; n < 3; ++n) {
                        const size_t bar = val.find(L'|', from);
                        if (bar == std::wstring::npos) break;
                        f.push_back(val.substr(from, bar - from));
                        from = bar + 1;
                    }
                    f.push_back(val.substr(from));
                    if (f.size() >= 3 && (int)buttons[idx].size() < kMaxLearned) {
                        ButtonMap b;
                        b.sig    = f[0];
                        b.action = _wtoi(f[1].c_str());
                        const size_t colon = f[2].find(L':');
                        if (colon != std::wstring::npos) {
                            b.mods = (UINT)wcstoul(f[2].c_str(), nullptr, 16) & (kModCtrl | kModAlt | kModShift | kModWin);
                            b.vk   = (UINT)wcstoul(f[2].c_str() + colon + 1, nullptr, 16);
                            if (b.vk > 0xFF) b.vk = 0;   // not a virtual-key code
                        }
                        b.label = f.size() >= 4 ? f[3] : L"";
                        if (b.action < 0 || b.action >= kActCount) b.action = kActNone;
                        if (b.action == kActKeys && b.vk == 0) b.action = kActNone;
                        if (b.Parse()) buttons[idx].push_back(std::move(b));
                    }
                }
                else if (key == L"color") {
                    const unsigned long v = wcstoul(val.c_str(), nullptr, 16);
                    seatColors[idx] = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
                }
            }
        }
    }
    if (seatCount < 1) seatCount = 1;
    if (seatCount > kMaxSeats) seatCount = kMaxSeats;
    EnsureSeatSlots(seatCount);
    return true;
}

bool Config::Save(const std::wstring& path) const {
    std::wstring t;
    t += L"; openMouse configuration\n";
    t += L"; Device keys are raw input interface paths. Run openmouse.exe --list\n";
    t += L"; to print the paths of every mouse and keyboard on this machine.\n\n";

    t += L"[general]\n";
    t += L"seats=" + std::to_wstring(seatCount) + L"\n";
    t += L"touchpad=" + std::wstring(touchpad == Touchpad::On ? L"on"
                                   : touchpad == Touchpad::Off ? L"off" : L"auto") + L"\n";
    t += L"capture_keyboards=" + std::wstring(captureKeyboards ? L"1" : L"0") + L"\n";
    t += L"mark_active_seat="  + std::wstring(markActiveSeat  ? L"1" : L"0") + L"\n\n";

    for (int i = 0; i < seatCount; ++i) {
        wchar_t buf[128];
        const COLORREF c = (i < (int)seatColors.size()) ? seatColors[i] : RGB(200,200,200);
        const double s   = (i < (int)sensitivity.size()) ? sensitivity[i] : 1.0;
        swprintf(buf, 128, L"[seat%d]\ncolor=%02X%02X%02X\nsensitivity=%.2f\n\n",
                 i, GetRValue(c), GetGValue(c), GetBValue(c), s);
        t += buf;
        if (i < (int)accel.size() && accel[i].on) {
            t.resize(t.size() - 1);   // keep the accel keys inside the section
            swprintf(buf, 128, L"accel_slow=%.2f\naccel_fast=%.2f\naccel_speed=%.2f\n\n",
                     accel[i].slow, accel[i].fast, accel[i].speed);
            t += buf;
        }
        if (i < (int)cursorStyle.size()) {
            t.resize(t.size() - 1);
            t += L"cursor=" + std::wstring(CursorStyleKey(cursorStyle[i])) + L"\n\n";
        }
        if (i < (int)screen.size() && !screen[i].empty()) {
            t.resize(t.size() - 1);
            t += L"screen=" + screen[i] + L"\n\n";
        }
        if (i < (int)buttons.size() && !buttons[i].empty()) {
            t.resize(t.size() - 1);
            for (const ButtonMap& b : buttons[i]) {
                std::wstring label = b.label;
                for (wchar_t& ch : label) if (ch == L'|' || ch == L'\n' || ch == L'\r') ch = L' ';
                wchar_t head[64];
                swprintf(head, 64, L"|%d|%X:%X|", b.action, b.mods, b.vk);
                t += L"button=" + b.sig + head + label + L"\n";
            }
            t += L"\n";
        }
    }

    t += L"[mouse]\n";
    for (const auto& kv : mouseBinding)
        t += kv.first + L"=" + std::to_wstring(kv.second) + L"\n";
    t += L"\n[keyboard]\n";
    for (const auto& kv : keyboardBinding)
        t += kv.first + L"=" + std::to_wstring(kv.second) + L"\n";

    return WriteFileUtf8(path, t);
}

// ---------------------------------------------------------------- learn file

namespace {

std::wstring LearnPath() {
    std::wstring p = Config::DefaultPath();
    const size_t slash = p.rfind(L'\\');
    return (slash == std::wstring::npos) ? std::wstring(L"openmouse.learn")
                                         : p.substr(0, slash + 1) + L"openmouse.learn";
}

} // namespace

// Three lines: seat, signature, label. Written by the engine, read once (and
// removed) by the settings window the engine then wakes.
bool WriteLearnResult(int seat, const std::wstring& sig, const std::wstring& label) {
    std::wstring clean = label;
    for (wchar_t& c : clean) if (c == L'\n' || c == L'\r') c = L' ';
    return WriteFileUtf8(LearnPath(), std::to_wstring(seat) + L"\n" + sig + L"\n" + clean + L"\n");
}

bool ReadLearnResult(int& seat, std::wstring& sig, std::wstring& label) {
    const std::wstring path = LearnPath();
    const std::wstring text = ReadFileUtf16(path);
    DeleteFileW(path.c_str());
    if (text.empty()) return false;
    const size_t a = text.find(L'\n');
    if (a == std::wstring::npos) return false;
    const size_t b = text.find(L'\n', a + 1);
    if (b == std::wstring::npos) return false;
    const size_t c = text.find(L'\n', b + 1);
    seat  = _wtoi(Trim(text.substr(0, a)).c_str());
    sig   = Trim(text.substr(a + 1, b - a - 1));
    label = Trim(text.substr(b + 1, c == std::wstring::npos ? std::wstring::npos : c - b - 1));
    return !sig.empty();
}

// ---------------------------------------------------------------- logging

namespace {

// Opened by OpenLogFile(). The engine runs as a GUI process with no console,
// so without this the only trace is OutputDebugStringW, which needs a debugger
// attached to read. Diagnosing a system-wide input hook that way is not
// practical, hence the file.
HANDLE g_logFile = INVALID_HANDLE_VALUE;

// WriteConsoleW only works on a real console handle. When stdout is redirected
// to a pipe or file — which is what happens whenever the output is captured —
// it fails and the text is silently dropped. Fall back to a UTF-8 WriteFile.
void WriteWideTo(HANDLE h, const wchar_t* text, size_t len) {
    if (!h || h == INVALID_HANDLE_VALUE || len == 0) return;

    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        DWORD written = 0;
        WriteConsoleW(h, text, (DWORD)len, &written, nullptr);
        return;
    }

    const int need = WideCharToMultiByte(CP_UTF8, 0, text, (int)len,
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    std::string raw((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, (int)len, &raw[0], need, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(h, raw.data(), (DWORD)raw.size(), &written, nullptr);
}

} // namespace

// Appends to %APPDATA%\openMouse\openmouse.log. Safe to call more than once.
void OpenLogFile() {
    if (g_logFile != INVALID_HANDLE_VALUE) return;
    std::wstring path = Config::DefaultPath();
    const size_t slash = path.rfind(L'\\');
    path = (slash == std::wstring::npos) ? std::wstring(L"openmouse.log")
                                         : path.substr(0, slash + 1) + L"openmouse.log";
    // Start fresh once the log passes a few megabytes. This runs for hours at
    // a time and appends on every session; without a cap it grows without
    // bound and the interesting lines are unfindable.
    constexpr LONGLONG kMaxLogBytes = 4LL * 1024 * 1024;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        const LONGLONG size = ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (size > kMaxLogBytes) {
            const HANDLE fresh = OpenSafeFile(path, SafeOpen::Rewrite);   // empties it
            if (fresh != INVALID_HANDLE_VALUE) CloseHandle(fresh);
        }
    }

    // Shared for writing: the engine and a diagnostic mode (--monitor) may
    // have this open at once, and appends interleave safely.
    g_logFile = OpenSafeFile(path, SafeOpen::Append);
}

void CloseLogFile() {
    if (g_logFile != INVALID_HANDLE_VALUE) { CloseHandle(g_logFile); g_logFile = INVALID_HANDLE_VALUE; }
}

std::wstring SelfPath() {
    std::wstring p(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &p[0], (DWORD)p.size());
        if (n == 0) return L"";
        if (n < p.size()) { p.resize(n); return p; }   // n == size: cut short
        if (p.size() >= 32768) return L"";
        p.resize(p.size() * 2);
    }
}

void LogLine(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;

    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");

    const size_t len = wcslen(buf);
    WriteWideTo(GetStdHandle(STD_OUTPUT_HANDLE), buf, len);
    WriteWideTo(GetStdHandle(STD_OUTPUT_HANDLE), L"\r\n", 2);

    if (g_logFile != INVALID_HANDLE_VALUE) {
        WriteWideTo(g_logFile, buf, len);
        WriteWideTo(g_logFile, L"\r\n", 2);
    }
}

} // namespace om
