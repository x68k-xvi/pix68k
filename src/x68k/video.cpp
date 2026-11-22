#include "x68k/video.h"

#include <string.h>

namespace x68k {

void Video::reset() {
    if (journal_) journal_->push(VideoJournal::make(VideoJournal::OP_CONTROL, VideoJournal::CTL_RESET, 0));
    // 電源投入時の CRTC は不定。IPL ROM が設定するまでの間も表示タイミングが回るよう、
    // 768x512 (31kHz) の値を入れておく
    static const uint16_t kDefault[24] = {
        0x0089, 0x000e, 0x001c, 0x007c, 0x0237, 0x0005, 0x0028, 0x0228,
        0x001b, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000, 0x0016, 0x0000, 0x0000, 0x0000,
    };
    for (int i = 0; i < 24; i++) crtc_[i] = kDefault[i];
    operation_ = 0;
    fastClearRequested_ = false;
    fastClearFields_ = 0;
    memset(tvram_, 0, sizeof tvram_);
    memset(gvram_, 0, sizeof gvram_);
    memset(palette_, 0, sizeof palette_);
    memset(sprite_, 0, sizeof sprite_);
    rebuildPaletteTables(); // パレットが全部 0 のときの表にする
    crtcWrites_++;
    spriteRowsWrites_++;
}

// パレット ($E82000-$E823FF) とビデオコントローラのレジスタへの書き込み。
// 書いた語は colors_.word / colors_.xrgb (0..255 がグラフィック、256..511 がテキスト / スプライト) にも入れる。
// 65536 色モードは、グラフィックパレットの 512 バイトを色コードの下位バイト用と上位バイト用の 2 組の表として使う
// (Data Crystal の X68k:IOMAP のグラフィックパレットの節):
//   色の下位バイト CL = $E82000 + PL x 2 - (PL & 1) のバイト (PL = 画素の下位バイト)。偶数番のワードの 2 バイト
//   色の上位バイト CH = $E82002 + PH x 2 - (PH & 1) のバイト (PH = 画素の上位バイト)。奇数番のワードの 2 バイト
void Video::paletteWrite(uint32_t offset, uint8_t value) {
    // 窓の外は無視する。記録より先に調べる (記録のオフセットは下位 21 ビットしか持たないので、
    // 窓の外の書き込みを記録すると、再生する側では窓の中への書き込みになってしまう)
    const bool inside = offset < PALETTE_SIZE;
    if (!inside) return;
    if (journal_) journal_->push(VideoJournal::make(VideoJournal::OP_PALETTE, offset, value));
    const uint32_t at = paletteSlot(offset);
    palette_[at] = value;
    // パレットの部分 (語 0..511) なら、その語から作る表も直す。ビデオコントローラのレジスタは表に入らない
    if (at < 0x400u) updatePaletteWord(at / 2u);
}

uint8_t Video::paletteRead(uint32_t offset) const {
    if (offset >= PALETTE_SIZE) return 0;
    return palette_[paletteSlot(offset)];
}

void Video::updatePaletteWord(uint32_t w) {
    const uint16_t c = (uint16_t)(palette_[w * 2] << 8 | palette_[w * 2 + 1]);
    colors_.word[w] = c;
    colors_.xrgb[w] = paletteToRgb(c);
    if (w >= 256) return; // テキストとスプライトのパレットは 65536 色の表に入らない
    // 上の式で、L = 2k と 2k + 1 のバイトは語 2k の上位と下位 ($E82000 + 4k、+ 4k + 1)、
    // H = 2k と 2k + 1 のバイトは語 2k + 1 の上位と下位 ($E82002 + 4k、+ 4k + 3)
    const uint32_t k2 = w & ~1u;
    if (w & 1) {
        colors_.high[k2] = (uint16_t)(c & 0xff00);
        colors_.high[k2 + 1] = (uint16_t)(c << 8);
    } else {
        colors_.low[k2] = (uint16_t)(c >> 8);
        colors_.low[k2 + 1] = (uint16_t)(c & 0xff);
    }
}

void Video::rebuildPaletteTables() {
    for (uint32_t w = 0; w < 512; w++) updatePaletteWord(w);
    colors_.word[512] = 0;
    colors_.xrgb[512] = 0;
}

// 表示の大きさは、CRTC の表示期間のレジスタから求める (Data Crystal の X68k:IOMAP の CRTC R02、R03、R06、R07):
//   R02 = 水平表示の始まり、R03 = 水平表示の終わり。単位は 8 ドット (R00 の説明の「データ表示期間 x 8」と同じ単位)
//   R06 = 垂直表示の始まり、R07 = 垂直表示の終わり。単位はラスタ (水平同期 1 回分)
// 1 ラスタに出るデータのライン数は、R20 の HF (bit4) と VD (bit3-2) で決まる:
//   31kHz (HF = 1): VD = %00 は 256 ラインを 2 回ずつ出す (2 ラスタで 1 ライン)、%01 は 512 ライン (1 ラスタで 1 ライン)、
//                   %10 / %11 は 1024 ラインのインタレース (1 ラスタで 2 ライン。2 フィールドで全ラインを出す)
//   15kHz (HF = 0): VD = %00 は 256 ライン (1 ラスタで 1 ライン)、それ以外は 512 ラインのインタレース (1 ラスタで 2 ライン)
// 終わりが始まり以下なら 0 にする。
// 資料では R02 / R03 は 8 ビット、R06 / R07 は 10 ビットのレジスタなので、その幅の外のビットは見ない
// (horizontalDisplayStart() などのアクセサでマスクする。video.h の表示タイミングの説明)
void Video::screenSize(int& w, int& h) const {
    const int columns = horizontalDisplayEnd() - horizontalDisplayStart(); // 8 ドット単位
    const int rasters = verticalDisplayEnd() - verticalDisplayStart();
    w = columns > 0 ? columns * 8 : 0;
    if (rasters <= 0) {
        h = 0;
        return;
    }
    const unsigned vd = (crtc_[20] >> 2) & 3u;
    const bool khz31 = (crtc_[20] & 0x10) != 0;
    if (interlaced()) h = rasters * 2;          // 1 ラスタで 2 ライン
    else if (khz31 && vd == 0) h = rasters / 2; // 2 ラスタで 1 ライン
    else h = rasters;                           // 1 ラスタで 1 ライン
}

// CRTC の窓は 0x800 バイトごとに同じものが見え、レジスタは $E80000-$E803FF の中で 0x40 バイトごとに繰り返す
uint8_t Video::crtcRead(uint32_t offset) {
    offset &= 0x7ff;
    if (offset < 0x400) {
        offset &= 0x3f;
        // 読めるのは R20、R21 だけ。ほかは 0 が読める
        int n = (int)(offset >> 1);
        if (n != 20 && n != 21) return 0;
        return (offset & 1) ? (uint8_t)crtc_[n] : (uint8_t)(crtc_[n] >> 8);
    }
    // 動作ポート。bit1 (高速クリア) は、要求してから終わるまで 1 が読める
    if (offset == 0x481) return (uint8_t)(operation_ | (fastClearBusy() ? 0x02 : 0x00));
    return 0;
}

void Video::crtcWrite(uint32_t offset, uint8_t v) {
    if (journal_) journal_->push(VideoJournal::make(VideoJournal::OP_CRTC, offset, v));
    offset &= 0x7ff;
    if (offset < 0x400) {
        offset &= 0x3f;
        if (offset >= 0x30) return;
        int n = (int)(offset >> 1);
        crtcWrites_++;
        if (offset & 1) crtc_[n] = (uint16_t)((crtc_[n] & 0xff00) | v);
        else crtc_[n] = (uint16_t)((crtc_[n] & 0x00ff) | (v << 8));
        return;
    }
    if (offset == 0x481) {
        // bit3: テキストのラスタコピー。立っている間、水平帰線期間ごとに R22 の転送をする (horizontalBlank)
        // bit1: グラフィックの高速クリア。1 で始め、0 で止める。終わると自動で 0 に戻る (X68k:IOMAP の動作ポート)。
        //       消すのは次の垂直表示期間から (beginVerticalDisplay)
        // bit0: 画像入力 (未実装)
        if (v & 0x02) {
            if (!fastClearBusy()) fastClearRequested_ = true;
        } else {
            fastClearRequested_ = false;
            fastClearFields_ = 0;
        }
        operation_ = (uint8_t)(v & ~0x02);
    }
}

void Video::tvramWrite(uint32_t offset, uint8_t v) {
    if (journal_) journal_->push(VideoJournal::make(VideoJournal::OP_TVRAM, offset, v));
    offset &= TVRAM_SIZE - 1;
    uint16_t r21 = crtc_[21];
    // R21 bit9: R23 のビットマスクを使う (1 のビットは書き換えない)
    uint8_t mask = 0;
    if (r21 & 0x0200) mask = (offset & 1) ? (uint8_t)crtc_[23] : (uint8_t)(crtc_[23] >> 8);
    if (r21 & 0x0100) {
        // R21 bit8: 同時アクセス。bit4..7 で選んだプレーンに同じ値を書く
        uint32_t o = offset & 0x1ffff;
        for (int p = 0; p < 4; p++) {
            if (!(r21 & (0x10 << p))) continue;
            uint8_t& d = tvram_[p * 0x20000 + o];
            d = (uint8_t)((d & mask) | (v & ~mask));
        }
    } else {
        uint8_t& d = tvram_[offset];
        d = (uint8_t)((d & mask) | (v & ~mask));
    }
}

void Video::horizontalBlank() {
    if (journal_ && (operation_ & 0x08)) journal_->push(VideoJournal::make(VideoJournal::OP_HBLANK, 0, 0));
    if (operation_ & 0x08) rasterCopy();
}

// 垂直表示期間の始まり。高速クリアを進める。
// CRTC は画面を表示するための走査に合わせてグラフィック VRAM を消していくので、要求のあとの最初の垂直表示期間の
// 始まりから 1 画面分の走査 (インタレースは 2 フィールド) がかかる、として作った。動作ポートの bit1 は、その走査が
// 終わった次の垂直表示期間の始まりで 0 に戻す (X68k:IOMAP: 「高速クリア動作が終了すると自動的に 0 になる」)。
// 途中の様子 (上のラスタから順に消えていく) は作らず、始まりで全部を消す。消す範囲は fastClear() の説明
void Video::beginVerticalDisplay() {
    if (journal_) journal_->push(VideoJournal::make(VideoJournal::OP_CONTROL, VideoJournal::CTL_VDISP, 0));
    if (fastClearFields_ > 0) {
        fastClearFields_--;
    } else if (fastClearRequested_) {
        fastClearRequested_ = false;
        fastClear();
        fastClearFields_ = interlaced() ? 2 : 1;
    }
}

// インタレースの画面か (R20 の HF と VD。31kHz の 1024 ライン、15kHz の 512 ライン。screenSize() と同じ分け方)
bool Video::interlaced() const {
    const unsigned vd = (crtc_[20] >> 2) & 3u;
    return (crtc_[20] & 0x10) ? vd >= 2 : vd != 0; // 31kHz は %10 / %11、15kHz は %00 のほか全部
}

uint32_t Video::dotClock(bool hrl) const {
    // R20 の bit4 が 1 なら 31kHz (69.552MHz の発振)、0 なら 15kHz (38.864MHz の発振)。
    // bit1..0 (水平解像度) で分周する: 31kHz は 256/512/768 ドットで 1/6、1/3、1/2
    // (システムポートの HRL が 1 なら 256/512 ドットは 1/8、1/4)、15kHz は 1/8、1/4、1/2
    int hres = crtc_[20] & 3;
    if (crtc_[20] & 0x10) {
        static const uint8_t kDiv[4] = {6, 3, 2, 2};
        static const uint8_t kDivHrl[4] = {8, 4, 2, 2};
        return 69552000u / (hrl ? kDivHrl[hres] : kDiv[hres]);
    }
    static const uint8_t kDiv[4] = {8, 4, 2, 2};
    return 38864000u / kDiv[hres];
}

void Video::rasterCopy() {
    // R22 の上位 8 ビットが転送元、下位 8 ビットが転送先のラスタ番号。1 ラスタ = 4 ライン = 512 バイト
    // R21 の bit0..3 で選んだプレーンをコピーする
    uint32_t src = (uint32_t)(crtc_[22] >> 8) * 512;
    uint32_t dst = (uint32_t)(crtc_[22] & 0xff) * 512;
    for (int p = 0; p < 4; p++) {
        if (!(crtc_[21] & (1 << p))) continue;
        memmove(tvram_ + p * 0x20000 + dst, tvram_ + p * 0x20000 + src, 512);
    }
}

// 高速クリア: R21 の bit3-0 (CP3-CP0) で選んだグラフィックのページを 0 にする。
// CP は、テキストのラスタコピーのプレーンの選択 (rasterCopy()) と同じビットで、1 のビットが選んだページ。
// X68k:IOMAP の R21 の説明は「%0 : 選択」と読めるが、同じビットを使うラスタコピーは、1 で選ぶとして Human68k の
// 画面のスクロールが正しく動いているので、高速クリアも 1 で選ぶとした。
// グラフィック VRAM の 1 ワードは 4 ビットずつ 4 ページ分なので、CPn はワードの bit 4n..4n+3 を消す。
// 256 色 (ページ 0 = bit0-7) や 65536 色でページ全体を消すには、ソフトウェアが対応する CP をすべて 1 にする。
//
// 消す範囲は、資料 (X68k:IOMAP) に書いていない。CRTC は表示のための走査に合わせて消す (beginVerticalDisplay() の
// 説明) ので、1 画面の走査で読む所だけが消える、として作った。細かい所は、前の版と同じ結果になるように決めた
// (前の版のソースは読まず、同じ設定で消えるワードを比べた。実機では確かめていない):
//   - 大きさは今の表示の大きさ (screenSize())。実画面より大きい分は、折り返して同じ所を読むので実画面の全体になる
//     (実画面の大きさで打ち切る)
//   - 位置は、選んだどのページも、ページ 0 のスクロール (X は R12、Y は R13) から。X の bit0 は見ない
//   - 実画面 1024x1024 (R20 の bit10) は CP を見ない。Y は R13 の下位 10 ビットで、走査のラインが上半分 (0-511) なら
//     ページ 0 と 1、下半分ならページ 2 と 3 の、同じ位置 (X は下位 9 ビット) を消す
void Video::fastClear() {
    int w = 0, h = 0;
    screenSize(w, h);
    if (w <= 0 || h <= 0) return;
    // X と Y は折り返すので、実画面の幅 (512) と高さ (512、1024x1024 なら 1024) を超えて回しても同じ所を消すだけ。そこで打ち切る
    const int lines = crtc_[20] & 0x0400 ? 1024 : 512;
    if (w > 512) w = 512;
    if (h > lines) h = lines;
    const uint32_t left = crtc_[12] & ~1u; // ページ 0 のスクロール X (bit0 は見ない)
    const uint32_t top = crtc_[13];
    if (crtc_[20] & 0x0400) {
        // 実画面 1024x1024: 上半分のラインはページ 0 と 1、下半分はページ 2 と 3 (同じ 512x512 の語の別のビット)
        for (int line = 0; line < h; line++) {
            const uint32_t y = (top + (uint32_t)line) & 0x3ffu;
            const uint16_t keep = y < 512 ? 0xff00 : 0x00ff;
            uint16_t* row = gvram_ + (y & 0x1ffu) * 512u;
            for (int dot = 0; dot < w; dot++) row[(left + (uint32_t)dot) & 0x1ffu] &= keep;
        }
        return;
    }
    // 512x512: CP3-CP0 の 1 のビットのページ (語の 4 ビットずつ) を消す
    uint16_t erase = 0;
    for (int page = 0; page < 4; page++)
        if (crtc_[21] & (1u << page)) erase |= (uint16_t)(0xfu << (4 * page));
    if (!erase) return;
    const uint16_t keep = (uint16_t)~erase;
    for (int line = 0; line < h; line++) {
        uint16_t* row = gvram_ + ((top + (uint32_t)line) & 0x1ffu) * 512u;
        for (int dot = 0; dot < w; dot++) row[(left + (uint32_t)dot) & 0x1ffu] &= keep;
    }
}

int Video::verticalTotal() const {
    int t = (crtc_[4] & 0x3ff) + 1;
    return t < 16 ? 16 : t;
}

void Video::horizontalDisplay(uint32_t& start, uint32_t& end) const {
    uint32_t total = (uint32_t)horizontalTotal() + 1;
    start = (uint32_t)horizontalDisplayStart() * 65536 / total;
    end = (uint32_t)horizontalDisplayEnd() * 65536 / total;
    if (end > 65536) end = 65536;
}

} // namespace x68k
