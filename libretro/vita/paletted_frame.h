#pragma once
#include "x68k/render.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace pix68k_retro {
// Immutable while the GPU consumes it. Palette snapshots preserve changes
// between scanlines; unsupported lines retain the normal CPU framebuffer.
struct PalettedFrame : x68k::IndexedLineSink {
    static constexpr int stride = x68k::Renderer::MAX_WIDTH;
    static constexpr int rows = x68k::Renderer::MAX_HEIGHT;
    template<class T, size_t N> struct Buffer {
        std::unique_ptr<T[]> storage{new (std::nothrow) T[N]()};
        T* data() { return storage.get(); }
        const T* data() const { return storage.get(); }
        T& operator[](size_t i) { return storage[i]; }
        const T& operator[](size_t i) const { return storage[i]; }
        T* begin() { return data(); }
        T* end() { return data() ? data() + N : nullptr; }
    };
    Buffer<uint8_t, stride * rows> front, graphic;
    Buffer<uint32_t, 256 * rows> textColors, graphicColors;
    Buffer<uint16_t, rows> widths;
    Buffer<uint8_t, rows> supported;
    bool valid() const {
        return front.data() && graphic.data() && textColors.data() &&
            graphicColors.data() && widths.data() && supported.data() && paletteSerial.data();
    }
    Buffer<uint64_t, rows> paletteSerial;
    uint32_t lastText[256] = {}, lastGraphic[256] = {};
    uint64_t serial = 0;
    static bool samePalette(const uint32_t* a, const uint32_t* b) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        uint32x4_t diff = vdupq_n_u32(0);
        for (unsigned i = 0; i < 256; i += 4)
            diff = vorrq_u32(diff, veorq_u32(vld1q_u32(a + i), vld1q_u32(b + i)));
        const uint32x2_t half = vorr_u32(vget_low_u32(diff), vget_high_u32(diff));
        return (vget_lane_u32(half, 0) | vget_lane_u32(half, 1)) == 0;
#else
        return !std::memcmp(a, b, 256 * sizeof(uint32_t));
#endif
    }
    bool skipSoftware = false;
    uint32_t* software = nullptr;
    void resolveRange(int y, int begin, int end, uint32_t* pixels) const {
        const auto* tp = textColors.data() + y * 256;
        const auto* gp = graphicColors.data() + y * 256;
        for (int x = begin; x < end; x++) {
            const uint32_t f = tp[front[y * stride + x]];
            pixels[y * stride + x] = f ? f : gp[graphic[y * stride + x]];
        }
    }
    void beginLine(int y, int width) override {
        if (y < 0 || y >= rows) return;
        // A narrower mode must preserve older pixels outside the new width,
        // just as the software renderer does. They are part of savestates.
        if (software && supported[y] && width < widths[y])
            resolveRange(y, std::max(0, width), widths[y], software);
        supported[y] = false;
        widths[y] = (uint16_t)width;
    }
    bool above(int y, int width, const uint8_t* f, const uint8_t* g,
               const uint32_t* tp, const uint32_t* gp) override {
        if (y < 0 || y >= rows || width <= 0 || width > stride) return false;
        auto* outF = front.data() + y * stride;
        auto* outG = graphic.data() + y * stride;
        if (f) std::memcpy(outF, f, width); else std::memset(outF, 0, width);
        if (g) std::memcpy(outG, g, width); else std::memset(outG, 0, width);
        static const uint32_t black[256] = {};
        if (!gp) gp = black;
        // A serial is reused only while both complete palettes are unchanged.
        // Row snapshots remain independent: historical rows survive mode changes.
        if (!serial || !samePalette(tp, lastText) || !samePalette(gp, lastGraphic)) {
            serial++;
            std::memcpy(lastText, tp, sizeof lastText);
            std::memcpy(lastGraphic, gp, sizeof lastGraphic);
        }
        paletteSerial[y] = serial;
        std::memcpy(textColors.data() + y * 256, lastText, sizeof lastText);
        std::memcpy(graphicColors.data() + y * 256, lastGraphic, sizeof lastGraphic);
        supported[y] = true;
        widths[y] = (uint16_t)width;
        return skipSoftware;
    }
    void invalidate() { std::fill(supported.begin(), supported.end(), 0); }
    // Materialize the canonical software surface for savestates or fallback.
    void resolve(uint32_t* pixels, int width, int height) const {
        for (int y = 0; y < std::min(height, rows); y++) if (supported[y]) {
            resolveRange(y, 0, std::min<int>(width, widths[y]), pixels);
        }
    }
};
}
