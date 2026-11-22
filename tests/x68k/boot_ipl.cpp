// 本物の IPL ROM で X68000 を起動し、指定フレーム数だけ動かす。
// 終わったら、合成した画面 (テキスト、グラフィック、スプライト) を PPM 画像に書き出す。
//   x68k_boot [-r ROM のディレクトリ] [-f フレーム数] [-o 出力.ppm] [-t] [-0 FD0 のイメージ] [-1 FD1 のイメージ]
//            [-h HDF] [-s HDS] [--scsi-rom ROM] [--sasi-trace] [--scsi-trace] [ほかのオプションは下の一覧]
//   ROM のディレクトリの既定は ~/.cache/pix68k/rom (IPLROM.DAT と CGROM.DAT)
//   -t: 1 秒ごとに PC を表示する
//   -0 / -1: フロッピーディスクのイメージ (XDF / DIM / D88)。書き込みはファイルに戻さない
//   -h: SASI のハードディスクのイメージ (HDF) を ID 0 につなぐ。書き込みはファイルに戻さない
//   --sasi-trace: SASI のコマンドが終わるたびに、PC、コマンドの 6 バイト、ステータス、センスの 1 バイト目を表示する
//   -s: SCSI のハードディスクのイメージ (HDS、512 バイト / ブロック) を内蔵 SCSI の ID 0 につなぐ。書き込みはファイルに戻さない
//   --scsi-rom: 内蔵 SCSI の ROM のファイル。渡すと内蔵 SCSI の機種として動かす
//   --scsi-trace: SCSI のコマンドが終わるたびに (内蔵と外付けの両方)、PC、ID、コマンド、ステータスを表示する
//   --stream: -s のイメージを、ベアメタル版の大きなイメージと同じくチャンクのキャッシュ (x68k/disk_cache.h) を通して
//             読み書きする。頼まれたチャンクはフレームの終わりにまとめて読む (コントローラは最大 1 フレーム待つ)
//   --scsi-ex-rom: 外付け SCSI ボード (CZ-6BS1 相当) の ROM ($EA0000 / $EA0020 から)。渡すとボードがつながる
//   --se: SCSI のハードディスクのイメージ (HDS) を外付け SCSI の ID 0 につなぐ。書き込みはファイルに戻さない
//   --cd: CD-ROM のイメージ (ISO、2048 バイト / セクタ) を SCSI の CD-ROM 装置としてつなぐ。
//         外付け SCSI ボードがあればそちら、なければ内蔵 SCSI。ID は --cd-id (既定 6)
//   --sram-cache 値: SRAM のキャッシュの設定 ($ED0090。X68030 / 060turbo。bit0 が命令キャッシュ、bit1 がデータキャッシュ) を
//         書き換えて電源を入れ直す (空の SRAM では 0 で、060turbo.sys はキャッシュを有効にしない)
//   --boot-rom アドレス: SRAM の起動デバイスを ROM 起動 ($ED0018 = $A000) にし、ROM 起動のアドレス ($ED000C) を
//         この値 (16 進) にして起動する (外付け SCSI の ID n なら EA0020 + n x 4)。一度電源を入れて IPL ROM に
//         SRAM を初期化させてから書き換え、電源を入れ直す
//   --bus-wait N: 060turbo で、本体のバスの読み書き 1 回に足すサイクル数 (Config::busWait060。既定はクロックから決まる)
//   --bus-sync ns: X68030 と 060turbo で、本体の I/O の読み書きをそろえる周期 (Config::setBusSync()。既定 1600、0 でそろえない)
//   --bus-sync-areas n: そろえる場所 (Config::BUS_SYNC_* の和。1 = I/O、2 = スプライト、4 = VRAM。既定 1)
//   --bus-area 名前=pre,period,post: 場所の種類ごとの待ち (ns。Config::busArea。--bus-sync と --bus-sync-areas のあとに
//         当てる)。名前は Config::busAreaName() (gvram tvram crtc palette video mfp rtc opm io sprite bgctrl pcg bg ext など)。名前のあとに .r か .w を付けると
//         読みか書きだけを変える。値は 0 から 1000000 (1 ミリ秒)。何回でも書ける (例: --bus-area pcg=750,0,0 --bus-area rtc.r=0,1600,0)
//   --clock MHz: CPU のクロック (MHz)。68000 は 10 か 16 (既定、XVI)、68060 は 50 (既定) か 75
//   --cpu 68000|68030: 68030 なら X68030 (25MHz、68882)。IPL ROM の既定は IPLROM30.DAT になり、
//                      --scsi-rom がなければ内蔵 SCSI の ROM を IPL ROM から作る
//   --ipl ファイル: IPL ROM のファイル (既定は ROM のディレクトリの IPLROM.DAT / IPLROM30.DAT)
//   --ts6bga: TS-6BGA を $EE0000 につなぐ。--ts6bga-out FILE: 終了時の仮想VRAMをPPMで保存 (接続も行う)
//   --fpu-board: 数値演算プロセッサボード (CZ-6BP1 相当) をつなぐ。--mercury: まーきゅりーゆにっとをつなぐ
//   --high-mem MB: X68030 のハイメモリ ($01000000 から、最大 16MB)
//   --cpu 68060: 060turbo (68060 50MHz)。IPL ROM の既定は ROM のディレクトリの IPLROM060.DAT (68060 で動く IPL ROM。
//                XEiJ の IPLROM 1.6 を書き出したものなど)、なければ IPLROM30.DAT (machine.cpp の patchIplFor060)。
//                IPLROM060.DAT を使うときは、ROM30_060.DAT があれば $FC0000 からの ROM として使う
//   --local-mem MB: 060turbo のローカルメモリ ($10000000 から、最大 256MB。既定は 128MB)
//   --ram MB: 本体のメモリの容量 (1-12MB、既定 4MB)
//   --trace-unimpl: 未実装の命令などの例外 (不当命令、F ライン、未実装整数命令) を表示する
//   --check-prompt: --expect "A>" と同じ (Human68k のプロンプトが出ているか)
//   --check-no-disk: ディスクがないときの「ディスクから起動できません」がテキスト画面に出ているかを調べ、
//                    出ていれば 0、出ていなければ 1 で終わる。ROM がなければ 77 (ctest の SKIP_RETURN_CODE)
//   --type 文字列: -f のフレーム数だけ動かしたあと、キーボードからこの文字列を打つ (\n は改行、\p は 5 秒待つ、
//                  \d は ↓、\u は ↑、\e は ESC)。
//                  打ち終えてから --after のフレーム数 (既定 300) だけ動かす
//   --expect 文字列: 終わったときのテキスト画面 (CGROM の 8x16 の字形で読んだもの) に、この文字列が
//                    あるかを調べる (何回でも指定できる)。すべてあれば 0、なければ 1。
//                    ROM、CGROM、ディスクのイメージのどれかがなければ 77
//   --reject 文字列: 終わったときのテキスト画面に、この文字列がないことを調べる (何回でも指定できる)
//   --dump-text: 終わったときのテキスト画面を、読めた文字で表示する (読めない字は ?)
//   --save-hds ファイル: 終わったときの SCSI のディスク (-s) の中身を書き出す (X68000 で書いたファイルを取り出すため)
//   --save-hdf ファイル: 終わったときの SASI のディスク (-h) の中身を書き出す (FORMAT.X が書いた管理情報を見るため)
//   --save-0 ファイル: 終わったときの FD0 (-0) の中身を書き出す (XDF などのベタのイメージだけ。D88 は書き出さない)。
//                     書き出せなければ 1 で終わる
//   --neptune: Neptune-X をつなぎ、線の向こうに試験用の相手 (platform/ether_peer.h の EchoPeer。どの IP アドレスの
//              ARP にも答え、ping に答える) を置く。終わったときに、相手が答えた ARP と ping の数を表示する
//   --wav ファイル: 音 (OPM と ADPCM) を 48kHz の WAV に書く
//   --expect-sound N: 音の振幅の最大が N 以上なら成功 (--wav がなくても音を作って調べる)
//   --time N: RTC の時刻を、地方時の 1970-01-01 00:00:00 から N 秒の時刻から始める (ホストの時間帯は見ない。
//     1700000000 はどこでも 2023-11-14 22:13:20)。既定はホストの今の時刻を UTC のまま入れたもの (地方時に直さない)。
//             固定すると、同じ入力で同じ画面と音になる (実行ごとに比べられる)
//   --type の文字列の \a は CTRL+G (Human68k のベル)、\n は改行
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "x68k/machine.h"
#include "x68k/render.h"
#include "x68k/textscreen.h"
#include "platform/ether_peer.h"
#include "platform/time_arg.h"
#include "platform/wav_file.h"

// --stream: フレームの終わりに、書いたチャンクを data に戻し、頼まれたチャンクを data から読む
// (ベアメタル版のコア 0 の書き戻しと読み込みの代わり)
static void serviceStream(x68k::HardDiskImage& img, x68k::DiskCache& cache, uint8_t* data) {
    x68k::DirtyMap& map = img.dirtyMap();
    for (int64_t c; (c = map.next(0)) >= 0;) {
        uint32_t len;
        const uint8_t* p = img.chunkData((uint32_t)c, len);
        if (!p) break;
        memcpy(data + ((size_t)c << x68k::DirtyMap::CHUNK_SHIFT), p, len);
        map.clear((uint32_t)c);
    }
    unsigned slot;
    uint32_t c, len;
    uint8_t* buf;
    while (cache.nextLoad(slot, c, buf, len)) {
        memcpy(buf, data + ((size_t)c << x68k::DirtyMap::CHUNK_SHIFT), len);
        cache.loaded(slot, true);
    }
}

// イメージや ROM が読めなかったことを知らせて、main() の終了コード (1) を返す。what は "SCSI ROM" など
static int cannotLoad(const char* what, const char* path) {
    fprintf(stderr, "cannot load %s %s\n", what, path);
    return 1;
}

// ファイル全体を読む (大きさは呼び出し側が知らないとき)
static uint8_t* loadWhole(const char* path, size_t& size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = n > 0 ? new uint8_t[(size_t)n] : nullptr;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        delete[] buf;
        buf = nullptr;
    }
    fclose(f);
    size = buf ? (size_t)n : 0;
    return buf;
}

static bool loadFile(const char* path, uint8_t* buf, size_t size) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, size, f);
    int extra = fgetc(f);
    fclose(f);
    return n == size && extra == EOF;
}

// 合成した画面 (Renderer が描いたもの) を PPM で書き出す
static void writeScreen(const uint32_t* pixels, int pitch, int w, int h, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t c = pixels[y * pitch + x];
            uint8_t rgb[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

// テキストパレットで暗い色 (輝度が半分未満) になる画素を 1 とした 768x512 の画像
static void darkPixels(x68k::Machine& m, uint8_t* out) {
    const uint8_t* t = m.video().tvram();
    const uint8_t* pal = m.video().palette() + 0x200;
    for (int y = 0; y < 512; y++) {
        for (int x = 0; x < 768; x++) {
            uint32_t o = (uint32_t)y * 128 + (uint32_t)(x >> 3);
            int b = 7 - (x & 7), idx = 0;
            for (int p = 0; p < 4; p++) idx |= ((t[p * 0x20000 + o] >> b) & 1) << p;
            uint16_t c = (uint16_t)((pal[idx * 2] << 8) | pal[idx * 2 + 1]);
            int sum = ((c >> 6) & 31) + ((c >> 11) & 31) + ((c >> 1) & 31);
            out[y * 768 + x] = sum < 48;
        }
    }
}

// 「ディスクから起動できません。」の 1 行目 (212x16 ドット、1 = 暗い画素)。
// IPL ROM は CGROM ではなく自分の字形でこの文を描くので、公開されている X68000 の IPL ROM
// (IPLROM.DAT) で表示されたものを写してある。ほかの版の IPL ROM では字形が違うかもしれない
static const int kMsgW = 212, kMsgH = 16, kMsgBytes = 27;
static const char kMsg[kMsgH][kMsgBytes + 1] = {
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x18\x00\x07\x60\x00\x00\x06\x00\x01\x80\x00\x00\x03\x00\x00",
    "\x00\x18\x00\x00\x00\x00\x03\x00\x18\x00\x03\x00\x18\xfc\x7c\x60\x00\x3c\x06\x00\x01\x80\x00\xc0\x03\x00\x00",
    "\x00\x6c\x00\x00\x00\x00\x03\x00\x18\x00\x01\xc0\xff\x0c\x18\x60\x0f\xe0\x07\xe0\x7f\xf8\x18\xc0\x06\x00\x00",
    "\x3f\xfc\x00\x00\x7f\xc0\x07\xf8\x18\x60\x30\x00\x18\x0d\xff\xe1\xf8\xcc\x7f\x00\x01\x80\x18\xc0\x06\x00\x00",
    "\x00\x30\x00\x00\x00\xc0\x06\x18\x18\x30\x30\x00\x18\x0c\x19\xfc\x01\xb4\x03\x00\x01\x80\x18\xfc\x0c\x00\x00",
    "\x00\x00\x00\x60\x00\xc0\x0c\x30\xff\x18\x30\x00\x18\x0c\x7f\x6c\x03\x1c\x03\xf0\x01\xf0\x1f\xc0\x0c\x00\x00",
    "\x00\x00\x00\xc0\x01\x80\x18\x30\x19\x98\x30\x01\xff\xfc\x7b\x6c\x03\x18\xff\x80\x7f\x81\xf8\xc0\x0c\x00\x00",
    "\xff\xfc\x01\x80\x01\x80\x30\x60\x31\x8c\x33\xe0\x18\xc0\x7f\x6c\x06\x00\x00\xc0\x01\x80\x18\xc0\x1f\x00\x00",
    "\x01\x80\x03\x00\x03\x00\x60\x60\x31\x8c\x3e\x30\x78\xc0\x7b\x6c\x06\x00\x00\xc0\x01\x80\x18\xc0\x1d\x80\x00",
    "\x01\x80\x0f\x00\x03\x80\x00\xc0\x31\x8c\x38\x18\x7f\xc0\x7b\x6c\x06\x00\x1f\xe0\x01\x80\x18\xc0\x39\x80\x00",
    "\x01\x80\x3b\x00\x06\xc0\x01\x80\x61\x80\x30\x18\x78\xc4\x7f\x6c\x06\x00\x30\xe0\x1f\x80\x19\x80\x31\x80\x00",
    "\x03\x00\x03\x00\x0c\x60\x03\x00\x61\x80\x00\x30\x78\x64\x18\x6c\x03\x00\x60\x00\x31\xe0\x18\x00\x61\x84\xe0",
    "\x03\x00\x03\x00\x18\x30\x06\x00\xc3\x00\x00\x60\x78\x3c\xfe\xcc\x01\x80\x60\x00\x61\xb8\x0c\x00\x61\x8d\xb0",
    "\x06\x00\x03\x00\x70\x18\x1c\x00\xdb\x00\x01\xc0\xdc\x00\x1f\xcc\x00\xf0\x38\x00\x71\x80\x07\xf0\xc0\xd9\xb0",
    "\x1c\x00\x03\x01\xc0\x18\x70\x00\x0e\x00\x0f\x00\xc7\x80\x3d\x8c\x00\x00\x0f\x80\x1f\x00\x00\x00\xc0\x70\xe0",
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x80\xfd\xe3\x38\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
};

// テキスト画面のどこかに、上の 1 行がそのまま出ているか
static bool findMessage(x68k::Machine& m) {
    static uint8_t dark[768 * 512];
    darkPixels(m, dark);
    for (int y = 0; y + kMsgH <= 512; y++) {
        for (int x = 0; x + kMsgW <= 768; x++) {
            bool match = true;
            for (int r = 0; r < kMsgH && match; r++)
                for (int c = 0; c < kMsgW; c++) {
                    int bit = ((uint8_t)kMsg[r][c >> 3] >> (7 - (c & 7))) & 1;
                    if (bit != dark[(y + r) * 768 + x + c]) {
                        match = false;
                        break;
                    }
                }
            if (match) return true;
        }
    }
    return false;
}

// コマンドラインの \n を改行にする
static void unescape(const char* in, char* out, size_t size) {
    size_t n = 0;
    for (; *in && n + 1 < size; in++) {
        if (in[0] == '\\' && in[1] == 'n') {
            out[n++] = '\n';
            in++;
        } else if (in[0] == '\\' && in[1] == 'a') {
            out[n++] = '\a';
            in++;
        } else if (in[0] == '\\' && in[1] == 'p') {
            out[n++] = '\x01'; // 待つ
            in++;
        } else if (in[0] == '\\' && (in[1] == 'd' || in[1] == 'u' || in[1] == 'e')) {
            out[n++] = in[1] == 'd' ? '\x02' : in[1] == 'u' ? '\x03' : '\x04'; // ↓、↑、ESC
            in++;
        } else {
            out[n++] = *in;
        }
    }
    out[n] = 0;
}

// SASI のトレース (--sasi-trace)。Sasi::setTrace() から、ステータスフェーズに入るたびに呼ばれる
// どちらも標準出力に、今のフレームの番号 (下の実行のループの番号) と PC を付けて 1 行で出す
static int g_traceFrame = 0;

static uint32_t tracePc(void* machine) {
    m68k::Registers regs;
    static_cast<x68k::Machine*>(machine)->getRegisters(regs);
    return regs.pc;
}

static void traceSasi(void* machine, const uint8_t* cmd, uint8_t status, uint8_t sense) {
    printf("frame %d pc=%06x SASI", g_traceFrame, (unsigned)tracePc(machine));
    for (int k = 0; k < 6; k++) printf(" %02x", cmd[k]);
    printf(" -> status %x sense %02x\n", status, sense);
}

// SCSI のトレース (--scsi-trace)。コマンドの長さは 1 バイト目の上位 3 ビットで決まる (グループ 1、2 は 10 バイト、5 は 12 バイト)
static void traceScsi(void* machine, int id, const uint8_t* cdb, uint8_t status) {
    static const uint8_t kLength[8] = {6, 10, 10, 6, 6, 12, 6, 6}; // グループ (上位 3 ビット) ごとの長さ
    printf("frame %d pc=%06x SCSI id %d", g_traceFrame, (unsigned)tracePc(machine), id);
    for (int k = 0; k < kLength[cdb[0] >> 5]; k++) printf(" %02x", cdb[k]);
    printf(" -> status %x\n", status);
}

static bool fileExists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    char romDir[1024];
    const char* home = getenv("HOME");
    snprintf(romDir, sizeof romDir, "%s/.cache/pix68k/rom", home ? home : ".");
    int frames = 600;
    const char* out = "x68k_screen.ppm";
    bool trace = false;
    const char* disk[2] = {nullptr, nullptr};
    bool check = false;
    static char typeText[1024];
    bool type = false, dumpText = false;
    int after = 300;
    const char* expect[8];
    int nexpect = 0;
    const char* reject[8];
    int nreject = 0;
    const char* wavPath = nullptr;
    long expectSound = -1;
    const char* hdf = nullptr;     // -h
    const char* hdsPath = nullptr; // -s
    const char* scsiRom = nullptr; // --scsi-rom
    const char* saveHds = nullptr;
    const char* saveHdf = nullptr;
    uint8_t* hdfData = nullptr;
    size_t hdfSize = 0;
    const char* saveFd0 = nullptr;
    bool neptune = false;
    bool stream = false;
    static x68k::DiskCache streamCache;
    uint8_t* hdsData = nullptr;
    size_t hdsSize = 0;
    bool sasiTrace = false, scsiTrace = false, traceUnimpl = false;
    const char* scsiExRom = nullptr;
    bool fpuBoard = false, mercury = false, ts6bga = false;
    const char* ts6bgaOut = nullptr;
    const char* seHds = nullptr;
    const char* cdIso = nullptr;
    int cdId = 6;
    long bootRom = -1;
    long sramCache = -1;
    int localMemMB = -1;
    int clockMHz = 0; // --clock (0 は CPU の既定)
    int busSyncNs = -1, busSyncAreas = -1, busWaitCycles = -1; // --bus-sync、--bus-sync-areas、--bus-wait (負は既定)
    const char* busAreaArgs[32]; // --bus-area
    int busAreaArgCount = 0;
    const char* iplPath = nullptr;
    int64_t startTime = (int64_t)time(nullptr);
    x68k::Config config;
    // SASI と SCSI のディスクと ROM のオプション。ファイルの名前を 1 つとるものと、トレースを出すかだけのもの
    const struct {
        const char* flag;
        const char** path;
    } kDiskPaths[] = {{"-h", &hdf}, {"-s", &hdsPath}, {"--scsi-rom", &scsiRom}};
    const struct {
        const char* flag;
        bool* on;
    } kDiskTraces[] = {{"--sasi-trace", &sasiTrace}, {"--scsi-trace", &scsiTrace}};
    auto takeDiskOption = [&](int& at) {
        for (const auto& o : kDiskPaths)
            if (at + 1 < argc && strcmp(argv[at], o.flag) == 0) {
                *o.path = argv[++at];
                return true;
            }
        for (const auto& o : kDiskTraces)
            if (strcmp(argv[at], o.flag) == 0) return *o.on = true;
        return false;
    };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) snprintf(romDir, sizeof romDir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-t")) trace = true;
        else if (!strcmp(argv[i], "-0") && i + 1 < argc) disk[0] = argv[++i];
        else if (!strcmp(argv[i], "-1") && i + 1 < argc) disk[1] = argv[++i];
        else if (!strcmp(argv[i], "--check-no-disk")) check = true;
        else if (!strcmp(argv[i], "--type") && i + 1 < argc) {
            unescape(argv[++i], typeText, sizeof typeText);
            type = true;
        } else if (!strcmp(argv[i], "--after") && i + 1 < argc) after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--expect") && i + 1 < argc && nexpect < 8) expect[nexpect++] = argv[++i];
        else if (!strcmp(argv[i], "--reject") && i + 1 < argc && nreject < 8) reject[nreject++] = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wavPath = argv[++i];
        else if (!strcmp(argv[i], "--expect-sound") && i + 1 < argc) expectSound = atol(argv[++i]);
        else if (!strcmp(argv[i], "--dump-text")) dumpText = true;
        else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
            const char* v = argv[++i];
            if (!platform::parseTimeArg(v, startTime)) {
                fprintf(stderr, "%s: --time: want seconds since 1970-01-01 00:00:00 local time (0 or more), got '%s'\n", argv[0], v);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--check-prompt") && nexpect < 8) expect[nexpect++] = "A>";
        else if (takeDiskOption(i)) continue;
        else if (!strcmp(argv[i], "--save-hds") && i + 1 < argc) saveHds = argv[++i];
        else if (!strcmp(argv[i], "--save-hdf") && i + 1 < argc) saveHdf = argv[++i];
        else if (!strcmp(argv[i], "--save-0") && i + 1 < argc) saveFd0 = argv[++i];
        else if (!strcmp(argv[i], "--neptune")) neptune = true;
        else if (!strcmp(argv[i], "--stream")) stream = true;
        else if (!strcmp(argv[i], "--scsi-ex-rom") && i + 1 < argc) scsiExRom = argv[++i];
        else if (!strcmp(argv[i], "--se") && i + 1 < argc) seHds = argv[++i];
        else if (!strcmp(argv[i], "--cd") && i + 1 < argc) cdIso = argv[++i];
        else if (!strcmp(argv[i], "--cd-id") && i + 1 < argc) {
            cdId = atoi(argv[++i]);
            if (cdId < 0 || cdId > 6) {
                fprintf(stderr, "%s: --cd-id: must be 0..6 (7 is the host)\n", argv[0]);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--boot-rom") && i + 1 < argc) bootRom = strtol(argv[++i], nullptr, 16);
        else if (!strcmp(argv[i], "--sram-cache") && i + 1 < argc) sramCache = strtol(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) clockMHz = atoi(argv[++i]); // --cpu のあとで選ぶ
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc && config.setCpu(atoi(argv[i + 1]))) i++;
        else if (!strcmp(argv[i], "--ipl") && i + 1 < argc) iplPath = argv[++i];
        else if (!strcmp(argv[i], "--fpu-board")) fpuBoard = true;
        else if (!strcmp(argv[i], "--mercury")) mercury = true;
        else if (!strcmp(argv[i], "--ts6bga")) ts6bga = true;
        else if (!strcmp(argv[i], "--ts6bga-out") && i + 1 < argc) {
            ts6bga = true; ts6bgaOut = argv[++i];
        }
        else if (!strcmp(argv[i], "--high-mem") && i + 1 < argc) config.highMemSize = (uint32_t)atoi(argv[++i]) << 20;
        else if (!strcmp(argv[i], "--local-mem") && i + 1 < argc) localMemMB = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bus-sync") && i + 1 < argc) busSyncNs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bus-wait") && i + 1 < argc) busWaitCycles = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bus-sync-areas") && i + 1 < argc) busSyncAreas = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bus-area") && i + 1 < argc && busAreaArgCount < 32) busAreaArgs[busAreaArgCount++] = argv[++i];
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) config.ramSize = (uint32_t)atoi(argv[++i]) << 20;
        else if (!strcmp(argv[i], "--trace-unimpl")) traceUnimpl = true;
        else {
            fprintf(stderr,
                    "usage: %s [-r romdir] [-f frames] [-o out.ppm] [-t] [-0 fd0] [-1 fd1] [--check-no-disk]\n"
                    "          [--type text] [--after frames] [--expect text]... [--reject text]... [--dump-text] [--check-prompt]\n"
                    "          [--wav out.wav] [--expect-sound N] [--time seconds]\n"
                    "          [-h hdf] [--sasi-trace] [--scsi-rom rom] [-s hds] [--save-hds file] [--scsi-trace] [--clock MHz]\n"
                    "          [--scsi-ex-rom rom] [--se hds] [--cd iso] [--cd-id N] [--boot-rom hexaddr] [--fpu-board] [--mercury] [--ts6bga] [--ts6bga-out file.ppm]\n"
                    "          [--neptune] [--save-0 file]\n"
                    "          [--cpu 68000|68030|68060] [--ipl iplrom] [--high-mem MB] [--local-mem MB] [--ram MB] [--trace-unimpl]\n"
                    "          [--bus-wait N] [--bus-sync ns] [--bus-sync-areas n] [--bus-area name=pre,period,post]\n",
                    argv[0]);
            return 2;
        }
    }
    if (clockMHz && !config.setClockMHz(clockMHz)) {
        fprintf(stderr, "%s: --clock %d: not for this CPU (68000: 10, 16; 68060: 50, 75)\n", argv[0], clockMHz);
        return 2;
    }
    static uint8_t ipl[x68k::Machine::IPL_SIZE], cg[x68k::Machine::CG_SIZE];
    char path[1100];
    // X68030 と 060turbo は X68030 の IPL ROM を使う
    const bool x68030 = config.cpuModel != m68k::CpuModel::M68000;
    if (config.cpuModel == m68k::CpuModel::M68060) config.localMemSize = (uint32_t)(localMemMB < 0 ? 128 : localMemMB) << 20;
    if (busSyncNs >= 0 || busSyncAreas >= 0)
        config.setBusSync(busSyncNs >= 0 ? busSyncNs : x68k::Config::BUS_SYNC_NS,
                          busSyncAreas >= 0 ? (uint8_t)busSyncAreas
                                            : x68k::Config::BUS_SYNC_DEFAULT);
    // 本体のバスの待ちの表は X68030 と 060turbo だけのもの。68000 で指定したら、黙って無視せずに断る
    if (config.cpuModel == m68k::CpuModel::M68000 && (busSyncNs >= 0 || busSyncAreas >= 0 || busAreaArgCount > 0)) {
        fprintf(stderr, "--bus-sync, --bus-sync-areas and --bus-area need --cpu 68030 or --cpu 68060\n");
        return 2;
    }
    for (int i = 0; i < busAreaArgCount; i++) {
        if (!config.setBusAreaText(busAreaArgs[i])) {
            fprintf(stderr, "--bus-area: bad value: %s (name[.r|.w]=pre,period,post, each 0 to %d ns)\n", busAreaArgs[i],
                    x68k::Config::BUS_NS_MAX);
            return 2;
        }
    }
    if (busWaitCycles >= 0) config.busWait060 = busWaitCycles; // setCpu() と setClockMHz() が決めた値を書き換える
    static char rom30Path[1100];
    if (iplPath) {
        snprintf(path, sizeof path, "%s", iplPath);
    } else {
        snprintf(path, sizeof path, "%s/%s", romDir, x68030 ? "IPLROM30.DAT" : "IPLROM.DAT");
        if (config.cpuModel == m68k::CpuModel::M68060) {
            char p060[1100];
            snprintf(p060, sizeof p060, "%s/IPLROM060.DAT", romDir);
            // ROM30_060.DAT は IPLROM060.DAT と組で使う (X68030 の IPL ROM のときは IPL ROM から作る)
            if (fileExists(p060)) {
                snprintf(path, sizeof path, "%s", p060);
                snprintf(rom30Path, sizeof rom30Path, "%s/ROM30_060.DAT", romDir);
                if (!scsiRom && fileExists(rom30Path)) scsiRom = rom30Path;
            }
        }
    }
    // --expect のテストは、ROM やディスクがなければ飛ばす (77)
    const int missing = (check || nexpect > 0 || nreject > 0 || expectSound >= 0) ? 77 : 1;
    if (!loadFile(path, ipl, sizeof ipl)) {
        fprintf(stderr, "cannot read %s (128KB)\n", path);
        return missing;
    }
    snprintf(path, sizeof path, "%s/CGROM.DAT", romDir);
    bool haveCg = loadFile(path, cg, sizeof cg);
    if (!haveCg && (nexpect > 0 || nreject > 0)) {
        fprintf(stderr, "cannot read %s (768KB); --expect needs it\n", path);
        return missing; // ROM がないときと同じに扱う
    }
    if (!haveCg) {
        // ディスクがないときの文は IPL ROM の字形で描かれるので、CGROM がなくても確かめられる
        fprintf(stderr, "warning: cannot read %s (768KB); characters will be blank\n", path);
    }

    // Machine は大きいので (SRAM、FDC のバッファなど) スタックに置かない
    static x68k::Machine m(config);
    if (!m.ok()) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    m.setIplRom(ipl, sizeof ipl);
    if (haveCg) m.setCgRom(cg, sizeof cg);
    static x68k::FloppyImage images[2];
    uint8_t* fd0Data = nullptr;
    size_t fd0Size = 0;
    for (int d = 0; d < 2; d++) {
        if (!disk[d]) continue;
        if (!fileExists(disk[d])) {
            fprintf(stderr, "cannot open disk image %s\n", disk[d]);
            return missing;
        }
        size_t size;
        uint8_t* data = loadWhole(disk[d], size);
        if (!data || !images[d].load(data, size, false, disk[d])) {
            return cannotLoad("disk image", disk[d]);
        }
        m.insertDisk(d, &images[d]);
        if (d == 0) {
            fd0Data = data;
            fd0Size = size;
        }
    }
    static x68k::HardDiskImage hd, sd;
    if (hdf != nullptr) { // SASI の ID 0
        if (!fileExists(hdf)) {
            fprintf(stderr, "cannot open hard disk image %s\n", hdf);
            return missing;
        }
        size_t bytes = 0;
        uint8_t* data = loadWhole(hdf, bytes);
        if (!data || !hd.load(data, bytes, x68k::Sasi::BLOCK_SIZE, false)) {
            return cannotLoad("hard disk image", hdf);
        }
        m.attachSasi(0, &hd); // ID 0、論理ユニット 0
        hdfData = data;
        hdfSize = bytes;
    }
    if (sasiTrace) m.sasi().setTrace(&traceSasi, &m);
    if (traceUnimpl && m.cpu020()) {
        m.cpu020()->onUnimplemented = [](void*, uint32_t pc, uint16_t op, int vector) {
            if (vector != 11 || (op & 0xff00) != 0xff00) // DOS コール ($FFxx) は出さない
                fprintf(stderr, "unimplemented: pc=%08x op=%04x vector=%d\n", pc, op, vector);
        };
        m.cpu020()->onUnimplementedCtx = &m;
        m.cpu020()->onAccessFault = [](void* ctx, uint32_t pc, uint32_t addr, uint32_t fslw) {
            auto* mm = (x68k::Machine*)ctx;
            m68k::Cpu020* c = mm->cpu020();
            fprintf(stderr, "access fault: pc=%08x addr=%08x fslw=%08x tc=%04x code:", pc, addr, fslw, c->tc060_);
            for (int i = 0; i < 8; i++) {
                bool ok;
                uint8_t hi = mm->peek8(pc + 2 * i, ok), lo = mm->peek8(pc + 2 * i + 1, ok);
                fprintf(stderr, " %02x%02x", hi, lo);
            }
            fprintf(stderr, "\n  d:");
            for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", c->d_[i]);
            fprintf(stderr, "\n  a:");
            for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", c->a_[i]);
            fprintf(stderr, "\n");
        };
    }
    if (scsiRom != nullptr) { // 内蔵 SCSI の ROM。読めないときも大きすぎるときも 1 で終わる (--expect でも飛ばしにしない)
        size_t bytes = 0;
        uint8_t* rom = loadWhole(scsiRom, bytes);
        const bool ok = rom && m.setScsiRom(rom, bytes);
        delete[] rom;
        if (!ok) {
            return cannotLoad("SCSI ROM", scsiRom);
        }
    }
    if (!scsiRom && x68030) {
        static uint8_t scsi30[x68k::Machine::SCSI_ROM_SIZE];
        x68k::Machine::makeScsiRom30(ipl, scsi30);
        m.setScsiRom(scsi30, sizeof scsi30);
    }
    if (hdsPath != nullptr) { // 内蔵 SCSI の ID 0。ファイルがないときも 1 で終わる (--expect でも飛ばしにしない)
        size_t bytes = 0;
        uint8_t* data = loadWhole(hdsPath, bytes);
        const bool ok = data && (stream ? streamCache.init(bytes, 64) && sd.loadCached(&streamCache, bytes, 512, false)
                                        : sd.load(data, bytes, 512, false));
        if (!ok) {
            return cannotLoad("SCSI image", hdsPath);
        }
        m.attachScsi(0, &sd); // ID 0
        hdsData = data;
        hdsSize = bytes;
    }
    if (fpuBoard) m.setFpuBoard(true);
    if (mercury) m.setMercury(true);
    if (ts6bga && !m.setTs6bga(true)) { fprintf(stderr, "cannot allocate TS-6BGA\n"); return 1; }
    static platform::EchoPeer echoPeer;
    if (neptune) {
        if (!m.setNeptune(true)) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        m.neptune()->setLink(&echoPeer);
    }
    if (scsiExRom) {
        // ROM がなければ、ROM やディスクがないときと同じに扱う (--expect のテストは飛ばす)
        if (!fileExists(scsiExRom)) {
            fprintf(stderr, "cannot open SCSI ROM %s\n", scsiExRom);
            return missing;
        }
        size_t size = 0; // loadWhole() が入れる
        uint8_t* data = loadWhole(scsiExRom, size);
        if (!data || !m.setScsiExRom(data, size)) {
            return cannotLoad("SCSI ROM", scsiExRom);
        }
        delete[] data;
    }
    static x68k::HardDiskImage se, cd;
    if (seHds) {
        if (!fileExists(seHds)) {
            fprintf(stderr, "cannot open SCSI image %s\n", seHds);
            return missing;
        }
        size_t size = 0; // loadWhole() が入れる
        uint8_t* data = loadWhole(seHds, size);
        if (!data || !se.load(data, size, 512, false)) {
            return cannotLoad("SCSI image", seHds);
        }
        m.attachScsiEx(0, &se);
    }
    if (cdIso) {
        if (!fileExists(cdIso)) {
            fprintf(stderr, "cannot open CD-ROM image %s\n", cdIso);
            return missing;
        }
        size_t size = 0; // loadWhole() が入れる
        uint8_t* data = loadWhole(cdIso, size);
        if (!data || !cd.load(data, size, 2048, true)) {
            return cannotLoad("CD-ROM image", cdIso);
        }
        if (m.hasScsiEx()) m.attachScsiEx(cdId, &cd, true);
        else m.attachScsi(cdId, &cd, true);
    }
    if (scsiTrace) {
        m.spc().setTrace(traceScsi, &m);
        m.spcEx().setTrace(traceScsi, &m);
    }
    m.setTime(startTime);
    static uint32_t screen[x68k::Renderer::MAX_WIDTH * x68k::Renderer::MAX_HEIGHT];
    x68k::Renderer renderer(m.video());
    const int frameW = x68k::Renderer::MAX_WIDTH, frameH = x68k::Renderer::MAX_HEIGHT; // 描く枠はいつも最大の大きさ
    renderer.setTarget(screen, frameW, frameW, frameH);
    m.setLineListener(&renderer);
    // 音: WAV に書くか、振幅を調べるときだけ作る
    static platform::WavFile wav;
    const bool sound = wavPath || expectSound >= 0;
    if (sound) m.sound().setOutputRate(48000);
    if (wavPath && !wav.open(wavPath, 48000)) {
        perror(wavPath);
        return 1;
    }
    long peak = 0;
    static int16_t audio[2 * 4096];
    auto runFrame = [&]() {
        m.runFrame();
        if (stream && hdsData) serviceStream(sd, streamCache, hdsData);
        if (!sound) return;
        int n;
        while ((n = m.sound().ring().drain(audio, 4096)) > 0) {
            for (int i = 0; i < 2 * n; i++) {
                long v = audio[i] < 0 ? -(long)audio[i] : audio[i];
                if (v > peak) peak = v;
            }
            wav.write(audio, n);
        }
    };
    m.powerOn();
    if (bootRom >= 0) {
        // IPL ROM が空の SRAM に初期値を書くまで動かしてから、起動デバイスを書き換えて電源を入れ直す
        for (int f = 0; f < 60; f++) m.runFrame();
        uint8_t* s = m.sram();
        for (int i = 0; i < 4; i++) s[0x0c + i] = (uint8_t)(bootRom >> (24 - 8 * i));
        s[0x18] = 0xa0;
        s[0x19] = 0x00;
        m.powerOn();
    }
    if (sramCache >= 0) {
        // 同じように SRAM に初期値を書かせてから、キャッシュの設定 ($ED0090) を書き換えて電源を入れ直す
        for (int f = 0; f < 120; f++) m.runFrame();
        m.sram()[0x90] = (uint8_t)sramCache;
        m.powerOn();
    }
    for (int f = 0; f < frames; f++) {
        g_traceFrame = f;
        runFrame();
        if (trace && f % 55 == 0) {
            m68k::Registers r;
            m.getRegisters(r);
            printf("frame %d: pc=%06x sr=%04x halted=%d\n", f, r.pc, r.sr, m.halted());
        }
    }
    if (type) {
        // キーボードのキューに収まる分ずつ打つ (1 文字は SHIFT を含めて最大 4 バイト)
        const char* p = typeText;
        int f = 0;
        while (*p && f < 6000) {
            while (*p && m.keyboard().queued() < x68k::Keyboard::QUEUE_SIZE / 2) {
                if (*p == '\x01') {
                    // \p: キューが空になってから 300 フレーム (約 5 秒) 待つ (起動を待つときなど)
                    if (m.keyboard().queued() > 0) break;
                    for (int i = 0; i < 300; i++) runFrame();
                    p++;
                    break;
                }
                if (*p == '\a') {
                    // CTRL+G (USB HID: 左 CTRL = $E0、G = $0A)。前後の文字と順番が入れ替わらないよう、
                    // キューが空になってから押し、押したらキューが空になるまで次を打たない
                    if (m.keyboard().queued() > 0) break;
                    m.keyboard().onKey(0xe0, true);
                    m.keyboard().onKey(0x0a, true);
                    m.keyboard().onKey(0x0a, false);
                    m.keyboard().onKey(0xe0, false);
                    p++;
                    break;
                } else if (*p >= '\x02' && *p <= '\x04') {
                    // \d、\u、\e: ↓、↑、ESC (USB HID の $51、$52、$29)。メニューを選ぶとき (FORMAT.X など)。
                    // キーごとに画面が変わるのを待つため、キューが空になってから押し、30 フレーム待つ
                    if (m.keyboard().queued() > 0) break;
                    const uint8_t hid = *p == '\x02' ? 0x51 : *p == '\x03' ? 0x52 : 0x29;
                    m.keyboard().onKey(hid, true);
                    m.keyboard().onKey(hid, false);
                    for (int i = 0; i < 30; i++) runFrame();
                    p++;
                    break;
                } else if (!m.keyboard().typeAscii(*p)) {
                    fprintf(stderr, "cannot type 0x%02x\n", (uint8_t)*p);
                }
                p++;
            }
            runFrame();
            f++;
        }
        for (int i = 0; i < after; i++) runFrame();
        printf("typed %d chars in %d frames, then %d frames\n", (int)strlen(typeText), f, after);
    }
    m68k::Registers r;
    m.getRegisters(r);
    printf("after %d frames: pc=%06x sr=%04x halted=%d\n", frames, r.pc, r.sr, m.halted());
    if (neptune) printf("neptune peer: %u ARP replies, %u echo replies\n", echoPeer.arpReplies(), echoPeer.echoReplies());
    // --save-0 が書けなければ、最後に 1 で終わる (--expect などが通っても)
    bool saveFailed = false;
    if (saveFd0 && fd0Data) {
        if (images[0].format() == x68k::FloppyImage::D88) {
            fprintf(stderr, "--save-0: D88 images are not written\n");
            saveFailed = true;
        } else {
            FILE* f = fopen(saveFd0, "wb");
            bool ok = f && fwrite(fd0Data, 1, fd0Size, f) == fd0Size;
            if (f && fclose(f) != 0) ok = false;
            if (!ok) {
                fprintf(stderr, "cannot write %s\n", saveFd0);
                saveFailed = true;
            }
        }
    } else if (saveFd0) {
        fprintf(stderr, "--save-0: no disk in FD0\n");
        saveFailed = true;
    }
    if (out[0]) {
        writeScreen(screen, x68k::Renderer::MAX_WIDTH, renderer.width(), renderer.height(), out);
        printf("screen %dx%d: %s\n", renderer.width(), renderer.height(), out);
    }
    if (ts6bgaOut) {
        auto& b = *m.ts6bga();
        const int w = int(b.width()), h = int(b.height());
        uint8_t* rgb = new uint8_t[size_t(w) * h * 3];
        int sw = 0, sh = 0;
        FILE* f = fopen(ts6bgaOut, "wb");
        if (!f || !b.snapshotRgb(rgb, unsigned(w * h * 3), sw, sh)) {
            if (f) fclose(f);
            delete[] rgb;
            fprintf(stderr, "cannot save TS-6BGA screenshot\n"); return 1;
        }
        const bool header = fprintf(f, "P6\n%d %d\n255\n", sw, sh) > 0;
        const size_t bytes = size_t(sw) * sh * 3;
        const bool saved = fwrite(rgb, 1, bytes, f) == bytes;
        const bool closed = fclose(f) == 0;
        delete[] rgb;
        if (!header || !saved || !closed) return 1;
        printf("TS-6BGA screen %dx%d (selected=%d): %s\n", sw, sh, b.selected(), ts6bgaOut);
    }
    if (wavPath) {
        printf("wav: %u frames (%.2f s) to %s\n", wav.frames(), wav.frames() / 48000.0, wavPath);
        wav.close();
    }
    if (sound) printf("sound peak: %ld\n", peak);
    if (expectSound >= 0 && peak < expectSound) {
        printf("expect sound >= %ld: NOT reached\n", expectSound);
        return 1;
    }
    if (dumpText || nexpect > 0 || nreject > 0) {
        static char text[x68k::TEXT_ROWS][x68k::TEXT_COLS + 1];
        x68k::readTextScreen(m.video(), cg, text);
        if (dumpText)
            for (int row = 0; row < x68k::TEXT_ROWS; row++) printf("|%s\n", text[row]);
        int found = 0;
        for (int e = 0; e < nexpect; e++) {
            bool hit = false;
            for (int row = 0; row < x68k::TEXT_ROWS && !hit; row++) hit = strstr(text[row], expect[e]) != nullptr;
            printf("expect \"%s\": %s\n", expect[e], hit ? "found" : "NOT found");
            if (hit) found++;
        }
        int rejected = 0;
        for (int e = 0; e < nreject; e++) {
            bool hit = false;
            for (int row = 0; row < x68k::TEXT_ROWS && !hit; row++) hit = strstr(text[row], reject[e]) != nullptr;
            printf("reject \"%s\": %s\n", reject[e], hit ? "FOUND" : "not found");
            if (hit) rejected++;
        }
        if (found != nexpect || rejected) return 1;
    }
    if (check) {
        bool ok = findMessage(m);
        printf("no-disk message: %s\n", ok ? "found" : "NOT found");
        return ok && !saveFailed ? 0 : 1;
    }
    if (stream && hdsData) serviceStream(sd, streamCache, hdsData);
    if (saveHdf && hdfData) {
        FILE* f = fopen(saveHdf, "wb");
        if (!f || fwrite(hdfData, 1, hdfSize, f) != hdfSize) fprintf(stderr, "cannot write %s\n", saveHdf);
        if (f) fclose(f);
    }
    if (saveHds && hdsData) {
        FILE* f = fopen(saveHds, "wb");
        if (!f || fwrite(hdsData, 1, hdsSize, f) != hdsSize) fprintf(stderr, "cannot write %s\n", saveHds);
        if (f) fclose(f);
    }
    return saveFailed ? 1 : 0;
}
