#pragma once
#include <memory>
#include <string>
namespace x68k { class Machine; class Sound; }
namespace pix68k_retro {
// Public methods run on the libretro execution thread. flush() completes all
// audio events; pause()/resume() bracket native state loading while no producer
// can publish new events. The consumer sleeps when emulation is paused.
class AudioWorker {
public:
    static std::unique_ptr<AudioWorker> create(x68k::Machine&, std::string& error);
    ~AudioWorker();
    void flush();
    void snapshotToMachine();
    void pause();
    void resume();
    x68k::Sound& sound();
private:
    struct Impl;
    explicit AudioWorker(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
