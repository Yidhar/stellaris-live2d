#pragma once
// Puts Live2D frames into portrait render targets. Configure and Shutdown run on the plugin's worker thread; Paint runs
// on the game's render thread, from inside the portrait hook.
#include "live2d.hpp"
#include "portrait_registry.hpp"

#include <d3d11.h>
#include <memory>
#include <string>

namespace l2d {

enum class PaintResult {
    Painted,
    Skipped,  // no model is registered for this portrait: the engine's own picture stays
    Failed,
};

class Live2DPainter {
public:
    Live2DPainter();
    ~Live2DPainter();

    // Loads the Cubism Core and the models the registry names (or, when it names none, those of `s.models`) when they
    // changed (file and image work, a few tens of milliseconds per model, on the calling thread), and takes the frame rate and
    // physics switch. With live2d off or nothing to load, unloads everything.
    void Configure(const Settings& s, const Registry& registry);
    bool Ready() const;

    // After the engine rendered `target` (the render target of `portrait`, which shows the portrait with key `key`), replaces
    // its content with a frame of the model registered for that key. In the fallback mode (no registrations) a portrait object
    // gets a model the first time it is seen, the models handed out in turn. Each model advances and redraws at most `fps`
    // times a second, and only while one of its portraits is on screen; portraits of one model and size share the frame.
    PaintResult Paint(const void* portrait, const char* key, ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& desc);

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
