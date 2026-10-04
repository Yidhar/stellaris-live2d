#pragma once
// Plays the voice lines of motions: one sound per owner (a loaded model's slot), a new one cuts the old one off. Decoding of WAV,
// MP3, FLAC and Ogg Vorbis files is done by miniaudio while the sound streams from the file, on miniaudio's own thread.
#include <filesystem>
#include <memory>

namespace l2d {

class Voice {
public:
    static Voice& Get();

    // Opens the default playback device (a few tens of milliseconds: call it from the worker thread, not the render thread).
    // `volume` is the master volume, 0..1. False when no device could be opened; Play then does nothing.
    bool Start(float volume);
    bool Running() const;
    void SetVolume(float volume);

    // Starts the file (louder or quieter by `gain`) for `owner`, cutting off what that owner was still saying. Returns at once.
    bool Play(const void* owner, const std::filesystem::path& file, float gain);

    // Stops everything and closes the device. Only once nothing can call Play any more.
    void Shutdown();

private:
    Voice();
    ~Voice();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace l2d
