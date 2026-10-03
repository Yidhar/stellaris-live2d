#pragma once
// Puts Live2D frames into portrait render targets. Configure and Shutdown run on the plugin's worker thread; Paint runs
// on the game's render thread, from inside the portrait hook.
#include "live2d.hpp"

#include <d3d11.h>
#include <memory>
#include <string>

namespace l2d {

class Live2DPainter {
public:
    Live2DPainter();
    ~Live2DPainter();

    // Loads the Cubism Core and the models when their paths changed (file and image work, a few tens of milliseconds per
    // model, on the calling thread), and takes the frame rate and physics switch. With live2d off or nothing to load,
    // unloads everything.
    void Configure(const Settings& s);
    bool Ready() const;

    // After the engine rendered `target` (the render target of `portrait`), replaces its content with a frame of that
    // portrait's model. A portrait gets its model the first time it is seen: the models are handed out in turn. Each model
    // advances and redraws at most `fps` times a second, and only while one of its portraits is on screen; portraits of one
    // model and size share the frame.
    bool Paint(const void* portrait, ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& desc);

    // Releases everything. Only once the portrait hook is removed and no Paint can be running.
    void Shutdown();

    // counters for the statistics line
    std::string Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Live2DPainter& Painter();

} // namespace l2d
