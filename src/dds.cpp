#include "dds.hpp"

#include <algorithm>
#include <cstring>

namespace l2d {

namespace {

constexpr uint32_t kMagic = 0x20534444;  // "DDS "
constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | (uint32_t)(uint8_t)b << 8 | (uint32_t)(uint8_t)c << 16 | (uint32_t)(uint8_t)d << 24;
}
constexpr uint32_t kDXT1 = FourCC('D', 'X', 'T', '1'), kDXT3 = FourCC('D', 'X', 'T', '3'), kDXT5 = FourCC('D', 'X', 'T', '5');
constexpr uint32_t kDXT2 = FourCC('D', 'X', 'T', '2'), kDXT4 = FourCC('D', 'X', 'T', '4'), kDX10 = FourCC('D', 'X', '1', '0');

uint32_t U32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

size_t BlockBytes(PixelFormat f) { return f == PixelFormat::BC1 ? 8 : 16; }

size_t LevelBytes(PixelFormat f, int w, int h) {
    if (f == PixelFormat::RGBA8) return (size_t)w * h * 4;
    return (size_t)std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * BlockBytes(f);
}

// the four colours of a BC1-style colour block; `four_colour` forces the opaque mode that BC2 and BC3 always use
void ColourPalette(const uint8_t* b, bool four_colour, uint8_t pal[4][4]) {
    const uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    auto expand = [](uint16_t c, uint8_t* o) {
        const int r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
        o[0] = (uint8_t)((r << 3) | (r >> 2));
        o[1] = (uint8_t)((g << 2) | (g >> 4));
        o[2] = (uint8_t)((bl << 3) | (bl >> 2));
        o[3] = 255;
    };
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    if (c0 > c1 || four_colour) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;  // transparent black
    }
}

void DecodeLevel(PixelFormat f, const uint8_t* src, int w, int h, std::vector<uint8_t>* rgba) {
    rgba->assign((size_t)w * h * 4, 0);
    if (f == PixelFormat::RGBA8) {
        std::memcpy(rgba->data(), src, rgba->size());
        return;
    }
    const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    const size_t bytes = BlockBytes(f);
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + ((size_t)by * bw + bx) * bytes;
            const uint8_t* colour = f == PixelFormat::BC1 ? blk : blk + 8;
            uint8_t pal[4][4];
            ColourPalette(colour, f != PixelFormat::BC1, pal);
            uint8_t alpha[16];
            if (f == PixelFormat::BC1) {
                std::fill(alpha, alpha + 16, 255);  // overwritten below by the palette's alpha
            } else if (f == PixelFormat::BC2) {
                for (int i = 0; i < 16; ++i) {
                    const int v = (blk[i / 2] >> ((i & 1) * 4)) & 15;
                    alpha[i] = (uint8_t)(v * 17);
                }
            } else {
                const int a0 = blk[0], a1 = blk[1];
                uint8_t table[8] = { (uint8_t)a0, (uint8_t)a1, 0, 0, 0, 0, 0, 0 };
                if (a0 > a1) {
                    for (int i = 1; i <= 6; ++i) table[1 + i] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
                } else {
                    for (int i = 1; i <= 4; ++i) table[1 + i] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
                    table[6] = 0;
                    table[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= (uint64_t)blk[2 + i] << (8 * i);
                for (int i = 0; i < 16; ++i) alpha[i] = table[(bits >> (3 * i)) & 7];
            }
            const uint32_t idx = U32(colour + 4);
            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px, y = by * 4 + py;
                    if (x >= w || y >= h) continue;
                    const int i = py * 4 + px;
                    const uint8_t* c = pal[(idx >> (2 * i)) & 3];
                    uint8_t* o = rgba->data() + ((size_t)y * w + x) * 4;
                    o[0] = c[0];
                    o[1] = c[1];
                    o[2] = c[2];
                    o[3] = f == PixelFormat::BC1 ? c[3] : alpha[i];
                }
            }
        }
    }
}

} // namespace

size_t Image::Pitch(int level) const {
    const int w = mip_width[level];
    if (format == PixelFormat::RGBA8) return (size_t)w * 4;
    return (size_t)std::max(1, (w + 3) / 4) * BlockBytes(format);
}

size_t Image::Bytes() const {
    size_t n = 0;
    for (const auto& m : mips) n += m.size();
    return n;
}

void DecodeToRgba(const Image& img, int level, std::vector<uint8_t>* rgba) {
    DecodeLevel(img.format, img.mips[level].data(), img.mip_width[level], img.mip_height[level], rgba);
}

bool ParseDds(const uint8_t* data, size_t size, Image* out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    if (size < 128 || U32(data) != kMagic || U32(data + 4) != 124) return fail("not a DDS file");
    const uint32_t flags = U32(data + 8);
    const int height = (int)U32(data + 12), width = (int)U32(data + 16);
    uint32_t mip_count = U32(data + 28);
    const uint32_t pf_flags = U32(data + 80), fourcc = U32(data + 84), bit_count = U32(data + 88);
    const uint32_t rmask = U32(data + 92), gmask = U32(data + 96), bmask = U32(data + 100), amask = U32(data + 104);
    const uint32_t caps2 = U32(data + 112);
    (void)flags;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return fail("unreasonable DDS size");
    if (caps2 & (0x200 | 0x200000)) return fail("cube maps and volume textures are not supported");
    size_t offset = 128;

    enum class Kind { Block, Raw32 };
    Kind kind = Kind::Block;
    PixelFormat bc = PixelFormat::BC3;
    bool bgra = false;
    if (pf_flags & 4) {  // DDPF_FOURCC
        uint32_t code = fourcc;
        if (code == kDX10) {
            if (size < 148) return fail("truncated DDS (DX10 header)");
            const uint32_t dxgi = U32(data + 128);
            offset = 148;
            if (U32(data + 140) > 1) return fail("DDS texture arrays are not supported");
            switch (dxgi) {
            case 71: case 72: code = kDXT1; break;
            case 74: case 75: code = kDXT3; break;
            case 77: case 78: code = kDXT5; break;
            case 28: case 29: code = 0; kind = Kind::Raw32; bgra = false; break;
            case 87: case 91: code = 0; kind = Kind::Raw32; bgra = true; break;
            default: return fail("unsupported DXGI format " + std::to_string(dxgi) + " in a DDS file (BC1, BC2, BC3 and 8-bit RGBA are supported)");
            }
        }
        if (code == kDXT1) { bc = PixelFormat::BC1; kind = Kind::Block; }
        else if (code == kDXT3) { bc = PixelFormat::BC2; kind = Kind::Block; }
        else if (code == kDXT5) { bc = PixelFormat::BC3; kind = Kind::Block; }
        else if (code == kDXT2 || code == kDXT4) return fail("premultiplied DXT2/DXT4 are not supported, save the texture as DXT5");
        else if (code != 0) return fail("unsupported DDS four-character code");
    } else if ((pf_flags & 0x40) && bit_count == 32) {  // DDPF_RGB, 32 bits
        kind = Kind::Raw32;
        if (rmask == 0x00FF0000 && gmask == 0x0000FF00 && bmask == 0x000000FF) bgra = true;
        else if (rmask == 0x000000FF && gmask == 0x0000FF00 && bmask == 0x00FF0000) bgra = false;
        else return fail("unsupported 32-bit DDS channel layout");
        (void)amask;
    } else {
        return fail("unsupported DDS pixel format (use DXT1, DXT3 or DXT5)");
    }

    if (mip_count == 0) mip_count = 1;
    if (kind == Kind::Raw32) {
        const size_t need = (size_t)width * height * 4;
        if (size < offset + need) return fail("truncated DDS");
        std::vector<uint8_t> rgba(data + offset, data + offset + need);
        if (bgra)
            for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
        // a file with no alpha channel bits would leave alpha 0 or garbage; treat the mask as the authority
        if (!(pf_flags & 1))
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        out->width = width;
        out->height = height;
        BuildMipChain(out, std::move(rgba));
        return true;
    }

    // block-compressed
    out->width = width;
    out->height = height;
    out->format = bc;
    out->mips.clear();
    out->mip_width.clear();
    out->mip_height.clear();
    const bool aligned = width % 4 == 0 && height % 4 == 0;
    int levels_max = 1;
    for (int s = std::max(width, height); s > 1; s >>= 1) ++levels_max;
    const int levels = (int)std::min<uint32_t>(mip_count, (uint32_t)levels_max);
    size_t pos = offset;
    for (int l = 0; l < levels; ++l) {
        const int w = std::max(1, width >> l), h = std::max(1, height >> l);
        const size_t n = LevelBytes(bc, w, h);
        if (size < pos + n) {
            if (l == 0) return fail("truncated DDS");
            break;  // a chain cut short is still usable
        }
        out->mips.emplace_back(data + pos, data + pos + n);
        out->mip_width.push_back(w);
        out->mip_height.push_back(h);
        pos += n;
    }
    // Direct3D 11 wants a block-compressed texture's level 0 to be a whole number of blocks, and a texture drawn small needs
    // its mips: when either is missing, decode and build as RGBA8 (more memory, always correct)
    if (!aligned || (out->mips.size() == 1 && levels_max > 1)) {
        std::vector<uint8_t> rgba;
        DecodeLevel(bc, out->mips[0].data(), width, height, &rgba);
        out->format = PixelFormat::RGBA8;
        BuildMipChain(out, std::move(rgba));
    }
    return true;
}

void BuildMipChain(Image* img, std::vector<uint8_t> rgba) {
    img->format = PixelFormat::RGBA8;
    int levels = 1;
    for (int s = std::max(img->width, img->height); s > 1; s >>= 1) ++levels;
    img->mips.assign(levels, {});
    img->mip_width.assign(levels, 0);
    img->mip_height.assign(levels, 0);
    img->mips[0] = std::move(rgba);
    img->mip_width[0] = img->width;
    img->mip_height[0] = img->height;
    for (int l = 1; l < levels; ++l) {
        const int pw = img->mip_width[l - 1], ph = img->mip_height[l - 1];
        const int w = std::max(1, pw / 2), h = std::max(1, ph / 2);
        img->mip_width[l] = w;
        img->mip_height[l] = h;
        const std::vector<uint8_t>& p = img->mips[l - 1];
        std::vector<uint8_t> o((size_t)w * h * 4);
        for (int y = 0; y < h; ++y) {
            const int y0 = std::min(y * 2, ph - 1), y1 = std::min(y * 2 + 1, ph - 1);
            for (int x = 0; x < w; ++x) {
                const int x0 = std::min(x * 2, pw - 1), x1 = std::min(x * 2 + 1, pw - 1);
                const uint8_t* s[4] = { &p[((size_t)y0 * pw + x0) * 4], &p[((size_t)y0 * pw + x1) * 4],
                                        &p[((size_t)y1 * pw + x0) * 4], &p[((size_t)y1 * pw + x1) * 4] };
                const int asum = s[0][3] + s[1][3] + s[2][3] + s[3][3];
                uint8_t* d = &o[((size_t)y * w + x) * 4];
                for (int c = 0; c < 3; ++c) {
                    if (asum > 0) d[c] = (uint8_t)((s[0][c] * s[0][3] + s[1][c] * s[1][3] + s[2][c] * s[2][3] + s[3][c] * s[3][3] + asum / 2) / asum);
                    else d[c] = (uint8_t)((s[0][c] + s[1][c] + s[2][c] + s[3][c] + 2) / 4);
                }
                d[3] = (uint8_t)((asum + 2) / 4);
            }
        }
        img->mips[l] = std::move(o);
    }
}

} // namespace l2d
