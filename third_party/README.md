# third_party

Single-header libraries copied unchanged, so the build needs no network access for them.

| File | Project | Version | License |
|---|---|---|---|
| `miniaudio.h` | [mackron/miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | public domain or MIT-0; the voice playback (WASAPI, WAV/MP3/FLAC decoders) |
| `stb_vorbis.c` | [nothings/stb](https://github.com/nothings/stb) | commit 2c980bb (stb_vorbis 1.22) | public domain; the Ogg Vorbis decoder for miniaudio |
| `stb_dxt.h` | [nothings/stb](https://github.com/nothings/stb) | commit 2c980bb (stb_dxt 1.12) | public domain (or MIT, see the end of the file); used by `l2d_pack` only |
| `stb_image.h`, `stb_image_write.h` | [nothings/stb](https://github.com/nothings/stb) | commit 2c980bb (stb_image 2.30) | public domain (or MIT, see the end of each file) |
| `nlohmann/json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT |
