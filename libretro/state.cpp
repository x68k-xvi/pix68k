#include "state.h"
#include "x68k/state.h"
#include "x68k/sram_file.h"
#include <cstring>
#include <memory>

namespace pix68k_retro {
namespace {
constexpr size_t header = 1024, pixels = 1024 * 1024, audioBytes = 4 * x68k::AudioRing::SIZE;
constexpr size_t nativeOffset = header + pixels * 4 + audioBytes * 2;
const char magic[] = {'P','I','X','6','8','K','L','R'};
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
uint64_t get64(const uint8_t* p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }
void pcmOut(uint8_t* p, const int16_t* src, size_t frames) {
    for (size_t i = 0; i < frames * 2; i++) { p[i * 2] = (uint8_t)src[i]; p[i * 2 + 1] = (uint8_t)((uint16_t)src[i] >> 8); }
}
void pcmIn(int16_t* out, const uint8_t* p, size_t frames) {
    for (size_t i = 0; i < frames * 2; i++) out[i] = (int16_t)((uint16_t)p[i * 2] | (uint16_t)p[i * 2 + 1] << 8);
}
unsigned mask(const bool* values) { return (values[0] ? 1u : 0u) | (values[1] ? 2u : 0u); }
void mount(x68k::Machine& m, Media& media, const unsigned* selected, unsigned ejected) {
    for (unsigned d = 0; d < 2; d++) {
        if (!(ejected & (1u << d)) && selected[d] < media.floppies.size() && media.floppies[selected[d]])
            m.insertDisk((int)d, &media.floppies[selected[d]]->floppy);
        else m.ejectDisk((int)d);
    }
    if (media.hard) m.attachSasi(0, &media.hard->hard);
}
bool reject(std::string& error, const char* why) { error = why; return false; }
} // namespace

size_t serializeCapacity(StateContext c) { return nativeOffset + x68k::stateSizeBound(c.machine); }

bool serialize(StateContext c, void* buffer, size_t size, std::string& error) {
    size_t capacity = serializeCapacity(c);
    if (!buffer || size < capacity || c.pendingAudio > x68k::AudioRing::SIZE || c.media.floppies.size() > Media::MAX_IMAGES)
        return reject(error, "state buffer is too small");
    auto* out = static_cast<uint8_t*>(buffer);
    std::memset(out, 0, capacity);
    std::memcpy(out, magic, 8);
    put32(out + 8, 1);
    size_t written = 0;
    if (!x68k::saveState(c.machine, out + nativeOffset, capacity - nativeOffset, written))
        return reject(error, "cannot save machine state");
    put32(out + 12, (uint32_t)(nativeOffset + written));
    put32(out + 20, (uint32_t)written);
    put32(out + 24, c.sound().outputPosition());
    put32(out + 28, (uint32_t)c.pendingAudio);
    std::vector<int16_t> ring(x68k::AudioRing::SIZE * 2);
    unsigned count = c.sound().ring().copyPending(ring.data(), x68k::AudioRing::SIZE);
    put32(out + 32, count);
    put32(out + 36, c.flushFrames);
    put32(out + 40, c.firstRun ? 1 : 0);
    put32(out + 44, c.selected[0]); put32(out + 48, c.selected[1]);
    put32(out + 52, mask(c.ejected)); put32(out + 56, mask(c.pendingEject));
    put32(out + 60, (uint32_t)c.media.floppies.size());
    put64(out + 64, c.media.hard ? c.media.hard->identity() : 0);
    for (size_t i = 0; i < c.media.floppies.size(); i++) put64(out + 72 + 8 * i, c.media.floppies[i] ? c.media.floppies[i]->identity() : 0);
    for (size_t i = 0; i < 256; i++) out[584 + i] = c.keys[i] ? 1 : 0;
    put32(out + 840, x68k::SramSlots::crc32(c.machine.cgRom(), x68k::Machine::CG_SIZE));
    for (size_t i = 0; i < pixels; i++) put32(out + header + 4 * i, c.pixels[i]);
    pcmOut(out + header + pixels * 4, c.audio, c.pendingAudio);
    pcmOut(out + header + pixels * 4 + audioBytes, ring.data(), count);
    put32(out + 16, x68k::SramSlots::crc32(out + 20, nativeOffset + written - 20));
    return true;
}

bool unserialize(StateContext c, const void* buffer, size_t size, std::string& error) {
    if (!buffer || size < nativeOffset + x68k::STATE_HEADER_SIZE) return reject(error, "state is too short");
    const auto* in = static_cast<const uint8_t*>(buffer);
    size_t used = get32(in + 12), nativeSize = get32(in + 20);
    if (std::memcmp(in, magic, 8) || get32(in + 8) != 1 || used > size || used < nativeOffset + x68k::STATE_HEADER_SIZE ||
        nativeSize != used - nativeOffset || nativeSize > x68k::stateSizeBound(c.machine)) return reject(error, "invalid libretro state header");
    if (get32(in + 16) != x68k::SramSlots::crc32(in + 20, used - 20)) return reject(error, "state checksum mismatch");
    unsigned phase = get32(in + 24), pending = get32(in + 28), queued = get32(in + 32), flushFrames = get32(in + 36);
    unsigned selected[2] = {get32(in + 44), get32(in + 48)};
    unsigned eject = get32(in + 52), pendingEject = get32(in + 56);
    if (phase >= ((uint64_t)x68k::Sound::OPM_RATE << 16) / 48000 || pending > x68k::AudioRing::SIZE || queued > x68k::AudioRing::SIZE ||
        flushFrames >= 120 || get32(in + 40) > 1 || eject > 3 || pendingEject > 3 || (eject & pendingEject))
        return reject(error, "invalid frontend state values");
    if (get32(in + 60) != c.media.floppies.size() || get64(in + 64) != (c.media.hard ? c.media.hard->identity() : 0))
        return reject(error, "disk list or contents have changed");
    for (size_t i = 0; i < c.media.floppies.size(); i++)
        if (get64(in + 72 + 8 * i) != (c.media.floppies[i] ? c.media.floppies[i]->identity() : 0)) return reject(error, "disk list or contents have changed");
    for (unsigned d = 0; d < 2; d++) {
        if (selected[d] != ~0u && (selected[d] >= c.media.floppies.size() || (!(eject & (1u << d)) && !c.media.floppies[selected[d]])))
            return reject(error, "invalid disk index in state");
    }
    if (!(eject & 1) && !(eject & 2) && selected[0] != ~0u && selected[0] == selected[1]) return reject(error, "same disk in both drives");
    for (size_t i = 0; i < 256; i++) if (in[584 + i] > 1) return reject(error, "invalid key state");
    if (get32(in + 840) != x68k::SramSlots::crc32(c.machine.cgRom(), x68k::Machine::CG_SIZE)) return reject(error, "CGROM differs from state");
    // 違う媒体を入れていた場合も、動作中の Machine を変える前に完全な読み込みを検証する。
    auto candidate = std::make_unique<x68k::Machine>(c.machine.config());
    if (!candidate->ok()) return reject(error, "cannot allocate state validation machine");
    candidate->setIplRom(c.machine.iplRom(), x68k::Machine::IPL_SIZE);
    candidate->setCgRom(c.machine.cgRom(), x68k::Machine::CG_SIZE);
    candidate->sound().setOutputRate(48000);
    mount(*candidate, c.media, selected, eject);
    candidate->powerOn();
    const char* why = nullptr;
    if (!x68k::loadState(*candidate, in + nativeOffset, nativeSize, why)) return reject(error, why ? why : "invalid machine state");
    // SRAM / RAM のポインタは frontend が保持するので、Machine 自体は交換しない。
    std::vector<uint8_t> previous(x68k::stateSize(c.machine));
    size_t previousSize = 0;
    if (!x68k::saveState(c.machine, previous.data(), previous.size(), previousSize)) return reject(error, "cannot preserve current state");
    mount(c.machine, c.media, selected, eject);
    if (!x68k::loadState(c.machine, in + nativeOffset, nativeSize, why)) {
        mount(c.machine, c.media, c.selected, mask(c.ejected));
        const char* ignored = nullptr;
        x68k::loadState(c.machine, previous.data(), previousSize, ignored);
        return reject(error, why ? why : "cannot restore machine state");
    }
    c.sound().restoreOutputPosition(phase);
    auto& ring = c.sound().ring();
    ring.clearStopped();
    std::vector<int16_t> ringData(queued * 2);
    pcmIn(ringData.data(), in + header + pixels * 4 + audioBytes, queued);
    for (unsigned i = 0; i < queued; i++) ring.push(ringData[2 * i], ringData[2 * i + 1]);
    pcmIn(c.audio, in + header + pixels * 4, pending);
    c.pendingAudio = pending;
    for (size_t i = 0; i < pixels; i++) c.pixels[i] = get32(in + header + 4 * i);
    for (size_t i = 0; i < 256; i++) c.keys[i] = in[584 + i] != 0;
    for (unsigned d = 0; d < 2; d++) {
        c.selected[d] = selected[d]; c.ejected[d] = (eject & (1u << d)) != 0; c.pendingEject[d] = (pendingEject & (1u << d)) != 0;
    }
    c.flushFrames = flushFrames;
    c.firstRun = get32(in + 40) != 0;
    return true;
}
} // namespace pix68k_retro
