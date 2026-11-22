// CRTC (表示タイミングのレジスタ、テキスト VRAM の同時アクセスとラスタコピー) と、
// 画面まわりのメモリ (テキスト VRAM、グラフィック VRAM、パレット、スプライト) の置き場所。
// 画面への描画 (合成) は render.h の Renderer が、ここにある VRAM とレジスタを読んで行う。
#pragma once

#include <stdint.h>

#include "x68k/video_journal.h"

namespace x68k {

class StateIO;

// パレットの 1 語 (16 ビット) を画面の 0x00RRGGBB に直す。
// 語の並びは上から G 5 ビット (15-11)、R 5 ビット (10-6)、B 5 ビット (5-1)、I 1 ビット (0)。
// I は輝度のビットで、3 色に共通の最下位ビットとして足される。各色は (5 ビット << 1) | I の 6 ビット
// (0〜63) の階調になる。グラフィック ($E82000-)、テキストとスプライト ($E82200-) のパレットで同じ形式。
// 6 ビットを 8 ビットへは、上位 2 ビットを下に繰り返して広げる (0 → 0x00、63 → 0xFF)。
// 根拠: パレットの番地は X68k:IOMAP のビデオコントローラの表。ビットの並びは chibiakumas の X68000 の解説と
// x68000-dev-guide (JC-000) の docs/graphics.md。graphics.md の I の足し方の段落は、MAME のソースを出どころに
// した第三者の資料 (MAME のソースそのものは読んでいない)。8 ビットへの広げ方は、graphics.md に MAME の変換関数の
// 名前 (pal6bit) が出ているだけで計算の中身は書かれていないので、ここで選んだもの。
// render.cpp は 65536 色のときに上位バイトと下位バイトを別々にこの関数で変換して OR する。そのため変換は
// 各出力ビットが入力の 1 ビットだけで決まる (ビットを繰り返す) 方式でないと、65536 色の描画が壊れる。
inline uint32_t paletteToRgb(uint16_t word) {
    // 3 色の 5 ビットの場所 (G は bit15-11、R は bit10-6、B は bit5-1)。I (bit0) は 3 色とも 6 ビット目の最下位になる
    const uint32_t lowest = word & 1u;
    uint32_t level[3];
    const unsigned shift[3] = {6, 11, 1}; // R、G、B の順
    for (int k = 0; k < 3; k++) {
        const uint32_t six = (((uint32_t)word >> shift[k]) & 0x1fu) * 2u + lowest; // 0..63
        level[k] = six * 4u + six / 16u; // 0..255。上位 2 ビットを下に繰り返す
    }
    return level[0] << 16 | level[1] << 8 | level[2];
}

class Video {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    void copyStateFrom(const Video& from); // x68k::copyVideoState()
    static constexpr uint32_t TVRAM_SIZE = 0x80000;  // 128KB x 4 プレーン
    static constexpr uint32_t GVRAM_SIZE = 0x80000;  // 512KB
    static constexpr uint32_t PALETTE_SIZE = 0x800;  // $E82000-$E827FF (パレットとビデオコントローラ)
    static constexpr uint32_t SPRITE_SIZE = 0x10000; // $EB0000-$EBFFFF

    void reset();

    // 状態を変える操作をすべて journal に記録する (画面の合成を別のコアで行うとき。video_journal.h)。nullptr でやめる
    void setJournal(VideoJournal* journal) { journal_ = journal; }

    // CRTC のレジスタ ($E80000-$E8002F、R00..R23) と動作ポート ($E80481)
    uint8_t crtcRead(uint32_t offset);
    void crtcWrite(uint32_t offset, uint8_t v);
    uint16_t reg(int n) const { return crtc_[n]; }
    // CRTC のレジスタに書いた回数。前に見た値と同じなら、CRTC から求めた値 (画面の大きさなど) を使い回してよい
    uint32_t crtcGeneration() const { return crtcWrites_; }
    // 表示する画面の幅と高さ (ドット)。高さは VRAM のライン数で数える。31kHz の 256 ライン (1 ラインを 2 ラスタで出す)
    // は 256、インタレースは 2 フィールドを合わせたライン数
    void screenSize(int& w, int& h) const;

    // テキスト VRAM ($E00000-$E7FFFF)
    uint8_t tvramRead(uint32_t offset) const { return tvram_[offset & (TVRAM_SIZE - 1)]; }
    void tvramWrite(uint32_t offset, uint8_t v);
    const uint8_t* tvram() const { return tvram_; }

    // グラフィック VRAM ($C00000-$DFFFFF、offset は $C00000 からの位置)。
    // 実体は 512x512 ワード。CPU からの見え方は CRTC R20 のメモリモード (色数と 1024 モード) で変わる (gvram.cpp)
    uint8_t gvramRead(uint32_t offset) const;
    void gvramWrite(uint32_t offset, uint8_t v);
    // 実体のワード (i = y * 512 + x)
    uint16_t gvramWord(uint32_t i) const { return gvram_[i & (GVRAM_SIZE / 2 - 1)]; }
    // 実体の先頭 (512x512 ワード。y * 512 + x の順)
    const uint16_t* gvramWords() const { return &gvram_[0]; }

    // $E82000-$E827FF の 0x800 バイトの読み書き (offset は $E82000 からのバイト数)。
    // $E82000-$E823FF はパレット (グラフィック 256 語、テキストとスプライト 256 語) で、そのまま置く。
    // $E82400-、$E82500-、$E82600- はビデオコントローラの R0、R1、R2 で、それぞれ 0x100 バイトの範囲で
    // 同じ 1 語が繰り返し見える (x68000-dev-guide の docs/graphics.md。この節は MAME のソースを出どころにした
    // 第三者の資料)。下の paletteSlot で範囲の先頭の 2 バイトにまとめる。$E82700-$E827FF は資料がなく、
    // main と同じ動作にするため、同じ規則で 0x700 の 2 バイトにまとめる。
    uint8_t paletteRead(uint32_t offset) const;
    void paletteWrite(uint32_t offset, uint8_t value);
    const uint8_t* palette() const { return palette_; }
    // ビデオコントローラのレジスタ R0 ($E82400)、R1 ($E82500)、R2 ($E82600) の 16 ビットの値。
    // R0 は実画面の大きさと色数、R1 は画面の優先順位、R2 は画面ごとの表示のオンオフと特殊プライオリティ / 半透明
    // (X68k:IOMAP のビデオコントローラの節)
    uint16_t videoControl(int n) const {
        const uint32_t at = 0x400u + (uint32_t)n * 0x100u;
        return (uint16_t)(palette_[at] << 8 | palette_[at + 1]);
    }
    // パレットの 16 ビットの値 (512 語) と、それを paletteToRgb() で変換した色。添字の 0..255 はグラフィック、
    // 256..511 はテキストとスプライト。添字 512 はどの番号にも当たらない色 0 (黒) で、パレットに書いても変わらない
    const uint16_t* paletteColors() const { return colors_.word; }
    const uint32_t* paletteXrgb() const { return colors_.xrgb; }
    // 65536 色モードで、画素の値 (16 ビット) を色に直す表 (グラフィックパレットに書くたびに作り直す)。
    // 画素の上位バイトを H、下位バイトを L として、色は color64kHigh()[H] | color64kLow()[L]。
    // color64kHigh() は色の上位バイトを bit15-8 に置いた値、color64kLow() は色の下位バイトだけ
    const uint16_t* color64kHigh() const { return colors_.high; }
    const uint16_t* color64kLow() const { return colors_.low; }

    uint8_t spriteRead(uint32_t offset) const { return sprite_[offset & (SPRITE_SIZE - 1)]; }
    void spriteWrite(uint32_t offset, uint8_t v) {
        if (journal_) journal_->push(VideoJournal::make(VideoJournal::OP_SPRITE, offset, v));
        const uint32_t at = offset & (SPRITE_SIZE - 1);
        // Only Y and priority affect which scanlines can contain a sprite.
        if (at < 0x400 && ((at & 7) == 2 || (at & 7) == 3 || (at & 7) == 7) && sprite_[at] != v)
            spriteRowsWrites_++;
        sprite_[at] = v;
    }
    const uint8_t* sprite() const { return sprite_; }
    uint32_t spriteRowsGeneration() const { return spriteRowsWrites_; }

    // ---- 表示タイミング ----
    // 1 ラスタの長さは「水平のドット数 (R00 + 1) x 8」/ ドットクロック。1 フレームは R04 + 1 ラスタ
    //
    // タイミングのレジスタは、資料 (Data Crystal の X68k:IOMAP) の幅でマスクしてから使う。幅の外のビットは見ない:
    //   R00 (水平の全体)、R02 / R03 (水平表示の始まりと終わり) は bit7-0
    //   R04 (垂直の全体)、R06 / R07 (垂直表示の始まりと終わり)、R09 (ラスタ割り込み) は bit9-0
    // これらのレジスタを使う所 (screenSize()、horizontalDisplay()、Machine のラスタと表示期間の判定、
    // Renderer のスプライトの位置) は、すべて下のアクセサを通す。reg() は書いた 16 ビットの値をそのまま返す。
    // R01、R05、R08 (同期パルスと水平の調整) はどこからも読まないので、アクセサを置いていない
    bool highResolution() const { return (crtc_[20] & 0x10) != 0; }
    int horizontalTotal() const { return crtc_[0] & 0xff; } // R00
    uint32_t horizontalTotalDots() const { return ((uint32_t)horizontalTotal() + 1) * 8; }
    // Hz。hrl はシステムポート $E8E007 の bit1 (HRL)
    uint32_t dotClock(bool hrl = false) const;
    // 水平帰線期間の始まりに呼ぶ (ラスタコピー)
    void horizontalBlank();
    // 垂直表示期間の始まりに呼ぶ (高速クリアを始める / 終える)
    void beginVerticalDisplay();
    // インタレースの画面か (31kHz の 1024 ライン、15kHz の 512 ライン)
    bool interlaced() const;
    // 高速クリアを要求してから終わるまで (動作ポートの bit1 に 1 が読める間)
    bool fastClearBusy() const { return fastClearRequested_ || fastClearFields_ > 0; }
    int verticalTotal() const;       // (R04 & 0x3ff) + 1。16 より小さければ 16
    // 表示期間 (R02 / R03 は 8 ドット単位、R06 / R07 はラスタ単位)
    int horizontalDisplayStart() const { return crtc_[2] & 0xff; }
    int horizontalDisplayEnd() const { return crtc_[3] & 0xff; }
    int verticalDisplayStart() const { return crtc_[6] & 0x3ff; }
    int verticalDisplayEnd() const { return crtc_[7] & 0x3ff; }
    int rasterInterruptLine() const { return crtc_[9] & 0x3ff; } // R09
    // 1 ラスタの中で水平表示期間が占める範囲 (0..1 を 65536 倍した値)
    void horizontalDisplay(uint32_t& start, uint32_t& end) const;

private:
    // $E82000-$E827FF のバイトの位置を palette_ の添字にする。パレット (0x000-0x3FF) はそのまま。
    // 0x400 から先は 0x100 バイトの範囲ごとに 1 語なので、範囲の中の位置の bit7-1 を捨てる
    static uint32_t paletteSlot(uint32_t offset) { return offset < 0x400u ? offset : (offset & ~0xfeu); }
    // 語 w (0..511) の colors_.word / colors_.xrgb と、グラフィックの語なら 65536 色の表を作り直す
    void updatePaletteWord(uint32_t w);
    // パレットから作る表を全部作り直す
    void rebuildPaletteTables();
    void rasterCopy();
    void fastClear();
    VideoJournal* journal_ = nullptr;
    uint16_t crtc_[24] = {};
    uint32_t crtcWrites_ = 0;
    uint32_t spriteRowsWrites_ = 0; // Derived renderer cache generation; not serialized.
    uint8_t operation_ = 0;
    bool fastClearRequested_ = false; // 高速クリアを要求された (次の垂直表示期間の始まりで消す)
    int fastClearFields_ = 0;         // 消し始めてから終わるまでの、残りの垂直表示期間の数
    uint8_t tvram_[TVRAM_SIZE];
    uint16_t gvram_[GVRAM_SIZE / 2];
    uint8_t palette_[PALETTE_SIZE];
    uint8_t sprite_[SPRITE_SIZE];
    // palette_ から作る表 (updatePaletteWord()、rebuildPaletteTables())。ステートには、この順に書く
    struct ColorTables {
        uint16_t word[513]; // 語の値。512 は色 0 (どの番号にも当たらない)
        uint32_t xrgb[513]; // word を paletteToRgb() で変換したもの
        uint16_t high[256]; // 65536 色の、画素の上位バイトから色の上位バイト
        uint16_t low[256];  // 65536 色の、画素の下位バイトから色の下位バイト
    } colors_;
    // ステートの読み書きで、色の表を書く (state.cpp)
    void colorTablesState(StateIO& s);
};

} // namespace x68k
