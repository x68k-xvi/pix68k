#pragma once
#include "paletted_frame.h"
#include <vita2d.h>
namespace pix68k_retro {
// The caller owns the GXM context and must keep it alive until destruction.
// render() ends its offscreen scene; it does not reset the shared vertex pool.
class PalettedGpu {
public:
    ~PalettedGpu();
    vita2d_texture* render(const PalettedFrame&, const uint32_t* software, int width, int height);
    unsigned gpuRows = 0, drawCalls = 0;
private:
    vita2d_texture *front_ = nullptr, *graphic_ = nullptr, *fallback_ = nullptr;
    vita2d_texture *textColors_ = nullptr, *graphicColors_ = nullptr, *output_ = nullptr;
    int width_ = 0, height_ = 0;
    int paletteRow[PalettedFrame::rows] = {};
    bool resize(int width, int height);
    void clear();
};
}
