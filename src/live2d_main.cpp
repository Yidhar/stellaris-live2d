// stellaris_live2d.dll entry: installs the portrait hook and follows stellaris_live2d.ini (next to stellaris.exe),
// re-reading it every 2 seconds so settings can be changed while the game runs. Statistics go to stellaris_live2d.log
// every 30 seconds and on every change.
//
// Unloading: never FreeLibrary this DLL from outside while the game runs; a game thread may be inside the detour.
// Signal the event Local\stellaris_live2d_unload_<pid> instead (scripts/l2dctl.py): the worker removes the hook, waits
// for in-flight calls to drain and unloads the DLL itself.
#include "live2d.hpp"

#include <windows.h>
#include <cstdio>
#include <string>

namespace {

HMODULE g_module = nullptr;
HANDLE g_unload_event = nullptr;

std::string IniPath() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    return s.substr(0, s.find_last_of("\\/") + 1) + "stellaris_live2d.ini";
}

void WriteDefaultIni(const std::string& path) {
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (FILE* f = fopen(path.c_str(), "w")) {
        fputs("; stellaris_live2d.dll settings, re-read every 2 seconds while the game runs.\n"
              "[live2d]\n"
              "; proof of concept: paint a test pattern into the render target of every visible portrait (0 = off)\n"
              "test_pattern=0\n"
              "; only paint render targets of exactly this size (0 = every size), e.g. 575x380 for the character portraits\n"
              "only_width=0\n"
              "only_height=0\n", f);
        fclose(f);
    }
}

l2d::Settings ReadIni(const std::string& path) {
    l2d::Settings s;
    s.test_pattern = GetPrivateProfileIntA("live2d", "test_pattern", 0, path.c_str()) != 0;
    s.only_width = GetPrivateProfileIntA("live2d", "only_width", 0, path.c_str());
    s.only_height = GetPrivateProfileIntA("live2d", "only_height", 0, path.c_str());
    return s;
}

[[noreturn]] void Finish() {
    l2d::Uninstall();
    l2d::Log("stellaris_live2d.dll unloading");
    CloseHandle(g_unload_event);
    FreeLibraryAndExitThread(g_module, 0);
}

DWORD WINAPI Worker(LPVOID) {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    l2d::Log("stellaris_live2d.dll loaded, image base 0x%llX", (unsigned long long)base);
    if (!l2d::Install(base)) {
        l2d::Log("hooks not installed; the DLL stays idle");
        WaitForSingleObject(g_unload_event, INFINITE);
        Finish();
    }
    const std::string ini = IniPath();
    WriteDefaultIni(ini);
    l2d::Settings last;
    bool first = true;
    int ticks = 0;
    for (;;) {
        const l2d::Settings s = ReadIni(ini);
        const bool changed = first || !(s == last);
        if (changed) {
            l2d::Apply(s);
            l2d::Log("settings: test_pattern=%d only_size=%dx%d", (int)s.test_pattern, s.only_width, s.only_height);
            last = s;
            first = false;
        }
        if (changed || ++ticks % 15 == 0) l2d::Log("%s", l2d::StatsLine().c_str());
        if (WaitForSingleObject(g_unload_event, 2000) == WAIT_OBJECT_0) break;
    }
    l2d::Log("unload requested: %s", l2d::StatsLine().c_str());
    Finish();
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(module);
        g_module = module;
        char name[64];
        wsprintfA(name, "Local\\stellaris_live2d_unload_%lu", GetCurrentProcessId());
        g_unload_event = CreateEventA(nullptr, TRUE, FALSE, name);
        if (!g_unload_event) return FALSE;
        // The worker releases the injector's reference with FreeLibraryAndExitThread.
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        break;
    }
    case DLL_PROCESS_DETACH:
        l2d::Log("stellaris_live2d.dll unloaded");
        break;
    }
    return TRUE;
}
