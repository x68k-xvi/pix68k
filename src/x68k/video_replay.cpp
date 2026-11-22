#include "x68k/video_replay.h"
#include "x68k/state.h"

namespace x68k {

// ---------------------------------------------------------------- VideoJournal

void VideoJournal::init(uint32_t* buf, unsigned log2) {
    buf_ = buf;
    size_ = 1u << log2;
    mask_ = size_ - 1;
    head_ = published_ = tailCache_ = 0;
    rbuf_ = buf;
    rsize_ = size_;
    rmask_ = mask_;
    tail_ = 0;
    shared_.head = 0;
    shared_.tail = 0;
    shared_.stopped = 0;
    stats_ = Stats();
}

void VideoJournal::waitForSpace() {
    // ためた分を見せてから待つ (見せないと読み手が進めない)
    publish();
    // tailCache_ は満杯に見えたときだけ読み直すので、まず読み直す
    tailCache_ = __atomic_load_n(&shared_.tail, __ATOMIC_ACQUIRE);
    if (head_ - tailCache_ < size_) return;
    stats_.waits++;
    const uint64_t t0 = clockNow();
    for (;;) {
        tailCache_ = __atomic_load_n(&shared_.tail, __ATOMIC_ACQUIRE);
        if (head_ - tailCache_ < size_) break;
        if (stopped()) {
            // 読み手がいない: 古い記録を捨てて場所を作る (止めたあとの記録は使われない)
            tailCache_ = head_ - size_ + 1;
            break;
        }
        stats_.waitSpins++;
        if (relax_) relax_();
    }
    stats_.waitTicks += clockNow() - t0;
}

void VideoJournal::flush() {
    publish();
    for (;;) {
        unsigned t = __atomic_load_n(&shared_.tail, __ATOMIC_ACQUIRE);
        if (t == head_ || stopped()) break;
        if (relax_) relax_();
    }
    tailCache_ = __atomic_load_n(&shared_.tail, __ATOMIC_ACQUIRE);
}

// ---------------------------------------------------------------- RenderLink

void RenderLink::attach(Machine& machine, VideoJournal& journal, Video& shadow, const VideoReplayer& reader) {
    Video& live = machine.video();
    live.setJournal(nullptr);
    shadow.setJournal(nullptr);
    copyVideoState(live, shadow);
    journal_ = &journal;
    reader_ = &reader;
    frames_ = reader.frames();
    live.setJournal(&journal);
    machine.setLineListener(this);
}

void RenderLink::detach(Machine& machine) {
    if (!journal_) return;
    journal_->flush();
    machine.video().setJournal(nullptr);
    machine.setLineListener(nullptr);
    journal_ = nullptr;
    reader_ = nullptr;
}

void RenderLink::onRaster(int raster) {
    rasters_++;
    journal_->push(VideoJournal::make(VideoJournal::OP_RASTER, (uint32_t)raster, 0));
    // ラスタの区切りで読み手に見せる (合成するコアの遅れを小さくする)
    journal_->publish();
    if (probe_) {
        unsigned lag = (unsigned)(rasters_ - probe_->rasters());
        if (lag > maxLag_) maxLag_ = lag;
    }
}

void RenderLink::onFrameEnd() {
    journal_->push(VideoJournal::make(VideoJournal::OP_CONTROL, VideoJournal::CTL_FRAME_END, 0));
    journal_->publish();
    frames_++;
    // 合成するコアが MAX_FRAMES_AHEAD フレーム以上遅れていたら、追いつくまで待つ。
    // 待つのは実時間だけで、X68000 の時間は進めないので、エミュレーションの結果は変わらない
    if (frames_ - reader_->frames() < MAX_FRAMES_AHEAD) return;
    frameWaits_++;
    const uint64_t t0 = journal_->clockNow();
    while (frames_ - reader_->frames() >= MAX_FRAMES_AHEAD && !journal_->stopped()) journal_->relax();
    frameWaitTicks_ += journal_->clockNow() - t0;
}

// ---------------------------------------------------------------- VideoReplayer

unsigned VideoReplayer::step() {
    const uint32_t* p;
    unsigned n = journal_.available(p);
    if (n == 0) return 0;
    // 書き手が待たないよう、まとめすぎずに tail を進める
    if (n > 1024) n = 1024;
    for (unsigned i = 0; i < n; i++) apply(p[i]);
    journal_.consume(n);
    return n;
}

void VideoReplayer::apply(uint32_t e) {
    uint32_t off = VideoJournal::offsetOf(e);
    uint8_t v = VideoJournal::valueOf(e);
    switch (VideoJournal::opOf(e)) {
    case VideoJournal::OP_TVRAM: shadow_.tvramWrite(off, v); break;
    case VideoJournal::OP_GVRAM: shadow_.gvramWrite(off, v); break;
    case VideoJournal::OP_PALETTE: shadow_.paletteWrite(off, v); break;
    case VideoJournal::OP_SPRITE: shadow_.spriteWrite(off, v); break;
    case VideoJournal::OP_CRTC: shadow_.crtcWrite(off, v); break;
    case VideoJournal::OP_HBLANK: shadow_.horizontalBlank(); break;
    case VideoJournal::OP_RASTER:
        // ラスタの番号は負のこともある (垂直表示期間より前)。21 ビットに入れたので符号を戻す
        out_.onRaster((int32_t)(off << 11) >> 11);
        __atomic_store_n(&rasters_, rasters_ + 1, __ATOMIC_RELAXED);
        break;
    default:
        if (off == VideoJournal::CTL_RESET) shadow_.reset();
        else if (off == VideoJournal::CTL_FRAME_END) {
            out_.onFrameEnd();
            // 描き終えた画素は release で見せる (frames() を acquire で読んだ側が画素を読めるように)
            __atomic_store_n(&frames_, frames_ + 1, __ATOMIC_RELEASE);
        }
        else if (off == VideoJournal::CTL_VDISP) shadow_.beginVerticalDisplay();
        break;
    }
}

} // namespace x68k
