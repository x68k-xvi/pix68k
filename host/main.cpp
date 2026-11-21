// ホスト版 (Mac/Linux、SDL2) のエントリポイント。X68000 を実時間で動かし、画面を出し、キーボードをつなぐ。
//   pix68k [-r ROM のディレクトリ] [-0 FD0] [-1 FD1] [-h HDF] [--frames N] [その他]
//   -r DIR           IPLROM.DAT と CGROM.DAT の場所 (既定 ~/.cache/pix68k/rom)
//   -0 / -1 FILE     フロッピーディスクのイメージ (XDF / DIM / D88)。書き込めるが、既定ではファイルに書き戻さない
//   -h FILE          SASI のハードディスクのイメージ (HDF、256 バイト / レコード) を ID 0 につなぐ
//   --write-back     ディスク (FD、HD) に書き込まれていたら、終了時にイメージのファイルを書き換える
//   --read-only      ディスク (FD、HD) を書き込み禁止にする
//   --frames N       X68000 の N フレーム分動かしたら終わる (CI 用。SDL_VIDEODRIVER=dummy と組み合わせる)
//   --no-wait        実時間に合わせず、できるだけ速く動かす
//   --sram FILE      SRAM の保存先 (既定 ~/.cache/pix68k/sram.dat)。起動時に読み、書き換えられていたら終了時に書く
//   --no-sram        SRAM のファイルを読み書きしない
//   --screenshot F   終了時の画面を PPM で書き出す
//   --type TEXT      起動して --type-after フレーム (既定 900) たったら、TEXT をキーボードから打つ (\n は改行)
//   --no-audio       音を出さない (--no-wait のときも出さない)
//   --wav FILE       音を出す代わりに、48kHz の WAV に書く (--no-wait でも書ける)
//   --time N         RTC の時刻を、地方時の 1970-01-01 00:00:00 から N 秒の時刻から始める (ホストの時間帯は見ない。
//                    1700000000 はどこでも 2023-11-14 22:13:20)。既定はホストの今の地方時。
//                    ゲームによっては RTC の値で動きが変わるので、WAV や画面を前の版と比べるときは決めておく。
//                    RTC が表せるのは 1980〜2079 年なので、その外の時刻は端 (1980-01-01 か 2079-12-31 23:59:59) にする
//   --dump-text      終了時のテキスト画面を、読めた半角の字で書き出す (読めない字は ?。試験用)
//   --message TEXT   ウィンドウの左上に TEXT (UTF-8。\n は改行) を重ねて出す (ベアメタル版の POST /message と同じ。
//                    platform/osd_message.h)。1 行は半角 10 桁 (全角は 2 桁) で折り返し、4 行まで。入りきらなければ
//                    4 行目の終わりを「…」にする。--message-sec N 秒 (既定 10、0 なら出したまま) で消える
//   --message-dump F 試験用。--message の字の絵 (1 倍) を PPM で書き出す (CGROM の全角の字を確かめる)
//   --test-pattern   X68000 を動かさず、M0 のテストパターンを出す (キーを押すと色が変わる)
//   --clock 10|16    X68000 の CPU のクロック (MHz)。10 は初代〜SUPER、16 は XVI (既定)
//   --clock 50|75    --cpu 68060 のときの 68060 のクロック (MHz)。50 は実機の 060turbo (既定)、75 は実機の 060turbo を
//                    オーバークロックして使う構成。CPU に合わないクロックは使わない (警告を出して、その CPU の既定にする)
//   --cpu 68000|68030  68030 なら X68030 (68030 25MHz と 68882)。IPL ROM は IPLROM30.DAT を読み、
//                    SRAM の既定は ~/.cache/pix68k/sram30.dat になる
//   --ipl FILE       IPL ROM のファイル (既定は -r の場所の IPLROM.DAT / IPLROM30.DAT)
//   --high-mem MB    X68030 のハイメモリ ($01000000 から、最大 16MB)
//   --cpu 68060      060turbo (68060 50MHz、FPU は 68060 の内蔵)。IPL ROM は -r の場所の IPLROM060.DAT (68060 で動く
//                    IPL ROM。XEiJ の IPLROM 1.6 を書き出したものなど)、なければ IPLROM30.DAT (x68k/machine.cpp の
//                    patchIplFor060)。IPLROM060.DAT のときは、ROM30_060.DAT があれば $FC0000 からの ROM にする。
//                    SRAM の既定は sram060.dat
//   --local-mem MB   060turbo のローカルメモリ ($10000000 から、最大 256MB、既定 128MB)
//   --ram MB         本体のメモリ (1-12MB、既定 4MB)
//   --scsi-ex        外付け SCSI ボード (CZ-6BS1 相当) をつなぐ。ROM は -r の場所の SCSIEXROM.DAT
//   --no-scsi-ex     外付け SCSI ボードをつながない (設定ファイルの scsi_ex=on を打ち消す)
//   --scsi-ex-rom F  外付け SCSI ボードの ROM のファイル ($EA0000 からの 8192 バイトか $EA0020 からの 8160 バイト)。
//                    指定すると --scsi-ex も付けたことになる
//   --fpu-board [N]  数値演算プロセッサボード (CZ-6BP1 相当、MC68881) をつなぐ ($E9E000。--no-fpu-board で外す)。
//                    続けて 2 を書くと 2 枚目も $E9E080 につなぐ (1 なら 1 枚。--fpu-board だけと同じ)
//   --mercury        まーきゅりーゆにっと (16 ビットの PCM、$ECC080) をつなぐ (--no-mercury で外す)
//   --neptune        ネットワークボード Neptune-X (NE2000 互換、$ECE000) をつなぐ (--no-neptune で外す)
//   --neptune-link L フレームの送受信の先 (echo (既定)、loopback、pcap:IF、tap:NAME。host_ether.h)。--neptune も付けたことになる
//   --neptune-mac M  ボードの MAC アドレス (既定 02:68:6b:00:00:01)
//   --rs232c SPEC    RS-232C (SCC のチャネル A) の相手 (設定ファイルの rs232c=)。tcp:PORT で 127.0.0.1 の PORT に待ち受け、
//                    tcp:ADDR:PORT で ADDR に待ち受ける。off (既定) はつながない (host_serial.h)
//   --midi           MIDI ボード CZ-6BM1 (YM3802、$EAFA00、IRQ4) をつなぐ (--no-midi で外す)
//   --midi-sf2 FILE  MIDI ボードが送った MIDI を、この SoundFont (SF2) で鳴らして音に混ぜる。--midi も付けたことになる
//                    (指定しなければ、送った MIDI は捨てる)
//   --midi-voices N  SoundFont の音源の同時に鳴らすボイスの数 (4-256、既定 48)
//   --midi-out OUT   MIDI の音源: sf2 (既定。--midi-sf2 の SoundFont)、mt32 (--midi-mt32-ctrl と --midi-mt32-pcm の
//                    ROM で Munt を鳴らす)、sc55 (--midi-sc55 の ROM で Nuked-SC55 を鳴らす)、none (捨てる)。
//                    uart (ベアメタル版で UART から実機の音源に送るバイト列を、--midi-dump のファイルに書く)
//   --midi-mt32-ctrl FILE  MT-32 / CM-32L の CONTROL の ROM。--midi と --midi-out mt32 も付けたことになる
//   --midi-mt32-pcm FILE   MT-32 / CM-32L の PCM の ROM。--midi と --midi-out mt32 も付けたことになる
//   --midi-sc55 DIR  SC-55mkII の 5 つの ROM を置いたディレクトリ (platform/sc55_synth.h)。--midi と --midi-out sc55 も
//                    付けたことになる
//   --midi-dump FILE MIDI のバイト列をファイルに書く (試験用)。--midi-out uart なら、ベアメタル版が UART に送るバイト列
//                    (リセットのときに止めるバイトを差し込んだもの。platform/uart_midi.h)。それ以外なら、音源に渡したバイト列
//                    (音源が鳴らせなくても書く)
//   --midi-uart-family gs|mt32  --midi-out uart の先の音源 (設定ファイルの midi_uart_family=)。止めるバイトが変わる
//   --sc ID FILE     SCSI のハードディスクのイメージ (HDS、512 バイト / ブロック) を SCSI ID (0-6) につなぐ。
//                    外付け SCSI ボードがあればそちら、なければ内蔵 SCSI (X68030 と 060turbo) につなぐ
//   --cd FILE        CD-ROM のイメージ (ISO、2048 バイト / セクタ。または CUE シートと、そこに書かれた BIN / WAV) を
//                    SCSI の CD-ROM 装置としてつなぐ (読み出しだけ)。つなぐ先は --sc と同じ。ID は --cd-id (既定 6)。
//                    CUE シートのオーディオのトラックは、PLAY AUDIO のコマンドで再生できる (音は、X68000 の出力のフィルタを通さずに OPM / ADPCM の音に足す)。
//                    メニューの CD-ROM で、動かしたまま媒体を入れ替えられる (x68k/cd_drive.h)
//   --max-speed      CPU (68000 / 68030 / 68060) の命令を、実時間に間に合う範囲で一番速く動かす (x68k/max_speed.h。周辺機器と
//                    画面は実時間のまま)。終わるときに、そのときの速さ (既定のクロックの何 % か。060turbo では
//                    選んだクロック (--clock) の実機の 68060 の何 % か) を出す
//   --speed060 N     060turbo の命令の速さ (選んだクロックの実機の 68060 を 100 とした %。5 から 3200、既定 100)。100 で命令の
//                    サイクル数が MC68060UM の表どおりになる。小さくすると実機より遅くなり、ホストの負荷が下がる
//   --max-speed-limit N  060turbo で、--max-speed が上げる速さの上限 (同じ単位。既定の 0 は上限なし。100 で実機の速さまで)
//   --bus-sync-sprite  X68030 と 060turbo で、スプライト ($EB0000-$EBFFFF) の読み書きも、I/O と同じ 1.6 マイクロ秒ごとの
//                    区切りにそろえる (既定はそろえない。測っていない仮定の値。設定ファイルの bus_sync_sprite=on)
//   --sram-memsize fix|keep  fix (既定) なら起動のたびに、SRAM に書かれたメインメモリの容量をエミュレータの容量
//                    (--ram、既定 4MB) に書き換える。keep なら SRAM の値をそのまま使う (設定ファイルの sram_memsize=)
//   --joy-keys       カーソルキーと Z / X をジョイスティック 1 (上下左右とトリガ A / B) にする。
//                    これらのキーは X68000 のキーボードには送らない (既定では送る)
//   --config FILE    設定ファイル (ベアメタル版の pix68k.cfg と同じ形式と項目。platform/settings.h)。
//                    コマンドラインのほうが優先する。ファイル名の値は、/ で始まらなければ設定ファイルのディレクトリから。
//                    メニューの「Save settings」はこのファイルに書く (知らない行と注釈は残す)。
//                    省略すると ~/.cache/pix68k/pix68k.cfg を使う (なければ読まず、「Save settings」で作る)
//   --no-config      設定ファイルを使わない (~/.cache/pix68k/pix68k.cfg も読まない)
//   --profile NAME   起動の組み合わせ: --config のディレクトリの pix68k-NAME.cfg を、--config のファイルの共通の項目
//                    (ネットワーク、画面の出し方など) に重ねて読む (platform/boot_profile.h。ベアメタル版の起動時の
//                    メニューで選ぶのと同じ)。autotype= があれば --type / --type-after (autotype_wait= 秒を 55 フレーム / 秒で
//                    数える) にする。メニューの「Save settings」は、共通の項目を --config に、そのほかを pix68k-NAME.cfg に書く
//   --image-dir DIR  メニューでディスクのイメージを選ぶディレクトリ (既定は --config のディレクトリ、
//                    なければ ~/.cache/pix68k/disk)
//   --state-dir DIR  ステートのファイル (state0.p68 から state9.p68) の場所 (既定 ~/.cache/pix68k)
//   --press KEYS     試験用。--press-after 回 (既定 60) 回ってから、KEYS (コンマで区切ったキーの名前か usage の
//                    16 進: menu f12 up down left right enter esc home end pgup pgdn、例 menu,down,enter) を
//                    2 回に 1 つずつ押して離す。メニューを開いている間も数える
// 設定メニュー: F12 (か Scroll Lock、Application) で開く (platform/menu.h)。開いている間は X68000 を止める。
// ホスト版では F12 をメニューに使うので、X68000 の XF2 は F12 では打てない (JIS キーボードの無変換で打てる)。
// マウス: ウィンドウの中を左クリックすると、ホストのマウスを X68000 のマウスにする (カーソルは消え、
// ウィンドウから出なくなる)。中ボタンのクリックか、Ctrl と Alt (Mac は control と option) を同時に押すと戻る。
// ウィンドウからフォーカスが外れたときも戻る。
// ジョイスティック: SDL のゲームコントローラ (つないだ順に 1、2) の十字キーと左スティックを上下左右、
// A / X をトリガ A、B / Y をトリガ B にする。
// ウィンドウのタイトルに、キーボードの LED (CAPS、かな、INS など) とマウスの状態を出す。
// ROM がなければ、そのことを表示してテストパターンを出す (ROM のない CI でも host_smoke が通るように)。
#include <SDL.h>
#include <dirent.h>

#ifdef _WIN32
// Windows (MinGW) 用の置き換え
#include <io.h>
#include <stdlib.h>
static inline int fsync(int fd) { return _commit(fd); }
static inline char* realpath(const char* p, char* out) { return _fullpath(out, p, PATH_MAX); }
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "host_ether.h"
#include "host_serial.h"

#include "platform/crt_effect.h"
#include "platform/boot_profile.h"
#include "platform/cue_sheet.h"
#include "platform/menu.h"
#include "platform/osd.h"
#include "platform/osd_message.h"
#include "platform/platform.h"
#include "platform/setting_warning.h"
#include "platform/settings_file.h"
#include "platform/mt32_synth.h"
#include "platform/sc55_synth.h"
#include "platform/sf2_synth.h"
#include "platform/uart_midi.h"
#include "platform/time_arg.h"
#include "platform/wav_file.h"
#include "platform/test_pattern.h"
#include "x68k/machine.h"
#include "x68k/max_speed.h"
#include "x68k/render.h"
#include "x68k/sram_file.h"
#include "x68k/state.h"
#include "x68k/textscreen.h"

namespace {

// ホスト版の設定の既定の値。video_scale はウィンドウに合わせて 4:3 を保って最大 (aspect。ウィンドウが小さくても切らない)。
// ベアメタル版は整数倍 (integer) のまま。v0.1.0 の scale= と scanline= は、v0.1.0 のホスト版と同じく読まない
// (ベアメタル版の項目として残す。保存でも書き直さない)
platform::Settings hostDefaultSettings() {
    platform::Settings d;
    d.videoScale = platform::VIDEO_SCALE_ASPECT;
    d.legacyVideo = 0;
    return d;
}

class SdlVideo : public platform::Video {
public:
    bool init(int w, int h) {
        window_ = SDL_CreateWindow("pix68k", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window_) return false;
        // 速さは X68000 の時間に合わせるので、ホストの垂直同期 (60Hz) は待たない
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer_) return false;
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                     x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_HEIGHT);
        return texture_ != nullptr;
    }
    void setTitle(const char* title) {
        if (window_) SDL_SetWindowTitle(window_, title);
    }
    // 画面の隅の数字 (osd=fps。platform/osd.h)。valid() でなければ出さない。次の present() から出す
    // overlay が false (osd=on) なら、X68000 の画面に重なるときは出さない
    void setOsd(const platform::OsdValue& packed, bool overlay = true) {
        osd_ = packed;
        osdOverlay_ = overlay;
    }
    // 左上のメッセージ (platform/osd_message.h)。g が nullptr なら出さない。gen が変わったら描き直す。次の present() から出す
    void setMessage(const platform::MessageGlyphs* g, uint32_t gen) {
        msg_ = g;
        msgGen_ = gen;
    }
    // 画面の出し方 (video_scale= の VideoScale と、video_filter= の CrtPreset のビット)。次の present() から使う
    // モニタの枠 (cz-600de、cz-600db) はホスト版では使わない (起動するときにログで知らせる)
    void setVideo(int scale, int filter) {
        scale_ = scale;
        crt_ = filter & platform::CRT_EFFECT_BITS;
    }
    ~SdlVideo() override {
        if (crtTexture_) SDL_DestroyTexture(crtTexture_);
        if (msgTexture_) SDL_DestroyTexture(msgTexture_);
        if (osdTexture_) SDL_DestroyTexture(osdTexture_);
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
    }
    // X68000 の画面をウィンドウに描く。置き方はベアメタル版と同じ (platform::videoWindowFrame())。
    // 描くのはウィンドウの画素の座標で (SDL_RenderSetLogicalSize() は使わない)
    void present(const uint32_t* pixels, int width, int height, int pitch) override {
        if (width <= 0 || height <= 0) return;
        if (width > x68k::Renderer::MAX_WIDTH) width = x68k::Renderer::MAX_WIDTH;
        if (height > x68k::Renderer::MAX_HEIGHT) height = x68k::Renderer::MAX_HEIGHT;
        width_ = width;
        height_ = height;
        SDL_RenderClear(renderer_);
        int ow = 0, oh = 0;
        if (SDL_GetRendererOutputSize(renderer_, &ow, &oh) != 0 || ow <= 0 || oh <= 0) ow = oh = 0;
        frame_ = platform::videoWindowFrame(scale_, ow, oh, width, height);
        const platform::FrameScale& fs = frame_.s;
        if (fs.dstW > 0 && !presentCrt(pixels, pitch)) {
            // 効果がなければ SDL に拡大させる (整数倍でない枠も、いちばん近い画素を取る)
            SDL_Rect r = {0, 0, width, height};
            SDL_UpdateTexture(texture_, &r, pixels, pitch * 4);
            const SDL_Rect src = {fs.srcX, fs.srcY, fs.srcW, fs.srcH};
            const SDL_Rect dst = {fs.dstX, fs.dstY, fs.dstW, fs.dstH};
            SDL_RenderCopy(renderer_, texture_, &src, &dst);
        }
        osdRect_ = platform::OsdRect();
        if (osd_.valid()) drawOsd();
        if (msg_ != nullptr && msg_->lines != 0) drawMessage();
        SDL_RenderPresent(renderer_);
    }

private:
    // 効果 (video_filter=) を掛けた絵を、表 (frame_.cw x frame_.ch) の大きさで CPU で作り、SDL に絵の大きさまで拡大させて
    // 描く (ベアメタル版と同じ platform/crt_effect.h)。掛けなかったら (効果がない、表を作れない) false を返す。
    // X68000 の画面 (pixels) は変えない (--screenshot とステートには入らない)。右上の数字と左上のメッセージはこのあとに
    // 描くので、ゆがまない
    bool presentCrt(const uint32_t* pixels, int pitch) {
        if (crt_ <= platform::CRT_OFF || crt_ >= platform::CRT_PRESET_COUNT) return false;
        const platform::FrameScale& fs = frame_.s;
        const int cw = frame_.cw, ch = frame_.ch;
        if (cw == 0) return false;
        // 表の大きさで描くか (1 画素が 1 画素)、SDL に拡大させるか
        const bool scaled = cw != fs.dstW || ch != fs.dstH;
        if (!crtMap_.tried(crt_, fs.srcW, fs.srcH, cw, ch)) {
            if (cw > crtMap_.maxW || ch > crtMap_.maxH) {
                crtMem_.assign(platform::crtMapBytes(cw, ch) / 4 + 1, 0);
                platform::crtMapInit(crtMap_, crtMem_.data(), cw, ch);
                crtLine_[0].assign((size_t)cw + 4, 0);
                crtLine_[1].assign((size_t)cw + 4, 0);
            }
            platform::crtBuildMap(crtMap_, crt_, fs.srcW, fs.srcH, cw, ch);
        }
        if (!crtMap_.valid) return false;
        if (!crtTexture_ || crtTexW_ != cw || crtTexH_ != ch) {
            if (crtTexture_) SDL_DestroyTexture(crtTexture_);
            crtTexture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, cw, ch);
            if (!crtTexture_) return false;
            crtTexW_ = cw;
            crtTexH_ = ch;
        }
        // 1 画素が 1 画素に出るときは混ぜない (アパーチャーグリルの縦じまをそのまま出す)
        SDL_SetTextureScaleMode(crtTexture_, scaled ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        void* tex = nullptr;
        int texPitch = 0;
        if (SDL_LockTexture(crtTexture_, nullptr, &tex, &texPitch) != 0) return false;
        const uint32_t* src = pixels + (size_t)fs.srcY * pitch + fs.srcX;
        for (int y = 0; y < ch; y++) {
            const uint32_t* line = platform::crtRenderRow(crtMap_, src, pitch, y, crtLine_[0].data(), crtLine_[1].data());
            memcpy((uint8_t*)tex + (size_t)y * texPitch, line, (size_t)cw * 4);
        }
        SDL_UnlockTexture(crtTexture_);
        const SDL_Rect dst = {fs.dstX, fs.dstY, fs.dstW, fs.dstH};
        SDL_RenderCopy(renderer_, crtTexture_, nullptr, &dst);
        return true;
    }

    // 画面の隅の数字を、ウィンドウの右上に描く (X68000 の画面の外の余白に入ればそこ、入らなければ重ねる)。
    // 大きさはウィンドウの画素で決める (論理的な大きさで描くと、256 ラインの画面では大きくなりすぎる)。
    // 文字は数字が変わったときだけ小さなテクスチャに描き直す。X68000 の画面 (screen) には描かない
    // (--screenshot とメニューの下の画面に入らないように)
    void drawOsd() {
        int ow = 0, oh = 0;
        if (SDL_GetRendererOutputSize(renderer_, &ow, &oh) != 0 || ow <= 0 || oh <= 0) return;
        // 真ん中に置いた X68000 の画面の位置 (video_scale= の枠。present())
        const platform::FrameScale& fs = frame_.s;
        const int scale = oh / 512 < 1 ? 1 : oh / 512 > 4 ? 4 : oh / 512;
        // 横 1 行の文字数は出す項目で決める (ホスト版は THR とファンを出さないので OSD_CHARS_BASE)。
        // テクスチャの幅は OSD_CHARS なので、それより長くしない (ホスト版は IP アドレスを入れないが、念のため)
        const int n = platform::osdChars(osd_);
        const int chars = n < platform::OSD_CHARS ? n : platform::OSD_CHARS;
        const platform::OsdRect rect = platform::placeOsd(ow, oh, fs.dstX, fs.dstY, fs.dstW, fs.dstH, scale, chars);
        if (rect.w <= 0 || (rect.overlay && !osdOverlay_)) return;
        osdRect_ = rect;
        const int tw = platform::OSD_CHARS * 8, th = 16;
        if (!osdTexture_) {
            osdTexture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, tw, th);
            if (!osdTexture_) return;
            SDL_SetTextureScaleMode(osdTexture_, SDL_ScaleModeNearest);
            osdDrawn_ = platform::OsdValue();
        }
        if (osd_ != osdDrawn_) {
            char text[platform::OSD_CHARS + 1];
            platform::formatOsd(text, osd_);
            uint32_t pixels[16][platform::OSD_CHARS * 8];
            for (int y = 0; y < th; y++) platform::renderOsdRow(pixels[y], text, y, 1);
            SDL_UpdateTexture(osdTexture_, nullptr, pixels, tw * 4);
            osdDrawn_ = osd_;
        }
        const SDL_Rect dst = {rect.x, rect.y, rect.w, rect.h};
        // 文字は右寄せなので、テクスチャの右の chars 文字分を写す
        const SDL_Rect src = {tw - chars * 8, 0, chars * 8, th};
        SDL_RenderCopy(renderer_, osdTexture_, &src, &dst);
    }

    // 左上のメッセージを、ウィンドウの左上に描く (置き方はベアメタル版と同じ platform::placeMessage()。右上の数字と
    // 同じ行に並ぶときは、数字にかからないよう右を切る)。字は 1 倍でテクスチャに描き、倍率は SDL が拡大する
    void drawMessage() {
        int ow = 0, oh = 0;
        if (SDL_GetRendererOutputSize(renderer_, &ow, &oh) != 0 || ow <= 0 || oh <= 0) return;
        const platform::FrameScale& fs = frame_.s;
        const int scale = oh / 512 < 1 ? 1 : oh / 512;
        const int limitX = platform::messageLimitX(ow, oh, osdRect_, *msg_, scale);
        const platform::OsdRect r = platform::placeMessage(ow, oh, fs.dstX, fs.dstY, fs.dstW, fs.dstH, *msg_, scale, limitX);
        if (r.w <= 0) return;
        const int tw = platform::MSG_COLS * 8, th = platform::MSG_LINES * 16;
        if (!msgTexture_) {
            msgTexture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, tw, th);
            if (!msgTexture_) return;
            SDL_SetTextureScaleMode(msgTexture_, SDL_ScaleModeNearest);
            msgDrawn_ = !msgGen_;
        }
        if (msgGen_ != msgDrawn_) {
            static uint32_t pixels[platform::MSG_LINES * 16 * platform::MSG_COLS * 8];
            platform::OsdRect full;
            full.w = tw;
            full.h = th;
            platform::renderMessage(pixels, full, *msg_);
            SDL_UpdateTexture(msgTexture_, nullptr, pixels, tw * 4);
            msgDrawn_ = msgGen_;
        }
        const SDL_Rect src = {0, 0, r.w / r.scale, r.h / r.scale};
        const SDL_Rect dst = {r.x, r.y, r.w, r.h};
        SDL_RenderCopy(renderer_, msgTexture_, &src, &dst);
    }

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_Texture* osdTexture_ = nullptr;
    SDL_Texture* msgTexture_ = nullptr;
    // 画面の出し方 (setVideo())
    int scale_ = platform::VIDEO_SCALE_ASPECT;
    int crt_ = 0;
    platform::CrtWindowFrame frame_; // 最後の present() の置き方
    SDL_Texture* crtTexture_ = nullptr;
    int crtTexW_ = 0, crtTexH_ = 0;
    platform::CrtMap crtMap_;
    std::vector<uint32_t> crtMem_, crtLine_[2];
    platform::OsdRect osdRect_; // 最後に描いた右上の数字の場所 (描かなければ w = 0)
    const platform::MessageGlyphs* msg_ = nullptr;
    uint32_t msgGen_ = 0, msgDrawn_ = 0;
    platform::OsdValue osd_, osdDrawn_;
    bool osdOverlay_ = true;
    int width_ = 0, height_ = 0;
};

class KeyLogger : public platform::InputSink {
public:
    uint8_t lastKey = 0;
    void onKey(uint8_t usage, bool down) override {
        if (down) lastKey = usage;
        printf("key %02X %s\n", usage, down ? "down" : "up");
    }
};

// SDL のイベントを処理する。キーは input に渡す。終了の要求があれば true
bool pollEvents(platform::InputSink& input) {
    bool quit = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = true;
        // SDL の scancode は USB HID の usage ID と同じ値 (4 = A ... 0xE7 = 右 GUI)。
        // OS のキーリピート (e.key.repeat) は捨てる。リピートは X68000 のキーボードがする
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat && e.key.keysym.scancode < 256)
            input.onKey((uint8_t)e.key.keysym.scancode, e.type == SDL_KEYDOWN);
    }
    return quit;
}

// X68000 を動かすときの入力: キー、マウス (捕まえている間だけ)、ゲームコントローラ、ジョイスティックにするキー。
// 設定メニュー (menu) を開いている間は、キーはメニューに渡し、X68000 には何も送らない
class HostInput {
public:
    using Sink = platform::InputSink;
    HostInput(Sink& sink, bool joyKeys, platform::Menu* menu = nullptr, std::function<void()> onOpen = nullptr)
        : sink_(sink), joyKeys_(joyKeys), menu_(menu), onOpen_(onOpen) {
        const char* driver = SDL_GetCurrentVideoDriver();
        // 画面のないドライバ (CI の dummy) では、マウスを捕まえない
        canCapture_ = !(driver && !strcmp(driver, "dummy"));
    }
    ~HostInput() {
        for (int i = 0; i < 2; i++)
            if (pads_[i]) SDL_GameControllerClose(pads_[i]);
    }
    HostInput(const HostInput&) = delete;
    HostInput& operator=(const HostInput&) = delete;
    bool captured() const { return captured_; }

    // イベントを処理する。終了の要求があれば true
    bool poll() {
        bool quit = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (e.key.keysym.scancode < 256)
                    key((uint8_t)e.key.keysym.scancode, e.type == SDL_KEYDOWN, e.key.repeat != 0);
                break;
            case SDL_MOUSEMOTION:
                if (captured_ && (e.motion.xrel || e.motion.yrel)) sink_.onMouse(e.motion.xrel, e.motion.yrel, buttons_);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: mouseButton(e.button); break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) setCapture(false);
                break;
            case SDL_CONTROLLERDEVICEADDED: openPad(e.cdevice.which); break;
            case SDL_CONTROLLERDEVICEREMOVED: closePad(e.cdevice.which); break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
            case SDL_CONTROLLERAXISMOTION: updateJoysticks(); break;
            default: break;
            }
        }
        return quit;
    }

    // キーを押した / 離した (SDL の scancode は USB HID の usage ID と同じ値。--press からも呼ぶ)
    void key(uint8_t sc, bool down, bool repeat = false) {
        if (menuOpen()) {
            // メニューには押したときと、OS のキーリピートを渡す (↑↓ を押し続けて動かせるように)
            if (down) menu_->key(sc);
            return;
        }
        // OS のキーリピートは捨てる。リピートは X68000 のキーボードがする
        if (repeat) return;
        if (down && menu_ && platform::isMenuKey(sc, true)) {
            openMenu();
            return;
        }
        // X68000 に押したと送っていないキーを離した (メニューを開いている間に押したキー、メニューのキー) なら捨てる
        if (!down && !sent(sc)) return;
        if (joyKeys_) {
            uint8_t bit = 0;
            switch (sc) {
            case SDL_SCANCODE_UP: bit = Sink::JOY_UP; break;
            case SDL_SCANCODE_DOWN: bit = Sink::JOY_DOWN; break;
            case SDL_SCANCODE_LEFT: bit = Sink::JOY_LEFT; break;
            case SDL_SCANCODE_RIGHT: bit = Sink::JOY_RIGHT; break;
            case SDL_SCANCODE_Z: bit = Sink::JOY_A; break;
            case SDL_SCANCODE_X: bit = Sink::JOY_B; break;
            default: break;
            }
            if (bit) {
                if (down) keyJoy_ |= bit;
                else keyJoy_ &= (uint8_t)~bit;
                setSent(sc, down);
                updateJoysticks();
                return;
            }
        }
        sink_.onKey(sc, down);
        setSent(sc, down);
        // Ctrl と Alt を同時に押したら、マウスを放す
        if (captured_ && down) {
            SDL_Keymod mod = SDL_GetModState();
            if ((mod & KMOD_CTRL) && (mod & KMOD_ALT)) setCapture(false);
        }
    }

    // メニューを閉じたあとに呼ぶ (ゲームコントローラの今の状態を X68000 に送り直す)
    void refresh() { updateJoysticks(); }

private:
    bool menuOpen() const { return menu_ && menu_->isOpen(); }

    // メニューを開く: 押しているキー、マウスのボタン、ジョイスティックをすべて離したことにして、マウスを放す
    void openMenu() {
        for (int u = 0; u < 256; u++)
            if (sent((uint8_t)u)) {
                setSent((uint8_t)u, false);
                if (!(joyKeys_ && joyKeyBit((uint8_t)u))) sink_.onKey((uint8_t)u, false);
            }
        keyJoy_ = 0;
        setCapture(false);
        if (onOpen_) onOpen_();
        updateJoysticks();
    }
    static bool joyKeyBit(uint8_t sc) {
        return sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT ||
               sc == SDL_SCANCODE_Z || sc == SDL_SCANCODE_X;
    }
    bool sent(uint8_t u) const { return (sent_[u >> 3] >> (u & 7)) & 1; }
    void setSent(uint8_t u, bool on) {
        if (on) sent_[u >> 3] |= (uint8_t)(1u << (u & 7));
        else sent_[u >> 3] &= (uint8_t)~(1u << (u & 7));
    }

    void mouseButton(const SDL_MouseButtonEvent& b) {
        if (menuOpen()) return;
        bool down = b.type == SDL_MOUSEBUTTONDOWN;
        if (b.button == SDL_BUTTON_MIDDLE) {
            if (down) setCapture(!captured_);
            return;
        }
        if (!captured_) {
            // 捕まえていないときの左クリックは、捕まえるだけで X68000 には送らない
            if (down && b.button == SDL_BUTTON_LEFT) setCapture(true);
            return;
        }
        uint8_t bit = b.button == SDL_BUTTON_LEFT ? Sink::MOUSE_LEFT : b.button == SDL_BUTTON_RIGHT ? Sink::MOUSE_RIGHT : 0;
        if (!bit) return;
        if (down) buttons_ |= bit;
        else buttons_ &= (uint8_t)~bit;
        sink_.onMouse(0, 0, buttons_);
    }

    void setCapture(bool on) {
        if (on && !canCapture_) return;
        if (on == captured_) return;
        if (SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) != 0) {
            fprintf(stderr, "cannot capture the mouse: %s\n", SDL_GetError());
            return;
        }
        captured_ = on;
        if (!on && buttons_) {
            buttons_ = 0;
            sink_.onMouse(0, 0, 0);
        }
    }

    void openPad(int index) {
        if (!SDL_IsGameController(index)) return;
        for (int i = 0; i < 2; i++) {
            if (pads_[i]) continue;
            pads_[i] = SDL_GameControllerOpen(index);
            if (pads_[i]) printf("joystick %d: %s\n", i + 1, SDL_GameControllerName(pads_[i]));
            updateJoysticks();
            return;
        }
    }

    void closePad(SDL_JoystickID id) {
        for (int i = 0; i < 2; i++) {
            if (!pads_[i] || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads_[i])) != id) continue;
            SDL_GameControllerClose(pads_[i]);
            pads_[i] = nullptr;
            printf("joystick %d removed\n", i + 1);
        }
        updateJoysticks();
    }

    static uint8_t padBits(SDL_GameController* p) {
        if (!p) return 0;
        const int dead = 12000; // 左スティックの遊び (最大 32767)
        auto btn = [p](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(p, b) != 0; };
        int ax = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
        uint8_t v = 0;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -dead) v |= Sink::JOY_UP;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > dead) v |= Sink::JOY_DOWN;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -dead) v |= Sink::JOY_LEFT;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > dead) v |= Sink::JOY_RIGHT;
        if (btn(SDL_CONTROLLER_BUTTON_A) || btn(SDL_CONTROLLER_BUTTON_X)) v |= Sink::JOY_A;
        if (btn(SDL_CONTROLLER_BUTTON_B) || btn(SDL_CONTROLLER_BUTTON_Y)) v |= Sink::JOY_B;
        return v;
    }

    // メニューを開いている間は、ジョイスティックを離したことにする
    void updateJoysticks() {
        for (int i = 0; i < 2; i++) {
            uint8_t v = menuOpen() ? 0 : padBits(pads_[i]);
            if (i == 0 && !menuOpen()) v |= keyJoy_;
            if (v != joy_[i]) {
                joy_[i] = v;
                sink_.onJoystick(i, v);
            }
        }
    }

    Sink& sink_;
    bool joyKeys_;
    platform::Menu* menu_;
    std::function<void()> onOpen_;
    bool canCapture_ = true;
    bool captured_ = false;
    uint8_t buttons_ = 0;
    uint8_t keyJoy_ = 0;
    uint8_t joy_[2] = {0, 0};
    uint8_t sent_[32] = {}; // X68000 に押したと送ったキー (ジョイスティックにしたキーを含む)
    SDL_GameController* pads_[2] = {nullptr, nullptr};
};

// ウィンドウのタイトル: キーボードの LED (点灯しているもの) とマウスの状態
void makeTitle(uint8_t leds, bool captured, char* out, size_t size) {
    static const struct {
        uint8_t bit;
        const char* name;
    } kLeds[] = {
        {x68k::Keyboard::LED_KANA, "かな"},       {x68k::Keyboard::LED_ROMAJI, "ローマ字"},
        {x68k::Keyboard::LED_CODE, "コード入力"}, {x68k::Keyboard::LED_CAPS, "CAPS"},
        {x68k::Keyboard::LED_INS, "INS"},         {x68k::Keyboard::LED_HIRAGANA, "ひらがな"},
        {x68k::Keyboard::LED_ZENKAKU, "全角"},
    };
    int n = snprintf(out, size, "pix68k");
    for (const auto& l : kLeds)
        if (!(leds & l.bit) && n >= 0 && (size_t)n < size) n += snprintf(out + n, size - (size_t)n, " [%s]", l.name);
    if (n >= 0 && (size_t)n < size)
        snprintf(out + n, size - (size_t)n, "%s",
                 captured ? " - mouse captured (middle click or Ctrl+Alt to release)" : " - click to use the mouse");
}

int runTestPattern(long maxFrames) {
    static platform::TestPattern pattern;
    SdlVideo video;
    KeyLogger input;
    if (!video.init(platform::TestPattern::kWidth, platform::TestPattern::kHeight)) {
        fprintf(stderr, "video init: %s\n", SDL_GetError());
        return 1;
    }
    bool quit = false;
    for (long frame = 0; !quit && (maxFrames < 0 || frame < maxFrames); frame++) {
        quit = pollEvents(input);
        uint32_t t0 = SDL_GetTicks();
        pattern.draw(input.lastKey, (int)frame);
        video.present(pattern.pixels, platform::TestPattern::kWidth, platform::TestPattern::kHeight,
                      platform::TestPattern::kWidth);
        uint32_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay(16 - dt);
    }
    printf("frames done\n");
    return 0;
}

// ---------------------------------------------------------------- ファイル

bool loadFile(const char* path, uint8_t* buf, size_t size) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, size, f);
    int extra = fgetc(f);
    fclose(f);
    return n == size && extra == EOF;
}

uint8_t* loadWhole(const char* path, size_t& size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = n > 0 ? (uint8_t*)malloc((size_t)n) : nullptr;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = nullptr;
    }
    fclose(f);
    size = buf ? (size_t)n : 0;
    return buf;
}

// MT-32 の ROM を new[] で確保したバッファに読む (Mt32Synth::load() が引き取る)。大きさが ROM のものでなければ
// 読まずに大きさだけを返す
uint8_t* loadRom(const char* path, size_t& size, bool (*sizeOk)(uint64_t)) {
    size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = nullptr;
    if (n > 0 && sizeOk((uint64_t)n)) {
        buf = new uint8_t[(size_t)n];
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
            delete[] buf;
            buf = nullptr;
        }
    }
    fclose(f);
    size = n > 0 ? (size_t)n : 0;
    return buf;
}

// 書いている途中で止まっても元のファイルが壊れないよう、同じディレクトリの一時ファイルに書いて
// fsync してから rename で置き換え、ディレクトリも fsync する
bool saveFile(const char* path, const uint8_t* buf, size_t size) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t done = 0; ok && done < size;) {
        ssize_t n = write(fd, buf + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else done += (size_t)n;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) {
        int e = errno;
        unlink(tmp);
        errno = e;
        return false;
    }
    char dir[1200];
    snprintf(dir, sizeof dir, "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else snprintf(dir, sizeof dir, ".");
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return true;
}

// path の親ディレクトリを (なければ上から順に) 作る
void makeParentDir(const char* path) {
    char dir[1200];
    snprintf(dir, sizeof dir, "%s", path);
    char* slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = 0;
    for (char* p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
#ifdef _WIN32
        mkdir(dir);
#else
        mkdir(dir, 0755);
#endif
        *p = '/';
    }
#ifdef _WIN32
    mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

void writePpm(const uint32_t* pixels, int pitch, int w, int h, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = pixels[y * pitch + x];
            uint8_t rgb[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

// コマンドラインの \n を改行にする
void unescape(const char* in, char* out, size_t size) {
    size_t n = 0;
    for (; *in && n + 1 < size; in++) {
        if (in[0] == '\\' && in[1] == 'n') {
            out[n++] = '\n';
            in++;
        } else {
            out[n++] = *in;
        }
    }
    out[n] = 0;
}

// 今の地方時を「1970 年からの秒数」で返す (Machine::setTime() は地方時を受け取る)
int64_t localSeconds() {
    time_t now = time(nullptr);
#ifdef _WIN32
    struct tm lt, gt;
    localtime_s(&lt, &now);
    gmtime_s(&gt, &now);
    lt.tm_isdst = 0; gt.tm_isdst = 0;
    return (int64_t)now + (int64_t)difftime(mktime(&lt), mktime(&gt));
#else
    struct tm lt;
    localtime_r(&now, &lt);
    return (int64_t)now + lt.tm_gmtoff;
#endif
}

struct Options {
    char romDir[1024];
    bool readOnly = false;
    bool writeBack = false;
    long maxFrames = -1;
    bool noWait = false;
    char sram[1024];
    bool useSram = true;
    const char* screenshot = nullptr;
    char typeText[1024] = {0};
    char messageText[1024] = {0}; // --message
    long messageSec = platform::MSG_SEC_DEFAULT;
    const char* messageDump = nullptr; // --message-dump
    long typeAfter = 900;
    bool testPattern = false;
    bool noAudio = false;
    const char* wav = nullptr;
    int64_t startTime = -1; // --time (-1 ならホストの今の地方時)
    bool dumpText = false;  // --dump-text
    bool joyKeys = false;
    int clockMHz = 16;
    int cpu = 68000;
    const char* ipl = nullptr;
    int highMemMB = 0;
    int localMemMB = 128;
    int ramMB = 4;
    bool maxSpeed = false;
    int speed060 = 100;    // --speed060 (060turbo の命令の速さ。選んだクロックの実機の 68060 を 100 とした %)
    int maxSpeedLimit = 0; // --max-speed-limit (060turbo の最高速モードの上限。0 は上限なし)
    bool busSyncSprite = false; // --bus-sync-sprite (スプライトの読み書きも 1.6 マイクロ秒ごとにそろえる)
    int osd = 2;                // osd= (画面の隅の数字。メニューで変えるとすぐに反映する)
    int videoScale = platform::VIDEO_SCALE_ASPECT; // video_scale= (メニューで変えるとすぐに反映する。既定は hostDefaultSettings())
    int videoFilter = 0;                            // video_filter= (同じ)
    bool sramGiven = false;
    bool fixSramMemorySize = true; // --sram-memsize (false なら keep)
    char config[1024] = {0};    // --config (空ならなし。省略時は ~/.cache/pix68k/pix68k.cfg)
    bool configDefault = false; // config は --config ではなく既定のパス
    char profile[64] = {0};     // --profile (空ならなし)
    char profilePath[1024] = {0}; // --profile のファイル (--config のディレクトリの pix68k-NAME.cfg)
    char imageDir[1024] = {0};  // --image-dir
    char stateDir[1024] = {0};  // --state-dir
    char press[1024] = {0};     // --press
    long pressAfter = 60;
    // 設定ファイルを読んだときの値 (メニューの「Save settings」は、これと違う項目だけを書く)
    platform::Settings fileSettings = hostDefaultSettings();
    // ディスクのイメージの名前 (-0 / -1 / -h か設定ファイル。メニューで替えると書き換える)
    char diskPath[2][1024] = {{0}, {0}};
    char hdfPath[1024] = {0};
    bool scsiEx = false;          // 外付け SCSI ボード
    int fpuBoard = 0;             // 数値演算プロセッサボードの枚数 (0..2)
    bool mercury = false;         // まーきゅりーゆにっと
    bool neptune = false;         // Neptune-X
    char neptuneMac[platform::Settings::PATH_SIZE] = {0};  // その MAC アドレス (空なら既定)
    char neptuneLink[platform::Settings::PATH_SIZE] = {0}; // フレームの送受信の先 (空なら echo)
    char rs232c[platform::Settings::PATH_SIZE] = {0};      // RS-232C の相手 (空ならつながない)
    bool midi = false;            // MIDI ボード
    char midiSf2[1024] = {0};     // その音源の SoundFont (空なら鳴らさない)
    int midiVoices = 48;
    int midiOut = 0;              // MIDI の音源 (platform::Settings::midiOut と同じ値。0 sf2、1 uart、2 none、3 mt32)
    char midiMt32Ctrl[1024] = {0}; // MT-32 の音源の ROM
    char midiMt32Pcm[1024] = {0};
    char midiSc55[1024] = {0};     // SC-55mkII の ROM のディレクトリ
    char midiDump[1024] = {0};     // --midi-dump のファイル (空なら書かない)
    int midiUartFamily = 0;        // --midi-out uart の先の音源 (0 gs、1 mt32)
    char scsiExRom[1024] = {0};   // その ROM (空なら romDir/SCSIEXROM.DAT)
    char sc[8][1024] = {};        // SCSI ID ごとのハードディスクのイメージ
    char cd[1024] = {0};          // CD-ROM のイメージ
    int cdId = 6;
};

// ---------------------------------------------------------------- 音

// SDL の音のコールバック (SDL の音のスレッド) から、Machine のリングバッファの音を取り出す
void audioCallback(void* userdata, Uint8* stream, int len) {
    platform::AudioSource* src = (platform::AudioSource*)userdata;
    src->readAudio((int16_t*)stream, len / 4);
}

// 音の出力を開く。開けなければ 0 (音なしで続ける)
SDL_AudioDeviceID openAudio(x68k::Machine& m) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "audio: %s; continuing without sound\n", SDL_GetError());
        return 0;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512; // 約 10ms ずつ取り出す
    want.callback = audioCallback;
    want.userdata = static_cast<platform::AudioSource*>(&m.sound().ring());
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (dev == 0) {
        fprintf(stderr, "audio: %s; continuing without sound\n", SDL_GetError());
        return 0;
    }
    m.sound().setOutputRate(have.freq);
    // リングバッファのたまり具合の最小値が 20ms 分になるよう、作る速さを合わせる (x68k/sound.h)
    m.sound().setLatency((unsigned)have.freq * 20 / 1000);
    SDL_PauseAudioDevice(dev, 0);
    return dev;
}

// ---------------------------------------------------------------- X68000

struct Disk {
    x68k::FloppyImage image;
    uint8_t* data = nullptr;
    size_t size = 0;
    char path[1024] = {0};
};

// メニューを開いている間の画面の大きさ (X68000 の画面をこの大きさに引き伸ばしてメニューを重ねる)
const int kMenuWidth = 768, kMenuHeight = 512;
// runMachine() の戻り値: メニューの「Restart」で、新しい設定で作り直す
const int kRestart = 3;

bool loadDisk(Disk& d, const char* path, bool readOnly) {
    d.data = loadWhole(path, d.size);
    if (!d.data || !d.image.load(d.data, d.size, readOnly, path)) {
        free(d.data);
        d.data = nullptr;
        return false;
    }
    snprintf(d.path, sizeof d.path, "%s", path);
    return true;
}

// ドライブから取り出す。書き込まれていて --write-back なら、イメージのファイルに書き戻す
void unloadDisk(x68k::Machine& m, int drive, Disk& d, bool writeBack) {
    if (!d.data) return;
    if (d.image.dirty()) {
        if (!writeBack) printf("disk %s was written, but not saved (use --write-back)\n", d.path);
        else if (saveFile(d.path, d.data, d.size)) printf("disk saved: %s\n", d.path);
        else perror(d.path);
    }
    m.ejectDisk(drive);
    d.image.unload();
    free(d.data);
    d.data = nullptr;
    d.path[0] = 0;
}

// path の最後の / までを dir に入れる (なければ ".")
void dirName(const char* path, char* dir, size_t size) {
    snprintf(dir, size, "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else snprintf(dir, size, ".");
}

// 設定ファイルの値のファイル名: / で始まらなければ、設定ファイルのあるディレクトリから
void configPath(const char* cfg, const char* value, char* out, size_t size) {
    if (value[0] == 0 || value[0] == '/' || !cfg[0]) {
        snprintf(out, size, "%s", value);
        return;
    }
    char dir[1024];
    dirName(cfg, dir, sizeof dir);
    snprintf(out, size, "%s/%s", dir, value);
}

// 設定 (pix68k.cfg の項目) を Options に写す
void applySettings(const platform::Settings& s, Options& opt) {
    for (int d = 0; d < 2; d++) configPath(opt.config, s.fd[d], opt.diskPath[d], sizeof opt.diskPath[d]);
    configPath(opt.config, s.hd0, opt.hdfPath, sizeof opt.hdfPath);
    opt.cpu = s.cpu;
    opt.clockMHz = s.cpu == 68030 ? 16 : s.effectiveClockMHz(); // 68060 では 50 か 75
    opt.ramMB = s.ramMB;
    opt.highMemMB = s.highMemMB;
    opt.localMemMB = s.localMemMB >= 0 ? s.localMemMB : 128;
    opt.maxSpeed = s.maxSpeed != 0;
    opt.speed060 = s.speed060;
    opt.maxSpeedLimit = s.maxSpeedLimit;
    opt.busSyncSprite = s.busSyncSprite != 0;
    opt.osd = s.osd;
    opt.videoScale = s.videoScale;
    opt.videoFilter = s.videoFilter;
    opt.fixSramMemorySize = s.fixSramMemsize != 0;
    opt.scsiEx = s.scsiEx != 0;
    configPath(opt.config, s.scsiExRom, opt.scsiExRom, sizeof opt.scsiExRom);
    for (int id = 0; id < 7; id++) configPath(opt.config, s.sc[id], opt.sc[id], sizeof opt.sc[id]);
    configPath(opt.config, s.cd, opt.cd, sizeof opt.cd);
    opt.cdId = s.cdId;
    opt.fpuBoard = s.fpuBoard;
    opt.mercury = s.mercury != 0;
    opt.neptune = s.neptune != 0;
    platform::copyString(opt.neptuneMac, sizeof opt.neptuneMac, s.neptuneMac);
    platform::copyString(opt.neptuneLink, sizeof opt.neptuneLink, s.neptuneLink);
    platform::copyString(opt.rs232c, sizeof opt.rs232c, s.rs232c);
    opt.midi = s.midi != 0;
    configPath(opt.config, s.midiSf2, opt.midiSf2, sizeof opt.midiSf2);
    opt.midiVoices = s.midiVoices;
    opt.midiOut = s.midiOut;
    configPath(opt.config, s.midiMt32Ctrl, opt.midiMt32Ctrl, sizeof opt.midiMt32Ctrl);
    configPath(opt.config, s.midiMt32Pcm, opt.midiMt32Pcm, sizeof opt.midiMt32Pcm);
    configPath(opt.config, s.midiSc55, opt.midiSc55, sizeof opt.midiSc55);
    opt.midiUartFamily = s.midiUartFamily;
}

// メニューの「Restart」の設定を Options に写す。ファイル名は、動いている値と同じ (Settings に入りきらずに
// 切れたものを含む) なら今のまま、メニューで選んだものなら、設定ファイルのディレクトリか --image-dir の
// どちらかにあるほうを使う (メニューは --image-dir の一覧から選ぶ)
void restartPath(const Options& opt, const char* value, char* cur, size_t size) {
    char trunc[platform::Settings::PATH_SIZE];
    platform::copyString(trunc, sizeof trunc, cur);
    if (strcmp(trunc, value) == 0) return;
    if (value[0] != 0 && value[0] != '/') {
        char path[1100];
        snprintf(path, sizeof path, "%s/%s", opt.imageDir, value);
        if (access(path, R_OK) == 0) {
            char inCfg[1100];
            configPath(opt.config, value, inCfg, sizeof inCfg);
            if (!opt.config[0] || access(inCfg, R_OK) != 0) {
                platform::copyString(cur, size, path); // 入りきらなければ切る (設定のパスの長さの上限)
                return;
            }
        }
    }
    configPath(opt.config, value, cur, size);
}

// メニューの設定を設定ファイルに書く値に直す。動いている値は「今のディレクトリからのパス」(メニューで選んだ
// HD0 / CD-ROM は --image-dir の中の名前) なので、設定ファイルからの相対パス (設定ファイルのディレクトリの中なら)
// か絶対パスにする。設定ファイルの値をそのまま使っているもの (切れたものを含む) は、書いてあったとおりにする
void cfgValue(const Options& opt, const char* value, const char* base, char* out, size_t size) {
    char resolved[1100];
    configPath(opt.config, base, resolved, sizeof resolved);
    char trunc[platform::Settings::PATH_SIZE];
    platform::copyString(trunc, sizeof trunc, resolved);
    if (value[0] == 0 || strcmp(value, base) == 0 || strcmp(value, trunc) == 0) {
        snprintf(out, size, "%s", value[0] == 0 ? "" : base);
        return;
    }
    // realpath() の答え (PATH_MAX まで) が入る大きさにする。小さいと、長い絶対パスが途中で切れて別のパスになる
    // (macOS は PATH_MAX が 1024 なので、前と同じ 1100 より小さくはしない)
    char path[PATH_MAX > 1100 ? PATH_MAX : 1100];
    platform::copyString(path, sizeof path, value);
    if (!strchr(value, '/')) {
        snprintf(path, sizeof path, "%s/%s", opt.imageDir, value);
        if (access(path, R_OK) != 0) configPath(opt.config, value, path, sizeof path);
    }
    char abs[PATH_MAX];
    if (path[0] != '/' && realpath(path, abs)) platform::copyString(path, sizeof path, abs);
    char dir[1100], dirAbs[PATH_MAX];
    dirName(opt.config, dir, sizeof dir);
    if (realpath(dir, dirAbs)) {
        const size_t n = strlen(dirAbs);
        if (strncmp(path, dirAbs, n) == 0 && path[n] == '/') {
            platform::copyString(out, size, path + n + 1);
            return;
        }
    }
    platform::copyString(out, size, path); // 入りきらなければ切る (設定のパスの長さの上限)
}

platform::Settings settingsForFile(const Options& opt, const platform::Settings& s) {
    platform::Settings f = s;
    const platform::Settings& b = opt.fileSettings;
    for (int d = 0; d < 2; d++) cfgValue(opt, s.fd[d], b.fd[d], f.fd[d], sizeof f.fd[d]);
    cfgValue(opt, s.hd0, b.hd0, f.hd0, sizeof f.hd0);
    cfgValue(opt, s.scsiExRom, b.scsiExRom, f.scsiExRom, sizeof f.scsiExRom);
    for (int id = 0; id < 7; id++) cfgValue(opt, s.sc[id], b.sc[id], f.sc[id], sizeof f.sc[id]);
    cfgValue(opt, s.cd, b.cd, f.cd, sizeof f.cd);
    return f;
}

void applyRestartSettings(const platform::Settings& s, Options& opt) {
    Options before = opt;
    applySettings(s, opt);
    auto keep = [&](const char* value, const char* old, char* cur, size_t size) {
        snprintf(cur, size, "%s", old);
        restartPath(opt, value, cur, size);
    };
    for (int d = 0; d < 2; d++) keep(s.fd[d], before.diskPath[d], opt.diskPath[d], sizeof opt.diskPath[d]);
    keep(s.hd0, before.hdfPath, opt.hdfPath, sizeof opt.hdfPath);
    keep(s.scsiExRom, before.scsiExRom, opt.scsiExRom, sizeof opt.scsiExRom);
    for (int id = 0; id < 7; id++) keep(s.sc[id], before.sc[id], opt.sc[id], sizeof opt.sc[id]);
    keep(s.cd, before.cd, opt.cd, sizeof opt.cd);
}

// いま動いている設定 (メニューに渡す)。ホスト版で使わない項目 (render_core など) は設定ファイルの値のまま
platform::Settings runningSettings(const Options& opt) {
    platform::Settings s = opt.fileSettings;
    for (int d = 0; d < 2; d++) platform::copyString(s.fd[d], sizeof s.fd[d], opt.diskPath[d]);
    platform::copyString(s.hd0, sizeof s.hd0, opt.hdfPath);
    s.cpu = opt.cpu;
    s.clockMHz = opt.clockMHz;
    s.ramMB = opt.ramMB;
    s.highMemMB = opt.highMemMB;
    if (!(s.localMemMB < 0 && opt.localMemMB == 128)) s.localMemMB = opt.localMemMB;
    s.maxSpeed = opt.maxSpeed;
    s.speed060 = opt.speed060;
    s.maxSpeedLimit = opt.maxSpeedLimit;
    s.busSyncSprite = opt.busSyncSprite ? 1 : 0;
    s.osd = opt.osd;
    s.videoScale = opt.videoScale;
    s.videoFilter = opt.videoFilter;
    s.fixSramMemsize = opt.fixSramMemorySize;
    s.scsiEx = opt.scsiEx;
    platform::copyString(s.scsiExRom, sizeof s.scsiExRom, opt.scsiExRom);
    for (int id = 0; id < 7; id++) platform::copyString(s.sc[id], sizeof s.sc[id], opt.sc[id]);
    platform::copyString(s.cd, sizeof s.cd, opt.cd);
    s.cdId = opt.cdId;
    s.fpuBoard = opt.fpuBoard;
    s.mercury = opt.mercury;
    s.neptune = opt.neptune;
    platform::copyString(s.neptuneMac, sizeof s.neptuneMac, opt.neptuneMac);
    platform::copyString(s.neptuneLink, sizeof s.neptuneLink, opt.neptuneLink);
    platform::copyString(s.rs232c, sizeof s.rs232c, opt.rs232c);
    s.midi = opt.midi;
    platform::copyString(s.midiSf2, sizeof s.midiSf2, opt.midiSf2);
    s.midiVoices = opt.midiVoices;
    s.midiOut = opt.midiOut;
    platform::copyString(s.midiMt32Ctrl, sizeof s.midiMt32Ctrl, opt.midiMt32Ctrl);
    platform::copyString(s.midiMt32Pcm, sizeof s.midiMt32Pcm, opt.midiMt32Pcm);
    platform::copyString(s.midiSc55, sizeof s.midiSc55, opt.midiSc55);
    s.midiUartFamily = opt.midiUartFamily;
    // ファイル名は設定ファイルに書く形にそろえる (メニューが設定ファイルの値と比べて「*」を付けるため)
    return settingsForFile(opt, s);
}

// 設定ファイルを読み、Options と opt.fileSettings に入れる。読めなければ false。
// --profile があれば、そのファイルを設定ファイルの共通の項目に重ねて読む (platform/boot_profile.h)
bool readConfigFile(Options& opt) {
    size_t size = 0;
    uint8_t* data = loadWhole(opt.config, size);
    if (!data) {
        fprintf(stderr, "--config: cannot read %s\n", opt.config);
        return false;
    }
    platform::ConfigText text, prof;
    text.load((const char*)data, size);
    free(data);
    if (opt.profile[0]) {
        char name[64], dir[1024];
        if (!platform::profileFileName(opt.profile, name, sizeof name)) {
            fprintf(stderr, "--profile: %s: use letters, digits, - and _ (up to %d)\n", opt.profile,
                    platform::PROFILE_NAME_MAX);
            return false;
        }
        dirName(opt.config, dir, sizeof dir);
        snprintf(opt.profilePath, sizeof opt.profilePath, "%s/%s", dir, name);
        data = loadWhole(opt.profilePath, size);
        if (!data) {
            fprintf(stderr, "--profile: cannot read %s\n", opt.profilePath);
            return false;
        }
        prof.load((const char*)data, size);
        free(data);
    }
    platform::Settings s;
    platform::readProfileSettings(
        text, opt.profile[0] ? &prof : nullptr, s, true,
        // net_token の値は出さない (ベアメタル版と同じ。platform/setting_warning.h)
        [&](bool fromProfile, const char* key, const char* value, platform::SettingResult r) {
            char msg[384];
            platform::formatSettingWarning(msg, sizeof msg, key, value, r);
            fprintf(stderr, "%s: %s\n", fromProfile ? opt.profilePath : opt.config, msg);
        },
        [&](bool fromProfile, const char* line) {
            char msg[384];
            platform::formatIgnoredLine(msg, sizeof msg, line);
            fprintf(stderr, "%s: %s\n", fromProfile ? opt.profilePath : opt.config, msg);
        },
        hostDefaultSettings());
    // autotype= (起動したあとに打つ文字列)。コマンドラインの --type / --type-after のほうが勝つ (あとで読む)
    if (s.autotype[0]) {
        platform::unescapeTyped(s.autotype, strlen(s.autotype), opt.typeText, sizeof opt.typeText);
        opt.typeAfter = (long)s.autotypeWait * 55;
    }
    if (!x68k::Config().setCpu(s.cpu)) {
        fprintf(stderr, "%s: cpu=%d: this build has no 68030 / 68060; using 68000\n",
                opt.profile[0] ? opt.profilePath : opt.config, s.cpu);
        s.cpu = 68000;
    }
    // モニタの枠 (video_filter= の cz-600de / cz-600db) はベアメタル版だけ。設定は残し (保存しても消さない)、使わない
    if (const char* bz = platform::videoBezelName(s.videoFilter))
        fprintf(stderr, "%s: video_filter=%s: the bezel is not supported by the host version; ignored\n",
                opt.profile[0] ? opt.profilePath : opt.config, bz);
    applySettings(s, opt);
    // ファイル名は Options と同じ形 (設定ファイルのディレクトリから) にしておく (メニューの値と比べるため)
    for (int d = 0; d < 2; d++) platform::copyString(s.fd[d], sizeof s.fd[d], opt.diskPath[d]);
    platform::copyString(s.hd0, sizeof s.hd0, opt.hdfPath);
    platform::copyString(s.scsiExRom, sizeof s.scsiExRom, opt.scsiExRom);
    for (int id = 0; id < 7; id++) platform::copyString(s.sc[id], sizeof s.sc[id], opt.sc[id]);
    platform::copyString(s.cd, sizeof s.cd, opt.cd);
    opt.fileSettings = s;
    return true;
}

// X68000 の画面 (w x h) を kMenuWidth x kMenuHeight に引き伸ばし、メニューを重ねる
void composeMenu(const uint32_t* screen, int w, int h, uint32_t* out, const platform::Menu& menu) {
    for (int y = 0; y < kMenuHeight; y++) {
        uint32_t* row = out + (size_t)y * kMenuWidth;
        if (w <= 0 || h <= 0) {
            memset(row, 0, (size_t)kMenuWidth * 4);
            continue;
        }
        const uint32_t* src = screen + (size_t)(y * h / kMenuHeight) * x68k::Renderer::MAX_WIDTH;
        for (int x = 0; x < kMenuWidth; x++) row[x] = src[x * w / kMenuWidth];
    }
    menu.draw(out, kMenuWidth, kMenuWidth, kMenuHeight);
}

// --press: 決めた回から、キーを 2 回に 1 つずつ押して離す (試験用)
class PressScript {
public:
    PressScript(const char* list, long after) : after_(after) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", list);
        for (char* tok = strtok(buf, ","); tok && count_ < 128; tok = strtok(nullptr, ",")) {
            static const struct {
                const char* name;
                uint8_t usage;
            } kNames[] = {
                {"menu", platform::menu_key::SCROLL_LOCK}, {"f12", platform::menu_key::F12},
                {"up", platform::menu_key::UP},            {"down", platform::menu_key::DOWN},
                {"left", platform::menu_key::LEFT},        {"right", platform::menu_key::RIGHT},
                {"enter", platform::menu_key::ENTER},      {"esc", platform::menu_key::ESC},
                {"home", platform::menu_key::HOME},        {"end", platform::menu_key::END},
                {"pgup", platform::menu_key::PAGE_UP},     {"pgdn", platform::menu_key::PAGE_DOWN},
            };
            int u = -1;
            for (const auto& n : kNames)
                if (!strcmp(tok, n.name)) u = n.usage;
            if (u < 0) u = (int)strtol(tok, nullptr, 16);
            if (u > 0 && u < 256) keys_[count_++] = (uint8_t)u;
            else fprintf(stderr, "--press: unknown key: %s\n", tok);
        }
    }
    void step(long frame, HostInput& input) {
        const long i = frame - after_;
        if (i < 0 || i >= 2L * count_) return;
        input.key(keys_[i / 2], (i & 1) == 0);
    }

private:
    long after_;
    uint8_t keys_[128];
    int count_ = 0;
};

struct ScsiUnit {
    x68k::HardDiskImage image;
    uint8_t* data = nullptr;
    size_t size = 0;
    const char* path = nullptr;
    bool cdrom = false;
    x68k::CdToc* toc = nullptr; // CD-ROM のトラックの並び (CUE シートのとき。ISO なら nullptr)
};

// CD-ROM のイメージを読む。path が .cue なら CUE シートで、そこに書かれたファイル (BIN、WAV) を CUE シートと同じ
// ディレクトリから読み、順につなげて 1 つのイメージにする (toc にトラックの並び、blockSize は 1)。
// それ以外は ISO (toc は nullptr、blockSize は 2048)。読めなければ false (err に理由)
bool loadCdImage(const char* path, uint8_t*& data, size_t& size, x68k::CdToc*& toc, uint32_t& blockSize, std::string& err) {
    data = nullptr;
    size = 0;
    toc = nullptr;
    blockSize = 2048;
    const size_t n = strlen(path);
    if (n < 4 || strcasecmp(path + n - 4, ".cue") != 0) {
        data = loadWhole(path, size);
        if (!data) err = "cannot read the file";
        return data != nullptr;
    }
    size_t textSize = 0;
    uint8_t* text = loadWhole(path, textSize);
    if (!text) {
        err = "cannot read the file";
        return false;
    }
    std::unique_ptr<platform::CueSheet> cue(new platform::CueSheet);
    std::vector<uint8_t*> parts;
    std::vector<size_t> sizes;
    uint64_t base[platform::CueSheet::MAX_FILES], bytes[platform::CueSheet::MAX_FILES];
    bool ok = cue->parse((const char*)text, textSize);
    if (!ok) err = cue->error();
    char dir[1100];
    dirName(path, dir, sizeof dir);
    uint64_t total = 0;
    for (int i = 0; ok && i < cue->fileCount(); i++) {
        char name[512], file[1700];
        ok = cue->fileName(i, name, sizeof name);
        if (!ok) {
            err = "a FILE name is too long";
            break;
        }
        if (name[0] == '/') snprintf(file, sizeof file, "%s", name);
        else snprintf(file, sizeof file, "%s/%s", dir, name);
        size_t partSize = 0;
        uint8_t* part = loadWhole(file, partSize);
        if (!part) {
            err = std::string("cannot read ") + file;
            ok = false;
            break;
        }
        parts.push_back(part);
        sizes.push_back(partSize);
        uint32_t offset = 0;
        uint64_t len = partSize;
        if (cue->fileType(i) == platform::CueSheet::WAVE) {
            const char* why = nullptr;
            ok = platform::wavData(part, partSize < 4096 ? partSize : 4096, partSize, offset, len, why);
            if (!ok) err = std::string(file) + ": " + why;
        }
        base[i] = total + offset;
        bytes[i] = len;
        total += partSize;
    }
    toc = new x68k::CdToc;
    if (ok && !cue->layout(base, bytes, *toc)) {
        err = cue->error();
        ok = false;
    }
    free(text); // CueSheet はファイルの名前を text の中に指しているので、使い終わってから
    if (ok && parts.size() == 1) { // ファイルが 1 つなら、そのまま使う
        data = parts[0];
        parts.clear();
    } else if (ok) {
        data = (uint8_t*)malloc((size_t)total);
        size_t at = 0;
        for (size_t i = 0; data && i < parts.size(); i++) {
            memcpy(data + at, parts[i], sizes[i]);
            at += sizes[i];
        }
        if (!data) {
            err = "out of memory";
            ok = false;
        }
    }
    for (uint8_t* p : parts) free(p);
    if (!ok) {
        delete toc;
        toc = nullptr;
        return false;
    }
    size = (size_t)total;
    blockSize = 1;
    return true;
}

// メニューから頼まれたことをする (ホスト版)
class HostMenu : public platform::MenuHost {
public:
    HostMenu(Options& opt, x68k::Machine& m, Disk* disks, ScsiUnit* units) : opt_(opt), m_(m), disks_(disks), units_(units) {}

    void listFiles(platform::MenuFileKind kind, platform::MenuFileList& list) override {
        DIR* dir = opendir(opt_.imageDir);
        if (!dir) return;
        while (struct dirent* e = readdir(dir)) {
            if (e->d_name[0] == '.') continue;
            if (platform::MenuFileList::matches(e->d_name, kind) && !list.add(e->d_name)) break;
        }
        closedir(dir);
    }
    bool insertDisk(int drive, const char* name, platform::MenuText& msg) override {
        char path[1100] = {0};
        if (name[0]) snprintf(path, sizeof path, "%s/%s", opt_.imageDir, name);
        // 読めるかを先に確かめる (読めなければ入っているディスクはそのまま)
        Disk next;
        if (name[0] && !loadDisk(next, path, opt_.readOnly)) {
            msg.add("cannot load ").add(name);
            return false;
        }
        unloadDisk(m_, drive, disks_[drive], opt_.writeBack);
        opt_.diskPath[drive][0] = 0;
        if (name[0]) {
            // Disk の image はデータを指しているので、読んだものを入れ直す
            next.image.unload();
            free(next.data);
            next.data = nullptr;
            if (!loadDisk(disks_[drive], path, opt_.readOnly)) {
                msg.add("cannot load ").add(name);
                return false;
            }
            m_.insertDisk(drive, &disks_[drive].image);
            snprintf(opt_.diskPath[drive], sizeof opt_.diskPath[drive], "%s", path);
        }
        printf("menu: fd%d: %s\n", drive, name[0] ? path : "ejected");
        return true;
    }
    const char* diskName(int drive) override { return opt_.diskPath[drive]; }
    // CD-ROM 装置の媒体を、動かしたまま入れ替える (name が空なら取り出す)。装置がなければ 0 (次の起動から)
    int changeCd(const char* name, platform::MenuText& msg) override {
        x68k::CdDrive* drive = m_.cdDrive();
        if (!drive) return 0;
        ScsiUnit& u = units_[drive->id()];
        char path[1100] = {0};
        uint8_t* data = nullptr;
        size_t size = 0;
        x68k::CdToc* toc = nullptr;
        uint32_t blockSize = 2048;
        if (name[0]) {
            // 読めなければ、入っている媒体はそのまま
            snprintf(path, sizeof path, "%s/%s", opt_.imageDir, name);
            std::string err;
            if (!loadCdImage(path, data, size, toc, blockSize, err)) {
                msg.add("cannot load ").add(name);
                fprintf(stderr, "menu: CD-ROM: %s: %s\n", path, err.c_str());
                return -1;
            }
        }
        const bool prevented = drive->prevented();
        m_.changeCd(nullptr); // 前のイメージを外してから解放する
        u.image.unload();
        free(u.data);
        delete u.toc;
        u.data = data;
        u.size = size;
        u.toc = toc;
        u.cdrom = true;
        if (data && !u.image.load(data, size, blockSize, true)) {
            free(u.data);
            delete u.toc;
            u.data = nullptr;
            u.toc = nullptr;
            platform::menuCdEmptyText(msg, name);
            opt_.cd[0] = 0;
            return -2; // 前の媒体は外した: 装置は空
        }
        if (data) m_.changeCd(&u.image, u.toc);
        snprintf(opt_.cd, sizeof opt_.cd, "%s", path);
        u.path = opt_.cd;
        printf("menu: CD-ROM: %s%s\n", name[0] ? path : "ejected", prevented ? " (was locked by the X68000)" : "");
        // X68000 が取り出しを禁止していた (PREVENT ALLOW MEDIUM REMOVAL) ことを、メニューに出す
        if (prevented) platform::menuCdChangedText(msg, name[0] != 0, true);
        return 1;
    }
    void resetMachine() override {
        m_.powerOn();
        printf("menu: reset\n");
    }
    bool saveState(int slot, platform::MenuText& msg) override {
        char path[1100];
        statePath(slot, path, sizeof path);
        const size_t cap = x68k::stateSize(m_);
        uint8_t* buf = (uint8_t*)malloc(cap);
        size_t n = 0;
        bool ok = buf && x68k::saveState(m_, buf, cap, n) && saveFile(path, buf, n);
        free(buf);
        if (!ok) {
            msg.add("cannot save state ").add((long)slot);
            fprintf(stderr, "menu: cannot save %s\n", path);
            return false;
        }
        printf("menu: state saved to %s (%zu bytes)\n", path, n);
        msg.add("state ").add((long)slot).add(" saved");
        return true;
    }
    bool loadState(int slot, platform::MenuText& msg) override {
        char path[1100];
        statePath(slot, path, sizeof path);
        size_t size = 0;
        uint8_t* data = loadWhole(path, size);
        if (!data) {
            msg.add("no state ").add((long)slot);
            return false;
        }
        const char* err = nullptr;
        bool ok = x68k::loadState(m_, data, size, err);
        free(data);
        if (!ok) {
            msg.add("state ").add((long)slot).add(": ").add(err ? err : "error");
            printf("menu: cannot load %s: %s\n", path, err ? err : "error");
            return false;
        }
        printf("menu: state loaded from %s\n", path);
        msg.add("state ").add((long)slot).add(" loaded");
        return true;
    }
    bool saveSettings(const platform::Settings& s, platform::MenuText& msg) override {
        if (!opt_.config[0]) {
            msg.add("no settings file (use --config)");
            return false;
        }
        // 知らない行と注釈を残すため、ファイルを読み直してから、変えた項目の行だけを書き換える。
        // ファイルがあるのに読めなければ、書かない (空のファイルとして書くと、ほかの設定が消える)
        size_t size = 0;
        uint8_t* data = loadWhole(opt_.config, size);
        if (!data) {
            struct stat st;
            const int r = stat(opt_.config, &st);
            const bool missing = r != 0 && errno == ENOENT, empty = r == 0 && st.st_size == 0;
            if (!missing && !empty) {
                msg.add("cannot read the settings file");
                fprintf(stderr, "menu: cannot read %s; settings not saved\n", opt_.config);
                return false;
            }
        }
        if (!data && opt_.configDefault) makeParentDir(opt_.config); // 既定のパスに初めて書くとき
        platform::ConfigText text;
        text.load(data ? (const char*)data : "", data ? size : 0);
        free(data);
        const platform::Settings fileValues = settingsForFile(opt_, s);
        if (opt_.profile[0]) {
            // --profile のときは、共通の項目を --config に、そのほかを組み合わせのファイルに書く (platform/boot_profile.h)
            data = loadWhole(opt_.profilePath, size);
            if (!data) {
                msg.add("cannot read the profile");
                fprintf(stderr, "menu: cannot read %s; settings not saved\n", opt_.profilePath);
                return false;
            }
            platform::ConfigText prof;
            prof.load((const char*)data, size);
            free(data);
            bool changedBase = false, changedProfile = false;
            if (!platform::writeProfileSettings(text, prof, fileValues, opt_.fileSettings, changedBase, changedProfile,
                                               hostDefaultSettings()) ||
                (changedProfile && !saveFile(opt_.profilePath, (const uint8_t*)prof.data(), prof.size())) ||
                (changedBase && !saveFile(opt_.config, (const uint8_t*)text.data(), text.size()))) {
                msg.add("cannot write the settings file");
                return false;
            }
            opt_.fileSettings = fileValues;
            printf("menu: settings saved to %s%s%s\n", changedProfile ? opt_.profilePath : "",
                   changedProfile && changedBase ? " and " : "", changedBase ? opt_.config : "");
            msg.add("settings saved");
            return true;
        }
        if (!platform::writeSettings(text, fileValues, opt_.fileSettings) ||
            !saveFile(opt_.config, (const uint8_t*)text.data(), text.size())) {
            msg.add("cannot write the settings file");
            return false;
        }
        opt_.fileSettings = fileValues;
        printf("menu: settings saved to %s\n", opt_.config);
        msg.add("settings saved");
        return true;
    }
    bool restart(const platform::Settings& s, platform::MenuText&) override {
        restart_ = s;
        return true;
    }
    const platform::Settings& restartSettings() const { return restart_; }
    void osdChanged(int osd) override { opt_.osd = osd; }
    // ホスト版はどの組み合わせも選べる
    bool hasVideo() const override { return true; }
    void videoChanged(int scale, int filter) override {
        opt_.videoScale = scale;
        opt_.videoFilter = filter;
    }
    // uart はベアメタル版の行き先。ホスト版は --midi-dump でバイト列をファイルに書くときだけ使える
    bool hasMidiOut(int out) const override { return out != 1 || opt_.midiDump[0] != 0; }

private:
    void statePath(int slot, char* out, size_t size) const { snprintf(out, size, "%s/state%d.p68", opt_.stateDir, slot); }
    Options& opt_;
    x68k::Machine& m_;
    Disk* disks_;
    ScsiUnit* units_;
    platform::Settings restart_;
};

// --midi-out uart --midi-dump FILE: ベアメタル版が UART に送るバイト列 (platform/uart_midi.h) をファイルに書く。
// 速さは配分せず、受けたらすぐ書く
class MidiUartDump : public platform::UartMidi {
public:
    MidiUartDump(FILE* f, Family family) : UartMidi(family, 0), f_(f) { setPumper(SELF); }
    ~MidiUartDump() override { fclose(f_); }

protected:
    void put(uint8_t b) override { fputc(b, f_); }
    uint64_t nowUs() override { return 0; }

private:
    FILE* f_;
};

// --midi-dump FILE (--midi-out uart 以外): 音源に渡すバイトをファイルにも書く。inner (nullptr なら捨てる) に渡す
class MidiDumpTee : public x68k::MidiOut {
public:
    MidiDumpTee(FILE* f, x68k::MidiOut* inner) : f_(f), inner_(inner) {}
    ~MidiDumpTee() override { fclose(f_); }
    void midiByte(uint8_t b) override {
        fputc(b, f_);
        if (inner_) inner_->midiByte(b);
    }
    void midiReset() override {
        if (inner_) inner_->midiReset();
    }
    Family midiFamily() const override { return inner_ ? inner_->midiFamily() : GENERAL; }
    void mixFrame(int32_t& l, int32_t& r) override {
        if (inner_) inner_->mixFrame(l, r);
    }
    bool idleWork() override { return inner_ && inner_->idleWork(); }

private:
    FILE* f_;
    x68k::MidiOut* inner_;
};

// SC-55mkII の ROM をディレクトリ dir から読み、Nuked-SC55 を起動する。読めなければ知らせて nullptr
x68k::MidiOut* loadSc55(const char* dir, int rate) {
    using platform::Sc55Synth;
    std::vector<std::string> names;
    std::vector<uint64_t> sizes;
    if (DIR* d = opendir(dir)) {
        while (dirent* e = readdir(d)) {
            const std::string path = std::string(dir) + "/" + e->d_name;
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                names.push_back(e->d_name);
                sizes.push_back((uint64_t)st.st_size);
            }
        }
        closedir(d);
    }
    std::vector<const char*> cnames;
    for (const std::string& n : names) cnames.push_back(n.c_str());
    int index[Sc55Synth::ROM_COUNT];
    const int missing = Sc55Synth::pickRoms(cnames.data(), sizes.data(), (int)names.size(), index);
    if (missing >= 0) {
        fprintf(stderr, "midi: %s: no %s (%s, %u bytes); MIDI is not played\n", dir, Sc55Synth::romFileName(missing),
                Sc55Synth::romPartName(missing), Sc55Synth::romSize(missing));
        return nullptr;
    }
    uint8_t* data[Sc55Synth::ROM_COUNT] = {};
    bool ok = true;
    for (int i = 0; i < Sc55Synth::ROM_COUNT; i++) {
        size_t size = 0;
        data[i] = loadWhole((std::string(dir) + "/" + names[index[i]]).c_str(), size);
        if (!data[i] || size != Sc55Synth::romSize(i)) ok = false;
    }
    Sc55Synth* synth = new Sc55Synth;
    const uint8_t* const roms[Sc55Synth::ROM_COUNT] = {data[0], data[1], data[2], data[3], data[4]};
    const Sc55Synth::Result r = ok ? synth->load(roms, rate) : Sc55Synth::READ_FAILED;
    for (uint8_t* p : data) free(p);
    if (r != Sc55Synth::OK) {
        fprintf(stderr, "midi: cannot load the SC-55mkII ROMs in %s: %s; MIDI is not played\n", dir,
                Sc55Synth::resultText(r));
        delete synth;
        return nullptr;
    }
    printf("MIDI: SC-55mkII (Nuked-SC55): %s\n", dir);
    return synth;
}

// MIDI ボードの音源を用意する (opt.midiOut で選ぶ)。読めなければ知らせて nullptr (MIDI は鳴らさずに続ける)
x68k::MidiOut* loadMidiSynth(const Options& opt, int rate) {
    if (opt.midiOut == 2) return nullptr; // none
    if (opt.midiOut == 4) {               // sc55
        if (!opt.midiSc55[0]) {
            fprintf(stderr, "midi: --midi-out sc55 needs --midi-sc55; MIDI is not played\n");
            return nullptr;
        }
        return loadSc55(opt.midiSc55, rate);
    }
    if (opt.midiOut == 3) {               // mt32
        if (!opt.midiMt32Ctrl[0] || !opt.midiMt32Pcm[0]) {
            fprintf(stderr, "midi: --midi-out mt32 needs --midi-mt32-ctrl and --midi-mt32-pcm; MIDI is not played\n");
            return nullptr;
        }
        size_t ctrlSize = 0, pcmSize = 0;
        uint8_t* ctrl = loadRom(opt.midiMt32Ctrl, ctrlSize, platform::Mt32Synth::controlSizeOk);
        uint8_t* pcm = loadRom(opt.midiMt32Pcm, pcmSize, platform::Mt32Synth::pcmSizeOk);
        platform::Mt32Synth* synth = new platform::Mt32Synth;
        const platform::Mt32Synth::Result r = synth->load(ctrl, ctrlSize, pcm, pcmSize, rate);
        if (r != platform::Mt32Synth::OK) {
            fprintf(stderr, "midi: cannot load the MT-32 ROMs %s and %s: %s; MIDI is not played\n", opt.midiMt32Ctrl,
                    opt.midiMt32Pcm, platform::Mt32Synth::resultText(r));
            delete synth;
            return nullptr;
        }
        printf("MIDI: MT-32 (Munt): %s, %s\n", synth->controlName(), synth->pcmName());
        return synth;
    }
    // sf2。SoundFont を指定していなければ、送った MIDI は捨てる (uart は runMachine() で扱う)
    if (!opt.midiSf2[0]) return nullptr;
    size_t size = 0;
    uint8_t* data = loadWhole(opt.midiSf2, size);
    platform::Sf2Synth* synth = new platform::Sf2Synth;
    if (!data || !synth->load(data, size, rate, opt.midiVoices)) {
        fprintf(stderr, "midi: cannot load the SoundFont %s; MIDI is not played\n", opt.midiSf2);
        delete synth;
        synth = nullptr;
    } else {
        printf("MIDI: SoundFont %s (%d presets, %d voices)\n", opt.midiSf2, synth->presetCount(), opt.midiVoices);
    }
    free(data);
    return synth;
}

int runMachine(Options& opt) {
    bool restart = false;
    std::unique_ptr<x68k::EtherLink> etherLink; // Neptune-X の送受信の口 (Machine を消したあとに閉じる)
    std::unique_ptr<x68k::SerialLink> serialLink; // RS-232C の相手 (同じく Machine を消したあとに閉じる)
    std::unique_ptr<x68k::MidiOut> midiSynth; // MIDI ボードの音源 (Machine を消したあとに消す)
    std::unique_ptr<x68k::MidiOut> midiTee;   // --midi-dump で音源に渡すバイトを書くとき (midiSynth より先に消す)
    static uint8_t ipl[x68k::Machine::IPL_SIZE], cg[x68k::Machine::CG_SIZE];
    char path[1100];
    // X68030 と 060turbo は X68030 の内蔵 SCSI を持つ
    const bool x68030 = opt.cpu == 68030 || opt.cpu == 68060;
    if (opt.ipl) snprintf(path, sizeof path, "%s", opt.ipl);
    else snprintf(path, sizeof path, "%s/%s", opt.romDir, x68030 ? "IPLROM30.DAT" : "IPLROM.DAT");
    bool ipl060 = false; // IPLROM060.DAT を使う (ROM30_060.DAT と組で使う)
    if (!opt.ipl && opt.cpu == 68060) {
        char p060[1100];
        snprintf(p060, sizeof p060, "%s/IPLROM060.DAT", opt.romDir);
        if (loadFile(p060, ipl, sizeof ipl)) {
            snprintf(path, sizeof path, "%s", p060);
            ipl060 = true;
        }
    }
    if (!loadFile(path, ipl, sizeof ipl)) {
        fprintf(stderr, "cannot read %s (128KB); showing the test pattern instead (see -r)\n", path);
        return runTestPattern(opt.maxFrames);
    }
    snprintf(path, sizeof path, "%s/CGROM.DAT", opt.romDir);
    bool haveCg = loadFile(path, cg, sizeof cg);
    if (!haveCg) fprintf(stderr, "warning: cannot read %s (768KB); characters will be blank\n", path);

    // Machine は大きいので (メイン RAM、SRAM、FDC のバッファなど) ヒープに置く
    x68k::Config config;
    config.setCpu(opt.cpu);
    config.setClockMHz(platform::Settings::clockFor(opt.cpu, opt.clockMHz));
    config.setBusSyncSprite(opt.busSyncSprite);
    config.highMemSize = (uint32_t)opt.highMemMB << 20;
    config.localMemSize = (uint32_t)opt.localMemMB << 20;
    config.ramSize = (uint32_t)opt.ramMB << 20;
    if (!opt.fixSramMemorySize) config.fixSramMemorySize = false; // --sram-memsize keep
    config.speed060 = opt.speed060;
    config.maxSpeedLimit060 = opt.maxSpeedLimit;
    x68k::Machine* m = new x68k::Machine(config);
    if (!m || !m->ok()) {
        fprintf(stderr, "out of memory\n");
        delete m;
        return 1;
    }
    m->setIplRom(ipl, sizeof ipl);
    if (haveCg) m->setCgRom(cg, sizeof cg);
    if (x68030) { // X68030 の内蔵 SCSI の ROM は IPL ROM から作る (IPLROM060.DAT を使い、ROM30_060.DAT があればそれ)
        static uint8_t scsi30[x68k::Machine::SCSI_ROM_SIZE];
        snprintf(path, sizeof path, "%s/ROM30_060.DAT", opt.romDir);
        if (!ipl060 || !loadFile(path, scsi30, sizeof scsi30)) x68k::Machine::makeScsiRom30(ipl, scsi30);
        m->setScsiRom(scsi30, sizeof scsi30);
    }

    if (opt.scsiEx) {
        if (opt.scsiExRom[0]) snprintf(path, sizeof path, "%s", opt.scsiExRom);
        else snprintf(path, sizeof path, "%s/SCSIEXROM.DAT", opt.romDir);
        size_t size = 0;
        uint8_t* rom = loadWhole(path, size);
        if (!rom || !m->setScsiExRom(rom, size)) {
            fprintf(stderr, "cannot load the SCSI board ROM %s (8192 bytes or less)\n", path);
            free(rom);
            delete m;
            return 1;
        }
        free(rom);
        printf("SCSI board (CZ-6BS1): %s\n", path);
    }
    if ((opt.fpuBoard && !m->setFpuBoards(opt.fpuBoard)) || (opt.mercury && !m->setMercury(true))) {
        fprintf(stderr, "out of memory\n");
        delete m;
        return 1;
    }
    if (opt.midi) {
        if (!m->setMidiBoard(true)) {
            fprintf(stderr, "out of memory\n");
            delete m;
            return 1;
        }
        printf("MIDI board (CZ-6BM1, YM3802) at $EAFA00\n");
    }
    if (opt.fpuBoard) printf("FPU board (CZ-6BP1, MC68881) at $E9E000\n");
    if (opt.fpuBoard >= 2) printf("FPU board (CZ-6BP1, MC68881) at $E9E080\n");
    if (opt.mercury) printf("Mercury Unit at $ECC080\n");
    if (opt.neptune) {
        uint8_t mac[6];
        memcpy(mac, x68k::Neptune::DEFAULT_MAC, 6);
        if (opt.neptuneMac[0] && !platform::parseMac(opt.neptuneMac, mac)) {
            fprintf(stderr, "neptune: bad MAC address %s (like 02:68:6b:00:00:01)\n", opt.neptuneMac);
            delete m;
            return 1;
        }
        char err[256] = {0};
        etherLink.reset(openHostEtherLink(opt.neptuneLink, mac, err, sizeof err));
        if (!etherLink || !m->setNeptune(true)) {
            fprintf(stderr, "neptune: %s\n", err[0] ? err : "out of memory");
            delete m;
            return 1;
        }
        m->neptune()->setMac(mac);
        m->neptune()->setLink(etherLink.get());
        printf("Neptune-X at $ECE000 (MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s)\n", mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5], opt.neptuneLink[0] ? opt.neptuneLink : "echo");
    }
    if (opt.rs232c[0]) {
        char err[256] = {0};
        serialLink.reset(openHostSerialLink(opt.rs232c, err, sizeof err));
        if (!serialLink && err[0]) {
            fprintf(stderr, "rs232c: %s\n", err);
            delete m;
            return 1;
        }
        m->setSerialLink(serialLink.get());
    }
    // SCSI の装置: 外付け SCSI ボードがあればそちら、なければ内蔵 SCSI (X68030 と 060turbo)
    static ScsiUnit units[8];
    for (int id = 0; id < 8; id++) {
        const bool isCd = opt.cd[0] && id == opt.cdId;
        const char* file = isCd ? opt.cd : opt.sc[id];
        if (!file[0]) continue;
        if (!m->hasScsiEx() && !x68030) {
            fprintf(stderr, "SCSI ID %d: %s: no SCSI interface (use --scsi-ex, or --cpu 68030)\n", id, file);
            delete m;
            return 1;
        }
        ScsiUnit& u = units[id];
        u.path = file;
        u.cdrom = isCd;
        uint32_t blockSize = 512;
        std::string err = "not an image";
        if (isCd) loadCdImage(file, u.data, u.size, u.toc, blockSize, err);
        else u.data = loadWhole(file, u.size);
        if (!u.data || !u.image.load(u.data, u.size, blockSize, isCd || opt.readOnly)) {
            fprintf(stderr, "cannot load SCSI image %s%s%s\n", file, isCd ? ": " : "", isCd ? err.c_str() : "");
            delete m;
            return 1;
        }
        if (m->hasScsiEx()) m->attachScsiEx(id, &u.image, isCd, u.toc);
        else m->attachScsi(id, &u.image, isCd, u.toc);
        if (isCd) {
            const x68k::CdToc& toc = m->cdDrive()->toc();
            int audio = 0;
            for (int t = 0; t < toc.count; t++) audio += toc.tracks[t].audio();
            printf("SCSI ID %d (%s): %s (%u blocks, %d tracks, %d audio), CD-ROM\n", id, m->hasScsiEx() ? "board" : "built-in",
                   file, toc.leadOut, toc.count, audio);
        } else {
            printf("SCSI ID %d (%s): %s (%u blocks)\n", id, m->hasScsiEx() ? "board" : "built-in", file, u.image.blocks());
        }
    }

    static Disk disks[2];
    for (int d = 0; d < 2; d++) {
        if (!opt.diskPath[d][0]) continue;
        if (!loadDisk(disks[d], opt.diskPath[d], opt.readOnly)) {
            fprintf(stderr, "cannot load disk image %s\n", opt.diskPath[d]);
            delete m;
            return 1;
        }
        m->insertDisk(d, &disks[d].image);
    }
    static x68k::HardDiskImage hd;
    uint8_t* hdData = nullptr;
    size_t hdSize = 0;
    if (opt.hdfPath[0]) {
        hdData = loadWhole(opt.hdfPath, hdSize);
        if (!hdData || !hd.load(hdData, hdSize, x68k::Sasi::BLOCK_SIZE, opt.readOnly)) {
            fprintf(stderr, "cannot load hard disk image %s\n", opt.hdfPath);
            free(hdData);
            delete m;
            return 1;
        }
        m->attachSasi(0, &hd);
        printf("SASI ID 0: %s (%u records)\n", opt.hdfPath, hd.blocks());
    }

    if (opt.useSram) {
        // 正しい SRAM のファイル (16KB か、ベアメタル版の 16KB + 後書き。x68k/sram_file.h) のときだけ写す
        // (途中までのファイルで SRAM を半端に書き換えない)
        size_t size = 0;
        uint8_t* data = loadWhole(opt.sram, size);
        uint32_t generation;
        if (data && x68k::SramSlots::parse(data, size, generation)) {
            memcpy(m->sram(), data, x68k::Machine::SRAM_SIZE);
            printf("SRAM: %s\n", opt.sram);
        } else if (data) {
            fprintf(stderr, "SRAM: %s is not a valid SRAM file (%zu bytes); ignored\n", opt.sram, size);
        }
        free(data);
    }
    m->setTime(opt.startTime >= 0 ? opt.startTime : localSeconds());

    uint32_t* screen = (uint32_t*)calloc((size_t)x68k::Renderer::MAX_WIDTH * x68k::Renderer::MAX_HEIGHT, 4);
    // メニューを開いている間に出す画面 (X68000 の画面を 768x512 に引き伸ばし、メニューを重ねる)
    uint32_t* overlay = (uint32_t*)calloc((size_t)kMenuWidth * kMenuHeight, 4);
    SdlVideo* video = new SdlVideo;
    if (!screen || !overlay || !video->init(768, 512)) {
        fprintf(stderr, "video init: %s\n", SDL_GetError());
        delete video;
        free(screen);
        free(overlay);
        delete m;
        return 1;
    }
    x68k::Renderer renderer(m->video());
    renderer.setTarget(screen, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_HEIGHT);
    m->setLineListener(&renderer);
    static platform::WavFile wav;
    static int16_t audioBuf[2 * 4096];
    SDL_AudioDeviceID audioDev = 0;
    if (opt.wav) {
        m->sound().setOutputRate(48000);
        if (!wav.open(opt.wav, 48000)) perror(opt.wav);
    } else if (!opt.noAudio && !opt.noWait) {
        audioDev = openAudio(*m);
    }
    // MIDI ボードの音源: 音を作る周波数が決まってから SoundFont か ROM を読む (音を出さないなら読まない)
    if (opt.midi) {
        FILE* dump = nullptr;
        if (opt.midiDump[0] && !(dump = fopen(opt.midiDump, "wb"))) perror(opt.midiDump);
        if (opt.midiOut == 1) { // uart
            if (dump) {
                const bool mt32 = opt.midiUartFamily == 1;
                midiSynth.reset(new MidiUartDump(dump, mt32 ? x68k::MidiOut::MT32 : x68k::MidiOut::GENERAL));
                printf("MIDI: UART bytes to %s (%s)\n", opt.midiDump, mt32 ? "mt32" : "gs");
            } else {
                fprintf(stderr, "midi: --midi-out uart needs --midi-dump FILE on the host; MIDI is not played\n");
            }
        } else {
            if (m->sound().outputRate() > 0) midiSynth.reset(loadMidiSynth(opt, m->sound().outputRate()));
            if (dump) midiTee.reset(new MidiDumpTee(dump, midiSynth.get()));
        }
    }
    if (midiTee) m->setMidiOut(midiTee.get());
    else if (midiSynth) m->setMidiOut(midiSynth.get());
    m->powerOn();

    // 速さの合わせ方: X68000 が進めた CPU サイクルを実時間に直し、その時刻まで待つ。
    // フレームの長さは CRTC の設定から決まるので、31kHz の画面なら約 55.46Hz になる
    const uint64_t freq = SDL_GetPerformanceFrequency();
    const double cpuHz = (double)m->cpuHz();
    uint64_t baseTicks = SDL_GetPerformanceCounter();
    // サイクル数は totalCycles() で見る (runFrame() が時刻を戻しても、差が狂わない。Machine::rebaseTime())
    uint64_t baseCycles = m->totalCycles();
    const uint64_t startTicks = baseTicks, startCycles = baseCycles;
    const char* typing = opt.typeText[0] ? opt.typeText : nullptr;
    // 左上のメッセージ (--message)。ベアメタル版の POST /message と同じ部品で作り、ログも同じ形で出す
    static platform::MessageGlyphs message;
    uint64_t messageDeadline = 0;
    uint32_t messageGen = 0;
    if (opt.messageText[0]) {
        static platform::MessageLayout lay;
        const size_t n = strlen(opt.messageText);
        platform::layoutMessage(opt.messageText, n, lay);
        platform::rasterMessage(lay, haveCg ? cg : nullptr, haveCg ? sizeof cg : 0, message);
        messageGen = 1;
        if (opt.messageSec > 0) messageDeadline = (uint64_t)SDL_GetTicks() + (uint64_t)opt.messageSec * 1000;
        char line[256];
        platform::formatMessageLog(line, sizeof line, opt.messageText, n, (unsigned)opt.messageSec, message.lines == 0);
        printf("%s\n", line);
        if (opt.messageDump) {
            static uint32_t pixels[platform::MSG_LINES * 16 * platform::MSG_COLS * 8];
            platform::OsdRect r;
            r.w = message.maxCols() * 8;
            r.h = message.lines * 16;
            platform::renderMessage(pixels, r, message);
            if (r.w > 0 && r.h > 0) writePpm(pixels, r.w, r.w, r.h, opt.messageDump);
        }
    }

    HostMenu menuHost(opt, *m, disks, units);
    platform::Menu menu(menuHost);
    HostInput input(m->input(), opt.joyKeys, &menu, [&]() {
        menu.open(runningSettings(opt), opt.fileSettings);
        printf("menu: opened\n");
    });
    PressScript press(opt.press, opt.pressAfter);
    int shownLeds = -1;
    bool shownCapture = false;
    bool quit = false;
    bool menuWasOpen = false;
    long frame = 0;
    x68k::MaxSpeed maxSpeed;
    // 画面の隅の数字 (osd=on / fps): 1 秒ごとに、その間に出したフレームの数と速さを数える。ホスト版は合成したフレームを
    // すべてそのまま出すので、合成した数と出した数は同じで、置き換わった数は 0。温度は出さない
    platform::OsdValue osdValue;
    unsigned osdEffMHz = 0; // 最高速モードの実効のクロック (platform::osdEffMHzShown())
    uint64_t osdTicks = baseTicks, osdCycles = baseCycles;
    long osdFrame = 0;
    for (; !quit && (opt.maxFrames < 0 || frame < opt.maxFrames); frame++) {
        const uint64_t frameTicks = SDL_GetPerformanceCounter(), frameCycles = m->totalCycles();
        quit = input.poll();
        press.step(frame, input);
        if (m->keyboard().leds() != shownLeds || input.captured() != shownCapture) {
            shownLeds = m->keyboard().leds();
            shownCapture = input.captured();
            char title[256];
            makeTitle((uint8_t)shownLeds, shownCapture, title, sizeof title);
            video->setTitle(title);
        }
        // 効果は、メニューを開いている間も掛ける (メニューで変えると、すぐに見た目が変わる)
        video->setVideo(opt.videoScale, opt.videoFilter);
        if (menu.isOpen()) {
            // メニューを開いている間は X68000 を止め、止めたときの画面にメニューを重ねて出す
            menuWasOpen = true;
            composeMenu(screen, renderer.width(), renderer.height(), overlay, menu);
            video->setOsd(platform::OsdValue()); // メニューを開いている間は出さない
            video->setMessage(nullptr, messageGen);
            video->present(overlay, kMenuWidth, kMenuHeight, kMenuWidth);
            if (!opt.noWait) SDL_Delay(16);
            continue;
        }
        if (menuWasOpen) {
            menuWasOpen = false;
            printf("menu: closed (%s)\n", menu.status());
            input.refresh();
            if (menu.restartRequested()) {
                restart = true;
                break;
            }
            // 止めていた間は数えない
            baseTicks = SDL_GetPerformanceCounter();
            baseCycles = m->totalCycles();
            osdTicks = baseTicks;
            osdCycles = baseCycles;
            osdFrame = frame;
            osdValue = platform::OsdValue(); // 数え直すまで出さない (ベアメタル版と同じ)
        }
        if (typing && frame >= opt.typeAfter) {
            while (*typing && m->keyboard().queued() < x68k::Keyboard::QUEUE_SIZE / 2) m->keyboard().typeAscii(*typing++);
            if (!*typing) typing = nullptr;
        }
        m->runFrame();
        if (opt.wav) {
            int n;
            while ((n = m->sound().ring().drain(audioBuf, 4096)) > 0) wav.write(audioBuf, n);
        }
        {
            const uint64_t now = SDL_GetPerformanceCounter();
            if (now - osdTicks >= freq) {
                platform::OsdStats st;
                const double emu = (double)(m->totalCycles() - osdCycles) / cpuHz;
                const double wall = (double)(now - osdTicks) / (double)freq;
                // 経過時間で割って 1 秒あたりにする (ベアメタル版と同じ)
                st.composedDeci = st.shownDeci = (unsigned)((double)(frame - osdFrame) * 10.0 / wall + 0.5);
                st.speedPct = (unsigned)(emu / wall * 100.0 + 0.5);
                // MPU とクロック (縦に並べるときのいちばん上。ベアメタル版と同じ)
                const m68k::CpuModel model = m->config().cpuModel;
                st.mpu = model == m68k::CpuModel::M68060 ? 68060 : model == m68k::CpuModel::M68030 ? 68030 : 68000;
                st.mpuMHz = (unsigned)((m->cpuHz() + 500000) / 1000000);
                // 最高速モードなら、実効のクロック (クロック × cpuSpeed() / 100)。ベアメタル版と同じ値を詰める
                // (ホスト版は横 1 行で、MPU の行は出さないので、今は表示に出ない)
                osdEffMHz = opt.maxSpeed ? platform::osdEffMHzShown(osdEffMHz, platform::osdEffKHz(m->cpuHz(), m->cpuSpeed()))
                                         : 0;
                st.effMHz = osdEffMHz;
                osdValue = platform::packOsd(st);
                osdTicks = now;
                osdCycles = m->totalCycles();
                osdFrame = frame;
            }
            video->setOsd(opt.osd ? osdValue : platform::OsdValue(), opt.osd == platform::OSD_MODE_FPS);
            video->setMessage(platform::messageVisible(message, messageDeadline, (uint64_t)SDL_GetTicks()) ? &message : nullptr,
                              messageGen);
        }
        video->present(screen, renderer.width(), renderer.height(), x68k::Renderer::MAX_WIDTH);
        if (opt.maxSpeed)
            maxSpeed.update(*m, (uint64_t)((double)(m->totalCycles() - frameCycles) * 1e6 / cpuHz),
                            (SDL_GetPerformanceCounter() - frameTicks) * 1000000 / freq);
        if (opt.noWait) continue;
        double emu = (double)(m->totalCycles() - baseCycles) / cpuHz;
        double wall = (double)(SDL_GetPerformanceCounter() - baseTicks) / (double)freq;
        if (emu > wall) {
            SDL_Delay((uint32_t)((emu - wall) * 1000.0));
        } else if (wall - emu > 0.25) {
            // 大きく遅れたら (ホストが重い、ウィンドウを動かしていたなど) 追いつこうとせずに基準を取り直す
            baseTicks = SDL_GetPerformanceCounter();
            baseCycles = m->totalCycles();
        }
    }
    double emuSec = (double)(m->totalCycles() - startCycles) / cpuHz;
    double wallSec = (double)(SDL_GetPerformanceCounter() - startTicks) / (double)freq;
    printf("%ld frames: emulated %.3f s, wall %.3f s (%.1f%% of real time), %.2f frames/s emulated\n", frame, emuSec,
           wallSec, wallSec > 0 ? emuSec / wallSec * 100.0 : 0.0, emuSec > 0 ? frame / emuSec : 0.0);
    if (opt.maxSpeed) {
        // 060turbo の 100% は、命令のサイクル数が選んだクロックの実機の 68060 と同じという意味 (m68k/cpu060_cycles.inc)
        if (opt.cpu == 68060)
            printf("max speed: %d%% of a real 68060 (%u MHz)\n", m->cpuSpeed(), (unsigned)(cpuHz / 1000000));
        else printf("max speed: %d%% of %u MHz\n", m->cpuSpeed(), (unsigned)(cpuHz / 1000000));
    }
    if (audioDev) {
        SDL_CloseAudioDevice(audioDev);
        printf("audio: %u underrun(s)\n", m->sound().ring().underruns());
    }
    if (opt.wav) {
        printf("wav: %u frames to %s\n", wav.frames(), opt.wav);
        wav.close();
    }

    if (opt.dumpText && haveCg) {
        static char text[x68k::TEXT_ROWS][x68k::TEXT_COLS + 1];
        x68k::readTextScreen(m->video(), cg, text);
        for (int row = 0; row < x68k::TEXT_ROWS; row++) printf("|%s\n", text[row]);
    }
    if (opt.screenshot && menu.isOpen()) {
        // メニューを開いたまま終わったときは、メニューを重ねた画面を書き出す
        writePpm(overlay, kMenuWidth, kMenuWidth, kMenuHeight, opt.screenshot);
        printf("screenshot %dx%d (menu): %s\n", kMenuWidth, kMenuHeight, opt.screenshot);
    } else if (opt.screenshot && renderer.width() > 0 && renderer.height() > 0) {
        writePpm(screen, x68k::Renderer::MAX_WIDTH, renderer.width(), renderer.height(), opt.screenshot);
        printf("screenshot %dx%d: %s\n", renderer.width(), renderer.height(), opt.screenshot);
    }
    if (opt.useSram && m->sramDirty()) {
        if (saveFile(opt.sram, m->sram(), x68k::Machine::SRAM_SIZE)) printf("SRAM saved: %s\n", opt.sram);
        else perror(opt.sram);
        m->clearSramDirty();
    }
    for (int d = 0; d < 2; d++) unloadDisk(*m, d, disks[d], opt.writeBack);
    if (hdData) {
        if (hd.dirty()) {
            if (!opt.writeBack) printf("hard disk %s was written, but not saved (use --write-back)\n", opt.hdfPath);
            else if (saveFile(opt.hdfPath, hdData, hdSize)) printf("hard disk saved: %s\n", opt.hdfPath);
            else perror(opt.hdfPath);
        }
        m->attachSasi(0, nullptr);
        hd.unload();
        free(hdData);
    }
    for (int id = 0; id < 8; id++) {
        ScsiUnit& u = units[id];
        if (!u.data) continue;
        if (u.image.dirty()) {
            if (!opt.writeBack) printf("SCSI disk %s was written, but not saved (use --write-back)\n", u.path);
            else if (saveFile(u.path, u.data, u.size)) printf("SCSI disk saved: %s\n", u.path);
            else perror(u.path);
        }
        m->attachScsiEx(id, nullptr);
        m->attachScsi(id, nullptr);
        u.image.unload();
        free(u.data);
        delete u.toc;
        u.toc = nullptr;
        u.data = nullptr;
    }
    m->setLineListener(nullptr);
    delete video;
    free(screen);
    free(overlay);
    delete m;
    if (restart) {
        // メニューで選んだ設定で作り直す (main() がもう一度 runMachine() を呼ぶ)
        applyRestartSettings(menuHost.restartSettings(), opt);
        opt.press[0] = 0; // --press は最初の起動だけ
        printf("restarting with the new settings\n");
        return kRestart;
    }
    return 0;
}

void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [-r romdir] [-0 fd0] [-1 fd1] [-h hdf] [--write-back | --read-only] [--frames N] [--no-wait]\n"
            "          [--sram file | --no-sram] [--screenshot out.ppm] [--type text] [--type-after N]\n"
            "          [--no-audio] [--wav out.wav] [--time seconds] [--dump-text] [--test-pattern] [--joy-keys] [--clock 10|16|50|75]\n"
            "          [--sram-memsize fix|keep] [--cpu 68000|68030|68060] [--ipl file] [--high-mem MB]\n"
            "          [--local-mem MB] [--ram MB] [--max-speed] [--scsi-ex | --no-scsi-ex] [--scsi-ex-rom file]\n"
            "          [--speed060 percent] [--max-speed-limit percent] [--bus-sync-sprite]\n"
            "          [--sc id file]... [--cd file] [--cd-id id] [--fpu-board [1|2]] [--mercury] [--config file | --no-config] [--profile name] [--image-dir dir]\n"
            "          [--neptune] [--neptune-link echo|loopback|pcap:IF|tap:NAME] [--neptune-mac MAC]\n"
            "          [--rs232c off|tcp:PORT|tcp:ADDR:PORT]\n"
            "          [--midi] [--midi-sf2 file.sf2] [--midi-voices N] [--midi-out sf2|mt32|sc55|none]\n"
            "          [--midi-mt32-ctrl control.rom] [--midi-mt32-pcm pcm.rom] [--midi-sc55 dir]\n"
            "          [--state-dir dir] [--press keys] [--press-after N]\n",
            argv0);
}

bool parseCount(const char* s, long& out) {
    char* end;
    out = strtol(s, &end, 10);
    return *s && *end == 0 && out >= 0;
}

// 値の検査 (コマンドラインと設定ファイルで共通)。正しくなければ false
bool parseRange(const char* s, int lo, int hi, int& out) {
    char* end;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < lo || v > hi) return false;
    out = (int)v;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    static Options opt;
    const char* home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    snprintf(opt.romDir, sizeof opt.romDir, "%s/.cache/pix68k/rom", home ? home : ".");
    snprintf(opt.sram, sizeof opt.sram, "%s/.cache/pix68k/sram.dat", home ? home : ".");
    snprintf(opt.stateDir, sizeof opt.stateDir, "%s/.cache/pix68k", home ? home : ".");
    // 設定ファイル (--config) と組み合わせ (--profile) を先に読む (コマンドラインのほうを優先する)。ほかのオプションの値 (--type "--config" など) を
    // 拾わないよう、値を取るオプションはその値を飛ばす
    bool noConfig = false; // --no-config
    static const char* const kTakesValue[] = {
        "--frames", "-r", "-0", "-1", "-h", "--sram", "--screenshot", "--type", "--type-after", "--wav", "--time",
        "--sram-memsize", "--cpu", "--ipl", "--high-mem", "--local-mem", "--ram", "--clock", "--scsi-ex-rom",
        "--cd", "--cd-id", "--image-dir", "--state-dir", "--press", "--press-after", "--midi-sf2", "--midi-voices", "--midi-out",
        "--midi-mt32-ctrl", "--midi-mt32-pcm", "--midi-sc55", "--midi-dump", "--midi-uart-family", "--speed060",
        "--max-speed-limit", "--message", "--message-sec", "--message-dump", "--profile"};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sc")) {
            i += 2;
            continue;
        }
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            snprintf(opt.profile, sizeof opt.profile, "%s", argv[i + 1]);
            i++;
            continue;
        }
        bool skip = false;
        for (const char* o : kTakesValue) skip = skip || !strcmp(argv[i], o);
        if (skip) {
            i++;
            continue;
        }
        if (!strcmp(argv[i], "--no-config")) {
            noConfig = true;
            continue;
        }
        if (strcmp(argv[i], "--config") != 0 || i + 1 >= argc) continue;
        snprintf(opt.config, sizeof opt.config, "%s", argv[i + 1]);
        i++;
    }
    if (noConfig) {
        opt.config[0] = 0;
    } else if (!opt.config[0]) {
        // 既定の設定ファイル。なければ読まないが、メニューの「Save settings」はここに書く
        snprintf(opt.config, sizeof opt.config, "%s/.cache/pix68k/pix68k.cfg", home ? home : ".");
        opt.configDefault = true;
    }
    if (opt.profile[0] && !opt.config[0]) {
        fprintf(stderr, "--profile needs --config (the profile is pix68k-NAME.cfg next to it)\n");
        return 2;
    }
    if (opt.config[0] && !(opt.configDefault && access(opt.config, R_OK) != 0) && !readConfigFile(opt)) return 2;
    bool clockGiven = false; // --clock を書いた (CPU に合わなければ警告する)
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool hasArg = i + 1 < argc;
        if ((!strcmp(a, "--config") || !strcmp(a, "--profile")) && hasArg) {
            i++; // 上で読んだ
        } else if (!strcmp(a, "--no-config")) {
            // 上で読んだ
        } else if (!strcmp(a, "--image-dir") && hasArg) {
            snprintf(opt.imageDir, sizeof opt.imageDir, "%s", argv[++i]);
        } else if (!strcmp(a, "--state-dir") && hasArg) {
            snprintf(opt.stateDir, sizeof opt.stateDir, "%s", argv[++i]);
        } else if (!strcmp(a, "--press") && hasArg) {
            snprintf(opt.press, sizeof opt.press, "%s", argv[++i]);
        } else if (!strcmp(a, "--press-after") && hasArg) {
            if (!parseCount(argv[++i], opt.pressAfter)) {
                fprintf(stderr, "--press-after: not a number: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--scsi-ex")) {
            opt.scsiEx = true;
        } else if (!strcmp(a, "--no-scsi-ex")) {
            opt.scsiEx = false; // 設定ファイルの scsi_ex=on を打ち消す
        } else if (!strcmp(a, "--fpu-board")) {
            // 続く引数がちょうど 1 か 2 なら枚数として読む (ほかの引数はみな - で始まるので、取り違えない)
            opt.fpuBoard = 1;
            if (hasArg && (!strcmp(argv[i + 1], "1") || !strcmp(argv[i + 1], "2"))) opt.fpuBoard = argv[++i][0] - '0';
        } else if (!strcmp(a, "--no-fpu-board")) {
            opt.fpuBoard = 0;
        } else if (!strcmp(a, "--mercury") || !strcmp(a, "--no-mercury")) {
            opt.mercury = a[2] == 'm';
        } else if (!strcmp(a, "--neptune") || !strcmp(a, "--no-neptune")) {
            opt.neptune = a[2] == 'n' && a[3] == 'e';
        } else if (!strcmp(a, "--neptune-link") && hasArg) {
            platform::copyString(opt.neptuneLink, sizeof opt.neptuneLink, argv[++i]);
            opt.neptune = true;
        } else if (!strcmp(a, "--midi") || !strcmp(a, "--no-midi")) {
            opt.midi = a[2] == 'm';
        } else if (!strcmp(a, "--midi-sf2") && hasArg) {
            snprintf(opt.midiSf2, sizeof opt.midiSf2, "%s", argv[++i]);
            opt.midi = true;
            opt.midiOut = 0;
        } else if ((!strcmp(a, "--midi-mt32-ctrl") || !strcmp(a, "--midi-mt32-pcm")) && hasArg) {
            const bool ctrl = a[12] == 'c';
            char* dst = ctrl ? opt.midiMt32Ctrl : opt.midiMt32Pcm;
            snprintf(dst, sizeof opt.midiMt32Ctrl, "%s", argv[++i]);
            opt.midi = true;
            opt.midiOut = 3;
        } else if (!strcmp(a, "--midi-sc55") && hasArg) {
            snprintf(opt.midiSc55, sizeof opt.midiSc55, "%s", argv[++i]);
            opt.midi = true;
            opt.midiOut = 4;
        } else if (!strcmp(a, "--midi-out") && hasArg) {
            platform::Settings tmp;
            if (platform::applySetting(tmp, "midi_out", argv[++i]) != platform::SettingResult::OK) {
                fprintf(stderr, "--midi-out: want sf2, mt32, sc55, uart or none: %s\n", argv[i]);
                return 2;
            }
            opt.midiOut = tmp.midiOut;
        } else if (!strcmp(a, "--midi-dump") && hasArg) {
            snprintf(opt.midiDump, sizeof opt.midiDump, "%s", argv[++i]);
        } else if (!strcmp(a, "--midi-uart-family") && hasArg) {
            platform::Settings tmp;
            if (platform::applySetting(tmp, "midi_uart_family", argv[++i]) != platform::SettingResult::OK) {
                fprintf(stderr, "--midi-uart-family: want gs or mt32: %s\n", argv[i]);
                return 2;
            }
            opt.midiUartFamily = tmp.midiUartFamily;
        } else if (!strcmp(a, "--midi-voices") && hasArg) {
            if (!parseRange(argv[++i], 4, 256, opt.midiVoices)) {
                fprintf(stderr, "--midi-voices: want 4..256: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--rs232c") && hasArg) {
            platform::copyString(opt.rs232c, sizeof opt.rs232c, argv[++i]);
        } else if (!strcmp(a, "--neptune-mac") && hasArg) {
            platform::copyString(opt.neptuneMac, sizeof opt.neptuneMac, argv[++i]);
        } else if (!strcmp(a, "--scsi-ex-rom") && hasArg) {
            snprintf(opt.scsiExRom, sizeof opt.scsiExRom, "%s", argv[++i]);
            opt.scsiEx = true;
        } else if (!strcmp(a, "--sc") && i + 2 < argc) {
            int id;
            if (!parseRange(argv[i + 1], 0, 6, id)) {
                fprintf(stderr, "--sc: SCSI ID must be 0..6: %s\n", argv[i + 1]);
                return 2;
            }
            snprintf(opt.sc[id], sizeof opt.sc[id], "%s", argv[i + 2]);
            i += 2;
        } else if (!strcmp(a, "--cd") && hasArg) {
            snprintf(opt.cd, sizeof opt.cd, "%s", argv[++i]);
        } else if (!strcmp(a, "--cd-id") && hasArg) {
            if (!parseRange(argv[++i], 0, 6, opt.cdId)) {
                fprintf(stderr, "--cd-id: must be 0..6: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--frames") && hasArg) {
            if (!parseCount(argv[++i], opt.maxFrames)) {
                fprintf(stderr, "--frames: not a number: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "-r") && hasArg) {
            snprintf(opt.romDir, sizeof opt.romDir, "%s", argv[++i]);
        } else if (!strcmp(a, "-0") && hasArg) {
            snprintf(opt.diskPath[0], sizeof opt.diskPath[0], "%s", argv[++i]);
        } else if (!strcmp(a, "-1") && hasArg) {
            snprintf(opt.diskPath[1], sizeof opt.diskPath[1], "%s", argv[++i]);
        } else if (!strcmp(a, "-h") && hasArg) {
            snprintf(opt.hdfPath, sizeof opt.hdfPath, "%s", argv[++i]);
        } else if (!strcmp(a, "--write-back")) {
            opt.writeBack = true;
        } else if (!strcmp(a, "--read-only")) {
            opt.readOnly = true;
        } else if (!strcmp(a, "--no-wait")) {
            opt.noWait = true;
        } else if (!strcmp(a, "--sram") && hasArg) {
            snprintf(opt.sram, sizeof opt.sram, "%s", argv[++i]);
            opt.sramGiven = true;
        } else if (!strcmp(a, "--no-sram")) {
            opt.useSram = false;
        } else if (!strcmp(a, "--screenshot") && hasArg) {
            opt.screenshot = argv[++i];
        } else if (!strcmp(a, "--type") && hasArg) {
            unescape(argv[++i], opt.typeText, sizeof opt.typeText);
        } else if (!strcmp(a, "--message") && hasArg) {
            unescape(argv[++i], opt.messageText, sizeof opt.messageText);
        } else if (!strcmp(a, "--message-sec") && hasArg) {
            if (!parseCount(argv[++i], opt.messageSec) || opt.messageSec > (long)platform::MSG_SEC_MAX) {
                fprintf(stderr, "--message-sec: seconds from 0 to %u: %s\n", platform::MSG_SEC_MAX, argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--message-dump") && hasArg) {
            opt.messageDump = argv[++i];
        } else if (!strcmp(a, "--type-after") && hasArg) {
            if (!parseCount(argv[++i], opt.typeAfter)) {
                fprintf(stderr, "--type-after: not a number: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--no-audio")) {
            opt.noAudio = true;
        } else if (!strcmp(a, "--wav") && hasArg) {
            opt.wav = argv[++i];
        } else if (!strcmp(a, "--time") && hasArg) {
            const char* v = argv[++i];
            if (!platform::parseTimeArg(v, opt.startTime)) {
                fprintf(stderr, "--time: want seconds since 1970-01-01 00:00:00 local time (0 or more), got '%s'\n", v);
                return 2;
            }
        } else if (!strcmp(a, "--dump-text")) {
            opt.dumpText = true;
        } else if (!strcmp(a, "--test-pattern")) {
            opt.testPattern = true;
        } else if (!strcmp(a, "--joy-keys")) {
            opt.joyKeys = true;
        } else if (!strcmp(a, "--sram-memsize") && hasArg) { // 設定ファイルの sram_memsize= と同じ値
            platform::Settings tmp;
            if (platform::applySetting(tmp, "sram_memsize", argv[++i]) != platform::SettingResult::OK) {
                fprintf(stderr, "--sram-memsize: must be fix or keep: %s\n", argv[i]);
                return 2; // fix と keep のほかは受け付けない
            }
            opt.fixSramMemorySize = tmp.fixSramMemsize != 0;
        } else if (!strcmp(a, "--cpu") && hasArg) {
            opt.cpu = atoi(argv[++i]);
            if (!x68k::Config().setCpu(opt.cpu)) {
                fprintf(stderr, "--cpu: must be 68000, 68030 or 68060: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--ipl") && hasArg) {
            opt.ipl = argv[++i];
        } else if (!strcmp(a, "--high-mem") && hasArg) {
            opt.highMemMB = atoi(argv[++i]);
            if (opt.highMemMB < 0 || opt.highMemMB > 16) {
                fprintf(stderr, "--high-mem: must be 0..16: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--max-speed")) {
            opt.maxSpeed = true;
        } else if (!strcmp(a, "--bus-sync-sprite")) {
            opt.busSyncSprite = true;
        } else if (!strcmp(a, "--speed060") && hasArg) {
            opt.speed060 = atoi(argv[++i]);
            if (opt.speed060 < 5 || opt.speed060 > 3200) {
                fprintf(stderr, "--speed060: must be 5..3200: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--max-speed-limit") && hasArg) {
            opt.maxSpeedLimit = atoi(argv[++i]);
            if (opt.maxSpeedLimit < 0 || opt.maxSpeedLimit > 3200) {
                fprintf(stderr, "--max-speed-limit: must be 0..3200: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--local-mem") && hasArg) {
            opt.localMemMB = atoi(argv[++i]);
            if (opt.localMemMB < 0 || opt.localMemMB > 256) {
                fprintf(stderr, "--local-mem: must be 0..256: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--ram") && hasArg) {
            opt.ramMB = atoi(argv[++i]);
            if (opt.ramMB < 1 || opt.ramMB > 12) {
                fprintf(stderr, "--ram: must be 1..12: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--clock") && hasArg) {
            opt.clockMHz = atoi(argv[++i]);
            if (opt.clockMHz != 10 && opt.clockMHz != 16 && !x68k::Config::valid060MHz(opt.clockMHz)) {
                fprintf(stderr, "--clock: must be 10, 16 (68000) or 50, 75 (68060): %s\n", argv[i]);
                return 2;
            }
            clockGiven = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    // メニューでイメージを選ぶディレクトリ: 設定ファイルのディレクトリ、なければ ~/.cache/pix68k/disk
    if (!opt.imageDir[0]) {
        if (opt.config[0] && !opt.configDefault) dirName(opt.config, opt.imageDir, sizeof opt.imageDir);
        else snprintf(opt.imageDir, sizeof opt.imageDir, "%s/.cache/pix68k/disk", home ? home : ".");
    }
    // ハイメモリは X68030 だけ (68000 のアドレスバスは 24 ビットで $01000000 から上を区別できない。060turbo は
    // ローカルメモリを使う)
    if (opt.highMemMB && opt.cpu != 68030) fprintf(stderr, "warning: --high-mem is for --cpu 68030 only; ignored\n");
    // クロックは CPU ごとに選べる値が違う (68000 は 10 と 16、68060 は 50 と 75、68030 は 25 だけ)
    if (clockGiven && platform::Settings::clockFor(opt.cpu, opt.clockMHz) != opt.clockMHz)
        fprintf(stderr, "warning: --clock %d is not for --cpu %d; using %d MHz\n", opt.clockMHz, opt.cpu,
                platform::Settings::clockFor(opt.cpu, opt.clockMHz));
    if (opt.cd[0] && opt.sc[opt.cdId][0]) {
        fprintf(stderr, "--cd: SCSI ID %d is already used by --sc\n", opt.cdId);
        return 2;
    }
    // Neptune-X の MAC アドレスと送受信の先は、ROM を読む前に確かめる (ROM がなく試験の画面を出すときも誤りにする)
    if (opt.neptune) {
        uint8_t mac[6];
        const char* arg;
        if (opt.neptuneMac[0] && !platform::parseMac(opt.neptuneMac, mac)) {
            fprintf(stderr, "neptune: bad MAC address %s (like 02:68:6b:00:00:01)\n", opt.neptuneMac);
            return 2;
        }
        const platform::NeptuneLink kind = platform::parseNeptuneLink(opt.neptuneLink, arg);
        if (kind == platform::NeptuneLink::BAD) { // eth はベアメタル版の値なので、ホスト版は echo にする (host_ether.cpp)
            fprintf(stderr, "neptune: bad link '%s' (echo, loopback, pcap:IF, tap:NAME)\n", opt.neptuneLink);
            return 2;
        }
        if (kind == platform::NeptuneLink::ETH)
            fprintf(stderr, "neptune: link eth is for the bare metal build; using echo (use pcap:IF for a host interface)\n");
    }
    // RS-232C の相手も、ROM を読む前に確かめる
    {
        unsigned port;
        char addr[64];
        if (platform::parseRs232c(opt.rs232c, port, addr, sizeof addr) == platform::Rs232cLink::BAD) {
            fprintf(stderr, "rs232c: bad value '%s' (off, tcp:PORT or tcp:ADDR:PORT)\n", opt.rs232c);
            return 2;
        }
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // ゲームコントローラは使えなくても動かす
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "warning: game controllers are not available: %s\n", SDL_GetError());
    int rc;
    if (opt.testPattern) {
        rc = runTestPattern(opt.maxFrames);
    } else {
        do {
            // X68030 と 060turbo の SRAM は X68000 のものと分ける (メモリの容量などが違う)
            if (!opt.sramGiven)
                snprintf(opt.sram, sizeof opt.sram, "%s/.cache/pix68k/%s", home ? home : ".",
                         opt.cpu == 68030 ? "sram30.dat" : opt.cpu == 68060 ? "sram060.dat" : "sram.dat");
            rc = runMachine(opt);
        } while (rc == kRestart);
    }
    SDL_Quit();
    return rc;
}
