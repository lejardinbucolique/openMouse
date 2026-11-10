// openMouse — start with Windows
// SPDX-License-Identifier: MIT
//
// "Start with Windows" is a scheduled task that fires when the user logs on and
// runs openmouse.exe with the highest run level available to that user. A Run
// registry key would start it without elevation, and a pointer engine that is
// not elevated cannot steer windows that are (Task Manager, UAC-elevated
// tools). A task starts elevated at logon without any prompt; the only prompt
// is the one when the task is created, which is the explicit choice.
//
// Creating or deleting a task with the highest run level needs administrator
// rights. The elevated engine does it on request of the settings window; when
// the engine is not running or not elevated, the settings window starts a
// one-shot elevated copy of this program (--autostart=on / --autostart=off).
#include "openmouse.h"
#include <taskschd.h>

#ifdef _MSC_VER
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

namespace om {

namespace {

constexpr wchar_t kTaskName[] = L"openMouse";

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
    // S_FALSE (already initialised here) and RPC_E_CHANGED_MODE (initialised
    // with another model by the caller) both leave COM usable.
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// The root folder of the Task Scheduler. The caller releases both.
HRESULT OpenRoot(ITaskService*& svc, ITaskFolder*& root) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, (void**)&svc);
    if (FAILED(hr)) return hr;
    VARIANT none;
    VariantInit(&none);
    hr = svc->Connect(none, none, none, none);
    if (FAILED(hr)) return hr;
    BSTR path = SysAllocString(L"\\");
    hr = svc->GetFolder(path, &root);
    SysFreeString(path);
    return hr;
}

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
        case L'&':  o += L"&amp;";  break;
        case L'<':  o += L"&lt;";   break;
        case L'>':  o += L"&gt;";   break;
        case L'"':  o += L"&quot;"; break;
        case L'\'': o += L"&apos;"; break;
        default:    o += c;
        }
    }
    return o;
}

// DOMAIN\name of the account this process runs as, read from its token: the
// environment is the caller's to set, and the task runs under this name.
std::wstring UserFromToken() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"";
    std::wstring out;
    DWORD need = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &need);
    if (need) {
        std::vector<BYTE> buf(need);
        if (GetTokenInformation(token, TokenUser, buf.data(), need, &need)) {
            wchar_t name[256], domain[256];
            DWORD nl = 256, dl = 256;
            SID_NAME_USE use;
            if (LookupAccountSidW(nullptr, reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid,
                                  name, &nl, domain, &dl, &use) && *name)
                out = *domain ? std::wstring(domain) + L"\\" + name : std::wstring(name);
        }
    }
    CloseHandle(token);
    return out;
}

std::wstring CurrentUser() {
    const std::wstring fromToken = UserFromToken();
    if (!fromToken.empty()) return fromToken;
    wchar_t domain[256] = L"", user[256] = L"";
    GetEnvironmentVariableW(L"USERDOMAIN", domain, 256);
    GetEnvironmentVariableW(L"USERNAME", user, 256);
    return *domain ? std::wstring(domain) + L"\\" + user : std::wstring(user);
}

std::wstring TaskXml() {
    const std::wstring exe = SelfPath();
    if (exe.empty()) return L"";
    std::wstring dir = exe;
    const size_t slash = dir.rfind(L'\\');
    if (slash != std::wstring::npos) dir.resize(slash);

    const std::wstring user = XmlEscape(CurrentUser());
    // Only this user's logon starts it; never on battery-only refusal, never
    // stopped after a time limit, never a second copy beside a running one.
    return
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Description>openMouse: one pointer per mouse, started at logon.</Description></RegistrationInfo>"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + user + L"</UserId></LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"Author\"><UserId>" + user + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings>"
        L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowHardTerminate>true</AllowHardTerminate>"
        L"<StartWhenAvailable>false</StartWhenAvailable>"
        L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand>"
        L"<Enabled>true</Enabled><Hidden>false</Hidden>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
        L"<Priority>4</Priority>"
        L"</Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>" + XmlEscape(exe) + L"</Command>"
        L"<Arguments>--startup</Arguments><WorkingDirectory>" + XmlEscape(dir) + L"</WorkingDirectory></Exec></Actions>"
        L"</Task>";
}

} // namespace

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

int AutostartState() {
    ComScope com;
    if (!com.ok()) return -1;
    ITaskService* svc = nullptr;
    ITaskFolder* root = nullptr;
    int state = -1;
    if (SUCCEEDED(OpenRoot(svc, root))) {
        BSTR name = SysAllocString(kTaskName);
        IRegisteredTask* task = nullptr;
        const HRESULT hr = root->GetTask(name, &task);
        SysFreeString(name);
        if (SUCCEEDED(hr))                                      state = 1;
        else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) state = 0;
        Release(task);   // anything else (access denied...) stays unknown
    }
    Release(root);
    Release(svc);
    return state;
}

bool AutostartInstall() {
    const std::wstring xml = TaskXml();
    if (xml.empty()) return false;
    ComScope com;
    if (!com.ok()) return false;
    ITaskService* svc = nullptr;
    ITaskFolder* root = nullptr;
    bool ok = false;
    HRESULT hr = OpenRoot(svc, root);
    if (SUCCEEDED(hr)) {
        BSTR name = SysAllocString(kTaskName);
        BSTR def  = SysAllocString(xml.c_str());
        VARIANT none;
        VariantInit(&none);
        IRegisteredTask* task = nullptr;
        hr = root->RegisterTask(name, def, TASK_CREATE_OR_UPDATE, none, none,
                                TASK_LOGON_INTERACTIVE_TOKEN, none, &task);
        SysFreeString(name);
        SysFreeString(def);
        ok = SUCCEEDED(hr);
        Release(task);
    }
    LogLine(ok ? L"[autostart] task created" : L"[autostart] could not create the task: 0x%08lX", (unsigned long)hr);
    Release(root);
    Release(svc);
    return ok;
}

bool AutostartRemove() {
    ComScope com;
    if (!com.ok()) return false;
    ITaskService* svc = nullptr;
    ITaskFolder* root = nullptr;
    bool ok = false;
    HRESULT hr = OpenRoot(svc, root);
    if (SUCCEEDED(hr)) {
        BSTR name = SysAllocString(kTaskName);
        hr = root->DeleteTask(name, 0);
        SysFreeString(name);
        ok = SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    LogLine(ok ? L"[autostart] task removed" : L"[autostart] could not remove the task: 0x%08lX", (unsigned long)hr);
    Release(root);
    Release(svc);
    return ok;
}

int RunAutostartHelper(bool enable) {
    LogLine(L"[autostart] one-shot helper: %s", enable ? L"enable" : L"disable");
    return (enable ? AutostartInstall() : AutostartRemove()) ? 0 : 1;
}

int RunAutostartDryRun() {
    LogLine(L"[autostart] dry run: nothing is registered, nothing is deleted");
    LogLine(L"[autostart] this process is %s", IsElevated() ? L"elevated" : L"not elevated (creating the task would be refused)");
    const int state = AutostartState();
    LogLine(L"[autostart] task \"%s\" now: %s", kTaskName,
            state == 1 ? L"present" : state == 0 ? L"absent" : L"unknown (not readable from here)");
    std::wstring xml = TaskXml();
    if (xml.empty()) { LogLine(L"[autostart] could not build the definition"); return 1; }
    LogLine(L"[autostart] enable would call ITaskFolder::RegisterTask(\"%s\", xml, TASK_CREATE_OR_UPDATE, TASK_LOGON_INTERACTIVE_TOKEN) with:", kTaskName);
    size_t at;
    while ((at = xml.find(L"><")) != std::wstring::npos) xml.replace(at, 2, L">\n<");
    for (size_t from = 0; from < xml.size();) {
        size_t nl = xml.find(L'\n', from);
        if (nl == std::wstring::npos) nl = xml.size();
        LogLine(L"    %s", xml.substr(from, nl - from).c_str());
        from = nl + 1;
    }
    LogLine(L"[autostart] disable would call ITaskFolder::DeleteTask(\"%s\", 0)", kTaskName);
    return 0;
}

} // namespace om
