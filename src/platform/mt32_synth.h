// MT-32 / CM-32L の MIDI 音源。Munt (external/munt/mt32emu、LGPL 2.1 以降) で鳴らす。
// MIDI ボード (x68k/ym3802.h) が送ったバイトを x68k::MidiOut として受け、出力のフレームごとに音を足す。
//
// ROM は利用者が用意する。CONTROL (64KB か 128KB) と PCM (512KB か 1MB) のファイルを 1 つずつ渡す (MAME の
// 2 つに分かれたもの (_a / _b、_l / _h) は受けない)。Munt が大きさと SHA-1 で種類を調べ、知らないもの、
// CONTROL と PCM が逆のもの、組み合わせが合わないもの (MT-32 の CONTROL と CM-32L の PCM など) は読まない。
//
// 受けるバイト: チャネルメッセージとシステムエクスクルーシブを Munt の MIDI の待ち行列に入れる (Synth::playMsg()、
// playSysex())。システムエクスクルーシブは SYSEX_MAX バイト ($F0 と $F7 を含む) までで、超えたものは捨てる
// (MidiStream (64 バイトまで) は使わない。MT-32 の音色の書き込みは 1 つで 250 バイトほどある)。
// $F7 なしに次のステータスのバイトが来たら、そこで終わったものとして $F7 を足して渡す。リアルタイムのバイトは捨てる。
// システムコモン ($F1-$F6) はデータのバイトを読み飛ばす。
//
// 時刻: 音は BLOCK フレームずつまとめて作る。受けたメッセージには、Munt の中の時刻 (32kHz) で「作り終えた位置 +
// 今のまとまりの中で出したフレームの数」を付ける。すべてのメッセージが 1 まとまり (48kHz で 1.3ms) 遅れて鳴り、
// 間隔は保たれる。YM3802 が送り終えた時刻に届くので、Munt の MIDI の転送の遅れ (MIDIDelayMode) は足さない。
//
// 出力の周波数: Munt の中は 32kHz。SampleRateConverter::getBestAnalogOutputMode() で出力に近いアナログの出力の
// やり方を選び、周波数が同じならそのまま、違えば Munt の SampleRateConverter (中の変換) で変える。48kHz では
// AnalogOutputMode_ACCURATE (Munt のアナログの低域フィルタを 48kHz で通す) になり、変換はしない。
// ベアメタル版は変換 (Munt の srchelper) を入れていない (RTTI を使うため) ので、Munt がそのまま出せる周波数
// (32kHz、48kHz、96kHz) だけを受ける (出力は 48kHz)。
//
// メモリ: Munt は new で確保する (TinySoundFont の TSF_MALLOC のような差し替えの口がない)。ベアメタルでは、
// load() の前に heapBytes() の量がヒープに残っているかを確かめる (Circle の new は足りないと全体を止めるため)。
// heapBytes() が実際の確保より多いことは、tests/unit/mt32_test.cpp で operator new を数えて確かめる。
// load() のあとは確保しない: リバーブの 4 つのモードのバッファは先に確保し (preallocateReverbMemory())、
// システムエクスクルーシブは先に確保した QUEUE_SYSEX_BYTES の中に置く (configureMIDIEventQueueSysexStorage())。
//
// 状態はステートに入れない。ステートを読んだときは、Machine が x68k/midi_shadow.h で設定を送り直す
// (midiFamily() が MT32 なので、GS のリセットやバンクセレクトは送らない)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "x68k/midi_out.h"

namespace MT32Emu {
class Synth;
class SampleRateConverter;
class ROMImage;
class ArrayFile;
} // namespace MT32Emu

namespace platform {

class Mt32Synth : public x68k::MidiOut {
public:
    static constexpr int BLOCK = 64;
    static constexpr int SYSEX_MAX = 4096;
    static constexpr uint32_t QUEUE_SYSEX_BYTES = 32768;

    enum Result {
        OK,
        BAD_CONTROL, // CONTROL の ROM の大きさが違うか、Munt の知らないもの
        BAD_PCM,     // PCM の ROM の大きさが違うか、Munt の知らないもの
        MISMATCH,    // CONTROL と PCM の組み合わせが合わない
        OPEN_FAILED, // Munt が開けなかった
        BAD_RATE,    // 出力の周波数に変えられない (変換を入れていないビルドで、Munt の出力と違う周波数)
    };
    static const char* resultText(Result r);

    // ROM のファイルの大きさとして受けるもの (読む前に断るため)
    static bool controlSizeOk(uint64_t n) { return n == 65536 || n == 131072; }
    static bool pcmSizeOk(uint64_t n) { return n == 524288 || n == 1048576; }
    // load() に渡す ROM のバッファと、load() の中で Munt が確保する量の合計の上限 (バイト)。
    // ベアメタルの確保の切り上げ (ヘッダとバケット) を含めても超えないよう、多めにしてある
    static uint64_t heapBytes(uint64_t controlSize, uint64_t pcmSize) {
        return controlSize + pcmSize * 2 + (4u << 20);
    }

    Mt32Synth();
    ~Mt32Synth() override;
    Mt32Synth(const Mt32Synth&) = delete;
    Mt32Synth& operator=(const Mt32Synth&) = delete;

    // ROM を調べて Munt を開く。control と pcm は new[] で確保したもので、load() が引き取る (失敗しても解放する)。
    // rate は出力のサンプリング周波数。一度だけ呼ぶ。
    // controlSha1 / pcmSha1 は試験用: nullptr でなければ、ファイルの SHA-1 を計算せずにこの値 (16 進の 40 文字) とみなす
    // (ROM のない環境で、中身が 0 の ROM を Munt に読ませるため)
    Result load(uint8_t* control, size_t controlSize, uint8_t* pcm, size_t pcmSize, int rate,
                const char* controlSha1 = nullptr, const char* pcmSha1 = nullptr);
    bool loaded() const { return synth_ != nullptr; }
    // 読んだ ROM の名前 ("MT-32 Control v1.07" など。読んでいなければ "")
    const char* controlName() const;
    const char* pcmName() const;
    // 音量 (Munt の setOutputGain() と setReverbOutputGain()。1.0 で Munt の既定)
    void setGain(float gain);
    // 長すぎて捨てたシステムエクスクルーシブと、待ち行列が満杯で捨てたメッセージの数
    // ほかのコアやスレッドからログ用に読んでよい (音を作る側が数えるので、読むのも足すのもアトミックにする)
    uint32_t droppedSysex() const { return __atomic_load_n(&droppedSysex_, __ATOMIC_RELAXED); }
    uint32_t droppedMessages() const { return __atomic_load_n(&droppedMessages_, __ATOMIC_RELAXED); }
    // 試験用: MT-32 のメモリ (システムエクスクルーシブのアドレス) を読む
    bool readMemory(uint32_t address, uint32_t length, uint8_t* out) const;
    // 試験用: 鳴っているパーシャルの数
    int activePartials() const;

    // ---- x68k::MidiOut ----
    void midiByte(uint8_t b) override;
    void midiReset() override;
    Family midiFamily() const override { return MT32; }
    void mixFrame(int32_t& l, int32_t& r) override {
        if (!synth_) return;
        if (pos_ >= BLOCK) render();
        l += buf_[2 * pos_];
        r += buf_[2 * pos_ + 1];
        pos_++;
    }

    // 試験用: n フレームの音を作って out (ステレオ、16 ビット) に書く (mixFrame() を n 回呼ぶのと同じ)
    void renderFrames(int16_t* out, int n);

private:
    void render();
    void freeAll();
    uint32_t timestamp() const;
    void sendShort(uint32_t msg);
    void endSysex();

    MT32Emu::Synth* synth_ = nullptr;
    MT32Emu::SampleRateConverter* src_ = nullptr;
    MT32Emu::ArrayFile* controlFile_ = nullptr;
    MT32Emu::ArrayFile* pcmFile_ = nullptr;
    const MT32Emu::ROMImage* controlImage_ = nullptr;
    const MT32Emu::ROMImage* pcmImage_ = nullptr;
    uint8_t* controlData_ = nullptr;
    uint8_t* pcmData_ = nullptr;
    struct Reporter;
    Reporter* reporter_ = nullptr;
    bool direct_ = false; // 出力の周波数が Munt の出力と同じ (変換しない)
    int rate_ = 0;

    // バイトの読み方 (上の説明)
    uint8_t status_ = 0;
    uint8_t need_ = 0, have_ = 0, skip_ = 0;
    uint8_t data_[2] = {};
    bool inSysex_ = false;
    bool sysexOver_ = false;
    int sysexLen_ = 0;
    uint8_t sysex_[SYSEX_MAX];
    uint32_t droppedSysex_ = 0;    // 読むのは droppedSysex()、足すのは __atomic_fetch_add()
    uint32_t droppedMessages_ = 0; // 読むのは droppedMessages()、足すのは __atomic_fetch_add()

    int pos_ = BLOCK;
    int16_t buf_[2 * BLOCK] = {};
    float fbuf_[2 * BLOCK] = {};
};

} // namespace platform
