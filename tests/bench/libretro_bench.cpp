// Shared-core throughput, including software video/audio; frontend presentation is excluded.
#include "core_api.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>

namespace {
const char* systemDir;
const char* memory;
const char* clockMHz;
size_t audioFrames = 0;
unsigned width = 0, height = 0;
bool environment(unsigned cmd, void* data) {
    if (cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) {
        *static_cast<const char**>(data) = systemDir; return true;
    }
    if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE) {
        auto& v = *static_cast<retro_variable*>(data);
        if (!std::strcmp(v.key, "pix68k_memory")) v.value = memory;
        else if (!std::strcmp(v.key, "pix68k_clock")) v.value = clockMHz;
        else if (!std::strcmp(v.key, "pix68k_rtc")) v.value = "fixed";
        else return false;
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
        return *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
    return cmd == RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO || cmd == RETRO_ENVIRONMENT_SET_GEOMETRY;
}
void video(const void*, unsigned w, unsigned h, size_t) { width = w; height = h; }
size_t audio(const int16_t*, size_t n) { audioFrames += n; return n; }
void poll() {}
int16_t input(unsigned, unsigned, unsigned, unsigned) { return 0; }
}
int main(int argc, char** argv) {
    if (argc < 4 || argc > 7) {
        std::fprintf(stderr, "usage: %s core.so system-directory disk [frames=6000] [2MB|12MB] [16MHz|10MHz]\n", argv[0]);
        return 2;
    }
    char* end = nullptr;
    long frames = argc > 4 ? std::strtol(argv[4], &end, 10) : 6000;
    memory = argc > 5 ? argv[5] : "2MB";
    clockMHz = argc > 6 ? argv[6] : "16MHz";
    if ((end && *end) || frames < 1 || frames > 1000000 ||
        (std::strcmp(memory, "2MB") && std::strcmp(memory, "12MB")) ||
        (std::strcmp(clockMHz, "16MHz") && std::strcmp(clockMHz, "10MHz"))) {
        std::fprintf(stderr, "invalid frames, memory or clock argument\n"); return 2;
    }
    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    Api api;
#define LOAD(name) api.name = reinterpret_cast<decltype(api.name)>(dlsym(handle, #name)); if (!api.name) { std::fprintf(stderr, "missing symbol: %s\n", #name); dlclose(handle); return 1; }
    CORE_API(LOAD)
#undef LOAD
    systemDir = argv[2];
    api.retro_set_environment(environment);
    api.retro_set_video_refresh(video);
    api.retro_set_audio_sample_batch(audio);
    api.retro_set_input_poll(poll);
    api.retro_set_input_state(input);
    api.retro_init();
    retro_game_info game = {argv[3], nullptr, 0, nullptr};
    if (!api.retro_load_game(&game)) { api.retro_deinit(); dlclose(handle); return 1; }
    for (unsigned i = 0; i < 600; i++) api.retro_run();
    audioFrames = 0;
    std::vector<double> times;
    times.reserve(frames);
    double emulated = 0;
    unsigned late = 0;
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    for (long i = 0; i < frames; i++) {
        auto t = Clock::now();
        api.retro_run();
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        retro_system_av_info av;
        api.retro_get_system_av_info(&av);
        if (!(av.timing.fps > 0)) { std::fprintf(stderr, "invalid core frame rate\n"); api.retro_deinit(); dlclose(handle); return 1; }
        double budget = 1000 / av.timing.fps;
        emulated += budget / 1000;
        if (ms > budget) late++;
        times.push_back(ms);
    }
    double wall = std::chrono::duration<double>(Clock::now() - start).count();
    std::sort(times.begin(), times.end());
    std::printf("%s %s: %ld frames, %ux%u, audio=%zu frames\n", memory, clockMHz, frames, width, height, audioFrames);
    std::printf("emulated=%.3fs wall=%.3fs speed=%.2fx mean=%.3fms p95=%.3fms p99=%.3fms max=%.3fms over-budget=%u\n",
        emulated, wall, emulated / wall, wall * 1000 / frames,
        times[(times.size() - 1) * 95 / 100], times[(times.size() - 1) * 99 / 100], times.back(), late);
    api.retro_unload_game(); api.retro_deinit(); dlclose(handle);
    return 0;
}
