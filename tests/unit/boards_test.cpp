// 拡張ボードのテスト: 数値演算プロセッサボード (CZ-6BP1 相当、x68k/fpu_board.h) と、まーきゅりーゆにっと (x68k/mercury.h)。
// 数値演算プロセッサボードは、68000 のソフトウェアが CIR を読み書きする手順 (MC68881UM 7.5) をそのまま
// FpuBoard に対して行う。最後に、テスト用の IPL ROM の 68000 の命令から $E9E000 を読み書きする。
// まーきゅりーゆにっとは、レジスタ、取り出す速さ、DMAC のチャネル 2 からの転送、音の合成を別のコアで行うとき
// (記録を当てる側) の音が 1 つのコアのときと同じになることを調べる。
#include <stdio.h>
#include <string.h>

#include <initializer_list>
#include <vector>

#include "x68k/fpu_board.h"
#include "x68k/machine.h"
#include "x68k/mercury.h"
#include "x68k/sound.h"
#include "x68k/sound_replay.h"
#include "x68k/state.h"
#include "x68k/video_journal.h"

namespace {

int failures = 0;

#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);              \
        if (va_ != vb_) {                                                                              \
            printf("  %s:%d: %s == %s: got %llx, want %llx\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            failures++;                                                                                \
        }                                                                                              \
    } while (0)

// ---------------------------------------------------------------- 数値演算プロセッサボード

// 68000 のソフトウェアから見た CIR (16 ビットの読み書き)
struct Cir {
    x68k::FpuBoard b;
    uint16_t resp() { return b.read16(0x00); }
    void control() { b.write16(0x02, 0); }
    uint16_t save() { return b.read16(0x04); }
    void restore(uint16_t f) { b.write16(0x06, f); }
    uint16_t restoreRead() { return b.read16(0x06); }
    void cmd(uint16_t w) { b.write16(0x0a, w); }
    void cond(uint16_t c) { b.write16(0x0e, c); }
    // 68000 の move.l は上位の語から書く
    void put(uint32_t v) {
        b.write16(0x10, (uint16_t)(v >> 16));
        b.write16(0x12, (uint16_t)v);
    }
    uint32_t get() {
        uint32_t hi = b.read16(0x10);
        return hi << 16 | b.read16(0x12);
    }
    uint16_t regSelect() { return b.read16(0x14); }
    m68k::fp::X& fp(int n) { return b.core().fp_[n]; }
};

// コマンドの語 (cpGEN の 2 語目)
uint16_t gen(int opclass, int m, int n, int opmode) { return (uint16_t)(opclass << 13 | m << 10 | n << 7 | opmode); }
enum { L = 0, S = 1, X = 2, P = 3, W = 4, D = 5, B = 6 };

// <ea>,FPn の手順 (FMOVE.L #v,FPn など): 評価と転送のプリミティブ、$8900、オペランド、$0900、$0802
void loadLong(Cir& c, int n, uint32_t v, int opmode = 0x00, uint16_t first = 0x9504) {
    c.cmd(gen(2, L, n, opmode));
    CHECK_EQ(c.resp(), first);
    CHECK_EQ(c.resp(), 0x8900);
    c.put(v);
    CHECK_EQ(c.resp(), 0x0900);
    CHECK_EQ(c.resp(), 0x0802);
}

// FMOVE.L FPn,<ea>: $8900、評価と転送のプリミティブ、$8900、オペランド、$0802
uint32_t storeLong(Cir& c, int n) {
    c.cmd(gen(3, L, n, 0));
    CHECK_EQ(c.resp(), 0x8900);
    CHECK_EQ(c.resp(), 0xb104);
    CHECK_EQ(c.resp(), 0x8900);
    uint32_t v = c.get();
    CHECK_EQ(c.resp(), 0x0802);
    return v;
}

void testFpuBasic() {
    printf("fpu board: reset, write-only registers, general instructions\n");
    Cir c;
    CHECK_EQ(c.resp(), 0x0802);
    CHECK_EQ(c.save(), 0x0018); // リセットの直後は NULL
    CHECK_EQ(c.b.read16(0x0a), 0xffff); // 書き込み専用
    CHECK_EQ(c.b.read16(0x1c), 0xffff);

    // FMOVE.L #3,FP0、FMOVE.L #4,FP1、FADD.X FP1,FP0、FMOVE.L FP0,<ea>
    loadLong(c, 0, 3);
    loadLong(c, 1, 4);
    c.cmd(gen(0, 1, 0, 0x22)); // FADD FP1,FP0 (レジスタどうし: $0900 のあと $0802)
    CHECK_EQ(c.resp(), 0x0900);
    CHECK_EQ(c.resp(), 0x0802);
    CHECK_EQ(storeLong(c, 0), 7);

    // FMOVE.D #1.5,FP2、FMUL.L #-2,FP2 → -3、FMOVE.D FP2,<ea>
    c.cmd(gen(2, D, 2, 0x00));
    CHECK_EQ(c.resp(), 0x9608);
    CHECK_EQ(c.resp(), 0x8900);
    c.put(0x3ff80000);
    c.put(0);
    CHECK_EQ(c.resp(), 0x0900);
    CHECK_EQ(c.resp(), 0x0802);
    loadLong(c, 2, (uint32_t)-2, 0x23);
    c.cmd(gen(3, D, 2, 0));
    CHECK_EQ(c.resp(), 0x8900);
    CHECK_EQ(c.resp(), 0xb208);
    CHECK_EQ(c.get(), 0xc0080000);
    CHECK_EQ(c.get(), 0);
    CHECK_EQ(c.resp(), 0x0802);

    // バイトのオペランドは 1 回 (上位バイト)。FMOVE.B #-2,FP3、FMOVE.B FP3,<ea>
    c.cmd(gen(2, B, 3, 0x00));
    CHECK_EQ(c.resp(), 0x9501);
    CHECK_EQ(c.resp(), 0x8900);
    c.b.write8(0x10, 0xfe);
    CHECK_EQ(c.resp(), 0x0900);
    CHECK_EQ(c.resp(), 0x0802);
    c.cmd(gen(3, B, 3, 0));
    CHECK_EQ(c.resp(), 0x8900);
    CHECK_EQ(c.resp(), 0xb101);
    CHECK_EQ(c.resp(), 0x8900);
    CHECK_EQ(c.b.read16(0x10), 0xfeff);
    CHECK_EQ(c.resp(), 0x0802);
    // ワード: FMOVE.W #300,FP3、FMOVE.W FP3,<ea>
    c.cmd(gen(2, W, 3, 0x00));
    CHECK_EQ(c.resp(), 0x9502);
    c.b.write16(0x10, 300);
    CHECK_EQ(c.resp(), 0x0900);
    c.cmd(gen(3, W, 3, 0));
    c.resp();
    CHECK_EQ(c.resp(), 0xb102);
    CHECK_EQ(c.b.read16(0x10), 300);
    CHECK_EQ(c.resp(), 0x0802);

    // FMOVECR #$32 (1.0),FP4 はレジスタどうしと同じ手順。FSQRT.X FP4 は 1、FMOVE.X FP4,<ea>
    c.cmd(gen(2, 7, 4, 0x32));
    CHECK_EQ(c.resp(), 0x0900);
    CHECK_EQ(c.resp(), 0x0802);
    CHECK_EQ(c.fp(4).se, 0x3fff);
    CHECK_EQ(c.fp(4).m, 0x8000000000000000ull);
    c.cmd(gen(3, X, 4, 0));
    c.resp();
    CHECK_EQ(c.resp(), 0xb20c);
    CHECK_EQ(c.get(), 0x3fff0000);
    CHECK_EQ(c.get(), 0x80000000);
    CHECK_EQ(c.get(), 0);
    CHECK_EQ(c.resp(), 0x0802);

    // 定義されていない命令 (opclass 001) は F ラインの例外
    c.cmd(0x2000);
    CHECK_EQ(c.resp(), 0x5c0b); // 命令の前の例外なので PC = 1
    c.control();
    CHECK_EQ(c.resp(), 0x0802);
    // 待っていないオペランドの読み出しはプロトコル違反。制御に書くと待機に戻る
    c.b.read16(0x10);
    CHECK_EQ(c.resp(), 0x1d0d);
    c.control();
    CHECK_EQ(c.resp(), 0x0802);
}

void testFpuControlAndMovem() {
    printf("fpu board: control registers and FMOVEM\n");
    Cir c;
    // FMOVEM.L #imm,FPCR/FPSR/FPIAR (3 個): $960C。FPCR の下位 4 ビットなどは 0 になる
    c.cmd(gen(4, 7, 0, 0));
    CHECK_EQ(c.resp(), 0x960c);
    CHECK_EQ(c.resp(), 0x8900);
    c.put(0x0000003f);
    c.put(0x0f00ff07);
    c.put(0x00fe0100);
    CHECK_EQ(c.resp(), 0x0802);
    c.cmd(gen(5, 7, 0, 0));
    CHECK_EQ(c.resp(), 0xb20c);
    CHECK_EQ(c.resp(), 0x8900);
    CHECK_EQ(c.get(), 0x00000030);
    CHECK_EQ(c.get(), 0x0f00ff00);
    CHECK_EQ(c.get(), 0x00fe0100);
    CHECK_EQ(c.resp(), 0x0802);
    // 1 個: FPIAR は $9704 / $B304、FPCR は $9504 / $B104、2 個は $9608 / $B208
    c.cmd(gen(4, 1, 0, 0));
    CHECK_EQ(c.resp(), 0x9704);
    c.put(0x12345678);
    CHECK_EQ(c.resp(), 0x0802);
    c.cmd(gen(5, 1, 0, 0));
    CHECK_EQ(c.resp(), 0xb304);
    CHECK_EQ(c.get(), 0x12345678);
    c.cmd(gen(4, 4, 0, 0));
    CHECK_EQ(c.resp(), 0x9504);
    c.put(0);
    c.cmd(gen(5, 4, 0, 0));
    CHECK_EQ(c.resp(), 0xb104);
    CHECK_EQ(c.get(), 0);
    c.cmd(gen(5, 6, 0, 0));
    CHECK_EQ(c.resp(), 0xb208);
    c.get();
    c.get();
    CHECK_EQ(c.resp(), 0x0802);
    // 命令アドレス CIR は FPIAR に入る
    c.b.write16(0x18, 0x00ff);
    c.b.write16(0x1a, 0x1234);
    CHECK_EQ(c.b.core().fpiar_, 0x00ff1234);

    loadLong(c, 0, 10);
    loadLong(c, 2, 20);
    loadLong(c, 7, 70);
    // FMOVEM.X FP0/FP2,<ea> (ポストインクリメント / 制御: bit7 が FP0): $A10C、レジスタ選択、FP0、FP2 の順
    c.cmd(0xe000 | 0x1000 | 0xa0);
    CHECK_EQ(c.resp(), 0xa10c);
    CHECK_EQ(c.resp(), 0x8900);
    CHECK_EQ(c.regSelect(), 0xa000);
    uint32_t r[6];
    for (uint32_t& v : r) v = c.get();
    CHECK_EQ(r[0], 0x40020000); // 10 = 1.25 x 2^3
    CHECK_EQ(r[1], 0xa0000000);
    CHECK_EQ(r[3], 0x40030000); // 20
    CHECK_EQ(c.resp(), 0x0802);
    // プレデクリメント (bit7 が FP7): リスト $81 は FP7、FP0 の順
    c.cmd(0xe000 | 0x81);
    CHECK_EQ(c.resp(), 0xa10c);
    CHECK_EQ(c.regSelect(), 0x8100);
    CHECK_EQ(c.get(), 0x40050000); // 70 = 1.09375 x 2^6
    c.get();
    c.get();
    CHECK_EQ(c.get(), 0x40020000);
    c.get();
    c.get();
    CHECK_EQ(c.resp(), 0x0802);
    // 動的なリスト: FMOVEM.X D3,<ea> から FP5 へ (メモリから)。$8C03 で D3 を受け取り、$810C
    c.cmd(0xc000 | 0x1000 | 0x0800 | 3 << 4);
    CHECK_EQ(c.resp(), 0x8c03);
    CHECK_EQ(c.resp(), 0x8900);
    c.put(0x04); // ポストインクリメントの bit2 = FP5
    CHECK_EQ(c.resp(), 0x810c);
    CHECK_EQ(c.regSelect(), 0x0400);
    c.put(0x7fff0000); // シグナリング NaN もそのまま入る
    c.put(0x00000000);
    c.put(0x00000001);
    CHECK_EQ(c.resp(), 0x0802);
    CHECK_EQ(c.fp(5).se, 0x7fff);
    CHECK_EQ(c.fp(5).m, 1);
    // リストが空なら、レジスタ選択を読んだら終わり
    c.cmd(0xc000 | 0x1000);
    CHECK_EQ(c.resp(), 0x810c);
    CHECK_EQ(c.regSelect(), 0x0000);
    CHECK_EQ(c.resp(), 0x0802);
}

void testFpuCondition() {
    printf("fpu board: conditions and BSUN\n");
    Cir c;
    loadLong(c, 0, 0);
    c.cmd(gen(0, 0, 0, 0x3a)); // FTST FP0 (0)
    c.resp();
    c.resp();
    c.cond(0x01); // EQ: 成立 (TF = 1)
    CHECK_EQ(c.resp(), 0x0801);
    CHECK_EQ(c.resp(), 0x0802);
    c.cond(0x0e); // NE: 不成立
    CHECK_EQ(c.resp(), 0x0800);
    // NaN にして、BSUN を起こす条件 (GT = $12) を調べる。許可していなければ BSUN を立てるだけ
    c.cmd(gen(2, S, 1, 0x00));
    c.resp();
    c.put(0x7fc00000);
    c.resp();
    c.cmd(gen(0, 1, 1, 0x3a)); // FTST FP1 (NaN)
    c.resp();
    c.cond(0x12);
    CHECK_EQ(c.resp(), 0x0800);
    CHECK_EQ(c.b.core().fpsr_ & 0x8080, 0x8080); // BSUN と AEXC の IOP
    c.cond(0x08); // UN: 成立 (BSUN は起こさない)
    CHECK_EQ(c.resp(), 0x0801);
    // BSUN を許可すると、命令の前の例外 $5C30
    c.cmd(gen(4, 4, 0, 0));
    c.resp();
    c.put(0x8000);
    c.cond(0x12);
    CHECK_EQ(c.resp(), 0x5c30);
    c.control();
    c.cond(0x20); // 条件は 6 ビットまで
    CHECK_EQ(c.resp(), 0x5c0b); // 命令の前の例外なので PC = 1
}

void testFpuExceptions() {
    printf("fpu board: arithmetic exceptions, FSAVE / FRESTORE\n");
    Cir c;
    loadLong(c, 0, 5);
    loadLong(c, 6, 0);
    // DZ を許可して FDIV FP6,FP0。PC を求める ($4900)。FP0 は変えず、次のコマンドで $1C32
    c.cmd(gen(4, 4, 0, 0));
    c.resp();
    c.put(0x0400);
    c.cmd(gen(0, 6, 0, 0x20));
    CHECK_EQ(c.resp(), 0x4900);
    CHECK_EQ(c.resp(), 0x0802);
    CHECK_EQ(c.fp(0).se, 0x4001); // 5 のまま
    // ここで FSAVE: IDLE のフレームの BIU フラグ (最初に送るロング) の bit27 が 0 (保留あり)
    CHECK_EQ(c.save(), 0x1f18);
    uint32_t frame[6];
    for (int i = 5; i >= 0; i--) frame[i] = c.get(); // 高いアドレスから
    CHECK_EQ(frame[5] & 0x08000000, 0);
    CHECK_EQ(frame[0] >> 16, gen(0, 6, 0, 0x20));
    // 保留はフレームに移ったので、次のコマンドは実行される (FPSR の例外ステータスも消える)
    const uint32_t savedFpsr = c.b.core().fpsr_; // 例外ハンドラが FMOVEM で保存した FPSR の代わり
    CHECK_EQ(savedFpsr & 0x0400, 0x0400);
    loadLong(c, 1, 1, 0x00, 0xd504);
    // FRESTORE で戻し、FPSR も戻すと、また保留になる (例外の種類は FPCR の ENABLE と FPSR の EXC で決まる)
    c.restore(0x1f18);
    CHECK_EQ(c.restoreRead(), 0x1f18);
    for (int i = 0; i < 6; i++) c.put(frame[i]); // 低いアドレスから
    c.cmd(gen(4, 2, 0, 0)); // FMOVE.L <ea>,FPSR (保留していても知らせずに実行する)
    CHECK_EQ(c.resp(), 0x9504);
    c.put(savedFpsr);
    CHECK_EQ(c.resp(), 0x0802);
    // FMOVE.L FPSR,<ea> と FMOVEM も知らせずに実行する (MC68881UM 6.4.2.2、7.5.4.1)
    c.cmd(gen(5, 2, 0, 0));
    CHECK_EQ(c.resp(), 0xb104);
    CHECK_EQ(c.get(), savedFpsr);
    CHECK_EQ(c.resp(), 0x0802);
    c.cmd(0xf000 | 0x80); // FMOVEM.X FP0,<ea> (ポストインクリメントの形、静的なリスト)
    CHECK_EQ(c.resp(), 0xa10c);
    CHECK_EQ(c.regSelect(), 0x8000);
    c.get();
    c.get();
    c.get();
    CHECK_EQ(c.resp(), 0x0802);
    c.cmd(gen(2, L, 1, 0));
    CHECK_EQ(c.resp(), 0x1c32);
    c.control();
    // 同じフレームを、FPCR と FPSR を INEX2 に変えて戻すと、INEX の例外 ($1C31) になる
    c.restore(0x1f18);
    for (int i = 0; i < 6; i++) c.put(frame[i]);
    c.cmd(gen(4, 4, 0, 0)); // FPCR
    c.resp();
    c.put(0x0200);
    c.cmd(gen(4, 2, 0, 0)); // FPSR
    c.resp();
    c.put(0x0200);
    c.cmd(gen(2, L, 1, 0));
    CHECK_EQ(c.resp(), 0x1c31);
    c.control();
    c.cmd(gen(4, 4, 0, 0));
    c.resp();
    c.put(0x0400); // 下の試験のため DZ の許可に戻す
    c.control(); // 例外の受け付け
    CHECK_EQ(c.resp(), 0x0802);
    c.cmd(gen(2, L, 1, 0));
    CHECK_EQ(c.resp(), 0xd504); // 例外を許可しているので PC を求める
    c.put(9);
    CHECK_EQ(c.resp(), 0x0900);

    // FMOVE.L FP2,<ea> で INEX2 を許可: $C900 (PC)、送り終えると命令の途中の例外 $1D31
    c.cmd(gen(4, 4, 0, 0));
    c.resp();
    c.put(0x0200);
    c.cmd(gen(2, D, 2, 0));
    c.resp();
    c.put(0x3ff80000); // 1.5
    c.put(0);
    c.resp();
    c.cmd(gen(3, L, 2, 0));
    CHECK_EQ(c.resp(), 0xc900);
    CHECK_EQ(c.resp(), 0xb104);
    CHECK_EQ(c.get(), 2);
    CHECK_EQ(c.resp(), 0x1d31);
    c.control();
    CHECK_EQ(c.resp(), 0x0802);

    // 正しくないフォーマット語は $0218。NULL はリセット (以後の FSAVE は NULL)
    c.restore(0x1238);
    CHECK_EQ(c.restoreRead(), 0x0218);
    c.restore(0x0000);
    CHECK_EQ(c.restoreRead(), 0x0000);
    CHECK_EQ(c.save(), 0x0018);
    CHECK_EQ(c.fp(0).se, 0x7fff);
    CHECK_EQ(c.b.core().fpcr_, 0);
}

// 例外を知らせたあとの FSAVE には例外の状態が残る。ハンドラが bit27 を立てて FRESTORE すると、もう知らせない
// (MC68881UM 6.4.2.2)
void testFpuHandlerFrame() {
    printf("fpu board: FSAVE in the exception handler\n");
    Cir c;
    loadLong(c, 0, 5);
    loadLong(c, 6, 0);
    c.cmd(gen(4, 4, 0, 0)); // DZ を許可
    c.resp();
    c.put(0x0400);
    c.cmd(gen(0, 6, 0, 0x20)); // FDIV FP6,FP0 (保留)
    c.resp();
    c.resp();
    c.cmd(gen(0, 6, 0, 0x22));
    CHECK_EQ(c.resp(), 0x1c32);
    c.control(); // 例外の受け付け
    // ハンドラ: FSAVE のフレームは保留ありのまま
    CHECK_EQ(c.save(), 0x1f18);
    uint32_t frame[6];
    for (int i = 5; i >= 0; i--) frame[i] = c.get();
    CHECK_EQ(frame[5] & 0x08000000, 0);
    CHECK_EQ(frame[0] >> 16, gen(0, 6, 0, 0x20));
    // bit27 を立てて戻すと、次の演算は実行される
    frame[5] |= 0x08000000;
    c.restore(0x1f18);
    for (int i = 0; i < 6; i++) c.put(frame[i]);
    c.cmd(gen(0, 0, 1, 0x00)); // FMOVE FP0,FP1
    CHECK_EQ(c.resp(), 0x4900);
    CHECK_EQ(c.resp(), 0x0802);
    // 新しい演算を始めたあとの FSAVE は、保留なしのフレーム
    CHECK_EQ(c.save(), 0x1f18);
    for (int i = 5; i >= 0; i--) frame[i] = c.get();
    CHECK_EQ(frame[5] & 0x08000000, 0x08000000);
}

// 応答とコマンドのバイトの読み書き。偶数番地は 16 ビットと同じく進み、奇数番地の応答は今の値を見るだけ
void testFpuBytes() {
    printf("fpu board: byte access to the response and command CIRs\n");
    Cir c;
    CHECK_EQ(c.b.read8(0x01), 0x02); // $0802 の下位バイト
    c.b.write8(0x0a, 0x00);          // コマンド $0000 (FMOVE FP0,FP0)
    CHECK_EQ(c.b.read8(0x01), 0x00); // $0900 の下位バイト (進まない)
    CHECK_EQ(c.b.read8(0x01), 0x00);
    CHECK_EQ(c.b.read8(0x00), 0x09); // 上位バイトを読むと次の応答へ
    CHECK_EQ(c.resp(), 0x0802);
    // 定義されていない命令 (opclass 001) を上位バイトだけ書く: $2020
    c.b.write8(0x0a, 0x20);
    CHECK_EQ(c.b.read8(0x00), 0x5c);
    CHECK_EQ(c.b.read8(0x01), 0x0b);
    c.b.write8(0x02, 0x00); // 制御: 待機に戻る
    CHECK_EQ(c.resp(), 0x0802);
}

void testFpuBusyFrame() {
    printf("fpu board: FSAVE in the middle of an instruction (busy frame)\n");
    Cir c;
    loadLong(c, 0, 1);
    // FADD.D #2.0,FP0 の途中 (1 ロングだけ送った) で FSAVE / FRESTORE し、残りを送る
    c.cmd(gen(2, D, 0, 0x22));
    CHECK_EQ(c.resp(), 0x9608);
    c.put(0x40000000);
    CHECK_EQ(c.save(), 0x1fb4);
    uint32_t frame[45];
    for (int i = 44; i >= 0; i--) frame[i] = c.get();
    CHECK_EQ(c.resp(), 0x0802);
    loadLong(c, 3, 33); // ほかの命令
    c.restore(0x1fb4);
    CHECK_EQ(c.restoreRead(), 0x1fb4);
    for (uint32_t v : frame) c.put(v);
    c.put(0);
    CHECK_EQ(c.resp(), 0x0900);
    CHECK_EQ(storeLong(c, 0), 3);
    CHECK_EQ(storeLong(c, 3), 33);

    // 例外を保留したまま FMOVEM.L FPCR/FPSR,<ea> を始め、その途中で FSAVE / FRESTORE しても、保留は残る
    Cir d;
    loadLong(d, 0, 5);
    loadLong(d, 6, 0);
    d.cmd(gen(4, 4, 0, 0)); // DZ を許可
    d.resp();
    d.put(0x0400);
    d.cmd(gen(0, 6, 0, 0x20)); // FDIV FP6,FP0 (DZ を保留)
    d.resp();
    d.resp();
    d.cmd(gen(5, 6, 0, 0)); // FPCR と FPSR を出す (保留していても始める)
    CHECK_EQ(d.resp(), 0xb208);
    CHECK_EQ(d.get(), 0x0400u); // FPCR
    CHECK_EQ(d.save(), 0x1fb4);
    for (int i = 44; i >= 0; i--) frame[i] = d.get();
    CHECK_EQ(frame[44] & 0x08000000, 0); // BIU フラグ: 保留あり
    d.restore(0x1fb4);
    for (uint32_t v : frame) d.put(v);
    CHECK_EQ(d.get() & 0x0400, 0x0400u); // 残りの FPSR (DZ)
    CHECK_EQ(d.resp(), 0x0802);
    d.cmd(gen(0, 0, 1, 0x00));
    CHECK_EQ(d.resp(), 0x1c32);
}

void testFpuPacked() {
    printf("fpu board: packed decimal with a dynamic k factor\n");
    Cir c;
    loadLong(c, 0, 1234);
    // 静的な k (k = 2) と、D1 で渡す動的な k (2) は同じ結果
    c.cmd(gen(3, P, 0, 2));
    c.resp();
    CHECK_EQ(c.resp(), 0xb20c);
    uint32_t a[3] = {c.get(), c.get(), c.get()};
    CHECK_EQ(c.resp(), 0x0802);
    c.cmd(gen(3, 7, 0, 1 << 4));
    CHECK_EQ(c.resp(), 0x8c01);
    CHECK_EQ(c.resp(), 0x8900);
    c.put(2);
    CHECK_EQ(c.resp(), 0xb20c);
    CHECK_EQ(c.resp(), 0x8900);
    uint32_t b[3] = {c.get(), c.get(), c.get()};
    CHECK_EQ(c.resp(), 0x0802);
    for (int i = 0; i < 3; i++) CHECK_EQ(a[i], b[i]);
    CHECK_EQ(a[0], 0x00030001); // 1.2E+3 (k = 2 で 2 桁)
    CHECK_EQ(a[1], 0x20000000);
}

// ---------------------------------------------------------------- 68000 から $E9E000 を読み書きする

struct Rom {
    uint8_t b[x68k::Machine::IPL_SIZE];
    uint32_t pos = 0x100;
    Rom() {
        memset(b, 0xff, sizeof b);
        put32(0x10000, 0x00002000);
        put32(0x10004, 0x00fe0100);
        put16(0x0f00, 0x7e01); // バスエラー: moveq #1,d7
        put16(0x0f02, 0x60fe);
        code({0x21fc, 0x00fe, 0x0f00, 0x0008}); // move.l #$fe0f00,$8.w
    }
    void put16(uint32_t o, uint16_t v) {
        b[o] = (uint8_t)(v >> 8);
        b[o + 1] = (uint8_t)v;
    }
    void put32(uint32_t o, uint32_t v) {
        put16(o, (uint16_t)(v >> 16));
        put16(o + 2, (uint16_t)v);
    }
    void code(std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) {
            put16(pos, w);
            pos += 2;
        }
    }
};

m68k::Registers runRom(x68k::Machine& m, Rom& rom, int64_t cycles = 20000) {
    rom.code({0x7c01, 0x60fe}); // moveq #1,d6; bra.s *
    m.setIplRom(rom.b, sizeof rom.b);
    m.powerOn();
    m.run(cycles);
    m68k::Registers r;
    m.getRegisters(r);
    return r;
}

void testFpuFrom68000() {
    printf("fpu board: driven by 68000 code at $E9E000\n");
    // FMOVE.L #5,FP0; FMUL.L #6,FP0; FMOVE.L FP0,D4
    Rom rom;
    rom.code({
        0x33fc, 0x4000, 0x00e9, 0xe00a, // move.w #$4000,$e9e00a
        0x3039, 0x00e9, 0xe000,         // move.w $e9e000,d0     ($9504)
        0x3239, 0x00e9, 0xe000,         // move.w $e9e000,d1     ($8900)
        0x23fc, 0x0000, 0x0005, 0x00e9, 0xe010, // move.l #5,$e9e010
        0x3439, 0x00e9, 0xe000,         // move.w $e9e000,d2     ($0900)
        0x4a79, 0x00e9, 0xe000,         // tst.w $e9e000         ($0802)
        0x33fc, 0x40a3, 0x00e9, 0xe00a, // move.w #$40a3,$e9e00a (FMUL.L #imm,FP1 ... FP1 は NaN)
        0x4a79, 0x00e9, 0xe000,
        0x23fc, 0x0000, 0x0006, 0x00e9, 0xe010,
        0x4a79, 0x00e9, 0xe000,
        0x33fc, 0x0023, 0x00e9, 0xe00a, // move.w #$0023,$e9e00a (FMUL.X FP0,FP0)
        0x4a79, 0x00e9, 0xe000,
        0x4a79, 0x00e9, 0xe000,
        0x33fc, 0x6000, 0x00e9, 0xe00a, // move.w #$6000,$e9e00a (FMOVE.L FP0,<ea>)
        0x4a79, 0x00e9, 0xe000,         // $8900
        0x3639, 0x00e9, 0xe000,         // move.w $e9e000,d3     ($B104)
        0x2839, 0x00e9, 0xe010,         // move.l $e9e010,d4
        0x3a39, 0x00e9, 0xe000,         // move.w $e9e000,d5     ($0802)
    });
    x68k::Machine m;
    CHECK_EQ(m.setFpuBoard(true), true);
    m68k::Registers r = runRom(m, rom);
    CHECK_EQ(r.d[6], 1);
    CHECK_EQ(r.d[7], 0);
    CHECK_EQ(r.d[0] & 0xffff, 0x9504);
    CHECK_EQ(r.d[1] & 0xffff, 0x8900);
    CHECK_EQ(r.d[2] & 0xffff, 0x0900);
    CHECK_EQ(r.d[3] & 0xffff, 0xb104);
    CHECK_EQ(r.d[4], 25);
    CHECK_EQ(r.d[5] & 0xffff, 0x0802);

    // ボードがなければバスエラー
    Rom rom2;
    rom2.code({0x3039, 0x00e9, 0xe000});
    x68k::Machine m2;
    r = runRom(m2, rom2);
    CHECK_EQ(r.d[7], 1);
    // $E9E020 から先は、ボードがあってもバスエラー
    Rom rom3;
    rom3.code({0x1039, 0x00e9, 0xe020});
    x68k::Machine m3;
    m3.setFpuBoard(true);
    r = runRom(m3, rom3);
    CHECK_EQ(r.d[7], 1);
}

// 2 枚目のボード ($E9E080-$E9E09F)。1 枚目と別の MC68881 として動き、CIR の外はバスエラー
void testFpuSecondBoard() {
    printf("fpu board: second board at $E9E080\n");
    // 1 枚目と 2 枚目に別の値を入れ (FMOVE.L #imm,FP0)、続けて FMOVE.L FP0,<ea> で読み出す
    auto load = [](Rom& rom, uint16_t base, uint16_t v) {
        rom.code({
            0x33fc, 0x4000, 0x00e9, (uint16_t)(base + 0x0a), // move.w #$4000,cmd (FMOVE.L <ea>,FP0)
            0x4a79, 0x00e9, base,                            // tst.w resp ($9504)
            0x4a79, 0x00e9, base,                            // tst.w resp ($8900)
            0x23fc, 0x0000, v, 0x00e9, (uint16_t)(base + 0x10), // move.l #v,operand
            0x4a79, 0x00e9, base,                            // tst.w resp ($0900)
            0x4a79, 0x00e9, base,                            // tst.w resp ($0802)
        });
    };
    auto store = [](Rom& rom, uint16_t base, uint16_t dreg) {
        rom.code({
            0x33fc, 0x6000, 0x00e9, (uint16_t)(base + 0x0a), // move.w #$6000,cmd (FMOVE.L FP0,<ea>)
            0x4a79, 0x00e9, base,                            // tst.w resp ($8900)
            0x4a79, 0x00e9, base,                            // tst.w resp ($B104)
            (uint16_t)(0x2039 | dreg << 9), 0x00e9, (uint16_t)(base + 0x10), // move.l operand,Dn
            0x4a79, 0x00e9, base,                            // tst.w resp ($0802)
        });
    };
    int ran = 0;
    for (int model : {68000, 68030, 68060}) {
        x68k::Config config;
        if (!config.setCpu(model)) continue; // 68030 / 68060 のコアが入っていないビルド
        ran++;
        Rom rom;
        load(rom, 0xe000, 5);
        load(rom, 0xe080, 7);
        store(rom, 0xe080, 4); // d4 = 7 (2 枚目)
        store(rom, 0xe000, 3); // d3 = 5 (1 枚目)
        rom.code({0x1a39, 0x00e9, 0xe080}); // move.b $e9e080,d5 (応答の上位バイト。読んでも進まない)
        rom.code({0x1239, 0x00e9, 0xe081}); // move.b $e9e081,d1 (応答の下位バイト)
        x68k::Machine m(config);
        CHECK_EQ(m.setFpuBoards(2), true);
        CHECK_EQ(m.fpuBoardCount(), 2);
        m68k::Registers r = runRom(m, rom, model == 68000 ? 20000 : 40000);
        CHECK_EQ(r.d[6], 1);
        CHECK_EQ(r.d[7], 0);
        CHECK_EQ(r.d[3], 5);
        CHECK_EQ(r.d[4], 7);
        CHECK_EQ(r.d[5] & 0xff, 0x08);
        CHECK_EQ(r.d[1] & 0xff, 0x02);
        // 2 つのボードの中身は別
        CHECK_EQ(m.fpuBoard(0) != m.fpuBoard(1), true);
        CHECK_EQ(m.fpuBoard(0)->core().fp_[0].m, 0xa000000000000000ull); // 5.0
        CHECK_EQ(m.fpuBoard(1)->core().fp_[0].m, 0xe000000000000000ull); // 7.0
        CHECK_EQ(m.fpuBoard(0)->response(), 0x0802);
        CHECK_EQ(m.fpuBoard(1)->response(), 0x0802);
        // 片方の制御レジスタに書いても (手順をやめる)、もう片方の手順は続く
        m.fpuBoard(1)->write16(0x0a, 0x4000);
        CHECK_EQ(m.fpuBoard(1)->response(), 0x9504);
        m.fpuBoard(0)->write16(0x02, 0);
        CHECK_EQ(m.fpuBoard(1)->response(), 0x9504);
    }
    CHECK_EQ(ran, x68k::Config::has020() ? 3 : 1);

    // バイトの書き込みも 2 枚目に届く: 両方に FMOVE.L <ea>,FP0 を書いてオペランドを待たせ、2 枚目の制御 ($E9E083) に
    // move.b で書くと、2 枚目だけが待機に戻る
    {
        Rom rom;
        rom.code({
            0x33fc, 0x4000, 0x00e9, 0xe00a, // move.w #$4000,$e9e00a
            0x33fc, 0x4000, 0x00e9, 0xe08a, // move.w #$4000,$e9e08a
            0x13fc, 0x0000, 0x00e9, 0xe083, // move.b #0,$e9e083 (2 枚目の制御)
        });
        x68k::Machine m;
        CHECK_EQ(m.setFpuBoards(2), true);
        m68k::Registers r = runRom(m, rom);
        CHECK_EQ(r.d[7], 0);
        CHECK_EQ(m.fpuBoard(0)->response(), 0x9504);
        CHECK_EQ(m.fpuBoard(1)->response(), 0x0802);
    }

    // CIR の外とつないでいないボードはバスエラー (d7 = 1)
    struct Case {
        int boards;
        uint16_t op, lo; // move.b / move.w のどちらか ($E9xxxx から読む)
        int busError;
    };
    const Case cases[] = {
        {2, 0x1039, 0xe020, 1}, // move.b $e9e020,d0: 1 枚目の CIR の次
        {2, 0x3039, 0xe07e, 1}, // move.w $e9e07e,d0: 2 枚目の CIR の手前
        {2, 0x1039, 0xe0a0, 1}, // move.b $e9e0a0,d0: 2 枚目の CIR の次
        {2, 0x3039, 0xe0a0, 1}, // move.w $e9e0a0,d0
        {2, 0x3039, 0xe100, 1}, // move.w $e9e100,d0: $80 ごとに繰り返して見えはしない
        {2, 0x3039, 0xe09e, 0}, // move.w $e9e09e,d0: 2 枚目の CIR の最後の語
        {1, 0x3039, 0xe080, 1}, // 1 枚なら $E9E080 はバスエラー (今までと同じ)
        {1, 0x1039, 0xe081, 1},
        {1, 0x3039, 0xe000, 0},
    };
    for (const Case& c : cases) {
        Rom rom;
        rom.code({c.op, 0x00e9, c.lo});
        x68k::Machine m;
        CHECK_EQ(m.setFpuBoards(c.boards), true);
        m68k::Registers r = runRom(m, rom);
        if ((int)r.d[7] != c.busError) printf("  at $E9%04X with %d board(s)\n", c.lo, c.boards);
        CHECK_EQ((int)r.d[7], c.busError);
    }

    // 枚数の切り替え
    x68k::Machine m;
    CHECK_EQ(m.fpuBoardCount(), 0);
    CHECK_EQ(m.setFpuBoards(3), false);
    CHECK_EQ(m.setFpuBoards(-1), false);
    CHECK_EQ(m.fpuBoardCount(), 0);
    CHECK_EQ(m.setFpuBoards(2), true);
    x68k::FpuBoard* first = m.fpuBoard();
    CHECK_EQ(m.setFpuBoards(1), true); // 2 枚目だけを外す。1 枚目はそのまま
    CHECK_EQ(m.fpuBoardCount(), 1);
    CHECK_EQ(m.fpuBoard() == first, true);
    CHECK_EQ(m.fpuBoard(1) == nullptr, true);
    CHECK_EQ(m.setFpuBoard(false), true);
    CHECK_EQ(m.fpuBoardCount(), 0);
    CHECK_EQ(m.fpuBoard(2) == nullptr, true);
}

// ---------------------------------------------------------------- まーきゅりーゆにっと

void testMercuryRegisters() {
    printf("mercury: registers and sample rate\n");
    x68k::Mercury u;
    CHECK_EQ(u.rate(), 22050); // コマンド 0: 44.1kHz の半分
    u.writeCommand(0x80 | 0x30 | 0x0e);
    CHECK_EQ(u.rate(), 48000);
    CHECK_EQ(u.read(0x91), 0xbe);
    CHECK_EQ(u.read(0xa1), 0xbf);
    u.writeCommand(0x90);
    CHECK_EQ(u.rate(), 32000);
    u.writeCommand(0x20);
    CHECK_EQ(u.rate(), 22050);
    CHECK_EQ(u.read(0x80), 0xff);
    // ステレオでバッファがいっぱいのときに書いた語は捨て、L / R の順も進めない
    {
        x68k::Mercury s;
        s.writeCommand(0x80 | 0x30 | 0x0e); // 48kHz、L と R、ステレオ
        for (int i = 0; i < x68k::Mercury::FIFO; i++) s.writeData((uint16_t)(i & 1 ? 0x2000 : 0x1000)); // L、R、...
        CHECK_EQ(s.drq(), false);
        s.writeData(0x7777); // 捨てる
        s.advance(4000000 / 48000 + 1);      // 1 サンプル取り出す
        CHECK_EQ(s.left(), 0x1000);
        CHECK_EQ(s.right(), 0x2000);
        s.writeData(0x1100); // 空いた 2 語に L、R
        s.writeData(0x2200);
        for (int i = 0; i < x68k::Mercury::FIFO / 2; i++) s.advance(4000000 / 48000 + 1);
        CHECK_EQ(s.left(), 0x1100);
        CHECK_EQ(s.right(), 0x2200);
    }
    // $ECC090: EXREQ (bit2) は空きがあれば 0。あふれるまで書くと 1
    u.writeCommand(0x80 | 0x30 | 0x0e); // 48kHz ステレオ
    u.writeMode(0x02);
    CHECK_EQ(u.read(0x90), 0xf8 | 0x02); // PCS = 1 (1 サンプルの前半)、要求あり
    CHECK_EQ(u.read(0x90, 4000000 / 48000 / 2 + 1) & 0x08, 0); // 後半は 0 (EXPCL と同じ)
    CHECK_EQ(u.read(0x90, 4000000 / 48000 / 2 + 1) & 0x08, u.expcl(4000000 / 48000 / 2 + 1) ? 0x08 : 0);
    u.writeData(0x1000);
    for (int i = 1; i < x68k::Mercury::FIFO; i++) u.writeData((uint16_t)(0x1000 + i));
    CHECK_EQ(u.drq(), false);
    CHECK_EQ(u.read(0x90) & 0x04, 0x04);
    // 1 サンプル (L と R) を取り出す間隔: 4MHz / 48kHz = 83.3 クロック
    u.advance(64);
    CHECK_EQ(u.drq(), false);
    CHECK_EQ(u.left(), 0);
    u.advance(64);
    CHECK_EQ(u.drq(), true);
    CHECK_EQ(u.left(), 0x1000);
    CHECK_EQ(u.right(), 0x1001);
    // L だけ出す
    u.writeCommand(0x80 | 0x30 | 0x06);
    CHECK_EQ(u.left(), 0x1000);
    CHECK_EQ(u.right(), 0);
    // モノラル: 1 語を L と R に
    u.reset();
    u.writeCommand(0x80 | 0x30 | 0x0c);
    u.writeData(0x8000);
    u.advance(128);
    CHECK_EQ(u.left(), -32768);
    CHECK_EQ(u.right(), -32768);
    // データがなくなったら前の値のまま
    u.advance(4000);
    CHECK_EQ(u.left(), -32768);
}

// DMAC のチャネル 2 でメモリから $ECC080 に送る。転送の速さがサンプリング周波数の 2 倍 (ステレオ) になる
void testMercuryDma() {
    printf("mercury: DMAC channel 2\n");
    // ボードがなければ $ECC080 はバスエラー
    {
        x68k::Machine m0;
        Rom rom0;
        rom0.code({0x4a39, 0x00ec, 0xc091});
        m68k::Registers r0 = runRom(m0, rom0);
        CHECK_EQ(r0.d[7], 1);
    }
    x68k::Machine m;
    CHECK_EQ(m.setMercury(true), true);
    Rom rom;
    rom.code({
        0x13fc, 0x00be, 0x00ec, 0xc091, // move.b #$be,$ecc091 (48kHz、等倍、L と R、ステレオ)
        0x1039, 0x00ec, 0xc0a1,         // move.b $ecc0a1,d0
        0x1239, 0x00ec, 0xc0c1,         // move.b $ecc0c1,d1 (OPN は作っていない: $FF)
    });
    m68k::Registers r = runRom(m, rom);
    CHECK_EQ(r.d[7], 0);
    CHECK_EQ(r.d[6], 1);
    CHECK_EQ(r.d[0] & 0xff, 0xbf);
    CHECK_EQ(r.d[1] & 0xff, 0xff);
    // チャネル 2: メモリ ($10000 から) → デバイス ($ECC080)、ワード、16 ビットポート、外部要求
    const int fc = m68k::FC_SUPER_DATA;
    const uint32_t ch = 0xe84080;
    const uint32_t words = 4000;
    for (uint32_t i = 0; i < words; i++) m.write16(0x10000 + 2 * i, (uint16_t)(i * 7), fc);
    m.write8(ch + 0x00, 0xff, fc); // CSR を消す
    m.write8(ch + 0x04, 0x08, fc); // DCR: 16 ビットポート
    m.write8(ch + 0x05, 0x12, fc); // OCR: メモリ → デバイス、ワード、外部要求
    m.write8(ch + 0x06, 0x04, fc); // SCR: MAR を増やす
    m.write8(ch + 0x29, 0x05, fc); // MFC
    m.write8(ch + 0x31, 0x05, fc); // DFC
    m.write16(ch + 0x0a, (uint16_t)words, fc);
    m.write16(ch + 0x0c, 0x0001, fc);
    m.write16(ch + 0x0e, 0x0000, fc);
    m.write16(ch + 0x14, 0x00ec, fc);
    m.write16(ch + 0x16, 0xc080, fc);
    m.write8(ch + 0x07, 0x80, fc); // CCR: 始める
    // 10ms 動かすと、48kHz のステレオで 960 語を取り出す (それにバッファの分を足した数を送る)
    m.run(160000);
    const uint32_t left = m.read16(ch + 0x0a, fc);
    const uint32_t sent = words - left;
    if (sent < 960 || sent > 960 + x68k::Mercury::FIFO + 4) {
        printf("  sent %u words in 10ms, want about 960\n", sent);
        failures++;
    }
}

// DMAC のチャネル 2 の CSR の PCS (bit0) に、まーきゅりーゆにっとの L / R のクロック (EXPCL) が見える。
// PCM8PP は PCS が 1 から 0 に変わるのを待ってからコマンドを書く (変わらないと止まる)
void testMercuryPcl() {
    printf("mercury: EXPCL on DMAC channel 2 PCS (rates, CPUs, state)\n");
    const int fc = m68k::FC_SUPER_DATA;
    // CSR を 40 サイクルごとに読み、変わった回数と 1 から 0 への変わり目の数を数える。読んだ値は bits に残す
    auto countEdges = [&](x68k::Machine& m, uint64_t cycles, int& falls, std::vector<uint8_t>* bits = nullptr) {
        int changes = 0;
        falls = 0;
        int last = m.read8(0xe84080, fc) & 1;
        const uint64_t t0 = m.cycles();
        while (m.cycles() - t0 < cycles) {
            m.run(40);
            const int v = m.read8(0xe84080, fc) & 1;
            if (bits) bits->push_back((uint8_t)v);
            if (v != last) changes++;
            if (last && !v) falls++;
            last = v;
        }
        return changes;
    };
    struct Cpu {
        int model;
        uint32_t hz;
    };
    const Cpu cpus[] = {{68000, 16000000}, {68030, 25000000}, {68060, 50000000}};
    for (const Cpu& cpu : cpus) {
        x68k::Config config;
        if (!config.setCpu(cpu.model)) continue; // 68030 のコアが入っていないビルド
        const uint64_t tenMs = cpu.hz / 100;
        {
            x68k::Machine m0(config);
            Rom rom0;
            runRom(m0, rom0);
            int falls = 0;
            CHECK_EQ(countEdges(m0, tenMs, falls), 0); // ボードがなければ変わらない
        }
        // コマンドの bit7 は等倍 (0 なら半分)、bit5-4 は周波数 (00 と 10 は 44.1kHz、01 は 32kHz、11 は 48kHz)
        const struct {
            uint16_t cmd;
            uint32_t hz;
        } rates[] = {{0xbe, 48000}, {0x8e, 44100}, {0x9e, 32000}, {0x3e, 24000}, {0x1e, 16000}};
        for (const auto& r : rates) {
            x68k::Machine m(config);
            CHECK_EQ(m.setMercury(true), true);
            Rom rom;
            rom.code({0x13fc, r.cmd, 0x00ec, 0xc091}); // move.b #cmd,$ecc091
            runRom(m, rom);
            int falls = 0;
            const uint64_t t0 = m.cycles();
            const int changes = countEdges(m, tenMs, falls);
            const double ms = (double)(m.cycles() - t0) * 1000.0 / cpu.hz;
            const int want = (int)(ms * r.hz / 1000.0 + 0.5);
            if (falls < want - 3 || falls > want + 3 || changes < 2 * falls - 2 || changes > 2 * falls + 2) {
                printf("  %d: cmd $%02x: %d changes, %d falling edges in %.2fms, want %d falling edges\n", cpu.model,
                       r.cmd, changes, falls, ms, want);
                failures++;
            }
        }
        // CPU のループで CSR の PCS ($E84080 の bit0) と $ECC090 の bit3 を読み、1 から 0 への変わり目を D1 に数える
        // (I/O を読むときに音の時刻をそろえないと、OPM の 1 サンプルごとにしか変わらず、数が合わない)
        for (const bool ecc : {false, true}) {
            x68k::Machine m(config);
            CHECK_EQ(m.setMercury(true), true);
            Rom rom;
            rom.code({0x13fc, 0x00be, 0x00ec, 0xc091, 0x7200, 0x7400}); // 48kHz、moveq #0,d1、moveq #0,d2
            const uint16_t sh = ecc ? 2 : 0;
            rom.code({0x1039, 0x00e8, 0x4080});                          // loop: move.b $e84080,d0
            if (ecc) {
                rom.b[rom.pos - 3] = 0xec;                               //   ($ecc090)
                rom.b[rom.pos - 2] = 0xc0;
                rom.b[rom.pos - 1] = 0x90;
                rom.code({0xe608});                                      //   lsr.b #3,d0
            }
            rom.code({0x0240, 0x0001,                                    // andi.w #1,d0
                      0xb440,                                            // cmp.w d0,d2
                      (uint16_t)(0x6700 | (uint8_t)(-(14 + sh))),        // beq.s loop
                      0x4a40,                                            // tst.w d0
                      0x6602,                                            // bne.s +2
                      0x5281,                                            // addq.l #1,d1
                      0x3400,                                            // move.w d0,d2
                      (uint16_t)(0x6000 | (uint8_t)(-(24 + sh)))});      // bra.s loop
            const m68k::Registers r0 = runRom(m, rom);
            const uint64_t t0 = m.cycles();
            m.run((int64_t)tenMs);
            m68k::Registers r1;
            m.getRegisters(r1);
            const int falls = (int)(r1.d[1] - r0.d[1]);
            const double ms = (double)(m.cycles() - t0) * 1000.0 / cpu.hz;
            const int want = (int)(ms * 48 + 0.5);
            if (falls < want - 3 || falls > want + 3) {
                printf("  %d: CPU loop on %s: %d falling edges in %.2fms, want %d\n", cpu.model,
                       ecc ? "$ECC090" : "$E84080", falls, ms, want);
                failures++;
            }
        }
        // 保存して読み込んだものは、保存せずに続けたものと同じ時刻に PCS が変わる (位相が続く)
        {
            x68k::Machine m(config);
            CHECK_EQ(m.setMercury(true), true);
            Rom rom;
            rom.code({0x13fc, 0x008e, 0x00ec, 0xc091}); // 44.1kHz (4MHz の倍数にならない周期)
            runRom(m, rom);
            m.run(12345);
            std::vector<uint8_t> buf(x68k::stateSize(m));
            size_t written = 0;
            CHECK_EQ(x68k::saveState(m, buf.data(), buf.size(), written), true);
            x68k::Machine m2(config);
            CHECK_EQ(m2.setMercury(true), true);
            m2.setIplRom(rom.b, sizeof rom.b);
            m2.powerOn();
            const char* err = nullptr;
            const bool loaded = x68k::loadState(m2, buf.data(), written, err);
            if (!loaded) printf("  %d: load failed: %s\n", cpu.model, err ? err : "?");
            CHECK_EQ(loaded, true);
            CHECK_EQ(m2.cycles(), m.cycles());
            std::vector<uint8_t> a, b;
            int fa = 0, fb = 0;
            countEdges(m, tenMs / 5, fa, &a);
            countEdges(m2, tenMs / 5, fb, &b);
            CHECK_EQ(fa > 0, true);
            if (a != b) {
                size_t i = 0;
                while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
                printf("  %d: PCS differs after load from read %zu (%zu / %zu reads)\n", cpu.model, i, a.size(), b.size());
                failures++;
            }
        }
    }
}

// 音の合成を別のコアで行うとき (SoundReplayer が記録を当てる側) の音が、1 つのコアのときと同じになる
void testMercuryJournal() {
    printf("mercury: journal replay gives the same samples\n");
    static uint32_t buf[1 << 18];
    x68k::VideoJournal journal;
    journal.init(buf, 18);
    x68k::Sound single, emu, synth;
    x68k::Mercury singleUnit, emuUnit;
    single.setOutputRate(48000);
    synth.setOutputRate(48000);
    x68k::SoundReplayer replayer(journal, synth);
    emu.setJournal(&journal);
    emu.mercuryAttached(true); // Machine::setMercury() と同じ
    // 同じ書き込みと進め方をする。データは空きがあるだけ書く (DMAC のチャネル 2 の代わり)
    uint16_t v = 0;
    for (int step = 0; step < 3000; step++) {
        if (step == 0 || step == 1500) {
            const uint8_t cmd = step == 0 ? 0xbe : 0x2c; // 48kHz ステレオ、22.05kHz モノラル
            single.mercuryWrite(singleUnit, 0x91, cmd);
            emu.mercuryWrite(emuUnit, 0x91, cmd);
        }
        while (emuUnit.drq()) {
            CHECK_EQ(singleUnit.drq(), true);
            v = (uint16_t)(v * 31 + 1234);
            single.mercuryWrite(singleUnit, 0x80, (uint8_t)(v >> 8));
            single.mercuryWrite(singleUnit, 0x81, (uint8_t)v);
            emu.mercuryWrite(emuUnit, 0x80, (uint8_t)(v >> 8));
            emu.mercuryWrite(emuUnit, 0x81, (uint8_t)v);
        }
        single.advance(37, &singleUnit);
        emu.advance(37, &emuUnit);
    }
    emu.flushJournal();
    journal.publish();
    while (replayer.step(1 << 20) != 0) {
    }
    CHECK_EQ(replayer.mercury() != nullptr, true);
    // まーきゅりーゆにっとに書き続けていても、1ms (4000 クロック) ごとに読み手に見せる
    {
        static uint32_t buf3[1 << 12];
        x68k::VideoJournal j3;
        j3.init(buf3, 12);
        x68k::Sound s;
        x68k::Mercury unit;
        s.setJournal(&j3);
        s.mercuryAttached(true);
        s.mercuryWrite(unit, 0x91, 0xbe);
        const uint32_t* p3;
        CHECK_EQ(j3.available(p3), 0u);
        for (int i = 0; i < 120; i++) { // 120 x 37 = 4440 クロック
            s.mercuryWrite(unit, 0x80, 0x12);
            s.mercuryWrite(unit, 0x81, 0x34);
            s.advance(37, &unit);
        }
        if (j3.available(p3) == 0) {
            printf("  journal was not published while writing to the Mercury Unit\n");
            failures++;
        }
    }
    // SoundLink でつなぐ前にボードをつないだ Machine でも、読み手にまーきゅりーゆにっとを置く
    {
        static uint32_t buf2[1 << 10];
        x68k::VideoJournal j2;
        j2.init(buf2, 10);
        x68k::Machine m;
        m.setMercury(true);
        x68k::SoundLink link;
        link.attach(m, j2);
        j2.publish();
        x68k::Sound synth2;
        x68k::SoundReplayer r2(j2, synth2);
        r2.step();
        CHECK_EQ(r2.mercury() != nullptr, true);
    }
    static int16_t a[2 * 4096], b[2 * 4096];
    int na = single.ring().drain(a, 4096), nb = synth.ring().drain(b, 4096);
    CHECK_EQ(na, nb);
    if (na < 1000) {
        printf("  only %d frames\n", na);
        failures++;
    }
    int nonzero = 0, diff = 0;
    for (int i = 0; i < 2 * na; i++) {
        if (a[i] != b[i]) diff++;
        if (a[i] != 0) nonzero++;
    }
    CHECK_EQ(diff, 0);
    if (nonzero < na) {
        printf("  the output is almost silent\n");
        failures++;
    }
}

// ステートの保存と読み込みに、2 枚のボードの状態が入る。ボードの有無が違えば読まない
void testBoardState() {
    printf("boards: state save / load\n");
    Rom rom;
    rom.code({
        0x33fc, 0x4000, 0x00e9, 0xe00a, // move.w #$4000,$e9e00a (FMOVE.L <ea>,FP0。オペランドを待つところで止める)
        0x13fc, 0x00be, 0x00ec, 0xc091, // move.b #$be,$ecc091
    });
    static Rom rom2, rom3;
    rom2 = rom; // IPL ROM が同じでないと読まない
    rom3 = rom;
    x68k::Machine a;
    a.setFpuBoard(true);
    a.setMercury(true);
    runRom(a, rom);
    a.fpuBoard()->core().fp_[3] = m68k::fp::X{0x4000, 0xc000000000000000ull}; // 3.0
    static uint8_t buf[1 << 24];
    size_t n = 0;
    CHECK_EQ(x68k::saveState(a, buf, sizeof buf, n), true);
    const uint16_t resp = a.fpuBoard()->response();

    x68k::Machine b;
    b.setFpuBoard(true);
    b.setMercury(true);
    runRom(b, rom2);
    b.mercury()->writeCommand(0); // 読む前の状態を変えておく
    b.fpuBoard()->reset();
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(b, buf, n, err), true);
    CHECK_EQ(b.mercury()->command(), 0xbe);
    CHECK_EQ(b.fpuBoard()->response(), resp);
    CHECK_EQ(b.fpuBoard()->core().fp_[3].m, 0xc000000000000000ull);

    // ボードがない Machine には読まない
    x68k::Machine c;
    runRom(c, rom3);
    CHECK_EQ(x68k::loadState(c, buf, n, err), false);
    CHECK_EQ(strcmp(err, "expansion boards differ"), 0);

    // 版 2 のステート (ボードなし) は、ボードのない Machine には読み、ボードのある Machine には読まない
    size_t n2 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n2, 2), true); // 版 2 の並び (版 3 と同じ)
    CHECK_EQ(buf[8], 2);                                        // 版 (リトルエンディアン)
    CHECK_EQ(x68k::loadState(c, buf, n2, err), true);
    buf[8] = 4; // 版 4 の並びではない (SPC の 1 バイトがない)
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 5; // 版 5 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 6; // 版 6 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 7; // 版 7 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 8; // 版 8 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 9; // 版 9 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 10; // 版 10 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 11; // 版 11 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 12; // 版 12 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 13; // 版 13 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 14; // 版 14 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 15; // 版 15 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = 16; // 版 16 の並びでもない
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    buf[8] = x68k::STATE_VERSION + 1; // 知らない版
    CHECK_EQ(x68k::loadState(c, buf, n2, err), false);
    CHECK_EQ(strcmp(err, "unsupported state version"), 0);
    buf[8] = 2;
    CHECK_EQ(x68k::loadState(b, buf, n2, err), false);
    // 版 3 のステートも読む
    size_t n3 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n3, 3), true);
    CHECK_EQ(n3, n2); // 版 2 と版 3 は同じ並び
    CHECK_EQ(x68k::loadState(c, buf, n3, err), true);
    size_t n4 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n4, 4), true);
    CHECK_EQ(n4 > n3, true); // 版 4 は SPC ごとに tempPending_ の 1 バイトが増えた
    CHECK_EQ(x68k::loadState(c, buf, n4, err), true);
    size_t n5 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n5, 5), true);
    CHECK_EQ(n5, n4 + 8); // 版 5 は Machine の speedFrac_ (8 バイト) が増えた
    CHECK_EQ(x68k::loadState(c, buf, n5, err), true);
    size_t n6 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n6, 6), true);
    CHECK_EQ(n6, n5 + 8); // 版 6 は 68000 のバスのウェイトと DRAM リフレッシュの端数 (4 バイトずつ) が増えた
    CHECK_EQ(x68k::loadState(c, buf, n6, err), true);
    size_t n7 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n7, 7), true);
    CHECK_EQ(buf[8], 7);
    CHECK_EQ(n7, n6); // 版 7 は並びが版 6 と同じ (DMAC のアレイチェーンの BTC の数え方だけが違う)
    CHECK_EQ(x68k::loadState(c, buf, n7, err), true);
    size_t n8 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n8, 8), true);
    CHECK_EQ(buf[8], 8);
    // 版 8 は SPC ごとに CD-ROM 装置の状態が増えた: バッファの位置 4 + セクタの並び 4 + 区切り 4 + 状態 22 + 応答 1024
    CHECK_EQ(n8, n7 + 2 * 1058);
    CHECK_EQ(x68k::loadState(c, buf, n8, err), true);
    size_t n9 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n9, 9), true);
    CHECK_EQ(buf[8], 9);
    CHECK_EQ(n9, n8); // 版 9 で増えたのは 68030 / 68060 の TLB と ATC の部分だけ (68000 では同じ並び)
    CHECK_EQ(x68k::loadState(c, buf, n9, err), true);
    size_t n10 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n10, 10), true);
    CHECK_EQ(buf[8], 10);
    CHECK_EQ(n10, n9); // 版 10 で増えたのは 68030 / 68060 のサイクル数の数え方の状態だけ (68000 では同じ並び)
    CHECK_EQ(x68k::loadState(c, buf, n10, err), true);
    size_t n11 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n11, 11), true);
    CHECK_EQ(buf[8], 11);
    CHECK_EQ(n11, n10); // 版 11 で増えたのは Neptune-X の送信の残りの時間だけ (Neptune-X がなければ同じ並び)
    CHECK_EQ(x68k::loadState(c, buf, n11, err), true);
    size_t n12 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n12, 12), true);
    CHECK_EQ(buf[8], 12);
    // 版 12 は SASI と SPC の状態の並びが変わった (書き直し)。CD-ROM 装置の状態は装置があるときだけになり、小さくなった
    CHECK_EQ(n12 < n11, true);
    CHECK_EQ(x68k::loadState(c, buf, n12, err), true);
    size_t n13 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n13, 13), true);
    CHECK_EQ(buf[8], 13);
    CHECK_EQ(n13, n12); // 版 13 は ADPCM の中身の並びが変わっただけで、大きさは同じ
    CHECK_EQ(x68k::loadState(c, buf, n13, err), true);
    size_t n14 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n14, 14), true);
    CHECK_EQ(buf[8], 14);
    // 版 14 は SCC の並びが変わった: チャネルごとの 16 バイト x 2 が、40 バイト x 2 + ポインタ 4 + 時刻 8 になった
    CHECK_EQ(n14, n13 + 2 * (40 - 16) + 4 + 8);
    CHECK_EQ(x68k::loadState(c, buf, n14, err), true);
    size_t n15 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n15, 15), true);
    CHECK_EQ(buf[8], 15);
    CHECK_EQ(n15, n14); // 版 15 は 68030 / 68060 の時刻の単位が変わっただけで、並びは同じ
    CHECK_EQ(x68k::loadState(c, buf, n15, err), true);
    size_t n16 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, n16), true);
    CHECK_EQ(buf[8], x68k::STATE_VERSION);
    CHECK_EQ(n16, n15); // 版 16 で増えたのは 2 枚目の数値演算プロセッサボードだけ (なければ同じ並び)
    CHECK_EQ(x68k::loadState(c, buf, n16, err), true);
}

// 2 枚目の数値演算プロセッサボードのステート (版 16 から)
void testSecondBoardState() {
    printf("boards: second fpu board state\n");
    static Rom rom, rom1, rom2, rom3;
    rom.code({
        0x33fc, 0x4000, 0x00e9, 0xe08a, // move.w #$4000,$e9e08a (2 枚目に FMOVE.L <ea>,FP0。オペランドを待つところで止める)
    });
    rom1 = rom; // IPL ROM が同じでないと読まない
    rom2 = rom;
    rom3 = rom;
    static uint8_t buf[1 << 24];
    const char* err = nullptr;

    x68k::Machine a;
    CHECK_EQ(a.setFpuBoards(2), true);
    runRom(a, rom);
    a.fpuBoard(0)->core().fp_[3] = m68k::fp::X{0x4000, 0xc000000000000000ull}; // 3.0
    a.fpuBoard(1)->core().fp_[3] = m68k::fp::X{0x4001, 0xa000000000000000ull}; // 5.0
    const uint16_t resp0 = a.fpuBoard(0)->response(), resp1 = a.fpuBoard(1)->response();
    CHECK_EQ(resp1, 0x9504); // 2 枚目だけがオペランドを待っている
    CHECK_EQ(resp0 != resp1, true);
    size_t n = 0;
    CHECK_EQ(x68k::saveState(a, buf, sizeof buf, n), true);
    CHECK_EQ(buf[8], x68k::STATE_VERSION);
    CHECK_EQ(buf[60], 1 | 16); // 1 枚目 (bit0) と 2 枚目 (bit4)
    // 版 15 までの並びには 2 枚目のボードがないので、その版では書かない
    size_t n15 = 0;
    CHECK_EQ(x68k::saveState(a, buf + n, sizeof buf - n, n15, 15), false);

    x68k::Machine b;
    CHECK_EQ(b.setFpuBoards(2), true);
    runRom(b, rom1);
    b.fpuBoard(0)->reset();
    b.fpuBoard(1)->reset();
    CHECK_EQ(x68k::loadState(b, buf, n, err), true);
    CHECK_EQ(b.fpuBoard(0)->response(), resp0);
    CHECK_EQ(b.fpuBoard(1)->response(), resp1);
    CHECK_EQ(b.fpuBoard(0)->core().fp_[3].se, 0x4000);
    CHECK_EQ(b.fpuBoard(0)->core().fp_[3].m, 0xc000000000000000ull);
    CHECK_EQ(b.fpuBoard(1)->core().fp_[3].se, 0x4001);
    CHECK_EQ(b.fpuBoard(1)->core().fp_[3].m, 0xa000000000000000ull);

    // 1 枚だけの Machine には読まない
    x68k::Machine c;
    CHECK_EQ(c.setFpuBoards(1), true);
    runRom(c, rom2);
    CHECK_EQ(x68k::loadState(c, buf, n, err), false);
    CHECK_EQ(strcmp(err, "expansion boards differ"), 0);

    // 1 枚の Machine の版 15 のステートは、1 枚の Machine には読み、2 枚の Machine には読まない
    size_t m15 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, m15, 15), true);
    CHECK_EQ(buf[8], 15);
    CHECK_EQ(buf[60], 1);
    CHECK_EQ(x68k::loadState(c, buf, m15, err), true);
    CHECK_EQ(x68k::loadState(b, buf, m15, err), false);
    CHECK_EQ(strcmp(err, "expansion boards differ"), 0);
    // 版 15 のファイルで bit4 を立てたもの (作ったもの) は、2 枚の Machine にも読まない
    buf[60] |= 16;
    CHECK_EQ(x68k::loadState(b, buf, m15, err), false);
    CHECK_EQ(strcmp(err, "expansion boards differ"), 0);
    // 1 枚の版 15 と版 16 は同じ並び
    size_t m16 = 0;
    CHECK_EQ(x68k::saveState(c, buf, sizeof buf, m16), true);
    CHECK_EQ(m16, m15);

    // 2 枚のステートは、ボードのない Machine にも読まない
    x68k::Machine d;
    runRom(d, rom3);
    CHECK_EQ(x68k::saveState(a, buf, sizeof buf, n), true);
    CHECK_EQ(x68k::loadState(d, buf, n, err), false);
    CHECK_EQ(strcmp(err, "expansion boards differ"), 0);
}

} // namespace

int main() {
    testFpuBasic();
    testFpuControlAndMovem();
    testFpuCondition();
    testFpuExceptions();
    testFpuHandlerFrame();
    testFpuBytes();
    testFpuBusyFrame();
    testFpuPacked();
    testFpuFrom68000();
    testFpuSecondBoard();
    testMercuryRegisters();
    testMercuryDma();
    testMercuryPcl();
    testMercuryJournal();
    testBoardState();
    testSecondBoardState();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
