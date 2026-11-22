// 画面の合成。Video (VRAM、パレット、CRTC、ビデオコントローラ、スプライトコントローラのレジスタ) を読み、
// テキスト画面、グラフィック画面、スプライト + BG 画面を重ねて XRGB8888 の画素にする。
//
// 根拠にした資料は「Inside X68000」(桑野雅彦、ソフトバンク、1992 年) の画面制御の章。合成の決まりと、
// 資料に書かれていないために決めたことは render.cpp の先頭にまとめた。
//
// ベアメタルでも使うので、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

#include "x68k/machine.h"
#include "x68k/video.h"

namespace x68k {

// Optional backend for ordinary indexed-color layers. Returning true means
// the backend owns final color conversion for this line; other lines keep
// using the software renderer. Calls follow the original raster order.
class IndexedLineSink {
public:
    virtual ~IndexedLineSink() = default;
    virtual void beginLine(int y, int width) = 0;
    virtual bool above(int y, int width, const uint8_t* front, const uint8_t* graphic,
                       const uint32_t* textPalette, const uint32_t* graphicPalette) = 0;
};

class Renderer : public LineListener {
public:
    static constexpr int MAX_WIDTH = 1024;
    static constexpr int MAX_HEIGHT = 1024;

    explicit Renderer(const Video& source); // source の VRAM とレジスタを読んで描く

    // 描く先。dest は XRGB8888 の画素の並びで、行の頭は stride 画素ずつずれる。書いてよいのは
    // columns x rows 画素まで (columns が stride より大きくても、1 行に書くのは stride 画素まで)。
    // dest が nullptr なら描かない
    void setTarget(uint32_t* dest, int stride, int columns, int rows);
    void setIndexedLineSink(IndexedLineSink* sink) { indexedSink_ = sink; }

    // 今の CRTC の設定での画面の大きさ (MAX_WIDTH x MAX_HEIGHT まで)
    void geometry(int& width, int& height) const;
    // 最後に描いたときの画面の大きさ
    int width() const { return width_; }
    int height() const { return height_; }
    // 描き終えたフレームの数
    uint32_t frameCount() const { return frameEnds_; }

    // 画面のライン y (0 が一番上。ラスタの 2 度読みやインタレースを直したあとの、データのラインの番号) を描く
    void renderLine(int y);
    // 今の Video の中身で 1 画面を描く
    void renderFrame();

    // LineListener。raster は垂直表示期間の最初のラスタを 0 とした番号。そのラスタに当たるラインを描く
    // (256 ラインの 2 度読みでは偶数のラスタで 1 ライン、インタレースでは 1 ラスタで 2 ライン)
    void onRaster(int raster) override;
    void onFrameEnd() override { frameEnds_ += 1; } // LineListener。描き終えたフレームを数える

private:
    // バッファの前後の余白。スプライトや BG のパターンを、画面の端からはみ出したまま書けるようにする
    static constexpr int MARGIN = 32;
    static constexpr int LINE_SIZE = MAX_WIDTH + 2 * MARGIN;

    void syncGeometry();
    void buildColorSplitTables();
    bool buildSpriteLine(int y, int w);
    int buildGraphicLine(int y, int w, uint16_t r0, uint16_t r1, uint16_t r2, bool split);
    void composeSpecial(int w, uint32_t* out, uint16_t r2, int graphic, const uint8_t* front, const uint8_t* back,
                        int stacking);

    const Video& video_;
    // 描く先 (setTarget())。columns は 1 行に書いてよい画素数 (幅と pitch の小さいほう)、rows は行の数
    struct Surface {
        uint32_t* pixels = nullptr;
        int pitch = 0;
        int columns = 0;
        int rows = 0;
    } surface_;
    int width_ = 0;
    int height_ = 0;
    int linesPerTwoRasters_ = 2; // 2 ラスタで進むデータのライン数 (2 度読み 1、ふつう 2、インタレース 4)
    uint32_t frameEnds_ = 0; // onFrameEnd() の回数
    uint32_t crtcSeen_ = 0;
    bool geometryValid_ = false;
    bool hasSecondPage_ = false; // 半透明、特殊プライオリティ用: 今のラインのグラフィックにセカンドページがあるか
    // Sprite Y is 0..1023 with a -16 display offset. Each row holds a
    // 128-sprite candidate mask, rebuilt only when Y/priority changes.
    uint32_t spriteRows_[1040][4] = {};
    uint16_t spriteYSeen_[128] = {};
    uint8_t spriteEnabledSeen_[128] = {};
    uint32_t spriteRowsSeen_ = 0;
    bool spriteRowsValid_ = false;

    // 1 ライン分の作業用。どれも [MARGIN, MARGIN + 幅) を使う
    // (最初は 0 で埋めておく)
    uint8_t textLine_[LINE_SIZE] = {};     // テキストの色コード (0..15)
    uint8_t spriteLine_[LINE_SIZE] = {};   // スプライト + BG の色コード (0..255)
    uint8_t graphicLine_[LINE_SIZE] = {};  // グラフィックの色コード (ページを重ねたあと)
    uint8_t slotLine_[4][LINE_SIZE] = {};  // グラフィックの 4 ビットずつのデータ (優先順位の高い順)
    uint8_t restLine_[LINE_SIZE] = {};     // ベースページより奥のページを重ねたもの (半透明、特殊プライオリティ用)
    uint16_t wordLine_[LINE_SIZE] = {};    // 65536 色のデータ、または半透明・特殊プライオリティ用のベースページのデータ
    uint8_t mergeLine_[LINE_SIZE] = {};    // テキストとスプライトを重ねたもの
    uint8_t zeroLine_[LINE_SIZE] = {};     // すべて 0 (何もない画面の代わり)

    // 65536 色の、ワードの上位バイト / 下位バイトから XRGB の表。パレットが変わったら作り直す
    uint32_t wordHiXrgb_[256] = {};
    uint32_t wordLoXrgb_[256] = {};
    uint16_t wordHiSeen_[256];
    uint16_t wordLoSeen_[256];
    bool wordTablesValid_ = false;
    uint32_t colorHiXrgb_[256]; // 色 (16 ビット) の上位バイト / 下位バイトから XRGB の表 (半透明用)
    uint32_t colorLoXrgb_[256];
    // テキストだけのライン用: となり合う 2 ドットの色コードから 2 ドット分の画素を引く表。パレットが変わったら作り直す
    uint64_t textPairs_[256];
    uint32_t textPairsSeen_[16];
    bool textPairsValid_ = false;
    IndexedLineSink* indexedSink_ = nullptr;
};

} // namespace x68k
