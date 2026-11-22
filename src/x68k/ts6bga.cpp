#include "x68k/ts6bga.h"
#include "x68k/state.h"
#include <string.h>

namespace x68k {
void Ts6bga::reset() {
    memset(sr_, 0, sizeof sr_); memset(cr_, 0, sizeof cr_);
    memset(gr_, 0, sizeof gr_); memset(ar_, 0, sizeof ar_);
    memset(palette_, 0, sizeof palette_);
    si_ = ci_ = gi_ = ai_ = dacReads_ = hiddenDac_ = 0;
    dacRead_ = dacWrite_ = control_ = 0; dacMask_ = 255;
    attrData_ = false; dirty_ = true;
    resetBlt();
}
unsigned Ts6bga::bankAddress(unsigned off) const {
    // GR0B bit5: 16KB 単位 (0 なら 4KB)。64KB 窓は連続する。
    const bool bank16K = (gr_[11] & 0x20) != 0;
    // 4KB粒度は20bit、16KB粒度は22bitのアドレス加算器 (TRM D1-7/8)。
    const unsigned mask = bank16K ? VRAM_SIZE - 1 : 0xfffff;
    return ((unsigned(gr_[9]) << (bank16K ? 14 : 12)) + off) & mask;
}
uint8_t Ts6bga::memoryRead(unsigned off) const { return vram_[bankAddress(off)]; }
void Ts6bga::memoryWrite(unsigned off, uint8_t v) {
    if (gr_[0x31] & 1) {
        hostData_[hostCount_++] = v;
        if (hostCount_ == 4) {
            hostCount_ = 0;
            // TS-6BGA の 16-bit バスは、GD5434 の隣接バイトが逆。
            for (unsigned i = 0; i < 4 && (gr_[0x31] & 1); ++i) hostBltByte(hostData_[i ^ 1]);
        }
        return;
    }
    vram_[bankAddress(off)] = v; dirty_ = true;
}
int Ts6bga::read(unsigned off, bool retrace) {
    switch (off) {
    case 0x210: return control_ >> 8;
    case 0x211: return control_ & 0xff; // PCM の転送要求は出さない
    case 0x3c5: return si_;
    case 0x3c4: return sr_[si_];
    case 0x3cf: return gi_;
    case 0x3ce: return gr_[gi_];
    case 0x3d5: return ci_;
    case 0x3d4: return cr_[ci_];
    case 0x3c0: return ar_[ai_ & 31];
    case 0x3c7:
        if (dacReads_ == 4) { dacReads_ = 0; return hiddenDac_; }
        ++dacReads_; return dacMask_;
    case 0x3c8: {
        dacReads_ = 0; unsigned i = dacRead_; dacRead_ = (dacRead_ + 1) % 768; return palette_[i];
    }
    case 0x3db: attrData_ = false; return retrace ? 8 : 0;
    default: return off >= 0x3b0 && off < 0x3e0 ? 0 : -1;
    }
}
bool Ts6bga::write(unsigned off, uint8_t v) {
    switch (off) {
    case 0x210: control_ = (control_ & 255) | (uint16_t(v) << 8); break;
    case 0x211:
        if ((control_ ^ v) & 0x40) dirty_ = true;
        control_ = (control_ & 0xff00) | (v & ~2); break;
    case 0x3c5: si_ = v; break;
    case 0x3c4:
        if (sr_[si_] != v) dirty_ = true;
        sr_[si_] = v; break;
    case 0x3cf: gi_ = v; break;
    case 0x3ce: graphicsWrite(gi_, v); break;
    case 0x3d5: ci_ = v; break;
    case 0x3d4:
        if (cr_[ci_] != v) dirty_ = true;
        cr_[ci_] = v; break;
    case 0x3c1:
        if (attrData_) ar_[ai_ & 31] = v;
        else ai_ = v;
        attrData_ = !attrData_; break;
    // TS-6BGA は VGA ポートの隣接バイトが逆の配置。
    case 0x3c7:
        if (dacReads_ == 4) {
            if (hiddenDac_ != v) dirty_ = true;
            hiddenDac_ = v;
        } else {
            if (dacMask_ != v) dirty_ = true;
            dacMask_ = v;
        }
        dacReads_ = 0; break;
    case 0x3c6: dacRead_ = unsigned(v) * 3; dacReads_ = 0; break;
    case 0x3c9: dacWrite_ = unsigned(v) * 3; dacReads_ = 0; break;
    case 0x3c8:
        if (palette_[dacWrite_] != (v & 63)) dirty_ = true;
        palette_[dacWrite_] = v & 63;
        dacWrite_ = (dacWrite_ + 1) % 768; dacReads_ = 0; break;
    default: return off >= 0x3b0 && off < 0x3e0;
    }
    return true;
}
// BitBLT のアドレスは GD5434 側のバイト順。CPU 窓の順とは A0 が逆になる。
uint8_t Ts6bga::chipRead(uint32_t a) const { return vram_[(a ^ 1) & (VRAM_SIZE - 1)]; }
void Ts6bga::chipWrite(uint32_t a, uint8_t v) { vram_[(a ^ 1) & (VRAM_SIZE - 1)] = v; dirty_ = true; }
unsigned Ts6bga::bltWord(unsigned i) const {
    const unsigned v = unsigned(gr_[i]) | (unsigned(gr_[i + 1]) << 8);
    return i == 0x22 ? v & 0x3ff : (i == 0x20 || i == 0x24 || i == 0x26) ? v & 0x1fff : v;
}
uint32_t Ts6bga::bltAddress(unsigned i) const { return bltWord(i) | (uint32_t(gr_[i + 2]) << 16); }
void Ts6bga::resetBlt() {
    gr_[0x31] = 0; hostPos_ = 0; hostCount_ = 0;
    memset(hostData_, 0, sizeof hostData_);
}
void Ts6bga::graphicsWrite(unsigned i, uint8_t v) {
    if (i == 0x31) {
        if (v & 4) { resetBlt(); gr_[0x31] = 4; }
        else if (!(gr_[0x31] & 1)) {
            gr_[0x31] = 0;
            if (v & 2) startBlt();
        }
        return;
    }
    // GD5434 にはパラメータのダブルバッファがない。転送中の変更は受け付けない。
    if (gr_[0x31] & 1) return;
    switch (i) {
    case 0x21: case 0x25: case 0x27: v &= 0x1f; break;
    case 0x23: v &= 3; break; // GD5434: 高さは10bit (GD5436は11bit)
    case 0x2a: case 0x2e: v &= 0x3f; break;
    case 0x2b: case 0x2f: case 0x33: v = 0; break; // GD5434 では予約
    case 0x30: v &= 0xfd; break; // bit1 は予約 (system destination はない)
    default: break;
    }
    gr_[i] = v;
}
void Ts6bga::ropWrite(uint32_t dest, uint8_t src) {
    const uint8_t d = chipRead(dest);
    uint8_t v;
    switch (gr_[0x32]) {
    case 0x00: v = 0; break;
    case 0x90: v = uint8_t(~src & ~d); break;
    case 0x50: v = uint8_t(~src & d); break;
    case 0xd0: v = uint8_t(~src); break;
    case 0x09: v = uint8_t(src & ~d); break;
    case 0x0b: v = uint8_t(~d); break;
    case 0x59: v = src ^ d; break;
    case 0xda: v = uint8_t(~src | ~d); break;
    case 0x05: v = src & d; break;
    case 0x95: v = uint8_t(~(src ^ d)); break;
    case 0x06: return;
    case 0xd6: v = uint8_t(~src | d); break;
    case 0x0d: v = src; break;
    case 0xad: v = uint8_t(src | ~d); break;
    case 0x6d: v = src | d; break;
    case 0x0e: v = 255; break;
    default: return; // 未定義のROPは書かない
    }
    chipWrite(dest, v);
}
void Ts6bga::expandByte(uint32_t dest, unsigned x, bool fg) {
    const bool transparent = (gr_[0x30] & 8) != 0;
    if (transparent && !fg) return;
    const unsigned pixelBytes = 1u + ((gr_[0x30] >> 4) & 3);
    // 透過時は4バイト分の色レジスタが必要 (TRM D8-11)。通常の8/16bit色は先頭1/2バイトを繰り返す。
    const unsigned c = transparent ? dest & 3 : x % pixelBytes;
    static constexpr unsigned foreground[4] = {1, 0x11, 0x13, 0x15};
    static constexpr unsigned background[4] = {0, 0x10, 0x12, 0x14};
    ropWrite(dest, gr_[fg ? foreground[c] : background[c]]);
}
void Ts6bga::startBlt() {
    const unsigned mode = gr_[0x30], pixelBytes = 1u + ((mode >> 4) & 3);
    const bool mono = mode & 0x80, pattern = mode & 0x40, backward = mode & 1, host = mode & 4;
    // GD5434 は packed-24 色展開、逆向き色展開/パターン、色キー比較コピーを持たない。
    if (((mono || pattern) && pixelBytes == 3) ||
        (backward && (mono || pattern || host)) || (host && pattern) || (!mono && (mode & 8))) return;
    gr_[0x31] = 0x0b; // Start / Progress / Busy
    hostPos_ = 0; hostCount_ = 0;
    if (host) return; // CPUのDWORD書き込みを待つ
    const unsigned w = bltWord(0x20) + 1, h = bltWord(0x22) + 1;
    const unsigned dp = bltWord(0x24), sp = bltWord(0x26);
    const uint32_t dst = bltAddress(0x28), src = bltAddress(0x2c);
    const unsigned monoPitch = ((w + pixelBytes - 1) / pixelBytes + 7) / 8;
    const uint32_t patternBase = src & ~(uint32_t(mono ? 8 : 64 * pixelBytes) - 1);
    for (unsigned y = 0; y < h; ++y) {
        const uint32_t dr = backward ? dst - y * dp : dst + y * dp;
        const uint32_t sr = backward ? src - y * sp : src + y * sp;
        const unsigned d = dr & (VRAM_SIZE - 1), s = sr & (VRAM_SIZE - 1);
        // 偶数境界の非重複 SRCCOPY は、隣接バイトの反転を両側で相殺してまとめて写せる。
        // 重なりがあるときは指定方向の1バイト転送を守る (memmove に置き換えない)。
        if (!mono && !pattern && !backward && gr_[0x32] == 0x0d && !((dr | sr | w) & 1) &&
            d + w <= VRAM_SIZE && s + w <= VRAM_SIZE && (d + w <= s || s + w <= d)) {
            memcpy(vram_ + d, vram_ + s, w); dirty_ = true;
            continue;
        }
        for (unsigned x = 0; x < w; ++x) {
            const uint32_t da = backward ? dr - x : dr + x;
            if (mono) {
                const unsigned px = x / pixelBytes;
                const uint8_t mask = chipRead(pattern ? patternBase + (y & 7) : src + y * monoPitch + px / 8);
                expandByte(da, x, (mask & (0x80 >> (px & 7))) != 0);
            } else {
                const uint32_t sa = pattern ? patternBase + (y & 7) * 8 * pixelBytes + x % (8 * pixelBytes)
                                           : backward ? sr - x : sr + x;
                ropWrite(da, chipRead(sa));
            }
        }
    }
    resetBlt(); // VRAM内の転送は開始レジスタの書き込み中に完了する (所要時間は未再現)
}
void Ts6bga::hostBltByte(uint8_t v) {
    const unsigned mode = gr_[0x30], pixelBytes = 1u + ((mode >> 4) & 3);
    const unsigned w = bltWord(0x20) + 1, h = bltWord(0x22) + 1;
    const bool mono = mode & 0x80;
    const unsigned inputPitch = mono ? ((w + pixelBytes - 1) / pixelBytes + 7) / 8 : (w + 3) & ~3u;
    const unsigned y = hostPos_ / inputPitch, x = hostPos_ % inputPitch;
    const uint32_t dr = bltAddress(0x28) + y * bltWord(0x24);
    if (mono) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            for (unsigned b = 0; b < pixelBytes; ++b) {
                const unsigned dx = (x * 8 + bit) * pixelBytes + b;
                if (dx < w) expandByte(dr + dx, dx, (v & (0x80 >> bit)) != 0);
            }
        }
    } else if (x < w) ropWrite(dr + x, v);
    if (++hostPos_ >= inputPitch * h) resetBlt();
}
unsigned Ts6bga::depth() const {
    switch ((sr_[7] >> 1) & 7) {
    case 3: return (hiddenDac_ & 1) ? 16 : 15;
    case 4: return 32;
    case 2: return 24;
    default: return 8;
    }
}
unsigned Ts6bga::width() const { return (unsigned(cr_[1]) + 1) * 8; }
unsigned Ts6bga::height() const {
    return 1 + unsigned(cr_[0x12]) + ((cr_[7] & 2) << 7) + ((cr_[7] & 0x40) << 3);
}
unsigned Ts6bga::stride() const {
    unsigned n = (unsigned(cr_[0x13]) | ((cr_[0x1b] & 0x10) << 4)) * 8;
    if (depth() == 32) n *= 2;
    return n;
}
unsigned Ts6bga::start() const {
    return (unsigned(cr_[0x0d]) | (unsigned(cr_[0x0c]) << 8) |
            ((cr_[0x1b] & 1) << 16) | ((cr_[0x1b] & 0x0c) << 15) |
            ((cr_[0x1d] & 0x80) << 12)) * 4;
}
uint32_t Ts6bga::pixel(unsigned x, unsigned y) const {
    const unsigned bits = depth(), bytes = (bits + 7) / 8;
    unsigned a = start() + y * stride() + x * bytes;
    auto get = [&]() { return unsigned(chipRead(a++)); };
    unsigned r, g, b;
    if (bits == 8) {
        unsigned i = (get() & dacMask_) * 3;
        auto expand = [](unsigned v) { return (v << 2) | (v >> 4); };
        r = expand(palette_[i]); g = expand(palette_[i + 1]); b = expand(palette_[i + 2]);
    } else if (bits == 15 || bits == 16) {
        unsigned v = get(); v |= get() << 8;
        r = (v >> (bits == 16 ? 11 : 10)) & 31;
        g = (v >> 5) & (bits == 16 ? 63 : 31); b = v & 31;
        r = (r << 3) | (r >> 2); b = (b << 3) | (b >> 2);
        g = bits == 16 ? (g << 2) | (g >> 4) : (g << 3) | (g >> 2);
    } else if (bits == 32) {
        b = get(); g = get(); r = get(); get(); // native BGRI = CPU窓 G8B8I8R8
    } else {
        b = get(); g = get(); r = get();
    }
    return 0xff000000u | (r << 16) | (g << 8) | b;
}
bool Ts6bga::snapshotRgb(uint8_t* rgb, unsigned capacity, int& w, int& h) const {
    const unsigned sw = width(), sh = height();
    if (!rgb || uint64_t(sw) * sh * 3 > capacity) return false;
    w = int(sw); h = int(sh);
    for (unsigned y = 0; y < sh; ++y) {
        for (unsigned x = 0; x < sw; ++x) {
            const uint32_t c = pixel(x, y);
            *rgb++ = uint8_t(c >> 16); *rgb++ = uint8_t(c >> 8); *rgb++ = uint8_t(c);
        }
    }
    return true;
}
void Ts6bga::render(uint32_t* dst, unsigned w, unsigned h, unsigned pitch) const {
    if (!dst || pitch < w) return;
    struct Target { uint32_t* dst; unsigned pitch; } target{dst, pitch};
    renderRows(w, h, [](void* ctx, const uint32_t* row, unsigned y, unsigned n) {
        auto& t = *static_cast<Target*>(ctx);
        memcpy(t.dst + y * t.pitch, row, n * sizeof *row);
    }, &target);
}
void Ts6bga::renderRows(unsigned w, unsigned h, RowWriter write, void* ctx, const Image* passthrough) const {
    if (!w || w > 4096 || !h || !write) return;
    // RAM の行を再利用。Device 属性の HDMI メモリを読み戻さない。
    alignas(16) uint32_t row[4096];
    const bool pass = !selected() && passthrough && passthrough->pixels && passthrough->width &&
                      passthrough->height && passthrough->pitch >= passthrough->width;
    const unsigned sw = pass ? passthrough->width : width(), sh = pass ? passthrough->height : height();
    unsigned dw = w, dh = unsigned(uint64_t(w) * sh / sw);
    if (dh > h) { dh = h; dw = unsigned(uint64_t(h) * sw / sh); }
    const unsigned ox = (w - dw) / 2, oy = (h - dh) / 2;
    const bool on = pass || (selected() && (sr_[7] & 1) && !(sr_[1] & 0x20));
    unsigned prevY = ~0u;
    const unsigned step = dw ? sw / dw : 0, remainder = dw ? sw % dw : 0;
    for (unsigned y = 0; y < h; ++y) {
        const bool active = on && y >= oy && y - oy < dh;
        const unsigned sy = active ? unsigned(uint64_t(y - oy) * sh / dh) : ~0u;
        if (y && sy == prevY) { write(ctx, row, y, w); continue; }
        for (unsigned x = 0; x < w; ++x) row[x] = 0xff000000u;
        if (active) {
            unsigned sx = 0, error = 0;
            for (unsigned x = 0; x < dw; ++x) {
                row[ox + x] = pass ? passthrough->pixels[sy * passthrough->pitch + sx] | 0xff000000u
                                  : pixel(sx, sy);
                sx += step; error += remainder;
                if (error >= dw) { ++sx; error -= dw; }
            }
        }
        write(ctx, row, y, w);
        prevY = sy;
    }
}
void Ts6bga::state(StateIO& s) {
    s.memory(vram_, sizeof vram_);
    s.io(sr_); s.io(cr_);
    uint8_t savedGr[256]; memcpy(savedGr, gr_, sizeof savedGr);
    s.local(savedGr, sizeof savedGr);
    if (s.applying()) memcpy(gr_, savedGr, sizeof gr_);
    s.io(ar_); s.io(palette_);
    s.io(si_); s.io(ci_); s.io(gi_); s.io(ai_);
    s.io(dacMask_); s.io(hiddenDac_); s.range(dacReads_, uint8_t(0), uint8_t(4));
    s.range(dacRead_, uint16_t(0), uint16_t(767)); s.range(dacWrite_, uint16_t(0), uint16_t(767)); s.io(control_); s.io(attrData_);
    if (s.version() >= 18) {
        const unsigned pos = s.range(hostPos_, uint32_t(0), uint32_t(8192 * 1024));
        s.io(hostData_);
        const unsigned count = s.range(hostCount_, uint8_t(0), uint8_t(3));
        const unsigned status = savedGr[0x31], mode = savedGr[0x30];
        if (status & 1) {
            const unsigned w = (unsigned(savedGr[0x20]) | ((savedGr[0x21] & 31u) << 8)) + 1;
            const unsigned h = (unsigned(savedGr[0x22]) | ((savedGr[0x23] & 3u) << 8)) + 1;
            const unsigned pixelBytes = 1 + ((mode >> 4) & 3);
            const unsigned pitch = mode & 0x80 ? ((w + pixelBytes - 1) / pixelBytes + 7) / 8 : (w + 3) & ~3u;
            if (status != 11 || !(mode & 4) || (mode & 0x43) ||
                ((mode & 0x80) && pixelBytes == 3) || (!(mode & 0x80) && (mode & 8)) ||
                pos >= pitch * h || (pos & 3)) s.fail("invalid TS-6BGA BitBLT input state");
        } else if ((status != 0 && status != 4) || pos != 0 || count != 0) {
            s.fail("invalid TS-6BGA idle BitBLT state");
        }
    } else if (s.applying()) resetBlt();
    if (s.applying()) dirty_ = true;
}
} // namespace x68k
