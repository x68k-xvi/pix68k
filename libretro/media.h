#pragma once

#include "x68k/fdd.h"
#include "x68k/hdd.h"
#include <memory>
#include <string>
#include <vector>

namespace pix68k_retro {
bool readFile(const std::string& path, std::vector<uint8_t>& data, size_t limit);
std::string absolutePath(const std::string& path);
std::string extension(const std::string& path);

// 元のイメージは rb で読むだけ。書込みを許可した場合も save directory のコピーにだけ保存する。
struct MediaImage {
    std::string path, savePath;
    std::vector<uint8_t> data;
    x68k::FloppyImage floppy;
    x68k::HardDiskImage hard;
    bool hardDisk = false;
    bool dirty() const { return hardDisk ? hard.dirty() : floppy.dirty(); }
    uint32_t writeCount() const { return hardDisk ? hard.writeCount() : floppy.writeCount(); }
    uint64_t identity() const;
    bool flushIdle(std::string& error);
    bool flush(std::string& error);
private:
    mutable uint64_t cachedIdentity_ = 0;
    mutable uint32_t identityWrites_ = 0;
    mutable bool identityValid_ = false;
    uint32_t observedWrites_ = 0;
    unsigned idleFrames_ = 0;
    bool flushFailed_ = false;
};

class Media {
public:
    static constexpr size_t MAX_IMAGES = 64;
    bool writable = false;
    std::string saveDirectory;
    std::vector<std::unique_ptr<MediaImage>> floppies;
    std::unique_ptr<MediaImage> hard;

    bool load(const std::string& content, std::string& error);
    std::unique_ptr<MediaImage> loadFloppy(const std::string& path, std::string& error, const MediaImage* replacing = nullptr);
    bool flush(std::string& error);
    bool flushIdle(std::string& error);
private:
    std::unique_ptr<MediaImage> loadImage(const std::string& path, bool hdf, std::string& error);
};
} // namespace pix68k_retro
