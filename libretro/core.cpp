// RetroArch の実行スレッドから Machine を 1 フレームずつ進める。
#include "libretro.h"
#include "input.h"
#include "media.h"
#include "render_worker.h"
#include "audio_worker.h"
#include "state.h"
#include "x68k/machine.h"
#include "x68k/render.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <string>
#include <vector>
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
#include "vita/paletted_gpu.h"
#include <atomic>
#endif

namespace {
retro_environment_t environment = nullptr;
retro_log_printf_t logger = nullptr;
retro_video_refresh_t videoCallback = nullptr;
retro_audio_sample_t audioCallback = nullptr;
retro_audio_sample_batch_t audioBatchCallback = nullptr;
retro_input_poll_t pollCallback = nullptr;
retro_input_state_t inputCallback = nullptr;
unsigned controllers[2] = {RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};
constexpr int rate = 48000;
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
std::atomic<bool> gpuFrontendReady{false};
std::unique_ptr<pix68k_retro::PalettedGpu> gpuRenderer;
#endif
using pix68k_retro::readFile;
unsigned initialIndex = 0;
std::string initialPath;

void error(const char* message, const char* detail = "") {
    if (logger) logger(RETRO_LOG_ERROR, "pix68k: %s%s\n", message, detail);
    else std::fprintf(stderr, "pix68k: %s%s\n", message, detail);
    if (environment) {
        std::string text = std::string(message) + detail;
        retro_message notice = {text.c_str(), 240};
        environment(RETRO_ENVIRONMENT_SET_MESSAGE, &notice);
    }
}

const char* option(const char* name, const char* fallback) {
    retro_variable var = {name, nullptr};
    return environment && environment(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value ? var.value : fallback;
}

uint64_t micros() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool loadRom(x68k::Machine& machine, const std::string& dir, bool cg) {
    std::vector<uint8_t> data;
    const char* upper = cg ? "CGROM.DAT" : "IPLROM.DAT";
    const char* lower = cg ? "cgrom.dat" : "iplrom.dat";
    const size_t size = cg ? x68k::Machine::CG_SIZE : x68k::Machine::IPL_SIZE;
    if ((!readFile(dir + upper, data, size) && !readFile(dir + lower, data, size)) || data.size() != size) {
        error("missing or invalid ROM: ", (dir + upper).c_str());
        return false;
    }
    return cg ? machine.setCgRom(data.data(), data.size()) : machine.setIplRom(data.data(), data.size());
}

int64_t localSeconds() {
    const time_t now = std::time(nullptr);
    struct tm local;
    if (!localtime_r(&now, &local)) return 1700000000;
#ifndef PIX68K_VITA
    return (int64_t)now + local.tm_gmtoff;
#else
    struct tm utc;
    if (!gmtime_r(&now, &utc)) return 1700000000;
    utc.tm_isdst = local.tm_isdst;
    const time_t interpreted = std::mktime(&utc);
    if (interpreted == (time_t)-1) return (int64_t)now;
    return (int64_t)now + (int64_t)std::difftime(now, interpreted);
#endif
}

#ifdef PIX68K_VITA
constexpr const char* padDefault = "enabled";
constexpr const char* padChoices = "enabled|disabled";
constexpr const char* clockDefault = "10MHz";
constexpr const char* audioThreadDefault = "enabled";
constexpr const char* audioThreadChoices = "enabled|disabled";
constexpr const char* renderThreadDefault = "enabled";
constexpr const char* renderThreadChoices = "enabled|disabled";
constexpr const char* clockChoices = "10MHz|16MHz";
#else
constexpr const char* padDefault = "disabled";
constexpr const char* padChoices = "disabled|enabled";
constexpr const char* clockDefault = "16MHz";
constexpr const char* audioThreadDefault = "disabled";
constexpr const char* audioThreadChoices = "disabled|enabled";
constexpr const char* renderThreadDefault = "disabled";
constexpr const char* renderThreadChoices = "disabled|enabled";
constexpr const char* clockChoices = "16MHz|10MHz";
#endif

struct Session : x68k::LineListener {
    pix68k_retro::Media media;
    unsigned selected[2] = {~0u, ~0u};
    bool ejected[2] = {};
    bool pendingEject[2] = {};
    unsigned flushFrames = 0;
    bool saveError = false;
    size_t stateCapacity = 0;
    std::unique_ptr<x68k::Machine> machine;
    std::vector<uint32_t> pixels;
    std::unique_ptr<x68k::Renderer> renderer;
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
    std::unique_ptr<pix68k_retro::PalettedFrame> gpuFrame;
    bool gpuWanted = false, gpuActive = false;
    void configureGpu(bool eligible) {
        const bool active = gpuWanted && eligible && gpuFrontendReady.load(std::memory_order_acquire);
        if (active == gpuActive) return;
        if (renderWorker) renderWorker->flush();
        if (active && !gpuFrame) gpuFrame.reset(new (std::nothrow) pix68k_retro::PalettedFrame);
        if (active && (!gpuFrame || !gpuFrame->valid())) {
            gpuFrame.reset(); gpuWanted = false;
            if (logger) logger(RETRO_LOG_WARN, "pix68k: insufficient memory for GPU layers; using CPU composition\n");
            return;
        }
        if (gpuFrame) gpuFrame->software = pixels.data();
        if (gpuFrame) {
            gpuFrame->resolve(pixels.data(), pix68k_retro::PalettedFrame::stride, pix68k_retro::PalettedFrame::rows);
            gpuFrame->invalidate();
            gpuFrame->skipSoftware = active;
        }
        renderer->setIndexedLineSink(active ? gpuFrame.get() : nullptr);
        if (renderWorker) renderWorker->setIndexedLineSink(active ? gpuFrame.get() : nullptr);
        gpuActive = active;
        renderNext = true;
    }
    void resolveGpu() {
        if (gpuFrame) gpuFrame->resolve(pixels.data(), pix68k_retro::PalettedFrame::stride, pix68k_retro::PalettedFrame::rows);
    }
#endif
    std::unique_ptr<pix68k_retro::RenderWorker> renderWorker;
    std::unique_ptr<pix68k_retro::AudioWorker> audioWorker;
    x68k::Sound& outputSound() { return audioWorker ? audioWorker->sound() : machine->sound(); }
    int16_t audio[2 * x68k::AudioRing::SIZE] = {};
    size_t pendingAudio = 0;
    bool keys[256] = {};
    bool firstRun = true;
    retro_system_av_info av = {};
    bool renderNext = true;
    bool measureRaster = false;
    uint64_t rasterUs = 0;
    uint64_t emulationUs = 0, presentationUs = 0, audioUs = 0, renderUs = 0;
    unsigned measuredFrames = 0;

    void onRaster(int raster) override {
        if (!measureRaster) { renderer->onRaster(raster); return; }
        const auto start = micros();
        renderer->onRaster(raster);
        rasterUs += micros() - start;
    }
    void onFrameEnd() override { renderer->onFrameEnd(); }

    ~Session() {
        audioWorker.reset();
        renderWorker.reset();
        if (machine) machine->setLineListener(nullptr);
    }

    pix68k_retro::StateContext stateContext() {
        return {*machine, media, pixels.data(), audio, pendingAudio, keys, selected, ejected, pendingEject, flushFrames, firstRun, audioWorker ? &audioWorker->sound() : nullptr};
    }

    bool flushMedia() {
        std::string message;
        bool ok = media.flush(message);
        if (!ok && !saveError) error(message.c_str());
        saveError = !ok;
        return ok;
    }

    void serviceEjects() {
        bool requested = false;
        for (unsigned d = 0; d < 2; d++) {
            if (machine->fdc().takeEjectRequest((int)d)) { pendingEject[d] = true; requested = true; }
        }
        if ((pendingEject[0] || pendingEject[1]) && (requested || flushFrames == 0) && flushMedia()) {
            for (unsigned d = 0; d < 2; d++) if (pendingEject[d]) {
                machine->ejectDisk((int)d);
                ejected[d] = true;
                pendingEject[d] = false;
            }
        }
    }

    void reset() {
        if (audioWorker) audioWorker->flush();
        if (renderWorker) renderWorker->flush();
        machine->powerOn();
        if (renderWorker) renderWorker->flush();
        if (audioWorker) audioWorker->flush();
        outputSound().ring().clearStopped();
        renderer->renderFrame();
        std::fill(pixels.begin(), pixels.end(), 0);
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
        if (gpuFrame) gpuFrame->invalidate();
#endif
        std::memset(keys, 0, sizeof keys);
        pendingAudio = 0;
        pendingEject[0] = pendingEject[1] = false;
        firstRun = false;
        renderNext = true;
    }

    retro_system_av_info currentAv() {
        retro_system_av_info info = {};
        int width, height;
        renderer->geometry(width, height);
        info.geometry = {(unsigned)std::max(1, width), (unsigned)std::max(1, height),
                         x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_HEIGHT, 4.0f / 3.0f};
        info.timing = {machine->frameRate(), rate};
        return info;
    }

    void updateAv() {
        auto next = currentAv();
        bool timing = std::abs(next.timing.fps - av.timing.fps) > 1e-8;
        bool geometry = next.geometry.base_width != av.geometry.base_width || next.geometry.base_height != av.geometry.base_height;
        if (environment) {
            if (timing) environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &next);
            else if (geometry) environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &next.geometry);
        }
        av = next;
    }

    void pollInput() {
        if (pollCallback) pollCallback();
        bool down[256] = {};
        if (inputCallback) {
            for (unsigned key = 0; key < RETROK_LAST; key++) {
                uint8_t usage = pix68k_retro::keyUsage(key);
                if (usage && inputCallback(0, RETRO_DEVICE_KEYBOARD, 0, key)) down[usage] = true;
            }
        }
        auto pad = [](unsigned id) {
            return inputCallback && controllers[0] == RETRO_DEVICE_JOYPAD &&
                   inputCallback(0, RETRO_DEVICE_JOYPAD, 0, id);
        };
        const bool keyboardMode = std::strcmp(option("pix68k_pad_keyboard", padDefault), "enabled") == 0 &&
                                  pad(RETRO_DEVICE_ID_JOYPAD_L);
        if (keyboardMode) {
            if (pad(RETRO_DEVICE_ID_JOYPAD_A)) down[0x28] = true; // Return
            if (pad(RETRO_DEVICE_ID_JOYPAD_B)) down[0x2c] = true; // Space
            if (pad(RETRO_DEVICE_ID_JOYPAD_Y)) down[0x29] = true; // Escape
            if (pad(RETRO_DEVICE_ID_JOYPAD_X)) down[0x3a] = true; // F1
            if (pad(RETRO_DEVICE_ID_JOYPAD_RIGHT)) down[0x4f] = true;
            if (pad(RETRO_DEVICE_ID_JOYPAD_LEFT)) down[0x50] = true;
            if (pad(RETRO_DEVICE_ID_JOYPAD_DOWN)) down[0x51] = true;
            if (pad(RETRO_DEVICE_ID_JOYPAD_UP)) down[0x52] = true;
        }
        for (unsigned i = 1; i < 256; i++) {
            if (keys[i] != down[i]) machine->input().onKey((uint8_t)i, down[i]);
            keys[i] = down[i];
        }
        using Sink = platform::InputSink;
        const unsigned ids[] = {RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT,
                                RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A};
        for (unsigned port = 0; port < 2; port++) {
            uint8_t bits = 0;
            if (controllers[port] == RETRO_DEVICE_JOYPAD && inputCallback) {
                for (unsigned i = 0; i < 6; i++)
                    if (!(port == 0 && keyboardMode) && inputCallback(port, RETRO_DEVICE_JOYPAD, 0, ids[i]))
                        bits |= (uint8_t)(1u << i);
            }
            machine->input().onJoystick(port, bits);
        }
        int dx = 0, dy = 0;
        uint8_t buttons = 0;
        if (inputCallback) {
            dx = inputCallback(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
            dy = inputCallback(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
            if (inputCallback(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT)) buttons |= Sink::MOUSE_LEFT;
            if (inputCallback(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT)) buttons |= Sink::MOUSE_RIGHT;
        }
        if (inputCallback && controllers[0] == RETRO_DEVICE_JOYPAD &&
            std::strcmp(option("pix68k_pad_mouse", padDefault), "enabled") == 0) {
            auto motion = [](int value) { return std::abs(value) <= 8192 ? 0 : value * 8 / 32767; };
            dx += motion(inputCallback(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X));
            dy += motion(inputCallback(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));
            if (pad(RETRO_DEVICE_ID_JOYPAD_R))
                buttons |= pad(RETRO_DEVICE_ID_JOYPAD_SELECT) ? Sink::MOUSE_RIGHT : Sink::MOUSE_LEFT;
        }
        machine->input().onMouse(dx, dy, buttons);
    }

    void outputAudio() {
        pendingAudio += outputSound().ring().drain(audio + 2 * pendingAudio, x68k::AudioRing::SIZE - (int)pendingAudio);
        size_t used = 0;
        if (audioBatchCallback && pendingAudio) used = std::min(pendingAudio, audioBatchCallback(audio, pendingAudio));
        else if (audioCallback) {
            for (size_t i = 0; i < pendingAudio; i++) audioCallback(audio[2 * i], audio[2 * i + 1]);
            used = pendingAudio;
        } else used = pendingAudio;
        pendingAudio -= used;
        if (pendingAudio) std::memmove(audio, audio + 2 * used, pendingAudio * 2 * sizeof(int16_t));
    }
};
std::unique_ptr<Session> session;


unsigned targetDrive() { return std::strcmp(option("pix68k_disk_drive", "FD0"), "FD1") == 0 ? 1 : 0; }
bool RETRO_CALLCONV setEjected(bool eject) {
    if (!session) return false;
    unsigned drive = targetDrive();
    if (session->ejected[drive] == eject) return true;
    if (eject) {
        if (!session->flushMedia()) return false;
        session->machine->ejectDisk((int)drive);
    } else {
        unsigned index = session->selected[drive];
        auto& disks = session->media.floppies;
        if (index < disks.size()) {
            if (!disks[index]) return false;
            if (!session->ejected[drive ^ 1] && session->selected[drive ^ 1] == index) return false;
            session->machine->insertDisk((int)drive, &disks[index]->floppy);
        }
    }
    session->ejected[drive] = eject;
    session->pendingEject[drive] = false;
    return true;
}
bool RETRO_CALLCONV getEjected() { return session ? session->ejected[targetDrive()] : true; }
unsigned RETRO_CALLCONV imageCount() { return session ? (unsigned)session->media.floppies.size() : 0; }
unsigned RETRO_CALLCONV imageIndex() {
    return session ? std::min(imageCount(), session->selected[targetDrive()]) : 0;
}
bool RETRO_CALLCONV setImageIndex(unsigned index) {
    if (!session || !getEjected()) return false;
    session->selected[targetDrive()] = index < imageCount() ? index : ~0u;
    return true;
}
bool RETRO_CALLCONV replaceImage(unsigned index, const retro_game_info* info) {
    if (!session || !getEjected() || index >= imageCount()) return false;
    for (unsigned d = 0; d < 2; d++)
        if (!session->ejected[d] && session->selected[d] == index) return false;
    auto& disks = session->media.floppies;
    if (info && (!info->path || !*info->path)) return false;
    if (info && disks[index] && disks[index]->path == pix68k_retro::absolutePath(info->path)) return true;
    std::unique_ptr<pix68k_retro::MediaImage> image;
    if (info) {
        std::string message;
        image = session->media.loadFloppy(info->path, message, disks[index].get());
        if (!image) { error(message.c_str()); return false; }
    }
    std::string message;
    if (disks[index] && !disks[index]->flush(message)) { error(message.c_str()); return false; }
    if (info) disks[index] = std::move(image);
    else {
        disks.erase(disks.begin() + index);
        for (auto& selected : session->selected) {
            if (selected == index) selected = ~0u;
            else if (selected != ~0u && selected > index) selected--;
        }
    }
    return true;
}
bool RETRO_CALLCONV addImage() {
    if (!session || imageCount() >= pix68k_retro::Media::MAX_IMAGES) return false;
    session->media.floppies.push_back(nullptr);
    return true;
}
bool RETRO_CALLCONV setInitialImage(unsigned index, const char* path) {
    if (!path || !*path) return false;
    initialIndex = index;
    initialPath = path;
    return true;
}
bool copyText(const std::string& value, char* out, size_t len) {
    if (!out || len == 0) return false;
    out[0] = 0;
    if (value.size() >= len) return false;
    std::memcpy(out, value.c_str(), value.size() + 1);
    return true;
}
bool RETRO_CALLCONV imagePath(unsigned index, char* out, size_t len) {
    if (!session || index >= imageCount() || !session->media.floppies[index]) return false;
    return copyText(session->media.floppies[index]->path, out, len);
}
bool RETRO_CALLCONV imageLabel(unsigned index, char* out, size_t len) {
    if (!session || index >= imageCount() || !session->media.floppies[index]) return false;
    const auto& path = session->media.floppies[index]->path;
    size_t slash = path.find_last_of("/\\");
    return copyText(path.substr(slash == std::string::npos ? 0 : slash + 1), out, len);
}
void registerDisks() {
    initialIndex = 0;
    initialPath.clear();
    if (!environment) return;
    unsigned version = 0;
    static const retro_disk_control_ext_callback extended = {
        setEjected, getEjected, imageIndex, setImageIndex, imageCount, replaceImage, addImage,
        setInitialImage, imagePath, imageLabel
    };
    if (environment(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &version) && version >= 1 &&
        environment(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, const_cast<retro_disk_control_ext_callback*>(&extended))) return;
    static const retro_disk_control_callback legacy = {
        setEjected, getEjected, imageIndex, setImageIndex, imageCount, replaceImage, addImage
    };
    environment(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, const_cast<retro_disk_control_callback*>(&legacy));
}

void defaultAv(retro_system_av_info* info) {
    *info = {};
    info->geometry = {768, 512, 1024, 1024, 4.0f / 3.0f};
    info->timing = {55.46, rate};
}
} // namespace

void retro_set_environment(retro_environment_t cb) {
    environment = cb;
    logger = nullptr;
    if (!environment) return;
    bool noGame = true;
    environment(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
    static const std::string keyboardChoice = std::string("Pad keyboard shortcuts; ") + padChoices;
    static const std::string mouseChoice = std::string("Pad mouse; ") + padChoices;
    static const std::string clockChoice = std::string("CPU clock (restart content); ") + clockChoices;
    static const std::string renderThreadChoice = std::string("Render on another CPU core (restart content); ") + renderThreadChoices;
    static const std::string audioThreadChoice = std::string("Synthesize audio on another CPU core (restart content); ") + audioThreadChoices;
    static const retro_variable variables[] = {
        {"pix68k_memory", "Memory (restart content); 2MB|12MB"},
        {"pix68k_clock", clockChoice.c_str()},
        {"pix68k_rtc", "RTC start (restart content); host|fixed"},
        {"pix68k_disk_drive", "Disk Control target drive; FD0|FD1"},
        {"pix68k_disk_write", "Disk writes (restart content); read-only|save-copy"},
        {"pix68k_pad_keyboard", keyboardChoice.c_str()},
        {"pix68k_pad_mouse", mouseChoice.c_str()},
        {"pix68k_frameskip", "Frameskip (video only); 0|1|2|3"},
        {"pix68k_performance_log", "Performance timing log; disabled|enabled"},
        {"pix68k_render_thread", renderThreadChoice.c_str()},
        {"pix68k_audio_thread", audioThreadChoice.c_str()},
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
        {"pix68k_gpu_layers", "Vita GPU palette/layer composition (restart content); enabled|disabled"},
#endif
        {nullptr, nullptr}
    };
    environment(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(variables));
    static const retro_controller_description types[] = {{"X68000 joystick", RETRO_DEVICE_JOYPAD}};
    static const retro_controller_info ports[] = {{types, 1}, {types, 1}, {nullptr, 0}};
    environment(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<retro_controller_info*>(ports));
}
void retro_set_video_refresh(retro_video_refresh_t cb) { videoCallback = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audioCallback = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audioBatchCallback = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { pollCallback = cb; }
void retro_set_input_state(retro_input_state_t cb) { inputCallback = cb; }

void retro_init() {
    logger = nullptr;
    if (environment) {
        retro_log_callback logging = {};
        if (environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) logger = logging.log;
    }
    controllers[0] = controllers[1] = RETRO_DEVICE_JOYPAD;
    registerDisks();
}
void retro_deinit() {
    retro_unload_game();
    logger = nullptr;
}
unsigned retro_api_version() { return RETRO_API_VERSION; }
void retro_get_system_info(retro_system_info* info) {
    *info = {};
    info->library_name = "pix68k";
    info->library_version = "0.1-dev";
    info->valid_extensions = "xdf|dim|d88|88d|m3u|hdf";
    info->need_fullpath = true;
    info->block_extract = false;
}
void retro_get_system_av_info(retro_system_av_info* info) {
    if (session) *info = session->currentAv();
    else defaultAv(info);
}
void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (port < 2) controllers[port] = device & RETRO_DEVICE_MASK;
}
void retro_reset() { if (session) session->reset(); }
void retro_run() {
    if (!session) return;
    // RetroArch は load_game のあとで SAVE_RAM を復元する。容量補正をそのあとに行う。
    if (session->firstRun) session->reset();
    session->pollInput();
    const char* skipOption = option("pix68k_frameskip", "0");
    unsigned skip = skipOption[0] >= '0' && skipOption[0] <= '3' && !skipOption[1] ? skipOption[0] - '0' : 0;
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
    session->configureGpu(skip == 0);
#endif
    bool render = session->renderNext || session->machine->frameCount() % (skip + 1) == 0;
    const bool measure = logger && std::strcmp(option("pix68k_performance_log", "disabled"), "enabled") == 0;
    session->measureRaster = measure;
    session->rasterUs = 0;
    if (session->renderWorker) session->renderWorker->prepareFrame(render, measure);
    if (!render) session->renderer->setTarget(nullptr, 0, 0, 0);
    const auto start = measure ? micros() : 0;
    session->machine->runFrame();
    if (session->renderWorker) {
        session->renderWorker->flush();
        session->rasterUs = session->renderWorker->rasterMicros();
    }
    if (session->audioWorker) session->audioWorker->flush();
    session->renderer->setTarget(session->pixels.data(), x68k::Renderer::MAX_WIDTH,
                                 x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_HEIGHT);
    auto previousAv = session->av;
    session->updateAv();
    if (!render && (session->av.geometry.base_width != previousAv.geometry.base_width ||
                    session->av.geometry.base_height != previousAv.geometry.base_height)) {
        // A mode change during a skipped frame still needs a complete surface.
        const auto beforeRender = measure ? micros() : 0;
        session->renderer->renderFrame();
        if (measure) session->rasterUs += micros() - beforeRender;
        render = true;
    }
    session->renderNext = false;
    const auto emulated = measure ? micros() : 0;
    bool canDupe = false;
    if (!render && environment) environment(RETRO_ENVIRONMENT_GET_CAN_DUPE, &canDupe);
    if (videoCallback) videoCallback(render || !canDupe ? session->pixels.data() : nullptr, session->av.geometry.base_width,
                                    session->av.geometry.base_height, x68k::Renderer::MAX_WIDTH * sizeof(uint32_t));
    const auto presented = measure ? micros() : 0;
    session->outputAudio();
    if (measure) {
        session->emulationUs += emulated - start;
        session->presentationUs += presented - emulated;
        session->audioUs += micros() - presented;
        session->renderUs += session->rasterUs;
        if (++session->measuredFrames == 300) {
            logger(RETRO_LOG_INFO, "pix68k timing: 300 frames, clock=%uHz skip=%u emulation=%.2fms (raster=%.2fms) video=%.2fms audio=%.2fms per frame\n",
                   session->machine->config().cpuHz, skip, session->emulationUs / 300000.0,
                   session->renderUs / 300000.0, session->presentationUs / 300000.0, session->audioUs / 300000.0);
            session->emulationUs = session->presentationUs = session->audioUs = session->renderUs = 0;
            session->measuredFrames = 0;
        }
    } else {
        session->emulationUs = session->presentationUs = session->audioUs = session->renderUs = 0;
        session->measuredFrames = 0;
    }
    if (++session->flushFrames >= 120) {
        session->flushFrames = 0;
    }
    {
        std::string message;
        bool ok = session->media.flushIdle(message);
        if (!ok && !session->saveError) error(message.c_str());
        session->saveError = !ok;
    }
    session->serviceEjects();
}
bool retro_load_game(const retro_game_info* game) {
    unsigned requestedIndex = initialIndex;
    std::string requestedPath = std::move(initialPath);
    initialIndex = 0;
    initialPath.clear();
    retro_unload_game();
    const char* systemDir = nullptr;
    if (!environment || !environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &systemDir) || !systemDir || !*systemDir) {
        error("frontend system directory is unavailable");
        return false;
    }
    if (game && (!game->path || !*game->path)) {
        error("a full disk image path is required");
        return false;
    }
    auto next = std::unique_ptr<Session>(new (std::nothrow) Session);
    if (!next) return false;
    x68k::Config config;
    config.ramSize = std::strcmp(option("pix68k_memory", "2MB"), "12MB") == 0 ? 12u << 20 : 2u << 20;
    config.setClockMHz(std::strcmp(option("pix68k_clock", clockDefault), "10MHz") == 0 ? 10 : 16);
    next->machine.reset(new (std::nothrow) x68k::Machine(config));
    if (!next->machine || !next->machine->ok()) { error("cannot allocate machine"); return false; }
    std::string romDir = std::string(systemDir) + "/pix68k/";
    if (!loadRom(*next->machine, romDir, false) || !loadRom(*next->machine, romDir, true)) return false;
    next->media.writable = std::strcmp(option("pix68k_disk_write", "read-only"), "save-copy") == 0;
    const char* saveDir = nullptr;
    if (environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDir) && saveDir) next->media.saveDirectory = saveDir;
    if (game) {
        std::string message;
        if (!next->media.load(game->path, message)) { error(message.c_str()); return false; }
        auto& disks = next->media.floppies;
        if (!disks.empty()) {
            unsigned first = 0;
            if (requestedIndex < disks.size() && disks[requestedIndex]->path == pix68k_retro::absolutePath(requestedPath)) first = requestedIndex;
            next->selected[0] = first;
            next->machine->insertDisk(0, &disks[first]->floppy);
            if (disks.size() > 1 && first != 1) {
                next->selected[1] = 1;
                next->machine->insertDisk(1, &disks[1]->floppy);
            }
        }
        if (next->media.hard) next->machine->attachSasi(0, &next->media.hard->hard);
    }
    retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        error("frontend does not support XRGB8888");
        return false;
    }
    next->pixels.resize((size_t)x68k::Renderer::MAX_WIDTH * x68k::Renderer::MAX_HEIGHT);
    next->renderer.reset(new (std::nothrow) x68k::Renderer(next->machine->video()));
    if (!next->renderer) return false;
    next->renderer->setTarget(next->pixels.data(), x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_HEIGHT);
    next->machine->setLineListener(next.get());
    next->machine->sound().setOutputRate(rate);
    next->machine->setTime(std::strcmp(option("pix68k_rtc", "host"), "fixed") == 0 ? 1700000000 : localSeconds());
    next->machine->powerOn();
    next->av = next->currentAv();
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
    next->gpuWanted = std::strcmp(option("pix68k_gpu_layers", "enabled"), "enabled") == 0;
#endif
    if (std::strcmp(option("pix68k_render_thread", renderThreadDefault), "enabled") == 0) {
        std::string message;
        next->renderWorker = pix68k_retro::RenderWorker::create(*next->machine, next->pixels.data(), message);
        if (!next->renderWorker) {
            next->machine->setLineListener(next.get());
            if (logger) logger(RETRO_LOG_WARN, "pix68k: rendering on execution thread: %s\n", message.c_str());
        }
    }
    if (std::strcmp(option("pix68k_audio_thread", audioThreadDefault), "enabled") == 0) {
        std::string message;
        next->audioWorker = pix68k_retro::AudioWorker::create(*next->machine, message);
        if (!next->audioWorker && logger) logger(RETRO_LOG_WARN, "pix68k: synthesizing audio on execution thread: %s\n", message.c_str());
    }
    if (logger) logger(RETRO_LOG_INFO, "pix68k config: 68000, RAM=%u, clock=%uHz, frameskip=%s, timing=%s, render_thread=%s audio_thread=%s\n",
                       config.ramSize, config.cpuHz, option("pix68k_frameskip", "0"), option("pix68k_performance_log", "disabled"), next->renderWorker ? "enabled" : "disabled", next->audioWorker ? "enabled" : "disabled");
    next->stateCapacity = pix68k_retro::serializeCapacity(next->stateContext());
    session = std::move(next);
    return true;
}
bool retro_load_game_special(unsigned, const retro_game_info*, size_t) {
    error("subsystem loading is not supported");
    return false;
}
void retro_unload_game() {
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
    gpuRenderer.reset();
#endif
    if (session) session->flushMedia();
    session.reset();
}
unsigned retro_get_region() { return RETRO_REGION_NTSC; }
size_t retro_serialize_size() { return session ? session->stateCapacity : 0; }
bool retro_serialize(void* out, size_t size) {
    if (!session) return false;
    if (session->renderWorker) session->renderWorker->flush();
    if (session->audioWorker) session->audioWorker->snapshotToMachine();
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
    session->resolveGpu();
#endif
    std::string message;
    if (!pix68k_retro::serialize(session->stateContext(), out, size, message)) { error(message.c_str()); return false; }
    return true;
}
bool retro_unserialize(const void* data, size_t size) {
    if (!session) return false;
    if (session->renderWorker) session->renderWorker->flush();
    std::string message;
    if (session->audioWorker) session->audioWorker->pause();
    const bool restored = pix68k_retro::unserialize(session->stateContext(), data, size, message);
    if (session->audioWorker) session->audioWorker->resume();
    if (!restored) { error(message.c_str()); return false; }
    session->saveError = false;
    if (session->renderWorker) session->renderWorker->resync();
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
    if (session->gpuFrame) session->gpuFrame->invalidate();
#endif
    session->renderNext = true;
    return true;
}
void retro_cheat_reset() {}
#if defined(__vita__) && defined(PIX68K_VITA_GPU_LAYERS)
// Private bridge for the packaged vita2d backend. Vanilla libretro frontends
// never activate it and continue to receive the ordinary software surface.
extern "C" void pix68k_vita_gpu_frontend(bool ready) {
    const bool previous = gpuFrontendReady.exchange(ready, std::memory_order_acq_rel);
    if (!ready) {
        if (previous && session) session->configureGpu(false);
        gpuRenderer.reset();
    }
}
extern "C" void pix68k_vita_gpu_resolve() {
    if (!session || !session->gpuActive) return;
    if (session->renderWorker) session->renderWorker->flush();
    session->resolveGpu();
}
extern "C" void* pix68k_vita_gpu_frame() {
    if (!gpuFrontendReady.load(std::memory_order_acquire) || !session || !session->gpuActive) return nullptr;
    auto& frame = *session->gpuFrame;
    const int width = session->av.geometry.base_width, height = session->av.geometry.base_height;
    // A mode change can leave older, narrower indexed rows in the surface.
    // Materialize those rows before using the software fallback for them.
    for (int y = 0; y < height; y++) if (frame.supported[y] && frame.widths[y] < width) {
        const auto* tp = frame.textColors.data() + y * 256;
        const auto* gp = frame.graphicColors.data() + y * 256;
        for (unsigned x = 0; x < frame.widths[y]; x++) {
            const auto at = y * pix68k_retro::PalettedFrame::stride + x;
            const uint32_t f = tp[frame.front[at]];
            session->pixels[at] = f ? f : gp[frame.graphic[at]];
        }
        frame.supported[y] = false;
    }
    if (!gpuRenderer) gpuRenderer.reset(new (std::nothrow) pix68k_retro::PalettedGpu);
    void* result = gpuRenderer ? gpuRenderer->render(frame, session->pixels.data(), width, height) : nullptr;
    if (!result) session->resolveGpu();
    return result;
}
#endif
void retro_cheat_set(unsigned, bool, const char*) {}
void* retro_get_memory_data(unsigned id) {
    if (!session) return nullptr;
    if (id == RETRO_MEMORY_SAVE_RAM) return session->machine->sram();
    if (id == RETRO_MEMORY_SYSTEM_RAM) return session->machine->mainRam();
    return nullptr;
}
size_t retro_get_memory_size(unsigned id) {
    if (!session) return 0;
    if (id == RETRO_MEMORY_SAVE_RAM) return x68k::Machine::SRAM_SIZE;
    if (id == RETRO_MEMORY_SYSTEM_RAM) return session->machine->config().ramSize;
    return 0;
}
