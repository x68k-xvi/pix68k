#include "x68k/sound.h"

namespace x68k {

// ---------------------------------------------------------------- AudioRing

bool AudioRing::push(int16_t l, int16_t r) {
    unsigned h = __atomic_load_n(&head_, __ATOMIC_RELAXED);
    unsigned t = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
    if (h - t >= SIZE) return false;
    buf_[h & (SIZE - 1)] = (uint32_t)(uint16_t)l | (uint32_t)(uint16_t)r << 16;
    __atomic_store_n(&head_, h + 1, __ATOMIC_RELEASE);
    return true;
}

int AudioRing::drain(int16_t* out, int maxFrames) {
    unsigned t = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
    unsigned h = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
    int n = 0;
    for (; t != h && n < maxFrames; t++, n++) {
        uint32_t v = buf_[t & (SIZE - 1)];
        out[2 * n] = (int16_t)(uint16_t)v;
        out[2 * n + 1] = (int16_t)(uint16_t)(v >> 16);
    }
    __atomic_store_n(&tail_, t, __ATOMIC_RELEASE);
    return n;
}

int AudioRing::copyPending(int16_t* out, int maxFrames) const {
    unsigned t = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
    unsigned h = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
    int n = 0;
    for (; t != h && n < maxFrames; t++, n++) {
        uint32_t v = buf_[t & (SIZE - 1)];
        out[2 * n] = (int16_t)(uint16_t)v;
        out[2 * n + 1] = (int16_t)(uint16_t)(v >> 16);
    }
    return n;
}

void AudioRing::clearStopped() {
    __atomic_store_n(&head_, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&tail_, 0u, __ATOMIC_RELAXED);
    primed_ = starved_ = false;
    lastL_ = lastR_ = 0;
}

void AudioRing::readAudio(int16_t* out, int frames) {
    int n = 0;
    if (!primed_ && fill() >= prebuffer()) {
        __atomic_store_n(&primed_, true, __ATOMIC_RELAXED);
        if (starved_) { // 鳴らし直すときに、無音で埋めた分をまとめて渡す
            __atomic_fetch_add(&missing_, starvedFrames_, __ATOMIC_RELAXED);
            starved_ = false;
        }
    }
    if (primed_) {
        n = drain(out, frames);
        if (n > 0) {
            lastL_ = out[2 * n - 2];
            lastR_ = out[2 * n - 1];
        }
        if (n < frames) {
            __atomic_store_n(&primed_, false, __ATOMIC_RELAXED);
            starved_ = true;
            starvedFrames_ = 0;
            __atomic_store_n(&underruns_, underruns_ + 1, __ATOMIC_RELAXED);
        }
    }
    // 長く止まっても一周しないよう、SIZE で頭打ちにする (SIZE を超える分は tuneLatency() で長い停止として扱う)
    if (starved_) {
        const unsigned m = (unsigned)(frames - n);
        starvedFrames_ = (m >= SIZE || starvedFrames_ >= SIZE - m) ? SIZE : starvedFrames_ + m;
    }
    // 足りない分は、直前の値から 0 へ少しずつ近づける (急に 0 にするとプツッと鳴る)
    for (; n < frames; n++) {
        lastL_ -= lastL_ >> 8;
        lastR_ -= lastR_ >> 8;
        if (lastL_ > -256 && lastL_ < 256) lastL_ = lastL_ > 0 ? lastL_ - 1 : lastL_ < 0 ? lastL_ + 1 : 0;
        if (lastR_ > -256 && lastR_ < 256) lastR_ = lastR_ > 0 ? lastR_ - 1 : lastR_ < 0 ? lastR_ + 1 : 0;
        out[2 * n] = (int16_t)lastL_;
        out[2 * n + 1] = (int16_t)lastR_;
    }
}

// ---------------------------------------------------------------- Sound

void Sound::reset() {
    if (journal_) {
        flushPending();
        journal_->push(J_RESET << 28);
        lines_ = ~0u;
    }
    resetLocal();
}

void Sound::resetLocal() {
    opm_.reset();
    adpcm_.reset();
    // 合成とフィルタの状態も戻す (前の状態の直流や補間の途中の値が、リセットのあとの音に混ざらないように)
    for (int c = 0; c < 2; c++) {
        lp1_[c] = lp2_[c] = prev_[c] = 0;
        hpIn_[c] = 0;
        hpOut_[c] = 0;
    }
    sub_ = 0;
    pos_ = 0;
}

void Sound::mercuryWrite(Mercury& m, uint32_t off, uint8_t v) {
    switch (off & 0x1fff) {
    case 0x80: m.latchHigh(v); return; // 上位バイトは覚えるだけ (下位バイトで 1 語になる)
    case 0x81: {
        const uint16_t w = (uint16_t)(m.latched() << 8 | v);
        journalMercury(4, w);
        m.writeData(w);
        return;
    }
    case 0x90:
        journalMercury(3, v);
        m.writeMode(v);
        return;
    case 0x91:
        journalMercury(2, v);
        m.writeCommand(v);
        return;
    default: return;
    }
}

void Sound::opmWrite(uint32_t offset, uint8_t v) {
    if (journal_) {
        flushPending();
        journal_->push(J_OPM << 28 | (offset & 3) << 8 | v);
    }
    opm_.write(offset, v);
}

void Sound::adpcmWrite(uint32_t offset, uint8_t v) {
    if (journal_) {
        flushPending();
        journal_->push(J_ADPCM << 28 | (offset & 3) << 8 | v);
    }
    adpcm_.write(offset, v);
}

void Sound::setAdpcmLines(bool clock4M, uint8_t pan, uint8_t rate) {
    if (journal_) {
        uint32_t lines = (clock4M ? 0x10u : 0u) | (rate & 3u) << 2 | (pan & 3u);
        if (lines != lines_) { // Machine は OPM に書くたびに呼ぶので、変わったときだけ記録する
            flushPending();
            journal_->push(J_LINES << 28 | lines);
            lines_ = lines;
        }
    }
    adpcm_.setClock4M(clock4M);
    adpcm_.setPanRate(pan, rate);
}

void Sound::flushJournal() {
    if (!journal_) return;
    flushPending();
    journal_->publish();
    sincePublish_ = 0;
}

void Sound::replay(uint32_t e, Mercury*& mercury, MidiOut* midi, CdMixer* cd) {
    switch (e >> 28) {
    case J_ADVANCE: advanceLocal(e & 0x0fffffff, mercury, midi, cd); break;
    case J_OPM: opm_.write((e >> 8) & 3, (uint8_t)e); break;
    case J_ADPCM: adpcm_.write((e >> 8) & 3, (uint8_t)e); break;
    case J_LINES:
        adpcm_.setClock4M((e & 0x10) != 0);
        adpcm_.setPanRate((uint8_t)(e & 3), (uint8_t)((e >> 2) & 3));
        break;
    case J_RESET:
        resetLocal();
        if (mercury) mercury->reset();
        if (cd) cd->reset();
        break;
    case J_CD:
        if (cd) {
            if (!(e & 0x10000)) cd->holdLeft((int16_t)(uint16_t)e);
            else cd->pushRight((int16_t)(uint16_t)e);
        }
        break;
    case J_CD_DELTA:
        if (cd) cd->pushDelta(((int32_t)(e << 4)) >> 18, ((int32_t)(e << 18)) >> 18);
        break;
    case J_MIDI:
        if (midi) {
            if (e & 0x100) midi->midiReset();
            else midi->midiByte((uint8_t)e);
        }
        break;
    case J_MERCURY:
        switch ((e >> 24) & 15) {
        case 0:
            if (!mercury) mercury = new Mercury; // 確保できなければ鳴らさない
            break;
        case 1:
            delete mercury;
            mercury = nullptr;
            break;
        case 2:
            if (mercury) mercury->writeCommand((uint8_t)e);
            break;
        case 3:
            if (mercury) mercury->writeMode((uint8_t)e);
            break;
        case 4:
            if (mercury) mercury->writeData((uint16_t)e);
            break;
        }
        break;
    default: break;
    }
}

void Sound::setOutputRate(int hz) {
    rate_ = hz > 0 ? hz : 0;
    baseStep_ = rate_ ? (uint32_t)(((uint64_t)OPM_RATE << 16) / (uint32_t)rate_) : 0;
    step_ = baseStep_;
    pos_ = 0;
}

void Sound::advanceLocal(uint32_t clocks, Mercury* mercury, MidiOut* midi, CdMixer* cd) {
    opm_.elapse(clocks);
    sub_ += clocks;
    while (sub_ >= (uint32_t)Opm::CLOCKS_PER_SAMPLE) {
        sub_ -= Opm::CLOCKS_PER_SAMPLE;
        adpcm_.advance(Opm::CLOCKS_PER_SAMPLE);
        if (mercury) mercury->advance(Opm::CLOCKS_PER_SAMPLE);
        if (rate_ == 0) {
            opm_.clockSample(nullptr);
            continue;
        }
        int32_t fm[2];
        opm_.clockSample(fm);
        // OPM 1 チャネル (+-8191 x 4) と、ADPCM (12 ビット) の音量の比は
        // おおよそで決めた。OPM を 1/2 に、ADPCM を 8 倍にして足す
        int32_t l = (fm[0] >> 1) + adpcm_.left() * 8, r = (fm[1] >> 1) + adpcm_.right() * 8;
        // まーきゅりーゆにっとの 16 ビットの PCM は 1/2 にして足す (OPM と ADPCM との比はおおよそ)
        if (mercury) {
            l += mercury->left() >> 1;
            r += mercury->right() >> 1;
        }
        // CD-ROM 装置のオーディオは、ここでは足さない (X68000 の出力のフィルタを通さない)。OPM の 1 サンプル分だけ進め、
        // output() が出力のフレームごとに足す
        CdMixer* playing = nullptr;
        if (cd && !cd->idle()) {
            cd->step();
            playing = cd;
        }
        output(l, r, midi, playing);
    }
}

// 0.25 秒ごとに output() から呼ぶ。目標 (setLatency() の量) に足す量 extra_ を、足りなくなったかで増やし、
// 足りなくならない時間が続けば減らす。目標と足す量の合計は、リングバッファの半分まで
void Sound::tuneLatency() {
    const unsigned cap = AudioRing::SIZE >> 1; // 目標と足す量の合計の上限
    // 足りなくなるたびに、鳴らし直すまでに無音で埋めた分を足す (少なくとも setLatency() の量)。目標の上限を
    // 超える分を埋めたとき (設定のメニューなどで長く止まったとき) は、目標を上げても足りるようにはならないので、
    // setLatency() の量だけ足す。埋めた分は鳴らし直したときに渡されるので、足りなくなった回数とは別に見る
    unsigned add = ring_.takeMissing();
    if (add && (add < target() || target() >= cap || add > cap - target())) add = target();
    // 読み手が数えた、足りなくなった回数が前に見たときから増えたか
    const unsigned seen = lastUnderruns_;
    lastUnderruns_ = ring_.underruns();
    const bool under = lastUnderruns_ != seen;
    if (add) {
        unsigned room = target() + extra() < cap ? cap - target() - extra() : 0;
        setExtra(extra() + (add < room ? add : room));
    }
    if (under) {
        restartCalm(); // 足りなくならない区間を数え直す
        return;
    }
    if (minFill_ < calmMin_) calmMin_ = (uint16_t)minFill_;
    if (++calm_ >= 120) {
        // 減らすのは、この 30 秒のたまり具合の最小値が目標 (setLatency()) を超えた分まで。ときどき止まって
        // たまり具合が下がっているなら、足した量はまだ要るので減らさない
        unsigned room = calmMin_ > target() ? calmMin_ - target() : 0;
        unsigned dec = extra() / 8 + 1;
        if (dec > extra()) dec = extra();
        if (dec > room) dec = room;
        setExtra(extra() - dec);
        restartCalm();
    }
}

// 1/32 秒ごとに output() から呼ぶ。エミュレータが実時間に間に合わない間は、作る音が出す音より少なく、
// リングバッファが空になって音が途切れる (#217)。0.5% の調整では追いつかないので、この 1/32 秒のたまり具合の
// 最小値が setLatency() の量の 3/4 を切ったら、切った分に比例して刻みを小さくし (最大 25%。3/8 以下で最大)、
// 同じ X68000 の時間から多くのフレームを作る。その間は音の高さと速さが下がる (途切れる代わりに、遅くなった分だけ
// 低く鳴る)。下がるのは Sound が作る音 (OPM、ADPCM、まーきゅりーゆにっと、CD-ROM 装置のオーディオ) だけで、出力の
// フレームごとに音を作る MIDI の音源は、音の高さが変わらない (sound.h の先頭の説明)。0.5% の調整と違い、たまり具合が足した量を含む目標より少ないだけでは働かない (ためた分は、
// 遅れを吸収するために使う)。読み手が鳴らしていない間 (鳴らし始める前と、足りなくなって鳴らし直すのを待つ間) も働かない。
// 多く作った分は、足りなくなったときと同じく目標に足す (遅れを取り戻したあと、その分だけ多くたまっているので)
void Sound::stretchIfLow() {
    const uint32_t low = target() * 3 / 4, slowest = baseStep_ / 200; // 0.5% の調整で一番小さい刻みとの差
    const uint32_t seen = shortMin_;
    shortMin_ = UINT16_MAX;
    if (seen >= low || !ring_.playing()) {
        // 働かせていたら、0.5% の調整の範囲に戻す (次の 0.25 秒の区切りで決め直す)
        if (step_ < baseStep_ - slowest) step_ = baseStep_ - slowest;
        return;
    }
    const uint32_t most = baseStep_ / STRETCH_MAX_DIV;
    uint32_t cut = (uint32_t)((uint64_t)most * (low - seen) * 2 / low);
    if (cut > most) cut = most;
    if (cut < slowest) cut = slowest;
    step_ = baseStep_ - cut;
    // 次の 1/32 秒に多く作るフレーム数 (おおよそ)
    const unsigned more = (unsigned)((uint64_t)shortFrames() * cut / baseStep_);
    const unsigned cap = AudioRing::SIZE >> 1;
    const unsigned room = target() + extra() < cap ? cap - target() - extra() : 0;
    setExtra(extra() + (more < room ? more : room));
    restartCalm(); // 足りなくなったときと同じく、足りなくならない区間を数え直す
}

// 62.5kHz の 1 サンプルを、フィルタを通して出力の周波数に直す。
// cd (鳴らすものがあるときだけ渡す) と midi の音は、フィルタのあとの出力のフレームに足す (どちらも X68000 の外の機器の音で、
// X68000 の音声の回路を通らない)。CD-ROM 装置のオーディオ (16 ビット) は 1/2 にして足す (OPM と ADPCM との比はおおよそ。
// まーきゅりーゆにっとと同じ)。足して 16 ビットを超えた分は、上限と下限で止める
void Sound::output(int32_t l, int32_t r, MidiOut* midi, CdMixer* cd) {
    // 直流を切る 1 次の高域通過フィルタ: y = k (y + x - x')。k = exp(-2 pi 10 / 62500) = 0.998995 (2^20 倍)
    const int64_t k = 1047522;
    // 1 次の低域通過フィルタを 2 段 (それぞれ 20kHz、係数 0.866 = 28377 / 32768)
    const int32_t a = 28377;
    int32_t in[2] = {l, r};
    int32_t cur[2];
    for (int c = 0; c < 2; c++) {
        hpOut_[c] = (k * (hpOut_[c] + (int64_t)(in[c] - hpIn_[c]) * (1 << 20)) + (1 << 19)) >> 20; // 四捨五入 (切り捨てだと負へずれる)
        hpIn_[c] = in[c];
        in[c] = (int32_t)((hpOut_[c] + (1 << 19)) >> 20);
        lp1_[c] += (int32_t)(((int64_t)(in[c] - lp1_[c]) * a) >> 15);
        lp2_[c] += (int32_t)(((int64_t)(lp1_[c] - lp2_[c]) * a) >> 15);
        int32_t v = lp2_[c];
        cur[c] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    while (pos_ < 65536) {
        int32_t f = (int32_t)(pos_ >> 4); // 12 ビットにして掛け算があふれないようにする
        int32_t ol = prev_[0] + (((cur[0] - prev_[0]) * f) >> 12);
        int32_t orr = prev_[1] + (((cur[1] - prev_[1]) * f) >> 12);
        if (cd) {
            int32_t cl, cr;
            cd->between(f, cl, cr);
            ol += cl >> 1;
            orr += cr >> 1;
        }
        const int32_t bl = ol, br = orr;
        if (midi) midi->mixFrame(ol, orr);
        noteLevels(bl, br, ol, orr);
        if (cd || midi) {
            ol = ol > 32767 ? 32767 : ol < -32768 ? -32768 : ol;
            orr = orr > 32767 ? 32767 : orr < -32768 ? -32768 : orr;
        }
        ring_.push((int16_t)ol, (int16_t)orr);
        pos_ += step_;
        // たまり具合の最小値が目標より多ければ刻みを大きく (作る量を少なく)、少なければ小さくする
        if (target()) {
            unsigned fill = ring_.fill();
            if (fill < minFill_) minFill_ = (uint16_t)fill;
            if (fill < shortMin_) shortMin_ = (uint16_t)fill;
            ++adjustCount_;
            if (adjustCount_ % shortFrames() == 0) stretchIfLow();
            if (adjustCount_ >= quarterFrames()) {
                adjustCount_ = 0;
                tuneLatency();
                int32_t maxAdj = (int32_t)(baseStep_ / 200); // 0.5%
                // 目標は setLatency() の値に、足りなくなったときに足した量を加えたもの
                const int32_t goal = (int32_t)target() + (int32_t)extra();
                // 最小値と目標の差に比例させ、差が目標と同じ大きさで 0.5% にする (それより大きい差でも 0.5% で止める)
                int32_t adj = (int32_t)(((int64_t)minFill_ - goal) * maxAdj / goal);
                if (adj > maxAdj) adj = maxAdj;
                if (adj < -maxAdj) adj = -maxAdj;
                // 遅れを吸収するために刻みを小さくしている間 (stretchIfLow()) は、そのままにする
                if (step_ >= baseStep_ - (uint32_t)maxAdj) step_ = (uint32_t)((int32_t)baseStep_ + adj);
                minFill_ = UINT16_MAX;
            }
        }
    }
    pos_ -= 65536;
    prev_[0] = cur[0];
    prev_[1] = cur[1];
}

} // namespace x68k
