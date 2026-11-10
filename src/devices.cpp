// openMouse — raw input device enumeration
// SPDX-License-Identifier: MIT
#include "openmouse.h"
#include <hidsdi.h>
#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "hid.lib")
#endif

namespace om {

std::wstring DevicePathFromHandle(HANDLE h) {
    if (!h) return L"";
    UINT len = 0;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, nullptr, &len) != 0 || len == 0)
        return L"";
    std::wstring buf(len + 1, L'\0');
    UINT written = len + 1;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, &buf[0], &written) == (UINT)-1)
        return L"";
    buf.resize(wcslen(buf.c_str()));
    return buf;
}

// Raw input hands us an interface path of the form
//     \\?\HID#VID_046D&PID_C52B&MI_01#7&2f8e7b0d&0&0000#{4d1e55b2-...}
// The corresponding registry key holding a human-readable description is
//     HKLM\SYSTEM\CurrentControlSet\Enum\HID\VID_046D&PID_C52B&MI_01\7&2f8e7b0d&0&0000
// so we strip the \\?\ prefix, drop the trailing interface GUID, and swap
// '#' separators for backslashes.
static std::wstring PathToEnumKey(const std::wstring& devPath) {
    std::wstring s = devPath;
    if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);

    // Drop the trailing "#{guid}" component if present.
    size_t brace = s.find(L'{');
    if (brace != std::wstring::npos) {
        size_t hash = s.rfind(L'#', brace);
        if (hash != std::wstring::npos) s = s.substr(0, hash);
    }
    std::replace(s.begin(), s.end(), L'#', L'\\');
    return L"SYSTEM\\CurrentControlSet\\Enum\\" + s;
}

std::wstring FriendlyNameFromPath(const std::wstring& path) {
    const std::wstring key = PathToEnumKey(path);
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return L"";

    auto readValue = [&](const wchar_t* name) -> std::wstring {
        DWORD type = 0, cb = 0;
        if (RegQueryValueExW(hk, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS ||
            type != REG_SZ || cb == 0)
            return L"";
        std::wstring out(cb / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(hk, name, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&out[0]), &cb) != ERROR_SUCCESS)
            return L"";
        out.resize(wcslen(out.c_str()));
        return out;
    };

    // FriendlyName is nicer when present; DeviceDesc always is, but is
    // prefixed with a resource path we have to trim.
    std::wstring name = readValue(L"FriendlyName");
    if (name.empty()) {
        name = readValue(L"DeviceDesc");
        size_t semi = name.rfind(L';');
        if (semi != std::wstring::npos) name = name.substr(semi + 1);
    }
    RegCloseKey(hk);
    return name;
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// "\\?\HID#VID_046D&PID_C077#6&1b42cbdd&0&0000#{...}" -> the three '#'
// separated components after the prefix: class, hardware id, instance id.
static std::wstring PathComponent(const std::wstring& path, int index) {
    std::wstring s = path;
    if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
    size_t start = 0;
    for (int i = 0; i < index; ++i) {
        start = s.find(L'#', start);
        if (start == std::wstring::npos) return L"";
        ++start;
    }
    size_t end = s.find(L'#', start);
    return s.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

std::wstring StableKey(const std::wstring& path) {
    const std::wstring cls = PathComponent(path, 0), hw = PathComponent(path, 1);
    const std::wstring key = Lower(cls + L"#" + hw);
    if (cls.empty() || hw.empty() || key.find(L"vid_") == std::wstring::npos) return L"";
    return key;
}

std::wstring HardwareKey(const std::wstring& path) {
    const std::wstring cls = PathComponent(path, 0), hw = PathComponent(path, 1);
    if (cls.empty() || hw.empty()) return L"";
    return Lower(cls + L"#" + hw);
}

std::wstring HardwareBase(const std::wstring& path) {
    std::wstring key = HardwareKey(path);
    for (const wchar_t* tag : { L"&mi_", L"&col" }) {
        const size_t at = key.find(tag);
        if (at != std::wstring::npos) key.resize(at);
    }
    return key;
}

bool IsAutoSeatCandidate(const std::wstring& path) {
    const std::wstring p = Lower(path);
    if (p.find(L"acpi#") != std::wstring::npos || p.find(L"acpi\\") != std::wstring::npos)
        return false;                                  // integrated touchpad, PS/2
    return p.find(L"vid_") != std::wstring::npos ||    // USB
           p.find(L"vid&") != std::wstring::npos;      // Bluetooth
}

bool IsIntegratedPointer(const std::wstring& path) {
    return !IsAutoSeatCandidate(path);
}

// ---------------------------------------------------------------- make of a mouse

namespace {

// Makers of mice by USB vendor id. Source: the USB-IF vendor list as kept in
// the usb.ids database (http://www.linux-usb.org/usb.ids, also
// https://usb-ids.gowdy.us/). Only makers who sell mice under their own name
// are listed: the chip makers and contract makers whose id sits inside
// countless unbranded mice (PixArt 093A, Sunplus 1BCF, Holtek 04D9, Sino
// Wealth 258A, Primax 0461, Chicony 04F2...) are left out on purpose, they
// say nothing about the mouse in the hand.
struct Maker { USHORT vid; const wchar_t* name; };
const Maker kMakers[] = {
    { 0x046D, L"Logitech" },   { 0x045E, L"Microsoft" },  { 0x1532, L"Razer" },
    { 0x1038, L"SteelSeries" },{ 0x1B1C, L"Corsair" },    { 0x0B05, L"ASUS" },
    { 0x413C, L"Dell" },       { 0x03F0, L"HP" },         { 0x17EF, L"Lenovo" },
    { 0x05AC, L"Apple" },      { 0x0458, L"Genius" },     { 0x09DA, L"A4Tech" },
    { 0x046A, L"Cherry" },     { 0x056E, L"Elecom" },     { 0x24AE, L"Rapoo" },
    { 0x1E7D, L"Roccat" },     { 0x2516, L"Cooler Master" },{ 0x3367, L"Endgame Gear" },
    { 0x0951, L"HyperX" },     { 0x04B3, L"IBM" },
};

std::wstring Lower2(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// The vendor id in an interface path: "vid_046d" (USB) or "vid&0002046d"
// (Bluetooth, whose first four digits say who assigned the id: 0002 is USB-IF).
// 0 when there is none.
USHORT VendorOf(const std::wstring& path) {
    const std::wstring p = Lower2(path);
    auto hex = [&](size_t at, size_t n) -> long {
        if (at + n > p.size()) return -1;
        long v = 0;
        for (size_t i = 0; i < n; ++i) {
            const wchar_t c = p[at + i];
            if (c >= L'0' && c <= L'9') v = v * 16 + (c - L'0');
            else if (c >= L'a' && c <= L'f') v = v * 16 + (c - L'a' + 10);
            else return -1;
        }
        return v;
    };
    size_t at = p.find(L"vid_");
    if (at != std::wstring::npos) { const long v = hex(at + 4, 4); return v > 0 ? (USHORT)v : 0; }
    at = p.find(L"vid&");
    if (at != std::wstring::npos && hex(at + 4, 4) == 2) { const long v = hex(at + 8, 4); return v > 0 ? (USHORT)v : 0; }
    return 0;
}

// A name that tells nothing about who made the mouse: only words like "HID",
// "USB", "mouse", "standard", in English or French, or numbers.
bool IsGenericName(const std::wstring& name) {
    static const wchar_t* const kGeneric[] = {
        L"hid", L"compliant", L"mouse", L"mice", L"usb", L"input", L"device", L"devices",
        L"optical", L"laser", L"wireless", L"bluetooth", L"receiver", L"dongle", L"standard",
        L"generic", L"unknown", L"system", L"types", L"type", L"pointing", L"compatible",
        L"gaming", L"game", L"office", L"composite", L"ps", L"2", L"the", L"and", L"with",
        L"souris", L"peripherique", L"périphérique", L"conforme", L"entree", L"entrée", L"de",
        L"pointage", L"sans", L"fil", L"optique", L"standards", L"système", L"systeme",
    };
    std::wstring word;
    auto flush = [&]() -> bool {
        if (word.empty()) return true;
        const std::wstring w = Lower2(word);
        word.clear();
        bool numeric = true;
        for (wchar_t c : w) if (c < L'0' || c > L'9') { numeric = false; break; }
        if (numeric) return true;
        // "2.4g", "24ghz", "5g"
        if (w.size() >= 2 && w.back() == L'g' && w.find_first_not_of(L"0123456789g") == std::wstring::npos) return true;
        if (w.size() >= 4 && w.compare(w.size() - 3, 3, L"ghz") == 0) return true;
        for (const wchar_t* g : kGeneric) if (w == g) return true;
        return false;
    };
    for (wchar_t c : name) {
        if (iswalnum(c)) { word.push_back(c); continue; }
        if (!flush()) return false;
    }
    if (!flush()) return false;
    return true;   // every word was generic, or there was none
}

// Trims, drops control characters and the legal form ("Logitech, Inc." ->
// "Logitech"), and bounds the length: the text is shown in a title.
std::wstring CleanMaker(std::wstring s) {
    std::wstring out;
    for (wchar_t c : s) out.push_back(c < 32 ? L' ' : c);
    auto trim = [](std::wstring& t) {
        const size_t a = t.find_first_not_of(L" ,.;");
        if (a == std::wstring::npos) { t.clear(); return; }
        t = t.substr(a, t.find_last_not_of(L" ,.;") - a + 1);
    };
    trim(out);
    static const wchar_t* const kLegal[] = { L" inc", L" corp", L" corporation", L" co", L" ltd",
                                             L" gmbh", L" aps", L" llc", L" ag", L" sa", L" s.a", L" b.v" };
    for (bool again = true; again; ) {
        again = false;
        const std::wstring low = Lower2(out);
        for (const wchar_t* l : kLegal) {
            const size_t n = wcslen(l);
            if (low.size() > n && low.compare(low.size() - n, n, l) == 0) {
                out.resize(out.size() - n);
                trim(out);
                again = true;
                break;
            }
        }
    }
    if (out.size() > 32) { out.resize(32); trim(out); }
    return out;
}

// A string the device reports about itself. Opening an interface with no
// access rights at all is enough to ask, and moves nothing.
struct HidStrings { std::wstring maker, product; };

HidStrings ReadHidStrings(const std::wstring& path) {
    HidStrings r;
    // Only what raw input listed as a HID interface is opened, never another
    // kind of path that the configuration file might carry.
    if (path.size() < 8 || _wcsnicmp(path.c_str(), L"\\\\?\\HID#", 8) != 0) return r;
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    wchar_t buf[128] = {};
    if (HidD_GetManufacturerString(h, buf, sizeof(buf) - sizeof(wchar_t))) r.maker = buf;
    wchar_t buf2[128] = {};
    if (HidD_GetProductString(h, buf2, sizeof(buf2) - sizeof(wchar_t))) r.product = buf2;
    CloseHandle(h);
    return r;
}

} // namespace

std::wstring MouseBrand(const std::wstring& path, bool plugged) {
    if (plugged) {
        const HidStrings s = ReadHidStrings(path);
        const std::wstring maker = CleanMaker(s.maker);
        if (!maker.empty() && !IsGenericName(maker)) return maker;
    }
    if (const USHORT vid = VendorOf(path))
        for (const Maker& m : kMakers)
            if (m.vid == vid) return m.name;
    if (plugged) {
        const HidStrings s = ReadHidStrings(path);
        const std::wstring product = CleanMaker(s.product);
        if (!product.empty() && !IsGenericName(product)) return product;
    }
    const std::wstring win = CleanMaker(FriendlyNameFromPath(path));
    if (!win.empty() && !IsGenericName(win)) return win;
    return L"";
}

std::vector<DeviceInfo> EnumerateDevices() {
    std::vector<DeviceInfo> result;

    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        return result;

    std::vector<RAWINPUTDEVICELIST> list(count);
    UINT got = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (got == (UINT)-1) return result;
    list.resize(got);

    for (const auto& d : list) {
        if (d.dwType != RIM_TYPEMOUSE && d.dwType != RIM_TYPEKEYBOARD)
            continue;

        DeviceInfo info;
        info.handle = d.hDevice;
        info.type   = d.dwType;
        info.path   = DevicePathFromHandle(d.hDevice);
        if (info.path.empty()) continue;

        // Terminal Services and injected-input pseudo devices show up here and
        // must not be bound to a seat — they are not physical hardware.
        if (info.path.find(L"RDP_MOU") != std::wstring::npos ||
            info.path.find(L"RDP_KBD") != std::wstring::npos)
            continue;

        info.friendlyName = FriendlyNameFromPath(info.path);
        if (info.friendlyName.empty())
            info.friendlyName = (d.dwType == RIM_TYPEMOUSE) ? L"Mouse" : L"Keyboard";

        result.push_back(std::move(info));
    }
    return result;
}

bool IsExtendedVk(UINT vk) {
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
    case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_SNAPSHOT:
        return true;
    }
    return false;
}

std::vector<DeviceInfo> EnumerateHidDevices() {
    std::vector<DeviceInfo> result;

    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        return result;
    std::vector<RAWINPUTDEVICELIST> list(count);
    UINT got = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (got == (UINT)-1) return result;
    list.resize(got);

    for (const auto& d : list) {
        if (d.dwType != RIM_TYPEHID) continue;

        DeviceInfo info;
        info.handle = d.hDevice;
        info.type   = d.dwType;
        info.path   = DevicePathFromHandle(d.hDevice);
        if (info.path.empty()) continue;

        RID_DEVICE_INFO di{};
        di.cbSize = sizeof(di);
        UINT size = sizeof(di);
        if (GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICEINFO, &di, &size) == (UINT)-1) continue;
        info.page = di.hid.usUsagePage;
        info.friendlyName = FriendlyNameFromPath(info.path);
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace om
