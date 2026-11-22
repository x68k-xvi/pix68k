// フロッピーディスクのイメージ (XDF / DIM / D88)。
// イメージのバイト列は環境 (ホスト / Pi) が読み込んで渡し、このクラスはそれを指すだけで写さない。
// 書き込みはそのバイト列を直接書き換え、書いた部分に dirty フラグを立てる (dirtyMap()。dirty() はどこかに dirty フラグが立っているか)。
// 環境はその dirty フラグを見て書き戻す (ホスト版は全体を、ベアメタル版は dirty フラグの立ったチャンクだけ。platform/disk_sync.h)。
// どの形式も「トラックごとのセクタの表 (C/H/R/N とデータの位置)」に直して扱う。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "x68k/dirty_map.h"

namespace x68k {

class FloppyImage final : public DirtySource {
public:
    static constexpr int MAX_TRACKS = 168; // 84 シリンダ x 2 ヘッド

    struct Sector {
        uint8_t c, h, r, n;
        uint8_t status;  // D88 の状態 (0 = 正常)。ほかの形式は 0
        bool deleted;    // デリーテッドデータ
        uint32_t offset; // イメージの中のデータの位置
        uint32_t size;   // データのバイト数
    };

    enum Format { NONE, XDF, DIM, D88 };

    FloppyImage() {}
    ~FloppyImage();
    FloppyImage(const FloppyImage&) = delete;
    FloppyImage& operator=(const FloppyImage&) = delete;

    // イメージを読む。形式は大きさとヘッダで判断する。読めなければ false
    // name は拡張子の判断にだけ使う (nullptr でよい)
    bool load(uint8_t* data, size_t size, bool writeProtect, const char* name = nullptr);
    void unload();

    bool loaded() const { return format_ != NONE; }
    Format format() const { return format_; }
    bool writeProtected() const { return writeProtect_; }
    bool dirty() const { return dirty_.any(); }
    uint32_t writeCount() const { return dirty_.writes(); }
    void clearDirty() { dirty_.clearAll(); }

    // トラック (シリンダ x 2 + ヘッド) のセクタの数と、i 番目のセクタ (回転の順)
    int sectorCount(int track) const;
    const Sector* sector(int track, int i) const;
    // ID が一致するセクタを探す。なければ nullptr
    const Sector* find(int track, uint8_t c, uint8_t h, uint8_t r, uint8_t n) const;
    // セクタのデータ
    uint8_t* data(const Sector* s) { return data_ + s->offset; }
    const uint8_t* data(const Sector* s) const { return data_ + s->offset; }
    // セクタ s のデータ (と D88 のセクタのヘッダ) を書いた
    void markDirty(const Sector* s);
    // WRITE DATA / WRITE DELETED DATA のあと: デリーテッドデータかどうかを書き、CRC エラーなどの状態を消す
    void setMark(const Sector* s, bool deleted);
    // セクタの表の中の番号と、その逆 (状態の保存と復元で使う。範囲外なら -1 / nullptr)
    int sectorIndex(const Sector* s) const { return s && s >= sectors_ && s < sectors_ + used_ ? (int)(s - sectors_) : -1; }
    const Sector* sectorAt(int i) const { return i >= 0 && i < used_ ? sectors_ + i : nullptr; }
    size_t size() const { return size_; }

    // ---- 書き戻し (DirtySource) ----
    DirtyMap& dirtyMap() override { return dirty_; }
    const uint8_t* chunkData(uint32_t c, uint32_t& len) override {
        len = dirty_.chunkBytes(c);
        return len ? data_ + ((size_t)c << DirtyMap::CHUNK_SHIFT) : nullptr;
    }

private:
    bool loadXdf();
    bool loadDim();
    bool loadD88();
    bool allocate(int sectors);
    void addTrack(int track, int first, int count);

    Format format_ = NONE;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool writeProtect_ = false;
    DirtyMap dirty_;
    Sector* sectors_ = nullptr; // 全トラックのセクタの表
    int capacity_ = 0, used_ = 0;
    int16_t trackFirst_[MAX_TRACKS] = {};
    uint8_t trackCount_[MAX_TRACKS] = {};
};

} // namespace x68k
