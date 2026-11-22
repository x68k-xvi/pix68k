#include "media.h"
#include "x68k/sasi.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <unistd.h>

namespace pix68k_retro {
namespace fs = std::filesystem;
namespace {
constexpr size_t maxFloppy = 8u << 20;
#ifdef PIX68K_VITA
constexpr size_t maxHard = 64u << 20;
constexpr size_t maxFloppiesTotal = 32u << 20;
#else
constexpr size_t maxHard = 256u << 20;
constexpr size_t maxFloppiesTotal = 128u << 20;
#endif

bool atomicWrite(const std::string& path, const std::vector<uint8_t>& data) {
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    int fd = mkstemp(name.data());
    if (fd < 0) return false;
    FILE* f = fdopen(fd, "wb");
    if (!f) { close(fd); std::remove(name.data()); return false; }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (ok) ok = std::rename(name.data(), path.c_str()) == 0;
    if (!ok) std::remove(name.data());
    return ok;
}
uint64_t imageHash(const std::string& path, const std::vector<uint8_t>& data) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char b : path) { hash ^= b; hash *= 1099511628211ull; }
    hash ^= 0; hash *= 1099511628211ull;
    for (uint8_t b : data) { hash ^= b; hash *= 1099511628211ull; }
    return hash;
}
std::string copyName(const std::string& path, const std::vector<uint8_t>& data) {
    uint64_t hash = imageHash(path, data);
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)hash);
    auto ext = extension(path);
    if (ext != ".xdf" && ext != ".dim" && ext != ".d88" && ext != ".88d" && ext != ".hdf") ext = ".img";
    return std::string(hex) + ext + ".pxm";
}
} // namespace

bool readFile(const std::string& path, std::vector<uint8_t>& data, size_t limit) {
    data.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    long size = ok ? std::ftell(f) : -1;
    ok = size > 0 && (size_t)size <= limit && std::fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        data.resize((size_t)size);
        ok = std::fread(data.data(), 1, data.size(), f) == data.size() && std::fgetc(f) == EOF && !std::ferror(f);
    }
    if (std::fclose(f) != 0) ok = false;
    if (!ok) data.clear();
    return ok;
}
namespace {
bool mountedPath(const std::string& path) {
#ifdef PIX68K_VITA
    const auto colon = path.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; i++)
        if (!std::isalnum((unsigned char)path[i]) && path[i] != '_') return false;
    return true;
#else
    (void)path;
    return false;
#endif
}
}
std::string absolutePath(const std::string& path) {
    if (mountedPath(path)) {
        const auto colon = path.find(':');
        auto tail = path.substr(colon + 1);
        if (tail.empty() || tail.front() != '/') tail.insert(0, "/");
        return path.substr(0, colon + 1) + fs::path(tail).lexically_normal().generic_string();
    }
    std::error_code ec;
    auto p = fs::absolute(fs::path(path), ec);
    return ec ? std::string() : p.lexically_normal().string();
}
std::string extension(const std::string& path) {
    auto ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    return ext;
}

bool MediaImage::flush(std::string& error) {
    if (!dirty() || savePath.empty()) return true;
    flushFailed_ = !atomicWrite(savePath, data);
    if (flushFailed_) {
        error = "cannot save disk copy: " + savePath;
        return false;
    }
    if (hardDisk) hard.clearDirty();
    else floppy.clearDirty();
    return true;
}

uint64_t MediaImage::identity() const {
    const auto writes = writeCount();
    if (!identityValid_ || identityWrites_ != writes) {
        cachedIdentity_ = imageHash(path, data);
        identityWrites_ = writes;
        identityValid_ = true;
    }
    return cachedIdentity_;
}

bool MediaImage::flushIdle(std::string& error) {
    const auto writes = writeCount();
    if (observedWrites_ != writes) {
        observedWrites_ = writes;
        idleFrames_ = 0;
    }
    if (!dirty() || savePath.empty()) return true;
    // 書き込みが続く間は全イメージの同期保存を避ける。失敗時も間隔を空けて再試行する。
    if (++idleFrames_ < 120) {
        if (flushFailed_) error = "cannot save disk copy: " + savePath;
        return !flushFailed_;
    }
    idleFrames_ = 0;
    return flush(error);
}

std::unique_ptr<MediaImage> Media::loadImage(const std::string& name, bool hdf, std::string& error) {
    auto image = std::make_unique<MediaImage>();
    image->path = absolutePath(name);
    image->hardDisk = hdf;
    if (image->path.empty() || !readFile(image->path, image->data, hdf ? maxHard : maxFloppy)) {
        error = "cannot read disk image (or size limit exceeded): " + name;
        return nullptr;
    }
    if (writable) {
        if (saveDirectory.empty()) { error = "frontend save directory is unavailable"; return nullptr; }
        std::error_code ec;
        fs::path dir = fs::path(saveDirectory) / "pix68k-media";
        fs::create_directories(dir, ec);
        if (ec) { error = "cannot create disk save directory: " + dir.string(); return nullptr; }
        image->savePath = (dir / copyName(image->path, image->data)).string();
        bool exists = fs::exists(image->savePath, ec);
        if (ec) { error = "cannot inspect disk save copy: " + image->savePath; return nullptr; }
        if (exists) {
            const auto originalSize = image->data.size();
            // No mounted image references this buffer yet. Reuse it rather than
            // keeping two full HDF images resident while loading the save copy.
            if (!readFile(image->savePath, image->data, originalSize) || image->data.size() != originalSize) {
                error = "invalid disk save copy: " + image->savePath;
                return nullptr;
            }
        }
    }
    bool ok = hdf ? image->data.size() % x68k::Sasi::BLOCK_SIZE == 0 &&
                   image->hard.load(image->data.data(), image->data.size(), x68k::Sasi::BLOCK_SIZE, !writable)
                 : image->floppy.load(image->data.data(), image->data.size(), !writable, name.c_str());
    if (!ok) { error = "invalid disk image: " + name; return nullptr; }
    return image;
}
std::unique_ptr<MediaImage> Media::loadFloppy(const std::string& name, std::string& error, const MediaImage* replacing) {
    size_t total = 0;
    for (const auto& image : floppies) if (image && image.get() != replacing) total += image->data.size();
    auto image = loadImage(name, false, error);
    if (!image) return nullptr;
    if (total + image->data.size() > maxFloppiesTotal) { error = "floppy image memory limit exceeded"; return nullptr; }
    if (writable) {
        for (const auto& old : floppies) if (old && old.get() != replacing && old->savePath == image->savePath) {
            error = "duplicate writable floppy image: " + name;
            return nullptr;
        }
    }
    return image;
}

bool Media::load(const std::string& content, std::string& error) {
    if (extension(content) == ".hdf") {
        hard = loadImage(content, true, error);
        return hard != nullptr;
    }
    std::vector<std::string> paths;
    if (extension(content) == ".m3u") {
        std::vector<uint8_t> data;
        if (!readFile(content, data, 65536)) { error = "cannot read M3U playlist: " + content; return false; }
        std::string text(data.begin(), data.end());
        if (text.find('\0') != std::string::npos) { error = "invalid M3U playlist"; return false; }
        if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            auto line = text.substr(pos, end - pos);
            pos = end + 1;
            size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
            if (a == std::string::npos || line[a] == '#') continue;
            line = line.substr(a, b - a + 1);
            if (paths.size() >= MAX_IMAGES) { error = "too many M3U images (maximum 64)"; return false; }
            auto path = fs::path(line);
            if (path.is_relative() && !mountedPath(line)) path = fs::path(absolutePath(content)).parent_path() / path;
            paths.push_back(absolutePath(path.string()));
        }
        if (paths.empty()) { error = "empty M3U playlist"; return false; }
    } else paths.push_back(content);
    for (const auto& path : paths) {
        auto image = loadFloppy(path, error);
        if (!image) return false;
        floppies.push_back(std::move(image));
    }
    return true;
}
bool Media::flushIdle(std::string& error) {
    bool ok = true;
    for (auto& image : floppies) if (image && !image->flushIdle(error)) ok = false;
    if (hard && !hard->flushIdle(error)) ok = false;
    return ok;
}
bool Media::flush(std::string& error) {
    bool ok = true;
    for (auto& image : floppies) if (image && !image->flush(error)) ok = false;
    if (hard && !hard->flush(error)) ok = false;
    return ok;
}
} // namespace pix68k_retro
