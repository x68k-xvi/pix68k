// src/platform/input_map.h のテスト: USB マウスのレポートの溜め方 (送り先が満杯のとき、短いクリック)、
// USB ゲームパッドの状態からジョイスティックのビットへの対応 (配置が分かるもの、ハット、アナログ軸の遊び)、
// ゲームパッドの Start + Select (リセット) の見つけ方、ゲームパッドでの設定メニューの操作
#include <limits.h>

#include "platform/input_map.h"
#include "test_check.h"

namespace {

using platform::InputSink;

struct Sent {
    int dx, dy;
    uint8_t b;
};

struct Sink {
    Sent log[32];
    int n = 0;
    bool full = false;
    bool operator()(int dx, int dy, uint8_t b) {
        if (full || n >= 32) return false;
        log[n++] = {dx, dy, b};
        return true;
    }
};

void testMouseTracker() {
    printf("mouse tracker\n");
    platform::MouseTracker t(100);
    Sink s;
    auto emit = [&s](int dx, int dy, uint8_t b) { return s(dx, dy, b); };
    CHECK_EQ(t.report(3, -2, 0, emit), 1);
    CHECK_EQ(s.n, 1);
    CHECK_EQ(s.log[0].dx, 3);
    CHECK_EQ(s.log[0].dy, (uint64_t)(int64_t)-2);
    // 満杯の間は溜める
    s.full = true;
    t.report(5, 0, 0, emit);
    t.report(5, 1, 0, emit);
    CHECK_EQ(t.pending(), 1);
    s.full = false;
    CHECK_EQ(t.flush(emit), 1);
    CHECK_EQ(s.n, 2);
    CHECK_EQ(s.log[1].dx, 10);
    CHECK_EQ(s.log[1].dy, 1);
    // 1 回に送るのは ±limit まで
    t.report(250, 0, 0, emit);
    CHECK_EQ(s.n, 5);
    CHECK_EQ(s.log[2].dx, 100);
    CHECK_EQ(s.log[3].dx, 100);
    CHECK_EQ(s.log[4].dx, 50);
    // 満杯の間に押して離したクリックは、押した → 離したの 2 回で送る
    s.n = 0;
    s.full = true;
    t.report(0, 0, InputSink::MOUSE_LEFT, emit);
    t.report(0, 0, 0, emit);
    s.full = false;
    t.flush(emit);
    CHECK_EQ(s.n, 2);
    CHECK_EQ(s.log[0].b, InputSink::MOUSE_LEFT);
    CHECK_EQ(s.log[1].b, 0);
    CHECK_EQ(t.pending(), 0);
    // 押したままなら 1 回
    s.n = 0;
    t.report(0, 0, InputSink::MOUSE_RIGHT, emit);
    CHECK_EQ(s.n, 1);
    CHECK_EQ(s.log[0].b, InputSink::MOUSE_RIGHT);
    t.releaseAll(emit);
    CHECK_EQ(s.n, 2);
    CHECK_EQ(s.log[1].b, 0);
    // 溜めた移動量に int の範囲を超える値を足しても、あふれずに同じ向きのまま
    s.n = 0;
    s.full = true;
    t.report(1000, -1000, 0, emit);
    t.report(INT_MAX, INT_MIN, 0, emit);
    s.full = false;
    CHECK_EQ(t.flush(emit), 0); // 1 回に 64 個までしか送らないので残る
    CHECK_EQ(s.n, 32);
    CHECK_EQ(s.log[0].dx, 100);
    CHECK_EQ(s.log[0].dy, (uint64_t)(int64_t)-100);
}

// コンフィグレーション記述子から HID 記述子のレポート記述子の長さを読む
void testHidReportDescriptorInfo() {
    printf("hid report descriptor info\n");
    const uint8_t config[] = {
        9, 0x02, 59, 0, 2, 1, 0, 0x80, 50,                // コンフィグレーション
        9, 0x04, 0, 0, 1, 0x03, 0, 0, 0,                  // インタフェース 0 (HID)
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0x3f, 0x00,      // HID: レポート記述子 63 バイト
        7, 0x05, 0x81, 0x03, 8, 0, 10,                    // エンドポイント
        9, 0x04, 1, 0, 1, 0x03, 0, 0, 0,                  // インタフェース 1
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0x89, 0x01,      // HID: 393 バイト
        7, 0x05, 0x82, 0x03, 8, 0, 10,
    };
    uint8_t type = 0;
    unsigned len = 0;
    CHECK_EQ(platform::hidReportDescriptorInfo(config, sizeof config, 0, type, len), 1);
    CHECK_EQ(type, 0x22);
    CHECK_EQ(len, 63u);
    CHECK_EQ(platform::hidReportDescriptorInfo(config, sizeof config, 1, type, len), 1);
    CHECK_EQ(len, 393u);
    CHECK_EQ(platform::hidReportDescriptorInfo(config, sizeof config, 2, type, len), 0);
    // 長さが壊れている
    uint8_t bad[sizeof config];
    for (unsigned i = 0; i < sizeof config; i++) bad[i] = config[i];
    bad[9] = 0;
    CHECK_EQ(platform::hidReportDescriptorInfo(bad, sizeof bad, 0, type, len), 0);
    // HID 記述子に 2 つの記述子が並び、レポート記述子 (0x22) が 2 つ目にある (1 つ目は物理記述子 0x23)
    const uint8_t two[] = {
        9, 0x02, 30, 0, 1, 1, 0, 0x80, 50,
        9, 0x04, 0, 0, 1, 0x03, 0, 0, 0,
        12, 0x21, 0x11, 0x01, 0, 2, 0x23, 0x10, 0x00, 0x22, 0x5a, 0x00, // HID: 物理 16 バイト、レポート 90 バイト
    };
    type = 0;
    len = 0;
    CHECK_EQ(platform::hidReportDescriptorInfo(two, sizeof two, 0, type, len), 1);
    CHECK_EQ(type, 0x22);
    CHECK_EQ(len, 90u);
    // bNumDescriptors が 0、レポート記述子がない、bLength に収まらない
    uint8_t none[sizeof two];
    for (unsigned i = 0; i < sizeof two; i++) none[i] = two[i];
    none[18 + 5] = 0;
    CHECK_EQ(platform::hidReportDescriptorInfo(none, sizeof none, 0, type, len), 0);
    none[18 + 5] = 1; // 物理記述子だけ
    CHECK_EQ(platform::hidReportDescriptorInfo(none, sizeof none, 0, type, len), 0);
    none[18] = 9; // bLength 9 では 2 つ目は読まない (total が 27 なので残りも読まず、0x22 がないので false)
    none[18 + 5] = 2;
    CHECK_EQ(platform::hidReportDescriptorInfo(none, 27, 0, type, len), 0);

    // bCountryCode (HID 記述子の 5 バイト目)
    uint8_t jp[sizeof config];
    for (unsigned i = 0; i < sizeof config; i++) jp[i] = config[i];
    jp[18 + 4] = 15;
    uint8_t country = 0xff;
    CHECK_EQ(platform::hidCountryCode(config, sizeof config, 0, country), 1);
    CHECK_EQ(country, 0);
    CHECK_EQ(platform::hidCountryCode(jp, sizeof jp, 0, country), 1);
    CHECK_EQ(country, 15);
    CHECK_EQ(platform::hidCountryCode(jp, sizeof jp, 1, country), 1);
    CHECK_EQ(country, 0);
    CHECK_EQ(platform::hidCountryCode(jp, sizeof jp, 2, country), 0);
    CHECK_EQ(platform::hidCountryCode(bad, sizeof bad, 0, country), 0);
}

// レポート記述子からハットの論理範囲を読む
void testHidHatRange() {
    printf("hid hat range\n");
    // よくあるゲームパッド: X / Y 軸、ボタン 12 個、ハット (論理 1-8、Null 状態あり)
    const uint8_t desc18[] = {
        0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,                               // Game Pad, Collection
        0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02, // 軸
        0x05, 0x09, 0x19, 0x01, 0x29, 0x0c, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0c, 0x81, 0x02, // ボタン
        0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14,
        0x75, 0x04, 0x95, 0x01, 0x81, 0x42, // ハット
        0xc0,
    };
    int lo = -1, hi = -1;
    CHECK_EQ(platform::hidHatLogicalRange(desc18, sizeof desc18, lo, hi), 1);
    CHECK_EQ(lo, 1);
    CHECK_EQ(hi, 8);
    // 論理 0-7 のハット (ジョイスティックの用途)
    const uint8_t desc07[] = {
        0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
        0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0xc0,
    };
    CHECK_EQ(platform::hidHatLogicalRange(desc07, sizeof desc07, lo, hi), 1);
    CHECK_EQ(lo, 0);
    CHECK_EQ(hi, 7);
    // ボタンの Usage Page の Usage 0x39 (57 番目のボタン) はハットではない
    const uint8_t button39[] = {
        0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x05, 0x09, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
        0x75, 0x04, 0x95, 0x01, 0x81, 0x02, 0xc0,
    };
    CHECK_EQ(platform::hidHatLogicalRange(button39, sizeof button39, lo, hi), 0);
    // 8 方向でない論理範囲 (0-3、角度 0-315 など) は使わない
    const uint8_t hat4[] = {
        0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x03,
        0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0xc0,
    };
    CHECK_EQ(platform::hidHatLogicalRange(hat4, sizeof hat4, lo, hi), 0);
    // 4 バイトの論理範囲で、差を int で計算するとあふれて 7 になるもの (0x7ffffffc..0x80000003) は使わない
    const uint8_t wrap[] = {
        0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, 0x09, 0x39, 0x17, 0xfc, 0xff, 0xff, 0x7f, 0x27, 0x03, 0x00, 0x00, 0x80,
        0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0xc0,
    };
    CHECK_EQ(platform::hidHatLogicalRange(wrap, sizeof wrap, lo, hi), 0);
    // ハットがない、途中で切れている
    CHECK_EQ(platform::hidHatLogicalRange(desc18, 21, lo, hi), 0);
    CHECK_EQ(platform::hidHatLogicalRange(desc07, 11, lo, hi), 0);
}

// ハットの値の直し方
void testHatDecoder() {
    printf("hat decoder\n");
    // 論理最小値が分かっているとき
    platform::HatDecoder h;
    h.reset(1);
    CHECK_EQ(h.decode(0), 8);
    CHECK_EQ(h.decode(1), 0); // 上
    CHECK_EQ(h.decode(8), 7); // 左上
    h.reset(0);
    CHECK_EQ(h.decode(0), 0);
    CHECK_EQ(h.decode(8), 8);
    CHECK_EQ(h.decode(15), 8);
    // 分からないとき: 離してつないだ 0-7 の機種 (最初が 8)
    h.reset(-1);
    CHECK_EQ(h.decode(8), 8);
    CHECK_EQ(h.base(), 0);
    CHECK_EQ(h.decode(0), 0); // 上
    CHECK_EQ(h.decode(3), 3);
    // 離してつないだ 1-8 の機種 (最初が 0)
    h.reset(-1);
    CHECK_EQ(h.decode(0), 8);
    CHECK_EQ(h.base(), 1);
    CHECK_EQ(h.decode(1), 0); // 上
    CHECK_EQ(h.decode(8), 7); // 左上
    CHECK_EQ(h.decode(0), 8);
    // 離すと 15 の 0-7 の機種
    h.reset(-1);
    CHECK_EQ(h.decode(15), 8);
    CHECK_EQ(h.decode(0), 0);
    // 0-7 の機種で上を押したままつないだ (最初が 0): 1-8 と見誤るが、離したときの 15 で 0-7 に直る
    h.reset(-1);
    CHECK_EQ(h.decode(0), 8);
    CHECK_EQ(h.decode(15), 8);
    CHECK_EQ(h.base(), 0);
    CHECK_EQ(h.decode(0), 0);
    // 操作したときに初めてレポートを送る機種 (最初が 1-7): 0 と 8 のどちらかが来るまでは 0-7 として読む
    h.reset(-1);
    CHECK_EQ(h.decode(2), 2);
    CHECK_EQ(h.base(), -1);
    CHECK_EQ(h.decode(8), 8);
    CHECK_EQ(h.base(), 0);
    // joystickBits() に通しても、1-8 の機種で離したときに上にならない
    h.reset(-1);
    platform::GamePadInput in;
    in.nhats = 1;
    in.hat = h.decode(0);
    CHECK_EQ(platform::joystickBits(in), 0);
    in.hat = h.decode(5); // 下
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_DOWN);
}

// キーボードの LED: 送れなかったら間を空けて送り直し、決まった回数でやめる。値が変わればすぐ送る
void testLedSender() {
    printf("led sender\n");
    platform::LedSender l(10);
    int calls = 0;
    bool ok = false;
    auto send = [&](int) {
        calls++;
        return ok;
    };
    CHECK_EQ(l.update(1, 0, send), platform::LedSender::FAILED);
    CHECK_EQ(l.tries(), 1);
    // 10 経つまでは送らない
    CHECK_EQ(l.update(1, 9, send), platform::LedSender::IDLE);
    CHECK_EQ(calls, 1);
    CHECK_EQ(l.update(1, 10, send), platform::LedSender::FAILED);
    CHECK_EQ(calls, 2);
    // 次は 20 空ける
    CHECK_EQ(l.update(1, 29, send), platform::LedSender::IDLE);
    ok = true;
    CHECK_EQ(l.update(1, 30, send), platform::LedSender::SENT);
    CHECK_EQ(calls, 3);
    CHECK_EQ(l.update(1, 1000, send), platform::LedSender::IDLE);
    CHECK_EQ(calls, 3);
    // 送れないキーボード: MAX_TRIES 回でやめる
    ok = false;
    calls = 0;
    uint64_t t = 1000;
    for (int i = 0; i < 100; i++, t += 1000) l.update(2, t, send);
    CHECK_EQ(calls, platform::LedSender::MAX_TRIES);
    // 値が変わったら、すぐにまた送る
    CHECK_EQ(l.update(3, t, send), platform::LedSender::FAILED);
    CHECK_EQ(calls, platform::LedSender::MAX_TRIES + 1);
    CHECK_EQ(l.tries(), 1);
    // 0 を送れた → 1 を送るのをあきらめた → 0 に戻した (送ってある値なので送らない) → 1 に変えた: 1 をまた送る
    platform::LedSender m(10);
    ok = true;
    CHECK_EQ(m.update(0, 0, send), platform::LedSender::SENT);
    ok = false;
    for (int i = 0; i < 100; i++) m.update(1, 1000 * (uint64_t)i, send);
    CHECK_EQ(m.tries(), platform::LedSender::MAX_TRIES);
    CHECK_EQ(m.update(0, 200000, send), platform::LedSender::IDLE);
    ok = true;
    CHECK_EQ(m.update(1, 200001, send), platform::LedSender::SENT);
}

void testGamePad() {
    printf("gamepad mapping\n");
    platform::GamePadInput in;
    CHECK_EQ(platform::joystickBits(in), 0);
    // 配置の分かるゲームパッド: 十字キーと A / B
    in.known = true;
    in.buttons = (1u << 15) | (1u << 9);
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_UP | InputSink::JOY_A);
    in.buttons = (1u << 16) | (1u << 17) | (1u << 8);
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_RIGHT | InputSink::JOY_DOWN | InputSink::JOY_B);
    in.buttons = (1u << 18) | (1u << 10) | (1u << 7); // 左、X (A)、Y (B)
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_LEFT | InputSink::JOY_A | InputSink::JOY_B);
    // 配置が分からない: ボタン 1、2
    in.known = false;
    in.buttons = 3;
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_A | InputSink::JOY_B);
    in.buttons = 0;
    // ハット
    in.nhats = 1;
    in.hat = 3;
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_RIGHT | InputSink::JOY_DOWN);
    in.hat = 6;
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_LEFT);
    in.hat = 8; // 離している
    CHECK_EQ(platform::joystickBits(in), 0);
    in.hat = 15;
    CHECK_EQ(platform::joystickBits(in), 0);
    // アナログ軸 (0..255): 真ん中の近くは遊び
    in.naxes = 2;
    in.value[0] = 128;
    in.value[1] = 128;
    CHECK_EQ(platform::joystickBits(in), 0);
    in.value[0] = 100; // 遊びの中
    CHECK_EQ(platform::joystickBits(in), 0);
    in.value[0] = 20;
    in.value[1] = 250;
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_LEFT | InputSink::JOY_DOWN);
    // 符号付きの軸 (-32768..32767)
    in.minimum[0] = in.minimum[1] = -32768;
    in.maximum[0] = in.maximum[1] = 32767;
    in.value[0] = 30000;
    in.value[1] = -30000;
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_RIGHT | InputSink::JOY_UP);
    in.value[0] = 5000;
    in.value[1] = -5000;
    CHECK_EQ(platform::joystickBits(in), 0);
    // int の端までの軸 (差を int で計算するとあふれる)
    in.minimum[0] = in.minimum[1] = INT_MIN;
    in.maximum[0] = in.maximum[1] = INT_MAX;
    in.value[0] = INT_MAX;
    in.value[1] = INT_MIN;
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_RIGHT | InputSink::JOY_UP);
    in.value[0] = INT_MIN;
    in.value[1] = INT_MAX;
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_LEFT | InputSink::JOY_DOWN);
    in.value[0] = 0;
    in.value[1] = -1;
    CHECK_EQ(platform::joystickBits(in), 0);
}

void testPadResetCombo() {
    printf("gamepad Start + Select\n");
    const unsigned START = 1u << 14, SELECT = 1u << 11, PLUS = 1u << 19, MINUS = 1u << 20, A = 1u << 9;
    platform::PadResetCombo c;
    CHECK_EQ(c.update(true, 0), 0);
    // 片方だけでは何もしない
    CHECK_EQ(c.update(true, START), 0);
    CHECK_EQ(c.update(true, 0), 0);
    CHECK_EQ(c.update(true, SELECT | A), 0);
    // 別々のレポートで押しても、両方そろったときに 1 回
    CHECK_EQ(c.update(true, SELECT | START), 1);
    // 押しっぱなしでは 2 回目はない (ほかのボタンを押しても)
    CHECK_EQ(c.update(true, SELECT | START), 0);
    CHECK_EQ(c.update(true, SELECT | START | A), 0);
    // 片方を離して押し直しても、両方を離すまでは受けない
    CHECK_EQ(c.update(true, SELECT), 0);
    CHECK_EQ(c.update(true, SELECT | START), 0);
    // 両方を離せば、次を受ける
    CHECK_EQ(c.update(true, A), 0);
    CHECK_EQ(c.update(true, SELECT | START), 1);
    CHECK_EQ(c.update(true, 0), 0);
    // Switch Pro コントローラの + と -
    CHECK_EQ(c.update(true, PLUS | MINUS), 1);
    CHECK_EQ(c.update(true, 0), 0);
    // 配置の分からないゲームパッドでは何もしない
    CHECK_EQ(c.update(false, SELECT | START), 0);
    CHECK_EQ(c.update(false, 0xffffffffu), 0);
    // ゲームパッドごとに別に見る: 1 台目の Start と 2 台目の Select では何もしない
    platform::PadResetCombo pads[2];
    CHECK_EQ(pads[0].update(true, START), 0);
    CHECK_EQ(pads[1].update(true, SELECT), 0);
    // 両方を押したままつないだ: 最初のレポートではリセットせず、両方を離して押し直したら受ける
    platform::PadResetCombo d;
    CHECK_EQ(d.update(true, SELECT | START), 0);
    CHECK_EQ(d.update(true, START), 0);
    CHECK_EQ(d.update(true, SELECT | START), 0);
    CHECK_EQ(d.update(true, 0), 0);
    CHECK_EQ(d.update(true, SELECT | START), 1);
    // reset() (つなぎ直した) のあとも同じ
    CHECK_EQ(d.update(true, 0), 0);
    d.reset();
    CHECK_EQ(d.update(true, SELECT | START), 0);
    CHECK_EQ(d.update(true, 0), 0);
    CHECK_EQ(d.update(true, SELECT | START), 1);
}

void testPadMenuInput() {
    printf("gamepad settings menu\n");
    namespace mk = platform::menu_key;
    typedef platform::PadMenuInput P;
    const unsigned HOME = 1u << 0, SELECT = 1u << 11, MINUS = 1u << 20;
    const uint8_t U = InputSink::JOY_UP, D = InputSink::JOY_DOWN, R = InputSink::JOY_RIGHT, A = InputSink::JOY_A,
                  B = InputSink::JOY_B;
    uint8_t out = 0, ev[P::MAX_EVENTS];
    int n = 0;
    P p;
    p.reset();
    // つないだあと最初のレポートで Home を押していれば開く (変わったときだけ送るゲームパッド)
    CHECK_EQ(p.update(true, HOME, 0, false, out, ev, n), P::OPEN);
    CHECK_EQ(p.update(true, 0, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(p.update(true, HOME, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(ev[0], mk::SCROLL_LOCK);
    p.closed();
    CHECK_EQ(p.update(true, 0, U | A, false, out, ev, n), P::PASS);
    CHECK_EQ(out, U | A); // ふだんは X68000 に送る
    CHECK_EQ(p.update(true, HOME, U | A, false, out, ev, n), P::OPEN);
    // 開いている間: 新しく押した方向とボタンをメニューのキーにする (押していた上と A は出さない)
    CHECK_EQ(p.update(true, HOME, U | A, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 0);
    CHECK_EQ(p.update(true, 0, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 0);
    CHECK_EQ(out, 0);
    CHECK_EQ(p.update(true, 0, D, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 1);
    CHECK_EQ(ev[0], mk::DOWN);
    CHECK_EQ(p.holds(mk::DOWN), 1); // 押し続けたら繰り返す
    CHECK_EQ(p.holds(mk::UP), 0);
    CHECK_EQ(p.update(true, 0, D | R | A | B, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 3);
    CHECK_EQ(ev[0], mk::RIGHT);
    CHECK_EQ(ev[1], mk::ENTER);
    CHECK_EQ(ev[2], mk::ESC);
    // Select を押しながら上: 閉じる (Scroll Lock)。上は ↑ として渡さない
    CHECK_EQ(p.update(true, 0, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(p.update(true, SELECT, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 0);
    CHECK_EQ(p.update(true, SELECT, U | A, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 1);
    CHECK_EQ(ev[0], mk::SCROLL_LOCK);
    // 閉じたときに押していた上と A は、離すまで X68000 に送らない
    p.closed();
    CHECK_EQ(p.update(true, SELECT, U | A | B, false, out, ev, n), P::PASS);
    CHECK_EQ(out, B);
    CHECK_EQ(p.update(true, 0, A | B, false, out, ev, n), P::PASS);
    CHECK_EQ(out, B);
    CHECK_EQ(p.update(true, 0, 0, false, out, ev, n), P::PASS);
    CHECK_EQ(p.update(true, 0, U | A, false, out, ev, n), P::PASS);
    CHECK_EQ(out, U | A);
    // 上を押してから Select (Switch Pro の -) を押しても開く
    CHECK_EQ(p.update(true, MINUS, U, false, out, ev, n), P::OPEN);
    CHECK_EQ(p.update(true, MINUS, U, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 0);
    // Home で閉じる
    CHECK_EQ(p.update(true, HOME, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 1);
    CHECK_EQ(ev[0], mk::SCROLL_LOCK);
    p.closed();
    // 押したままの Home では開き直さない
    CHECK_EQ(p.update(true, HOME, 0, false, out, ev, n), P::PASS);
    CHECK_EQ(p.update(true, 0, 0, false, out, ev, n), P::PASS);
    // 配置の分からないゲームパッドでは開かないが、開いているメニューの中では操作できる
    P q;
    q.reset();
    CHECK_EQ(q.update(false, 0, 0, false, out, ev, n), P::PASS);
    CHECK_EQ(q.update(false, HOME | SELECT, U, false, out, ev, n), P::PASS);
    CHECK_EQ(out, U);
    CHECK_EQ(q.update(false, 0, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(q.update(false, 1, A, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 1);
    CHECK_EQ(ev[0], mk::ENTER);
}

void testPadMenuStart() {
    printf("gamepad Select + up with Start\n");
    typedef platform::PadMenuInput P;
    const unsigned SELECT = 1u << 11, START = 1u << 14;
    const uint8_t U = InputSink::JOY_UP;
    uint8_t out = 0, ev[P::MAX_EVENTS];
    int n = 0;
    P p;
    p.reset();
    platform::PadResetCombo c;
    c.reset();
    CHECK_EQ(c.update(true, 0), 0);
    // 上と Start を押したまま Select を足す: リセットだけで、メニューは開かない
    CHECK_EQ(p.update(true, START, U, false, out, ev, n), P::PASS);
    CHECK_EQ(c.update(true, START), 0);
    CHECK_EQ(p.update(true, START | SELECT, U, false, out, ev, n), P::PASS);
    CHECK_EQ(c.update(true, START | SELECT), 1);
    // Start を先に離しても、Select と上を離して押し直すまでは開かない
    CHECK_EQ(p.update(true, SELECT, U, false, out, ev, n), P::PASS);
    CHECK_EQ(p.update(true, SELECT, 0, false, out, ev, n), P::PASS);
    CHECK_EQ(p.update(true, SELECT, U, false, out, ev, n), P::OPEN);
    // メニューの中でも、Start を押しながらの Select + 上では閉じない
    CHECK_EQ(p.update(true, 0, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(p.update(true, START | SELECT, U, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 1);
    CHECK_EQ(ev[0], platform::menu_key::UP);
}

void testPadPairInput() {
    printf("two gamepads in one update\n");
    namespace mk = platform::menu_key;
    typedef platform::PadPairInput PP;
    const unsigned HOME = 1u << 0, SELECT = 1u << 11, START = 1u << 14;
    const uint8_t U = InputSink::JOY_UP, A = InputSink::JOY_A, B = InputSink::JOY_B;
    PP pp;
    PP::Pad in[2];
    in[0].present = in[1].present = true;
    pp.reset(0);
    pp.reset(1);
    PP::Result r = pp.update(in, false);
    CHECK_EQ(r.reset || r.open, 0);
    // 1 台目の Home で開く
    in[0].buttons = HOME;
    r = pp.update(in, false);
    CHECK_EQ(r.open, 1);
    CHECK_EQ(r.reset, 0);
    in[0].buttons = 0;
    r = pp.update(in, true);
    // 同じ更新で、1 台目が閉じ (Home)、2 台目が Start + Select: リセットしない
    in[0].buttons = HOME;
    in[1].buttons = START | SELECT;
    r = pp.update(in, true);
    CHECK_EQ(r.reset, 0);
    CHECK_EQ(r.open, 0);
    CHECK_EQ(r.nEvents, 1);
    CHECK_EQ(r.events[0], mk::SCROLL_LOCK);
    // 閉じたあとも、押したままの Start + Select ではリセットしない (離して押し直せばする)
    r = pp.update(in, false);
    CHECK_EQ(r.reset, 0);
    in[0].buttons = in[1].buttons = 0;
    r = pp.update(in, false);
    in[1].buttons = START | SELECT;
    r = pp.update(in, false);
    CHECK_EQ(r.reset, 1);
    in[1].buttons = 0;
    r = pp.update(in, false);
    // メニューの中で押していた A は、閉じたあと離すまで送らない (キーボードで閉じたときも)
    r = pp.update(in, false);
    in[0].buttons = HOME;
    r = pp.update(in, false);
    CHECK_EQ(r.open, 1);
    in[0].buttons = 0;
    in[0].bits = A;
    in[1].bits = B;
    r = pp.update(in, true);
    CHECK_EQ(r.nEvents, 2);
    CHECK_EQ(r.events[0], mk::ENTER);
    CHECK_EQ(r.events[1], mk::ESC);
    CHECK_EQ(r.joy[0] | r.joy[1], 0);
    r = pp.update(in, false); // メニューが閉じた
    CHECK_EQ(r.joy[0] | r.joy[1], 0);
    in[0].bits = 0;
    in[1].bits = 0;
    r = pp.update(in, false);
    in[0].bits = A;
    r = pp.update(in, false);
    CHECK_EQ(r.joy[0], A);
    in[0].bits = 0;
    r = pp.update(in, false);
    // 同じ更新で、1 台目が Start + Select、2 台目が Home: リセットだけ
    in[0].buttons = START | SELECT;
    in[1].buttons = HOME;
    r = pp.update(in, false);
    CHECK_EQ(r.reset, 1);
    CHECK_EQ(r.open, 0);
    in[0].buttons = in[1].buttons = 0;
    r = pp.update(in, false);
    // 上と Start を押したまま Select を足す: リセットだけ
    in[0].buttons = START;
    in[0].bits = U;
    r = pp.update(in, false);
    in[0].buttons = START | SELECT;
    r = pp.update(in, false);
    CHECK_EQ(r.reset, 1);
    CHECK_EQ(r.open, 0);
    CHECK_EQ(r.joy[0], U);
}

void testPadMenuHold() {
    printf("gamepad menu repeat and Select + up again\n");
    namespace mk = platform::menu_key;
    typedef platform::PadMenuInput P;
    const unsigned SELECT = 1u << 11;
    const uint8_t U = InputSink::JOY_UP, L = InputSink::JOY_LEFT, R = InputSink::JOY_RIGHT;
    uint8_t out = 0, ev[P::MAX_EVENTS];
    int n = 0;
    // メニューを開く前から押していた左は、繰り返さない (もう 1 台のパッドで押したままのとき)
    P p;
    p.reset();
    CHECK_EQ(p.update(true, 0, L, false, out, ev, n), P::PASS);
    CHECK_EQ(p.update(true, 0, L, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 0);
    CHECK_EQ(p.holds(mk::LEFT), 0);
    // メニューの中で押した右と上は繰り返す。離したらやめる
    CHECK_EQ(p.update(true, 0, L | R, true, out, ev, n), P::MENU);
    CHECK_EQ(p.holds(mk::RIGHT), 1);
    CHECK_EQ(p.holds(mk::LEFT), 0);
    CHECK_EQ(p.update(true, 0, U, true, out, ev, n), P::MENU);
    CHECK_EQ(p.holds(mk::UP), 1);
    CHECK_EQ(p.holds(mk::RIGHT), 0);
    // 左を離して押し直せば、繰り返す
    CHECK_EQ(p.update(true, 0, U | L, true, out, ev, n), P::MENU);
    CHECK_EQ(p.holds(mk::LEFT), 1);
    CHECK_EQ(p.holds(mk::UP), 1);
    // メニューの外では繰り返さない
    CHECK_EQ(p.update(true, 0, U | L, false, out, ev, n), P::PASS);
    CHECK_EQ(p.holds(mk::UP) || p.holds(mk::LEFT), 0);
    // メニューの中で押した右を押したまま、キーボードでメニューを閉じて開き直した (パッドの次のレポートより前): 右は繰り返さない
    CHECK_EQ(p.update(true, 0, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(p.update(true, 0, R, true, out, ev, n), P::MENU);
    CHECK_EQ(p.holds(mk::RIGHT), 1);
    p.closed();
    CHECK_EQ(p.holds(mk::RIGHT), 0);
    // X68000 を動かす前 (メニューを開いていることにして呼ぶ) に押した方向も、forgetHeld() で繰り返さない
    CHECK_EQ(p.update(true, 0, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(p.update(true, 0, U, true, out, ev, n), P::MENU);
    p.forgetHeld();
    CHECK_EQ(p.holds(mk::UP), 0);
    // Select を押したまま上を押し直すと、閉じて開き直せる
    P q;
    q.reset();
    CHECK_EQ(q.update(true, SELECT, U, false, out, ev, n), P::OPEN);
    CHECK_EQ(q.update(true, SELECT, 0, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 0);
    CHECK_EQ(q.update(true, SELECT, U, true, out, ev, n), P::MENU);
    CHECK_EQ(n, 1);
    CHECK_EQ(ev[0], mk::SCROLL_LOCK);
    q.closed();
    CHECK_EQ(q.update(true, SELECT, 0, false, out, ev, n), P::PASS);
    CHECK_EQ(q.update(true, SELECT, U, false, out, ev, n), P::OPEN);
}

void testPadLayout() {
    printf("gamepad layout by VID:PID\n");
    CHECK_EQ(platform::findPadLayout(0x0f0d, 0x0000) == nullptr, 1);
    CHECK_EQ(platform::findPadLayout(0x1234, 0x00c1) == nullptr, 1);
    const platform::PadLayout* h = platform::findPadLayout(0x0f0d, 0x00c1); // HORIPAD S
    CHECK_EQ(h != nullptr, 1);
    if (h == nullptr) return;
    // Y、B、A、X、L、R、ZL、ZR、-、+、左スティック、右スティック、Home、Capture
    CHECK_EQ(platform::remapPadButtons(*h, 1u << 0), 1u << 7);  // Y
    CHECK_EQ(platform::remapPadButtons(*h, 1u << 1), 1u << 8);  // B
    CHECK_EQ(platform::remapPadButtons(*h, 1u << 2), 1u << 9);  // A
    CHECK_EQ(platform::remapPadButtons(*h, 1u << 3), 1u << 10); // X
    CHECK_EQ(platform::remapPadButtons(*h, 1u << 8), 1u << 20); // -
    CHECK_EQ(platform::remapPadButtons(*h, 1u << 9), 1u << 19); // +
    CHECK_EQ(platform::remapPadButtons(*h, 1u << 12), 1u << 0); // Home
    CHECK_EQ(platform::remapPadButtons(*h, 1u << 13), 1u << 11); // Capture (Select)
    CHECK_EQ(platform::remapPadButtons(*h, 1u << 14), 0u);      // 表にないボタン
    // 並べ直したあとは、配置の分かるゲームパッドと同じ: A / X はトリガ A、B / Y はトリガ B
    platform::GamePadInput in;
    in.known = true;
    in.buttons = platform::remapPadButtons(*h, (1u << 2) | (1u << 1));
    CHECK_EQ(platform::joystickBits(in), InputSink::JOY_A | InputSink::JOY_B);
    // + と - でリセット、Home でメニュー
    platform::PadResetCombo c;
    CHECK_EQ(c.update(true, 0), 0);
    CHECK_EQ(c.update(true, platform::remapPadButtons(*h, (1u << 8) | (1u << 9))), 1);
    platform::PadMenuInput m;
    uint8_t out = 0, ev[platform::PadMenuInput::MAX_EVENTS];
    int n = 0;
    CHECK_EQ(m.update(true, platform::remapPadButtons(*h, 1u << 12), 0, false, out, ev, n), platform::PadMenuInput::OPEN);
}

} // namespace

int main() {
    testMouseTracker();
    testHidReportDescriptorInfo();
    testHidHatRange();
    testHatDecoder();
    testLedSender();
    testGamePad();
    testPadResetCombo();
    testPadMenuInput();
    testPadMenuStart();
    testPadPairInput();
    testPadMenuHold();
    testPadLayout();
    return finish();
}
