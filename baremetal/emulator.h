// ベアメタル版で X68000 を動かす部分。コア 1 が run() で X68000 を実時間で動かし、
// コア 2 が runRender() で画面を合成し、コア 3 が runSound() で音 (OPM と ADPCM) を合成する
// (render_core=off / sound_core=off なら、コア 1 がそれも行う)。
// コア 0 (画面、USB、SD カード) とは下のやり方でデータを受け渡す。
//   入力:     コア 0 (USB の割り込み) → コア 1。ロックのない 1 対 1 のリングバッファ (キー、マウス、ジョイスティック)
//   LED:      コア 1 がフレームの切れ目でキーボードの LED の状態を置き、コア 0 が USB キーボードに送る
//   合成:     コア 1 → コア 2。Video への書き込みとラスタの区切りを記録したリング (x68k/video_replay.h)。
//             コア 2 は Video の写しに記録を当てながら 1 ラスタずつ描く。リングが満杯ならコア 1 が待つ
//   音の合成: コア 1 → コア 3。OPM と ADPCM への書き込み、ADPCM の設定、進めたクロック数を記録したリング
//             (x68k/sound_replay.h)。コア 1 の Sound は CPU から見える状態 (タイマ、割り込み、BUSY、ADPCM の
//             転送の要求) だけを計算し、コア 3 の Sound が記録を当てて音を作る。リングが満杯ならコア 1 が待つ
//   画面:     コア 2 (render_core=off ならコア 1) → コア 0。3 枚のバッファを atomic な交換で回す
//             (書き手と読み手が待たない)
//   SRAM:     コア 1 がフレームの切れ目で写しを作り、コア 0 がファイルに書く
//   ディスク: コア 1 がフレームの切れ目で、X68000 が書いたチャンクを写し、コア 0 がイメージのファイルに書く
//             (platform/disk_sync.h の DiskChannel。コア 0 が頼んだときだけ写す)
//   テキスト画面のダンプ: コア 0 が頼み、コア 1 がフレームの切れ目で読んで渡す
//   一時停止: コア 0 が頼み (requestPause())、コア 1 はフレームの切れ目でコア 2 / 3 への記録を処理し終えるまで待ってから
//             止まり、止まったことを返す (paused())。止まっている間は、コア 0 が Machine を触ってよい
//             (設定メニューのディスクの入れ替え、リセット、ステートの保存と読み込み)
//   音:       コア 3 (sound_core=off ならコア 1 の Machine の Sound) → コア 0 (音のデバイスの割り込み)。
//             ロックのない 1 対 1 のリングバッファ
// コア 1 が触るのは Machine、ここのバッファと atomic な変数だけ。コア 2 が触るのは Video の写し、Renderer、
// 画面のバッファだけ。
// FatFs、ログ、USB、ヒープ (Circle の new はスピンロックを使う) はコア 0 だけが使う。
#pragma once

#include <stdint.h>

#include "platform/frame_handoff.h"
#include "platform/core_work.h"
#include "platform/interval_handoff.h"
#include "x68k/machine.h"
#include "x68k/render.h"
#include "x68k/textscreen.h"
#include "x68k/sound_replay.h"
#include "x68k/video_replay.h"

namespace platform {
class DiskChannel;
}

// 1 つの書き手と 1 つの読み手のための、ロックのないリングバッファ
class InputRing {
public:
    // 書き手 (コア 0)。満杯なら false (捨てる)
    bool push(uint32_t v);
    // 読み手 (コア 1)。空なら false
    bool pop(uint32_t& v);

private:
    static constexpr unsigned SIZE = 256; // 2 のべき乗
    uint32_t buf_[SIZE];
    unsigned head_ = 0; // 書き手だけが書く
    unsigned tail_ = 0; // 読み手だけが書く
};

class Emulator {
public:
    struct Frame {
        uint32_t* pixels = nullptr; // XRGB8888、1 行 PITCH 画素
        int width = 0, height = 0;
    };
    static constexpr int PITCH = x68k::Renderer::MAX_WIDTH;

    Emulator() {}
    ~Emulator();
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    // ---- コア 0 で、コア 1 を動かす前に呼ぶ ----
    // cg は nullptr でもよい (文字が出ず、テキストのダンプもできない)。メモリが足りなければ false。
    // renderCore なら画面の合成をコア 2 で (コア 2 で runRender() を呼ぶこと)、soundCore なら音の合成を
    // コア 3 で行う (コア 3 で runSound() を呼ぶこと)。scsiRom は X68030 / 060turbo の $FC0000 からの ROM
    // (SCSI_ROM_SIZE バイト)。nullptr なら IPL ROM から作る
    bool init(const uint8_t* ipl, const uint8_t* cg, int64_t localSeconds, const x68k::Config& config = x68k::Config(),
              bool renderCore = true, bool soundCore = true, const uint8_t* scsiRom = nullptr);
    x68k::Machine& machine() { return *machine_; }
    bool initTs6bga(uint32_t base);
    const x68k::Ts6bga* takeTs6bga() {
        const int i = tsHandoff_.take();
        return i < 0 ? nullptr : tsFrames_ + i;
    }

    // ---- コア 1 ----
    // 電源を入れ、フレームごとに実時間に合わせて動かし続ける (戻らない)。
    // 合成をコア 2 / コア 3 で行う設定なら、まずそのコアが runRender() / runSound() に入るのを待つ。
    // 1 秒待っても入らなければ、コア 1 で合成する (renderCoreActive() / soundCoreActive() が false になる)
    void run();

    // ---- コア 2 ----
    // コア 1 から届く記録を処理し、画面を合成し続ける (戻らない)。合成をコア 2 で行わない設定ならすぐ戻る
    void runRender();
    // 合成をコア 2 で行っているか (コア 1 が run() で決めたあとに意味を持つ)
    bool renderCoreActive() const { return __atomic_load_n(&renderCoreActive_, __ATOMIC_ACQUIRE) != 0; }
    bool renderCoreRequested() const { return replayer_ != nullptr; }
    // コア 1 が、どちらのコアで合成するかを決め終えたか
    bool renderCoreDecided() const { return __atomic_load_n(&renderCoreGo_, __ATOMIC_ACQUIRE) != 0; }

    // ---- コア 3 ----
    // コア 1 から届く記録を処理し、音を合成し続ける (戻らない)。音の合成をコア 3 で行わない設定ならすぐ戻る
    void runSound();
    bool soundCoreActive() const { return __atomic_load_n(&soundCoreGo_, __ATOMIC_ACQUIRE) == 1; }
    // コア 1 が、音をどちらのコアで合成するかを決め終えたか
    bool soundCoreDecided() const { return __atomic_load_n(&soundCoreGo_, __ATOMIC_ACQUIRE) != 0; }
    bool soundCoreRequested() const { return soundReplayer_ != nullptr; }
    // 音を出すか (enableSound() で周波数を決めたか)。出さないなら sound_core=on でもコア 1 が計算する
    bool soundOutputEnabled() const { return soundRate_ != 0; }
    // コア 3 が wfe から起きた回数と、そのうち記録がなかった (ほかのコアへの sev で起きた) 回数
    void soundCoreWakes(uint32_t& wakes, uint32_t& empty) const {
        wakes = __atomic_load_n(&soundWakes_, __ATOMIC_RELAXED);
        empty = __atomic_load_n(&soundEmptyWakes_, __ATOMIC_RELAXED);
    }

    // コア 2 が画面の合成に、コア 3 が音の合成に使った時間 (ログ用)。which は 0 が画面、1 が音。
    // 初めて呼ぶと区間の区切りを頼んで false を返す。合成するコアが次のフレームの終わりで区切ったあとに呼ぶと、
    // 前に受け取ってからの区間 (合計の時間、フレームの数、1 フレームの最大) を返して true (platform/core_work.h)
    bool takeCoreWork(int which, platform::CoreWork& out) { return (which == 0 ? renderWork_ : soundWork_).take(out); }

    // ---- コア 0 ----
    // 入力をコア 1 に送る。リングバッファが満杯なら false (呼んだ側が後で送り直す)。
    // 書き手は 1 つでなければならないので、USB の割り込みか、割り込みを止めたコア 0 から呼ぶ
    // USB HID のキーが押された / 離された
    // setDiscardInput(true) の間は送らずに捨て、true を返す (X68000 をずっと止めておくとき。キューが一杯にならないように)
    bool keyEvent(uint8_t usage, bool down) { return discard() || input_.push(EV_KEY | usage | (down ? 0x100u : 0u)); }
    // マウス: 移動量は ±2047 まで (platform::MouseTracker で切り分ける)、buttons は InputSink::MOUSE_*
    bool mouseEvent(int dx, int dy, uint8_t buttons) {
        return discard() || input_.push(EV_MOUSE | (uint32_t)(buttons & 7) << 24 | ((uint32_t)dy & 0xfff) << 12 | ((uint32_t)dx & 0xfff));
    }
    // ジョイスティック (port は 0 か 1、bits は InputSink::JOY_*)
    bool joystickEvent(int port, uint8_t bits) { return discard() || input_.push(EV_JOY | (uint32_t)(port & 1) << 8 | bits); }
    void setDiscardInput(bool on) { __atomic_store_n(&discardInput_, on ? 1u : 0u, __ATOMIC_RELEASE); }
    // X68000 のキーボードの LED (Keyboard::leds() の値)
    uint8_t leds() const { return __atomic_load_n(&leds_, __ATOMIC_RELAXED); }
    // マウス: X68000 に送ったパケットの数と、受け取った移動量の合計 (ログ用)
    void mouseStats(uint32_t& packets, int32_t& dx, int32_t& dy) const {
        packets = __atomic_load_n(&mousePackets_, __ATOMIC_RELAXED);
        dx = __atomic_load_n(&mouseDx_, __ATOMIC_RELAXED);
        dy = __atomic_load_n(&mouseDy_, __ATOMIC_RELAXED);
    }
    // 新しいフレームがあれば返す。次に呼ぶまでコア 1 はそのバッファに書かない
    const Frame* takeFrame();
    // 画面をフレームバッファに写す役を、しばらく合成するコア (コア 2、コア 2 を使わなければコア 1) に任せる。
    // コア 0 が USB の転送で長く止まる間 (装置を探す、キーボードの LED を送る) は takeFrame() を呼べず、フレームが
    // 置き換わって表示されなくなるので、その間は合成するコアが publishFrame() の中で fn(ctx, フレーム) を呼んで写す。
    // 受け渡しの順序とメモリのバリアは platform/frame_handoff.h に書いた。setPresenter() はコア 1 を起動する前に 1 回呼ぶ
    void setPresenter(void (*fn)(void* ctx, const Frame& frame), void* ctx) {
        presentFn_ = fn;
        presentCtx_ = ctx;
    }
    // コア 0 で呼ぶ。呼んだあと endComposerPresent() が返るまで、コア 0 は takeFrame() を呼ばず、フレームバッファに書かない
    void beginComposerPresent() {
        if (presentFn_ != nullptr) handoff_.begin();
    }
    // 写す役を合成するコアに任せられる (setPresenter() で写す関数を渡した)。false なら beginComposerPresent() は
    // 何もせず、takeFrameWhileHanded() はいつも nullptr を返す (コア 0 が写す役を持ったまま。takeFrame() で受け取る)
    bool hasPresenter() const { return presentFn_ != nullptr; }
    // コア 0 で呼ぶ。写す役をコア 0 に戻す。合成するコアが写している途中なら、写し終えるまで wait(ctx) を繰り返し呼ぶ
    void endComposerPresent(void (*wait)(void* ctx), void* ctx) {
        handoff_.end([&] {
            if (wait != nullptr) wait(ctx);
        });
    }
    // コア 0 で、beginComposerPresent() のあとに呼ぶ。役を一度戻してフレームを受け取り (受け渡し中に残っていてそれが
    // いちばん新しければそれ、なければ次に合成し終えたもの)、show(ctx, フレーム) で写してから、また任せる
    // (platform::FrameHandoff::takeWhileHanded())。合成するコアが写し終えるのを待つ間は waitPresent(ctx) を呼ぶ
    // (打ち切らない)。フレームを待つ間は wait(ctx) を呼び、false を返したら写さずに nullptr。受け取ったフレームは
    // takeFrame() と同じく、次に takeFrame() かこれを呼ぶまで書き換わらない
    const Frame* takeFrameWhileHanded(void (*waitPresent)(void* ctx), bool (*wait)(void* ctx),
                                      void (*show)(void* ctx, const Frame* frame), void* ctx) {
        if (presentFn_ == nullptr) return nullptr; // 任せていない (beginComposerPresent() は何もしていない)
        const int i = handoff_.takeWhileHanded([&] { waitPresent(ctx); }, [&] { return wait(ctx); },
                                               [&](unsigned n) { show(ctx, &frames_[n]); });
        return i < 0 ? nullptr : &frames_[i];
    }
    // コア 0 で、X68000 が止まっている (paused()) 間に、最後に合成し終えたフレームを返す (写す役をどちらが持っていても、
    // 合成するコアが自分で写してコア 0 に渡らなかったものでも)。止まっていなければ、まだ 1 つも合成していなければ nullptr。
    // コア 1 は合成するコアが記録を処理し終えるのを待ってから止まったことを知らせるので (pauseHere())、止まっている間は
    // 合成するコアはこのバッファに描かない。再開すると書き換わるので、再開を頼む前に読み終えること。
    // 合成するコアが自分で写したフレームのバッファには、止まる前に次のフレームの頭の数ラスタを描いていることがある
    // (コア 1 は次のフレームの表示期間に入ったところで止まる)。その場合、頭の数ラスタだけが次のフレームのものになる。
    // 止めている間に VRAM などを書き換えなければ止める前と同じ画面になることが多いが、同じとは限らない (ラスタごとに
    // 画面を変えるプログラムなど)。直し方がないので、そのまま返す
    // 再開を頼んだあと、コア 1 が動き出して paused() が false になるまでの間も nullptr (止めるよう頼んでいることも見る)
    const Frame* latestFrameWhilePaused() const {
        if (!__atomic_load_n(&pauseReq_, __ATOMIC_ACQUIRE) || !paused()) return nullptr;
        const int i = handoff_.latest();
        return i < 0 ? nullptr : &frames_[i];
    }
    uint32_t framesSelfPresented() const { return handoff_.selfPresented(); }
    // SRAM の写しが書き戻しを待っていれば返す。書き終えたら sramSaved() を呼ぶ
    const uint8_t* pendingSram();
    void sramSaved();
    // テキスト画面のダンプを頼む。できていれば textDump() が返す (読み終えたら textDumpDone())
    void requestTextDump();
    const char (*textDump())[x68k::TEXT_COLS + 1];
    void textDumpDone();
    bool canDumpText() const { return cg_ != nullptr; }
    // CGROM の写し (CG_SIZE バイト。画面の左上のメッセージの字にも使う)。CGROM を読めていなければ nullptr
    const uint8_t* cgRom() const { return cg_; }
    // 一時停止を頼む / やめる。コア 1 が止まったら paused() が true になる (それまでは Machine を触らない)。
    // 再開すると、コア 1 は止まっていた時間を取り戻そうとせずに、実時間の基準を取り直す
    void requestPause(bool on) { __atomic_store_n(&pauseReq_, on ? 1u : 0u, __ATOMIC_RELEASE); }
    bool paused() const { return __atomic_load_n(&pausedAck_, __ATOMIC_ACQUIRE) != 0; }
    // 止めている間に Machine の状態を丸ごと替えた (ステートを読んだ) あとに呼ぶ。コア 2 の Video の写しと
    // コア 3 の音の合成の状態を、Machine のものに合わせる
    void resyncAfterLoad();
    // 止めている間、ステートを保存する前に呼ぶ。音をコア 3 で合成しているときは、Machine の Sound は CPU から見える
    // 状態 (タイマなど) しか進めないので、コア 3 の合成の状態 (鳴っている音のエンベロープなど) を Machine に写す
    void prepareSave();
    // 起動してからの X68000 の CPU サイクル、実時間 (us)、フレーム数
    void stats(uint64_t& cycles, uint64_t& micros, uint64_t& frames) const;
    // 最高速モード (x68k/max_speed.h)。run() の前に呼ぶ。cpuSpeed() はそのときの速さ (既定のクロックの何 % か)
    void setMaxSpeed(bool on) { maxSpeed_ = on; }
    // 次のフレームの時刻まで待つ間、コア 1 を wfe で眠らせる (event_wait.h)。run() の前に呼ぶ。
    // コア 0 でイベントストリームが届くと確かめたときだけ true にする (false なら前と同じく空回りする)
    void setEventStream(bool on) { eventStream_ = on; }
    // ディスクの書き戻しの受け渡し。run() の前に呼ぶ (nullptr なら書き戻さない)
    void setDiskChannel(platform::DiskChannel* ch) { disks_ = ch; }
    int cpuSpeed() const { return __atomic_load_n(&cpuSpeed_, __ATOMIC_RELAXED); }
    // フレームごとの時間の統計 (前に takeFrameTiming() を呼んでからの区間)
    struct FrameTiming {
        uint32_t frames = 0;      // フレーム数
        uint64_t cycles = 0;      // CPU サイクルの合計
        uint32_t minCycles = 0, maxCycles = 0; // 1 フレームの CPU サイクルの最小と最大
        uint64_t busyUs = 0;      // 処理時間 (X68000 を動かし、入力と頼まれたことを片付けるまで) の合計
        uint32_t worstBusyUs = 0; // 最も時間のかかったフレームの処理時間
        int32_t minSpareUs = 0;   // 処理を終えてから次のフレームまでの残り時間の最小値 (負なら遅れた)
        uint32_t late = 0;        // 残り時間が負だったフレームの数
        uint32_t renderWaits = 0; // 起動してから、コア 1 が合成を待った (リングが満杯だったか、2 フレーム以上遅れていた) 回数 (区間ではない)
        uint32_t soundWaits = 0;  // 起動してから、コア 1 が音の合成を待った回数 (区間ではない)
        uint64_t sleptUs = 0;     // 次のフレームの時刻まで wfe で眠った時間の合計
        uint32_t worstWakeUs = 0; // wfe で眠ったとき、起きる予定の時刻から実際に起きるまでの最大 (イベントストリームの周期以内のはず)
        uint32_t streamUs = 0;    // コア 1 のイベントストリームの周期 (0 なら使っていない。空回りで待つ)
        uint32_t probeWakes = NOT_PROBED; // run() の始めにコア 1 で 2ms 眠って戻った回数 (確かめていなければ NOT_PROBED)
        uint32_t probeOnTime = 0; // 続けて確かめた、期限のすぐあとに起きられた回数 (event_wait::wakesOnTime()。見なければ 0)
        uint32_t worstLateUs = 0; // 待ち終えた時刻の、フレームの時刻に対する遅れの最大 (眠っても空回りでも数える)
        // コア 2 / 3 を待った時間 (リングが満杯、合成が 2 フレーム以上遅れた)。処理時間 (busyUs) に入っている
        uint64_t waitUs = 0;      // 合計
        uint32_t worstOwnUs = 0;  // 処理時間からこの待ちを除いた時間の、フレームごとの最大
        uint32_t lateByWait = 0;  // 遅れたフレームのうち、待ちを除けば遅れなかったもの
        uint32_t latePresenting = 0; // 遅れたフレームのうち、その処理の間にコア 2 がフレームバッファに写していた
                                     // (コア 0 が USB の転送などで止まっている間。WithComposerPresenting()) もの。
                                     // コア 2 で合成しているときだけ数える (FrameHandoff::presentSeq())
        static constexpr uint32_t NOT_PROBED = 0xFFFFFFFFu;
    };
    // 前に返してからの区間の統計を返す。待たない: 初めて呼ぶと区切りを頼んで false を返し、コア 1 が
    // フレームの終わりで区切ったあとに呼ぶと、その区間を返して true (返すまでは同じ頼みのまま)
    bool takeFrameTiming(FrameTiming& out);
    uint32_t cpuHz() const { return machine_->cpuHz(); }
    // 音: コア 1 を動かす前に enableSound() で出力の周波数を決める。音のデバイスは audioSource() から取り出す
    // (音の合成をどのコアで行うかはコア 1 が run() で決めるので、ここでは周波数を覚えておき、決めたときに設定する)
    void enableSound(unsigned rate, unsigned latencyFrames) {
        soundRate_ = rate;
        soundLatency_ = latencyFrames;
    }
    platform::AudioSource* audioSource() { return &audioProxy_; }
    // MIDI ボードの音源 (SoundFont、UART)。コア 1 が動いている間に呼んでよい。音をコア 3 で作るならコア 3 の
    // SoundReplayer が、そうでなければ Machine が使う (どちらで作るかはコア 1 が run() で決めるので、両方に渡す。
    // 使わない方は音源を呼ばない: コア 3 で作るときの Machine の Sound は記録するだけで音を作らない)。out は消さないこと
    void setMidiOut(x68k::MidiOut* out) {
        if (soundReplayer_ != nullptr) soundReplayer_->setMidiOut(out);
        // それまでに送ったプログラムチェンジなどの設定を、コア 1 が次に MIDI ボードを進めるときに送り直す
        machine_->setMidiOut(out, true);
    }
    unsigned audioUnderruns() const { return audioProxy_.underruns(); }
    // 音のリングバッファに今たまっているフレーム数と、たまり具合の最小値の目標 (ログ用。x68k::Sound::latencyGoal())
    unsigned audioFill() const { return audioProxy_.fill(); }
    unsigned audioGoal() const {
        const unsigned go = __atomic_load_n(&soundCoreGo_, __ATOMIC_ACQUIRE);
        return go == 1 ? synth_->latencyGoal() : go == 2 ? machine_->sound().latencyGoal() : 0;
    }
    // 前に取ってからの音の大きさ (ログ用。x68k::Sound::takeLevels())。音を作っている方の Sound から取る
    x68k::Sound::Levels takeAudioLevels() {
        const unsigned go = __atomic_load_n(&soundCoreGo_, __ATOMIC_ACQUIRE);
        if (go == 1) return synth_->takeLevels();
        if (go == 2) return machine_->sound().takeLevels();
        return {0, 0, 0};
    }
    // 合成したフレームの数と、そのうちコア 0 が取る前に次のフレームで置き換わった (画面に出なかった) 数
    void frameCounts(uint32_t& published, uint32_t& dropped) const {
        published = handoff_.published();
        dropped = handoff_.dropped();
    }

private:
    x68k::Ts6bga* tsFrames_ = nullptr;
    platform::FrameHandoff tsHandoff_;
    // 表示期間が終わったら (onFrameEnd)、描き終えたフレームをコア 0 に渡す
    class FrameRenderer : public x68k::Renderer {
    public:
        FrameRenderer(const x68k::Video& video, Emulator& owner) : Renderer(video), owner_(owner) {}
        void onFrameEnd() override {
            Renderer::onFrameEnd();
            owner_.publishFrame();
        }

    private:
        Emulator& owner_;
    };

    void publishFrame();
    void chooseCores();
    void pauseHere();
    void dispatchInput(uint32_t v);
    void serviceRequests();
    void publishStats(uint64_t cycles, uint64_t micros, uint64_t frames);
    void addFrameTiming(uint32_t cycles, uint32_t busyUs, int32_t spareUs, uint32_t sleptUs, uint32_t wakeUs,
                        uint32_t lateUs, uint32_t waitUs, bool presenting);
    // コア 1 がコア 2 / 3 を待った時間の合計 (us。journal の setClock() で測る)
    uint64_t coreWaitUs() const {
        return journal_.stats().waitTicks + link_.frameWaitTicks() + soundJournal_.stats().waitTicks;
    }

    x68k::Machine* machine_ = nullptr;
    FrameRenderer* renderer_ = nullptr; // 合成に使っている Renderer (下の 2 つのどちらか)
    FrameRenderer* liveRenderer_ = nullptr;   // コア 1 で合成するとき (Machine の Video を読む)
    FrameRenderer* shadowRenderer_ = nullptr; // コア 2 で合成するとき (Video の写しを読む)

    // コア 2 で合成するとき
    static constexpr unsigned JOURNAL_LOG2 = 18; // リングの記録の数 (2^18 個、1MB)
    x68k::Video* shadow_ = nullptr;
    uint32_t* journalBuf_ = nullptr;
    x68k::VideoJournal journal_;
    x68k::VideoReplayer* replayer_ = nullptr; // nullptr なら合成はコア 1
    x68k::RenderLink link_;
    unsigned renderCoreEntered_ = 0; // コア 2 が runRender() に入った
    unsigned renderCoreGo_ = 0;      // 1: コア 2 で合成する、2: しない (コア 1 が決める)
    unsigned renderCoreActive_ = 0;

    // コア 3 で音を合成するとき
    static constexpr unsigned SOUND_JOURNAL_LOG2 = 14; // 記録の数 (2^14 個、64KB。1ms ごとに見せる)
    x68k::Sound* synth_ = nullptr;
    uint32_t* soundJournalBuf_ = nullptr;
    x68k::VideoJournal soundJournal_;
    x68k::SoundReplayer* soundReplayer_ = nullptr; // nullptr なら音の合成はコア 1
    x68k::SoundLink soundLink_;
    unsigned soundCoreEntered_ = 0; // コア 3 が runSound() に入った
    unsigned soundCoreGo_ = 0;      // 1: コア 3 で合成する、2: しない (コア 1 が決める)
    // コア 2 / コア 3 が合成に使った時間 (足すのはそのコアだけ。コア 0 が takeCoreWork() で区間ごとに受け取る)
    platform::CoreWorkMeter renderWork_, soundWork_;
    uint32_t soundWakes_ = 0;       // コア 3 が wfe から起きた回数 (書くのはコア 3)
    uint32_t soundEmptyWakes_ = 0;  // そのうち記録がなかった回数 (書くのはコア 3)
    unsigned soundRate_ = 0, soundLatency_ = 0;
    // 音のデバイスに渡す AudioSource。音を作っている Sound のリングから取り出す (決まるまでは無音)
    class AudioProxy : public platform::AudioSource {
    public:
        explicit AudioProxy(Emulator& owner) : owner_(owner) {}
        void readAudio(int16_t* out, int frames) override;
        unsigned underruns() const;
        unsigned fill() const;

    private:
        x68k::AudioRing* ring() const;
        Emulator& owner_;
    };
    AudioProxy audioProxy_{*this};
    uint8_t* cg_ = nullptr; // テキストのダンプ用の CGROM の写し

    // 入力のリングバッファの値: bit31-28 が種類
    static constexpr uint32_t EV_KEY = 0u << 28, EV_MOUSE = 1u << 28, EV_JOY = 2u << 28;
    InputRing input_;
    uint8_t leds_ = 0xff;
    uint32_t mousePackets_ = 0;
    int32_t mouseDx_ = 0, mouseDy_ = 0;

    // 3 枚のバッファ: 書き手 (合成するコア) の 1 枚、読み手 (コア 0) の 1 枚、受け渡し中の 1 枚。番号は handoff_ が持つ
    Frame frames_[3];
    platform::FrameHandoff handoff_;
    void (*presentFn_)(void* ctx, const Frame& frame) = nullptr;
    void* presentCtx_ = nullptr;

    platform::DiskChannel* disks_ = nullptr;
    uint8_t* sramShadow_ = nullptr;
    unsigned sramPending_ = 0; // 1: sramShadow_ が書き戻しを待っている

    enum { DUMP_IDLE, DUMP_REQUESTED, DUMP_READY };
    unsigned dumpState_ = DUMP_IDLE;
    char (*dump_)[x68k::TEXT_COLS + 1] = nullptr;

    // 速さの統計。3 つを 1 組で読めるよう、statSeq_ を書く前後で 1 ずつ増やす (奇数の間は書いている途中)
    unsigned statSeq_ = 0;
    uint64_t statCycles_ = 0, statMicros_ = 0, statFrames_ = 0;
    bool maxSpeed_ = false;
    bool eventStream_ = false;
    unsigned streamUs_ = 0; // コア 1 のイベントストリームの周期 (run() が決める)
    uint32_t streamProbeWakes_ = FrameTiming::NOT_PROBED; // run() の始めに確かめた結果 (FrameTiming::probeWakes)
    uint32_t streamProbeOnTime_ = 0;                      // 同じく (FrameTiming::probeOnTime)
    int cpuSpeed_ = 100;
    unsigned pauseReq_ = 0;  // コア 0 が書く
    unsigned discardInput_ = 0; // コア 0 が書く (入力を送る側が読む)
    bool discard() const { return __atomic_load_n(&discardInput_, __ATOMIC_ACQUIRE) != 0; }
    unsigned pausedAck_ = 0; // コア 1 が書く
    // フレームの時間の統計。コア 1 が足し、コア 0 に頼まれたらフレームの終わりで区切って渡す
    // (区切るのはコア 1 なので、コア 0 が読んでから区切るまでに終わったフレームも落とさない)
    platform::IntervalHandoff<FrameTiming> timing_;
};
