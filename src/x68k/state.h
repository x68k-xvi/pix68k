// 実行状態の保存と復元 (ステートセーブ / ロード)。
//
// CPU (68000、68030 / 68060 と FPU)、メモリ (メイン RAM、ハイメモリ、060turbo のローカルメモリ、SRAM)、Video、
// 周辺機器 (MFP、キーボード、RTC、システムポート、IOC、FDC、OPM と ADPCM、DMAC、SCC、マウス、8255、SASI、SPC、
// つないでいれば数値演算プロセッサボード (2 枚まで)、まーきゅりーゆにっと、Neptune-X) と、Machine の時間の状態を 1 つのバイト列にする。ROM とディスクのイメージは入れない (環境が持つもの)。
// フロッピーディスクとハードディスクは、読むときに今つないでいるイメージをそのまま使う
// (保存したときのイメージの有無と大きさは入れておき、違えば転送の途中の状態を捨てる)。
//
// 形式 (数値はすべてリトルエンディアン):
//   +0   "PIX68KST" (8 バイト)
//   +8   版 (STATE_VERSION)。違う版は読まない (版 2 は、拡張ボードのない Machine にだけ読む。版 3 から版 17 も読む。
//        版 11 までの SASI と SPC の状態は読み飛ばす)
//   +12  ヘッダの大きさ (HEADER_SIZE)
//   +16  CPU の種類 (m68k::CpuModel の値)、+20 メイン RAM、+24 ハイメモリ、+28 ローカルメモリ (バイト)、
//   +32  CPU のクロック (Hz)、+36 IPL ROM の CRC-32、+40 内蔵 SCSI の ROM の CRC-32 (なければ 0)、
//   +44  外付け SCSI ボードの ROM の CRC-32 (ボードがなければ 0)
//   +48  中身の大きさ (64 ビット)、+56 中身の CRC-32、
//   +60  つないでいる拡張ボード (bit0 数値演算プロセッサボード、bit1 まーきゅりーゆにっと、bit2 Neptune-X、bit3 MIDI ボード、
//        bit4 2 枚目の数値演算プロセッサボード (版 16 から)、bit5 TS-6BGA、bit6 TS-6BGA が $EF0000 (版 17 から))
//   +64  中身 (各部の状態を決まった順に並べたもの。メモリは 4KB ごとに、全部 0 なら省く)
// 機種、メモリの大きさ、クロック、ROM (IPL、内蔵 SCSI、外付け SCSI ボード)、拡張ボードのどれかが違えば読まない (Machine を作り直してから読むこと)。
//
// 読み書きは、各クラスの state(StateIO&) が保存と復元の両方を受け持つ (並べる順は 1 か所に書く)。
// 保存と復元は、Machine を動かしていない間 (フレームの切れ目など) に呼ぶ。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace m68k {
namespace fp {
struct X;
}
} // namespace m68k

namespace x68k {

class Machine;
class Sound;
class Video;

static constexpr uint32_t STATE_VERSION = 18; // 3: 拡張ボード (+60 と、中身の最後のボードの状態) を足した
                                             // 4: SPC の、TEMP に入れるバイトの読み込みを待っているか (Spc::tempPending_)
                                             // 5: 68000 の速さを変えて動かしたときの時間の端数 (Machine::speedFrac_)
                                             // 6: 68000 のバスのウェイトと DRAM リフレッシュの端数 (Machine の TIME の最後)
                                             // 7: 並びは 6 と同じ。DMAC のアレイチェーンの BTC を、表を読んだときに減らす
                                             //    (6 まではブロックの終わりに減らしていた。Dmac::state())
                                             // 8: CD-ROM 装置の状態 (Spc の最後。x68k/cd_drive.h)
                                             // 9: 68060 の MMU が有効な間の TLB と ATC (68030 / 68060 のとき、END の前)
                                             // 10: 68060 のサイクル数の数え方の状態 (同時実行、分岐の折りたたみ、分岐キャッシュ、
                                             //     命令の取り出しの位置。68030 / 68060 のとき、9 の次)
                                             // 11: Neptune-X の送っているフレームの残りの時間と、最初の poll() の前か (Neptune の最後)
                                             // 12: SASI と SPC (SCSI) を書き直して、状態の並びが変わった (Sasi::state()、Spc::state())。
                                             //     CD-ROM 装置の状態は、装置があるときだけ入れる。版 11 までの SASI と SPC の
                                             //     状態は、SPC のレジスタだけを戻し、ほかは読み飛ばして、バスフリーから始める
                                             //     (SCSI のコマンドの途中で保存したものは読まない)
                                             // 13: ADPCM (Adpcm) を書き直して、メンバごとに書く。12 までの 32 バイトは読み捨てる
                                             // 14: SCC の RS-232C (チャネル A の送受信の時刻、外部 / 状態の割り込み、
                                             //     特殊受信条件) と、2 つのチャネルで 1 つにしたレジスタの選択のポインタ
                                             // 15: 並びは 14 と同じ。68030 / 68060 の時刻 (Cpu020::sub_) の単位が 1/800 サイクルから
                                             //     1/12800 サイクル (Cpu020::SUB) になった。14 までは読むときに 16 倍する
                                             // 16: 2 枚目の数値演算プロセッサボード ($E9E080)。+60 の bit4 と、中身の "FPU2"
                                             //     (1 枚目の "FPUB" の次。2 枚目があるときだけ)。0 枚か 1 枚なら並びは 15 と同じ
// 18: TS-6BGA の BitBLT host 転送の途中の入力位置とDWORD FIFO。
// 17: TS-6BGA の VRAM とレジスタ、+60 の bit5 (接続) と bit6 ($EF0000)。
static constexpr uint32_t STATE_REWRITTEN_SCSI = 12;
static constexpr uint32_t STATE_REWRITTEN_ADPCM = 13;
static constexpr uint32_t STATE_SCC_RS232C = 14;
static constexpr uint32_t STATE_CPU020_SUB = 15;
static constexpr uint32_t STATE_FPU_BOARD2 = 16;
static constexpr uint64_t STATE_CPU020_SUB_OLD = 800; // 版 14 までの Cpu020::sub_ の単位 (1/800 サイクル)
static constexpr size_t STATE_HEADER_SIZE = 64;
// 読むステートの CPU の時刻 (電源を入れてからのサイクル数) を、そのまま使う上限。これを超える時刻のステートは
// 断らずに読み、時刻を小さい値に直す (stateKeptTime())。CPU の時刻と、時刻で持っているほかの値 (ラスタの始まり、
// キーボード、マウス、MIDI ボードなど。StateIO::time()) を同じだけ戻すので、値どうしの差は変わらず、続きは同じに動く。
// 直さないと、64 ビットの上限の近くで進める先の時刻の足し算があふれる: 68030 / 68060 は CPU が進まないまま
// Machine::run() が戻らなくなり、68000 は進まずに戻る。
// 直したあとの時刻は STATE_CPU_TIME_PAST 以上で、上限の半分 + STATE_CPU_TIME_PAST より小さい。
// STATE_CPU_TIME_PAST (約 2^40 サイクル、50MHz で 6 時間ぶん) は、今より前の時刻を表すための余裕: 時刻で持っている値は、
// 今より約 2^40 サイクル以上前でなければ、差を保ったまま戻せる (それより前の値は 0 にする。どちらも「とうに過ぎた時刻」)。
// 読んだあとの時刻はいつも上限以下なので、そこから動かせる時間は、上限からあふれるまでの時間より短くならない。
// - 68000: 2^62 サイクル。16MHz で動かし続けて 9 千年ぶんで、あふれるまではその 3 倍ある。ふつうに動かして
//   届くことはなく、超えるステートは手で作らないかぎりできない
// - 68030 / 68060: 約 2^49 サイクル。中では 1/12800 サイクル (Cpu020::SUB) の単位で数えるので、64 ビットに入るのは
//   2^64 / 12800 = 1.4 x 10^15 サイクルまで。2^49 サイクルは 50MHz で動かし続けて 130 日ぶんで、そこからあふれるまでは
//   50MHz でさらに 203 日ぶんある。130 日を超えて動かし続けたステートは、読むと時刻が直る (保存はいつでもでき、読める)。
//   続けて動かしたときは、上限を超えたあとのフレームの切れ目で、同じ戻し方で時刻を戻すので、あふれない
//   (Machine::rebaseTime()。#165。前は、ステートを読まずに 50MHz で 333 日動かし続けると、あふれて止まった)。
//   版 14 までは 1/800 サイクルの単位で、上限は 2^53 サイクルだった (読むときに今の単位と上限に直す)
static constexpr uint64_t STATE_CPU_CYCLES_MAX = 1ull << 62;
// 68030 / 68060 の上限と余裕は、2^49 と 2^40 をそれぞれ 480 と 240 の倍数に切り下げた値にする。X68030 と 060turbo の本体の I/O の
// 読み書きは、CPU の時刻を 1.6 マイクロ秒 (25MHz の 40 サイクル、50MHz の 80 サイクル、75MHz の 120 サイクル) で割った余りで待ち時間を決める
// (Config::busArea の period)。戻す量 (上限の半分の倍数 - 余裕) が 240 サイクルの倍数なら、時刻を直しても余りは変わらず、
// 続きは同じに動く (2^48 を 240 で割った余りは 16 なので、切り下げなければ区切りの位置がずれる)
// 区切りの位置が保たれるのは、既定の 1600ns (25 / 50 / 75MHz) のように、周期が 240 サイクルの約数の場合。ほかの値を
// 入れたとき (x68k_boot の --bus-sync や --bus-area) は、約数とは限らないので、時刻を戻すと位置がずれることがある
// (Machine::busSyncKeepsPhase() で分かる)
static constexpr uint64_t STATE_TIME_SHIFT_UNIT = 240;
static constexpr uint64_t STATE_CPU020_CYCLES_MAX = (1ull << 49) / (2 * STATE_TIME_SHIFT_UNIT) * (2 * STATE_TIME_SHIFT_UNIT);
static constexpr uint64_t STATE_CPU_TIME_PAST = (1ull << 40) / STATE_TIME_SHIFT_UNIT * STATE_TIME_SHIFT_UNIT;
// 時刻を戻す処理は、どれも「上限の半分の倍数 - 余裕」だけ戻す (stateKeptTime())。この 2 つが 240 の倍数であること
static_assert(STATE_CPU020_CYCLES_MAX / 2 % STATE_TIME_SHIFT_UNIT == 0 && STATE_CPU_TIME_PAST % STATE_TIME_SHIFT_UNIT == 0,
              "the time shift must keep the phase of Config::busArea");
// 読んだ時刻 t を、使う時刻に直す。limit は上限、past は今より前の時刻のための余裕 (どちらも t と同じ単位)。
// 上限以下ならそのまま。超えていれば past + (t を上限の半分で割った余り)
constexpr uint64_t stateKeptTime(uint64_t t, uint64_t limit, uint64_t past) {
    return t > limit ? past + t % (limit / 2) : t;
}

// 保存に要るバイト数 (このあと Machine を動かさずに saveState() を呼べば、この大きさになる)
size_t stateSize(Machine& m);
// ゼロページを省かない場合の上限。同じ機種・接続機器のままなら、実行中に増えない。
size_t stateSizeBound(Machine& m);
// out (cap バイト) に書く。足りなければ false
// version は試験用 (古い版の並びで書く。2 以上 STATE_VERSION 以下。版 11 までの SASI と SPC の状態は、バスフリーの形で書く)
bool saveState(Machine& m, uint8_t* out, size_t cap, size_t& written, uint32_t version = STATE_VERSION);
// data から読んで m に戻す。ヘッダ、機種、CRC、中身の並びをすべて確かめてから戻す
// (合わなければ m を変えずに false を返し、error に理由 (英語の短い文) を入れる)
bool loadState(Machine& m, const uint8_t* data, size_t size, const char*& error);
// 音の合成の状態 (OPM、ADPCM、フィルタ) を写す。to の出力の周波数、リング、記録の設定は変えない。
// ベアメタル版で音をコア 3 の Sound で作っているとき、読んだあとに Machine の Sound からコア 3 の Sound へ、
// 保存の前にコア 3 の Sound から Machine の Sound へ写す (state.cpp の説明を参照)
void copySoundState(const Sound& from, Sound& to);
// Video の内容を写す。to の記録の先 (setJournal()) は変えない。読んだあとに、コア 2 の写しを Machine の Video と
// 同じにするのに使う
void copyVideoState(const Video& from, Video& to);

// 詰め物: メンバを丸ごと書く io()、raw()、rawChecked() (state.cpp) は、メンバの間や終わりに詰め物のバイトがある型を
// 受け付けない (static_assert)。詰め物のバイトの値は決まっていない (メモリを 0 で埋めてから作っても、GCC は
// コンストラクタの前に書いた値を捨ててよいものとして memset() を消す) ので、入れると、同じ状態を保存しても違う
// バイト列になる。詰め物のできる並びのクラスには、同じ位置に uint8_t のメンバ (pad_ など。0 で始める) を置く。
// 並びは変えない (ステートのファイルの形が変わる)。float と double を持つ型もこの検査に通らない
// 状態の読み書き。SIZE は数えるだけ、SAVE は書く、CHECK は読むが何も変えない (並びの確認)、LOAD は読んで戻す。
// SHIFT は読み書きをせず、時刻で持っている値 (time()) だけを setTimeShift() の量だけ戻す (Machine::shiftTime()。
// 値は今のものをそのまま使うので、並びの確認は CHECK と同じに通る)
class StateIO {
public:
    enum Mode { SIZE, SAVE, CHECK, LOAD, SHIFT };
    StateIO(Mode mode, uint8_t* buf, size_t cap, uint32_t version = STATE_VERSION)
        : mode_(mode), buf_(buf), cap_(cap), version_(version) {}
    Mode mode() const { return mode_; }
    // 読んでいるファイルの版 (保存するときは STATE_VERSION)。古い版にない値は、版を見て読み飛ばす
    uint32_t version() const { return version_; }
    bool saving() const { return mode_ == SAVE || mode_ == SIZE; }
    bool applying() const { return mode_ == LOAD; }
    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }
    void countFullMemory() { fullMemory_ = true; }
    void fail() { ok_ = false; }
    // 理由を付けて読むのをやめる (loadState() のエラーの文になる。最初の理由だけを残す)
    void fail(const char* why) {
        if (ok_ && !why_) why_ = why;
        ok_ = false;
    }
    const char* why() const { return why_; }
    // CPU の時刻を上限 (STATE_CPU_CYCLES_MAX) に収めるために戻したサイクル数 (LOAD と SHIFT のときだけ。ふつうは 0)。
    // CPU の状態を読むところが決め、そのあとに読む、時刻で持っている値 (time()) を同じだけ戻す
    void setTimeShift(uint64_t cycles) { timeShift_ = cycles; }
    uint64_t timeShift() const { return timeShift_; }
    // CPU の時刻 (サイクル数) で持っている値。CPU の時刻を戻したら、同じだけ戻す (戻す量より小さければ 0)
    void time(uint64_t& v) {
        bytes(&v, sizeof v);
        if (((applying() && ok_) || mode_ == SHIFT) && timeShift_) v = v >= timeShift_ ? v - timeShift_ : 0;
    }

    // メンバの値 (n バイト)。CHECK では読み飛ばす
    void bytes(void* p, size_t n);
    // 読んだ値で並びが決まるもの (フラグ、長さ)。CHECK でも読む (p は呼んだ側の一時変数)
    void local(void* p, size_t n);
    template <class T> void io(T& v) {
        static_assert(__has_unique_object_representations(T), "io() needs a type without padding");
        bytes(&v, sizeof v);
    }
    // FPU の拡張精度の値。se と m の間の詰め物は 0 で書き、読むときは捨てる (上の「詰め物」の説明)
    void io(m68k::fp::X& v);
    template <size_t N> void io(m68k::fp::X (&v)[N]) {
        for (m68k::fp::X& x : v) io(x);
    }
    template <class T> void loc(T& v) { local(&v, sizeof v); }
    // bool。0 か 1 でなければ読まない (ほかの値を bool として読むと未定義の動作になる)。CHECK で確かめるので、
    // Machine を変える前に断る
    void io(bool& v) {
        uint8_t t = v;
        local(&t, 1);
        if (saving()) return;
        if (t > 1) fail();
        else if (applying()) v = t != 0;
    }
    // 添字や個数など、lo 以上 hi 以下でなければならない値。外れていれば読まない (CHECK で確かめる)。
    // 読んだ値を返す (CHECK ではメンバは変わらないので、続く値の範囲を決めるのに使う)
    template <class T> T range(T& v, T lo, T hi) {
        T t = v;
        local(&t, sizeof t);
        if (saving()) return t;
        if (t < lo || t > hi) fail();
        else if (applying()) v = t;
        return t;
    }
    // 列挙の値 (0 以上 count 未満)。外れていれば読まない
    template <class E> void en(E& v, int count) {
        __underlying_type(E) t = (__underlying_type(E))v;
        local(&t, sizeof t);
        if (saving()) return;
        const long long n = (long long)t;
        if (n < 0 || n >= count) fail();
        else if (applying()) v = (E)t;
    }
    // ポインタも仮想関数も持たないクラスを丸ごと
    template <class T> void raw(T& v) {
        static_assert(__is_trivially_copyable(T), "raw() needs a trivially copyable type");
        static_assert(__has_unique_object_representations(T), "raw() needs a type without padding");
        bytes(&v, sizeof v);
    }
    // 区切りの印。読むときに合わなければ失敗にする (並びのずれを早く見つける)
    void tag(uint32_t t);
    // 大きなメモリ。4KB ごとに、全部 0 なら 1 バイトの印だけにする
    void memory(uint8_t* p, size_t n);

private:
    Mode mode_;
    uint8_t* buf_;
    size_t cap_;
    uint32_t version_;
    size_t pos_ = 0;
    bool ok_ = true;
    const char* why_ = nullptr;
    uint64_t timeShift_ = 0;
    bool fullMemory_ = false;
};

} // namespace x68k
