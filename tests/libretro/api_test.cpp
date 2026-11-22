// 実際の共有ライブラリを読み、RetroArch と同じ C ABI を通して検査する。
// ROM / SDL / RetroArch 本体は不要。
#include "libretro.h"
#include "test_check.h"

#include <dlfcn.h>
#include <cstring>
#include <initializer_list>

#include "core_api.h"

int logs = 0, notifications = 0, callbacks = 0;
bool offerLog = true;

void RETRO_CALLCONV logMessage(enum retro_log_level level, const char*, ...) {
    CHECK_EQ(level, RETRO_LOG_ERROR);
    logs++;
}

bool RETRO_CALLCONV environment(unsigned cmd, void* data) {
    if (cmd == RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME) {
        CHECK_EQ(*static_cast<bool*>(data), true);
        notifications++;
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_GET_LOG_INTERFACE && offerLog) {
        static_cast<retro_log_callback*>(data)->log = logMessage;
        return true;
    }
    return false;
}

void RETRO_CALLCONV video(const void*, unsigned, unsigned, size_t) { callbacks++; }
void RETRO_CALLCONV audio(int16_t, int16_t) { callbacks++; }
size_t RETRO_CALLCONV audioBatch(const int16_t*, size_t frames) {
    callbacks++;
    return frames;
}
void RETRO_CALLCONV poll() { callbacks++; }
int16_t RETRO_CALLCONV input(unsigned, unsigned, unsigned, unsigned) {
    callbacks++;
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    Api api;
#define RESOLVE(name)                                                                   \
    api.name = reinterpret_cast<decltype(api.name)>(dlsym(library, #name));               \
    if (!api.name) {                                                                    \
        fprintf(stderr, "missing export: %s\n", #name);                                 \
        dlclose(library);                                                               \
        return 1;                                                                      \
    }
    CORE_API(RESOLVE)
#undef RESOLVE

    CHECK_EQ(api.retro_api_version(), RETRO_API_VERSION);
    retro_system_info info;
    std::memset(&info, 0xff, sizeof info);
    api.retro_get_system_info(&info);
    CHECK_EQ(std::strcmp(info.library_name, "pix68k"), 0);
    CHECK_EQ(info.library_version != nullptr, true);
    CHECK_EQ(std::strcmp(info.valid_extensions, "xdf|dim|d88|88d|m3u|hdf"), 0);
    CHECK_EQ(info.need_fullpath, true);
    CHECK_EQ(info.block_extract, false);
    retro_system_av_info av;
    std::memset(&av, 0xff, sizeof av);
    api.retro_get_system_av_info(&av);
    CHECK_EQ(av.geometry.base_width > 0 && av.geometry.base_width <= av.geometry.max_width, true);
    CHECK_EQ(av.geometry.base_height > 0 && av.geometry.base_height <= av.geometry.max_height, true);
    CHECK_EQ(av.geometry.max_width, 1024);
    CHECK_EQ(av.geometry.max_height, 1024);
    CHECK_EQ(av.geometry.aspect_ratio > 0, true);
    CHECK_EQ(av.timing.fps > 0, true);
    CHECK_EQ(av.timing.sample_rate, 48000);

    api.retro_set_environment(environment);
    CHECK_EQ(notifications, 1);
    api.retro_set_video_refresh(video);
    api.retro_set_audio_sample(audio);
    api.retro_set_audio_sample_batch(audioBatch);
    api.retro_set_input_poll(poll);
    api.retro_set_input_state(input);

    // 失敗したロードのあと、同一プロセスで init / deinit を繰り返せる。
    for (int i = 0; i < 2; i++) {
        api.retro_init();
        const retro_game_info game = {"missing.xdf", nullptr, 0, nullptr};
        CHECK_EQ(api.retro_load_game(&game), false);
        CHECK_EQ(api.retro_load_game(nullptr), false);
        CHECK_EQ(api.retro_load_game_special(0, &game, 1), false);
        api.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
        api.retro_set_controller_port_device(1, RETRO_DEVICE_NONE);
        api.retro_reset();
        api.retro_run();
        CHECK_EQ(api.retro_serialize_size(), 0);
        uint8_t buffer[32] = {};
        CHECK_EQ(api.retro_serialize(buffer, sizeof buffer), false);
        CHECK_EQ(api.retro_unserialize(buffer, sizeof buffer), false);
        CHECK_EQ(api.retro_serialize(nullptr, 0), false);
        CHECK_EQ(api.retro_unserialize(nullptr, 0), false);
        for (unsigned memory : {RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_SYSTEM_RAM, RETRO_MEMORY_VIDEO_RAM}) {
            CHECK_EQ(api.retro_get_memory_data(memory) == nullptr, true);
            CHECK_EQ(api.retro_get_memory_size(memory), 0);
        }
        api.retro_cheat_reset();
        api.retro_cheat_set(0, true, "invalid");
        CHECK_EQ(api.retro_get_region(), RETRO_REGION_NTSC);
        api.retro_unload_game();
        api.retro_deinit();
    }
    CHECK_EQ(logs, 6);
    CHECK_EQ(callbacks, 0);

    // ログ機能がないフロントエンドで、以前のログ関数を使わない。
    offerLog = false;
    api.retro_init();
    CHECK_EQ(api.retro_load_game(nullptr), false);
    CHECK_EQ(logs, 6);
    api.retro_deinit();
    api.retro_set_environment(nullptr);
    api.retro_set_video_refresh(nullptr);
    api.retro_set_audio_sample(nullptr);
    api.retro_set_audio_sample_batch(nullptr);
    api.retro_set_input_poll(nullptr);
    api.retro_set_input_state(nullptr);
    api.retro_init();
    CHECK_EQ(api.retro_load_game(nullptr), false);
    api.retro_deinit();
    CHECK_EQ(dlclose(library), 0);
    return finish();
}
