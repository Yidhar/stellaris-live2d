// stellaris_live2d.dll entry: installs the portrait hook and follows stellaris_live2d.ini (next to stellaris.exe),
// re-reading it every 2 seconds so settings can be changed while the game runs. Statistics go to stellaris_live2d.log
// every 30 seconds and on every change.
//
// Unloading: never FreeLibrary this DLL from outside while the game runs; a game thread may be inside the detour.
// Signal the event Local\stellaris_live2d_unload_<pid> instead (scripts/l2dctl.py): the worker removes the hook, waits
// for in-flight calls to drain and unloads the DLL itself.
#include "live2d.hpp"
#include "portrait_registry.hpp"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
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
              "only_height=0\n"
              "; Live2D: draw a model into the render target of every visible portrait (0 = off). Needs core_dll and model.\n"
              "live2d=0\n"
              "; path of Live2DCubismCore.dll (Live2D's own, or a compatible one such as Purism Core); not shipped with this plugin\n"
              "core_dll=\n"
              "; path of the model's model3.json (one model), shown with the view_* values below\n"
              "model=\n"
              "; or several models, separated by ';': path, path|x,y,h (that view) or path|auto or path|auto:0.5 (a view worked out from the model: from the head down, that fraction of the figure's height; default 0.46).\n"
              "; Each portrait gets one of them, handed out in turn when the portrait is first seen.\n"
              "models=\n"
              "; the part of the model canvas that is shown, as fractions of the canvas: centre from the left, centre from the top, height\n"
              "view_x=0.44\n"
              "view_y=0.19\n"
              "view_h=0.26\n"
              "; how many times a second the model is advanced and redrawn\n"
              "fps=30\n"
              "; secondary motion (hair, clothes) from the model's physics3.json\n"
              "physics=1\n"
              "; mod root folders read as if they were enabled (development): the portrait registrations in them are used\n"
              "extra_mod_dirs=\n", f);
        fclose(f);
    }
}

std::string IniString(const char* key, const std::string& path) {
    static char buf[16384];
    GetPrivateProfileStringA("live2d", key, "", buf, sizeof buf, path.c_str());
    std::string v = buf;
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '"')) v.pop_back();
    size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t' || v[i] == '"')) ++i;
    return v.substr(i);
}

float IniFloat(const char* key, float fallback, const std::string& path) {
    const std::string v = IniString(key, path);
    return v.empty() ? fallback : (float)atof(v.c_str());
}

l2d::Settings ReadIni(const std::string& path) {
    l2d::Settings s;
    s.live2d = GetPrivateProfileIntA("live2d", "live2d", 0, path.c_str()) != 0;
    s.core_dll = IniString("core_dll", path);
    const float vx = IniFloat("view_x", 0.44f, path), vy = IniFloat("view_y", 0.19f, path), vh = IniFloat("view_h", 0.26f, path);
    // `model=` is one model with the view_* values; `models=` is a list: path|x,y,h or path|auto or just path (view_* values)
    const std::string single = IniString("model", path);
    if (!single.empty()) {
        l2d::Settings::ModelEntry e;
        e.path = single;
        e.view_x = vx; e.view_y = vy; e.view_h = vh;
        s.models.push_back(e);
    }
    const std::string list = IniString("models", path);
    for (size_t pos = 0; pos < list.size();) {
        size_t end = list.find(';', pos);
        if (end == std::string::npos) end = list.size();
        std::string item = list.substr(pos, end - pos);
        pos = end + 1;
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.pop_back();
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
        if (item.empty()) continue;
        l2d::Settings::ModelEntry e;
        e.view_x = vx; e.view_y = vy; e.view_h = vh;
        const size_t bar = item.find('|');
        e.path = item.substr(0, bar);
        if (bar != std::string::npos) {
            const std::string v = item.substr(bar + 1);
            if (v.rfind("auto", 0) == 0) {
                e.auto_view = true;
                if (v.size() > 5 && v[4] == ':') e.auto_body = (float)atof(v.c_str() + 5);
            } else {
                sscanf(v.c_str(), "%f,%f,%f", &e.view_x, &e.view_y, &e.view_h);
            }
        }
        s.models.push_back(e);
    }
    const std::string dirs = IniString("extra_mod_dirs", path);
    for (size_t pos = 0; pos < dirs.size();) {
        size_t end = dirs.find(';', pos);
        if (end == std::string::npos) end = dirs.size();
        std::string item = dirs.substr(pos, end - pos);
        pos = end + 1;
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.pop_back();
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
        if (!item.empty()) s.extra_mod_dirs.push_back(item);
    }
    s.fps = GetPrivateProfileIntA("live2d", "fps", 30, path.c_str());
    s.physics = GetPrivateProfileIntA("live2d", "physics", 1, path.c_str()) != 0;
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
    l2d::Registry registry;
    uint64_t registry_signature = 0;
    bool first = true;
    int ticks = 0;
    for (;;) {
        const l2d::Settings s = ReadIni(ini);
        // the mods' portrait registrations: scanned again when the playset or one of the scanned files changed
        bool registry_changed = false;
        const uint64_t signature = l2d::ScanRegistry(s.extra_mod_dirs, false).signature;
        if (first || signature != registry_signature) {
            registry = l2d::ScanRegistry(s.extra_mod_dirs, true);
            registry_signature = signature;
            registry_changed = true;
            for (const std::string& m : registry.messages) l2d::Log("registry: %s", m.c_str());
            l2d::Log("registry: %zu portrait key(s) registered by the enabled mods", registry.entries.size());
        }
        const bool changed = first || registry_changed || !(s == last);
        if (changed) {
            l2d::Apply(s, registry);
            l2d::Log("settings: test_pattern=%d only_size=%dx%d live2d=%d models=%zu fps=%d physics=%d", (int)s.test_pattern,
                     s.only_width, s.only_height, (int)s.live2d, s.models.size(), s.fps, (int)s.physics);
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
