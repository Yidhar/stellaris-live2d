// Offline checks of what needs neither the game nor the Cubism Core: the script reader, the portrait registry, DDS reading, mip chains and
// the framing maths. Plain asserts, no framework; run through ctest (build the l2d_tests target) or directly.
#include "dds.hpp"
#include "live2d_model.hpp"
#include "pdx_script.hpp"
#include "portrait_registry.hpp"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

namespace fs = std::filesystem;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                            \
    } while (0)

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

static void TestScript() {
    pdx::Node doc;
    std::string err;
    const char* text =
        "\xEF\xBB\xBF# a comment\n"
        "portraits = {\n"
        "\thuman_female_01 = { entity = \"portrait_x\" # trailing\n"
        "\t\tflag = yes  count = 3  ratio = -0.25\n"
        "\t\tlist = { a b \"c d\" }\n"
        "\t\tnested = { inner = { deep = 1 } }\n"
        "\t}\n"
        "\tscope:thing = { OR = { gender = male age >= 30 } }\n"
        "}\n";
    CHECK(pdx::Parse(text, &doc, &err));
    const pdx::Node* top = doc.Find("portraits");
    CHECK(top && top->block);
    const pdx::Node* e = top ? top->Find("human_female_01") : nullptr;
    CHECK(e != nullptr);
    if (e) {
        CHECK(e->Str("entity") == "portrait_x");
        CHECK(e->Bool("flag"));
        CHECK(!e->Bool("missing"));
        CHECK(e->Bool("missing", true));
        CHECK(Near(e->Num("count"), 3.0));
        CHECK(Near(e->Num("ratio"), -0.25));
        const auto list = e->List("list");
        CHECK(list.size() == 3 && list[2] == "c d");
        CHECK(e->Find("nested") && e->Find("nested")->Find("inner") && Near(e->Find("nested")->Find("inner")->Num("deep"), 1.0));
    }
    CHECK(top && top->Find("scope:thing") != nullptr);
    pdx::Node bad;
    CHECK(!pdx::Parse("a = { b = 1\n", &bad, &err));  // missing }
    CHECK(err.find("line") == 0);
    CHECK(!pdx::Parse("a = \"unterminated\n", &bad, &err));
    CHECK(!pdx::Parse("}", &bad, &err));
}

static void WriteFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static void TestRegistry() {
    const fs::path root = fs::temp_directory_path() / "l2d_test_mod";
    fs::remove_all(root);
    WriteFile(root / "gfx/portraits/live2d/a.txt",
        "portraits = {\n"
        "  key_one = {\n"
        "    live2d = yes\n"
        "    live2d_model = \"gfx/live2d/m/model.model3.json\"\n"
        "    live2d_unmirror = no\n"
        "    live2d_scale = 1.5\n"
        "    live2d_view = { auto = yes body = 0.5 }\n"
        "    live2d_view_character_large = { x = 0.4 y = 0.3 height = 0.2 }\n"
        "    live2d_view_800x400 = { auto = yes body = 0.7 scale = 2 }\n"
        "    live2d_view_nonsense = { auto = yes }\n"
        "    live2d_actions = {\n"
        "      mouse_follow = { enabled = yes strength = 0.6 }\n"
        "      click = { motion_group = \"touch*\" voices = { touch_1 = \"s/a.wav\" touch_2 = { \"s/b.wav\" \"s/c.wav\" } } sounds = { \"s/d.wav\" } volume = 0.5 }\n"
        "      click_head = { motion_groups = { a b } expression = \"smile\" expression_hold = 2 }\n"
        "      hover = yes\n"
        "      idle = { motion_group = \"wait*\" interval = { 10 20 } }\n"
        "      greeting = { replace_engine_sound = yes sound = \"s/e.ogg\" }\n"
        "      drag = { enabled = yes }\n"
        "    }\n"
        "  }\n"
        "  not_live2d = { entity = \"x\" }\n"
        "  no_model = { live2d = yes }\n"
        "}\n");
    const l2d::Registry reg = l2d::ScanRegistry({ root.string() }, true, false);
    CHECK(reg.entries.size() == 1);
    if (reg.entries.empty()) return;
    const l2d::PortraitEntry& p = reg.entries[0];
    CHECK(p.key == "key_one" && p.live2d && !p.spine);
    CHECK(p.model.find("model.model3.json") != std::string::npos && fs::path(p.model).is_absolute());
    CHECK(!p.unmirror);
    CHECK(Near(p.view.scale, 1.5) && p.view.auto_view && Near(p.view.body, 0.5));
    CHECK(p.views.size() == 2);  // the nonsense selector is dropped
    if (p.views.size() == 2) {
        CHECK(p.views[0].kind == 1 && !p.views[0].auto_view && Near(p.views[0].h, 0.2));
        CHECK(Near(p.views[0].scale, 1.5));  // what a variant leaves out is the default's
        CHECK(p.views[1].width == 800 && p.views[1].height == 400 && Near(p.views[1].scale, 2.0) && Near(p.views[1].body, 0.7));
    }
    CHECK(p.mouse_follow.enabled && Near(p.mouse_follow.strength, 0.6));
    CHECK(p.click.enabled && p.click.motion_groups.size() == 1 && p.click.motion_groups[0] == "touch*" && Near(p.click.volume, 0.5));
    CHECK(p.click.voices.size() == 2 && p.click.voices[1].pattern == "touch_2" && p.click.voices[1].lines.size() == 2);
    CHECK(p.click.sounds.size() == 1);
    CHECK(p.click_areas.size() == 1 && p.click_areas[0].first == "head" && p.click_areas[0].second.motion_groups.size() == 2 &&
          p.click_areas[0].second.expression == "smile" && Near(p.click_areas[0].second.expression_hold, 2.0));
    CHECK(p.hover.enabled);
    CHECK(p.idle.enabled && Near(p.idle.interval_min, 10.0) && Near(p.idle.interval_max, 20.0));
    CHECK(p.greeting.enabled && p.greeting.replace_engine_sound && p.greeting.sounds.size() == 1);
    bool complained_about_drag = false, complained_about_view = false, complained_about_model = false;
    for (const std::string& m : reg.messages) {
        if (m.find("`drag`") != std::string::npos) complained_about_drag = true;
        if (m.find("live2d_view_nonsense") != std::string::npos) complained_about_view = true;
        if (m.find("no_model") != std::string::npos) complained_about_model = true;
    }
    CHECK(complained_about_drag && complained_about_view && complained_about_model);
    // the signature changes when a scanned file does
    const uint64_t before = l2d::ScanRegistry({ root.string() }, false, false).signature;
    WriteFile(root / "gfx/portraits/live2d/b.txt", "portraits = { }\n");
    CHECK(l2d::ScanRegistry({ root.string() }, false, false).signature != before);
    fs::remove_all(root);
}

static std::vector<uint8_t> MakeDds(const std::vector<uint8_t>& rgba, int w, int h, bool mips) {
    l2d::Image chain;
    chain.width = w;
    chain.height = h;
    l2d::BuildMipChain(&chain, rgba);
    std::vector<uint8_t> out(128, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(&out[at], &v, 4); };
    std::memcpy(out.data(), "DDS ", 4);
    put(4, 124);
    put(12, h);
    put(16, w);
    put(28, mips ? (uint32_t)chain.mips.size() : 1);
    put(76, 32);
    put(80, 4);
    std::memcpy(&out[84], "DXT5", 4);
    for (size_t l = 0; l < (mips ? chain.mips.size() : 1); ++l) {
        const int lw = chain.mip_width[l], lh = chain.mip_height[l];
        const int bw = (lw + 3) / 4, bh = (lh + 3) / 4;
        const size_t base = out.size();
        out.resize(base + (size_t)bw * bh * 16);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                unsigned char block[64];
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                        std::memcpy(block + (y * 4 + x) * 4, &chain.mips[l][((size_t)std::min(by * 4 + y, lh - 1) * lw + std::min(bx * 4 + x, lw - 1)) * 4], 4);
                stb_compress_dxt_block(&out[base + ((size_t)by * bw + bx) * 16], block, 1, STB_DXT_NORMAL);
            }
    }
    return out;
}

static void TestImages() {
    // an alpha-weighted mip: one opaque red pixel among transparent black ones stays red, not dark red
    {
        l2d::Image img;
        img.width = img.height = 2;
        std::vector<uint8_t> px = { 255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        l2d::BuildMipChain(&img, px);
        CHECK(img.mips.size() == 2);
        CHECK(img.mips[1][0] == 255 && img.mips[1][1] == 0 && img.mips[1][3] == 64);
    }
    // a BC3 file with a mip chain reads back close to the picture
    const int w = 64, h = 64;
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = (uint8_t)(x * 4); p[1] = (uint8_t)(y * 4); p[2] = (uint8_t)((x + y) * 2); p[3] = (uint8_t)(x < 32 ? 255 : 128 + y);
        }
    const std::vector<uint8_t> dds = MakeDds(px, w, h, true);
    l2d::Image img;
    std::string err;
    CHECK(l2d::ParseDds(dds.data(), dds.size(), &img, &err));
    CHECK(img.format == l2d::PixelFormat::BC3 && img.width == w && img.height == h && img.mips.size() == 7);
    std::vector<uint8_t> back;
    l2d::DecodeToRgba(img, 0, &back);
    double mse = 0;
    for (size_t i = 0; i < px.size(); ++i) mse += (double)(px[i] - back[i]) * (px[i] - back[i]);
    mse /= (double)px.size();
    CHECK(10.0 * std::log10(255.0 * 255.0 / std::max(mse, 1e-9)) > 33.0);
    // without a chain the file is decoded and given one (a texture drawn small needs it)
    const std::vector<uint8_t> flat = MakeDds(px, w, h, false);
    l2d::Image img2;
    CHECK(l2d::ParseDds(flat.data(), flat.size(), &img2, &err));
    CHECK(img2.format == l2d::PixelFormat::RGBA8 && img2.mips.size() == 7);
    // garbage is refused with a message
    std::vector<uint8_t> junk(200, 7);
    CHECK(!l2d::ParseDds(junk.data(), junk.size(), &img2, &err) && !err.empty());
    std::vector<uint8_t> cut(dds.begin(), dds.begin() + 140);
    CHECK(!l2d::ParseDds(cut.data(), cut.size(), &img2, &err));
}

static void TestViews() {
    l2d::Model::PortraitBounds b;
    b.valid = true;
    b.top = 0.9f;
    b.bottom = 0.1f;
    b.center_x = 0.45f;
    float cx, cy, h;
    l2d::Model::ViewFromBounds(b, 0.5f, &cx, &cy, &h);
    CHECK(Near(cx, 0.45) && Near(h, 0.4));
    // the crop starts 3% of the picture above the head: top edge (from the top) = (1 - 0.9) - 0.03 * 0.8
    CHECK(Near(cy - h * 0.5, 0.1 - 0.024));
    l2d::Model::ViewFromBounds(l2d::Model::PortraitBounds{}, 0.5f, &cx, &cy, &h);  // nothing measured: a fixed fallback
    CHECK(Near(h, 0.3));
}

int main() {
    TestScript();
    TestRegistry();
    TestImages();
    TestViews();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
