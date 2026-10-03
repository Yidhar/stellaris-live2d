#pragma once
// Reading DDS texture files, the format the game's own textures use: DXT1/DXT3/DXT5 (BC1/BC2/BC3) with or without a
// pre-built mip chain, and uncompressed 32-bit. The block-compressed data is kept as it is and uploaded to the GPU as such.
#include "live2d_model.hpp"

namespace l2d {

// Parses a DDS file in memory into `out`. BC1/BC2/BC3 files whose level 0 is a multiple of 4 wide and high keep their blocks
// and, when the file carries no mip chain, are decoded and given one (as RGBA8), because a texture drawn at a fraction of its
// size without mips shimmers. 32-bit uncompressed files become RGBA8 with a built chain.
bool ParseDds(const uint8_t* data, size_t size, Image* out, std::string* error);

// One level of `img` as straight-alpha RGBA, whatever its format (for tests and tools).
void DecodeToRgba(const Image& img, int level, std::vector<uint8_t>* rgba);

} // namespace l2d
