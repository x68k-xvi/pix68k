#include "audio_worker.h"
#include "test_check.h"
#include "../bench/scene.h"
#include "x68k/state.h"
#include <cstring>
#include <memory>
#include <unistd.h>

namespace {
void compare(x68k::Sound& a, x68k::Sound& b) {
    int16_t left[2048], right[2048];
    for (;;) {
        const int n = a.ring().drain(left, 1024), m = b.ring().drain(right, 1024);
        CHECK_EQ(n, m);
        if (n != m) return;
        CHECK_EQ(std::memcmp(left, right, n * 4), 0);
        if (!n) break;
    }
    CHECK_EQ(a.outputPosition(), b.outputPosition());
}
}
int main() {
    x68k::Config config; config.ramSize = 2u << 20; config.cpuHz = 10000000;
    auto a = std::make_unique<x68k::Machine>(config);
    auto b = std::make_unique<x68k::Machine>(config);
    CHECK_EQ(a->ok() && b->ok(), true);
    a->sound().setOutputRate(48000); b->sound().setOutputRate(48000);
    scene::setupSound(*a); scene::setupSound(*b);
    a->sound().advance(10000); b->sound().advance(10000);
    // Starting with both a nonzero resampling phase and queued audio must
    // preserve the exact next sample and the already generated prefix.
    std::string error;
    auto worker = pix68k_retro::AudioWorker::create(*b, error);
    CHECK_EQ(worker != nullptr, true);
    if (!worker) return 1;
    CHECK_EQ(b->sound().outputRate(), 0);
    compare(a->sound(), worker->sound());
    for (unsigned i = 0; i < 1000; i++) {
        const uint32_t clocks = 1 + (i * 997) % 4000;
        if (i % 37 == 0) {
            scene::opm(*a, 0x30, (uint8_t)i); scene::opm(*b, 0x30, (uint8_t)i);
            a->sound().setAdpcmLines(i & 1, i & 3, i % 3);
            b->sound().setAdpcmLines(i & 1, i & 3, i % 3);
            a->sound().adpcmWrite(3, (uint8_t)i); b->sound().adpcmWrite(3, (uint8_t)i);
        }
        a->sound().advance(clocks); b->sound().advance(clocks);
        worker->flush();
        compare(a->sound(), worker->sound());
        CHECK_EQ(a->sound().opm().read(1), b->sound().opm().read(1));
    }
    worker->snapshotToMachine();
    a->sound().advance(4321); b->sound().advance(4321); worker->flush();
    compare(a->sound(), worker->sound());
    auto saved = std::make_unique<x68k::Sound>();
    x68k::copySoundState(a->sound(), *saved);
    const uint32_t phase = a->sound().outputPosition();
    a->sound().advance(4000); b->sound().advance(4000); worker->flush();
    compare(a->sound(), worker->sound());
    worker->pause();
    x68k::copySoundState(*saved, a->sound());
    x68k::copySoundState(*saved, b->sound());
    CHECK_EQ(a->sound().restoreOutputPosition(phase), true);
    CHECK_EQ(worker->sound().restoreOutputPosition(phase), true);
    worker->resume();
    a->sound().advance(12345); b->sound().advance(12345); worker->flush();
    compare(a->sound(), worker->sound());
    // A queued suffix must return to the Machine when stopping the worker.
    a->sound().advance(4567); b->sound().advance(4567); worker->flush();
    usleep(2000); worker.reset();
    CHECK_EQ(b->sound().outputRate(), 48000);
    compare(a->sound(), b->sound());
    for (unsigned pass = 0; pass < 2; pass++) {
        worker = pix68k_retro::AudioWorker::create(*b, error);
        CHECK_EQ(worker != nullptr, true);
        if (!worker) return 1;
        for (unsigned i = 0; i < 50000; i++) {
            if ((i & 255) == 0) usleep(2000);
            b->sound().opmWrite(1, 0x30);
            b->sound().opmWrite(3, (uint8_t)i);
            worker->flush();
        }
        usleep(2000); worker.reset();
    }
    // Unsupported adaptive output must fail without changing the live Sound.
    b->sound().setLatency(100);
    worker = pix68k_retro::AudioWorker::create(*b, error);
    CHECK_EQ(worker == nullptr, true);
    CHECK_EQ(b->sound().outputRate(), 48000);
    b->sound().setLatency(0);
    return finish();
}
