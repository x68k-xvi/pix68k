// 画面まわりのテスト: Video (CRTC、テキスト VRAM、グラフィック VRAM、パレット) と Renderer (画面の合成)。
//
// 期待値は「Inside X68000」(桑野雅彦、ソフトバンク、1992 年) の画面制御の章の説明から作った。
// 本に書かれていない動き (透明なドットの決まりなど) は、src/x68k/render.cpp の先頭にまとめた決まりを確かめている。
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "test_check.h"
#include "x68k/machine.h"
#include "x68k/render.h"
#include "x68k/state.h"
#include "x68k/video.h"
#include "x68k/video_replay.h"
#include "../../libretro/vita/paletted_frame.h"

namespace {

using x68k::paletteToRgb;
using x68k::Renderer;
using x68k::Video;

const uint32_t kUntouched = 0xdeadbeef;

struct Screen {
    Video v;
    Renderer r{v};
    uint32_t px[Renderer::MAX_WIDTH * Renderer::MAX_HEIGHT];

    void crtc(int n, uint16_t val) {
        v.crtcWrite((uint32_t)n * 2, (uint8_t)(val >> 8));
        v.crtcWrite((uint32_t)n * 2 + 1, (uint8_t)val);
    }
    // index: 0..255 がグラフィックパレット、256..511 がテキストパレット
    void palette(int index, uint16_t c) {
        v.paletteWrite((uint32_t)index * 2, (uint8_t)(c >> 8));
        v.paletteWrite((uint32_t)index * 2 + 1, (uint8_t)c);
    }
    void vc(int n, uint16_t val) {
        v.paletteWrite(0x400 + (uint32_t)n * 0x100, (uint8_t)(val >> 8));
        v.paletteWrite(0x401 + (uint32_t)n * 0x100, (uint8_t)val);
    }
    void sp16(uint32_t off, uint16_t val) {
        v.spriteWrite(off, (uint8_t)(val >> 8));
        v.spriteWrite(off + 1, (uint8_t)val);
    }
    void sp32(uint32_t off, uint32_t val) {
        sp16(off, (uint16_t)(val >> 16));
        sp16(off + 2, (uint16_t)val);
    }
    // グラフィック VRAM のワード (4 つのブロックの 4 ビットずつ) を、実画面 512x512 の (x, y) にそのまま書く
    void gword(int x, int y, uint16_t word) {
        const uint16_t saved = v.reg(20);
        crtc(20, (uint16_t)((saved & 0xff) | 0x0300)); // 65536 色のメモリモードではワードがそのまま並ぶ
        const uint32_t at = ((uint32_t)y * 512 + (uint32_t)x) * 2;
        v.gvramWrite(at, (uint8_t)(word >> 8));
        v.gvramWrite(at + 1, (uint8_t)word);
        crtc(20, saved); // メモリモードを元に戻す
    }
    // テキスト画面の (x, y) のドットを色コード code にする
    void tdot(int x, int y, int code) {
        for (int p = 0; p < 4; p++) {
            uint32_t off = (uint32_t)p * 0x20000 + (uint32_t)y * 128 + (uint32_t)(x >> 3);
            uint8_t b = v.tvramRead(off), bit = (uint8_t)(0x80 >> (x & 7));
            v.tvramWrite(off, (code >> p) & 1 ? (uint8_t)(b | bit) : (uint8_t)(b & ~bit));
        }
    }
    // スプライト n。x、y は画面上の位置 (レジスタには 16 を足して書く)
    void sprite(int n, int x, int y, uint16_t attr, int prw) {
        const uint32_t base = (uint32_t)n * 8;
        sp16(base + 0, (uint16_t)(x + 16));
        sp16(base + 2, (uint16_t)(y + 16));
        sp16(base + 4, attr);
        sp16(base + 6, (uint16_t)prw);
    }
    // 8x8 ドットのパターン n の row 行目 (ロングワードの最上位の 4 ビットが左端)
    void pcg8(int n, int row, uint32_t dots) { sp32(0x8000 + (uint32_t)n * 32 + (uint32_t)row * 4, dots); }
    // BG データエリア area の (column, row) のブロック
    void bgCell(int area, int column, int row, uint16_t cell) {
        sp16(0xc000 + (uint32_t)area * 0x2000 + (uint32_t)(row * 64 + column) * 2, cell);
    }

    // パレットの色。どれも $0000 でなく、番号ごとに違う
    static uint16_t gcolor(int i) { return (uint16_t)((((i * 37 + 11) & 0x7fff) << 1) | 1); }
    static uint16_t tcolor(int i) { return (uint16_t)((((i * 53 + 7) & 0x7fff) << 1) | 1); }
    uint32_t G(int i) const { return paletteToRgb(v.paletteColors()[i]); }
    uint32_t T(int i) const { return paletteToRgb(v.paletteColors()[256 + i]); }

    // 512x512 (31kHz)、画面はすべて OFF、パレット 0 は両方とも $0000
    void init() {
        v.reset();
        r.setTarget(px, Renderer::MAX_WIDTH, Renderer::MAX_WIDTH, Renderer::MAX_HEIGHT);
        crtc(0, 0x5b);
        crtc(2, 0x11);
        crtc(3, 0x51);
        crtc(6, 0x28);
        crtc(7, 0x228);
        crtc(20, 0x0015);
        for (int i = 1; i < 256; i++) {
            palette(i, gcolor(i));
            palette(256 + i, tcolor(i));
        }
        vc(0, 0);
        vc(1, 0x06e4);
        vc(2, 0);
        // スプライトコントローラの標準の設定 (図 57): H-DISP = R02 + 4、V-DISP = R06、解像度 = R20 の下位バイト
        sp16(0x80a, 0xff);
        sp16(0x80c, 0x15);
        sp16(0x80e, 0x28);
        sp16(0x810, 0x15);
        sp16(0x808, 0x0200);
    }
    // 256x256 (31kHz、同じラインを 2 回出す)。BG は 8x8 ドットで 2 面
    void mode256x256() {
        crtc(0, 0x2d);
        crtc(2, 0x06);
        crtc(3, 0x26);
        crtc(20, 0x0010);
        sp16(0x80c, 0x0a);
        sp16(0x810, 0x10);
    }
    uint32_t at(int x, int y) {
        r.renderLine(y);
        return px[y * Renderer::MAX_WIDTH + x];
    }
};

Screen& S = *new Screen;

// 色を足して 2 で割る (3.5.1 節の注): RGB の 5 ビットずつを足して 1 ビット右にずらす。輝度ビットは相手のもの
uint16_t mix(uint16_t base, uint16_t other) {
    unsigned g = ((base >> 11) + (other >> 11)) / 2;
    unsigned r = (((base >> 6) & 31) + ((other >> 6) & 31)) / 2;
    unsigned b = (((base >> 1) & 31) + ((other >> 1) & 31)) / 2;
    return (uint16_t)((g << 11) | (r << 6) | (b << 1) | (other & 1));
}

// ---- Video ----

// パレットの 1 語は G5 R5 B5 I1 (chibiakumas の X68000 の解説、x68000-dev-guide の docs/graphics.md)。
// 各色は (5 ビット << 1) | I の 6 ビットで、0〜63 を 0x00〜0xFF に広げる (広げ方はこのリポジトリで選んだもの)。
static uint32_t channel6To8(uint32_t v) { return (v << 2) | (v >> 4); }

void testPaletteColor() {
    // 6 ビットの階調の端と途中を、paletteToRgb を通して確かめる
    CHECK_EQ(paletteToRgb(0x0000), 0x000000u); // 0 → 0x00
    CHECK_EQ(paletteToRgb(0x0001), 0x040404u); // 1 → 0x04
    CHECK_EQ(paletteToRgb(0x3000), 0x003000u); // G 6: 6 ビットで 12 → 0x30 (四捨五入なら 0x31)
    CHECK_EQ(paletteToRgb(0xffff), 0xffffffu); // 63 → 0xff

    CHECK_EQ(paletteToRgb(0xfffe), 0xfbfbfbu); // I だけ 0: 各色 62
    CHECK_EQ(paletteToRgb(0x0003), 0x04040cu); // B の最下位ビットと I: B は 3、ほかは 1
    CHECK_EQ(paletteToRgb(0xa800), 0x00aa00u); // G (15-11 ビット) だけ 21: 6 ビットで 42
    CHECK_EQ(paletteToRgb(0x0540), 0xaa0000u); // R (10-6 ビット) だけ 21
    CHECK_EQ(paletteToRgb(0x002a), 0x0000aau); // B (5-1 ビット) だけ 21
    CHECK_EQ(paletteToRgb(0xa801), 0x04ae04u); // G 21 と I: G は 43、R と B は 1
    CHECK_EQ(paletteToRgb(0x8000), 0x008200u); // G の最上位ビット: 6 ビットで 32
    CHECK_EQ(paletteToRgb(0x0040), 0x080000u); // R の最下位ビット: 6 ビットで 2
    CHECK_EQ(paletteToRgb(0x0002), 0x000008u); // B の最下位ビット: 6 ビットで 2
    CHECK_EQ(paletteToRgb(0x07c1), 0xff0404u); // R 31 と I: R は 63、ほかは 1

    // 全部の値で、各色がその色の 5 ビットと I だけで決まること
    int bad = 0;
    for (uint32_t c = 0; c < 0x10000; c++) {
        uint32_t i = c & 1;
        uint32_t want = channel6To8(((c >> 6) & 31) << 1 | i) << 16 | channel6To8(((c >> 11) & 31) << 1 | i) << 8 |
                        channel6To8(((c >> 1) & 31) << 1 | i);
        if (paletteToRgb((uint16_t)c) != want) bad++;
    }
    CHECK_EQ(bad, 0);

    // パレット領域の読み書き。語は上位バイトが偶数番地 (ビッグエンディアン)
    static Video v;
    v.reset();
    v.paletteWrite(0x00e, 0x9a); // グラフィックパレット 7 番
    v.paletteWrite(0x00f, 0xbc);
    CHECK_EQ(v.paletteRead(0x00e), 0x9a);
    CHECK_EQ(v.paletteRead(0x00f), 0xbc);
    CHECK_EQ(v.paletteColors()[7], 0x9abc);
    CHECK_EQ(v.paletteXrgb()[7], paletteToRgb(0x9abc));
    v.paletteWrite(0x3fe, 0xab); // テキストとスプライトのパレットの最後 (511 番)
    v.paletteWrite(0x3ff, 0xcd);
    CHECK_EQ(v.paletteColors()[511], 0xabcd);
    CHECK_EQ(v.paletteXrgb()[511], paletteToRgb(0xabcd));
    v.paletteWrite(0x202, 0x56); // テキストパレット 1 番 ($E82202)
    v.paletteWrite(0x203, 0x78);
    CHECK_EQ(v.paletteColors()[257], 0x5678);
    CHECK_EQ(v.paletteColors()[7], 0x9abc); // 別の番号は変わらない

    // ビデオコントローラ R0/R1/R2 は 0x100 バイトの範囲のどの偶数番地からも同じ語が見える
    v.paletteWrite(0x4fe, 0x00);
    v.paletteWrite(0x4ff, 0x03);
    CHECK_EQ(v.videoControl(0), 0x0003);
    CHECK_EQ(v.paletteRead(0x400), 0x00);
    CHECK_EQ(v.paletteRead(0x401), 0x03);
    CHECK_EQ(v.paletteRead(0x481), 0x03);
    v.paletteWrite(0x500, 0x12);
    v.paletteWrite(0x5a1, 0xe4);
    CHECK_EQ(v.videoControl(1), 0x12e4);
    CHECK_EQ(v.paletteRead(0x5fe), 0x12);
    v.paletteWrite(0x642, 0x00);
    v.paletteWrite(0x643, 0x7f);
    CHECK_EQ(v.videoControl(2), 0x007f);
    CHECK_EQ(v.videoControl(0), 0x0003); // R2 に書いても R0 は変わらない
    // $E82700-$E827FF は資料がなく、main と同じ動作 (0x700 の 2 バイトにまとめる) にした
    v.paletteWrite(0x7fe, 0x5a);
    v.paletteWrite(0x7ff, 0xc3);
    CHECK_EQ(v.paletteRead(0x700), 0x5a);
    CHECK_EQ(v.paletteRead(0x701), 0xc3);
    CHECK_EQ(v.videoControl(2), 0x007f); // R2 は変わらない
    CHECK_EQ(v.paletteRead(0x800), 0); // 範囲の外は 0
}

void testTextVram() {
    S.init();
    // T0..T3 は $E00000 から $20000 バイトずつ (2.2.2 節)
    S.v.tvramWrite(0x00010, 0x11);
    S.v.tvramWrite(0x20010, 0x22);
    CHECK_EQ(S.v.tvramRead(0x00010), 0x11);
    CHECK_EQ(S.v.tvramRead(0x20010), 0x22);
    CHECK_EQ(S.v.tvramRead(0x40010), 0x00);
    // 同時アクセス (R21 の bit8、対象のプレーンは bit4..7。図 25)
    S.crtc(21, 0x0100 | 0x00a0); // T1 と T3
    S.v.tvramWrite(0x00020, 0x5a);
    CHECK_EQ(S.v.tvramRead(0x00020), 0x00);
    CHECK_EQ(S.v.tvramRead(0x20020), 0x5a);
    CHECK_EQ(S.v.tvramRead(0x40020), 0x00);
    CHECK_EQ(S.v.tvramRead(0x60020), 0x5a);
    // アクセスマスク (R21 の bit9、R23 のビットが 1 の所は書き換えない)
    S.crtc(21, 0x0200);
    S.crtc(23, 0xf00f);
    S.v.tvramWrite(0x00030, 0xff); // 偶数番地は R23 の上位バイト
    S.v.tvramWrite(0x00031, 0xff);
    CHECK_EQ(S.v.tvramRead(0x00030), 0x0f);
    CHECK_EQ(S.v.tvramRead(0x00031), 0xf0);
    // ラスタコピー (動作ポートの bit3、R22 の上位が転送元、下位が転送先、R21 の bit0..3 がプレーン)。1 ラスタは 4 ライン
    S.crtc(21, 0x0005); // T0 と T2
    S.crtc(23, 0);
    for (int p = 0; p < 4; p++) S.v.tvramWrite((uint32_t)p * 0x20000 + 3 * 512 + 100, (uint8_t)(0x40 + p));
    S.crtc(22, 0x0307); // ラスタ 3 から 7 へ
    S.v.crtcWrite(0x481, 0x08);
    S.v.horizontalBlank();
    CHECK_EQ(S.v.tvramRead(0x00000 + 7 * 512 + 100), 0x40);
    CHECK_EQ(S.v.tvramRead(0x20000 + 7 * 512 + 100), 0x00);
    CHECK_EQ(S.v.tvramRead(0x40000 + 7 * 512 + 100), 0x42);
    CHECK_EQ(S.v.tvramRead(0x60000 + 7 * 512 + 100), 0x00);
    // 止めれば写さない
    S.v.crtcWrite(0x481, 0x00);
    S.crtc(22, 0x0309);
    S.v.horizontalBlank();
    CHECK_EQ(S.v.tvramRead(0x00000 + 9 * 512 + 100), 0x00);
}

void testGraphicVram() {
    S.init();
    // 16 色: ページ p は $C00000 + p * $80000 から。1 ドット 1 ワードで、下位 4 ビットが色コード
    S.crtc(20, 0x0015);
    for (int p = 0; p < 4; p++) {
        uint32_t off = (uint32_t)p * 0x80000 + (10 * 512 + 20) * 2 + 1;
        S.v.gvramWrite(off, (uint8_t)(0xf1 + p));
        CHECK_EQ(S.v.gvramRead(off), 1 + p);
        CHECK_EQ(S.v.gvramRead(off - 1), 0);
    }
    // 4 つのブロックの 4 ビットずつが 1 ワードになる (ブロック 0 が最下位)
    CHECK_EQ(S.v.gvramWord(10 * 512 + 20), 0x4321);
    // 256 色: ページ 0 がブロック 0、1、ページ 1 がブロック 2、3 (コラムの図 20)
    S.crtc(20, 0x0115);
    CHECK_EQ(S.v.gvramRead((10 * 512 + 20) * 2 + 1), 0x21);
    CHECK_EQ(S.v.gvramRead(0x80000 + (10 * 512 + 20) * 2 + 1), 0x43);
    S.v.gvramWrite(0x80000 + (10 * 512 + 21) * 2 + 1, 0xab);
    CHECK_EQ(S.v.gvramWord(10 * 512 + 21), 0xab00);
    // 65536 色: ワードがそのまま
    S.crtc(20, 0x0315);
    CHECK_EQ(S.v.gvramRead((10 * 512 + 20) * 2), 0x43);
    CHECK_EQ(S.v.gvramRead((10 * 512 + 20) * 2 + 1), 0x21);
    // 実画面 1024x1024: 左上、右上、左下、右下がブロック 0、1、2、3
    S.crtc(20, 0x0416);
    S.v.gvramWrite((uint32_t)(10 * 1024 + 20 + 512) * 2 + 1, 0x09);       // 右上
    S.v.gvramWrite((uint32_t)((10 + 512) * 1024 + 20) * 2 + 1, 0x0a);     // 左下
    S.v.gvramWrite((uint32_t)((10 + 512) * 1024 + 20 + 512) * 2 + 1, 0x0b); // 右下
    CHECK_EQ(S.v.gvramWord(10 * 512 + 20), 0xba91);
    CHECK_EQ(S.v.gvramRead((uint32_t)(10 * 1024 + 20 + 512) * 2 + 1), 0x09);
}

void testFastClear() {
    S.init();
    S.gword(5, 5, 0xffff);
    S.gword(300, 400, 0xffff);
    // R21 の bit0..3 が消すブロック。動作ポートの bit1 で始まり、終わると 0 に戻る (図 24、図 25)
    S.crtc(21, 0x0005);
    S.v.crtcWrite(0x481, 0x02);
    S.v.beginVerticalDisplay();
    CHECK_EQ(S.v.crtcRead(0x481) & 0x02, 0x02); // 消している間
    CHECK_EQ(S.v.gvramWord(5 * 512 + 5), 0xf0f0);
    CHECK_EQ(S.v.gvramWord(400 * 512 + 300), 0xf0f0);
    S.v.beginVerticalDisplay();
    CHECK_EQ(S.v.crtcRead(0x481) & 0x02, 0x00);
    // 終わったあとは、もう消さない
    S.gword(5, 5, 0xffff);
    S.v.beginVerticalDisplay();
    CHECK_EQ(S.v.gvramWord(5 * 512 + 5), 0xffff);
}

// ---- Renderer: 画面の大きさとラスタ ----

void fillUntouched() {
    for (size_t i = 0; i < sizeof S.px / sizeof S.px[0]; i++) S.px[i] = kUntouched;
}
bool lineDrawn(int y) { return S.px[y * Renderer::MAX_WIDTH] != kUntouched; }

void testGeometryAndRasters() {
    S.init();
    int w, h;
    S.r.geometry(w, h);
    CHECK_EQ(w, 512);
    CHECK_EQ(h, 512);
    fillUntouched();
    S.r.onRaster(3);
    CHECK_EQ(S.r.width(), 512);
    CHECK_EQ(S.r.height(), 512);
    CHECK_EQ(lineDrawn(2), 0);
    CHECK_EQ(lineDrawn(3), 1);
    CHECK_EQ(lineDrawn(4), 0);
    CHECK_EQ(S.px[3 * Renderer::MAX_WIDTH + 511] != kUntouched, 1);
    CHECK_EQ(S.px[3 * Renderer::MAX_WIDTH + 512], kUntouched);
    // 表示期間の外のラスタでは何も描かない
    S.r.onRaster(-1);
    S.r.onRaster(512);
    CHECK_EQ(lineDrawn(0), 0);
    CHECK_EQ(lineDrawn(512), 0);

    // 256 ライン (同じラインを 2 回出す): 2 ラスタで 1 ライン
    S.crtc(20, 0x0010);
    fillUntouched();
    S.r.onRaster(6);
    S.r.onRaster(9);
    CHECK_EQ(S.r.height(), 256);
    CHECK_EQ(lineDrawn(3), 1);
    CHECK_EQ(lineDrawn(4), 0);
    CHECK_EQ(lineDrawn(6), 0);
    // インタレース: 1 ラスタで 2 ライン
    S.crtc(20, 0x0018);
    fillUntouched();
    S.r.onRaster(5);
    CHECK_EQ(S.r.height(), 1024);
    CHECK_EQ(lineDrawn(9), 0);
    CHECK_EQ(lineDrawn(10), 1);
    CHECK_EQ(lineDrawn(11), 1);
    CHECK_EQ(lineDrawn(12), 0);

    // 描く先より大きい分は描かない
    S.crtc(20, 0x0015);
    S.r.setTarget(S.px, Renderer::MAX_WIDTH, 100, 50);
    fillUntouched();
    S.r.renderFrame();
    CHECK_EQ(lineDrawn(49), 1);
    CHECK_EQ(lineDrawn(50), 0);
    CHECK_EQ(S.px[99] != kUntouched, 1);
    CHECK_EQ(S.px[100], kUntouched);
    // 幅が 1 行の画素数 (pitch) より大きくても、1 行に書くのは pitch まで (最後の行で描く先の外に書かない)
    {
        static uint32_t small[10 * 10 + 8];
        for (uint32_t& p : small) p = kUntouched;
        S.r.setTarget(small, 10, 100, 10);
        S.r.renderLine(9);
        CHECK_EQ(small[89], kUntouched);
        CHECK_EQ(small[90] != kUntouched, 1);
        CHECK_EQ(small[99] != kUntouched, 1);
        for (int i = 100; i < 108; i++) CHECK_EQ(small[i], kUntouched);
        S.r.renderLine(0);
        CHECK_EQ(small[9] != kUntouched, 1);
        CHECK_EQ(small[10], kUntouched);
    }
    // 描く先がなければ何もしない
    S.r.setTarget(nullptr, 0, 0, 0);
    S.r.renderFrame();

    uint32_t f = S.r.frameCount();
    S.r.onFrameEnd();
    CHECK_EQ(S.r.frameCount(), f + 1);
}

// ---- Renderer: テキスト ----

void testText() {
    S.init();
    S.vc(2, 0x0020);
    S.palette(256, 0x1234); // テキストだけの画面では、色コード 0 の所にテキストパレット 0 の色が出る
    S.tdot(5, 3, 9);
    S.tdot(6, 3, 15);
    S.tdot(511, 511, 1);
    CHECK_EQ(S.at(5, 3), S.T(9));
    CHECK_EQ(S.at(6, 3), S.T(15));
    CHECK_EQ(S.at(4, 3), S.T(0));
    CHECK_EQ(S.at(7, 3), S.T(0));
    CHECK_EQ(S.at(5, 2), S.T(0));
    CHECK_EQ(S.at(511, 511), S.T(1));
    // 4 つのプレーンがそれぞれ色コードの 1 ビット
    for (int code = 0; code < 16; code++) {
        S.tdot(40 + code, 8, code);
        CHECK_EQ(S.at(40 + code, 8), S.T(code));
    }
    // スクロール (R10、R11): 表示画面の左上に出る実画面の位置
    S.crtc(10, 8);
    S.crtc(11, 2);
    S.tdot(13, 5, 9);
    CHECK_EQ(S.at(5, 3), S.T(9));
    S.crtc(10, 3); // 8 の倍数でない
    CHECK_EQ(S.at(10, 3), S.T(9));
    CHECK_EQ(S.at(9, 3), S.T(0));
    CHECK_EQ(S.at(11, 3), S.T(0));
    // 縦は実画面の端でつながる。実画面は 1024x1024
    S.crtc(10, 0);
    S.crtc(11, 1023);
    S.tdot(2, 1, 6);
    CHECK_EQ(S.at(2, 2), S.T(6));
    S.tdot(700, 900, 4);
    S.crtc(10, 512);
    S.crtc(11, 512);
    CHECK_EQ(S.at(188, 388), S.T(4));
    // 表示 OFF
    S.vc(2, 0x0000);
    CHECK_EQ(S.at(188, 388), S.T(0));
}

// ---- Renderer: グラフィック ----

void testGraphic16Colors() {
    S.init();
    S.vc(0, 0);
    S.vc(1, 0x06e4);
    S.vc(2, 0x000f);
    S.gword(10, 10, 0x4321); // ブロック 0..3 に 1、2、3、4
    S.gword(11, 10, 0x4300);
    CHECK_EQ(S.at(10, 10), S.G(1)); // GD0 (ブロック 0) が一番手前
    CHECK_EQ(S.at(11, 10), S.G(3)); // 色コード 0 のページは奥のページが見える
    CHECK_EQ(S.at(12, 10), S.T(0));
    // R2 の bit0..3 は GD0..GD3 の ON/OFF
    S.vc(2, 0x000e);
    CHECK_EQ(S.at(10, 10), S.G(2));
    S.vc(2, 0x0008);
    CHECK_EQ(S.at(10, 10), S.G(4));
    S.vc(2, 0x0005);
    CHECK_EQ(S.at(10, 10), S.G(1));
    CHECK_EQ(S.at(11, 10), S.G(3));
    // GP0..GP3 で、GD がどのブロックから来るかが変わる。ON/OFF はページではなく優先順位に付く (3.2.1 節)
    S.vc(1, 0x061b); // GD0 = ブロック 3、GD1 = 2、GD2 = 1、GD3 = 0
    S.vc(2, 0x000f);
    CHECK_EQ(S.at(10, 10), S.G(4));
    S.vc(2, 0x000e); // GD0 (ブロック 3) を OFF
    CHECK_EQ(S.at(10, 10), S.G(3));
    S.vc(2, 0x0008); // GD3 (ブロック 0) だけ
    CHECK_EQ(S.at(10, 10), S.G(1));
    // 同じブロックをいくつかの優先順位に入れてもよい (コラム)
    S.vc(1, 0x0655); // すべてブロック 1
    S.vc(2, 0x0004);
    CHECK_EQ(S.at(10, 10), S.G(2));

    // スクロールはブロックごと (R12/R13 がブロック 0、R14/R15 がブロック 1 ...)
    S.vc(1, 0x06e4);
    S.vc(2, 0x000f);
    S.gword(30, 20, 0x0001);
    S.gword(40, 20, 0x0020);
    S.gword(50, 20, 0x0300);
    S.gword(60, 20, 0x4000);
    S.crtc(14, 5);
    S.crtc(15, 3);
    CHECK_EQ(S.at(30, 20), S.G(1));
    CHECK_EQ(S.at(35, 17), S.G(2));
    CHECK_EQ(S.at(40, 20), S.T(0));
    CHECK_EQ(S.at(50, 20), S.G(3));
    S.crtc(18, 10);
    S.crtc(19, 510);
    CHECK_EQ(S.at(50, 22), S.G(4));
    S.crtc(12, 1);
    S.crtc(13, 1);
    S.crtc(16, 2);
    S.crtc(17, 2);
    CHECK_EQ(S.at(29, 19), S.G(1));
    CHECK_EQ(S.at(48, 18), S.G(3));
    // はみ出した分は反対側がつながる (3.3 節)
    S.crtc(14, 500);
    S.crtc(15, 0);
    S.gword(2, 30, 0x0050);
    CHECK_EQ(S.at(14, 30), S.G(5));
    S.crtc(15, 500);
    S.gword(3, 4, 0x0060);
    CHECK_EQ(S.at(15, 16), S.G(6));
}

void testGraphic256Colors() {
    S.init();
    S.crtc(20, 0x0115);
    S.vc(0, 1);
    S.vc(1, 0x06e4);
    S.vc(2, 0x000f);
    S.gword(10, 10, 0xab12);
    S.gword(11, 10, 0xab00);
    S.gword(12, 10, 0x0010);
    // GD1:GD0 が手前のページ、GD3:GD2 が奥のページ
    CHECK_EQ(S.at(10, 10), S.G(0x12));
    CHECK_EQ(S.at(11, 10), S.G(0xab));
    CHECK_EQ(S.at(12, 10), S.G(0x10));
    // ページ 1 を手前にする設定 ($4E)
    S.vc(1, 0x064e);
    CHECK_EQ(S.at(10, 10), S.G(0xab));
    CHECK_EQ(S.at(12, 10), S.G(0x10));
    // 手前のページを OFF ('1100')
    S.vc(1, 0x06e4);
    S.vc(2, 0x000c);
    CHECK_EQ(S.at(10, 10), S.G(0xab));
    CHECK_EQ(S.at(12, 10), S.T(0));
    S.vc(2, 0x0003);
    CHECK_EQ(S.at(11, 10), S.T(0));
    // コラムの例: $D8 にすると、手前はブロック 0 が下位、ブロック 2 が上位。奥はブロック 1 が下位、ブロック 3 が上位
    S.vc(1, 0x06d8);
    S.vc(2, 0x0003);
    CHECK_EQ(S.at(10, 10), S.G(0xb2));
    S.vc(2, 0x000c);
    CHECK_EQ(S.at(10, 10), S.G(0xa1));
    // スクロールは R12/R13 (と R14/R15) がページ 0、R16/R17 (と R18/R19) がページ 1 (図 22)
    S.vc(1, 0x06e4);
    S.vc(2, 0x000f);
    S.gword(100, 100, 0x0034);
    S.gword(200, 100, 0x5600);
    S.crtc(12, 4);
    S.crtc(14, 4);
    S.crtc(13, 1);
    S.crtc(15, 1);
    CHECK_EQ(S.at(96, 99), S.G(0x34));
    CHECK_EQ(S.at(200, 100), S.G(0x56));
    S.crtc(16, 510);
    S.crtc(18, 510);
    CHECK_EQ(S.at(202, 100), S.G(0x56));
    // 上位と下位で表示開始位置を変えると、別々にずれる (コラム「スクロールと高速クリア制御のからくり」)
    S.crtc(14, 5);
    CHECK_EQ(S.at(96, 99), S.G(0x04));
    CHECK_EQ(S.at(95, 99), S.G(0x30));
}

void testGraphic65536() {
    S.init();
    S.crtc(20, 0x0315);
    S.vc(0, 3);
    S.vc(1, 0x06e4);
    S.vc(2, 0x000f);
    // パレットを適当な値で埋める。色は、ワードの上位バイトと下位バイトでそれぞれパレットを引いたものの OR
    uint32_t seed = 1;
    for (int i = 0; i < 256; i++) {
        seed = seed * 1103515245 + 12345;
        S.palette(i, (uint16_t)(seed >> 8));
    }
    for (int i = 0; i < 512; i++) {
        seed = seed * 1103515245 + 12345;
        S.gword(i, 7, (uint16_t)(seed >> 8));
    }
    S.gword(0, 7, 0xffff);
    S.gword(1, 7, 0x0100);
    S.r.renderLine(7);
    for (int i = 0; i < 512; i++) {
        uint16_t word = S.v.gvramWord(7 * 512 + (uint32_t)i);
        uint32_t want = paletteToRgb((uint16_t)(S.v.color64kHigh()[word >> 8] | S.v.color64kLow()[word & 0xff]));
        if (!want) want = S.T(0);
        CHECK_EQ(S.px[7 * Renderer::MAX_WIDTH + i], want);
    }
    // パレットを書き換えたら、次のラインから変わる
    S.palette(0, 0x1357);
    S.palette(1, 0x2468);
    S.r.renderLine(7);
    uint16_t word = S.v.gvramWord(7 * 512 + 1);
    CHECK_EQ(S.px[7 * Renderer::MAX_WIDTH + 1],
             paletteToRgb((uint16_t)(S.v.color64kHigh()[word >> 8] | S.v.color64kLow()[word & 0xff])));
    // GD3:GD2:GD1:GD0 で 1 ページ。GD の ON/OFF とブロックの入れ替えは 4 ビットごと
    S.gword(20, 9, 0x4321);
    S.vc(2, 0x000d); // GD1 を OFF
    S.r.renderLine(9);
    CHECK_EQ(S.px[9 * Renderer::MAX_WIDTH + 20], paletteToRgb((uint16_t)(S.v.color64kHigh()[0x43] | S.v.color64kLow()[0x01])));
    S.vc(1, 0x06d8); // GD1 = ブロック 2、GD2 = ブロック 1
    S.vc(2, 0x000f);
    S.r.renderLine(9);
    CHECK_EQ(S.px[9 * Renderer::MAX_WIDTH + 20], paletteToRgb((uint16_t)(S.v.color64kHigh()[0x42] | S.v.color64kLow()[0x31])));
    // ブロックごとのスクロール
    S.vc(1, 0x06e4);
    S.crtc(14, 1);
    S.gword(21, 9, 0x8765);
    S.r.renderLine(9);
    CHECK_EQ(S.px[9 * Renderer::MAX_WIDTH + 20], paletteToRgb((uint16_t)(S.v.color64kHigh()[0x43] | S.v.color64kLow()[0x61])));
}

void testGraphic1024Screen() {
    S.init();
    S.crtc(0, 0x89);
    S.crtc(2, 0x1c);
    S.crtc(3, 0x7c);
    S.crtc(20, 0x0416);
    S.vc(0, 4);
    S.vc(1, 0x06e4);
    S.vc(2, 0x0010);
    // 実画面の (600, 100) は右上 (GD1) の (88, 100)
    S.gword(88, 100, 0x4350);
    CHECK_EQ(S.at(600, 100), S.G(5));
    CHECK_EQ(S.at(88, 100), S.T(0)); // 左上 (GD0) は 0
    // スクロールは R12/R13 だけ (10 ビット)
    S.crtc(12, 512);
    CHECK_EQ(S.at(88, 100), S.G(5));
    CHECK_EQ(S.at(600, 100), S.T(0));
    S.crtc(13, 512);
    CHECK_EQ(S.at(88, 100), S.G(4));  // 右下 (GD3)
    CHECK_EQ(S.at(600, 100), S.G(3)); // 左下 (GD2)。横は実画面の端でつながる
    S.crtc(14, 77); // 使わない
    S.crtc(15, 77);
    CHECK_EQ(S.at(88, 100), S.G(4));
    // $D8 にすると GD1 と GD2 の領域が入れ替わる。$00 ならどの領域にも GD0 と同じものが出る (コラム)
    S.crtc(12, 0);
    S.crtc(13, 0);
    S.gword(88, 100, 0x4351);
    S.vc(1, 0x06d8);
    CHECK_EQ(S.at(600, 100), S.G(3));
    S.vc(1, 0x0600);
    CHECK_EQ(S.at(600, 100), S.G(1));
    CHECK_EQ(S.at(88, 100), S.G(1));
    // 表示の ON/OFF は bit4。bit0..3 は関係しない
    S.vc(1, 0x06e4);
    S.vc(2, 0x000f);
    CHECK_EQ(S.at(600, 100), S.T(0));
    S.vc(2, 0x0010);
    CHECK_EQ(S.at(600, 100), S.G(5));
}

// ---- Renderer: スプライトと BG ----

void testSprite() {
    S.init();
    S.vc(2, 0x0040);
    // 16x16 ドットのパターン 1 は 8x8 ドットのパターン 4 (左上)、5 (左下)、6 (右上)、7 (右下)
    S.pcg8(4, 0, 0x12345678);
    S.pcg8(6, 0, 0x9abcdef0);
    S.pcg8(5, 0, 0x10000000);
    S.pcg8(7, 7, 0x0000000e);
    S.pcg8(4, 2, 0x00200000);
    S.sprite(0, 20, 30, 0x0301, 3);
    // 色コードは上位 4 ビットがスプライトのレジスタ、下位 4 ビットがパターン。ロングワードの最上位の 4 ビットが左端
    for (int i = 0; i < 15; i++) CHECK_EQ(S.at(20 + i, 30), S.T(0x30 + i + 1));
    CHECK_EQ(S.at(35, 30), S.T(0)); // パターンのドットが 0 の所
    CHECK_EQ(S.at(19, 30), S.T(0));
    CHECK_EQ(S.at(22, 32), S.T(0x32));
    CHECK_EQ(S.at(20, 38), S.T(0x31));
    CHECK_EQ(S.at(35, 45), S.T(0x3e));
    CHECK_EQ(S.at(20, 46), S.T(0));
    // 左右反転、上下反転
    S.sprite(0, 20, 30, 0x4301, 3);
    CHECK_EQ(S.at(35, 30), S.T(0x31));
    CHECK_EQ(S.at(21, 30), S.T(0x3f));
    CHECK_EQ(S.at(20, 45), S.T(0x3e));
    S.sprite(0, 20, 30, 0x8301, 3);
    CHECK_EQ(S.at(20, 45), S.T(0x31));
    CHECK_EQ(S.at(35, 30), S.T(0x3e));
    S.sprite(0, 20, 30, 0xc301, 3);
    CHECK_EQ(S.at(35, 45), S.T(0x31));
    CHECK_EQ(S.at(20, 30), S.T(0x3e));
    // 座標 (16, 16) が画面の左上。それより小さいと、はみ出した分は出ない
    S.sprite(0, 0, 0, 0x0301, 3);
    CHECK_EQ(S.at(0, 0), S.T(0x31));
    S.sprite(0, -8, 0, 0x0301, 3);
    CHECK_EQ(S.at(0, 0), S.T(0x39));
    CHECK_EQ(S.at(6, 0), S.T(0x3f));
    S.sprite(0, -16, 0, 0x0301, 3);
    CHECK_EQ(S.at(0, 0), S.T(0));
    S.sprite(0, 0, -15, 0x0301, 3);
    CHECK_EQ(S.at(15, 0), S.T(0x3e));
    S.sprite(0, 505, 500, 0x0301, 3);
    CHECK_EQ(S.at(511, 500), S.T(0x37));
    // 第 4 ワードの下位 2 ビットが 0 なら出ない
    S.sprite(0, 20, 30, 0x0301, 0);
    CHECK_EQ(S.at(20, 30), S.T(0));
    // 番号の小さいスプライトが手前
    S.sprite(3, 20, 30, 0x0301, 1);
    S.sprite(5, 22, 30, 0x0501, 1);
    CHECK_EQ(S.at(22, 30), S.T(0x33));
    CHECK_EQ(S.at(36, 30), S.T(0x5f));
    // スプライトどうしは、優先度によらず番号の小さいほうが手前 (優先度は勝ったドットが BG の前か後ろかを決めるだけ。
    // Gradius II のサウンドモードのボタンの枠で、XM6 TypeG と同じ画面になる)
    S.sprite(5, 22, 30, 0x0501, 3);
    CHECK_EQ(S.at(22, 30), S.T(0x33));
    // 番号の大きいスプライトが優先度 3 でも、番号の小さいスプライトの透明なドットの所には出る
    CHECK_EQ(S.at(35, 30), S.T(0x5e));
    CHECK_EQ(S.at(36, 30), S.T(0x5f));
    // BG コントロールの bit9 が 0 なら出ない。768 ドットの画面でも出ない
    S.sp16(0x808, 0x0000);
    CHECK_EQ(S.at(22, 30), S.T(0));
    S.sp16(0x808, 0x0200);
    S.crtc(0, 0x89);
    S.crtc(2, 0x1c);
    S.crtc(3, 0x7c);
    S.crtc(20, 0x0016);
    S.sp16(0x80c, 0x20);
    CHECK_EQ(S.at(22, 30), S.T(0));
}

void testSpriteLimit() {
    S.init();
    S.vc(2, 0x0040);
    S.pcg8(4, 0, 0x10000000);
    // 同じ水平線上には 32 個まで。33 個目からは出ない (2.4.2 節)
    for (int n = 0; n < 40; n++) S.sprite(n, n * 8, 50, 0x0101, 3);
    S.r.renderLine(50);
    for (int n = 0; n < 40; n++) CHECK_EQ(S.px[50 * Renderer::MAX_WIDTH + n * 8], n < 32 ? S.T(0x11) : S.T(0));
    // 表示しないスプライトと、別のラインのスプライトは数えない
    S.sprite(3, 3 * 8, 50, 0x0101, 0);
    S.sprite(4, 4 * 8, 80, 0x0101, 3);
    S.r.renderLine(50);
    CHECK_EQ(S.px[50 * Renderer::MAX_WIDTH + 33 * 8], S.T(0x11));
    CHECK_EQ(S.px[50 * Renderer::MAX_WIDTH + 34 * 8], S.T(0));
    // 横にはみ出して見えないスプライトも数える
    for (int n = 0; n < 32; n++) S.sprite(n, -16, 50, 0x0101, 3);
    S.r.renderLine(50);
    CHECK_EQ(S.px[50 * Renderer::MAX_WIDTH + 32 * 8], S.T(0));
}

void testSpriteRowsAfterCopy() {
    S.init();
    S.vc(2, 0x0040);
    S.pcg8(4, 0, 0x10000000);
    S.sprite(0, 10, 20, 0x0301, 3);
    CHECK_EQ(S.at(10, 20), S.T(0x31));
    static Video saved;
    x68k::copyVideoState(S.v, saved);
    S.sprite(0, 10, 40, 0x0301, 3);
    CHECK_EQ(S.at(10, 20), S.T(0));
    CHECK_EQ(S.at(10, 40), S.T(0x31));
    x68k::copyVideoState(saved, S.v);
    CHECK_EQ(S.at(10, 20), S.T(0x31));
    CHECK_EQ(S.at(10, 40), S.T(0));
}

void testSpriteRowBounds() {
    S.init();
    S.vc(2, 0x0040);
    S.crtc(20, 0x0019); // Interlaced 1024-line surface.
    S.sp16(0x810, 0x0019);
    for (int row = 0; row < 8; row++) {
        S.pcg8(4, row, 0x11111111);
        S.pcg8(5, row, 0x11111111);
    }
    for (int y = 1020; y <= 1023; y++) {
        S.sprite(0, 0, y - 16, 0x0301, 3);
        CHECK_EQ(S.at(0, y - 17), S.T(0));
        CHECK_EQ(S.at(0, y - 16), S.T(0x31));
        CHECK_EQ(S.at(0, y - 1), S.T(0x31));
        CHECK_EQ(S.at(0, y), S.T(0));
    }
    S.init();
    S.vc(2, 0x0040);
    S.pcg8(4, 0, 0x10000000);
    S.sprite(0, 0, -16, 0x0301, 3); // Raw Y=0.
    S.sp16(0x80e, 0x38); // At display y=0, sprite scanline is -16.
    CHECK_EQ(S.at(0, 0), S.T(0x31));
    S.sp16(0x80e, 0x39); // Scanline -17 has no sprite candidates.
    CHECK_EQ(S.at(0, 0), S.T(0));
    CHECK_EQ(S.at(0, 1), S.T(0x31));
}

void testBg() {
    S.init();
    S.mode256x256();
    S.vc(2, 0x0040);
    S.pcg8(4, 0, 0x10000000);
    S.pcg8(5, 0, 0x22000000);
    S.pcg8(8, 0, 0x33300000); // 16x16 ドットのパターン 2 の左上
    S.pcg8(4, 3, 0x00000007);
    S.bgCell(0, 0, 0, 0x0104); // BG データエリア 0 の左上: パターン 4、色コードの上位 1
    S.bgCell(1, 0, 0, 0x0205); // BG データエリア 1 の左上: パターン 5、色コードの上位 2
    S.sp16(0x808, 0x0200 | 0x01 | 0x08 | 0x10); // BG0 は エリア 0、BG1 は エリア 1
    // BG0 はいつも BG1 より手前
    CHECK_EQ(S.at(0, 0), S.T(0x11));
    CHECK_EQ(S.at(1, 0), S.T(0x22));
    CHECK_EQ(S.at(2, 0), S.T(0));
    CHECK_EQ(S.at(7, 3), S.T(0x17));
    // スプライトは、3 なら BG の手前、2 なら BG0 と BG1 の間、1 なら BG の後ろ (3.2.2 節)
    S.sprite(0, 0, 0, 0x0402, 3);
    CHECK_EQ(S.at(0, 0), S.T(0x43));
    CHECK_EQ(S.at(1, 0), S.T(0x43));
    CHECK_EQ(S.at(2, 0), S.T(0x43));
    S.sprite(0, 0, 0, 0x0402, 2);
    CHECK_EQ(S.at(0, 0), S.T(0x11));
    CHECK_EQ(S.at(1, 0), S.T(0x43));
    S.sprite(0, 0, 0, 0x0402, 1);
    CHECK_EQ(S.at(0, 0), S.T(0x11));
    CHECK_EQ(S.at(1, 0), S.T(0x22));
    CHECK_EQ(S.at(2, 0), S.T(0x43));
    S.sprite(0, 0, 0, 0x0402, 0);
    CHECK_EQ(S.at(2, 0), S.T(0));
    // それぞれの ON/OFF と、使う BG データエリア
    S.sp16(0x808, 0x0200 | 0x08 | 0x10);
    CHECK_EQ(S.at(0, 0), S.T(0x22));
    S.sp16(0x808, 0x0200 | 0x01);
    CHECK_EQ(S.at(1, 0), S.T(0));
    S.sp16(0x808, 0x0200 | 0x01 | 0x02); // BG0 が エリア 1
    CHECK_EQ(S.at(1, 0), S.T(0x22));
    S.sp16(0x808, 0x0200 | 0x08); // BG1 が エリア 0
    CHECK_EQ(S.at(0, 0), S.T(0x11));
    // パターンのドットがすべて 0 のブロックは、色コードの上位 4 ビット (bit11-8) が 0 でなくても透明
    // (上位 4 ビットだけの色コード $30 にはならない。本に記述がなく、render.cpp の先頭の注釈のとおりに決めた)
    S.sp16(0x808, 0x0200 | 0x01);
    S.bgCell(0, 5, 5, 0x0300 | 200); // パターン 200 は何も書いていない
    S.palette(256 + 0x30, 0xffff);
    // ブロック (5, 5) の中のドット: 8x8 ドットのパターンなら (43, 43)、16x16 ドットなら (83, 83)
    CHECK_EQ(S.at(43, 43), S.T(0));
    CHECK_EQ(S.at(83, 83), S.T(0));
    S.palette(256, 0x1234);
    CHECK_EQ(S.at(43, 43), paletteToRgb(0x1234)); // どの画面も透明なので、テキストパレット 0 の色
    CHECK_EQ(S.at(83, 83), paletteToRgb(0x1234));
    S.palette(256, 0);
    S.palette(256 + 0x30, Screen::tcolor(0x30));
    // 上下と左右の反転
    S.bgCell(0, 0, 0, 0x4104);
    CHECK_EQ(S.at(7, 0), S.T(0x11));
    CHECK_EQ(S.at(0, 3), S.T(0x17));
    S.bgCell(0, 0, 0, 0x8104);
    CHECK_EQ(S.at(0, 7), S.T(0x11));
    CHECK_EQ(S.at(7, 4), S.T(0x17));
    // スクロール。実画面は 64x64 ブロック (512x512 ドット) で、はみ出した分は反対側がつながる
    S.bgCell(0, 0, 0, 0x0104);
    S.bgCell(0, 3, 2, 0x0505);
    S.sp16(0x800, 24 - 5);
    S.sp16(0x802, 16 - 9);
    CHECK_EQ(S.at(5, 9), S.T(0x52));
    CHECK_EQ(S.at(6, 9), S.T(0x52));
    CHECK_EQ(S.at(7, 9), S.T(0));
    S.sp16(0x800, 512 - 10);
    S.sp16(0x802, 512 - 20);
    CHECK_EQ(S.at(10, 20), S.T(0x11));
    S.bgCell(0, 63, 63, 0x0605);
    CHECK_EQ(S.at(2, 12), S.T(0x62));
    // BG1 のスクロールは $EB0804 / $EB0806
    S.sp16(0x808, 0x0200 | 0x08 | 0x10);
    S.sp16(0x804, 8);
    S.bgCell(1, 1, 0, 0x0705);
    CHECK_EQ(S.at(0, 0), S.T(0x72));

    // 512 ドット: BG は 16x16 ドットで BG0 だけ。実画面は 1024x1024 ドット
    S.init();
    S.vc(2, 0x0040);
    S.pcg8(8, 0, 0x33300000);
    S.pcg8(11, 7, 0x0000000c); // パターン 2 の右下の隅
    S.bgCell(0, 1, 1, 0x0102);
    S.bgCell(1, 0, 0, 0x0202);
    S.sp16(0x808, 0x0200 | 0x01 | 0x08 | 0x10);
    CHECK_EQ(S.at(16, 16), S.T(0x13));
    CHECK_EQ(S.at(31, 31), S.T(0x1c));
    CHECK_EQ(S.at(0, 0), S.T(0)); // BG1 は出ない
    S.bgCell(0, 1, 1, 0xc102);
    CHECK_EQ(S.at(16, 16), S.T(0x1c));
    CHECK_EQ(S.at(31, 31), S.T(0x13));
    S.bgCell(0, 63, 63, 0x0302);
    S.sp16(0x800, 1024 - 16);
    S.sp16(0x802, 1024 - 16);
    CHECK_EQ(S.at(0, 0), S.T(0x33));
    CHECK_EQ(S.at(32, 32), S.T(0x1c));
}

void testSpritePosition() {
    S.init();
    S.vc(2, 0x0040);
    S.pcg8(4, 0, 0x10000000);
    S.sprite(0, 40, 40, 0x0101, 3);
    S.bgCell(0, 0, 0, 0x0201);
    S.sp16(0x808, 0x0201);
    CHECK_EQ(S.at(40, 40), S.T(0x11));
    CHECK_EQ(S.at(0, 0), S.T(0x21));
    // 水平表示位置が R02 + 4、垂直表示位置が R06 のときに、ほかの画面と位置が合う (5.3 節)。
    // 大きくすると、その分だけ右と下にずれる (1 につき 8 ドット、1 ラスタ)
    S.sp16(0x80c, 0x15 + 2);
    CHECK_EQ(S.at(56, 40), S.T(0x11));
    CHECK_EQ(S.at(16, 0), S.T(0x21));
    S.sp16(0x80c, 0x15 - 1);
    CHECK_EQ(S.at(32, 40), S.T(0x11));
    S.sp16(0x80c, 0x15);
    S.sp16(0x80e, 0x28 + 3);
    CHECK_EQ(S.at(40, 43), S.T(0x11));
    CHECK_EQ(S.at(0, 3), S.T(0x21));
    S.sp16(0x80e, 0x28);
    // R02 は bit7-0、R06 は bit9-0 のレジスタ (X68k:IOMAP)。幅の外のビットを立てても位置は変わらない (#152)
    S.crtc(2, 0xff00 | 0x11);
    S.crtc(6, 0xfc00 | 0x28);
    CHECK_EQ(S.at(40, 40), S.T(0x11));
    CHECK_EQ(S.at(0, 0), S.T(0x21));
    // 幅の中のビットは効く (R02 を 1 減らすと 8 ドット右、R06 を 1 減らすと 1 ラスタ下)
    S.crtc(2, 0xff00 | 0x10);
    S.crtc(6, 0xfc00 | 0x27);
    CHECK_EQ(S.at(48, 41), S.T(0x11));
    CHECK_EQ(S.at(8, 1), S.T(0x21));
    S.crtc(2, 0x11);
    S.crtc(6, 0x28);

    // スプライト画面のドットの幅は表示画面と同じ。$EB0810 の HRES は BG のパターンの大きさと面数だけを変える
    // (超連射68K の設定: CRTC は 512 ドットのクロックで 256 ドットを表示、$EB0810 は 256 ドット)
    S.crtc(2, 0x12);
    S.crtc(3, 0x32);
    S.crtc(20, 0x0111);
    S.sp16(0x80c, 0x16);
    S.sp16(0x810, 0x10);
    S.sprite(0, 40, 40, 0x0101, 3);
    S.pcg8(1, 0, 0x30000000);
    S.bgCell(0, 1, 0, 0x0201); // 8x8 ドットのパターン 1
    CHECK_EQ(S.at(40, 40), S.T(0x11));
    CHECK_EQ(S.at(41, 40), S.T(0));
    CHECK_EQ(S.r.width(), 256);
    CHECK_EQ(S.at(8, 0), S.T(0x23));

    // 縦の解像度が違うとき: CRTC が 512 ライン、スプライト画面が 256 ライン (同じラインを 2 回出す) なら、
    // スプライト画面の 1 ラインが表示画面の 2 ラインになる
    S.init();
    S.vc(2, 0x0040);
    S.pcg8(4, 0, 0x10000000);
    S.pcg8(4, 1, 0x02000000);
    S.sprite(0, 40, 40, 0x0101, 3);
    S.sp16(0x810, 0x11);
    CHECK_EQ(S.at(40, 80), S.T(0x11));
    CHECK_EQ(S.at(40, 81), S.T(0x11));
    CHECK_EQ(S.at(41, 82), S.T(0x12));
    CHECK_EQ(S.at(41, 83), S.T(0x12));
    CHECK_EQ(S.at(40, 40), S.T(0));
}

// ---- Renderer: 画面どうしの重なり ----

void testLayerPriority() {
    S.init();
    S.vc(2, 0x006f);
    S.pcg8(4, 0, 0x30000000);
    S.sprite(0, 4, 4, 0x0401, 3);
    S.sprite(1, 12, 4, 0x0401, 3);
    S.sprite(2, 28, 4, 0x0401, 3);
    S.tdot(4, 4, 5);
    S.tdot(8, 4, 5);
    S.tdot(12, 4, 5);
    S.tdot(20, 4, 5);
    S.gword(4, 4, 6);
    S.gword(8, 4, 6);
    S.gword(16, 4, 6);
    S.gword(28, 4, 6);
    // x = 4: 3 つとも、8: テキストとグラフィック、12: スプライトとテキスト、16: グラフィックだけ、
    // 20: テキストだけ、24: 何もない、28: スプライトとグラフィック
    const uint32_t sp = S.T(0x43), tx = S.T(5), gr = S.G(6), none = S.T(0);
    struct Case {
        uint16_t r1; // bit13-12 スプライト、bit11-10 テキスト、bit9-8 グラフィック。0 が一番手前
        uint32_t want[7];
    };
    const Case cases[] = {
        {0x06e4, {sp, tx, sp, gr, tx, none, sp}}, // スプライト、テキスト、グラフィック
        {0x09e4, {sp, gr, sp, gr, tx, none, sp}}, // スプライト、グラフィック、テキスト
        {0x12e4, {tx, tx, tx, gr, tx, none, sp}}, // テキスト、スプライト、グラフィック
        {0x21e4, {tx, tx, tx, gr, tx, none, gr}}, // テキスト、グラフィック、スプライト
        {0x18e4, {gr, gr, sp, gr, tx, none, gr}}, // グラフィック、スプライト、テキスト
        {0x24e4, {gr, gr, tx, gr, tx, none, gr}}, // グラフィック、テキスト、スプライト
    };
    for (const Case& c : cases) {
        S.vc(1, c.r1);
        S.r.renderLine(4);
        for (int i = 0; i < 7; i++) CHECK_EQ(S.px[4 * Renderer::MAX_WIDTH + 4 + i * 4], c.want[i]);
    }
    // 画面ごとの ON/OFF (R2 の bit5 がテキスト、bit6 がスプライト)
    S.vc(1, 0x06e4);
    S.vc(2, 0x002f);
    CHECK_EQ(S.at(4, 4), tx);
    S.vc(2, 0x004f);
    CHECK_EQ(S.at(4, 4), sp);
    CHECK_EQ(S.at(8, 4), gr);
    S.vc(2, 0x0060);
    CHECK_EQ(S.at(16, 4), none);
    S.vc(2, 0x0000);
    CHECK_EQ(S.at(4, 4), none);
}

// 透明なドットの決まり (render.cpp の先頭の「透明なドットと、重ね方」)
void testTransparency() {
    S.init();
    S.vc(2, 0x006f);
    S.pcg8(4, 0, 0x30000000);
    S.sprite(0, 4, 4, 0x0401, 3);
    S.tdot(8, 4, 5);
    S.gword(4, 4, 6);
    S.gword(8, 4, 6);
    S.gword(12, 4, 6);
    S.gword(16, 4, 7);

    // グラフィックは、パレットの色が $0000 のドットが透明 (色コードによらない)
    S.vc(1, 0x18e4); // グラフィック、スプライト、テキスト
    S.palette(6, 0x0000);
    CHECK_EQ(S.at(4, 4), S.T(0x43));
    CHECK_EQ(S.at(8, 4), S.T(5));
    CHECK_EQ(S.at(12, 4), S.T(0));
    CHECK_EQ(S.at(16, 4), S.G(7));
    // グラフィックパレット 0 が $0000 でなければ、色コード 0 の所も透明でない
    S.palette(0, 0x1234);
    CHECK_EQ(S.at(20, 4), S.G(0));
    CHECK_EQ(S.at(4, 4), S.T(0x43)); // 色コード 6 は透明のまま
    S.vc(1, 0x06e4); // グラフィックが一番奥でも同じ
    CHECK_EQ(S.at(20, 4), S.G(0));
    CHECK_EQ(S.at(12, 4), S.T(0));
    S.palette(0, 0x0000);
    S.palette(6, Screen::gcolor(6));

    // テキストパレット 0 が $0000 でないとき:
    //   テキストもスプライトもグラフィックより手前なら、色コード 0 の所もその色で、グラフィックは見えない
    S.palette(256, 0x4321);
    S.vc(1, 0x06e4);
    CHECK_EQ(S.at(12, 4), S.T(0));
    CHECK_EQ(S.at(4, 4), S.T(0x43));
    S.vc(1, 0x12e4);
    CHECK_EQ(S.at(12, 4), S.T(0));
    //   どちらかがグラフィックより奥なら、グラフィックが見える。グラフィックが透明の所はテキストパレット 0 の色
    //   (悪魔城ドラキュラ: スプライト、グラフィック、テキストの順で、テキストは OFF、テキストパレット 0 は $0842)
    S.vc(1, 0x09e4);
    S.vc(2, 0x004f);
    CHECK_EQ(S.at(12, 4), S.G(6));
    CHECK_EQ(S.at(4, 4), S.T(0x43));
    CHECK_EQ(S.at(20, 4), S.T(0));
    S.vc(2, 0x006f);
    CHECK_EQ(S.at(12, 4), S.G(6));
    CHECK_EQ(S.at(8, 4), S.G(6));
    S.vc(1, 0x21e4); // テキスト、グラフィック、スプライト
    CHECK_EQ(S.at(12, 4), S.G(6));
    CHECK_EQ(S.at(8, 4), S.T(5));
    CHECK_EQ(S.at(20, 4), S.T(0));
    S.vc(1, 0x24e4); // グラフィックが一番手前
    CHECK_EQ(S.at(12, 4), S.G(6));
    CHECK_EQ(S.at(20, 4), S.T(0));
    //   グラフィックを表示していなければ、どの並びでもテキストパレット 0 の色
    S.vc(2, 0x0060);
    CHECK_EQ(S.at(12, 4), S.T(0));
    S.vc(2, 0x0000);
    CHECK_EQ(S.at(12, 4), S.T(0));
    S.vc(2, 0x000f);
    S.vc(1, 0x06e4);
    CHECK_EQ(S.at(12, 4), S.T(0)); // テキストもスプライトも OFF でも、グラフィックより手前の位置にある
    S.vc(1, 0x24e4);
    CHECK_EQ(S.at(12, 4), S.G(6));
    S.palette(256, 0x0000);

    // テキストやスプライトも、パレットの色が $0000 なら透明
    S.vc(2, 0x006f);
    S.vc(1, 0x06e4);
    S.palette(256 + 5, 0x0000);
    CHECK_EQ(S.at(8, 4), S.G(6));
    S.palette(256 + 5, Screen::tcolor(5));
    // スプライトのドットの色が $0000 なら、その下の BG もテキストも出ずに、グラフィックが見える (Seafy 氏の資料)
    S.bgCell(0, 0, 0, 0x0501);
    S.sp16(0x808, 0x0201);
    S.sprite(0, 0, 4, 0x0401, 3);
    S.tdot(0, 4, 5);
    S.gword(0, 4, 7);
    S.pcg8(4, 4, 0x30000000);
    CHECK_EQ(S.at(0, 4), S.T(0x43));
    S.palette(256 + 0x43, 0x0000);
    CHECK_EQ(S.at(0, 4), S.G(7));
    S.gword(0, 4, 0);
    CHECK_EQ(S.at(0, 4), 0x000000); // グラフィックも透明なら、そのスプライトの色 ($0000)
    S.sprite(0, 0, 4, 0x0401, 0);
    CHECK_EQ(S.at(0, 4), S.T(0x53)); // スプライトがなければ BG
    // スプライトがテキストより奥なら、テキストのドットがある所はテキスト
    S.sprite(0, 0, 4, 0x0401, 3);
    S.vc(1, 0x12e4);
    CHECK_EQ(S.at(0, 4), S.T(5));
}

// ---- Renderer: 半透明と特殊プライオリティ ----

void testHalfTransparency() {
    S.init();
    // リスト 7 (V2.C) の設定: 優先順位はグラフィック、テキスト、スプライトの順 ($24xx)、R2 の上位は $1F
    S.vc(1, 0x24e4);
    S.vc(2, 0x1f2f);
    // 端数の出る色にする: 赤 31 + 0、緑 1 + 2、青 4 + 5
    const uint16_t c2 = (uint16_t)((1 << 11) | (31 << 6) | (4 << 1) | 1);
    const uint16_t c7 = (uint16_t)((2 << 11) | (0 << 6) | (5 << 1) | 0);
    S.palette(2, c2);
    S.palette(7, c7);
    CHECK_EQ(mix(c2, c7), (uint16_t)((1 << 11) | (15 << 6) | (4 << 1) | 0)); // 端数は捨てる。輝度は相手のもの
    const uint16_t t5 = Screen::tcolor(5), t0 = 0x5a5a;
    S.palette(256, t0);
    S.gword(4, 4, 0x0073); // ベースページ (GD0) が 3、セカンドページ (GD1) が 7
    S.gword(8, 4, 0x0072); // 最下位ビットが 0: 対象でない
    S.gword(12, 4, 0x0003);
    S.gword(16, 4, 0x0071); // 最下位ビットを 0 にすると 0
    S.gword(20, 4, 0x0703); // 3 番目のページ
    S.gword(24, 4, 0x0073);
    S.tdot(4, 4, 5);
    S.tdot(8, 4, 5);
    S.tdot(12, 4, 5);

    // セカンドページとテキストの両方 ($1F)
    CHECK_EQ(S.at(4, 4), paletteToRgb(mix(mix(c2, c7), t5)));
    CHECK_EQ(S.at(8, 4), paletteToRgb(c2));
    CHECK_EQ(S.at(12, 4), paletteToRgb(mix(mix(c2, Screen::gcolor(1)), t5))); // セカンドページが 0 ならパレット 1
    CHECK_EQ(S.at(16, 4), paletteToRgb(c7));
    CHECK_EQ(S.at(20, 4), paletteToRgb(mix(mix(c2, Screen::gcolor(1)), t0))); // 3 番目のページとは混ぜない
    CHECK_EQ(S.at(24, 4), paletteToRgb(mix(mix(c2, c7), t0))); // テキストのドットがなければテキストパレット 0
    // セカンドページだけ ($1E)、テキストだけ ($1D)、どちらもなし ($1C)
    S.vc(2, 0x1e2f);
    CHECK_EQ(S.at(4, 4), paletteToRgb(mix(c2, c7)));
    // R の成分が両方とも奇数の組 (c2 と c7 では R は 31 と 0)。期待する値は mix() を使わず数字で書く。
    // R=3 と R=5 は 4 (足してから 2 で割る)。どの成分も 31 どうしなら 31 のままで、となりの成分に繰り上がらない
    S.palette(2, (uint16_t)(3 << 6));
    S.palette(7, (uint16_t)((5 << 6) | 1));
    CHECK_EQ(S.at(4, 4), paletteToRgb((uint16_t)((4 << 6) | 1)));
    S.palette(2, 0xfffe);
    S.palette(7, 0xffff);
    CHECK_EQ(S.at(4, 4), paletteToRgb(0xffff));
    S.palette(2, (uint16_t)((1 << 11) | (3 << 6) | (1 << 1)));
    S.palette(7, (uint16_t)((1 << 11) | (5 << 6) | (1 << 1)));
    CHECK_EQ(S.at(4, 4), paletteToRgb((uint16_t)((1 << 11) | (4 << 6) | (1 << 1))));
    // テキストとの組 ($1D) でも同じ
    S.vc(2, 0x1d2f);
    S.palette(2, (uint16_t)(31 << 6));
    S.palette(256 + 5, (uint16_t)(31 << 6));
    CHECK_EQ(S.at(4, 4), paletteToRgb((uint16_t)(31 << 6)));
    S.palette(256 + 5, (uint16_t)((3 << 6) | 1));
    S.palette(2, (uint16_t)(5 << 6));
    CHECK_EQ(S.at(4, 4), paletteToRgb((uint16_t)((4 << 6) | 1)));
    S.palette(2, c2);
    S.palette(7, c7);
    S.palette(256 + 5, t5);
    S.vc(2, 0x1d2f);
    CHECK_EQ(S.at(4, 4), paletteToRgb(mix(c2, t5)));
    CHECK_EQ(S.at(24, 4), paletteToRgb(mix(c2, t0)));
    S.vc(2, 0x1c2f);
    CHECK_EQ(S.at(4, 4), paletteToRgb(c2)); // 最下位ビットを 0 にした色で出る
    // 最下位ビットが 1 のままの色 (色コード 3) は使われない
    S.palette(3, 0xffff);
    CHECK_EQ(S.at(4, 4), paletteToRgb(c2));

    // テキスト、グラフィック、スプライトの順: グラフィックとスプライトが混ざり、その上にテキストが重なる
    S.vc(1, 0x21e4);
    S.vc(2, 0x1d6f);
    S.pcg8(4, 0, 0x30000000);
    S.sprite(0, 4, 4, 0x0401, 3);
    S.sprite(1, 24, 4, 0x0401, 3);
    const uint16_t s43 = Screen::tcolor(0x43);
    CHECK_EQ(S.at(4, 4), paletteToRgb(t5));
    CHECK_EQ(S.at(24, 4), paletteToRgb(mix(c2, s43)));
    CHECK_EQ(S.at(20, 4), paletteToRgb(mix(c2, t0)));
    // グラフィックのほうが優先順位が低い相手とは混ぜない
    S.palette(256, 0x0000);
    S.vc(1, 0x06e4); // スプライト、テキスト、グラフィック
    CHECK_EQ(S.at(4, 4), paletteToRgb(s43));
    CHECK_EQ(S.at(12, 4), paletteToRgb(t5));
    CHECK_EQ(S.at(20, 4), paletteToRgb(c2));

    // bit14: ほかのビットによらず、テキストパレット 0 の色と混ぜる
    S.palette(256, t0);
    S.vc(1, 0x24e4);
    S.vc(2, 0x402f);
    CHECK_EQ(S.at(4, 4), paletteToRgb(mix(c2, t0)));
    CHECK_EQ(S.at(8, 4), paletteToRgb(c2));
    S.vc(2, 0x5f2f);
    CHECK_EQ(S.at(4, 4), paletteToRgb(mix(c2, t0)));

    // bit10 が 0 なら (未定義)、bit12 があってもふつうの表示
    S.vc(2, 0x1b2f);
    CHECK_EQ(S.at(4, 4), paletteToRgb(0xffff));
    S.vc(2, 0x002f);
    CHECK_EQ(S.at(4, 4), paletteToRgb(0xffff));

    // セカンドページの色コードも最下位ビットを無視し、グラフィックパレットの奇数番を引く
    // (Inside X68000 p.209 図 30)。偶数番と奇数番を違う色にして、奇数番が使われることを確かめる
    S.init();
    S.vc(1, 0x24e4);
    S.vc(2, 0x1e2f);
    S.palette(2, c2);
    S.palette(6, 0x0000);
    S.palette(7, c7);
    S.palette(1, (uint16_t)((3 << 11) | (7 << 6) | (9 << 1)));
    S.gword(4, 4, 0x0063); // セカンドページが 6 (偶数)
    S.gword(8, 4, 0x0003); // セカンドページが 0
    CHECK_EQ(S.at(4, 4), paletteToRgb(mix(c2, c7)));
    CHECK_EQ(S.at(8, 4), paletteToRgb(mix(c2, (uint16_t)((3 << 11) | (7 << 6) | (9 << 1)))));

    // 256 色: ベースページは手前のページ、セカンドページは奥のページ
    S.init();
    S.crtc(20, 0x0115);
    S.vc(0, 1);
    S.vc(1, 0x24e4);
    S.vc(2, 0x1e2f);
    S.gword(4, 4, 0x4521);
    S.gword(8, 4, 0x4520);
    S.gword(12, 4, 0x4501);
    S.gword(16, 4, 0x4421); // セカンドページが 0x44 (偶数): 奇数番の 0x45 と混ぜる
    CHECK_EQ(S.at(4, 4), paletteToRgb(mix(Screen::gcolor(0x20), Screen::gcolor(0x45))));
    CHECK_EQ(S.at(16, 4), paletteToRgb(mix(Screen::gcolor(0x20), Screen::gcolor(0x45))));
    CHECK_EQ(S.at(8, 4), S.G(0x20));
    CHECK_EQ(S.at(12, 4), S.G(0x45));

    // 65536 色: ワードの最下位ビットが領域。セカンドページはない
    S.init();
    S.crtc(20, 0x0315);
    S.vc(0, 3);
    S.vc(1, 0x24e4);
    S.vc(2, 0x1f2f);
    for (int i = 0; i < 256; i++) S.palette(i, (uint16_t)(((i & ~1) << 8) | (i | 1))); // ワードがそのまま色になる
    S.gword(4, 4, 0x1235);
    S.gword(8, 4, 0x1234);
    S.tdot(4, 4, 5);
    CHECK_EQ(S.at(4, 4), paletteToRgb(mix(0x1234, Screen::tcolor(5))));
    CHECK_EQ(S.at(8, 4), paletteToRgb(0x1234));
    S.vc(2, 0x1e2f);
    CHECK_EQ(S.at(4, 4), paletteToRgb(0x1234));
}

void testSpecialPriority() {
    S.init();
    S.vc(1, 0x06e4); // スプライト、テキスト、グラフィック
    S.vc(2, 0x146f);
    S.pcg8(4, 0, 0x30000000);
    S.sprite(0, 4, 4, 0x0401, 3);
    S.sprite(1, 8, 4, 0x0401, 3);
    S.sprite(2, 20, 4, 0x0401, 3);
    S.tdot(12, 4, 5);
    S.tdot(16, 4, 5);
    S.gword(4, 4, 0x0073);
    S.gword(8, 4, 0x0072);
    S.gword(12, 4, 0x0073);
    S.gword(16, 4, 0x0072);
    S.gword(20, 4, 0x0071);
    S.gword(24, 4, 0x0073);
    // ベースページの最下位ビットが 1 のドットだけ、テキストやスプライトより手前に出る (3.5.2 節)
    CHECK_EQ(S.at(4, 4), S.G(2));
    CHECK_EQ(S.at(8, 4), S.T(0x43));
    CHECK_EQ(S.at(12, 4), S.G(2));
    CHECK_EQ(S.at(16, 4), S.T(5));
    CHECK_EQ(S.at(20, 4), S.T(0x43)); // ベースページにドットがない。セカンドページは対象でない
    CHECK_EQ(S.at(24, 4), S.G(2));
    // ベースページは GD0。ブロックを入れ替えれば、そのブロックが対象になる
    S.vc(1, 0x06e1); // GD0 = ブロック 1、GD1 = ブロック 0
    CHECK_EQ(S.at(4, 4), S.G(6));
    CHECK_EQ(S.at(8, 4), S.G(6));
    // ベースページを OFF にすれば起きない
    S.vc(1, 0x06e4);
    S.vc(2, 0x146e);
    CHECK_EQ(S.at(4, 4), S.T(0x43));
    // bit14 が 1 だと、テキストパレット 0 との半透明になり、特殊プライオリティにはならない
    S.vc(2, 0x546f);
    CHECK_EQ(S.at(4, 4), S.T(0x43));
    // 256 色
    S.crtc(20, 0x0115);
    S.vc(0, 1);
    S.vc(2, 0x146f);
    S.gword(4, 4, 0x4521);
    S.gword(8, 4, 0x4520);
    CHECK_EQ(S.at(4, 4), S.G(0x20));
    CHECK_EQ(S.at(8, 4), S.T(0x43));
}

// ---- Machine を通したラスタ割り込み ----
//
// CRTC の R09 のラスタで MFP の GPIP6 (CIRQ) が 0 になり、割り込み (MFP の 14 番目の要因。ベクタは VR + 14) が入る
// (Inside X68000 の CRTC の章と MFP の章)。割り込みの処理でパレットを書き換えると、画面はそのラスタから下だけ
// 色が変わる。ROM は使わず、試験用の IPL ROM を組み立てて動かす。

// high が 0 でなければ、タイミングのレジスタ (R00、R02-R04、R06、R07、R09) に資料の幅の外のビットを立てて書く。
// Machine のラスタ、表示期間、ラスタ割り込みの判定はマスクした値を使うので、結果は同じになる (#178)
void testRasterInterrupt(bool high = false) {
    printf("raster interrupt%s\n", high ? " (high bits)" : "");
    static x68k::Machine m;
    static uint8_t rom[x68k::Machine::IPL_SIZE]; // 試験用の IPL ROM
    static uint32_t px[Renderer::MAX_WIDTH * Renderer::MAX_HEIGHT];
    for (uint8_t& b : rom) b = 0; // 組み立てない所は 0
    auto put = [&](uint32_t o, std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) {
            rom[o++] = (uint8_t)(w >> 8);
            rom[o++] = (uint8_t)w;
        }
    };
    const uint16_t kBefore = 0x07c1, kAfter = 0xf801; // テキストパレット 0 の色 (割り込みの前と後)
    put(0x10000, {0x0000, 0x2000, 0x00fe, 0x0100}); // $FF0000: SSP = $2000、PC = $FE0100
    put(0x0100, {
                    0x21fc, 0x00fe, 0x0e00, 0x0138,         // move.l #$fe0e00,$138.w  (ベクタ $4E)
                    0x13fc, 0x0040, 0x00e8, 0x8017,         // move.b #$40,$e88017     (VR: ベクタは $40 から)
                    0x13fc, 0x0000, 0x00e8, 0x8003,         // move.b #$00,$e88003     (AER: GPIP6 は立ち下がり)
                    0x13fc, 0x0040, 0x00e8, 0x8007,         // move.b #$40,$e88007     (IERA: GPIP6 を許可)
                    0x13fc, 0x0040, 0x00e8, 0x8013,         // move.b #$40,$e88013     (IMRA)
                    0x7a00,                                 // moveq #0,d5
                    0x46fc, 0x2000,                         // move.w #$2000,sr
                    0x60fe,                                 // bra.s *
                });
    put(0x0e00, {
                    0x33fc, kAfter, 0x00e8, 0x2200, // move.w #kAfter,$e82200  (テキストパレット 0)
                    0x5285,                         // addq.l #1,d5
                    0x4e73,                         // rte
                });
    CHECK_EQ(m.setIplRom(rom, sizeof rom), true);
    m.powerOn();

    // 512x512 (31kHz) の画面。画面はどれも透明なので、全面がテキストパレット 0 の色になる
    Video& v = m.video();
    auto crtc = [&](int n, uint16_t val) {
        v.crtcWrite((uint32_t)n * 2, (uint8_t)(val >> 8));
        v.crtcWrite((uint32_t)n * 2 + 1, (uint8_t)val);
    };
    const int kLine = 100; // 画面の上から数えた、割り込みを入れるライン
    const uint16_t h8 = high ? 0xff00 : 0, h10 = high ? 0xfc00 : 0; // bit7-0 と bit9-0 のレジスタの幅の外
    crtc(0, h8 | 0x5b);
    crtc(1, 0x09);
    crtc(2, h8 | 0x11);
    crtc(3, h8 | 0x51);
    crtc(4, h10 | 0x237);
    crtc(5, 0x05);
    crtc(6, h10 | 0x28);
    crtc(7, h10 | 0x228);
    crtc(8, 0x1b);
    crtc(9, (uint16_t)(h10 | (0x28 + kLine)));
    crtc(20, 0x0015);
    auto word = [&](uint32_t offset, uint16_t val) {
        v.paletteWrite(offset, (uint8_t)(val >> 8));
        v.paletteWrite(offset + 1, (uint8_t)val);
    };
    word(0x200, kBefore); // テキストパレット 0
    word(0x500, 0x06e4);  // ビデオコントローラ R1
    word(0x600, 0x0020);  // R2: テキストだけを表示

    Renderer r(v);
    r.setTarget(px, Renderer::MAX_WIDTH, Renderer::MAX_WIDTH, Renderer::MAX_HEIGHT);
    m.setLineListener(&r);
    // 1 フレーム目: 割り込みは 1 回。割り込みのラインより上は前の色、下は後の色
    for (size_t i = 0; i < sizeof px / sizeof px[0]; i++) px[i] = kUntouched;
    while (px[511 * Renderer::MAX_WIDTH] == kUntouched && m.cycles() < 2000000) m.run(1000);
    m68k::Registers reg;
    m.getRegisters(reg);
    CHECK_EQ(reg.d[5], 1);
    int first = -1; // 後の色になった最初のライン
    for (int y = 0; y < 512; y++) {
        uint32_t c = px[y * Renderer::MAX_WIDTH + 256];
        if (first < 0 && c == paletteToRgb(kAfter)) first = y;
        CHECK_EQ(c, paletteToRgb(first < 0 ? kBefore : kAfter));
        CHECK_EQ(px[y * Renderer::MAX_WIDTH], c);
        CHECK_EQ(px[y * Renderer::MAX_WIDTH + 511], c);
    }
    // 割り込みはラスタの始まりに入り、そのラスタを描く前に処理が終わるので、色が変わるのはそのラインから
    CHECK_EQ(first, kLine);
    // 次のフレームでも 1 回ずつ入る (最初の runFrame() は、今のフレームの残りを次の V-DISP の始まりまで進めるだけ)
    uint32_t count = reg.d[5];
    const uint32_t frames = r.frameCount();
    m.runFrame();
    m.runFrame();
    m.runFrame();
    m.getRegisters(reg);
    CHECK_EQ(reg.d[5], count + 2);
    // 垂直表示期間の終わり (R07) ごとにフレームが終わる
    CHECK_EQ(r.frameCount(), frames + 3);
    // MFP の GPIP4 (V-DISP) は R06 から R07 の手前までのラスタで 1、ほかで 0
    auto vdispAt = [&](int line) {
        for (int i = 0; i < 2000000 && m.rasterLine() != line; i++) m.run(16);
        CHECK_EQ(m.rasterLine(), line);
        return m.mfp().gpip(x68k::Mfp::GPIP_VDISP);
    };
    CHECK_EQ(vdispAt(0x27), false);
    CHECK_EQ(vdispAt(0x28), true);
    CHECK_EQ(vdispAt(0x227), true);
    CHECK_EQ(vdispAt(0x228), false);
    CHECK_EQ(vdispAt(0x237), false);
    m.setLineListener(nullptr);
}

// パレットとビデオコントローラの窓 ($E82000-$E827FF) の外への書き込みは、元の Video でも写しでも何も変えない
// (記録のオフセットは 21 ビットなので、窓の外を記録すると写しでは窓の中への書き込みになってしまう)
void testJournalPaletteOutside() {
    printf("palette writes outside the window are not replayed\n");
    static Screen source, shadow;
    static uint32_t ring[1 << 8];
    x68k::VideoJournal journal;
    journal.init(ring, 8);
    x68k::VideoReplayer replayer(journal, shadow.v, shadow.r);
    source.init();
    shadow.init();
    source.v.setJournal(&journal);
    source.v.paletteWrite(0, 0x12);
    source.v.paletteWrite(1, 0x34);
    for (uint32_t off : {0x200000u, 0x200001u, 0x3fffffu, 0x2000u}) source.v.paletteWrite(off, 0x5a);
    journal.publish();
    while (replayer.step()) {
    }
    for (Screen* s : {&source, &shadow}) {
        CHECK_EQ(s->v.paletteRead(0), 0x12);
        CHECK_EQ(s->v.paletteRead(1), 0x34);
    }
    source.v.setJournal(nullptr);
}

// ---- 記録を通した合成 (VideoJournal / VideoReplayer) ----
//
// Video への書き込みとラスタを記録し、写しの Video に当ててから描いても、直接描いたときと同じ画面になる。
// フレームの途中でビデオコントローラの R2 ($E82600) を書き換え、半透明と特殊プライオリティに切り替える。

void testJournalReplay() {
    static Screen direct;   // 書き込みのたびに、その場で描く
    static Screen source;   // 書き込みを記録するだけ
    static Screen shadow;   // 記録を当てる写し。描くのはこれ
    static uint32_t ring[1 << 12];
    x68k::VideoJournal journal;
    journal.init(ring, 12);
    x68k::VideoReplayer replayer(journal, shadow.v, shadow.r);
    auto pump = [&]() { // たまった記録をすべて写しに当てる (リングが小さいので、こまめに呼ぶ)
        journal.publish();
        while (replayer.step()) {
        }
    };
    shadow.r.setTarget(shadow.px, Renderer::MAX_WIDTH, Renderer::MAX_WIDTH, Renderer::MAX_HEIGHT);
    source.v.setJournal(&journal);

    const int kHalf = 200, kSpecial = 350; // R2 を書き換えるライン
    auto setup = [&](Screen& s) {
        s.init();
        pump();
        s.vc(1, 0x24e4); // 手前からグラフィック、テキスト、スプライト
        s.vc(2, 0x002f); // テキストとグラフィック 4 ページ
        // 縦の縞: グラフィックは 4 ページに違う色コード (最下位ビットが 0 と 1 の両方)、テキストは一部に重ねる
        for (int y = 0; y < 512; y++) {
            for (int x = 0; x < 64; x++) {
                s.gword(x, y, (uint16_t)((x & 15) | (((x >> 1) & 15) << 4) | (((x >> 2) & 15) << 8) | ((x >> 3) << 12)));
            }
            for (int x = 8; x < 80; x++) s.tdot(x, y, 1 + (x >> 3));
            pump();
        }
    };
    setup(direct);
    setup(source);
    for (size_t i = 0; i < sizeof direct.px / sizeof direct.px[0]; i++) direct.px[i] = shadow.px[i] = kUntouched;
    for (int y = 0; y < 512; y++) {
        if (y == kHalf) {
            direct.vc(2, 0x1d2f); // テキストとの半透明
            source.vc(2, 0x1d2f);
        }
        if (y == kSpecial) {
            direct.vc(2, 0x142f); // 特殊プライオリティ
            source.vc(2, 0x142f);
        }
        direct.r.onRaster(y);
        journal.push(x68k::VideoJournal::make(x68k::VideoJournal::OP_RASTER, (uint32_t)y, 0));
        pump();
    }
    journal.push(x68k::VideoJournal::make(x68k::VideoJournal::OP_CONTROL, x68k::VideoJournal::CTL_FRAME_END, 0));
    pump();
    source.v.setJournal(nullptr);
    CHECK_EQ(replayer.rasters(), 512);
    CHECK_EQ(replayer.frames(), 1);
    CHECK_EQ(shadow.r.frameCount(), 1);

    int different = 0;
    for (int y = 0; y < 512; y++) {
        different += memcmp(direct.px + y * Renderer::MAX_WIDTH, shadow.px + y * Renderer::MAX_WIDTH, 512 * 4) != 0;
    }
    CHECK_EQ(different, 0);
    // VRAM の中身はどのラインも同じなので、ラインの違いは R2 の違いだけ。書き換えたラインから画面が変わる
    auto sameLines = [&](int a, int b) {
        return memcmp(shadow.px + a * Renderer::MAX_WIDTH, shadow.px + b * Renderer::MAX_WIDTH, 512 * 4) == 0;
    };
    CHECK_EQ(sameLines(0, kHalf - 1), 1);
    CHECK_EQ(sameLines(kHalf - 1, kHalf), 0);
    CHECK_EQ(sameLines(kHalf, kSpecial - 1), 1);
    CHECK_EQ(sameLines(kSpecial - 1, kSpecial), 0);
    CHECK_EQ(sameLines(kSpecial, 511), 1);
    CHECK_EQ(sameLines(0, kSpecial), 0);
}

// Compare the canonical pixels after GPU capture against the established CPU
// compositor, including opaque palette zero and black nonzero color codes.
void testIndexedCapture() {
    pix68k_retro::PalettedFrame frame;
    frame.skipSoftware = true;
    unsigned captured = 0, fallback = 0;
    for (int mode : {0, 1, 3}) for (int zero : {0, 1}) {
        S.init();
        S.crtc(20, (uint16_t)((mode << 8) | 0x15));
        S.vc(0, (uint16_t)mode);
        S.palette(256, zero ? Screen::tcolor(0) : 0);
        S.palette(256 + 5, 0); // A nonzero code may map to transparent black.
        S.pcg8(4, 0, 0x33550000);
        S.sprite(0, 4, 4, 0x0401, 3);
        for (int x = 0; x < 32; x++) {
            S.tdot(x, 4, x % 4 == 0 ? 5 : x % 3);
            S.gword(x, 4, (uint16_t)(x % 7));
        }
        for (int ranks = 0; ranks < 64; ranks++) for (int enable : {0, 0x20, 0x40, 0x60, 0x6f, 0x406f, 0x146f}) {
            S.vc(1, (uint16_t)((ranks << 8) | 0xe4));
            S.vc(2, (uint16_t)enable);
            S.r.setIndexedLineSink(nullptr);
            S.r.renderLine(4);
            uint32_t expected[512];
            memcpy(expected, S.px + 4 * Renderer::MAX_WIDTH, sizeof expected);
            std::fill(S.px + 4 * Renderer::MAX_WIDTH, S.px + 4 * Renderer::MAX_WIDTH + 512, kUntouched);
            S.r.setIndexedLineSink(&frame);
            S.r.renderLine(4);
            if (frame.supported[4]) {
                captured++;
                CHECK_EQ(S.px[4 * Renderer::MAX_WIDTH], kUntouched);
            } else fallback++;
            frame.resolve(S.px, 512, 5);
            CHECK_EQ(memcmp(expected, S.px + 4 * Renderer::MAX_WIDTH, sizeof expected), 0);
        }
        S.r.setIndexedLineSink(nullptr);
    }
    CHECK_EQ(captured > 0, true);
    CHECK_EQ(fallback > 0, true);
    // Preserve previously drawn columns when the display becomes narrower.
    uint8_t codes[16] = {}; uint32_t colors[256] = {};
    colors[1] = 0x123456; codes[12] = 1;
    frame.software = S.px;
    frame.above(7, 16, codes, nullptr, colors, nullptr);
    S.px[7 * Renderer::MAX_WIDTH + 12] = kUntouched;
    frame.beginLine(7, 8);
    CHECK_EQ(S.px[7 * Renderer::MAX_WIDTH + 12], 0x123456);
    CHECK_EQ(frame.supported[7], 0);
    // Captured palettes are snapshots, independent of later register writes.
    frame.above(8, 16, codes, nullptr, colors, nullptr);
    colors[1] = 0;
    frame.resolve(S.px, Renderer::MAX_WIDTH, Renderer::MAX_HEIGHT);
    CHECK_EQ(S.px[8 * Renderer::MAX_WIDTH + 12], 0x123456);
    colors[1] = 0x123456;
    frame.above(20, 16, codes, nullptr, colors, nullptr);
    const auto serialA = frame.paletteSerial[20];
    frame.above(21, 16, codes, nullptr, colors, nullptr);
    CHECK_EQ(frame.paletteSerial[21], serialA);
    colors[1] = 0x654321;
    frame.above(22, 16, codes, nullptr, colors, nullptr);
    CHECK_EQ(frame.paletteSerial[22] != serialA, true);
    colors[1] = 0x123456;
    frame.above(23, 16, codes, nullptr, colors, nullptr);
    CHECK_EQ(frame.paletteSerial[23] != serialA, true);
    frame.resolve(S.px, Renderer::MAX_WIDTH, Renderer::MAX_HEIGHT);
    CHECK_EQ(S.px[20 * Renderer::MAX_WIDTH + 12], 0x123456);
    CHECK_EQ(S.px[22 * Renderer::MAX_WIDTH + 12], 0x654321);
    frame.invalidate();
    for (auto line : frame.supported) CHECK_EQ(line, 0);
}

} // namespace

int main() {
    testIndexedCapture();
    testPaletteColor();
    testTextVram();
    testGraphicVram();
    testFastClear();
    testGeometryAndRasters();
    testText();
    testGraphic16Colors();
    testGraphic256Colors();
    testGraphic65536();
    testGraphic1024Screen();
    testSprite();
    testSpriteLimit();
    testSpriteRowsAfterCopy();
    testSpriteRowBounds();
    testBg();
    testSpritePosition();
    testLayerPriority();
    testTransparency();
    testHalfTransparency();
    testSpecialPriority();
    testRasterInterrupt();
    testRasterInterrupt(true);
    testJournalReplay();
    testJournalPaletteOutside();
    return finish();
}
