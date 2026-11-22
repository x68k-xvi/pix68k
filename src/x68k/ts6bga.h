// TS-6BGA のソフトウェア描画用 VRAM と GD5434 の表示レジスタ。
// 資料: https://gorry.haun.org/ts6bga.html と TGA_005B の ts6bga.ap。
// BitBLT は Alpine TRM (Feb 1995) の GD5434 の範囲を実装。PCM は未実装。
#pragma once
#include <stdint.h>

namespace x68k {
class StateIO;
class Ts6bga {
public:
    static constexpr unsigned VRAM_SIZE = 2 * 1024 * 1024;
    explicit Ts6bga(uint32_t base = 0xee0000) : base_(base) { reset(); }
    void reset(); // VRAM は保持する
    bool window(uint32_t addr) const { return addr >= base_ && addr - base_ < 0x10000; }
    uint8_t memoryRead(unsigned off) const;
    void memoryWrite(unsigned off, uint8_t value);
    int read(unsigned off, bool retrace); // $E9E000 からの位置。未実装は -1
    bool write(unsigned off, uint8_t value);
    bool bltBusy() const { return (gr_[0x31] & 1) != 0; }
    bool dirty() const { return dirty_; }
    void snapshot(Ts6bga& out) { out = *this; dirty_ = false; }
    // スナップショットを読むコアから呼ぶ。余白は黒、アスペクト比を保って拡大縮小。
    using RowWriter = void (*)(void*, const uint32_t*, unsigned, unsigned);
    struct Image { const uint32_t* pixels; unsigned width, height, pitch; };
    void renderRows(unsigned width, unsigned height, RowWriter write, void* ctx,
                    const Image* passthrough = nullptr) const;
    bool selected() const { return (control_ & 0x40) != 0; }
    void render(uint32_t* dst, unsigned width, unsigned height, unsigned pitch) const;
    bool snapshotRgb(uint8_t* rgb, unsigned capacity, int& width, int& height) const;
    unsigned width() const;
    unsigned height() const;
    uint32_t pixel(unsigned x, unsigned y) const;
    void state(StateIO& s);
private:
    void graphicsWrite(unsigned index, uint8_t value);
    void startBlt();
    void resetBlt();
    void hostBltByte(uint8_t value);
    void expandByte(uint32_t dest, unsigned x, bool foreground);
    unsigned bltWord(unsigned index) const;
    uint32_t bltAddress(unsigned index) const;
    uint8_t chipRead(uint32_t addr) const;
    void chipWrite(uint32_t addr, uint8_t value);
    void ropWrite(uint32_t dest, uint8_t source);
    unsigned bankAddress(unsigned off) const;
    unsigned stride() const;
    unsigned start() const;
    unsigned depth() const;
    uint32_t base_;
    uint8_t vram_[VRAM_SIZE] = {};
    uint8_t sr_[256] = {}, cr_[256] = {}, gr_[256] = {}, ar_[32] = {};
    uint8_t palette_[768] = {};
    uint8_t si_ = 0, ci_ = 0, gi_ = 0, ai_ = 0;
    uint8_t dacMask_ = 255, hiddenDac_ = 0, dacReads_ = 0;
    uint16_t dacRead_ = 0, dacWrite_ = 0, control_ = 0;
    bool attrData_ = false, dirty_ = true;
    uint32_t hostPos_ = 0; // system-to-screen の入力バイト数 (行末の DWORD padding も含む)
    uint8_t hostData_[4] = {};
    uint8_t hostCount_ = 0; // X68000 の DWORD を GD5434 のバイト順に直すための FIFO

};
} // namespace x68k
