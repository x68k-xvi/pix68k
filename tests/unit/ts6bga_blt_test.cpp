#include "x68k/machine.h"
#include "x68k/state.h"
#include <stdio.h>
#include <memory>
#include <vector>

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%d: %s\n", __LINE__, #x); ++failures; } } while (0)
static void reg(x68k::Ts6bga& b, unsigned i, unsigned v) { b.write(0x3cf, i); b.write(0x3ce, v); }
static unsigned getreg(x68k::Ts6bga& b, unsigned i) { b.write(0x3cf, i); return b.read(0x3ce, false); }
static void word(x68k::Ts6bga& b, unsigned i, unsigned v) { reg(b, i, v); reg(b, i + 1, v >> 8); }
static void addr(x68k::Ts6bga& b, unsigned i, unsigned v) { word(b, i, v); reg(b, i + 2, v >> 16); }
// Data addresses in these tests use the native GD5434 order, not the byte-swapped CPU window.
static void put(x68k::Ts6bga& b, unsigned a, unsigned v) {
    reg(b, 11, 0x20); reg(b, 9, a >> 14); b.memoryWrite((a & 0x3fff) ^ 1, v);
}
static unsigned get(x68k::Ts6bga& b, unsigned a) {
    reg(b, 11, 0x20); reg(b, 9, a >> 14); return b.memoryRead((a & 0x3fff) ^ 1);
}
static void setup(x68k::Ts6bga& b, unsigned src, unsigned dst, unsigned w, unsigned h,
                  unsigned sp = 16, unsigned dp = 16, unsigned mode = 0, unsigned rop = 0x0d) {
    word(b, 0x20, w - 1); word(b, 0x22, h - 1);
    word(b, 0x24, dp); word(b, 0x26, sp);
    addr(b, 0x28, dst); addr(b, 0x2c, src);
    reg(b, 0x30, mode); reg(b, 0x32, rop);
}
static void start(x68k::Ts6bga& b) { reg(b, 0x31, 2); }
static void colors(x68k::Ts6bga& b) {
    reg(b, 1, 0x11); reg(b, 0x11, 0x22); reg(b, 0x13, 0x33); reg(b, 0x15, 0x44);
    reg(b, 0, 0xee); reg(b, 0x10, 0xdd); reg(b, 0x12, 0xcc); reg(b, 0x14, 0xbb);
}
static void copies() {
    auto b = std::make_unique<x68k::Ts6bga>();
    for (unsigned y = 0; y < 3; ++y) for (unsigned x = 0; x < 8; ++x) put(*b, 0x3ffc + y * 20 + x, y * 20 + x + 1);
    setup(*b, 0x3ffc, 0x10000, 8, 3, 20, 24); start(*b);
    CHECK(getreg(*b, 0x31) == 0);
    for (unsigned y = 0; y < 3; ++y) for (unsigned x = 0; x < 8; ++x)
        CHECK(get(*b, 0x10000 + y * 24 + x) == y * 20 + x + 1);
    CHECK(get(*b, 0x10008) == 0);
    // Address wrap at installed 2MiB, independent of the CPU's bank selection.
    for (unsigned x = 0; x < 8; ++x) put(*b, 0x1ffffc + x, x + 30);
    setup(*b, 0x1ffffc, 0x20000, 8, 1); start(*b);
    for (unsigned x = 0; x < 8; ++x) CHECK(get(*b, 0x20000 + x) == x + 30);
    // Correct reverse order for overlap. Incorrect forward order must retain hardware smear semantics.
    for (unsigned x = 0; x < 16; ++x) put(*b, x, x + 1);
    setup(*b, 7, 9, 8, 1, 16, 16, 1); start(*b);
    for (unsigned x = 0; x < 8; ++x) CHECK(get(*b, x + 2) == x + 1);
    for (unsigned x = 0; x < 16; ++x) put(*b, x, x + 1);
    setup(*b, 0, 2, 8, 1); start(*b);
    for (unsigned x = 0; x < 8; ++x) CHECK(get(*b, x + 2) == (x % 2) + 1);
    // Reverse both axes: move three rows down by one row.
    for (unsigned y = 0; y < 3; ++y) for (unsigned x = 0; x < 8; ++x) put(*b, y * 16 + x, y * 20 + x + 1);
    setup(*b, 39, 55, 8, 3, 16, 16, 1); start(*b);
    for (unsigned y = 0; y < 3; ++y) for (unsigned x = 0; x < 8; ++x)
        CHECK(get(*b, (y + 1) * 16 + x) == y * 20 + x + 1);
    reg(*b, 0x21, 0xff); reg(*b, 0x23, 0xff); reg(*b, 0x25, 0xff); reg(*b, 0x2a, 0xff);
    CHECK(getreg(*b, 0x21) == 31 && getreg(*b, 0x23) == 3);
    CHECK(getreg(*b, 0x25) == 31 && getreg(*b, 0x2a) == 63);
    reg(*b, 0x31, 0x81); CHECK(getreg(*b, 0x31) == 0); // readonly status / no GD5436 autostart
}
static void rops() {
    auto b = std::make_unique<x68k::Ts6bga>();
    static const unsigned codes[16] = {0,0x90,0x50,0xd0,9,0xb,0x59,0xda,5,0x95,6,0xd6,0xd,0xad,0x6d,0xe};
    // Every boolean truth table, all four (source,destination) combinations in each test byte.
    for (unsigned truth = 0; truth < 16; ++truth) {
        put(*b, 0, 0xcc); put(*b, 16, 0xaa);
        setup(*b, 0, 16, 1, 1, 16, 16, 0, codes[truth]); start(*b);
        unsigned expected = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const unsigned input = (((0xcc >> bit) & 1) << 1) | ((0xaa >> bit) & 1);
            expected |= ((truth >> input) & 1) << bit;
        }
        CHECK(get(*b, 16) == expected);
    }
    put(*b, 16, 0x87); setup(*b, 0, 16, 1, 1, 16, 16, 0, 0xff); start(*b); CHECK(get(*b, 16) == 0x87);
}
static void patternsAndMasks() {
    auto b = std::make_unique<x68k::Ts6bga>();
    for (unsigned mode : {0x40u, 0x50u, 0x70u}) {
        const unsigned bytes = 1 + ((mode >> 4) & 3);
        for (unsigned i = 0; i < 64 * bytes; ++i) put(*b, 0x10000 + i, i);
        setup(*b, 0x10000, 0x20000, 13 * bytes, 11, 77, 64, mode); start(*b);
        for (unsigned y = 0; y < 11; ++y) for (unsigned x = 0; x < 13 * bytes; ++x)
            CHECK(get(*b, 0x20000 + y * 64 + x) == ((y % 8) * 8 * bytes + x % (8 * bytes)) % 256);
    }
    colors(*b);
    // Mono bitmap: each line is byte-aligned, source pitch ignored.
    put(*b, 0x10000, 0x80); put(*b, 0x10001, 0x40); put(*b, 0x10002, 0x40); put(*b, 0x10003, 0);
    setup(*b, 0x10000, 0x20000, 20, 2, 1000, 24, 0x90); start(*b);
    CHECK(get(*b, 0x20000) == 0x11 && get(*b, 0x20001) == 0x22);
    CHECK(get(*b, 0x20002) == 0xee && get(*b, 0x20003) == 0xdd);
    CHECK(get(*b, 0x20012) == 0x11 && get(*b, 0x20013) == 0x22);
    CHECK(get(*b, 0x20018) == 0xee && get(*b, 0x2001a) == 0x11);
    // GD5434 fill: all-one mono 8x8 pattern plus foreground registers.
    for (unsigned i = 0; i < 8; ++i) put(*b, 0x10000 + i, 255);
    setup(*b, 0x10000, 0x30000, 36, 9, 0, 40, 0xf0); start(*b);
    for (unsigned y = 0; y < 9; ++y) for (unsigned x = 0; x < 36; ++x)
        CHECK(get(*b, 0x30000 + y * 40 + x) == ((x % 4) + 1) * 0x11);
    // Transparent expansion preserves the zero bits; all four foreground byte lanes matter on GD5434.
    reg(*b, 0x13, 0x11); reg(*b, 0x15, 0x22);
    reg(*b, 0x12, 0xee); reg(*b, 0x14, 0xdd);
    put(*b, 0x10000, 0xa0);
    for (unsigned i = 0; i < 8; ++i) put(*b, 0x30000 + i, 0x77);
    setup(*b, 0x10000, 0x30000, 8, 1, 0, 16, 0x98); start(*b);
    CHECK(get(*b, 0x30000) == 0x11 && get(*b, 0x30001) == 0x22);
    CHECK(get(*b, 0x30002) == 0x77 && get(*b, 0x30003) == 0x77);
    CHECK(get(*b, 0x30004) == 0x11 && get(*b, 0x30005) == 0x22);
    // Reserved packed-24 expansion and color-key blit do not corrupt VRAM or leave Busy stuck.
    for (unsigned m : {0xa0u, 8u, 0x81u}) {
        setup(*b, 0, 0x30002, 1, 1, 0, 16, m); start(*b);
        CHECK(!b->bltBusy() && get(*b, 0x30002) == 0x77);
    }
}
static void hostTransferAndState() {
    auto m = std::make_unique<x68k::Machine>(); CHECK(m->setTs6bga(true)); auto& b = *m->ts6bga();
    reg(b, 11, 0x20); reg(b, 9, 0);
    setup(b, 0, 0x100, 3, 2, 0, 8, 4); start(b);
    CHECK(getreg(b, 0x31) == 11);
    b.memoryWrite(0x20, 2); b.memoryWrite(0x40, 1); b.memoryWrite(0x60, 99);
    CHECK(b.memoryRead(0x101) == 0); // waits for complete DWORD
    // Validate corrupt pending-FIFO states in the non-mutating CHECK pass.
    std::vector<uint8_t> boardState(64 * 1024);
    x68k::StateIO saveBoard(x68k::StateIO::SAVE, boardState.data(), boardState.size());
    b.state(saveBoard); CHECK(saveBoard.ok());
    const size_t stateBytes = saveBoard.pos();
    boardState[stateBytes - 1] = 4;
    x68k::StateIO badCount(x68k::StateIO::CHECK, boardState.data(), stateBytes);
    b.state(badCount); CHECK(!badCount.ok());
    boardState[stateBytes - 1] = 3; boardState[stateBytes - 9] = 1;
    x68k::StateIO badPos(x68k::StateIO::CHECK, boardState.data(), stateBytes);
    b.state(badPos); CHECK(!badPos.ok());
    std::vector<uint8_t> state(x68k::stateSize(*m)); size_t n = 0;
    CHECK(x68k::saveState(*m, state.data(), state.size(), n));
    size_t old; CHECK(!x68k::saveState(*m, state.data(), state.size(), old, 17));
    reg(b, 0x31, 4); CHECK(!b.bltBusy());
    const char* error = nullptr; CHECK(x68k::loadState(*m, state.data(), n, error));
    CHECK(b.bltBusy());
    // Attempts to change active parameters do not alter this transfer.
    reg(b, 0x20, 0); CHECK(getreg(b, 0x20) == 2);
    b.memoryWrite(0x80, 3); CHECK(b.bltBusy());
    b.memoryWrite(0, 5); b.memoryWrite(0, 4); b.memoryWrite(0, 88); b.memoryWrite(0, 6);
    CHECK(!b.bltBusy());
    CHECK(get(b, 0x100) == 1 && get(b, 0x101) == 2 && get(b, 0x102) == 3);
    CHECK(get(b, 0x108) == 4 && get(b, 0x109) == 5 && get(b, 0x10a) == 6);
    CHECK(get(b, 0x103) == 0 && get(b, 0x10b) == 0);
    colors(b); setup(b, 0, 0x200, 12, 2, 0, 16, 0x94); start(b);
    // Two rows, one monochrome byte per row. Remaining DWORD bytes are discarded.
    for (unsigned v : {0x40u, 0xa0u, 0u, 0u}) b.memoryWrite(0, v);
    CHECK(!b.bltBusy());
    CHECK(get(b, 0x200) == 0x11 && get(b, 0x202) == 0xee && get(b, 0x204) == 0x11);
    CHECK(get(b, 0x210) == 0xee && get(b, 0x212) == 0x11);
    // Cancel partial FIFO, then ordinary CPU window writes work again.
    setup(b, 0, 0x400, 4, 1, 0, 16, 4); start(b); b.memoryWrite(0, 0xab);
    reg(b, 0x31, 4); reg(b, 0x31, 0); b.memoryWrite(1, 0x5a);
    CHECK(get(b, 0) == 0x5a && get(b, 0x400) == 0);
    // An idle board still supports the previous version's state layout.
    state.resize(x68k::stateSize(*m)); CHECK(x68k::saveState(*m, state.data(), state.size(), n, 17));
    CHECK(x68k::loadState(*m, state.data(), n, error)); CHECK(!b.bltBusy());
}
int main() { copies(); rops(); patternsAndMasks(); hostTransferAndState(); return failures ? 1 : 0; }
