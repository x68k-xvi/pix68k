// 合成した画面を、合成するコア (書き手) から画面を写すコア (コア 0) へ渡す 3 枚のバッファと、写す役の受け渡し。
// ベアメタル版の Emulator (baremetal/emulator.cpp) が使う。バッファの中身 (画素) は持たず、0〜2 の番号だけを扱う。
//
// ふだん: 書き手は描き終えたバッファを publish() で受け渡し中のもの (shared_) と交換し、コア 0 は take() で取る。
// コア 0 が取る前に次のフレームが来たら、前のフレームは画面に出ずに置き換わる (dropped() で数える)。
//
// 写す役の受け渡し: コア 0 が長く止まる間 (USB の転送など) は、begin() から end() までの間、書き手が publish() の中で
// present(番号) を呼んで自分で写す。mode_ の値と、書くコア:
//   CORE0 -(コア 0: begin())-> SELF
//   SELF -(書き手: 写し始める)-> BUSY -(写し終える)-> SELF
//   SELF -(コア 0: end())-> CORE0
//   BUSY -(コア 0: end())-> BUSY_HANDBACK -(書き手: 写し終える)-> CORE0
// 書き手が写すのは BUSY / BUSY_HANDBACK の間だけ、コア 0 が写すのは CORE0 の間だけなので、同時に写すことはない。
// メモリのバリア:
//   - コア 0 は自分の最後の書き込みのあとに SELF を release で書き、書き手は SELF -> BUSY を acquire で書き換えてから
//     写す (コア 0 の書き込みのあとに書き手の書き込みが来る)。
//   - 書き手は写し終えてから BUSY -> SELF / CORE0 を release で書き、コア 0 は SELF -> CORE0 の書き換え、または CORE0 を
//     読むのを acquire で行ってから写す (書き手の書き込みのあとにコア 0 の書き込みが来る)。
// 受け渡しで、同じフレームを 2 回写すことも、抜けることもない: SELF にする前に take() で取ったものまではコア 0 が写し、
// 受け渡し中に残っていたものと、SELF の間に描いたものは書き手が写し、CORE0 に戻ったあとに描いたものは、ふだんどおり
// shared_ でコア 0 に渡す。ふだんの受け渡しでコア 0 が取る前に置き換わるもの (dropped()) は、これとは別にある。
// retainSelfFrames() を起動前に指定した場合は、直接表示した最新フレームも shared_ に残す。
// 読み手は end() のあと take() で受け取り、第2出力に使える。表示済みのフレームは dropped() に数えず、
// 書き手も再表示しない。読み手がメインにも再表示した場合、その分は selfPresented() と重複する。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>

namespace platform {

class FrameHandoff {
public:
    // 起動前に設定。自分で表示したフレームも読み手へ渡し、第2出力がコピーなしで参照できるようにする。
    void retainSelfFrames() { retainSelf_ = true; }

    // ---- 書き手 ----
    // いま描いているバッファの番号
    unsigned writer() const { return writer_; }
    // 描き終えたバッファ (writer()) を渡し、次に描くバッファの番号を返す。写す役を任されていれば、present(番号) を
    // 呼んで自分で写す。既定では同じ番号を返すが、retainSelfFrames() 指定時は shared_ と交換する
    template <class Present> unsigned publish(Present&& present) {
        unsigned mode = SELF;
        if (__atomic_load_n(&mode_, __ATOMIC_RELAXED) == SELF &&
            __atomic_compare_exchange_n(&mode_, &mode, (unsigned)BUSY, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            count(presentSeq_); // 奇数: 写している (presentSeq())
            // コア 0 が取らなかったフレームが受け渡し中に残っていれば、先にそれを写す。コア 0 は CORE0 に戻るまで
            // take() を呼ばないので、shared_ を書き換えてよい
            const unsigned pending = __atomic_load_n(&shared_, __ATOMIC_ACQUIRE);
            if ((pending & NEW_FRAME) && !(pending & SELF_SHOWN)) {
                present(pending & 3);
                count(selfPresented_);
                __atomic_store_n(&shared_, pending & 3, __ATOMIC_RELAXED); // 下の release で CORE0 より前に見える
            }
            present(writer_);
            count(selfPresented_);
            count(published_);
            count(presentSeq_); // 偶数に戻す
            // 写し終えたことを release で示す。コア 0 が戻すよう頼んでいたら (BUSY_HANDBACK) CORE0 にする
            __atomic_store_n(&latest_, (int)writer_, __ATOMIC_RELEASE);
            if (retainSelf_) {
                const unsigned old = __atomic_exchange_n(&shared_, writer_ | NEW_FRAME | SELF_SHOWN, __ATOMIC_ACQ_REL);
                writer_ = old & 3;
            }
            mode = BUSY;
            if (!__atomic_compare_exchange_n(&mode_, &mode, (unsigned)SELF, false, __ATOMIC_RELEASE,
                                             __ATOMIC_RELAXED))
                __atomic_store_n(&mode_, (unsigned)CORE0, __ATOMIC_RELEASE);
            return writer_;
        }
        const unsigned old = __atomic_exchange_n(&shared_, writer_ | NEW_FRAME, __ATOMIC_ACQ_REL);
        // コア 0 が前のフレームを取る前に次のフレームを渡した (前のフレームは画面に出ない)
        if ((old & NEW_FRAME) && !(old & SELF_SHOWN)) count(dropped_);
        count(published_);
        __atomic_store_n(&latest_, (int)writer_, __ATOMIC_RELEASE);
        writer_ = old & 3;
        return writer_;
    }

    // ---- コア 0 ----
    // 新しいフレームがあれば番号を返す (なければ -1)。次に呼ぶまで、書き手はそのバッファに描かない
    int take() {
        if (!(__atomic_load_n(&shared_, __ATOMIC_ACQUIRE) & NEW_FRAME)) return -1;
        const unsigned old = __atomic_exchange_n(&shared_, reader_, __ATOMIC_ACQ_REL);
        reader_ = old & 3;
        return (int)reader_;
    }
    // 写す役を書き手に任せる。呼んだあと end() が返るまで、take() を呼ばず、写さない
    void begin() { __atomic_store_n(&mode_, (unsigned)SELF, __ATOMIC_RELEASE); }
    // 写す役を戻す。書き手が写している途中なら、写し終えるまで wait() を繰り返し呼ぶ。待つのは、書き手がそのとき
    // publish() の中で写している分だけ: ふつうは 1 フレーム、受け渡し中に残っていたフレームを先に写しているときは 2 フレーム
    template <class Wait> void end(Wait&& wait) {
        for (;;) {
            unsigned mode = SELF;
            if (__atomic_compare_exchange_n(&mode_, &mode, (unsigned)CORE0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return;
            if (mode == CORE0) return; // 書き手が写し終えて戻した (acquire で読んだ)
            if (mode == BUSY)
                __atomic_compare_exchange_n(&mode_, &mode, (unsigned)BUSY_HANDBACK, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE);
            wait();
        }
    }

    // 写す役を任せている間 (begin() から end() まで) に、今のフレームを受け取って写す (ネットワークで画面の写しを
    // 返すとき)。任せている間に合成したフレームは書き手が写し、take() には渡らないので、コア 0 が前に受け取ったフレームは
    // 任せる前のもののまま古くなる (#57)。そこで一度 end() で役を戻し、フレームを take() で受け取って show(番号) で
    // 写してから、begin() で任せ直す。
    // 受け取るフレーム: 任せる前に受け渡し中に残っていて、任せている間に書き手が 1 つも描き終えていなければ、それを
    // すぐに受け取る (それがいちばん新しい)。そうでなければ (残っていたものは書き手が写して消している)、end() のあとに
    // 書き手が次に描き終えたものを待って受け取る。
    // 待ち方: まず end() で、書き手が写している途中なら写し終えるまで waitPresent() を繰り返し呼んで待つ (打ち切れない。
    // 長さは end() に書いた)。そのあとフレームを待つ間は wait() を繰り返し呼び、false を返したら待つのをやめ、写さずに
    // 任せ直して -1 を返す (X68000 が止まっているとき、時間切れなど)。受け取ったら番号を返す。show() で写す時間も
    // 打ち切れない。waitPresent() と wait() を分けているのは、呼び手が待つ時間の上限に、フレームを待つ時間だけを
    // 数えられるようにするため (platform/snapshot_wait.h)
    template <class WaitPresent, class Wait, class Show>
    int takeWhileHanded(WaitPresent&& waitPresent, Wait&& wait, Show&& show) {
        end(waitPresent);
        int i;
        while ((i = take()) < 0 && wait()) {
        }
        if (i >= 0) show((unsigned)i);
        begin();
        return i;
    }

    // 最後に書き手が描き終えたフレーム (publish() に渡したもの) の番号 (まだなければ -1)。写す役をどちらが持っていても、
    // また、コア 0 が受け取ったかどうかにかかわらず返す。書き手が止まっている (publish() を呼んでおらず、次のフレームも
    // 描いていない) と分かっているときだけ使う: 書き手が自分で写したフレームは、次に描くバッファ (writer()) のままなので、
    // 書き手が動いていれば上書きされる。止まったことは、書き手が止まったあとに release で書いたものを acquire で読んで
    // 知ること (このクラスの外。ベアメタル版では Emulator::paused())。そうすれば、その番号のバッファの中身も読める
    int latest() const { return __atomic_load_n(&latest_, __ATOMIC_ACQUIRE); }

    // ---- 数 (書き手が書き、ほかのコアが読む。ログ用) ----
    uint32_t published() const { return __atomic_load_n(&published_, __ATOMIC_RELAXED); }
    uint32_t dropped() const { return __atomic_load_n(&dropped_, __ATOMIC_RELAXED); }
    uint32_t selfPresented() const { return __atomic_load_n(&selfPresented_, __ATOMIC_RELAXED); }
    // 書き手が写し始めるときと写し終えたときに 1 ずつ増える。奇数なら写している途中。ほかのコアが前後で 2 回読み、
    // 前が奇数か、値が変わっていれば、その間に書き手が写していた (写しが区間をまたいでも漏れない)
    uint32_t presentSeq() const { return __atomic_load_n(&presentSeq_, __ATOMIC_RELAXED); }

private:
    enum : unsigned { CORE0 = 0, SELF = 1, BUSY = 2, BUSY_HANDBACK = 3 };
    static constexpr unsigned NEW_FRAME = 4, SELF_SHOWN = 8;
    bool retainSelf_ = false; // コア起動前にだけ設定
    static void count(uint32_t& n) { __atomic_store_n(&n, n + 1, __ATOMIC_RELAXED); } // 書き手だけが書く

    unsigned writer_ = 0, reader_ = 2;
    unsigned shared_ = 1; // 下位 2 ビットがバッファの番号。NEW_FRAME はまだ読まれていないことを表す
    unsigned mode_ = CORE0;
    int latest_ = -1; // 最後に publish() で描き終えたバッファの番号 (latest())
    uint32_t published_ = 0, dropped_ = 0, selfPresented_ = 0, presentSeq_ = 0;
};

} // namespace platform
