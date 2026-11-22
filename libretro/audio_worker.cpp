#include "audio_worker.h"
#include "x68k/state.h"
#include "x68k/sound_replay.h"
#include <new>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace pix68k_retro {
namespace {
void relax() {
#ifdef __vita__
    asm volatile("yield");
#else
    sched_yield();
#endif
}
void processorRelax() {
#if defined(__arm__) || defined(__aarch64__)
    asm volatile("yield");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

}

struct AudioWorker::Impl {
    x68k::Machine& machine;
    x68k::Sound synth;
    x68k::VideoJournal journal;
    x68k::SoundReplayer reader{journal, synth};
    x68k::SoundLink link;
    std::unique_ptr<uint32_t[]> events{new (std::nothrow) uint32_t[1u << 16]};
    std::unique_ptr<int16_t[]> pending{new (std::nothrow) int16_t[2 * x68k::AudioRing::SIZE]};
    int originalRate;
    bool paused = false;
    unsigned quit = 0, sleeping = 0;
    bool attached = false, running = false;
#ifdef __vita__
    SceUID thread = -1, event = -1;
    int originalAffinity = -1;
#else
    pthread_t thread{};
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    bool signaled = false;
#endif

    explicit Impl(x68k::Machine& m) : machine(m), originalRate(m.sound().outputRate()) {
        if (!events || !pending || originalRate <= 0) return;
        journal.init(events.get(), 16);
        journal.setRelax(relax);
        journal.setWakeContext(wake, this);
        synth.setOutputRate(originalRate);
        x68k::copySoundState(machine.sound(), synth);
        const unsigned count = machine.sound().ring().copyPending(pending.get(), x68k::AudioRing::SIZE);
        for (unsigned i = 0; i < count; i++) synth.ring().push(pending[2 * i], pending[2 * i + 1]);
    }
    void flush() { machine.sound().flushJournal(); journal.flush(); }
    void snapshot() { flush(); x68k::copySoundState(synth, machine.sound()); }
    void pause() {
        if (paused) return;
        snapshot();
        machine.sound().setJournal(nullptr);
        paused = true;
    }
    void resume() {
        if (!paused) return;
        x68k::copySoundState(machine.sound(), synth);
        machine.sound().setJournal(&journal);
        paused = false;
    }
    static void wake(void* context) {
        auto* self = static_cast<Impl*>(context);
        // Pair with the reader's store->load fence. Release/acquire alone
        // permits both sides to observe old values and miss the notification.
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&self->sleeping, __ATOMIC_ACQUIRE)) self->signal();
    }
    void signal() {
#ifdef __vita__
        sceKernelSetEventFlag(event, 1);
#else
        pthread_mutex_lock(&mutex);
        signaled = true;
        pthread_cond_signal(&condition);
        pthread_mutex_unlock(&mutex);
#endif
    }
    void wait() {
#ifdef __vita__
        sceKernelWaitEventFlag(event, 1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, nullptr, nullptr);
#else
        pthread_mutex_lock(&mutex);
        while (!signaled) pthread_cond_wait(&condition, &mutex);
        signaled = false;
        pthread_mutex_unlock(&mutex);
#endif
    }
    void loop() {
        unsigned idle = 0;
        while (!__atomic_load_n(&quit, __ATOMIC_ACQUIRE)) {
            if (reader.step()) { idle = 0; continue; }
            // Briefly spin between journal publications; block when the
            // frontend pauses emulation in its menu.
            if (idle++ < 4096) { processorRelax(); continue; }
            // Publish that we may sleep, then recheck the queue. A producer
            // racing this second check leaves the event signaled for wait().
            __atomic_store_n(&sleeping, 1u, __ATOMIC_RELEASE);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (!reader.step() && !__atomic_load_n(&quit, __ATOMIC_ACQUIRE)) wait();
            __atomic_store_n(&sleeping, 0u, __ATOMIC_RELEASE);
            idle = 0;
        }
    }
#ifdef __vita__
    static int entry(SceSize, void* args) { (*static_cast<Impl**>(args))->loop(); return 0; }
#else
    static void* entry(void* arg) { static_cast<Impl*>(arg)->loop(); return nullptr; }
#endif
    bool start(std::string& error) {
        if (!events || !pending) { error = "cannot allocate audio worker buffers"; return false; }
        if (machine.mercury() || machine.cdDrive() || machine.midiOut()) {
            error = "audio worker supports fixed-step OPM/ADPCM only"; return false;
        }
        if (originalRate <= 0 || machine.sound().latencyGoal() || !synth.restoreOutputPosition(machine.sound().outputPosition())) {
            error = "unsupported audio output timing"; return false;
        }
#ifdef __vita__
        event = sceKernelCreateEventFlag("pix68k-audio-event", 0, 0, nullptr);
        if (event < 0) { error = "cannot create audio event"; return false; }
        originalAffinity = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
        if (originalAffinity < 0 || sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0) < 0) {
            error = "cannot set execution CPU affinity"; return false;
        }
#endif
#ifdef __vita__
        thread = sceKernelCreateThread("pix68k-audio", entry, 0x10000100, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
        if (thread < 0) { error = "cannot create audio thread"; return false; }
        Impl* self = this;
        if (sceKernelStartThread(thread, sizeof self, &self) < 0) {
            error = "cannot start audio thread"; return false;
        }
#else
        if (pthread_create(&thread, nullptr, entry, this) != 0) {
            error = "cannot create audio thread"; return false;
        }
#endif
        running = true;
        // No producer records exist until the consumer has started. Failed
        // startup leaves the Machine and its pending audio untouched.
        machine.sound().setOutputRate(0);
        machine.sound().ring().clearStopped();
        link.attach(machine, journal);
        attached = true;
        return true;
    }
    ~Impl() {
        if (paused) resume();
        if (attached) link.detach(machine);
        if (running) {
            __atomic_store_n(&quit, 1u, __ATOMIC_RELEASE);
            signal();
#ifdef __vita__
            sceKernelWaitThreadEnd(thread, nullptr, nullptr);
#else
            pthread_join(thread, nullptr);
#endif
        }
        if (attached) {
            machine.sound().setOutputRate(originalRate);
            x68k::copySoundState(synth, machine.sound());
            machine.sound().restoreOutputPosition(synth.outputPosition());
            auto& ring = machine.sound().ring(); ring.clearStopped();
            const unsigned count = synth.ring().copyPending(pending.get(), x68k::AudioRing::SIZE);
            for (unsigned i = 0; i < count; i++) ring.push(pending[2 * i], pending[2 * i + 1]);
        }
#ifdef __vita__
        if (thread >= 0) sceKernelDeleteThread(thread);
        if (event >= 0) sceKernelDeleteEventFlag(event);
        if (originalAffinity >= 0) sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), originalAffinity);
#else
        pthread_cond_destroy(&condition);
        pthread_mutex_destroy(&mutex);
#endif
    }
};

AudioWorker::AudioWorker(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AudioWorker::~AudioWorker() = default;
std::unique_ptr<AudioWorker> AudioWorker::create(x68k::Machine& m, std::string& error) {
    auto impl = std::unique_ptr<Impl>(new (std::nothrow) Impl(m));
    if (!impl || !impl->start(error)) return nullptr;
    auto result = std::unique_ptr<AudioWorker>(new (std::nothrow) AudioWorker(std::move(impl)));
    if (!result) error = "cannot allocate audio worker";
    return result;
}
void AudioWorker::flush() { impl_->flush(); }
void AudioWorker::snapshotToMachine() { impl_->snapshot(); }
void AudioWorker::pause() { impl_->pause(); }
void AudioWorker::resume() { impl_->resume(); }
x68k::Sound& AudioWorker::sound() { return impl_->synth; }
}
