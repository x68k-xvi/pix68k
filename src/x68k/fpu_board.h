// 数値演算プロセッサボード (CZ-6BP1 相当。MC68881 を 1 個載せたもの)。68000 の機種で使う。
//
// 68000 はコプロセッサインターフェースを持たないので、ソフトウェア (FLOAT3.X など) が $E9E000-$E9E01F に見える
// MC68881 のコプロセッサインターフェースレジスタ (CIR) を直接読み書きして使う。アドレスは Inside X68000 と
// XEiJ の説明書。2 枚目のボードの先頭が $E9E080 なのも同じ説明書による (ここに前から書いてあった注釈と issue #109 から。
// 説明書は読み直していない)。2 枚目も 1 枚目と同じ 32 バイトの CIR を持ち、別の MC68881 として独立に動くとしたのは
// 推測 (Machine が 2 個目のインスタンスを BASE2 に置く)。
// CIR の動きは MC68881/MC68882 User's Manual (MC68881UM) の 7 章 (表 7-7 の応答プリミティブと 7.5 の手順) に従う:
//   $00 応答 (R)        今の応答プリミティブ。読むと次の応答に進むものがある (評価と転送のプリミティブなど)
//   $02 制御 (W)        書くと実行中の手順をやめ、保留している例外を消して待機に戻る (68881 はどの値でも同じ)
//   $04 セーブ (R)      FSAVE の始まり。フォーマット語を返し、続けてオペランドから状態のフレームを読ませる
//   $06 リストア (R/W)  FRESTORE の始まり。書いたフォーマット語を読み返す (正しくなければ $0218)
//   $08 命令語 (W)      使わない
//   $0A コマンド (W)    cpGEN の命令の 2 語目 (FADD、FMOVE、FMOVEM など)
//   $0E 条件 (W)        FBcc などの条件 (6 ビット)。応答は $0800 | 成立 (TF。成立なら 1)
//   $10 オペランド (R/W) データ。1 回の読み書き (16 ビット) で次の 2 バイトを送る。バイトの読み書きは 1 バイト
//   $14 レジスタ選択 (R) FMOVEM で送るレジスタのリスト (上位バイト)
//   $18 命令アドレス (W) PC (FPIAR に入れる)
//   $1C オペランドアドレス  使わない
// 書き込み専用のレジスタを読むと $FFFF。
//
// 68881 として振る舞う (MC68882 との違い): PC の転送の要求 (PC ビット) は無視してよい。評価と転送のプリミティブは
// いつも CA = 1。FSAVE の IDLE のフレームは $1F18 で 6 ロング、BUSY は $1FB4 で 45 ロング (Inside X68000 の
// フォーマット語)。中の計算は m68k::FpuCore (68882 として作ったもの) をそのまま使う。
//
// 実機と違う点:
// - 計算はコマンドやオペランドを受け取った時点ですぐ終わる (応答が $8900 のまま待たせることはない)。
// - 浮動小数点の例外は、許可された例外が起きると保留し、次の演算のコマンドか条件を書いたときに命令の前の例外 ($1C3x)
//   を返す。FMOVE FPn,<ea> だけは、オペランドを読み終えたときに命令の途中の例外 ($1D3x) を返す。
//   保留中の IDLE のフレームの例外のオペランドには、ソースのオペランドを入れる (実機の中身は確かめていない)。
//   知らせる例外の種類は、そのときの FPCR の ENABLE と FPSR の EXC から決める。制御レジスタの転送と FMOVEM は、
//   保留していても知らせずに実行する。
// - BUSY のフレーム ($1FB4、45 ロング) は、手順の途中の状態 (このクラスのメンバ) をそのまま詰めたもの (実機の
//   中身とは違う)。保留している例外も入れる: 低いアドレスから 30-35 ロング目に IDLE のフレームの中身、36 ロング目の
//   bit0 に知らせたあとか、最後のロング (BIU フラグ) の bit27 が 0 なら保留あり (packBusy())。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

#ifndef PIX68K_NO_FPU_BOARD
#include "m68k/fpu_core.h"
#endif

namespace x68k {

class StateIO;

class FpuBoard {
public:
    static constexpr uint32_t BASE = 0xe9e000;  // CIR の先頭。$E9E000-$E9E01F
    static constexpr uint32_t BASE2 = 0xe9e080; // 2 枚目のボードの CIR の先頭。$E9E080-$E9E09F
    static constexpr uint32_t SIZE = 0x20;
    static constexpr int MAX_BOARDS = 2;

#ifndef PIX68K_NO_FPU_BOARD
    FpuBoard() { reset(); }
    void reset();

    // 16 ビットの読み書き。off は CIR の先頭 ($E9E000 か $E9E080) からの位置 (偶数)
    uint16_t read16(uint32_t off);
    void write16(uint32_t off, uint16_t v);
    // 8 ビットの読み書き。オペランドは 1 バイトを送る。ほかのレジスタは、偶数番地の読み出しを 16 ビットの読み出しの
    // 上位バイト、奇数番地は下位バイト (読んでも応答は進まない)、書き込みは同じ値を上位と下位に書いたものとする
    uint8_t read8(uint32_t off);
    void write8(uint32_t off, uint8_t v);

    m68k::FpuCore& core() { return core_; }
    uint16_t response() const { return resp_; }
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)

private:
    // 手順の段階
    enum Phase : uint8_t {
        P_IDLE,    // 待機 (オペランドの読み書きは手順の違反)
        P_IN,      // オペランドを受け取る (in_ がそろったら finishIn())
        P_OUT,     // オペランドを送る (out_ を送り終えたら finishOut())
        P_MAINREG, // 「メインプロセッサのレジスタを 1 個送れ」($8C0r) のあと、4 バイトを受け取る
        P_REGSEL,  // 「複数のレジスタを送れ」($810C / $A10C) のあと、レジスタ選択を読むのを待つ
        P_SAVE,    // FSAVE のフレームを送る
        P_RESTORE, // FRESTORE のフレームを受け取る
    };
    // 受け取ったオペランドの使い道 (P_IN)
    enum InKind : uint8_t { IN_SOURCE, IN_CONTROL, IN_MOVEM };

    void command(uint16_t w);
    void condition(uint16_t cc);
    uint16_t save();
    void restoreFormat(uint16_t f);
    void abort();

    // 手順の操作
    void setResponse(uint16_t first, uint16_t next = 0, uint16_t next2 = 0) {
        resp_ = first;
        next_ = next;
        next2_ = next2;
    }
    void startIn(InKind kind, int bytes);
    void startOut(const uint32_t* w, int bytes);
    void violation();
    void operandIn(uint8_t b);
    bool operandOut(uint8_t& b);
    void finishIn();
    void finishOut();
    void finishMainReg();
    void startMovem();

    // 計算 (MC68881UM 7.5 の各命令の手順で、データがそろったところ)
    void arith(const m68k::fp::X* srcReg, const uint32_t* srcMem);
    void fmoveOut(int k);
    void raisePending(const m68k::fp::X& operand);
    bool takePending();
    uint16_t pcBit() const { return (core_.fpcr_ & 0xff00) ? 0x4000 : 0; } // 例外が 1 つでも許可されていれば PC を求める
    void packBusy(uint32_t* w) const;
    void unpackBusy(const uint32_t* w);

    m68k::FpuCore core_;
    bool nullState_ = true; // リセット後、まだ命令を実行していない (FSAVE は NULL のフレーム)
    bool excPending_ = false; // 浮動小数点の例外を保留している (次のコマンドか条件で $1C3x を返す)
    // 保留していた例外を $1C3x で知らせたあと。FSAVE のフレームには、まだ例外の状態を残す (例外ハンドラが FSAVE して、
    // BIU フラグの bit27 を立ててから FRESTORE する手順のため。MC68881UM 6.4.2.2)。次の演算か条件で消える
    bool excReported_ = false;
    uint32_t pendFrame_[6] = {}; // 保留中の IDLE のフレーム (低いアドレスから)

    uint16_t resp_ = 0x0802; // 今の応答
    uint16_t next_ = 0;      // 応答を読んだあとの応答 (0 なら変えない)
    uint16_t next2_ = 0;     // その次
    uint16_t cmd_ = 0;       // 実行中のコマンド
    uint8_t phase_ = P_IDLE;
    uint8_t inKind_ = IN_SOURCE;
    uint8_t list_ = 0;       // FMOVEM のリスト
    uint16_t outDone_ = 0;   // P_OUT を送り終えたときの応答
    uint16_t restoreEcho_ = 0xffff; // リストアを読んだときの値
    int len_ = 0, pos_ = 0;  // P_IN / P_OUT / P_SAVE / P_RESTORE で送るバイト数と送った数
    uint32_t fpiarHi_ = 0;   // 命令アドレスの上位の語
    uint8_t buf_[180] = {};  // オペランド、FMOVEM のデータ、状態のフレーム
#endif
};

} // namespace x68k
