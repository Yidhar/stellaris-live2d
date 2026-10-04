// l2d_voice_test: plays a sound file through the plugin's voice playback outside the game and prints the level the lip sync would see.
//   l2d_voice_test <file.wav|mp3|flac|ogg> [volume]
#include "live2d.hpp"
#include "voice.hpp"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace l2d {
void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
} // namespace l2d

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: l2d_voice_test <file> [volume 0..1]\n");
        return 2;
    }
    const float volume = argc > 2 ? (float)atof(argv[2]) : 0.8f;
    l2d::Voice& voice = l2d::Voice::Get();
    if (!voice.Start(volume)) return 1;
    int owner = 0;
    if (!voice.Play(&owner, std::filesystem::u8path(argv[1]), 1.0f)) return 1;
    float peak = 0.0f;
    int frames_with_sound = 0;
    for (int i = 0; i < 400; ++i) {  // up to 8 s at 50 Hz
        Sleep(20);
        const float level = voice.Level(&owner);
        if (level > 0.002f) ++frames_with_sound;
        if (level > peak) peak = level;
        if (i % 5 == 0) printf("level %.3f %s\n", level, std::string((size_t)(level * 60), '#').c_str());
        if (i > 10 && level == 0.0f && frames_with_sound > 5) {  // finished
            Sleep(100);
            if (voice.Level(&owner) == 0.0f) break;
        }
    }
    printf("peak level %.3f, %d of the samples above the mouth threshold\n", peak, frames_with_sound);
    voice.Shutdown();
    return peak > 0.01f ? 0 : 3;
}
