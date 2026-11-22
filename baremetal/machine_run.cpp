// X68000 を動かすときのコア 0 の処理: ROM、フロッピーディスク、SRAM のファイルを読み、
// コア 1 で X68000 を動かし、画面、キーボード、SRAM の書き戻し、ログ (速さ、テキスト画面のダンプ) を受け持つ。
//
// boot パーティションに置くファイル (ROM は /pix68k になければルートも探す):
//   /pix68k/IPLROM.DAT  IPL ROM (128KB、必須。なければ M0 の動作になる)
//   /pix68k/CGROM.DAT   CGROM (768KB。なければ文字が出ない)
//   /pix68k/fd0.xdf     ドライブ 0 のフロッピーディスク (.xdf / .dim / .d88)。fd1.* はドライブ 1
//   /pix68k/pix68k.cfg  設定 (なくてよい)。1 行に 1 つ「キー=値」、# から行末までは注釈
//                         fd0=HUMAN302.XDF  ドライブ 0 のイメージ (/ で始まらなければ /pix68k から)
//                         fd1=...           ドライブ 1
//                         hd0=HUMAN.HDF     SASI ID 0 のハードディスクのイメージ (HDF、256 バイト / レコード)
//                         clock=16          CPU のクロック (MHz): 10 (初代〜SUPER) か 16 (XVI、既定)。cpu=68060 では
//                                           50 (実機の 060turbo、既定) か 75 (実機の 060turbo をオーバークロックした構成)。
//                                           CPU に合わない値は使わない (その CPU の既定にする。platform::Settings::clockFor())
//                         cpu=68000         CPU: 68000 (既定) か 68030 (X68030、25MHz。PIX68K_CPU020=0 のビルドを除く)。
//                                           68030 なら IPL ROM は IPLROM30.DAT、SRAM は SRAM30.DAT / SRAM30.BAK
//                         high_mem=0        X68030 のハイメモリ (MB、最大 16)
//                         cpu=68060         060turbo (68060 50MHz、clock=75 で 75MHz。PIX68K_CPU020=0 のビルドを除く)。IPL ROM は IPLROM060.DAT (68060 で
//                                           動く IPL ROM。ROM30_060.DAT があれば $FC0000 からの ROM)、なければ
//                                           IPLROM30.DAT。SRAM は SRAM060.DAT / SRAM060.BAK
//                         local_mem=128     060turbo のローカルメモリ (MB、0 から 256。cpu=68060 のときの既定は 128。ホスト版と同じ)
//                         max_speed=off     on なら CPU (68000 / 68030 / 68060) の命令を、実時間に間に合う範囲で一番速く
//                                           動かす (x68k/max_speed.h。周辺機器と画面は実時間のまま)。そのときの速さは
//                                           2 秒ごとにログに出す
//                         speed060=100      060turbo の命令の速さ (選んだクロックの実機の 68060 を 100 とした %。5 から 3200)。
//                                           100 で命令のサイクル数が MC68060UM の表どおり。小さくすると実機より
//                                           遅くなり、ホストの負荷が下がる
//                         max_speed_limit=0 060turbo で、max_speed=on が上げる速さの上限 (同じ単位。0 は上限なし)
//                         bus_sync_sprite=off  on なら X68030 と 060turbo で、スプライト ($EB0000-$EBFFFF) の読み書きも I/O と同じ
//                                           1.6 マイクロ秒ごとの区切りにそろえる (測っていない仮定の値。x68k/machine.h の setBusSync())
//                         render_core=on    画面の合成をコア 2 で行う (既定)。off ならコア 1 が X68000 と合成の両方をする
//                         sound_core=on     音 (OPM と ADPCM) の合成をコア 3 で行う (既定)。off ならコア 1 が行う
//                         hat=auto          ゲームパッドのハットの値の範囲: auto (レポート記述子か値から決める、既定)、
//                                           0 (0-7、離すと 8 以上)、1 (1-8、離すと 0)
//                         keyboard=auto     キーボードの種類: auto (つないだキーボードの VID:PID から決める、既定)、usb (普通の
//                                           USB キーボード)、pi400us / pi400jp (Pi 400 の英語版 / 日本語版。platform/key_remap.h)
//                         sram_memsize=fix  fix (既定) なら電源投入のたびに、SRAM に書かれたメインメモリの容量を ram= の
//                                           値に書き換える。keep なら SRAM の値をそのまま使う
//                         ram=4             本体のメモリ (MB、1 から 12)
//                         scsi_ex=off       on なら外付け SCSI ボード (CZ-6BS1 相当) をつなぐ。ROM は SCSIEXROM.DAT
//                                           ($EA0000 からの 8192 バイトか $EA0020 からの 8160 バイト)
//                         scsi_ex_rom=...   外付け SCSI ボードの ROM のファイル (/ で始まらなければ /pix68k から)
//                         fpu_board=off     on なら数値演算プロセッサボード (CZ-6BP1 相当、MC68881) をつなぐ ($E9E000。
//                                           FLOAT3.X などから使う)。2 なら 2 枚目も $E9E080 につなぐ
//                         mercury=off       on ならまーきゅりーゆにっと (16 ビットの PCM、$ECC080、DMAC のチャネル 2) をつなぐ
//                         neptune=off       on ならネットワークボード Neptune-X (NE2000 互換、$ECE000、IRQ2) をつなぐ。
//                                           有線の Ethernet でフレームを出し入れする (net_bridge.h)
//                         neptune_mac=...   ボードの MAC アドレス (既定 02:68:6b:00:00:01)。pi なら Pi の MAC アドレスを使う
//                         neptune_link=eth  eth (有線の Ethernet、既定) か echo (ネットワークにつながず、ARP と ping に答える。試験用)
//                         rs232c=off        RS-232C (SCC のチャネル A) の相手。tcp:PORT なら net=on の TCP の PORT で待ち受け、
//                                           つないできた相手とバイトをやり取りする (host_net.h の RS-232C)
//                         midi=off          on なら MIDI ボード CZ-6BM1 (YM3802、$EAFA00、IRQ4) をつなぐ
//                         midi_out=sf2      MIDI の行き先: sf2 (midi_sf2= の SoundFont で鳴らし、音に混ぜる。既定)、
//                                           mt32 (midi_mt32_ctrl= と midi_mt32_pcm= の ROM で Munt を鳴らし、音に混ぜる)、
//                                           sc55 (midi_sc55= の ROM で Nuked-SC55 を鳴らし、音に混ぜる。Pi 4 と Pi 5 だけ)、
//                                           uart (midi_uart= の UART から 31.25kbps で出し、実機の音源を鳴らす。ログと同じ
//                                           UART なら、ログは UART に出さなくなる)、none (捨てる)
//                         midi_sf2=...      SoundFont (SF2) のファイル (/ で始まらなければ /pix68k から)。標本を float に
//                                           直して持つので、ファイルの大きさの 2 倍ほどのヒープを使う (読んだあとも減った
//                                           まま)。X68000 を動かし始めてから読み、読み終えるまでに送った MIDI のうち、
//                                           音は鳴らさない (プログラムチェンジなどの設定は、読み終えたときに送り直す)
//                         midi_voices=48    SoundFont の音源の同時に鳴らすボイスの数 (4 から 256)
//                         midi_mt32_ctrl=.. MT-32 / CM-32L の CONTROL の ROM (64KB か 128KB。/ で始まらなければ /pix68k から)
//                         midi_mt32_pcm=..  MT-32 / CM-32L の PCM の ROM (512KB か 1MB)。2 つとも SoundFont と同じく、
//                                           X68000 を動かし始めてから読む
//                         midi_sc55=...     SC-55mkII の 5 つの ROM を置いたディレクトリ (/ で始まらなければ /pix68k から。
//                                           ファイルの名前は platform/sc55_synth.h)。X68000 を動かし始めてから読み、
//                                           SC-55mkII を起動する (Pi 5 で 3 秒ほど)。音はコア 3 が OPM と ADPCM の合間に
//                                           作り、OPM より 32ms 遅れて鳴る。Pi 3 では実時間の半分ほどの速さしか出ないので使わない
//                         midi_uart=0       midi_out=uart で使う UART (Circle の CSerialDevice の番号)。0 は GPIO 14 (どの機種も)。
//                                           Pi 4 は 2-5 (TXD は GPIO 0、4、8、12)、Pi 5 は 1-4 (GPIO 0、4、8、12) と 10 (UART の端子)。
//                                           Pi 3 と Pi 4 の 0、Pi 5 の 10 はログと同じ UART なので、ログを UART に出さなくなる
//                         midi_uart_family=gs  midi_out=uart の先の音源: gs (GS / GM の音源、既定) か mt32 (MT-32 / CM-32L)。
//                                           ステートを読んだときの送り直しと、鳴っている音の止め方を決める
//                         sc0=HUMAN.HDS     SCSI ID 0 (sc0 から sc6) のハードディスクのイメージ (HDS、512 バイト / ブロック)。
//                                           外付け SCSI ボードがあればそちら、なければ内蔵 SCSI (X68030 と 060turbo)
//                         cd=GAME.ISO       CD-ROM のイメージ (ISO、2048 バイト / セクタ)。つなぐ先は sc0= と同じ
//                         cd_id=6           CD-ROM をつなぐ SCSI ID (0 から 6)
//                                           ハードディスクと CD-ROM のイメージは、全体を読まずに使うチャンクだけを読む
//                                           (x68k/disk_cache.h。kStreamMinMB 以下なら全体をメモリに読む)。全体が
//                                           メモリ (Pi 4 / Pi 5 は 1GB より上のヒープ、なければ 1GB より下) に入るなら、
//                                           読んだチャンクはすべて残し、残りのチャンクも裏で読む。
//                                           cd= と sc<N>= が同じ ID なら cd= を使う
//                         write_back=on     X68000 がディスクに書いた内容を、SD カードのイメージのファイルに書き戻す (既定)。
//                                           off ならメモリの上だけで、電源を切ると消える (使うチャンクだけを読む大きな
//                                           イメージは、書いた内容を残せないので書き込み禁止にする)
//                         write_protect=    書き込み禁止にするディスク (「fd1,sc0」のように fd0 fd1 hd0 sc0〜sc6 を並べる)。
//                                           ファイルの読み取り専用の属性と、D88 のヘッダの書き込み禁止でも書き込み禁止になる
//                         autotype=         X68000 を動かし始めて autotype_wait= 秒 (既定 10) たったら打つ文字列
//                                           (POST /type と同じエスケープ。boot_menu.cpp)
//                         profile_wait=5    起動の組み合わせのメニューで待つ秒数 (0 ならメニューを出さない)
//   /pix68k/pix68k-<名前>.cfg  起動の組み合わせ。1 つでもあれば起動時にメニューで選ぶ (boot_menu.cpp、platform/boot_profile.h)
//   /pix68k/profile.sel 前回選んだ起動の組み合わせの名前
//   /pix68k/hd0.hdf     hd0= がないときの SASI ID 0 のイメージ。kMaxHardDiskBytes まで
//   /pix68k/SRAM.DAT    SRAM (16KB)。起動時に読み、書き換えられたら書き戻す (5 秒に 1 回まで)。
//   /pix68k/SRAM.BAK    電源が切れても前の内容が残るよう、SRAM.DAT と交互に書く (x68k/sram_file.h)
// ディスクの書き戻し (platform/disk_sync.h): 書き込みが 2 秒止んだとき (書き続けていても 30 秒ごと)、FD の入れ替えと
// 取り出し、設定メニューのリセット / Restart / 設定とステートの保存のときに書く。ハードディスクはファイルの中の
// 書いたチャンク (16KB) を上書きし、フロッピーディスクは <名前>.new に全体を書いてから置き換える。
//
// カーネルのオプション (cmdline.txt):
//   pix68k_textdump=N   N 秒ごとにテキスト画面を読み、変わっていれば UART のログに出す (テスト用)
//   pix68k_sound=X      音の出力先: hdmi (既定)、pwm (Pi 3 は 3.5mm のジャック、Pi 5 は GPIO 12 / 13)、
//                       i2s (GPIO 18 / 19 / 21 の I2S の DAC)、off、
//                       null (試験用。音を作って捨てる)。QEMU 用のビルドでは既定は off (QEMU の raspi3b は HDMI の音を持たない)
//   pix68k_i2s_dac=X    pix68k_sound=i2s の DAC の設定: auto (既定。I2C (GPIO 2 / 3) で PCM512x (0x4C / 0x4D) と
//                       WM8960 (0x1A) を探し、なければ設定の要らない DAC として動かす)、none (I2C を使わない。PCM5102A など)、
//                       0x4c のような I2C の番地 (その番地の PCM512x か WM8960。見つからなければ音を出さない)
//   pix68k_fail_core=N  試験用。N 番以降のコアを起動しなかったものとして扱う (1 なら止まる)
//   pix68k_net_probe=1  試験用。neptune=on のとき、X68000 の MAC アドレスから ARP を問い合わせ、答えをログに出す (net_bridge.h)
//   pix68k_stream_min=N 試験用。N MB より大きいハードディスクのイメージは、使うチャンクだけを読む (既定 kStreamMinMB)
//   pix68k_gpu_scale=N  フレームバッファを表示の 1/N (AxB なら横 1/A、縦 1/B) にし、表示への拡大を GPU に任せる (kernel.cpp の
//                       InitScreen())。既定は使わない
//   pix68k_crt_all=1    試験用。Pi 3 でも video_scale= と video_filter= のどの組み合わせも選べるようにする (時間を測るため。
//                       CKernel::VideoAllowed())
//   pix68k_fb_rows=0    試験用。変わっていない行も毎回フレームバッファに写す (platform/frame_scale.h の FrameRowCache)
//   pix68k_wfe=0        試験用。待つ間 (コア 0 の主ループ、コア 1 が次のフレームの時刻を待つ間) を wfe で眠らせず、空回りで待つ
//                       (event_wait.h。温度や速さを比べるため)
#include <circle/bcmframebuffer.h>
#include <circle/bcmpropertytags.h>
#include <circle/heapallocator.h>
#include <circle/machineinfo.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <circle/util.h>

#include "event_wait.h"
#include "kernel.h"
#include "platform/cue_sheet.h"
#include "platform/disk_cache_size.h"
#include "platform/disk_sync.h"
#include "platform/file_replace.h"
#include "platform/net_config.h"
#include "platform/net_files.h"
#include "platform/setting_warning.h"
#include "platform/mt32_synth.h"
#include "platform/sc55_synth.h"
#include "platform/sf2_synth.h"

static const char FromMachine[] = "x68k";

// Pi には時計がないので、RTC はこの時刻 (2026-01-01 00:00) から始める
static const int64_t kDefaultLocalTime = 1767225600;

namespace {
const unsigned kSramSaveInterval = 5;   // SRAM を書き戻す間隔の下限 (秒)
const unsigned kSpeedReportInterval = 10; // 速さをログに出す間隔 (秒)
const unsigned kMaxSpeedReportInterval = 2; // 最高速モードの CPU の速さをログに出す間隔 (秒)
// 画面の隅の数字の絵 (platform::renderOsd())。68KB あるので、CKernel の中 (main() のスタックに置かれる。
// スタックは 128KB) には置かない。書くのは、写す役を持っているコアだけ (CKernel::ShowFrame())
alignas(16) u32 s_OsdImage[platform::OSD_IMAGE_PIXELS];
// 画面の左上のメッセージ (POST /message。platform/osd_message.h) に使うメモリ (85KB ほど)。初めて POST /message が来たときに
// コア 0 がヒープに確保し、s_pMsg に置く (.bss に置くと、aarch64-linux-gnu-gcc のビルドでカーネルの大きさの上限を超えるため。
// 使わなければ確保しない)。一度確保したら放さない。
// box はコア 0 が書き、写す役のコアが読む (seqlock)。shown (写した字形) と image (絵) は写す役のコアだけ、
// layout と build (作る途中) はコア 0 だけが触る
struct MessageMem {
    platform::MessageBox box;
    platform::MessageGlyphs shown;
    platform::MessageLayout layout;
    platform::MessageGlyphs build;
    alignas(16) u32 image[platform::MSG_IMAGE_PIXELS];
};
MessageMem* s_pMsg = nullptr;
// ハードディスクのイメージは全体をメモリに読むので、大きさに上限を置く。
// SASI のいちばん大きいドライブ (40MB、$27930 レコード = 41,627,648 バイト) が入る大きさにする
const unsigned kMaxHardDiskBytes = 48u * 1024 * 1024;
// SCSI のイメージ (HDS、CD-ROM) を全体をメモリに読むときの上限。CD-ROM 1 枚 (74 分、約 650MB) が入る大きさにする。
// 実際に読めるかはヒープの空きによる (下の HeapHasRoom)。これより大きいか、ヒープに入らないイメージは、
// 使うチャンクだけを読む (x68k/disk_cache.h。FAT32 のファイルの大きさの上限 4GB まで)
const unsigned kMaxScsiImageBytes = 700u * 1024 * 1024;
// これより大きいイメージは、メモリに入っても全体を読まずに使うチャンクだけを読む (起動を待たせないため。
// SASI の 20MB のイメージを全体を読むと、QEMU で 2.7 秒かかった)。SASI のいちばん小さいドライブ (10MB) より小さくして、
// ハードディスクのイメージはふつう全体を読まないようにする。
// カーネルのオプション pix68k_stream_min=N (MB) で変えられる (試験用。0 ならすべて)
const unsigned kStreamMinMB = 1;
// 使うチャンクだけを読むイメージの枠の数と必要なメモリは platform/disk_cache_size.h で決める。
// Circle のヒープの確保の仕方 (lib/heapallocator.cpp の DoAllocate): バケットへの切り上げ、ヘッダ、そろえ方
const uint32_t kHeapBuckets[] = {HEAP_BLOCK_BUCKET_SIZES, 0};
const platform::HeapModel kHeapModel = {kHeapBuckets, (uint32_t)sizeof(THeapBlockHeader), HEAP_BLOCK_ALIGN};
// イメージを読んだあとにも残すヒープ (SRAM のファイル、画面のダンプ、USB の確保などに使う分の見積もり)
const size_t kHeapReserve = 32u * 1024 * 1024;

// ヒープに size バイトを確保しても kHeapReserve が残るか。
// Circle の new は確保に失敗すると nullptr を返さず、"Out of memory" で全体を止める (STDLIB_SUPPORT が 3 より小さいとき。
// lib/heapallocator.cpp の DoAllocate)。new が使うのは 1GB より下のヒープ (HEAP_LOW) なので、Pi 4 / Pi 5 でもそこを見る。
// GetHeapFreeSpace は一度も使っていない部分の大きさで、解放したブロックは数えない (少なめに見積もる)
bool HeapHasRoom(size_t size, size_t& freeBytes) {
    freeBytes = CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW);
    return freeBytes >= kHeapReserve && size <= freeBytes - kHeapReserve;
}

// 1GB より上のヒープ (Pi 4 / Pi 5 でメモリが 1GB より多いとき。Circle の HEAP_HIGH) で、イメージのキャッシュに使ってよい大きさ。
// なければ 0 (Pi 3 と、メモリが 1GB の Pi 4)。Circle の new はここを使わないので、使うのはイメージのキャッシュの枠の中身だけ。
// 確保に失敗すると Circle は全体を止めるので、空きを調べてから確保する (kHighHeapReserve を残す)
const size_t kHighHeapReserve = 16u * 1024 * 1024;
const uintptr kHighHeapStart = 0x40000000ul;
uint64_t HighHeapRoom() {
    const size_t freeBytes = CMemorySystem::Get()->GetHeapFreeSpace(HEAP_HIGH);
    return freeBytes > kHighHeapReserve ? freeBytes - kHighHeapReserve : 0;
}
void ReleaseHighHeap(uint8_t* p) { CMemorySystem::HeapFree(p); }

// n バイトを malloc したときに減るヒープの大きさ (SoundFont を読むときの見積もり。platform::LoadLimit)
uint64_t HeapBlockBytes(size_t n) { return platform::heapBlockBytes(n, kHeapModel); }
} // namespace

// ファイル全体をヒープに読む。読めなければ nullptr
u8* CKernel::ReadWholeFile(const char* path, unsigned& size) {
    FIL f;
    size = 0;
    if (f_open(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return nullptr;
    unsigned n = (unsigned)f_size(&f);
    u8* buf = n > 0 ? new u8[n] : nullptr;
    UINT got = 0;
    if (buf != nullptr && (f_read(&f, buf, n, &got) != FR_OK || got != n)) {
        delete[] buf;
        buf = nullptr;
    }
    f_close(&f);
    if (buf != nullptr) size = n;
    return buf;
}

// ファイルを読む。ないときと読めないときを分ける (ないなら FR_NO_FILE か FR_NO_PATH、読めたら FR_OK で、
// 空のファイルなら data は nullptr のまま)
FRESULT CKernel::ReadFileChecked(const char* path, u8*& data, unsigned& size) {
    data = nullptr;
    size = 0;
    FIL f;
    FRESULT r = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
    if (r != FR_OK) return r;
    const unsigned n = (unsigned)f_size(&f);
    u8* buf = n > 0 ? new u8[n] : nullptr;
    UINT got = 0;
    if (n > 0) {
        if (buf == nullptr) r = FR_NOT_ENOUGH_CORE;
        else if ((r = f_read(&f, buf, n, &got)) == FR_OK && got != n) r = FR_INT_ERR;
    }
    f_close(&f);
    if (r != FR_OK) {
        delete[] buf;
        return r;
    }
    data = buf;
    size = n;
    return FR_OK;
}

// path を書き直す。書いている途中で止まっても前の内容が残るよう、tmp に書き終えてから置き換える
// (手順は platform/file_replace.h)。読む側は ReadWithFallback() を使う
struct CFatOps {
    FRESULT last = FR_OK; // 最後に失敗した操作の値 (ログ用)
    platform::FsResult conv(FRESULT r) {
        if (r == FR_OK) return platform::FsResult::OK;
        if (r == FR_NO_FILE || r == FR_NO_PATH) return platform::FsResult::NO_FILE;
        last = r;
        return platform::FsResult::ERROR;
    }
    platform::FsResult exists(const char* path) {
        FILINFO fi;
        return conv(f_stat(path, &fi));
    }
    // 新しいファイルを書く (ステート、pix68k.cfg、フロッピーディスクのイメージの .new)。boot ドライブが USB メモリの
    // ときは、/pix68k の下で、書いたあとも空きが残るときだけ書く (CKernel::CanWriteNewFile())。だめなら FR_DENIED
    platform::FsResult write(const char* path, const void* data, unsigned size) {
        if (!CKernel::CanWriteNewFile(path, size)) {
            last = FR_DENIED;
            return platform::FsResult::ERROR;
        }
        FIL f;
        FRESULT r = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
        if (r != FR_OK) {
            last = r;
            return platform::FsResult::ERROR;
        }
        UINT written = 0;
        r = f_write(&f, data, size, &written);
        if (r == FR_OK && written != size) r = FR_DENIED; // SD カードがいっぱい
        if (r == FR_OK) r = f_sync(&f);
        FRESULT rc = f_close(&f);
        if (r == FR_OK) r = rc;
        if (r != FR_OK) {
            last = r;
            return platform::FsResult::ERROR;
        }
        return platform::FsResult::OK;
    }
    platform::FsResult remove(const char* path) { return conv(f_unlink(path)); }
    platform::FsResult rename(const char* from, const char* to) { return conv(f_rename(from, to)); }
    // 頭から cap バイトまで読む (platform::readProfileChoice())
    platform::FsResult read(const char* path, char* buf, unsigned cap, unsigned& n) {
        n = 0;
        FIL f;
        const platform::FsResult o = conv(f_open(&f, path, FA_READ));
        if (o != platform::FsResult::OK) return o;
        UINT got = 0;
        const FRESULT r = f_read(&f, buf, cap, &got);
        f_close(&f);
        if (r != FR_OK) return conv(r);
        n = got;
        return platform::FsResult::OK;
    }
    // 途中で止まった置き換えを戻す名前の変更の前に、platform::replaceFile() が聞く。boot ドライブが USB メモリのときは、
    // ディレクトリが伸びても空きが残るときだけ (CKernel::CanRenameFile())
    bool canRename(const char*, const char* to) { return CKernel::CanRenameFile(to); }
};

// ディスクの書き戻しと大きなイメージの読み込み (platform/disk_sync.h の Fs)
static const char* const kDiskNames[platform::DiskChannel::MAX_DISKS] = {"fd0", "fd1", "hd0", "sc0", "sc1",
                                                                          "sc2", "sc3", "sc4", "sc5", "sc6"};
static const int kDiskSasi = 2, kDiskScsi = 3; // ディスクの番号 (SCSI ID N は kDiskScsi + N)
static const unsigned kFdShadowBytes = 2u * 1024 * 1024; // フロッピーディスクの写しの大きさ (D88 の 2HD は 1.3MB ほど)

// FatFs の fast seek の表を作る (ファイルの中の位置を、FAT をたどらずに変えられるようにする)。作れなければ false
static bool CreateLinkMap(FIL* f) {
    DWORD n = 64;
    for (int tries = 0; tries < 2; tries++) {
        DWORD* table = new DWORD[n];
        table[0] = n;
        f->cltbl = table;
        const FRESULT r = f_lseek(f, CREATE_LINKMAP);
        if (r == FR_OK) return true;
        f->cltbl = nullptr;
        const DWORD want = table[0];
        delete[] table;
        if (r != FR_NOT_ENOUGH_CORE || want <= n) break;
        n = want; // ファイルが断片になっている: 要る大きさで作り直す
    }
    return false;
}

struct CDiskFs : CFatOps {
    typedef FIL File;
    CLogger* log = nullptr;
    // 開いたままのイメージのファイル (開き直すため)
    struct Open {
        FIL* f = nullptr;
        CString path;
        BYTE mode = 0;
        const char* label = "";
        bool broken = false; // 開き直せなかった (読み書きはせずに失敗にし、次の失敗でまた開き直す)
    };
    bool broken(FIL* f) const {
        for (const Open& o : open)
            if (o.f == f) return o.broken;
        return false;
    }
    Open open[platform::DiskChannel::MAX_DISKS];
    void untrack(int disk) { open[disk] = Open(); }
    void track(int disk, FIL* f, const char* path, BYTE mode) {
        open[disk].f = f;
        open[disk].path = path;
        open[disk].mode = mode;
        open[disk].label = disk >= 0 && disk < platform::DiskChannel::MAX_DISKS ? kDiskNames[disk] : "disk";
    }
    // 読み書きに失敗したあとに、ファイルを開き直す。FatFs は f_read / f_write / f_lseek でディスクのエラーが出ると
    // FIL に記録し (fp->err)、その FIL の以後の読み書きはすべて同じエラーを返すため (f_sync の失敗は残らない)
    void recover(FIL* f) {
        for (Open& o : open) {
            if (o.f != f) continue;
            f_close(f); // 失敗してもよい (FF_FS_LOCK = 0 なので、開き直せば FIL は作り直される)
            delete[] f->cltbl;
            f->cltbl = nullptr;
            const FRESULT r = f_open(f, o.path, o.mode | FA_OPEN_EXISTING);
            o.broken = r != FR_OK;
            if (r == FR_OK) {
                CreateLinkMap(f);
                log->Write(FromMachine, LogNotice, "%s: %s reopened after an error", o.label, (const char*)o.path);
            } else {
                // 開けないままなら、FIL は使えない (読み書きは失敗し続け、次の失敗でまた開き直す)
                log->Write(FromMachine, LogWarning, "%s: cannot reopen %s (FRESULT %d)", o.label, (const char*)o.path,
                           (int)r);
            }
            return;
        }
    }
    // 読み込みの時間 (ログ用)
    u64 readCount = 0, readUs = 0;
    unsigned readMaxUs = 0;
    // 1GB より上のバッファ (イメージのキャッシュの枠) には、FatFs とドライバに直接読ませず、1GB より下のここに読んでから写す
    // (FatFs はセクタの倍数の読み込みを呼んだ側のバッファに直接読ませる。ドライバによっては、1GB より上のバッファに
    // DMA で転送できない。写すのは 16KB で、読み込みの時間に比べて短い)
    u8 bounce[x68k::DiskCache::CHUNK];
    bool readAt(FIL* f, uint64_t off, void* buf, uint32_t len) {
        if (broken(f)) return false;
        const u64 t0 = CTimer::GetClockTicks64();
        FRESULT r = f_lseek(f, (FSIZE_t)off);
        const bool high = (uintptr)buf >= kHighHeapStart;
        for (uint32_t done = 0; r == FR_OK && done < len;) {
            const uint32_t n = high && len - done > sizeof bounce ? (uint32_t)sizeof bounce : len - done;
            UINT got = 0;
            r = f_read(f, high ? (void*)bounce : (void*)((u8*)buf + done), n, &got);
            if (r == FR_OK && got != n) r = FR_INT_ERR;
            if (r == FR_OK && high) memcpy((u8*)buf + done, bounce, n);
            done += n;
        }
        if (r != FR_OK) last = r;
        const unsigned us = (unsigned)(CTimer::GetClockTicks64() - t0);
        readCount++;
        readUs += us;
        if (us > readMaxUs) readMaxUs = us;
        return r == FR_OK;
    }
    // ファイルの大きさは変えない (クラスタの並びと FAT を変えないため。イメージの後ろの端数は書かない)
    bool writeAt(FIL* f, uint64_t off, const void* p, uint32_t len) {
        if (broken(f)) return false;
        const FSIZE_t size = f_size(f);
        if (off >= size) return true;
        if (len > size - off) len = (uint32_t)(size - off);
        UINT done = 0;
        FRESULT r = f_lseek(f, (FSIZE_t)off);
        if (r == FR_OK) r = f_write(f, p, len, &done);
        if (r == FR_OK && done != len) r = FR_DENIED;
        if (r != FR_OK) last = r;
        return r == FR_OK;
    }
    bool sync(FIL* f) {
        if (broken(f)) return false;
        const FRESULT r = f_sync(f);
        if (r != FR_OK) last = r;
        return r == FR_OK;
    }
    void report(int disk, platform::DiskEvent e, uint32_t value) {
        const char* name = disk >= 0 ? kDiskNames[disk] : "disk";
        switch (e) {
        case platform::DiskEvent::WRITTEN:
            log->Write(FromMachine, LogNotice, "%s: written back (%u bytes)", name, value);
            break;
        case platform::DiskEvent::REPLACED:
            log->Write(FromMachine, LogNotice, "%s: image file replaced (%u bytes)", name, value);
            break;
        case platform::DiskEvent::WRITE_FAILED:
        case platform::DiskEvent::REPLACE_FAILED:
            // 続けて失敗したときは、1 回目と 10 回ごとに出す
            if (value == 0 || value % 10 == 0)
                log->Write(FromMachine, LogWarning, "%s: cannot write back (FRESULT %d, %u failures); will retry", name,
                           (int)last, value + 1);
            break;
        case platform::DiskEvent::RECOVERED:
            log->Write(FromMachine, LogNotice, "%s: written back after %u failures", name, value);
            break;
        case platform::DiskEvent::READ_FAILED:
            log->Write(FromMachine, LogWarning, "%s: cannot read chunk %u (FRESULT %d)", name, value, (int)last);
            break;
        case platform::DiskEvent::READ_RECOVERED:
            log->Write(FromMachine, LogNotice, "%s: read after %u failures", name, value);
            break;
        }
    }
};

struct CDiskWriter {
    CDiskFs fs;
    platform::DiskChannel channel;
    platform::DiskSync<CDiskFs> sync{fs, channel};
};

// 大文字と小文字を区別せずに比べる (FAT のファイル名)
static bool SamePath(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) return false;
    }
    return *a == *b;
}

FRESULT CKernel::WriteFileReplacing(const char* path, const char* tmp, const void* data, unsigned size) {
    CFatOps fs;
    if (platform::replaceFile(fs, path, tmp, data, size) == platform::FsResult::OK) return FR_OK;
    return fs.last != FR_OK ? fs.last : FR_DENIED;
}

// POST /reboot?profile= の要求を profile.sel に once 付きで書く / 取り消す (platform::ProfileChoiceStore)
bool CKernel::RequestProfileChoice(const char* name) {
    CFatOps fs;
    CString path, tmp;
    path.Format("%s/pix68k/%s", m_BootDrive, platform::PROFILE_CHOICE_FILE);
    tmp.Format("%s/pix68k/%s", m_BootDrive, platform::PROFILE_CHOICE_TMP);
    if (m_ProfileChoice.request(fs, path, tmp, name)) return true;
    m_Logger.Write(FromMachine, LogWarning, "%s: write failed (FRESULT %d)%s", (const char*)path, (int)fs.last,
                   m_ProfileChoice.pending() ? "; the request may be used at the next boot" : "");
    return false;
}

// 再起動 (Ctrl + Alt + Delete、設定メニューの Restart) の前に取り消す。取り消せなければログと画面の左上に出して false
bool CKernel::CancelProfileChoiceForRestart(const char* who) {
    if (CancelProfileChoice()) return true;
    m_Logger.Write(FromMachine, LogWarning,
                   "%s: not restarting (the refused POST /reboot?profile= is still in %s and would be used)", who,
                   platform::PROFILE_CHOICE_FILE);
    static const char kMsg[] = "再起動中止";
    netMessage(kMsg, sizeof kMsg - 1, 10);
    return false;
}

bool CKernel::CancelProfileChoice() {
    if (!m_ProfileChoice.pending() || m_BootDrive == nullptr) return true;
    CFatOps fs;
    CString path, tmp;
    path.Format("%s/pix68k/%s", m_BootDrive, platform::PROFILE_CHOICE_FILE);
    tmp.Format("%s/pix68k/%s", m_BootDrive, platform::PROFILE_CHOICE_TMP);
    if (m_ProfileChoice.cancel(fs, path, tmp)) return true;
    m_Logger.Write(FromMachine, LogWarning, "%s: cannot cancel the network request (FRESULT %d)", (const char*)path,
                   (int)fs.last);
    return false;
}

// WriteFileReplacing() で書いたファイルを読む。path がないときだけ tmp を読む (path があれば、空でも path)
// (手順は platform/file_replace.h の readWithFallback())
FRESULT CKernel::ReadWithFallback(const char* path, const char* tmp, u8*& data, unsigned& size, bool& usedTmp) {
    struct Reader {
        u8*& data;
        unsigned& size;
        FRESULT read(const char* p) { return ReadFileChecked(p, data, size); }
        bool missing(FRESULT r) const { return r == FR_NO_FILE || r == FR_NO_PATH; }
        bool ok(FRESULT r) const { return r == FR_OK; }
    } reader{data, size};
    return platform::readWithFallback(reader, path, tmp, usedTmp);
}

// ROM を /pix68k、なければルートから読む。大きさが違えば読まなかったことにする
u8* CKernel::ReadRom(const char* name, unsigned size) {
    static const char* const dirs[] = {"/pix68k/", "/"};
    for (const char* dir : dirs) {
        CString path;
        path.Format("%s%s%s", m_BootDrive, dir, name);
        unsigned n;
        u8* data = ReadWholeFile(path, n);
        if (data == nullptr) continue;
        if (n == size) {
            m_Logger.Write(FromMachine, LogNotice, "%s (%u bytes)", (const char*)path, n);
            return data;
        }
        m_Logger.Write(FromMachine, LogWarning, "%s: %u bytes, expected %u; ignored", (const char*)path, n, size);
        delete[] data;
    }
    return nullptr;
}

// /pix68k の name を読んで text に入れる。書き直しの途中で止まって name がなく tmpName だけがあるとき
// (SaveConfig() を参照) は、tmpName を読む。path に読んだファイルを入れる。読めなければ text は空
FRESULT CKernel::LoadConfigText(const char* name, const char* tmpName, platform::ConfigText& text, CString& path) {
    CString cfg, tmp;
    cfg.Format("%s/pix68k/%s", m_BootDrive, name);
    tmp.Format("%s/pix68k/%s", m_BootDrive, tmpName);
    u8* data = nullptr;
    unsigned size = 0;
    bool usedTmp = false;
    const FRESULT r = ReadWithFallback(cfg, tmp, data, size, usedTmp);
    path = usedTmp ? tmp : cfg;
    const bool loaded = text.load(r == FR_OK && data != nullptr ? (const char*)data : "", r == FR_OK ? size : 0);
    delete[] data;
    // メモリが足りずに中身を持てなければ、読めなかったことにする (空のまま書き直して、ほかの行を消さないように)
    return r == FR_OK && !loaded ? FR_NOT_ENOUGH_CORE : r;
}

// /pix68k/pix68k.cfg を読む。項目の表は platform/settings.h。
// 書き直しの途中で止まって pix68k.cfg がなく pix68k.new だけがあるとき (SaveConfig() を参照) は、pix68k.new を読む。
// /pix68k に起動の組み合わせ (pix68k-<名前>.cfg) があれば、選ばせてから (ChooseProfile())、pix68k.cfg の共通の項目と
// 組み合わせのファイルを重ねて読む (platform/boot_profile.h)
void CKernel::ReadConfig() {
    platform::ConfigText base;
    CString path;
    FRESULT r = LoadConfigText("pix68k.cfg", "pix68k.new", base, path);
    if (r == FR_OK) {
        m_Logger.Write(FromMachine, LogNotice, "%s", (const char*)path);
    } else if (r != FR_NO_FILE && r != FR_NO_PATH) {
        m_Logger.Write(FromMachine, LogWarning, "%s: cannot read (FRESULT %d); using the defaults", (const char*)path,
                       (int)r);
    }
    // トークンは /log (トークンなしで読める) に出さない (platform/setting_warning.h)
    const char* file = "pix68k.cfg";
    auto warn = [this, &file](bool fromProfile, const char* key, const char* value, platform::SettingResult res) {
        char msg[384];
        platform::formatSettingWarning(msg, sizeof msg, key, value, res);
        m_Logger.Write(FromMachine, LogWarning, "%s: %s", fromProfile ? file : "pix68k.cfg", msg);
    };
    auto bad = [this, &file](bool fromProfile, const char* line) {
        char msg[384];
        platform::formatIgnoredLine(msg, sizeof msg, line);
        m_Logger.Write(FromMachine, LogWarning, "%s: %s", fromProfile ? file : "pix68k.cfg", msg);
    };
    platform::readProfileSettings(base, nullptr, m_Settings, true, warn, bad);
    // メニューのキーボードとゲームパッドは pix68k.cfg の keyboard= と hat= で読む
    ApplySettings();
    const int keyboard = m_Settings.keyboard, hat = m_Settings.hat;
    ChooseProfile();
    const platform::ConfigText* fdText = &base;
    platform::ConfigText prof;
    char name[64];
    if (m_Profile[0] != 0) {
        char tmpName[64];
        platform::profileFileName(m_Profile, name, sizeof name);
        platform::profileFileName(m_Profile, tmpName, sizeof tmpName, true);
        r = LoadConfigText(name, tmpName, prof, path);
        if (r != FR_OK) {
            m_Logger.Write(FromMachine, LogWarning, "%s: cannot read (FRESULT %d); using pix68k.cfg", (const char*)path,
                           (int)r);
            m_Profile[0] = 0;
        } else {
            m_Logger.Write(FromMachine, LogNotice, "profile %s: %s (shared settings from pix68k.cfg)", m_Profile,
                           (const char*)path);
            file = name;
            platform::readProfileSettings(base, &prof, m_Settings, false, warn, bad);
            fdText = &prof;
        }
    }
    // fd0= / fd1= を空で書いてあれば、ドライブを空にする (fd0.xdf などを探さない。メニューで取り出して保存したとき)
    for (int d = 0; d < 2; d++) {
        char v[platform::Settings::PATH_SIZE];
        m_FdExplicit[d] = fdText->get(d == 0 ? "fd0" : "fd1", v, sizeof v) != 0;
    }
    m_FileSettings = m_Settings;
    ApplySettings();
    ReapplyInputSettings(keyboard, hat);
}

// 最高速モード (max_speed=on) の、今の CPU の速さ (既定のクロックの何 %) と、それをクロックに直した値をログに出す
void CKernel::ReportMaxSpeed() {
    if (m_pEmulator == nullptr) return;
    const unsigned pct = (unsigned)m_pEmulator->cpuSpeed();
    const unsigned mhz = (unsigned)(m_pEmulator->cpuHz() / 1000000);
    // 060turbo の 100% は、命令のサイクル数が選んだクロックの実機の 68060 と同じという意味 (m68k/cpu060_cycles.inc)
    if (m_Settings.cpu == 68060) {
        m_Logger.Write(FromMachine, LogNotice, "max speed: %u%% of a real 68060 (%u MHz)", pct, mhz);
        return;
    }
    m_Logger.Write(FromMachine, LogNotice, "max speed: %u%% of %u MHz (%u.%02u MHz)", pct, mhz, pct * mhz / 100,
                   pct * mhz % 100);
}

// m_Settings を、X68000 の設定 (m_Config) と、それぞれの部分の変数に写す
void CKernel::ApplySettings() {
    if (m_Settings.cpu != 68000 && !x68k::Config::has020()) {
        char file[64] = "pix68k.cfg";
        if (m_Profile[0] != 0) platform::profileFileName(m_Profile, file, sizeof file);
        m_Logger.Write(FromMachine, LogWarning, "%s: cpu=%d: this build has no 68030 / 68060; using 68000", file,
                       m_Settings.cpu);
        m_Settings.cpu = 68000;
    }
    m_Settings.toConfig(m_Config);
    m_MaxSpeed = m_Settings.maxSpeed != 0;
    m_RenderCore = m_Settings.renderCore != 0;
    m_SoundCore = m_Settings.soundCore != 0;
    m_HatBase = m_Settings.hat;
    m_PadLog = m_Settings.padLog != 0;
    m_KeyLayoutAuto = m_Settings.keyboard == 0;
    m_KeyLayoutCfg = m_Settings.keyboard == 2   ? platform::KeyLayout::PI400_US
                     : m_Settings.keyboard == 3 ? platform::KeyLayout::PI400_JP
                                                : platform::KeyLayout::STANDARD;
}

// 書き直す前に、/pix68k の name (なければ tmpName) を読む。ないファイルは空として読む。
// ファイルがあるのに読めなければ false (空のファイルとして書くと、ほかの設定が消える)
bool CKernel::LoadSavedConfigText(const char* name, const char* tmpName, platform::ConfigText& text) {
    CString path;
    const FRESULT r = LoadConfigText(name, tmpName, text, path);
    if (r != FR_OK && r != FR_NO_FILE && r != FR_NO_PATH) {
        m_Logger.Write(FromMachine, LogWarning, "%s: cannot read (FRESULT %d); settings not saved", (const char*)path,
                       (int)r);
        return false;
    }
    return true;
}

// s のうち pix68k.cfg と違う項目を書く (行があれば値を書き換え、なければ足す。知らない行と注釈は残す)。
// 書いている途中で電源が切れても前の設定が残るよう、pix68k.new に書いてから pix68k.cfg と置き換える
// (FatFs の f_rename は上書きしないので、pix68k.cfg を消してから名前を変える。その間に切れたら、次の起動は
// pix68k.new を読む)。
// 起動の組み合わせで動いているときは、共通の項目は pix68k.cfg に、そのほかは組み合わせのファイル (pix68k-<名前>.cfg、
// 一時ファイルは pix68k-<名前>.new) に書く (platform/boot_profile.h の writeProfileSettings())
bool CKernel::SaveConfig(const platform::Settings& s) {
    char name[64] = "pix68k.cfg", tmpName[64] = "pix68k.new";
    platform::ConfigText base, prof;
    if (!LoadSavedConfigText("pix68k.cfg", "pix68k.new", base)) return false;
    bool changedBase = true, changedProfile = false, ok;
    if (m_Profile[0] != 0) {
        platform::profileFileName(m_Profile, name, sizeof name);
        platform::profileFileName(m_Profile, tmpName, sizeof tmpName, true);
        if (!LoadSavedConfigText(name, tmpName, prof)) return false;
        ok = platform::writeProfileSettings(base, prof, s, m_FileSettings, changedBase, changedProfile);
        // 保存の途中で止まって .new だけが残っていれば、変えた項目がなくても書いて .cfg に戻す
        CString cfgPath;
        cfgPath.Format("%s/pix68k/%s", m_BootDrive, name);
        FILINFO fi;
        CString tmpPath;
        tmpPath.Format("%s/pix68k/%s", m_BootDrive, tmpName);
        if (f_stat(cfgPath, &fi) == FR_NO_FILE && f_stat(tmpPath, &fi) == FR_OK) changedProfile = true;
    } else {
        ok = platform::writeSettings(base, s, m_FileSettings);
    }
    if (!ok) {
        m_Logger.Write(FromMachine, LogWarning, "%s: cannot write a value (out of memory, or # in a name)",
                       m_Profile[0] != 0 ? name : "pix68k.cfg");
        return false;
    }
    MakeBootDir();
    // 組み合わせのファイルを先に書く (pix68k.cfg を書けなかったときに、組み合わせのほうだけが変わっているように)
    struct {
        bool write;
        const char* name;
        const char* tmp;
        const platform::ConfigText* text;
    } files[2] = {{changedProfile, name, tmpName, &prof}, {changedBase, "pix68k.cfg", "pix68k.new", &base}};
    for (const auto& f : files) {
        if (!f.write) continue;
        CString cfg, tmp;
        cfg.Format("%s/pix68k/%s", m_BootDrive, f.name);
        tmp.Format("%s/pix68k/%s", m_BootDrive, f.tmp);
        const FRESULT r = WriteFileReplacing(cfg, tmp, f.text->data(), (unsigned)f.text->size());
        if (r != FR_OK) {
            m_Logger.Write(FromMachine, LogWarning, "%s: write failed (FRESULT %d)", (const char*)cfg, (int)r);
            return false;
        }
        m_Logger.Write(FromMachine, LogNotice, "settings saved to %s (%u bytes)", (const char*)cfg,
                       (unsigned)f.text->size());
    }
    m_FileSettings = s;
    return true;
}

// ディスクを書き込み禁止にするか (ファイルの読み取り専用の属性、pix68k.cfg の write_protect=、boot ドライブが
// USB メモリのときの /pix68k の外のファイル、ほかのドライブで書き戻している同じファイル)。why に理由を入れる
bool CKernel::DiskReadOnly(int disk, const char* path, const char*& why) {
    why = nullptr;
    FILINFO fi;
    if (f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_RDO)) {
        why = "read-only file";
    } else if (platform::listHas(m_Settings.writeProtect, kDiskNames[disk])) {
        why = "write_protect=";
    } else if (!UsbPathWritable(path)) {
        why = "write-protected: outside /pix68k on the USB drive";
    } else {
        for (int i = 0; i < platform::DiskChannel::MAX_DISKS; i++)
            if (i != disk && m_DiskPath[i].GetLength() != 0 && SamePath(m_DiskPath[i], path)) {
                why = "write-protected: the same file is written back by another drive";
                break;
            }
    }
    return why != nullptr;
}

// フロッピーディスクのイメージを読んでドライブに入れる。name が空なら /pix68k/fd<N>.{xdf,dim,d88} を探す。
// 書き戻すときは <名前>.new に書いてから置き換えるので、置き換えの途中で止まって <名前> がなければ <名前>.new を読む
bool CKernel::LoadDisk(int drive, const char* name) {
    // 入っているディスクは、新しいイメージを読む前に書き戻す (同じファイルを入れ直すとき、書き戻す前の古い内容を
    // 読まないように。X68000 は止まっているので、このあと取り出すまでに書き込みは増えない)。
    // もう一方のドライブに同じファイルが入っていることもあるので、両方のドライブを書く
    for (int d = 0; d < 2 && m_pDiskWriter != nullptr; d++) {
        if (m_pDiskWriter->sync.attached(d) && !m_pDiskWriter->sync.flushPaused(d)) {
            m_Logger.Write(FromMachine, LogWarning, "fd%d: cannot write back fd%d; not changed", drive, d);
            return false;
        }
    }
    CString path, tmp;
    unsigned size = 0;
    u8* data = nullptr;
    bool usedTmp = false;
    if (name[0] != 0) {
        if (name[0] == '/') path.Format("%s%s", m_BootDrive, name);
        else path.Format("%s/pix68k/%s", m_BootDrive, name);
        tmp.Format("%s.new", (const char*)path);
        if (ReadWithFallback(path, tmp, data, size, usedTmp) != FR_OK || data == nullptr) {
            delete[] data;
            m_Logger.Write(FromMachine, LogWarning, "fd%d: cannot read %s", drive, (const char*)path);
            return false;
        }
    } else {
        static const char* const exts[] = {"xdf", "dim", "d88"};
        for (const char* ext : exts) {
            path.Format("%s/pix68k/fd%d.%s", m_BootDrive, drive, ext);
            tmp.Format("%s.new", (const char*)path);
            if (ReadWithFallback(path, tmp, data, size, usedTmp) == FR_OK && data != nullptr) break;
            delete[] data;
            data = nullptr;
        }
        if (data == nullptr) return false;
        // 見つけた名前を、動いている設定と pix68k.cfg の設定の両方に入れておく (メニューに出す。メニューで
        // 取り出してから「Save settings」を選ぶと fd<N>= を空にして書き、次からは探さない)
        const char* found = platform::baseName(path);
        platform::copyString(m_Settings.fd[drive], sizeof m_Settings.fd[drive], found);
        platform::copyString(m_FileSettings.fd[drive], sizeof m_FileSettings.fd[drive], found);
    }
    if (usedTmp)
        m_Logger.Write(FromMachine, LogWarning, "fd%d: %s not found; read %s", drive, (const char*)path, (const char*)tmp);
    // 入っているディスクと同じファイルを入れ直すときは、取り出してから書き戻すので、このドライブ自身は数えない
    const CString current = m_DiskPath[drive];
    m_DiskPath[drive] = "";
    const char* why = nullptr;
    const bool ro = DiskReadOnly(drive, usedTmp ? (const char*)tmp : (const char*)path, why);
    m_DiskPath[drive] = current;
    x68k::FloppyImage* image = new x68k::FloppyImage;
    if (image == nullptr || !image->load(data, size, ro, path)) {
        m_Logger.Write(FromMachine, LogWarning, "fd%d: %s is not a disk image (%u bytes)", drive, (const char*)path,
                       size);
        delete image;
        delete[] data;
        return false;
    }
    // data は image が指し続けるので、取り出すまで解放しない。入っていたディスクは書き戻してから取り出して解放する
    if (!EjectDisk(drive)) {
        m_Logger.Write(FromMachine, LogWarning, "fd%d: cannot write back the current disk; not changed", drive);
        delete image;
        delete[] data;
        return false;
    }
    m_pDisks[drive] = image;
    m_pDiskData[drive] = data;
    m_InUsePath[drive] = path;
    m_pEmulator->machine().insertDisk(drive, image);
    const char* mode = "written back to the file";
    if (image->writeProtected()) {
        mode = why ? why : "write-protected in the D88 header";
    } else if (!m_Settings.writeBack || m_pDiskWriter == nullptr) {
        mode = "writes are not saved (write_back=off)";
    } else {
        // 写しは一度確保したら使い回す (Circle は 512KB を超えるブロックを解放しても使い回さない)
        if (m_pFdShadow[drive] == nullptr) m_pFdShadow[drive] = new u8[kFdShadowBytes];
        if (m_pFdShadow[drive] != nullptr && size <= kFdShadowBytes) {
            memcpy(m_pFdShadow[drive], data, size);
            m_DiskPath[drive] = path;
            m_DiskTmp[drive] = tmp;
            m_pDiskWriter->sync.addFloppy(drive, image, m_DiskPath[drive], m_DiskTmp[drive], m_pFdShadow[drive], size);
        } else {
            mode = "writes are not saved (image too large)";
        }
    }
    m_Logger.Write(FromMachine, LogNotice, "fd%d: %s (%u bytes; %s)", drive, (const char*)path, size, mode);
    return true;
}

// ドライブのディスクを取り出し、イメージを解放する (入っていなければ何もしない)。書き戻すディスクは先に書く
// (X68000 を止めている間に呼ぶ)。書けなければ取り出さずに false
// discard なら、書けなくても取り出す (メニューで利用者が選んだとき。書けなかった内容は捨てる)
bool CKernel::EjectDisk(int drive, bool discard) {
    if (m_pDisks[drive] == nullptr) return true;
    if (m_pDiskWriter != nullptr && m_pDiskWriter->sync.attached(drive)) {
        if (!m_pDiskWriter->sync.flushPaused(drive)) {
            if (!discard) return false;
            m_Logger.Write(FromMachine, LogWarning, "fd%d: ejected without writing back", drive);
        }
        m_pDiskWriter->sync.remove(drive);
    }
    m_DiskPath[drive] = "";
    m_InUsePath[drive] = "";
    m_pEmulator->machine().ejectDisk(drive);
    delete m_pDisks[drive];
    delete[] m_pDiskData[drive];
    m_pDisks[drive] = nullptr;
    m_pDiskData[drive] = nullptr;
    return true;
}

// ハードディスク (SASI の HDF、SCSI の HDS) か CD-ROM (ISO) のイメージを読む。disk はディスクの番号
// (kDiskSasi、kDiskScsi + ID)。name が / で始まらなければ /pix68k から。
// 書き戻すときはファイルを開いたままにする (m_pDiskFile)。1 なら読めた、0 なら読まなかった、
// -1 なら読む途中で失敗した (メモリの見積もりが当てにならないので、残りのイメージも読まない)
// 開いたままにするファイルを閉じて解放する (SetupFastSeek() の表も)
static void CloseFil(FIL* f) {
    f_close(f);
    delete[] f->cltbl;
    delete f;
}

// cdBlockSize は CD-ROM のイメージのブロックの大きさ (ISO は 2048。CUE シートのイメージは、トラックの並びを別に持つので 1)
int CKernel::OpenHardDiskImage(int disk, const char* name, bool cdrom, x68k::HardDiskImage*& image, u32 cdBlockSize) {
    const char* label = kDiskNames[disk];
    const bool sasi = disk == kDiskSasi;
    CString path;
    if (name[0] == '/') path.Format("%s%s", m_BootDrive, name);
    else path.Format("%s/pix68k/%s", m_BootDrive, name);
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) {
        m_Logger.Write(FromMachine, LogWarning, "%s: cannot read %s", label, (const char*)path);
        return 0;
    }
    const FSIZE_t fileSize = fi.fsize;
    if (fileSize == 0) {
        m_Logger.Write(FromMachine, LogWarning, "%s: %s is empty; ignored", label, (const char*)path);
        return 0;
    }
    if (sasi && fileSize > kMaxHardDiskBytes) {
        m_Logger.Write(FromMachine, LogWarning, "%s: %s is too large (%u bytes; the limit is %u bytes); ignored", label,
                       (const char*)path, (unsigned)fileSize, kMaxHardDiskBytes);
        return 0;
    }
    // 全体を読むか、使うチャンクだけを読むか。空きが足りなければ読まない (確保に失敗すると Circle は全体を止める)。
    // 読んだ分だけ空きが減るので、イメージごとに順に調べれば、全部のイメージの合計も空きを超えない
    size_t freeBytes;
    const char* why = nullptr;
    bool ro = cdrom || DiskReadOnly(disk, path, why);
    const bool fits = fileSize <= kMaxScsiImageBytes && HeapHasRoom((size_t)fileSize, freeBytes);
    bool stream = (uint64_t)fileSize > m_StreamMinBytes || !fits;
    // write_back=off で書けるイメージは、書いた内容をメモリに残すため、入るなら全体を読む
    if (stream && fits && !ro && !m_Settings.writeBack) stream = false;
    // 全体がメモリに入るなら、すべてのチャンクの枠を用意する。一度読んだチャンクは空けず、残りのチャンクは X68000 を
    // 動かしながら裏で順に読むので (DiskCache::fillAhead())、しばらくすると全体を読んだのと同じになる (メモリも同じだけ使う)。
    // 枠の中身は、1GB より上のヒープ (Pi 4 / Pi 5) に入ればそこに、なければ 1GB より下のヒープに置く
    // (platform/disk_cache_size.h の planDiskCache())。入らなければ 4MB の枠にする
    const size_t heapFree = CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW);
    const uint64_t room = heapFree >= kHeapReserve ? heapFree - kHeapReserve : 0;
    const platform::DiskCachePlan plan = platform::planDiskCache(fileSize, room, HighHeapRoom(), kHeapModel);
    const unsigned slots = plan.slots;
    const size_t need = !stream ? (size_t)fileSize
                        : plan.high ? (size_t)platform::diskCacheTableBytes(fileSize, slots, kHeapModel)
                                    : (size_t)platform::diskCacheHeapBytes(fileSize, slots, kHeapModel);
    if (!HeapHasRoom(need, freeBytes)) {
        m_Logger.Write(FromMachine, LogWarning, "%s: %s: not enough memory (%u bytes; %u bytes free); ignored", label,
                       (const char*)path, (unsigned)need, (unsigned)freeBytes);
        return 0;
    }
    if (stream && !ro && !m_Settings.writeBack) {
        // 書いた内容をメモリに残しておけないので、書き込み禁止にする
        ro = true;
        why = "write-protected (write_back=off; the image is read on demand)";
    }
    const bool writeBack = !ro && m_Settings.writeBack;
    FIL* f = new FIL;
    const BYTE openMode = FA_READ | (writeBack ? FA_WRITE : 0);
    FRESULT r = f_open(f, path, openMode | FA_OPEN_EXISTING);
    if (r != FR_OK) {
        m_Logger.Write(FromMachine, LogWarning, "%s: cannot open %s (FRESULT %d)", label, (const char*)path, (int)r);
        delete f;
        return 0;
    }
    if (writeBack || stream) SetupFastSeek(f, label);
    const uint32_t blockSize = sasi ? x68k::Sasi::BLOCK_SIZE : cdrom ? cdBlockSize : 512;
    if (stream) {
        x68k::DiskCache* cache = new x68k::DiskCache;
        image = new x68k::HardDiskImage;
        // 枠の中身を 1GB より上に置くときは、ここで確保して渡す (空きは planDiskCache() で調べてある。
        // 渡したあとは、cache を捨てるときに ReleaseHighHeap() で返る)
        u8* highData = plan.high ? (u8*)CMemorySystem::HeapAllocate((size_t)slots * x68k::DiskCache::CHUNK, HEAP_HIGH) : nullptr;
        const bool ok = plan.high ? highData != nullptr && cache->init(fileSize, slots, highData, ReleaseHighHeap)
                                  : cache->init(fileSize, slots);
        if (!ok || !image->loadCached(cache, fileSize, blockSize, ro)) {
            m_Logger.Write(FromMachine, LogWarning, "%s: %s: cannot set up the cache (%u bytes); ignored", label,
                           (const char*)path, (unsigned)fileSize);
            CloseFil(f);
            delete image;
            image = nullptr;
            delete cache;
            return 0;
        }
        m_pDiskFile[disk] = f;
        m_pDiskWriter->fs.track(disk, f, path, openMode);
        if (writeBack) m_DiskPath[disk] = path;
        m_pDiskWriter->sync.addHard(disk, writeBack ? image : nullptr, f, cache);
        m_InUsePath[disk] = path;
        m_Logger.Write(FromMachine, LogNotice, "%s: %s (%u bytes; read on demand, %u KB cache%s%s; %s)", label,
                       (const char*)path, (unsigned)fileSize, slots * (x68k::DiskCache::CHUNK / 1024),
                       plan.high ? " above 1 GB" : "", plan.whole ? ", the rest is read in the background" : "",
                       cdrom ? "CD-ROM" : ro ? why : "written back to the file");
        return 1;
    }
    const unsigned size = (unsigned)fileSize;
    u8* data = new u8[size];
    UINT got = 0;
    r = data != nullptr ? f_read(f, data, size, &got) : FR_NOT_ENOUGH_CORE;
    if (r != FR_OK || got != size) {
        // Circle は 512KB を超えるブロックを解放しても使い回さないので、解放した分のメモリは戻らない
        m_Logger.Write(FromMachine, LogWarning, "%s: cannot read %s (%u bytes, FRESULT %d)", label, (const char*)path,
                       size, (int)r);
        CloseFil(f);
        delete[] data;
        return -1;
    }
    image = new x68k::HardDiskImage;
    if (!image->load(data, size, blockSize, ro)) {
        m_Logger.Write(FromMachine, LogWarning, "%s: %s is not an image (%u bytes)", label, (const char*)path, size);
        CloseFil(f);
        delete image;
        image = nullptr;
        delete[] data;
        return 0;
    }
    // data と image はつないでいる間ずっと使うので、解放しない
    const char* mode = cdrom ? "CD-ROM" : ro ? why : "written back to the file";
    m_InUsePath[disk] = path;
    if (writeBack) {
        m_pDiskFile[disk] = f;
        m_pDiskWriter->fs.track(disk, f, path, openMode);
        m_DiskPath[disk] = path;
        m_pDiskWriter->sync.addHard(disk, image, f);
    } else {
        CloseFil(f);
        if (!ro) mode = "writes are not saved (write_back=off)";
    }
    m_Logger.Write(FromMachine, LogNotice, "%s: %s (%u bytes; %s)", label, (const char*)path, size, mode);
    return 1;
}

// SASI ID 0 のハードディスクのイメージ (HDF) を読んでつなぐ。name が空なら /pix68k/hd0.hdf を探す
bool CKernel::LoadHardDisk(const char* name) {
    if (name[0] == 0) {
        // 既定の名前は、なければ何も出さない
        CString path;
        path.Format("%s/pix68k/hd0.hdf", m_BootDrive);
        FILINFO fi;
        if (f_stat(path, &fi) != FR_OK) return false;
        name = "hd0.hdf";
    }
    x68k::HardDiskImage* image = nullptr;
    if (OpenHardDiskImage(kDiskSasi, name, false, image) != 1) return false;
    m_pHardDisk = image;
    m_pEmulator->machine().attachSasi(0, image);
    return true;
}

static bool EndsWithCue(const char* name) {
    const size_t n = strlen(name);
    if (n < 4 || name[n - 4] != '.') return false;
    const char* e = name + n - 3;
    return (e[0] | 0x20) == 'c' && (e[1] | 0x20) == 'u' && (e[2] | 0x20) == 'e';
}

// CD-ROM のイメージの名前 (pix68k.cfg の cd=、メニューで選んだ名前) から、読むファイルとトラックの並びを決める。
// name が .cue なら CUE シート (platform/cue_sheet.h)。そこに書かれたファイル (BIN か WAV) を CUE シートと同じ
// ディレクトリから探す。ファイルが 1 つの CUE シートだけを扱う (複数のファイルは、1 つのイメージとして読む仕組みがない)。
// data に読むファイルの名前 (OpenHardDiskImage() に渡す形)、toc にトラックの並び (CUE シートでなければ nullptr。
// 使い終わったら delete する)。決められなければ false (why に理由)
bool CKernel::ResolveCdImage(const char* name, CString& data, x68k::CdToc*& toc, const char*& why) {
    toc = nullptr;
    if (!EndsWithCue(name)) {
        data = name;
        return true;
    }
    CString path;
    if (name[0] == '/') path.Format("%s%s", m_BootDrive, name);
    else path.Format("%s/pix68k/%s", m_BootDrive, name);
    unsigned textSize = 0;
    u8* text = ReadWholeFile(path, textSize);
    why = "cannot read the CUE sheet";
    if (text == nullptr) return false;
    platform::CueSheet* cue = new platform::CueSheet;
    bool ok = cue->parse((const char*)text, textSize);
    if (!ok) why = cue->error();
    char file[256];
    if (ok && cue->fileCount() != 1) {
        why = "CUE sheets with more than one FILE are not supported";
        ok = false;
    }
    if (ok && !cue->fileName(0, file, sizeof file)) {
        why = "the FILE name is too long";
        ok = false;
    }
    uint64_t base = 0, bytes = 0;
    if (ok) {
        // CUE シートと同じディレクトリ (name の最後の / まで) から
        const char* slash = nullptr;
        for (const char* q = name; *q; q++)
            if (*q == '/') slash = q;
        static char rel[600];
        const size_t dirLen = file[0] == '/' || slash == nullptr ? 0 : (size_t)(slash - name + 1);
        if (dirLen + strlen(file) + 1 > sizeof rel) {
            why = "the FILE name is too long";
            ok = false;
        } else {
            memcpy(rel, name, dirLen);
            strcpy(rel + dirLen, file);
            data = rel;
        }
    }
    if (ok) {
        CString dataPath;
        if (((const char*)data)[0] == '/') dataPath.Format("%s%s", m_BootDrive, (const char*)data);
        else dataPath.Format("%s/pix68k/%s", m_BootDrive, (const char*)data);
        FILINFO fi;
        if (f_stat(dataPath, &fi) != FR_OK || fi.fsize == 0) {
            why = "cannot read the FILE of the CUE sheet";
            ok = false;
        }
        bytes = ok ? (uint64_t)fi.fsize : 0;
        if (ok && cue->fileType(0) == platform::CueSheet::WAVE) {
            // WAV のヘッダを読んで、PCM のデータの位置を探す
            static u8 head[4096];
            FIL f;
            UINT got = 0;
            ok = f_open(&f, dataPath, FA_READ | FA_OPEN_EXISTING) == FR_OK;
            if (ok) {
                ok = f_read(&f, head, sizeof head, &got) == FR_OK;
                f_close(&f);
            }
            uint32_t offset = 0;
            why = "cannot read the WAV file";
            if (ok) ok = platform::wavData(head, got, (uint64_t)fi.fsize, offset, bytes, why);
            base = offset;
        }
    }
    if (ok) {
        toc = new x68k::CdToc;
        if (!cue->layout(&base, &bytes, *toc)) {
            why = cue->error();
            delete toc;
            toc = nullptr;
            ok = false;
        }
    }
    delete cue; // why は文字列の定数を指しているので、消しても使える
    delete[] text;
    return ok;
}

// メニューで CD-ROM の媒体を入れ替える (X68000 は止まっている)。name が空なら取り出すだけ。
// 1 なら入れ替えた、0 なら CD-ROM 装置をつないでいない (次の起動から)、-1 なら失敗 (入っている媒体はそのまま)、
// -2 なら前の媒体を外したあとで失敗した (装置は空)。X68000 が取り出しを禁止していても入れ替え、そのことを msg に入れる。
// 新しいイメージは、大きさによらず使うチャンクだけを読む。前の媒体のキャッシュの枠があれば使い回す
// (Circle は 512KB を超えるブロックを解放しても使い回さないので、入れ替えるたびに確保し直すとメモリが減っていく)。
// 前の媒体が全体をメモリに読んだイメージ (小さい ISO) だったときは、そのメモリは解放しない (1 回だけ)
int CKernel::changeCd(const char* name, platform::MenuText& msg) {
    x68k::Machine& m = m_pEmulator->machine();
    x68k::CdDrive* drive = m.cdDrive();
    if (drive == nullptr) return 0;
    if (m_pDiskWriter == nullptr) {
        msg.add("cannot change the CD-ROM");
        return -1;
    }
    const int disk = kDiskScsi + drive->id();
    const char* label = kDiskNames[disk];
    FIL* f = nullptr;
    x68k::CdToc* toc = nullptr;
    CString path;
    uint64_t fileSize = 0;
    if (name[0] != 0) {
        // 読めなければ、入っている媒体はそのまま
        CString dataName;
        const char* why = "";
        if (platform::isReplaceTempName(name)) {
            msg.add("cannot use a temporary file: ").add(name);
            return -1;
        }
        if (!ResolveCdImage(name, dataName, toc, why)) {
            m_Logger.Write(FromMachine, LogWarning, "%s: %s: %s", label, name, why);
            msg.add("cannot load ").add(name);
            return -1;
        }
        if (((const char*)dataName)[0] == '/') path.Format("%s%s", m_BootDrive, (const char*)dataName);
        else path.Format("%s/pix68k/%s", m_BootDrive, (const char*)dataName);
        FILINFO fi;
        f = new FIL;
        if (f_stat(path, &fi) != FR_OK || fi.fsize == 0 || f_open(f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
            m_Logger.Write(FromMachine, LogWarning, "%s: cannot open %s", label, (const char*)path);
            msg.add("cannot load ").add(name);
            delete f;
            delete toc;
            return -1;
        }
        fileSize = fi.fsize;
    }
    // 入っている媒体を外す
    const bool prevented = drive->prevented();
    m.changeCd(nullptr);
    m_pDiskWriter->sync.remove(disk);
    m_pDiskWriter->fs.untrack(disk);
    if (m_pDiskFile[disk] != nullptr) {
        CloseFil(m_pDiskFile[disk]);
        m_pDiskFile[disk] = nullptr;
    }
    m_InUsePath[disk] = "";
    m_Settings.cd[0] = 0;
    if (m_pCdImage != nullptr) m_pCdImage->unload();
    if (name[0] == 0) {
        m_Logger.Write(FromMachine, LogNotice, "%s: CD-ROM ejected%s", label, prevented ? " (was locked by the X68000)" : "");
        if (prevented) platform::menuCdChangedText(msg, false, true);
        return 1;
    }
    // キャッシュ: 前の媒体のものに枠が十分あれば使い回す。なければ作る
    const char* why = nullptr;
    if (m_pCdCache != nullptr && (m_pCdCache->slots() < 32 || !m_pCdCache->reinit(fileSize))) {
        delete m_pCdCache;
        m_pCdCache = nullptr;
    }
    if (m_pCdCache == nullptr) {
        // 1GB より上のヒープに全体が入れば、すべてのチャンクの枠をそこに置く (残りは裏で読む)。入らなければ 4MB の枠
        // (1GB より下のヒープには、メニューから入れ替えるときは全体の枠を置かない。作り直すたびに減るため)。
        // 作った枠は、次の媒体にも使い回す (次の媒体のほうが大きければ、全体は入らず、使うチャンクだけを読む)
        size_t freeBytes = CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW);
        const uint64_t room = freeBytes >= kHeapReserve ? freeBytes - kHeapReserve : 0;
        const platform::DiskCachePlan plan = platform::planDiskCache(fileSize, room, HighHeapRoom(), kHeapModel);
        const bool high = plan.high;
        const unsigned slots = high ? plan.slots : platform::DISK_CACHE_SLOTS;
        const size_t need = high ? (size_t)platform::diskCacheTableBytes(fileSize, slots, kHeapModel)
                                 : (size_t)platform::diskCacheHeapBytes(fileSize, slots, kHeapModel);
        if (!HeapHasRoom(need, freeBytes)) {
            why = "not enough memory";
        } else {
            m_pCdCache = new x68k::DiskCache;
            u8* highData = high ? (u8*)CMemorySystem::HeapAllocate((size_t)slots * x68k::DiskCache::CHUNK, HEAP_HIGH) : nullptr;
            const bool ok = high ? highData != nullptr && m_pCdCache->init(fileSize, slots, highData, ReleaseHighHeap)
                                 : m_pCdCache->init(fileSize, slots);
            if (!ok) {
                delete m_pCdCache;
                m_pCdCache = nullptr;
                why = "cannot set up the cache";
            }
        }
    }
    if (why == nullptr) {
        if (m_pCdImage == nullptr) m_pCdImage = new x68k::HardDiskImage;
        if (!m_pCdImage->loadCached(m_pCdCache, fileSize, toc != nullptr ? 1 : 2048, true)) why = "not an image";
    }
    if (why != nullptr) {
        m_Logger.Write(FromMachine, LogWarning, "%s: %s: %s; the CD-ROM drive is empty", label, (const char*)path, why);
        platform::menuCdEmptyText(msg, name);
        CloseFil(f);
        delete toc;
        return -2;
    }
    SetupFastSeek(f, label);
    m_pDiskFile[disk] = f;
    m_pDiskWriter->fs.track(disk, f, path, FA_READ);
    m_pDiskWriter->sync.addHard(disk, nullptr, f, m_pCdCache);
    m_InUsePath[disk] = path;
    m.changeCd(m_pCdImage, toc);
    delete toc;
    platform::copyString(m_Settings.cd, sizeof m_Settings.cd, name);
    m_Logger.Write(FromMachine, LogNotice, "%s: CD-ROM changed to %s (%u bytes; read on demand, %u KB cache%s)", label,
                   (const char*)path, (unsigned)fileSize, m_pCdCache->slots() * (x68k::DiskCache::CHUNK / 1024),
                   m_pCdCache->whole() ? ", the rest is read in the background" : "");
    if (prevented) {
        m_Logger.Write(FromMachine, LogNotice, "%s: the drive was locked by the X68000", label);
        platform::menuCdChangedText(msg, true, true);
    }
    return 1;
}

// 外付け SCSI ボードの ROM と、SCSI の装置 (pix68k.cfg の scsi_ex=、sc0= .. sc6=、cd=) を読んでつなぐ
void CKernel::LoadScsi() {
    x68k::Machine& m = m_pEmulator->machine();
    if (m_Settings.scsiEx) {
        static const char* const dirs[] = {"/pix68k/", "/"};
        bool ok = false;
        for (const char* dir : dirs) {
            CString path;
            if (m_Settings.scsiExRom[0] == '/') path.Format("%s%s", m_BootDrive, m_Settings.scsiExRom);
            else if (m_Settings.scsiExRom[0] != 0) path.Format("%s/pix68k/%s", m_BootDrive, m_Settings.scsiExRom);
            else path.Format("%s%sSCSIEXROM.DAT", m_BootDrive, dir);
            unsigned n;
            u8* rom = ReadWholeFile(path, n);
            if (rom == nullptr) continue;
            ok = m.setScsiExRom(rom, n);
            delete[] rom;
            m_Logger.Write(FromMachine, ok ? LogNotice : LogWarning, "%s (%u bytes)%s", (const char*)path, n,
                           ok ? "" : ": not a SCSI ROM (8192 bytes or less); ignored");
            break;
        }
        if (!ok) m_Logger.Write(FromMachine, LogWarning, "scsi_ex=on, but the SCSI ROM was not found; no SCSI board");
    }
    const bool builtIn = m_Config.cpuModel != m68k::CpuModel::M68000; // X68030 と 060turbo は内蔵 SCSI を持つ
    for (int id = 0; id < 7; id++) {
        const bool cd = m_Settings.cd[0] != 0 && id == m_Settings.cdId;
        const char* name = cd ? m_Settings.cd : m_Settings.sc[id];
        if (name[0] == 0) continue;
        if (cd && m_Settings.sc[id][0] != 0)
            m_Logger.Write(FromMachine, LogWarning, "sc%d=%s: SCSI ID %d is used by cd=; ignored", id, m_Settings.sc[id],
                           id);
        if (!m.hasScsiEx() && !builtIn) {
            m_Logger.Write(FromMachine, LogWarning, "SCSI ID %d: %s: no SCSI interface (scsi_ex=on or cpu=68030)", id,
                           name);
            continue;
        }
        x68k::HardDiskImage* image = nullptr;
        // CD-ROM: CUE シートなら、読むファイル (BIN か WAV) とトラックの並びを決める
        x68k::CdToc* toc = nullptr;
        CString cdData;
        if (cd) {
            const char* why = "";
            if (!ResolveCdImage(name, cdData, toc, why)) {
                m_Logger.Write(FromMachine, LogWarning, "cd=%s: %s; ignored", name, why);
                continue;
            }
            name = cdData;
        }
        const int r = OpenHardDiskImage(kDiskScsi + id, name, cd, image, toc != nullptr ? 1 : 2048);
        if (r < 0) {
            m_Logger.Write(FromMachine, LogWarning, "the remaining SCSI images are skipped");
            delete toc;
            break;
        }
        if (r == 0) {
            delete toc;
            continue;
        }
        if (m.hasScsiEx()) m.attachScsiEx(id, image, cd, toc);
        else m.attachScsi(id, image, cd, toc);
        if (cd) {
            m_pCdImage = image;
            m_pCdCache = image->cache();
            const x68k::CdToc& t = m.cdDrive()->toc();
            int audio = 0;
            for (int i = 0; i < t.count; i++) audio += t.tracks[i].audio();
            m_Logger.Write(FromMachine, LogNotice, "SCSI ID %d (%s): CD-ROM (%u blocks, %d tracks, %d audio)", id,
                           m.hasScsiEx() ? "board" : "built-in", (unsigned)t.leadOut, t.count, audio);
        } else {
            m_Logger.Write(FromMachine, LogNotice, "SCSI ID %d (%s)", id, m.hasScsiEx() ? "board" : "built-in");
        }
        delete toc;
    }
}

// X68000 を止めている間に、書き戻すディスクの書いた分をすべて書く。only >= 0 ならそのディスクだけ
bool CKernel::FlushDisks(int only) {
    if (m_pDiskWriter == nullptr) return true;
    const u64 t0 = CTimer::GetClockTicks64();
    const bool ok = m_pDiskWriter->sync.flushPaused(only);
    const unsigned ms = (unsigned)((CTimer::GetClockTicks64() - t0) / (CLOCKHZ / 1000));
    // 続けて失敗したとき (deploy の再起動を待たせている間は 1 秒ごとに呼ぶ) は、1 回目と 10 回ごとに出す
    static unsigned failedInRow = 0;
    if (!ok) {
        if (failedInRow++ % 10 == 0)
            m_Logger.Write(FromMachine, LogWarning, "disks: cannot write back (%u ms, %u times in a row)", ms, failedInRow);
    } else {
        failedInRow = 0;
        if (ms >= 10) m_Logger.Write(FromMachine, LogNotice, "disks: written back in %u ms", ms);
    }
    return ok;
}

// 動いている X68000 を止めてから書き戻す (再起動の前。止めたまま戻る)
bool CKernel::PauseAndFlushDisks() {
    if (m_pDiskWriter == nullptr) return true;
    m_pEmulator->requestPause(true);
    const u64 t0 = CTimer::GetClockTicks64();
    while (!m_pEmulator->paused()) {
        asm volatile("sev");
        if (CTimer::GetClockTicks64() - t0 > 5 * CLOCKHZ) {
            m_Logger.Write(FromMachine, LogWarning, "disks: core 1 did not pause; not written back");
            return false;
        }
    }
    return FlushDisks();
}

// 大きなファイルの中の位置を、FAT をたどらずに変えられるようにする (FatFs の fast seek。クラスタの並びの表を作る)
void CKernel::SetupFastSeek(FIL* f, const char* label) {
    if (!CreateLinkMap(f))
        m_Logger.Write(FromMachine, LogWarning, "%s: fast seek is not available; seeking may be slow", label);
}

// 大きなイメージのチャンクの読み込みの時間と、書き戻した量 (前の報告から変わったときだけ。実機で SD カードの速さを見る)
void CKernel::ReportDisks() {
    if (m_pDiskWriter == nullptr) return;
    CDiskFs& fs = m_pDiskWriter->fs;
    // 裏で読んでいたイメージの全体がメモリに入ったら、1 回だけ出す (CD-ROM の媒体を入れ替えたら、読み終えたときにまた出す)
    static const x68k::DiskCache* filled[platform::DiskChannel::MAX_DISKS];
    for (int i = 0; i < platform::DiskChannel::MAX_DISKS; i++) {
        const x68k::DiskCache* c = m_pDiskWriter->sync.cache(i);
        // 頼み終えただけでなく、すべてのチャンクが読めたときに出す (読めなかったチャンクが残っていれば出さない)
        if (c == nullptr || !c->allLoaded()) {
            filled[i] = nullptr;
            continue;
        }
        if (filled[i] == c) continue;
        filled[i] = c;
        m_Logger.Write(FromMachine, LogNotice, "%s: the whole image is in memory (%u KB)", kDiskNames[i],
                       c->slots() * (x68k::DiskCache::CHUNK / 1024));
    }
    static u64 shownReads = ~0ull, shownBytes = ~0ull;
    const u64 bytes = m_pDiskWriter->sync.bytesWritten();
    if (fs.readCount == shownReads && bytes == shownBytes) return;
    shownReads = fs.readCount;
    shownBytes = bytes;
    m_Logger.Write(FromMachine, LogNotice, "disks: %u chunk reads (avg %u us, max %u us), %u KB written back (in total)",
                   (unsigned)fs.readCount, fs.readCount ? (unsigned)(fs.readUs / fs.readCount) : 0, fs.readMaxUs,
                   (unsigned)(bytes / 1024));
}

// 書き戻すハードディスクのファイルを閉じる (再起動の前)
void CKernel::CloseDiskFiles() {
    for (FIL*& f : m_pDiskFile) {
        if (f == nullptr) continue;
        CloseFil(f);
        f = nullptr;
    }
}

// X68030 と 060turbo の SRAM は X68000 のものと分ける (メモリの容量などが違う)
static const char* SramName(const x68k::Config& config, int i) {
    static const char* const kNames[2] = {"SRAM.DAT", "SRAM.BAK"};
    static const char* const kNames30[2] = {"SRAM30.DAT", "SRAM30.BAK"};
    static const char* const kNames060[2] = {"SRAM060.DAT", "SRAM060.BAK"};
    if (config.cpuModel == m68k::CpuModel::M68060) return kNames060[i];
    return config.cpuModel == m68k::CpuModel::M68030 ? kNames30[i] : kNames[i];
}

// SRAM.DAT と SRAM.BAK のうち、正しくて新しいほうを読む (形式は x68k/sram_file.h)
void CKernel::LoadSram() {
    m_pSramFile = new u8[x68k::SramSlots::FILE_SIZE];
    u8* data[2];
    unsigned size[2];
    for (int i = 0; i < 2; i++) {
        CString path;
        path.Format("%s/pix68k/%s", m_BootDrive, SramName(m_Config, i));
        data[i] = ReadWholeFile(path, size[i]);
        m_SramSlots.load(i, data[i], size[i]);
    }
    int n = m_SramSlots.newest();
    if (n >= 0) {
        memcpy(m_pEmulator->machine().sram(), data[n], x68k::Machine::SRAM_SIZE);
        m_Logger.Write(FromMachine, LogNotice, "SRAM: %s/pix68k/%s (generation %u)", m_BootDrive, SramName(m_Config, n),
                       (unsigned)(m_SramSlots.nextGeneration() - 1));
    } else {
        m_Logger.Write(FromMachine, LogNotice, "SRAM: no valid %s/pix68k/%s; it will be created when SRAM is written",
                       m_BootDrive, SramName(m_Config, 0));
    }
    for (int i = 0; i < 2; i++) {
        if (data[i] != nullptr && n != i) {
            uint32_t gen;
            if (!x68k::SramSlots::parse(data[i], size[i], gen))
                m_Logger.Write(FromMachine, LogWarning, "SRAM: %s is broken (%u bytes); ignored", SramName(m_Config, i), size[i]);
        }
        delete[] data[i];
    }
}

// 古いほうのファイル (または壊れているほう) に書く。電源が切れても、もう一方の前の内容が残る
bool CKernel::SaveSram(const u8* data) {
    if (m_pSramFile == nullptr) return false;
    MakeBootDir();
    CString path;
    const int slot = m_SramSlots.nextSlot();
    const uint32_t gen = m_SramSlots.nextGeneration();
    x68k::SramSlots::build(data, gen, m_pSramFile);
    path.Format("%s/pix68k/%s", m_BootDrive, SramName(m_Config, slot));
    m_SramSlots.writing(slot);
    FIL f;
    FRESULT r = CanWriteNewFile(path, x68k::SramSlots::FILE_SIZE) ? f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) : FR_DENIED;
    UINT written = 0;
    if (r == FR_OK) {
        r = f_write(&f, m_pSramFile, x68k::SramSlots::FILE_SIZE, &written);
        if (r == FR_OK) r = f_sync(&f);
        FRESULT rc = f_close(&f);
        if (r == FR_OK) r = rc;
    }
    if (r != FR_OK || written != x68k::SramSlots::FILE_SIZE) {
        m_Logger.Write(FromMachine, LogWarning, "%s: write failed (FRESULT %d); will retry", (const char*)path, (int)r);
        return false;
    }
    m_SramSlots.saved(slot, gen);
    m_Logger.Write(FromMachine, LogNotice, "SRAM saved to %s (generation %u)", (const char*)path, (unsigned)gen);
    return true;
}

// 起動の時間をログに出す。Circle が読む ARM のカウンタは電源を入れたときから数えているはずなので、電源を入れてからの時間になる
// (ファームウェアの時間がどこまで入るかは実機で確かめていない)。前にログに出した段階からの時間も出す。
// すでに時刻を取ってある段階 (Initialize() の中の段階。ロガーを用意する前のものもある) は、その時刻で出す
void CKernel::LogBootTime(BootStage stage, const char* what) {
    if (m_BootTicks[stage] == 0) m_BootTicks[stage] = CTimer::GetClockTicks64();
    const u64 now = m_BootTicks[stage];
    // +M は、時刻がこの段階のすぐ前の段階からの時間。ログに出す順と時刻の順が違うことがある (first frame shown は
    // 合成するコアが写した時刻を、USB の装置を探し終えてから出す)
    u64 prev = 0;
    for (int s = 0; s < BootStages; s++) {
        const u64 t = __atomic_load_n(&m_BootTicks[s], __ATOMIC_RELAXED);
        if (s != stage && t <= now && t > prev) prev = t;
    }
    if (prev == 0) prev = now;
    m_Logger.Write(FromMachine, LogNotice, "boot time: %s at %u ms since power-on (+%u ms)", what,
                   (unsigned)(now / (CLOCKHZ / 1000)), (unsigned)((now - prev) / (CLOCKHZ / 1000)));
}

// ROM があれば X68000 の用意をする (コア 1 はまだ動かさない)。なければ false
bool CKernel::StartMachine() {
    m_TextDumpSeconds = m_Options.GetAppOptionDecimal("pix68k_textdump", 0);
    if (m_BootDrive == nullptr) {
        m_Logger.Write(FromMachine, LogWarning, "no boot drive; cannot read the ROMs, showing the test pattern");
        return false;
    }
    LogBootTime(BootConfig, "reading pix68k.cfg");
    ReadConfig();
    const char* iplName = m_Config.cpuModel == m68k::CpuModel::M68000 ? "IPLROM.DAT" : "IPLROM30.DAT";
    u8* ipl = nullptr;
    u8* scsiRom = nullptr;
    // 060turbo は 68060 で動く IPL ROM (IPLROM060.DAT) を先に探す。なければ X68030 の IPL ROM に手を入れて使う。
    // IPLROM060.DAT を使うときは、ROM30_060.DAT があれば $FC0000 からの ROM にする
    if (m_Config.cpuModel == m68k::CpuModel::M68060) {
        ipl = ReadRom("IPLROM060.DAT", x68k::Machine::IPL_SIZE);
        if (ipl != nullptr) {
            iplName = "IPLROM060.DAT";
            scsiRom = ReadRom("ROM30_060.DAT", x68k::Machine::SCSI_ROM_SIZE);
        }
    }
    if (ipl == nullptr) ipl = ReadRom(iplName, x68k::Machine::IPL_SIZE);
    if (ipl == nullptr) {
        m_Logger.Write(FromMachine, LogWarning, "%s (128KB) not found in %s/pix68k/ or %s/; showing the test pattern",
                       iplName, m_BootDrive, m_BootDrive);
        delete[] m_pBootCgrom; // メニューのために読んだ CGROM
        m_pBootCgrom = nullptr;
        return false;
    }
    // 起動の組み合わせのメニューで読んでいれば、それを使う (boot_menu.cpp)
    u8* cg = m_pBootCgrom != nullptr ? m_pBootCgrom : ReadRom("CGROM.DAT", x68k::Machine::CG_SIZE);
    m_pBootCgrom = nullptr;
    LogBootTime(BootRoms, "ROMs read");
    if (cg == nullptr) m_Logger.Write(FromMachine, LogWarning, "CGROM.DAT (768KB) not found; characters will be blank");

    Emulator* emu = new Emulator;
    bool ok = emu != nullptr && emu->init(ipl, cg, kDefaultLocalTime, m_Config, m_RenderCore, m_SoundCore, scsiRom);
    delete[] ipl;
    delete[] cg;
    delete[] scsiRom;
    if (!ok) {
        m_Logger.Write(FromMachine, LogError, "out of memory; showing the test pattern");
        delete emu;
        return false;
    }
    emu->setMaxSpeed(m_MaxSpeed);
    emu->setEventStream(m_EventStreamUs != 0);
    m_pEmulator = emu;
    // 拡張ボード (音の合成をコア 3 で始める前につなぐ)
    if (m_Settings.fpuBoard) {
        const int boards = m_Settings.fpuBoard >= 2 ? 2 : 1;
        ok = emu->machine().setFpuBoards(boards);
        m_Logger.Write(FromMachine, ok ? LogNotice : LogWarning, "FPU board (CZ-6BP1, MC68881) at $E9E000%s%s",
                       boards >= 2 ? " and $E9E080" : "", ok ? "" : ": out of memory");
    }
    if (m_Settings.mercury) {
        ok = emu->machine().setMercury(true);
        m_Logger.Write(FromMachine, ok ? LogNotice : LogWarning, "Mercury Unit at $ECC080%s", ok ? "" : ": out of memory");
    }
    if (m_Settings.neptune) {
#if RASPPI <= 3
        // Pi 3 の有線の Ethernet は USB につながっている。neptune_mac=pi で Pi の MAC アドレスを使えるよう、
        // Neptune-X をつなぐときは X68000 を動かす前に USB の装置を探す (起動はその分遅くなる)
        ScanUsb(false);
#endif
        u8 mac[6];
        CNetBridge* net = new CNetBridge(m_Logger);
        if (net != nullptr && net->Initialize(m_Settings.neptuneLink, m_Settings.neptuneMac, mac) &&
            emu->machine().setNeptune(true)) {
            emu->machine().neptune()->setMac(mac);
            emu->machine().neptune()->setLink(net->Link());
            m_pNetBridge = net;
            if (m_Options.GetAppOptionDecimal("pix68k_net_probe", 0) != 0) net->EnableProbe();
            m_Logger.Write(FromMachine, LogNotice, "Neptune-X at $ECE000 (MAC %02x:%02x:%02x:%02x:%02x:%02x)", mac[0],
                           mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            delete net;
            m_Logger.Write(FromMachine, LogWarning, "Neptune-X is not connected");
        }
    }
#if RASPPI >= 4
    // 明示的な opt-in。接続済みの第2ディスプレイだけを使う。
    const unsigned tsPort = m_Options.GetAppOptionDecimal("pix68k_ts6bga", 0);
    if (tsPort != 0) {
        if ((tsPort == 1 || tsPort == 2) && CBcmFrameBuffer::GetNumDisplays() >= 2) {
            CBcmPropertyTags tags;
            TPropertyTagSimple select;
            select.nValue = 1;
            // Pi 5 は表示サイズの変更を無視するので、ファームウェアの実サイズを使う。
            // display 選択に失敗したときは primary のバッファを確保し直さない。
            const bool selected = tags.GetTag(PROPTAG_SET_DISPLAY_NUM, &select, sizeof select, 4);
            const unsigned selectedValue = select.nValue;
            // Pi 5 は成功時も nValue=0 を返す。Circle と同じくタグの成否で判定する。
            if (selected)
                m_TsDisplay = new CBcmFrameBuffer(0, 0, 32, 0, 0, 1);
            TPropertyTagSimple depth;
            depth.nValue = 0;
            const bool initialized = m_TsDisplay && m_TsDisplay->Initialize();
            const bool depthRead = initialized && tags.GetTag(0x00040005, &depth, sizeof depth);
            bool ready = initialized && depthRead && depth.nValue == 32;
            m_Logger.Write(FromMachine, LogNotice,
                "TS-6BGA HDMI 2: select=%u value=%u, init=%u depth-query=%u depth=%u, buffer=%X size=%u pitch=%u (%ux%u)",
                unsigned(selected), selectedValue, unsigned(initialized), unsigned(depthRead), depth.nValue,
                m_TsDisplay ? m_TsDisplay->GetBuffer() : 0, m_TsDisplay ? m_TsDisplay->GetSize() : 0,
                m_TsDisplay ? m_TsDisplay->GetPitch() : 0, m_TsDisplay ? m_TsDisplay->GetWidth() : 0,
                m_TsDisplay ? m_TsDisplay->GetHeight() : 0);
            if (ready) {
                const auto* primary = m_Screen.GetFrameBuffer();
                const uint64_t lo = m_TsDisplay->GetBuffer(), hi = lo + m_TsDisplay->GetSize();
                ready = m_TsDisplay->GetPitch() >= m_TsDisplay->GetWidth() * 4 &&
                        m_TsDisplay->GetSize() >= uint64_t(m_TsDisplay->GetPitch()) * m_TsDisplay->GetHeight() &&
                        (!primary || hi <= primary->GetBuffer() ||
                         lo >= uint64_t(primary->GetBuffer()) + primary->GetSize());
            }
            if (!ready || !emu->initTs6bga(tsPort == 2 ? 0xef0000 : 0xee0000)) {
                delete m_TsDisplay; m_TsDisplay = nullptr;
                m_Logger.Write(FromMachine, LogWarning, "TS-6BGA: cannot initialize HDMI 2 or allocate VRAM");
            } else {
                SetPixelOrderBgr(); // Initialize() が選んだ display 1 に設定
                for (unsigned y = 0; y < m_TsDisplay->GetHeight(); ++y)
                    platform::frame_scale_detail::fillRow<platform::DeviceStore>((uint32_t*)(uintptr_t)
                        (m_TsDisplay->GetBuffer() + y * m_TsDisplay->GetPitch()), 0xff000000u,
                        m_TsDisplay->GetWidth());
                m_Logger.Write(FromMachine, LogNotice, "TS-6BGA at $%X, HDMI 2 %ux%u, scanout on core 0",
                               tsPort == 2 ? 0xef0000 : 0xee0000, m_TsDisplay->GetWidth(), m_TsDisplay->GetHeight());
            }
            select.nValue = 0;
            tags.GetTag(PROPTAG_SET_DISPLAY_NUM, &select, sizeof select, 4);
        } else {
            m_Logger.Write(FromMachine, LogWarning, "TS-6BGA: use pix68k_ts6bga=1 or 2; two connected displays required");
        }
    }
#endif
    // RS-232C (SCC のチャネル A): rs232c=tcp:PORT なら、ネットワークの口 (net=on) の TCP のタスクとつなぐリングを、
    // コア 1 を動かす前につなぐ (つながる相手が来るまでは DCD と CTS が 0)
    {
        unsigned port = 0;
        char addr[64];
        const platform::Rs232cLink kind = platform::parseRs232c(m_Settings.rs232c, port, addr, sizeof addr);
        if (kind == platform::Rs232cLink::BAD) {
            m_Logger.Write(FromMachine, LogWarning, "rs232c=%s: use off or tcp:PORT; RS-232C is not connected", m_Settings.rs232c);
        } else if (kind == platform::Rs232cLink::TCP) {
            m_pSerialLink = new NetSerialLink;
            if (m_pSerialLink != nullptr) {
                emu->machine().setSerialLink(m_pSerialLink);
                m_Logger.Write(FromMachine, LogNotice, "RS-232C on TCP port %u%s", port,
                               m_Settings.net ? "" : " (needs net=on; nothing can connect)");
            }
        }
    }
    StartSound();
    if (m_Settings.midi) StartMidi();
    // ディスクの書き戻し (write_back=on のとき) と、大きなイメージの読み込み。受け渡しのバッファはコア 1 を動かす前に確保する
    m_StreamMinBytes = (u64)m_Options.GetAppOptionDecimal("pix68k_stream_min", kStreamMinMB) << 20;
    m_pDiskWriter = new CDiskWriter;
    m_pDiskWriter->fs.log = &m_Logger;
    if (m_Settings.writeBack) emu->setDiskChannel(&m_pDiskWriter->channel);
    LogBootTime(BootSetUp, "X68000 set up");
    for (int d = 0; d < 2; d++)
        if (m_Settings.fd[d][0] != 0 || !m_FdExplicit[d]) LoadDisk(d, m_Settings.fd[d]);
    LoadHardDisk(m_Settings.hd0);
    LoadScsi();
    LoadSram();
    LogBootTime(BootDisks, "disk images read");
    // 変わった行だけをフレームバッファに写すための、前に写した画面の写し (platform/frame_scale.h の FrameRowCache)。
    // カーネルのオプション pix68k_fb_rows=0 なら使わず、毎回すべての行を写す (前と比べるため)
    if (m_Options.GetAppOptionDecimal("pix68k_fb_rows", 1) != 0) {
        m_RowCache.pixels = new u32[(size_t)Emulator::PITCH * x68k::Renderer::MAX_HEIGHT];
        if (m_RowCache.pixels != nullptr) {
            m_RowCache.pitch = Emulator::PITCH;
            m_RowCache.rows = x68k::Renderer::MAX_HEIGHT;
        } else {
            m_Logger.Write(FromMachine, LogWarning, "screen: out of memory; copying every row to the framebuffer");
        }
    } else {
        m_Logger.Write(FromMachine, LogNotice, "screen: pix68k_fb_rows=0; copying every row to the framebuffer");
    }
    StartCrt();
    if (m_TextDumpSeconds != 0) {
        if (!emu->canDumpText()) {
            m_Logger.Write(FromMachine, LogWarning, "pix68k_textdump needs CGROM.DAT; disabled");
        } else {
            m_pLastDump = new char[x68k::TEXT_ROWS][x68k::TEXT_COLS + 1];
            if (m_pLastDump != nullptr) memset(m_pLastDump, 0, sizeof(char[x68k::TEXT_ROWS][x68k::TEXT_COLS + 1]));
        }
    }
    return true;
}

TShutdownMode CKernel::RunMachine() {
    m_Logger.Write(FromMachine, LogNotice, "starting the X68000 on core 1; from now on the log goes to the serial port only");
    m_Tee.EnableSecond(false);
    // コア 1 を起動する前に設定する (Initialize() がコアを起動する)
    m_Cores.emulator = m_pEmulator;
    const unsigned failFrom = m_Options.GetAppOptionDecimal("pix68k_fail_core", 0);
    if (failFrom >= 1 && failFrom < CORES) {
        m_Cores.failFrom = failFrom;
        m_Logger.Write(FromMachine, LogWarning, "pix68k_fail_core=%u: cores %u-%u are treated as not started",
                       failFrom, failFrom, CORES - 1);
    }
    // コア 0 が USB の転送で止まる間は、合成するコアが画面を写す (WithComposerPresenting())
    m_pEmulator->setPresenter(PresentFromComposer, this);
    bool allStarted = m_Cores.Initialize();
    if (m_Cores.failFrom != 0) allStarted = false; // Initialize() が FALSE を返したのと同じに扱う
    if (!allStarted) {
        // Pi 3 の Initialize() は起動できなかったコアで止まる (それより後のコアは起動しない)。
        // Pi 5 では後のコアが動いていることもある。どちらでも、コアごとに入ってきたかどうかで決める。
        // X68000 を動かすコア 1 が起動していなければ続けられない。コア 2 / 3 だけなら、その仕事はコア 1 が行う
        // (Emulator はコア 2 / 3 が 1 秒待っても入ってこなければ、コア 1 で合成する)
        const u64 t0 = CTimer::GetClockTicks64();
        while (!(__atomic_load_n(&m_Cores.startedMask, __ATOMIC_ACQUIRE) & (1u << 1)) &&
               CTimer::GetClockTicks64() - t0 < CLOCKHZ) {
        }
        const unsigned mask = __atomic_load_n(&m_Cores.startedMask, __ATOMIC_ACQUIRE);
        if (!(mask & (1u << 1))) {
            m_Logger.Write(FromMachine, LogError, "core 1 did not start; cannot run the X68000");
            return ShutdownHalt;
        }
        m_Logger.Write(FromMachine, LogWarning, "not all secondary cores started (mask=%X); core 1 does their work",
                       mask);
    }

    const u64 start = CTimer::GetClockTicks64();
    u64 lastSpeed = start, lastMaxSpeed = start, lastDump = start, lastSram = start, lastAlive = start;
    u64 lastDeployTry = 0;
    unsigned lastMask = 0;
    bool renderReported = false;
    LogBootTime(BootCore1, "core 1 started");
    StartAutoType();
    // X68000 が動き出してから USB の装置を探す。探し終えるまで、コア 0 はこの下の主ループに入らないが、
    // その間は合成するコアが画面を写し、コア 0 は探している途中の待ち時間に大きなイメージのチャンクを読む
    // (WithComposerPresenting()、ServiceDuringUsbScan())
    ScanUsb(true);
    // ネットワークの口は USB の装置を探し終えてから始める (Pi 3 の Ethernet は USB の先にある)
    StartHostNet();
    // MIDI の SoundFont か MT-32 の ROM (midi=on、midi_out=sf2 か mt32 のとき)。主ループに入る前に読む
    LoadMidiSoundFont();
    LoadMidiMt32();
    LoadMidiSc55();
    // 主な確保 (X68000 のメモリ、ディスクのイメージ、MIDI の音源の ROM と SoundFont) を終えたあとのヒープの使った量
    // (それぞれの大きさは、読んだときのログに出ている)
    ReportHeap(FromMachine, "after loading");
    for (;;) {
        ShowTsFrame();
        // 装置をつないだり外したりしたあとは、UpdatePlugAndPlay() が装置を登録するまで 0.1 秒ほど戻らない。
        // メニューを開く途中と開いている間は任せない。コア 0 がメニューを重ねて描き、その下には最後にコア 0 が
        // 受け取ったフレーム (m_pLastFrame) を写すので、止める直前のフレームもコア 0 が受け取るようにする
        if (m_MenuActive)
            UpdateUsbDevices();
        else
            WithComposerPresenting([this] { UpdateUsbDevices(); });
        // Circle のスピンロックを QEMU で待っているコアを起こす (RunM0() を参照)
        asm volatile("sev");
        unsigned mask = __atomic_load_n(&m_Cores.startedMask, __ATOMIC_RELAXED);
        if (mask != lastMask) {
            m_Logger.Write(FromMachine, LogNotice, "secondary cores started: mask=%X", mask);
            lastMask = mask;
        }
        if (!renderReported && m_pEmulator->renderCoreDecided()) {
            // コア 1 が合成するコアを決めたあとに 1 回だけ出す
            if (m_pEmulator->renderCoreActive())
                m_Logger.Write(FromMachine, LogNotice, "screen composition on core 2");
            else if (m_pEmulator->renderCoreRequested())
                m_Logger.Write(FromMachine, LogWarning, "core 2 did not start; screen composition on core 1");
            else
                m_Logger.Write(FromMachine, LogNotice, "screen composition on core 1 (render_core=off)");
            if (m_pEmulator->soundCoreActive())
                m_Logger.Write(FromMachine, LogNotice, "sound synthesis on core 3");
            else if (m_pEmulator->soundCoreRequested() && !m_pEmulator->soundOutputEnabled())
                m_Logger.Write(FromMachine, LogNotice, "no sound output; sound state on core 1 (core 3 not used)");
            else if (m_pEmulator->soundCoreRequested())
                m_Logger.Write(FromMachine, LogWarning, "core 3 did not start; sound synthesis on core 1");
            else
                m_Logger.Write(FromMachine, LogNotice, "sound synthesis on core 1 (sound_core=off)");
            renderReported = true;
        }
        if (m_pUartMidi != nullptr) ServiceMidiUart();
        DrainNullAudio();
        ShowNextFrame();
        ExpireOsd();
        ServiceMessage();
        ServiceAutoType();
        ServiceMenu();
        // ゲームパッドの Start + Select。設定メニューを開いている間と、deploy の再起動を待たせている間 (X68000 を止めている) は捨てる
        if (__atomic_exchange_n(&m_PadResetRequested, false, __ATOMIC_ACQ_REL) && !m_MenuActive && !m_DeployHeld)
            ResetFromGamePad();
        if (m_RebootRequested) {
            m_Logger.Write(FromMachine, LogNotice, "restarting (settings changed in the menu)");
            return ShutdownReboot;
        }
        if (m_RebootRequest.take()) {
            if (RebootFromKeyboard()) {
                m_Logger.Write(FromMachine, LogNotice, "restarting (Ctrl+Alt+Delete)");
                return ShutdownReboot;
            }
        }
        // ネットワークから頼まれた再起動 (POST /reboot)。Ctrl + Alt + Delete と同じく、ディスクと SRAM を書いてから
        if (NetRebootDue()) {
            if (RebootWritingBack("network reboot", m_NetReboot)) {
                m_Logger.Write(FromMachine, LogNotice, "restarting (requested over the network)");
                return ShutdownReboot;
            }
            // 断った: POST /reboot?profile= で once 付きで書いた profile.sel を前回の選択に戻す (ほかの再起動で
            // 頼まれた組み合わせにならないように。60 秒以内に同じ要求を送り直せば、また書く)
            if (m_ProfileChoice.pending()) WhileComposerPresents([this] { CancelProfileChoice(); });
        }
        if (!m_MenuActive) {
            if (m_Keys.pending()) ResendKeys();
            if (InputPending()) ResendInput();
        }
        UpdateKeyboardLeds();
        ReportMouse();

        u64 now = CTimer::GetClockTicks64();
        if (now - lastSpeed >= kSpeedReportInterval * CLOCKHZ) {
            WhileComposerPresents([this] { ReportSpeed(); });
            lastSpeed = now;
        }
        if (m_MaxSpeed && now - lastMaxSpeed >= kMaxSpeedReportInterval * CLOCKHZ) {
            WhileComposerPresents([this] { ReportMaxSpeed(); });
            lastMaxSpeed = now;
        }
        ControlFan();
        if (m_FrameTimingPending) WhileComposerPresents([this] { ReportFrameTiming(); });
        if (m_CoreWorkPending[0] || m_CoreWorkPending[1]) WhileComposerPresents([this] { ReportCoreWork(); });
        if (m_pLastDump != nullptr) {
            if (now - lastDump >= (u64)m_TextDumpSeconds * CLOCKHZ) {
                m_pEmulator->requestTextDump();
                lastDump = now;
            }
            if (m_pEmulator->textDump() != nullptr) WhileComposerPresents([this] { DumpText(); });
        }
        // SD カードを傷めないよう、書き戻しは間を空ける
        if (now - lastSram >= kSramSaveInterval * CLOCKHZ) {
            if (const u8* sram = m_pEmulator->pendingSram()) {
                // 失敗したら写しを持ったまま、間を空けて書き直す
                bool saved = false;
                WhileComposerPresents([&] { saved = SaveSram(sram); });
                if (saved) m_pEmulator->sramSaved();
                lastSram = now;
            }
        }
        if (m_DeploySeconds != 0) {
            if (now - lastAlive >= CLOCKHZ) {
                KeepAlive();
                lastAlive = now;
            }
            // 決めた時間がたったら再起動する。書き戻せなければ再起動しない (書いた内容を捨てないように)。
            // X68000 は止めたまま 1 秒ごとに書き直し、入力は捨てる (止めたコア 1 の入力のキューが一杯にならないように)。
            // メニューは開ける (閉じても X68000 は止めたまま)。抜けるのは、書けたとき (そのまま再起動する) か、
            // メニューの Restart を 2 回選んで書き戻さずに再起動したとき
            if (now - start >= (u64)m_DeploySeconds * CLOCKHZ && !m_MenuActive && now - lastDeployTry >= CLOCKHZ) {
                lastDeployTry = now;
                if (const u8* sram = m_pEmulator->pendingSram()) SaveSram(sram);
                if (PauseAndFlushDisks()) {
                    CloseDiskFiles();
                    CancelProfileChoice(); // 断った POST /reboot?profile= が残っていれば戻す (戻せなくても時間で再起動する)
                    FinishDeploy();
                    return ShutdownReboot;
                }
                if (!m_DeployHeld) {
                    m_Logger.Write(FromMachine, LogWarning,
                                   "deploy: cannot write back disks; restart is held (X68000 paused, input discarded)");
                    m_DeployHeld = true;
                    m_pEmulator->setDiscardInput(true);
                }
            }
        }
        // 大きなイメージのチャンクを読み、ディスクを書き戻す。メニューを開いて X68000 を止めている間は、
        // 書いていない分をすぐに書く (メニューを開いたまま電源を切られても残るように)。
        // 読んだら、続けて頼まれていることが多いので待たずに回る。
        // SD カードを読み書きするのは、写す役を合成するコアに任せたときだけ。ioPending() を見たあとでコア 1 が
        // 読み込みを頼んだり書き戻しの準備を進めたりしても、任せていなければ読み書きせず、次の周回に回す
        // メニューを開いて X68000 を止めている間は、写す役を任せずに読み書きする (メニューの画面は変わらない)。
        // メニューを開くキーを受け取った直後 (m_MenuActive は立ったが、まだ止めていない周回) は、X68000 が動いていて
        // コア 0 が画面を写しているので、読み書きしない (WhileComposerPresents() はメニューの間は任せずに f() を呼ぶ)
        unsigned loads = 0;
        if (m_MenuActive)
            loads = ServiceDisks(now, m_Menu.isOpen() && m_pEmulator->paused());
        else if (m_pDiskWriter != nullptr && m_pDiskWriter->sync.ioPending())
            WhileComposerPresents([&] { loads = ServiceDisks(now); });
        else
            loads = ServiceDisks(now, false);
        if (m_pNetBridge != nullptr) m_pNetBridge->Service();
        // することがなければ 2ms 待つ。イベントストリームが届けば wfe で眠る (USB や SD カードの割り込みでも起きる)。
        // ネットワークの口を動かしていれば、待つ間にそのタスクを動かす (ServiceHostNet())
        ServiceHostNet(loads == 0 ? 2000 : 0);
    }
}

// ---- ネットワークの口 (host_net.h) ----

// pix68k.cfg (読んでいなければ既定の値) と cmdline.txt の net= を見て、ネットワークの口を始める。
// USB の装置を探し終えてから呼ぶ。Pi 4 / Pi 5 は Ethernet のコントローラの初期化に時間がかかるので、
// X68000 を動かしていれば、その間は合成するコアに画面を任せる
void CKernel::StartHostNet() {
    CHostNet* net = new CHostNet(m_Logger, *this);
    if (net == nullptr || !net->Configure(m_Settings, m_Options)) {
        delete net;
        return;
    }
    if (m_pNetBridge != nullptr && m_pNetBridge->UsesDevice()) {
        // Circle の TCP/IP と Neptune-X のブリッジは、どちらもネットワークデバイスの受信を全部取ってしまうので、
        // まだ同じ Ethernet を分け合えない
        m_Logger.Write(FromMachine, LogWarning,
                       "net=on: the Ethernet is used by Neptune-X (neptune=on); the network services are off");
        delete net;
        return;
    }
    bool ok = false;
    if (m_pEmulator != nullptr) WithComposerPresenting([&] { ok = net->Start(); });
    else ok = net->Start();
    if (!ok) {
        // CScheduler などを作ったあとかもしれないので、消さずに持つ (Service() は何もしない)
        return;
    }
    m_pHostNet = net;
}

// 主ループから呼ぶ。waitUs の間 (0 なら 1 回) ネットワークのタスクを動かす。口がなければ待つだけ
// ネットワークのタスク (TCP/IP の送受信、HTTP、syslog) を動かす間は、いつも画面を写す役を合成するコアに任せる。
// USB の Ethernet は、受信も送信も転送が終わるまで待つ (Circle の CLAN7800Device::ReceiveFrame() は 1 回ごとに
// SubmitBlockingRequest() で待つ)。要求がなくても、その間にコア 0 が takeFrame() を呼べず、フレームが置き換わって
// 表示されなくなる (実機の Pi 3B+ で、net=on だと 10 秒に 2 つほど出ていた)
void CKernel::ServiceHostNet(unsigned waitUs) {
    if (m_pHostNet == nullptr) {
        if (waitUs != 0) IdleUntil(CTimer::GetClockTicks64() + (u64)waitUs * (CLOCKHZ / 1000000));
        return;
    }
    netBeginHeavy();
    m_pHostNet->Service(waitUs);
    netEndHeavy();
    // 待つ間はほとんど合成するコアが写すので、コア 0 が写すとき (ShowNextFrame()) だけでは osd=fps の数字が
    // 古いままになりうる。ここでも 1 秒ごとに数え直す (UpdateOsd() は 1 秒たつまで何もしない)
    if (m_pEmulator != nullptr) UpdateOsd();
}

// 頼まれた再起動の時刻が来たら true (1 回だけ。書けずに断ったら、もう一度 POST /reboot を待つ)
bool CKernel::NetRebootDue() {
    if (m_NetRebootAt == 0 || CTimer::GetClockTicks64() < m_NetRebootAt) return false;
    m_NetRebootAt = 0;
    return true;
}

void CKernel::netRequestReboot() {
    // 応答を送り終えるまで 0.3 秒待つ
    if (m_NetRebootAt == 0) m_NetRebootAt = CTimer::GetClockTicks64() + CLOCKHZ * 3 / 10;
}

// paths (起動したドライブの中のパス) のどれかが、X68000 につないだイメージか、その置き換えに使う名前と同じファイルか。
// 両方を、実際のディレクトリエントリの長い名前に直してから、大文字と小文字を区別せずに比べる (platform::canonicalPath())
namespace {
struct CBootDriveNames {
    const char* drive;
    FILINFO info;
    platform::NameLookup longName(const char* path, char* out, size_t size) {
        CString full;
        full.Format("%s%s", drive, path);
        const FRESULT r = f_stat(full, &info);
        // ないなら書いた名前のまま比べてよい。それ以外の失敗 (読めない、パスがおかしいなど) は決められない
        if (r == FR_NO_FILE || r == FR_NO_PATH) return platform::NameLookup::MISSING;
        if (r != FR_OK || strlen(info.fname) + 1 > size) return platform::NameLookup::ERROR;
        platform::copyString(out, size, info.fname);
        return platform::NameLookup::FOUND;
    }
};
} // namespace

platform::ImageCheck CKernel::netImageInUse(const char* const* paths, int count) {
    if (m_BootDrive == nullptr) return platform::ImageCheck::UNKNOWN;
    CBootDriveNames fs{m_BootDrive, {}};
    const size_t dn = strlen(m_BootDrive);
    for (const CString& p : m_InUsePath) {
        if (p.GetLength() == 0) continue;
        // ほかのドライブのイメージなら、起動したドライブのファイルとは重ならない
        const char* colon = strchr(p, ':');
        if (colon != nullptr && ((size_t)(colon + 1 - (const char*)p) != dn || strncmp(p, m_BootDrive, dn) != 0)) continue;
        const platform::ImageCheck c = platform::touchesImage(fs, paths, count, p);
        if (c != platform::ImageCheck::FREE) return c;
    }
    return platform::ImageCheck::FREE;
}

// SD カードの読み書きの前後。X68000 を動かしていて、メニューを開いていなければ、写す役を合成するコアに任せる
// (WhileComposerPresents() と同じ。m_ComposerPresenting も立てるので、その中で WhileComposerPresents() を呼べば
// そのまま呼ぶ)。ネットワークのタスク (TCP/IP、HTTP、syslog) は、ServiceHostNet() の中でこの組に囲んで動かすので、
// SD カードを読み書きするのは、写す役を任せている間か、メニューを開いている間 (コア 0 がメニューを描く) だけになる。
// 入れ子に呼んでよい (ServiceHostNet() の中で HTTP のタスクが呼ぶ)。いちばん外側の組で任せ、戻す。
// すでに任せている (WithComposerPresenting() の USB の転送の中でタスクが動いた) なら、何もしない
void CKernel::netBeginHeavy() {
    if (m_NetHeavyDepth++ != 0) return;
    if (m_pEmulator == nullptr || m_MenuActive || m_ComposerPresenting) return;
    ShowNextFrame();
    m_pEmulator->beginComposerPresent();
    m_ComposerPresenting = true;
    m_NetHeavy = true;
}

void CKernel::netEndHeavy() {
    if (m_NetHeavyDepth == 0 || --m_NetHeavyDepth != 0) return;
    if (!m_NetHeavy) return;
    m_NetHeavy = false;
    m_ComposerPresenting = false;
    m_pEmulator->endComposerPresent(WaitComposerPresent, this);
    m_LastPollTicks = CTimer::GetClockTicks64(); // 任せていた間は、takeFrame() を見に行く間隔に数えない
    LogShownScreen();
}

// コア0専用。表示とスクショは同じ、読み手が所有するスナップショットを使う。
void CKernel::ShowTsFrame() {
    if (!m_TsDisplay) return;
    if (const auto* frame = m_pEmulator->takeTs6bga()) {
        m_TsFrame = frame;
        m_TsChanged = true;
    }
    if (!m_TsFrame || (!m_TsChanged &&
        (m_TsFrame->selected() || m_TsMainShown == m_FramesShownByCore0))) return;
    const Emulator::Frame* main = m_pLastFrame;
    const x68k::Ts6bga::Image pass{main ? main->pixels : nullptr,
        main ? unsigned(main->width) : 0, main ? unsigned(main->height) : 0, Emulator::PITCH};
    m_TsChanged = false;
    m_TsMainShown = m_FramesShownByCore0;
    m_TsFrame->renderRows(m_TsDisplay->GetWidth(), m_TsDisplay->GetHeight(),
        [](void* ctx, const uint32_t* row, unsigned y, unsigned n) {
            auto* k = static_cast<CKernel*>(ctx);
            auto* fb = k->m_TsDisplay;
            auto* dst = (uint32_t*)(uintptr_t)(fb->GetBuffer() + y * fb->GetPitch());
            platform::frame_scale_detail::storeRow<platform::DeviceStore>(dst, row, n);
            // 実出力だけに重ねる。スクショ用の元画像やゲストVRAMは変更しない。
            constexpr unsigned scale = 2, w = 6 * 8 * scale + 8, h = 16 * scale + 8;
            if (n < w + 8 || y < 8 || y >= 8 + h) return;
            const char* label = k->m_TsFrame->selected() ? "TS6BGA" : "MIRROR";
            const unsigned gy = y - 8;
            for (unsigned x = 0; x < w; x++) {
                bool ink = false;
                if (x >= 4 && x < w - 4 && gy >= 4 && gy < h - 4) {
                    const unsigned tx = (x - 4) / scale;
                    ink = (platform::kMenuFont[label[tx / 8] - platform::MENU_FONT_FIRST][(gy - 4) / scale]
                           & (0x80 >> (tx % 8))) != 0;
                }
                platform::DeviceStore::store4(dst + n - w - 8 + x, ink ? 0x00ffffff : 0x00202020);
            }
        }, this, &pass);
}

bool CKernel::netSnapshotTs6bga(u8* rgb, unsigned capacity, int& width, int& height, bool output) {
    if (!m_TsDisplay || !m_pEmulator) return false;
    if (const auto* frame = m_pEmulator->takeTs6bga()) {
        m_TsFrame = frame;
        m_TsChanged = true; // スクショが受け取ったフレームも次の表示で写す
    }
    if (!m_TsFrame) return false;
    if (output && !m_TsFrame->selected()) return netSnapshotScreen(rgb, capacity, width, height);
    // RGBへの変換が終わるまでタスクを譲らない。コア1はこのコピーを上書きしない。
    return m_TsFrame->snapshotRgb(rgb, capacity, width, height);
}

// 今の画面を RGB で写す (GET /screen.ppm)。写す役を合成するコアに任せている間 (HTTP のタスクはいつも
// netBeginHeavy() の中で動く) は、その間に合成したフレームがコア 0 に渡らず、m_pLastFrame は任せる前のまま古い。
// 主ループもほとんどの時間を任せたまま過ごすので (USB の装置の確認とネットワークのタスク)、コア 0 が受け取るのは
// 10 秒に数フレームほどしかなく、起動の途中の黒い 768x512 の画面が残っていることもあった (#57)。
// そこで任せている間は、役を一度戻してフレームを受け取り (受け渡し中に残っていればそれ、なければ次に合成し終えたもの。
// コア 0 が HDMI に写す)、また任せる。フレームを待つのは合わせて長くて kSnapshotWaitMs。合成するコアが写し終えるのを
// 待つ間 (ふつうは 1 フレーム、残っていたフレームを先に写していれば 2 フレームを写す時間) と、受け取ったフレームを
// HDMI に写す時間と、下のディスクの読み書きの時間は、この上限に数えない。時間切れのときは最後に受け取ったフレームを写す。
// コア 1 が大きなイメージのチャンクを待っていると、コア 0 が読むまでフレームが来ない。そこで、ディスクの読み書きが
// 残っている (ioPending()) ときは kSnapshotSliceMs ごとに待つのをやめ、役を任せたまま ServiceDisks() を呼んでから
// 待ち直す (主ループで任せてから読み書きするのと同じ)。
// X68000 が止まっていてメニューを開いていないとき (再起動の前の書き戻しなど) は、最後に合成し終えたフレームを写す
// (合成するコアが自分で写してコア 0 に渡らなかったものでも)。メニューを開いている間は、メニューの下に描くフレーム
// (m_pLastFrame) を写す。写し終えるまでタスクを譲らないので、その間に m_pLastFrame が書き換わることも、主ループが
// X68000 を動かし直すこともない。
// 再開を頼んだ直後、コア 1 がまだ止まったと知らせたまま (paused()) の短い間は、止まったように見えることがある。
// その間にもう一度止めるよう頼むと、前の「止まった」が残っているので latestFrameWhilePaused() が止まったときの
// フレームを返す (コア 1 が動き出す途中なら、そのバッファに合成するコアが描くこともある)。止め直す前に
// 頼まれたときは、フレームを待たずに最後に受け取ったフレームを写す。再開を頼む箇所 (メニューを閉じるとき、再起動をやめたとき) は paused() が false に
// なるのを長くて 1 秒待ってから主ループに戻る (メニューを開き直すときに前の「止まった」を見ないためで、同じ間) ので、
// ふつうはその間に HTTP のタスクは動かない。コア 1 が 1 秒たっても動き出さないときだけ起きる
bool CKernel::netSnapshotScreen(u8* rgb, unsigned capacity, int& width, int& height) {
    if (m_pEmulator == nullptr) return false;
    const Emulator::Frame* f = nullptr;
    if (m_MenuActive) {
        // メニューの下に描くフレーム (m_pLastFrame) をそのまま写す
    } else if (const Emulator::Frame* latest = m_pEmulator->latestFrameWhilePaused()) {
        f = latest;
    } else if (!m_ComposerPresenting || !m_pEmulator->hasPresenter()) {
        // 任せていない。合成するコアに写す関数を渡していない (hasPresenter() が false) ときも、役はコア 0 にあるので
        // takeFrame() で受け取る (WaitSnapshotFrames() では takeFrameWhileHanded() がいつも nullptr を返し、
        // WaitSnapshotFrame() が 1 回も呼ばれないので、待つのをやめられない)
        ShowNextFrame();
    } else {
        WaitSnapshotFrames();
        // 待つ間に止まった (止めるのは主ループだけなので、ふつうは起きない) なら、最後に合成し終えたフレーム
        f = m_pEmulator->latestFrameWhilePaused();
    }
    if (f == nullptr) f = m_pLastFrame;
    if (f == nullptr || f->width <= 0 || f->height <= 0 ||
        (unsigned)f->width * (unsigned)f->height * 3 > capacity)
        return false;
    width = f->width;
    height = f->height;
    for (int y = 0; y < height; y++)
        platform::xrgbToRgb(f->pixels + (size_t)y * Emulator::PITCH, rgb + (size_t)y * width * 3, width);
    return true;
}

// netSnapshotScreen() で、写す役を任せている間に、役を戻してフレームを受け取る (m_pLastFrame が新しくなる)。
// 受け取れなければ、kSnapshotWarnSeconds 秒に 1 回だけ警告する (頼まれるたびに 1 行ずつ増えないように)
void CKernel::WaitSnapshotFrames() {
    // 上限に数えるのはフレームを待つ間だけ (platform/snapshot_wait.h)。ServiceDisks() の時間などは数えない
    platform::SnapshotWait wait((u64)kSnapshotWaitMs * (CLOCKHZ / 1000), (u64)kSnapshotSliceMs * (CLOCKHZ / 1000));
    m_pSnapshotWait = &wait;
    unsigned disks = 0;
    for (;;) {
        m_LastPollTicks = CTimer::GetClockTicks64(); // 任せていた間は、takeFrame() を見に行く間隔に数えない
        wait.beginSlice();
        const Emulator::Frame* f =
            m_pEmulator->takeFrameWhileHanded(WaitComposerPresent, WaitSnapshotFrame, ShowSnapshotFrame, this);
        wait.endSlice(CTimer::GetClockTicks64()); // 受け取って写したときは、写す前に閉じている (ShowSnapshotFrame())
        if (f != nullptr || m_pEmulator->paused()) { // 止まったら、呼び手が最後に合成し終えたフレームを使う
            m_pSnapshotWait = nullptr;
            return;
        }
        if (wait.expired()) break;
        // 区切ったのは、ディスクの読み書きが残っているから (WaitSnapshotFrame())。役を任せたまま読み書きする。
        // ServiceDisks() はタスクを譲らないので、主ループや UsbScanDelay() の ServiceDisks() の途中にこのタスクが動くことは
        // ない。それでも UsbScanDelay() の中なら読み書きしない (入れ子にしない)。読み書きの中の待ち (CTimer::MsDelay()) で
        // UsbScanDelay() が ServiceDisks() を入れ子に呼ばないよう、m_InScanDelay を立てる
        if (m_InScanDelay || m_pDiskWriter == nullptr) continue;
        m_InScanDelay = true;
        ServiceDisks(CTimer::GetClockTicks64());
        m_InScanDelay = false;
        disks++;
    }
    m_pSnapshotWait = nullptr;
    const u64 now = CTimer::GetClockTicks64();
    if (m_SnapshotWarnTicks != 0 && now - m_SnapshotWarnTicks < (u64)kSnapshotWarnSeconds * CLOCKHZ) {
        m_SnapshotWarnSuppressed++;
        return;
    }
    if (m_SnapshotWarnSuppressed != 0)
        m_Logger.Write(FromMachine, LogWarning,
                       "screen: no new frame in %u ms (disks serviced %u times); sending the last one "
                       "(%u more since the last warning)",
                       kSnapshotWaitMs, disks, m_SnapshotWarnSuppressed);
    else
        m_Logger.Write(FromMachine, LogWarning,
                       "screen: no new frame in %u ms (disks serviced %u times); sending the last one",
                       kSnapshotWaitMs, disks);
    m_SnapshotWarnTicks = now;
    m_SnapshotWarnSuppressed = 0;
}

// netSnapshotScreen() で、役を戻している間に受け取ったフレームを写す
// (写す前に、フレームを待った区間を閉じる。写す時間は待った時間に数えない)
void CKernel::ShowSnapshotFrame(void* ctx, const Emulator::Frame* frame) {
    CKernel* k = (CKernel*)ctx;
    k->m_pSnapshotWait->endSlice(CTimer::GetClockTicks64());
    k->ShowTakenFrame(frame);
}

// netSnapshotScreen() で、次のフレームを待つ間に呼ぶ。待つのをやめるなら false: X68000 が止まった、フレームを待った
// 時間の合計が上限に達した、または kSnapshotSliceMs たってディスクの読み書きが残っている (コア 1 がチャンクを
// 待っているかもしれないので、WaitSnapshotFrames() で読み書きする)。合成するコアが写し終えるのを待つ間は
// WaitComposerPresent() を呼び、上限に数えない
bool CKernel::WaitSnapshotFrame(void* ctx) {
    CKernel* k = (CKernel*)ctx;
    WaitComposerPresent(ctx);
    if (k->m_pEmulator->paused()) return false;
    const bool io = k->m_pDiskWriter != nullptr && k->m_pDiskWriter->sync.ioPending();
    return k->m_pSnapshotWait->keepWaiting(CTimer::GetClockTicks64(), io);
}

// ネットワークからのキー入力 (POST /type、/key)。USB キーボードと同じく KeyMerger の枠の 1 つ (仮想キーボード) に入れ、
// すべてのキーボードと合わせてから送る (Ctrl + Alt + Delete の見分けも同じ)。キーボードの種類に合わせた書き換えはしない。
// キューの書き手を 1 つにするため、USB の割り込みを止める
bool CKernel::netKeyReport(u8 modifiers, const u8 keys[6]) {
    if (m_pEmulator == nullptr) return false;
    EnterCritical();
    const bool ok = m_KeyMerge.set(VirtualKeyboard, modifiers, keys);
    if (ok) DispatchKeys();
    LeaveCritical();
    return ok;
}

// コア 1 / 2 が合成した画面があれば写す
void CKernel::ShowNextFrame() {
    // 主ループが takeFrame() を見に行く間隔の最大 (ログ用)。メニューを開いている間は X68000 が止まっているので数えない
    const u64 now = CTimer::GetClockTicks64();
    if (m_LastPollTicks != 0 && !m_MenuActive && now - m_LastPollTicks > m_PollGapMax) m_PollGapMax = now - m_LastPollTicks;
    m_LastPollTicks = now;
    const Emulator::Frame* frame = m_pEmulator->takeFrame();
    if (frame != nullptr) ShowTakenFrame(frame);
}

// コア 0 が受け取ったフレームを写す
void CKernel::ShowTakenFrame(const Emulator::Frame* frame) {
    ShowFrame(*frame);
    m_FramesShownByCore0++;
    UpdateOsd();
    m_pLastFrame = frame; // 次に takeFrame() を呼ぶまで、コア 1 / 2 はこのバッファに描かない
    ShowTsFrame(); // 通常画面はフィルター前の新しいフレームから第2 HDMIへ写す
    if (__atomic_load_n(&m_BootTicks[BootFrame], __ATOMIC_RELAXED) == 0)
        __atomic_store_n(&m_BootTicks[BootFrame], CTimer::GetClockTicks64(), __ATOMIC_RELAXED);
    LogShownScreen();
    if (m_Menu.isOpen()) DrawMenu(); // 止める前に合成していたフレーム
}

// 写す役を合成するコアから戻すのを待つ間 (ふつうは 1 フレーム、長くて 2 フレームを写す時間) に呼ぶ
void CKernel::WaitComposerPresent(void* ctx) {
    CKernel* k = (CKernel*)ctx;
    asm volatile("sev");
    k->DrainNullAudio();
}

// 最初の画面を出した時刻をログに出す (1 回だけ。合成するコアが写したときは、その時刻を取ってある)
// 画面の大きさが変わっていれば、それもログに出す。コア 0 が写す役を持っているときに呼ぶ
void CKernel::LogShownScreen() {
    if (m_ScreenModeChanged) {
        m_ScreenModeChanged = false;
        // kx と ky が 0 なら、整数倍でない枠 (video_scale=aspect、full。platform::videoFrameScale())
        // 枠を写しているときは、枠の名前 (ガラスの矩形に写すので、video_scale= は使っていない)
        const int bezelShown = __atomic_load_n(&m_BezelShown, __ATOMIC_RELAXED);
        if (m_ShownScale.kx == 0 && bezelShown != 0)
            m_Logger.Write(FromMachine, LogNotice, "screen mode %dx%d, shown %dx%d (bezel %s)", m_ShownWidth,
                           m_ShownHeight, m_ShownScale.dstW, m_ShownScale.dstH,
                           platform::videoBezelName(bezelShown == 1 ? platform::CRT_BEZEL_DE : platform::CRT_BEZEL_DB));
        else if (m_ShownScale.kx == 0)
            m_Logger.Write(FromMachine, LogNotice, "screen mode %dx%d, shown %dx%d (video_scale=%s)", m_ShownWidth,
                           m_ShownHeight, m_ShownScale.dstW, m_ShownScale.dstH,
                           platform::videoScaleName(__atomic_load_n(&m_Video, __ATOMIC_RELAXED) >> 16));
        else
            m_Logger.Write(FromMachine, LogNotice, "screen mode %dx%d, shown %dx%d (x%d, y%d)", m_ShownWidth,
                           m_ShownHeight, m_ShownScale.dstW, m_ShownScale.dstH, m_ShownScale.kx, m_ShownScale.ky);
    }
    // 絵は読めたのに枠を出さないときは、理由が変わったときに出す (画面モードの変更とは別に見る。設定メニューで枠を
    // 選んだときなど、写す範囲が変わらなくても理由は変わる)。画面モードを行き来するたびには出さない
    {
        const int skip = __atomic_load_n(&m_BezelSkip, __ATOMIC_ACQUIRE);
        if (m_BezelSkipLog.update(skip)) {
            const platform::BezelImage& b = m_Bezel[(skip >> 4 == 2 ? 2 : 1) - 1];
            const char* name = platform::videoBezelName(skip >> 4 == 2 ? platform::CRT_BEZEL_DB : platform::CRT_BEZEL_DE);
            if ((skip & 15) == 1)
                m_Logger.Write(FromMachine, LogWarning, "bezel: %s is not shown with pix68k_gpu_scale; showing without the bezel",
                               name);
            else if ((skip & 15) == 2)
                m_Logger.Write(FromMachine, LogWarning,
                               "bezel: %s is not shown: screen mode %dx%d does not fit in the glass (%dx%d); showing without the bezel",
                               name, m_ShownWidth, m_ShownHeight, b.h.gw, b.h.gh);
        }
    }
    if (m_FrameLogged || __atomic_load_n(&m_BootTicks[BootFrame], __ATOMIC_ACQUIRE) == 0) return;
    m_FrameLogged = true;
    LogBootTime(BootFrame, "first frame shown");
}

// コア 0 が USB の転送で止まっている間に、合成するコア (コア 2、コア 2 を使わなければコア 1) が
// Emulator::publishFrame() から呼ぶ。コア 0 はこの間フレームバッファに書かない (platform/frame_handoff.h を
// 参照)
void CKernel::PresentFromComposer(void* ctx, const Emulator::Frame& frame) {
    CKernel* k = (CKernel*)ctx;
    k->ShowFrame(frame);
    if (__atomic_load_n(&k->m_BootTicks[BootFrame], __ATOMIC_RELAXED) == 0)
        __atomic_store_n(&k->m_BootTicks[BootFrame], CTimer::GetClockTicks64(), __ATOMIC_RELEASE);
}

// 大きなイメージのチャンクを読み、ディスクを書き戻す。読んだチャンクの数を返す
// io が false なら SD カードを読み書きせず、書き戻すかどうかを決めるだけにする (写す役を任せていないとき。
// メニューを開いて X68000 を止めている間は、io が false なら何もしない)
unsigned CKernel::ServiceDisks(u64 now, bool io) {
    if (m_pDiskWriter == nullptr) return 0;
    return platform::serviceDiskSync(m_pDiskWriter->sync, now / (CLOCKHZ / 1000), io, m_MenuActive,
                                     m_Menu.isOpen() && m_pEmulator->paused());
}

// USB の転送 (WithComposerPresenting() の中) の待ち時間 (CTimer::MsDelay()) に、コア 0 の主ループの代わりに呼ぶ。
// USB を使うもの (キーボードの LED、メニュー、ネットワーク) と、フレームバッファには触らない。
// insideUsbTransfer は、USB の転送の途中から呼ばれたとき (UsbScanDelay())。boot ドライブが USB メモリなら、このときは
// イメージを読み書きしない (ほかの装置の転送の途中に USB メモリの転送を始めない。USB メモリ自身の転送の途中なら、
// FatFs のボリュームのロックを二重に取って止まる)。読み書きは、転送から戻ったあとの主ループで行う。
// ROM や SoundFont を読む合間 (CMidiFileSource など) に呼ばれたときは、USB の転送の途中ではないので読み書きする
void CKernel::ServiceDuringUsbScan(bool insideUsbTransfer) {
    // Circle のスピンロックを QEMU で待っているコアを起こす (RunM0() を参照)
    asm volatile("sev");
    DrainNullAudio();
    // 画面は合成するコアが写している (PresentFromComposer())。コア 0 はフレームバッファに書かない
    const u64 now = CTimer::GetClockTicks64();
    ServiceDisks(now, platform::diskIoAllowed(m_BootOnUsb, insideUsbTransfer));
    // midi_out=uart: 主ループに入る前 (USB の装置を探す間、ROM を読む間) も、送るコアを決めて送る
    if (m_pUartMidi != nullptr) ServiceMidiUart();
    if (m_DeploySeconds != 0 && now - m_ScanLastAlive >= CLOCKHZ) {
        KeepAlive();
        m_ScanLastAlive = now;
    }
}

// 画面の出し方 (video_scale=、video_filter=) を、この機種で選べるか。
// Pi 3 (Zero 2 W、Pi 3A+ も同じ Cortex-A53) は、写すのは合成するコア (コア 2) で、1 フレーム (約 18ms) のうちに合成と
// 写しの両方を終える必要がある。実機で測って (docs/baremetal.md の「画面の出し方」)、収まる組み合わせだけを選べる。
// カーネルのオプション pix68k_crt_all=1 (試験用) ならどれも選べる (測るため)。表のメモリが取れなければ、表を使わない
// 組み合わせ (1 倍か整数倍で、効果はスキャンラインだけ) にする
bool CKernel::VideoAllowed(int scale, int filter) const {
    // 枠 (cz-600de、cz-600db) はガラスの矩形に写すので、表を使う
    const bool needsMap = platform::crtNeedsMap(filter) || scale == platform::VIDEO_SCALE_ASPECT ||
                          scale == platform::VIDEO_SCALE_FULL || (filter & platform::CRT_BEZEL_BITS) != 0;
    if (needsMap && m_CrtMap.sx == nullptr) return false;
#if RASPPI <= 3
    if (m_VideoAll) return true;
    return platform::videoAllowedPi3(scale, filter);
#else
    return true;
#endif
}

// 画面の出し方の用意。表のメモリを取り、pix68k.cfg の組み合わせがこの機種で選べなければ選べるものに替える
// (ShowFrame() はログに書けず、合成するコアからも呼ばれるので、ここで済ませる)
void CKernel::StartCrt() {
#if RASPPI <= 3
    m_VideoAll = m_Options.GetAppOptionDecimal("pix68k_crt_all", 0) != 0;
#endif
    int maxW = (int)m_Screen.GetWidth(), maxH = (int)m_Screen.GetHeight();
    if (maxW > ScaleLinePixels - 4) maxW = ScaleLinePixels - 4;
    u32* mem = maxW > 0 && maxH > 0 ? new u32[platform::crtMapBytes(maxW, maxH) / 4 + 1] : nullptr;
    if (mem != nullptr)
        platform::crtMapInit(m_CrtMap, mem, maxW, maxH);
    else
        m_Logger.Write(FromMachine, LogWarning, "screen: out of memory; video_scale=aspect and full, and video_filter=curve and mask are not available");
    int scale = m_Settings.videoScale, filter = m_Settings.videoFilter;
    if (!VideoAllowed(scale, filter)) {
        // curve、mask の順に外し、それでも選べなければ整数倍にする (整数倍のスキャンラインは表を使わないので、いつも選べる)
        const int was = scale, wasFilter = filter;
        filter &= ~platform::CRT_CURVE;
        if (!VideoAllowed(scale, filter)) filter &= ~platform::CRT_MASK;
        // 表のメモリがないと、枠 (表で写す) も使えない
        if (!VideoAllowed(scale, filter)) filter &= ~platform::CRT_BEZEL_BITS;
        if (!VideoAllowed(scale, filter)) scale = platform::VIDEO_SCALE_INTEGER;
        m_Logger.Write(FromMachine, LogWarning,
                       "video_scale=%s video_filter=%s is not available on this Pi; using video_scale=%s video_filter=%s",
                       platform::videoScaleName(was), platform::videoFilterName(wasFilter), platform::videoScaleName(scale),
                       platform::videoFilterName(filter));
        // 動いている設定も合わせる (設定メニューは、保存してある値を出す)
        m_Settings.videoScale = scale;
        m_Settings.videoFilter = filter;
    }
    m_Logger.Write(FromMachine, LogNotice, "screen: video_scale=%s video_filter=%s", platform::videoScaleName(scale),
                   platform::videoFilterName(filter));
    // 枠の絵を先に読む (読めなければログに理由を出し、枠なしで写す。設定は変えない)
    if ((filter & platform::CRT_BEZEL_BITS) != 0) LoadBezel(filter);
    __atomic_store_n(&m_Video, scale << 16 | filter, __ATOMIC_RELAXED);
}

// モニタの枠の絵 (video_filter= の cz-600de / cz-600db) を起動したドライブの /pix68k/ から読む (platform/bezel.h)。読めたもの、
// 読めなかったものは覚えておき、2 回目からは読まない。読めなければ (ない、形が違う、フレームバッファと大きさが違う、
// メモリがない) ログに理由を出して nullptr を返す (枠なしで写す)。コア 0 だけが呼ぶ
const platform::BezelImage* CKernel::LoadBezel(int filter) {
    const int id = platform::videoBezel(filter);
    if (id == 0) return nullptr;
    platform::BezelImage& b = m_Bezel[id - 1];
    if (b.pixels != nullptr) return &b;
    if (m_BezelTried[id - 1]) return nullptr;
    m_BezelTried[id - 1] = true;
    const char* name = platform::bezelFileName(filter);
    CString path; // 起動したドライブ (SD カード、NVMe、USB) の /pix68k/ (設定や ROM と同じ)
    path.Format("%s/pix68k/%s", m_BootDrive != nullptr ? m_BootDrive : "", name);
    FIL f;
    if (f_open(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        m_Logger.Write(FromMachine, LogWarning, "bezel: %s not found; showing without the bezel", (const char*)path);
        return nullptr;
    }
    u8 hdr[platform::kBezelHeaderBytes];
    UINT got = 0;
    platform::BezelHeader h;
    const char* err = nullptr;
    if (f_read(&f, hdr, sizeof hdr, &got) != FR_OK)
        err = "cannot read";
    else // 16 バイトより短ければ、parseBezelHeader() が「not a bezel file」にする
        err = platform::parseBezelHeader(hdr, got < sizeof hdr ? got : f_size(&f), h);
    const int sw = (int)m_Screen.GetWidth(), sh = (int)m_Screen.GetHeight();
    if (err == nullptr && (h.w != sw || h.h != sh)) {
        f_close(&f);
        m_Logger.Write(FromMachine, LogWarning, "bezel: %s is %dx%d but the screen is %dx%d; showing without the bezel",
                       (const char*)path, h.w, h.h, sw, sh);
        return nullptr;
    }
    u32* px = nullptr;
    u16* edges = nullptr;
    if (err == nullptr) {
        px = new u32[(size_t)h.w * h.h];
        edges = new u16[(size_t)h.gh * 2];
        if (px == nullptr || edges == nullptr) err = "out of memory";
    }
    // 最後のバッファに直接読む (ファイル全体を別に読んで写すと、一時に 2 倍のメモリを使う)
    if (err == nullptr && (f_read(&f, px, (UINT)((size_t)h.w * h.h * 4), &got) != FR_OK || got != (UINT)((size_t)h.w * h.h * 4)))
        err = "cannot read";
    f_close(&f);
    if (err == nullptr && !platform::bezelEdges(px, h, edges, edges + h.gh)) err = "the frame has pixels inside the glass";
    if (err != nullptr) {
        delete[] px;
        delete[] edges;
        m_Logger.Write(FromMachine, LogWarning, "bezel: %s: %s; showing without the bezel", (const char*)path, err);
        return nullptr;
    }
    b.left = edges;
    b.right = edges + h.gh;
    b.h = h;
    b.filter = filter & platform::CRT_BEZEL_BITS;
    __atomic_store_n(&b.pixels, (const uint32_t*)px, __ATOMIC_RELEASE);
    m_Logger.Write(FromMachine, LogNotice, "bezel: %s (glass %dx%d at %d,%d)", (const char*)path, h.gw, h.gh, h.gx, h.gy);
    return &b;
}

// X68000 の画面をフレームバッファの真ん中に写す。video_scale=integer (aspect、full で表ができるまでも) なら整数倍で
// 拡大し、off なら拡大しない (platform/frame_scale.h。フレームバッファより大きければ真ん中を切り出す)。aspect、full は
// 表を引いて写す (platform/crt_effect.h)。ふだんはコア 0 が呼び、
// コア 0 が USB の転送で止まっている間は合成するコアが呼ぶ (PresentFromComposer()。同時に 2 つのコアが呼ぶことはない)。
// 合成するコアからも呼ぶので、ログには書かない
// 32 ビットのフレームバッファの画素は X68000 の Renderer と同じ XRGB の並び
void CKernel::ShowFrame(const Emulator::Frame& frame, bool stopped) {
    CBcmFrameBuffer* fb = m_Screen.GetFrameBuffer();
    if (fb == nullptr || fb->GetDepth() != 32 || frame.width <= 0 || frame.height <= 0) return;
    u8* base = (u8*)(uintptr)fb->GetBuffer();
    const unsigned pitch = fb->GetPitch();
    const int sw = (int)m_Screen.GetWidth(), sh = (int)m_Screen.GetHeight();
    // 画面の出し方 (video_scale= は上の 16 ビット、video_filter= は下の 16 ビット。CKernel::videoChanged())
    const int video = __atomic_load_n(&m_Video, __ATOMIC_RELAXED);
    const int mode = video >> 16;
    const bool scaleOn = mode != platform::VIDEO_SCALE_OFF;
    const platform::FrameScale intScale = platform::computeFrameScale(
        frame.width, frame.height, sw, sh, scaleOn, ScaleLinePixels - 4, m_GpuScaleX, m_GpuScaleY);
    // 整数倍で写すと、大きさか倍率が変わる (すべての行を写すフレーム)
    const bool intChanged = frame.width != m_ShownWidth || frame.height != m_ShownHeight || intScale != m_ShownScale;
    platform::FrameScale scale = intScale; // 写す範囲 (表ができていれば、下で整数倍でない枠にする)
    // ブラウン管のように見せる効果 (video_filter=)。整数倍 (か 1 倍) でスキャンラインだけなら、presentScaled() が
    // 縦の倍率が 2 以上のときに暗くする (platform/frame_scale.h)。curve か mask があるときと、整数倍でない枠
    // (video_scale=aspect、full。platform::videoFrameScale()) は、表を作って presentCrt() で写す (platform/crt_effect.h)。
    // 表は、組み合わせか画面の大きさが変わったときに作り直す。ゆがみのある表は時間がかかる
    // (Pi 3 で 39ms) ので、1 回で作ると、その間に合成するコアが次のフレームを渡し、写していないフレームが置き換わる
    // (コア 0 が写しているとき) か、合成するコアが遅れる (合成するコアが写しているとき)。そこで、フレームを写すたびに
    // kCrtBuildSliceUs だけ作り進め、できあがるまでは整数倍の枠に、スキャンラインだけ (なければ効果なし) で写す
    // (どのフレームも写す)。大きさが変わったフレームは、すべての行を写すので作り進めない。X68000 が止まっていて
    // 次のフレームが来ないとき (stopped) は、置き換わるフレームがないので最後まで作る。作れなければ整数倍の枠のままにする。
    // 整数倍でない枠は、表ができたフレームで替える (枠の外を塗り、すべての行を写す)
    int crt = video & (platform::CRT_PRESET_COUNT - 1);
    platform::FrameScale target = platform::videoFrameScale(mode, intScale, frame.width, frame.height, sw, sh,
                                                            ScaleLinePixels - 4, m_GpuScaleX, m_GpuScaleY);
    // モニタの枠 (platform/bezel.h)。絵が読めていて、フレームバッファと同じ大きさで、画面がガラスに入るなら、
    // video_scale= の枠の代わりにガラスの矩形に写す。読めていなければ枠なし (読むのはコア 0。LoadBezel())
    const int bezelId = platform::videoBezel(video & 0xffff);
    const platform::BezelImage* bezel = nullptr;
    const bool bezelLoaded = bezelId != 0 && __atomic_load_n(&m_Bezel[bezelId - 1].pixels, __ATOMIC_ACQUIRE) != nullptr;
    if (bezelLoaded && platform::bezelFrameScale(m_Bezel[bezelId - 1], frame.width, frame.height, sw, sh, m_GpuScaleX,
                                                 m_GpuScaleY, target))
        bezel = &m_Bezel[bezelId - 1];
    // 絵は読めたのに枠を出さない理由 (ログ用。コア 0 が LogShownScreen() で、変わったときに出す)。写す範囲が変わらない
    // 設定の変更 (768x1024 の画面で枠を選んだ、外した、替えた) でも変わるので、毎フレーム置く
    // (置くのは、下で m_ShownWidth などを書いたあと。ログに画面モードを出すため)
    const int bezelSkip =
        platform::bezelSkipCode(video & 0xffff, bezelLoaded, bezel != nullptr, m_GpuScaleX != 1 || m_GpuScaleY != 1);
    bool useMap = platform::crtNeedsMap(crt) || target.kx == 0;
    if (useMap) {
        const platform::FrameScale& cs = target;
        const u64 b0 = CTimer::GetClockTicks64();
        // 作れなかった表 (大きすぎる、区間が多すぎる) は作り直さず、ログの数にも入れない (platform::crtMapPrepare())
        const platform::CrtPrepared prep = platform::crtMapPrepare(
            m_CrtMap, crt, cs.srcW, cs.srcH, cs.dstW, cs.dstH, stopped,
            intChanged ? 0 : (u64)kCrtBuildSliceUs * (CLOCKHZ / 1000000), [] { return (uint64_t)CTimer::GetClockTicks64(); });
        if (prep.worked) {
            // ログ用 (コア 0 が読む)。作り進めた回数、合わせた時間、1 回の最大、作り終えた表の数
            const uint32_t us = (uint32_t)((CTimer::GetClockTicks64() - b0) * 1000000 / CLOCKHZ);
            __atomic_store_n(&m_CrtBuildTotalUs, m_CrtBuildTotalUs + us, __ATOMIC_RELAXED);
            __atomic_store_n(&m_CrtBuildSlices, m_CrtBuildSlices + 1, __ATOMIC_RELAXED);
            if (us > __atomic_load_n(&m_CrtBuildUs, __ATOMIC_RELAXED)) __atomic_store_n(&m_CrtBuildUs, us, __ATOMIC_RELAXED);
            if (prep.finished) __atomic_store_n(&m_CrtBuildsDone, m_CrtBuildsDone + 1, __ATOMIC_RELAXED);
        }
        if (prep.ready) {
            scale = cs;
            // 作り直した表で初めて写すときは、すべての行を写す
            if (m_CrtMap.serial != m_CrtSerialShown) {
                m_CrtSerialShown = m_CrtMap.serial;
                m_RowCache.valid = false;
            }
        } else {
            // 表ができるまで (と、作れなかったとき) は、整数倍の枠にスキャンラインだけで写す。表は組み合わせごとに作り直す
            // ので、切り替えるとできるまで整数倍の枠を通る (枠の外を塗り直す)。切り替えるのは設定メニューだけ
            // (CKernel::videoChanged()) で、そのときは X68000 を止めている (stopped) ので 1 回で作り、ふつうは
            // 通らない (止まりきる前に写したフレームでは、数フレーム通ることがある)
            crt &= platform::CRT_SCANLINE;
            useMap = false;
        }
    }
    // 大きさか倍率が変わった (下で枠の外を塗り、すべての行を写す)
    // 枠を写すか (表ができてガラスの矩形に写すときだけ。枠を替えたときも枠の外を描き直す)
    const int bezelShown = bezel != nullptr && platform::isBezelScale(*bezel, scale) ? bezelId : 0;
    const bool modeChanged = frame.width != m_ShownWidth || frame.height != m_ShownHeight || scale != m_ShownScale ||
                             bezelShown != m_BezelShown;
    const bool scanline = !useMap && crt == platform::CRT_SCANLINE;
    // 効果か写し方を切り替えたときは、すべての行を写し直す
    const int shownKey = (useMap ? kCrtShownMap : 0) | crt;
    if (shownKey != m_CrtLast) {
        m_CrtLast = shownKey;
        m_RowCache.valid = false;
    }
    // ログ用 (コア 0 が読む)。表で写したか、掛けた効果 (スキャンラインだけのときは、暗くする行を入れたときだけ)
    __atomic_store_n(&m_CrtShown, useMap ? shownKey : scanline && scale.ky >= 2 ? (int)platform::CRT_SCANLINE : 0,
                     __ATOMIC_RELAXED);
    if (modeChanged) {
        // 大きさか倍率が変わったら (最初の 1 回はログの文字を消すため)、写す範囲の外を不透明の黒にする
        // (アルファが 0 の黒は、HDMI で濃い灰色に見えることがあった)
        // 枠を写すときは、枠の外を枠の絵にする
        if (bezelShown != 0)
            platform::drawBezelOutside(base, pitch, *bezel);
        else
            platform::fillOutside(base, pitch, sw, sh, scale, 0xff000000u);
        __atomic_store_n(&m_BezelShown, bezelShown, __ATOMIC_RELAXED);
        m_OsdState.invalidate();
        m_MsgState.invalidate();
        m_RowCache.valid = false; // 写しを作り直す (すべての行を写す)
        m_ScreenModeChanged = true; // ログはコア 0 が出す (LogShownScreen())。合成するコアからはログに書かない
        m_ShownWidth = frame.width;
        m_ShownHeight = frame.height;
        m_ShownScale = scale;
    }
    __atomic_store_n(&m_BezelSkip, bezelSkip, __ATOMIC_RELEASE);
    const u64 t0 = CTimer::GetClockTicks64();
    // 画面の隅の数字 (osd=on / fps)。数字か場所が変わったときだけ描く。X68000 の画面の枠の外に出すときは写したあとに描き、
    // 重ねるときは写すときに重なる部分だけ数字の絵を書く (X68000 の画面の中身は変えない。写したあとに上から描くと、
    // 写してから描くまでの間に重ねる場所に X68000 の画面が出てちらつく)。メニューを開いている間は出さない
    const int osdMode = __atomic_load_n(&m_Settings.osd, __ATOMIC_RELAXED);
    const bool osdOn = osdMode != 0 && !m_MenuActive;
    const platform::OsdValue osdValue = LoadOsd();
    // 置く場所は、X68000 の画面の右の余白 (縦に並べる) か、なければ上の余白の横 1 行。どちらも X68000 の画面に重なるときは、
    // 縦に並べたものを右上に重ねる (platform::chooseOsd())。枠は写す範囲 (scale): video_scale=aspect、full は表ができたら
    // platform::videoFrameScale() の枠、できるまでは整数倍の枠 (枠が替わるフレームで置き直す)
    const platform::OsdRect osdRect = platform::chooseOsd(osdMode, sw, sh, scale.dstX, scale.dstY, scale.dstW, scale.dstH,
                                                          platform::osdChars(osdValue), platform::osdRows(osdValue));
    // 置く場所がなければ、UpdateOsd() は温度を読まない (コア 0 が読む)
    __atomic_store_n(&m_OsdPlaced, osdRect.w > 0, __ATOMIC_RELAXED);
    const platform::OsdState::Step osd = m_OsdState.update(osdOn, osdValue, osdRect);
    // 画面の左上のメッセージ (POST /message)。コア 0 が新しい字形を置いていたら写し、消す時刻を過ぎたら出さない。
    // 右上の数字と同じ行に並ぶときは、数字にかからないよう右を切る。描き方は右上の数字と同じ
    // まだ 1 度も POST /message が来ていなければ (s_pMsg が nullptr)、何も出さない
    MessageMem* const mm = __atomic_load_n(&s_pMsg, __ATOMIC_ACQUIRE);
    if (mm != nullptr) {
        const unsigned msgSeq = mm->box.version();
        if (msgSeq != m_MsgSeqLoaded && !(msgSeq & 1)) m_MsgSeqLoaded = mm->box.load(mm->shown, m_MsgDeadline);
    }
    const bool msgOn = mm != nullptr && !m_MenuActive && platform::messageVisible(mm->shown, m_MsgDeadline, t0);
    platform::OsdRect msgRect;
    if (msgOn) {
        const int ms = platform::messageScale(sh);
        const int limitX =
            platform::messageLimitX(sw, sh, m_OsdState.shown ? m_OsdState.rect : platform::OsdRect(), mm->shown, ms);
        msgRect = platform::placeMessage(sw, sh, scale.dstX, scale.dstY, scale.dstW, scale.dstH, mm->shown, ms, limitX);
    }
    const platform::OsdState::Step msg = m_MsgState.update(msgOn, m_MsgSeqLoaded, msgRect);
    // 消すのを先にすべて済ませてから描く (消す場所が、もう一方の新しい場所に重なることがあるため)
    // 枠を写しているときは、消した場所を枠の絵に戻す
    const platform::BezelImage* const shownBezel = m_BezelShown != 0 ? &m_Bezel[m_BezelShown - 1] : nullptr;
    auto erase = [&](const platform::OsdRect& r) {
        if (shownBezel != nullptr)
            platform::restoreBezel(base, pitch, *shownBezel, r.x, r.y, r.w, r.h);
        else
            platform::eraseOsd(base, pitch, r, platform::OSD_BG);
    };
    if (osd.erase) {
        erase(osd.eraseRect);
        // 消した場所に重なる X68000 の画面の行は、変わっていなくても写して元に戻す
        ForceRows(osd.eraseRect.y, osd.eraseRect.y + osd.eraseRect.h);
    }
    if (msg.erase) {
        erase(msg.eraseRect);
        ForceRows(msg.eraseRect.y, msg.eraseRect.y + msg.eraseRect.h);
    }
    // 重ねる数字やメッセージを描き直したら、重なる行を写す
    if (osd.draw && osdRect.overlay) ForceRows(osdRect.y, osdRect.y + osdRect.h);
    if (msg.draw && msgRect.overlay) ForceRows(msgRect.y, msgRect.y + msgRect.h);
    if (osd.draw) platform::renderOsd(s_OsdImage, osdRect, osdValue);
    if (msg.draw) platform::renderMessage(mm->image, msgRect, mm->shown);
    platform::FrameOverlay ov, ovMsg;
    if (m_OsdState.overlaid()) {
        ov.pixels = s_OsdImage;
        ov.x = osdRect.x;
        ov.y = osdRect.y;
        ov.w = osdRect.w;
        ov.h = osdRect.h;
    }
    if (m_MsgState.overlaid()) {
        ovMsg.pixels = mm->image;
        ovMsg.x = msgRect.x;
        ovMsg.y = msgRect.y;
        ovMsg.w = msgRect.w;
        ovMsg.h = msgRect.h;
    }
    platform::CrtBezel cb;
    if (shownBezel != nullptr) {
        cb.pixels = shownBezel->pixels + (size_t)scale.dstY * shownBezel->h.w + scale.dstX;
        cb.pitch = shownBezel->h.w;
        cb.left = shownBezel->left;
        cb.right = shownBezel->right;
    }
    if (useMap)
        platform::presentCrt(frame.pixels, Emulator::PITCH, base, pitch, scale, m_CrtMap, m_ScaleLine, m_ScaleLine2,
                             m_OsdState.overlaid() ? &ov : nullptr, m_RowCache.pixels != nullptr ? &m_RowCache : nullptr,
                             m_MsgState.overlaid() ? &ovMsg : nullptr, shownBezel != nullptr ? &cb : nullptr);
    else
        platform::presentScaled(frame.pixels, Emulator::PITCH, base, pitch, scale, m_ScaleLine,
                                m_OsdState.overlaid() ? &ov : nullptr,
                                m_RowCache.pixels != nullptr ? &m_RowCache : nullptr, scanline,
                                m_MsgState.overlaid() ? &ovMsg : nullptr);
    // 写した行と省いた行の数 (ログ用。コア 0 が読む)
    __atomic_store_n(&m_RowsCopied, m_RowCache.copied, __ATOMIC_RELAXED);
    __atomic_store_n(&m_RowsSkipped, m_RowCache.skipped, __ATOMIC_RELAXED);
    // 枠の外の部分 (重ねるときに枠からはみ出した部分を含む) は、変わったときに描く
    // 枠を写しているときは、背景を枠の絵の半分の明るさにする (黒の半透明に見える。m_ScaleLine は写し終えたので使ってよい)
    auto drawImage = [&](const platform::OsdRect& r, const uint32_t* image) {
        if (shownBezel != nullptr)
            platform::drawOsdOnBezel(base, pitch, *shownBezel, r.x, r.y, r.w, r.h, image, platform::OSD_BG, m_ScaleLine);
        else
            platform::drawOsdImage(base, pitch, r, image);
    };
    if (osd.draw) drawImage(osdRect, s_OsdImage);
    if (msg.draw) drawImage(msgRect, mm->image);
    const u64 us = (CTimer::GetClockTicks64() - t0) * 1000000 / CLOCKHZ;
    // コア 0 が読む (ログ)。書くのは写す役を持っているコアだけ
    __atomic_store_n(&m_CopyTotalUs, m_CopyTotalUs + us, __ATOMIC_RELAXED);
    if (us > __atomic_load_n(&m_CopyMaxUs, __ATOMIC_RELAXED)) __atomic_store_n(&m_CopyMaxUs, (uint32_t)us, __ATOMIC_RELAXED);
    __atomic_store_n(&m_CopyCount, m_CopyCount + 1, __ATOMIC_RELAXED);
}

// フレームバッファの y0 行目から y1 行目の手前までに重なる X68000 の画面の行を、次に写すときに変わっていなくても写す
// (FrameRowCache。ShowFrame() の中で、写す前に呼ぶ)
void CKernel::ForceRows(int y0, int y1) {
    if (y0 >= y1) return;
    platform::FrameRowCache& c = m_RowCache;
    if (c.forceY0 >= c.forceY1) {
        c.forceY0 = y0;
        c.forceY1 = y1;
        return;
    }
    if (y0 < c.forceY0) c.forceY0 = y0;
    if (y1 > c.forceY1) c.forceY1 = y1;
}

// 画面の隅の数字 (osd=on / fps) を数える。コア 0 がフレームを写した直後に呼び、前に数えてから 1 秒たっていれば、
// その間の合成した数、写した数、置き換わった数、X68000 の速さ、温度を m_OsdValue に置く
// (フレームを写した直後に数えるので、受け渡しの途中のフレームで合成した数と写した数がずれることはまずない)
// 画面の隅の数字に出す Raspberry Pi のモデル (Circle の CMachineInfo から)
int CKernel::OsdPiModel() {
    switch (CMachineInfo::Get()->GetMachineModel()) {
    case MachineModel3B: return platform::OSD_PI_3B;
    case MachineModel3BPlus: return platform::OSD_PI_3BPLUS;
    case MachineModel3APlus: return platform::OSD_PI_3APLUS;
    case MachineModelCM3: return platform::OSD_PI_CM3;
    case MachineModelCM3Plus: return platform::OSD_PI_CM3PLUS;
    case MachineModelZero2W: return platform::OSD_PI_ZERO2W;
    case MachineModel4B: return platform::OSD_PI_4B;
    case MachineModel400: return platform::OSD_PI_400;
    case MachineModelCM4: return platform::OSD_PI_CM4;
    case MachineModelCM4S: return platform::OSD_PI_CM4S;
    case MachineModel5: return platform::OSD_PI_5;
    case MachineModel500: return platform::OSD_PI_500;
    case MachineModelCM5: return platform::OSD_PI_CM5;
    case MachineModelCM5Lite: return platform::OSD_PI_CM5LITE;
    default: return platform::OSD_PI_OTHER;
    }
}

void CKernel::UpdateOsd() {
    if (m_Settings.osd == 0 || m_MenuActive) return;
    const u64 now = CTimer::GetClockTicks64();
    if (m_OsdTicks != 0 && now - m_OsdTicks < CLOCKHZ) return;
    uint32_t published, dropped;
    m_pEmulator->frameCounts(published, dropped);
    const uint32_t shown = m_FramesShownByCore0 + m_pEmulator->framesSelfPresented();
    uint64_t cycles, us, frames;
    m_pEmulator->stats(cycles, us, frames);
    if (m_OsdTicks != 0) {
        // 置き換わった数は、前に数えてからの時間で割って 1 秒あたりにする (四捨五入)。メニューを開けずに戻ったときなど、
        // 前に数えてから 1 秒より長くたっていることがある
        const u64 dt = now - m_OsdTicks;
        platform::OsdStats s;
        s.replaced = (unsigned)(((u64)(dropped - m_OsdFrames[1]) * CLOCKHZ + dt / 2) / dt);
        // 合成した fps は、コア 1 がフレームの終わりに記録した X68000 のフレームの数と時間から求める (0.1 まで出すので、
        // コア 0 が数えるときの時刻のずれ (数 ms) を入れない)。写した fps は、それに「写した数 / 合成した数」を掛ける
        s.composedDeci = platform::osdFpsDeci(frames - m_OsdEmuFrames, us - m_OsdUs);
        const uint32_t dPublished = published - m_OsdFrames[0], dShown = shown - m_OsdFrames[2];
        s.shownDeci = dPublished != 0 ? (unsigned)(((u64)s.composedDeci * dShown + dPublished / 2) / dPublished) : 0;
        const uint64_t dWallUs = us - m_OsdUs;
        const uint64_t dEmuUs = (cycles - m_OsdCycles) * 1000000 / m_pEmulator->cpuHz();
        s.speedPct = dWallUs != 0 ? (unsigned)((dEmuUs * 100 + dWallUs / 2) / dWallUs) : 0;
        // 読めなければ (QEMU など) 0 が返るので出さない。メールボックスを 1 回読む (1 秒に 1 回)。
        // 出す場所がないとき (フレームバッファの高さが 148 ラインより低く、横 1 行も X68000 の画面に重なり、osd=on の
        // ときだけ) は読まない
        const bool placed = __atomic_load_n(&m_OsdPlaced, __ATOMIC_RELAXED);
        // 0.1 度まで出すので、CCPUThrottle::GetTemperature() (1 度単位) ではなく 1/1000 度で読む
        const unsigned milliC = placed ? ReadMilliC() : 0;
        s.tempDeciC = milliC != 0 ? (int)((milliC + 50) / 100) : -1;
        // ファームウェアが ARM のクロックを下げたか、電圧が足りない (10 秒ごとの cpu: の行で読んだ値。ReportCpu())
        s.throttled = m_CpuStatus.throttledOk && (m_CpuStatus.throttled & 0xF000Fu) != 0;
#if RASPPI >= 5
        if (m_Fan.drive() != CPi5Fan::Drive::None) {
            s.fanLevel = m_Fan.level();
            m_OsdRpmShown = platform::osdRpmShown(m_OsdRpmShown, m_Fan.rpm());
            s.fanRpm = m_OsdRpmShown;
        }
#endif
        // Raspberry Pi のモデルと ARM のクロック (縦に並べるときのいちばん上)。クロックは 10 秒ごとの cpu: の行で測った値
        // (ReportCpu()。測れなければ要求した値)
        s.pi = OsdPiModel();
        s.armMHz = platform::osdArmMHzShown(m_CpuStatus.measuredMHz != 0 ? m_CpuStatus.measuredMHz : m_CpuStatus.setMHz);
        // MPU とクロック (縦に並べるときの 2 行目)
        s.mpu = m_Config.cpuModel == m68k::CpuModel::M68060   ? 68060
                : m_Config.cpuModel == m68k::CpuModel::M68030 ? 68030
                                                              : 68000;
        s.mpuMHz = (m_pEmulator->cpuHz() + 500000) / 1000000;
        // 最高速モードなら、実効のクロック (クロック × cpuSpeed() / 100) を出す
        m_OsdEffMHzShown =
            m_MaxSpeed ? platform::osdEffMHzShown(
                             m_OsdEffMHzShown, platform::osdEffKHz(m_pEmulator->cpuHz(), m_pEmulator->cpuSpeed()))
                       : 0;
        s.effMHz = m_OsdEffMHzShown;
        // 1GB より下のヒープの使った量 (一度でも切り出した量。ReadHeap()。メールボックスは使わない)。縦に並べるときだけ出る
        const HeapStatus heap = ReadHeap();
        if (heap.lowBytes != 0) s.heapMB = (int)((heap.lowBytes - heap.lowUnused + 512 * 1024) / (1024 * 1024));
        // ネットワークの IP アドレス (縦に並べるときのいちばん下。net=on でアドレスが決まっているときだけ)。
        // UpdateOsd() はコア 0 で動くので、ネットワークのタスクと同じコアで読む
        if (m_pHostNet != nullptr) s.ipv4 = m_pHostNet->IPv4();
        // 1 枚も合成していなければ (X68000 が止まっている) 出さない (ExpireOsd())
        PublishOsd(published != m_OsdFrames[0] ? platform::packOsd(s) : platform::OsdValue());
    }
    m_OsdTicks = now;
    m_OsdFrames[0] = published;
    m_OsdFrames[1] = dropped;
    m_OsdFrames[2] = shown;
    m_OsdCycles = cycles;
    m_OsdUs = us;
    m_OsdEmuFrames = frames;
}

// 画面の隅の数字を、フレームが来ないまま 2 秒たったら消す (X68000 が止まっている間、最後の数字が残らないように)。
// コア 0 が写す役を持っているときに、主ループから呼ぶ。合成したフレームの数が変わらなければ、写す役がどのコアでも
// フレームは来ていない。X68000 が止まっていれば、最後の画面を写し直して数字を消す (止まっている間は次のフレームが
// 来ないので)。動いていれば、次に写すフレームで消える
void CKernel::ExpireOsd() {
    if (m_pEmulator == nullptr || m_MenuActive || m_ComposerPresenting) return;
    uint32_t published, dropped;
    m_pEmulator->frameCounts(published, dropped);
    const u64 now = CTimer::GetClockTicks64();
    if (published != m_OsdPublished || m_OsdPublishedTicks == 0) {
        m_OsdPublished = published;
        m_OsdPublishedTicks = now;
        return;
    }
    if (now - m_OsdPublishedTicks < 2 * CLOCKHZ) return;
    PublishOsd(platform::OsdValue());
    m_OsdTicks = 0; // フレームが来たら数え直す
    if (m_OsdState.shown && m_pLastFrame != nullptr && m_pEmulator->paused()) ShowFrame(*m_pLastFrame, true);
}

// POST /message (CHttpTask、コア 0) から呼ぶ。字を並べて字形にし、写す役のコアに渡す。消す時刻は ShowFrame() が見る。
// メモリを確保できなければ false
bool CKernel::netMessage(const char* text, unsigned n, unsigned sec) {
    MessageMem* mm = s_pMsg;
    if (mm == nullptr) {
        if (n == 0) return true; // まだ何も出していないので、消すものもない
        mm = new MessageMem;
        if (mm == nullptr) return false;
        // 中身を作り終えてから写す役のコアに見せる
        __atomic_store_n(&s_pMsg, mm, __ATOMIC_RELEASE);
    }
    platform::layoutMessage(text, n, mm->layout);
    const uint8_t* cg = m_pEmulator != nullptr ? m_pEmulator->cgRom() : nullptr;
    platform::rasterMessage(mm->layout, cg, cg != nullptr ? x68k::Machine::CG_SIZE : 0, mm->build);
    const u64 until = mm->build.lines != 0 && sec != 0 ? CTimer::GetClockTicks64() + (u64)sec * CLOCKHZ : 0;
    mm->box.publish(mm->build, until);
    m_MsgExpireTicks = until;
    m_MsgRedraw = true;
    return true;
}

// 主ループから呼ぶ。動いている間は次に写すフレームでメッセージが出る (消える) が、X68000 が止まっている間は
// フレームが来ないので、新しいメッセージを置いたときと消す時刻が来たときに、最後の画面を写し直す (ExpireOsd() と同じ)
void CKernel::ServiceMessage() {
    if (m_pEmulator == nullptr || m_MenuActive || m_ComposerPresenting) return;
    bool need = m_MsgRedraw;
    if (m_MsgExpireTicks != 0 && CTimer::GetClockTicks64() >= m_MsgExpireTicks) {
        m_MsgExpireTicks = 0;
        need = true;
    }
    if (!need) return;
    m_MsgRedraw = false;
    if (m_pLastFrame != nullptr && m_pEmulator->paused()) ShowFrame(*m_pLastFrame, true);
}

// コア 1 が読んだテキスト画面が前回と違えば、UART のログに出す。tools/qemu_human68k.sh が読む
void CKernel::DumpText() {
    const char(*text)[x68k::TEXT_COLS + 1] = m_pEmulator->textDump();
    if (text == nullptr) return;
    const size_t bytes = sizeof(char[x68k::TEXT_ROWS][x68k::TEXT_COLS + 1]);
    if (memcmp(text, m_pLastDump, bytes) != 0) {
        memcpy(m_pLastDump, text, bytes);
        int rows = x68k::TEXT_ROWS;
        while (rows > 0 && text[rows - 1][0] == 0) rows--;
        uint64_t cycles, us, frames;
        m_pEmulator->stats(cycles, us, frames);
        m_Logger.Write(FromMachine, LogNotice, "textdump begin (frame %u)", (unsigned)frames);
        for (int r = 0; r < rows; r++) m_Logger.Write(FromMachine, LogNotice, "text|%s", text[r]);
        m_Logger.Write(FromMachine, LogNotice, "textdump end");
    }
    m_pEmulator->textDumpDone();
}

// 音のデバイスを用意して動かす (コア 1 を動かす前に、Machine の音の出力を有効にする)
void CKernel::StartSound() {
#ifdef PIX68K_QEMU
    const char* def = "off";
#else
    const char* def = "hdmi";
#endif
    const char* mode = m_Options.GetAppOptionString("pix68k_sound", def);
    CSoundBaseDevice* dev = nullptr;
    if (strcmp(mode, "null") == 0) {
        // 試験用: 音のデバイスを使わず、コア 0 が実時間に合わせて音を取り出して捨てる (DrainNullAudio())。
        // QEMU には音のデバイスがないので、音を作る経路 (コア 3 か、コア 1) をこれで動かす
        m_pEmulator->enableSound(pix68k_sound::RATE, pix68k_sound::RATE * 20 / 1000);
        m_NullAudio = true;
        m_NullAudioClock.start(CTimer::GetClockTicks64());
        m_Logger.Write(FromMachine, LogNotice, "sound: null (discarded), %u Hz", pix68k_sound::RATE);
        return;
    }
    if (strcmp(mode, "hdmi") == 0) dev = new CHdmiSoundOut(&m_Interrupt, m_pEmulator->audioSource());
    else if (strcmp(mode, "pwm") == 0) dev = new CPwmSoundOut(&m_Interrupt, m_pEmulator->audioSource());
    else if (strcmp(mode, "i2s") == 0) dev = NewI2sSound();
    else if (strcmp(mode, "off") != 0) m_Logger.Write(FromMachine, LogWarning, "pix68k_sound=%s: unknown; sound off", mode);
    if (dev == nullptr) {
        m_Logger.Write(FromMachine, LogNotice, "sound: off");
        return;
    }
    // リングバッファのたまり具合の最小値の目標は 20ms (音のデバイスは 11-16ms ずつ取り出す)
    m_pEmulator->enableSound(pix68k_sound::RATE, pix68k_sound::RATE * 20 / 1000);
    if (!dev->Start()) {
        m_Logger.Write(FromMachine, LogWarning, "sound: cannot start %s output; sound off", mode);
        delete dev;
        delete m_pI2CMaster; // I2S の DAC を設定するために作ったもの (dev より後に消す)
        m_pI2CMaster = nullptr;
        m_pEmulator->enableSound(0, 0);
        return;
    }
    m_pSound = dev;
    m_Logger.Write(FromMachine, LogNotice, "sound: %s, %u Hz", mode, pix68k_sound::RATE);
    if (strcmp(mode, "i2s") == 0) {
        // DAC を I2C で見つけて設定したかどうか (見つからなければ、設定の要らない DAC として送っている)
        if (dev->GetController() != nullptr) m_Logger.Write(FromMachine, LogNotice, "sound: i2s DAC configured over I2C");
        else m_Logger.Write(FromMachine, LogNotice, "sound: i2s without DAC control (no I2C DAC found or pix68k_i2s_dac=none)");
    }
}

// pix68k_sound=i2s の音のデバイスを作る。pix68k_i2s_dac= が none でなければ、DAC を設定するための I2C を用意する
CSoundBaseDevice* CKernel::NewI2sSound() {
    const char* dac = m_Options.GetAppOptionString("pix68k_i2s_dac", "auto");
    unsigned address = 0;
    bool useI2c = true;
    if (strcmp(dac, "none") == 0) {
        useI2c = false;
    } else if (strcmp(dac, "auto") != 0) {
        // 0x4c / 4c のような 16 進数 (7 ビットの番地)
        const char* p = dac;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        bool ok = *p != '\0';
        for (; *p != '\0' && ok; p++) {
            char c = *p;
            unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0')
                       : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                       : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16u;
            if (d >= 16) ok = false;
            else address = address * 16 + d;
            if (address > 0x7f) ok = false;
        }
        if (!ok || address == 0) {
            m_Logger.Write(FromMachine, LogWarning, "pix68k_i2s_dac=%s: unknown; using auto", dac);
            address = 0;
        }
    }
    if (useI2c) {
        unsigned bus = CMachineInfo::Get()->GetDevice(DeviceI2CMaster);
        m_pI2CMaster = new CI2CMaster(bus);
        if (!m_pI2CMaster->Initialize()) {
            delete m_pI2CMaster;
            m_pI2CMaster = nullptr;
            if (address != 0) {
                // 番地を指定したときは、DAC を設定できないので音を出さない (見つからなかったときと同じ)
                m_Logger.Write(FromMachine, LogWarning, "sound: cannot initialize I2C %u for DAC at 0x%02x; sound off", bus,
                               address);
                return nullptr;
            }
            m_Logger.Write(FromMachine, LogWarning, "sound: cannot initialize I2C %u; i2s without DAC control", bus);
        } else if (address != 0) {
            m_Logger.Write(FromMachine, LogNotice, "sound: i2s, DAC at I2C %u address 0x%02x", bus, address);
        } else {
            m_Logger.Write(FromMachine, LogNotice, "sound: i2s, probing DAC on I2C %u (PCM512x 0x4c/0x4d, WM8960 0x1a)", bus);
        }
    } else {
        m_Logger.Write(FromMachine, LogNotice, "sound: i2s, no DAC control (pix68k_i2s_dac=none)");
    }
    CSoundBaseDevice* dev = new CI2sSoundOut(&m_Interrupt, m_pEmulator->audioSource(), m_pI2CMaster, (u8)address);
    // Start() で止まったときに、どこまで進んだかをログで分かるようにする (DAC の探索は Start() の中で行う)
    m_Logger.Write(FromMachine, LogNotice, "sound: i2s device created; starting");
    return dev;
}

// MIDI ボードと音源 (StartSound() のあと、コア 1 を動かす前に呼ぶ)。SoundFont と MT-32 の ROM は場所と大きさを
// 確かめるだけで、読むのは LoadMidiSoundFont() と LoadMidiMt32()
void CKernel::StartMidi() {
    static const char FromMidi[] = "midi";
    Emulator* emu = m_pEmulator;
    if (!emu->machine().setMidiBoard(true)) {
        m_Logger.Write(FromMidi, LogWarning, "MIDI board: out of memory");
        return;
    }
    m_Logger.Write(FromMidi, LogNotice, "MIDI board (CZ-6BM1, YM3802) at $EAFA00");
    if (m_Settings.midiOut == 2) {
        m_Logger.Write(FromMidi, LogNotice, "midi_out=none: MIDI is not played");
        return;
    }
    if (m_Settings.midiOut == 1) {
        StartMidiUart();
        return;
    }
    if (m_Settings.midiOut == 3) {
        StartMidiMt32();
        return;
    }
    if (m_Settings.midiOut == 4) {
        StartMidiSc55();
        return;
    }
    if (m_Settings.midiSf2[0] == 0) {
        m_Logger.Write(FromMidi, LogWarning, "midi_sf2= is not set; MIDI is not played");
        return;
    }
    if (!emu->soundOutputEnabled()) {
        m_Logger.Write(FromMidi, LogWarning, "sound is off; the SoundFont is not loaded");
        return;
    }
    CString path;
    if (m_Settings.midiSf2[0] == '/') path.Format("%s%s", m_BootDrive, m_Settings.midiSf2);
    else path.Format("%s/pix68k/%s", m_BootDrive, m_Settings.midiSf2);
    FILINFO info;
    if (f_stat(path, &info) != FR_OK) {
        m_Logger.Write(FromMidi, LogWarning, "%s: not found; MIDI is not played", (const char*)path);
        return;
    }
    // 標本は float に直して持つ (ファイルの大きさの 2 倍ほど)。ここでは目安で断り、読むときに確保する量を数える
    // (LoadMidiSoundFont())
    size_t freeBytes = 0;
    if (!HeapHasRoom((size_t)info.fsize * 2, freeBytes)) {
        m_Logger.Write(FromMidi, LogWarning, "%s: %u MB is too large for the free heap (%u MB); MIDI is not played",
                       (const char*)path, (unsigned)(info.fsize >> 20), (unsigned)(freeBytes >> 20));
        return;
    }
    // 読むのは X68000 を動かし始めてから (LoadMidiSoundFont())。それまで MIDI ボードが送ったバイトは捨てる
    m_MidiSf2Path = path;
    m_Logger.Write(FromMidi, LogNotice, "%s: read after the X68000 starts (%u MB)", (const char*)path,
                   (unsigned)(info.fsize >> 20));
}

// SoundFont と MT-32 の ROM を読む口 (platform::Sf2Source)。SD カードのファイルから 256KB ずつ読み (Pi 3B+ で 20ms ほど)、その合間に
// コア 0 の仕事 (ディスクのイメージのチャンクの読み書きなど。ServiceDuringUsbScan()) をし、読んだのと同じ時間だけ休む
// (休まずに読むと、QEMU でコア 1 のフレームの遅れが増えた)
class CMidiFileSource : public platform::Sf2Source {
public:
    CMidiFileSource(CKernel& kernel, FIL& file) : m_Kernel(kernel), m_File(file) {}
    int read(void* out, unsigned n) override {
        unsigned done = 0;
        while (done < n && m_Result == FR_OK) {
            const unsigned want = n - done < kChunk ? n - done : kChunk;
            UINT got = 0;
            const u64 c0 = CTimer::GetClockTicks64();
            const FRESULT r = f_read(&m_File, (u8*)out + done, want, &got);
            done += got;
            // 読めなかったか、ファイルの終わりで足りなかったら、それを覚える (あとの読み書きで上書きしない)
            if (r != FR_OK) m_Result = r;
            else if (got != want) m_Result = FR_INT_ERR;
            m_Kernel.ServiceDuringUsbScan();
            const u64 c1 = CTimer::GetClockTicks64();
            m_Kernel.IdleUntil(c1 + (c1 - c0));
            m_Ticks += CTimer::GetClockTicks64() - c0;
        }
        return (int)done;
    }
    bool skip(unsigned n) override {
        if (m_Result != FR_OK) return false;
        // f_lseek() はファイルの終わりを超えると終わりで止めて FR_OK を返すので、先に確かめる
        const FSIZE_t pos = f_tell(&m_File), size = f_size(&m_File);
        if (n > size - pos) {
            m_Result = FR_INT_ERR;
            return false;
        }
        const FRESULT r = f_lseek(&m_File, pos + n);
        if (r != FR_OK) m_Result = r;
        return r == FR_OK;
    }
    FRESULT result() const { return m_Result; }
    u64 ticks() const { return m_Ticks; } // 読むのと休むのにかかった時間

private:
    static constexpr unsigned kChunk = 256u << 10;
    CKernel& m_Kernel;
    FIL& m_File;
    FRESULT m_Result = FR_OK;
    u64 m_Ticks = 0;
};

// StartMidi() が決めた SoundFont を読み、音源を X68000 につなぐ。コア 1 を動かし始めたあと、主ループに入る前に呼ぶ
// (起動を遅らせないため。Pi 3B+ で 30MB の SF2 に 4.6 秒かかる)。読む間は画面を合成するコアが画面を写す。
// ファイルは CMidiFileSource で少しずつ読む (その合間にハードディスクのイメージのチャンクを読み書きする)。読み終えたあと
// TinySoundFont が標本を float に直す間 (Pi 3B+ で 0.5 秒ほど) は分けられないので、その間はチャンクの読み書きが待つ。
// 主ループに入る前に読み終えるので、メニュー、ネットワークの口 (PUT など)、Ctrl + Alt + Delete はこのあとで扱う。
// 読み終えるまで音源はつないでいないので、MIDI ボードが送ったバイトは捨てる。つなぐときに、それまでに送った
// プログラムチェンジなどの設定は送り直す (Machine::setMidiOut() の resync)。X68000 がその間にリセットしても、
// つなぐ前なので何もしない。音源はつないだら消さない (コア 3 かコア 1 が使い続ける)。
// TinySoundFont が確保する量は、ヒープの空き (kHeapReserve を残す) を上限にして数え、超えるなら確保せずにやめる
// (platform::LoadLimit。Circle の new / malloc は足りないと全体を止めるため)
void CKernel::LoadMidiSoundFont() {
    static const char FromMidi[] = "midi";
    if (m_MidiSf2Path.GetLength() == 0 || m_pEmulator == nullptr) return;
    CString path = m_MidiSf2Path;
    m_MidiSf2Path = "";
    const u64 t0 = CTimer::GetClockTicks64();
    netBeginHeavy();
    FIL f;
    FRESULT r = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
    platform::Sf2Synth* synth = nullptr;
    bool overLimit = false;
    uint64_t limitBytes = 0, usedBytes = 0;
    u64 fileTicks = 0;
    if (r == FR_OK) {
        const size_t freeBytes = CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW);
        platform::LoadLimit limit;
        limit.maxBytes = freeBytes > kHeapReserve ? freeBytes - kHeapReserve : 0;
        limit.blockBytes = HeapBlockBytes;
        limitBytes = limit.maxBytes;
        synth = new platform::Sf2Synth;
        CMidiFileSource src(*this, f);
        if (!synth->load(src, (int)pix68k_sound::RATE, m_Settings.midiVoices, limit)) {
            overLimit = synth->overLimit();
            usedBytes = synth->loadBytes();
            if (src.result() != FR_OK) r = src.result();
            delete synth;
            synth = nullptr;
        } else {
            usedBytes = synth->loadBytes();
        }
        fileTicks = src.ticks();
        f_close(&f);
    }
    if (synth != nullptr) {
        m_pMidiOut = synth;
        m_pEmulator->setMidiOut(synth);
    }
    netEndHeavy();
    if (r != FR_OK) {
        m_Logger.Write(FromMidi, LogWarning, "%s: cannot read (FRESULT %d); MIDI is not played", (const char*)path,
                       (int)r);
        return;
    }
    if (synth == nullptr) {
        if (overLimit)
            m_Logger.Write(FromMidi, LogWarning,
                           "%s: needs more than %u MB of the free heap; MIDI is not played", (const char*)path,
                           (unsigned)(limitBytes >> 20));
        else
            m_Logger.Write(FromMidi, LogWarning, "%s: cannot load the SoundFont (%u MB allocated); MIDI is not played",
                           (const char*)path, (unsigned)(usedBytes >> 20));
        return;
    }
    const u64 t1 = CTimer::GetClockTicks64();
    m_Logger.Write(FromMidi, LogNotice,
                   "%s: %u presets, %d voices, %u MB of the heap (read in %u ms: file %u ms, samples %u ms)",
                   (const char*)path, (unsigned)synth->presetCount(), m_Settings.midiVoices, (unsigned)(usedBytes >> 20),
                   (unsigned)((t1 - t0) * 1000 / CLOCKHZ), (unsigned)(fileTicks * 1000 / CLOCKHZ),
                   (unsigned)((t1 - t0 - fileTicks) * 1000 / CLOCKHZ));
}

// midi_out=uart: midi_uart= の UART を 31.25kbps にして、MIDI ボードの音源にする (StartMidi() から呼ぶ)。
// 使える UART (Circle の CSerialDevice の番号と TXD の GPIO):
//   Pi 3: 0 (PL011、GPIO 14) だけ。Circle には PL011 のドライバしかなく、mini UART も TXD は GPIO 14 なので、ログと分けられない
//   Pi 4: 0 (GPIO 14)、2 (GPIO 0)、3 (GPIO 4)、4 (GPIO 8)、5 (GPIO 12)。どれも PL011 (FIFO 16 バイト)
//   Pi 5: 0 (GPIO 14)、1 (GPIO 0)、2 (GPIO 4)、3 (GPIO 8)、4 (GPIO 12) は RP1 の PL011 (クロック 50MHz)、10 は UART の端子
// ログは Pi 3 / Pi 4 では 0、Pi 5 では 10 に出している。同じ UART を選んだときは、ログを UART に出すのをやめてから
// 31.25kbps にする (ログの文字が MIDI の音源に届かないように)。別の UART ならログはそのまま。
// 分周: PL011 のクロック (48MHz、50MHz、44MHz、古いファームウェアの 3MHz) は、どれも 31250 × 16 で割り切れる
void CKernel::StartMidiUart() {
    static const char FromMidi[] = "midi";
    const unsigned dev = (unsigned)m_Settings.midiUart;
#if RASPPI <= 3
    static const int kTxd[] = {14};
    const unsigned logDev = 0;
#elif RASPPI == 4
    static const int kTxd[] = {14, -1, 0, 4, 8, 12};
    const unsigned logDev = 0;
#else
    static const int kTxd[] = {14, 0, 4, 8, 12, -1, -1, -1, -1, -1, -2};
    const unsigned logDev = 10;
#endif
    const int txd = dev < sizeof kTxd / sizeof kTxd[0] ? kTxd[dev] : -1;
    if (txd == -1) {
        m_Logger.Write(FromMidi, LogWarning,
                       "midi_uart=%u is not available on this Pi (Pi 3: 0; Pi 4: 0, 2-5; Pi 5: 0-4, 10); MIDI is not played",
                       dev);
        return;
    }
    CString pin;
    if (txd >= 0) pin.Format("GPIO %d", txd);
    else pin = "the UART connector";
    CSerialDevice* serial = nullptr;
    if (dev == logDev) {
        m_Logger.Write(FromMidi, LogNotice,
                       "midi_out=uart: UART %u (%s) is the log UART; the log no longer goes to the UART "
                       "(use syslog= or the network log)",
                       dev, (const char*)pin);
        m_Tee.DisableFirst();
        CTimer::SimpleMsDelay(5); // UART の FIFO に残ったログ (16 文字、115200bps で 1.4ms) を送り終えるのを待つ
        if (!m_Serial.Initialize(31250)) {
            m_Logger.Write(FromMidi, LogWarning, "cannot set the UART to 31250 bps; MIDI is not played");
            return;
        }
        serial = &m_Serial;
    } else {
        serial = new CSerialDevice(nullptr, FALSE, dev); // 割り込みを使わない (FIFO の空きは時刻から計算する)
        if (serial == nullptr || !serial->Initialize(31250)) {
            m_Logger.Write(FromMidi, LogWarning, "cannot set UART %u to 31250 bps; MIDI is not played", dev);
            delete serial;
            return;
        }
        m_pMidiSerial = serial;
    }
    serial->SetOptions(0); // 改行 (0x0A) の前に CR (0x0D) を足さない
    // Circle の CSerialDevice::Initialize() と同じ計算で、実際の速さをログに出す
#if RASPPI >= 5
    const unsigned clock = dev < 10 ? 50000000u : CMachineInfo::Get()->GetClockRate(CLOCK_ID_UART);
#else
    const unsigned clock = CMachineInfo::Get()->GetClockRate(CLOCK_ID_UART);
#endif
    const unsigned baud16 = 31250 * 16;
    const unsigned intDiv = clock / baud16;
    const unsigned fract2 = (clock % baud16) * 8 / 31250;
    const unsigned fractDiv = fract2 / 2 + fract2 % 2;
    const unsigned actual = intDiv != 0 ? (unsigned)((u64)clock * 4 / (intDiv * 64 + fractDiv)) : 0;
    const x68k::MidiOut::Family family = m_Settings.midiUartFamily == 1 ? x68k::MidiOut::MT32 : x68k::MidiOut::GENERAL;
    m_Logger.Write(FromMidi, LogNotice,
                   "midi_out=uart: MIDI goes out of UART %u (%s TXD) at %u bps (UART clock %u Hz, divisor %u + %u/64); "
                   "sound module: %s",
                   dev, (const char*)pin, actual, clock, intDiv, fractDiv,
                   family == x68k::MidiOut::MT32 ? "MT-32 (midi_uart_family=mt32)" : "GS (midi_uart_family=gs)");
    if (actual < 30938 || actual > 31563) // 1% より違えば、音源が受けられないかもしれない
        m_Logger.Write(FromMidi, LogWarning, "midi_out=uart: %u bps is more than 1%% off 31250 bps (check init_uart_clock=)",
                       actual);
    m_UartMidiDevice = dev;
    m_pUartMidi = new CUartMidiOut(serial, family);
    if (m_pUartMidi == nullptr) return;
    m_pMidiOut = m_pUartMidi;
    m_pEmulator->setMidiOut(m_pUartMidi);
}

// 主ループから呼ぶ (midi_out=uart のとき)。コア 1 が音を合成するコアを決めたら、送るコアを決める: コア 3 で合成するなら
// コア 3 (記録を処理するたびと、空いたとき。送るものが残っている間は眠らずに呼び続ける)、そうでなければコア 0 (ここ)。
// 決めるまでは、受けたバイトはリングに溜める
void CKernel::ServiceMidiUart() {
    if (m_pUartMidi->pumper() == platform::UartMidi::NONE) {
        if (!m_pEmulator->soundCoreDecided()) return;
        const bool core3 = m_pEmulator->soundCoreActive();
        m_pUartMidi->setPumper(core3 ? platform::UartMidi::SELF : platform::UartMidi::OTHER);
        m_Logger.Write("midi", LogNotice, "midi_out=uart: bytes are sent from core %u", core3 ? 3u : 0u);
    }
    if (m_pUartMidi->pumper() == platform::UartMidi::OTHER) m_pUartMidi->pump(CTimer::GetClockTicks64());
}

// midi_out=uart: 鳴っている音を止める (メニューを開いて X68000 を止めたとき、再起動の前)。溜まったバイトを送ってから止める。
// waitMs が 0 なら待たない (メニュー)。0 でなければ (再起動の前)、止めるバイトが UART から出終えるまで (FIFO の分も含めて)
// waitMs まで待つ。そのとき、溜まったバイトを送っていては間に合わなければ、読み手が溜まったバイトを捨てて止めるバイトを先に送る
// (platform/uart_midi.h の requestSilence() の期限。1ms は読み手が気づくまでの余裕)。送るのがコア 3 なら sev で起こす
void CKernel::SilenceMidiUart(unsigned waitMs) {
    if (m_pUartMidi == nullptr) return;
    const u64 t0 = CTimer::GetClockTicks64();
    const u64 end = t0 + (u64)waitMs * 1000;
    const uint32_t gen = m_pUartMidi->requestSilence(waitMs > 1 ? end - 1000 : 0);
    if (waitMs == 0) return;
    // 止めるバイトを FIFO に入れ終えるまで
    while (!m_pUartMidi->silenced(gen) && CTimer::GetClockTicks64() < end) {
        if (m_pUartMidi->pumper() == platform::UartMidi::SELF) asm volatile("sev");
        else m_pUartMidi->pump(CTimer::GetClockTicks64()); // コア 0 か、まだ決めていない (誰も送っていない)
    }
    // FIFO に入れた分 (最大 12 バイト、3.8ms) が UART から出終えるまで。drainedAt() は silenced() のあとに読む
    if (m_pUartMidi->silenced(gen)) {
        const u64 drained = m_pUartMidi->drainedAt() + platform::UartMidi::BYTE_US;
        while (CTimer::GetClockTicks64() < drained && CTimer::GetClockTicks64() < end) {
        }
    } else {
        m_Logger.Write("midi", LogWarning, "uart: could not send the note-off bytes in %u ms", waitMs);
    }
}

// midi_out=mt32: MT-32 の ROM の場所と大きさを確かめる (StartMidi() から呼ぶ)。読むのは LoadMidiMt32()
void CKernel::StartMidiMt32() {
    static const char FromMidi[] = "midi";
    if (m_Settings.midiMt32Ctrl[0] == 0 || m_Settings.midiMt32Pcm[0] == 0) {
        m_Logger.Write(FromMidi, LogWarning, "midi_out=mt32 needs midi_mt32_ctrl= and midi_mt32_pcm=; MIDI is not played");
        return;
    }
    if (!m_pEmulator->soundOutputEnabled()) {
        m_Logger.Write(FromMidi, LogWarning, "sound is off; the MT-32 ROMs are not loaded");
        return;
    }
    CString path[2];
    FSIZE_t size[2] = {0, 0};
    const char* const names[2] = {m_Settings.midiMt32Ctrl, m_Settings.midiMt32Pcm};
    for (int i = 0; i < 2; i++) {
        if (names[i][0] == '/') path[i].Format("%s%s", m_BootDrive, names[i]);
        else path[i].Format("%s/pix68k/%s", m_BootDrive, names[i]);
        FILINFO info;
        if (f_stat(path[i], &info) != FR_OK) {
            m_Logger.Write(FromMidi, LogWarning, "%s: not found; MIDI is not played", (const char*)path[i]);
            return;
        }
        size[i] = info.fsize;
        const bool ok = i == 0 ? platform::Mt32Synth::controlSizeOk(size[i]) : platform::Mt32Synth::pcmSizeOk(size[i]);
        if (!ok) {
            m_Logger.Write(FromMidi, LogWarning, "%s: %u bytes is not the size of an MT-32 / CM-32L %s ROM; MIDI is not played",
                           (const char*)path[i], (unsigned)size[i], i == 0 ? "CONTROL" : "PCM");
            return;
        }
    }
    size_t freeBytes = 0;
    if (!HeapHasRoom((size_t)platform::Mt32Synth::heapBytes(size[0], size[1]), freeBytes)) {
        m_Logger.Write(FromMidi, LogWarning, "MT-32: not enough free heap (%u MB); MIDI is not played",
                       (unsigned)(freeBytes >> 20));
        return;
    }
    // 読むのは X68000 を動かし始めてから (LoadMidiMt32())。それまで MIDI ボードが送ったバイトは捨てる
    m_MidiMt32Path[0] = path[0];
    m_MidiMt32Path[1] = path[1];
    m_Logger.Write(FromMidi, LogNotice, "MT-32: %s and %s: read after the X68000 starts", (const char*)path[0],
                   (const char*)path[1]);
}

// StartMidiMt32() が決めた ROM を読み、Munt を開いて X68000 につなぐ。LoadMidiSoundFont() と同じく、コア 1 を
// 動かし始めたあと、主ループに入る前に呼ぶ。ファイルは CMidiFileSource で 256KB ずつ読む (合わせて 1.1MB か 2.1MB)。
// Munt の確保には TinySoundFont のような差し替えの口がないので、確保する前に Mt32Synth::heapBytes() (確保の量の
// 上限。tests/unit/mt32_test.cpp で確かめている) がヒープに残っているかを確かめ直す (Circle の new は足りないと
// 全体を止めるため)。読めないとき (ROM が合わないなど) は知らせて、MIDI は鳴らさずに続ける。
// つないだあとの扱い (設定の送り直し、消さないこと) は LoadMidiSoundFont() と同じ
void CKernel::LoadMidiMt32() {
    static const char FromMidi[] = "midi";
    if (m_MidiMt32Path[0].GetLength() == 0 || m_pEmulator == nullptr) return;
    CString path[2] = {m_MidiMt32Path[0], m_MidiMt32Path[1]};
    m_MidiMt32Path[0] = "";
    m_MidiMt32Path[1] = "";
    const u64 t0 = CTimer::GetClockTicks64();
    netBeginHeavy();
    u8* data[2] = {nullptr, nullptr};
    unsigned size[2] = {0, 0};
    FRESULT r = FR_OK;
    int bad = -1; // 読めなかったファイル
    u64 fileTicks = 0;
    // 大きさを確かめ直し (StartMidi() のあとに替えられていないか)、確保する前にヒープの空きを確かめる
    for (int i = 0; i < 2 && r == FR_OK; i++) {
        FILINFO info;
        r = f_stat(path[i], &info);
        if (r == FR_OK) {
            size[i] = (unsigned)info.fsize;
            const bool ok =
                i == 0 ? platform::Mt32Synth::controlSizeOk(info.fsize) : platform::Mt32Synth::pcmSizeOk(info.fsize);
            if (!ok) r = FR_INT_ERR;
        }
        if (r != FR_OK) bad = i;
    }
    size_t freeBytes = 0;
    if (r == FR_OK && !HeapHasRoom((size_t)platform::Mt32Synth::heapBytes(size[0], size[1]), freeBytes)) {
        netEndHeavy();
        m_Logger.Write(FromMidi, LogWarning, "MT-32: not enough free heap (%u MB); MIDI is not played",
                       (unsigned)(freeBytes >> 20));
        return;
    }
    for (int i = 0; i < 2 && r == FR_OK; i++) {
        FIL f;
        r = f_open(&f, path[i], FA_READ | FA_OPEN_EXISTING);
        if (r != FR_OK) {
            bad = i;
            break;
        }
        if ((unsigned)f_size(&f) != size[i]) {
            r = FR_INT_ERR;
            bad = i;
        } else {
            data[i] = new u8[size[i]];
            CMidiFileSource src(*this, f);
            if (data[i] == nullptr || src.read(data[i], size[i]) != (int)size[i]) {
                r = data[i] == nullptr || src.result() == FR_OK ? FR_INT_ERR : src.result();
                bad = i;
            }
            fileTicks += src.ticks();
        }
        f_close(&f);
    }
    platform::Mt32Synth* synth = nullptr;
    platform::Mt32Synth::Result result = platform::Mt32Synth::OK;
    if (r == FR_OK) {
        synth = new platform::Mt32Synth;
        // ROM のバッファは load() が引き取る (読めなかったときも解放する)
        result = synth->load(data[0], size[0], data[1], size[1], (int)pix68k_sound::RATE);
        data[0] = data[1] = nullptr;
        if (result != platform::Mt32Synth::OK) {
            delete synth;
            synth = nullptr;
        }
    }
    delete[] data[0];
    delete[] data[1];
    if (synth != nullptr) {
        m_pMt32 = synth;
        m_pMidiOut = synth;
        m_pEmulator->setMidiOut(synth);
    }
    netEndHeavy();
    if (r != FR_OK) {
        m_Logger.Write(FromMidi, LogWarning, "%s: cannot read (FRESULT %d); MIDI is not played",
                       (const char*)path[bad < 0 ? 0 : bad], (int)r);
        return;
    }
    if (synth == nullptr) {
        m_Logger.Write(FromMidi, LogWarning, "MT-32: %s and %s: %s; MIDI is not played", (const char*)path[0],
                       (const char*)path[1], platform::Mt32Synth::resultText(result));
        return;
    }
    const u64 t1 = CTimer::GetClockTicks64();
    m_Logger.Write(FromMidi, LogNotice, "MT-32 (Munt): %s, %s (read in %u ms: files %u ms, open %u ms)",
                   synth->controlName(), synth->pcmName(), (unsigned)((t1 - t0) * 1000 / CLOCKHZ),
                   (unsigned)(fileTicks * 1000 / CLOCKHZ), (unsigned)((t1 - t0 - fileTicks) * 1000 / CLOCKHZ));
}

// midi_out=sc55: SC-55mkII の ROM のディレクトリを見て、5 つのファイルを選ぶ (StartMidi() から呼ぶ)。読むのは LoadMidiSc55()。
// Pi 3 では使わない: tests/bench/sc55_bench.cpp が Pi 3B+ (Linux) で実時間の 0.45 倍だった (Pi 5 は 1.6 倍)
void CKernel::StartMidiSc55() {
    static const char FromMidi[] = "midi";
#if RASPPI <= 3
    m_Logger.Write(FromMidi, LogWarning, "midi_out=sc55: the SC-55 emulation is too slow for this Pi (needs Pi 4 or later); "
                                         "MIDI is not played");
#else
    if (m_Settings.midiSc55[0] == 0) {
        m_Logger.Write(FromMidi, LogWarning, "midi_out=sc55 needs midi_sc55=; MIDI is not played");
        return;
    }
    if (!m_pEmulator->soundOutputEnabled()) {
        m_Logger.Write(FromMidi, LogWarning, "sound is off; the SC-55 ROMs are not loaded");
        return;
    }
    CString dirPath;
    if (m_Settings.midiSc55[0] == '/') dirPath.Format("%s%s", m_BootDrive, m_Settings.midiSc55);
    else dirPath.Format("%s/pix68k/%s", m_BootDrive, m_Settings.midiSc55);
    // ディレクトリの中のファイルの名前と大きさ (32 個まで見る)
    static constexpr int kMaxFiles = 32;
    CString names[kMaxFiles];
    const char* cnames[kMaxFiles];
    uint64_t sizes[kMaxFiles];
    int n = 0;
    DIR dir;
    if (f_opendir(&dir, dirPath) != FR_OK) {
        m_Logger.Write(FromMidi, LogWarning, "%s: not found; MIDI is not played", (const char*)dirPath);
        return;
    }
    FILINFO info;
    while (n < kMaxFiles && f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
        if (info.fattrib & AM_DIR) continue;
        names[n] = info.fname;
        sizes[n] = info.fsize;
        n++;
    }
    f_closedir(&dir);
    for (int i = 0; i < n; i++) cnames[i] = names[i];
    int index[platform::Sc55Synth::ROM_COUNT];
    const int missing = platform::Sc55Synth::pickRoms(cnames, sizes, n, index);
    if (missing >= 0) {
        m_Logger.Write(FromMidi, LogWarning, "%s: no %s (%s, %u bytes); MIDI is not played", (const char*)dirPath,
                       platform::Sc55Synth::romFileName(missing), platform::Sc55Synth::romPartName(missing),
                       platform::Sc55Synth::romSize(missing));
        return;
    }
    size_t freeBytes = 0;
    if (!HeapHasRoom((size_t)platform::Sc55Synth::heapBytes() + sizeof(platform::Sc55Synth), freeBytes)) {
        m_Logger.Write(FromMidi, LogWarning, "SC-55: not enough free heap (%u MB); MIDI is not played",
                       (unsigned)(freeBytes >> 20));
        return;
    }
    // 読むのは X68000 を動かし始めてから (LoadMidiSc55())。それまで MIDI ボードが送ったバイトは捨てる
    for (int i = 0; i < platform::Sc55Synth::ROM_COUNT; i++)
        m_MidiSc55Path[i].Format("%s/%s", (const char*)dirPath, (const char*)names[index[i]]);
    m_Logger.Write(FromMidi, LogNotice, "SC-55: %s: read after the X68000 starts", (const char*)dirPath);
#endif
}

// StartMidiSc55() が決めた ROM を読み、SC-55mkII を起動して X68000 につなぐ。LoadMidiMt32() と同じく、コア 1 を
// 動かし始めたあと、主ループに入る前に呼ぶ。ファイルは CMidiFileSource で 256KB ずつ読む (合わせて 3.6MB)。
// 起動 (Sc55Synth::BOOT_MS だけ進める。Pi 5 で 3 秒ほど) の間も、おおよそ 10ms ごとにコア 0 の仕事
// (ServiceDuringUsbScan()) をする。確保する前に Sc55Synth::heapBytes() がヒープに残っているかを確かめ直す。
// 音はコア 3 が作る (非同期。Sc55Synth::setAsync()、Emulator::runSound() の idleMidi())。音の合成がコア 3 でなければ
// (sound_core=off か、コア 3 が動かなかった) 使わない。読めないときは知らせて、MIDI は鳴らさずに続ける。
// つないだあとの扱い (設定の送り直し、消さないこと) は LoadMidiSoundFont() と同じ
void CKernel::LoadMidiSc55() {
    static const char FromMidi[] = "midi";
    if (m_MidiSc55Path[0].GetLength() == 0 || m_pEmulator == nullptr) return;
    CString path[platform::Sc55Synth::ROM_COUNT];
    for (int i = 0; i < platform::Sc55Synth::ROM_COUNT; i++) {
        path[i] = m_MidiSc55Path[i];
        m_MidiSc55Path[i] = "";
    }
    // コア 1 が音を合成するコアを決めるのを待つ (コア 3 が動くのを 1 秒まで待って決める)
    const u64 w0 = CTimer::GetClockTicks64();
    while (!m_pEmulator->soundCoreDecided() && CTimer::GetClockTicks64() - w0 < 3 * CLOCKHZ) ServiceDuringUsbScan();
    if (!m_pEmulator->soundCoreActive()) {
        m_Logger.Write(FromMidi, LogWarning, "SC-55: needs sound synthesis on core 3 (sound_core=on); MIDI is not played");
        return;
    }
    const u64 t0 = CTimer::GetClockTicks64();
    netBeginHeavy();
    FRESULT r = FR_OK;
    int bad = -1; // 読めなかったファイル
    FIL files[platform::Sc55Synth::ROM_COUNT];
    int opened = 0;
    // 大きさを確かめ直す (StartMidi() のあとに替えられていないか)
    for (int i = 0; i < platform::Sc55Synth::ROM_COUNT && r == FR_OK; i++) {
        r = f_open(&files[i], path[i], FA_READ | FA_OPEN_EXISTING);
        if (r != FR_OK) {
            bad = i;
            break;
        }
        opened++;
        if ((unsigned)f_size(&files[i]) != platform::Sc55Synth::romSize(i)) {
            r = FR_INT_ERR;
            bad = i;
        }
    }
    size_t freeBytes = 0;
    const bool room =
        r == FR_OK && HeapHasRoom((size_t)platform::Sc55Synth::heapBytes() + sizeof(platform::Sc55Synth), freeBytes);
    platform::Sc55Synth* synth = nullptr;
    platform::Sc55Synth::Result result = platform::Sc55Synth::OK;
    u64 fileTicks = 0;
    if (room) {
        CMidiFileSource s0(*this, files[0]), s1(*this, files[1]), s2(*this, files[2]), s3(*this, files[3]),
            s4(*this, files[4]);
        CMidiFileSource* all[platform::Sc55Synth::ROM_COUNT] = {&s0, &s1, &s2, &s3, &s4};
        platform::Sf2Source* src[platform::Sc55Synth::ROM_COUNT] = {&s0, &s1, &s2, &s3, &s4};
        synth = new platform::Sc55Synth;
        if (synth != nullptr) {
            result = synth->load(src, (int)pix68k_sound::RATE,
                                 [](void* k) { static_cast<CKernel*>(k)->ServiceDuringUsbScan(); }, this);
            for (int i = 0; i < platform::Sc55Synth::ROM_COUNT; i++) {
                fileTicks += all[i]->ticks();
                if (all[i]->result() != FR_OK && r == FR_OK) {
                    r = all[i]->result();
                    bad = i;
                }
            }
            if (result != platform::Sc55Synth::OK) {
                delete synth;
                synth = nullptr;
            }
        }
    }
    for (int i = 0; i < opened; i++) f_close(&files[i]);
    if (synth != nullptr) {
        synth->setAsync(true);
        m_pMidiOut = synth;
        m_pEmulator->setMidiOut(synth);
    }
    netEndHeavy();
    if (r != FR_OK) {
        m_Logger.Write(FromMidi, LogWarning, "%s: cannot read (FRESULT %d); MIDI is not played",
                       (const char*)path[bad < 0 ? 0 : bad], (int)r);
        return;
    }
    if (!room) {
        m_Logger.Write(FromMidi, LogWarning, "SC-55: not enough free heap (%u MB); MIDI is not played",
                       (unsigned)(freeBytes >> 20));
        return;
    }
    if (synth == nullptr) {
        m_Logger.Write(FromMidi, LogWarning, "SC-55: %s; MIDI is not played", platform::Sc55Synth::resultText(result));
        return;
    }
    const u64 t1 = CTimer::GetClockTicks64();
    m_Logger.Write(FromMidi, LogNotice,
                   "SC-55mkII (Nuked-SC55): %u Hz, %u ms behind OPM (read and booted in %u ms: files %u ms)",
                   (unsigned)(synth->sourceRate() + 0.5),
                   (unsigned)(platform::Sc55Synth::ASYNC_DELAY * 1000 / pix68k_sound::RATE),
                   (unsigned)((t1 - t0) * 1000 / CLOCKHZ), (unsigned)(fileTicks * 1000 / CLOCKHZ));
}

void CKernel::ReportSpeed() {
    uint64_t cycles, us, frames;
    m_pEmulator->stats(cycles, us, frames);
    if (us == 0) return;
    unsigned emuMs = (unsigned)(cycles * 1000 / m_pEmulator->cpuHz());
    unsigned wallMs = (unsigned)(us / 1000);
    unsigned pct = wallMs ? (unsigned)((u64)emuMs * 100 / wallMs) : 0;
    // 起動からの合計に加え、前のログからの区間の速さも出す (合計は、起動中などにコア 1 が 0.25 秒より大きく遅れて
    // 基準を取り直した分 (取り戻さない) を含むので、そのあとずっと 100% に届かない)
    const uint64_t dEmuUs = (cycles - m_SpeedCyclesLogged) * 1000000 / m_pEmulator->cpuHz();
    const uint64_t dWallUs = us - m_SpeedUsLogged;
    const unsigned pctNow = dWallUs ? (unsigned)(dEmuUs * 1000 / dWallUs) : 0;
    m_SpeedCyclesLogged = cycles;
    m_SpeedUsLogged = us;
    m_Logger.Write(FromMachine, LogNotice,
                   "speed: %u frames, emulated %u.%03u s in %u.%03u s (%u%% of real time; %u.%u%% in the last interval)",
                   (unsigned)frames, emuMs / 1000, emuMs % 1000, wallMs / 1000, wallMs % 1000, pct, pctNow / 10,
                   pctNow % 10);
    // midi_out=uart: 送ったバイト、捨てたバイト (リングがあふれた)、溜まった量の最大、音を止めた回数 (起動からの合計)
    if (m_pUartMidi != nullptr)
        m_Logger.Write("midi", LogNotice,
                       "uart: %u bytes sent, %u dropped, %u flushed, %u queued at most, %u silenced (in total)",
                       m_pUartMidi->sent(), m_pUartMidi->dropped(), m_pUartMidi->flushed(), m_pUartMidi->maxBacklog(),
                       m_pUartMidi->resets());
    // フレームごとの時間の区間を区切るよう頼む。コア 1 が次のフレームの終わりで区切り、主ループの
    // ReportFrameTiming() が受け取って出す (コア 0 はここで待たない)
    m_FrameTimingPending = true;
    ReportFrameTiming();
    // 数はログを書く前にまとめて取る (ログを書く間も合成するコアが写すので、行ごとに取るとずれる)
    uint32_t published, dropped;
    m_pEmulator->frameCounts(published, dropped);
    const uint32_t self = m_pEmulator->framesSelfPresented();
    // 合成した数 = 置き換わった数 + コア 0 が写した数 + 合成するコアが写した数 (受け渡し中の 1 フレームの差はありうる)
    m_Logger.Write(FromMachine, LogNotice,
                   "screen: %u frames composed, %u replaced before shown, %u shown by core 0, %u by the composing core "
                   "(in total)",
                   published, dropped, m_FramesShownByCore0, self);
    // 同じ数の、前のログからの差。コア 0 が takeFrame() を見に行く間隔の最大 (主ループの 1 回にかかった時間。
    // 写す役を合成するコアに任せている間と、メニューを開いている間は数えない) も出す。これが 1 フレーム
    // (約 18ms) を超えると、その間に合成したフレームは置き換わることがある
    {
        const uint32_t gapUs = (uint32_t)(m_PollGapMax * 1000000 / CLOCKHZ);
        m_PollGapMax = 0;
        m_Logger.Write(FromMachine, LogNotice,
                       "screen: last 10 s: %u composed, %u replaced, %u shown by core 0, %u by the composing core; "
                       "core 0 polled for frames every %u us at most",
                       published - m_ScreenLogged[0], dropped - m_ScreenLogged[1],
                       m_FramesShownByCore0 - m_ScreenLogged[2], self - m_ScreenLogged[3], gapUs);
        m_ScreenLogged[0] = published;
        m_ScreenLogged[1] = dropped;
        m_ScreenLogged[2] = m_FramesShownByCore0;
        m_ScreenLogged[3] = self;
    }
    // 1 フレームを写す時間は、前のログからの区間で出す (最大は、合成するコアが同時に書き換えると 1 フレーム分
    // ずれることがある。ログ用なので気にしない)
    const uint32_t copies = __atomic_load_n(&m_CopyCount, __ATOMIC_RELAXED);
    const uint64_t copyUs = __atomic_load_n(&m_CopyTotalUs, __ATOMIC_RELAXED);
    const uint32_t copyMax = __atomic_exchange_n(&m_CopyMaxUs, 0u, __ATOMIC_RELAXED);
    const uint32_t rowsCopied = __atomic_load_n(&m_RowsCopied, __ATOMIC_RELAXED);
    const uint32_t rowsSkipped = __atomic_load_n(&m_RowsSkipped, __ATOMIC_RELAXED);
    if (copies != m_CopyCountLogged) {
        // 変わっていないので写さなかった行の割合 (pix68k_fb_rows=0 なら出さない)
        const uint32_t dc = rowsCopied - m_RowsLogged[0], ds = rowsSkipped - m_RowsLogged[1];
        CString rows;
        if (m_RowCache.pixels != nullptr && dc + ds != 0)
            rows.Format("; %u%% of the rows unchanged and not copied", (unsigned)((u64)ds * 100 / (dc + ds)));
        // 効果を掛けて写した (スキャンラインだけなら「; scanlines」、表で写したら「; video_filter=curve,mask」など。
        // 整数倍でない枠で効果がなければ「; video_filter=none」。あり / なしの時間を比べるため)
        // (m_ShownScale は写す役の合成するコアが書き換えるので読まず、ShowFrame() が置いた値を読む)
        const int crtShown = __atomic_load_n(&m_CrtShown, __ATOMIC_RELAXED);
        if (crtShown & kCrtShownMap) {
            rows.Append("; video_filter=");
            rows.Append(platform::videoFilterName(crtShown & (platform::CRT_PRESET_COUNT - 1)));
        } else if (crtShown == platform::CRT_SCANLINE) {
            rows.Append("; scanlines");
        }
        m_Logger.Write(FromMachine, LogNotice, "screen: copy to the framebuffer %u us on average, %u us at most%s",
                       (unsigned)((copyUs - m_CopyUsLogged) / (copies - m_CopyCountLogged)), copyMax,
                       (const char*)rows);
    }
    m_CopyCountLogged = copies;
    m_CopyUsLogged = copyUs;
    // 効果の表を作っていたら、かかった時間 (画面モードか組み合わせが変わったときに、写す役のコアがフレームを写すたびに
    // 少しずつ作る。ShowFrame())。作り終えた表の数、作り進めたフレームの数、合わせた時間、1 フレームでの最大
    const uint32_t slices = __atomic_load_n(&m_CrtBuildSlices, __ATOMIC_RELAXED);
    if (slices != m_CrtBuildSlicesLogged) {
        const uint32_t done = __atomic_load_n(&m_CrtBuildsDone, __ATOMIC_RELAXED);
        const uint32_t total = __atomic_load_n(&m_CrtBuildTotalUs, __ATOMIC_RELAXED);
        m_Logger.Write(FromMachine, LogNotice,
                       "screen: video table: %u built over %u frames, %u us in total, %u us at most in one frame",
                       done - m_CrtBuildsDoneLogged, slices - m_CrtBuildSlicesLogged, total - m_CrtBuildTotalUsLogged,
                       __atomic_exchange_n(&m_CrtBuildUs, 0u, __ATOMIC_RELAXED));
        m_CrtBuildSlicesLogged = slices;
        m_CrtBuildsDoneLogged = done;
        m_CrtBuildTotalUsLogged = total;
    }
    // コア 2 が 1 フレームを合成するのに使った時間と、コア 3 が 1 フレーム分の音を合成するのに使った時間は、
    // 区間を区切るよう頼む。そのコアが次のフレームの終わりで区切り、主ループの ReportCoreWork() が受け取って出す
    m_CoreWorkPending[0] = m_pEmulator->renderCoreActive();
    m_CoreWorkPending[1] = m_pEmulator->soundCoreActive();
    ReportCoreWork();
    m_RowsLogged[0] = rowsCopied;
    m_RowsLogged[1] = rowsSkipped;
    ReportCpu(FromMachine);
    if (m_pSound != nullptr || m_NullAudio)
        // たまっている量と、たまり具合の最小値の目標 (20ms から。足りなくなったときと、間に合わない間に多く作った
        // ときに増える。x68k/sound.h)
        m_Logger.Write(FromMachine, LogNotice, "sound: %u underrun(s), %u ms buffered, aiming for at least %u ms",
                       m_pEmulator->audioUnderruns(), m_pEmulator->audioFill() * 1000 / pix68k_sound::RATE,
                       m_pEmulator->audioGoal() * 1000 / pix68k_sound::RATE);
    ReportSoundLevels();
    ReportDisks();
    if (m_pEmulator->soundCoreActive()) {
        uint32_t wakes, empty;
        m_pEmulator->soundCoreWakes(wakes, empty);
        m_Logger.Write(FromMachine, LogNotice, "core 3: woke %u times, %u of them with nothing to do (in total)", wakes,
                       empty);
    }
}

// 前に出してからの音の大きさ (x68k::Sound::takeLevels()): MIDI の音源を足す前の音の最大、MIDI の音源が足した量の最大
// (どちらも 16 ビットの上限 32767 に対する値)、足して 16 ビットを超え、止めたフレームの数。MIDI の音源が鳴ったか、
// 止めたフレームがあった区間だけ出す。音が割れるときに、振り切れているのか (止めたフレームが多い)、MIDI の音源の
// 出力そのものが上限に張りついているのか (最大が 32767)、どちらでもないのかを見分けるためのもの。
// MT-32 の音源が捨てたメッセージ (待ち行列が満杯) とシステムエクスクルーシブ (長すぎ) の数は、増えたときに出す
void CKernel::ReportSoundLevels() {
    const x68k::Sound::Levels lv = m_pEmulator->takeAudioLevels();
    if (lv.midi != 0 || lv.clipped != 0)
        m_Logger.Write(FromMachine, LogNotice, "sound level: peak %u before MIDI, MIDI peak %u (of 32767), %u frame(s) clipped after the mix",
                       (unsigned)lv.base, (unsigned)lv.midi, (unsigned)lv.clipped);
    if (m_pMt32 != nullptr) {
        const uint32_t messages = m_pMt32->droppedMessages(), sysex = m_pMt32->droppedSysex();
        if (messages != m_Mt32DroppedLogged[0] || sysex != m_Mt32DroppedLogged[1])
            m_Logger.Write("midi", LogWarning, "MT-32: dropped %u message(s) (queue full) and %u SysEx (too long) in total",
                           (unsigned)messages, (unsigned)sysex);
        m_Mt32DroppedLogged[0] = messages;
        m_Mt32DroppedLogged[1] = sysex;
    }
}

// コア 2 が 1 フレームを合成するのに使った時間と、コア 3 が 1 フレーム分の音を合成するのに使った時間 (前に受け取って
// からの区間の平均と最大)。ReportSpeed() で頼んだあと、そのコアが区切るまで主ループで毎回呼ぶ。
// X68000 が止まっていてフレームが進まない間は区切られないので、動き出してから出る
// 最大 (at most) は、合成するコアが何フレーム分かをまとめて処理したときは、その時間をフレームの数で割った値で、
// フレームごとの最大ではない (platform/core_work.h)
void CKernel::ReportCoreWork() {
    for (int which = 0; which < 2; which++) {
        if (!m_CoreWorkPending[which]) continue;
        platform::CoreWork w;
        if (!m_pEmulator->takeCoreWork(which, w)) continue;
        m_CoreWorkPending[which] = false;
        if (w.frames == 0) continue;
        m_Logger.Write(FromMachine, LogNotice,
                       which == 0 ? "screen: core 2 composed a frame in %u us on average, %u us at most"
                                  : "sound: core 3 synthesized a frame of sound in %u us on average, %u us at most",
                       w.averageUs(), w.maxUs);
    }
}

// 前の報告からのフレームごとの時間: CPU サイクル (平均、最小..最大)、処理時間 (平均、最悪)、
// 次のフレームまでの残り時間の最小値 (負なら遅れた)、遅れたフレームの数。
// ReportSpeed() で頼んだあと、コア 1 が区切るまで主ループで毎回呼ぶ
void CKernel::ReportFrameTiming() {
    if (!m_FrameTimingPending) return;
    Emulator::FrameTiming t;
    if (!m_pEmulator->takeFrameTiming(t)) return;
    m_FrameTimingPending = false;
    if (t.frames == 0) return;
    m_Logger.Write(FromMachine, LogNotice,
                   "frames: %u, cycles/frame %u (%u..%u), busy %u us/frame (worst %u us), min spare %d us, %u late, "
                   "%u waits for core 2 and %u for core 3 in total (%u and %u in this interval)",
                   t.frames, (unsigned)(t.cycles / t.frames), t.minCycles, t.maxCycles, (unsigned)(t.busyUs / t.frames),
                   t.worstBusyUs, (int)t.minSpareUs, t.late, t.renderWaits, t.soundWaits,
                   t.renderWaits - m_RenderWaitsLogged, t.soundWaits - m_SoundWaitsLogged);
    m_RenderWaitsLogged = t.renderWaits;
    m_SoundWaitsLogged = t.soundWaits;
    // コア 2 / 3 を待った時間 (busy に入っている)。待ちを除いた処理時間の最大と、遅れたフレームのうち、待ちを除けば
    // 遅れなかったもの、その間にコア 2 が画面を写していたもの (コア 0 が USB の装置を探す間など) の数。
    // 遅れの原因がコア 1 の処理か、コア 2 / 3 を待ったことかを分けるため (#9)
    if (t.waitUs != 0 || t.late != 0)
        m_Logger.Write(FromMachine, LogNotice,
                       "core 1: waited %u us/frame for cores 2 and 3 (worst frame without the wait %u us); "
                       "of %u late frames, %u were late only because of the wait and %u overlapped with core 2 copying to the framebuffer",
                       (unsigned)(t.waitUs / t.frames), t.worstOwnUs, t.late, t.lateByWait, t.latePresenting);
    // 次のフレームまで wfe で眠った時間 (1 フレームあたり)、起きる予定から実際に起きるまでの最大、
    // 待ち終えた時刻のフレームの時刻に対する遅れの最大。起きるまでの最大がイベントストリームの周期を大きく超えるなら、
    // コア 1 にイベントストリームが届いていない
    // 起動時の確認の結果: 2ms に戻った回数と、期限のすぐあとに起きられた回数 (16 回中。14 回以上で使う)
    if (t.streamUs != 0)
        m_Logger.Write(FromMachine, LogNotice,
                       "core 1: slept %u us/frame (wfe), woke up to %u us late, finished waiting up to %u us after "
                       "the frame time (event stream every %u us, %u wakes in 2000 us and %u/%u on time at start)",
                       (unsigned)(t.sleptUs / t.frames), t.worstWakeUs, t.worstLateUs, t.streamUs, t.probeWakes,
                       t.probeOnTime, event_wait::WAKE_TRIALS);
    else if (t.probeWakes != Emulator::FrameTiming::NOT_PROBED)
        m_Logger.Write(FromMachine, LogWarning,
                       "core 1: event stream not delivered (%u wakes in 2000 us, %u/%u on time at start); "
                       "waiting by spinning, up to %u us after the frame time",
                       t.probeWakes, t.probeOnTime, event_wait::WAKE_TRIALS, t.worstLateUs);
    else
        m_Logger.Write(FromMachine, LogNotice, "core 1: waiting by spinning, up to %u us after the frame time",
                       t.worstLateUs);
}

// キーボード slot の USB のレポート (修飾キーと 6 つのキー。USB の割り込み)。キーボードの種類に合わせて書き換え
// (Pi 400 のラズベリーのキーなど。platform/key_remap.h)、すべてのキーボードの押しているキーと合わせてから送る
// (platform/key_merge.h)。ErrorRollOver などを含むレポートは無視する (そのキーボードの前の状態のまま)
void CKernel::KeyReport(int slot, unsigned char modifiers, const unsigned char rawKeys[6]) {
    if (slot < 0 || slot >= MaxKeyboards) return;
    unsigned char mods, keys[6];
    m_KeyRemap[slot].apply(modifiers, rawKeys, mods, keys);
    if (!m_KeyMerge.set(slot, mods, keys)) return;
    DispatchKeys();
}

// すべてのキーボードを合わせた押しているキーから、Ctrl + Alt + Delete (Raspberry Pi の再起動。platform/reboot_keys.h) を
// 見つけ、残りを X68000 と設定メニューに渡す (USB の割り込みか、割り込みを止めたコア 0 から呼ぶ)。
// Ctrl + Alt + Delete を見つけたら、主ループが書き戻してから再起動する (RebootFromKeyboard())
void CKernel::DispatchKeys() {
    u8 mods, down[32];
    m_KeyMerge.merged(mods, down);
    if (m_RebootKeys.filter(mods, down)) m_RebootRequest.press();
    if (m_pEmulator != nullptr) SendKeyReport(mods, down);
    else if (m_BootMenuActive) BootMenuKeys(mods, down); // 起動の組み合わせのメニュー (boot_menu.cpp)
}

// 押しているキー (修飾キーのビットと usage ごとの 1 ビット) を前回と比べ、押した / 離したキーを X68000 に送る。
// キューが満杯で送れなかった分は m_Keys が覚えていて、次のレポートか ResendKeys() で送る
void CKernel::SendKeyReport(u8 mods, const u8 down[32]) {
    Emulator* emu = m_pEmulator;
    m_LastMods = mods;
    // 設定メニューのキーと、メニューを開いている間のキーは X68000 に送らない (machine_menu.cpp)
    u8 outMods = 0, outDown[32], events[6];
    int nEvents = 0;
    switch (m_MenuKeys.filterBits(mods, down, m_MenuActive, outMods, outDown, events, nEvents)) {
    case platform::MenuKeyFilter::OPEN:
        m_Keys.releaseAll([emu](uint8_t u, bool d) { return emu->keyEvent(u, d); });
        m_MenuActive = true; // 主ループがコア 1 を止めてから開く
        return;
    case platform::MenuKeyFilter::MENU:
        for (int i = 0; i < nEvents; i++) PushMenuKey(events[i]);
        return;
    case platform::MenuKeyFilter::PASS:
        break;
    }
    m_Keys.reportBits(outMods, outDown, [emu](uint8_t u, bool d) { return emu->keyEvent(u, d); });
}

// Ctrl + Alt + Delete を押したとき (コア 0 の主ループ)。メニューの Restart と同じく、X68000 を止めてディスクと SRAM を
// 書いてから再起動する (true を返したら、主ループが ShutdownReboot を返す)。
// ディスクか SRAM を書けなければ再起動せず、X68000 を動かし直す。60 秒以内にもう一度 Ctrl + Alt + Delete を押したら、もう一度
// 書くのを試し、それでも書けなければ書かずに再起動する (メニューの Restart を 2 回選んだときと同じ)。
// ネットワークからの再起動 (POST /reboot) も同じ手順で行う (who はログに出す名前、request は 60 秒の数え方を別々に持つ)
bool CKernel::RebootWritingBack(const char* who, platform::RebootRequest& request) {
    Emulator* emu = m_pEmulator;
    m_Logger.Write(FromMachine, LogNotice, "%s: writing back disks and SRAM before restarting", who);
    KeepAlive();
    // メニューを開いているときと、deploy の再起動を待たせているときは、もう止まっている
    emu->requestPause(true);
    const u64 t0 = CTimer::GetClockTicks64();
    while (!emu->paused() && CTimer::GetClockTicks64() - t0 < 5 * CLOCKHZ) asm volatile("sev");
    const bool paused = emu->paused();
    if (!paused) m_Logger.Write(FromMachine, LogWarning, "%s: core 1 did not pause", who);
    // 止まらなければディスクは書き戻せない (書き戻すものがなければそれでよい)
    const bool flushed = m_pDiskWriter == nullptr || (paused && FlushDisks());
    // SRAM: 止まっていれば Machine の SRAM を、止まらなければコア 1 が作った写し (あれば) を書く
    bool sramOk = true;
    if (paused) {
        if (emu->machine().sramDirty() || emu->pendingSram() != nullptr) sramOk = SaveSram(emu->machine().sram());
    } else if (const u8* sram = emu->pendingSram()) {
        sramOk = SaveSram(sram);
    }
    if (!flushed || !sramOk) {
        const char* what = !flushed && !sramOk ? "disks and SRAM" : !flushed ? "disks" : "SRAM";
        // 断るときは、書いている間に押し直した分も捨てる (platform::RebootRequest)
        if (!request.refused(CTimer::GetClockTicks64())) {
            m_Logger.Write(FromMachine, LogWarning,
                           "%s: cannot write back %s; not restarting "
                           "(%s again within 60 s to restart without writing back)",
                           who, what, &request == &m_RebootRequest ? "press Ctrl+Alt+Delete" : "POST /reboot");
            // メニューを開いている間と、deploy の再起動を待たせている間は止めたまま
            if (!m_MenuActive && !m_DeployHeld) {
                emu->requestPause(false);
                const u64 t1 = CTimer::GetClockTicks64();
                while (emu->paused() && CTimer::GetClockTicks64() - t1 < CLOCKHZ) asm volatile("sev");
            }
            return false;
        }
        m_Logger.Write(FromMachine, LogWarning, "%s: restarting without writing back %s", who, what);
    }
    // POST /reboot?profile= を断ったあと取り消せずに残っていれば、ほかの再起動 (Ctrl + Alt + Delete) の前に戻す
    // (ディスクを閉じ、アンマウントする FinishDeploy() の前に)。戻せなければ、断った組み合わせで起動しないよう
    // 再起動しない
    if (&request != &m_NetReboot && !CancelProfileChoiceForRestart(who)) {
        if (!m_MenuActive && !m_DeployHeld) {
            emu->requestPause(false);
            const u64 t1 = CTimer::GetClockTicks64();
            while (emu->paused() && CTimer::GetClockTicks64() - t1 < CLOCKHZ) asm volatile("sev");
        }
        return false;
    }
    CloseDiskFiles();
    if (m_DeploySeconds != 0) FinishDeploy(false);
    return true;
}

// 送れていないキーの変化を送り直す (コア 0 の主ループ)。キューの書き手を 1 つにするため、USB の割り込みを止める
void CKernel::ResendKeys() {
    Emulator* emu = m_pEmulator;
    EnterCritical();
    m_Keys.resync([emu](uint8_t u, bool d) { return emu->keyEvent(u, d); });
    LeaveCritical();
}

// ゲームパッドのジョイスティックのビットが、コア 1 に送ったものと違えば送る
// (USB の割り込みか、割り込みを止めたコア 0 から呼ぶ)
void CKernel::SendGamePad(int slot) {
    if (m_PadSent[slot] == (int)m_PadWant[slot]) return;
    if (m_pEmulator->joystickEvent(slot, m_PadWant[slot])) m_PadSent[slot] = m_PadWant[slot];
}

// ゲームパッドの Start + Select を押したとき (コア 0 の主ループ)。設定メニューの Reset と同じく、X68000 を止めて
// ディスクを書いてからリセットし (resetMachine())、動かし直す。止まらなければリセットしない
void CKernel::ResetFromGamePad() {
    Emulator* emu = m_pEmulator;
    m_Logger.Write(FromMachine, LogNotice, "gamepad Start+Select: resetting the X68000");
    emu->requestPause(true);
    const u64 t0 = CTimer::GetClockTicks64();
    while (!emu->paused()) {
        asm volatile("sev");
        if (CTimer::GetClockTicks64() - t0 > 5 * CLOCKHZ) {
            m_Logger.Write(FromMachine, LogWarning, "gamepad Start+Select: core 1 did not pause; not reset");
            emu->requestPause(false);
            return;
        }
    }
    // midi_out=uart なら、実機の音源で鳴っている音を止める (メニューを開いたときと同じ)
    SilenceMidiUart(0);
    resetMachine();
    // 止めていた間は数えない。画面の隅の数字は数え直し、数え終えるまで出さない (CloseMenu() と同じ)
    PublishOsd(platform::OsdValue());
    m_OsdTicks = 0;
    // コア 1 が動き出すまで待つ (CloseMenu() と同じ)
    emu->requestPause(false);
    const u64 t1 = CTimer::GetClockTicks64();
    while (emu->paused() && CTimer::GetClockTicks64() - t1 < CLOCKHZ) asm volatile("sev");
}

bool CKernel::InputPending() const {
    return m_Mouse.pending() || m_PadSent[0] != (int)m_PadWant[0] || m_PadSent[1] != (int)m_PadWant[1];
}

// 送れていないマウスとジョイスティックの変化を送り直す (コア 0 の主ループ)
void CKernel::ResendInput() {
    Emulator* emu = m_pEmulator;
    EnterCritical();
    m_Mouse.flush([emu](int x, int y, uint8_t b) { return emu->mouseEvent(x, y, b); });
    SendGamePad(0);
    SendGamePad(1);
    LeaveCritical();
}

// X68000 のキーボードの LED が変わったら、USB キーボードの LED に送る (CAPS → Caps Lock、かな → Scroll Lock、
// INS → Num Lock。x68k::Keyboard::hidLeds()。Pi 400 では Num Lock は Num Lock のキーで切り替えた状態)。SetLEDs() は割り込みの中から呼べないので、コア 0 の主ループで呼ぶ。
// 送れなかったら、間を空けて何回か送り直す (platform::LedSender)。送れないキーボード (QEMU など) で毎回は試さない
// つないでいるすべてのキーボードに送る (キーボードごとに、種類に合わせて直した値を、送れたかを別々に覚えて送る)。
// 答えないキーボードへの転送は Circle の中で最大 3 秒ほど待つことがあるので、1 回の主ループで送る (送ろうとする) のは
// 1 台までにし、次の回は次の枠から見る (4 台に続けて送ってコア 0 が長く止まらないように)
void CKernel::UpdateKeyboardLeds() {
    const u8 base = x68k::Keyboard::hidLeds(m_pEmulator->leds());
    for (int i = 0; i < MaxKeyboards; i++) {
        const int s = (m_LedNext + i) % MaxKeyboards;
        CUSBKeyboardDevice* kb = m_pKeyboard[s];
        if (kb == nullptr) continue;
        const int leds = m_KeyRemap[s].hidLeds(base);
        platform::LedSender& sender = m_Leds[s];
        // LED を送る転送は QEMU で 30ms ほどかかるので、その間は合成するコアが画面を写す
        const platform::LedSender::Result result = sender.update(leds, CTimer::GetClockTicks64(), [this, kb](int v) {
            bool ok = false;
            if (m_MenuActive)
                ok = kb->SetLEDs((u8)v) != FALSE;
            else
                WithComposerPresenting([&] { ok = kb->SetLEDs((u8)v) != FALSE; });
            return ok;
        });
        switch (result) {
        case platform::LedSender::SENT:
            m_Logger.Write(FromMachine, LogDebug, "keyboard %d LEDs: %02X", s + 1, (unsigned)leds);
            break;
        case platform::LedSender::FAILED:
            // 値ごとに、初めて失敗したときと、送るのをやめたときだけ出す
            if (sender.tries() == 1)
                m_Logger.Write(FromMachine, LogWarning, "keyboard %d LEDs: cannot set %02X; will retry", s + 1,
                               (unsigned)leds);
            else if (sender.tries() == platform::LedSender::MAX_TRIES)
                m_Logger.Write(FromMachine, LogWarning, "keyboard %d LEDs: gave up setting %02X", s + 1, (unsigned)leds);
            break;
        default:
            break;
        }
        if (result != platform::LedSender::IDLE) {
            m_LedNext = (s + 1) % MaxKeyboards;
            return;
        }
    }
}

// X68000 のマウスが動いたら (受け取った移動量の合計が変わったら) ログに出す。tools/qemu_human68k.sh が読む。
// 動かし続けるとログ (16KB のリングバッファ) が埋まるので、1 秒に 1 回までにする
void CKernel::ReportMouse() {
    u64 now = CTimer::GetClockTicks64();
    if (m_MouseShownAt != 0 && now - m_MouseShownAt < CLOCKHZ) return;
    uint32_t packets;
    int32_t dx, dy;
    m_pEmulator->mouseStats(packets, dx, dy);
    if (dx == m_MouseShownDx && dy == m_MouseShownDy) return;
    m_MouseShownDx = dx;
    m_MouseShownDy = dy;
    m_MouseShownAt = now;
    m_Logger.Write(FromMachine, LogNotice, "mouse: moved (%d,%d) in total, %u packets sent to the SCC", (int)dx, (int)dy,
                   (unsigned)packets);
}

// pix68k_sound=null のとき、前に呼んでから経った時間の分の音を取り出して捨てる (コア 0 の繰り返しから呼ぶ)
void CKernel::DrainNullAudio() {
    if (!m_NullAudio) return;
    static int16_t buf[2 * 256];
    // 始めた時刻と取り出した総数から数を決める (platform::SampleClock)。1 秒以上止まっていたら 1 秒分だけ
    u64 due = m_NullAudioClock.due(CTimer::GetClockTicks64(), pix68k_sound::RATE);
    while (due > 0) {
        const unsigned n = due > 256 ? 256 : (unsigned)due;
        m_pEmulator->audioSource()->readAudio(buf, (int)n);
        due -= n;
    }
}
