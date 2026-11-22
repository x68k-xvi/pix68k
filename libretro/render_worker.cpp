#include "render_worker.h"
#include "x68k/render.h"
#include "x68k/state.h"
#include "x68k/video_replay.h"
#include <chrono>
#include <new>
#include <vector>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace pix68k_retro {
namespace {
uint64_t micros() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
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
struct TimedRenderer : x68k::Renderer {
    using Renderer::Renderer;
    bool measure = false;
    uint64_t elapsed = 0;
    void onRaster(int raster) override {
        if (!measure) { Renderer::onRaster(raster); return; }
        const auto start = micros();
        Renderer::onRaster(raster);
        elapsed += micros() - start;
    }
};
}

struct RenderWorker::Impl {
    x68k::Machine& machine;
    uint32_t* pixels;
    x68k::Video shadow;
    TimedRenderer renderer{shadow};
    x68k::VideoJournal journal;
    x68k::VideoReplayer reader{journal, shadow, renderer};
    x68k::RenderLink link;
    std::vector<uint32_t> events;
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

    Impl(x68k::Machine& m, uint32_t* p) : machine(m), pixels(p), events(1u << 18) {
        journal.init(events.data(), 18);
        journal.setRelax(relax);
        journal.setWakeContext(wake, this);
        renderer.setTarget(pixels, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_HEIGHT);
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
            // Rasters arrive tens of microseconds apart. Avoid a kernel wake
            // for every raster, but still block promptly when a menu pauses us.
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
#ifdef __vita__
        event = sceKernelCreateEventFlag("pix68k-render-event", 0, 0, nullptr);
        if (event < 0) { error = "cannot create render event"; return false; }
        originalAffinity = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
        if (originalAffinity < 0 || sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0) < 0) {
            error = "cannot set execution CPU affinity"; return false;
        }
#endif
        link.attach(machine, journal, shadow, reader);
        attached = true;
#ifdef __vita__
        thread = sceKernelCreateThread("pix68k-render", entry, 0x10000100, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
        if (thread < 0) { error = "cannot create render thread"; return false; }
        Impl* self = this;
        if (sceKernelStartThread(thread, sizeof self, &self) < 0) {
            error = "cannot start render thread"; return false;
        }
#else
        if (pthread_create(&thread, nullptr, entry, this) != 0) {
            error = "cannot create render thread"; return false;
        }
#endif
        running = true;
        return true;
    }
    ~Impl() {
        // No records are generated between attach() and a failed start().
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

RenderWorker::RenderWorker(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RenderWorker::~RenderWorker() = default;
std::unique_ptr<RenderWorker> RenderWorker::create(x68k::Machine& m, uint32_t* p, std::string& error) {
    auto impl = std::unique_ptr<Impl>(new (std::nothrow) Impl(m, p));
    if (!impl || !impl->start(error)) return nullptr;
    return std::unique_ptr<RenderWorker>(new (std::nothrow) RenderWorker(std::move(impl)));
}
void RenderWorker::flush() { impl_->journal.flush(); }
void RenderWorker::resync() {
    flush();
    x68k::copyVideoState(impl_->machine.video(), impl_->shadow);
}
void RenderWorker::prepareFrame(bool render, bool measure) {
    // The previous frame was flushed, so the renderer is not being accessed.
    impl_->renderer.setTarget(render ? impl_->pixels : nullptr, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_HEIGHT);
    impl_->renderer.measure = measure;
    impl_->renderer.elapsed = 0;
}
uint64_t RenderWorker::rasterMicros() const { return impl_->renderer.elapsed; }
void RenderWorker::setIndexedLineSink(x68k::IndexedLineSink* sink) {
    flush();
    impl_->renderer.setIndexedLineSink(sink);
}
}
