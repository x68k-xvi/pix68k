#pragma once
#include <cstdint>
#include <memory>
#include <string>
namespace x68k { class Machine; class IndexedLineSink; }
namespace pix68k_retro {
// All public methods run on the libretro execution thread. Each frame is
// flushed before its surface is read, saved, restored, or retargeted.
class RenderWorker {
public:
    static std::unique_ptr<RenderWorker> create(x68k::Machine&, uint32_t*, std::string&);
    ~RenderWorker();
    void flush();
    void resync();
    void prepareFrame(bool render, bool measure);
    uint64_t rasterMicros() const;
    void setIndexedLineSink(x68k::IndexedLineSink*);
private:
    struct Impl;
    explicit RenderWorker(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
