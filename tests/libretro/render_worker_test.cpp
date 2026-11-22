// Exercise notifications after both long idle gaps and rapid publication.
#include "render_worker.h"
#include "test_check.h"
#include "x68k/machine.h"
#include "x68k/render.h"
#include <memory>
#include <vector>
#include <unistd.h>
int main() {
    x68k::Config config;
    config.ramSize = 2u << 20;
    auto machine = std::make_unique<x68k::Machine>(config);
    CHECK_EQ(machine->ok(), true);
    std::vector<uint32_t> pixels(x68k::Renderer::MAX_WIDTH * x68k::Renderer::MAX_HEIGHT);
    for (unsigned pass = 0; pass < 2; pass++) {
        std::string error;
        auto worker = pix68k_retro::RenderWorker::create(*machine, pixels.data(), error);
        CHECK_EQ(worker != nullptr, true);
        if (!worker) return 1;
        for (unsigned i = 0; i < 50000; i++) {
            if ((i & 255) == 0) usleep(2000);
            machine->video().spriteWrite(0x8000 + (i & 127), (uint8_t)i);
            worker->flush();
        }
        // Also wake a blocked reader during teardown, then recreate it.
        usleep(2000);
    }
    return finish();
}
