#include "voice.hpp"

#include "live2d.hpp"

// miniaudio with only what a voice line needs: WASAPI playback and the decoders (WAV, FLAC, MP3 built in, Ogg Vorbis through
// stb_vorbis, which has to be declared before the implementation and defined after it)
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace l2d {

// A node between a sound and the output that measures what passes: the mean square of the last block, kept in an atomic.
struct LevelNode {
    ma_node_base base;
    std::atomic<float> level{ 0.0f };
};

static void LevelProcess(ma_node* node, const float** frames_in, ma_uint32* frame_count_in, float** frames_out, ma_uint32* frame_count_out) {
    auto* n = (LevelNode*)node;
    const ma_uint32 frames = *frame_count_in < *frame_count_out ? *frame_count_in : *frame_count_out;
    const ma_uint32 channels = ma_node_get_input_channels(node, 0);
    double sum = 0.0;
    for (ma_uint32 i = 0; i < frames * channels; ++i) {
        const float v = frames_in[0][i];
        sum += (double)v * v;
        frames_out[0][i] = v;
    }
    *frame_count_out = frames;
    n->level.store(frames ? (float)std::sqrt(sum / (double)(frames * channels)) : 0.0f);
}

static ma_node_vtable g_level_vtable = { LevelProcess, nullptr, 1, 1, 0 };

struct Voice::Impl {
    std::mutex mutex;
    ma_engine engine;
    bool running = false;
    struct Playing {
        const void* owner;
        std::unique_ptr<ma_sound> sound;
        std::unique_ptr<LevelNode> level;
    };
    std::vector<Playing> playing;

    // drops sounds that finished; with `owner`, also that owner's sound even if still going
    void Reap(const void* owner) {
        for (auto it = playing.begin(); it != playing.end();) {
            if (it->owner == owner || ma_sound_at_end(it->sound.get())) {
                ma_sound_uninit(it->sound.get());
                if (it->level) ma_node_uninit(&it->level->base, nullptr);
                it = playing.erase(it);
            } else {
                ++it;
            }
        }
    }
};

Voice::Voice() : impl_(new Impl) {}
Voice::~Voice() = default;

Voice& Voice::Get() {
    static Voice voice;
    return voice;
}

bool Voice::Start(float volume) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->running) {
        ma_engine_set_volume(&impl_->engine, volume);
        return true;
    }
    ma_engine_config config = ma_engine_config_init();
    const ma_result r = ma_engine_init(&config, &impl_->engine);
    if (r != MA_SUCCESS) {
        Log("voice: could not open the playback device (miniaudio error %d)", (int)r);
        return false;
    }
    ma_engine_set_volume(&impl_->engine, volume);
    impl_->running = true;
    Log("voice: playback device open, %u Hz, %u channels", ma_engine_get_sample_rate(&impl_->engine), ma_engine_get_channels(&impl_->engine));
    return true;
}

bool Voice::Running() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running;
}

void Voice::SetVolume(float volume) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->running) ma_engine_set_volume(&impl_->engine, volume);
}

bool Voice::Play(const void* owner, const std::filesystem::path& file, float gain) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running) return false;
    impl_->Reap(owner);
    auto sound = std::make_unique<ma_sound>();
    const ma_result r = ma_sound_init_from_file_w(&impl_->engine, file.c_str(), MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, sound.get());
    if (r != MA_SUCCESS) {
        Log("voice: cannot play %s (miniaudio error %d: missing file or a format that is not WAV, MP3, FLAC or Ogg Vorbis)", file.string().c_str(), (int)r);
        return false;
    }
    ma_sound_set_volume(sound.get(), gain);
    // a node in the sound's way to the output measures how loud it is, for the mouth
    auto level = std::make_unique<LevelNode>();
    const ma_uint32 channels = ma_engine_get_channels(&impl_->engine);
    ma_node_config node_config = ma_node_config_init();
    node_config.vtable = &g_level_vtable;
    node_config.pInputChannels = &channels;
    node_config.pOutputChannels = &channels;
    if (ma_node_init(ma_engine_get_node_graph(&impl_->engine), &node_config, nullptr, &level->base) == MA_SUCCESS) {
        ma_node_attach_output_bus(sound.get(), 0, &level->base, 0);
        ma_node_attach_output_bus(&level->base, 0, ma_engine_get_endpoint(&impl_->engine), 0);
    } else {
        level.reset();  // no lip sync, the line is still said
    }
    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        ma_sound_uninit(sound.get());
        if (level) ma_node_uninit(&level->base, nullptr);
        return false;
    }
    impl_->playing.push_back({ owner, std::move(sound), std::move(level) });
    return true;
}

float Voice::Level(const void* owner) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running) return 0.0f;
    for (const auto& p : impl_->playing)
        if (p.owner == owner && p.level && !ma_sound_at_end(p.sound.get())) return p.level->level.load();
    return 0.0f;
}

void Voice::Shutdown() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running) return;
    for (auto& p : impl_->playing) {
        ma_sound_uninit(p.sound.get());
        if (p.level) ma_node_uninit(&p.level->base, nullptr);
    }
    impl_->playing.clear();
    ma_engine_uninit(&impl_->engine);
    impl_->running = false;
}

} // namespace l2d
