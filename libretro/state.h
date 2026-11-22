#pragma once

#include "media.h"
#include "x68k/machine.h"

namespace pix68k_retro {
// Machine とフロントエンドが持つ状態を一緒に保存する。呼出し中は CPU / 入出力を止める。
struct StateContext {
    x68k::Machine& machine;
    Media& media;
    uint32_t* pixels;
    int16_t* audio;
    size_t& pendingAudio;
    bool* keys;
    unsigned* selected;
    bool* ejected;
    bool* pendingEject;
    unsigned& flushFrames;
    bool& firstRun;
    x68k::Sound* output = nullptr;
    x68k::Sound& sound() const { return output ? *output : machine.sound(); }
};
size_t serializeCapacity(StateContext c);
bool serialize(StateContext c, void* out, size_t size, std::string& error);
bool unserialize(StateContext c, const void* data, size_t size, std::string& error);
} // namespace pix68k_retro
