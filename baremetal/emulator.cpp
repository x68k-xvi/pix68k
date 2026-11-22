#include "emulator.h"

#include <circle/timer.h>
#include <circle/util.h>

#include "event_wait.h"
#include "platform/disk_sync.h"
#include "x68k/max_speed.h"
#include "x68k/state.h"

bool InputRing::push(uint32_t v) {
    unsigned h = __atomic_load_n(&head_, __ATOMIC_RELAXED);
    unsigned t = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
    if (h - t >= SIZE) return false;
    buf_[h & (SIZE - 1)] = v;
    __atomic_store_n(&head_, h + 1, __ATOMIC_RELEASE);
    return true;
}

bool InputRing::pop(uint32_t& v) {
    unsigned t = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
    unsigned h = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
    if (t == h) return false;
    v = buf_[t & (SIZE - 1)];
    __atomic_store_n(&tail_, t + 1, __ATOMIC_RELEASE);
    return true;
}

// コア 2 とコア 3 を起こす (記録がないと wfe で眠る)
static void wakeCores() { asm volatile("dsb ish; sev" ::: "memory"); }
// コア 1 がコア 2 / 3 を待った時間を測る時計 (us)。待つときにだけ読む
static uint64_t waitClock() { return CTimer::GetClockTicks64(); }

Emulator::~Emulator() {
    delete[] tsFrames_;
    if (machine_) machine_->setLineListener(nullptr);
    delete liveRenderer_;
    delete shadowRenderer_;
    delete replayer_;
    delete shadow_;
    delete[] journalBuf_;
    delete soundReplayer_;
    delete synth_;
    delete[] soundJournalBuf_;
    delete machine_;
    for (Frame& f : frames_) delete[] f.pixels;
    delete[] cg_;
    delete[] sramShadow_;
    delete[] dump_;
}

bool Emulator::initTs6bga(uint32_t base) {
    if (!machine_) return false;
    if (tsFrames_) return machine_->ts6bga() && machine_->ts6bga()->window(base);
    tsFrames_ = new x68k::Ts6bga[3];
    if (!tsFrames_ || !machine_->setTs6bga(true, base)) {
        delete[] tsFrames_; tsFrames_ = nullptr;
        return false;
    }
    handoff_.retainSelfFrames();
    return true;
}

bool Emulator::init(const uint8_t* ipl, const uint8_t* cg, int64_t localSeconds, const x68k::Config& config,
                    bool renderCore, bool soundCore, const uint8_t* scsiRom) {
    // Machine は大きいので (メイン RAM、SRAM、FDC のバッファなど) ヒープに置く。
    // このファイルは -fcheck-new でビルドするので、確保に失敗すれば nullptr になる
    machine_ = new x68k::Machine(config);
    if (machine_ == nullptr || !machine_->ok()) return false;
    for (Frame& f : frames_) {
        f.pixels = new uint32_t[(size_t)PITCH * x68k::Renderer::MAX_HEIGHT];
        if (f.pixels == nullptr) return false;
        memset(f.pixels, 0, (size_t)PITCH * x68k::Renderer::MAX_HEIGHT * 4);
    }
    sramShadow_ = new uint8_t[x68k::Machine::SRAM_SIZE];
    liveRenderer_ = new FrameRenderer(machine_->video(), *this);
    if (sramShadow_ == nullptr || liveRenderer_ == nullptr) return false;
    renderer_ = liveRenderer_;
    if (renderCore) {
        // コア 2 はヒープを使えないので、ここ (コア 0) ですべて確保する
        shadow_ = new x68k::Video;
        journalBuf_ = new uint32_t[1u << JOURNAL_LOG2];
        if (shadow_ == nullptr || journalBuf_ == nullptr) return false;
        shadowRenderer_ = new FrameRenderer(*shadow_, *this);
        if (shadowRenderer_ == nullptr) return false;
        journal_.init(journalBuf_, JOURNAL_LOG2);
        journal_.setWake(wakeCores);
        journal_.setClock(waitClock);
        replayer_ = new x68k::VideoReplayer(journal_, *shadow_, *shadowRenderer_);
        if (replayer_ == nullptr) return false;
    }
    if (soundCore) {
        synth_ = new x68k::Sound;
        soundJournalBuf_ = new uint32_t[1u << SOUND_JOURNAL_LOG2];
        if (synth_ == nullptr || soundJournalBuf_ == nullptr) return false;
        soundJournal_.init(soundJournalBuf_, SOUND_JOURNAL_LOG2);
        soundJournal_.setWake(wakeCores);
        soundJournal_.setClock(waitClock);
        soundReplayer_ = new x68k::SoundReplayer(soundJournal_, *synth_);
        if (soundReplayer_ == nullptr) return false;
    }
    if (cg != nullptr) {
        cg_ = new uint8_t[x68k::Machine::CG_SIZE];
        dump_ = new char[x68k::TEXT_ROWS][x68k::TEXT_COLS + 1];
        if (cg_ == nullptr || dump_ == nullptr) return false;
        memcpy(cg_, cg, x68k::Machine::CG_SIZE);
        machine_->setCgRom(cg, x68k::Machine::CG_SIZE);
    }
    machine_->setIplRom(ipl, x68k::Machine::IPL_SIZE);
    if (config.cpuModel != m68k::CpuModel::M68000) { // X68030 / 060turbo の内蔵 SCSI の ROM は IPL ROM から作る
        uint8_t* scsi = new uint8_t[x68k::Machine::SCSI_ROM_SIZE];
        if (scsi == nullptr) return false;
        if (scsiRom != nullptr) memcpy(scsi, scsiRom, x68k::Machine::SCSI_ROM_SIZE);
        else x68k::Machine::makeScsiRom30(ipl, scsi);
        bool ok = machine_->setScsiRom(scsi, x68k::Machine::SCSI_ROM_SIZE);
        delete[] scsi;
        if (!ok) return false;
    }
    machine_->setTime(localSeconds);
    machine_->setLineListener(nullptr); // どちらで合成するかは run() で決める
    return true;
}

// コア 2 / コア 3 が runRender() / runSound() に入るのを待ち、どのコアで合成するかを決める (コア 1、powerOn() の前)
void Emulator::chooseCores() {
    bool useCore2 = false, useCore3 = false;
    // 音を出さないなら (音のデバイスがない)、作った音は誰も取り出さないので、コア 3 は使わない。
    // CPU から見える状態 (タイマ、割り込み、ADPCM の転送の要求) は、どちらでもコア 1 が計算する
    const bool wantCore3 = soundReplayer_ != nullptr && soundRate_ != 0;
    const uint64_t start = CTimer::GetClockTicks64();
    for (;;) {
        useCore2 = replayer_ != nullptr && __atomic_load_n(&renderCoreEntered_, __ATOMIC_ACQUIRE);
        useCore3 = wantCore3 && __atomic_load_n(&soundCoreEntered_, __ATOMIC_ACQUIRE);
        if ((useCore2 || replayer_ == nullptr) && (useCore3 || !wantCore3)) break;
        if (CTimer::GetClockTicks64() - start >= CLOCKHZ) break;
    }
    // 音: コア 3 で作るなら、Machine の Sound は音を作らず (周波数 0)、CPU から見える状態だけを計算する。
    // 2 つの Sound は作ったばかりの同じ状態なので、powerOn() の前に記録を始めれば同じ入力を受ける
    if (useCore3) {
        synth_->setOutputRate((int)soundRate_);
        synth_->setLatency(soundLatency_);
        soundLink_.attach(*machine_, soundJournal_);
        __atomic_store_n(&soundCoreGo_, 1u, __ATOMIC_RELEASE);
    } else {
        machine_->sound().setOutputRate((int)soundRate_);
        machine_->sound().setLatency(soundLatency_);
        __atomic_store_n(&soundCoreGo_, 2u, __ATOMIC_RELEASE);
    }
    if (useCore2) {
        renderer_ = shadowRenderer_;
        renderer_->setTarget(frames_[handoff_.writer()].pixels, PITCH, PITCH, x68k::Renderer::MAX_HEIGHT);
        // 写しを今の Video と同じにしてから記録を始める。コア 2 は記録が届くまで写しを読まない
        link_.attach(*machine_, journal_, *shadow_, *replayer_);
        __atomic_store_n(&renderCoreActive_, 1u, __ATOMIC_RELAXED);
        // ここまでの書き込み (setTarget、写し) は release でコア 2 に見せる
        __atomic_store_n(&renderCoreGo_, 1u, __ATOMIC_RELEASE);
    } else {
        renderer_ = liveRenderer_;
        renderer_->setTarget(frames_[handoff_.writer()].pixels, PITCH, PITCH, x68k::Renderer::MAX_HEIGHT);
        machine_->setLineListener(renderer_);
        __atomic_store_n(&renderCoreGo_, 2u, __ATOMIC_RELEASE);
    }
}

void Emulator::runSound() {
    if (soundReplayer_ == nullptr) return;
    __atomic_store_n(&soundCoreEntered_, 1u, __ATOMIC_RELEASE);
    unsigned go;
    while ((go = __atomic_load_n(&soundCoreGo_, __ATOMIC_ACQUIRE)) == 0) {
    }
    if (go != 1) return;
    // 画面の合成と同じく、記録がなければ wfe で眠る (コア 1 は記録を見せるたびに sev を送る)。
    // sev は特定のコアに送れないので、コア 2 への記録を見せたとき (ラスタごと) にもコア 3 は起きる。
    // 起きても head を読んで記録がなければまた眠るだけなので、そのままにする。
    // 起きた回数と、そのうち記録がなかった回数を数え、コア 0 がログに出す (soundCoreWakes())
    // 合成に使った時間を、X68000 のフレームごとに数える (ログ用。takeCoreWork())。記録を処理する step() と、音を作る
    // idleMidi() の前後で時刻を読む。記録がなくて起きただけのときは、時刻を読まない (先に記録があるかを見る)。
    // idleMidi() は、音を作ったことが 1 度でもあれば (SC-55 の音源をつないでいる)、それからは毎回前後で時刻を読む。
    // フレームの区切りは、コア 1 が 1 フレームを終えるたびに進める statFrames_ で見る
    // (音の記録にはフレームの区切りがない。コア 3 が処理した記録とは 1ms ほどずれる)
    bool woke = false;
    bool timeIdleMidi = false;
    uint64_t busyUs = 0;
    uint64_t seenFrames = __atomic_load_n(&statFrames_, __ATOMIC_RELAXED);
    for (;;) {
        const uint32_t* first;
        bool worked;
        if (soundJournal_.available(first) != 0) {
            const uint64_t t0 = CTimer::GetClockTicks64();
            worked = soundReplayer_->step() != 0;
            busyUs += CTimer::GetClockTicks64() - t0;
        } else if (timeIdleMidi) {
            // 記録を処理し終えたら、重い MIDI の音源 (SC-55) が先に音を作る (OPM と ADPCM の記録を先にする)
            const uint64_t t0 = CTimer::GetClockTicks64();
            worked = soundReplayer_->idleMidi();
            if (worked) busyUs += CTimer::GetClockTicks64() - t0;
        } else {
            worked = timeIdleMidi = soundReplayer_->idleMidi();
        }
        if (worked) {
            woke = false;
            const uint64_t f = __atomic_load_n(&statFrames_, __ATOMIC_RELAXED);
            if (f != seenFrames) {
                // ステートを読んだときなどにフレームの数が戻ったら、数えずに区切りだけ合わせる
                if (f > seenFrames) soundWork_.addFrames(busyUs, (uint32_t)(f - seenFrames));
                busyUs = 0;
                seenFrames = f;
            }
            continue;
        }
        if (woke) __atomic_store_n(&soundEmptyWakes_, soundEmptyWakes_ + 1, __ATOMIC_RELAXED);
        asm volatile("wfe" ::: "memory");
        __atomic_store_n(&soundWakes_, soundWakes_ + 1, __ATOMIC_RELAXED);
        woke = true;
    }
}

x68k::AudioRing* Emulator::AudioProxy::ring() const {
    unsigned go = __atomic_load_n(&owner_.soundCoreGo_, __ATOMIC_ACQUIRE);
    if (go == 1) return &owner_.synth_->ring();
    if (go == 2) return &owner_.machine_->sound().ring();
    return nullptr;
}

void Emulator::AudioProxy::readAudio(int16_t* out, int frames) {
    if (x68k::AudioRing* r = ring()) {
        r->readAudio(out, frames);
        return;
    }
    memset(out, 0, (size_t)frames * 4);
}

unsigned Emulator::AudioProxy::underruns() const {
    x68k::AudioRing* r = ring();
    return r ? r->underruns() : 0;
}

unsigned Emulator::AudioProxy::fill() const {
    x68k::AudioRing* r = ring();
    return r ? r->fill() : 0;
}

void Emulator::runRender() {
    if (replayer_ == nullptr) return;
    __atomic_store_n(&renderCoreEntered_, 1u, __ATOMIC_RELEASE);
    unsigned go;
    while ((go = __atomic_load_n(&renderCoreGo_, __ATOMIC_ACQUIRE)) == 0) {
    }
    if (go != 1) return;
    // コア 2 でも Renderer は NEON を使う。QEMU (浮動小数点が無効なら例外になる) で動くことを確かめた。
    // Circle はすべてのコアで IRQ を有効にする (EntrySecondary() の EnableIRQs())。周辺機器の割り込みは
    // コア 0 にだけ向くが、コア 2 にもコア間の割り込み (IPI。止めるときの IPI_HALT_CORE など) は届く。
    // そのときも IRQ の入口で NEON のレジスタを退避するので (run() の前の説明を参照)、合成の途中で壊れない。
    // 記録がなければ wfe で眠る (空回りで熱くならないように)。コア 1 は記録を見せるたびに sev を送る。
    // step() が head を読んだあとに送られた sev はイベントレジスタに残るので、wfe はすぐ戻る (起こし損ねない)
    // 合成に使った時間を、フレームごとに数える (ログ用。takeCoreWork())。記録を処理する step() の前後で時刻を読み、
    // フレームの終わりの記録を処理したら 1 フレーム分として足す。記録がなくて起きただけのときは、時刻を読まない
    // (先に記録があるかを見る)
    uint64_t busyUs = 0;
    uint64_t seenFrames = replayer_->frames();
    for (;;) {
        const uint32_t* first;
        if (journal_.available(first) == 0) {
            asm volatile("wfe" ::: "memory");
            continue;
        }
        const uint64_t t0 = CTimer::GetClockTicks64();
        replayer_->step();
        busyUs += CTimer::GetClockTicks64() - t0;
        const uint64_t f = replayer_->frames();
        if (f != seenFrames) {
            // フレームの数が戻ったら (今の VideoReplayer では起きないが、数え直すようになったときのため)、
            // 数えずに区切りだけ合わせる
            if (f > seenFrames) renderWork_.addFrames(busyUs, (uint32_t)(f - seenFrames));
            busyUs = 0;
            seenFrames = f;
        }
    }
}

// コア 1 では Renderer が NEON (SIMD) のレジスタを使う。Circle は GCC 12 以降なら SAVE_VFP_REGS_ON_IRQ を
// 自動で有効にし (sysconfig.h)、IRQ と FIQ の入口で q0-q31 を退避する (exceptionstub64.S)。
// コア 1 に割り込みが来ても NEON のレジスタは壊れない (ビルドに使う aarch64-elf-gcc は 16)
void Emulator::run() {
    x68k::Machine& m = *machine_;
    chooseCores();
    m.powerOn();
    const uint64_t hz = m.cpuHz();
    // イベントストリームはコアごとに有効にする (コア 0 は CKernel::StartEventStream() で有効にし、届くことを確かめた)。
    // コア 1 でも 2ms 眠って戻った回数を数え、見込みの半分に届かなければ使わない (空回りで待つ)。
    // 届かなくても、コア 0 がタイマの割り込みから 100Hz で送る sev で wfe は戻るので、確認は 2ms + 10ms ほどで終わる
    // (CKernel::StartEventStream()、event_wait.h)。戻った回数にはほかのコアの sev も入るので、続けて
    // 周期の数倍だけ眠って、期限のすぐあとに起きられるかも見る (event_wait::wakesOnTime()。届かなければ長くて 160ms ほどかかる)
    streamUs_ = eventStream_ ? event_wait::enableStream() : 0;
    if (streamUs_ != 0) {
        const unsigned window = 2000;
        streamProbeWakes_ = window / streamUs_ != 0 ? event_wait::countWakes(window) : 0;
        if (platform::streamProbeOk(streamProbeWakes_, window, streamUs_))
            streamProbeOnTime_ = event_wait::wakesOnTime(streamUs_);
        if (streamProbeOnTime_ < event_wait::WAKE_TRIALS_NEEDED) {
            event_wait::disableStream();
            streamUs_ = 0;
        }
    }
    // 前のフレームのあとに眠った時間、起きるのが遅れた時間、待ち終えた時刻のフレームの時刻に対する遅れ
    uint32_t sleptUs = 0, wakeUs = 0, lateUs = 0;
    // 速さの統計の基準。止めていた時間と、ステートを読んで変わったサイクル数とフレーム数は数えない。
    // サイクル数は totalCycles() で見る (runFrame() が時刻を戻しても、差が狂わない。Machine::rebaseTime())
    uint64_t startTicks = CTimer::GetClockTicks64(), startCycles = m.totalCycles();
    int64_t frameBase = 0;
    // 速さの合わせ方 (ホスト版と同じ): X68000 が進めた CPU サイクルを実時間に直し、その時刻まで待つ。
    // 大きく遅れたら追いつこうとせずに基準を取り直す
    uint64_t baseTicks = startTicks, baseCycles = startCycles;
    x68k::MaxSpeed maxSpeed;
    for (;;) {
        if (__atomic_load_n(&pauseReq_, __ATOMIC_ACQUIRE)) {
            const uint64_t t0 = CTimer::GetClockTicks64(), c0 = m.totalCycles();
            const uint64_t f0 = m.frameCount();
            pauseHere();
            startTicks += CTimer::GetClockTicks64() - t0;
            startCycles += m.totalCycles() - c0; // 符号なしの差 (読んで減っても足し引きで合う)
            frameBase += (int64_t)(m.frameCount() - f0);
            // 止めていた時間は取り戻さない
            baseTicks = CTimer::GetClockTicks64();
            baseCycles = m.totalCycles();
        }
        const uint64_t frameStart = CTimer::GetClockTicks64(), frameCycles = m.totalCycles();
        const uint64_t wait0 = coreWaitUs();
        const uint32_t presentSeq0 = handoff_.presentSeq();
        uint32_t ev;
        while (input_.pop(ev)) dispatchInput(ev);
        m.runFrame(); // 表示期間の終わりで publishFrame() が呼ばれる
        // まとめている音のクロック数を、実時間を待つ前にコア 3 に見せる (待つ間にコア 3 が音を作れるように)
        m.sound().flushJournal();
        if (tsFrames_ && m.ts6bga()->dirty()) {
            m.ts6bga()->snapshot(tsFrames_[tsHandoff_.writer()]);
            tsHandoff_.publish([](unsigned) {}); // コア1は表示を受け持たない
        }
        __atomic_store_n(&leds_, m.keyboard().leds(), __ATOMIC_RELAXED);
        __atomic_store_n(&mousePackets_, m.mouse().packets(), __ATOMIC_RELAXED);
        serviceRequests();

        uint64_t now = CTimer::GetClockTicks64();
        // このフレームの処理時間と、X68000 の 1 フレームの時間に対する残り
        const uint64_t cyc = m.totalCycles() - frameCycles;
        const int64_t frameUs = (int64_t)(cyc * CLOCKHZ / hz);
        addFrameTiming((uint32_t)cyc, (uint32_t)(now - frameStart), (int32_t)(frameUs - (int64_t)(now - frameStart)),
                       sleptUs, wakeUs, lateUs, (uint32_t)(coreWaitUs() - wait0),
                       renderCoreActive() && ((presentSeq0 & 1) != 0 || handoff_.presentSeq() != presentSeq0));
        sleptUs = wakeUs = lateUs = 0;
        if (maxSpeed_) {
            maxSpeed.update(m, (uint64_t)frameUs, now - frameStart);
            __atomic_store_n(&cpuSpeed_, m.cpuSpeed(), __ATOMIC_RELAXED);
        }
        uint64_t emuUs = (m.totalCycles() - baseCycles) * CLOCKHZ / hz;
        uint64_t wallUs = now - baseTicks;
        if (emuUs > wallUs) {
            // 次のフレームの時刻まで待つ。イベントストリームを使えるなら、周期 1 つ分手前まで wfe で眠り、
            // 残り (周期 1 つ分以内) は空回りする。眠ったままだと周期 1 つ分まで遅れて起きるので、
            // 最後だけ空回りして、フレームの時刻の合わせ方は空回りだけで待っていたときと同じにする。
            // コア 1 が送る sev (コア 2 / 3 を起こす) でもコア 1 の wfe は戻るが、時刻を見て眠り直すだけ
            const uint64_t deadline = baseTicks + emuUs;
            if (streamUs_ != 0 && deadline - now > streamUs_) {
                const uint64_t target = deadline - streamUs_;
                const uint64_t sleepStart = CTimer::GetClockTicks64(); // now のあとの統計の処理の時間は入れない
                event_wait::sleepUntil(target);
                const uint64_t woke = CTimer::GetClockTicks64();
                sleptUs = (uint32_t)(woke - sleepStart);
                wakeUs = (uint32_t)(woke - target);
            }
            uint64_t t;
            while ((t = CTimer::GetClockTicks64()) < deadline) {
            }
            lateUs = (uint32_t)(t - deadline);
        } else if (wallUs - emuUs > CLOCKHZ / 4) {
            baseTicks = now;
            baseCycles = m.totalCycles();
            emuUs = 0;
        }
        // 上の掛け算があふれないよう、10 秒ごとに基準を進める (遅れの分はそのまま残る。端数の切り捨ては 1 回 1us 未満)
        if (emuUs > 10 * CLOCKHZ) {
            baseTicks += emuUs;
            baseCycles = m.totalCycles();
        }
        publishStats(m.totalCycles() - startCycles, CTimer::GetClockTicks64() - startTicks,
                     (uint64_t)((int64_t)m.frameCount() - frameBase));
    }
}

// フレームの切れ目で止まる (コア 1)。コア 2 / 3 が記録を処理し終えるのを待ってから止まったことを知らせ、
// コア 0 が再開を頼むまで待つ。止まっている間はコア 0 が Machine を触る (journal_ への書き込みもコア 0 が行うことがある。
// 書き手が入れ替わるのは、止まったことと再開の知らせ (release / acquire) をはさんだときだけ)
void Emulator::pauseHere() {
    // メニューを開くときに離したキー、マウス、ジョイスティックを先に Machine に渡す (止めている間に保存した
    // ステートに、押したままのキーが残らないように)
    uint32_t ev;
    while (input_.pop(ev)) dispatchInput(ev);
    if (renderCoreActive()) journal_.flush();
    if (soundCoreActive()) soundJournal_.flush();
    __atomic_store_n(&pausedAck_, 1u, __ATOMIC_RELEASE);
    // コア 0 の主ループは 2ms ごとに sev を出すので、wfe で待つ
    while (__atomic_load_n(&pauseReq_, __ATOMIC_ACQUIRE)) asm volatile("wfe" ::: "memory");
    // コア 0 が止めている間に記録を入れたかもしれないので、コア 2 / 3 に見せる
    if (renderCoreActive()) journal_.publish();
    if (soundCoreActive()) soundJournal_.publish();
    __atomic_store_n(&pausedAck_, 0u, __ATOMIC_RELEASE);
}

// 止めている間はコア 0 が記録の書き手になる (リセットで Machine が記録を書く)。コア 2 / 3 は動き続けて記録を
// 処理するので、写しを読み書きする前に、処理し終えるのを待つ
void Emulator::prepareSave() {
    if (soundCoreActive()) {
        machine_->sound().flushJournal();
        soundJournal_.flush();
        x68k::copySoundState(*synth_, machine_->sound());
    }
}

void Emulator::resyncAfterLoad() {
    if (renderCoreActive()) {
        journal_.flush();
        x68k::copyVideoState(machine_->video(), *shadow_);
    }
    if (soundCoreActive()) {
        machine_->sound().flushJournal();
        soundJournal_.flush();
        x68k::copySoundState(machine_->sound(), *synth_);
        // まーきゅりーゆにっと: Machine のものは音を作らない間も同じ入力で進めているので、それを写す
        x68k::Mercury* unit = soundReplayer_->mercury();
        if (unit && machine_->mercury()) *unit = *machine_->mercury();
    }
}

// 12 ビットの符号付きの値を広げる
static int signExtend12(uint32_t v) { return (int)((v & 0xfff) ^ 0x800) - 0x800; }

void Emulator::dispatchInput(uint32_t v) {
    platform::InputSink& in = machine_->input();
    switch (v & 0xf0000000u) {
    case EV_KEY:
        in.onKey((uint8_t)v, (v & 0x100) != 0);
        break;
    case EV_MOUSE: {
        int dx = signExtend12(v), dy = signExtend12(v >> 12);
        in.onMouse(dx, dy, (uint8_t)((v >> 24) & 7));
        // ログ用の合計は、符号付きであふれないよう符号なしで足す (あふれたら一周する)
        __atomic_store_n(&mouseDx_, (int32_t)((uint32_t)mouseDx_ + (uint32_t)dx), __ATOMIC_RELAXED);
        __atomic_store_n(&mouseDy_, (int32_t)((uint32_t)mouseDy_ + (uint32_t)dy), __ATOMIC_RELAXED);
        break;
    }
    case EV_JOY:
        in.onJoystick((int)((v >> 8) & 1), (uint8_t)(v & 0x3f));
        break;
    default:
        break;
    }
}

void Emulator::publishStats(uint64_t cycles, uint64_t micros, uint64_t frames) {
    unsigned seq = __atomic_load_n(&statSeq_, __ATOMIC_RELAXED);
    __atomic_store_n(&statSeq_, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&statCycles_, cycles, __ATOMIC_RELAXED);
    __atomic_store_n(&statMicros_, micros, __ATOMIC_RELAXED);
    __atomic_store_n(&statFrames_, frames, __ATOMIC_RELAXED);
    __atomic_store_n(&statSeq_, seq + 2, __ATOMIC_RELEASE);
}

void Emulator::addFrameTiming(uint32_t cycles, uint32_t busyUs, int32_t spareUs, uint32_t sleptUs, uint32_t wakeUs,
                              uint32_t lateUs, uint32_t waitUs, bool presenting) {
    FrameTiming& t = timing_.current();
    if (waitUs > busyUs) waitUs = busyUs;
    t.waitUs += waitUs;
    if (busyUs - waitUs > t.worstOwnUs) t.worstOwnUs = busyUs - waitUs;
    if (spareUs < 0) {
        if (spareUs + (int32_t)waitUs >= 0) t.lateByWait++;
        if (presenting) t.latePresenting++;
    }
    t.sleptUs += sleptUs;
    if (wakeUs > t.worstWakeUs) t.worstWakeUs = wakeUs;
    if (lateUs > t.worstLateUs) t.worstLateUs = lateUs;
    t.streamUs = streamUs_;
    t.probeWakes = streamProbeWakes_;
    t.probeOnTime = streamProbeOnTime_;
    if (t.frames == 0 || cycles < t.minCycles) t.minCycles = cycles;
    if (t.frames == 0 || cycles > t.maxCycles) t.maxCycles = cycles;
    if (t.frames == 0 || spareUs < t.minSpareUs) t.minSpareUs = spareUs;
    if (busyUs > t.worstBusyUs) t.worstBusyUs = busyUs;
    if (spareUs < 0) t.late++;
    t.frames++;
    t.cycles += cycles;
    t.renderWaits = (uint32_t)(journal_.stats().waits + link_.frameWaits());
    t.soundWaits = (uint32_t)soundJournal_.stats().waits;
    t.busyUs += busyUs;
    // コア 0 に頼まれていれば、このフレームまでを区間としてコア 0 に渡し、次のフレームから新しい区間にする
    timing_.endSample();
}

bool Emulator::takeFrameTiming(FrameTiming& out) {
    // 初めて呼ぶと区切りを頼んで false。コア 1 が次のフレームの終わりで区切ったあとに呼ぶと true
    return timing_.take(out);
}

// 描き終えたバッファを受け渡し中のものと交換し、次のフレームは戻ってきたバッファに描く
// (合成するコアで、Renderer::onFrameEnd() から呼ばれる)
void Emulator::publishFrame() {
    Frame& f = frames_[handoff_.writer()];
    f.width = renderer_->width();
    f.height = renderer_->height();
    // コア 0 が USB の転送で止まっている間は、自分でフレームバッファに写す (platform/frame_handoff.h)
    const unsigned next = handoff_.publish([this](unsigned i) { presentFn_(presentCtx_, frames_[i]); });
    renderer_->setTarget(frames_[next].pixels, PITCH, PITCH, x68k::Renderer::MAX_HEIGHT);
}

const Emulator::Frame* Emulator::takeFrame() {
    const int i = handoff_.take();
    return i < 0 ? nullptr : &frames_[i];
}

// フレームの切れ目で、コア 0 から頼まれたこと (SRAM とディスクの写し、テキストのダンプ) をする
void Emulator::serviceRequests() {
    x68k::Machine& m = *machine_;
    if (m.sramDirty() && !__atomic_load_n(&sramPending_, __ATOMIC_ACQUIRE)) {
        memcpy(sramShadow_, m.sram(), x68k::Machine::SRAM_SIZE);
        m.clearSramDirty();
        __atomic_store_n(&sramPending_, 1u, __ATOMIC_RELEASE);
    }
    if (disks_) disks_->serviceCore1();
    if (dump_ != nullptr && __atomic_load_n(&dumpState_, __ATOMIC_ACQUIRE) == DUMP_REQUESTED) {
        x68k::readTextScreen(m.video(), cg_, dump_);
        __atomic_store_n(&dumpState_, (unsigned)DUMP_READY, __ATOMIC_RELEASE);
    }
}

const uint8_t* Emulator::pendingSram() {
    return __atomic_load_n(&sramPending_, __ATOMIC_ACQUIRE) ? sramShadow_ : nullptr;
}

void Emulator::sramSaved() { __atomic_store_n(&sramPending_, 0u, __ATOMIC_RELEASE); }

void Emulator::requestTextDump() {
    unsigned idle = DUMP_IDLE;
    __atomic_compare_exchange_n(&dumpState_, &idle, (unsigned)DUMP_REQUESTED, false, __ATOMIC_ACQ_REL,
                                __ATOMIC_RELAXED);
}

const char (*Emulator::textDump())[x68k::TEXT_COLS + 1] {
    return __atomic_load_n(&dumpState_, __ATOMIC_ACQUIRE) == DUMP_READY ? dump_ : nullptr;
}

void Emulator::textDumpDone() { __atomic_store_n(&dumpState_, (unsigned)DUMP_IDLE, __ATOMIC_RELEASE); }

void Emulator::stats(uint64_t& cycles, uint64_t& micros, uint64_t& frames) const {
    for (;;) {
        unsigned seq = __atomic_load_n(&statSeq_, __ATOMIC_ACQUIRE);
        cycles = __atomic_load_n(&statCycles_, __ATOMIC_RELAXED);
        micros = __atomic_load_n(&statMicros_, __ATOMIC_RELAXED);
        frames = __atomic_load_n(&statFrames_, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!(seq & 1) && __atomic_load_n(&statSeq_, __ATOMIC_RELAXED) == seq) return;
    }
}
