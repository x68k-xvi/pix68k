#pragma once
#include "libretro.h"

#define CORE_API(X)                   \
    X(retro_set_environment)          \
    X(retro_set_video_refresh)        \
    X(retro_set_audio_sample)         \
    X(retro_set_audio_sample_batch)   \
    X(retro_set_input_poll)           \
    X(retro_set_input_state)          \
    X(retro_init)                     \
    X(retro_deinit)                   \
    X(retro_api_version)              \
    X(retro_get_system_info)          \
    X(retro_get_system_av_info)       \
    X(retro_set_controller_port_device) \
    X(retro_reset)                    \
    X(retro_run)                      \
    X(retro_load_game)                \
    X(retro_load_game_special)        \
    X(retro_unload_game)              \
    X(retro_get_region)               \
    X(retro_serialize_size)           \
    X(retro_serialize)                \
    X(retro_unserialize)              \
    X(retro_cheat_reset)              \
    X(retro_cheat_set)                \
    X(retro_get_memory_data)          \
    X(retro_get_memory_size)

struct Api {
#define MEMBER(name) decltype(&::name) name = nullptr;
    CORE_API(MEMBER)
#undef MEMBER
};
