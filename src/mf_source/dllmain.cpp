// Include initguid.h BEFORE our header so DEFINE_GUID actually defines the GUID
// (not just declares it). This must happen in exactly one translation unit.
#include <initguid.h>
#include "FluxMicMediaSource.h"

#include <windows.h>
#include <new>
#include <string>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

// Module instance handle
static HMODULE g_hModule = nullptr;
static LONG g_serverLocks = 0;

// Global class factory (static lifetime)
static FluxMic::FluxMicMediaSourceFactory g_ClassFactory;

// CLSID string for registry
static const wchar_t* kCLSIDString = L"{ED9215F3-52D5-4E94-8AC2-B2D31F0C448A}";
static const wchar_t* kFriendlyName = L"FluxMic Camera Source";

// File-based debug log (same as source files)
static void DllDbgLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    CreateDirectoryA("C:\\ProgramData\\FluxMic", nullptr);
    FILE* f = fopen("C:\\ProgramData\\FluxMic\\mf_cam_debug.log", "a");
    if (f) {
        // Also log the process ID so we can tell which process loaded us
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "[PID=%lu] ", GetCurrentProcessId());
        fprintf(f, "%s%s", prefix, buf);
        fflush(f);
        fclose(f);
    }
}

// ============================================================================
// DLL Entry Point
// ============================================================================

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID /*lpvReserved*/) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hInstDLL;
        DisableThreadLibraryCalls(hInstDLL);
        DllDbgLog("[FluxMic] DllMain(DLL_PROCESS_ATTACH)\n");
        break;
    case DLL_PROCESS_DETACH:
        DllDbgLog("[FluxMic] DllMain(DLL_PROCESS_DETACH)\n");
        break;
    }
    return TRUE;
}

// ============================================================================
// COM Exports
// ============================================================================

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    DllDbgLog("[FluxMic] DllGetClassObject() called\n");
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (rclsid == FluxMic::CLSID_FluxMicMediaSource) {
        DllDbgLog("[FluxMic] DllGetClassObject() -> our CLSID matched\n");
        return g_ClassFactory.QueryInterface(riid, ppv);
    }

    DllDbgLog("[FluxMic] DllGetClassObject() -> CLASS_E_CLASSNOTAVAILABLE\n");
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
    return (g_serverLocks == 0) ? S_OK : S_FALSE;
}

// ============================================================================
// COM Registration (regsvr32 support)
// ============================================================================

static std::wstring GetModulePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, path, MAX_PATH);
    return path;
}

STDAPI DllRegisterServer() {
    std::wstring modulePath = GetModulePath();
    HRESULT hr = S_OK;

    // Register CLSID under HKLM\Software\Classes\CLSID\{...}
    std::wstring keyPath = L"Software\\Classes\\CLSID\\";
    keyPath += kCLSIDString;

    HKEY hKey = nullptr;
    LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(),
                                  0, nullptr, REG_OPTION_NON_VOLATILE,
                                  KEY_WRITE, nullptr, &hKey, nullptr);
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);

    // Set default value = friendly name
    RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(kFriendlyName),
                   (DWORD)((wcslen(kFriendlyName) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);

    // Register InprocServer32
    std::wstring inprocPath = keyPath + L"\\InprocServer32";
    result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, inprocPath.c_str(),
                             0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_WRITE, nullptr, &hKey, nullptr);
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);

    // Set default value = DLL path
    RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(modulePath.c_str()),
                   (DWORD)((modulePath.length() + 1) * sizeof(wchar_t)));

    // Set ThreadingModel = Both
    const wchar_t* threadingModel = L"Both";
    RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(threadingModel),
                   (DWORD)((wcslen(threadingModel) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);

    return S_OK;
}

STDAPI DllUnregisterServer() {
    std::wstring keyPath = L"Software\\Classes\\CLSID\\";
    keyPath += kCLSIDString;

    // Delete InprocServer32 subkey first
    std::wstring inprocPath = keyPath + L"\\InprocServer32";
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, inprocPath.c_str());

    // Delete CLSID key
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, keyPath.c_str());

    return S_OK;
}
