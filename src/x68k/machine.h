// X68000 本体: メモリマップ、周辺機器、割り込み、時間の進め方。
//
// メモリマップ (24 ビット):
//   $000000-$BFFFFF  メイン RAM (つないだ容量まで。その先はバスエラー)
//   $C00000-$DFFFFF  グラフィック VRAM
//   $E00000-$E7FFFF  テキスト VRAM
//   $E80000-$EFFFFF  I/O (8KB ごとにデバイス。つながっていない場所はバスエラー)
//   $E9E000-$E9E01F  数値演算プロセッサボード (CZ-6BP1 相当。setFpuBoard() でつないだときだけ。なければバスエラー)
//   $E9E080-$E9E09F  2 枚目の数値演算プロセッサボード (setFpuBoards(2) のときだけ。なければバスエラー)
//   $EA0000-$EA1FFF  外付け SCSI ボード (CZ-6BS1。setScsiExRom() で ROM を渡したときだけ。なければバスエラー)
//                    $EA0001-$EA001F の奇数番地に SPC、$EA0020-$EA1FFF に SCSI の ROM
//   $EAFA00-$EAFA0F  MIDI ボード CZ-6BM1 (YM3802。setMidiBoard() でつないだときだけ。x68k/ym3802.h)
//   $ECC080-$ECC0FF  まーきゅりーゆにっと (setMercury() でつないだときだけ。$ECC000-$ECDFFF のほかはバスエラー)
//   $ECE000-$ECE3FF  ネットワークボード Neptune-X (setNeptune() でつないだときだけ。x68k/neptune.h)
//   $ED0000-$ED3FFF  SRAM (16KB)
//   $F00000-$FBFFFF  CGROM (768KB)
//   $FC0000-$FDFFFF  SCSI の ROM (内蔵 SCSI の機種だけ。なければバスエラー)
//                    (setScsiRom() で ROM を渡したときだけ。そのときは $E96021-$E9603F の奇数番地に内蔵 SCSI の SPC もつながる)
//   $FE0000-$FFFFFF  IPL ROM (128KB)
// ROM への書き込みはバスエラーにする。
//
// 68000 のアドレスバスは 24 本なので、$01000000 から上は $000000 からと同じ場所になる (readByte() の addr &= 0xffffff)。
// ハイメモリ (TS-6BE16 など) は 68000 からは見えないので、68000 の機種では Config::highMemSize を 0 にする。
//
// X68030 (Config::setCpu(68030)) では CPU が 68030 (m68k::Cpu020) になる。アドレスは 32 ビットだが、
// 本体の 16MB は 4GB の空間に繰り返し見えるとした (上位 8 ビットをデコードしない作り。実機では確かめていない。
// IPL ROM 1.3 と Human68k の起動の試験 x68k_boot_x68030 はこの作りで通る)。ハイメモリ (Config::highMemSize) は
// $01000000 から置き、繰り返さない。68030 のコアは PIX68K_NO_CPU020 を定義すると外せる (PIX68K_CPU020=0 のベアメタル版のビルド)。
//
// 060turbo (Config::setCpu(68060)) は、X68030 の CPU を 68060 に差し替えるボード。68060 と、ボードの SIMM の
// ローカルメモリ ($10000000 から Config::localMemSize。$10000000-$1FFFFFFF のうち SIMM のないところは、読むと $FF で
// 書いても変わらない)
// を持つ。IPL ROM は X68030 のもの (IPLROM30.DAT) を使う。
//
// ROM のファイルは環境 (ホスト / Pi) が読み、setIplRom() などで渡す。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "m68k/cpu020.h"
#include "m68k/m68k.h"
#include "x68k/devices.h"
#include "x68k/dmac.h"
#include "x68k/fdc.h"
#include "x68k/fpu_board.h"
#include "x68k/mercury.h"
#include "x68k/midi_out.h"
#include "x68k/neptune.h"
#include "x68k/ts6bga.h"
#include "x68k/ym3802.h"
#include "x68k/fdd.h"
#include "x68k/hdd.h"
#include "x68k/keyboard.h"
#include "x68k/mfp.h"
#include "x68k/mouse.h"
#include "x68k/ppi.h"
#include "x68k/rtc.h"
#include "x68k/sasi.h"
#include "x68k/scc.h"
#include "x68k/spc.h"
#include "x68k/sound.h"
#include "x68k/video.h"

namespace x68k {

class StateIO;

// Machine が、画面を合成する相手 (render.h の Renderer など) に、合成するタイミングを伝えるためのインタフェース。
// Machine::setLineListener() で渡す。呼び出しは Machine を動かしているスレッドから来る
class LineListener {
public:
    virtual ~LineListener() {}
    // 垂直表示期間のラスタごとに 1 回呼ばれる。呼ばれるのは、そのラスタの水平表示期間に入るところ。水平表示期間の
    // ないラスタ (CRTC の R02 >= R03、または R02 が R00 + 1 以上) では、そのラスタの終わり (次のラスタへ進む直前)。
    // このときの Video の状態で 1 ライン分を合成する。そこまでに CPU が変えたレジスタやパレットは、このラスタから反映される。
    // raster: 表示期間の最初のラスタ (CRTC R06) を 0 とした番号
    virtual void onRaster(int raster) = 0;
    // 垂直表示期間を抜けたところで、フレームごとに 1 回呼ばれる。そのフレームの onRaster() はすべて済んでいる
    virtual void onFrameEnd() = 0;
};

// 速さの内訳を測るための積算 (ベンチマーク用)。clock は単調に増える時刻を返す関数 (単位は任意)。
// 時間はすべて clock() の単位。CPU の時間は、全体から devices を引いたものになる
struct Profile {
    uint64_t (*clock)() = nullptr;
    uint64_t devices = 0; // 周辺機器を進める処理 (syncDevices) の時間。音の合成と LineListener (画面の合成) を含む
    uint64_t sound = 0;   // そのうち音の合成 (Sound::advance) の時間
    uint64_t syncs = 0;   // syncDevices の回数
    uint64_t slices = 0;  // run() が Cpu::run() を呼んだ回数
    uint64_t dmaBlockBytes = 0; // DMA でまとめて転送したバイト数 (Machine::dmaBlockToRam())
};

struct Config {
    uint32_t ramSize = 4 * 1024 * 1024; // 1MB 単位、最大 12MB
    uint32_t cpuHz = 16000000;          // XVI: 16MHz
    uint8_t cpuInfo = 0xfe;             // システムポート $E8E00B の値 (XVI)
    // SRAM のメインメモリの容量 ($ED0008) を、ramSize にそろえるかどうか。そろえないと、IPL ROM の初期値 (1MB) や
    // SRAM のファイルに残っていた値のままになり、Human68k が使うメモリがつないだ容量と食い違う
    bool fixSramMemorySize = true;      // 既定ではそろえる

    // CPU。M68030 なら X68030 (68030 と 68882)
    m68k::CpuModel cpuModel = m68k::CpuModel::M68000;
    // X68030 のハイメモリ (TS-6BE16 相当。$01000000 から。0 ならなし)。1MB 単位、最大 16MB。
    // 68000 (アドレスが 24 ビット) と 060turbo では使わない (Machine が 0 にする)
    uint32_t highMemSize = 0;
    // 060turbo のローカルメモリ ($10000000 から。0 ならなし)。1MB 単位、最大 256MB
    uint32_t localMemSize = 0;
    static constexpr uint32_t LOCAL_MEM_BASE = 0x10000000;
    static constexpr uint32_t LOCAL_MEM_MAX = 0x10000000;
    // 060turbo で、本体のバスに出す読み書き 1 回 (16 ビット) にかかるサイクル数 (68060 のクロックで数える)。
    // 060turbo の 68060 は、ローカルメモリ以外 (本体の RAM、VRAM、I/O) を X68030 の本体のバス (25MHz) を通して読み書きする。
    // 68030 の非同期のバスサイクルは最短 3 クロック (25MHz で 120ns) なので、50MHz の 68060 では 6 サイクル、75MHz では
    // 9 サイクルになる。本体のバスは 68060 のクロックを上げても 25MHz のままなので、時間 (120ns) は変わらない。
    // setCpu(68060) と setClockMHz() が busWait060For() の値にする (試験で変えるときは、そのあとに書く)。
    // 本体の RAM と ROM は、060turbo.sys がキャッシュを有効にしていれば (VRAM はキャッシュ禁止、I/O はシリアライズ。
    // 060turbo.sys の説明書のキャッシュモードの節)、このエミュレータはキャッシュに当たったものとしてサイクル数を足さない。
    // CACR でキャッシュを切っている間のメモリの読み書きは、ここの値ではなく Cpu020::updateCacheCosts() の値を足す。
    // サイクル数を足すのは Cpu020 の setFastPage() のメモリでないところ (I/O、グラフィック VRAM、テキスト VRAM への
    // 書き込み) だけ。I/O は、このあと busArea の表の区切りまで待つ (スプライトは bus_sync_sprite=on のときだけ)。VRAM は、本体のバスの側で延びる分が
    // 分からないので入れていない (最短の見積もり)
    int busWait060 = 6;
    // X68030 と 060turbo の本体のバスの待ち。場所の種類 (BusArea) ごとに、読みと書きで別に 3 つの値 (ns) を持つ:
    //   終わる時刻 = (始まる時刻 + pre) を period の倍数に切り上げ + post
    // (period が 0 なら切り上げない。060turbo の「始まる時刻」は、Cpu020 が busWait060 を足したあと)。
    // pre だけなら長さの決まった待ち、period だけなら本体の側の決まった時刻で終わる待ちになる。3 つとも 0 なら待たない。
    // 本体の側の時間なので、68060 のクロック (50 / 75MHz)、speed060、最高速モードでは変わらない。DMA の転送は待たせない。
    // period は、CPU のクロックで 240 サイクルを割り切る値にする (1600、800、400、200、100 など)。割り切れないと、
    // ステートを読んだときなどに時刻を直すと区切りの位置が動く (state.h の STATE_TIME_SHIFT_UNIT。
    // Machine::busSyncKeepsPhase())。
    // 表は setCpu() が既定の値にする (setBusSync())。変えるときは、そのあとに書く
    enum BusArea : uint8_t {
        BUS_AREA_NONE = 0, // 待たせない場所 (メインメモリ、SRAM、ROM など)
        BUS_AREA_GVRAM,    // グラフィック VRAM ($C00000-$DFFFFF)
        BUS_AREA_TVRAM,    // テキスト VRAM ($E00000-$E7FFFF)
        BUS_AREA_CRTC,     // CRTC ($E80000-$E81FFF)
        BUS_AREA_PALETTE,  // パレット ($E82000-$E823FF)
        BUS_AREA_VIDEO,    // ビデオコントローラ ($E82400-$E83FFF)
        BUS_AREA_DMAC,     // DMAC ($E84000-$E85FFF)
        BUS_AREA_AREASET,  // エリアセット ($E86000-$E87FFF)
        BUS_AREA_MFP,      // MFP ($E88000-$E89FFF)
        BUS_AREA_RTC,      // RTC ($E8A000-$E8BFFF)
        BUS_AREA_PRINTER,  // プリンタ ($E8C000-$E8DFFF)
        BUS_AREA_SYSPORT,  // システムポート ($E8E000-$E8FFFF)
        BUS_AREA_OPM,      // OPM ($E90000-$E91FFF)
        BUS_AREA_ADPCM,    // ADPCM ($E92000-$E93FFF)
        BUS_AREA_FDC,      // FDC ($E94000-$E95FFF)
        BUS_AREA_HDC,      // SASI と内蔵 SCSI ($E96000-$E97FFF)
        BUS_AREA_SCC,      // SCC ($E98000-$E99FFF)
        BUS_AREA_PPI,      // PPI ($E9A000-$E9BFFF)
        BUS_AREA_IOCTRL,   // I/O コントローラ ($E9C000-$E9DFFF)
        BUS_AREA_IO,       // そのほかの I/O ($E9E000-$EAFFFF。コプロセッサ、外付け SCSI ボード、MIDI ボードなど)
        BUS_AREA_SPRITE,   // スプライトのレジスタ ($EB0000-$EB07FF)
        BUS_AREA_BGCTRL,   // BG のスクロールレジスタと制御レジスタ ($EB0800-$EB7FFF)
        BUS_AREA_PCG,      // PCG ($EB8000-$EBBFFF)
        BUS_AREA_BG,       // BG のテキストエリア ($EBC000-$EBFFFF)
        BUS_AREA_EXT,      // 拡張スロットのボード ($EC0000-$ECFFFF)
        BUS_AREAS
    };
    struct BusTiming {
        int pre = 0, period = 0, post = 0; // ns
    };
    struct BusAreaTiming {
        BusTiming read, write;
    };
    BusAreaTiming busArea[BUS_AREAS];
    // pre、period、post の上限 (1 ミリ秒)。負の値は 0、これより大きい値はこの値として扱う (busNsToSub())
    static constexpr int BUS_NS_MAX = 1000000;
    // ns を、クロック hz の CPU の 1/SUB サイクル単位に直す (端数は切り捨て)。掛け算があふれないよう、
    // hz x SUB を 10^9 で割った商と余りに分けて計算する
    static uint64_t busNsToSub(uint32_t hz, int ns) {
        if (ns <= 0) return 0;
        if (ns > BUS_NS_MAX) ns = BUS_NS_MAX;
        const uint64_t perSecond = (uint64_t)hz * m68k::Cpu020::SUB; // 2^32 x 12800 より小さい
        return perSecond / 1000000000u * (uint64_t)ns + perSecond % 1000000000u * (uint64_t)ns / 1000000000u;
    }
    // "名前=pre,period,post" (名前は busAreaName()。名前のあとに .r か .w を付けると読みか書きだけ) で表の 1 つを変える。
    // 値は 0 から BUS_NS_MAX までの 10 進数。形が違うとき、名前がないとき、値が範囲の外のときは false で、何も変えない
    bool setBusAreaText(const char* text) {
        const char* eq = text;
        const char* dot = nullptr; // '=' より前の初めの '.'
        for (; *eq && *eq != '='; eq++)
            if (*eq == '.' && !dot) dot = eq;
        if (!*eq) return false;
        const size_t nameLen = (size_t)((dot ? dot : eq) - text);
        int area = 0;
        for (int a = 1; a < BUS_AREAS; a++)
            if (strlen(busAreaName(a)) == nameLen && !memcmp(busAreaName(a), text, nameLen)) area = a;
        if (!area) return false;
        char rw = 0;
        if (dot) {
            if (eq - dot != 2 || (dot[1] != 'r' && dot[1] != 'w')) return false;
            rw = dot[1];
        }
        int v[3];
        const char* p = eq + 1;
        for (int i = 0; i < 3; i++) {
            if (*p < '0' || *p > '9') return false;
            int n = 0;
            for (; *p >= '0' && *p <= '9'; p++) {
                n = n * 10 + (*p - '0');
                if (n > BUS_NS_MAX) return false; // ここで止めるので、n はあふれない
            }
            if (*p != (i < 2 ? ',' : '\0')) return false;
            p++;
            v[i] = n;
        }
        BusTiming t;
        t.pre = v[0];
        t.period = v[1];
        t.post = v[2];
        if (rw != 'w') busArea[area].read = t;
        if (rw != 'r') busArea[area].write = t;
        return true;
    }
    // addr (24 ビット) の場所の種類。1KB ごとに決まる (BUS_AREA_SHIFT)
    static constexpr int BUS_AREA_SHIFT = 10;
    static BusArea busAreaAt(uint32_t addr) {
        if (addr < 0xc00000 || addr >= 0xed0000) return BUS_AREA_NONE; // Machine::BUS_AREA_BASE、BUS_AREA_END と同じ
        if (addr < 0xe00000) return BUS_AREA_GVRAM;
        if (addr < 0xe80000) return BUS_AREA_TVRAM;
        if (addr >= 0xec0000) return BUS_AREA_EXT;
        if (addr >= 0xebc000) return BUS_AREA_BG;
        if (addr >= 0xeb8000) return BUS_AREA_PCG;
        if (addr >= 0xeb0800) return BUS_AREA_BGCTRL;
        if (addr >= 0xeb0000) return BUS_AREA_SPRITE;
        if (addr >= 0xe9e000) return BUS_AREA_IO;
        static constexpr BusArea io[15] = {BUS_AREA_CRTC,    BUS_AREA_PALETTE, BUS_AREA_DMAC, BUS_AREA_AREASET, BUS_AREA_MFP,
                                           BUS_AREA_RTC,     BUS_AREA_PRINTER, BUS_AREA_SYSPORT, BUS_AREA_OPM,  BUS_AREA_ADPCM,
                                           BUS_AREA_FDC,     BUS_AREA_HDC,     BUS_AREA_SCC,  BUS_AREA_PPI,     BUS_AREA_IOCTRL};
        if (addr >= 0xe82400 && addr < 0xe84000) return BUS_AREA_VIDEO;
        return io[(addr - 0xe80000) >> 13];
    }
    // 試験と測定のための名前 (x68k_boot の --bus-area)
    static const char* busAreaName(int area) {
        static const char* const names[BUS_AREAS] = {
            "none", "gvram", "tvram",   "crtc", "palette", "video", "dmac", "areaset", "mfp",    "rtc",    "printer", "sysport", "opm",
            "adpcm", "fdc",  "hdc",     "scc",  "ppi",     "ioctrl", "io",  "sprite",  "bgctrl", "pcg",    "bg",      "ext"};
        return area >= 0 && area < BUS_AREAS ? names[area] : "";
    }
    // 表を、areas (BUS_SYNC_* の和) の場所の読み書きが ns ごとの区切りで終わる値にする (ns が 0 なら、どこも待たせない)。
    // 既定は 1600ns で I/O だけ (BUS_SYNC_DEFAULT)。スプライトもそろえるなら setBusSyncSprite(true)。
    // 実機の si.r の system performance (RTC の $E8A001 を読むループ) は、X68030、040turbo、060turbo のどれでも、
    // 命令キャッシュ / データキャッシュの有効 / 無効によらず 177.82% で、1 周がちょうど 1.6 マイクロ秒 (毎秒 625000 周)
    // になる。CPU の側の時間 (1 周 0.04 から約 1 マイクロ秒) を足した値にならないので、I/O の読み書きは CPU が出した
    // 時刻からの長さではなく、本体の側の 1.6 マイクロ秒ごと (10MHz の 16 クロック) の時刻で終わるものとした。
    // 読み書き 1 回は、060turbo では busWait060 (最短のバスサイクル) を足したあと、次の区切りまで待つ
    // (Machine::busWait()。出どころと仮定は docs/060turbo.md の「X68030 と 060turbo の本体のバス」)。68030 では、最短のバスサイクル
    // (3 クロック = 120ns) を pre に入れる (足さないと、32 ビットの読み書きの 2 回目が 1 回目と同じ区切りで終わってしまう)。
    // 実機の値があるのは RTC だけで、ほかの I/O ($E80000-$EAFFFF、$EC0000-$ECFFFF) は RTC と同じとした。
    // スプライト ($EB0000-$EBFFFF) も RTC と同じとする値 (BUS_SYNC_SPRITE) は、測っていない仮定で、既定では入れない:
    // 入れると、スプライトを多く書くゲーム (CBX68) が 1 フレームの間に書き終えられず、画面が崩れた (#218)。
    // pix68k.cfg の bus_sync_sprite=on (ホスト版は --bus-sync-sprite) で入れられる。グラフィック VRAM と
    // テキスト VRAM も、実機の値がないので入れていない (busWait060 だけ。試験と測定のために BUS_SYNC_VRAM で入れられる)
    static constexpr uint8_t BUS_SYNC_IO = 1;
    static constexpr uint8_t BUS_SYNC_SPRITE = 2;
    static constexpr uint8_t BUS_SYNC_VRAM = 4;
    static constexpr uint8_t BUS_SYNC_DEFAULT = BUS_SYNC_IO;
    static constexpr int BUS_SYNC_NS = 1600;
    void setBusSync(int ns, uint8_t areas = BUS_SYNC_DEFAULT) {
        BusTiming t;
        if (ns > 0) {
            t.period = ns;
            if (cpuModel == m68k::CpuModel::M68030) t.pre = 120;
        }
        for (int i = 1; i < BUS_AREAS; i++) {
            const uint8_t bit = i <= BUS_AREA_TVRAM ? BUS_SYNC_VRAM
                                : i >= BUS_AREA_SPRITE && i <= BUS_AREA_BG ? BUS_SYNC_SPRITE : BUS_SYNC_IO;
            busArea[i].read = busArea[i].write = areas & bit ? t : BusTiming();
        }
    }
    // スプライト ($EB0000-$EBFFFF) の読み書きも I/O と同じ 1.6 マイクロ秒ごとの区切りにそろえるか (既定はそろえない)。
    // 表を既定の値に作り直すので、setCpu() のあと、busArea を書き換える前に呼ぶ。68000 では何もしない
    void setBusSyncSprite(bool on) {
        if (cpuModel == m68k::CpuModel::M68000) return;
        setBusSync(BUS_SYNC_NS, on ? (uint8_t)(BUS_SYNC_IO | BUS_SYNC_SPRITE) : BUS_SYNC_DEFAULT);
    }
    // 060turbo の 68060 の命令の速さ (選んだクロック (cpuHz) の実機の 68060 を 100 とした %。SPEED060_MIN から
    // Machine::SPEED_MAX)。100 で、命令のサイクル数が MC68060UM 10 章の表どおりになる (m68k/cpu060_cycles.inc)。
    // 75MHz の 100 は、50MHz の 150 と同じ命令の速さ (本体のバスの時間は除く)。100 より小さくすると、
    // 実機より遅くなるかわりにホストの負荷が下がる。Machine::setCpuSpeed() の下限もこの値になる
    int speed060 = 100;
    static constexpr int SPEED060_MIN = 5;
    // 060turbo で、Machine::setCpuSpeed() (最高速モード) が上げてよい速さの上限 (speed060 と同じ単位)。
    // 0 なら Machine::SPEED_MAX まで。100 にすると、実機の速さを超えない
    int maxSpeedLimit060 = 0;
    // 68000 の機種で、メインメモリ以外 (VRAM、I/O、SRAM、ROM) の読み書きのウェイトと、メインメモリの DRAM リフレッシュで
    // 遅れる分を入れる (Machine::setupBusWaits() の注釈)。false なら入れない (#71 より前の速さ)
    bool busWait68000 = true;

    // 68030 のコアが入っているか (PIX68K_NO_CPU020 のビルドでは入っていない)
    static bool has020() {
#ifdef PIX68K_NO_CPU020
        return false;
#else
        return true;
#endif
    }

    // 060turbo の 68060 のクロック (MHz)。50 が実機の 060turbo、75 は実機の 060turbo をオーバークロックして使う構成。
    // ほかのクロックを足すときは、ここに足す (時間で決まる値は cpuHz と busWait060For() から決まる)
    static bool valid060MHz(int mhz) { return mhz == 50 || mhz == 75; }
    // 本体のバスの読み書き 1 回 (25MHz の 3 クロック) を、cpuHz の 68060 のサイクルに直した数 (50MHz で 6、75MHz で 9)
    static int busWait060For(uint32_t hz) { return (int)((uint64_t)hz * 3 / 25000000); }

    // CPU を選ぶ: 68000 か 68030 (X68030: 25MHz、システムポートの値は $DC) か 68060 (060turbo: 50MHz、
    // システムポートは X68030 と同じ $DC。75MHz は、そのあとに setClockMHz(75))。ほかの値や、
    // 68030 のコアが入っていないビルドでは false で、何も変えない。
    // 68030 から 68000 に戻すときは XVI (16MHz、$FE) に戻す (68000 のままなら setClockMHz() で選んだクロックを残す)
    bool setCpu(int model) {
        if (model == 68000) {
            if (cpuModel != m68k::CpuModel::M68000) {
                cpuHz = 16000000;
                cpuInfo = 0xfe;
            }
            cpuModel = m68k::CpuModel::M68000;
        } else if (model == 68030 && has020()) {
            cpuModel = m68k::CpuModel::M68030;
            cpuHz = 25000000;
            cpuInfo = 0xdc;
            setBusSync(BUS_SYNC_NS);
        } else if (model == 68060 && has020()) {
            if (cpuModel != m68k::CpuModel::M68060) cpuHz = 50000000; // 68060 のままなら setClockMHz() で選んだクロックを残す
            cpuModel = m68k::CpuModel::M68060;
            cpuInfo = 0xdc;
            busWait060 = busWait060For(cpuHz);
            setBusSync(BUS_SYNC_NS);
        } else {
            return false;
        }
        return true;
    }

    // CPU のクロックを MHz で選ぶ。68000 では 10 (初代〜SUPER、68000 10MHz) か 16 (XVI)、68060 (060turbo) では
    // 50 か 75 (valid060MHz())。ほかの値なら false で、何も変えない (X68030 は setCpu(68030) で 25MHz のまま。
    // 68030 のときは false で、何も変えない)。CPU は先に setCpu() で選んでおく
    bool setClockMHz(int mhz) {
        if (cpuModel == m68k::CpuModel::M68060) {
            if (!valid060MHz(mhz)) return false;
            cpuHz = (uint32_t)mhz * 1000000;
            busWait060 = busWait060For(cpuHz);
            return true;
        }
        if (cpuModel != m68k::CpuModel::M68000) return false;
        if (mhz == 10) {
            cpuHz = 10000000;
            cpuInfo = 0xff;
        } else if (mhz == 16) {
            cpuHz = 16000000;
            cpuInfo = 0xfe;
        } else {
            return false;
        }
        return true;
    }
};

class Machine : public m68k::Bus, private Dmac::Host {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    static constexpr uint32_t IPL_SIZE = 0x20000;
    static constexpr uint32_t CG_SIZE = 0xc0000;
    static constexpr uint32_t SCSI_ROM_SIZE = 0x20000;
    static constexpr uint32_t SRAM_SIZE = 0x4000;
    static constexpr uint32_t SCSI_EX_ROM_SIZE = 0x2000; // 外付け SCSI の ROM の範囲 ($EA0000-$EA1FFF)
    static constexpr uint32_t SCSI_EX_BASE = 0xea0000;
    // 外付け SCSI の SPC の割り込み: 拡張スロットの割り込みレベル 2 (IRQ2)、ベクタ $F6。
    // アドレス ($EA0000-$EA001F が SPC、$EA0020-$EA1FFF が ROM) とこの割り込みは、外付け SCSI ボードの ROM で
    // SCSI のハードディスクから起動でき (x68k_boot_scsi_ex。SRAM の ROM 起動のアドレスは $EA0020)、CD-ROM を読める
    // (x68k_scsi_ex_cdrom) 値にした。実機のボードと資料では確かめていない
    static constexpr int SCSI_EX_IRQ_LEVEL = 2;
    static constexpr int SCSI_EX_VECTOR = 0xf6;

    explicit Machine(const Config& config = Config());
    ~Machine() override;
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // メモリが確保できたか (ベアメタルでは new が失敗しうる)
    bool ok() const {
        return ram_ && ipl_ && cg_ && video_ && (config_.cpuModel == m68k::CpuModel::M68000 || cpu020_) &&
               (!config_.highMemSize || highMem_) && (!config_.localMemSize || localMem_);
    }

    // ROM を渡す。大きさが違えば false
    bool setIplRom(const uint8_t* data, size_t size);
    bool setCgRom(const uint8_t* data, size_t size);
    bool setScsiRom(const uint8_t* data, size_t size); // 内蔵 SCSI の機種だけ
    // X68030 の内蔵 SCSI の ROM ($FC0000、128KB) を IPL ROM から作る。X68030 の SCSI のドライバは IPL ROM にあり、
    // $FC0000 にはその入り口のアドレス (IPL ROM の $FF0020 と $FF0024 にある) と 'SCSIIN' の文字列だけがあればよい。
    // out は SCSI_ROM_SIZE バイト。入り口が見つからなければ全部 $FF (内蔵 SCSI なし) にする
    static void makeScsiRom30(const uint8_t* ipl, uint8_t* out);
    // SRAM の中身。環境がファイルから読み、dirty() なら書き戻す。
    // 電源を入れる (powerOn()) 前に、ファイルの中身をここへ写しておく。電源投入時の自動補正で書き換えるのは、
    // 設定で有効にしたときのメモリの容量の 4 バイトと、060turbo のキャッシュの設定の 1 バイトだけで、
    // メモリの容量を書き換えたときは sramDirty() が true になる (machine.cpp の powerOn() を参照)
    uint8_t* sram() { return sram_; }
    uint8_t* mainRam() { return ram_; }
    bool sramDirty() const { return sramDirty_; }
    void clearSramDirty() { sramDirty_ = false; }
    // フロッピーディスク: イメージは環境が持ち、ここは指すだけ (drive は 0..3)
    void insertDisk(int drive, FloppyImage* image);
    void ejectDisk(int drive);
    Fdc& fdc() { return fdc_; }
    // SASI のハードディスク。イメージ (HDF、1 レコード 256 バイト) は環境が持ち、Machine はポインタだけを持つ。
    // unit は ID x 2 + 論理ユニット番号 (0..15)。disk を nullptr にすると外れる
    void attachSasi(int unit, HardDiskImage* image);
    Sasi& sasi() { return sasi_; } // SASI のインタフェース ($E96001-$E96007)
    // 内蔵 SCSI のハードディスク (HDS)。setScsiRom() で SCSI の ROM を渡した機種だけで、そのときは
    // $E96020-$E9603F に SPC (MB89352) が見える。id は 0..7。cdrom なら CD-ROM 装置 (toc が nullptr なら ISO、
    // 2048 バイト / ブロック。toc があれば CUE シートなどのトラックの並び。x68k/cd_drive.h)
    void attachScsi(int id, HardDiskImage* image, bool cdrom = false, const CdToc* toc = nullptr) {
        spc_.attach(id, image, cdrom, toc);
        cdChanged();
    }
    Spc& spc() { return spc_; } // 内蔵 SCSI の SPC
    // 外付け SCSI ボード (CZ-6BS1 相当)。ROM を渡すとボードがつながり、$EA0000-$EA1FFF に SPC と ROM が見える。
    // SPC は DMAC にはつながない (データはプログラム転送で受け渡す。DMA は作っていない)。
    // ROM のファイルは $EA0000 からの 8192 バイトか、$EA0020 からの 8160 バイト。ほかの大きさ (8192 バイトまで) は、
    // 'SCSIEX' が $EA0044 に来るように置く (ファイルの $24 にあれば $EA0020 から、なければ $EA0000 から)。
    // 足りないところは $FF。大きさが 0 か 8192 を超えるなら false
    bool setScsiExRom(const uint8_t* data, size_t size);
    bool hasScsiEx() const { return scsiExRom_ != nullptr; }
    void attachScsiEx(int id, HardDiskImage* image, bool cdrom = false, const CdToc* toc = nullptr) {
        spcEx_.attach(id, image, cdrom, toc);
        cdChanged();
    }
    // CD-ROM 装置 (外付け SCSI ボードにあればそちら、なければ内蔵 SCSI のもの。なければ nullptr)。
    // オーディオの再生は、この 1 台の音だけを出す
    CdDrive* cdDrive() { return cdDrive_; }
    // CD-ROM 装置の媒体を入れ替える (image が nullptr なら取り出すだけ)。X68000 には、次のコマンドで UNIT ATTENTION
    // (媒体が替わったかもしれない) が返る。CD-ROM 装置がなければ false。X68000 を止めている間に呼ぶこと
    bool changeCd(HardDiskImage* image, const CdToc* toc = nullptr) {
        if (!cdDrive_) return false;
        return spcEx_.cd() == cdDrive_ ? spcEx_.changeCd(image, toc) : spc_.changeCd(image, toc);
    }
    Spc& spcEx() { return spcEx_; }
    // 数値演算プロセッサボード (CZ-6BP1 相当、x68k/fpu_board.h)。つなぐと $E9E000-$E9E01F に MC68881 の CIR が見える。
    // setFpuBoards(2) なら 2 枚目も $E9E080-$E9E09F につなぐ (2 枚目だけはつながない)。count は 0 から 2。
    // 確保できなければ false。setFpuBoard(on) は setFpuBoards(on ? 1 : 0) と同じ
    bool setFpuBoards(int count);
    bool setFpuBoard(bool on) { return setFpuBoards(on ? 1 : 0); }
    int fpuBoardCount() const { return fpuBoards_[1] ? 2 : fpuBoards_[0] ? 1 : 0; }
    FpuBoard* fpuBoard(int i = 0) { return i >= 0 && i < FpuBoard::MAX_BOARDS ? fpuBoards_[i] : nullptr; }
    // まーきゅりーゆにっと (x68k/mercury.h)。つなぐと $ECC080- にレジスタが見え、DMAC のチャネル 2 で PCM を受け取る。
    // 確保できなければ false
    bool setMercury(bool on);
    Mercury* mercury() { return mercury_; }
    // ネットワークボード Neptune-X (x68k/neptune.h)。つなぐと $ECE000-$ECE3FF に NE2000 互換のコントローラが見え、
    // IRQ2 (ベクタ $F9) で割り込む。フレームの送受信の口は neptune()->setLink() で渡す。確保できなければ false
    bool setNeptune(bool on);
    Neptune* neptune() { return neptune_; }
    bool setTs6bga(bool on, uint32_t base = 0xee0000);
    Ts6bga* ts6bga() { return ts6bga_; }
    // MIDI ボード CZ-6BM1 (x68k/ym3802.h)。つなぐと $EAFA01-$EAFA0F の奇数番地に YM3802 が見え、IRQ4 で割り込む。
    // 送り終えたバイトは Sound を通して setMidiOut() の音源に届く (音の合成を別のコアで行うときは、そのコアの音源)。
    // 確保できなければ false
    bool setMidiBoard(bool on);
    Ym3802* midiBoard() { return midi_; }
    // MIDI の音源。音をこの Machine の Sound で作るとき (sound().setOutputRate() が 0 でない) は、出力のフレームごとに
    // その音を足す。nullptr なら送ったバイトは捨てる。動いている間にほかのコアから設定してよい (ベアメタル版は
    // SoundFont を X68000 を動かし始めてから読み、読み終えたら設定する)。設定したあとは out を消さないこと。
    // resync なら、次に MIDI ボードを進めるときに、それまでに送ったバイトから覚えた音源の設定 (x68k/midi_shadow.h) を
    // out に送り直す (動いている途中でつないだ音源を、それまでのプログラムチェンジなどに合わせる)
    void setMidiOut(MidiOut* out, bool resync = false) {
        __atomic_store_n(&midiOut_, out, __ATOMIC_RELEASE);
        if (resync) __atomic_store_n(&midiResync_, true, __ATOMIC_RELEASE);
    }
    MidiOut* midiOut() const { return __atomic_load_n(&midiOut_, __ATOMIC_ACQUIRE); }
    // RTC の基準の時刻 (1970 年からの秒数、地方時)
    void setTime(int64_t unixSeconds) { rtc_.setTime(unixSeconds); }

    // 電源投入: 周辺機器を初期化し、IPL ROM のベクタから起動する
    void powerOn();
    // CPU のサイクル数で n だけ進める
    void run(int64_t cycles);
    // 1 フレーム (V-DISP の立ち上がりから次の立ち上がりまで) 進める。初めに rebaseTime() を呼ぶ
    void runFrame();
    // 時刻 (CPU のサイクル数) が上限 (state.h の STATE_CPU020_CYCLES_MAX、68000 は STATE_CPU_CYCLES_MAX) を超えていれば、
    // ステートを読むときと同じ値 (約 2^40 サイクル + 上限の半分で割った余り) まで、CPU と周辺機器の時刻を同じだけ戻す。
    // 時刻は 64 ビットで、68030 / 68060 は 1/m68k::Cpu020::SUB サイクルの単位で数えるので、続けて動かすとあふれる
    // (75MHz で約 222 日。あふれると Cpu020::run() の進める先の時刻が回り込み、Machine::run() が進まなくなる)。#165
    void rebaseTime();
    // Config::busArea の周期がどれも、時刻を直しても区切りの位置が動かない値か (X68030 と 060turbo。setupBusWaits())
    bool busSyncKeepsPhase() const { return busSyncKeepsPhase_; }
    // 時刻を cycles サイクル戻す (cycles() 以下。試験でも使う)。時刻で持っている値どうしの差は変わらないので、続きは
    // 戻さなかったときと同じに動く (今より約 2^40 サイクル以上前の値は 0 になるが、どれも「とうに過ぎた時刻」の意味)。
    // run() / runFrame() の外で呼ぶ
    void shiftTime(uint64_t cycles);
    // これまでに shiftTime() で戻したサイクル数の合計と、それを足した、戻さなかったときの時刻。フロントエンドが
    // 実時間と合わせるときは、戻した分で差が狂わないよう totalCycles() を使う
    uint64_t timeShifted() const { return timeShifted_; }
    uint64_t totalCycles() const { return cycles() + timeShifted_; }

    // 68000 のコア。X68030 のときは動いていないので、下の cycles() などを使う
    m68k::Cpu& cpu() { return cpu_; }
    // 68030 のコア (X68030 でなければ nullptr)
    m68k::Cpu020* cpu020() { return use020() ? cpu020_ : nullptr; }
    // 動いている CPU のサイクル数、レジスタ、停止
    // (68000 を速さ 100 以外で動かしている間は、CPU が進めたサイクル数を X68000 のクロックに直した値。scaledCycles())
    uint64_t cycles() const { return use020() ? cpu020_->cycles() : inScaledSlice_ ? scaledCycles() : cpu_.cycles(); }
    void getRegisters(m68k::Registers& r) const;
    void setRegisters(const m68k::Registers& r);
    bool halted() const { return use020() ? cpu020_->halted() : cpu_.halted(); }
    // CPU の命令の速さ (100 で既定の速さ、200 でクロックを 2 倍にしたのと同じ。100 から SPEED_MAX)。
    // 060turbo では、選んだクロック (Config::cpuHz) の実機の 68060 を 100 とした値で、範囲は Config::speed060 から
    // Config::maxSpeedLimit060 (0 なら SPEED_MAX) まで。
    // 周辺機器と画面の時間は変わらない。68030 / 68060 は m68k::Cpu020::setSpeed で命令のサイクル数を減らす。
    // 68000 はコアを変えずに、X68000 の時間の percent/100 倍のサイクル数を CPU に走らせ、走ったサイクル数を
    // 100/percent 倍して X68000 の時間に直す (runCpu000Scaled())。
    // run() / runFrame() の外 (フレームの切れ目など) で呼ぶ
    static constexpr int SPEED_MAX = 3200;
    void setCpuSpeed(int percent);
    int cpuSpeed() const;
    Video& video() { return *video_; }
    void setLineListener(LineListener* listener) { lineListener_ = listener; }
    // 速さの内訳を測る。nullptr で止める
    void setProfile(Profile* profile) { profile_ = profile; }
    // 試験用: false なら DMA の転送をまとめず、1 バイトずつ転送する (まとめた転送と結果を比べるため。既定は true)
    void setDmaBlock(bool on) { dmaBlock_ = on; }
    Mfp& mfp() { return mfp_; }
    // キーボード。キーだけなら keyboard().onKey() (platform::InputSink) に渡してもよい
    Keyboard& keyboard() { return keyboard_; }
    // 音 (OPM と ADPCM)。sound().setOutputRate() で出力の周波数を決めると音を作り、sound().ring() にためる
    Sound& sound() { return sound_; }
    // ホストの入力 (キー、マウス、ジョイスティック) の受け口
    platform::InputSink& input() { return input_; }
    Mouse& mouse() { return mouse_; }
    Scc& scc() { return scc_; }
    // RS-232C (SCC のチャネル A) の相手 (x68k/serial_link.h)。nullptr ならつながっていない。消す前に nullptr にすること
    void setSerialLink(SerialLink* link) { scc_.setLink(link); }
    Ppi& ppi() { return ppi_; }
    uint32_t cpuHz() const { return config_.cpuHz; }
    // フロントエンドに通知する CRTC の周期。HRL のドットクロックの切替も反映する。
    double frameRate() const {
        return (double)video_->dotClock(sysPort_.hrl()) / video_->horizontalTotalDots() / video_->verticalTotal();
    }
    const Config& config() const { return config_; }
    const uint8_t* iplRom() const { return ipl_; }
    const uint8_t* cgRom() const { return cg_; }
    const uint8_t* scsiRom() const { return scsiRom_; }     // 内蔵 SCSI の ROM (なければ nullptr)
    const uint8_t* scsiExRom() const { return scsiExRom_; } // 外付け SCSI ボードの ROM (なければ nullptr)
    uint64_t frameCount() const { return frames_; }
    int rasterLine() const { return line_; }

    // ---- m68k::Bus ----
    uint8_t read8(uint32_t addr, int fc) override;
    uint16_t read16(uint32_t addr, int fc) override;
    void write8(uint32_t addr, uint8_t value, int fc) override;
    void write16(uint32_t addr, uint16_t value, int fc) override;
    int interruptAck(int level) override;
    void resetDevices() override;

    // バスエラーを起こさずに読む (デバッガ、テスト用)。読めない場所は ok = false
    uint8_t peek8(uint32_t addr, bool& ok);

private:
    // 読み書きの結果。BUS_ERROR ならバスエラー
    static constexpr int BUS_ERROR = -1;
    int readByte(uint32_t addr, int fc);
    bool writeByte(uint32_t addr, uint8_t v, int fc);
    int ioRead(uint32_t addr);
    bool ioWrite(uint32_t addr, uint8_t v);
    int boardRead(uint32_t addr);
    // 数値演算プロセッサボードの選択。2 枚目の CIR は 1 枚目から $80 だけ上 (FpuBoard::BASE2)。
    // $E9E000-$E9FFFF のうち、2 つの CIR の外 ($E9E020-$E9E07F、$E9E0A0 から上) と、つないでいないボードの CIR は
    // バスエラーにする。実機で確かめたものではなく、1 枚だけのときの今までの振る舞い ($E9E020 から上はバスエラー) に
    // そろえた推測 (ボードは自分の CIR だけに DTACK を返し、ほかはどこも返さないとした)
    static constexpr uint32_t FPU_BOARD_SELECT = FpuBoard::BASE2 - FpuBoard::BASE; // $80
    // off は $E9E000 からの位置。CIR に当たり、そのボードをつないでいれば返す
    FpuBoard* fpuBoardAt(uint32_t off) const {
        if ((off & ~(FPU_BOARD_SELECT | (FpuBoard::SIZE - 1))) != 0) return nullptr;
        return fpuBoards_[(off & FPU_BOARD_SELECT) ? 1 : 0];
    }
    bool boardWrite(uint32_t addr, uint8_t v);
    bool userBlocked(uint32_t addr, int fc) const;
    bool use020() const { return Config::has020() && cpu020_; }
    [[noreturn]] void busError(uint32_t addr, bool write, int fc);
    void setFastPage(bool supervisor, int page, const uint8_t* read, uint8_t* write);
    void patchIplFor060();
    void setupBusWaits();
    static constexpr uint32_t REFRESH_PERIOD = 45; // DRAM リフレッシュでこのクロック数ごとに 1 クロック遅れる (refreshWait())
    void refreshWait(uint64_t start);
    // 68000 のバスの読み書き 1 回のウェイトを足す (setupBusWaits())。addr は 24 ビットにマスクしたもの
    // X68030 と 060turbo では、場所の種類ごとの表 (Config::busArea) の時間だけ待たせる
    void busWait(uint32_t addr, bool write) {
        if (!busWaits_) {
#ifndef PIX68K_NO_CPU020
            // 外付け SCSI ボードの ROM ($EA0020-$EA1FFF) は、ほかの ROM と同じく待たせない (同じ 8KB の始めの SPC は待たせる)
            // 表は $C00000-$ECFFFF の分だけ持つ (それより下は引き算で大きい値になり、範囲の外になる)
            const uint32_t block = (addr - BUS_AREA_BASE) >> Config::BUS_AREA_SHIFT;
            const uint8_t area = block < sizeof busAreaOf_ ? busAreaOf_[block] : 0;
            if (area && addr - (SCSI_EX_BASE + 0x20) >= SCSI_EX_ROM_SIZE - 0x20) {
                // 終わる時刻 = (始まる時刻 + pre) を period の倍数に切り上げ + post (Config::busArea)
                const BusWaitSub& w = busWaitSub_[area][write];
                uint64_t add = w.pre;
                if (w.period) {
                    const uint64_t r = (cpu020_->subCycles() + add) % w.period;
                    if (r) add += w.period - r;
                }
                add += w.post;
                if (add) cpu020_->addSubCycles(add);
            }
#endif
            return;
        }
        const uint32_t q = waitFrac_ + (write ? waitWrite_ : waitRead_)[addr >> 13];
        cpu_.addCycles((int)(q >> 2));
        waitCycles_ += q >> 2;
        waitFrac_ = q & 3;
    }

    // ---- Dmac::Host ----
    uint32_t dmaMemAddr(int ch, uint32_t mar) override;
    bool dmaRead8(uint32_t addr, uint8_t& v, int fc) override;
    bool dmaWrite8(uint32_t addr, uint8_t v, int fc) override;
    bool dmaReady(int ch) override;
    void dmaDone(int ch) override;
    void dmaError(int ch) override;
    int dmaReadBlock(int ch, uint32_t dar, int dfc, uint32_t mar, int mfc, int n) override;
    int dmaReadBlockAuto(int ch, uint32_t dar, int dfc, uint32_t mar, int mfc, int n) override;
    int dmaBlockToRam(int ch, bool external, uint32_t dar, int dfc, uint32_t mar, int mfc, int n);
    int dmaBlockCopy(int ch, bool external, uint32_t dar, int dfc, uint32_t mar, int mfc, int n);
    // DMA を進める。DMA の転送が MFP などに書いて syncDevices() に戻ってきたときは、何もしない
    void runDma();

    template <class C> void runCpu(C& cpu, int64_t cycles, int64_t maxSlice, bool timers);
    void runCpu000Scaled(int64_t cycles); // 68000 を速さ 100 以外で動かす (setCpuSpeed())
    uint64_t scaledCycles() const {
        return sliceStart_ + (((cpu_.cycles() - sliceStart_) * speedMul_ + speedFrac_) >> SPEED_FRAC_BITS);
    }
    void updateIrq();
    void updateFastPages(); // CPU が Bus を通さずに読み書きするページを決め直す
    void updateSoundLines(); // OPM の割り込み、CT1 / CT2、8255 のポート C を、つながっている先に伝える
    void syncDevices() { syncDevices(cycles()); } // CPU が進んだ分だけ周辺機器を進める
    void syncDevices(uint64_t now);                // now は今の CPU のサイクル数 (runCpu() は CPU から直接渡す)
    void advanceDevices(uint64_t now);             // syncDevices() の中身
    void startLine();   // ラスタの始まりの処理
    void computeLine(); // 今のラスタの長さと、次のラスタの始まりを決める
    void horizontalDisplayTimes(uint64_t& start, uint64_t& end) const;
    enum LineEvent { EV_DISPLAY_START, EV_DISPLAY_END, EV_LINE };
    LineEvent pendingEvent(uint64_t& when) const;
    uint64_t nextEvent() const;
    // MFP のタイマ (ディレイモード) と OPM のタイマが、次に割り込みを要求する時刻 (CPU のサイクル数)。なければ UINT64_MAX。
    // 68030 / 68060 の runCpu() が、nextEvent() と合わせて、CPU を止めて周辺機器を進める時刻にする
    uint64_t nextTimerIrq() const;
    // タイマの設定が変わった: 走っている CPU を今の命令で止め、周辺機器を次に進める時刻を決め直す (68030 / 68060 だけ)
    void timersChanged() {
#ifndef PIX68K_NO_CPU020
        if (cpu020_) cpu020_->endRun();
#endif
    }
    void beginHorizontalBlank();
    void syncMidi(uint64_t now); // MIDI ボードを now まで進め、送り終えたバイトを Sound に渡す
    // 音源の音を止め、覚えた音源の設定を送り直す (ステートを読んだとき、setMidiOut() の resync)
    void resyncMidi(MidiOut* out);
    // CD-ROM 装置をつないだ / 外した
    void cdChanged() {
        cdDrive_ = spcEx_.cd() ? spcEx_.cd() : spc_.cd();
        sound_.setCdOn(cdDrive_ != nullptr);
    }
    void feedCdAudio(CdDrive& cd, uint32_t clocks4M); // 再生中のオーディオのフレームを Sound に渡す

    Config config_;
    m68k::Cpu cpu_;
    uint8_t* ram_ = nullptr;
    uint8_t* ipl_ = nullptr;
    uint8_t* cg_ = nullptr;
    uint8_t* scsiRom_ = nullptr; // なければ nullptr
    uint8_t sram_[SRAM_SIZE];
    bool sramDirty_ = false;
    // 空の SRAM を IPL ROM が初期化する間、メインメモリの容量への書き込みを差し替えるための状態 (ステートに入る)。
    // sramInitPending_ は差し替えている間だけ true。sramInitWritten_ は、先頭の識別子がそろってから差し替えた
    // バイトの記録で、ビット n が $ED0008 + n に当たる
    bool sramInitPending_ = false;
    uint8_t sramInitWritten_ = 0;
    void storeSram(uint32_t offset, uint8_t v);
    void writeSram(uint32_t offset, uint8_t v);
    void notifyRaster();
    uint8_t areaSet_ = 0;

    Video* video_ = nullptr;
    LineListener* lineListener_ = nullptr;
    Profile* profile_ = nullptr;
    int profileDepth_ = 0; // syncDevices() の入れ子 (時間を二重に数えないため)
    Mfp mfp_;
    Keyboard keyboard_;
    Rtc rtc_;
    SysPort sysPort_;
    Ioc ioc_;
    Fdc fdc_;
    Sound sound_; // OPM と ADPCM
    Dmac dmac_;
    bool inDma_ = false; // dmac_.run() / notify() の途中
    bool dmaWroteDmac_ = false; // dmac_.runBusHolders() の転送が DMAC のレジスタを書いた (その呼び出しの間だけ使う)
    bool dmaBlock_ = true; // DMA の転送をまとめる (setDmaBlock())
    Scc scc_;
    Mouse mouse_;
    Ppi ppi_;

    // input() の中身: キーはキーボード、マウスはマウス、ジョイスティックは 8255 に渡す
    class Input : public platform::InputSink {
    public:
        explicit Input(Machine& m) : m_(m) {}
        void onKey(uint8_t usage, bool down) override { m_.keyboard_.onKey(usage, down); }
        void onMouse(int dx, int dy, uint8_t buttons) override { m_.mouse_.onMouse(dx, dy, buttons); }
        void onJoystick(int port, uint8_t bits) override { m_.ppi_.setJoystick(port, bits); }

    private:
        Machine& m_;
    };
    Input input_{*this};
    Sasi sasi_;
    Spc spc_; // 内蔵 SCSI。scsiRom_ がなければ、バスからは見えない

    // 時間
    uint64_t synced_ = 0;     // 周辺機器を進め終えた CPU サイクル
    // 68000 の速さ (setCpuSpeed())。100 以外なら、runCpu000Scaled() は 1 回の区切りで CPU に X68000 の時間の
    // speed000_/100 倍のサイクル数を走らせ、走ったサイクル数に speedMul_ / 2^SPEED_FRAC_BITS (= 100/speed000_) を
    // 掛けて X68000 の時間に直す。区切りの途中 (inScaledSlice_) は cycles() が直した値を返す (周辺機器の時間が
    // 戻らないよう、途中で読んでも区切りの終わりの値を超えない)。区切りの外では cpu_.cycles() が X68000 の時間
    static constexpr int SPEED_FRAC_BITS = 16;
    int speed000_ = 100;
    uint32_t speedMul_ = 1u << SPEED_FRAC_BITS;
    uint64_t speedFrac_ = 0;     // 直したときの端数 (1/2^SPEED_FRAC_BITS サイクル)
    uint64_t sliceStart_ = 0;    // 区切りを始めたときの cpu_.cycles()
    bool inScaledSlice_ = false;
    uint64_t mfpAcc_ = 0;     // MFP のクロックへの換算の端数 (mfpMul_ / mfpDiv_ = 4MHz / CPU のクロック)
    uint32_t mfpMul_ = 1, mfpDiv_ = 4;
    uint64_t secondAcc_ = 0;  // RTC の 1 秒の端数
    uint64_t lineStart_ = 0;  // 今のラスタが始まった CPU サイクル
    uint64_t nextLine_ = 0;   // 次のラスタが始まる CPU サイクル
    uint32_t lineCycles_ = 0; // 1 ラスタの CPU サイクル数
    uint64_t lineRem_ = 0;    // ラスタの長さの端数 (CPU サイクル x ドットクロック)
    int hphase_ = 0;          // 今のラスタの位置: 0 表示の前、1 表示中、2 表示の後
    int line_ = 0;
    uint64_t frames_ = 0;
    // horizontalDisplayTimes() の結果と、それを計算したときの CRTC の世代とラスタ
    mutable uint64_t hdStart_ = 0, hdEnd_ = 0, hdLineStart_ = ~0ull;
    mutable uint32_t hdGen_ = 0, hdLineCycles_ = 0;

    // X68030 のときだけ使う (68000 のときに使うメンバの並びを変えないよう、最後に置く)
    m68k::Cpu020* cpu020_ = nullptr;
    uint8_t* highMem_ = nullptr; // ハイメモリ
    uint8_t* localMem_ = nullptr; // 060turbo のローカルメモリ

    // 外付け SCSI ボード (scsiExRom_ があるときだけつながる。上と同じ理由で最後に置く)
    uint8_t* scsiExRom_ = nullptr; // $EA0000-$EA1FFF の ROM (先頭の $20 バイトは SPC に隠れる)
    Spc spcEx_;

    // 数値演算プロセッサボードとまーきゅりーゆにっと (つないだときだけ。上と同じ理由で最後に置く)
    FpuBoard* fpuBoards_[FpuBoard::MAX_BOARDS] = {}; // [0] が $E9E000、[1] が $E9E080
    Mercury* mercury_ = nullptr;
    Ts6bga* ts6bga_ = nullptr;
    Neptune* neptune_ = nullptr; // ネットワークボード (つないだときだけ。上と同じ理由で最後に置く)
    Ym3802* midi_ = nullptr;      // MIDI ボード (つないだときだけ。上と同じ理由で最後に置く)
    MidiOut* midiOut_ = nullptr;  // MIDI の音源
    bool midiResync_ = false;     // 音源に設定を送り直す (setMidiOut() の resync。ほかのコアが立てる)

    // 68000 のバスのウェイト (setupBusWaits()。上と同じ理由で最後に置く)
    static constexpr int WAIT_BLOCKS = 0x1000000 >> 13; // 8KB ごと
    bool busWaits_ = false;           // ウェイトを入れる (68000 で Config::busWait68000)
    uint8_t waitRead_[WAIT_BLOCKS];   // 読み出し 1 回のウェイト (1/4 クロック単位)
    uint8_t waitWrite_[WAIT_BLOCKS];  // 書き込み 1 回のウェイト (1/4 クロック単位)
    uint32_t waitFrac_ = 0;           // ウェイトの 1 クロックに満たない端数 (1/4 クロック単位)
    uint32_t refreshAcc_ = 0;         // DRAM リフレッシュの遅れの端数 (CPU サイクル)
    uint32_t waitCycles_ = 0;         // busWait() で足したクロック数 (refreshWait() が区切りごとに 0 に戻す)
    // X68030 と 060turbo の本体のバスの待ち (Config::busArea を Cpu020 の 1/SUB サイクル単位に直したもの。[0] が読み、
    // [1] が書き) と、1KB ごとの場所の種類 (Config::BusArea。待たない場所と、待ちが 0 の種類の場所は 0)
    struct BusWaitSub {
        uint64_t pre = 0, period = 0, post = 0;
    };
    BusWaitSub busWaitSub_[Config::BUS_AREAS][2];
    // 待たせる場所があるのは $C00000-$ECFFFF だけなので、表はその範囲だけ持つ (2880 バイト。Machine を大きくすると、
    // ほかのメンバの配置が変わって Pi 5 で遅くなる)
    static constexpr uint32_t BUS_AREA_BASE = 0xc00000, BUS_AREA_END = 0xed0000;
    uint8_t busAreaOf_[(BUS_AREA_END - BUS_AREA_BASE) >> Config::BUS_AREA_SHIFT];
    bool busSyncKeepsPhase_ = true;

    // オーディオを再生する CD-ROM 装置 (Spc が持つものを指す。上と同じ理由で最後に置く。Sound::cdOn() のときだけ読む)
    CdDrive* cdDrive_ = nullptr;

    // shiftTime() で戻したサイクル数の合計 (フロントエンドがフレームの切れ目で読むだけ。上と同じ理由で最後に置く)
    uint64_t timeShifted_ = 0;
};

} // namespace x68k
