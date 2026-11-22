// src/platform/frame_handoff.h のテスト: 合成するスレッドとコア 0 のスレッドの間で、写す役を受け渡しても、
// 同じフレームを 2 回写すこと、順が入れ替わること、2 つのスレッドが同時に写すことがなく、
// 合成した数 = コア 0 が写した数 + 合成するスレッドが写した数 + 置き換わった数 になること。
// -fsanitize=thread でビルドすると、2 つのスレッドの間のデータの競合も調べられる
#include <stdlib.h>

#include <chrono>
#include <thread>

#include "platform/frame_handoff.h"
#include "platform/snapshot_wait.h"
#include "test_check.h"

namespace {

// 相手のスレッドが flag を 0 以外にするのを待つ。10 秒たっても変わらなければ、呼んだ行を出して失敗で終わる
// (相手のスレッドは止まったままなので、join できない。そのまま終わる)
void spinAt(const unsigned& flag, int line) {
    const auto start = std::chrono::steady_clock::now();
    for (unsigned n = 0; __atomic_load_n(&flag, __ATOMIC_ACQUIRE) == 0; n++) {
        if ((n & 1023) == 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
            printf("  %s:%d: spin: the other thread did not set the flag within 10 s\n", __FILE__, line);
            fflush(stdout);
            _Exit(1);
        }
        std::this_thread::yield();
    }
}
#define spin(flag) spinAt(flag, __LINE__)

// 第2出力向けに、直接表示したフレームも所有権を保って受け取れる。
void testRetainSelfFrames() {
    platform::FrameHandoff h;
    h.retainSelfFrames();
    const unsigned first = h.writer();
    h.publish([](unsigned) {});
    h.begin();
    unsigned calls = 0;
    h.publish([&](unsigned) { calls++; });
    CHECK_EQ(calls, 2u); // 残っていた通常フレームも先に表示
    for (unsigned n = 0; n < 100; n++) {
        const unsigned rendered = h.writer();
        h.publish([&](unsigned) { calls++; });
        CHECK_EQ(h.latest(), (int)rendered);
        CHECK_EQ(h.writer() != rendered, true);
        h.end([] {});
        CHECK_EQ(h.take(), (int)rendered);
        CHECK_EQ(h.writer() != rendered, true);
        CHECK_EQ(h.take(), -1);
        h.begin();
    }
    CHECK_EQ(calls, 102u); // 保持した表示済みフレームを重複表示しない
    h.publish([&](unsigned) { calls++; });
    h.publish([&](unsigned) { calls++; }); // 読み手が遅れても表示済みは drop に数えない
    h.end([] {});
    h.publish([](unsigned) { CHECK_EQ(0, 1); });
    CHECK_EQ(h.dropped(), 0u);
    CHECK_EQ(first != h.writer(), true);
}

// 並行して更新しても、受け取った画像は次の take() まで不変。
void testRetainThreads() {
    platform::FrameHandoff h;
    h.retainSelfFrames();
    unsigned pixels[3] = {}, done = 0;
    std::thread producer([&] {
        for (unsigned n = 1; n <= 100000; n++) {
            pixels[h.writer()] = n;
            h.publish([](unsigned) {});
        }
        __atomic_store_n(&done, 1u, __ATOMIC_RELEASE);
    });
    unsigned last = 0;
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        h.end([] { std::this_thread::yield(); });
        const int i = h.take();
        h.begin();
        if (i >= 0) {
            const unsigned value = pixels[i];
            CHECK_EQ(value > last, true);
            last = value;
            for (unsigned n = 0; n < 20; n++) std::this_thread::yield();
            CHECK_EQ(pixels[i], value);
        }
    }
    producer.join();
    h.end([] {});
    CHECK_EQ(pixels[h.latest()], 100000u);
}

void testNormal() {
    printf("normal handoff\n");
    platform::FrameHandoff h;
    CHECK_EQ(h.take(), -1);
    const unsigned w0 = h.writer();
    const unsigned w1 = h.publish([](unsigned) { CHECK_EQ(0, 1); });
    CHECK_EQ(w1 != w0, true);
    CHECK_EQ(h.take(), (int)w0);
    CHECK_EQ(h.take(), -1);
    h.publish([](unsigned) {});
    h.publish([](unsigned) {}); // コア 0 が取る前に次のフレーム
    CHECK_EQ(h.dropped(), 1u);
    CHECK_EQ(h.published(), 3u);
    CHECK_EQ(h.take() >= 0, true);
}

// 受け渡し中に残っていたフレームは、合成するスレッドが先に写す
void testPendingFrame() {
    printf("pending frame at begin\n");
    platform::FrameHandoff h;
    const unsigned first = h.writer();
    h.publish([](unsigned) {});
    h.begin();
    unsigned shown[4], n = 0;
    const unsigned second = h.writer();
    const unsigned next = h.publish([&](unsigned i) { shown[n++] = i; });
    CHECK_EQ(n, 2u);
    CHECK_EQ(shown[0], first);
    CHECK_EQ(shown[1], second);
    CHECK_EQ(next, second); // 写し終えたバッファにまた描く
    CHECK_EQ(h.selfPresented(), 2u);
    int waits = 0;
    h.end([&] { waits++; });
    CHECK_EQ(waits, 0);
    CHECK_EQ(h.take(), -1); // 残っていたフレームは写したので、コア 0 には渡らない
    h.publish([](unsigned) { CHECK_EQ(0, 1); }); // 戻したあとは、ふだんの受け渡し
    CHECK_EQ(h.take(), (int)second);
    CHECK_EQ(h.dropped(), 0u);
}

// begin() のあと、合成するスレッドが写す前に end() したら、写さずにふだんの受け渡しをする
void testEndBeforePublish() {
    printf("end before publish\n");
    platform::FrameHandoff h;
    h.begin();
    h.end([] {});
    const unsigned w = h.writer();
    h.publish([](unsigned) { CHECK_EQ(0, 1); });
    CHECK_EQ(h.take(), (int)w);
    CHECK_EQ(h.selfPresented(), 0u);
}

// 合成するスレッドが写している途中に end() を呼ぶと、BUSY_HANDBACK にして、写し終えるまで待つ
void testHandbackWhileBusy() {
    printf("end while the composer is presenting\n");
    static platform::FrameHandoff h;
    static unsigned inPresent = 0, go = 0, waited = 0, ended = 0;
    h.begin();
    std::thread composer([] {
        h.publish([](unsigned) {
            __atomic_store_n(&inPresent, 1u, __ATOMIC_RELEASE);
            spin(go);
        });
    });
    spin(inPresent);
    std::thread core0([] {
        h.end([] { __atomic_store_n(&waited, 1u, __ATOMIC_RELEASE); });
        __atomic_store_n(&ended, 1u, __ATOMIC_RELEASE);
    });
    spin(waited); // end() は写し終えるのを待っている
    CHECK_EQ(__atomic_load_n(&ended, __ATOMIC_ACQUIRE), 0u);
    CHECK_EQ(h.presentSeq(), 1u); // 写している途中は奇数
    __atomic_store_n(&go, 1u, __ATOMIC_RELEASE);
    core0.join();
    composer.join();
    CHECK_EQ(ended, 1u);
    CHECK_EQ(h.selfPresented(), 1u);
    CHECK_EQ(h.presentSeq(), 2u); // 写し終えたら偶数
    const unsigned w = h.writer();
    h.publish([](unsigned) { CHECK_EQ(0, 1); }); // CORE0 に戻っている
    CHECK_EQ(h.take(), (int)w);
}

// 写す役を任せている間に今のフレームを取る (ネットワークの画面の写し。#57)。任せている間に合成したフレームは
// take() に渡らない。takeWhileHanded() は役を戻し、次に合成し終えたフレームを写してから任せ直す
void testTakeWhileHanded() {
    printf("take the latest frame while the composer presents\n");
    platform::FrameHandoff h;
    unsigned seq[3] = {0, 0, 0}; // バッファに描いたフレームの番号 (1 から)
    unsigned n = 0;
    auto draw = [&] { seq[h.writer()] = ++n; };
    draw();
    h.publish([](unsigned) { CHECK_EQ(0, 1); });
    const int first = h.take();
    CHECK_EQ(first >= 0 && seq[first] == 1, true);
    h.begin();
    for (int k = 0; k < 5; k++) {
        draw();
        h.publish([](unsigned) {});
    }
    CHECK_EQ(h.selfPresented(), 5u);
    CHECK_EQ(h.take(), -1); // 任せている間の 5 つはコア 0 に渡らず、コア 0 が持つのは 1 番目のまま
    // 待つ間に、合成するコアが次のフレーム (7 番目) を描き終える
    int waits = 0, shown = -1, shows = 0;
    const int i = h.takeWhileHanded(
        [] {},
        [&] {
            if (waits++ == 0) {
                draw();
                h.publish([](unsigned) { CHECK_EQ(0, 1); }); // 役を戻しているので、合成するコアは写さない
            }
            return true;
        },
        [&](unsigned b) {
            shown = (int)b;
            shows++;
        });
    CHECK_EQ(i >= 0 && seq[i] == 7, true);
    CHECK_EQ(shown, i);
    CHECK_EQ(shows, 1);
    CHECK_EQ(h.selfPresented(), 5u);
    // 任せ直している: 次のフレームは合成するコアが写し、受け取ったバッファ (i) には描かない
    draw();
    CHECK_EQ(h.writer() != (unsigned)i, true);
    h.publish([](unsigned) {});
    CHECK_EQ(h.selfPresented(), 6u);
    CHECK_EQ(h.take(), -1);
    // 待つのをやめた (時間切れ) ときは写さずに -1 を返し、任せ直す
    shows = 0;
    CHECK_EQ(h.takeWhileHanded([] {}, [] { return false; }, [&](unsigned) { shows++; }), -1);
    CHECK_EQ(shows, 0);
    draw();
    h.publish([](unsigned) {});
    CHECK_EQ(h.selfPresented(), 7u);
    h.end([] {});
    CHECK_EQ(h.dropped(), 0u);
}

// latest(): 最後に描き終えたフレームの番号。コア 0 が受け取ったか、どちらが写したかにかかわらず返す
// (X68000 を止めているときの画面の写し)
void testLatest() {
    printf("latest frame\n");
    platform::FrameHandoff h;
    CHECK_EQ(h.latest(), -1);
    unsigned seq[3] = {0, 0, 0}, n = 0;
    auto draw = [&] { seq[h.writer()] = ++n; };
    draw();
    h.publish([](unsigned) {});
    CHECK_EQ(seq[h.latest()], 1u); // コア 0 がまだ受け取っていない (受け渡し中)
    CHECK_EQ(h.take(), h.latest());
    CHECK_EQ(seq[h.latest()], 1u); // コア 0 が受け取った
    draw();
    h.publish([](unsigned) {});
    CHECK_EQ(seq[h.latest()], 2u);
    h.begin(); // 受け渡し中の 2 番目と、任せている間の 3 番目は書き手が写す
    draw();
    h.publish([](unsigned) {});
    CHECK_EQ(seq[h.latest()], 3u);
    CHECK_EQ(h.latest(), (int)h.writer()); // 書き手が写したフレームは、次に描くバッファのまま
    h.end([] {});
    CHECK_EQ(h.take(), -1); // コア 0 には渡らないが、latest() で分かる
    CHECK_EQ(seq[h.latest()], 3u);
    // 待たずに任せ直したときも同じ (X68000 を止めていてフレームが来ない)
    h.begin();
    CHECK_EQ(h.takeWhileHanded([] {}, [] { return false; }, [](unsigned) { CHECK_EQ(0, 1); }), -1);
    CHECK_EQ(seq[h.latest()], 3u);
    h.end([] {});
    draw();
    h.publish([](unsigned) {});
    CHECK_EQ(seq[h.latest()], 4u);
}

// 画面の写しでフレームを待つ時間 (platform/snapshot_wait.h)。時計を模して ms で数える (wait() を呼ぶたびに 1ms 進む)。
// 待つ流れは CKernel::WaitSnapshotFrames() と同じ: 区間を始め、takeWhileHanded() で待ち、受け取ったら写す前に区間を
// 閉じ、区切ったらディスクを読み書きして待ち直す。上限は 200ms、区間は 40ms。
// - ioLeft: ディスクの読み書きが残る回数 (この回数だけ区切って読み書きする。1 回に diskMs かかる)。読み書きが
//   終わってから 18ms 後にフレームが来る (frameAt を決めていなければ)
// - presentMs: 役を戻すときに合成するスレッドが写している途中で、写し終えるのを待つ時間 (初めの区間だけ)
// - showMs: 受け取ったフレームを写す時間
struct SimSnapshot {
    platform::FrameHandoff h;
    platform::SnapshotWait w{200, 40};
    uint64_t t = 0, frameAt = ~0ull;
    bool published = false;
    unsigned seq[3] = {0, 0, 0}, n = 0, disks = 0, ioLeft = 0;
    uint64_t diskMs = 300, presentMs = 0, showMs = 0, presentWaited = 0;
    unsigned inPresent = 0, go = 0; // 合成するスレッドが写している / 写し終えてよい
    int run() {
        seq[h.writer()] = ++n; // 前に受け取ったフレーム
        h.publish([](unsigned) {});
        h.take();
        h.begin();
        std::thread composer;
        if (presentMs != 0) { // 合成するスレッドが、任せている間のフレームを写している途中
            composer = std::thread([this] {
                seq[h.writer()] = ++n;
                h.publish([this](unsigned) {
                    __atomic_store_n(&inPresent, 1u, __ATOMIC_RELEASE);
                    spin(go);
                });
            });
            spin(inPresent);
        }
        if (ioLeft == 0 && frameAt == ~0ull) frameAt = presentMs + 18;
        for (;;) {
            w.beginSlice();
            const int i = h.takeWhileHanded(
                [&] { // 写し終えるのを待つ間は上限に数えない
                    if (presentWaited < presentMs) {
                        t++;
                        presentWaited++;
                    } else {
                        __atomic_store_n(&go, 1u, __ATOMIC_RELEASE);
                        std::this_thread::yield();
                    }
                },
                [&] {
                    if (composer.joinable()) composer.join(); // 役を戻した (書き手を、このスレッドが模す)
                    t++;
                    if (t >= frameAt && !published) {
                        seq[h.writer()] = ++n;
                        h.publish([](unsigned) { CHECK_EQ(0, 1); }); // 役を戻しているので書き手は写さない
                        published = true;
                    }
                    return w.keepWaiting(t, ioLeft != 0);
                },
                [&](unsigned) {
                    w.endSlice(t); // 写す前に区間を閉じる (写す時間は数えない)
                    t += showMs;
                });
            w.endSlice(t);
            if (composer.joinable()) composer.join();
            if (i >= 0 || w.expired()) return i;
            // 区切った: ディスクの読み書き (役は任せたまま)
            t += diskMs;
            disks++;
            if (ioLeft != 0 && --ioLeft == 0 && frameAt == ~0ull) frameAt = t + 18;
        }
    }
};

// 模した時計は区間ごとに 1ms ほどずれる (区間を始めた時刻は、初めに wait() を呼んで 1ms 進めたあと) ので、数 ms まで許す
bool near(uint64_t v, uint64_t expect) { return v + 5 >= expect && v <= expect + 5; }

void testSnapshotWait() {
    printf("snapshot wait: disk I/O time is not counted\n");
    {
        SimSnapshot s;
        s.ioLeft = 1; // コア 1 がチャンクを待っている
        const int i = s.run();
        CHECK_EQ(i >= 0 && s.seq[i] == 2, true);
        CHECK_EQ(s.disks, 1u);
        // 待ったのは区切るまでの 40ms と、読み書きのあとの 18ms
        CHECK_EQ(near(s.w.used(), 40 + 18), true);
        CHECK_EQ(near(s.t, 40 + 300 + 18), true);
    }
    {
        // ディスクの読み書きが残っていなければ区切らず、フレームが来なければ 200ms で時間切れ
        SimSnapshot s;
        s.frameAt = ~0ull - 1; // 来ない
        CHECK_EQ(s.run(), -1);
        CHECK_EQ(s.disks, 0u);
        CHECK_EQ(s.w.used(), 200u);
        CHECK_EQ(near(s.t, 200), true);
    }
    {
        // 読み書きのあともフレームが来なければ、待った時間の合計が 200ms で時間切れ (読み書きの 300ms は数えない)
        SimSnapshot s;
        s.ioLeft = 1;
        s.frameAt = ~0ull - 1; // 来ない
        CHECK_EQ(s.run(), -1);
        CHECK_EQ(s.disks, 1u);
        CHECK_EQ(s.w.used(), 200u);
        CHECK_EQ(near(s.t, 300 + 200), true);
    }
    printf("snapshot wait: several disk services in one wait\n");
    {
        // 読み書きが 3 回続けて残る: 40ms ごとに 3 回区切って読み書きし、そのあと 18ms で来るフレームを受け取る
        SimSnapshot s;
        s.ioLeft = 3;
        const int i = s.run();
        CHECK_EQ(i >= 0 && s.seq[i] == 2, true);
        CHECK_EQ(s.disks, 3u);
        CHECK_EQ(near(s.w.used(), 3 * 40 + 18), true);
        CHECK_EQ(near(s.t, 3 * (40 + 300) + 18), true);
    }
    {
        // 読み書きがずっと残り、フレームが来ない: 40ms の区間を 5 つ使い切ったところで時間切れ。読み書きは 4 回
        SimSnapshot s;
        s.ioLeft = 1000;
        CHECK_EQ(s.run(), -1);
        CHECK_EQ(s.disks, 4u);
        CHECK_EQ(s.w.used(), 200u);
        CHECK_EQ(near(s.t, 4 * (40 + 300) + 40), true);
    }
    printf("snapshot wait: waiting for the composer and showing the frame are not counted\n");
    {
        // 合成するスレッドが写し終えるのを 150ms 待ち、受け取ったフレームを写すのに 120ms かかる。
        // フレームを待ったのは 18ms だけなので、上限 (200ms) を超えても受け取って写す
        SimSnapshot s;
        s.presentMs = 150;
        s.showMs = 120;
        const int i = s.run();
        CHECK_EQ(i >= 0 && s.seq[i] == 3, true); // 2 番目は合成するスレッドが写した
        CHECK_EQ(s.h.selfPresented(), 1u);
        CHECK_EQ(s.presentWaited, 150u);
        CHECK_EQ(near(s.w.used(), 18), true);
        CHECK_EQ(near(s.t, 150 + 18 + 120), true);
    }
    {
        // 写し終えるのを待ち、読み書きを 2 回挟んでから受け取る
        SimSnapshot s;
        s.presentMs = 150;
        s.showMs = 120;
        s.ioLeft = 2;
        const int i = s.run();
        CHECK_EQ(i >= 0 && s.seq[i] == 3, true);
        CHECK_EQ(s.disks, 2u);
        CHECK_EQ(near(s.w.used(), 2 * 40 + 18), true);
        CHECK_EQ(near(s.t, 150 + 2 * (40 + 300) + 18 + 120), true);
    }
    {
        // 写し終えるのを待つ時間 (250ms) が上限より長くても、それだけでは時間切れにならない。そのあとフレームが来なければ
        // フレームを待った 200ms で時間切れ
        SimSnapshot s;
        s.presentMs = 250;
        s.frameAt = ~0ull - 1;
        CHECK_EQ(s.run(), -1);
        CHECK_EQ(s.w.used(), 200u);
        CHECK_EQ(near(s.t, 250 + 200), true);
    }
}

void testThreads() {
    printf("two threads\n");
    static platform::FrameHandoff h;
    static unsigned seq[3];        // バッファに描いたフレームの番号 (1 から)
    static unsigned shownSeq[400000];
    static unsigned shownCount = 0; // 写したフレームの数 (どちらのスレッドも、写している間だけ書く)
    static unsigned presenting = 0; // 写している途中のスレッドの数
    static unsigned overlaps = 0, done = 0;
    // コア 0 が写す役を任せている間 (begin() のあとから end() の前まで) 1。takeWhileHanded() は中で役をいったん
    // 戻す (end() してから take() で待ち、begin() で任せ直す) ので、その間は 0 にしておく。1 のままだと、コア 0 の
    // スレッドが takeWhileHanded() の中で止まっている間、合成するスレッドは任されたと思って待たずに渡し続け、
    // 1 回も写さないまま終わることがある (負荷が高いときに handoffs = 1、selfPresented() = 0 で落ちていた)
    static unsigned handed = 0;
    // takeWhileHanded() の中でコア 0 がフレームを待つたび (wait() を呼ぶたび) に 1 にする。合成するスレッドは、
    // 任されるのを待っている間にこれが 1 なら 0 に戻して 1 フレームだけ渡す (handed は 0 なので、渡さないとコア 0 は
    // フレームを、合成するスレッドは任されるのを待ち合ったままになる)。1 回 wait() を呼ぶごとに 1 フレームだけなので、
    // コア 0 のスレッドが止まっている間に渡し続けることはない
    static unsigned frameWanted = 0;
    const unsigned N = 300000;
    auto show = [](unsigned i) {
        if (__atomic_fetch_add(&presenting, 1u, __ATOMIC_ACQ_REL) != 0) __atomic_fetch_add(&overlaps, 1u, __ATOMIC_RELAXED);
        if (shownCount < sizeof shownSeq / sizeof shownSeq[0]) shownSeq[shownCount] = seq[i];
        shownCount++;
        __atomic_fetch_sub(&presenting, 1u, __ATOMIC_ACQ_REL);
    };
    std::thread composer([&] {
        bool waitStarted = false, waitExpired = false;
        std::chrono::steady_clock::time_point waitDeadline;
        for (unsigned n = 1; n <= N; n++) {
            // 負荷が高いと、コア 0 のスレッドがほとんど動かないうちに N フレームを描き終え、任されて写すことが
            // 1 回もないまま終わることがある。後半でまだ 1 回も写していなければ、任されるまで待ってから渡す
            // (任されている間に end() されて写せなければ、次のフレームでまた待つ。コア 0 が takeWhileHanded() の中で
            // フレームを待っていれば、1 フレームだけ渡す (上の frameWanted)。待つ時間は合わせて 10 秒までで、
            // 超えたらそのあとは待たない。そのときは下の handoffs > 0 などの検査で失敗する)
            if (n > N / 2 && h.selfPresented() == 0 && !waitExpired) {
                if (!waitStarted) {
                    waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    waitStarted = true;
                }
                while (!__atomic_load_n(&handed, __ATOMIC_ACQUIRE) &&
                       !__atomic_exchange_n(&frameWanted, 0u, __ATOMIC_ACQ_REL)) {
                    if (std::chrono::steady_clock::now() >= waitDeadline) {
                        waitExpired = true;
                        break;
                    }
                    std::this_thread::yield();
                }
            }
            seq[h.writer()] = n;
            h.publish(show);
        }
        __atomic_store_n(&done, 1u, __ATOMIC_RELEASE);
    });
    unsigned core0Shown = 0, handoffs = 0, snapshots = 0, rnd = 1;
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        rnd = rnd * 1103515245u + 12345u;
        if ((rnd >> 16) % 64 == 0) { // ときどき写す役を任せ、少しして戻す
            h.begin();
            __atomic_store_n(&handed, 1u, __ATOMIC_RELEASE);
            for (unsigned k = 0; k < ((rnd >> 8) & 255); k++) std::this_thread::yield();
            if (handoffs % 2 == 0) { // 1 回おきに、任せている間に今のフレームを取る (画面の写し。負荷が高いと
                                     // 合成するスレッドが先に終わって取れないことがあるので、取れた数は見ない)
                __atomic_store_n(&handed, 0u, __ATOMIC_RELEASE); // 中で役を戻すので、いったん 0 (上の handed)
                const int i = h.takeWhileHanded(
                    [] { std::this_thread::yield(); },
                    [] {
                        __atomic_store_n(&frameWanted, 1u, __ATOMIC_RELEASE);
                        std::this_thread::yield();
                        return !__atomic_load_n(&done, __ATOMIC_ACQUIRE);
                    },
                    show);
                __atomic_store_n(&frameWanted, 0u, __ATOMIC_RELEASE);
                if (i >= 0) {
                    core0Shown++;
                    snapshots++;
                }
                __atomic_store_n(&handed, 1u, __ATOMIC_RELEASE); // takeWhileHanded() は任せ直して返る
            }
            __atomic_store_n(&handed, 0u, __ATOMIC_RELEASE);
            h.end([] { std::this_thread::yield(); });
            handoffs++;
        }
        const int i = h.take();
        if (i >= 0) {
            show((unsigned)i);
            core0Shown++;
        }
    }
    composer.join();
    const int i = h.take();
    if (i >= 0) {
        show((unsigned)i);
        core0Shown++;
    }
    CHECK_EQ(overlaps, 0u);
    CHECK_EQ(h.published(), N);
    // 書き手のスレッドが終わった (join) あとは、最後に描き終えたフレームを latest() で読める
    CHECK_EQ(h.latest() >= 0 && seq[h.latest()] == N, true);
    CHECK_EQ(shownCount, core0Shown + h.selfPresented());
    CHECK_EQ(shownCount + h.dropped(), N);
    int disorder = 0;
    for (unsigned k = 1; k < shownCount && k < sizeof shownSeq / sizeof shownSeq[0]; k++)
        if (shownSeq[k] <= shownSeq[k - 1]) disorder++;
    CHECK_EQ(disorder, 0);
    CHECK_EQ(handoffs > 0, true);
    CHECK_EQ(h.selfPresented() > 0, true);
    printf("  %u frames: %u by core 0, %u by the composer, %u replaced, %u handoffs, %u snapshots\n", N, core0Shown,
           h.selfPresented(), h.dropped(), handoffs, snapshots);
}

} // namespace

int main() {
    // 出力をパイプに流していても (ctest)、途中で止まったときにそこまでの行が残るよう、1 行ごとに書き出す
    setvbuf(stdout, nullptr, _IOLBF, 0);
    testRetainSelfFrames();
    testRetainThreads();
    testNormal();
    testPendingFrame();
    testEndBeforePublish();
    testHandbackWhileBusy();
    testTakeWhileHanded();
    testLatest();
    testSnapshotWait();
    testThreads();
    return finish();
}
