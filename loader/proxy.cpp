// A stand-in for one of the system DLLs the game imports (see tools/gen_proxy.py for which and why), loaded from the game's own folder
// because the folder of the exe is searched first. It passes every call on to the real DLL in the system folder (the export stubs in
// proxy_exports.asm) and loads stellaris_live2d.dll, which sits next to it, a moment after the game starts: the plugin then needs no injector.
//
// Nothing happens in any other process, or when a file named stellaris_live2d.disabled is next to the exe. The log (the plugin's own
// stellaris_live2d.log) says what the loader did.
#include <windows.h>
#include <cstdio>
#include <cwchar>

#include "proxy_dll.inc"

extern "C" FARPROC g_real[];  // the real exports, in the order of proxy_names.inc

namespace {

const char* const kNames[] = {
#include "proxy_names.inc"
};
constexpr size_t kCount = sizeof(kNames) / sizeof(kNames[0]);

}  // namespace

extern "C" FARPROC g_real[kCount] = {};

namespace {

HMODULE g_self = nullptr;

void LogLine(const wchar_t* dir, const char* text, DWORD error) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\stellaris_live2d.log", dir);
    if (FILE* f = _wfopen(path, L"a")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        if (error) fprintf(f, "[%02d:%02d:%02d] loader: %s (error %lu)\n", t.wHour, t.wMinute, t.wSecond, text, error);
        else fprintf(f, "[%02d:%02d:%02d] loader: %s\n", t.wHour, t.wMinute, t.wSecond, text);
        fclose(f);
    }
}

DWORD WINAPI LoadPlugin(LPVOID) {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(g_self, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return 0;
    *slash = 0;
    wchar_t flag[MAX_PATH], dll[MAX_PATH];
    swprintf_s(flag, L"%s\\stellaris_live2d.disabled", dir);
    swprintf_s(dll, L"%s\\stellaris_live2d.dll", dir);
    if (GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES) {
        LogLine(dir, "stellaris_live2d.disabled is present: not loading the plugin", 0);
        return 0;
    }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        LogLine(dir, "stellaris_live2d.dll is not next to the loader", 0);
        return 0;
    }
    Sleep(3000);  // let the game get through its start-up first
    if (GetModuleHandleW(L"stellaris_live2d.dll")) return 0;  // already there (injected by hand)
    if (LoadLibraryW(dll)) LogLine(dir, "stellaris_live2d.dll loaded", 0);
    else LogLine(dir, "could not load stellaris_live2d.dll", GetLastError());
    return 0;
}

bool InStellaris() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    return name && _wcsicmp(name + 1, L"stellaris.exe") == 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        // the real DLL, from the system folder by its full path (ours would be found by name)
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\");
        wcscat_s(path, PROXY_DLL_NAME);
        HMODULE real = LoadLibraryW(path);
        if (!real) return FALSE;
        for (size_t i = 0; i < kCount; ++i) g_real[i] = GetProcAddress(real, kNames[i]);
        if (InStellaris()) {
            if (HANDLE t = CreateThread(nullptr, 0, LoadPlugin, nullptr, 0, nullptr)) CloseHandle(t);
        }
    }
    return TRUE;
}
