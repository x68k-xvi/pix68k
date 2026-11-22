// libretro の出力を、同じ ROM / 入力 / RTC で直接動かした Machine と照合する。
#include "core_api.h"
#include "test_check.h"
#include "x68k/machine.h"
#include "x68k/render.h"
#include "x68k/state.h"
#include "x68k/sram_file.h"
#include "x68k/textscreen.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
std::string systemDirectory, memory = "2MB", clockOption = "16MHz", diskDrive = "FD0";
std::string padInputs = "enabled";
std::string frameskip = "0";
std::string renderThread = "enabled", audioThread = "enabled";
bool deliveredFrame = false;
unsigned duplicateFrames = 0;
bool canDupe = true, performanceLog = false;
unsigned timingLogs = 0;
void RETRO_CALLCONV logMessage(enum retro_log_level level, const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof message, format, args);
    va_end(args);
    if (level == RETRO_LOG_INFO && std::strstr(message, "pix68k timing:")) {
        timingLogs++;
        CHECK_EQ(std::strstr(message, "=nan") == nullptr, true);
        CHECK_EQ(std::strstr(message, "=inf") == nullptr, true);
    }
}
retro_disk_control_ext_callback diskControl = {};
bool rejectPixel = false, shortAudio = false, stallAudio = false;
int frame = 0, polls = 0, videos = 0, timingChanges = 0, errors = 0, stateErrors = 0;
bool insideRun = false;
void run(Api& api) { insideRun = true; api.retro_run(); insideRun = false; }
bool nonblack = false, audible = false, realInput = false;
unsigned width = 0, height = 0;
std::vector<uint32_t> picture;
std::vector<int16_t> sound;

bool RETRO_CALLCONV environment(unsigned cmd, void* data) {
    if (cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) {
        *static_cast<const char**>(data) = systemDirectory.c_str();
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION) { *static_cast<unsigned*>(data) = 1; return true; }
    if (cmd == RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE) { diskControl = *static_cast<retro_disk_control_ext_callback*>(data); return true; }
    if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE) {
        auto& v = *static_cast<retro_variable*>(data);
        if (!std::strcmp(v.key, "pix68k_memory")) v.value = memory.c_str();
        else if (!std::strcmp(v.key, "pix68k_clock")) v.value = clockOption.c_str();
        else if (!std::strcmp(v.key, "pix68k_rtc")) v.value = "fixed";
        else if (!std::strcmp(v.key, "pix68k_disk_drive")) v.value = diskDrive.c_str();
        else if (!std::strcmp(v.key, "pix68k_pad_keyboard") || !std::strcmp(v.key, "pix68k_pad_mouse")) v.value = padInputs.c_str();
        else if (!std::strcmp(v.key, "pix68k_frameskip")) v.value = frameskip.c_str();
        else if (!std::strcmp(v.key, "pix68k_render_thread")) v.value = renderThread.c_str();
        else if (!std::strcmp(v.key, "pix68k_audio_thread")) v.value = audioThread.c_str();
        else if (!std::strcmp(v.key, "pix68k_performance_log")) v.value = performanceLog ? "enabled" : "disabled";
        else return false;
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_GET_CAN_DUPE) { *static_cast<bool*>(data) = canDupe; return true; }
    if (cmd == RETRO_ENVIRONMENT_GET_LOG_INTERFACE) { static_cast<retro_log_callback*>(data)->log = logMessage; return true; }
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        CHECK_EQ(*static_cast<retro_pixel_format*>(data), RETRO_PIXEL_FORMAT_XRGB8888);
        return !rejectPixel;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_GEOMETRY) {
        CHECK_EQ(insideRun, true);
        CHECK_EQ(static_cast<retro_game_geometry*>(data)->aspect_ratio, 4.0f / 3.0f);
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO) {
        CHECK_EQ(insideRun, true);
        const auto& av = *static_cast<retro_system_av_info*>(data);
        CHECK_EQ(av.timing.fps > 0, true);
        CHECK_EQ(av.geometry.aspect_ratio, 4.0f / 3.0f);
        timingChanges++;
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_MESSAGE) { errors++; return true; }
    return false;
}
void RETRO_CALLCONV video(const void* data, unsigned w, unsigned h, size_t pitch) {
    deliveredFrame = data != nullptr;
    if (!canDupe) CHECK_EQ(deliveredFrame, true);
    CHECK_EQ(w > 0 && w <= 1024 && h > 0 && h <= 1024, true);
    CHECK_EQ(pitch, 4096);
    width = w; height = h;
    videos++;
    if (!data) { duplicateFrames++; return; }
    picture.resize((size_t)w * h);
    for (unsigned y = 0; y < h; y++) {
        const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data) + pitch * y);
        std::copy(row, row + w, picture.begin() + (size_t)y * w);
        for (unsigned x = 0; x < w; x++) if (row[x] & 0xffffff) nonblack = true;
    }
}
size_t RETRO_CALLCONV audio(const int16_t* samples, size_t count) {
    size_t used = stallAudio ? 0 : shortAudio ? count / 2 : count;
    sound.insert(sound.end(), samples, samples + used * 2);
    for (size_t i = 0; i < used * 2; i++) if (samples[i]) audible = true;
    return used;
}
void RETRO_CALLCONV audioOne(int16_t l, int16_t r) { sound.push_back(l); sound.push_back(r); }
void RETRO_CALLCONV poll() { polls++; }
int16_t RETRO_CALLCONV input(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (device == RETRO_DEVICE_KEYBOARD) {
        if (realInput) {
            if (id == RETROK_d) return frame >= 720 && frame < 725;
            if (id == RETROK_i) return frame >= 731 && frame < 736;
            if (id == RETROK_r) return frame >= 742 && frame < 747;
            if (id == RETROK_RETURN) return frame >= 753 && frame < 758;
        }
        if (id == RETROK_a) return frame >= 5 && frame < 12;
        if (id == RETROK_LSHIFT) return frame >= 7 && frame < 9;
    }
    if (device == RETRO_DEVICE_JOYPAD) {
        if (!realInput && port == 0) {
            if (id == RETRO_DEVICE_ID_JOYPAD_L) return frame >= 90 && frame < 95;
            if (id == RETRO_DEVICE_ID_JOYPAD_B) return frame == 90;
            if (id == RETRO_DEVICE_ID_JOYPAD_A) return frame == 91;
            if (id == RETRO_DEVICE_ID_JOYPAD_Y) return frame == 92;
            if (id == RETRO_DEVICE_ID_JOYPAD_X) return frame == 93;
            if (id == RETRO_DEVICE_ID_JOYPAD_R || id == RETRO_DEVICE_ID_JOYPAD_SELECT) return frame == 94;
        }
        if (id == RETRO_DEVICE_ID_JOYPAD_UP) return port == 0 && frame % 4 == 0;
        if (id == RETRO_DEVICE_ID_JOYPAD_B) return port == 1 && frame % 3 == 0;
    }
    if (!realInput && port == 0 && device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
        if (frame == 91) return id == RETRO_DEVICE_ID_ANALOG_X ? 32767 : -32768;
        if (frame == 92) return 4096; // デッドゾーン内は動かさない
    }
    if (device == RETRO_DEVICE_MOUSE) {
        if (id == RETRO_DEVICE_ID_MOUSE_X) return (frame % 3) - 1;
        if (id == RETRO_DEVICE_ID_MOUSE_Y) return (frame % 5) - 2;
        if (id == RETRO_DEVICE_ID_MOUSE_LEFT) return frame % 7 == 0;
    }
    return 0;
}
std::vector<uint8_t> read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
}
void put16(std::vector<uint8_t>& data, size_t at, uint16_t value) {
    data[at] = value >> 8; data[at + 1] = value;
}
void put32(std::vector<uint8_t>& data, size_t at, uint32_t value) {
    put16(data, at, value >> 16); put16(data, at + 2, value);
}
void putLittle32(std::vector<uint8_t>& data, size_t at, uint32_t value) {
    for (int i = 0; i < 4; i++) data[at + i] = (uint8_t)(value >> (8 * i));
}
// RAM 上に、パレットと OPM を設定して待つプログラムを置く。
size_t program(uint8_t* ram, uint32_t at, bool mode15) {
    std::vector<uint16_t> code;
    auto move = [&](uint16_t value, uint32_t address, bool byte = false) {
        code.insert(code.end(), {(uint16_t)(byte ? 0x13fc : 0x33fc), value, (uint16_t)(address >> 16), (uint16_t)address});
    };
    move(0x07c0, 0xe82200); // 透明な画面の背景色 (テキストパレット 0)
    auto opm = [&](uint8_t reg, uint8_t value) {
        move(reg, 0xe90001, true); move(value, 0xe90003, true);
    };
    if (!mode15) {
        opm(0x20, 0xc7); opm(0x28, 0x4a);
        for (int s = 0; s < 4; s++) {
            opm(0x40 + s * 8, 1); opm(0x60 + s * 8, s == 3 ? 0 : 127);
            opm(0x80 + s * 8, 31); opm(0xa0 + s * 8, 0); opm(0xc0 + s * 8, 0); opm(0xe0 + s * 8, 15);
        }
        opm(8, 0x40);
    } else {
        move(0x00, 0xe80028); // 15kHz
        move(0x63, 0xe80000); move(0x103, 0xe80008);
        move(0x10, 0xe80004); move(0x50, 0xe80006);
        move(0x10, 0xe8000c); move(0x100, 0xe8000e);
    }
    move(0x1234, 0x1000);
    size_t loop = at + code.size() * 2;
    // MOVE.B $E9A001,$1002 / MOVE.B $E9A003,$1003: 両ポートの入力を RAM に読み続ける。
    code.insert(code.end(), {0x13f9, 0x00e9, 0xa001, 0, 0x1002, 0x13f9, 0x00e9, 0xa003, 0, 0x1003, 0x60ea});
    for (uint16_t word : code) { *ram++ = word >> 8; *ram++ = word; }
    return loop;
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4 && argc != 5) return 1;
    bool real = argc >= 4;
    int realFrames = 1000;
    if (argc == 5) {
        char* end = nullptr;
        long value = std::strtol(argv[4], &end, 10);
        if (*end || value < 1000 || value > 50000) return 1;
        realFrames = (int)value;
    }
    realInput = real;
    if (real && (!fs::exists(fs::path(argv[2]) / "IPLROM.DAT") || !fs::exists(argv[3]))) return 77;
    char temp[] = "/tmp/pix68k-retro-runtime.XXXXXX";
    char* dir = mkdtemp(temp);
    if (!dir) return 1;
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{dir};
    systemDirectory = dir;
    fs::path romDir = fs::path(dir) / "pix68k";
    fs::create_directory(romDir);
    std::vector<uint8_t> ipl(x68k::Machine::IPL_SIZE), cg(x68k::Machine::CG_SIZE), disk(1261568);
    if (real) {
        ipl = read(fs::path(argv[2]) / "IPLROM.DAT");
        cg = read(fs::path(argv[2]) / "CGROM.DAT");
        disk = read(argv[3]);
    } else {
        put32(ipl, 0x10000, 0x10000); put32(ipl, 0x10004, 0xfe0100);
        put16(ipl, 0x100, 0x4ef9); put32(ipl, 0x102, 0x2000); // JMP $2000
    }
    write(romDir / "IPLROM.DAT", ipl); write(romDir / "CGROM.DAT", cg);
    fs::path content = fs::path(dir) / "disk.xdf";
    write(content, disk);
    retro_game_info game = {content.c_str(), nullptr, 0, nullptr};
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    Api api;
#define RESOLVE(name) api.name = reinterpret_cast<decltype(api.name)>(dlsym(library, #name)); if (!api.name) return 1;
    CORE_API(RESOLVE)
#undef RESOLVE
    api.retro_set_environment(environment);
    api.retro_set_video_refresh(video);
    api.retro_set_audio_sample(audioOne);
    api.retro_set_audio_sample_batch(audio);
    api.retro_set_input_poll(poll);
    api.retro_set_input_state(input);
    api.retro_init();
    // フロントエンドが画素形式を拒否した場合、ロードに失敗し、資源を公開しない。
    rejectPixel = true;
    CHECK_EQ(api.retro_load_game(&game), false);
    CHECK_EQ(api.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) == nullptr, true);
    rejectPixel = false;
    std::vector<uint8_t> twoMbState;
    for (int pass = 0; pass < (real ? 2 : 3); pass++) {
        padInputs = pass ? "disabled" : "enabled";
        memory = pass ? "12MB" : "2MB";
        clockOption = pass == 1 ? "16MHz" : "10MHz";
        frameskip = pass == 1 ? "1" : "0";
        renderThread = pass == 1 ? "disabled" : "enabled";
        audioThread = renderThread;
        duplicateFrames = 0;
        timingLogs = 0;
        performanceLog = pass != 0;
        CHECK_EQ(api.retro_load_game(&game), true);
        auto* ram = static_cast<uint8_t*>(api.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
        if (!ram) return 1;
        CHECK_EQ(api.retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM), (pass ? 12 : 2) << 20);
        CHECK_EQ(api.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM), 16384);
        x68k::Config config;
        config.ramSize = (pass ? 12u : 2u) << 20;
        config.setClockMHz(pass == 1 ? 16 : 10);
        auto reference = std::make_unique<x68k::Machine>(config);
#ifdef PIX68K_NO_CPU020
        CHECK_EQ(x68k::Config::has020(), false);
#endif
#ifdef PIX68K_NO_FPU_BOARD
        CHECK_EQ(reference->setFpuBoards(1), false);
        CHECK_EQ(reference->setFpuBoards(0), true);
        CHECK_EQ(reference->fpuBoard() == nullptr, true);
#endif
        reference->setIplRom(ipl.data(), ipl.size()); reference->setCgRom(cg.data(), cg.size());
        std::vector<uint8_t> diskCopy = disk;
        x68k::FloppyImage floppy;
        CHECK_EQ(floppy.load(diskCopy.data(), diskCopy.size(), true, "disk.xdf"), true);
        reference->insertDisk(0, &floppy);
        reference->setTime(1700000000); reference->sound().setOutputRate(48000); reference->powerOn();
        std::vector<uint32_t> target(1024 * 1024);
        x68k::Renderer renderer(reference->video());
        renderer.setTarget(target.data(), 1024, 1024, 1024); reference->setLineListener(&renderer);
        size_t loop = 0;
        if (!real) {
            loop = program(ram + 0x2000, 0x2000, false);
            program(reference->mainRam() + 0x2000, 0x2000, false);
        }
        size_t capacity = api.retro_serialize_size();
        CHECK_EQ(capacity > 0, true);
        auto* sramAddress = api.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        sound.clear(); polls = videos = 0;
        std::vector<int16_t> expected;
        size_t comparedSamples = 0;
        int count = real ? realFrames : 110;
        for (frame = 0; frame < count && !failures; frame++) {
            canDupe = frame < 100;
            if (!real && frame == 40) {
                program(ram + 0x4000, 0x4000, true); program(reference->mainRam() + 0x4000, 0x4000, true);
                const uint8_t jump[] = {0x4e, 0xf9, 0, 0, 0x40, 0};
                std::memcpy(ram + loop, jump, 6); std::memcpy(reference->mainRam() + loop, jump, 6);
            }
            if (!real && frame == 18) {
                std::memset(ram + config.ramSize / 2, 0x5a, config.ramSize / 2);
                std::memset(reference->mainRam() + config.ramSize / 2, 0x5a, config.ramSize / 2);
            }
            shortAudio = frame >= 20 && frame < 30;
            stallAudio = frame >= 55 && frame < 67;
            if (!real && frame == 70) api.retro_set_controller_port_device(1, RETRO_DEVICE_NONE);
            if (!real && frame == 75) api.retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
            if (!real && frame == 95) api.retro_set_controller_port_device(0, RETRO_DEVICE_NONE);
            if (!real && frame == 98) api.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
            if (frame == 80) api.retro_set_audio_sample_batch(nullptr); // サンプル単位の出力にも切り替える
            if (frame == 85) api.retro_set_audio_sample_batch(audio);
            if (real) {
                reference->input().onKey(7, frame >= 720 && frame < 725);
                reference->input().onKey(12, frame >= 731 && frame < 736);
                reference->input().onKey(21, frame >= 742 && frame < 747);
                reference->input().onKey(40, frame >= 753 && frame < 758);
            }
            reference->input().onKey(4, frame >= 5 && frame < 12);
            reference->input().onKey(0xe1, frame >= 7 && frame < 9);
            bool shortcuts = !real && !pass;
            if (!real) {
                reference->input().onKey(0x28, shortcuts && frame == 91);
                reference->input().onKey(0x29, shortcuts && frame == 92);
                reference->input().onKey(0x2c, shortcuts && frame == 90);
                reference->input().onKey(0x3a, shortcuts && frame == 93);
                reference->input().onKey(0x52, shortcuts && frame == 92);
            }
            uint8_t joy0 = frame % 4 == 0 ? platform::InputSink::JOY_UP : 0;
            if (!real && pass && frame == 90) joy0 |= platform::InputSink::JOY_A;
            if (!real && pass && frame == 91) joy0 |= platform::InputSink::JOY_B;
            bool firstPad = real || frame < 95 || frame >= 98;
            if (!firstPad || (shortcuts && frame >= 90 && frame < 95)) joy0 = 0;
            reference->input().onJoystick(0, joy0);
            bool secondPad = real || frame < 70 || frame >= 75;
            reference->input().onJoystick(1, secondPad && frame % 3 == 0 ? platform::InputSink::JOY_A : 0);
            uint8_t mouseButtons = frame % 7 == 0 ? platform::InputSink::MOUSE_LEFT : 0;
            if (shortcuts && frame == 94) mouseButtons |= platform::InputSink::MOUSE_RIGHT;
            reference->input().onMouse(frame % 3 - 1 + (shortcuts && frame == 91 ? 8 : 0),
                frame % 5 - 2 - (shortcuts && frame == 91 ? 8 : 0), mouseButtons);
            reference->runFrame();
            int16_t buffer[2 * 4096]; int n;
            while ((n = reference->sound().ring().drain(buffer, 4096)) > 0) expected.insert(expected.end(), buffer, buffer + n * 2);
            unsigned previousWidth = width, previousHeight = height;
            run(api);
            CHECK_EQ(polls, frame + 1); CHECK_EQ(videos, frame + 1);
            int w, h; renderer.geometry(w, h);
            CHECK_EQ(width, std::max(w, 1)); CHECK_EQ(height, std::max(h, 1));
            // runFrame stops at vertical-display entry, so its surface may
            // contain the next frame's first raster. With video frames skipped,
            // that raster can intentionally come from an older displayed frame.
            // Check all pixels without skipping, and completed raster rows on
            // drawn frames with skipping. A skipped mode change uses a fresh
            // whole-frame snapshot instead of the reference's raster history.
            bool modeChanged = width != previousWidth || height != previousHeight;
            for (unsigned y = pass ? 2 : 0; deliveredFrame && (!pass || (canDupe && !modeChanged)) && y < height && !failures; y++)
                CHECK_EQ(std::memcmp(picture.data() + y * width, target.data() + y * 1024, width * 4), 0);
            CHECK_EQ(sound.size() <= expected.size(), true);
            if (sound.size() <= expected.size()) {
                CHECK_EQ(std::equal(sound.begin() + comparedSamples, sound.end(), expected.begin() + comparedSamples), true);
                comparedSamples = sound.size();
            }
            if (!shortAudio && !stallAudio && frame != 67) CHECK_EQ(sound.size(), expected.size());
            CHECK_EQ(std::memcmp(ram, reference->mainRam(), config.ramSize), 0);
            if (!real) {
                unsigned expectedPad = frame % 4 == 0 ? 0xfe : 0xff;
                if (!firstPad || (shortcuts && frame >= 90 && frame < 95)) expectedPad = 0xff;
                if (pass && frame == 90) expectedPad &= ~0x20u;
                if (pass && frame == 91) expectedPad &= ~0x40u;
                CHECK_EQ(ram[0x1002], expectedPad);
                CHECK_EQ(ram[0x1003], secondPad && frame % 3 == 0 ? 0xdf : 0xff);
            }
            retro_system_av_info av;
            api.retro_get_system_av_info(&av);
            CHECK_EQ(std::abs(av.timing.fps - reference->frameRate()) < 1e-8, true);
            if (frame == 26 || frame == 65 || frame == 91 || frame == 94) {
                CHECK_EQ(api.retro_serialize_size(), capacity); // RAM が埋まっても必要な最大サイズは増えない
                std::vector<uint8_t> saved(capacity), after(capacity);
                CHECK_EQ(api.retro_serialize(saved.data(), saved.size()), true);
                CHECK_EQ(api.retro_serialize(after.data(), after.size()), true);
                CHECK_EQ(saved == after, true);
                size_t accepted = sound.size();
                int previousTimingChanges = timingChanges;
                if (!real && frame == 26) {
                    // 未来側だけ画面モードを変え、復元時の AV 通知が run の外に出ないことを確認する。
                    program(ram + 0x4000, 0x4000, true);
                    const uint8_t jump[] = {0x4e, 0xf9, 0, 0, 0x40, 0};
                    std::memcpy(ram + loop, jump, sizeof jump);
                }
                for (int future = 0; future < 3; future++) run(api);
                if (!real && frame == 26) CHECK_EQ(timingChanges > previousTimingChanges, true);
                CHECK_EQ(api.retro_unserialize(saved.data(), saved.size()), true);
                CHECK_EQ(api.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) == ram, true);
                CHECK_EQ(api.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == sramAddress, true);
                sound.resize(accepted); polls = videos = frame + 1;
                CHECK_EQ(std::memcmp(ram, reference->mainRam(), config.ramSize), 0);
                CHECK_EQ(api.retro_serialize(after.data(), after.size()), true);
                CHECK_EQ(saved == after, true);
                auto rejectState = [&](std::vector<uint8_t>& bad, bool checksum) {
                    if (checksum) {
                        uint32_t used = (uint32_t)bad[12] | (uint32_t)bad[13] << 8 | (uint32_t)bad[14] << 16 | (uint32_t)bad[15] << 24;
                        putLittle32(bad, 16, x68k::SramSlots::crc32(bad.data() + 20, used - 20));
                    }
                    CHECK_EQ(api.retro_unserialize(bad.data(), bad.size()), false);
                    stateErrors++;
                    CHECK_EQ(api.retro_serialize(after.data(), after.size()), true);
                    CHECK_EQ(saved == after, true); // 拒否したデータで現在の CPU / RAM / 入力 / 画面 / 音を変えない
                };
                auto bad = saved; bad[1024] ^= 1; rejectState(bad, false); // 壊れた画面データ
                bad = saved; putLittle32(bad, 24, 0xffffffffu); rejectState(bad, true); // 不正な補間位置
                bad = saved; bad[1024 + 1024 * 1024 * 4 + 8 * x68k::AudioRing::SIZE] ^= 1; rejectState(bad, true); // native state のヘッダ
                if (!pass) twoMbState = saved;
                else {
                    CHECK_EQ(api.retro_unserialize(twoMbState.data(), twoMbState.size()), false);
                    stateErrors++;
                    CHECK_EQ(api.retro_serialize(after.data(), after.size()), true);
                    CHECK_EQ(saved == after, true); // 別の RAM 容量 / クロックのステートも拒否する
                }
            }

        }
        CHECK_EQ(frameskip == "0" ? duplicateFrames == 0 : duplicateFrames > 0, true);
        if (real && pass) CHECK_EQ(timingLogs > 0, true);
        printf("%s %s / %s: %d frames, %zu audio frames, %dx%d, duplicated=%u\n", real ? "Human68k" : "synthetic", memory.c_str(), clockOption.c_str(), frame, sound.size() / 2, width, height, duplicateFrames);
        if (real) {
            char text[x68k::TEXT_ROWS][x68k::TEXT_COLS + 1];
            x68k::readTextScreen(reference->video(), cg.data(), text);
            bool prompt = false, command = false;
            for (const auto& row : text) {
                if (*row) printf("|%s|\n", row);
                if (std::strstr(row, "A>dir")) prompt = true;
                if (std::strstr(row, "COMMAND            X")) command = true;
            }
            CHECK_EQ(prompt, true);
            CHECK_EQ(command, true);
        }
        api.retro_reset(); run(api);
        api.retro_unload_game();
        CHECK_EQ(api.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == nullptr, true);
    }
    CHECK_EQ(nonblack, true);
    if (!real) { CHECK_EQ(audible, true); CHECK_EQ(timingChanges > 0, true); }
    CHECK_EQ(errors, 1 + stateErrors);
    CHECK_EQ(read(content) == disk, true);
    // Disk Control の拡張 API: 初期ディスクの照合、両ドライブ、削除とインデックスの移動。
    if (!diskControl.set_eject_state) return 1;
    fs::path secondDisk = fs::path(dir) / "second disk.xdf", playlist = fs::path(dir) / "set.m3u";
    write(secondDisk, disk);
    const std::string entries = "# two disks\ndisk.xdf\nsecond disk.xdf\n";
    write(playlist, {entries.begin(), entries.end()});
    retro_game_info pair = {playlist.c_str(), nullptr, 0, nullptr};
    CHECK_EQ(diskControl.set_initial_image(1, secondDisk.c_str()), true);
    CHECK_EQ(api.retro_load_game(&pair), true);
    std::vector<uint8_t> pairState(api.retro_serialize_size());
    CHECK_EQ(api.retro_serialize(pairState.data(), pairState.size()), true);
    CHECK_EQ(diskControl.get_num_images(), 2);
    CHECK_EQ(diskControl.get_image_index(), 1);
    CHECK_EQ(diskControl.get_eject_state(), false);
    CHECK_EQ(diskControl.set_image_index(0), false);
    char path[2048], tiny[1] = {'x'};
    CHECK_EQ(diskControl.get_image_path(1, path, sizeof path), true);
    CHECK_EQ(std::strcmp(path, secondDisk.c_str()), 0);
    CHECK_EQ(diskControl.get_image_label(1, path, sizeof path), true);
    CHECK_EQ(std::strcmp(path, "second disk.xdf"), 0);
    CHECK_EQ(diskControl.get_image_path(1, tiny, 1), false);
    CHECK_EQ(tiny[0], 0);
    diskDrive = "FD1";
    CHECK_EQ(diskControl.get_image_index(), 2); // FD0 と同じ媒体を自動で FD1 にも挿さない
    CHECK_EQ(diskControl.set_eject_state(true), true);
    CHECK_EQ(diskControl.set_image_index(0), true);
    CHECK_EQ(diskControl.set_eject_state(false), true);
    diskDrive = "FD0";
    CHECK_EQ(diskControl.set_eject_state(true), true);
    CHECK_EQ(diskControl.set_image_index(0), true);
    CHECK_EQ(diskControl.set_eject_state(false), false); // FD1 に入っている
    CHECK_EQ(diskControl.set_image_index(1), true);
    CHECK_EQ(diskControl.replace_image_index(0, nullptr), false); // FD1 を差したまま削除しない
    diskDrive = "FD1";
    CHECK_EQ(diskControl.set_eject_state(true), true);
    diskDrive = "FD0";
    CHECK_EQ(diskControl.replace_image_index(0, nullptr), true);
    CHECK_EQ(diskControl.get_num_images(), 1);
    CHECK_EQ(diskControl.get_image_index(), 0);
    CHECK_EQ(diskControl.set_eject_state(false), true);
    CHECK_EQ(diskControl.add_image_index(), true);
    CHECK_EQ(diskControl.set_eject_state(true), true);
    CHECK_EQ(diskControl.set_image_index(1), true);
    CHECK_EQ(diskControl.set_eject_state(false), false); // 空き枠にはまだイメージがない
    CHECK_EQ(diskControl.replace_image_index(1, &game), true);
    CHECK_EQ(diskControl.set_eject_state(false), true);
    for (unsigned i = 2; i < 64; i++) CHECK_EQ(diskControl.add_image_index(), true);
    CHECK_EQ(diskControl.add_image_index(), false);
    CHECK_EQ(api.retro_unserialize(pairState.data(), pairState.size()), false);
    stateErrors++; // 画像一覧が異なるステートを読み込まない
    CHECK_EQ(diskControl.set_initial_image(1, "wrong-path.xdf"), true);
    CHECK_EQ(api.retro_load_game(&pair), true);
    CHECK_EQ(diskControl.get_image_index(), 0); // プレイリスト変更などでパスが合わなければ先頭
    CHECK_EQ(api.retro_unserialize(pairState.data(), pairState.size()), true);
    CHECK_EQ(diskControl.get_image_index(), 1); // 同じ一覧なら FD0 / FD1 の挿入状態も戻す
    diskDrive = "FD1";
    CHECK_EQ(diskControl.get_image_index(), 2);
    diskDrive = "FD0";
    if (!real) {
        auto* guest = static_cast<uint8_t*>(api.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
        const uint8_t ejectCode[] = {0x13,0xfc,0,0x21,0,0xe9,0x40,5, 0x13,0xfc,0,0,0,0xe9,0x40,5, 0x60,0xfe};
        std::memcpy(guest + 0x2000, ejectCode, sizeof ejectCode);
        run(api);
        CHECK_EQ(diskControl.get_eject_state(), true); // ゲストからの FD0 排出要求も処理する
        diskDrive = "FD1";
        CHECK_EQ(diskControl.get_eject_state(), false);
        diskDrive = "FD0";
    }
    fs::path hardDisk = fs::path(dir) / "blank.hdf";
    write(hardDisk, std::vector<uint8_t>(256 * 64));
    retro_game_info hd = {hardDisk.c_str(), nullptr, 0, nullptr};
    CHECK_EQ(api.retro_load_game(&hd), true);
    CHECK_EQ(diskControl.get_num_images(), 0);
    api.retro_unload_game();
    // 不正な ROM や FD の後でも、同じライブラリで再ロードできる。
    write(romDir / "CGROM.DAT", {0});
    CHECK_EQ(api.retro_load_game(&game), false);
    CHECK_EQ(api.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) == nullptr, true);
    write(romDir / "CGROM.DAT", cg);
    fs::rename(romDir / "IPLROM.DAT", romDir / "iplrom.dat");
    fs::rename(romDir / "CGROM.DAT", romDir / "cgrom.dat");
    memory = "2MB";
    CHECK_EQ(api.retro_load_game(nullptr), true); // 小文字の ROM と、コンテンツなしの起動
    auto* sram = static_cast<uint8_t*>(api.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    if (!sram) return 1;
    const uint8_t magic[] = {0x82, 0x77, '6', '8', '0', '0', '0', 'W'};
    std::memcpy(sram, magic, sizeof magic);
    sram[8] = 0; sram[9] = 0xc0; sram[10] = sram[11] = 0; // 12MB で保存された SRAM を frontend が復元
    auto* ram = static_cast<uint8_t*>(api.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    if (!real) program(ram + 0x2000, 0x2000, false);
    run(api);
    CHECK_EQ(sram[8], 0); CHECK_EQ(sram[9], 0x20); // 初回 run で今回の 2MB に補正する
    CHECK_EQ(sram[10], 0); CHECK_EQ(sram[11], 0);
    write(content, {0});
    CHECK_EQ(api.retro_load_game(&game), false);
    CHECK_EQ(api.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM), 0);
    retro_game_info noPath = {nullptr, disk.data(), disk.size(), nullptr};
    CHECK_EQ(api.retro_load_game(&noPath), false);
    CHECK_EQ(errors, 4 + stateErrors);
    api.retro_deinit(); dlclose(library);
    return finish();
}
