#include "x68k/machine.h"
#include "x68k/state.h"
#include "platform/frame_handoff.h"
#include <stdio.h>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>

static int failures;
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static void reg(x68k::Ts6bga& b, unsigned port, unsigned i, unsigned v) {
    CHECK(b.write(port, i)); CHECK(b.write(port ^ 1, v));
}
static void mode(x68k::Ts6bga& b, bool wide = false, bool rgb = false) {
    reg(b, 0x3c5, 7, rgb ? 9 : 7);
    reg(b, 0x3d5, 1, wide ? 127 : 95);
    reg(b, 0x3d5, 7, wide ? 0xfd : 0xb2);
    reg(b, 0x3d5, 0x12, 0xff);
    reg(b, 0x3d5, 0x13, 0);
    reg(b, 0x3d5, 0x1b, 0x32);
    reg(b, 0x3cf, 11, 0x20);
    CHECK(b.write(0x211, 0x40));
}
static void pixels() {
    auto b = std::make_unique<x68k::Ts6bga>(); mode(*b);
    CHECK(b->width() == 768 && b->height() == 512);
    b->memoryWrite(0, 0x7c); b->memoryWrite(1, 0);
    b->memoryWrite(2048, 3); b->memoryWrite(2049, 0xe0);
    CHECK(b->pixel(0, 0) == 0xffff0000);
    CHECK(b->pixel(0, 1) == 0xff00ff00);
    // スクショは表示選択やHDMIの倍率に依存せず、VRAMのRGBを元解像度で返す。
    std::vector<uint8_t> rgb(768u * 512u * 3u, 0xa5);
    int shotW = 0, shotH = 0;
    CHECK(!b->snapshotRgb(rgb.data(), unsigned(rgb.size() - 1), shotW, shotH));
    CHECK(rgb[0] == 0xa5);
    CHECK(!b->snapshotRgb(nullptr, unsigned(rgb.size()), shotW, shotH));
    b->write(0x211, 0);
    CHECK(b->snapshotRgb(rgb.data(), unsigned(rgb.size()), shotW, shotH));
    CHECK(shotW == 768 && shotH == 512);
    CHECK(rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0);
    CHECK(rgb[768 * 3] == 0 && rgb[768 * 3 + 1] == 255 && rgb[768 * 3 + 2] == 0);
    b->write(0x211, 0x40);

    reg(*b, 0x3cf, 9, 1); b->memoryWrite(0, 0x12);
    reg(*b, 0x3cf, 9, 0); CHECK(b->memoryRead(0x4000) == 0x12);
    reg(*b, 0x3cf, 9, 127); b->memoryWrite(0x4000, 0x7c);
    reg(*b, 0x3cf, 9, 0); CHECK(b->memoryRead(0) == 0x7c);
    reg(*b, 0x3d5, 0x0d, 0); reg(*b, 0x3d5, 0x0c, 2); // start=2048
    CHECK(b->pixel(0, 0) == 0xff00ff00);
    reg(*b, 0x3d5, 0x0c, 0);
    uint32_t out[8] = {};
    b->render(out, 4, 2, 4);
    CHECK(out[0] == 0xffff0000 && out[3] == 0xff000000); // centered 3x2
    CHECK(b->write(0x211, 0)); b->render(out, 4, 2, 4);
    for (auto v : out) CHECK(v == 0xff000000);
    const uint32_t original[2] = {0xffff0000, 0xff0000ff};
    x68k::Ts6bga::Image pass{original, 2, 1, 2};
    b->renderRows(4, 2, [](void* ctx, const uint32_t* row, unsigned y, unsigned n) {
        auto* out = static_cast<uint32_t*>(ctx);
        for (unsigned x = 0; x < n; ++x) out[y * n + x] = row[x];
    }, out, &pass);
    CHECK(out[0] == 0xffff0000 && out[1] == 0xffff0000 && out[3] == 0xff0000ff);
    CHECK(out[4] == 0xffff0000 && out[7] == 0xff0000ff);
    mode(*b, true); CHECK(b->width() == 1024 && b->height() == 768);
    mode(*b, false, true);
    b->memoryWrite(0, 0x23); b->memoryWrite(1, 0x45);
    b->memoryWrite(2, 0); b->memoryWrite(3, 0x67);
    CHECK(b->pixel(0, 0) == 0xff672345);
    b->memoryWrite(4096, 0x89); b->memoryWrite(4097, 0xab); b->memoryWrite(4099, 0xcd);
    CHECK(b->pixel(0, 1) == 0xffcd89ab);
    // Packed-24 crosses the swapped 16-bit CPU lanes at each odd pixel.
    reg(*b, 0x3c5, 7, 5);
    b->memoryWrite(0, 0x22); b->memoryWrite(1, 0x11); b->memoryWrite(3, 0x33);
    b->memoryWrite(2, 0x44); b->memoryWrite(5, 0x55); b->memoryWrite(4, 0x66);
    CHECK(b->pixel(0, 0) == 0xff332211 && b->pixel(1, 0) == 0xff665544);
    // indexed DAC, adjacent VGA ports reversed on TS-6BGA
    reg(*b, 0x3c5, 7, 1); b->memoryWrite(1, 1); b->memoryWrite(0, 2);
    b->write(0x3c9, 1); b->write(0x3c8, 63); b->write(0x3c8, 0); b->write(0x3c8, 0);
    CHECK(b->pixel(0, 0) == 0xffff0000);
    b->write(0x3c9, 2); b->write(0x3c8, 0); b->write(0x3c8, 0); b->write(0x3c8, 63);
    CHECK(b->pixel(1, 0) == 0xff0000ff); // native pixel 1 is CPU窓の偶数番地
    for (int i = 0; i < 4; ++i) b->read(0x3c7, false);
    b->write(0x3c7, 0xc5);
    for (int i = 0; i < 4; ++i) b->read(0x3c7, false);
    CHECK(b->read(0x3c7, false) == 0xc5);
    CHECK(b->read(0x3db, false) == 0 && b->read(0x3db, true) == 8);
}
static void mappingAndState(uint32_t base) {
    auto m = std::make_unique<x68k::Machine>();
    CHECK(m->setTs6bga(true, base));
    CHECK(m->setTs6bga(true, base));
    CHECK(!m->setTs6bga(true, base == 0xee0000 ? 0xef0000 : 0xee0000));
    CHECK(!m->setTs6bga(true, 0xed0000));
    auto& b = *m->ts6bga(); mode(b);
    // Same longword probe as TS6BGAG.X, supervisor CPU bus.
    m->write16(base, 0x0055, m68k::FC_SUPER_DATA);
    m->write16(base + 2, 0xaaff, m68k::FC_SUPER_DATA);
    CHECK(m->read16(base, m68k::FC_SUPER_DATA) == 0x0055);
    CHECK(m->read16(base + 2, m68k::FC_SUPER_DATA) == 0xaaff);
    m->write8(0xe9e3cf, 9, m68k::FC_SUPER_DATA);
    m->write8(0xe9e3ce, 1, m68k::FC_SUPER_DATA);
    m->write16(base, 0x7c00, m68k::FC_SUPER_DATA);
    m->write8(0xe9e3ce, 0, m68k::FC_SUPER_DATA);
    CHECK(b.pixel(0, 8) == 0xffff0000);
    std::vector<uint8_t> state(x68k::stateSize(*m)); size_t n = 0;
    CHECK(x68k::saveState(*m, state.data(), state.size(), n));
    size_t old = 0; CHECK(!x68k::saveState(*m, state.data(), state.size(), old, 16));
    b.memoryWrite(0x4000, 0);
    const char* error = nullptr;
    CHECK(x68k::loadState(*m, state.data(), n, error));
    CHECK(b.pixel(0, 8) == 0xffff0000);
    CHECK(m->setTs6bga(false));
    CHECK(!x68k::loadState(*m, state.data(), n, error));
}
static void handoff() {
    // Producer may replace pending frames; consumer owns its snapshot until take().
    auto live = std::make_unique<x68k::Ts6bga>(); mode(*live);
    auto slots = std::make_unique<x68k::Ts6bga[]>(3);
    platform::FrameHandoff h;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (unsigned i = 1; i <= 120; ++i) {
            live->memoryWrite(0, i); live->memoryWrite(4096, i);
            live->snapshot(slots[h.writer()]); h.publish([](unsigned) {});
        }
        done.store(true, std::memory_order_release);
    });
    unsigned seen = 0;
    do {
        int i = h.take();
        if (i >= 0) { CHECK(slots[i].memoryRead(0) == slots[i].memoryRead(4096)); ++seen; }
        std::this_thread::yield();
    } while (!done.load(std::memory_order_acquire));
    writer.join();
    int i = h.take(); if (i >= 0) { CHECK(slots[i].memoryRead(0) == 120); ++seen; }
    CHECK(seen != 0);
}
static void dirtyTracking() {
    auto b = std::make_unique<x68k::Ts6bga>();
    auto copy = std::make_unique<x68k::Ts6bga>();
    reg(*b, 0x3cf, 9, 255); // 4KB粒度の窓は1MiBで折り返す
    b->memoryWrite(0x1000, 0x66);
    reg(*b, 0x3cf, 9, 0); CHECK(b->memoryRead(0) == 0x66);
    reg(*b, 0x3cf, 11, 0x20); reg(*b, 0x3cf, 9, 64);
    CHECK(b->memoryRead(0) == 0); // 2MiBの上半分とは別
    b->snapshot(*copy); CHECK(!b->dirty());
    reg(*b, 0x3cf, 9, 3); reg(*b, 0x3cf, 11, 0x20); // CPUの窓だけを動かす
    b->write(0x3cf, 0x31); b->read(0x3ce, false); b->read(0x3db, true);
    b->write(0x3c5, 7); b->write(0x3d5, 1);
    b->write(0x3c9, 1); b->write(0x3c6, 1); // DACのインデックスだけ
    CHECK(!b->dirty());
    b->write(0x3c4, 7); CHECK(b->dirty()); b->snapshot(*copy);
    b->write(0x3c4, 7); CHECK(!b->dirty());
    b->write(0x3d4, 95); CHECK(b->dirty()); b->snapshot(*copy);
    b->write(0x211, 0x40); CHECK(b->dirty()); b->snapshot(*copy);
    b->write(0x211, 0xc0); CHECK(!b->dirty()); // PCMの制御だけでは画面は変わらない
    b->write(0x3c8, 63); CHECK(b->dirty()); b->snapshot(*copy);
    b->memoryWrite(0, 0x7c); CHECK(b->dirty());
}
int main() { dirtyTracking(); pixels(); mappingAndState(0xee0000); mappingAndState(0xef0000); handoff(); return failures ? 1 : 0; }
