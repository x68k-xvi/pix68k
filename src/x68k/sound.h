// 音: OPM と ADPCM を進めて混ぜ、出力のサンプリング周波数 (48kHz など) に直してリングバッファにためる。
//
// Machine は CPU が進んだ分のクロック (4MHz) を advance() に渡す。OPM の 1 サンプル (64 クロック、62.5kHz)
// ごとに ADPCM を進め、OPM の出力と ADPCM の出力を足し、直流を切る高域通過フィルタ (出力のコンデンサの代わり、
// 約 10Hz) と低域通過フィルタ (X68000 の出力のフィルタの代わり、1 次を 2 段) を通して、線形補間で出力の周波数に直す。
// ADPCM (MSM6258) の復号値は誤差がたまって 0 からずれたまま止まる (無音のデータを流し続けるドライバでは、
// そのずれが直流として残る)。実機の出力はコンデンサでつながっているので直流は出ない。
// 出力のフレームは AudioRing (1 つの書き手と 1 つの読み手のロックのないリングバッファ) にためる。
// 音を出す側は platform::AudioSource として別のスレッドやコアから取り出す。
//
// エミュレータの速さと音を出す側の速さ (水晶の誤差など) のずれは、リングバッファのたまり具合を見て
// 補間の刻みを少し (最大 0.5%) 変えて吸収する (setLatency() で目標を決めたときだけ)。
// 音を出す側はまとめて取り出すことがある (macOS の SDL では 100ms 分ほどを一度に取ることがあった) ので、
// 0.25 秒ごとのたまり具合の最小値が目標になるように合わせる。
// エミュレータが止まって書き手が遅れ、リングバッファが空になる (足りなくなる) ことがある。そのときは、鳴らし直す前に
// ためる量 (prebuffer、setLatency() の 2 倍) は変えずに、たまり具合の目標のほうに量を足していき (リングバッファの
// 半分まで)、エミュレータがときどき止まっても足りるようにする。足す量は、足りなくなってから鳴らし直すまでに無音で埋めたフレーム数 (少なくとも
// setLatency() の量)。120ms 止まったなら、1 回でほぼ 120ms 分を足す。目標の上限 (リングバッファの半分) を
// 超える分を埋めたとき (設定のメニューなどで長く止まったとき) は、setLatency() の量だけ足す。
// 30 秒間足りなくならなければ、足した量を 1/8 ずつ減らす。ただし減らすのは、その 30 秒のたまり具合の最小値が
// setLatency() の量を超えた分まで (ときどき止まってたまり具合が下がっているなら、足した量はまだ要るので減らさない)。
// この調整は Sound::tuneLatency() (目標に足す量) と Sound::output() (補間の刻み) にある。
// エミュレータが実時間に間に合わない間 (重い場面) は、作る音が足りないので、上の調整では途切れる。たまり具合が
// setLatency() の量の 3/4 を切ったら、補間の刻みを最大 25% 小さくして多くのフレームを作る (Sound::stretchIfLow())。
// その間、Sound が 62.5kHz のサンプルから作る音 (OPM、ADPCM、まーきゅりーゆにっと、CD-ROM 装置のオーディオ) は、
// 最大 25% 遅く、低く鳴る (25% で約 5 半音) が、途切れない。MIDI の音源 (MT-32、SF2、SC-55) は出力のフレームごとに
// 音を作るので、音の高さは変わらず、演奏が遅くなるだけ。OPM と MIDI を一緒に鳴らす曲では、その間だけ音の高さがずれる。
// エミュレータの遅れそのものは、この調整では縮まらない (取り戻すのは Machine を動かす側)。多く作った分は目標に足すので、
// 遅れを取り戻したあとは、足した分だけ多くたまり、音の遅延が長くなる (足した量は上の決まりで少しずつ減らす)。
// setLatency() を呼んでいれば働くので、ベアメタル版だけでなく、ホスト版の SDL の音でも同じになる
// (WAV に書くときは setLatency() を呼ばないので、働かない)。
// 出力の周波数が 0 (既定) なら音は作らない (OPM のタイマと ADPCM の転送の要求は動かす)。
//
// 音の合成を別のコア (スレッド) で行うとき (sound_replay.h): X68000 を動かす側の Sound は出力の周波数を 0 にし、
// CPU から見える状態 (OPM の BUSY、タイマ、割り込み、ADPCM の転送の要求と状態) だけを計算する。
// 同時に、OPM と ADPCM への書き込み、ADPCM のクロックと分周と左右の設定、リセット、進めたクロック数を
// 記録 (setJournal) に入れる。合成するコアの Sound は記録を順に当てて音を作る (SoundReplayer)。
// どちらも同じ順で同じ入力を受けるので、作る音は 1 つのコアのときと同じになる。
// まーきゅりーゆにっと (mercury.h) をつないでいれば、OPM の 1 サンプルごとに進めて、その PCM も混ぜる。
// まーきゅりーゆにっとの書き込み (コマンド、モード、データ) も記録し、合成するコアでは SoundReplayer が持つものに当てる。
// MIDI ボード (ym3802.h) が送り終えたバイトも記録する (J_MIDI)。記録の中の位置が送り終えた時刻になる。
// 合成するコアでは、SoundReplayer に渡した音源 (midi_out.h) に届ける。音源の音は出力のフレームごとに足す
// (X68000 の出力のフィルタは通さない。外につないだ音源の音なので)。
// CD-ROM 装置 (cd_drive.h) がオーディオのトラックを再生しているときは、Machine がそのフレーム (44.1kHz) を cdFrame() で
// 渡す。記録しているなら記録し (J_CD)、していなければ CdMixer に入れる。音を作る側は、OPM の 1 サンプルごとに CdMixer を
// 進め (62.5kHz に直す)、出力のフレームごとに、その値を 1/2 にして足す (音量の比はまーきゅりーゆにっとと同じ)。
// MIDI の音源と同じく、X68000 の出力のフィルタ (直流を切るフィルタと低域通過フィルタ) は通さない: 実機の CD-ROM 装置の
// オーディオは装置のオーディオの出力から出て、X68000 の音声の回路を通らない。
// 足して 16 ビットを超えた分は、上限と下限で止める (MIDI の音源を足したときと同じ)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

#include "platform/platform.h"
#include "x68k/cd_drive.h"
#include "x68k/video_journal.h"
#include "x68k/adpcm.h"
#include "x68k/mercury.h"
#include "x68k/midi_out.h"
#include "x68k/opm.h"

namespace x68k {

class StateIO;

class AudioRing : public platform::AudioSource {
public:
    static constexpr unsigned SIZE = 16384; // フレーム数 (2 のべき乗)。48kHz で 340ms

    // ---- 書き手 (エミュレータ) ----
    // 満杯なら false (捨てる)
    bool push(int16_t l, int16_t r);

    // ---- 読み手 (音を出す側) ----
    // たまっている分を返す (足りなくても埋めない)。WAV に書くときなど
    int drain(int16_t* out, int maxFrames);
    // 読み手・書き手を止めているときだけ使う、フロントエンドのステート用。
    int copyPending(int16_t* out, int maxFrames) const;
    void clearStopped();
    // platform::AudioSource: いつも frames フレームを返す。最初と、足りなくなったあとは、
    // prebuffer フレームたまるまで待つ (その間と足りない分は、直前の値から 0 へ少しずつ近づける)
    void readAudio(int16_t* out, int frames) override;
    // 鳴らし始める前と、足りなくなったあとにためるフレーム数 (SIZE - 1 まで)。書き手が決め、読み手が見る
    void setPrebuffer(unsigned frames) {
        const unsigned capped = frames < SIZE ? frames : SIZE - 1;
        __atomic_store_n(&prebuffer_, capped, __ATOMIC_RELAXED);
    }
    unsigned prebuffer() const {
        unsigned frames;
        __atomic_load(&prebuffer_, &frames, __ATOMIC_RELAXED);
        return frames;
    }
    // 読み手が鳴らしているか (prebuffer フレームたまるのを待っている間は false)。書き手が見る
    bool playing() const { return __atomic_load_n(&primed_, __ATOMIC_RELAXED); }
    // 足りなくなった回数 (読み手が数える)
    unsigned underruns() const { return __atomic_load_n(&underruns_, __ATOMIC_RELAXED); }
    // 足りなくなってから鳴らし直すまでに無音で埋めたフレーム数 (鳴らし直したときに足す) を返して 0 に戻す
    // (書き手が呼ぶ)。最初に鳴らし始める前に埋めた分は数えない
    unsigned takeMissing() { return __atomic_exchange_n(&missing_, 0u, __ATOMIC_RELAXED); }

    // どちらからも呼べる
    unsigned fill() const {
        return __atomic_load_n(&head_, __ATOMIC_ACQUIRE) - __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
    }

private:
    uint32_t buf_[SIZE]; // 下位 16 ビットが L、上位が R
    unsigned head_ = 0;  // 書き手だけが書く
    unsigned tail_ = 0;  // 読み手だけが書く
    bool primed_ = false; // 読み手だけが書く (playing())
    // 読み手だけが使う
    bool starved_ = false; // 足りなくなってから、まだ鳴らし直していない
    int32_t lastL_ = 0, lastR_ = 0;
    unsigned prebuffer_ = 0;
    unsigned underruns_ = 0;
    unsigned starvedFrames_ = 0; // 足りなくなってから無音で埋めたフレーム数
    unsigned missing_ = 0; // 読み手が足し、書き手が takeMissing() で取る
};

class Sound {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    void copyStateFrom(const Sound& from); // x68k::copySoundState()
    static constexpr uint32_t OPM_RATE = 4000000 / Opm::CLOCKS_PER_SAMPLE; // 62500

    Sound() { setOutputRate(0); }
    void reset();
    // 出力のサンプリング周波数。0 なら音を作らない
    void setOutputRate(int hz);
    int outputRate() const { return rate_; }
    // リングバッファのたまり具合の最小値の目標 (フレーム)。音を出し始める前にはこの 2 倍をため、
    // その後はたまり具合の最小値がこの量になるよう、作る速さを少し変える。0 なら速さを変えない (WAV に書くときなど)
    void setLatency(unsigned frames) {
        __atomic_store_n(&target_, frames, __ATOMIC_RELAXED);
        ring_.setPrebuffer(frames * 2);
        setExtra(0); // 足した量は捨てる
        ring_.takeMissing();
        lastUnderruns_ = ring_.underruns(); // ここから数える
        restartCalm();
    }
    // 今のたまり具合の最小値の目標 (setLatency() の量に、足りなくなったときなどに足した量を加えたもの)。
    // ほかのコアやスレッドからログ用に読んでよい (おおよその値)。target_ と extra_ は、読むのも書くのもいつも
    // アトミックにする (target()、extra()、setExtra())。書くのは setLatency() と音を作る側だけ。setLatency() は、
    // 音を作り始める前に呼ぶ (ベアメタル版は音を作るコアを動かす前、ホスト版は SDL の音を始める前)
    unsigned latencyGoal() const { return target() + extra(); }
    // 出力の大きさ (ログ用)。前に takeLevels() で取ってからの、
    //   base:    MIDI の音源を足す前の音 (OPM、ADPCM、まーきゅりーゆにっと、CD-ROM 装置のオーディオ) の絶対値の最大
    //   midi:    MIDI の音源が 1 フレームに足した量の絶対値の最大
    //   clipped: 足して 16 ビットを超え、上限か下限で止めたフレームの数
    // 音が割れるという報告のときに、振り切れているのか、どの音源が大きいのかを実機で見分けるためのもの。
    // ほかのコアやスレッドから呼んでよい (音を作る側はアトミックに書く)。clipped は 1 つずつアトミックに足すので、
    // 取るのと重なっても、消えることも 2 つの区間に入ることもない。最大は、取るのと重なった 1 フレームが次の区間に
    // 入ることがある。3 つを同時には取らないので、区間の境目は 1 フレームほどずれる
    struct Levels {
        uint32_t base, midi, clipped;
    };
    Levels takeLevels() {
        return {__atomic_exchange_n(&levelBase_, 0u, __ATOMIC_RELAXED), __atomic_exchange_n(&levelMidi_, 0u, __ATOMIC_RELAXED),
                __atomic_exchange_n(&levelClipped_, 0u, __ATOMIC_RELAXED)};
    }
    // 補間の刻みを、出力の周波数から決まる値よりどれだけ小さくしているか (千分率。大きくしているときは 0)。
    // 5 までは速さのずれの調整、それより大きければ間に合わない間の調整 (stretchIfLow())。音を作る側が読む (試験用)
    unsigned stretchPermille() const {
        return step_ < baseStep_ ? (unsigned)((uint64_t)(baseStep_ - step_) * 1000 / baseStep_) : 0;
    }
    // OPM のクロック (4MHz) で clocks だけ進める。mercury はつないだまーきゅりーゆにっと (なければ nullptr)。
    // midi は MIDI の音源 (なければ nullptr)。音を作るときは、出力のフレームごとにその音を足す。
    // まーきゅりーゆにっとと音源は Sound の中に持たない (Machine と SoundReplayer が持つ。Machine の中の並びを変えないため)
    // cd は CD-ROM 装置のオーディオ (なければ nullptr)。これも Sound の中には持たない
    void advance(uint32_t clocks, Mercury* mercury = nullptr, MidiOut* midi = nullptr, CdMixer* cd = nullptr) {
        if (journal_) journalAdvance(clocks);
        advanceLocal(clocks, mercury, midi, cd);
    }
    // CPU からの書き込みと、ほかの機器から決まる設定 (Machine はこれらを通して書く。記録するため)
    void opmWrite(uint32_t offset, uint8_t v);
    void adpcmWrite(uint32_t offset, uint8_t v);
    // ADPCM のクロック (OPM の CT2、true = 4MHz) と、8255 のポート C の左右と分周
    void setAdpcmLines(bool clock4M, uint8_t pan, uint8_t rate);
    // まーきゅりーゆにっと (mercury.h) への CPU からの書き込み (off は $ECC000 からの位置)。記録してから m に書く
    void mercuryWrite(Mercury& m, uint32_t off, uint8_t v);
    // まーきゅりーゆにっとをつないだ / 外した (記録の読み手にも同じものを置かせる)
    void mercuryAttached(bool on) {
        mercuryOn_ = on;
        journalMercury(on ? 0 : 1, 0);
    }
    bool mercuryOn() const { return mercuryOn_; }
    // OPM の 1 サンプルに足りず、まだ OPM、ADPCM、まーきゅりーゆにっとに渡していない 4MHz のクロック
    uint32_t pendingClocks() const { return sub_; }
    // CD-ROM 装置をつないだ / 外した (Machine が、音を進めるときに装置を見るかを決める)
    void setCdOn(bool on) { cdOn_ = on; }
    bool cdOn() const { return cdOn_; }
    // CD-ROM 装置が再生したオーディオの 1 フレーム (44.1kHz)。記録しているなら記録し、していなければ cd に入れる。
    // 記録は、前のフレームとの差が左右とも 14 ビットに入れば 1 個 (J_CD_DELTA)、入らなければ 2 個 (J_CD) にする
    // (1 秒に 44100 フレームあるので、記録のリングを早く埋めないように)。前のフレームは cd に覚える
    // (読み手の CdMixer と同じときにリセットされること。cd が nullptr なら、いつも 2 個)。
    // 前提: CD-ROM 装置 (CdDrive とその CdMixer) は、SoundLink::attach() の前に作り、記録している間は作り直さない。
    // 記録している間に作り直すと (Spc::attach() で CD-ROM 装置を外してつなぎ直すなど)、記録する側の「前のフレーム」だけが
    // 0 に戻り、読み手とずれて音が変わる。今の環境は起動のときにしか CD-ROM 装置をつながない (媒体の入れ替えの
    // Spc::changeCd() は装置を作り直さない)
    void cdFrame(CdMixer* cd, int16_t l, int16_t r) {
        if (journal_) {
            flushPending();
            int dl = 0, dr = 0;
            if (cd && cd->delta(l, r, dl, dr)) {
                journal_->push(J_CD_DELTA << 28 | ((uint32_t)dl & 0x3fff) << 14 | ((uint32_t)dr & 0x3fff));
            } else {
                journal_->push(J_CD << 28 | (uint16_t)l);
                journal_->push(J_CD << 28 | 0x10000 | (uint16_t)r);
            }
        } else if (cd) {
            cd->push(l, r);
        }
    }
    // MIDI ボードが送り終えたバイト。記録しているなら記録し、していなければ out (なければ捨てる) に渡す
    void midiWrite(MidiOut* out, uint8_t b) {
        if (journal_) {
            flushPending();
            journal_->push(J_MIDI << 28 | b);
        } else if (out) {
            out->midiByte(b);
        }
    }
    // 音源の音をすべて止める (X68000 のリセット、ステートを読んだとき)。記録のしかたは midiWrite() と同じ
    void midiReset(MidiOut* out) {
        if (journal_) {
            flushPending();
            journal_->push(J_MIDI << 28 | 0x100);
        } else if (out) {
            out->midiReset();
        }
    }

    // ---- 合成を別のコアで行うとき (sound_replay.h) ----
    // 記録の形 (32 ビット): bit31-28 が種類。9..15 は使っていない
    enum JournalOp : uint32_t {
        J_ADVANCE = 0, // bit27-0: 進めた 4MHz のクロック数
        J_OPM = 1,     // bit9-8: オフセットの下位 2 ビット、bit7-0: 値
        J_ADPCM = 2,   // 同上
        J_LINES = 3,   // bit4: clock4M、bit3-2: rate、bit1-0: pan
        J_RESET = 4,
        J_MIDI = 5,    // bit8: 1 なら音源の音をすべて止める、0 なら bit7-0 の MIDI のバイトを送る
        J_CD = 6,      // CD-ROM 装置のオーディオ。bit16: 0 なら左のサンプル (bit15-0)、1 なら右のサンプル (これで 1 フレーム)
        J_CD_DELTA = 7, // CD-ROM 装置のオーディオの 1 フレーム。前のフレームとの差 (bit27-14 が左、bit13-0 が右。14 ビットの符号つき)
        J_MERCURY = 8, // bit27-24: 0 つなぐ、1 外す、2 コマンド (bit7-0)、3 DMAC のモード (bit7-0)、4 データ (bit15-0)
    };
    // 進めたクロック数をまとめて記録する上限 (4MHz で 1ms)。これを超えたら記録して読み手に見せる
    static constexpr uint32_t JOURNAL_ADVANCE_CHUNK = 4000;
    // 記録を始める (nullptr でやめる)。記録する側は出力の周波数を 0 にしておくこと。
    // 読み手の Sound と同じ状態 (どちらも作ったばかりか、リセットの直前) のときに呼ぶ
    void setJournal(VideoJournal* journal) {
        journal_ = journal;
        pending_ = 0;
        lines_ = ~0u; // 新しい読み手には、最初の setAdpcmLines() を必ず記録する
    }
    // まとめているクロック数を記録して、読み手に見せる (記録していなければ何もしない)
    void flushJournal();
    // 記録を 1 つ当てる (読み手の側)。mercury は読み手のまーきゅりーゆにっと (つなぐ / 外す記録で作る / 消す)、
    // midi は読み手の MIDI の音源 (なければ nullptr。MIDI のバイトは捨てる)
    // cd は読み手の CD-ROM 装置のオーディオ (なければ nullptr。フレームは捨てる)
    void replay(uint32_t e, Mercury*& mercury, MidiOut* midi = nullptr, CdMixer* cd = nullptr);

    Opm& opm() { return opm_; }
    // OPM のタイマが次に割り込みを要求するまでのクロック数 (advance() にこの数を渡すと、ちょうど要求が出る)。
    // 要求を出すタイマがなければ (もう出ているときも) 0
    uint32_t clocksToOpmIrq() const {
        const uint32_t n = opm_.samplesToIrq();
        if (n == 0) return 0;
        return (n - 1) * (uint32_t)Opm::CLOCKS_PER_SAMPLE + ((uint32_t)Opm::CLOCKS_PER_SAMPLE - sub_);
    }
    Adpcm& adpcm() { return adpcm_; }
    AudioRing& ring() { return ring_; }
    // 同期出力 (setLatency を使わない) のフロントエンドが、補間位置を保存・復元する。
    uint32_t outputPosition() const { return pos_; }
    bool restoreOutputPosition(uint32_t pos) {
        if (target() || !baseStep_ || pos >= baseStep_) return false;
        pos_ = pos;
        step_ = baseStep_;
        return true;
    }

private:
    void resetLocal();
    void advanceLocal(uint32_t clocks, Mercury* mercury, MidiOut* midi, CdMixer* cd);
    void journalAdvance(uint32_t clocks) {
        pending_ += clocks;
        sincePublish_ = (uint16_t)(sincePublish_ + (clocks < JOURNAL_ADVANCE_CHUNK ? clocks : JOURNAL_ADVANCE_CHUNK));
        if (pending_ >= JOURNAL_ADVANCE_CHUNK || sincePublish_ >= JOURNAL_ADVANCE_CHUNK) flushJournal();
    }
    void flushPending() {
        if (pending_) {
            journal_->push(J_ADVANCE << 28 | pending_);
            pending_ = 0;
        }
    }
    void output(int32_t l, int32_t r, MidiOut* midi, CdMixer* cd);
    // 遅れを吸収するために刻みを小さくする割合の上限 (1/4 = 25%。実時間の 75% の速さまで途切れない)
    static constexpr uint32_t STRETCH_MAX_DIV = 4;
    void stretchIfLow(); // 1/32 秒ごとに、たまり具合が少なければ刻みを小さくする (sound.cpp)
    void tuneLatency(); // 0.25 秒ごとに、足りなくなったかを見て目標に足す量を決める (sound.cpp)
    // 足りなくならない区間の数と、その間のたまり具合の最小値を初めの値に戻す
    void restartCalm() { calm_ = 0; calmMin_ = UINT16_MAX; }
    // 出力の 1 フレームの大きさを記録する (takeLevels())。bl / br は MIDI の音源を足す前、l / r は足したあと (止める前)
    void noteLevels(int32_t bl, int32_t br, int32_t l, int32_t r) {
        const auto mag = [](int32_t a, int32_t b) {
            const uint32_t x = (uint32_t)(a < 0 ? -(int64_t)a : a), y = (uint32_t)(b < 0 ? -(int64_t)b : b);
            return x > y ? x : y;
        };
        const uint32_t base = mag(bl, br), midi = mag(l - bl, r - br);
        if (base > __atomic_load_n(&levelBase_, __ATOMIC_RELAXED)) __atomic_store_n(&levelBase_, base, __ATOMIC_RELAXED);
        if (midi > __atomic_load_n(&levelMidi_, __ATOMIC_RELAXED)) __atomic_store_n(&levelMidi_, midi, __ATOMIC_RELAXED);
        if (l > 32767 || l < -32768 || r > 32767 || r < -32768)
            __atomic_fetch_add(&levelClipped_, 1u, __ATOMIC_RELAXED); // 読んで足して書くと、取るのと重なったときに前の区間の数が残る
    }
    // extra_ を書く (latencyGoal() をほかのコアが読むので、アトミックに書く)
    void setExtra(unsigned frames) { __atomic_store_n(&extra_, frames, __ATOMIC_RELAXED); }
    unsigned target() const { return __atomic_load_n(&target_, __ATOMIC_RELAXED); }
    unsigned extra() const { return __atomic_load_n(&extra_, __ATOMIC_RELAXED); }
    // 0.25 秒と 1/32 秒のフレーム数 (出力の周波数が低くても 1 以上。0 で割らないため)
    unsigned quarterFrames() const { return (unsigned)rate_ >= 4 ? (unsigned)rate_ / 4 : 1; }
    unsigned shortFrames() const { return (unsigned)rate_ >= 32 ? (unsigned)rate_ / 32 : 1; }

    Opm opm_;
    Adpcm adpcm_;
    AudioRing ring_;
    int rate_ = 0;
    uint32_t sub_ = 0;           // OPM の 1 サンプルの中のクロック
    int32_t lp1_[2] = {}, lp2_[2] = {}; // フィルタ
    int64_t hpOut_[2] = {};             // 直流を切るフィルタの出力 (2^20 倍)
    int32_t hpIn_[2] = {};              // 直流を切るフィルタの前の入力
    int32_t prev_[2] = {};       // 補間の前のサンプル
    uint32_t pos_ = 0;           // 補間の位置 (16.16、前のサンプルからの距離)
    uint32_t step_ = 0, baseStep_ = 0; // 出力 1 フレームあたりに進む OPM のサンプル数 (16.16)
    unsigned target_ = 0;        // setLatency() の量 (読むときは target())
    uint32_t adjustCount_ = 0;   // 0.25 秒の区切りまでに作ったフレーム数 (出力の周波数の 1/4 まで数えるので 32 ビット)
    // 次の 2 つは合わせて 4 バイト (前は minFill_ が 4 バイト。Sound の大きさと並びを変えない)。どちらも
    // たまり具合 (AudioRing::SIZE 以下) なので 16 ビットで足りる
    uint32_t levelBase_ = 0, levelMidi_ = 0, levelClipped_ = 0; // takeLevels() の説明を参照 (ステートには入れない)
    uint16_t minFill_ = UINT16_MAX;  // 今の区間 (0.25 秒) のたまり具合の最小値
    uint16_t shortMin_ = UINT16_MAX; // この 1/32 秒のたまり具合の最小値 (stretchIfLow())
    unsigned extra_ = 0;         // 目標に足している量 (フレーム。読むときは extra()、書くときは setExtra())
    unsigned lastUnderruns_ = 0; // 前に見たときの AudioRing::underruns()
    uint16_t calm_ = 0;      // 足りなくならなかった区間 (0.25 秒) の数
    uint16_t calmMin_ = 0xffff; // 足りなくならなかった間のたまり具合の最小値 (AudioRing::SIZE 以下なので 16 ビットで足りる)
    // まーきゅりーゆにっとをつないでいるか (Machine が advance() に渡すかを決める。次のポインタの前の隙間に置き、
    // Sound の大きさと並びを変えない)
    bool mercuryOn_ = false;
    bool cdOn_ = false; // CD-ROM 装置をつないでいるか (これも隙間に置く)
    // 前に読み手に見せてから (publish()) 進めたクロック数。まーきゅりーゆにっとの書き込みは頻繁で、そのたびに
    // pending_ を記録して 0 にするので、1ms ごとに見せるのはこちらで決める (これも上の隙間に置く)
    uint16_t sincePublish_ = 0;
    VideoJournal* journal_ = nullptr; // 記録する先 (合成を別のコアで行うとき)
    uint32_t pending_ = 0;            // まだ記録していない、進めたクロック数
    uint32_t lines_ = ~0u;            // 最後に記録した J_LINES の値
    void journalMercury(uint32_t sub, uint32_t value) {
        if (!journal_) return;
        flushPending();
        journal_->push(J_MERCURY << 28 | sub << 24 | value);
    }
};

} // namespace x68k
