// ハードディスクのイメージ (SASI の HDF、SCSI の HDS、CD-ROM の ISO)。
// 環境が読み込んだバイト列を、ブロックの大きさ (SASI は 256、SCSI は 512 など) と一緒に預かる。バイト列は環境が持つ。
// 書き込みはそのバイト列を直接書き換え、書いた部分に dirty フラグを立てる (dirtyMap()。dirty() はどこかに dirty フラグが立っているか)。
// 環境はその dirty フラグを見て書き戻す (ホスト版は全体を、ベアメタル版は dirty フラグの立ったチャンクだけ。platform/disk_sync.h)。
//
// イメージは 2 通りの持ち方がある:
//   - load(): 全体がメモリにある。window() はいつでも残り全部を返す
//   - loadCached(): 大きなイメージ。使うチャンクだけを DiskCache に読む (x68k/disk_cache.h)。
//     window() はチャンクの終わりまでを返し、読み込み中なら nullptr を返す (コントローラは待つ)。block() は使えない
// コントローラ (SASI、SPC) は block() でなく window() (DiskCursor) でデータを受け渡す。
// 例外、RTTI、STL は使わない (ベアメタルでも使う)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "x68k/dirty_map.h"
#include "x68k/disk_cache.h"

namespace x68k {

class HardDiskImage final : public DirtySource {
public:
    // 全体がメモリにあるイメージを預かる。読み直したり外したりしたら、Machine::attachSasi() / attachScsi() でつなぎ直す
    // (コントローラは転送中の位置を持っているので、つないだまま読み直すと古い位置に書く)。
    // data は size バイト。ブロックの大きさで割り切れない端数は使わない。1 ブロックに満たなければ false
    bool load(uint8_t* data, size_t size, uint32_t blockSize, bool writeProtect = false) {
        unload(); // 前のイメージを外す
        if (!data || blockSize == 0 || size / blockSize == 0 || size / blockSize > 0xffffffffull) return false;

        const uint32_t blocks = (uint32_t)(size / blockSize);
        if (!dirty_.init((uint64_t)blocks * blockSize)) return false;
        whole_ = data;
        blockBytes_ = blockSize;
        blockCount_ = blocks;
        readOnly_ = writeProtect;
        return true;
    }
    // 大きなイメージ (bytes バイト) を、cache (同じ大きさで init() したもの) を通して読み書きする。
    // cache は外すまで環境が持つ
    bool loadCached(DiskCache* cache, uint64_t bytes, uint32_t blockSize, bool writeProtect) {
        unload();
        if (!cache || blockSize == 0 || bytes / blockSize == 0 || bytes / blockSize > 0xffffffffull) return false;
        const uint32_t blocks = (uint32_t)(bytes / blockSize);
        if (!dirty_.init((uint64_t)blocks * blockSize)) return false;
        cache_ = cache;
        cache_->setDirtyMap(&dirty_);
        blockBytes_ = blockSize;
        blockCount_ = blocks;
        readOnly_ = writeProtect;
        return true;
    }
    void unload() { // 外す (バイト列は環境が持つので、ここでは解放しない)
        whole_ = nullptr;
        cache_ = nullptr;
        blockBytes_ = blockCount_ = 0;
        dirty_.clearAll();
        readOnly_ = false;
    }
    bool loaded() const { return whole_ != nullptr || cache_ != nullptr; }
    bool cached() const { return cache_ != nullptr; }
    DiskCache* cache() const { return cache_; }
    uint32_t blockSize() const { return blockBytes_; } // 1 ブロックのバイト数
    uint32_t blocks() const { return blockCount_; }     // ブロック数
    uint64_t bytes() const { return (uint64_t)blockCount_ * blockBytes_; }
    bool writeProtected() const { return readOnly_; }
    bool dirty() const { return dirty_.any(); }
    uint32_t writeCount() const { return dirty_.writes(); }
    void clearDirty() { dirty_.clearAll(); }
    // イメージの off バイト目 / [off, off + len) を書いた
    void markDirty(uint64_t off) { dirty_.mark(off); }
    void markDirtyRange(uint64_t off, uint64_t len) { dirty_.markRange(off, len); }
    // ブロックの先頭。範囲の外か、全体がメモリにないイメージなら nullptr
    uint8_t* block(uint32_t lba) { return whole_ && lba < blockCount_ ? whole_ + (size_t)lba * blockBytes_ : nullptr; }

    // [off, off + want) のうち、続けて読み書きできる先頭の部分を返す (got にバイト数)。
    // 範囲の外なら nullptr (failed は false)
    uint8_t* window(uint64_t off, uint32_t want, uint32_t& got, bool& failed) {
        failed = false;
        got = 0;
        const uint64_t size = bytes();
        if (off >= size || want == 0) return nullptr;
        if (whole_) {
            got = size - off < want ? (uint32_t)(size - off) : want;
            return whole_ + off;
        }
        if (!cache_) return nullptr;
        const uint32_t c = (uint32_t)(off >> DirtyMap::CHUNK_SHIFT);
        uint8_t* p = cache_->lookup(c, true, failed);
        if (!p) return nullptr;
        const uint32_t in = (uint32_t)(off & (DirtyMap::CHUNK - 1));
        const uint32_t left = dirty_.chunkBytes(c) - in;
        got = left < want ? left : want;
        // 続けて読むときのために、次のチャンクを先に頼む
        if (got == left && want > left) cache_->prefetch(c + 1);
        return p + in;
    }
    // [off, off + len) をこれから読み書きする (先に読み込んでおけるなら読み込む)。
    // 大きなイメージでは、範囲のチャンク (枠の半分まで) と、その後ろの 2 チャンクを頼む
    void prefetch(uint64_t off, uint64_t len) {
        if (!cache_ || len == 0 || off >= bytes()) return;
        const uint32_t first = (uint32_t)(off >> DirtyMap::CHUNK_SHIFT);
        uint64_t last = (off + len - 1) >> DirtyMap::CHUNK_SHIFT;
        const uint64_t cap = cache_->slots() / 2;
        if (last - first + 1 > cap) last = first + cap - 1;
        for (uint64_t c = first; c <= last + 2 && c < dirty_.chunks(); c++) cache_->prefetch((uint32_t)c);
    }

    // ---- 書き戻し (DirtySource) ----
    DirtyMap& dirtyMap() override { return dirty_; }
    const uint8_t* chunkData(uint32_t c, uint32_t& len) override {
        len = dirty_.chunkBytes(c);
        if (!len) return nullptr;
        if (whole_) return whole_ + ((size_t)c << DirtyMap::CHUNK_SHIFT);
        return cache_ ? cache_->peek(c) : nullptr; // dirty フラグの立ったチャンクは空けないので、読み込めている
    }
    void chunkInFlight(uint32_t c) override {
        if (cache_) cache_->setInFlight(c, true);
    }
    void chunkWritten(uint32_t c) override {
        if (cache_) cache_->setInFlight(c, false);
    }

private:
    uint8_t* whole_ = nullptr; // 全体がメモリにあるイメージ (load())
    DiskCache* cache_ = nullptr;
    uint32_t blockBytes_ = 0;
    uint32_t blockCount_ = 0;
    bool readOnly_ = false; // 書き込み禁止
    DirtyMap dirty_;
};

// コントローラ (SASI、SPC) がイメージとデータを受け渡すときの位置。転送の始まり (イメージの base バイト目) から
// len バイトを、pos (転送の中の位置) で読み書きする。読み込みを待つ間は ready() が false を返す
// (コントローラは REQ を出さず、DMAC も待つ)。
// stride が 0 でなければ、イメージの中でセクタが stride バイトごとに並び、そのうちの payload バイトだけを順に
// 受け渡す (CD-ROM の 2352 バイト / セクタのイメージからユーザデータの 2048 バイトを読むとき。base は最初のセクタの
// ユーザデータの位置)
class DiskCursor {
public:
    void start(HardDiskImage* img, uint64_t base, uint32_t len, uint32_t stride = 0, uint32_t payload = 0) {
        img_ = img;
        base_ = base;
        len_ = len;
        stride_ = (uint16_t)(payload ? stride : 0);
        payload_ = (uint16_t)payload;
        win_ = nullptr;
        winStart_ = winEnd_ = 0;
        failed_ = false;
        if (img) img->prefetch(base, stride_ ? (uint64_t)((len + payload - 1) / payload) * stride_ : len);
    }
    void stop() {
        img_ = nullptr;
        stride_ = payload_ = 0;
        win_ = nullptr;
        winStart_ = winEnd_ = 0;
        failed_ = false;
    }
    bool active() const { return img_ != nullptr; }
    uint64_t base() const { return base_; }
    uint32_t stride() const { return stride_; }
    uint32_t payload() const { return payload_; }
    // 転送の pos バイト目を読み書きできるか
    bool ready(uint32_t pos) {
        if (pos >= winStart_ && pos < winEnd_) return true;
        return refill(pos);
    }
    // pos から続けて読み書きできるバイト数 (ready(pos) のあと)
    uint32_t avail(uint32_t pos) const { return pos >= winStart_ && pos < winEnd_ ? winEnd_ - pos : 0; }
    // ready(pos) のあとに読み書きする
    uint8_t& at(uint32_t pos) { return win_[pos - winStart_]; }
    // イメージから読めなかった (コントローラはエラーにする)
    bool failed() const { return failed_; }

private:
    bool refill(uint32_t pos) {
        if (!img_ || pos >= len_) return false;
        uint32_t got = 0;
        bool failed = false;
        uint64_t off = base_ + pos;
        uint32_t want = len_ - pos;
        if (stride_) { // セクタの中のユーザデータだけを、セクタごとに
            const uint32_t in = pos % payload_;
            off = base_ + (uint64_t)(pos / payload_) * stride_ + in;
            if (want > payload_ - in) want = payload_ - in;
            img_->prefetch(off, want); // 続きのチャンクを先に頼む (window() の先読みは、セクタごとの範囲では働かない)
        }
        uint8_t* p = img_->window(off, want, got, failed);
        if (!p) {
            failed_ = failed;
            return false;
        }
        win_ = p;
        winStart_ = pos;
        winEnd_ = pos + got;
        return true;
    }

    HardDiskImage* img_ = nullptr;
    uint64_t base_ = 0;
    uint32_t len_ = 0;
    uint16_t stride_ = 0, payload_ = 0; // len_ の後ろの隙間に置く (DiskCursor の大きさを変えない)
    uint8_t* win_ = nullptr;
    uint32_t winStart_ = 0, winEnd_ = 0;
    bool failed_ = false;
};

} // namespace x68k
