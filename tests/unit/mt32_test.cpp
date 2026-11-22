// MT-32 の音源 (platform/mt32_synth.h、Munt) のテスト。
// ROM のない環境でも通る試験: ROM の大きさと種類の確かめ、組み合わせの合わない ROM、読めなかったときの解放、
// 中身が 0 の ROM (SHA-1 だけを本物とみなす) で Munt を開き、確保の量が Mt32Synth::heapBytes() に収まること、
// 開いたあと (メッセージを受けて音を作る間) に確保しないこと、長いシステムエクスクルーシブと待ち行列があふれたときの扱い、
// MidiShadow の MT-32 向けの送り直し。
// 本物の ROM があれば鳴ることも見る (環境変数で場所を渡す。なければ飛ばす):
//   PIX68K_MT32_CTRL=mt32_ctrl_1_07.rom PIX68K_MT32_PCM=mt32_pcm.rom
//   PIX68K_CM32L_CTRL=cm32l_ctrl_1_02.rom PIX68K_CM32L_PCM=cm32l_pcm.rom (CM-32L と、合わない組み合わせの試験)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <initializer_list>
#include <new>
#include <thread>
#include <vector>

#include "platform/disk_cache_size.h"
#include "platform/mt32_synth.h"
#include "test_check.h"
#include "x68k/midi_shadow.h"

// ---------------------------------------------------------------- 確保を数える
// このテストの中の new / delete をすべてここに通す。counting の間の確保の数と、Circle のヒープで減る量
// (バケットへの切り上げとヘッダ。baremetal/machine_run.cpp の kHeapModel と同じもの) を足していく
namespace {
bool g_counting = false; // 数えている (Mt32Synth を呼んでいる間だけ立てる)
bool g_watch = false;    // feed() と render() の中で Mt32Synth を呼ぶ間、数える (試験の道具の確保は数えない)
uint64_t g_allocs = 0;    // counting の間に確保した数
uint64_t g_heapBytes = 0; // counting の間に確保した量 (Circle のヒープで減る量。解放しても減らさない)
long g_live = 0;          // 生きているブロックの数 (いつも数える)
const uint32_t kCircleBuckets[] = {0x40, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x80000, 0};
const platform::HeapModel kCircleHeap = {kCircleBuckets, 64, 64};

void* countedAlloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (p == nullptr) abort();
    g_live++;
    if (g_counting) {
        g_allocs++;
        g_heapBytes += platform::heapBlockBytes(n, kCircleHeap);
    }
    return p;
}
void countedFree(void* p) {
    if (p == nullptr) return;
    g_live--;
    free(p);
}
} // namespace

void* operator new(size_t n) { return countedAlloc(n); }
void* operator new[](size_t n) { return countedAlloc(n); }
void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, size_t) noexcept { countedFree(p); }
void operator delete[](void* p, size_t) noexcept { countedFree(p); }

namespace {

using platform::Mt32Synth;

// Munt の ROM の表 (external/munt/mt32emu/src/ROMInfo.cpp) にある SHA-1
const char* const kCtrlMt32_107 = "b083518fffb7f66b03c23b7eb4f868e62dc5a987"; // 64KB
const char* const kPcmMt32 = "f6b1eebc4b2d200ec6d3d21d51325d5b48c60252";      // 512KB
const char* const kCtrlCm32l_102 = "a439fbb390da38cada95a7cbb1d6ca199cd66ef8"; // 64KB
const char* const kPcmCm32l = "289cc298ad532b702461bfc738009d9ebe8025ea";      // 1MB

uint8_t* zeros(size_t n) {
    uint8_t* p = new uint8_t[n];
    memset(p, 0, n);
    return p;
}

uint8_t* randomBytes(size_t n, uint32_t seed) {
    uint8_t* p = new uint8_t[n];
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = (uint8_t)(seed >> 16);
    }
    return p;
}

// 中身が 0 の ROM で開く (SHA-1 は本物とみなす)
Mt32Synth::Result loadFake(Mt32Synth& s, size_t ctrlSize, const char* ctrlSha, size_t pcmSize, const char* pcmSha,
                           int rate = 48000) {
    return s.load(zeros(ctrlSize), ctrlSize, zeros(pcmSize), pcmSize, rate, ctrlSha, pcmSha);
}

void feed(Mt32Synth& s, std::initializer_list<uint8_t> bytes) {
    g_counting = g_watch;
    for (uint8_t b : bytes) s.midiByte(b);
    g_counting = false;
}
void feed(Mt32Synth& s, const std::vector<uint8_t>& bytes) {
    g_counting = g_watch;
    for (uint8_t b : bytes) s.midiByte(b);
    g_counting = false;
}

// Roland の DT1 (MT-32、デバイス ID 16 (17 番))。address は 3 バイトのアドレス
std::vector<uint8_t> dt1(uint32_t address, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> m = {0xf0, 0x41, 0x10, 0x16, 0x12, (uint8_t)(address >> 16), (uint8_t)(address >> 8),
                              (uint8_t)address};
    unsigned sum = (address >> 16) + ((address >> 8) & 0x7f) + (address & 0x7f);
    for (uint8_t b : data) {
        m.push_back(b);
        sum += b;
    }
    m.push_back((uint8_t)((128 - sum % 128) % 128));
    m.push_back(0xf7);
    return m;
}

// システムエクスクルーシブの 3 バイトのアドレスを、Munt の readMemory() のアドレスに直す
uint32_t memAddr(uint32_t a) { return ((a & 0x7f0000) >> 2) | ((a & 0x7f00) >> 1) | (a & 0x7f); }

double rms(const std::vector<int16_t>& v) {
    double sum = 0;
    for (int16_t x : v) sum += (double)x * x;
    return v.empty() ? 0 : __builtin_sqrt(sum / (double)v.size());
}

std::vector<int16_t> render(Mt32Synth& s, int frames) {
    std::vector<int16_t> out(2 * (size_t)frames);
    g_counting = g_watch;
    s.renderFrames(out.data(), frames);
    g_counting = false;
    return out;
}

// 左のチャネルの 0 をまたぐ数
int crossings(const std::vector<int16_t>& v) {
    int n = 0;
    for (size_t i = 2; i < v.size(); i += 2)
        if ((v[i - 2] < 0) != (v[i] < 0)) n++;
    return n;
}

// ---------------------------------------------------------------- ROM のない環境で通る試験

void testRomChecks() {
    printf("mt32: ROM sizes, unknown ROMs and mismatched pairs\n");
    CHECK_EQ(Mt32Synth::controlSizeOk(65536), true);
    CHECK_EQ(Mt32Synth::controlSizeOk(131072), true);
    CHECK_EQ(Mt32Synth::controlSizeOk(32768), false); // MAME の 2 つに分けたものは受けない
    CHECK_EQ(Mt32Synth::pcmSizeOk(524288), true);
    CHECK_EQ(Mt32Synth::pcmSizeOk(1048576), true);
    CHECK_EQ(Mt32Synth::pcmSizeOk(262144), false);
    const long live0 = g_live;
    {
        Mt32Synth s;
        CHECK_EQ((int)s.load(nullptr, 0, nullptr, 0, 48000), (int)Mt32Synth::BAD_CONTROL);
        CHECK_EQ((int)s.load(zeros(1000), 1000, zeros(524288), 524288, 48000), (int)Mt32Synth::BAD_CONTROL);
        CHECK_EQ((int)s.load(zeros(65536), 65536, zeros(1000), 1000, 48000, kCtrlMt32_107), (int)Mt32Synth::BAD_PCM);
        // 大きさは合うが Munt の知らない中身
        CHECK_EQ((int)s.load(randomBytes(65536, 1), 65536, randomBytes(524288, 2), 524288, 48000),
                 (int)Mt32Synth::BAD_CONTROL);
        CHECK_EQ((int)s.load(zeros(65536), 65536, randomBytes(524288, 3), 524288, 48000, kCtrlMt32_107),
                 (int)Mt32Synth::BAD_PCM);
        // CONTROL と PCM が逆 (大きさは合わせた SHA-1 の付け替え。PCM の SHA-1 の CONTROL)
        CHECK_EQ((int)s.load(zeros(65536), 65536, zeros(524288), 524288, 48000, kPcmMt32, kPcmMt32),
                 (int)Mt32Synth::BAD_CONTROL);
        CHECK_EQ((int)s.load(zeros(65536), 65536, zeros(524288), 524288, 48000, kCtrlMt32_107, kCtrlMt32_107),
                 (int)Mt32Synth::BAD_PCM);
        // 組み合わせが合わない
        CHECK_EQ((int)loadFake(s, 65536, kCtrlCm32l_102, 524288, kPcmMt32), (int)Mt32Synth::MISMATCH);
        CHECK_EQ((int)loadFake(s, 65536, kCtrlMt32_107, 1048576, kPcmCm32l), (int)Mt32Synth::MISMATCH);
        CHECK_EQ(s.loaded(), false);
        // 読めなかったあとも音を作れる (何もしない)
        int32_t l = 0, r = 0;
        s.mixFrame(l, r);
        s.midiByte(0x90);
        s.midiReset();
        CHECK_EQ(l, 0);
    }
    CHECK_EQ(g_live, live0); // 読めなかったときに確保したものを残さない
}

// 中身が 0 の ROM で開き、確保の量と、開いたあとに確保しないことを見る
void testFakeOpen(int rate, size_t ctrlSize, const char* ctrlSha, size_t pcmSize, const char* pcmSha) {
    printf("mt32: open with zero-filled ROMs (%d Hz, PCM %zu KB): heap bound and no allocation while playing\n",
           rate, pcmSize >> 10);
    const long live0 = g_live;
    {
        Mt32Synth s;
        g_counting = true;
        g_allocs = g_heapBytes = 0;
        // ROM のバッファ (呼ぶ側が確保する) も数える
        const Mt32Synth::Result r = loadFake(s, ctrlSize, ctrlSha, pcmSize, pcmSha, rate);
        g_counting = false;
        CHECK_EQ((int)r, (int)Mt32Synth::OK);
        CHECK_EQ(s.loaded(), true);
        printf("  load: %llu allocations, %llu KB of the Circle heap (bound %llu KB)\n", (unsigned long long)g_allocs,
               (unsigned long long)(g_heapBytes >> 10),
               (unsigned long long)(Mt32Synth::heapBytes(ctrlSize, pcmSize) >> 10));
        CHECK_EQ(g_heapBytes <= Mt32Synth::heapBytes(ctrlSize, pcmSize), true);

        // 開いたあと: ノート、コントロールチェンジ、長いシステムエクスクルーシブ、リバーブのモードを変える、
        // リセット、音を作る。どれも確保しない
        g_watch = true;
        g_allocs = 0;
        render(s, 1000);
        feed(s, {0x91, 0x3c, 0x7f, 0x40, 0x7f, 0xb1, 0x07, 0x64, 0xe1, 0x00, 0x50, 0xc1, 0x05});
        render(s, 1000);
        std::vector<uint8_t> big(200);
        for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i & 0x3f);
        for (uint8_t mode = 0; mode < 4; mode++) {
            feed(s, dt1(0x100001, {mode, 5, 3})); // システムのリバーブのモード、時間、レベル
            feed(s, dt1(0x080000, big));            // 音色のメモリ (200 バイト)
            render(s, 2000);
        }
        g_counting = true;
        s.midiReset();
        g_counting = false;
        render(s, 48000);
        g_watch = false;
        CHECK_EQ(g_allocs, 0u);
        CHECK_EQ(s.droppedSysex(), 0u);
        CHECK_EQ(s.droppedMessages(), 0u);
    }
    CHECK_EQ(g_live, live0); // 消したら確保したものを残さない
}

// システムエクスクルーシブの長さの上限と、待ち行列があふれたとき (待たずに捨てる)
void testOverflow() {
    printf("mt32: too long SysEx and a full MIDI queue are dropped\n");
    Mt32Synth s;
    CHECK_EQ((int)loadFake(s, 65536, kCtrlMt32_107, 524288, kPcmMt32), (int)Mt32Synth::OK);
    std::vector<uint8_t> big(Mt32Synth::SYSEX_MAX - 2, 0); // $F0 と $F7 を足してちょうど SYSEX_MAX
    std::vector<uint8_t> m = {0xf0};
    m.insert(m.end(), big.begin(), big.end());
    m.push_back(0xf7);
    feed(s, m);
    CHECK_EQ(s.droppedSysex(), 0u);
    m.insert(m.begin() + 1, 0); // 1 バイト長い
    feed(s, m);
    CHECK_EQ(s.droppedSysex(), 1u);
    render(s, 256);
    // $F7 なしに次のステータスで終わったもの、途中のリアルタイムのバイトは受ける
    feed(s, {0xf0, 0x41, 0xf8, 0x10, 0x16, 0x90, 0x3c, 0x40});
    CHECK_EQ(s.droppedSysex(), 1u);
    CHECK_EQ(s.droppedMessages(), 0u);
    // 音を作らずに送り続けると、待ち行列 (1024) があふれた分は捨てる
    for (int i = 0; i < 1100; i++) feed(s, {0x91, 0x3c, 0x40});
    CHECK_EQ(s.droppedMessages() > 0, true);
    render(s, 4800);
}

// MidiShadow の MT-32 向けの送り直し (GS のリセット、バンクセレクト、受けていないプログラムチェンジを送らない)
void testShadowMt32() {
    printf("mt32: MidiShadow resends without GS reset, bank select and unsent program changes\n");
    x68k::MidiShadow sh;
    const uint8_t song[] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7, // GS リセット
                            0xb1, 0x00, 0x01, 0xb1, 0x20, 0x02, 0xc1, 0x05,                   // ch 2: バンク、プログラム
                            0xb1, 0x07, 0x50, 0xe1, 0x00, 0x50, 0x92, 0x40};                  // 音量、ベンド、読みかけ
    for (uint8_t b : song) sh.feed(b);
    std::vector<uint8_t> gen, mt;
    sh.emit([&](uint8_t b) { gen.push_back(b); });
    sh.emit([&](uint8_t b) { mt.push_back(b); }, x68k::MidiOut::MT32);
    auto count = [](const std::vector<uint8_t>& v, std::initializer_list<uint8_t> seq) {
        const std::vector<uint8_t> w(seq);
        int n = 0;
        for (size_t i = 0; i + w.size() <= v.size(); i++)
            if (memcmp(v.data() + i, w.data(), w.size()) == 0) n++;
        return n;
    };
    int pcGen = 0, pcMt = 0;
    for (uint8_t b : gen) pcGen += (b & 0xf0) == 0xc0;
    for (uint8_t b : mt) pcMt += (b & 0xf0) == 0xc0;
    CHECK_EQ(gen[0], 0xf0); // GM / GS の音源にはリセットを送る
    CHECK_EQ(pcGen, 16);    // 16 チャネルすべてにプログラムチェンジ
    CHECK_EQ(count(mt, {0xf0}), 0);
    CHECK_EQ(pcMt, 1);
    CHECK_EQ(count(mt, {0xc1, 0x05}), 1);
    CHECK_EQ(count(mt, {0xb1, 0x00}), 0);
    CHECK_EQ(count(mt, {0xb1, 0x20}), 0);
    CHECK_EQ(count(mt, {0xb1, 0x07, 0x50}), 1);
    CHECK_EQ(count(mt, {0xe1, 0x00, 0x50}), 1);
    // 最後は読みかけのノートオン
    CHECK_EQ(mt.size() >= 2 && mt[mt.size() - 2] == 0x92 && mt.back() == 0x40, true);
}

// ---------------------------------------------------------------- 本物の ROM を使う試験

uint8_t* readFile(const char* path, size_t& size) {
    size = 0;
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return nullptr;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = new uint8_t[n > 0 ? (size_t)n : 1];
    if (n <= 0 || fread(p, 1, (size_t)n, f) != (size_t)n) {
        delete[] p;
        p = nullptr;
    } else {
        size = (size_t)n;
    }
    fclose(f);
    return p;
}

Mt32Synth::Result loadReal(Mt32Synth& s, const char* ctrl, const char* pcm, int rate = 48000) {
    size_t cn = 0, pn = 0;
    uint8_t* c = readFile(ctrl, cn);
    uint8_t* p = readFile(pcm, pn);
    return s.load(c, cn, p, pn, rate);
}

void testRealRom(const char* ctrl, const char* pcm) {
    printf("mt32: real ROMs %s, %s\n", ctrl, pcm);
    Mt32Synth s;
    g_counting = true;
    g_allocs = g_heapBytes = 0;
    CHECK_EQ((int)loadReal(s, ctrl, pcm), (int)Mt32Synth::OK);
    g_counting = false;
    if (!s.loaded()) return;
    printf("  %s, %s: load %llu KB of the Circle heap\n", s.controlName(), s.pcmName(),
           (unsigned long long)(g_heapBytes >> 10));
    // 鳴らす前は無音
    CHECK_EQ(rms(render(s, 4800)) < 1.0, true);
    // チャネル 2 (パート 1) のノートオンで鳴る
    feed(s, {0x91, 0x39, 0x7f});
    std::vector<int16_t> a = render(s, 24000);
    printf("  note 57: rms %.0f, %d partials\n", rms(a), s.activePartials());
    CHECK_EQ(rms(a) > 300.0, true);
    CHECK_EQ(s.activePartials() > 0, true);
    feed(s, {0x81, 0x39, 0x00});
    render(s, 96000);
    CHECK_EQ(s.activePartials(), 0);
    // 高い音は 0 をまたぐ数が多い
    feed(s, {0x91, 0x51, 0x7f});
    std::vector<int16_t> b = render(s, 24000);
    feed(s, {0x81, 0x51, 0x00});
    render(s, 96000);
    printf("  zero crossings: note 57 %d, note 81 %d\n", crossings(a), crossings(b));
    CHECK_EQ(crossings(b) > crossings(a) * 2, true);

    // 64 バイトを超えるシステムエクスクルーシブ: パッチのメモリ (05 00 00) に 16 パッチ (128 バイト) を書いて読み返す
    std::vector<uint8_t> patches;
    for (int i = 0; i < 16; i++) {
        const uint8_t p[8] = {(uint8_t)(i & 3), (uint8_t)(i * 3 & 63), (uint8_t)(i + 10), (uint8_t)(i * 6),
                              (uint8_t)(i & 15), (uint8_t)(i & 3), (uint8_t)(i & 1), 0};
        patches.insert(patches.end(), p, p + 8);
    }
    g_watch = true;
    g_allocs = 0;
    feed(s, dt1(0x050000, patches));
    render(s, 4800);
    g_watch = false;
    CHECK_EQ(g_allocs, 0u);
    std::vector<uint8_t> back(patches.size());
    s.readMemory(memAddr(0x050000), (uint32_t)back.size(), back.data());
    CHECK_EQ(back == patches, true);
    CHECK_EQ(s.droppedSysex(), 0u);
}

// MidiShadow の送り直しで、別の (新しく開いた) MT-32 のパートの設定が、直接送ったものと同じになる
void testRealResync(const char* ctrl, const char* pcm) {
    printf("mt32: MidiShadow resync restores the part settings on a fresh MT-32\n");
    Mt32Synth a, b, c;
    if (loadReal(a, ctrl, pcm) != Mt32Synth::OK || loadReal(b, ctrl, pcm) != Mt32Synth::OK ||
        loadReal(c, ctrl, pcm) != Mt32Synth::OK) {
        CHECK_EQ(0, 1);
        return;
    }
    x68k::MidiShadow sh;
    const uint8_t song[] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7, // GS リセット (MT-32 は無視)
                            0xc1, 0x05, 0xb1, 0x07, 0x50, 0xb1, 0x0a, 0x20, 0xe1, 0x00, 0x50,
                            0xc3, 0x30, 0xb3, 0x07, 0x40};
    for (uint8_t x : song) {
        sh.feed(x);
        a.midiByte(x);
    }
    sh.emit([&](uint8_t x) { b.midiByte(x); }, x68k::MidiOut::MT32);
    sh.emit([&](uint8_t x) { c.midiByte(x); }); // GM / GS の送り方
    render(a, 4800);
    render(b, 4800);
    render(c, 4800);
    // パートの一時領域 (03 00 00、パートごとに 16 バイト: パッチ 8 バイト、音量、パン ...)
    const uint32_t len = 16 * 8;
    std::vector<uint8_t> ma(len), mb(len), mc(len);
    a.readMemory(memAddr(0x030000), len, ma.data());
    b.readMemory(memAddr(0x030000), len, mb.data());
    c.readMemory(memAddr(0x030000), len, mc.data());
    CHECK_EQ(ma == mb, true);
    // GM / GS の送り方だと、プログラムチェンジを受けていないパートの音色がプログラム 0 に変わる
    CHECK_EQ(ma == mc, false);
}

// 送り直しのピッチベンドの幅 (RPN 0)。直接送った音源と、覚えた設定を送り直した音源で、パートの一時領域が同じになること
bool sameAfterResync(const char* ctrl, const char* pcm, const std::vector<uint8_t>& song) {
    Mt32Synth a, b;
    if (loadReal(a, ctrl, pcm) != Mt32Synth::OK || loadReal(b, ctrl, pcm) != Mt32Synth::OK) return false;
    x68k::MidiShadow sh;
    for (uint8_t x : song) {
        sh.feed(x);
        a.midiByte(x);
    }
    sh.emit([&](uint8_t x) { b.midiByte(x); }, x68k::MidiOut::MT32);
    render(a, 480);
    render(b, 480);
    const uint32_t len = 16 * 8;
    std::vector<uint8_t> ma(len), mb(len);
    a.readMemory(memAddr(0x030000), len, ma.data());
    b.readMemory(memAddr(0x030000), len, mb.data());
    for (uint32_t i = 0; i < len; i++)
        if (ma[i] != mb[i]) {
            printf("  part %u byte %u: direct %u, resent %u\n", i / 16 + 1, i % 16, ma[i], mb[i]);
            return false;
        }
    return true;
}

void testRealResyncBendRange(const char* ctrl, const char* pcm) {
    printf("mt32: MidiShadow resync restores the pitch bend range (RPN 0)\n");
    // 幅 2 (GS の初めの値と同じだが、MT-32 の初めの値 (パッチの値、12) とは違う)
    CHECK_EQ(sameAfterResync(ctrl, pcm, {0xb1, 0x65, 0x00, 0xb1, 0x64, 0x00, 0xb1, 0x06, 0x02}), true);
    // 幅 5 のあとのプログラムチェンジ (MT-32 はパッチの幅に戻す)
    CHECK_EQ(sameAfterResync(ctrl, pcm, {0xb2, 0x65, 0x00, 0xb2, 0x64, 0x00, 0xb2, 0x06, 0x05, 0xc2, 0x10}), true);
    // 幅 7 のあとの CC 121 (MT-32 は幅を変えない)
    CHECK_EQ(sameAfterResync(ctrl, pcm, {0xb3, 0x65, 0x00, 0xb3, 0x64, 0x00, 0xb3, 0x06, 0x07, 0xb3, 0x79, 0x00}), true);
    // プログラムチェンジのあとの幅 0 と、24 を超える幅 (Munt は 24 に収める)
    CHECK_EQ(sameAfterResync(ctrl, pcm, {0xc4, 0x20, 0xb4, 0x65, 0x00, 0xb4, 0x64, 0x00, 0xb4, 0x06, 0x00, 0xb5, 0x65,
                                         0x00, 0xb5, 0x64, 0x00, 0xb5, 0x06, 0x50}),
             true);
    // 乱数: プログラムチェンジ、RPN 0 を選んでのデータエントリ (選び直してから送る。CC 121 のあとに選び直さない
    // データエントリは、分かっている違い)、CC 121、ピッチベンド、RPN ヌルの選択を混ぜる
    uint32_t seed = 12345;
    auto next = [&](uint32_t n) {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) % n;
    };
    int bad = 0;
    for (int t = 0; t < 100; t++) {
        std::vector<uint8_t> song;
        for (int i = 0; i < 12; i++) {
            const uint8_t ch = (uint8_t)(1 + next(8));
            switch (next(5)) {
            case 0: song.insert(song.end(), {(uint8_t)(0xc0 | ch), (uint8_t)next(128)}); break;
            case 1:
                song.insert(song.end(), {(uint8_t)(0xb0 | ch), 0x65, 0x00, (uint8_t)(0xb0 | ch), 0x64, 0x00,
                                         (uint8_t)(0xb0 | ch), 0x06, (uint8_t)next(30)});
                break;
            case 2: song.insert(song.end(), {(uint8_t)(0xb0 | ch), 0x79, 0x00}); break;
            case 3: song.insert(song.end(), {(uint8_t)(0xe0 | ch), (uint8_t)next(128), (uint8_t)next(128)}); break;
            default: song.insert(song.end(), {(uint8_t)(0xb0 | ch), 0x65, 0x7f, (uint8_t)(0xb0 | ch), 0x64, 0x7f}); break;
            }
        }
        if (!sameAfterResync(ctrl, pcm, song)) bad++;
    }
    CHECK_EQ(bad, 0);
}

// 音を作る前に midiReset() を呼んだとき: 待ち行列に残っていたノートオンが、リセットのあとで鳴り続けないこと
// (ステートを読んだとき、送ったばかりのメッセージが待ち行列に残っていることがある)
void testRealResetQueued(const char* ctrl, const char* pcm) {
    printf("mt32: midiReset() also stops notes still in the MIDI queue\n");
    Mt32Synth s;
    if (loadReal(s, ctrl, pcm) != Mt32Synth::OK) {
        CHECK_EQ(0, 1);
        return;
    }
    feed(s, {0xc1, 0x00, 0x91, 0x45, 0x7f}); // パート 1 にプログラムチェンジとノートオン (待ち行列に入る)
    s.midiReset();
    render(s, 48000);
    const std::vector<int16_t> late = render(s, 48000); // 後半の 1 秒 (リリースは消えているはず)
    printf("  partials %d, rms of the second second %.1f\n", s.activePartials(), rms(late));
    CHECK_EQ(s.activePartials(), 0);
    CHECK_EQ(rms(late) < 10.0, true);
    // リセットのあとに送った音は鳴る (待ち行列は使える)
    feed(s, {0x91, 0x45, 0x7f});
    CHECK_EQ(rms(render(s, 24000)) > 300.0, true);
}

// 出力が 48kHz でないとき (ホスト版の SDL の 44.1kHz など) は Munt の変換で鳴らす
void testRealResample(const char* ctrl, const char* pcm) {
    printf("mt32: 44100 Hz output through the sample rate converter\n");
    Mt32Synth s;
    CHECK_EQ((int)loadReal(s, ctrl, pcm, 44100), (int)Mt32Synth::OK);
    g_watch = true;
    g_allocs = 0;
    feed(s, {0x91, 0x45, 0x7f});
    const std::vector<int16_t> v = render(s, 22050);
    g_watch = false;
    CHECK_EQ(g_allocs, 0u);
    CHECK_EQ(rms(v) > 300.0, true);
}

void testRealMismatch(const char* mt32Ctrl, const char* mt32Pcm, const char* cmCtrl, const char* cmPcm) {
    printf("mt32: real CM-32L ROMs and mismatched real pairs\n");
    {
        Mt32Synth s;
        CHECK_EQ((int)loadReal(s, cmCtrl, cmPcm), (int)Mt32Synth::OK);
        if (s.loaded()) printf("  %s, %s\n", s.controlName(), s.pcmName());
        feed(s, {0x91, 0x45, 0x7f});
        CHECK_EQ(rms(render(s, 24000)) > 300.0, true);
    }
    const long live0 = g_live;
    {
        Mt32Synth s;
        CHECK_EQ((int)loadReal(s, mt32Ctrl, cmPcm), (int)Mt32Synth::MISMATCH);
        CHECK_EQ((int)loadReal(s, cmCtrl, mt32Pcm), (int)Mt32Synth::MISMATCH);
        CHECK_EQ((int)loadReal(s, mt32Pcm, mt32Ctrl), (int)Mt32Synth::BAD_CONTROL); // 逆に渡した
    }
    CHECK_EQ(g_live, live0);
}

} // namespace

// 捨てた数は、音を作るスレッドが足している間に、ほかのスレッドからログ用に読んでよい (ベアメタル版はコア 0 が読む)。
// 読んだ値は減らず、終わったあとの数は合っている。-fsanitize=thread でビルドすると、データの競合も調べられる
void testDroppedFromAnotherThread() {
    printf("mt32: the dropped counters can be read from another thread\n");
    Mt32Synth s;
    CHECK_EQ((int)loadFake(s, 65536, kCtrlMt32_107, 524288, kPcmMt32), (int)Mt32Synth::OK);
    std::vector<uint8_t> m(Mt32Synth::SYSEX_MAX + 1, 0); // 1 バイト長い
    m.front() = 0xf0;
    m.back() = 0xf7;
    const std::vector<uint8_t> note = {0x91, 0x3c, 0x40};
    std::atomic<bool> done{false};
    int backwards = 0;
    unsigned reads = 0;
    std::thread reader([&] {
        uint32_t sysex = 0, messages = 0;
        while (!done.load(std::memory_order_acquire)) {
            const uint32_t x = s.droppedSysex(), y = s.droppedMessages();
            if (x < sysex || y < messages) backwards++;
            sysex = x;
            messages = y;
            reads++;
        }
    });
    const int rounds = 300;
    for (int i = 0; i < rounds; i++) {
        for (uint8_t b : m) s.midiByte(b);
        for (int k = 0; k < 8; k++)
            for (uint8_t b : note) s.midiByte(b); // 音を作らないので、待ち行列 (1024) があふれて捨てる
    }
    done.store(true, std::memory_order_release);
    reader.join();
    CHECK_EQ(reads > 0, true);
    CHECK_EQ(backwards, 0);
    CHECK_EQ(s.droppedSysex(), (uint32_t)rounds);
    CHECK_EQ(s.droppedMessages() > 0, true);
}

int main() {
    testRomChecks();
    testFakeOpen(48000, 65536, kCtrlMt32_107, 524288, kPcmMt32);
    testFakeOpen(48000, 65536, kCtrlCm32l_102, 1048576, kPcmCm32l);
    testFakeOpen(44100, 65536, kCtrlMt32_107, 524288, kPcmMt32);
    testFakeOpen(32000, 65536, kCtrlMt32_107, 524288, kPcmMt32);
    testOverflow();
    testDroppedFromAnotherThread();
    testShadowMt32();
    const char* ctrl = getenv("PIX68K_MT32_CTRL");
    const char* pcm = getenv("PIX68K_MT32_PCM");
    if (ctrl && pcm && *ctrl && *pcm) {
        testRealRom(ctrl, pcm);
        testRealResync(ctrl, pcm);
        testRealResample(ctrl, pcm);
        testRealResetQueued(ctrl, pcm);
        testRealResyncBendRange(ctrl, pcm);
        const char* cmCtrl = getenv("PIX68K_CM32L_CTRL");
        const char* cmPcm = getenv("PIX68K_CM32L_PCM");
        if (cmCtrl && cmPcm && *cmCtrl && *cmPcm) {
            testRealRom(cmCtrl, cmPcm);
            testRealMismatch(ctrl, pcm, cmCtrl, cmPcm);
        } else {
            printf("mt32: PIX68K_CM32L_CTRL / PIX68K_CM32L_PCM are not set; CM-32L tests skipped\n");
        }
    } else {
        printf("mt32: PIX68K_MT32_CTRL / PIX68K_MT32_PCM are not set; tests with real ROMs skipped\n");
    }
    return finish();
}
