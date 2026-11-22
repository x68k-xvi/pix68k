// ベアメタル版の FatFs の設定 (tools/ffconf_patch.sh) で、日本語のファイル名を UTF-8 で読み書きできるか (#275)。
// Circle の FatFs (external/circle/addon/fatfs) をホストで動かし、メモリの上の FAT のディスクで確かめる:
//   長い名前 (UTF-8 で 256 バイト) を短い名前に戻さずに返す。長い名前のない CP932 の 8.3 の名前を UTF-8 で返し、
//   その UTF-8 の名前で開ける。95 / 96 バイトの名前も FatFs は返し、設定メニューの一覧 (MenuFileList) は 95 バイトまでを出す
#include <stdlib.h>
#include <string.h>

#include "ff.h" // diskio.h より先に (diskio.h は ff.h の型を使う)
#include "diskio.h"
#include "platform/menu.h"
#include "test_check.h"

namespace {
constexpr int kSectors = 8192; // 4MB (FAT16)
BYTE g_disk[kSectors * 512];
} // namespace

extern "C" {
DSTATUS disk_initialize(BYTE) { return 0; }
DSTATUS disk_status(BYTE) { return 0; }
DRESULT disk_read(BYTE, BYTE* buf, LBA_t sector, UINT count) {
    memcpy(buf, g_disk + sector * 512, count * 512);
    return RES_OK;
}
DRESULT disk_write(BYTE, const BYTE* buf, LBA_t sector, UINT count) {
    memcpy(g_disk + sector * 512, buf, count * 512);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE, BYTE cmd, void* buf) {
    if (cmd == GET_SECTOR_COUNT) *(LBA_t*)buf = kSectors;
    if (cmd == GET_SECTOR_SIZE) *(WORD*)buf = 512;
    if (cmd == GET_BLOCK_SIZE) *(DWORD*)buf = 1;
    return RES_OK;
}
void* ff_memalloc(UINT n) { return malloc(n); }
void ff_memfree(void* p) { free(p); }
}

namespace {
bool create(const char* path) {
    FIL f;
    if (f_open(&f, path, FA_CREATE_NEW | FA_WRITE) != FR_OK) return false;
    return f_close(&f) == FR_OK;
}

// ルートのディレクトリに name があるか (FILINFO の fname で比べる)
bool listed(const char* name) {
    DIR d;
    FILINFO fi;
    bool found = false;
    if (f_opendir(&d, "0:/") != FR_OK) return false;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] != 0) found = found || strcmp(fi.fname, name) == 0;
    f_closedir(&d);
    return found;
}
} // namespace

int main() {
    static BYTE work[4096];
    const MKFS_PARM opt = {FM_FAT, 0, 0, 0, 0};
    CHECK_EQ(f_mkfs("0:", &opt, work, sizeof work), FR_OK);
    static FATFS fs;
    CHECK_EQ(f_mount(&fs, "0:", 1), FR_OK);

    // 長い名前: 「あ」84 字と .XDF で 256 バイト (FF_LFN_BUF が 255 のままだと返せない)
    static char longName[300] = "";
    for (int i = 0; i < 84; i++) strcat(longName, "あ");
    strcat(longName, ".XDF");
    CHECK_EQ(strlen(longName), 256u);
    char path[320] = "0:/";
    strcat(path, longName);
    CHECK_EQ(create(path), 1);
    CHECK_EQ(listed(longName), 1);
    // 短い日本語の名前 (長い名前も付く)
    CHECK_EQ(create("0:/日本.XDF"), 1);
    CHECK_EQ(listed("日本.XDF"), 1);

    // 95 / 96 バイトの名前 (「あ」30 字と a と .XDF、それに a をもう 1 つ)。FatFs はどちらも返す。一覧には 95 バイトだけを出す
    char n95[100] = "", n96[100] = "";
    for (int i = 0; i < 30; i++) strcat(n95, "あ");
    strcat(n95, "a.XDF");
    for (int i = 0; i < 30; i++) strcat(n96, "あ");
    strcat(n96, "aa.XDF");
    CHECK_EQ(strlen(n95), 95u);
    CHECK_EQ(strlen(n96), 96u);
    const char* const edge[] = {n95, n96};
    for (const char* n : edge) {
        char p[120] = "0:/";
        strcat(p, n);
        CHECK_EQ(create(p), 1);
        CHECK_EQ(listed(n), 1);
    }
    {
        platform::MenuFileList list;
        DIR d;
        FILINFO fi;
        CHECK_EQ(f_opendir(&d, "0:/"), FR_OK);
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] != 0) list.add(fi.fname);
        f_closedir(&d);
        bool has95 = false, has96 = false;
        for (int i = 0; i < list.count(); i++) {
            has95 = has95 || strcmp(list.name(i), n95) == 0;
            has96 = has96 || strcmp(list.name(i), n96) == 0;
        }
        CHECK_EQ(has95, 1);
        CHECK_EQ(has96, 0);
    }

    // 長い名前のない 8.3 の名前: FatFs は ASCII でない名前には必ず長い名前を付けるので、ASCII の名前で作ってから、
    // ディレクトリエントリの名前を CP932 に書き換える
    static const struct {
        const char* ascii; // 作るときの 8.3 の名前 (ディレクトリエントリの 11 バイト)
        const char* cp932; // 書き換える 11 バイト
        const char* utf8;  // 読めるはずの名前
    } kSfn[] = {
        {"A       XDF", "\x93\xfa\x96\x7b\x8c\xea\x96\xbc" "XDF", "日本語名.XDF"},        // UTF-8 で 16 バイト
        {"B       XDF", "\x83\x4f\x83\x89" "2\x96\xca XDF", "グラ2面.XDF"},                 // 全角と ASCII
        {"C       XDF", "\xb8\xde\xd7\xc3\xde\xa8\xb3\xbd" "XDF", "ｸﾞﾗﾃﾞｨｳｽ.XDF"},     // 半角カタカナ (UTF-8 で 28 バイト)
    };
    CHECK_EQ(create("0:/A.XDF"), 1);
    CHECK_EQ(create("0:/B.XDF"), 1);
    CHECK_EQ(create("0:/C.XDF"), 1);
    CHECK_EQ(f_unmount("0:"), FR_OK);
    for (const auto& e : kSfn) {
        int patched = 0;
        for (size_t o = 0; o + 32 <= sizeof g_disk; o += 32)
            if (memcmp(g_disk + o, e.ascii, 11) == 0) {
                memcpy(g_disk + o, e.cp932, 11);
                patched++;
            }
        CHECK_EQ(patched, 1);
    }
    CHECK_EQ(f_mount(&fs, "0:", 1), FR_OK);
    // FF_SFN_BUF が 12 のままだと「?」になる
    for (const auto& e : kSfn) {
        CHECK_EQ(listed(e.utf8), 1);
        char p[64] = "0:/";
        strcat(p, e.utf8);
        FIL f;
        CHECK_EQ(f_open(&f, p, FA_READ), FR_OK);
        f_close(&f);
    }
    // 長い名前も変わらない
    CHECK_EQ(listed(longName), 1);
    return finish();
}
