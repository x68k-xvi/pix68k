#include "x68k/machine.h"
#include "x68k/state.h"

#include <string.h>

namespace x68k {

// 確保の失敗を ok() で返すため、new が 0 を返す環境 (Circle) では -fcheck-new を付けてビルドする
// (付けないとコンパイラは new の結果を 0 でないものとみなし、下の検査を消してしまう)
Machine::Machine(const Config& config)
    : config_(config), cpu_(this), keyboard_(config.cpuHz), scc_(config.cpuHz), mouse_(config.cpuHz) {
    scc_.setListener(&mouse_);
    if (config_.ramSize > 0xc00000) config_.ramSize = 0xc00000;
    if (config_.ramSize < 0x100000) config_.ramSize = 0x100000;
    config_.ramSize &= ~0xfffffu;
    ram_ = new uint8_t[config_.ramSize];
    ipl_ = new uint8_t[IPL_SIZE];
    cg_ = new uint8_t[CG_SIZE];
    video_ = new Video;
    if (ram_) memset(ram_, 0, config_.ramSize);
    if (ipl_) memset(ipl_, 0xff, IPL_SIZE);
    if (cg_) memset(cg_, 0, CG_SIZE);
#ifndef PIX68K_NO_CPU020
    if (config_.cpuModel != m68k::CpuModel::M68000) {
        cpu020_ = new m68k::Cpu020(this, config_.cpuModel);
        if (cpu020_ && !cpu020_->fpu()) { // FPU が確保できなかった
            delete cpu020_;
            cpu020_ = nullptr;
        }
        if (config_.cpuModel == m68k::CpuModel::M68060) {
            config_.highMemSize = 0;
            if (config_.localMemSize > Config::LOCAL_MEM_MAX) config_.localMemSize = Config::LOCAL_MEM_MAX;
            config_.localMemSize &= ~0xfffffu;
            if (config_.localMemSize) {
                localMem_ = new uint8_t[config_.localMemSize];
                if (localMem_) memset(localMem_, 0, config_.localMemSize);
            }
            if (cpu020_) {
                // $10000000-$1FFFFFFF のうち SIMM のないところは何もつながっていない (Cpu020::setNoMemory)
                cpu020_->setNoMemory(Config::LOCAL_MEM_BASE, 0x10000000);
                cpu020_->setBusWait(config_.busWait060);
                cpu020_->setClock060(config_.cpuHz);
                if (config_.speed060 < Config::SPEED060_MIN) config_.speed060 = Config::SPEED060_MIN;
                if (config_.speed060 > SPEED_MAX) config_.speed060 = SPEED_MAX;
                cpu020_->setSpeed(config_.speed060);
            }
        } else {
            config_.localMemSize = 0;
        }
        if (config_.highMemSize > 0x1000000) config_.highMemSize = 0x1000000;
        config_.highMemSize &= ~0xfffffu;
        if (config_.highMemSize) {
            highMem_ = new uint8_t[config_.highMemSize];
            if (highMem_) memset(highMem_, 0, config_.highMemSize);
        }
    } else {
        config_.highMemSize = 0;
        config_.localMemSize = 0;
    }
#else
    config_.cpuModel = m68k::CpuModel::M68000;
    config_.highMemSize = 0;
    config_.localMemSize = 0;
#endif
    memset(sram_, 0, sizeof sram_);
    sysPort_.cpuInfo = config_.cpuInfo;
    uint32_t a = 4000000, b = config_.cpuHz; // 最大公約数で約分する
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    mfpMul_ = 4000000 / a;
    mfpDiv_ = config_.cpuHz / a;
    setupBusWaits();
    updateFastPages();
}

// 68000 の機種のバスのウェイト。CPU は読み書き 1 回を 4 クロックで数える (m68k::Bus) ので、それより延びる分を
// 読み書き 1 回ごとに busWait() で足す。1/4 クロック単位で持ち、端数は次の読み書きに持ち越す (平均の値を入れるため)。
// 68030 / 68060 では、この表のウェイトは入れない。X68030 と 060turbo は、I/O の読み書きを 1.6 マイクロ秒ごとの区切りに
// そろえる (Config::busArea の場所の種類ごとの表。この関数の始めで CPU のサイクル数に直す)。060turbo は、その前に busWait060 も足す。
//
// 値の出どころ:
// - cpupower.r (TOMY さん、1992。X68000 LIBRARY で配布) の説明書にある、実機の XVI で測った値。cpupower.r は
//   MFP のタイマ C (50us ごと) で、MOVE.B (A1),D4 を 11 x 4001 回読むのにかかる時間を測る。説明書の値から、
//   この命令 1 回の長さは XVI の 10MHz モードで RTC ($E8A001) が約 10.1 クロック、グラフィック VRAM が約 9.1、
//   テキスト VRAM が約 9.6、16MHz モード (16.67MHz) で約 15.2、10.2、10.4 クロックになる。ウェイトがなければ 8 クロック
//   なので、DRAM リフレッシュの分 (refreshWait()。ウェイトでない 8 クロックが 2.2% 延びて約 8.18 クロック) を除くと、
//   ウェイトは 10MHz モードで RTC 約 1.94、グラフィック VRAM 約 0.94、テキスト VRAM 約 1.44、16MHz モードで約 7.03、2.03、
//   2.18 クロック。それぞれ 1/4 クロック単位で一番近い値 (2、1、1.5、7、2、2.25) を入れた。
//   I/O は「クロックによらず約 1us」と説明書にあるが、これは命令全体の長さで、ウェイトのクロック数は 16MHz モードで増える。
// - MFP: X68000 XVI の 10MHz で約 4 クロック、16.7MHz で約 15 クロック (Makoto Kamada 氏、2015-09-29 の投稿。
//   GPIP の読み出しの平均。posfie「X68000 の挙動いろいろ」にまとめられている)
// - ADPCM: 読み出し 2、書き込み約 3.8 クロック。スプライト (CYNTHIA): $EB0000-$EB03FF 約 3.9、$EB8000-$EBFFFF は
//   表示を止めていると約 3.7 (GIMONS 氏、2015-10-12 と 10-13 の投稿。同じまとめ。クロックは書かれていないので 10MHz の値とした)
// - SRAM と ROM: 1 クロック (XEiJ の説明書の更新履歴「X68000 で SRAM にアクセスするときウェイトが 1 サイクル入ります」と、
//   si-ee の si_mpuclk.s の注釈「12 は dbra + メモリアクセスウェイトの動作クロック数」「ROM と同じウェイトの SRAM」)
// 16MHz モードでの ADPCM とスプライトの値は見つからなかったので、ほかの I/O と同じ 7 クロックとした (見積もり)。
// SRAM と ROM の 16MHz での値も見つからなかったので 1 クロックのまま (si-ee はどのクロックでも 12 クロックとして計算する)。
// 拡張スロットのボードとそのほかの I/O は RTC と同じ値にした。
// pix68k の 16MHz は 16.0MHz (実機の XVI は 16.67MHz) なので、ウェイトはクロック数で合わせている。
// 10MHz の表と 16MHz の表は Config::cpuHz (クロックの設定。XVI の 10MHz / 16MHz モードに当たる) で選ぶ。
//
// 最高速モード (setCpuSpeed() で 100 以外。runCpu000Scaled()) では、ウェイトとリフレッシュの遅れも命令のサイクル数と
// 同じく CPU のサイクル数として足すので、速さを上げるとその時間も同じ割合で縮む (表は Config::cpuHz のまま変えない)。
// 実機をクロックアップしたときとは違う (cpupower.r の説明書の例では、16MHz モードで 24MHz にすると I/O は 128.57% で、
// クロックほどは速くならない) が、そのままにする。最高速モードはホストが間に合う範囲で CPU を速く動かすためのもので、
// クロックアップした実機の速さを再現するものではない。I/O のウェイトを実時間のまま残すと、I/O を多く読み書きする
// プログラムほど速さが上がらなくなる。68030 / 68060 の setSpeed() も、命令のサイクル数をまとめて減らしている
void Machine::setupBusWaits() {
    busWaits_ = config_.busWait68000 && config_.cpuModel == m68k::CpuModel::M68000;
    memset(waitRead_, 0, sizeof waitRead_);
    memset(waitWrite_, 0, sizeof waitWrite_);
    for (auto& a : busWaitSub_) a[0] = a[1] = BusWaitSub();
    memset(busAreaOf_, 0, sizeof busAreaOf_);
    busSyncKeepsPhase_ = true;
#ifndef PIX68K_NO_CPU020
    if (cpu020_ && config_.cpuModel != m68k::CpuModel::M68000) {
        // ns を CPU のサイクル数に直す (1/SUB サイクル単位。1600ns は 68030 の 25MHz で 40 サイクル、68060 の 50MHz で
        // 80 サイクル、75MHz で 120 サイクル)。負の値は 0、Config::BUS_NS_MAX を超える値は BUS_NS_MAX として扱う
        auto toSub = [&](int ns) { return Config::busNsToSub(config_.cpuHz, ns); };
        bool used[Config::BUS_AREAS] = {};
        for (int i = 1; i < Config::BUS_AREAS; i++) {
            for (int w = 0; w < 2; w++) {
                const Config::BusTiming& t = w ? config_.busArea[i].write : config_.busArea[i].read;
                BusWaitSub& s = busWaitSub_[i][w];
                s.pre = toSub(t.pre);
                s.period = toSub(t.period);
                s.post = toSub(t.post);
                if (s.pre || s.period || s.post) used[i] = true;
                // 時刻を直す量 (STATE_TIME_SHIFT_UNIT サイクルの倍数) が周期の倍数でないと、直したときに区切りの位置が動く
                if (s.period && STATE_TIME_SHIFT_UNIT * m68k::Cpu020::SUB % s.period) busSyncKeepsPhase_ = false;
            }
        }
        for (uint32_t a = BUS_AREA_BASE; a < BUS_AREA_END; a += 1u << Config::BUS_AREA_SHIFT) {
            const Config::BusArea area = Config::busAreaAt(a);
            if (used[area]) busAreaOf_[(a - BUS_AREA_BASE) >> Config::BUS_AREA_SHIFT] = area;
        }
    }
#endif
    if (!busWaits_) return;
    const bool xvi16 = config_.cpuHz >= 16000000; // XVI の 16MHz モード
    // 1/4 クロック単位
    const uint8_t gvram = xvi16 ? 8 : 4;  // 2 / 1
    const uint8_t tvram = xvi16 ? 9 : 6;  // 2.25 / 1.5
    const uint8_t io = xvi16 ? 28 : 8;    // 7 / 2
    const uint8_t mfp = xvi16 ? 60 : 16;  // 15 / 4
    const uint8_t rom = 4;                // 1 (SRAM も)
    auto fill = [&](uint32_t start, uint32_t end, uint8_t r, uint8_t w) {
        for (uint32_t a = start; a < end; a += 0x2000) {
            waitRead_[a >> 13] = r;
            waitWrite_[a >> 13] = w;
        }
    };
    fill(0xc00000, 0xe00000, gvram, gvram);
    fill(0xe00000, 0xe80000, tvram, tvram);
    fill(0xe80000, 0xf00000, io, io);
    fill(0xe88000, 0xe8a000, mfp, mfp);
    if (!xvi16) {
        fill(0xe92000, 0xe94000, 8, 15); // ADPCM
        fill(0xeb0000, 0xeb8000, 16, 16); // スプライトのスクロールレジスタと制御レジスタ
        fill(0xeb8000, 0xec0000, 15, 15); // PCG とテキストエリア (BG)
    }
    fill(0xed0000, 0xed4000, rom, rom); // SRAM
    fill(0xf00000, 0x1000000, rom, rom); // CGROM、SCSI の ROM、IPL ROM
}

// CPU が Bus を通さずに読み書きするページ (64KB ごと) を決める。副作用のないメモリだけ:
// メイン RAM (読み書き)、テキスト VRAM (読み出しだけ。書き込みは同時アクセスなどがある)、ROM (読み出しだけ)。
// ユーザモードでは、エリアセットで守られた RAM の先頭にかかるページは Bus を通す (バスエラーにするため)
void Machine::setFastPage(bool supervisor, int page, const uint8_t* read, uint8_t* write) {
#ifndef PIX68K_NO_CPU020
    if (cpu020_) {
        cpu020_->setFastPage(supervisor, page, read, write, false); // TLB は updateFastPages() の最後で消す
        return;
    }
#endif
    cpu_.setFastPage(supervisor, page, read, write);
}

void Machine::updateFastPages() {
    cpu_.clearFastPages();
#ifndef PIX68K_NO_CPU020
    if (cpu020_) {
        cpu020_->clearFastPages();
        if (highMem_) cpu020_->setExtMemory(0x01000000, config_.highMemSize, highMem_);
        if (localMem_) cpu020_->setExtMemory(Config::LOCAL_MEM_BASE, config_.localMemSize, localMem_);
    }
#endif
    if (!ok()) return;
    const uint32_t userStart = ((uint32_t)areaSet_ + 1) * 0x2000;
    for (uint32_t a = 0; a < config_.ramSize; a += 0x10000) {
        setFastPage(true, (int)(a >> 16), ram_ + a, ram_ + a);
        if (a >= userStart) setFastPage(false, (int)(a >> 16), ram_ + a, ram_ + a);
    }
    // 68000 でウェイトを入れるときは、テキスト VRAM と ROM の読み出しも Bus を通す (busWait() でウェイトを足すため)
    if (busWaits_) return;
    for (int s = 0; s < 2; s++) {
        for (uint32_t a = 0; a < Video::TVRAM_SIZE; a += 0x10000)
            setFastPage(s, (int)((0xe00000 + a) >> 16), video_->tvram() + a, nullptr);
        for (uint32_t a = 0; a < CG_SIZE; a += 0x10000) setFastPage(s, (int)((0xf00000 + a) >> 16), cg_ + a, nullptr);
        if (scsiRom_)
            for (uint32_t a = 0; a < SCSI_ROM_SIZE; a += 0x10000)
                setFastPage(s, (int)((0xfc0000 + a) >> 16), scsiRom_ + a, nullptr);
        for (uint32_t a = 0; a < IPL_SIZE; a += 0x10000) setFastPage(s, (int)((0xfe0000 + a) >> 16), ipl_ + a, nullptr);
    }
#ifndef PIX68K_NO_CPU020
    if (cpu020_) cpu020_->flushTlb();
#endif
}

Machine::~Machine() {
#ifndef PIX68K_NO_CPU020
    delete cpu020_;
    delete[] highMem_;
    delete[] localMem_;
#endif
    delete[] ram_;
    delete[] ipl_;
    delete[] cg_;
    delete[] scsiRom_;
    delete[] scsiExRom_;
    for (FpuBoard* b : fpuBoards_) delete b;
    delete mercury_;
    delete ts6bga_;
    delete neptune_;
    delete midi_;
    delete video_;
}

bool Machine::setIplRom(const uint8_t* data, size_t size) {
    if (size != IPL_SIZE || !ipl_) return false;
    memcpy(ipl_, data, size);
    if (config_.cpuModel == m68k::CpuModel::M68060) patchIplFor060();
    return true;
}

// 060turbo では 68060 で動く IPL ROM (XEiJ の IPLROM 1.6 など、バージョン 1.4 以降) を使う。X68030 の IPL ROM (1.3) を
// 渡されたときは、メモリの上で手を入れて代わりに使う (ROM のファイルは変えない):
// - 060turbo.sys が見る IOCS _ROMVER の値を 1.4 にする (060turbo.sys は上位バイトが $14 以上でなければ組み込まれない)
// - IPL ROM の中の 68060 にない命令 (MOVEP など) を、ベクタ 61 が IPL ROM を指している間はその場で実行する
//   (Cpu020::setUnimplementedAssist の ASSIST_ROM)。060turbo.sys が 060SP を組み込んだ後は、060SP が実行する
// - 起動時に IOCS のワークの MMU の有無 ($CBE) を「あり」にする。IPL ROM 1.3 は 68030 の命令で MMU を調べるので、
//   68060 では「なし」になり、IOCS _SYS_STAT ($AC) の MPU の状態が MMU なし (68EC060) になる
// - 起動時に IOCS のワークの $0CB8 (ROM の上の dbra のループの 1ms あたりの回数) を、実機の 060turbo の値 8333 にする
//   (75MHz では、クロックに比例させた 12500)
// - IPL ROM が CACR に書く 68030 の値 (命令キャッシュ、データキャッシュの有効) を、68060 の CACR の値に読み替える
// - SCSI IOCS のデータの転送 (_S_DATAIN / _S_DATAOUT と、それを使う _S_READ / _S_WRITE / _S_READEXT / _S_WRITEEXT など) を、
//   バッファが本体の 16MB の外 (ローカルメモリ) にかかるときはソフト転送 (_S_DATAINI / _S_DATAOUTI と同じ処理) にする。
//   DMAC は本体のバスの 24 ビットのアドレスしか出せないので、IPL ROM 1.3 のまま DMA で転送すると、ローカルメモリの
//   $1xxxxxxx の代わりに本体の $00xxxxxx を読み書きする (060turbo.sys の拡張モードは Human68k のメモリをローカルメモリに
//   広げるので、COMMAND.X を読み込む先がローカルメモリになり、例外ベクタの表が壊れて止まる)。060turbo.sys は、
//   SCSI IOCS が IPL ROM の中 ($00FFxxxx) にあるときは -ss を付けても SCSI IOCS を差し替えないので、ROM の側で避ける
// フロッピーディスクの読み書き (DMAC のチャネル 0) には、IPL ROM にソフト転送がない。こちらは ROM を書き換えず、
// チャネル 0 のメモリ側のアドレスがローカルメモリの中なら、DMAC の転送をローカルメモリへ届ける (dmaMemAddr)。
// 実機の DMAC は本体のバスにいて、ローカルメモリには届かない。これも 060turbo で使うための互換の処理で、
// X68000 の側のメモリは使わない (スタックや作業用のバッファを取らないので、Human68k や 060turbo.sys が使うメモリと重ならない)。
// フロッピーディスクと内蔵 SCSI (チャネル 0 と 1) の扱いは、IPL ROM 1.3 でないとき (68060 で動く IPL ROM 1.6 など) にもする。IPL ROM 1.6 も
// フロッピーディスクの転送はチャネル 0 の DMA のままで、-xm で COMMAND.X を読み込む先がローカルメモリになると、
// 本体の $00xxxxxx を書いて止まる (pc=$273D7818 で停止。#124)
// 内蔵 SCSI と SASI (チャネル 1) も、68060 のときはいつも同じ扱いにする (#167)。IPL ROM 1.6 と組で使う $FC0000 からの
// ROM (SCSIINROM 16) の SCSI IOCS は、バッファがローカルメモリでも DMA で転送する (-xm で起動したときに、PC がその
// ROM の中にある間にチャネル 1 が MAR = $100000EA で始まることを、動かして確かめた)。-xm で COMMAND.X をローカルメモリへ
// 読み込むと、本体の $0000EA からの例外ベクタを書いて止まっていた。IPL ROM 1.3 のときは上の SCSI IOCS の書き換えでソフト転送に
// なるので、チャネル 1 にローカルメモリのアドレスは来ない
// 060turbo.sys には -dv (デバイスドライバの転送を、DMA の届く転送バッファ経由にする。大きさは -bs、既定 64KB) があり、
// -xm -dv なら、この互換の処理がなくても IPL ROM 1.3 でも 1.6 でも起動する (060turbo.sys 0.56 の逆アセンブルで、
// デバイスドライバの読み書きを包み、PLPAR / PLPAW で物理アドレスを求めてローカルメモリの範囲なら転送バッファを使うことを
// 確かめた)。実機は -xm だけではフロッピーディスクから起動しない見込み (逆アセンブルからの推定で、実機では確かめていない)。
// この互換の処理は -dv の代わりで、-dv を付けなくても起動できるようにするためのもの (#94)
// SCSI も同じ: 060turbo.sys の SCSI IOCS を包む処理は、-ss (と -np) で立つフラグが 0 なら何もせずに元の SCSI IOCS を
// 呼ぶ (0.56 を逆アセンブルして確かめた)。オプションを解釈する処理では、0.56 も 0.61 も、-xm はこのフラグも -dv の
// フラグも立てない。IPL ROM 1.6 と SCSIINROM 16 では、-xm -ss でも -xm -dv でも、チャネル 1 の互換の処理なしで SCSI の
// ディスクから起動した (フラグが立ったときの処理の中身は読んでいない)。-xm だけのときは、実機でも本体の $00xxxxxx を
// 書いて止まる見込み (逆アセンブルからの推定で、実機では確かめていない)
// SCSI の書き換えと、フロッピーディスクとチャネル 1 の扱いは、060turbo の ROM の動きを写したものではなく、エミュレータの
// 側の互換の処理 (SCSI の書き換えは IPL ROM 1.3 のときだけ、フロッピーディスクとチャネル 1 は 68060 のときいつも)。060turbo の ROM や、IPL ROM 1.6 を使う
// 実機・XEiJ がフロッピーディスクのデータをどうローカルメモリへ届けているかは確かめていない (060turbo.sys の readme には
// 中継のバッファを使う話はない)。060turbo.sys が SCSI IOCS を
// 差し替えないことは、060turbo.sys 0.61 (2025-10-31) の逆アセンブルで確かめた: IOCS のベクタを差し替える処理の中で、
// _SCSIDRV のベクタ ($7D4) の上位ワードが $00FF (IPL ROM の中) か $F5FF (未使用) なら _SCSIDRV の差し替えを飛ばす。
// -xm -ss で起動しても同じところで止まる。060turbo.sys 0.56 にはこの判定がない (0.56 の -ss の動きは確かめていない)
// X68030 の IPL ROM の _ROMVER ($FF0030: move.l #$13921127,d0 / rts) でなければ、フロッピーディスクとチャネル 1 の扱い
// (dmaMemAddr) のほかはどれもしない。
// 命令を書き換えるところは、元の命令が思ったとおりのときだけ書き換える
void Machine::patchIplFor060() {
    static const uint8_t kRomver30[] = {0x20, 0x3c, 0x13, 0x92, 0x11, 0x27, 0x4e, 0x75};
    uint8_t* p = ipl_ + (0xff0030 - 0xfe0000);
    const bool rom13 = memcmp(p, kRomver30, sizeof kRomver30) == 0;
    if (cpu020_) cpu020_->setUnimplementedAssist(rom13 ? m68k::Cpu020::ASSIST_ROM : m68k::Cpu020::ASSIST_OFF);
    if (!rom13) return;
    p[2] = 0x14;
    // [at, at + n) が want と同じなら with に書き換える (定数の memcpy を避けて 1 バイトずつ写す)
    auto same = [this](uint32_t at, const uint8_t* want, int n) {
        for (int i = 0; i < n; i++)
            if (ipl_[at - 0xfe0000 + (uint32_t)i] != want[i]) return false;
        return true;
    };
    auto put = [this](uint32_t at, const uint8_t* with, int n) {
        for (int i = 0; i < n; i++) ipl_[at - 0xfe0000 + (uint32_t)i] = with[i];
    };
    // $FF0124: tst.l d7 / smi (a0)+ (a0 = $CBE。d7 のビット 31 が MMU の有無) → tst.l d7 / st (a0)+
    static const uint8_t kMmuOld[] = {0x4a, 0x87, 0x5b, 0xd8}, kMmuNew[] = {0x4a, 0x87, 0x50, 0xd8};
    if (same(0xff0124, kMmuOld, 4)) put(0xff0124, kMmuNew, 4);
    // IOCS のワークの $0CB8 (IPL ROM が起動時に測る、ROM の上の dbra のループの 1ms あたりの回数。IOCS の待ちのループと
    // _SYS_STAT が使う) を 8333 にする。実機の 060turbo (50MHz) は si.r が 50.0MHz と表示し、si.r は $0CB8 x 6 を MHz の
    // 1000 倍として表示するので、実機の値は 8333 (8333 x 6 = 49998)。060turbo.sys 0.56 は $0CB8 を読むだけで書かないので、
    // 060turbo の ROM が入れた値と考えられる。IPL ROM 1.3 が測った値を $0CB8 / $0CBA に書くところ
    // ($FF0198: swap d2 / move.l d2,(a0)。a0 = $CB8) を、ROM の空き ($FF0830) に置いた次の処理の呼び出しにする:
    //   swap d2 / move.l d2,(a0) / move.w #8333,(a0) / rts   ($0CBA は測った値のまま)
    // 8333 は 50MHz の値。ほかのクロック (75MHz) では cpuHz / 6000 (si.r の表示が選んだクロックになる値。75MHz で 12500)
    // にする。dbra の 1 周は 68060 の 6 サイクルとした値 (8333 と同じ) なので、クロックに比例させれば、IOCS の待ちの
    // ループの 1ms あたりの回数も同じ比で合う (下の divu.w #583 はクロックによらない)。75MHz の実機の 060turbo で
    // $0CB8 がいくつになるかは確かめていない (060turbo の ROM が決まった値を入れるなら、8333 のままの可能性もある)
    static const uint8_t kClkOld[] = {0x48, 0x42, 0x20, 0x82}, kClkCall[] = {0x61, 0x00, 0x06, 0x96};
    const uint32_t loops = config_.cpuHz / 6000; // 50MHz で 8333 (切り捨て)
    const uint8_t kClkStub[] = {0x48, 0x42, 0x20, 0x82, 0x30, 0xbc, (uint8_t)(loops >> 8), (uint8_t)loops, 0x4e, 0x75};
    bool clkOk = same(0xff0198, kClkOld, 4);
    for (uint32_t a = 0xff0830; a < 0xff0830 + sizeof kClkStub; a++)
        if (ipl_[a - 0xfe0000] != 0xff) clkOk = false;
    if (clkOk) {
        put(0xff0830, kClkStub, (int)sizeof kClkStub);
        put(0xff0198, kClkCall, 4);
    }
    // CACR の読み替え。IPL ROM 1.3 は 68030 の CACR の値 (ビット 0 = EI 命令キャッシュ、ビット 8 = ED データキャッシュ) を
    // 書く。68060 の CACR ではこの 2 つのビットに意味がなく、命令キャッシュは EIC (ビット 15)、データキャッシュは EDC
    // (ビット 31) なので、そのままではキャッシュが無効になる。060turbo の ROM がどうしているかは資料がなく、ここは
    // 「68030 の EI / ED を 68060 の EIC / EDC に読み替える」という推定の手当て。書き換えるのは次の 4 か所:
    //   $FF014E  起動時の movec d0,cacr (d0 = $2101: EI、ED、WA)
    //   $FFC7CC  IOCS _SYS_STAT のキャッシュの設定 (movec d1,cacr / rts。値は $0000 / $0001 / $2100 / $2101)
    //   $FFC78C  IOCS _SYS_STAT のキャッシュの状態の取得 (movec cacr,d1 のあと、ビット 0 と 8 を見る)
    //   $FFAC82  IOCS _MS_ONTM / _MS_OFFTM の待ちのループの前の movec d0,cacr (d0 = 1: 命令キャッシュだけ有効。
    //            ループの回数は $0CB8 から決まり、終わったら元の CACR に戻す)。ここだけは EIC と EDC の両方を立てる。
    //            X68030 の本体のメモリは 68030 と同じバスにあり、データキャッシュを切ったときの読み込みの遅れは小さいと
    //            考えた (これも推定。68030 でもキャッシュに当たればバスサイクルは要らないので、遅れがないわけではない)。
    //            060turbo の 68060 では、本体のメモリの読み込みが、本体のバスに合わせる時間だけ遅くなる。ループは 1 周に
    //            本体のメモリを 6 回読むので、ループの時間の $0CB8 に対する比を X68030 に近く保つには、データキャッシュも
    //            有効にするのが近い。次の値は、68060 のサイクル数を MC68060UM の表から作る変更 (PR #96) と合わせた
    //            ビルドで測ったもの (この変更だけでは、CACR は命令の速さに関係しない): 待ちの 1 単位 (決めた長さは 1ms) は、
    //            読み替えなしで約 5.96ms、EIC だけで約 2.59ms、EIC と EDC で約 0.70ms、キャッシュをすべて有効にして約 0.38ms
    //            (どれも、下の回数の書き換えの前の値)
    //   $FFAC6E、$FFACDA  同じ待ちのループの回数を決める divu.w #834,d2 (1 単位の周の数 = $0CB8 x 100 / 834)。834 は、
    //            ループの 1 周 (68030 で 50 クロック) が、$0CB8 の数えている dbra の 1 周 (6 クロック) の 8.34 倍という
    //            意味で、1 単位が 1ms になる。68060 では、上の EIC と EDC の設定でループの 1 周が約 35.0 サイクル
    //            (MC68060UM の表から作ったサイクル数で測った値。1000 単位で 698.9ms) で、$0CB8 = 8333 は dbra の 1 周を
    //            6 サイクルとした値なので、比は 5.83 倍。834 のままだと 1 単位が約 0.70ms になるので、583 に書き換えて
    //            1 単位を 1ms にする (IOCS の呼び出しの決まりの長さに合わせる)。実機の 060turbo は ROM が違い
    //            (IPL ROM 1.50)、060turbo.sys 0.56 は _MS_ONTM / _MS_OFFTM を差し替えない (組み込んだあとも IOCS の
    //            ベクタは ROM の $FFAC26 / $FFAC3A のまま) ので、実機の待ちの長さは ROM 1.50 の作りで決まる。その作りと
    //            実機で測った値は見つかっていない。実機の値が分からないので、決まりの長さに合わせた。
    //            tests/x68k/iocs_wait060.cpp が、1000 単位が 950ms から 1050ms に入ることを確かめる
    // 実機 (060turbo.sys がキャッシュを設定した 68060) と違うところ:
    //   - 起動時と _SYS_STAT の設定は EIC と EDC だけを決め、ストアバッファ (ESB) と分岐キャッシュ (EBC) は有効にしない
    //     (060turbo.sys を組み込むと、060turbo.sys が自分の _SYS_STAT で設定し直す)
    //   - 待ちのループの間は CACR が $80008000 になるので、ESB と EBC は落ちる (終わると元の値に戻る)
    //   - $0808 を立てて書く所は、68060 ではキャッシュを消さない (68060 でキャッシュを消すのは CINV / CPUSH)
    // 0 を書く所 ($FF0070、$FF0338、$FF0382) と、キャッシュを消すために $0808 を立てて書き戻す所 ($FF8296、$FFC7B0 など) は、
    // 68060 でも同じ結果になるので変えない。$FE0A36-$FE168A (ROM デバッガ) も変えない。
    // 060turbo.sys 0.61 は IOCS _SYS_STAT を差し替えるので、組み込んだあとは $FFC7CC と $FFC78C は使われない。
    // _MS_ONTM / _MS_OFFTM は差し替えない (逆アセンブルとシンボル表で確かめた)。
    // ROM の終わりの空き ($FFEE40 から) に置く処理:
    //   conv ($FFEE40): d0 の 68030 の値を読み替えて CACR に書く (d0、d1 は変えない)
    //     move.l d1,-(sp) / moveq #0,d1 / btst #0,d0 / beq.s +4 / bset #15,d1 / btst #8,d0 / beq.s +4 / bset #31,d1
    //     movec d1,cacr / move.l (sp)+,d1 / rts
    //   convD1 ($FFEE60): exg d0,d1 / jsr conv / exg d0,d1 / rts
    //   get ($FFEE70): CACR を読み、68030 の並び (ビット 0 = EIC、ビット 8 = EDC) にして d1 に入れる (d0 は変えない)
    //     movec cacr,d1 / move.l d0,-(sp) / move.l d1,d0 / moveq #0,d1 / btst #15,d0 / beq.s +4 / bset #0,d1
    //     btst #31,d0 / beq.s +4 / bset #8,d1 / move.l (sp)+,d0 / rts
    //   both ($FFEE94): move.l #$80008000,d0 / movec d0,cacr / rts   (EIC と EDC。待ちのループの前)
    // $FF014E からは遠いので、近くの空き ($FF083C) に jmp conv を置いて、そこを呼ぶ
    static const uint8_t kConv[] = {0x2f, 0x01, 0x72, 0x00, 0x08, 0x00, 0x00, 0x00, 0x67, 0x04, 0x08, 0xc1, 0x00, 0x0f,
                                    0x08, 0x00, 0x00, 0x08, 0x67, 0x04, 0x08, 0xc1, 0x00, 0x1f, 0x4e, 0x7b, 0x10, 0x02,
                                    0x22, 0x1f, 0x4e, 0x75};
    static const uint8_t kConvD1[] = {0xc1, 0x41, 0x4e, 0xb9, 0x00, 0xff, 0xee, 0x40, 0xc1, 0x41, 0x4e, 0x75};
    static const uint8_t kGet[] = {0x4e, 0x7a, 0x10, 0x02, 0x2f, 0x00, 0x20, 0x01, 0x72, 0x00, 0x08, 0x00, 0x00, 0x0f,
                                   0x67, 0x04, 0x08, 0xc1, 0x00, 0x00, 0x08, 0x00, 0x00, 0x1f, 0x67, 0x04, 0x08, 0xc1,
                                   0x00, 0x08, 0x20, 0x1f, 0x4e, 0x75};
    static const uint8_t kBoth[] = {0x20, 0x3c, 0x80, 0x00, 0x80, 0x00, 0x4e, 0x7b, 0x00, 0x02, 0x4e, 0x75};
    static const uint8_t kJmpConv[] = {0x4e, 0xf9, 0x00, 0xff, 0xee, 0x40};
    static const uint8_t kBootOld[] = {0x30, 0x3c, 0x21, 0x01, 0x4e, 0x7b, 0x00, 0x02};  // move.w #$2101,d0 / movec d0,cacr
    static const uint8_t kBootNew[] = {0x30, 0x3c, 0x21, 0x01, 0x61, 0x00, 0x06, 0xec};  // ... / bsr.w $FF083C
    static const uint8_t kSetOld[] = {0x4e, 0x7b, 0x10, 0x02, 0x4e, 0x75};               // movec d1,cacr / rts
    static const uint8_t kSetNew[] = {0x4e, 0xf9, 0x00, 0xff, 0xee, 0x60};               // jmp convD1
    static const uint8_t kGetOld[] = {0x4e, 0x7a, 0x10, 0x02};                           // movec cacr,d1
    static const uint8_t kGetNew[] = {0x61, 0x00, 0x26, 0xe2};                           // bsr.w get
    static const uint8_t kOntmOld[] = {0x70, 0x01, 0x4e, 0x7b, 0x00, 0x02};              // moveq #1,d0 / movec d0,cacr
    static const uint8_t kOntmNew[] = {0x70, 0x01, 0x61, 0x00, 0x42, 0x10};              // moveq #1,d0 / bsr.w both
    static const uint8_t kDivOld[] = {0x84, 0xfc, 0x03, 0x42}; // divu.w #834,d2
    static const uint8_t kDivNew[] = {0x84, 0xfc, 0x02, 0x47}; // divu.w #583,d2
    bool cacrOk = same(0xff014a, kBootOld, 8) && same(0xffc7cc, kSetOld, 6) && same(0xffc78c, kGetOld, 4) &&
                  same(0xffac80, kOntmOld, 6) && same(0xffac6e, kDivOld, 4) && same(0xffacda, kDivOld, 4);
    for (uint32_t a = 0xffee40; a < 0xffee94 + sizeof kBoth; a++)
        if (ipl_[a - 0xfe0000] != 0xff) cacrOk = false;
    for (uint32_t a = 0xff083c; a < 0xff083c + sizeof kJmpConv; a++)
        if (ipl_[a - 0xfe0000] != 0xff) cacrOk = false;
    if (cacrOk) {
        put(0xffee40, kConv, (int)sizeof kConv);
        put(0xffee60, kConvD1, (int)sizeof kConvD1);
        put(0xffee70, kGet, (int)sizeof kGet);
        put(0xffee94, kBoth, (int)sizeof kBoth);
        put(0xff083c, kJmpConv, (int)sizeof kJmpConv);
        put(0xff014a, kBootNew, 8);
        put(0xffc7cc, kSetNew, 6);
        put(0xffc78c, kGetNew, 4);
        put(0xffac80, kOntmNew, 6);
        put(0xffac6e, kDivNew, 4);
        put(0xffacda, kDivNew, 4);
    }
    // SCSI IOCS: _S_DATAIN ($FFD5C0) は $FFD5E8 で DMA の転送 ($FFDB76) を、_S_DATAOUT ($FFD578) は $FFD59C で
    // DMA の転送 ($FFDB20) を呼ぶ。_S_DATAINI / _S_DATAOUTI は同じ形で、ソフト転送 ($FFD260 / $FFD1AA) を呼ぶ。
    // 呼び出しを、ROM の終わりの空き ($FF が続くところ) に置いた次の処理に向ける (a1 = バッファ、d3 = バイト数):
    //   move.l d3,d0 / bne.s +4 / move.w #$0100,d0 / add.l a1,d0 / bcs.s soft / cmpi.l #$01000000,d0 / bhi.s soft
    //   jmp DMA の転送 / soft: jmp ソフト転送
    // 転送の処理は d3 = 0 を 256 バイトとして扱うので、バッファの終わりも同じ長さで求める
    static const uint32_t kStub = 0xffee00;
    static const int kStubSize = 32;
    struct Site {
        uint32_t call;      // bsr.w のアドレス
        uint32_t dma, soft; // 転送の処理
    };
    static const Site kSites[2] = {{0xffd5e8, 0xffdb76, 0xffd260}, {0xffd59c, 0xffdb20, 0xffd1aa}};
    bool ok = true;
    for (int i = 0; i < 2 * kStubSize; i++)
        if (ipl_[kStub - 0xfe0000 + (uint32_t)i] != 0xff) ok = false;
    for (const Site& s : kSites) {
        const uint32_t d = s.dma - (s.call + 2);
        const uint8_t bsr[] = {0x61, 0x00, (uint8_t)(d >> 8), (uint8_t)d};
        if (!same(s.call, bsr, 4)) ok = false;
    }
    if (!ok) return;
    for (int k = 0; k < 2; k++) {
        const Site& s = kSites[k];
        const uint32_t stub = kStub + (uint32_t)(k * kStubSize);
        const uint8_t code[kStubSize] = {
            0x20, 0x03,                                                                     // move.l d3,d0
            0x66, 0x04,                                                                     // bne.s +4
            0x30, 0x3c, 0x01, 0x00,                                                         // move.w #$0100,d0
            0xd0, 0x89,                                                                     // add.l a1,d0
            0x65, 0x0e,                                                                     // bcs.s soft
            0x0c, 0x80, 0x01, 0x00, 0x00, 0x00,                                             // cmpi.l #$01000000,d0
            0x62, 0x06,                                                                     // bhi.s soft
            0x4e, 0xf9, 0x00, (uint8_t)(s.dma >> 16), (uint8_t)(s.dma >> 8), (uint8_t)s.dma, // jmp DMA の転送
            0x4e, 0xf9, 0x00, (uint8_t)(s.soft >> 16), (uint8_t)(s.soft >> 8), (uint8_t)s.soft, // soft: jmp ソフト転送
        };
        put(stub, code, kStubSize);
        const uint32_t d = stub - (s.call + 2);
        const uint8_t bsr[] = {0x61, 0x00, (uint8_t)(d >> 8), (uint8_t)d};
        put(s.call, bsr, 4);
    }
}

bool Machine::setCgRom(const uint8_t* data, size_t size) {
    if (size != CG_SIZE || !cg_) return false;
    memcpy(cg_, data, size);
    return true;
}

void Machine::makeScsiRom30(const uint8_t* ipl, uint8_t* out) {
    // 内蔵 SCSI の ROM の先頭の形:
    //   $FC0000-$FC001F  SCSI ID 0-7 の起動の入り口 (ロング 8 個)
    //   $FC0020          初期化の入り口
    //   $FC0024          'SCSIIN'
    // X68030 では SCSI のドライバは IPL ROM にあり、IPL ROM の $FF0020 に起動の入り口、$FF0024 に初期化の
    // 入り口がある。起動の入り口の $14 バイト前には 'SCSI'、8 バイト前には 'Human68k' がある。
    // Human68k は $FC0024 の 'SCSIIN' を見て、入り口の前のこの印をたどってドライバを組み込む。
    // IPL ROM 自身の起動 (IOCS $8E で返る入り口の $14 バイト前の 'SCSI' から ID を知る) も同じ印を使う
    memset(out, 0xff, SCSI_ROM_SIZE);
    auto rd32 = [ipl](uint32_t a) {
        const uint8_t* p = ipl + (a - 0xfe0000);
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    };
    const uint32_t boot = rd32(0xff0020), init = rd32(0xff0024);
    // 入り口が IPL ROM の中を指し、'SCSI' の印があるときだけ作る。なければ $FF のまま (内蔵 SCSI なし)
    if (boot < 0xfe0014 || boot > 0xfffffc || init < 0xfe0000 || init > 0xfffffe) return;
    if (memcmp(ipl + (boot - 0x14 - 0xfe0000), "SCSI", 4) != 0) return;
    auto wr32 = [out](uint32_t off, uint32_t v) {
        out[off] = (uint8_t)(v >> 24);
        out[off + 1] = (uint8_t)(v >> 16);
        out[off + 2] = (uint8_t)(v >> 8);
        out[off + 3] = (uint8_t)v;
    };
    for (int id = 0; id < 8; id++) wr32(id * 4, boot);
    wr32(0x20, init);
    memcpy(out + 0x24, "SCSIIN", 6);
}

bool Machine::setScsiRom(const uint8_t* data, size_t size) {
    if (size == 0 || size > SCSI_ROM_SIZE) return false;
    if (!scsiRom_) scsiRom_ = new uint8_t[SCSI_ROM_SIZE];
    if (!scsiRom_) return false;
    memset(scsiRom_, 0xff, SCSI_ROM_SIZE);
    memcpy(scsiRom_, data, size);
    updateFastPages();
    return true;
}

bool Machine::setFpuBoards(int count) {
#ifdef PIX68K_NO_FPU_BOARD
    return count == 0;
#else
    if (count < 0 || count > FpuBoard::MAX_BOARDS) return false;
    bool ok = true;
    for (int i = 0; i < FpuBoard::MAX_BOARDS; i++) {
        if (i < count) {
            if (!fpuBoards_[i]) fpuBoards_[i] = new FpuBoard;
            if (!fpuBoards_[i]) ok = false;
        } else {
            delete fpuBoards_[i];
            fpuBoards_[i] = nullptr;
        }
    }
    return ok;
#endif
}

bool Machine::setMercury(bool on) {
    if (on == (mercury_ != nullptr)) return true;
    if (on) {
        mercury_ = new Mercury;
        if (!mercury_) return false;
    } else {
        delete mercury_;
        mercury_ = nullptr;
    }
    sound_.mercuryAttached(on);
    return true;
}

bool Machine::setNeptune(bool on) {
    if (on && !neptune_) neptune_ = new Neptune;
    if (!on) {
        delete neptune_;
        neptune_ = nullptr;
    }
    updateIrq();
    return !on || neptune_ != nullptr;
}

void Machine::feedCdAudio(CdDrive& cd, uint32_t clocks4M) {
    int16_t buf[2 * 256];
    int n = cd.produce(clocks4M, buf, 256);
    for (;;) {
        for (int i = 0; i < n; i++) sound_.cdFrame(&cd.mixer(), buf[2 * i], buf[2 * i + 1]);
        if (n < 256) break;
        n = cd.produce(0, buf, 256);
    }
}

bool Machine::setMidiBoard(bool on) {
    if (on && !midi_) {
        midi_ = new Ym3802;
        if (midi_) midi_->setTime(cycles());
    }
    if (!on) {
        delete midi_;
        midi_ = nullptr;
    }
    updateIrq();
    return !on || midi_ != nullptr;
}

void Machine::syncMidi(uint64_t now) {
    midi_->advanceTo(now, config_.cpuHz);
    MidiOut* const out = midiOut();
    if (__atomic_load_n(&midiResync_, __ATOMIC_RELAXED) && __atomic_exchange_n(&midiResync_, false, __ATOMIC_ACQ_REL))
        resyncMidi(out);
    for (int b; (b = midi_->takeSent()) >= 0;) {
        midi_->shadow().feed((uint8_t)b);
        sound_.midiWrite(out, (uint8_t)b);
    }
}

void Machine::resyncMidi(MidiOut* out) {
    sound_.midiReset(out);
    midi_->shadow().emit([&](uint8_t b) { sound_.midiWrite(out, b); },
                         out != nullptr ? out->midiFamily() : MidiOut::GENERAL);
}

bool Machine::setScsiExRom(const uint8_t* data, size_t size) {
    if (size == 0 || size > SCSI_EX_ROM_SIZE) return false;
    if (!scsiExRom_) scsiExRom_ = new uint8_t[SCSI_EX_ROM_SIZE];
    if (!scsiExRom_) return false;
    memset(scsiExRom_, 0xff, SCSI_EX_ROM_SIZE);
    // 8160 バイトのファイルは SPC のレジスタの後ろ ($EA0020) から。ほかは 'SCSIEX' ($EA0044 にある) の位置で決める
    size_t base = 0;
    if (size == SCSI_EX_ROM_SIZE - 0x20 || (size >= 0x2a && memcmp(data + 0x24, "SCSIEX", 6) == 0)) base = 0x20;
    if (base + size > SCSI_EX_ROM_SIZE) size = SCSI_EX_ROM_SIZE - base;
    memcpy(scsiExRom_ + base, data, size);
    return true;
}

// ---------------------------------------------------------------- 起動と時間

void Machine::resetDevices() {
    mfp_.reset();
    sysPort_.reset();
    ioc_.reset();
    fdc_.reset();
    sound_.reset();
    if (sound_.cdOn()) cdDrive_->mixer().reset(); // 記録の読み手は J_RESET で同じことをする
    dmac_.reset();
    scc_.reset();
    ppi_.reset();
    sasi_.reset(); // SASI はバスフリーに戻り、割り込みの要求も消える
    spc_.reset();  // 内蔵 SCSI の SPC (ROM がなくても状態はそろえておく)
    spcEx_.reset();
#ifndef PIX68K_NO_FPU_BOARD
    for (FpuBoard* b : fpuBoards_)
        if (b) b->reset();
#endif
    if (mercury_) mercury_->reset(); // 記録の読み手は J_RESET で同じことをする
    if (ts6bga_) ts6bga_->reset();
    if (neptune_) neptune_->reset(); // 拡張スロットのリセットで DP8390 もリセットされる
    if (midi_) {
        // 拡張スロットのリセットで YM3802 もリセットされるとした。外の音源はリセットされないが、鳴ったままの音が
        // 残らないよう、音源の音も止める
        midi_->reset();
        sound_.midiReset(midiOut());
    }
    areaSet_ = 0;
    updateFastPages();
    updateSoundLines();
    updateIrq();
}

// SRAM が初期化済み ("\x82\x77" "68000W" で始まる) か
static bool sramValid(const uint8_t* sram) {
    static const uint8_t kMagic[8] = {0x82, 0x77, '6', '8', '0', '0', '0', 'W'};
    return memcmp(sram, kMagic, sizeof kMagic) == 0;
}

// SRAM のメインメモリの容量: $ED0008 からの 4 バイトに、バイト数がビッグエンディアンで入る (IPL ROM の初期値は 1MB)
static const uint32_t kSramMemorySize = 0x08;

static bool isSramMemorySize(uint32_t offset) {
    return offset >= kSramMemorySize && offset < kSramMemorySize + 4;
}

// メインメモリの容量が ramSize のときに、SRAM の offset 番地 (kSramMemorySize からの 4 バイトのどれか) に入るバイト
static uint8_t memorySizeByte(uint32_t ramSize, uint32_t offset) {
    const uint8_t bytes[4] = {(uint8_t)(ramSize >> 24), (uint8_t)(ramSize >> 16), (uint8_t)(ramSize >> 8), (uint8_t)ramSize};
    return bytes[offset - kSramMemorySize];
}

// SRAM の 1 バイトを書く。値が変わったときだけ、ファイルに書き戻す対象にする (sramDirty_)
void Machine::storeSram(uint32_t offset, uint8_t v) {
    if (sram_[offset] == v) return;
    sram_[offset] = v;
    sramDirty_ = true;
}

// CPU から SRAM への書き込み (書き込みが許されているとき)。offset は $ED0000 からの位置
void Machine::writeSram(uint32_t offset, uint8_t v) {
    if (sramInitPending_ && isSramMemorySize(offset)) {
        // 空の SRAM を IPL ROM が初期化している間は、メインメモリの容量の 4 バイトに、書かれた値ではなく
        // つないだ容量を入れる。X68030 の IPL ROM は全体を 0 にしてから初期値を写すので、同じ番地に 2 回書く。
        // 先頭の識別子 ("\x82\x77" "68000W") がそろってからの書き込みだけを数え、4 バイトとも済んだら差し替えをやめる
        v = memorySizeByte(config_.ramSize, offset);
        if (sramValid(sram_)) {
            sramInitWritten_ |= (uint8_t)(1u << (offset - kSramMemorySize));
            sramInitPending_ = sramInitWritten_ != 0x0f;
        }
    }
    // 060turbo で空の SRAM を IPL ROM が初期化している間 (先頭の識別子がまだない間) は、キャッシュの設定 ($ED0090) を
    // 命令・データとも有効 (3) のままにする (X68030 の IPL ROM は全体を 0 にする。powerOn() の説明を参照)
    if (offset == 0x90 && config_.cpuModel == m68k::CpuModel::M68060 && !sramValid(sram_)) v = 0x03;
    storeSram(offset, v);
}

void Machine::powerOn() {
    // Human68k は、SRAM にあるメインメモリの容量 ($ED0008 からの 4 バイト) までをメモリとして使う。IPL ROM が空の SRAM に
    // 書く初期値は 1MB なので、そのままでは 4MB をつないだ機種でも 1MB として動き、大きなプログラム (市販のゲーム) が
    // メモリ不足で実行できない。Config::fixSramMemorySize なら、この値をつないだ容量 (Config::ramSize) にそろえる
    // 060turbo で SRAM が空 (新しく作るところ) なら、キャッシュの設定 ($ED0090) を命令・データとも有効 (3) にしておく。
    // ビット 0 が命令キャッシュ、ビット 1 がデータキャッシュ (X68030 の IPL ROM と同じ割り当て。060turbo.sys は IOCS
    // _SYS_STAT でこの 2 ビットを CACR の EIC と EDC に写す。060turbo.sys 0.61 の逆アセンブルで確かめた)。X68030 の IPL ROM は
    // 空の SRAM を初期化するときに全体を 0 にし、このバイトには初期値を書かないので、そのままでは 0 (どちらも無効) で動く
    // (初期化の間の $ED0090 への書き込みは ioWrite() で 3 にする)。実機の 060turbo はキャッシュを有効にして使う。
    // 中身のある SRAM (先頭の識別子がある) は変えない
    if (config_.cpuModel == m68k::CpuModel::M68060 && !sramValid(sram_)) sram_[0x90] = 0x03;
    // 容量をそろえるのは、中身のある SRAM ならここで、空の SRAM なら IPL ROM が初期値を書くとき (ioWrite())。
    // 前の起動で途中まで進んだ初期化の記録は、電源を入れるたびに捨てる
    sramInitPending_ = false;
    sramInitWritten_ = 0; // 差し替えたバイトは、まだない
    if (config_.fixSramMemorySize) {
        if (sramValid(sram_)) {
            for (uint32_t n = 0; n < 4; n++)
                storeSram(kSramMemorySize + n, memorySizeByte(config_.ramSize, kSramMemorySize + n));
        } else {
            sramInitPending_ = true;
        }
    }
    video_->reset();
    rtc_.reset();
    keyboard_.reset();
    mouse_.reset();
    resetDevices();
    // GPIP の固定の入力: アラームなし (1)、EXPWON (1)、電源スイッチ (0)、OPM の割り込みなし (1)、
    // GPIP5 (未接続、1)
    mfp_.setGpip(Mfp::GPIP_ALARM, true);
    mfp_.setGpip(Mfp::GPIP_EXPWON, true);
    mfp_.setGpip(Mfp::GPIP_OPMIRQ, true);
    mfp_.setGpip(5, true);
    mfp_.setGpip(Mfp::GPIP_CIRQ, true);
    mfp_.setGpip(Mfp::GPIP_HSYNC, true);
    // リセット直後は $FF0000 からの 8 バイト (SSP と PC) が $000000 に見える。
    // IPL ROM はすぐに RAM の先頭を書き換えるので、RAM に写しておけば足りる
    memcpy(ram_, ipl_ + 0x10000, 8);
#ifndef PIX68K_NO_CPU020
    if (cpu020_)
        cpu020_->reset();
    else
#endif
        cpu_.reset();
    synced_ = cycles();
    speedFrac_ = 0;
    waitFrac_ = 0; // リセットで ROM から読んだ分の端数は捨てる
    refreshAcc_ = 0;
    waitCycles_ = 0;
    if (midi_) midi_->setTime(synced_);
    mfpAcc_ = secondAcc_ = 0;
    line_ = 0;
    frames_ = 0;
    lineStart_ = synced_;
    lineRem_ = 0;
    hphase_ = 0;
    hdLineStart_ = ~0ull; // horizontalDisplayTimes() を計算し直させる
    computeLine();
}

// 1 ラスタの長さ = 水平のドット数 / ドットクロック。CPU サイクルに直した端数は次のラスタに繰り越す
void Machine::computeLine() {
    uint64_t total = (uint64_t)config_.cpuHz * video_->horizontalTotalDots() + lineRem_;
    uint64_t clock = video_->dotClock(sysPort_.hrl());
    lineCycles_ = (uint32_t)(total / clock);
    lineRem_ = total % clock;
    if (lineCycles_ == 0) lineCycles_ = 1;
    nextLine_ = lineStart_ + lineCycles_;
}

void Machine::startLine() {
    lineStart_ = nextLine_;
    // ネットワークボードが受け取ったフレームを 1 ラスタに 1 つまで取り込む (割り込みは advanceDevices() の最後で伝える)
    if (neptune_) neptune_->poll((uint32_t)((uint64_t)lineCycles_ * 1000000000u / config_.cpuHz)); // 終わったラスタの長さ (ns)
    // MIDI ボードのタイマと送信をラスタごとに進める (レジスタを読み書きしたときも進める)
    if (midi_) syncMidi(lineStart_);
    int total = video_->verticalTotal();
    if (++line_ >= total) {
        line_ = 0;
    }
    computeLine();

    bool vdisp = line_ >= video_->verticalDisplayStart() && line_ < video_->verticalDisplayEnd();
    // 垂直表示期間の出入り (GPIP の VDISP は、まだ前のラスタの値)。入るときはフレームを数えて Video に知らせ、
    // 出るときは画面を合成する相手に、フレームが終わったことを知らせる
    const bool vdispBefore = mfp_.gpip(Mfp::GPIP_VDISP);
    if (vdisp != vdispBefore) {
        if (vdisp) {
            frames_++;
            video_->beginVerticalDisplay();
        } else if (lineListener_) {
            lineListener_->onFrameEnd();
        }
    }
    mfp_.setGpip(Mfp::GPIP_VDISP, vdisp);
    // ラスタ割り込み: R09 のラスタの間だけ CIRQ が 0 になる
    mfp_.setGpip(Mfp::GPIP_CIRQ, line_ != video_->rasterInterruptLine());
}

// 今のラスタの水平表示期間の始まりと終わり (CPU サイクル)。割り算が多いので、ラスタか CRTC のレジスタが
// 変わったときだけ計算し直す
void Machine::horizontalDisplayTimes(uint64_t& start, uint64_t& end) const {
    if (hdGen_ != video_->crtcGeneration() || hdLineStart_ != lineStart_ || hdLineCycles_ != lineCycles_) {
        uint32_t s, e;
        video_->horizontalDisplay(s, e);
        hdStart_ = lineStart_ + (uint64_t)lineCycles_ * s / 65536;
        hdEnd_ = lineStart_ + (uint64_t)lineCycles_ * e / 65536;
        hdGen_ = video_->crtcGeneration();
        hdLineStart_ = lineStart_;
        hdLineCycles_ = lineCycles_;
    }
    start = hdStart_;
    end = hdEnd_;
}

// 今のラスタで次に起きるイベントとその時刻。
// GPIP7 (H-SYNC) は水平表示期間の外で 1 になる: ラスタの始まり (1) → 表示の始まり (0) → 表示の終わり (1)
Machine::LineEvent Machine::pendingEvent(uint64_t& when) const {
    uint64_t start, end;
    horizontalDisplayTimes(start, end);
    if (hphase_ == 0 && start < end && start < nextLine_) {
        when = start;
        return EV_DISPLAY_START;
    }
    if (hphase_ == 1 && end < nextLine_) {
        when = end;
        return EV_DISPLAY_END;
    }
    when = nextLine_;
    return EV_LINE;
}

uint64_t Machine::nextEvent() const {
    uint64_t when;
    pendingEvent(when);
    return when;
}

uint64_t Machine::nextTimerIrq() const {
    uint32_t clocks = mfp_.clocksToTimerIrq();
    // OPM の割り込みは MFP の GPIP3 を通る。MFP が受け付けない設定なら見ない
    if (mfp_.sourceOpen(Mfp::SRC_GPIP3)) {
        const uint32_t opm = sound_.clocksToOpmIrq();
        if (opm && opm < clocks) clocks = opm;
    }
    if (clocks == Mfp::NO_TIMER_IRQ) return UINT64_MAX;
    // advanceDevices() の換算の逆: synced_ から n サイクル進めると (mfpAcc_ + n * mfpMul_) / mfpDiv_ クロック進む。
    // これが clocks に届く最小の n (切り上げ)
    const uint64_t need = (uint64_t)clocks * mfpDiv_;
    if (need <= mfpAcc_) return synced_;
    return synced_ + (need - mfpAcc_ + mfpMul_ - 1) / mfpMul_;
}

// 今のラスタが垂直表示期間の中なら、画面を合成する相手に、このラスタを合成するよう知らせる。1 ラスタに 1 回だけ呼ぶこと
void Machine::notifyRaster() {
    if (!lineListener_ || !mfp_.gpip(Mfp::GPIP_VDISP)) return;
    const int raster = line_ - video_->verticalDisplayStart(); // 垂直表示期間の最初のラスタが 0
    lineListener_->onRaster(raster);
}

// 水平帰線期間が始まった: H-SYNC を 1 にし (MFP の割り込み要因 15)、ラスタコピーをする
void Machine::beginHorizontalBlank() {
    mfp_.setGpip(Mfp::GPIP_HSYNC, true);
    video_->horizontalBlank();
}

void Machine::syncDevices(uint64_t now) {
    if (!profile_) {
        advanceDevices(now);
        return;
    }
    if (profileDepth_++ == 0) {
        uint64_t t0 = profile_->clock();
        advanceDevices(now);
        profile_->devices += profile_->clock() - t0;
        profile_->syncs++;
    } else {
        advanceDevices(now);
    }
    profileDepth_--;
}

void Machine::advanceDevices(uint64_t now) {
    uint64_t elapsed = now - synced_;
    synced_ = now;
    // MFP と OPM は 4MHz。CPU のクロックとの比を約分しておく (16MHz なら 1/4。割り算を小さな数で済ませる)
    mfpAcc_ += elapsed * mfpMul_;
    if (mfpAcc_ >= mfpDiv_) {
        // ふつうは端数が 32 ビットに収まるので、32 ビットの割り算で済ませる (Cortex-A53 では 64 ビットより速い)
        uint32_t clocks4M;
        if (mfpAcc_ <= UINT32_MAX) {
            const uint32_t acc = (uint32_t)mfpAcc_;
#if defined(__arm__) && !defined(__ARM_FEATURE_IDIV)
            // Cortex-A9 has no integer divide instruction. The 68000's
            // 16/10MHz ratios use 4/5 here; constant divisors compile to a
            // shift or multiply instead of a software divide on every sync.
            if (mfpDiv_ == 4) clocks4M = acc / 4;
            else if (mfpDiv_ == 5) clocks4M = acc / 5;
            else clocks4M = acc / mfpDiv_;
#else
            clocks4M = acc / mfpDiv_;
#endif
            mfpAcc_ = acc - clocks4M * mfpDiv_;
        } else {
            clocks4M = (uint32_t)(mfpAcc_ / mfpDiv_);
            mfpAcc_ %= mfpDiv_;
        }
        mfp_.advance(clocks4M);
        // まーきゅりーゆにっと (mercury_ は Machine の最後にあるので、つないでいないときは読まない)
        Mercury* const mercury = sound_.mercuryOn() ? mercury_ : nullptr;
        // CD-ROM 装置のオーディオ (つないでいるときだけ見る): 再生中なら、この間のフレームを先に渡す
        CdMixer* cdMixer = nullptr;
        if (sound_.cdOn()) {
            if (cdDrive_->playing()) feedCdAudio(*cdDrive_, clocks4M);
            cdMixer = &cdDrive_->mixer();
        }
        if (profile_) {
            uint64_t t0 = profile_->clock();
            sound_.advance(clocks4M, mercury, midiOut(), cdMixer);
            profile_->sound += profile_->clock() - t0;
        } else {
            sound_.advance(clocks4M, mercury, midiOut(), cdMixer);
        }
        mfp_.setGpip(Mfp::GPIP_OPMIRQ, !sound_.opm().irq()); // OPM の割り込みは負論理
    }
    secondAcc_ += elapsed;
    while (secondAcc_ >= config_.cpuHz) {
        secondAcc_ -= config_.cpuHz;
        rtc_.tickSecond();
    }
    // 前に同期してから今までに過ぎたイベントを、時刻の順にすべて処理する
    // (長い命令が水平帰線期間をまたいでも、H-SYNC の変化とラスタコピーを落とさない)
    for (;;) {
        uint64_t when;
        LineEvent ev = pendingEvent(when);
        if (when > now) break;
        if (ev == EV_DISPLAY_START) {
            // 水平表示期間に入る。このラスタを今の Video の状態で合成させる
            // (CPU がここまでに書いたレジスタとパレットが、このラスタから反映される)
            notifyRaster();
            mfp_.setGpip(Mfp::GPIP_HSYNC, false);
            hphase_ = 1;
        } else if (ev == EV_DISPLAY_END) {
            beginHorizontalBlank();
            hphase_ = 2;
        } else {
            // 水平表示期間が始まらなかったラスタ (R02 >= R03 か、R02 がラスタの長さ以上の設定) も、合成する相手には
            // 知らせる (幅 0 の画面として扱える)。ラスタの終わりで、次のラスタへ進む前に知らせる
            if (hphase_ == 0) notifyRaster();
            // 表示期間がラスタの終わりまで続く設定では、ここで帰線期間が始まる
            if (!mfp_.gpip(Mfp::GPIP_HSYNC)) beginHorizontalBlank();
            startLine();
            hphase_ = 0;
        }
    }
    runDma();
    keyboard_.update(now, mfp_);
    mouse_.update(now, scc_);
    scc_.update(now); // RS-232C (チャネル A) の送受信と DCD / CTS
    updateIrq();
}

// ---------------------------------------------------------------- DMA、フロッピーディスク、ハードディスク

void Machine::runDma() {
    if (inDma_ || dmac_.idle()) return;
    inDma_ = true;
    dmac_.run(*this);
    inDma_ = false;
}

// DMA のバスアクセスは、DMAC の MFC / DFC / BFC のファンクションコードで行う
// (ユーザのコードなら、スーパーバイザ領域でバスエラーになる)
// DMAC のメモリ側のアドレス。本体のバスにはアドレスが 24 ビットしか出ないので、下位 24 ビットを使う。
// 68060 のときのチャネル 0 (フロッピーディスク) と 1 (内蔵 SCSI と SASI) だけは、MAR がローカルメモリの中を指していれば、
// そのアドレスのまま読み書きする。IPL ROM (1.3 も 1.6 も) のフロッピーディスクの読み書きと、IPL ROM 1.6 と組で使う
// SCSIINROM 16 の SCSI IOCS は DMA で転送し、060turbo.sys の拡張モードではバッファがローカルメモリになるため
// (実機の DMAC はローカルメモリに届かない。互換の処理で、060turbo.sys の -dv / -ss の代わり。patchIplFor060 の前の説明)。
// MAR の値 (論理アドレス) をそのまま物理アドレスとして使う。060turbo.sys はローカルメモリを同じアドレスに割り当てる
// (-ls など、16MB より下の論理アドレスをローカルメモリに割り当てる設定は扱わない)
// ローカルメモリの範囲 ($10000000-$1FFFFFFF) のうち SIMM を積んでいないところは、CPU から見たときと同じにする: 読むと $FF、
// 書いても変わらず、エラーにもならない (machine.h の Config::localMemSize の説明、Cpu020::setNoMemory)。本体のメモリには
// 折り返さない。転送の途中で積んである範囲の終わりを超えても、範囲の中にいる間は同じ扱いが続く
uint32_t Machine::dmaMemAddr(int ch, uint32_t mar) {
    if (ch <= 1 && config_.cpuModel == m68k::CpuModel::M68060 && mar - Config::LOCAL_MEM_BASE < Config::LOCAL_MEM_MAX) return mar;
    return mar & 0xffffff;
}

bool Machine::dmaRead8(uint32_t addr, uint8_t& v, int fc) {
    if (addr >= Config::LOCAL_MEM_BASE) { // dmaMemAddr() が返したローカルメモリの範囲のアドレス
        const uint32_t off = addr - Config::LOCAL_MEM_BASE;
        v = off < config_.localMemSize ? localMem_[off] : 0xff;
        return true;
    }
    int r = readByte(addr, fc);
    if (r == BUS_ERROR) return false;
    v = (uint8_t)r;
    return true;
}

bool Machine::dmaWrite8(uint32_t addr, uint8_t v, int fc) {
    if (addr >= Config::LOCAL_MEM_BASE) {
        const uint32_t off = addr - Config::LOCAL_MEM_BASE;
        if (off < config_.localMemSize) localMem_[off] = v;
        return true;
    }
    return writeByte(addr, v, fc);
}

bool Machine::dmaReady(int ch) {
    switch (ch) {
    case 0: return fdc_.dataReady();
    case 1: return sasi_.dataReady() || (scsiRom_ != nullptr && spc_.dataReady()); // SASI と内蔵 SCSI
    case 2: return mercury_ && mercury_->drq(); // 拡張スロット (まーきゅりーゆにっと)
    case 3: return sound_.adpcm().drq();
    default: return false;
    }
}

// チャネル 1 (SASI と内蔵 SCSI) は TC で何もしない。転送の終わりはコントローラがフェーズを変えて決める
void Machine::dmaDone(int ch) {
    if (ch == 0) fdc_.terminalCount();
}

void Machine::dmaError(int ch) {
    if (ch == 0) fdc_.abortTransfer();
}

// FDC のデータのレジスタ ($E94003)、SASI のデータのレジスタ ($E96001)、内蔵 SCSI の SPC の DREG ($E96035) から RAM への
// まとめた転送。1 バイトずつのとき (dmaReady → dmaRead8 → ioRead → Fdc::read / Sasi::read / Spc::read、dmaWrite8 → ram_)
// と同じ結果になる場合だけする。途中の updateIrq() は省くが、CPU はその間に動かず、
// runDma() のあと (advanceDevices()) で必ず呼ぶので、CPU から見える割り込みは変わらない。
// ハードディスクの転送は X68000 の時間を使わない (データが出ている間、1 回の runDma() で 64KB まで進む) ので、
// 大きなファイルを読むと 1 フレームに数百 KB を転送する。1 バイトずつでは、その時間でフレームが遅れた
int Machine::dmaReadBlock(int ch, uint32_t dar, int dfc, uint32_t mar, int mfc, int n) {
    return dmaBlockToRam(ch, true, dar, dfc, mar, mfc, n);
}

int Machine::dmaReadBlockAuto(int ch, uint32_t dar, int dfc, uint32_t mar, int mfc, int n) {
    return dmaBlockToRam(ch, false, dar, dfc, mar, mfc, n);
}

// external: 外部要求の転送 (1 バイトごとに dmaReady(ch) を確かめる)。false ならオートリクエスト (確かめずに読む)
int Machine::dmaBlockToRam(int ch, bool external, uint32_t dar, int dfc, uint32_t mar, int mfc, int n) {
    if (!dmaBlock_) return 0;
    const int k = dmaBlockCopy(ch, external, dar, dfc, mar, mfc, n);
    if (profile_ && k > 0) profile_->dmaBlockBytes += (uint64_t)k;
    return k;
}

int Machine::dmaBlockCopy(int ch, bool external, uint32_t dar, int dfc, uint32_t mar, int mfc, int n) {
    if (ch != 0 && ch != 1) return 0;
    if (userBlocked(dar, dfc)) return 0;
    // RAM の外 (バスエラーなど) にかかるバイトは 1 バイトずつの処理に任せる。
    // userBlocked() はエリアセットより下だけを止めるので、先頭が止められなければ、そこから上の RAM も止められない
    // 写す先。dmaMemAddr() が返したローカルメモリの範囲のアドレス (68060 のチャネル 0 と 1) なら、積んである範囲の
    // 終わりまでをまとめて写す。積んでいないところは 1 バイトずつの処理に任せる (dmaWrite8() が書き込みを捨てる)
    uint8_t* dst;
    uint32_t room;
    if (mar >= Config::LOCAL_MEM_BASE) {
        const uint32_t off = mar - Config::LOCAL_MEM_BASE;
        if (off >= config_.localMemSize) return 0;
        dst = localMem_ + off;
        room = config_.localMemSize - off;
    } else {
        if (mar >= config_.ramSize || userBlocked(mar, mfc)) return 0;
        dst = ram_ + mar;
        room = config_.ramSize - mar;
    }
    if ((uint32_t)n > room) n = (int)room;
    if (ch == 0) {
        // FDC は外部要求の転送だけ (IOCS の使い方。オートリクエストで Fdc::readData() が 1 バイトずつと同じになるかは確かめていない)
        if (!external || dar < 0xe94000 || dar >= 0xe96000 || (dar & 7) != 3) return 0;
        return fdc_.readData(dst, n);
    }
    // ioRead() の $E96000-$E97FFF と同じ分け方: $E96000-$E9601F が SASI、$E96020-$E9603F の奇数番地が SPC
    if (dar < 0xe96000 || dar >= 0xe96040) return 0;
    if (dar < 0xe96020) {
        if ((dar & 7) != 1) return 0;
        // データインフェーズでデータが出ている間だけ写す。Sasi::read() がバイトを渡す条件と、SASI の dataReady() は、
        // データインフェーズでは同じ (外部要求でもオートリクエストでも、ここで止まったところからは 1 バイトずつに戻る)
        return sasi_.readData(dst, n);
    }
    if (!scsiRom_ || !(dar & 1) || (int)((dar & 0x1f) >> 1) != Spc::DREG) return 0;
    // 外部要求: チャネル 1 の要求は SASI と SPC の両方から来る (dmaReady())。SASI の要求で SPC の DREG を読む転送は、
    // 1 バイトずつに任せる
    if (external && sasi_.dataReady()) return 0;
    return spc_.readData(dst, n, external);
}

void Machine::insertDisk(int drive, FloppyImage* image) { fdc_.insert(drive, image); }

void Machine::ejectDisk(int drive) { fdc_.eject(drive); }

// SASI のディスクをつなぐ / 外す。範囲の外の unit と、転送の途中のつなぎ直しは Sasi::attach() が扱う
void Machine::attachSasi(int unit, HardDiskImage* image) {
    sasi_.attach(unit, image);
}

// メインメモリの DRAM リフレッシュで遅れる分 (68000 の機種でウェイトを入れるとき。runCpu() から呼ぶ)。
// メインメモリの読み書きはウェイトなしだが、ときどきリフレッシュで待たされる (si-ee の si_mpuclk.s の注釈「RAM 上で
// 測定して ×10 では、ときどきアクセスウェイトが入るため少し遅い値になる」。XEiJ の説明書の更新履歴にも、メインメモリで
// 測ると DRAM リフレッシュの影響で DMA の転送が少し遅くなるとある)。
// 待たされる割合は cpupower.r の説明書の値から決めた: CPU の項目 (メインメモリの上の SUBQ.W と BNE.W のループ 5000 回、
// ウェイトがなければ 70000 クロック) は、タイマ C の 50us を単位に、XVI の 10MHz で 143 (ウェイトなしなら 140)、
// 16.67MHz で 85 (同じく 84)。両方に合うのは時間が 2.1% から 2.4% 延びる場合なので、45 クロックごとに 1 クロック
// (2.2%) 足す。ここでは、この区切りの終わりに PC がメインメモリにあれば (命令をメインメモリから読んでいれば)、
// 区切りの間、ウェイト (busWait()) のほかはメインメモリを読み書きしていたとみなす (SRAM や ROM で動くループは
// 遅らせない。VRAM や I/O のウェイトで待っている間は、リフレッシュがあっても遅れないとする)。
// 区切りの終わりの PC だけで区切り全体を分けるので、区切りの途中で ROM とメインメモリを行き来すると、その区切りの
// 分だけずれる。区切りは長くても 256 クロック (runCpu())、ずれるのは区切り 1 つにつき最大 6 クロックほどで、
// IOCS の呼び出しのように行き来が多い区間でなければ平均では小さい。
// waitCycles_ は、呼び出し元が cpu.run() の直前に 0 にする (区切りの外の読み書きのウェイトを数えないため)。
// start は区切りを始めたときの cpu_.cycles()
void Machine::refreshWait(uint64_t start) {
    const uint64_t elapsed = cpu_.cycles() - start;
    const uint64_t waits = waitCycles_;
    waitCycles_ = 0;
    if ((cpu_.pc_ & 0xffffff) >= config_.ramSize || waits >= elapsed) return;
    refreshAcc_ += (uint32_t)(elapsed - waits);
    if (refreshAcc_ < REFRESH_PERIOD) return;
    cpu_.addCycles((int)(refreshAcc_ / REFRESH_PERIOD));
    refreshAcc_ %= REFRESH_PERIOD;
}

// 68000 と 68030 のどちらでも同じ進め方 (CPU は 1 回だけ選ぶ)
// maxSlice は 1 区間の長さの上限 (CPU のサイクル数)。timers なら、MFP と OPM のタイマが割り込みを要求する時刻でも区切る
template <class C>
void Machine::runCpu(C& cpu, int64_t cycles, int64_t maxSlice, bool timers) {
    const uint64_t end = cpu.cycles() + (uint64_t)cycles;
    while (cpu.cycles() < end) {
        uint64_t now = cpu.cycles();
        uint64_t next = nextEvent();
        if (timers) {
            const uint64_t t = nextTimerIrq();
            if (t < next) next = t;
        }
        uint64_t target = next < end ? next : end;
        int64_t slice = target > now ? (int64_t)(target - now) : 1;
        // 周辺機器への書き込みでタイミングが変わることがあるので、長く走らせすぎない
        if (slice > maxSlice) slice = maxSlice;
        if (profile_) profile_->slices++;
        waitCycles_ = 0;
        cpu.run(slice);
        if (busWaits_) refreshWait(now);
        syncDevices(cpu.cycles());
    }
}

void Machine::run(int64_t cycles) {
#ifndef PIX68K_NO_CPU020
    if (cpu020_) {
        // 68030 / 68060: 上限を X68000 の時間で 25.6us (10MHz の 68000 の 256 サイクルと同じ) にする。
        // CPU のサイクル数で 256 のままだと、50MHz の 68060 は 5.1us ごとに周辺機器を同期することになり、同期の
        // 回数がクロックに比例して増える。周辺機器の割り込みは同期のときに CPU に届くので、上限を伸ばすと、
        // 同期の時刻に入っていない割り込みの届く時刻の遅れが伸びる (最大で上限の長さ)。MFP のタイマ (ディレイモード)
        // と OPM のタイマは、要求を出す時刻を同期の時刻に入れて (nextTimerIrq())、要求が出た命令の切れ目で届ける。
        // 設定を書き換えた命令でも区切り直す (timersChanged())。DMAC、FDC、SASI、SCSI、SCC、MIDI ボード、キーボードの
        // 割り込みは入れていないので、最大で 25.6us 遅れる (10MHz の 68000 と同じ)
        runCpu(*cpu020_, cycles, (int64_t)config_.cpuHz * 256 / 10000000, true);
        return;
    }
#endif
    if (speed000_ != 100) {
        runCpu000Scaled(cycles);
        return;
    }
    runCpu(cpu_, cycles, 256, false);
}

// runCpu() と同じ進め方で、68000 に X68000 の時間の speed000_/100 倍のサイクル数を走らせる。
// コアの命令のサイクル数はそのまま (速さ 100 のときと同じ) で、走ったサイクル数を X68000 の時間に直す
void Machine::runCpu000Scaled(int64_t cycles) {
    const uint64_t end = cpu_.cycles() + (uint64_t)cycles;
    while (cpu_.cycles() < end) {
        const uint64_t now = cpu_.cycles();
        uint64_t next = nextEvent();
        uint64_t target = next < end ? next : end;
        int64_t slice = target > now ? (int64_t)(target - now) : 1;
        if (slice > 256) slice = 256;
        if (profile_) profile_->slices++;
        if (cpu_.halted() || cpu_.waitingInStop()) {
            // STOP で割り込みを待つ間 (と、止まっている間) は X68000 の時間のまま進める (速さを掛けると空回りが増える
            // だけ)。割り込みの要求は周辺機器を進めたとき (区切りの終わり) にしか変わらないので、この区切りの間は
            // ずっと待つだけ。割り込みで起きるときは下に進み、割り込みの処理からあとは速さを掛ける
            waitCycles_ = 0;
            cpu_.run(slice);
            if (busWaits_) refreshWait(now);
        } else {
            // X68000 の時間で slice サイクルになるだけ走らせる (切り上げ)
            const int64_t raw = (slice * speed000_ + 99) / 100;
            sliceStart_ = now;
            inScaledSlice_ = true;
            waitCycles_ = 0;
            cpu_.run(raw);
            // ウェイトとリフレッシュは、ほかの命令のサイクル数と同じく速さで縮める (setupBusWaits() の注釈)
            if (busWaits_) refreshWait(now);
            const uint64_t t = (cpu_.cycles() - now) * speedMul_ + speedFrac_;
            speedFrac_ = t & ((1u << SPEED_FRAC_BITS) - 1);
            inScaledSlice_ = false;
            cpu_.setCycles(now + (t >> SPEED_FRAC_BITS));
        }
        syncDevices(cpu_.cycles());
    }
}

void Machine::setCpuSpeed(int percent) {
#ifndef PIX68K_NO_CPU020
    if (use020() && cpu020_->is060()) {
        // 060turbo: 下限は設定の速さ、上限は設定の上限 (どちらも実機の 68060 を 100 とした %)
        int hi = config_.maxSpeedLimit060 > 0 ? config_.maxSpeedLimit060 : SPEED_MAX;
        if (hi > SPEED_MAX) hi = SPEED_MAX;
        if (hi < config_.speed060) hi = config_.speed060;
        if (percent < config_.speed060) percent = config_.speed060;
        if (percent > hi) percent = hi;
        cpu020_->setSpeed(percent);
        return;
    }
#endif
    if (percent < 100) percent = 100;
    if (percent > SPEED_MAX) percent = SPEED_MAX;
#ifndef PIX68K_NO_CPU020
    if (use020()) {
        cpu020_->setSpeed(percent);
        return;
    }
#endif
    speed000_ = percent;
    speedMul_ = (uint32_t)((100u << SPEED_FRAC_BITS) / (unsigned)percent);
    if (percent == 100) speedFrac_ = 0;
}

int Machine::cpuSpeed() const {
#ifndef PIX68K_NO_CPU020
    if (use020()) return cpu020_->speed();
#endif
    return speed000_;
}

void Machine::getRegisters(m68k::Registers& r) const {
#ifndef PIX68K_NO_CPU020
    if (cpu020_) {
        cpu020_->getRegisters(r);
        return;
    }
#endif
    cpu_.getRegisters(r);
}

void Machine::setRegisters(const m68k::Registers& r) {
#ifndef PIX68K_NO_CPU020
    if (cpu020_) {
        cpu020_->setRegisters(r);
        return;
    }
#endif
    cpu_.setRegisters(r);
}

void Machine::runFrame() {
    if (lineCycles_ == 0) return; // powerOn() の前
    rebaseTime(); // フレームの切れ目で、時刻が上限を超えていれば戻す
    uint64_t f = frames_;
    // 表示期間が来ない設定でも止まらないよう、上限を 1/10 秒分にする
    const uint64_t limit = cycles() + config_.cpuHz / 10;
    while (frames_ == f && cycles() < limit) run(lineCycles_);
}

// ---------------------------------------------------------------- 音

// OPM の CT1 は FDC の強制レディ、CT2 は ADPCM のクロック (1 = 4MHz)。8255 のポート C は ADPCM の分周と左右
void Machine::updateSoundLines() {
    Opm& opm = sound_.opm();
    fdc_.setForceReady(opm.ct1());
    sound_.setAdpcmLines(opm.ct2(), ppi_.adpcmPan(), ppi_.adpcmRate());
    mfp_.setGpip(Mfp::GPIP_OPMIRQ, !opm.irq());
}

// ---------------------------------------------------------------- 割り込み

void Machine::updateIrq() {
    ioc_.setRequest(Ioc::SRC_FDC, fdc_.irq());
    const bool sasiWants = sasi_.irq(); // SASI の割り込みは、IOC のハードディスクの要因として入る
    ioc_.setRequest(Ioc::SRC_HDD, sasiWants);
    int level = 0;
    if (ioc_.irq() || (scsiRom_ != nullptr && spc_.irq())) level = 1; // IOC の要因と内蔵 SCSI はレベル 1
    if (scsiExRom_ && spcEx_.irq()) level = SCSI_EX_IRQ_LEVEL;
    if (neptune_ && neptune_->irq()) level = Neptune::INTERRUPT_LEVEL;
    if (dmac_.irq()) level = 3;
    if (midi_ && midi_->irq()) level = Ym3802::INTERRUPT_LEVEL;
    if (scc_.irq()) level = 5;
    if (mfp_.irq()) level = 6;
#ifndef PIX68K_NO_CPU020
    if (cpu020_) cpu020_->setIPL(level);
#endif
    cpu_.setIPL(level);
}

int Machine::interruptAck(int level) {
    int v = -2; // 応答するデバイスがなければスプリアス割り込み
    switch (level) {
    case 6:
        v = mfp_.ack();
        break;
    case 5:
        v = scc_.ack();
        break;
    case Ym3802::INTERRUPT_LEVEL:
        if (midi_ && midi_->irq()) v = midi_->vector(); // 要因は ICR で消すまで続く
        break;
    case 3:
        v = dmac_.ack();
        break;
    case SCSI_EX_IRQ_LEVEL:
        // 外付け SCSI のボードか Neptune-X がベクタを出す。要求は SPC の INTS か DP8390 の ISR を消すまで続く。
        // 両方が要求しているときにどちらが先に応答するかは分からないので、外付け SCSI を先にした
        if (scsiExRom_ && spcEx_.irq()) v = SCSI_EX_VECTOR;
        else if (neptune_ && neptune_->irq()) v = Neptune::VECTOR;
        break;
    case 1:
        // 内蔵 SCSI の SPC は、IOC のほかの要因より先にベクタ $6C で答える (spc.h)
        if (scsiRom_ != nullptr && spc_.irq()) {
            v = 0x6c; // SPC のベクタ
            break;
        }
        v = ioc_.ack();
        {
            // IOC は要求が出ているかしか見ないので、SASI の要求は応答した時点で下げる (下げないと割り込みが続く)
            const bool hdd = v >= 0 && (v & 3) == Ioc::SRC_HDD;
            if (hdd) sasi_.acknowledgeInterrupt();
        }
        break;
    case 7:
        return -1; // NMI (インタラプトスイッチ) はオートベクタ
    }
    updateIrq();
    return v < 0 ? -2 : v;
}

// ---------------------------------------------------------------- メモリマップ

bool Machine::setTs6bga(bool on, uint32_t base) {
    if (on && base != 0xee0000 && base != 0xef0000) return false;
    if (on && ts6bga_) return ts6bga_->window(base);
    if (on) ts6bga_ = new Ts6bga(base);
    if (!on) { delete ts6bga_; ts6bga_ = nullptr; }
    return !on || ts6bga_ != nullptr;
}

bool Machine::userBlocked(uint32_t addr, int fc) const {
    if (fc != m68k::FC_USER_DATA && fc != m68k::FC_USER_PROG) return false;
    // エリアセット ($E86001) で決めた RAM の先頭と、I/O 領域はスーパーバイザ専用
    if (addr < ((uint32_t)areaSet_ + 1) * 0x2000) return true;
    return addr >= 0xe80000 && addr < 0xf00000;
}

int Machine::readByte(uint32_t addr, int fc) {
    addr &= 0xffffff;
    if (userBlocked(addr, fc)) return BUS_ERROR;
    if (addr < config_.ramSize) return ram_[addr];
    if (addr < 0xc00000) return BUS_ERROR;
    if (addr < 0xe00000) return video_->gvramRead(addr - 0xc00000);
    if (addr < 0xe80000) return video_->tvramRead(addr - 0xe00000);
    if (addr < 0xf00000) return ioRead(addr);
    if (addr < 0xfc0000) return cg_[addr - 0xf00000];
    if (addr < 0xfe0000) return scsiRom_ ? scsiRom_[addr - 0xfc0000] : BUS_ERROR;
    return ipl_[addr - 0xfe0000];
}

bool Machine::writeByte(uint32_t addr, uint8_t v, int fc) {
    addr &= 0xffffff;
    if (userBlocked(addr, fc)) return false;
    if (addr < config_.ramSize) {
        ram_[addr] = v;
        return true;
    }
    if (addr < 0xc00000) return false;
    if (addr < 0xe00000) {
        video_->gvramWrite(addr - 0xc00000, v);
        return true;
    }
    if (addr < 0xe80000) {
        video_->tvramWrite(addr - 0xe00000, v);
        return true;
    }
    if (addr < 0xf00000) return ioWrite(addr, v);
    return false; // ROM
}

// I/O 領域 ($E80000-$EFFFFF) は 8KB ごとにデバイスが並ぶ
int Machine::ioRead(uint32_t addr) {
    uint32_t off = addr & 0x1fff;
    bool odd = (addr & 1) != 0;
    switch ((addr - 0xe80000) >> 13) {
    case 0: return video_->crtcRead(off);
    case 1: return video_->paletteRead(off);
    case 2:
        if (off >= 0x100) return 0x00;
        // チャネル 2 の CSR の PCS (bit0) は、拡張スロットの EXPCL の今の値。まーきゅりーゆにっとが L / R のクロックを出す
        // (PCM8PP は PCS の変わり目を待ってからコマンドを書く)。今の時刻まで進めてから読む
        if (off == 0x80 && mercury_) {
            syncDevices();
            return (uint8_t)((dmac_.read(off) & ~0x01) | (mercury_->expcl(sound_.pendingClocks()) ? 0x01 : 0x00));
        }
        return dmac_.read(off);
    case 3: return 0x00; // エリアセット (書き込み専用)
    case 4: { // MFP: 奇数番地に 24 個のレジスタ
        if (!odd) return 0xff;
        int reg = (int)((off & 0x3f) >> 1);
        if (reg >= 24) return 0xff;
        syncDevices(); // タイマと GPIP を今の時刻まで進めてから読む (syncDevices() の最後に updateIrq() も呼ぶ)
        // 読んでも割り込みの要求は変わらないので (Mfp::irq() が見る IPR、IMR、ISR、VR は読むだけでは変わらない)、
        // ここで updateIrq() は呼ばない。GPIP を待つループでは、ここを 1 フレームに 1 万回近く通る
        return mfp_.read(reg);
    }
    case 5: return odd ? rtc_.read((int)((off & 0x1f) >> 1)) : 0xff;
    case 6: return 0xff; // プリンタ
    case 7: return sysPort_.read(off);
    case 8:
        syncDevices(); // タイマと BUSY を今の時刻まで進めてから読む
        return sound_.opm().read(off);
    case 9:
        syncDevices();
        return sound_.adpcm().read(off);
    case 10: {
        uint8_t v = fdc_.read(off);
        updateIrq();
        return v;
    }
    case 11: {
        // $E96000-$E9601F は SASI、$E96020-$E9603F は内蔵 SCSI の SPC (SCSI の ROM があるときだけ。奇数番地にレジスタ)
        if (off >= 0x40 || (off >= 0x20 && scsiRom_ == nullptr)) return BUS_ERROR;
        const uint8_t v = off < 0x20 ? sasi_.read(off) : odd ? spc_.read((int)((off - 0x20) >> 1)) : 0xff;
        updateIrq(); // SASI はステータスを読むと割り込みの要求が消える
        return v; // SASI か SPC のレジスタ
    }
    case 12: {
        // チャネル A (RS-232C) は送信バッファ空や受信を今の時刻にそろえてから読む。チャネル B (マウス) は前のまま
        if (off & 4) syncDevices();
        uint8_t v = scc_.read(off); // 受信データを読むと割り込みの要求が変わる
        updateIrq();
        return v;
    }
    case 13: return ppi_.read(off);
    case 14: return ioc_.read(off);
    case 16: { // $EA0000-$EA1FFF: 外付け SCSI ボード (ROM を渡したときだけ)
        if (!scsiExRom_) return BUS_ERROR;
        if (off >= 0x20) return scsiExRom_[off];
        uint8_t v = odd ? spcEx_.read((int)(off >> 1)) : 0xff;
        updateIrq();
        return v;
    }
    case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
        return video_->spriteRead(addr - 0xeb0000);
    case 40: case 41: // SRAM ($ED0000-$ED3FFF)
        return sram_[addr - 0xed0000];
    default:
        // 拡張ボード ($E9E000 と $E9E080 の数値演算プロセッサボード、$EAFA00 の MIDI ボード、$ECC000 のまーきゅりーゆにっと、
        // $ECE000 の Neptune-X)。$EC0000 (ユーザー I/O) などは、ボードがないのでバスエラー
        return boardRead(addr);
    }
}

// 拡張ボードの読み書き (ioRead() / ioWrite() の default から呼ぶ)。switch に case を足すと、ボードがないときの
// machine_bench が遅くなった (コードの配置のため) ので、別の関数にして呼ぶ回数の少ない側に置く
__attribute__((noinline, cold)) int Machine::boardRead(uint32_t addr) {
    const uint32_t off = addr & 0x1fff;
    if (ts6bga_) {
        if (ts6bga_->window(addr)) return ts6bga_->memoryRead(addr & 0xffff);
        if (addr >= 0xe9e200 && addr < 0xe9e400) {
            const uint64_t phase = cycles() % config_.cpuHz;
            const int v = ts6bga_->read(off, (phase * 60 % config_.cpuHz) >= config_.cpuHz * 9 / 10);
            return v < 0 ? BUS_ERROR : v;
        }
    }
    switch ((addr - 0xe80000) >> 13) {
    case 15: { // $E9E000-$E9E01F、$E9E080-$E9E09F: 数値演算プロセッサボード (16 ビットの読み書きは read16() で受ける)
        FpuBoard* b = fpuBoardAt(off);
        if (!b) return BUS_ERROR;
#ifndef PIX68K_NO_FPU_BOARD
        return b->read8(off & (FpuBoard::SIZE - 1));
#else
        return BUS_ERROR;
#endif
    }
    case 23: { // $EAE000-$EAFFFF: MIDI ボード ($EAFA00-$EAFA0F だけ)
        if (!midi_ || off < (Ym3802::BASE & 0x1fff) || off >= (Ym3802::BASE & 0x1fff) + Ym3802::SIZE) return BUS_ERROR;
        if (!(addr & 1)) return 0xff;
        syncDevices(); // 音の記録を今の時刻まで進めてから、送り終えたバイトを渡す
        syncMidi(cycles());
        const uint8_t v = midi_->read((int)(off >> 1) & 7);
        updateIrq();
        return v;
    }
    case 38: // $ECC000-$ECDFFF: まーきゅりーゆにっと ($ECC080-$ECC0FF だけ)
        if (!mercury_ || off < 0x80 || off >= 0x100) return BUS_ERROR;
        syncDevices(); // バッファの空き (EXREQ) を今の時刻にそろえる
        return mercury_->read(off, sound_.pendingClocks());
    case 39: { // $ECE000-$ECE3FF: Neptune-X
        if (!neptune_) return BUS_ERROR;
        const int v = neptune_->read(off);
        if (v < 0) return BUS_ERROR;
        updateIrq(); // データポートの読み出しでリモート DMA が終わると ISR が変わる
        return v;
    }
    default: return BUS_ERROR;
    }
}

__attribute__((noinline, cold)) bool Machine::boardWrite(uint32_t addr, uint8_t v) {
    const uint32_t off = addr & 0x1fff;
    if (ts6bga_) {
        if (ts6bga_->window(addr)) { ts6bga_->memoryWrite(addr & 0xffff, v); return true; }
        if (addr >= 0xe9e200 && addr < 0xe9e400) return ts6bga_->write(off, v);
    }
    switch ((addr - 0xe80000) >> 13) {
    case 15: {
        FpuBoard* b = fpuBoardAt(off);
        if (!b) return false;
#ifndef PIX68K_NO_FPU_BOARD
        b->write8(off & (FpuBoard::SIZE - 1), v);
        return true;
#else
        return false;
#endif
    }
    case 23:
        if (!midi_ || off < (Ym3802::BASE & 0x1fff) || off >= (Ym3802::BASE & 0x1fff) + Ym3802::SIZE) return false;
        if (!(addr & 1)) return true;
        syncDevices();
        syncMidi(cycles());
        midi_->write((int)(off >> 1) & 7, v);
        updateIrq();
        return true;
    case 38:
        if (!mercury_ || off < 0x80 || off >= 0x100) return false;
        syncDevices(); // 書く前の時間の分だけ、前の設定で音を作る
        sound_.mercuryWrite(*mercury_, off, v);
        return true;
    case 39:
        if (!neptune_ || !neptune_->write(off, v)) return false;
        updateIrq();
        return true;
    default: return false;
    }
}

bool Machine::ioWrite(uint32_t addr, uint8_t v) {
    uint32_t off = addr & 0x1fff;
    bool odd = (addr & 1) != 0;
    switch ((addr - 0xe80000) >> 13) {
    case 0: {
        syncDevices(); // H-SYNC を今の時刻にそろえる
        video_->crtcWrite(off, v);
        // ラスタコピー: 水平帰線期間の途中で動作ポートの bit3 を立てたとき、または bit3 が立ったまま
        // R22 の下位バイト (転送先) を書いたときは、次の帰線期間を待たずにすぐ転送する。すぐ転送することは資料でも
        // 実機でも確かめていない。IPL ROM の IOCS のラスタコピーと同じループは、これで 1 ラスタずつ写る
        // (tests/unit/x68k_test.cpp の rasterCopyLoop())
        uint32_t o = off & 0x7ff;
        bool trigger = (o == 0x481 && (v & 0x08)) ||
                       (o < 0x400 && (o & 0x3f) == 0x2d && (video_->crtcRead(0x481) & 0x08));
        if (trigger && mfp_.gpip(Mfp::GPIP_HSYNC)) video_->horizontalBlank();
        return true;
    }
    case 1: video_->paletteWrite(off, v); return true;
    case 2:
        if (off < 0x100) {
            dmac_.write(off, v);
            // チェーンで開始したチャネルの最初の表をすぐに読み、ソフトウェアアボートなどで止まったチャネルを
            // すぐデバイスに知らせる
            if (!inDma_) {
                inDma_ = true;
                dmac_.notify(*this);
                // オートリクエストの最大速度 (OCR の REQG = 01) の転送は、終わるまで DMAC がバスを持ち、CPU は止まる
                // (Inside X68000 の PDF の 36、38、53 ページ)。次の周辺機器の更新を待たずに、ここで最後まで転送する。
                // グラディウス II は、PCG とスプライトの表をチャネル 2 のこの転送で送り、開始を書いた直後に次の転送の
                // MAR / DAR / MTC を書く。後回しにすると、前の転送がその値で進み、別のパターンが PCG に入った
                // (タイトルのロゴや弾が別のキャラになる)。転送にかかる時間は数えていない (X68000 の時間は進まない)
                // 終わりのある転送は長さによらず最後まで進め、終わらない転送 (リンクチェーンの輪、転送が DMAC の
                // レジスタを書く場合) は途中で戻る (dmac.h の runBusHolders())。
                // HLT を外す書き込みもここを通るので、止めてあった転送はそこで最後まで進む
                if (dmac_.holdsBus()) {
                    dmaWroteDmac_ = false;
                    dmac_.runBusHolders(*this, &dmaWroteDmac_);
                }
                inDma_ = false;
            } else {
                dmaWroteDmac_ = true; // DMAC の転送が DMAC のレジスタを書いた
            }
            updateIrq();
        }
        return true;
    case 3:
        if (off == 1 && areaSet_ != v) {
            areaSet_ = v;
            updateFastPages();
        }
        return true;
    case 4: {
        if (!odd) return true;
        int reg = (int)((off & 0x3f) >> 1);
        syncDevices(); // 書く前の時間の分だけ、前の設定でタイマを進める
        if (reg < 24) mfp_.write(reg, v);
        updateIrq();
        if (reg <= 18) timersChanged(); // GPIP から TDDR まで: 割り込みの設定とタイマ (USART のレジスタは除く)
        return true;
    }
    case 5:
        if (odd) rtc_.write((int)((off & 0x1f) >> 1), v);
        return true;
    case 6: return true; // プリンタ
    case 7: sysPort_.write(off, v); return true;
    case 8:
        syncDevices(); // 書く前の時間の分だけ、前の設定で音を作る
        sound_.opmWrite(off, v);
        updateSoundLines();
        updateIrq();
        // タイマの値と制御 ($10-$14) を書いた
        if ((off & 3) == 3 && sound_.opm().address() >= 0x10 && sound_.opm().address() <= 0x14) timersChanged();
        return true;
    case 9:
        syncDevices();
        sound_.adpcmWrite(off, v);
        return true;
    case 10:
        fdc_.write(off, v);
        updateIrq();
        return true;
    case 11: // SASI と内蔵 SCSI (番地の割り当ては読み出しと同じ)
        if (off >= 0x40 || (off >= 0x20 && scsiRom_ == nullptr)) return false;
        if (off < 0x20) sasi_.write(off, v);
        else if (odd) spc_.write((int)((off - 0x20) >> 1), v);
        updateIrq(); // 書き込みで割り込みの要求が変わることがある
        return true;
    case 12:
        syncDevices(); // マウスが RTS を受けた時刻を今にそろえる
        scc_.write(off, v);
        updateIrq();
        return true;
    case 13:
        syncDevices();
        ppi_.write(off, v);
        updateSoundLines();
        return true;
    case 14:
        ioc_.write(off, v);
        updateIrq();
        return true;
    case 16: // 外付け SCSI ボード。ROM ($EA0020-) への書き込みはバスエラー
        if (!scsiExRom_ || off >= 0x20) return false;
        if (odd) spcEx_.write((int)(off >> 1), v);
        updateIrq();
        return true;
    case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
        video_->spriteWrite(addr - 0xeb0000, v);
        return true;
    case 40: case 41:
        // SRAM は、システムポートの $E8E00D に 0x31 を書いてある間だけ書き換えられる。それ以外のときの書き込みは
        // 捨てる (バスエラーにはならない)
        if (sysPort_.sramWritable()) writeSram(addr - 0xed0000, v);
        return true;
    default:
        return boardWrite(addr, v); // 拡張ボード (boardRead() を参照)
    }
}

// ---------------------------------------------------------------- m68k::Bus

// 動いている CPU にバスエラーを伝える (戻ってこない)。68030 には 32 ビットのアドレス、68000 には下位 24 ビットを渡す
void Machine::busError(uint32_t addr, bool write, int fc) {
#ifndef PIX68K_NO_CPU020
    if (cpu020_) cpu020_->busError(addr, write, fc);
#endif
    cpu_.busError(addr & 0xffffff, write, fc);
}

uint8_t Machine::read8(uint32_t addr, int fc) {
    busWait(addr & 0xffffff, false);
    int v = readByte(addr, fc);
    if (v == BUS_ERROR) busError(addr, false, fc);
    return (uint8_t)v;
}

uint16_t Machine::read16(uint32_t full, int fc) {
    uint32_t addr = full & 0xffffff;
    if (addr < config_.ramSize && !(fc == m68k::FC_USER_DATA || fc == m68k::FC_USER_PROG))
        return (uint16_t)((ram_[addr] << 8) | ram_[addr + 1]);
    busWait(addr, false);
    // IPL ROM と CGROM とテキスト VRAM (68000 でウェイトを入れるときは表 (updateFastPages) になく、ここを通る)。
    // ユーザモードでも読める場所なので、readByte() を 2 回呼ばずに読む。68000 だけ (アドレスは偶数。68030 は奇数の
    // アドレスで語を読むことがあり、ROM の終わりを越えるので readByte() で読む)
    if (busWaits_) {
        if (addr >= 0xfe0000) return (uint16_t)((ipl_[addr - 0xfe0000] << 8) | ipl_[addr - 0xfe0000 + 1]);
        if (addr >= 0xf00000 && addr < 0xfc0000)
            return (uint16_t)((cg_[addr - 0xf00000] << 8) | cg_[addr - 0xf00000 + 1]);
        if (addr >= 0xe00000 && addr < 0xe80000) {
            const uint8_t* t = video_->tvram() + (addr - 0xe00000);
            return (uint16_t)((t[0] << 8) | t[1]);
        }
    }
    // 数値演算プロセッサボードの CIR は 16 ビットで 1 回の読み書き (応答やオペランドは読むと進む)。
    // $E9E000-$E9E01F と $E9E080-$E9E09F を 1 回の比較で拾い、bit7 でボードを選ぶ
#ifndef PIX68K_NO_FPU_BOARD
    if ((addr & ~(FPU_BOARD_SELECT | (FpuBoard::SIZE - 1))) == FpuBoard::BASE) {
        FpuBoard* b = fpuBoards_[(addr & FPU_BOARD_SELECT) ? 1 : 0];
        if (b && !userBlocked(addr, fc)) return b->read16(addr & (FpuBoard::SIZE - 1));
    }
#endif
    int hi = readByte(addr, fc);
    if (hi == BUS_ERROR) busError(full, false, fc);
    int lo = readByte(addr + 1, fc);
    if (lo == BUS_ERROR) busError(full, false, fc);
    return (uint16_t)((hi << 8) | lo);
}

void Machine::write8(uint32_t addr, uint8_t value, int fc) {
    busWait(addr & 0xffffff, true);
    if (!writeByte(addr, value, fc)) busError(addr, true, fc);
}

void Machine::write16(uint32_t full, uint16_t value, int fc) {
    uint32_t addr = full & 0xffffff;
    if (addr < config_.ramSize && !(fc == m68k::FC_USER_DATA || fc == m68k::FC_USER_PROG)) {
        ram_[addr] = (uint8_t)(value >> 8);
        ram_[addr + 1] = (uint8_t)value;
        return;
    }
    busWait(addr, true);
#ifndef PIX68K_NO_FPU_BOARD
    if ((addr & ~(FPU_BOARD_SELECT | (FpuBoard::SIZE - 1))) == FpuBoard::BASE) {
        FpuBoard* b = fpuBoards_[(addr & FPU_BOARD_SELECT) ? 1 : 0];
        if (b && !userBlocked(addr, fc)) {
            b->write16(addr & (FpuBoard::SIZE - 1), value);
            return;
        }
    }
#endif
    // 上位バイトの書き込みでバスエラーになれば、下位バイトは書かない
    if (!writeByte(addr, (uint8_t)(value >> 8), fc)) busError(full, true, fc);
    if (!writeByte(addr + 1, (uint8_t)value, fc)) busError(full, true, fc);
}

uint8_t Machine::peek8(uint32_t addr, bool& ok) {
    addr &= 0xffffff;
    // デバイスのレジスタは読むと状態が変わるものがあるので、メモリ (スプライト、SRAM、外付け SCSI の ROM) だけ読む
    if (addr >= 0xe80000 && addr < 0xf00000 && !(addr >= 0xeb0000 && addr < 0xec0000) &&
        !(addr >= 0xed0000 && addr < 0xed4000) &&
        !(addr >= SCSI_EX_BASE + 0x20 && addr < SCSI_EX_BASE + SCSI_EX_ROM_SIZE)) {
        ok = false;
        return 0;
    }
    int v = readByte(addr, m68k::FC_SUPER_DATA);
    ok = v != BUS_ERROR;
    return ok ? (uint8_t)v : 0;
}

} // namespace x68k
