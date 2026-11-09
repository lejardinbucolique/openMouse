// openMouse — raw input device enumeration
// SPDX-License-Identifier: MIT
#include "openmouse.h"
#include <algorithm>

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

} // namespace om
