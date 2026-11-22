// MT-32 / CM-32L の MIDI 音源。mt32_synth.h を参照
#include "platform/mt32_synth.h"

#include <string.h>

#include "File.h"
#include "ROMInfo.h"
#include "SampleRateConverter.h"
#include "Synth.h"

namespace platform {

namespace {

// ROM が Munt の知っている完全なもの (2 つに分かれていない) か。知っていれば ROMImage を返す
const MT32Emu::ROMImage* identify(MT32Emu::ArrayFile* file, MT32Emu::ROMInfo::Type type) {
    const MT32Emu::ROMImage* image = MT32Emu::ROMImage::makeROMImage(file, MT32Emu::ROMInfo::getFullROMInfos());
    if (image == nullptr) return nullptr;
    const MT32Emu::ROMInfo* info = image->getROMInfo();
    if (info == nullptr || info->type != type || info->pairType != MT32Emu::ROMInfo::Full) {
        MT32Emu::ROMImage::freeROMImage(image);
        return nullptr;
    }
    return image;
}

bool listed(const MT32Emu::ROMInfo* const* list, const MT32Emu::ROMInfo* info) {
    for (; *list != nullptr; list++)
        if (*list == info) return true;
    return false;
}

// CONTROL と PCM を一緒に使う機種 (MT-32 v1.07、CM-32L v1.02 など) があるか
bool compatible(const MT32Emu::ROMInfo* control, const MT32Emu::ROMInfo* pcm) {
    for (const MT32Emu::MachineConfiguration* const* m = MT32Emu::MachineConfiguration::getAllMachineConfigurations();
         *m != nullptr; m++) {
        const MT32Emu::ROMInfo* const* roms = (*m)->getCompatibleROMInfos();
        if (listed(roms, control) && listed(roms, pcm)) return true;
    }
    return false;
}

MT32Emu::ArrayFile* newFile(const uint8_t* data, size_t size, const char* sha1) {
    if (sha1 == nullptr) return new MT32Emu::ArrayFile(data, size);
    MT32Emu::File::SHA1Digest digest = {};
    strncpy(digest, sha1, sizeof digest - 1);
    return new MT32Emu::ArrayFile(data, size, digest);
}

inline int16_t clamp16(float v) {
    const float x = v * 32768.0f;
    return x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (int16_t)x;
}

} // namespace

// Munt からの知らせ。既定の ReportHandler は printf でデバッグの文や LCD の文字を出すので、黙らせる。
// 待ち行列が満杯のときは、待たずに捨てる (onMIDIQueueOverflow() が false)
struct Mt32Synth::Reporter : public MT32Emu::ReportHandler {
    void printDebug(const char*, va_list) override {}
    void showLCDMessage(const char*) override {}
};

const char* Mt32Synth::resultText(Result r) {
    switch (r) {
    case OK: return "ok";
    case BAD_CONTROL: return "the CONTROL ROM is not a known MT-32 / CM-32L control ROM";
    case BAD_PCM: return "the PCM ROM is not a known MT-32 / CM-32L PCM ROM";
    case MISMATCH: return "the CONTROL and PCM ROMs are not of the same model";
    case OPEN_FAILED: return "Munt cannot open the ROMs";
    case BAD_RATE: return "the output sample rate is not supported";
    }
    return "?";
}

Mt32Synth::Mt32Synth() = default;

Mt32Synth::~Mt32Synth() { freeAll(); }

void Mt32Synth::freeAll() {
    delete src_;
    src_ = nullptr;
    if (synth_ != nullptr) synth_->close();
    delete synth_;
    synth_ = nullptr;
    if (controlImage_ != nullptr) MT32Emu::ROMImage::freeROMImage(controlImage_);
    if (pcmImage_ != nullptr) MT32Emu::ROMImage::freeROMImage(pcmImage_);
    controlImage_ = pcmImage_ = nullptr;
    delete controlFile_;
    delete pcmFile_;
    controlFile_ = pcmFile_ = nullptr;
    delete[] controlData_;
    delete[] pcmData_;
    controlData_ = pcmData_ = nullptr;
    delete reporter_;
    reporter_ = nullptr;
}

Mt32Synth::Result Mt32Synth::load(uint8_t* control, size_t controlSize, uint8_t* pcm, size_t pcmSize, int rate,
                                  const char* controlSha1, const char* pcmSha1) {
    freeAll();
    controlData_ = control;
    pcmData_ = pcm;
    if (control == nullptr || !controlSizeOk(controlSize)) {
        freeAll();
        return BAD_CONTROL;
    }
    if (pcm == nullptr || !pcmSizeOk(pcmSize)) {
        freeAll();
        return BAD_PCM;
    }
    controlFile_ = newFile(control, controlSize, controlSha1);
    pcmFile_ = newFile(pcm, pcmSize, pcmSha1);
    controlImage_ = identify(controlFile_, MT32Emu::ROMInfo::Control);
    if (controlImage_ == nullptr) {
        freeAll();
        return BAD_CONTROL;
    }
    pcmImage_ = identify(pcmFile_, MT32Emu::ROMInfo::PCM);
    if (pcmImage_ == nullptr) {
        freeAll();
        return BAD_PCM;
    }
    if (!compatible(controlImage_->getROMInfo(), pcmImage_->getROMInfo())) {
        freeAll();
        return MISMATCH;
    }
    reporter_ = new Reporter;
    synth_ = new MT32Emu::Synth(reporter_);
    // 音を作る間に確保しないための設定 (mt32_synth.h)。どちらも open() の前に決める
    synth_->preallocateReverbMemory(true);
    synth_->configureMIDIEventQueueSysexStorage(QUEUE_SYSEX_BYTES);
    const MT32Emu::AnalogOutputMode mode = MT32Emu::SampleRateConverter::getBestAnalogOutputMode(rate);
    // 変換 (srchelper) を入れていないビルド (ベアメタル版) では、Munt がそのまま出せる周波数だけを受ける
    const bool rateOk = rate > 0 && (MT32Emu::Synth::getStereoOutputSampleRate(mode) == (MT32Emu::Bit32u)rate ||
                                     MT32Emu::SampleRateConverter::getSupportedOutputSampleRate(rate) > 0);
    if (!rateOk) {
        freeAll();
        return BAD_RATE;
    }
    if (!synth_->open(*controlImage_, *pcmImage_, mode)) {
        freeAll();
        return OPEN_FAILED;
    }
    synth_->setMIDIDelayMode(MT32Emu::MIDIDelayMode_IMMEDIATE);
    rate_ = rate;
    direct_ = synth_->getStereoOutputSampleRate() == (MT32Emu::Bit32u)rate;
    src_ = new MT32Emu::SampleRateConverter(*synth_, rate, MT32Emu::SamplerateConversionQuality_GOOD);
    pos_ = BLOCK;
    return OK;
}

const char* Mt32Synth::controlName() const {
    return controlImage_ != nullptr ? controlImage_->getROMInfo()->description : "";
}

const char* Mt32Synth::pcmName() const { return pcmImage_ != nullptr ? pcmImage_->getROMInfo()->description : ""; }

void Mt32Synth::setGain(float gain) {
    if (synth_ == nullptr) return;
    synth_->setOutputGain(gain);
    synth_->setReverbOutputGain(gain);
}

bool Mt32Synth::readMemory(uint32_t address, uint32_t length, uint8_t* out) const {
    if (synth_ == nullptr) return false;
    synth_->readMemory(address, length, out);
    return true;
}

int Mt32Synth::activePartials() const {
    if (synth_ == nullptr) return 0;
    MT32Emu::PartialState states[256];
    const unsigned n = synth_->getPartialCount();
    if (n > sizeof states / sizeof states[0]) return -1;
    synth_->getPartialStates(states);
    int active = 0;
    for (unsigned i = 0; i < n; i++)
        if (states[i] != MT32Emu::PartialState_INACTIVE) active++;
    return active;
}

// Munt の中の時刻 (32kHz) で、今のまとまりの中で出したフレームの数だけ後ろ (mt32_synth.h)
uint32_t Mt32Synth::timestamp() const {
    return synth_->getInternalRenderedSampleCount() + (uint32_t)((uint64_t)pos_ * MT32Emu::SAMPLE_RATE / (uint32_t)rate_);
}

void Mt32Synth::sendShort(uint32_t msg) {
    if (!synth_->playMsg(msg, timestamp())) __atomic_fetch_add(&droppedMessages_, 1u, __ATOMIC_RELAXED);
}

void Mt32Synth::endSysex() {
    inSysex_ = false;
    if (sysexOver_) {
        __atomic_fetch_add(&droppedSysex_, 1u, __ATOMIC_RELAXED);
        return;
    }
    sysex_[sysexLen_++] = 0xf7;
    if (!synth_->playSysex(sysex_, (MT32Emu::Bit32u)sysexLen_, timestamp())) __atomic_fetch_add(&droppedMessages_, 1u, __ATOMIC_RELAXED);
}

void Mt32Synth::midiByte(uint8_t b) {
    if (synth_ == nullptr) return;
    if (b >= 0xf8) return; // リアルタイム
    if (b & 0x80) {
        if (inSysex_) endSysex(); // $F7 か、次のステータスのバイトで終わる
        if (b == 0xf7) {
            status_ = 0;
            return;
        }
        if (b == 0xf0) {
            status_ = 0;
            skip_ = 0;
            inSysex_ = true;
            sysexOver_ = false;
            sysex_[0] = 0xf0;
            sysexLen_ = 1;
            return;
        }
        if (b > 0xf0) { // システムコモン
            status_ = 0;
            skip_ = (b == 0xf2) ? 2 : (b == 0xf1 || b == 0xf3) ? 1 : 0;
            return;
        }
        status_ = b;
        need_ = ((b & 0xe0) == 0xc0) ? 1 : 2; // $Cx と $Dx はデータが 1 バイト
        have_ = 0;
        skip_ = 0;
        return;
    }
    if (inSysex_) {
        if (sysexLen_ < SYSEX_MAX - 1) sysex_[sysexLen_++] = b; // $F7 の分を残す
        else sysexOver_ = true;
        return;
    }
    if (skip_ != 0) {
        skip_--;
        return;
    }
    if (status_ == 0) return; // ステータスのないデータのバイト
    data_[have_++] = b;
    if (have_ < need_) return;
    have_ = 0; // 次はランニングステータス
    sendShort((uint32_t)status_ | (uint32_t)data_[0] << 8 | (need_ > 1 ? (uint32_t)data_[1] << 16 : 0));
}

// 鳴っている音を止める。Munt は CC 124-127 をホールドペダルを離してオールノートオフとして扱う (実機の LAPC-I と同じ)。
// チャネルの割り当てによらず、9 つのパートに直接送る (音はリリースに入って消える)。
// playMsgOnPart() はすぐに処理されるが、midiByte() で送ったメッセージは時刻を付けて待ち行列に入っている。先に
// 待ち行列を処理しきる (flushMIDIQueue())。そうしないと、リセットの前に送ったノートオンがリセットのあとで処理され、
// ノートオフが来ずに鳴り続ける。プログラムチェンジなども捨てずに処理するので、Munt の設定は MIDI ボードが送ったとおりになる
void Mt32Synth::midiReset() {
    if (synth_ == nullptr) return;
    synth_->flushMIDIQueue();
    for (MT32Emu::Bit8u part = 0; part < 9; part++) synth_->playMsgOnPart(part, 0xb, 0x7c, 0);
}

void Mt32Synth::render() {
    if (direct_) {
        // 周波数が同じなら、Munt は Synth::render() をそのまま呼ぶ (スタックに大きな配列を置かない)
        src_->getOutputSamples(buf_, BLOCK);
    } else {
        // 変換するときは float で受けて自分で 16 ビットにする (SampleRateConverter の 16 ビットの版は、
        // スタックに 32KB の配列を置くため)
        src_->getOutputSamples(fbuf_, BLOCK);
        for (int i = 0; i < 2 * BLOCK; i++) buf_[i] = clamp16(fbuf_[i]);
    }
    pos_ = 0;
}

void Mt32Synth::renderFrames(int16_t* out, int n) {
    for (int i = 0; i < n; i++) {
        int32_t l = 0, r = 0;
        mixFrame(l, r);
        out[2 * i] = (int16_t)l;
        out[2 * i + 1] = (int16_t)r;
    }
}

} // namespace platform
