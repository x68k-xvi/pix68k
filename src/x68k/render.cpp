// 画面の合成 (render.h)。
//
// 根拠は「Inside X68000」(桑野雅彦、ソフトバンク、1992 年) の画面制御の章。以下の「図」「節」はこの本のもの。
//
// ■ グラフィック画面 (コラム「グラフィックページ間プライオリティ制御のからくり」、図 20、図 22)
//   グラフィック VRAM は 512x512 ドット x 4 ビットのブロックが 4 つ (VRAM #0..#3。Video のワードの bit 4n..4n+3)。
//   CRTC のスクロールレジスタは R12/R13 が #0、R14/R15 が #1、R16/R17 が #2、R18/R19 が #3 の表示開始位置。
//   ビデオコントローラ R1 の下位 8 ビット (GP0..GP3) は、優先順位の高い順に並んだ 4 つのデータ GD0..GD3 が
//   どのブロックから来るかを決める (GDn は VRAM #GPn)。R2 の bit0..3 は GD0..GD3 の表示 ON/OFF
//   (ページではなく優先順位に付く)。色モード (R0) で GD の組み方が変わる:
//     16 色      GD0..GD3 がそれぞれ 1 ページ。GD0 が一番手前
//     256 色     GD1:GD0 (GD0 が下位 4 ビット) が手前のページ、GD3:GD2 が奥のページ
//     65536 色   GD3:GD2:GD1:GD0 で 1 ページ
//     実画面 1024x1024 (R0 の bit2)  GD0 が左上、GD1 が右上、GD2 が左下、GD3 が右下。スクロールは R12/R13 だけ。
//                表示 ON/OFF は R2 の bit4
//   はみ出した分は反対側がつながって出る (3.3 節)。
//
// ■ テキスト画面 (2.2 節、3.3 節)
//   1024x1024 ドットの 4 プレーン。4 プレーンの同じ位置のビットが色コード (0..15) になり、テキストパレットを引く。
//   スクロールは R10/R11。表示 ON/OFF は R2 の bit5。
//
// ■ スプライト + BG 画面 (2.3 節、2.4 節、3.2.2 節、3.3.2 節、5.3 節、図 9、図 10、図 18、図 19、図 57)
//   BG コントロール ($EB0808) の bit9 が表示 ON。bit0 が BG0、bit3 が BG1 の ON。bit1 と bit4 が、それぞれが使う
//   BG データエリア (0 = $EBC000、1 = $EBE000)。解像度のレジスタ ($EB0810) の HRES が 0 なら BG は 8x8 ドットの
//   パターンで 2 面 (実画面 512x512)、1 なら 16x16 ドットで BG0 だけ (実画面 1024x1024)。
//   BG のデータとスプライトの第 3 ワード: bit15 上下反転、bit14 左右反転、bit11-8 色コードの上位 4 ビット、bit7-0 パターン番号。
//   パターン (PCG、$EB8000 から) は 8x8 ドットが 8 ロングワードで、ロングワードの最上位の 4 ビットが左端。
//   16x16 ドットのパターン n は 8x8 ドットのパターン 4n..4n+3 を組み合わせる。
//   スプライトは 128 個で、いつも 16x16 ドット。座標 (16,16) が画面の左上。第 4 ワードの下位 2 ビットが
//   0 なら表示しない、1 なら BG の後ろ、2 なら BG0 と BG1 の間、3 なら BG の手前。BG0 はいつも BG1 より手前。
//   同じ水平線上には 32 個までしか出ない。色コードはテキストパレット (256 色分) を引く。
//   スプライト画面の位置は、水平表示位置 ($EB080C) に CRTC の R02 + 4、垂直表示位置 ($EB080E) に R06 を
//   入れたときに、テキストやグラフィックの画面と合う (5.3 節)。
//   スプライトが使えるのは表示画面が 512 ドットか 256 ドットのとき (2.4.1 節)。
//
// ■ 画面どうしの重なり (3.2.1 節、図 17)
//   R1 の bit13-12 がスプライト、bit11-10 がテキスト、bit9-8 がグラフィックの優先順位 (0 が一番手前)。
//
// ■ 半透明と特殊プライオリティ (3.5 節、図 31)
//   グラフィックの一番手前のページ (ベースページ) のデータの最下位ビットが 1 のドットが対象 (R2 の bit10 = 1。
//   本によれば、領域を決める方法はこれしかない)。
//   半透明 (bit12 と bit11 が 1): ベースページのデータは最下位ビットを 0 として表示する。対象のドットは、
//     bit9 が 1 なら 2 番目のページ (セカンドページ) と、bit8 が 1 ならテキスト / スプライトと、色を足して 2 で割る。
//     RGB の 5 ビットずつを足して 1 ビット右にずらし、端数は捨てる。輝度ビットは相手のものを使う。
//     セカンドページの色コードも最下位ビットを無視し、グラフィックパレットの奇数番の色を使う (p.209 図 30。
//     ベースページは偶数番)。色コードが 0 でも、パレット 1 の色と混ぜる。
//     テキスト / スプライトと混ぜるのは、グラフィックのほうが優先順位が高い相手だけ。グラフィックより手前の
//     画面は、混ぜた結果の上にそのまま重なる。
//   bit14 が 1: ほかのビットによらず、対象のドットをテキストパレット 0 の色と混ぜる。
//   特殊プライオリティ (bit12 が 1、bit11 が 0): 対象のドットを、テキストやスプライトより手前に出す。
//
// ■ 本に書かれていないので、ここで決めたこと (実機で確かめたものではない)
//   決めるときには、書き直す前の版 (ソースは見ていない) と同じ場面を描いて画素を比べ、食い違った所を調べた。
//   下の「透明なドットと、重ね方」の細かい所、特殊プライオリティの色、bit10 が 0 のときの扱いは、資料に直接の
//   記述がなく、前の版の出力と同じになるほうを選んだ。
//   - 透明なドットと、重ね方。本には色コード 0 が透明になることも書かれていないので、次の資料とゲームの動きから決めた。
//     (資料) Seafy 氏の「CRTC の設定 その他」(1993 年、crtc.dta。fj.sys.x68000 に再掲されたもの。要約で読んだ):
//       スプライトのパレットを 0 ($0000) にすると、そのスプライトの下の BG ではなくグラフィックが見える。
//       グラフィックの 0 番以外のパレットを 0 にしても透明になる。
//     (ゲーム) 悪魔城ドラキュラは、テキストを表示せずスプライトを一番手前にする場面 (優先順位はスプライト、
//       グラフィック、テキストの順) でテキストパレット 0 を $0842 にし、テキストをグラフィックより手前に出す場面
//       では $0000 にしている。前の場面ではグラフィックが見える。
//     ここから、次の作りとした。テキストとスプライトはパレットが 1 つなので、パレットを引く前に 1 つの色コードに
//     まとめ、引いたあとの色が $0000 かどうかでグラフィックとの重なりを決める:
//       1. テキストとスプライト + BG のうち優先順位の高いほうの色コードが 0 でなければそれ、0 なら低いほうの
//          色コードを使う。スプライト + BG は、パターンのドットが 0 の所 (スプライトも BG もない所) が色コード 0。
//          BG のブロックやスプライトの色コードの上位 4 ビット (bit11-8) が 0 でなくても、パターンのドットが 0 なら
//          色コードは 0 とする (上位 4 ビットだけの色コードにはしない。資料に記述がなく、ここで決めた)
//          表示していない画面は、すべてのドットが色コード 0
//       2. その色コードでテキストパレットを引く。色コードを出した画面がグラフィックより手前なら、色が $0000 で
//          なければその色、$0000 ならグラフィックの色を出す
//       3. グラフィックより奥なら、グラフィックの色が $0000 でなければグラフィックの色、$0000 なら
//          テキストパレットを引いた色を出す
//     つまり、色が $0000 のドットが透明で、色コードは関係しない (グラフィックも、テキストやスプライトも)。
//     テキストとスプライトがどちらも色コード 0 の所は、低いほうの画面の位置にテキストパレット 0 の色がある。
//     テキストパレット 0 が $0000 でなければ、テキストもスプライトもグラフィックより手前のとき、グラフィックは
//     見えない (ドラキュラは、その並びの場面では必ずテキストパレット 0 を $0000 にしていて、これと合う)。
//     スプライトのドットの色が $0000 なら、その下の BG もテキストも出ずにグラフィックが見える (Seafy 氏の資料と合う)。
//   - 優先順位に同じ値や 3 (どちらも禁止) を入れたときは、値の小さい順、同じならスプライト、テキスト、
//     グラフィックの順に手前とする。
//   - スプライトどうしは番号の小さいほうが手前。32 個は番号の小さい順に数え、横にはみ出して見えないものも数える。
//   - 16x16 ドットのパターンの 8x8 ドットの並びは、左上、左下、右上、右下の順 (図 8 は手元の資料で読めなかった。
//     ゲームの画面で確かめた)。
//   - スプライト画面の位置は、水平表示位置が R02 + 4 より大きい分だけ右に (1 につき 8 ドット)、垂直表示位置が
//     R06 より大きい分だけ下にずれるものとした (向きは水平と同じとした。本に向きの記述はなく、実機で確かめる)。
//   - スプライト画面のドットの幅は、$EB0810 の HRES によらず表示画面のドットと同じ (HRES は BG のパターンの大きさと
//     面数だけを変える)。超連射68K が、CRTC は 512 ドットのクロック、$EB0810 は 256 ドット (BG 2 面) の設定で
//     スプライトと BG を出しており、同じ幅で出すと正しい画面になる。
//   - $EB0810 と CRTC の R20 で縦の解像度 (同じラインを 2 回出すか、インタレースか) が違うときは、スプライト画面の
//     ラインを、その比のとおりに繰り返すか飛ばす。
//   - 特殊プライオリティでも、半透明と同じく、ベースページのデータは最下位ビットを 0 として表示する
//     (本は半透明についてだけ書いている。領域の指定のしくみが同じなので、同じとした)。
//   - ベースページのデータが最下位ビットを 0 にすると 0 になるドットは、ベースページにドットがないものとして扱う
//     (16 色や 256 色では奥のページが見える)。ベースページの色が $0000 (透明) のドットも対象にしない。
//   - セカンドページとの半透明では、セカンドページより奥のページは混ぜない。ページが 1 つだけのモード (65536 色、
//     実画面 1024x1024) では何もしない。
//   - テキスト / スプライトと混ぜるのは、上の 1. の色コードを出した画面がグラフィックより奥のときで、その色コードの
//     色と混ぜる (どちらも色コード 0 ならテキストパレット 0 の色)。bit9 と bit8 の両方が 1 のときは、セカンドページと
//     混ぜた色を、さらにテキスト / スプライトと混ぜる。
//   - R2 の bit10 が 0 のとき (本では未定義) は、領域の指定がないものとして、bit12 の半透明と特殊プライオリティを
//     行わない。bit14 の半透明は bit10 によらず行う。
//   - 65536 色では、ワードの最下位ビットを 0 にして色を引く。
//   - テキストの横のスクロールで実画面からはみ出した分は、反対側をつなげて出す (本では「表示がおかしくなる」)。
//
// 速さのための作り: 1 ラインごとに、各画面を色コードの並び (1 ドット 1 バイト) にしてから重ね、最後にパレットを引く。
// 色コードの並びは 8 ドットずつ 64 ビットの値として扱う (リトルエンディアンの CPU を前提にしている)。
#include "x68k/render.h"

#include <string.h>

namespace x68k {

// ベアメタル版には <algorithm> がないので、ここで用意する
static inline int smaller(int a, int b) { return a < b ? a : b; }

namespace {

enum { STACK_ABOVE, STACK_SPLIT, STACK_BELOW }; // テキストとスプライトが、グラフィックより手前か奥か
enum { GRAPHIC_NONE = 0, GRAPHIC_CODE = 1, GRAPHIC_WORD = 2 };

typedef uint8_t Bytes __attribute__((vector_size(8)));

// 2 バイトをビッグエンディアンの 16 ビットとして読む (スプライトのレジスタとデータ)
inline uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
// 8 バイトのどれもが 1
constexpr uint64_t kOnes = 0x0101010101010101ull;
// 8 バイトをそのまま 64 ビットとして読み書きする (番地は 8 の倍数でなくてよい)
inline uint64_t load64(const uint8_t* bytes) {
    uint64_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}
inline void store64(uint8_t* p, uint64_t value) { memcpy(p, &value, sizeof value); }
// bytes バイト (8 の倍数) が同じか。ベアメタルでは memcmp() が 1 バイトずつ比べる関数の呼び出しになるので、使わない
inline bool sameBytes(const void* a, const void* b, int bytes) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    uint64_t diff = 0;
    for (int i = 0; i < bytes; i += 8) diff |= load64(p + i) ^ load64(q + i);
    return diff == 0;
}
inline uint64_t reverseBytes(uint64_t v) { return __builtin_bswap64(v); }

// 1 バイト (8 ドット、最上位ビットが左端) を、1 ドット 1 バイト (0 か 1) に広げたもの。左端のドットが最下位のバイト
uint64_t gSpread[256];
bool gSpreadReady = false;

void prepareSpread() {
    if (gSpreadReady) return;
    for (int b = 0; b < 256; b++) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
            if (b & (0x80 >> i)) v |= 1ull << (8 * i);
        gSpread[b] = v;
    }
    gSpreadReady = true;
}

// 幅 w のラインを、大きさ size の実画面の位置 start から読む。実画面の端で折り返すので、
// 続けて読める区間ごとに f(ラインの位置, 実画面の位置, ドット数) を呼ぶ
template <class F>
inline void wrapRuns(int w, uint32_t start, uint32_t size, F f) {
    int x = 0;
    uint32_t pos = start;
    while (x < w) {
        int n = (int)(size - pos);
        if (n > w - x) n = w - x;
        f(x, pos, n);
        x += n;
        pos = 0;
    }
}

// 色コードの並びを重ねる: front が 0 でない所は front、0 の所は back。8 バイトずつ扱うので、w を 8 の倍数に
// 切り上げた所まで読み書きする
inline void mergeCodes(uint8_t* dst, const uint8_t* front, const uint8_t* back, int w) {
    for (int x = 0; x < w; x += 8) {
        Bytes f, b;
        memcpy(&f, front + x, sizeof f);
        memcpy(&b, back + x, sizeof b);
        const Bytes merged = f | (b & ~(Bytes)(f != 0));
        memcpy(dst + x, &merged, sizeof merged);
    }
}

// テキスト画面の 1 ライン。dst[0..w) に色コード (0..15)。dst の前後 8 バイトまで書くことがある
void buildTextLine(const Video& v, int y, int w, uint8_t* dst) {
    uint32_t sx = v.reg(10) & 1023, sy = v.reg(11) & 1023;
    const uint8_t* p0 = v.tvram() + (((uint32_t)y + sy) & 1023) * 128;
    const uint8_t* p1 = p0 + 0x20000;
    const uint8_t* p2 = p0 + 0x40000;
    const uint8_t* p3 = p0 + 0x60000;
    uint32_t col = sx >> 3;
    int sub = (int)(sx & 7);
    uint8_t* d = dst - sub;
    int n = (w + sub + 7) >> 3;
    for (int i = 0; i < n; i++) {
        uint32_t c = (col + (uint32_t)i) & 127;
        uint64_t code = gSpread[p0[c]] | (gSpread[p1[c]] << 1) | (gSpread[p2[c]] << 2) | (gSpread[p3[c]] << 3);
        store64(d + i * 8, code);
    }
}

// グラフィック VRAM のワードの並びを、1 ドット 1 バイトの色コードにする。8 ドットずつまとめて計算できるよう、
// GCC / Clang のベクトル型を使う (コンパイラの自動のベクトル化には頼らない。-O2 でも SIMD の命令になる)。
// f はワード 1 つ (unsigned) にも 8 つ (Words) にも使える式にする
typedef uint16_t Words __attribute__((vector_size(16)));

// front が 0 でなければ front、0 なら back
inline unsigned pick(unsigned front, unsigned back) { return front ? front : back; }
inline Words pick(Words front, Words back) { return front | (back & ~(Words)(front != 0)); }

template <class F>
inline void mapWords(const uint16_t* src, int n, uint8_t* dst, F f) {
    const int whole = n & ~7; // 8 ワードずつまとめて計算できる所
    for (int i = 0; i < whole; i += 8) {
        Words wd;
        memcpy(&wd, src + i, sizeof wd);
        Bytes b = __builtin_convertvector(f(wd), Bytes);
        memcpy(dst + i, &b, sizeof b);
    }
    for (int i = whole; i < n; i++) dst[i] = (uint8_t)f((unsigned)src[i]);
}

// テキスト画面だけのライン (ほかの画面がなく、横のスクロールが 8 の倍数で、w が 8 の倍数のとき)。
// 色コードの並びを作らずに、そのまま画素にする。pairs は、となり合う 2 ドットの色コード (左が下位 4 ビット) から
// 2 ドット分の画素を引く表
void drawTextOnly(const Video& v, int y, int w, uint32_t* out, const uint64_t* pairs) {
    uint32_t sx = v.reg(10) & 1023, sy = v.reg(11) & 1023;
    const uint8_t* p0 = v.tvram() + (((uint32_t)y + sy) & 1023) * 128;
    const uint8_t* p1 = p0 + 0x20000;
    const uint8_t* p2 = p0 + 0x40000;
    const uint8_t* p3 = p0 + 0x60000;
    uint32_t col = sx >> 3;
    int n = w >> 3;
    for (int i = 0; i < n; i++) {
        uint32_t c = (col + (uint32_t)i) & 127;
        uint64_t code = gSpread[p0[c]] | (gSpread[p1[c]] << 1) | (gSpread[p2[c]] << 2) | (gSpread[p3[c]] << 3);
        code |= code >> 4; // 2 バイトごとに、下位のバイトが 2 ドット分の色コード
        uint32_t* o = out + i * 8;
        memcpy(o, &pairs[code & 0xff], 8);
        memcpy(o + 2, &pairs[(code >> 16) & 0xff], 8);
        memcpy(o + 4, &pairs[(code >> 32) & 0xff], 8);
        memcpy(o + 6, &pairs[(code >> 48) & 0xff], 8);
    }
}

// グラフィックの GD0..GD3 のひとつ
struct Slot {
    int shift;       // ワードの中のビットの位置 (ブロックの番号 x 4)
    uint16_t mask;   // 表示 ON なら 15、OFF なら 0
    uint32_t sx, sy; // そのブロックの表示開始位置
};

inline bool sameStart(const Slot& a, const Slot& b) { return a.sx == b.sx && a.sy == b.sy; }

// GD のひとつを 1 ライン分取り出す (dst[0..w) に 0..15)
void fetchSlot(const Video& v, const Slot& s, int y, int w, uint8_t* dst) {
    if (!s.mask) {
        memset(dst, 0, (size_t)w);
        return;
    }
    const uint16_t* row = v.gvramWords() + (((uint32_t)y + s.sy) & 511) * 512;
    const int shift = s.shift;
    wrapRuns(w, s.sx, 512, [&](int x, uint32_t pos, int n) {
        mapWords(row + pos, n, dst + x, [=](auto wd) { return (wd >> shift) & (uint16_t)15; });
    });
}

// 256 色の 1 ページ (lo が下位 4 ビット、hi が上位 4 ビット)。どちらも OFF なら false。work は作業用
bool fetchPage256(const Video& v, const Slot& lo, const Slot& hi, int y, int w, uint8_t* dst, uint8_t* work) {
    if (!lo.mask && !hi.mask) return false;
    if (lo.mask && hi.mask && !sameStart(lo, hi)) {
        fetchSlot(v, lo, y, w, dst);
        fetchSlot(v, hi, y, w, work);
        for (int x = 0; x < w; x += 8) store64(dst + x, load64(dst + x) | (load64(work + x) << 4)); // どのバイトも 15 以下
        return true;
    }
    const Slot& ref = lo.mask ? lo : hi;
    const uint16_t* row = v.gvramWords() + (((uint32_t)y + ref.sy) & 511) * 512;
    const int ls = lo.shift, hs = hi.shift;
    const uint16_t lm = lo.mask, hm = hi.mask;
    wrapRuns(w, ref.sx, 512, [&](int x, uint32_t pos, int n) {
        mapWords(row + pos, n, dst + x, [=](auto wd) { return ((wd >> ls) & lm) | (((wd >> hs) & hm) << 4); });
    });
    return true;
}

// 実画面 1024x1024 の 1 ライン (dst[0..w) に 0..15)
void fetchLine1024(const Video& v, uint16_t r1, int y, int w, uint8_t* dst) {
    uint32_t sx = v.reg(12) & 1023;
    uint32_t ry = ((uint32_t)y + v.reg(13)) & 1023;
    const uint16_t* row = v.gvramWords() + (ry & 511) * 512;
    int lower = ry >= 512 ? 2 : 0;
    wrapRuns(w, sx, 1024, [&](int x, uint32_t pos, int n) {
        while (n > 0) {
            int quadrant = lower + (pos >= 512 ? 1 : 0);
            int shift = 4 * ((r1 >> (2 * quadrant)) & 3);
            uint32_t px = pos & 511;
            int m = (int)(512 - px);
            if (m > n) m = n;
            mapWords(row + px, m, dst + x, [=](auto wd) { return (wd >> shift) & (uint16_t)15; });
            x += m;
            pos += (uint32_t)m;
            n -= m;
        }
    });
}

// PCG の 1 ロングワード (8 ドット、最上位の 4 ビットが左端) を、1 ドット 1 バイト (0..15) に広げる。左端が最下位のバイト
inline uint64_t spreadPattern(const uint8_t* p) {
    uint32_t u;
    memcpy(&u, p, 4); // メモリの順のまま。最下位のバイトが左の 2 ドット
    uint64_t x = u;
    x = (x | (x << 16)) & 0x0000ffff0000ffffull;
    x = (x | (x << 8)) & 0x00ff00ff00ff00ffull; // 16 ビットごとに 1 バイト (2 ドット)
    return ((x >> 4) & 0x000f000f000f000full) | ((x & 0x000f000f000f000full) << 8);
}

// 8 ドットを重ねる。0 のドットは透明。0 でないドットは、色コードの上位 4 ビット (block) を付けて書く
inline void putPattern(uint8_t* d, uint64_t dots, unsigned block) {
    if (!dots) return;
    Bytes pixels, previous;
    memcpy(&pixels, &dots, sizeof pixels);
    memcpy(&previous, d, sizeof previous);
    const Bytes mask = (Bytes)(pixels != 0);
    const Bytes colors = pixels | (uint8_t)(block << 4);
    const Bytes merged = (colors & mask) | (previous & ~mask);
    memcpy(d, &merged, sizeof merged);
}

// 16x16 ドットのパターンの 1 行 (row は 0..15)。左右の 8 ドットずつを返す
inline void pattern16(const uint8_t* pcg, unsigned number, int row, bool flipH, uint64_t& left, uint64_t& right) {
    // 8x8 ドットのパターン 4n が左上、4n+1 が左下、4n+2 が右上、4n+3 が右下
    const uint8_t* p = pcg + number * 128 + (row >> 3) * 32 + (row & 7) * 4;
    uint64_t l = spreadPattern(p), r = spreadPattern(p + 64);
    left = flipH ? reverseBytes(r) : l;
    right = flipH ? reverseBytes(l) : r;
}

// CRTC の R20 と同じ並びの値 (bit4 が水平周波数、bit3-2 が垂直の解像度) から、2 ラスタで進むデータのライン数。
// 31kHz の 256 ライン (同じラインを 2 回出す) は 1、インタレースは 4、ほかは 2
inline int linesPerTwoRasters(unsigned mode) {
    unsigned vres = (mode >> 2) & 3;
    if (mode & 0x10) return vres == 0 ? 1 : vres == 1 ? 2 : 4;
    return vres == 0 ? 2 : 4;
}

inline int floorDiv(int a, int b) { // b > 0
    int q = a / b;
    if ((a % b) < 0) q--;
    return q;
}

// 色を足して 2 で割る (3.5.1 節の注)。RGB の 5 ビットずつで、端数は捨てる。輝度ビットは相手 (other) のもの。
// 3 つの 5 ビットをまとめて計算する: (a + b) / 2 = (a & b) + ((a ^ b) >> 1)。右にずらす前に、それぞれの最下位ビットを
// 消して、となりの色に入らないようにする
inline uint16_t blend(uint16_t base, uint16_t other) {
    unsigned half = (unsigned)(base & other & 0xfffe) + (((unsigned)(base ^ other) & 0xf7bc) >> 1);
    return (uint16_t)(half | (other & 1));
}

} // namespace

Renderer::Renderer(const Video& source) : video_(source) {
    prepareSpread();
    buildColorSplitTables();
}

// 半透明で使う、色 (16 ビット) を上位バイトと下位バイトに分けて XRGB にする表。paletteToRgb() は出力の各ビットが
// 入力の 1 ビットだけで決まるので、2 つの表を引いて OR すれば、16 ビットをまとめて変換したのと同じになる
void Renderer::buildColorSplitTables() {
    for (uint32_t byte = 0; byte < 256; byte++) {
        colorLoXrgb_[byte] = paletteToRgb((uint16_t)byte);
        colorHiXrgb_[byte] = paletteToRgb((uint16_t)(byte << 8));
    }
}

void Renderer::setTarget(uint32_t* dest, int stride, int columns, int rows) {
    surface_ = Surface{dest, stride, smaller(columns, stride), rows};
}

void Renderer::geometry(int& width, int& height) const {
    int screenW = 0, screenH = 0;
    video_.screenSize(screenW, screenH);
    width = smaller(screenW, MAX_WIDTH);
    height = smaller(screenH, MAX_HEIGHT);
}

void Renderer::syncGeometry() {
    uint32_t gen = video_.crtcGeneration();
    if (geometryValid_ && gen == crtcSeen_) return;
    crtcSeen_ = gen;
    geometryValid_ = true;
    geometry(width_, height_);
    linesPerTwoRasters_ = linesPerTwoRasters(video_.reg(20));
}

void Renderer::renderFrame() {
    syncGeometry();
    for (int y = 0; y < height_; y++) renderLine(y);
}

void Renderer::onRaster(int raster) {
    syncGeometry();
    if (raster < 0) return;
    switch (linesPerTwoRasters_) {
    case 1: // 同じラインを 2 回出す。1 回目のラスタで描く
        if (!(raster & 1)) renderLine(raster >> 1);
        break;
    case 4: // インタレース。2 つのフィールドのラインを続けて描く
        renderLine(raster * 2);
        renderLine(raster * 2 + 1);
        break;
    default:
        renderLine(raster);
        break;
    }
}

// スプライト + BG 画面の 1 ライン。spriteLine_[MARGIN..MARGIN + w) に色コード (0 は透明)。
// スプライト画面が出ていなければ false
bool Renderer::buildSpriteLine(int y, int w) {
    const uint8_t* mem = video_.sprite();
    const unsigned control = be16(mem + 0x808);
    if (!(control & 0x0200)) return false;
    const unsigned r20 = video_.reg(20);
    if ((r20 & 3) >= 2) return false; // 768 ドットの画面ではスプライトは使えない
    const unsigned mode = be16(mem + 0x810);
    const bool wide = (mode & 3) != 0; // 512 ドット: BG は 16x16 ドットで BG0 だけ

    // 表示画面のライン y に出るスプライト画面のライン
    const int crtcLines = linesPerTwoRasters_, spriteLines = linesPerTwoRasters(mode);
    const int down = (int)(be16(mem + 0x80e) & 0x3ff) - video_.verticalDisplayStart(); // ラスタ数
    int line;
    if (crtcLines == spriteLines) line = y - floorDiv(down * spriteLines, 2);
    else line = floorDiv((floorDiv(y * 2, crtcLines) - down) * spriteLines, 2);
    // 表示画面の位置 0 に出るスプライト画面の位置は -right
    const int right = ((int)(be16(mem + 0x80c) & 0xff) - 4 - video_.horizontalDisplayStart()) * 8;

    const int sw = w;
    uint8_t* const dst = spriteLine_ + MARGIN;
    memset(dst - MARGIN, 0, (size_t)sw + 2 * MARGIN);

    // このラインに掛かるスプライトを、番号の小さい順に 32 個まで
    uint8_t list[32];
    int total = 0;
    if (!spriteRowsValid_ || spriteRowsSeen_ != video_.spriteRowsGeneration()) {
        if (!spriteRowsValid_) {
            memset(spriteRows_, 0, sizeof spriteRows_);
            memset(spriteEnabledSeen_, 0, sizeof spriteEnabledSeen_);
        }
        for (unsigned n = 0; n < 128; n++) {
            const uint8_t* s = mem + n * 8;
            const bool enabled = (s[7] & 3) != 0;
            const unsigned y = be16(s + 2) & 1023;
            const bool moved = spriteYSeen_[n] != y;
            const uint32_t bit = 1u << (n & 31);
            if (spriteEnabledSeen_[n] && (!enabled || moved))
                for (unsigned row = spriteYSeen_[n]; row < spriteYSeen_[n] + 16u; row++) spriteRows_[row][n >> 5] &= ~bit;
            if (enabled && (!spriteEnabledSeen_[n] || moved))
                for (unsigned row = y; row < y + 16; row++) spriteRows_[row][n >> 5] |= bit;
            spriteYSeen_[n] = (uint16_t)y;
            spriteEnabledSeen_[n] = enabled;
        }
        spriteRowsSeen_ = video_.spriteRowsGeneration();
        spriteRowsValid_ = true;
    }
    if (line >= -16 && line < 1024) {
        for (unsigned word = 0; word < 4 && total < 32; word++) {
            uint32_t mask = spriteRows_[line + 16][word];
            while (mask && total < 32) {
                const unsigned n = word * 32 + __builtin_ctz(mask);
                mask &= mask - 1;
                list[total++] = (uint8_t)n;
            }
        }
    }

    // スプライトどうしは、優先度 (第 4 ワード) によらず、番号の小さいスプライトの不透明なドットが勝つ。優先度は、勝ったドットが
    // BG の前か後ろかを決めるだけ (スプライトのドットを選んでから BG と重ねる)。番号の小さい順に、すでに不透明なドットがある所を
    // 除いて見えるドットを決め (occ)、そのあと BG との重なりの順 (1..3) ごとに描く
    const uint8_t* pcg = mem + 0x8000;
    uint8_t occ[MAX_WIDTH + 2 * MARGIN];
    if (total) memset(occ, 0, (size_t)sw + 2 * MARGIN);
    struct Visible {
        int x;
        uint64_t left, right;
        unsigned block;
        unsigned group; // 0..2 (優先度 1..3)
    } vis[32];
    int visCount = 0;
    for (int k = 0; k < total; k++) {
        const uint8_t* s = mem + list[k] * 8;
        int x = (int)(be16(s) & 1023) - 16 + right;
        if (x <= -16 || x >= sw) continue;
        unsigned attr = be16(s + 4);
        int row = line + 16 - (int)(be16(s + 2) & 1023);
        if (attr & 0x8000) row = 15 - row;
        uint64_t left, rightDots;
        pattern16(pcg, attr & 0xff, row, (attr & 0x4000) != 0, left, rightDots);
        uint8_t* o = occ + MARGIN + x;
        const uint64_t occLeft = load64(o), occRight = load64(o + 8);
        Visible& v = vis[visCount++];
        v.x = x;
        v.left = left & ~occLeft;
        v.right = rightDots & ~occRight;
        v.block = (attr >> 8) & 15;
        v.group = (s[7] & 3) - 1;
        // 0 でないドットのバイトを 0xff にして、占めたことにする
        store64(o, occLeft | (((left + 0x0f0f0f0f0f0f0f0full) >> 4) & kOnes) * 0xff);
        store64(o + 8, occRight | (((rightDots + 0x0f0f0f0f0f0f0f0full) >> 4) & kOnes) * 0xff);
    }
    auto drawSprites = [&](unsigned group) {
        for (int k = 0; k < visCount; k++) {
            const Visible& v = vis[k];
            if (v.group != group) continue;
            putPattern(dst + v.x, v.left, v.block);
            putPattern(dst + v.x + 8, v.right, v.block);
        }
    };
    auto drawBg = [&](int n) {
        const uint8_t* data = mem + 0xc000 + ((control >> (n ? 4 : 1)) & 1) * 0x2000;
        const int scrollX = be16(mem + 0x800 + n * 4), scrollY = be16(mem + 0x802 + n * 4);
        const int shift = wide ? 4 : 3;
        const int tile = 1 << shift, size = 64 << shift;
        const int by = (line + scrollY) & (size - 1);
        const int bx = (scrollX - right) & (size - 1);
        const uint8_t* rowData = data + (by >> shift) * 128;
        const int inRow = by & (tile - 1);
        int column = bx >> shift;
        for (int x = -(bx & (tile - 1)); x < sw; x += tile, column = (column + 1) & 63) {
            unsigned cell = be16(rowData + column * 2);
            unsigned block = (cell >> 8) & 15;
            int row = (cell & 0x8000) ? tile - 1 - inRow : inRow;
            bool flipH = (cell & 0x4000) != 0;
            if (wide) {
                uint64_t left, rightDots;
                pattern16(pcg, cell & 0xff, row, flipH, left, rightDots);
                putPattern(dst + x, left, block);
                putPattern(dst + x + 8, rightDots, block);
            } else {
                uint64_t dots = spreadPattern(pcg + (cell & 0xff) * 32 + row * 4);
                if (flipH) dots = reverseBytes(dots);
                putPattern(dst + x, dots, block);
            }
        }
    };

    // 奥から順に: BG の後ろのスプライト、BG1、間のスプライト、BG0、手前のスプライト
    drawSprites(0);
    if (!wide && (control & 0x08)) drawBg(1);
    drawSprites(1);
    if (control & 0x01) drawBg(0);
    drawSprites(2);

    return true;
}

// グラフィック画面の 1 ライン。戻り値は GRAPHIC_NONE (表示していない)、GRAPHIC_CODE、GRAPHIC_WORD (65536 色)。
//   split が false: ページを重ねた結果を graphicLine_ (色コード) か wordLine_ (65536 色のワード) に入れる
//   split が true (半透明、特殊プライオリティ用): ベースページのデータを wordLine_、セカンドページの色コードを
//     slotLine_[1]、ベースページより奥のページを重ねた色コードを restLine_ に入れる。セカンドページがあるか
//     (16 色か 256 色か) を hasSecondPage_ に入れる
int Renderer::buildGraphicLine(int y, int w, uint16_t r0, uint16_t r1, uint16_t r2, bool split) {
    uint8_t* const out = graphicLine_ + MARGIN;
    uint8_t* const rest = restLine_ + MARGIN;
    uint16_t* const word = wordLine_ + MARGIN;
    uint8_t* const g0 = slotLine_[0] + MARGIN;
    uint8_t* const g1 = slotLine_[1] + MARGIN;
    uint8_t* const g2 = slotLine_[2] + MARGIN;
    uint8_t* const g3 = slotLine_[3] + MARGIN;

    if (r0 & 4) { // 実画面 1024x1024
        if (!(r2 & 0x10)) return GRAPHIC_NONE;
        if (!split) {
            fetchLine1024(video_, r1, y, w, out);
            return GRAPHIC_CODE;
        }
        fetchLine1024(video_, r1, y, w, g0);
        for (int x = 0; x < w; x++) word[x] = g0[x];
        memset(rest, 0, (size_t)w);
        hasSecondPage_ = false;
        return GRAPHIC_CODE;
    }
    if (!(r2 & 0x0f)) return GRAPHIC_NONE;

    Slot slot[4];
    for (int i = 0; i < 4; i++) {
        int bank = (r1 >> (2 * i)) & 3;
        slot[i].shift = bank * 4;
        slot[i].mask = (r2 & (1 << i)) ? 15 : 0;
        slot[i].sx = video_.reg(12 + bank * 2) & 511;
        slot[i].sy = video_.reg(13 + bank * 2) & 511;
    }
    const int colors = r0 & 3;

    if (split) {
        fetchSlot(video_, slot[0], y, w, g0);
        fetchSlot(video_, slot[1], y, w, g1);
        fetchSlot(video_, slot[2], y, w, g2);
        fetchSlot(video_, slot[3], y, w, g3);
        hasSecondPage_ = colors != 3;
        if (colors == 0) {
            for (int x = 0; x < w; x++) word[x] = g0[x];
            mergeCodes(rest, g2, g3, w);
            mergeCodes(rest, g1, rest, w);
            return GRAPHIC_CODE;
        }
        if (colors == 3) {
            for (int x = 0; x < w; x++) word[x] = (uint16_t)(g0[x] | (g1[x] << 4) | (g2[x] << 8) | (g3[x] << 12));
            memset(rest, 0, (size_t)w);
            return GRAPHIC_WORD;
        }
        for (int x = 0; x < w; x++) {
            word[x] = (uint16_t)(g0[x] | (g1[x] << 4));
            rest[x] = (uint8_t)(g2[x] | (g3[x] << 4));
        }
        memcpy(g1, rest, (size_t)w); // セカンドページは奥のページ
        return GRAPHIC_CODE;
    }

    // 表示している GD の表示開始位置がすべて同じか (ふつうの設定)
    const Slot* first = nullptr;
    bool together = true;
    for (int i = 0; i < 4; i++) {
        if (!slot[i].mask) continue;
        if (!first) first = &slot[i];
        else if (!sameStart(*first, slot[i])) together = false;
    }

    if (colors == 3) { // 65536 色
        if (!together) {
            fetchSlot(video_, slot[0], y, w, g0);
            fetchSlot(video_, slot[1], y, w, g1);
            fetchSlot(video_, slot[2], y, w, g2);
            fetchSlot(video_, slot[3], y, w, g3);
            for (int x = 0; x < w; x++) word[x] = (uint16_t)(g0[x] | (g1[x] << 4) | (g2[x] << 8) | (g3[x] << 12));
            return GRAPHIC_WORD;
        }
        const uint16_t* row = video_.gvramWords() + (((uint32_t)y + first->sy) & 511) * 512;
        const bool plain = (r1 & 0xff) == 0xe4 && (r2 & 0x0f) == 0x0f; // ワードがそのまま色コード
        const int s0 = slot[0].shift, s1 = slot[1].shift, s2 = slot[2].shift, s3 = slot[3].shift;
        const unsigned m0 = slot[0].mask, m1 = slot[1].mask, m2 = slot[2].mask, m3 = slot[3].mask;
        wrapRuns(w, first->sx, 512, [&](int x, uint32_t pos, int n) {
            const uint16_t* src = row + pos;
            uint16_t* d = word + x;
            if (plain) {
                memcpy(d, src, (size_t)n * 2);
            } else {
                for (int i = 0; i < n; i++) {
                    unsigned wd = src[i];
                    d[i] = (uint16_t)(((wd >> s0) & m0) | (((wd >> s1) & m1) << 4) | (((wd >> s2) & m2) << 8) |
                                      (((wd >> s3) & m3) << 12));
                }
            }
        });
        return GRAPHIC_WORD;
    }

    if (colors == 0) { // 16 色 4 ページ
        if (!together) {
            fetchSlot(video_, slot[0], y, w, g0);
            fetchSlot(video_, slot[1], y, w, g1);
            fetchSlot(video_, slot[2], y, w, g2);
            fetchSlot(video_, slot[3], y, w, g3);
            mergeCodes(out, g2, g3, w);
            mergeCodes(out, g1, out, w);
            mergeCodes(out, g0, out, w);
            return GRAPHIC_CODE;
        }
        const uint16_t* row = video_.gvramWords() + (((uint32_t)y + first->sy) & 511) * 512;
        const int s0 = slot[0].shift, s1 = slot[1].shift, s2 = slot[2].shift, s3 = slot[3].shift;
        const uint16_t m0 = slot[0].mask, m1 = slot[1].mask, m2 = slot[2].mask, m3 = slot[3].mask;
        wrapRuns(w, first->sx, 512, [&](int x, uint32_t pos, int n) {
            mapWords(row + pos, n, out + x, [=](auto wd) {
                auto c = pick((wd >> s2) & m2, (wd >> s3) & m3);
                c = pick((wd >> s1) & m1, c);
                return pick((wd >> s0) & m0, c);
            });
        });
        return GRAPHIC_CODE;
    }

    // 256 色 2 ページ (色モード 2 は未定義。256 色と同じに扱う)
    bool front = (slot[0].mask | slot[1].mask) != 0, back = (slot[2].mask | slot[3].mask) != 0;
    if (front && back && together) { // 2 ページとも同じ表示開始位置: ワードから一度に取り出して重ねる
        const uint16_t* row = video_.gvramWords() + (((uint32_t)y + first->sy) & 511) * 512;
        const int s0 = slot[0].shift, s1 = slot[1].shift, s2 = slot[2].shift, s3 = slot[3].shift;
        const uint16_t m0 = slot[0].mask, m1 = slot[1].mask, m2 = slot[2].mask, m3 = slot[3].mask;
        wrapRuns(w, first->sx, 512, [&](int x, uint32_t pos, int n) {
            mapWords(row + pos, n, out + x, [=](auto wd) {
                return pick(((wd >> s0) & m0) | (((wd >> s1) & m1) << 4), ((wd >> s2) & m2) | (((wd >> s3) & m3) << 4));
            });
        });
    } else if (front && back) {
        fetchPage256(video_, slot[0], slot[1], y, w, g0, g1);
        fetchPage256(video_, slot[2], slot[3], y, w, rest, g1);
        mergeCodes(out, g0, rest, w);
    } else if (front) {
        fetchPage256(video_, slot[0], slot[1], y, w, out, g1);
    } else { // 奥のページだけ (どちらもなければ上で GRAPHIC_NONE を返している)
        fetchPage256(video_, slot[2], slot[3], y, w, out, g1);
    }
    return GRAPHIC_CODE;
}

// 半透明、特殊プライオリティのあるライン (3.5 節)。1 ドットずつ色 (パレットの 16 ビットの値) で計算する。
// front、back、stacking は composeLayers() と同じ (ここでは nullptr にしない)
void Renderer::composeSpecial(int w, uint32_t* out, uint16_t r2, int graphic, const uint8_t* front, const uint8_t* back,
                              int stacking) {
    const uint16_t* pal = video_.paletteColors();
    const uint16_t* tpal = pal + 256;
    const uint16_t* hi = video_.color64kHigh();
    const uint16_t* lo = video_.color64kLow();
    // このラインでは、ベースページのデータの最下位ビットは領域の指定に使い、色コードとしては 0 として扱う
    const bool textPalette0 = (r2 & 0x4000) != 0;                    // テキストパレット 0 との半透明
    const bool priority = !textPalette0 && !(r2 & 0x0800);           // 特殊プライオリティ
    const bool withSecond = !textPalette0 && (r2 & 0x0a00) == 0x0a00 && hasSecondPage_; // セカンドページとの半透明
    const bool withText = !textPalette0 && (r2 & 0x0900) == 0x0900;   // テキスト / スプライトとの半透明
    const bool wordColor = graphic == GRAPHIC_WORD;

    const uint16_t* base = wordLine_ + MARGIN;
    const uint8_t* second = slotLine_[1] + MARGIN;
    const uint8_t* rest = restLine_ + MARGIN;

    // 予測の当たらない分岐を作らないよう、どのドットも同じ手順で計算して最後に選ぶ
    const uint16_t t0 = tpal[0];
    for (int x = 0; x < w; x++) {
        unsigned data = base[x];
        const bool marked = (data & 1) != 0;
        data &= ~1u;
        // グラフィックのドットの色。ベースページにドットがなければ奥のページ
        const unsigned code = data ? data : rest[x]; // 65536 色では使わない
        const uint16_t color = wordColor ? (uint16_t)(hi[data >> 8] | lo[data & 0xff]) : pal[code];
        // テキスト / スプライトの色と、それがグラフィックより手前か
        const bool above = stacking == STACK_ABOVE || (stacking == STACK_SPLIT && front[x]);
        const uint16_t other = tpal[above ? front[x] : back[x]];
        // ふつうの重なり
        const uint16_t over = other ? other : color, under = color ? color : other;
        uint16_t result = above ? over : under;
        // 領域に入っている、ベースページのドット
        if (priority) {
            // テキストやスプライトより手前に出す
            if (marked && (wordColor || data) && color) result = color;
        } else {
            // テキストパレット 0 と混ぜるか、セカンドページと (あれば) テキストと順に混ぜる
            uint16_t mixed = textPalette0 ? blend(color, t0) : color;
            if (!textPalette0 && withSecond) mixed = blend(mixed, pal[second[x] | 1]);
            if (!textPalette0 && withText && !above) mixed = blend(mixed, other);
            // 混ぜた結果が $0000 でも、ここにはグラフィックのドットがある
            const uint16_t shown = above && other ? other : mixed;
            if (marked && (wordColor || data) && color) result = shown;
        }
        out[x] = colorHiXrgb_[result >> 8] | colorLoXrgb_[result & 0xff];
    }
}

namespace {

// 画面を重ねて 1 ライン分の画素にする (先頭の説明の「透明なドットと、重ね方」)。
//   tlut: テキストパレットの色 (XRGB)。graphicAt(x): グラフィックのドットの色 (XRGB。$0000 なら 0)
//   stacking と、テキストとスプライトをまとめた色コード (nullptr ならすべて 0):
//     STACK_ABOVE  どちらもグラフィックより手前。front が 2 つをまとめたもの
//     STACK_BELOW  どちらもグラフィックより奥。back が 2 つをまとめたもの
//     STACK_SPLIT  front がグラフィックより手前のもの、back が奥のもの
template <class G>
inline void composeLayers(uint32_t* out, int w, int stacking, const uint8_t* front, const uint8_t* back,
                          const uint32_t* tlut, G graphicAt) {
    const uint32_t t0 = tlut[0];
    // 場合ごとに 1 つのループで描いて戻る
    if (stacking == STACK_ABOVE) {
        if (front) {
            for (int x = 0; x < w; x++) {
                uint32_t a = tlut[front[x]], g = graphicAt(x);
                out[x] = a ? a : g;
            }
            return;
        }
        if (t0) {
            for (int x = 0; x < w; x++) out[x] = t0;
            return;
        }
        for (int x = 0; x < w; x++) out[x] = graphicAt(x);
        return;
    }
    if (stacking == STACK_SPLIT && front) {
        // 手前のものがあるドットは手前のもの、ないドットはグラフィックの奥のものを見る。
        // 奥のものがなければ、奥はテキストパレット 0 の色 (ループの中で調べないよう、2 つに分ける)
        if (back) {
            for (int x = 0; x < w; x++) {
                const uint32_t g = graphicAt(x);
                const uint32_t a = tlut[front[x]];
                const uint32_t b = tlut[back[x]]; // 分岐にならないよう、使わないときも読む
                const uint32_t over = a ? a : g;
                const uint32_t under = g ? g : b;
                out[x] = front[x] ? over : under;
            }
            return;
        }
        for (int x = 0; x < w; x++) {
            const uint32_t g = graphicAt(x);
            const uint32_t a = tlut[front[x]];
            const uint32_t over = a ? a : g;
            const uint32_t under = g ? g : t0;
            out[x] = front[x] ? over : under;
        }
        return;
    }
    // 手前のものはない。グラフィックのドットがなければ奥のもの (なければテキストパレット 0 の色)
    if (back) {
        for (int x = 0; x < w; x++) {
            const uint32_t g = graphicAt(x);
            const uint32_t b = tlut[back[x]]; // 分岐にならないよう、使わないときも読む
            out[x] = g ? g : b;
        }
        return;
    }
    for (int x = 0; x < w; x++) {
        const uint32_t g = graphicAt(x);
        out[x] = g ? g : t0;
    }
}

} // namespace

void Renderer::renderLine(int y) {
    syncGeometry();
    if (!surface_.pixels || y < 0 || y >= smaller(height_, surface_.rows)) return;
    const int w = smaller(width_, surface_.columns);
    if (w <= 0) return;
    if (indexedSink_) indexedSink_->beginLine(y, w);
    uint32_t* out = surface_.pixels + (size_t)y * (size_t)surface_.pitch;
    const uint16_t r0 = video_.videoControl(0), r1 = video_.videoControl(1), r2 = video_.videoControl(2);
    const uint32_t* xrgb = video_.paletteXrgb();
    const uint32_t* const tlut = xrgb + 256;
    // 半透明か特殊プライオリティ: テキストパレット 0 との半透明 (bit14)、または bit12 と、領域の指定の bit10
    const bool special = (r2 & 0x4000) || (r2 & 0x1400) == 0x1400;

    const int graphic = buildGraphicLine(y, w, r0, r1, r2, special);
    const uint8_t* sprite = (r2 & 0x40) && buildSpriteLine(y, w) ? spriteLine_ + MARGIN : nullptr;
    const uint8_t* text = nullptr;
    if (r2 & 0x20) {
        if (graphic == GRAPHIC_NONE && !sprite && !(w & 7) && !(video_.reg(10) & 7)) {
            // 色コードがテキストだけなら、重なりの順によらずその色が出る
            if (!textPairsValid_ || !sameBytes(tlut, textPairsSeen_, sizeof textPairsSeen_)) {
                memcpy(textPairsSeen_, tlut, sizeof textPairsSeen_);
                for (int i = 0; i < 256; i++) textPairs_[i] = tlut[i & 15] | ((uint64_t)tlut[i >> 4] << 32);
                textPairsValid_ = true;
            }
            drawTextOnly(video_, y, w, out, textPairs_);
            return;
        }
        buildTextLine(video_, y, w, textLine_ + MARGIN);
        text = textLine_ + MARGIN;
    }

    // 画面どうしの優先順位 (R1 の上位バイト)。値の小さいものが手前。同じ値ならスプライト、テキスト、グラフィックの順
    const int spriteRank = (r1 >> 12) & 3, textRank = (r1 >> 10) & 3, graphicRank = (r1 >> 8) & 3;
    const bool spriteFirst = spriteRank <= textRank;
    const uint8_t* const upper = spriteFirst ? sprite : text; // テキストとスプライトのうち手前のもの
    const uint8_t* const lower = spriteFirst ? text : sprite;
    const bool upperAbove = (spriteFirst ? spriteRank : textRank) <= graphicRank;
    const bool lowerAbove = (spriteFirst ? textRank : spriteRank) <= graphicRank;
    // テキストとスプライトを、グラフィックより手前の分 (front) と奥の分 (back) にまとめる。
    // 表示していない画面は、すべてのドットが色コード 0 の画面として扱う
    const int stacking = lowerAbove ? STACK_ABOVE : upperAbove ? STACK_SPLIT : STACK_BELOW;
    const uint8_t* front = nullptr;
    const uint8_t* back = nullptr;
    if (stacking == STACK_SPLIT) {
        front = upper;
        back = lower;
    } else {
        const uint8_t* both = upper ? upper : lower;
        if (upper && lower) {
            mergeCodes(mergeLine_ + MARGIN, upper, lower, w);
            both = mergeLine_ + MARGIN;
        }
        (stacking == STACK_ABOVE ? front : back) = both;
    }

    if (graphic == GRAPHIC_NONE) {
        if (indexedSink_) {
            const uint8_t* codes = stacking == STACK_ABOVE ? front : back;
            if (stacking == STACK_SPLIT) {
                codes = front ? front : back;
                if (front && back) {
                    mergeCodes(mergeLine_ + MARGIN, front, back, w);
                    codes = mergeLine_ + MARGIN;
                }
            }
            if (indexedSink_->above(y, w, codes, nullptr, tlut, nullptr)) return;
        }
        composeLayers(out, w, stacking, front, back, tlut, [](int) { return (uint32_t)0; });
        return;
    }
    if (indexedSink_ && !special && graphic == GRAPHIC_CODE && stacking == STACK_ABOVE &&
        indexedSink_->above(y, w, front, graphicLine_ + MARGIN, tlut, xrgb)) return;
    if (special) {
        const uint8_t* zero = zeroLine_ + MARGIN;
        composeSpecial(w, out, r2, graphic, front ? front : zero, back ? back : zero, stacking);
        return;
    }
    if (graphic == GRAPHIC_WORD) {
        // 65536 色。ワードの上位バイトと下位バイトでそれぞれパレットを引いた色を、XRGB にしたものの表を使う
        // (XRGB にする計算は色のビットごとに分かれているので、上位バイトの分と下位バイトの分の OR になる)
        const uint16_t* hi = video_.color64kHigh();
        const uint16_t* lo = video_.color64kLow();
        if (!wordTablesValid_ || !sameBytes(hi, wordHiSeen_, sizeof wordHiSeen_) ||
            !sameBytes(lo, wordLoSeen_, sizeof wordLoSeen_)) {
            memcpy(wordHiSeen_, hi, sizeof wordHiSeen_);
            memcpy(wordLoSeen_, lo, sizeof wordLoSeen_);
            for (int i = 0; i < 256; i++) {
                wordHiXrgb_[i] = paletteToRgb(hi[i]);
                wordLoXrgb_[i] = paletteToRgb(lo[i]);
            }
            wordTablesValid_ = true;
        }
        const uint16_t* word = wordLine_ + MARGIN;
        const uint32_t* th = wordHiXrgb_;
        const uint32_t* tl = wordLoXrgb_;
        composeLayers(out, w, stacking, front, back, tlut, [=](int x) { return th[word[x] >> 8] | tl[word[x] & 0xff]; });
        return;
    }
    const uint8_t* gcode = graphicLine_ + MARGIN;
    composeLayers(out, w, stacking, front, back, tlut, [=](int x) { return xrgb[gcode[x]]; });
}

} // namespace x68k
