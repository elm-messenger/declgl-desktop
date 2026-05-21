#include "resources/image_decoder.h"

#include "stb_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace declgl {

namespace {

void stb_free(uint8_t* p) {
    if (p) stbi_image_free(p);
}

// Free for malloc'd buffers (used for the cropped copy below).
void cxx_free(uint8_t* p) {
    delete[] p;
}

}  // namespace

DecodedImage decode_image_file(const std::string& path,
                               const ImageCrop& crop) {
    DecodedImage out;

    int w = 0, h = 0, n_in = 0;
    uint8_t* raw = stbi_load(path.c_str(), &w, &h, &n_in, /*desired_channels=*/4);
    if (!raw) {
        const char* why = stbi_failure_reason();
        std::fprintf(stderr,
                     "[declgl/image] stbi_load failed for '%s': %s\n",
                     path.c_str(), why ? why : "(no reason)");
        return out;
    }

    const bool want_crop =
        crop.width > 0 && crop.height > 0 &&
        // Skip the copy if the crop spans the whole image.
        !(crop.x == 0 && crop.y == 0 &&
          crop.width == w && crop.height == h);

    if (!want_crop) {
        out.pixels = std::unique_ptr<uint8_t[], void(*)(uint8_t*)>(raw, &stb_free);
        out.width  = w;
        out.height = h;
        return out;
    }

    // Clip the crop to the source rectangle. Negative offsets are
    // clamped to 0; over-runs are clamped to (w,h).
    const int sx = std::clamp(crop.x,      0, w);
    const int sy = std::clamp(crop.y,      0, h);
    const int ex = std::clamp(crop.x + crop.width,  0, w);
    const int ey = std::clamp(crop.y + crop.height, 0, h);
    const int cw = std::max(0, ex - sx);
    const int ch = std::max(0, ey - sy);

    if (cw == 0 || ch == 0) {
        std::fprintf(stderr,
                     "[declgl/image] crop (%d,%d)+(%dx%d) is outside "
                     "image %dx%d\n",
                     crop.x, crop.y, crop.width, crop.height, w, h);
        stbi_image_free(raw);
        return out;
    }

    auto* buf = new (std::nothrow) uint8_t[static_cast<std::size_t>(cw * ch * 4)];
    if (!buf) {
        std::fprintf(stderr, "[declgl/image] OOM allocating %d-byte crop\n",
                     cw * ch * 4);
        stbi_image_free(raw);
        return out;
    }

    for (int row = 0; row < ch; ++row) {
        const uint8_t* src = raw + ((sy + row) * w + sx) * 4;
        uint8_t*       dst = buf + (row * cw) * 4;
        std::memcpy(dst, src, static_cast<std::size_t>(cw) * 4);
    }
    stbi_image_free(raw);

    out.pixels = std::unique_ptr<uint8_t[], void(*)(uint8_t*)>(buf, &cxx_free);
    out.width  = cw;
    out.height = ch;
    return out;
}

}  // namespace declgl
