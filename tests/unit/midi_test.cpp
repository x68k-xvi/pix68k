// MIDI ボード (CZ-6BM1、x68k/ym3802.h) と MIDI の音源 (platform/midi_stream.h、platform/sf2_synth.h) のテスト。
// - YM3802 のレジスタ、送信の時間 (31.25kbps で 1 バイト 320us)、FIFO、割り込みとベクタ、タイマ
// - テスト用の IPL ROM の 68000 のプログラムから、MIDI のドライバと同じ手順で使う (TSR を見て TDR に書く送信、
//   送信 FIFO が空の割り込みでの送信、汎用タイマの割り込み)。送ったバイトが送った順に、時刻どおりに音源に届くか
// - 音の合成を別のコアで行うとき (記録を当てる側) も、音源に届く位置と作る音が 1 つのコアのときと同じか
// - MIDI のバイト列の分け方 (ランニングステータス、リアルタイム、システムエクスクルーシブ)
// - SoundFont の音源 (試験用の正弦波の SF2) が鳴り、止まるか
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <thread>
#include <vector>

#include "platform/midi_stream.h"
#include "platform/sf2_synth.h"
#include "sf2_builder.h"
#include "test_check.h"
#include "x68k/machine.h"
#include "x68k/midi_shadow.h"
#include "x68k/sound.h"
#include "x68k/sound_replay.h"
#include "x68k/state.h"
#include "x68k/video_journal.h"
#include "x68k/ym3802.h"

namespace {

// ---------------------------------------------------------------- YM3802 だけ

// 群 g を選んで Rn に書く / 読む
void wr(x68k::Ym3802& y, int g, int n, uint8_t v) {
    y.write(1, (uint8_t)g);
    y.write(n, v);
}
uint8_t rd(x68k::Ym3802& y, int g, int n) {
    y.write(1, (uint8_t)g);
    return y.read(n);
}

void testRegisters() {
    printf("ym3802: registers, FIFO and transmit time\n");
    x68k::Ym3802 y;
    // 31.25kbps (TRR = $08)、8 ビット、ストップビット 1 つ: 10 ビットで 320us = 4MHz の 1280 クロック
    wr(y, 4, 4, 0x08);
    CHECK_EQ(y.byteClocks(), 1280u);
    wr(y, 4, 5, 0x02); // ストップビット 2 つ
    CHECK_EQ(y.byteClocks(), 1408u);
    wr(y, 4, 5, 0x00);
    // 送信 FIFO: 空で、空きがある
    CHECK_EQ(rd(y, 5, 4), 0xc0);
    // 送らない (TCR = 0) 間は FIFO にたまる。16 バイトで満杯
    for (int i = 0; i < 16; i++) y.write(6, (uint8_t)i);
    CHECK_EQ(y.read(4), 0x01); // 満杯、送るものがある
    y.write(6, 0x55);          // あふれた分は捨てる
    CHECK_EQ(y.txCount(), 16);
    y.advance(100000);
    CHECK_EQ(y.takeSent(), -1);
    // 送る
    y.write(5, 0x01);
    CHECK_EQ(y.txCount(), 15); // 1 バイト目はシフトレジスタへ
    y.advance(1279);
    CHECK_EQ(y.takeSent(), -1);
    y.advance(1);
    CHECK_EQ(y.takeSent(), 0);
    CHECK_EQ(y.takeSent(), -1);
    y.advance(1280 * 15);
    for (int i = 1; i < 16; i++) CHECK_EQ(y.takeSent(), i);
    CHECK_EQ(y.takeSent(), -1);
    CHECK_EQ(y.read(4), 0xc0);
    // 書いた値を読み返す (覚えるだけのレジスタ)、リセット
    wr(y, 9, 5, 0x5a);
    CHECK_EQ(rd(y, 9, 5), 0x5a);
    CHECK_EQ(y.read(1), 9);
    y.write(1, 0x80);
    CHECK_EQ(y.read(1), 0);
    CHECK_EQ(rd(y, 9, 5), 0);
    CHECK_EQ(rd(y, 4, 4), 0);
    // 受信: MIDI IN はないので、RSR と RDR は 0
    CHECK_EQ(rd(y, 3, 4), 0);
    CHECK_EQ(rd(y, 3, 6), 0);
}

void testInterrupts() {
    printf("ym3802: interrupts, vectors and timers\n");
    x68k::Ym3802 y;
    wr(y, 0, 4, 0x80); // IOR: ベクタ $80-$8E
    CHECK_EQ(y.read(0), 0x90); // 要因がない
    CHECK_EQ(y.irq(), false);
    // 送信 FIFO が空の要因: 許可すると、空なのですぐに立つ。ICR で消しても空ならまた立つ
    wr(y, 0, 6, 0x40);
    CHECK_EQ(y.isr(), 0x40);
    CHECK_EQ(y.irq(), true);
    CHECK_EQ(y.read(0), 0x8c);
    y.write(3, 0x40);
    CHECK_EQ(y.isr(), 0x40);
    // TDR に書くと消え、FIFO が空になると (シフトレジスタに移ると) また立つ
    wr(y, 4, 4, 0x08);
    y.write(1, 5);
    y.write(6, 0x90);
    y.write(6, 0x3c);
    CHECK_EQ(y.isr() & 0x40, 0);
    y.write(5, 0x01); // 送る: 1 バイト目がシフトレジスタへ。FIFO にはまだ 1 バイト
    CHECK_EQ(y.isr() & 0x40, 0);
    y.advance(1280); // 2 バイト目がシフトレジスタへ: FIFO が空
    CHECK_EQ(y.isr() & 0x40, 0x40);
    // 許可を外すと消える
    wr(y, 0, 6, 0x00);
    CHECK_EQ(y.isr(), 0);
    CHECK_EQ(y.irq(), false);

    // 汎用タイマ: 125 x 8us = 1ms ごと
    wr(y, 0, 6, 0x80);
    wr(y, 8, 4, 125);
    wr(y, 8, 5, 0x00);
    y.advance(3999);
    CHECK_EQ(y.isr(), 0);
    y.advance(1);
    CHECK_EQ(y.isr(), 0x80);
    CHECK_EQ(y.read(0), 0x8e);
    y.write(3, 0x80);
    CHECK_EQ(y.isr(), 0);
    y.advance(4000);
    CHECK_EQ(y.isr(), 0x80);
    y.write(3, 0x80);
    // MIDI クロックタイマ (14 ビット: R87 の下位 6 ビットが上位): $0101 x 8us
    wr(y, 0, 6, 0x82);
    wr(y, 8, 6, 0x01);
    wr(y, 8, 7, 0xc1); // 上位 2 ビットは使わない
    y.advance(257 * 32 - 1);
    CHECK_EQ(y.isr() & 0x02, 0);
    y.advance(1);
    CHECK_EQ(y.isr() & 0x02, 0x02);
    // 2 つ立っていれば、ビットの番号が小さい方のベクタ
    y.advance(4000);
    CHECK_EQ(y.isr(), 0x82);
    CHECK_EQ(y.read(0), 0x82);
    y.write(3, 0x02);
    CHECK_EQ(y.read(0), 0x8e);
    // 送信 FIFO が空 (bit6) と汎用タイマ (bit7) が同時に立てば、bit6 のベクタ ($8C) が先 (推測。ym3802.h)
    wr(y, 8, 6, 0);
    wr(y, 8, 7, 0); // MIDI クロックタイマを止める
    y.write(3, 0xff);
    wr(y, 0, 6, 0xc0);
    y.advance(4000);
    CHECK_EQ(y.isr(), 0xc0);
    CHECK_EQ(y.read(0), 0x8c);
    wr(y, 0, 6, 0x80); // bit6 の許可を外すと bit6 は消える
    CHECK_EQ(y.read(0), 0x8e);
    y.write(3, 0x80);
    // 0 を書くとタイマは止まる
    wr(y, 8, 4, 0);
    wr(y, 8, 5, 0);
    y.write(3, 0xff);
    y.advance(1000000);
    CHECK_EQ(y.isr() & 0x80, 0);
}

// ---------------------------------------------------------------- 68000 のプログラムから

struct Rom {
    uint8_t b[x68k::Machine::IPL_SIZE];
    uint32_t pos = 0x100;
    Rom() {
        memset(b, 0xff, sizeof b);
        put32(0x10000, 0x00002000);
        put32(0x10004, 0x00fe0100);
        put16(0x0f00, 0x7e01); // バスエラー: moveq #1,d7
        put16(0x0f02, 0x60fe);
        code({0x21fc, 0x00fe, 0x0f00, 0x0008}); // move.l #$fe0f00,$8.w
    }
    void put16(uint32_t o, uint16_t v) {
        b[o] = (uint8_t)(v >> 8);
        b[o + 1] = (uint8_t)v;
    }
    void put32(uint32_t o, uint32_t v) {
        put16(o, (uint16_t)(v >> 16));
        put16(o + 2, (uint16_t)v);
    }
    void code(std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) {
            put16(pos, w);
            pos += 2;
        }
    }
    void codeAt(uint32_t o, std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) {
            put16(o, w);
            o += 2;
        }
    }
    // move.b #v,$eafa00+off
    void movebMidi(uint8_t v, uint16_t off) { code({0x13fc, v, 0x00ea, (uint16_t)(0xfa00 + off)}); }
};

// 受け取ったバイトと、受け取ったときの CPU のサイクル数を覚える音源
class Recorder : public x68k::MidiOut {
public:
    x68k::Machine* m = nullptr;
    std::vector<uint8_t> bytes;
    std::vector<uint64_t> when;
    int resets = 0;
    void midiByte(uint8_t b) override {
        bytes.push_back(b);
        when.push_back(m ? m->cycles() : 0);
    }
    void midiReset() override { resets++; }
    Family family = GENERAL; // 送り直しのやり方を決める音源の種類 (MT32 なら MT-32 向け)
    Family midiFamily() const override { return family; }
};

void start(x68k::Machine& m, Rom& rom) {
    rom.code({0x7c01, 0x60fe}); // moveq #1,d6; bra.s *
    m.setIplRom(rom.b, sizeof rom.b);
    m.powerOn();
}

const uint8_t kSong[] = {0x90, 0x3c, 0x64, 0x40, 0x64, 0x80, 0x3c, 0x40, 0xc1, 0x05};

void testFrom68000() {
    printf("midi board: driven by 68000 code (polling, general timer)\n");
    // ボードがなければ $EAFA01 はバスエラー
    {
        x68k::Machine m0;
        Rom r0;
        r0.code({0x1039, 0x00ea, 0xfa01}); // move.b $eafa01,d0
        start(m0, r0);
        m0.run(20000);
        m68k::Registers r;
        m0.getRegisters(r);
        CHECK_EQ(r.d[7], 1);
    }
    // ボードがあっても、$EAFA10 から先はバスエラー。偶数番地は $FF
    {
        x68k::Machine m0;
        m0.setMidiBoard(true);
        Rom r0;
        r0.code({0x1039, 0x00ea, 0xfa00}); // move.b $eafa00,d0
        r0.code({0x1239, 0x00ea, 0xfa11}); // move.b $eafa11,d1
        start(m0, r0);
        m0.run(20000);
        m68k::Registers r;
        m0.getRegisters(r);
        CHECK_EQ(r.d[0] & 0xff, 0xff);
        CHECK_EQ(r.d[7], 1);
    }

    x68k::Machine m;
    CHECK_EQ(m.setMidiBoard(true), true);
    Recorder rec;
    rec.m = &m;
    m.setMidiOut(&rec);
    Rom rom;
    // 汎用タイマの割り込み ($8E) で d5 を数える
    rom.code({0x21fc, 0x00fe, 0x2000, 0x0238}); // move.l #$fe2000,$238.w
    rom.codeAt(0x2000, {
                           0x5285,                         // addq.l #1,d5
                           0x13fc, 0x0080, 0x00ea, 0xfa07, // move.b #$80,$eafa07 (ICR)
                           0x4e73,                         // rte
                       });
    rom.movebMidi(0x80, 0x03); // リセット
    rom.movebMidi(0x00, 0x03); // 群 0
    rom.movebMidi(0x80, 0x09); // IOR = $80
    rom.movebMidi(0x80, 0x0d); // IER = $80 (汎用タイマ)
    rom.movebMidi(0x04, 0x03); // 群 4
    rom.movebMidi(0x08, 0x09); // TRR = $08 (31.25kbps)
    rom.movebMidi(0x00, 0x0b); // TMR = 0
    rom.movebMidi(0x08, 0x03); // 群 8
    rom.movebMidi(125, 0x09);  // GTR = 125 (1ms)
    rom.movebMidi(0x00, 0x0b);
    rom.movebMidi(0x05, 0x03); // 群 5
    rom.movebMidi(0x01, 0x0b); // TCR = 1
    for (size_t i = 0; i < sizeof kSong; i += 2)
        rom.put16(0x3000 + (uint32_t)i, (uint16_t)(kSong[i] << 8 | (i + 1 < sizeof kSong ? kSong[i + 1] : 0)));
    rom.code({0x46fc, 0x2000});                           // move.w #$2000,sr
    rom.code({0x41f9, 0x00fe, 0x3000});                   // lea $fe3000,a0
    rom.code({(uint16_t)(0x7200 + sizeof kSong - 1)});    // moveq #n-1,d1
    rom.code({0x0839, 0x0006, 0x00ea, 0xfa09, 0x67f6});   // loop: btst #6,$eafa09 (TSR); beq.s loop
    rom.code({0x13d8, 0x00ea, 0xfa0d, 0x51c9, 0xffee});   // move.b (a0)+,$eafa0d (TDR); dbra d1,loop
    start(m, rom);
    const uint64_t t0 = m.cycles();
    m.run(m.cpuHz() / 50); // 20ms
    m68k::Registers r;
    m.getRegisters(r);
    CHECK_EQ(r.d[7], 0);
    CHECK_EQ(r.d[6], 1);
    CHECK_EQ(rec.bytes.size(), sizeof kSong);
    for (size_t i = 0; i < rec.bytes.size() && i < sizeof kSong; i++) CHECK_EQ(rec.bytes[i], kSong[i]);
    // 1 バイトは 320us。音源に渡すのはラスタごとか読み書きのときなので、1 ラスタ (64us) ほどずれてよい
    const double us = 1e6 / m.cpuHz();
    for (size_t i = 1; i < rec.when.size(); i++) {
        const double d = (double)(rec.when[i] - rec.when[i - 1]) * us;
        if (d < 320 - 70 || d > 320 + 70) {
            printf("  byte %zu came %.0fus after the previous one\n", i, d);
            failures++;
        }
    }
    if (rec.when.size() == sizeof kSong) {
        const double total = (double)(rec.when.back() - t0) * us;
        if (total < 320 * sizeof kSong - 20 || total > 320 * sizeof kSong + 200) {
            printf("  %zu bytes took %.0fus, want about %zu\n", sizeof kSong, total, 320 * sizeof kSong);
            failures++;
        }
    }
    // 汎用タイマ (1ms) は 20ms で 20 回ほど
    if (r.d[5] < 18 || r.d[5] > 21) {
        printf("  general timer interrupts in 20ms: %u, want about 20\n", r.d[5]);
        failures++;
    }
    // リセットで、音源の音も止める
    const int resets = rec.resets;
    m.powerOn();
    CHECK_EQ(rec.resets > resets, true);
}

void testTxInterrupt() {
    printf("midi board: sending from the transmit-empty interrupt\n");
    x68k::Machine m;
    m.setMidiBoard(true);
    Recorder rec;
    m.setMidiOut(&rec);
    Rom rom;
    rom.code({0x21fc, 0x00fe, 0x2100, 0x0230}); // move.l #$fe2100,$230.w (ベクタ $8C)
    // 割り込み: 1 バイト書き、なくなったら IER を 0 にして群 5 に戻す
    rom.codeAt(0x2100, {
                           0x13d9, 0x00ea, 0xfa0d,         // move.b (a1)+,$eafa0d
                           0x5344,                         // subq.w #1,d4
                           0x6618,                         // bne.s rte
                           0x13fc, 0x0000, 0x00ea, 0xfa03, // move.b #0,$eafa03
                           0x13fc, 0x0000, 0x00ea, 0xfa0d, // move.b #0,$eafa0d (IER)
                           0x13fc, 0x0005, 0x00ea, 0xfa03, // move.b #5,$eafa03
                           0x4e73,                         // rte
                       });
    for (size_t i = 0; i < sizeof kSong; i += 2)
        rom.put16(0x3000 + (uint32_t)i, (uint16_t)(kSong[i] << 8 | (i + 1 < sizeof kSong ? kSong[i + 1] : 0)));
    rom.code({0x43f9, 0x00fe, 0x3000});               // lea $fe3000,a1
    rom.code({0x383c, (uint16_t)sizeof kSong});       // move.w #n,d4
    rom.movebMidi(0x80, 0x03);
    rom.movebMidi(0x04, 0x03);
    rom.movebMidi(0x08, 0x09); // TRR
    rom.movebMidi(0x05, 0x03);
    rom.movebMidi(0x01, 0x0b); // TCR
    rom.movebMidi(0x00, 0x03);
    rom.movebMidi(0x80, 0x09); // IOR
    rom.movebMidi(0x40, 0x0d); // IER = $40 (送信 FIFO が空)
    rom.movebMidi(0x05, 0x03); // 群 5 (割り込みは TDR に書く)
    rom.code({0x46fc, 0x2000}); // move.w #$2000,sr
    start(m, rom);
    m.run(m.cpuHz() / 50);
    m68k::Registers r;
    m.getRegisters(r);
    CHECK_EQ(r.d[7], 0);
    CHECK_EQ(r.d[6], 1);
    CHECK_EQ(r.d[4] & 0xffff, 0);
    CHECK_EQ(rec.bytes.size(), sizeof kSong);
    for (size_t i = 0; i < rec.bytes.size() && i < sizeof kSong; i++) CHECK_EQ(rec.bytes[i], kSong[i]);
    CHECK_EQ(m.midiBoard()->irq(), false);
}

// ---------------------------------------------------------------- 音の合成を別のコアで行うとき

// 受け取ったバイトで出す値を変える音源 (受け取った位置がずれると、作る音が変わる)
class LevelSynth : public x68k::MidiOut {
public:
    int32_t level = 0;
    int resets = 0;
    void midiByte(uint8_t b) override { level = (int32_t)b * 100 - 6400; }
    void midiReset() override {
        resets++;
        level = 0;
    }
    void mixFrame(int32_t& l, int32_t& r) override {
        l += level;
        r -= level;
    }
};

// 16 ビットを超える量を足す音源 (左は正、右は負)
class LoudSynth : public x68k::MidiOut {
public:
    int32_t level = 0;
    void midiByte(uint8_t) override {}
    void mixFrame(int32_t& l, int32_t& r) override {
        l += level;
        r -= level;
    }
};

// MIDI の音源を足して 16 ビットを超えたら、上限と下限で止める (折り返さない)。大きさの記録 (takeLevels()) は
// 足した量の最大と、止めたフレームの数を返し、取ると 0 に戻る
void testMixSaturates() {
    printf("midi: the mix saturates instead of wrapping, and the levels are recorded\n");
    static int16_t buf[2 * 4096];
    x68k::Sound sound;
    sound.setOutputRate(48000);
    LoudSynth out;
    // 範囲の中: そのまま出て、止めたフレームはない
    out.level = 20000;
    for (int i = 0; i < 100; i++) sound.advance(41, nullptr, &out);
    int n = sound.ring().drain(buf, 4096);
    CHECK_EQ(n > 0, true);
    int bad = 0;
    for (int i = 0; i < n; i++)
        if (buf[2 * i] != 20000 || buf[2 * i + 1] != -20000) bad++;
    CHECK_EQ(bad, 0);
    x68k::Sound::Levels lv = sound.takeLevels();
    CHECK_EQ(lv.base, 0u);
    CHECK_EQ(lv.midi, 20000u);
    CHECK_EQ(lv.clipped, 0u);
    // 16 ビットを少し超える量と、int16_t に切り詰めると符号が変わる量 (40000 は -25536、70000 は 4464 になる)
    for (int32_t level : {32768, 40000, 70000}) {
        out.level = level;
        for (int i = 0; i < 100; i++) sound.advance(41, nullptr, &out);
        n = sound.ring().drain(buf, 4096);
        CHECK_EQ(n > 0, true);
        bad = 0;
        for (int i = 0; i < n; i++)
            if (buf[2 * i] != 32767 || buf[2 * i + 1] != -32768) bad++;
        CHECK_EQ(bad, 0);
        lv = sound.takeLevels();
        CHECK_EQ(lv.midi, (uint32_t)level);
        // 左は 32768 から、右は -32769 から止める (-32768 は範囲の中)
        CHECK_EQ(lv.clipped, (uint32_t)n);
    }
    lv = sound.takeLevels();
    CHECK_EQ(lv.midi, 0u);
    CHECK_EQ(lv.clipped, 0u);
}

// 止めたフレームの数は、音を作るのと取るのが別のスレッドで重なっても、消えも重なりもしない: 取った数の合計が、
// 1 つのスレッドで数えたときと同じになる (-fsanitize=thread でビルドすると、データの競合も調べられる)
void testLevelsFromAnotherThread() {
    printf("midi: clipped frames are counted once when another thread takes the levels\n");
    const int rounds = 200000;
    const auto run = [&](x68k::Sound& sound) {
        LoudSynth out;
        out.level = 40000;
        sound.setOutputRate(48000);
        for (int i = 0; i < rounds; i++) sound.advance(41, nullptr, &out);
    };
    uint32_t expected;
    {
        x68k::Sound sound;
        run(sound);
        expected = sound.takeLevels().clipped;
    }
    CHECK_EQ(expected > 0, true);
    x68k::Sound sound;
    std::atomic<bool> done{false};
    uint64_t total = 0;
    unsigned takes = 0;
    std::thread taker([&] {
        while (!done.load(std::memory_order_acquire)) {
            total += sound.takeLevels().clipped;
            takes++;
        }
    });
    run(sound);
    done.store(true, std::memory_order_release);
    taker.join();
    total += sound.takeLevels().clipped;
    CHECK_EQ(takes > 0, true);
    CHECK_EQ(total, (uint64_t)expected);
}

void testJournal() {
    printf("midi: journal replay gives the same samples\n");
    static uint32_t buf[1 << 18];
    x68k::VideoJournal journal;
    journal.init(buf, 18);
    x68k::Sound single, emu, synth;
    LevelSynth singleOut, replayOut;
    single.setOutputRate(48000);
    synth.setOutputRate(48000);
    x68k::SoundReplayer replayer(journal, synth);
    replayer.setMidiOut(&replayOut);
    emu.setJournal(&journal);
    for (int step = 0; step < 3000; step++) {
        if (step % 37 == 5) {
            const uint8_t b = (uint8_t)(step * 7);
            single.midiWrite(&singleOut, b);
            emu.midiWrite(nullptr, b);
        }
        if (step == 2000) {
            single.midiReset(&singleOut);
            emu.midiReset(nullptr);
        }
        single.advance(41, nullptr, &singleOut);
        emu.advance(41, nullptr, nullptr);
    }
    emu.flushJournal();
    journal.publish();
    while (replayer.step(1 << 20) != 0) {
    }
    CHECK_EQ(singleOut.resets, 1);
    CHECK_EQ(replayOut.resets, 1);
    static int16_t a[2 * 4096], b[2 * 4096];
    int na = single.ring().drain(a, 4096), nb = synth.ring().drain(b, 4096);
    CHECK_EQ(na, nb);
    int diff = 0, nonzero = 0;
    for (int i = 0; i < 2 * na; i++) {
        if (a[i] != b[i]) diff++;
        if (a[i] != 0) nonzero++;
    }
    CHECK_EQ(diff, 0);
    CHECK_EQ(nonzero > na, true);
}

// ステートに MIDI ボードの状態が入る。ボードの有無が違えば読まない
void testState() {
    printf("midi board: state save / load\n");
    Rom rom;
    rom.movebMidi(0x00, 0x03);
    rom.movebMidi(0xa0, 0x09); // IOR = $A0
    static Rom rom2, rom3; // IPL ROM が同じでないと読まない (start() が ROM に書き足すので、別々に写す)
    rom2 = rom;
    rom3 = rom;
    x68k::Machine a;
    a.setMidiBoard(true);
    start(a, rom);
    a.run(20000);
    static uint8_t buf[1 << 24];
    size_t n = 0;
    CHECK_EQ(x68k::saveState(a, buf, sizeof buf, n), true);
    x68k::Machine b;
    b.setMidiBoard(true);
    Recorder rec;
    b.setMidiOut(&rec);
    start(b, rom2);
    const int resets = rec.resets;
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(b, buf, n, err), true);
    CHECK_EQ(b.midiBoard()->vector(), 0xb0);
    CHECK_EQ(rec.resets, resets + 1); // 読んだら音源の音を止める
    x68k::Machine c;
    start(c, rom3);
    CHECK_EQ(x68k::loadState(c, buf, n, err), false);
    CHECK_EQ(strcmp(err, "expansion boards differ"), 0);
}

// ---------------------------------------------------------------- MIDI のバイト列

void testStream() {
    printf("midi stream: running status, realtime, sysex\n");
    platform::MidiStream s;
    // データだけのバイトは捨てる
    CHECK_EQ(s.feed(0x3c), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0x90), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0x3c), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0xf8), platform::MidiStream::NONE); // 途中のリアルタイム
    CHECK_EQ(s.feed(0x64), platform::MidiStream::CHANNEL);
    CHECK_EQ(s.status(), 0x90);
    CHECK_EQ(s.data1(), 0x3c);
    CHECK_EQ(s.data2(), 0x64);
    // ランニングステータス
    CHECK_EQ(s.feed(0x40), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0x00), platform::MidiStream::CHANNEL);
    CHECK_EQ(s.status(), 0x90);
    CHECK_EQ(s.data1(), 0x40);
    CHECK_EQ(s.data2(), 0x00);
    // 1 バイトのメッセージ
    CHECK_EQ(s.feed(0xc9), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0x10), platform::MidiStream::CHANNEL);
    CHECK_EQ(s.data1(), 0x10);
    CHECK_EQ(s.feed(0x11), platform::MidiStream::CHANNEL); // ランニングステータス
    CHECK_EQ(s.data1(), 0x11);
    // システムエクスクルーシブ ($F7 で終わる)
    const uint8_t gs[] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41};
    for (uint8_t b : gs) CHECK_EQ(s.feed(b), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0xf7), platform::MidiStream::SYSEX);
    CHECK_EQ(s.sysexLength(), 9);
    CHECK_EQ(s.sysex()[0], 0x41);
    CHECK_EQ(s.sysex()[8], 0x41);
    // システムエクスクルーシブのあとはランニングステータスがない
    CHECK_EQ(s.feed(0x12), platform::MidiStream::NONE);
    // 次のステータスで終わるシステムエクスクルーシブ
    s.feed(0xf0);
    s.feed(0x7e);
    CHECK_EQ(s.feed(0xb0), platform::MidiStream::SYSEX);
    CHECK_EQ(s.sysexLength(), 1);
    CHECK_EQ(s.feed(0x07), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0x64), platform::MidiStream::CHANNEL);
    CHECK_EQ(s.status(), 0xb0);
    // 長すぎるシステムエクスクルーシブ
    s.feed(0xf0);
    for (int i = 0; i < 100; i++) s.feed(0x01);
    CHECK_EQ(s.feed(0xf7), platform::MidiStream::SYSEX);
    CHECK_EQ(s.sysexTruncated(), true);
    CHECK_EQ(s.sysexLength(), platform::MidiStream::SYSEX_MAX);
    // システムコモン: データを読み飛ばし、ランニングステータスも消える
    s.feed(0x90);
    s.feed(0x3c);
    s.feed(0x64);
    CHECK_EQ(s.feed(0xf2), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0x10), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0x20), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0x3c), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0x64), platform::MidiStream::NONE);
    // reset() は読み飛ばしの途中も消す
    s.feed(0xf2);
    s.reset();
    s.feed(0x90);
    s.feed(0x3c);
    CHECK_EQ(s.feed(0x64), platform::MidiStream::CHANNEL);
    // $F7 なしに次の $F0: 前のものを中身ごと返し、次のバイトから新しいものとして読む
    s.feed(0xf0);
    s.feed(0x7e);
    s.feed(0x7f);
    CHECK_EQ(s.feed(0xf0), platform::MidiStream::SYSEX);
    CHECK_EQ(s.sysexLength(), 2);
    CHECK_EQ(s.sysex()[1], 0x7f);
    CHECK_EQ(s.feed(0x41), platform::MidiStream::NONE);
    CHECK_EQ(s.feed(0xf7), platform::MidiStream::SYSEX);
    CHECK_EQ(s.sysexLength(), 1);
    CHECK_EQ(s.sysex()[0], 0x41);
    // pending(): 新しい MidiStream に渡すと、同じところまで読んだ状態になる
    {
        platform::MidiStream a, b;
        for (int x : {0x90, 0x3c, 0x64, 0x40}) a.feed((uint8_t)x); // ランニングステータスで 1 バイト読んだところ
        uint8_t p[platform::MidiStream::PENDING_MAX];
        const int n = a.pending(p);
        CHECK_EQ(n, 2);
        for (int i = 0; i < n; i++) b.feed(p[i]);
        CHECK_EQ(a.feed(0x7f), platform::MidiStream::CHANNEL);
        CHECK_EQ(b.feed(0x7f), platform::MidiStream::CHANNEL);
        CHECK_EQ(b.data1(), 0x40);
    }
}

// ---------------------------------------------------------------- SoundFont の音源

int peak(const int16_t* p, int frames) {
    int m = 0;
    for (int i = 0; i < 2 * frames; i++) {
        const int v = p[i] < 0 ? -p[i] : p[i];
        if (v > m) m = v;
    }
    return m;
}

void testSf2() {
    printf("sf2 synth: sine SoundFont\n");
    std::vector<uint8_t> sf2 = sf2test::makeSineSf2();
    platform::Sf2Synth s;
    CHECK_EQ(s.load(sf2.data(), sf2.size(), 48000, 16), true);
    CHECK_EQ(s.presetCount(), 2);
    static int16_t out[2 * 48000];
    s.renderFrames(out, 4800);
    CHECK_EQ(peak(out, 4800), 0);
    // ノートオン (A4、ベロシティ 127): 鳴る。440Hz に近い (0 を横切る数)
    for (uint8_t b : {0x90, 0x45, 0x7f}) s.midiByte(b);
    s.renderFrames(out, 48000);
    CHECK_EQ(s.activeVoices(), 1);
    const int p = peak(out + 2 * 4800, 4800);
    if (p < 3000) {
        printf("  peak %d after note on\n", p);
        failures++;
    }
    int crossings = 0;
    for (int i = 1; i < 48000; i++)
        if ((out[2 * i - 2] < 0) != (out[2 * i] < 0)) crossings++;
    if (crossings < 2 * 441 - 4 || crossings > 2 * 441 + 4) {
        printf("  %d zero crossings in 1s, want about 882\n", crossings);
        failures++;
    }
    // ノートオフ (ベロシティ 0 のノートオン、ランニングステータス): しばらくして止まる
    for (uint8_t b : {0x45, 0x00}) s.midiByte(b);
    s.renderFrames(out, 48000);
    CHECK_EQ(peak(out + 2 * 40000, 8000), 0);
    CHECK_EQ(s.activeVoices(), 0);
    // ドラムのチャネル (10) とプログラムチェンジ (ないプログラムはバンク 0 の同じ番号、なければ変えない)
    for (uint8_t b : {0xc9, 0x00, 0x99, 0x24, 0x64, 0xc0, 0x30, 0x90, 0x3c, 0x64}) s.midiByte(b);
    s.renderFrames(out, 4800);
    CHECK_EQ(s.activeVoices(), 2);
    // GS リセットで音が止まる
    for (uint8_t b : {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7}) s.midiByte(b);
    s.renderFrames(out, 4800);
    CHECK_EQ(s.activeVoices(), 0);
    CHECK_EQ(peak(out + 2 * 1000, 3800), 0);
    // midiReset() でも止まる
    for (uint8_t b : {0x90, 0x3c, 0x64}) s.midiByte(b);
    s.renderFrames(out, 480);
    CHECK_EQ(s.activeVoices(), 1);
    s.midiReset();
    s.renderFrames(out, 4800);
    CHECK_EQ(s.activeVoices(), 0);
    // ボイスの数を超えても確保しない (古いものを止めて使う)
    for (int k = 0; k < 40; k++)
        for (uint8_t b : {(uint8_t)0x90, (uint8_t)(30 + k), (uint8_t)0x64}) s.midiByte(b);
    s.renderFrames(out, 480);
    CHECK_EQ(s.activeVoices(), 16);
    // 壊れたファイルは読まない
    platform::Sf2Synth bad;
    sf2[8] = 'x';
    CHECK_EQ(bad.load(sf2.data(), sf2.size(), 48000), false);
    CHECK_EQ(bad.loaded(), false);
    bad.midiByte(0x90); // 読んでいなくても落ちない
    int32_t l = 0, r = 0;
    bad.mixFrame(l, r);
    CHECK_EQ(l, 0);
}

// Machine の Sound で音を作るとき (ホスト版): 68000 から送ったノートオンで SoundFont の音が出る
void testMachineSf2() {
    printf("midi board: 68000 code plays the SoundFont through Machine::sound()\n");
    std::vector<uint8_t> sf2 = sf2test::makeSineSf2();
    platform::Sf2Synth s;
    CHECK_EQ(s.load(sf2.data(), sf2.size(), 48000, 16), true);
    x68k::Machine m;
    m.setMidiBoard(true);
    m.setMidiOut(&s);
    m.sound().setOutputRate(48000);
    Rom rom;
    rom.movebMidi(0x04, 0x03);
    rom.movebMidi(0x08, 0x09);
    rom.movebMidi(0x05, 0x03);
    rom.movebMidi(0x01, 0x0b);
    rom.movebMidi(0x90, 0x0d);
    rom.movebMidi(0x45, 0x0d);
    rom.movebMidi(0x7f, 0x0d);
    start(m, rom);
    m.run(m.cpuHz() / 10); // 100ms
    static int16_t out[2 * 8192];
    const int n = m.sound().ring().drain(out, 8192);
    if (n < 4000) {
        printf("  only %d frames\n", n);
        failures++;
    }
    // 送り終えるまで (3 バイトで 1ms) は無音、その後は鳴る
    CHECK_EQ(peak(out, 40), 0);
    const int p = peak(out + 2 * 2400, n - 2400);
    if (p < 3000) {
        printf("  peak %d\n", p);
        failures++;
    }
}

// 音源を、X68000 を動かし始めてからつなぐ (ベアメタル版は SoundFont を読み終えてからつなぐ)。つなぐまでに
// 送ったバイトは捨て、つないだあとのバイトだけが届く。1 つのコアで作るときと、記録を別のコアで当てるときの両方
void testLateAttach() {
    printf("midi board: the sound module is attached while running\n");
    {
        x68k::Machine m;
        m.setMidiBoard(true);
        Rom rom;
        rom.movebMidi(0x04, 0x03);
        rom.movebMidi(0x08, 0x09); // TRR = $08 (31.25kbps)
        rom.movebMidi(0x05, 0x03);
        rom.movebMidi(0x01, 0x0b); // TCR = 1
        for (size_t i = 0; i < sizeof kSong; i += 2)
            rom.put16(0x3000 + (uint32_t)i, (uint16_t)(kSong[i] << 8 | (i + 1 < sizeof kSong ? kSong[i + 1] : 0)));
        rom.code({0x41f9, 0x00fe, 0x3000});                 // lea $fe3000,a0
        rom.code({(uint16_t)(0x7200 + sizeof kSong - 1)});  // moveq #n-1,d1
        rom.code({0x0839, 0x0006, 0x00ea, 0xfa09, 0x67f6}); // loop: btst #6,$eafa09 (TSR); beq.s loop
        rom.code({0x13d8, 0x00ea, 0xfa0d, 0x51c9, 0xffee}); // move.b (a0)+,$eafa0d (TDR); dbra d1,loop
        start(m, rom);
        m.run(m.cpuHz() / 1000); // 1ms: 最初の数バイトは音源がないので捨てる
        Recorder rec;
        rec.m = &m;
        m.setMidiOut(&rec);
        m.run(m.cpuHz() / 50);
        const size_t n = rec.bytes.size();
        CHECK_EQ(n > 0 && n < sizeof kSong, true);
        // 届いたのは、送ったバイト列の後ろの部分 (途中から)
        for (size_t i = 0; i < n && n <= sizeof kSong; i++) CHECK_EQ(rec.bytes[i], kSong[sizeof kSong - n + i]);
        // つないだあとのリセットは音源に届く
        m.powerOn();
        CHECK_EQ(rec.resets, 1);
    }
    {
        static uint32_t buf[1 << 16];
        x68k::VideoJournal journal;
        journal.init(buf, 16);
        x68k::Sound emu, synth;
        synth.setOutputRate(48000);
        x68k::SoundReplayer replayer(journal, synth);
        emu.setJournal(&journal);
        Recorder rec;
        for (int i = 0; i < 10; i++) {
            emu.midiWrite(nullptr, (uint8_t)i);
            emu.advance(100, nullptr, nullptr);
            if (i == 4) {
                // 5 バイトを当てたあとでつなぐ
                emu.flushJournal();
                journal.publish();
                while (replayer.step(1 << 20) != 0) {
                }
                replayer.setMidiOut(&rec);
            }
        }
        emu.midiReset(nullptr);
        emu.flushJournal();
        journal.publish();
        while (replayer.step(1 << 20) != 0) {
        }
        CHECK_EQ(rec.bytes.size(), 5u);
        for (size_t i = 0; i < rec.bytes.size(); i++) CHECK_EQ(rec.bytes[i], (uint8_t)(5 + i));
        CHECK_EQ(rec.resets, 1);
    }
    {
        // SoundFont の音源に、メッセージの途中 (データのバイト) からつないでも、次のステータスから正しく鳴らす
        std::vector<uint8_t> sf2 = sf2test::makeSineSf2();
        platform::Sf2Synth s;
        CHECK_EQ(s.load(sf2.data(), sf2.size(), 48000, 16), true);
        for (int b : {0x3c, 0x64, 0x40, 0x90, 0x45, 0x7f}) s.midiByte((uint8_t)b);
        CHECK_EQ(s.activeVoices(), 1);
    }
}

// SoundFont を読むときの確保の上限 (Codex の指摘: 小さいファイルから大きく確保する SF2)
void testSf2Limit() {
    printf("sf2 synth: allocation limit while loading\n");
    const std::vector<uint8_t> wide = sf2test::makeWideSf2(1024, 1024);
    CHECK_EQ(wide.size() < 64 * 1024, true);
    const int before = platform::Sf2Synth::liveBlocks();
    platform::Sf2Synth s;
    platform::LoadLimit lim;
    lim.maxBytes = 16u << 20;
    CHECK_EQ(s.load(wide.data(), wide.size(), 48000, 16, lim), false);
    CHECK_EQ(s.overLimit(), true);
    CHECK_EQ(s.loadBytes() <= lim.maxBytes, true);
    CHECK_EQ(s.loaded(), false);
    // 読む途中で確保したものは、すべて解放した
    CHECK_EQ(platform::Sf2Synth::liveBlocks(), before);
    // 小さい SF2 は同じ上限で読める。読んだあとの確保の見積もりも出る
    const std::vector<uint8_t> sine = sf2test::makeSineSf2();
    CHECK_EQ(s.load(sine.data(), sine.size(), 48000, 16, lim), true);
    CHECK_EQ(s.overLimit(), false);
    CHECK_EQ(s.loadBytes() > sine.size(), true);
    // 上限が小さすぎれば読まない (確保の見積もりに blockBytes を使う)
    platform::Sf2Synth t;
    lim.maxBytes = 64u << 10;
    lim.blockBytes = [](size_t n) -> uint64_t { return (uint64_t)n * 4; };
    const int before2 = platform::Sf2Synth::liveBlocks();
    CHECK_EQ(t.load(sine.data(), sine.size(), 48000, 16, lim), false);
    CHECK_EQ(t.overLimit(), true);
    CHECK_EQ(platform::Sf2Synth::liveBlocks(), before2);
}

// 送ったバイトを 68000 のプログラム (TSR を見て TDR に書く) で送る IPL ROM
void sendingRom(Rom& rom, const uint8_t* bytes, size_t n) {
    rom.movebMidi(0x04, 0x03);
    rom.movebMidi(0x08, 0x09); // TRR = $08 (31.25kbps)
    rom.movebMidi(0x05, 0x03);
    rom.movebMidi(0x01, 0x0b); // TCR = 1
    for (size_t i = 0; i < n; i += 2)
        rom.put16(0x3000 + (uint32_t)i, (uint16_t)(bytes[i] << 8 | (i + 1 < n ? bytes[i + 1] : 0)));
    rom.code({0x41f9, 0x00fe, 0x3000});                 // lea $fe3000,a0
    rom.code({(uint16_t)(0x7200 + n - 1)});             // moveq #n-1,d1
    rom.code({0x0839, 0x0006, 0x00ea, 0xfa09, 0x67f6}); // loop: btst #6,$eafa09 (TSR); beq.s loop
    rom.code({0x13d8, 0x00ea, 0xfa0d, 0x51c9, 0xffee}); // move.b (a0)+,$eafa0d (TDR); dbra d1,loop
}

// 2 つの音源に同じバイトを渡す
class Tee : public x68k::MidiOut {
public:
    Tee(x68k::MidiOut& a, x68k::MidiOut& b) : a_(a), b_(b) {}
    void midiByte(uint8_t v) override {
        a_.midiByte(v);
        b_.midiByte(v);
    }
    void midiReset() override {
        a_.midiReset();
        b_.midiReset();
    }

private:
    x68k::MidiOut& a_;
    x68k::MidiOut& b_;
};

// ステートを読むと、音源の設定 (プログラム、コントロールチェンジ、ランニングステータス) を送り直す
// (Codex の指摘: 90 45 7F のあとに保存して読むと、続く 48 7F が無視されていた)
void testStateResync() {
    printf("midi board: state load sends the sound module settings again\n");
    const uint8_t song[] = {0xc0, 0x05, 0xb0, 0x07, 0x40, 0xb0, 0x0a, 0x10, 0x90, 0x45, 0x7f};
    Rom rom;
    sendingRom(rom, song, sizeof song);
    static Rom rom2;
    rom2 = rom;
    const std::vector<uint8_t> sf2 = sf2test::makeSineSf2();
    x68k::Machine a;
    a.setMidiBoard(true);
    start(a, rom);
    a.run(a.cpuHz() / 50); // 送り終える (11 バイトで 3.5ms)
    CHECK_EQ(a.midiBoard()->shadow().program(0), 5);
    CHECK_EQ(a.midiBoard()->shadow().control(0, 7), 0x40);
    static uint8_t buf[1 << 24];
    size_t n = 0;
    CHECK_EQ(x68k::saveState(a, buf, sizeof buf, n), true);

    // 別の音源をつないだ Machine に読む。音源は送り直したバイトを受け取る
    x68k::Machine b;
    b.setMidiBoard(true);
    platform::Sf2Synth s;
    CHECK_EQ(s.load(sf2.data(), sf2.size(), 48000, 16), true);
    Recorder rec;
    Tee tee(rec, s);
    b.setMidiOut(&tee);
    start(b, rom2);
    rec.bytes.clear();
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(b, buf, n, err), true);
    // プログラムチェンジ C0 05、ボリューム B0 07 40、パン B0 0A 10 が入っている
    auto has = [&](std::initializer_list<uint8_t> seq) {
        const std::vector<uint8_t> v(seq);
        for (size_t i = 0; i + v.size() <= rec.bytes.size(); i++)
            if (std::equal(v.begin(), v.end(), rec.bytes.begin() + (long)i)) return true;
        return false;
    };
    CHECK_EQ(has({0xc0, 0x05}), true);
    CHECK_EQ(has({0xb0, 0x07, 0x40}), true);
    CHECK_EQ(has({0xb0, 0x0a, 0x10}), true);
    // 最後はランニングステータス ($90)
    CHECK_EQ(rec.bytes.empty() ? -1 : rec.bytes.back(), 0x90);
    // 続くデータのバイト (ランニングステータスのノートオン) が鳴る
    CHECK_EQ(s.activeVoices(), 0); // 読んだときに鳴っていた音は止める
    s.midiByte(0x48);
    s.midiByte(0x7f);
    CHECK_EQ(s.activeVoices(), 1);
}

// MT-32 の音源 (MidiOut::MT32) では、ステートを読んだときの送り直しに GS のリセット、バンクセレクト、
// 受けていないチャネルのプログラムチェンジを入れない
void testStateResyncMt32() {
    printf("midi board: state load resends MT-32 style to an MT-32 sound module\n");
    const uint8_t song[] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7,
                            0xb1, 0x00, 0x01, 0xc1, 0x05, 0xb1, 0x07, 0x40};
    Rom rom;
    sendingRom(rom, song, sizeof song);
    static Rom rom2;
    rom2 = rom;
    x68k::Machine a;
    a.setMidiBoard(true);
    start(a, rom);
    a.run(a.cpuHz() / 20); // 送り終える (19 バイトで 6ms)
    CHECK_EQ(a.midiBoard()->shadow().program(1), 5);
    static uint8_t buf[1 << 24];
    size_t n = 0;
    CHECK_EQ(x68k::saveState(a, buf, sizeof buf, n), true);
    for (int k = 0; k < 2; k++) {
        x68k::Machine b;
        b.setMidiBoard(true);
        Recorder rec;
        rec.family = k == 0 ? x68k::MidiOut::GENERAL : x68k::MidiOut::MT32;
        b.setMidiOut(&rec);
        static Rom rom3;
        rom3 = rom2; // start() は ROM にコードを書き足すので、毎回写す
        start(b, rom3);
        rec.bytes.clear();
        rec.resets = 0;
        const char* err = nullptr;
        CHECK_EQ(x68k::loadState(b, buf, n, err), true);
        int f0 = 0, pc = 0, bank = 0;
        for (size_t i = 0; i < rec.bytes.size(); i++) {
            const uint8_t x = rec.bytes[i];
            f0 += x == 0xf0;
            pc += (x & 0xf0) == 0xc0;
            if ((x & 0xf0) == 0xb0 && i + 1 < rec.bytes.size() && (rec.bytes[i + 1] == 0 || rec.bytes[i + 1] == 32))
                bank++;
        }
        if (k == 0) {
            CHECK_EQ(f0, 1);
            CHECK_EQ(pc, 16);
            CHECK_EQ(bank > 0, true);
        } else {
            CHECK_EQ(f0, 0);
            CHECK_EQ(pc, 1);
            CHECK_EQ(bank, 0);
        }
        CHECK_EQ(rec.resets, 1);
    }
}

// 動いている途中で音源をつなぐとき (resync)、それまでのプログラムチェンジなどを送り直す
void testAttachResync() {
    printf("midi board: a sound module attached later gets the settings sent before\n");
    const uint8_t song[] = {0xc3, 0x10, 0xb3, 0x07, 0x22};
    Rom rom;
    sendingRom(rom, song, sizeof song);
    x68k::Machine m;
    m.setMidiBoard(true);
    start(m, rom);
    m.run(m.cpuHz() / 50);
    Recorder rec;
    m.setMidiOut(&rec, true);
    m.run(m.cpuHz() / 100);
    auto has = [&](std::initializer_list<uint8_t> seq) {
        const std::vector<uint8_t> v(seq);
        for (size_t i = 0; i + v.size() <= rec.bytes.size(); i++)
            if (std::equal(v.begin(), v.end(), rec.bytes.begin() + (long)i)) return true;
        return false;
    };
    CHECK_EQ(has({0xc3, 0x10}), true);
    CHECK_EQ(has({0xb3, 0x07, 0x22}), true);
    CHECK_EQ(rec.resets, 1);
}

// 音源 a と b の 16 チャネルの設定が同じか (違えば what と違うところを表示する)
bool sameChannels(const platform::Sf2Synth& a, const platform::Sf2Synth& b, const char* what) {
    bool same = true;
    for (int ch = 0; ch < 16; ch++) {
        const platform::Sf2Synth::ChannelInfo x = a.channelInfo(ch), y = b.channelInfo(ch);
        bool eq = x.presetIndex == y.presetIndex && x.bank == y.bank && x.wheel == y.wheel && x.volume == y.volume &&
                  x.pan == y.pan && x.range == y.range && x.tuning == y.tuning && x.rawBank == y.rawBank &&
                  x.sustain == y.sustain && x.rpnMsb == y.rpnMsb && x.rpnLsb == y.rpnLsb && x.nrpn == y.nrpn &&
                  x.midiVolume == y.midiVolume && x.midiExpression == y.midiExpression && x.midiPan == y.midiPan;
        for (int r = 0; r < 3; r++) eq = eq && x.dataMsb[r] == y.dataMsb[r] && x.dataLsb[r] == y.dataLsb[r];
        if (!eq && what) {
            printf("  %s: channel %d differs: preset %d/%d bank %d/%d (raw %d/%d) wheel %d/%d volume %g/%g pan %g/%g "
                   "range %g/%g tuning %g/%g sustain %d/%d rpn %d,%d/%d,%d nrpn %d/%d cc7 %d/%d cc11 %d/%d cc10 %d/%d\n",
                   what, ch, x.presetIndex, y.presetIndex, x.bank, y.bank, x.rawBank, y.rawBank, x.wheel, y.wheel,
                   x.volume, y.volume, x.pan, y.pan, x.range, y.range, x.tuning, y.tuning, x.sustain, y.sustain,
                   x.rpnMsb, x.rpnLsb, y.rpnMsb, y.rpnLsb, x.nrpn, y.nrpn, x.midiVolume, y.midiVolume,
                   x.midiExpression, y.midiExpression, x.midiPan, y.midiPan);
        }
        if (!eq) same = false;
    }
    return same;
}

// 音源 a に bytes を直接送ったときと、同じバイトを覚えた MidiShadow の emit() を新しい音源 b に送ったとき (ステートを
// 読んだときと同じ) で、続けて after を送ったあとのチャネルの設定が同じになるか
bool sameAfterResync(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& after, const char* what) {
    const std::vector<uint8_t> sf2 = sf2test::makeSineSf2();
    platform::Sf2Synth a, b;
    a.load(sf2.data(), sf2.size(), 48000, 16);
    b.load(sf2.data(), sf2.size(), 48000, 16);
    x68k::MidiShadow shadow;
    for (uint8_t v : bytes) {
        a.midiByte(v);
        shadow.feed(v);
    }
    // b は保存したあとに別のことをしていた音源 (送り直しで、それが消えること)
    for (uint8_t v : {0xb0, 0x07, 0x11, 0xe5, 0x00, 0x30, 0xb2, 0x0a, 0x00, 0xc0, 0x00, 0xb0, 0x00, 0x01, 0xb0, 0x20, 0x00})
        b.midiByte(v);
    b.midiReset();
    shadow.emit([&](uint8_t v) { b.midiByte(v); });
    for (uint8_t v : after) {
        a.midiByte(v);
        b.midiByte(v);
    }
    return sameChannels(a, b, what);
}

// MidiShadow の送り直しで、音源 (TinySoundFont) の設定が送ったバイトのとおりに戻る (Codex の再レビューの指摘)
void testShadowResync() {
    printf("midi shadow: resent settings match the sound module\n");
    // CC 121 のあとに、前の音量や RPN のピッチベンドの幅が戻らない
    CHECK_EQ(sameAfterResync({0xb0, 0x07, 0x20, 0xb0, 0x79, 0x00}, {}, "volume then CC 121"), true);
    CHECK_EQ(sameAfterResync({0xb1, 0x65, 0x00, 0xb1, 0x64, 0x00, 0xb1, 0x06, 0x0c, 0xb1, 0x79, 0x00}, {},
                             "pitch bend range then CC 121"),
             true);
    // 選んだ音色のバンクと、次のプログラムチェンジのためのバンクを分ける
    CHECK_EQ(sameAfterResync({0xb0, 0x00, 0x00, 0xc0, 0x00, 0xb0, 0x00, 0x01, 0xb0, 0x20, 0x00}, {}, "pending bank"),
             true);
    CHECK_EQ(sameAfterResync({0xb0, 0x00, 0x00, 0xc0, 0x00, 0xb0, 0x00, 0x01, 0xb0, 0x20, 0x00}, {0xc0, 0x00},
                             "pending bank, then program change"),
             true);
    CHECK_EQ(sameAfterResync({0xb3, 0x00, 0x01, 0xb3, 0x20, 0x00, 0xc3, 0x00, 0xb3, 0x20, 0x05}, {},
                             "bank 128, then LSB"),
             true);
    // ランニングステータスがない状態 ($F2 のあとの読み飛ばし) に戻る: 続くデータのバイトでピッチベンドが変わらない
    // CC 121 のあとの次のためのバンク (Codex と Fable の指摘)
    CHECK_EQ(sameAfterResync({0xb0, 0x00, 0x01, 0xb0, 0x20, 0x00, 0xc0, 0x00, 0xb0, 0x79, 0x00}, {0xc0, 0x00},
                             "bank 128, program change, CC 121, then program change"),
             true);
    CHECK_EQ(sameAfterResync({0xb0, 0x00, 0x05, 0xc0, 0x00, 0xb0, 0x79, 0x00}, {}, "bank 5, program, CC 121"), true);
    CHECK_EQ(sameAfterResync({0xb0, 0x00, 0x05, 0xc0, 0x00, 0xb0, 0x79, 0x00, 0xb0, 0x20, 0x03}, {},
                             "bank 5, program, CC 121, LSB 3"),
             true);
    // RPN ごとのデータ (ピッチベンドの幅の MSB のあとに、ファインチューンの LSB だけ)
    CHECK_EQ(sameAfterResync({0xb1, 0x65, 0x00, 0xb1, 0x64, 0x00, 0xb1, 0x06, 0x0c, 0xb1, 0x64, 0x01, 0xb1, 0x26,
                              0x05},
                             {0xb1, 0x06, 0x00}, "RPN data per RPN"),
             true);
    // リセットのシステムエクスクルーシブのあとに 16 バイトを超えるバイトが続く
    {
        std::vector<uint8_t> v = {0xb2, 0x07, 0x10, 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41};
        for (int i = 0; i < 20; i++) v.push_back(0x11);
        v.push_back(0xf7);
        CHECK_EQ(sameAfterResync(v, {}, "long GS reset"), true);
    }
    // 64 バイトを超えて捨てているリセットのシステムエクスクルーシブの途中
    {
        std::vector<uint8_t> v = {0xb2, 0x07, 0x10, 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41};
        for (int i = 0; i < 70; i++) v.push_back(0x11);
        CHECK_EQ(sameAfterResync(v, {0xf7}, "truncated GS reset"), true);
    }
    CHECK_EQ(sameAfterResync({0xe0, 0x00, 0x50, 0xf2}, {0x00, 0x00}, "song position pointer"), true);
    // ランニングステータスは戻る
    CHECK_EQ(sameAfterResync({0xe2, 0x00, 0x50}, {0x10, 0x30}, "running status of pitch bend"), true);
    CHECK_EQ(sameAfterResync({0xb4, 0x07, 0x40, 0x0a}, {0x10}, "half a control change"), true);
    // GS リセット、ボリューム、パン、ピッチベンド、チューニング
    CHECK_EQ(sameAfterResync({0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7, 0xb5, 0x07, 0x50,
                              0xb5, 0x0a, 0x20, 0xb5, 0x0b, 0x60, 0xe5, 0x10, 0x48, 0xb5, 0x65, 0x00, 0xb5, 0x64,
                              0x01, 0xb5, 0x06, 0x50, 0xb5, 0x26, 0x10},
                             {}, "mixed"),
             true);
}

// 書き換えたステート (配列の外を指す値) を読んでも、範囲に収める (Fable の指摘)
void testSanitize() {
    printf("midi board: state values are kept in range\n");
    x68k::MidiShadow sh;
    memset((void*)&sh, 0xff, sizeof sh);
    sh.sanitize();
    int n = 0;
    sh.emit([&](uint8_t) { n++; });
    CHECK_EQ(n > 0 && n < 16 * 1024, true); // 1 チャネルで 3 x (120 + 20) バイトほどまで
    // MT-32 に送り直すとき、ピッチベンドの幅 (RPN 0 のデータエントリ) は 0-24 に収める (Munt と同じ)。データエントリは、
    // 送り直しの中で RPN 0 を選んでから送ったものだけにする (壊れたステートの cc[] から、選び方の分からない
    // データエントリや RPN の選択を送らない)。いろいろな値で埋めたステートで確かめる (#63)
    {
        unsigned rnd = 12345;
        for (int fill = 0; fill < 64; fill++) {
            x68k::MidiShadow m;
            if (fill == 0) memset((void*)&m, 0xff, sizeof m);
            else if (fill == 1) memset((void*)&m, 0x7f, sizeof m);
            else if (fill == 2) memset((void*)&m, 0xcc, sizeof m); // flags の幅の 5 ビットが 25
            else
                for (size_t i = 0; i < sizeof m; i++) {
                    rnd = rnd * 1103515245u + 12345u;
                    ((uint8_t*)&m)[i] = (uint8_t)(rnd >> 16);
                }
            m.sanitize();
            platform::MidiStream in;
            int selMsb[16], selLsb[16], ranges = 0, maxRange = -1;
            bool ok = true;
            for (int c = 0; c < 16; c++) selMsb[c] = selLsb[c] = -1; // 送り直す前の選択は分からない
            m.emit(
                [&](uint8_t b) {
                    if (in.feed(b) != platform::MidiStream::CHANNEL || (in.status() & 0xf0) != 0xb0) return;
                    const int c = in.status() & 0x0f, k = in.data1(), v = in.data2();
                    if (k == 101) selMsb[c] = v;
                    if (k == 100) selLsb[c] = v;
                    if (k == 38 || (k >= 96 && k <= 97)) ok = false; // MT-32 には送らない
                    if (k == 6) {
                        if (selMsb[c] != 0 || selLsb[c] != 0 || v > 24) ok = false;
                        ranges++;
                        if (v > maxRange) maxRange = v;
                    }
                },
                x68k::MidiOut::MT32);
            if (!ok) printf("  fill %d: bad data entry\n", fill);
            CHECK_EQ(ok, true);
            if (fill == 0 || fill == 2) CHECK_EQ(ranges == 16 && maxRange == 24, true); // 31、25 は 24 に収める
            if (fill == 1) CHECK_EQ(ranges == 16 && maxRange == 15, true);
        }
    }
    platform::MidiStream st;
    memset((void*)&st, 0x7f, sizeof st);
    st.sanitize();
    uint8_t p[platform::MidiStream::PENDING_MAX];
    CHECK_EQ(st.pending(p) <= platform::MidiStream::PENDING_MAX, true);
    x68k::Ym3802 y;
    memset((void*)&y, 0x7f, sizeof y);
    y.sanitize();
    CHECK_EQ(y.txCount() <= x68k::Ym3802::TX_FIFO, true);
    for (int i = 0; i < 100 && y.takeSent() >= 0; i++) {
    }
    y.advance(100000);
}

// SF2 が途中で切れているとき (読む口が頼んだ量を返さない、読み飛ばせない) は読めない (Codex の再レビューの指摘)
class FailingSource : public platform::Sf2Source {
public:
    FailingSource(const std::vector<uint8_t>& d, bool failSkip) : d_(d), failSkip_(failSkip) {}
    int read(void* out, unsigned n) override {
        if (n > d_.size() - pos_) n = (unsigned)(d_.size() - pos_);
        memcpy(out, d_.data() + pos_, n);
        pos_ += n;
        return (int)n;
    }
    bool skip(unsigned n) override {
        if (failSkip_ || n > d_.size() - pos_) return false;
        pos_ += n;
        return true;
    }

private:
    const std::vector<uint8_t>& d_;
    size_t pos_ = 0;
    bool failSkip_;
};

void testSf2Truncated() {
    printf("sf2 synth: truncated file or failed skip\n");
    std::vector<uint8_t> sf2 = sf2test::makeSineSf2();
    const int before = platform::Sf2Synth::liveBlocks();
    platform::Sf2Synth s;
    std::vector<uint8_t> cut(sf2.begin(), sf2.end() - 2);
    CHECK_EQ(s.load(cut.data(), cut.size(), 48000, 16), false);
    CHECK_EQ(platform::Sf2Synth::liveBlocks(), before);
    FailingSource bad(sf2, true);
    CHECK_EQ(s.load(bad, 48000, 16), false);
    CHECK_EQ(platform::Sf2Synth::liveBlocks(), before);
    FailingSource good(sf2, false);
    CHECK_EQ(s.load(good, 48000, 16), true);
    // RIFF の大きさにファイル全体の大きさ (正しい値 + 8) を書いたものは読める (そう書く書き出しソフトがある。Fable の指摘)
    std::vector<uint8_t> big = sf2;
    const uint32_t riff = (uint32_t)big.size(); // 正しい値は big.size() - 8
    for (int i = 0; i < 4; i++) big[4 + i] = (uint8_t)(riff >> (8 * i));
    CHECK_EQ(s.load(big.data(), big.size(), 48000, 16), true);
    FailingSource bigSrc(big, false);
    CHECK_EQ(s.load(bigSrc, 48000, 16), true);
    // RIFF の大きさがほかの量だけ大きいもの、最後の LIST (pdta) と RIFF の大きさを両方大きくしたものは、チャンクが
    // 欠けているので読めない (Codex の指摘)
    auto put32 = [](std::vector<uint8_t>& v, size_t at, uint32_t x) {
        for (int i = 0; i < 4; i++) v[at + i] = (uint8_t)(x >> (8 * i));
    };
    size_t pdta = 0; // 最後の LIST の位置
    for (size_t i = 12; i + 12 <= sf2.size(); i += 8 + (size_t)(sf2[i + 4] | sf2[i + 5] << 8 | sf2[i + 6] << 16 | sf2[i + 7] << 24))
        pdta = i;
    CHECK_EQ(memcmp(&sf2[pdta + 8], "pdta", 4), 0);
    const uint32_t pdtaLen = (uint32_t)(sf2[pdta + 4] | sf2[pdta + 5] << 8 | sf2[pdta + 6] << 16 | sf2[pdta + 7] << 24);
    // + 8 は、RIFF だけなら読める (上) が、pdta も + 8 にしたものは pdta の 8 バイトが欠けているので読めない
    for (uint32_t extra : {4u, 8u, 16u, 4096u}) {
        std::vector<uint8_t> v = sf2;
        put32(v, 4, (uint32_t)sf2.size() - 8 + extra);
        if (extra != 8 && s.load(v.data(), v.size(), 48000, 16)) {
            printf("  loaded with RIFF size + %u\n", extra);
            failures++;
        }
        put32(v, pdta + 4, pdtaLen + extra);
        if (s.load(v.data(), v.size(), 48000, 16)) {
            printf("  loaded with RIFF and pdta size + %u\n", extra);
            failures++;
        }
    }
    // 必要なデータのあとに足したチャンク (RIFF の直下の LIST と、pdta の中のチャンク) を切り落としたものは読めない。
    // 切る位置は、足したチャンクの頭 (必要なデータの終わり) から最後の 1 バイトの手前まですべて (ID、大きさ、LIST の
    // 種類、本体のそれぞれの途中と境目)。読む口が範囲の外を読まないか ASan で見えるよう、切った長さの配列に写して渡す
    {
        auto cutsFail = [&](const std::vector<uint8_t>& v, const char* what) {
            CHECK_EQ(s.load(v.data(), v.size(), 48000, 16), true);
            for (size_t n = sf2.size(); n < v.size(); n++) {
                std::vector<uint8_t> part(v.begin(), v.begin() + (long)n);
                if (s.load(part.data(), part.size(), 48000, 16)) {
                    printf("  %s: loaded when cut at %zu of %zu bytes\n", what, n, v.size());
                    failures++;
                }
            }
        };
        std::vector<uint8_t> v = sf2;
        const uint8_t list[] = {'L', 'I', 'S', 'T', 12, 0, 0, 0, 'x', 't', 'r', 'a', 'j', 'u', 'n', 'k', 0, 0, 0, 0};
        v.insert(v.end(), list, list + sizeof list);
        put32(v, 4, (uint32_t)v.size() - 8);
        cutsFail(v, "extra LIST");
        v = sf2;
        const uint8_t chunk[] = {'j', 'u', 'n', 'k', 4, 0, 0, 0, 1, 2, 3, 4};
        v.insert(v.end(), chunk, chunk + sizeof chunk);
        put32(v, 4, (uint32_t)v.size() - 8);
        put32(v, pdta + 4, pdtaLen + (uint32_t)sizeof chunk);
        cutsFail(v, "extra chunk in pdta");
    }
    // どこで切れても読めない (RIFF の大きさが正しいものと、+ 8 のもの)
    int loaded = 0;
    for (int fix = 0; fix < 2; fix++) {
        const std::vector<uint8_t>& whole = fix ? big : sf2;
        for (size_t n = 0; n < whole.size(); n++) {
            std::vector<uint8_t> part(whole.begin(), whole.begin() + (long)n);
            if (s.load(part.data(), part.size(), 48000, 16)) {
                printf("  %s: loaded when cut at %zu of %zu bytes\n", fix ? "RIFF size + 8" : "RIFF size", n,
                       whole.size());
                loaded++;
            }
        }
    }
    CHECK_EQ(loaded, 0);
    CHECK_EQ(platform::Sf2Synth::liveBlocks(), before); // 最後に読めなかったときに、s の前のものも解放している
}


// RPN でピッチベンドの幅とチューニングを変えたときの音の高さ (差分試験は Sf2Synth どうしを比べるので、期待した
// 高さになるかはここで確かめる)。A4 (440Hz) の正弦波を鳴らし、1 秒で 0 を横切る数 (周波数の 2 倍) を数える
int crossingsAfter(std::initializer_list<uint8_t> setup) {
    const std::vector<uint8_t> sf2 = sf2test::makeSineSf2();
    platform::Sf2Synth s;
    if (!s.load(sf2.data(), sf2.size(), 48000, 16)) return -1;
    for (uint8_t b : setup) s.midiByte(b);
    for (uint8_t b : {0x90, 0x45, 0x7f}) s.midiByte(b);
    static int16_t out[2 * 48000];
    s.renderFrames(out, 48000);
    int n = 0;
    for (int i = 1; i < 48000; i++)
        if ((out[2 * i - 2] < 0) != (out[2 * i] < 0)) n++;
    return n;
}

void testRpnPitch() {
    printf("sf2 synth: pitch bend range and tuning by RPN\n");
    struct Case {
        const char* what;
        std::initializer_list<uint8_t> setup;
        double hz;
    };
    const Case cases[] = {
        {"no RPN", {}, 440.0},
        // ピッチベンドの幅 12 半音、ベンドの最大 (+8191/8192 x 12 半音)
        {"range 12, bend max", {0xb0, 0x65, 0x00, 0xb0, 0x64, 0x00, 0xb0, 0x06, 0x0c, 0xb0, 0x26, 0x00, 0xe0, 0x7f, 0x7f},
         880.0 * 0.99992},
        // 幅を変えなければ 2 半音
        {"range 2, bend min", {0xe0, 0x00, 0x00}, 440.0 / 1.122462},
        // ファインチューン +50 セント (データ 8192 + 4096)
        {"fine tune +50 cents", {0xb0, 0x65, 0x00, 0xb0, 0x64, 0x01, 0xb0, 0x06, 0x60, 0xb0, 0x26, 0x00}, 452.893},
        // コースチューン +12 半音
        {"coarse tune +12", {0xb0, 0x65, 0x00, 0xb0, 0x64, 0x02, 0xb0, 0x06, 0x4c}, 880.0},
        // コースチューン -12 半音とファインチューン -50 セント (順は関係ない)
        {"coarse -12, fine -50 cents",
         {0xb0, 0x65, 0x00, 0xb0, 0x64, 0x01, 0xb0, 0x06, 0x20, 0xb0, 0x26, 0x00, 0xb0, 0x64, 0x02, 0xb0, 0x06, 0x34},
         213.74},
        {"fine -50 cents, coarse -12",
         {0xb0, 0x65, 0x00, 0xb0, 0x64, 0x02, 0xb0, 0x06, 0x34, 0xb0, 0x64, 0x01, 0xb0, 0x06, 0x20, 0xb0, 0x26, 0x00},
         213.74},
        // NRPN を選んだあとのデータエントリは捨てる
        {"data entry after NRPN", {0xb0, 0x65, 0x00, 0xb0, 0x64, 0x02, 0xb0, 0x63, 0x01, 0xb0, 0x06, 0x4c}, 440.0},
        // RPN の番号の片方 (CC100 だけ、CC101 だけ) を送ったあとのデータエントリは捨てる。リセットのあとは
        // 両方 127 (RPN ヌル) なので、送らなかった側は 127 のまま。ピッチベンドの幅は 2 のままで、ベンドの最大は 2 半音上
        {"CC100 only, range 12 ignored", {0xb0, 0x64, 0x00, 0xb0, 0x06, 0x0c, 0xe0, 0x7f, 0x7f}, 440.0 * 1.122446},
        {"CC101 only, range 12 ignored", {0xb0, 0x65, 0x00, 0xb0, 0x06, 0x0c, 0xe0, 0x7f, 0x7f}, 440.0 * 1.122446},
    };
    for (const Case& c : cases) {
        const int n = crossingsAfter(c.setup);
        const int want = (int)(2 * c.hz + 0.5);
        if (n < want - 4 || n > want + 4) {
            printf("  %s: %d zero crossings in 1s, want about %d\n", c.what, n, want);
            failures++;
        }
    }
}

// ---------------------------------------------------------------- 乱数の差分試験

// 決まった種から同じ列を作る乱数 (xorshift)
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9e3779b97f4a7c15ull + 1) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (uint32_t)(s >> 16);
    }
    int below(int n) { return (int)(next() % (uint32_t)n); }
    bool chance(int percent) { return below(100) < percent; }
};

using Msg = std::vector<uint8_t>;

// データのバイト (端の値を多めに)
uint8_t dataByte(Rng& r) {
    static const uint8_t kEdge[] = {0, 1, 2, 63, 64, 127};
    return r.chance(50) ? kEdge[r.below(6)] : (uint8_t)r.below(128);
}

// MIDI のメッセージを 1 つ作る。ランニングステータス (前と同じステータスを省く)、途中で切れたメッセージ、
// メッセージの途中のリアルタイムのバイトも作る。status は前のメッセージのステータス (ランニングステータスのため)
Msg randomMessage(Rng& r, uint8_t& status) {
    static const uint8_t kChannels[] = {0, 1, 9, 15};
    static const uint8_t kControls[] = {0,  32, 0,  32, 6,  38, 6,   38,  96,  97,  98,  99,  100, 101,
                                        100, 101, 121, 121, 7, 39, 10, 42, 11, 43, 64, 1, 120, 123};
    Msg m;
    const int ch = r.chance(80) ? kChannels[r.below(4)] : r.below(16);
    const int kind = r.below(100);
    if (kind < 45) { // コントロールチェンジ
        const uint8_t k = r.chance(90) ? kControls[r.below((int)sizeof kControls)] : (uint8_t)r.below(128);
        uint8_t v = dataByte(r);
        if ((k == 100 || k == 101) && r.chance(60)) v = (uint8_t)(k == 101 ? (r.chance(80) ? 0 : 127) : r.below(4));
        m = {(uint8_t)(0xb0 | ch), k, v};
    } else if (kind < 55) {
        m = {(uint8_t)(0xc0 | ch), (uint8_t)(r.chance(60) ? r.below(3) : dataByte(r))};
    } else if (kind < 62) {
        m = {(uint8_t)(0xe0 | ch), dataByte(r), dataByte(r)};
    } else if (kind < 68) {
        m = {(uint8_t)((r.chance(50) ? 0x90 : 0x80) | ch), dataByte(r), dataByte(r)};
    } else if (kind < 71) {
        m = {(uint8_t)(0xa0 | ch), dataByte(r), dataByte(r)};
    } else if (kind < 73) {
        m = {(uint8_t)(0xd0 | ch), dataByte(r)};
    } else if (kind < 83) { // システムエクスクルーシブ (リセットと、そうでないもの)
        static const Msg kResets[] = {{0x7e, 0x7f, 0x09, 0x01},
                                      {0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41},
                                      {0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00}};
        m = {0xf0};
        if (r.chance(60)) {
            const Msg& reset = kResets[r.below(3)];
            const int keep = r.chance(80) ? (int)reset.size() : r.below((int)reset.size()); // 途中までのもの
            m.insert(m.end(), reset.begin(), reset.begin() + keep);
        }
        static const int kTails[] = {0, 0, 1, 7, 8, 15, 16, 50, 55, 56, 57, 64, 70};
        const int tail = r.chance(70) ? kTails[r.below((int)(sizeof kTails / sizeof kTails[0]))] : r.below(90);
        for (int i = 0; i < tail; i++) m.push_back((uint8_t)r.below(128));
        if (r.chance(75)) m.push_back(0xf7); // なければ次のステータスで終わる
    } else if (kind < 90) { // システムコモン
        static const uint8_t kCommon[] = {0xf1, 0xf2, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7};
        m = {kCommon[r.below(8)]};
        const int n = m[0] == 0xf2 ? 2 : (m[0] == 0xf1 || m[0] == 0xf3) ? 1 : 0;
        // データのバイトが揃わずに切れたもの (受ける側は読み飛ばしの途中 = MidiStream::skip_ が 0 でない) を多めに作る
        const int keep = n > 0 && r.chance(30) ? r.below(n) : n;
        for (int i = 0; i < keep; i++) m.push_back(dataByte(r));
    } else if (kind < 95) { // 宙に浮いたデータのバイト
        m = {dataByte(r)};
    } else { // リアルタイム
        m = {(uint8_t)(0xf8 + r.below(8))};
    }
    // ランニングステータス
    if (m[0] >= 0x80 && m[0] < 0xf0 && m[0] == status && m.size() > 1 && r.chance(50)) m.erase(m.begin());
    if (m[0] >= 0x80 && m[0] < 0xf0) status = m[0];
    else if (m[0] >= 0xf0 && m[0] < 0xf8) status = 0;
    // 途中で切れたメッセージ
    if (m.size() > 1 && r.chance(4)) m.resize(1 + r.below((int)m.size() - 1));
    // メッセージの途中のリアルタイムのバイト
    if (m.size() > 1 && r.chance(5)) m.insert(m.begin() + 1 + r.below((int)m.size() - 1), (uint8_t)(0xf8 + r.below(8)));
    return m;
}

// バイト列を受けたあと、システムコモン ($F1 / $F2 / $F3) のデータのバイトをあと何バイト読み飛ばすところか
// (MidiStream::skip_ と同じ値。MidiStream の外から確かめるために、ここで数え直す)
int skipLeft(const Msg& bytes) {
    int skip = 0;
    for (uint8_t b : bytes) {
        if (b >= 0xf8) continue;
        if (b == 0xf7) continue; // MidiStream は $F7 で skip_ を消さない
        if (b & 0x80) skip = b == 0xf2 ? 2 : (b == 0xf1 || b == 0xf3) ? 1 : 0;
        else if (skip > 0) skip--;
    }
    return skip;
}

std::vector<Msg> randomMessages(Rng& r, int n) {
    std::vector<Msg> v;
    uint8_t status = 0;
    for (int i = 0; i < n; i++) v.push_back(randomMessage(r, status));
    return v;
}

Msg flatten(const std::vector<Msg>& msgs) {
    Msg v;
    for (const Msg& m : msgs) v.insert(v.end(), m.begin(), m.end());
    return v;
}

// 音源 a に msgs を直接送り、cut バイトまでを送ったところで覚えた MidiShadow の emit() を、汚れた音源 b に送る。
// 送り直した直後と、残りを両方に送ったあとで、16 チャネルの設定が同じか
struct Diff {
    platform::Sf2Synth* a;
    platform::Sf2Synth* b;
    bool run(const Msg& bytes, size_t cut, const Msg& dirty, bool print) {
        // a は初めの状態に戻す (GM システムオン)。MidiShadow は何も受けていない状態
        for (uint8_t v : {0xf7, 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7}) a->midiByte(v);
        x68k::MidiShadow shadow;
        for (size_t i = 0; i < cut; i++) {
            a->midiByte(bytes[i]);
            shadow.feed(bytes[i]);
        }
        // ステートに保存して読んだときと同じく、そのまま写して sanitize() したものから送り直す
        x68k::MidiShadow saved;
        memcpy((void*)&saved, (const void*)&shadow, sizeof saved);
        saved.sanitize();
        for (uint8_t v : dirty) b->midiByte(v);
        b->midiReset();
        saved.emit([&](uint8_t v) { b->midiByte(v); });
        if (!sameChannels(*a, *b, print ? "right after the resend" : nullptr)) return false;
        for (size_t i = cut; i < bytes.size(); i++) {
            a->midiByte(bytes[i]);
            b->midiByte(bytes[i]);
        }
        return sameChannels(*a, *b, print ? "after the rest" : nullptr);
    }
};

void printBytes(const char* what, const Msg& v, size_t from, size_t to) {
    printf("  %s:", what);
    for (size_t i = from; i < to; i++) printf(" %02x", v[i]);
    printf("\n");
}

// 乱数で作った MIDI のバイト列で、送り直しが直接送ったときと同じ設定になるか (Codex の再レビューで頼まれたもの)。
// 合わない場面は、MidiShadow (送り直す側) と Sf2Synth (音源) の仕様をそろえて直した (midi_shadow.h、sf2_synth.h)。
// - TinySoundFont の仕様で合わなかったもの (Sf2Synth の側で扱いを変えた):
//   RPN のデータを RPN の間で 1 つだけ持つ、チューニングを今の値の整数の部分から計算する (受けた順で結果が変わる)、
//   SF2 にない音色を選ぶと前の音色のまま (前のプログラムチェンジによる)
// - MidiShadow の仕様で合わなかったもの (MidiShadow を直した):
//   CC 121 のあとのバンク、リセットのシステムエクスクルーシブの長さの上限、64 バイトを超えて捨てている途中の
//   システムエクスクルーシブの送り直し
void testRandomResync() {
    printf("midi shadow: random byte streams, resent settings match the sound module\n");
    const std::vector<uint8_t> sine = sf2test::makeSineSf2();
    const std::vector<uint8_t> wide = sf2test::makeWideSf2(300, 1); // バンク 0 と 1 の 128 音色と、バンク 2 の 44 音色
    const std::vector<uint8_t> noZero = sf2test::makeWideSf2(40, 1, 5); // バンク 0 のプログラム 5-44 (0 がない)
    const std::vector<uint8_t>* fonts[] = {&sine, &wide, &noZero};
    platform::Sf2Synth a[3], b[3];
    for (int f = 0; f < 3; f++) {
        const std::vector<uint8_t>& sf2 = *fonts[f];
        CHECK_EQ(a[f].load(sf2.data(), sf2.size(), 48000, 16), true);
        CHECK_EQ(b[f].load(sf2.data(), sf2.size(), 48000, 16), true);
    }
    const int kRuns = 30000;
    int failures = 0;
    for (int run = 0; run < kRuns && failures < 3; run++) {
        Rng r((uint64_t)run);
        const int f = run % 3;
        std::vector<Msg> msgs = randomMessages(r, 5 + r.below(r.chance(80) ? 60 : 400));
        Msg dirty = flatten(randomMessages(r, r.below(40)));
        // b のチャネルをバンク 128 の音色 (試験用の SF2 のドラム) にしておく (前の音色が残ると合わない)
        for (int ch = 0; ch < 16; ch++) {
            if (!r.chance(30)) continue;
            const uint8_t bc = (uint8_t)(0xb0 | ch);
            dirty.insert(dirty.end(), {0xf7, bc, 0x00, 0x01, bc, 0x20, 0x00, (uint8_t)(0xc0 | ch), 0x00});
        }
        Msg bytes = flatten(msgs);
        const size_t cut = (size_t)r.below((int)bytes.size() + 1);
        Diff d = {&a[f], &b[f]};
        if (d.run(bytes, cut, dirty, false)) continue;
        failures++;
        // 合わないままメッセージを 1 つずつ除いて小さくする (切る位置は、除いたメッセージの分だけ前に動かす)
        size_t c = cut;
        for (size_t i = msgs.size(); i-- > 0;) {
            std::vector<Msg> t = msgs;
            size_t at = 0;
            for (size_t j = 0; j < i; j++) at += msgs[j].size();
            const size_t len = msgs[i].size();
            if (c > at && c < at + len) continue; // 切る位置を含むメッセージは残す
            t.erase(t.begin() + (long)i);
            const size_t tc = c >= at + len ? c - len : c;
            if (!d.run(flatten(t), tc, dirty, false)) {
                msgs = t;
                c = tc;
            }
        }
        bytes = flatten(msgs);
        printf("  run %d (font %d) differs:\n", run, f);
        printBytes("sent before the save", bytes, 0, c);
        printBytes("sent after the save", bytes, c, bytes.size());
        d.run(bytes, c, dirty, true);
    }
    CHECK_EQ(failures, 0);
}

// MIDI のバイト列を受けて作ったステート (壊れていないステート) では、送り直すバイト (GENERAL と MT32) が sanitize() の
// 前と後で同じになる (#63。Codex の指摘)。sanitize() は、使わなくなった値 (ステータスがないときのデータの数、終わった
// システムエクスクルーシブの「切れた」など) を 0 に戻すことがあるので、中身のバイトは比べない。かわりに、続けて同じ
// バイト列を受けたあとの送り直しも同じになることを見る。バイト列は testRandomResync() と同じ作り方で、
// ランニングステータス、途中で切れたメッセージ、リセットのシステムエクスクルーシブなどを含む。
// 4 回に 1 回は、データのバイトが揃わないシステムコモン ($F1 / $F2 / $F3) で終え、読み飛ばしの途中
// (MidiStream::skip_ が 0 でない) のステートも試す。続くバイト列はデータのバイトで始めることがある。
// あわせて MidiStream だけでも、sanitize() の前と後で、続くバイトごとの feed() の結果と読んだ値が同じかを見る。
// なお、MIDI のバイト列から作ったステートでは、skip_ が 0 でないときは必ずステータスがない (status_ が 0) ので、
// 続くデータのバイトは skip_ によらず捨てられる。sanitize() が skip_ を 0 に戻しても、この試験では違いが出ない
void testSanitizeKeepsReachableState() {
    printf("midi shadow: sanitize() keeps the resent bytes of a state made from MIDI bytes\n");
    auto bytesOf = [](const x68k::MidiShadow& m, x68k::MidiOut::Family family) {
        Msg out;
        m.emit([&](uint8_t b) { out.push_back(b); }, family);
        return out;
    };
    auto same = [&](const x68k::MidiShadow& a, const x68k::MidiShadow& b) {
        return bytesOf(a, x68k::MidiOut::GENERAL) == bytesOf(b, x68k::MidiOut::GENERAL) &&
               bytesOf(a, x68k::MidiOut::MT32) == bytesOf(b, x68k::MidiOut::MT32);
    };
    int bad = 0, skipping = 0;
    for (int run = 0; run < 20000 && bad < 3; run++) {
        Rng r((uint64_t)run + 1000000);
        Msg bytes = flatten(randomMessages(r, 1 + r.below(r.chance(80) ? 60 : 400)));
        Msg after = flatten(randomMessages(r, 1 + r.below(60)));
        if (r.chance(25)) { // システムコモンの途中で終える
            static const Msg kCuts[] = {{0xf1}, {0xf3}, {0xf2}, {0xf2, 0x10}};
            const Msg& cut = kCuts[r.below(4)];
            bytes.insert(bytes.end(), cut.begin(), cut.end());
            if (r.chance(30)) bytes.push_back((uint8_t)(0xf8 + r.below(8))); // リアルタイムは読み飛ばしを変えない
            if (r.chance(60)) { // 続くバイト列をデータのバイト (1 か 2 バイト) で始める
                const int n = 1 + r.below(2);
                for (int i = 0; i < n; i++) after.insert(after.begin(), dataByte(r));
            }
        }
        if (skipLeft(bytes) != 0) skipping++;
        x68k::MidiShadow m;
        for (uint8_t b : bytes) m.feed(b);
        x68k::MidiShadow s = m;
        s.sanitize();
        const bool sameNow = same(m, s);
        for (uint8_t b : after) {
            m.feed(b);
            s.feed(b);
        }
        const bool sameAfter = same(m, s);
        // MidiStream だけで比べる
        platform::MidiStream sm;
        for (uint8_t b : bytes) sm.feed(b);
        platform::MidiStream ss = sm;
        ss.sanitize();
        bool sameStream = true;
        for (uint8_t b : after) {
            const platform::MidiStream::Result ra = sm.feed(b), rb = ss.feed(b);
            if (ra != rb) sameStream = false;
            else if (ra == platform::MidiStream::CHANNEL)
                sameStream &= sm.status() == ss.status() && sm.data1() == ss.data1() && sm.data2() == ss.data2();
            else if (ra == platform::MidiStream::SYSEX)
                sameStream &= sm.sysexLength() == ss.sysexLength() && sm.sysexTruncated() == ss.sysexTruncated() &&
                              memcmp(sm.sysex(), ss.sysex(), (size_t)sm.sysexLength()) == 0;
        }
        if (!sameStream) {
            bad++;
            printf("  run %d: MidiStream reads differently after sanitize()\n", run);
            printBytes("sent before sanitize()", bytes, 0, bytes.size());
            printBytes("sent after sanitize()", after, 0, after.size());
            continue;
        }
        if (sameNow && sameAfter) continue;
        bad++;
        printf("  run %d: resent bytes differ %s\n", run, sameNow ? "after more bytes" : "right after sanitize()");
        printBytes("sent before sanitize()", bytes, 0, bytes.size());
        printBytes("sent after sanitize()", after, 0, after.size());
    }
    CHECK_EQ(bad, 0);
    // 読み飛ばしの途中のステートを十分に試したか (20000 回のうち 4000 回ほど)
    printf("  %d of 20000 states were skipping system common data\n", skipping);
    CHECK_EQ(skipping > 2000, true);
}
} // namespace

int main() {
    testRegisters();
    testInterrupts();
    testFrom68000();
    testTxInterrupt();
    testJournal();
    testMixSaturates();
    testLevelsFromAnotherThread();
    testState();
    testStream();
    testSf2();
    testMachineSf2();
    testLateAttach();
    testSf2Limit();
    testStateResync();
    testStateResyncMt32();
    testAttachResync();
    testShadowResync();
    testSanitize();
    testSf2Truncated();
    testRpnPitch();
    testRandomResync();
    testSanitizeKeepsReachableState();
    return finish();
}
