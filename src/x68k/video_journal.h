// 画面の合成を別のコア (スレッド) で行うための、Video への書き込みの記録 (1 つの書き手と 1 つの読み手のリング)。
//
// しくみ:
//   X68000 を動かすコア (書き手) は、Video の状態を変える操作 (VRAM、パレット、スプライト、CRTC への書き込み、
//   ラスタコピー、垂直表示期間の始まりの処理 (高速クリア)、リセット) をすべて 32 ビットの記録にしてリングに入れる。画面の合成のラスタ (onRaster) と
//   フレームの終わり (onFrameEnd) も同じリングに入れる。
//   合成するコア (読み手) は自分の Video の写し (shadow) を持ち、記録を順に同じ Video の関数で写しに当て、
//   ラスタの記録が来たらそのラスタを写しから描く (VideoReplayer)。
//   写しは「そのラスタの時点の X68000 の Video」と同じになるので、合成の結果は 1 つのコアで描いたときと同じになる。
//   合成するコアが遅れても、遅れはリングに入る分まで。リングが満杯になったら書き手は空くまで待つ (記録は捨てない)。
//
// 記録の形 (32 ビット): bit31-29 が種類、bit28-8 がオフセット (21 ビット)、bit7-0 が値。
//   グラフィック VRAM のオフセット ($C00000 から 2MB) がちょうど 21 ビットに入る。
//
// 書き手は記録をためておき、ラスタの記録のたびと BATCH 個ごとに読み手に見せる (head を更新する)。
// head と tail は別のキャッシュラインに置く。メモリの順序は __atomic の acquire / release で守る
// (Pi 3 の ARMv8 は弱い順序のメモリモデル)。ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

namespace x68k {

class VideoJournal {
public:
    enum Op : uint32_t {
        OP_TVRAM = 0,   // tvramWrite(offset, v)
        OP_GVRAM = 1,   // gvramWrite(offset, v)
        OP_PALETTE = 2, // paletteWrite(offset, v)
        OP_SPRITE = 3,  // spriteWrite(offset, v)
        OP_CRTC = 4,    // crtcWrite(offset, v)
        OP_HBLANK = 5,  // horizontalBlank()
        OP_RASTER = 6,  // LineListener::onRaster(offset)
        OP_CONTROL = 7, // offset が CTL_*
    };
    enum Control : uint32_t { CTL_RESET = 0, CTL_FRAME_END = 1, CTL_VDISP = 2 }; // CTL_VDISP: beginVerticalDisplay()
    static constexpr unsigned BATCH = 256; // これだけたまったら、ラスタの区切りを待たずに読み手に見せる

    static uint32_t make(uint32_t op, uint32_t offset, uint32_t v) { return op << 29 | (offset & 0x1fffff) << 8 | (v & 0xff); }
    static uint32_t opOf(uint32_t e) { return e >> 29; }
    static uint32_t offsetOf(uint32_t e) { return (e >> 8) & 0x1fffff; }
    static uint8_t valueOf(uint32_t e) { return (uint8_t)e; }

    // buf は 2^log2 個の記録を置く場所 (呼んだ側が確保し、使い終わるまで持つ)
    void init(uint32_t* buf, unsigned log2);
    // 待つ間に呼ぶ関数 (ホストのスレッドなら yield など。nullptr なら何もしない)
    void setRelax(void (*relax)()) { relax_ = relax; }
    // 待つ間に 1 回呼ぶ (setRelax() で渡した関数。RenderLink がフレームの遅れを待つときにも使う)
    void relax() const {
        if (relax_) relax_();
    }
    // 書き手が待った時間を測る時計 (単調に増える時刻を返す関数。単位は任意。nullptr なら測らない)。
    // 時刻は待つときにだけ読む (Stats::waitTicks、RenderLink::frameWaitTicks())
    void setClock(uint64_t (*clock)()) { clock_ = clock; }
    uint64_t clockNow() const { return clock_ ? clock_() : 0; }
    // 読み手に見せたあとに呼ぶ関数 (読み手を起こす。ベアメタルなら sev。nullptr なら何もしない)
    void setWake(void (*wake)()) { wake_ = wake; wakeContextFn_ = nullptr; }
    void setWakeContext(void (*wake)(void*), void* context) { wake_ = nullptr; wakeContextFn_ = wake; wakeContext_ = context; }
    unsigned capacity() const { return size_; }

    // ---- 書き手 ----
    void push(uint32_t e) {
        if (head_ - tailCache_ >= size_) waitForSpace();
        buf_[head_ & mask_] = e;
        head_++;
        if (head_ - published_ >= BATCH) publish();
    }
    void publish() {
        published_ = head_;
        __atomic_store_n(&shared_.head, head_, __ATOMIC_RELEASE);
        if (wakeContextFn_) wakeContextFn_(wakeContext_);
        else if (wake_) wake_();
    }
    // ためた記録をすべて見せ、読み手が処理し終えるまで待つ。戻ったあとは、読み手が書いた画素を読んでよい
    void flush();
    // 読み手が止まったことにする (待っている書き手を戻す。以後の記録は捨てる)。どちらのスレッドからも呼べる
    void stop() { __atomic_store_n(&shared_.stopped, 1u, __ATOMIC_RELEASE); }
    bool stopped() const { return __atomic_load_n(&shared_.stopped, __ATOMIC_ACQUIRE) != 0; }

    // 書き手の統計 (書き手のスレッドで読む)
    struct Stats {
        uint64_t waits = 0;      // 満杯で待った回数
        uint64_t waitSpins = 0;  // 待つ間に tail を読んだ回数
        uint64_t waitTicks = 0;  // 満杯で待った時間 (setClock() の時計の単位。時計がなければ 0)
    };
    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = Stats(); }
    // 今までに入れた記録の数
    unsigned pushed() const { return head_; }
    // 今のたまり具合 (書き手から見た、読み手がまだ処理していない記録の数)
    unsigned fill() const { return head_ - __atomic_load_n(&shared_.tail, __ATOMIC_ACQUIRE); }

    // ---- 読み手 ----
    // 処理できる記録の数を返し、first に最初の記録の位置を入れる (リングの終わりで折り返さない範囲まで)
    unsigned available(const uint32_t*& first) {
        unsigned h = __atomic_load_n(&shared_.head, __ATOMIC_ACQUIRE);
        unsigned n = h - tail_;
        unsigned toEnd = rsize_ - (tail_ & rmask_);
        if (n > toEnd) n = toEnd;
        first = rbuf_ + (tail_ & rmask_);
        return n;
    }
    // n 個を処理し終えた (処理の結果は release で書き手に見える)
    void consume(unsigned n) {
        tail_ += n;
        __atomic_store_n(&shared_.tail, tail_, __ATOMIC_RELEASE);
    }

private:
    void waitForSpace();

    // 書き手だけが使う
    uint32_t* buf_ = nullptr;
    unsigned size_ = 0, mask_ = 0;
    unsigned head_ = 0, published_ = 0, tailCache_ = 0;
    void (*relax_)() = nullptr;
    void (*wake_)() = nullptr;
    void (*wakeContextFn_)(void*) = nullptr;
    void* wakeContext_ = nullptr;
    uint64_t (*clock_)() = nullptr;
    Stats stats_;
    // 読み手だけが使う
    alignas(64) unsigned tail_ = 0;
    const uint32_t* rbuf_ = nullptr;
    unsigned rsize_ = 0, rmask_ = 0;
    // 両方が読み書きする
    struct alignas(64) Shared {
        unsigned head = 0;
        alignas(64) unsigned tail = 0;
        alignas(64) unsigned stopped = 0;
    } shared_;
};

} // namespace x68k
