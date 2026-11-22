#include "paletted_gpu.h"
#include <cstring>
#include <arm_neon.h>
// vita2d's pinned implementation copies this matrix into each draw command.
extern "C" { extern float _vita2d_ortho_matrix[16];
void matrix_init_orthographic(float*, float, float, float, float, float, float); }
namespace pix68k_retro {
namespace {
// Vita's generic libc byte loops are costly for these full-row transfers.
// Compare complete palettes and copy GPU-visible rows with NEON blocks.
void copyRow(void* dest, const void* source, size_t bytes) {
    auto* d = static_cast<uint8_t*>(dest);
    const auto* s = static_cast<const uint8_t*>(source);
    while (bytes >= 64) {
        const auto a = vld1q_u8(s), b = vld1q_u8(s + 16);
        const auto c = vld1q_u8(s + 32), e = vld1q_u8(s + 48);
        vst1q_u8(d, a); vst1q_u8(d + 16, b); vst1q_u8(d + 32, c); vst1q_u8(d + 48, e);
        d += 64; s += 64; bytes -= 64;
    }
    while (bytes >= 16) { vst1q_u8(d, vld1q_u8(s)); d += 16; s += 16; bytes -= 16; }
    if (bytes) std::memcpy(d, s, bytes);
}
uint32_t rgba(uint32_t rgb, bool front) {
    const uint32_t alpha = front && !(rgb & 0xffffff) ? 0 : 0xff000000;
    return alpha | ((rgb >> 16) & 0xff) | (rgb & 0xff00) | ((rgb & 0xff) << 16);
}
void nearest(vita2d_texture* t) {
    if (t) vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
}
}
void PalettedGpu::clear() {
    if (output_) vita2d_wait_rendering_done();
    for (auto** p : {&front_, &graphic_, &fallback_, &textColors_, &graphicColors_, &output_}) {
        if (*p) vita2d_free_texture(*p);
        *p = nullptr;
    }
    width_ = height_ = 0;
}
PalettedGpu::~PalettedGpu() { clear(); }
bool PalettedGpu::resize(int width, int height) {
    if (width_ == width && height_ == height) return true;
    clear();
    if (width <= 0 || (width & 7) || width > PalettedFrame::stride || height <= 0 || height > PalettedFrame::rows) return false;
    // Paletted source rows are indexed bytes. Each immutable CLUT snapshot
    // occupies one aligned row of a GPU-visible RGBA allocation.
    front_ = vita2d_create_empty_texture_format(width, height, SCE_GXM_TEXTURE_FORMAT_P8_ABGR);
    graphic_ = vita2d_create_empty_texture_format(width, height, SCE_GXM_TEXTURE_FORMAT_P8_ABGR);
    fallback_ = vita2d_create_empty_texture_format(width, height, SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB);
    textColors_ = vita2d_create_empty_texture_format(256, height, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    graphicColors_ = vita2d_create_empty_texture_format(256, height, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    output_ = vita2d_create_empty_texture_rendertarget(width, height, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (!front_ || !graphic_ || !fallback_ || !textColors_ || !graphicColors_ || !output_) { clear(); return false; }
    nearest(front_); nearest(graphic_); nearest(fallback_); nearest(output_);
    width_ = width; height_ = height;
    return true;
}
vita2d_texture* PalettedGpu::render(const PalettedFrame& frame, const uint32_t* software, int width, int height) {
    if (width <= 0 || (width & 7) || width > PalettedFrame::stride || height <= 0 || height > PalettedFrame::rows) return nullptr;
    bool any = false;
    for (int y = 0; y < height; y++) if (frame.supported[y] && frame.widths[y] >= width) { any = true; break; }
    if (!any) return nullptr;
    // The previous frame may still reference these indexed textures/CLUTs.
    // Wait before reusing them; no CPU framebuffer readback is needed here.
    vita2d_wait_rendering_done();
    if (!resize(width, height)) return nullptr;
    auto* f = static_cast<uint8_t*>(vita2d_texture_get_datap(front_));
    auto* g = static_cast<uint8_t*>(vita2d_texture_get_datap(graphic_));
    auto* cpu = static_cast<uint8_t*>(vita2d_texture_get_datap(fallback_));
    auto* tp = static_cast<uint32_t*>(vita2d_texture_get_datap(textColors_));
    auto* gp = static_cast<uint32_t*>(vita2d_texture_get_datap(graphicColors_));
    const unsigned fp = vita2d_texture_get_stride(front_), gPitch = vita2d_texture_get_stride(graphic_);
    const unsigned cpuPitch = vita2d_texture_get_stride(fallback_);
    auto supported = [&](int y) { return frame.supported[y] && frame.widths[y] >= width; };
    gpuRows = drawCalls = 0;
    for (int y = 0; y < height; y++) {
        if (supported(y)) {
            gpuRows++;
            copyRow(f + y * fp, frame.front.data() + y * PalettedFrame::stride, width);
            copyRow(g + y * gPitch, frame.graphic.data() + y * PalettedFrame::stride, width);
            if (y && supported(y - 1) &&
                frame.paletteSerial[y] == frame.paletteSerial[y - 1]) {
                paletteRow[y] = paletteRow[y - 1];
            } else {
                paletteRow[y] = y;
                for (unsigned i = 0; i < 256; i++) {
                    tp[y * 256 + i] = rgba(frame.textColors[y * 256 + i], true);
                    gp[y * 256 + i] = rgba(frame.graphicColors[y * 256 + i], false);
                }
            }
        } else {
            copyRow(cpu + y * cpuPitch, software + y * PalettedFrame::stride, width * sizeof(uint32_t));
        }
    }
    float previousMatrix[16];
    const int clipped = vita2d_get_clipping_enabled();
    int clipX0, clipY0, clipX1, clipY1;
    vita2d_get_clip_rectangle(&clipX0, &clipY0, &clipX1, &clipY1);
    vita2d_disable_clipping();
    std::memcpy(previousMatrix, _vita2d_ortho_matrix, sizeof previousMatrix);
    matrix_init_orthographic(_vita2d_ortho_matrix, 0, (float)width, (float)height, 0, 0, 1);
    vita2d_start_drawing_advanced(output_, 0);
    sceGxmSetViewport(vita2d_get_context(), width / 2.0f, width / 2.0f,
                      height / 2.0f, -height / 2.0f, -0.5f, 0.5f);
    for (int y = 0; y < height;) {
        int end = y + 1;
        const bool gpu = supported(y);
        while (end < height && supported(end) == gpu && (!gpu || paletteRow[end] == paletteRow[y])) end++;
        if (gpu) {
            // Commands copy the texture descriptor. CLUT data remains immutable
            // for the whole scene, including when palettes change mid-frame.
            auto front = *front_, graphic = *graphic_;
            sceGxmTextureSetPalette(&front.gxm_tex, tp + y * 256);
            sceGxmTextureSetPalette(&graphic.gxm_tex, gp + y * 256);
            vita2d_draw_texture_part(&graphic, 0, (float)y, 0, (float)y, (float)width, (float)(end - y));
            vita2d_draw_texture_part(&front, 0, (float)y, 0, (float)y, (float)width, (float)(end - y));
            drawCalls += 2;
        } else {
            vita2d_draw_texture_part(fallback_, 0, (float)y, 0, (float)y, (float)width, (float)(end - y));
            drawCalls++;
        }
        y = end;
    }
    vita2d_end_drawing();
    std::memcpy(_vita2d_ortho_matrix, previousMatrix, sizeof previousMatrix);
    if (clipped) { vita2d_set_clip_rectangle(clipX0, clipY0, clipX1, clipY1); vita2d_enable_clipping(); }
    const float halfWidth = 1.0f / previousMatrix[0], halfHeight = -1.0f / previousMatrix[5];
    sceGxmSetViewport(vita2d_get_context(), halfWidth, halfWidth, halfHeight, -halfHeight, -0.5f, 0.5f);
    return output_;
}
}
