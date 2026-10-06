// l2d_pack: turns a Live2D model folder (model3.json + PNG/JPEG textures) into one whose textures are DDS files in DXT5
// (BC3) with a full mip chain, the format of the game's own textures: a quarter of the video memory of RGBA8, and no mip
// building at load time. Everything else in the folder (moc3, motions, physics, expressions...) is copied and model3.json is
// rewritten to point at the .dds files.
//
//   l2d_pack --in <model dir or model3.json> --out <new dir> [--max-size 2048] [--fast]
//
// Mips are made from alpha-weighted averages and the colour of fully transparent texels is bled outwards from the picture,
// so bilinear filtering of the straight-alpha texture does not draw dark or coloured fringes around the art.
#include "dds.hpp"
#include "utf8_path.hpp"
#include "live2d_model.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

namespace fs = std::filesystem;

namespace {

// Fills the colour of transparent pixels from their nearest opaque-ish neighbours, up to `passes` pixels away.
void BleedColour(std::vector<uint8_t>& px, int w, int h, int passes) {
    std::vector<uint8_t> filled((size_t)w * h);
    for (size_t i = 0; i < filled.size(); ++i) filled[i] = px[i * 4 + 3] > 0;
    std::vector<uint8_t> next_px;
    std::vector<uint8_t> next_filled;
    for (int pass = 0; pass < passes; ++pass) {
        next_px = px;
        next_filled = filled;
        bool any = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                if (filled[i]) continue;
                int sum[3] = { 0, 0, 0 }, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const size_t j = (size_t)ny * w + nx;
                        if (!filled[j]) continue;
                        for (int c = 0; c < 3; ++c) sum[c] += px[j * 4 + c];
                        ++n;
                    }
                }
                if (!n) continue;
                for (int c = 0; c < 3; ++c) next_px[i * 4 + c] = (uint8_t)((sum[c] + n / 2) / n);
                next_filled[i] = 1;
                any = true;
            }
        }
        px.swap(next_px);
        filled.swap(next_filled);
        if (!any) break;
    }
}

// 2x box filter, alpha weighted (the same one the plugin uses for textures without a chain)
void Downscale(std::vector<uint8_t>& px, int& w, int& h) {
    l2d::Image tmp;
    tmp.width = w;
    tmp.height = h;
    l2d::BuildMipChain(&tmp, px);
    px = tmp.mips[1];
    w = tmp.mip_width[1];
    h = tmp.mip_height[1];
}

std::vector<uint8_t> CompressBc3(const std::vector<uint8_t>& px, int w, int h, int mode, unsigned threads) {
    const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    std::vector<uint8_t> out((size_t)bw * bh * 16);
    auto work = [&](int row0, int row1) {
        uint8_t block[64];
        for (int by = row0; by < row1; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        const int sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);  // pad by repeating the edge
                        std::memcpy(block + (y * 4 + x) * 4, &px[((size_t)sy * w + sx) * 4], 4);
                    }
                }
                stb_compress_dxt_block(&out[((size_t)by * bw + bx) * 16], block, 1, mode);
            }
        }
    };
    std::vector<std::thread> pool;
    const int per = (bh + (int)threads - 1) / (int)threads;
    for (unsigned t = 0; t < threads; ++t) {
        const int r0 = (int)t * per, r1 = std::min(bh, r0 + per);
        if (r0 < r1) pool.emplace_back(work, r0, r1);
    }
    for (auto& t : pool) t.join();
    return out;
}

void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = (uint8_t)(x >> (8 * i));
}

// A DDS file the way the game's own DXT5 textures are written: legacy header, linear size of level 0, mip chain.
bool WriteDds(const fs::path& path, int w, int h, const std::vector<std::vector<uint8_t>>& levels) {
    std::vector<uint8_t> header(128, 0);
    std::memcpy(header.data(), "DDS ", 4);
    Put32(header, 4, 124);
    Put32(header, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);  // CAPS HEIGHT WIDTH PIXELFORMAT MIPMAPCOUNT LINEARSIZE
    Put32(header, 12, (uint32_t)h);
    Put32(header, 16, (uint32_t)w);
    Put32(header, 20, (uint32_t)levels[0].size());
    Put32(header, 28, (uint32_t)levels.size());
    Put32(header, 76, 32);
    Put32(header, 80, 0x4);  // DDPF_FOURCC
    std::memcpy(&header[84], "DXT5", 4);
    Put32(header, 108, 0x8 | 0x1000 | 0x400000);  // COMPLEX TEXTURE MIPMAP
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)header.data(), (std::streamsize)header.size());
    for (const auto& l : levels) f.write((const char*)l.data(), (std::streamsize)l.size());
    return (bool)f;
}

bool Pack(const fs::path& src, const fs::path& dst, fs::path* textures_out, int max_size, int mode, unsigned threads) {
    std::vector<uint8_t> bytes;
    {
        std::ifstream f(src, std::ios::binary);
        if (!f) { fprintf(stderr, "cannot open %s\n", l2d::U8(src).c_str()); return false; }
        bytes.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    int w = 0, h = 0, n = 0;
    uint8_t* raw = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!raw) { fprintf(stderr, "cannot decode %s: %s\n", l2d::U8(src).c_str(), stbi_failure_reason()); return false; }
    std::vector<uint8_t> px(raw, raw + (size_t)w * h * 4);
    stbi_image_free(raw);

    BleedColour(px, w, h, 8);
    while (max_size > 0 && std::max(w, h) > max_size) Downscale(px, w, h);
    if (w % 4 || h % 4) {
        fprintf(stderr, "  %s is %dx%d: a DXT5 texture needs a size that is a multiple of 4 (Live2D textures are normally powers of two)\n",
                l2d::U8(src.filename()).c_str(), w, h);
        return false;
    }

    l2d::Image chain;
    chain.width = w;
    chain.height = h;
    l2d::BuildMipChain(&chain, px);
    // bleed the colour of every level too: the alpha-weighted average leaves transparent texels with the plain average
    std::vector<std::vector<uint8_t>> levels;
    for (size_t l = 0; l < chain.mips.size(); ++l) {
        std::vector<uint8_t> lv = chain.mips[l];
        if (l > 0) BleedColour(lv, chain.mip_width[l], chain.mip_height[l], 4);
        levels.push_back(CompressBc3(lv, chain.mip_width[l], chain.mip_height[l], mode, threads));
    }
    fs::create_directories(dst.parent_path());
    if (!WriteDds(dst, w, h, levels)) { fprintf(stderr, "cannot write %s\n", l2d::U8(dst).c_str()); return false; }
    (void)textures_out;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    fs::path in, out;
    int max_size = 0;
    int mode = STB_DXT_HIGHQUAL;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--in") in = fs::u8path(next());
        else if (a == "--out") out = fs::u8path(next());
        else if (a == "--max-size") max_size = atoi(next().c_str());
        else if (a == "--fast") mode = STB_DXT_NORMAL;
    }
    if (in.empty() || out.empty()) {
        fprintf(stderr, "usage: l2d_pack --in <model dir or model3.json> --out <dir> [--max-size N] [--fast]\n");
        return 2;
    }
    fs::path dir = in, model3 = in;
    if (fs::is_directory(in)) {
        model3.clear();
        for (const auto& e : fs::directory_iterator(in))
            if (l2d::U8(e.path()).size() > 12 && l2d::U8(e.path()).rfind(".model3.json") == l2d::U8(e.path()).size() - 12) model3 = e.path();
        if (model3.empty()) { fprintf(stderr, "no .model3.json in %s\n", l2d::U8(in).c_str()); return 1; }
    } else {
        dir = in.parent_path();
    }
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());

    nlohmann::json j;
    {
        std::ifstream f(model3);
        try { j = nlohmann::json::parse(f, nullptr, true, true); }
        catch (const std::exception& e) { fprintf(stderr, "bad %s: %s\n", l2d::U8(model3).c_str(), e.what()); return 1; }
    }
    fs::create_directories(out);
    // everything except the textures and the json is copied as it is
    std::vector<fs::path> texture_files;
    for (const auto& t : j["FileReferences"]["Textures"]) texture_files.push_back(fs::u8path(t.get<std::string>()));
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const fs::path rel = fs::relative(e.path(), dir);
        if (e.path() == model3) continue;
        if (std::find(texture_files.begin(), texture_files.end(), rel) != texture_files.end()) continue;
        fs::create_directories((out / rel).parent_path());
        fs::copy_file(e.path(), out / rel, fs::copy_options::overwrite_existing);
    }
    size_t before = 0, after = 0;
    for (size_t i = 0; i < texture_files.size(); ++i) {
        fs::path rel = texture_files[i];
        rel.replace_extension(".dds");
        printf("  %s -> %s\n", l2d::U8(texture_files[i]).c_str(), l2d::U8(rel).c_str());
        fflush(stdout);
        if (!Pack(dir / texture_files[i], out / rel, nullptr, max_size, mode, threads)) return 1;
        before += fs::file_size(dir / texture_files[i]);
        after += fs::file_size(out / rel);
        j["FileReferences"]["Textures"][i] = rel.generic_string();
    }
    std::ofstream o(out / model3.filename());
    o << j.dump(2) << "\n";
    printf("packed %s: %zu texture(s), %.1f MB -> %.1f MB on disk (DXT5 with mips)\n", l2d::U8(model3.filename()).c_str(), texture_files.size(),
           before / 1048576.0, after / 1048576.0);
    return 0;
}
