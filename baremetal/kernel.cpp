#include "kernel.h"
#include "platform/net_files.h"

#include <circle/bcmpropertytags.h>
#include <circle/machineinfo.h>
#include <circle/memorymap.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <circle/util.h>

#include "bench/bench_program.h"
#include "event_wait.h"

static const char FromKernel[] = "pix68k";

// CTimer::MsDelay() から呼ぶ関数。tools/build_baremetal.sh が、ビルド先に写した Circle の include/circle/timer.h の
// MsDelay() を、これが nullptr でなければこれを呼ぶように書き換える (submodule は変えない)。ScanUsb() の間だけ設定する
extern "C" {
void (*volatile pix68k_msdelay_hook)(unsigned nMilliSeconds) = nullptr;
}

CKernel* CKernel::s_pThis = nullptr;


CKernel::CKernel()
    : m_Screen(m_Options.GetWidth(), m_Options.GetHeight()),
      m_Tee(&m_Serial, &m_Screen, &m_LogRing),
      m_Timer(&m_Interrupt),
      m_Logger(m_Options.GetLogLevel(), &m_Timer),
      m_USBHCI(&m_Interrupt, &m_Timer, TRUE),
      m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED)
#if RASPPI >= 5
      ,
      m_NVMe(&m_Interrupt)
#endif
{
    s_pThis = this;
    // 電源を入れてからの時刻 (Circle が読む ARM のカウンタは、タイマの初期化の前でも読める)。
    // 以前はここで ACT LED を 3 回点滅させていたが、その間 (2.1 秒) 何もせずに待つので、やめた
    m_BootTicks[BootKernel] = CTimer::GetClockTicks64();
}

CKernel::~CKernel() { s_pThis = nullptr; }

boolean CKernel::Initialize() {
    // tools/deploy_pi.sh から起動されたときは、止まってもウォッチドッグで再起動して Linux に戻れるようにする
    m_DeploySeconds = m_Options.GetAppOptionDecimal("pix68k_deploy", 0);
    if (m_DeploySeconds != 0) m_Watchdog.Start(CBcmWatchdog::MaxTimeoutSeconds);

    if (!InitScreen()) {
        // 画面を作れなかったことを UART に残す (ログはまだ初期化していないので、ここで直接書く)
        static const char msg[] =
            "pix68k: cannot create the 32-bit framebuffer"
#if RASPPI >= 5
            "; add framebuffer_depth=32 and framebuffer_ignore_alpha=1 to [pi5] in config.txt"
#endif
            "\n";
        if (m_Serial.Initialize(115200)) m_Serial.Write(msg, sizeof msg - 1);
        return FALSE;
    }
    SetPixelOrderBgr();
    if (!m_Serial.Initialize(115200)) return FALSE;
    // ログは UART に出し、画面にも出す
    if (!m_Logger.Initialize(&m_Tee)) return FALSE;
    if (!m_Interrupt.Initialize()) return FALSE;
    if (!m_Timer.Initialize()) return FALSE;
    m_BootTicks[BootDevices] = CTimer::GetClockTicks64();
    // Circle は何もしなければ ARM を低いクロックで動かす (Pi 3B+ は 1.4GHz (3B は 1.2GHz) ではなく 600MHz)。
    // X68000 を実時間で動かすため最大 (Pi 4B は 1.5GHz、config.txt に arm_boost=1 があれば 1.8GHz。Pi 400 は 1.8GHz、Pi 5 は 2.4GHz) にする。
    // 温度での調整 (CCPUThrottle::Update()) はしない (既定の 60 度で最低のクロックに落ちてしまう)。
    // 熱くなったときは、Linux のときと同じくファームウェアがクロックを下げるはず (Pi 3B+ の Linux では
    // 60 度で 1.2GHz になった。ベアメタルではまだ確かめていない)。下げたことは ReportCpu() が警告する
    // (ReportSpeed() から 10 秒ごと)
    m_pThrottle = new CCPUThrottle(CPUSpeedMaximum);
    if (m_pThrottle != nullptr) {
        // 要求したクロックは、最初の cpu: の行 (10 秒後) より前から画面の隅の数字に出す (UpdateOsd())
        m_CpuStatus.setMHz = m_pThrottle->GetClockRate() / 1000000;
        m_Logger.Write(FromKernel, LogNotice, "ARM clock: %u MHz (max %u MHz), %u C", m_CpuStatus.setMHz,
                       m_pThrottle->GetMaxClockRate() / 1000000, m_pThrottle->GetTemperature());
    }
    // 1GB より下のヒープの大きさを出すため (ReadHeap())。Circle の CMemorySystem が読むのと同じ値
    {
        CBcmPropertyTags tags;
        TPropertyTagMemory mem;
        if (tags.GetTag(PROPTAG_GET_ARM_MEMORY, &mem, sizeof mem) && mem.nBaseAddress == 0) m_ArmMemBytes = mem.nSize;
    }
#if RASPPI >= 5
    // Pi 5 のファンのコネクタ (Active Cooler など)。Linux と違って誰も回さないので、ここで回す (pi5_fan.h)。
    // まず全開にし、主ループに入ってから ControlFan() が温度で速さを決める
    m_Fan.Initialize(m_Logger, m_Options.GetAppOptionString("pix68k_fan", "auto"), m_Options.GetGPIOFanPin());
#endif
    KeepAlive();
    // USB はここでは初期化しない。装置を探すのに時間がかかる (Pi 3B+ ではハブ 2 段と LAN7800 で 2 秒ほど) ので、
    // X68000 を動かし始めてから ScanUsb() で行う
    MountBootDrive();
    m_BootTicks[BootMounted] = CTimer::GetClockTicks64();
    KeepAlive();
    return TRUE;
}

// USB のホストコントローラを初期化し、つながっている装置を探す。X68000 をコア 1 で動かし始めてから、コア 0 の主ループに
// 入る前に呼ぶ (探している間 (QEMU で約 2.8 秒) もコア 1 は IPL ROM を動かす)。serviceMachine なら、探している間は
// 合成するコアが画面を写し、探している途中の待ち時間 (CTimer::MsDelay()) に、コア 0 はハードディスクのイメージの
// チャンクを読み、書き戻す (WithComposerPresenting())。
// キーボード、マウス、ゲームパッドはこのあと UpdateUsbDevices() で見つかる。あとからつないだものも、Circle の
// プラグアンドプレイで同じように見つかる。2 回目からは何もしない。初期化に失敗しても、SD カードへのログの保存などはそのまま続ける
void CKernel::ScanUsb(bool serviceMachine) {
    if (m_UsbStarted) return;
    m_UsbStarted = true;
    KeepAlive();
    auto scan = [this] {
        m_UsbOk = m_USBHCI.Initialize();
        if (!m_UsbOk) m_Logger.Write(FromKernel, LogError, "USB: initialize failed");
        KeepAlive();
        LogBootTime(BootUsb, "USB devices scanned");
    };
    if (serviceMachine && m_pEmulator != nullptr) {
        WithComposerPresenting([&] {
            scan();
            // 最初の UpdatePlugAndPlay() は、探している間にたまったハブのポートの変化を処理し、キーボードなどを
            // 登録する (QEMU で 0.1 秒ほど)。これも合成するコアが画面を写している間に済ませる
            UpdateUsbDevices();
        });
        m_Logger.Write(FromKernel, LogNotice, "screen: %u frames shown by the composing core during the USB scan",
                       m_pEmulator->framesSelfPresented());
    } else {
        scan();
    }
}

// ScanUsb() の間の CTimer::MsDelay()。待つ間、コア 0 の仕事 (ServiceDuringUsbScan()) を回す。
// コア 0 のタスクの段階で、割り込みを止めていないときだけ (SD カードや NVMe の読み込みを割り込みの中や、
// スピンロックを持ったままで行わないため)。入れ子にはしない。そうでなければふつうに待つ。
// 頼まれた時間は下限で、ServiceDisks() が SD カードを読み書きしている間はそれより長く待つことがある
// (Circle の DWHCI の WaitForBit() などは MsDelay(1) を回数で数えるので、そのタイムアウトも同じだけ延びる。
// 待つ時間が短くなることはないので、USB の装置の扱いには影響しない)
// boot ドライブが USB メモリのときは、ここからはイメージを読み書きしない (ServiceDuringUsbScan() を参照)
void CKernel::UsbScanDelay(unsigned ms) {
    CKernel* k = s_pThis;
    u64 daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    if (k == nullptr || k->m_InScanDelay || CMultiCoreSupport::ThisCore() != 0 ||
        CurrentExecutionLevel() != TASK_LEVEL || (daif & (1u << 7)) != 0) {
        CTimer::SimpleMsDelay(ms);
        return;
    }
    k->m_InScanDelay = true;
    const u64 end = CTimer::GetClockTicks64() + (u64)ms * (CLOCKHZ / 1000);
    for (u64 now = CTimer::GetClockTicks64(); now < end; now = CTimer::GetClockTicks64()) {
        k->ServiceDuringUsbScan(true);
        // することがなくても回り続けないよう、少し待つ (最大 0.2ms)
        now = CTimer::GetClockTicks64();
        if (now < end) {
            const u64 left = (end - now) / (CLOCKHZ / 1000000);
            k->IdleUntil(now + (left < 200 ? left + 1 : 200));
        }
    }
    k->m_InScanDelay = false;
}

// 32 ビットの画素の並びを、Circle が前提にしている ARGB8888 (メモリ上は B, G, R, A の順) に揃える。
// Pi のファームウェアのメールボックスの「Set pixel order」(0x00048006) は 0 = BGR、1 = RGB で、
// 実機のファームウェアは何もしなければ BGR なので (Circle は設定せずに COLOR32 で赤を bit 16-23 に置いている)、
// 実機では何も変わらない。QEMU (raspi3b) は何もしなければ RGB として表示し、赤と青が入れ替わるので、明示的に設定する。
// 対応していないファームウェアでは失敗するが、そのときはそのまま進む
void CKernel::SetPixelOrderBgr() {
    TPropertyTagSimple tag;
    tag.nValue = 0; // BGR
    CBcmPropertyTags tags;
    if (!tags.GetTag(0x00048006, &tag, sizeof tag, 4)) m_PixelOrderResult = -1;
    else m_PixelOrderResult = (int)tag.nValue;
}

// フレームバッファを作る。カーネルのオプション pix68k_gpu_scale=N (縦横とも N 倍) か pix68k_gpu_scale=AxB (横 A 倍、
// 縦 B 倍。1 から 4) があれば、フレームバッファを表示の大きさ (width= / height=、なければファームウェアの表示の大きさ)
// の 1/A x 1/B にし、表示の大きさへの拡大はファームウェア (GPU) に任せる。X68000 の画面を写すときの拡大がその分
// 小さくなり、フレームバッファに書く量が減る (1920x1080 で pix68k_gpu_scale=2 なら、768x512 の画面は 1536x1024 では
// なく 768x512 を書く)。GPU の拡大はファームウェアの設定 (config.txt の scaling_kernel) で補間するので、ドットが
// にじむことがある。表示の大きさが割り切れないか、768x512 が入らなくなるときは使わない (ログに出す)。
// ログはまだ使えないので、結果は m_GpuScaleNote に置き、Run() がログに出す
boolean CKernel::InitScreen() {
    const char* opt = m_Options.GetAppOptionString("pix68k_gpu_scale", nullptr);
    if (opt == nullptr) return m_Screen.Initialize();
    unsigned gx = 0, gy = 0;
    const char* p = opt;
    while (*p >= '0' && *p <= '9') gx = gx * 10 + (unsigned)(*p++ - '0');
    if (*p == 'x') {
        p++;
        while (*p >= '0' && *p <= '9') gy = gy * 10 + (unsigned)(*p++ - '0');
    } else {
        gy = gx;
    }
    if (*p != 0 || gx < 1 || gx > 4 || gy < 1 || gy > 4) {
        m_GpuScaleNote = GpuScaleBadOption;
        return m_Screen.Initialize();
    }
    unsigned w = m_Options.GetWidth(), h = m_Options.GetHeight();
    if (w == 0 || h == 0) {
        TPropertyTagDisplayDimensions dim;
        CBcmPropertyTags tags;
        if (!tags.GetTag(PROPTAG_GET_DISPLAY_DIMENSIONS, &dim, sizeof dim)) {
            m_GpuScaleNote = GpuScaleNoDisplay;
            return m_Screen.Initialize();
        }
        w = dim.nWidth;
        h = dim.nHeight;
    }
    m_GpuDisplayW = w;
    m_GpuDisplayH = h;
    if (w % gx != 0 || h % gy != 0 || w / gx < (unsigned)platform::kScaleBoxW || h / gy < (unsigned)platform::kScaleBoxH) {
        m_GpuScaleNote = GpuScaleDoesNotFit;
        return m_Screen.Initialize();
    }
    // CScreenDevice::Resize() は、まだ作っていなければ作るだけ (Initialize() と同じ)。作るときの大きさを書き換えるので、
    // 失敗したら元の大きさ (width= / height=。なければ 0 で、Circle が表示の大きさにする) を渡して作り直す
    // (Initialize() では縮めた大きさのままになる)。それも失敗すれば、元の大きさでも作れないので FALSE を返す
    if (!m_Screen.Resize(w / gx, h / gy)) {
        m_GpuScaleNote = GpuScaleFailed;
        return m_Screen.Resize(m_Options.GetWidth(), m_Options.GetHeight());
    }
    m_GpuScaleX = (int)gx;
    m_GpuScaleY = (int)gy;
    m_GpuScaleNote = GpuScaleOn;
    return TRUE;
}

// InitScreen() の結果をログに出す
void CKernel::LogGpuScale() {
    switch (m_GpuScaleNote) {
    case GpuScaleOff:
        break;
    case GpuScaleOn:
        m_Logger.Write(FromKernel, LogNotice,
                       "screen: pix68k_gpu_scale: framebuffer %ux%u, scaled x%d y%d by the GPU to %ux%u "
                       "(scaling_kernel=8 in config.txt for nearest neighbour)",
                       m_Screen.GetWidth(), m_Screen.GetHeight(), m_GpuScaleX, m_GpuScaleY, m_GpuDisplayW, m_GpuDisplayH);
        break;
    case GpuScaleBadOption:
        m_Logger.Write(FromKernel, LogWarning, "screen: pix68k_gpu_scale: use N or AxB (1 to 4); not scaled by the GPU");
        break;
    case GpuScaleNoDisplay:
        m_Logger.Write(FromKernel, LogWarning, "screen: pix68k_gpu_scale: display size unknown; not scaled by the GPU");
        break;
    case GpuScaleDoesNotFit:
        m_Logger.Write(FromKernel, LogWarning,
                       "screen: pix68k_gpu_scale: display %ux%u is not divisible or 768x512 does not fit; "
                       "not scaled by the GPU",
                       m_GpuDisplayW, m_GpuDisplayH);
        break;
    case GpuScaleFailed:
        m_Logger.Write(FromKernel, LogWarning, "screen: pix68k_gpu_scale: framebuffer not allocated; not scaled by the GPU");
        break;
    }
}

// ファームウェアのオーバースキャン (メールボックスの「Get overscan」0x0004000a。上、下、左、右の画素数) をログに出す。
// config.txt に disable_overscan=1 がないと、ファームウェアは 1920x1080 などの HDMI のモードで上下左右を空け
// (1824x984 など)、Circle が決めるフレームバッファもその分小さくなる。768x512 の 2 倍 (1536x1024) が入らなくなり、
// X68000 の画面を拡大しなくなるので、そのときは disable_overscan=1 を書くよう出す。読めなければ (QEMU など) 何もしない
void CKernel::LogOverscan() {
    struct {
        TPropertyTag Tag;
        u32 top, bottom, left, right;
    } tag;
    CBcmPropertyTags tags;
    if (!tags.GetTag(0x0004000a, &tag, sizeof tag)) {
        m_Logger.Write(FromKernel, LogNotice, "screen: overscan unknown");
        return;
    }
    if (tag.top == 0 && tag.bottom == 0 && tag.left == 0 && tag.right == 0) {
        m_Logger.Write(FromKernel, LogNotice, "screen: overscan off");
        return;
    }
    m_Logger.Write(FromKernel, LogWarning,
                   "screen: overscan on (top %u, bottom %u, left %u, right %u); add disable_overscan=1 to config.txt "
                   "to use the whole screen",
                   tag.top, tag.bottom, tag.left, tag.right);
}

// deploy のときは、初期化の途中でもウォッチドッグを延長する (初期化全体が 15 秒を超えてもリセットされないように)
void CKernel::KeepAlive() {
    if (m_DeploySeconds != 0) m_Watchdog.Start(CBcmWatchdog::MaxTimeoutSeconds);
}

// NVMe (Pi 5 だけ) と SD カードの boot パーティション (FAT) をマウントし、読み書きするドライブ (boot ドライブ) を仮に決める。
// 選び方は platform/boot_drive.h。deploy のときは、deploy_pi.sh が cmdline の pix68k_id と同じ値を /pix68k/id.txt に
// 書いているので、値が一致するドライブを選ぶ (別の FAT のディスクがつながっていても、起動元にログを書くため)。
// USB メモリは USB の装置を探してからでないと使えないので、ここでは見ない。NVMe と SD カードで決まらなければ、
// Run() が FinishBootDrive() で USB メモリを探して決め直す。
// cmdline.txt の pix68k_drive=sd / nvme / usb で、使うドライブを 1 つに決められる (ほかのドライブは初期化しない)
// USB メモリは Circle の umsd1〜umsd3。Circle の FatFs (addon/fatfs/diskio.cpp) が USB: / USB2: / USB3: に割り当てている
static const char* const kBootDriveNames[platform::BOOT_DRIVES] = {"NVME:", "SD:", "USB:", "USB2:", "USB3:"};
static const char* const kUsbDeviceNames[platform::BOOT_USB_DRIVES] = {"umsd1", "umsd2", "umsd3"};

void CKernel::MountBootDrive() {
    m_WantId = m_Options.GetAppOptionDecimal("pix68k_id", 0);
    const char* opt = m_Options.GetAppOptionString("pix68k_drive", nullptr);
    m_DriveOption = platform::parseBootDriveOption(opt);
    if (m_DriveOption == platform::BootDriveOption::INVALID)
        m_Logger.Write(FromKernel, LogWarning, "pix68k_drive=%s: use auto, sd, nvme or usb; using auto", opt);
#if RASPPI >= 5
    if (platform::bootDriveAllowed(m_DriveOption, platform::BOOT_NVME) && m_NVMe.Initialize()) {
        FRESULT r = f_mount(&m_NvmeFs, "NVME:", 1);
        if (r == FR_OK) ProbeBootDrive(platform::BOOT_NVME);
        else {
            // f_mount(..., 1) は失敗しても FATFS の登録を残すので、外しておく
            f_unmount("NVME:");
            m_Logger.Write(FromKernel, LogWarning, "NVMe: mount failed (FRESULT %d)", (int)r);
        }
    }
#endif
    // deploy で NVMe の値が一致したら、SD カードは初期化しない
    const platform::BootDriveState& nvme = m_Drives[platform::BOOT_NVME];
    const bool nvmeMatched = m_WantId != 0 && nvme.mounted && nvme.id == m_WantId;
    if (platform::bootDriveAllowed(m_DriveOption, platform::BOOT_SD) && !nvmeMatched && m_EMMC.Initialize()) {
        FRESULT r = f_mount(&m_SdFs, "SD:", 1);
        if (r == FR_OK) ProbeBootDrive(platform::BOOT_SD);
        else {
            f_unmount("SD:"); // 失敗しても FATFS の登録が残るので外す
            m_Logger.Write(FromKernel, LogWarning, "SD card: mount failed (FRESULT %d)", (int)r);
        }
    }
    SelectBootDrive();
}

// マウントできたドライブに /pix68k のディレクトリがあるかと、/pix68k/id.txt の値を調べる
void CKernel::ProbeBootDrive(int drive) {
    platform::BootDriveState& d = m_Drives[drive];
    d.mounted = true;
    CString path;
    path.Format("%s/pix68k", kBootDriveNames[drive]);
    FILINFO fi;
    d.hasDir = f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR) != 0;
    d.id = d.hasDir ? ReadDeployId(kBootDriveNames[drive]) : 0;
}

void CKernel::SelectBootDrive() {
    const int pick = platform::pickBootDrive(m_Drives, m_WantId, m_DriveOption);
    m_BootDrive = pick < 0 ? nullptr : kBootDriveNames[pick];
    m_BootOnUsb = platform::isUsbBootDrive(pick);
    m_BootUsbFs = m_BootOnUsb ? &m_UsbFs[pick - platform::BOOT_USB] : nullptr;
}

// NVMe と SD カードで boot ドライブが決まらなければ (どちらもないか、/pix68k がない。pix68k_drive=usb のときも)、
// USB の装置を探し、USB メモリ (Circle の umsd1〜umsd3。FatFs の "USB:"、"USB2:"、"USB3:") のそれぞれの最初の FAT の
// パーティションをマウントして、boot ドライブを決め直す (USB メモリが 2 本以上あれば、/pix68k (deploy のときは
// /pix68k/id.txt の値) で選ぶ)。選ばなかった USB メモリはアンマウントする。決まったドライブをログに出す。
// USB の装置は、ふだんは X68000 を動かし始めてから探すが (ScanUsb())、このときは ROM を読む前に探す (起動はその分
// 遅くなる)。NVMe か SD カードに /pix68k があれば USB メモリは探さず、探す時期も変わらない。
// USB メモリは、つないでから使えるようになるまで時間がかかるものがあり、ハブの先のものは直結のものより遅れて出てくる
// ので、探し終えたあとも、規則で決まる USB メモリ (/pix68k があるか、deploy の値が一致する) が出てくるか、USB メモリが
// 出てきて装置の構成が 0.5 秒変わらなくなるまで待つ (長くても cmdline.txt の pix68k_usb_wait=ミリ秒。既定 2000)。
// 待つ間は Circle のプラグアンドプレイの処理だけを回し、
// キーボードなどの登録は、pix68k.cfg を読んだあとの UpdateUsbDevices() に任せる (m_UsbDevicesPending)
// USB メモリが削除されたときの通知 (CDevice::RegisterRemovedHandler())。FinishBootDrive() の見張りに知らせる
static void UsbDriveRemoved(CDevice*, void* context) { static_cast<platform::UsbSlotWatch*>(context)->gone = true; }

void CKernel::FinishBootDrive() {
    if (platform::needUsbDrive(m_Drives, m_WantId, m_DriveOption)) {
        const u64 t0 = CTimer::GetClockTicks64();
        ScanUsb(false);
        // 待つ時間は、探し終えてから数える (探すだけで Pi 3B+ は 2 秒ほどかかる)
        const u64 scanned = CTimer::GetClockTicks64();
        const u64 wait = (u64)m_Options.GetAppOptionDecimal("pix68k_usb_wait", 2000) * (CLOCKHZ / 1000);
        // umsdN の見張り (platform::UsbSlotWatch)。調べた装置には削除の通知を付け、抜かれたら (同じ番号とアドレスで
        // 別の USB メモリが出てきても) 前の結果を捨てて調べ直す。通知の登録は、待ち終えたときにまだある装置から外す
        CDevice::TRegistrationHandle removedHandle[platform::BOOT_USB_DRIVES] = {};
        for (int k = 0; k < platform::BOOT_USB_DRIVES; k++) m_UsbWatch[k] = platform::UsbSlotWatch{};
        bool any = false; // 今つながっている USB メモリがある
        u64 alive = scanned;
        u64 changed = scanned; // 最後に USB の装置の構成が変わった時刻
        while (m_UsbOk) {
            if (m_USBHCI.UpdatePlugAndPlay()) {
                m_UsbDevicesPending = true;
                changed = CTimer::GetClockTicks64();
            }
            // umsdN が出てきた、抜かれた、別の USB メモリに替わったときは、マウントし直して調べる
            any = false;
            bool chosen = false; // 規則で決まる USB メモリ (/pix68k があるか、deploy の値が一致する) がつながっている
            for (int k = 0; k < platform::BOOT_USB_DRIVES; k++) {
                const int drive = platform::BOOT_USB + k;
                CDevice* dev = m_DeviceNameService.GetDevice(kUsbDeviceNames[k], TRUE);
                const platform::UsbSlotAction act = m_UsbWatch[k].update(dev);
                if (act == platform::UsbSlotAction::DROP || act == platform::UsbSlotAction::DROP_AND_PROBE) {
                    // 削除された装置の通知の登録は、Circle が装置と一緒に消している
                    removedHandle[k] = 0;
                    if (m_Drives[drive].mounted) f_unmount(kBootDriveNames[drive]);
                    m_Drives[drive] = platform::BootDriveState{};
                    m_Logger.Write(FromKernel, LogNotice, "USB drive: %s (%s) removed", kBootDriveNames[drive],
                                   kUsbDeviceNames[k]);
                    changed = CTimer::GetClockTicks64();
                }
                if (act == platform::UsbSlotAction::PROBE || act == platform::UsbSlotAction::DROP_AND_PROBE) {
                    removedHandle[k] = dev->RegisterRemovedHandler(UsbDriveRemoved, &m_UsbWatch[k]);
                    changed = CTimer::GetClockTicks64();
                    MountUsbDrive(k, (unsigned)((changed - t0) / (CLOCKHZ / 1000)));
                }
                if (dev == nullptr) continue;
                any = true;
                const platform::BootDriveState& d = m_Drives[drive];
                if (d.mounted && (m_WantId != 0 ? d.id == m_WantId : d.hasDir)) chosen = true;
            }
            const u64 now = CTimer::GetClockTicks64();
            // 規則で決まる USB メモリが見つかるか、USB メモリが出てきて構成が kUsbSettle 変わらなければやめる。
            // ハブの先の USB メモリは、直結のものより遅れて出てくる (Pi 4 で Linux の USB メモリが USB 3.0 のポートに
            // 直結、データ用が USB 2.0 のハブ経由のとき) ので、最初の 1 本が出た時点ではやめない
            if (platform::usbWaitDone(chosen, any, now, changed, scanned, CLOCKHZ / 2, wait)) break;
            // deploy のときは、待つ間もウォッチドッグを延ばす (1 秒ごと)
            if (now - alive >= CLOCKHZ) {
                KeepAlive();
                alive = now;
            }
            IdleUntil(now + CLOCKHZ / 100);
        }
        KeepAlive();
        // 待ち終えたときにまだある装置から、削除の通知を外す (m_UsbWatch を書き換えないように)
        for (int k = 0; k < platform::BOOT_USB_DRIVES; k++) {
            if (removedHandle[k] != 0 && !m_UsbWatch[k].gone && m_UsbWatch[k].device != nullptr)
                ((CDevice*)m_UsbWatch[k].device)->UnregisterRemovedHandler(removedHandle[k]);
        }
        if (!any) {
            // 待つ間に出てきて抜かれたものは、上で外してある
            const unsigned ms = (unsigned)((CTimer::GetClockTicks64() - t0) / (CLOCKHZ / 1000));
            m_Logger.Write(FromKernel, LogNotice, "USB drive: no mass storage device after %u ms", ms);
        }
        SelectBootDrive();
        // 選ばなかった USB メモリはアンマウントする (読み書きしないことをはっきりさせるため)
        for (int k = 0; k < platform::BOOT_USB_DRIVES; k++) {
            const int drive = platform::BOOT_USB + k;
            if (m_Drives[drive].mounted && m_BootUsbFs != &m_UsbFs[k]) f_unmount(kBootDriveNames[drive]);
        }
    }
    if (m_BootDrive != nullptr) {
        m_Logger.Write(FromKernel, LogNotice, "boot drive: %s", m_BootDrive);
    } else if (m_WantId != 0) {
        // deploy なのに起動元が分からないときは、ログを書かない (ほかのディスクを書き換えないため)
        m_Logger.Write(FromKernel, LogWarning, "deploy: no drive has /pix68k/id.txt = %u", m_WantId);
    }
}

// USB メモリ k 本目 (umsd1 なら 0) の最初の FAT のパーティションをマウントし、/pix68k と /pix68k/id.txt を調べてログに出す。
// ms は USB の装置を探し始めてからの時間。マウントできなければ false
bool CKernel::MountUsbDrive(int k, unsigned ms) {
    const int drive = platform::BOOT_USB + k;
    FRESULT r = f_mount(&m_UsbFs[k], kBootDriveNames[drive], 1);
    if (r != FR_OK) {
        // f_mount(..., 1) は失敗しても FATFS の登録を残すので、外しておく (mounted は false のまま)
        f_unmount(kBootDriveNames[drive]);
        m_Drives[drive] = platform::BootDriveState{};
        m_Logger.Write(FromKernel, LogWarning, "USB drive: %s (%s) mount failed (FRESULT %d)", kBootDriveNames[drive],
                       kUsbDeviceNames[k], (int)r);
        return false;
    }
    ProbeBootDrive(drive);
    const platform::BootDriveState& d = m_Drives[drive];
    if (k == 0)
        m_Logger.Write(FromKernel, LogNotice, "USB drive: mounted %u ms after the USB scan started (/pix68k %s)", ms,
                       d.hasDir ? "found" : "not found");
    else
        m_Logger.Write(FromKernel, LogNotice, "USB drive: %s (%s) mounted %u ms after the USB scan started (/pix68k %s)",
                       kBootDriveNames[drive], kUsbDeviceNames[k], ms, d.hasDir ? "found" : "not found");
    if (d.id != 0) m_Logger.Write(FromKernel, LogNotice, "USB drive: %s/pix68k/id.txt = %u", kBootDriveNames[drive], d.id);
    return true;
}

// boot ドライブが USB メモリのとき、path (boot ドライブの名前で始まる "USB:/pix68k/HUMAN.HDS" の形) が /pix68k の下か。
// USB メモリの FAT のパーティションには Linux の起動ファイルが入っていることがあるので、/pix68k の外には書かない
// (pix68k.cfg に / で始まる名前で書いたイメージは書き込み禁止にし、ネットワークの口からの書き込みと削除は断る)。
// 判定は platform/boot_drive.h の usbPathWritable()。NVMe と SD カードのときは、前と同じくどこにでも書く
bool CKernel::UsbPathWritable(const char* path) {
    if (!m_BootOnUsb) return true;
    const size_t dn = strlen(m_BootDrive);
    return strncmp(path, m_BootDrive, dn) == 0 && platform::usbPathWritable(path + dn);
}

// boot ドライブが USB メモリのとき、pathInDrive (ドライブの中のパス) に kind の操作 (消す、bytes バイトの新しい
// ファイルを書く、ディレクトリを作る) をしてよいか。/pix68k の下で、書いても kUsbDriveReserve の空きが残るときだけ
// (Linux の起動ファイルの置き場所を使い切らないため)。使う大きさは、中身をクラスタに切り上げたものに、名前を足して
// ディレクトリが伸びる分の余裕 (64KiB) を足して数える (platform::usbWriteNeed())。消すだけなら空きは読まない。
// 空きは FatFs の f_getfree() で読む。FAT32 では FSInfo に書かれた空きの数をそのまま使うので、Linux が正しく
// アンマウントせずに終わったあとなどは、実際の空きとずれていることがある。
// NVMe と SD カードのときは調べない
static const unsigned long long kUsbDriveReserve = 32ull << 20;

platform::UsbWriteCheck CKernel::CheckUsbWrite(const char* pathInDrive, platform::UsbWriteKind kind,
                                               unsigned long long bytes) {
    if (!m_BootOnUsb) return platform::UsbWriteCheck::OK;
    unsigned long long freeBytes = 0;
    const bool known = kind == platform::UsbWriteKind::REMOVE || UsbFreeBytes(freeBytes);
    return platform::checkUsbWrite(true, pathInDrive, kind, bytes, known, freeBytes, kUsbDriveReserve,
                                   m_UsbBudget.pending, netClusterBytes());
}

// USB メモリの空き (f_getfree())。読めなければ false
bool CKernel::UsbFreeBytes(unsigned long long& freeBytes) {
    DWORD clusters = 0;
    FATFS* fs = nullptr;
    freeBytes = 0;
    if (m_BootDrive == nullptr || f_getfree(m_BootDrive, &clusters, &fs) != FR_OK || fs == nullptr) return false;
    freeBytes = (unsigned long long)clusters * netClusterBytes();
    return true;
}

// boot ドライブが USB メモリのときの、クラスタの大きさ (空きの判定はクラスタの単位で数える)。ほかのドライブでは 1
unsigned long long CKernel::netClusterBytes() {
    return m_BootUsbFs != nullptr ? (unsigned long long)m_BootUsbFs->csize * FF_MIN_SS : 1;
}

// <ドライブ>/pix68k のディレクトリを作る (すでにあれば何もしない)。USB メモリでは、新しいディレクトリの 1 クラスタと、
// ルートのディレクトリが伸びる分の余裕 (platform::usbDirMargin()) を使っても空きが残るときだけ作る
void CKernel::MakeBootDir() {
    if (CheckUsbWrite("/pix68k/x", platform::UsbWriteKind::NEW_DIR, 0) != platform::UsbWriteCheck::OK) return;
    CString path;
    path.Format("%s/pix68k", m_BootDrive);
    f_mkdir(path); // すでにあれば失敗するが構わない
}

// path (boot ドライブの名前で始まる) に名前を変えてよいか (途中で止まった置き換えを戻すとき。platform::replaceFile())。
// 名前を変えるとディレクトリが伸びることがあるので、USB メモリでは新しいファイルと同じく空きを確かめる
bool CKernel::CanRenameFile(const char* path) {
    CKernel* k = s_pThis;
    if (k == nullptr || !k->m_BootOnUsb) return true;
    const size_t dn = strlen(k->m_BootDrive);
    return strncmp(path, k->m_BootDrive, dn) == 0 &&
           k->CheckUsbWrite(path + dn, platform::UsbWriteKind::RENAME, 0) == platform::UsbWriteCheck::OK;
}

// path (boot ドライブの名前で始まる) に size バイトの新しいファイルを書いてよいか (ステート、pix68k.cfg、SRAM、
// フロッピーディスクのイメージの .new)。boot ドライブが USB メモリのときだけ調べる (CheckUsbWrite())
bool CKernel::CanWriteNewFile(const char* path, unsigned size) {
    CKernel* k = s_pThis;
    if (k == nullptr || !k->m_BootOnUsb) return true;
    const size_t dn = strlen(k->m_BootDrive);
    return strncmp(path, k->m_BootDrive, dn) == 0 &&
           k->CheckUsbWrite(path + dn, platform::UsbWriteKind::NEW_FILE, size) == platform::UsbWriteCheck::OK;
}

// ネットワークの口: path を消してよいか (bytes が負)、path に bytes バイト (0 でもよい) のファイルを受け取り始めてよいか。
// 受け取り始めるときは、使う大きさ (中身をクラスタに切り上げたものと、ディレクトリが伸びる分) を m_UsbBudget に足す (受け取りの途中のほかの保存の
// 判定から引くため)。呼んだ側は、書いて減った分と、やめたときの残りを netWriteDone() で返す (platform::UploadPlan で数える)
bool CKernel::netCanWrite(const char* path, long long bytes, const char*& why) {
    unsigned long long freeBytes = 0;
    const bool known = !m_BootOnUsb || bytes < 0 || UsbFreeBytes(freeBytes);
    switch (platform::beginNetWrite(m_UsbBudget, m_BootOnUsb, path, bytes, known, freeBytes, kUsbDriveReserve,
                                    netClusterBytes())) {
    case platform::UsbWriteCheck::OK: return true;
    case platform::UsbWriteCheck::OUTSIDE:
        why = "the boot drive is a USB drive; only files under /pix68k/ can be written or deleted\n";
        return false;
    default:
        why = "not enough free space on the USB drive (32 MB is kept free)\n";
        return false;
    }
}

void CKernel::netWriteDone(unsigned long long bytes) { m_UsbBudget.end(bytes); }

// <drive>/pix68k/id.txt の数値を返す。読めなければ 0
unsigned CKernel::ReadDeployId(const char* drive) {
    CString path;
    path.Format("%s/pix68k/id.txt", drive);
    FIL f;
    if (f_open(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return 0;
    char buf[16];
    UINT n = 0;
    FRESULT r = f_read(&f, buf, sizeof buf - 1, &n);
    f_close(&f);
    if (r != FR_OK) return 0;
    unsigned v = 0;
    for (UINT i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) v = v * 10 + (unsigned)(buf[i] - '0');
    return v;
}

// 16MB あるので .bss ではなくヒープに置く (カーネルの大きさの上限を超えるため)
static bench::Memory* s_benchMemory;

void CCoreRunner::Run(unsigned core) {
    if (failFrom != 0 && core >= failFrom) return; // 起動しなかったことにする (startedMask にも残さない)
    __atomic_fetch_or(&startedMask, 1u << core, __ATOMIC_SEQ_CST);
    if (core == 2 && emulator != nullptr) emulator->runRender(); // 合成をコア 2 で行うなら戻らない
    if (core == 3 && emulator != nullptr) emulator->runSound(); // 音の合成をコア 3 で行うなら戻らない
    if (core != 1) return;
    if (emulator != nullptr) emulator->run(); // 戻らない
    if (s_benchMemory == nullptr) return;
    memset(s_benchMemory->m, 0, sizeof s_benchMemory->m);
    bench::load(*s_benchMemory);
    m68k::Cpu cpu(s_benchMemory);
    cpu.reset();
    const int64_t total = 200000000LL; // 2 億サイクル
    u64 t0 = CTimer::GetClockTicks64();
    int64_t done = cpu.run(total);
    u64 t1 = CTimer::GetClockTicks64();
    __atomic_store_n(&benchCycles, (unsigned)done, __ATOMIC_RELAXED);
    __atomic_store_n(&benchMicros, (unsigned)((t1 - t0) * 1000000ULL / CLOCKHZ), __ATOMIC_RELAXED);
    __atomic_store_n(&benchDone, true, __ATOMIC_RELEASE);
}

void CKernel::ReportBenchmark() {
    // benchDone を acquire で読んだあとに呼ぶので、relaxed で読んでよい
    unsigned cycles = __atomic_load_n(&m_Cores.benchCycles, __ATOMIC_RELAXED);
    unsigned us = __atomic_load_n(&m_Cores.benchMicros, __ATOMIC_RELAXED);
    if (us == 0) us = 1;
    unsigned mhz10 = (unsigned)((u64)cycles * 10 / us); // 0.1MHz 単位
    m_Logger.Write(FromKernel, LogNotice,
                   "68000 benchmark (core 1): %u cycles in %u us = %u.%u MHz (x%u.%u of 16MHz)",
                   cycles, us, mhz10 / 10, mhz10 % 10, mhz10 / 160, (mhz10 % 160) * 10 / 160);
}

// HDMI のフレームバッファに直接描けることを確かめるため、画面の右上にカラーバーを描く
// (右上の角の 1 文字分は Rotor が使うので、その下から描く)
#if DEPTH == 32
#define BAR_COLOR(r, g, b) ((TScreenColor)(0xff000000u | (r) * 0xff0000u | (g) * 0xff00u | (b) * 0xffu))
#else
#define BAR_COLOR(r, g, b) COLOR16((r) * 31, (g) * 31, (b) * 31)
#endif

void CKernel::DrawTestPattern() {
    static const TScreenColor bars[8] = {
        BAR_COLOR(1, 1, 1), BAR_COLOR(1, 1, 0), BAR_COLOR(0, 1, 1), BAR_COLOR(0, 1, 0),
        BAR_COLOR(1, 0, 1), BAR_COLOR(1, 0, 0), BAR_COLOR(0, 0, 1), BAR_COLOR(0, 0, 0),
    };
    const unsigned w = 256, h = 64;
    unsigned x0 = m_Screen.GetWidth() - w, y0 = 32;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) m_Screen.SetPixel(x0 + x, y0 + y, bars[x * 8 / w]);
}

// boot パーティションのルートの一覧と、/pix68k/hello.txt の中身をログに出す
void CKernel::ListBootDrive() {
    if (m_BootDrive == nullptr) {
        m_Logger.Write(FromKernel, LogWarning, "boot drive: not mounted");
        return;
    }
    CString path;
    path.Format("%s/", m_BootDrive);
    DIR dir;
    FILINFO info;
    FRESULT r = f_findfirst(&dir, &info, path, "*");
    if (r != FR_OK) {
        m_Logger.Write(FromKernel, LogWarning, "%s cannot read root directory (FRESULT %d)", m_BootDrive, (int)r);
        return;
    }
    m_Logger.Write(FromKernel, LogNotice, "%s root:", m_BootDrive);
    for (unsigned i = 0; r == FR_OK && info.fname[0] && i < 32; i++) {
        m_Logger.Write(FromKernel, LogNotice, "  %s%s (%u bytes)", info.fname, (info.fattrib & AM_DIR) ? "/" : "",
                       (unsigned)info.fsize);
        r = f_findnext(&dir, &info);
    }
    f_closedir(&dir);

    path.Format("%s/pix68k/hello.txt", m_BootDrive);
    FIL f;
    if (f_open(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        m_Logger.Write(FromKernel, LogNotice, "%s not found", (const char*)path);
        return;
    }
    char buf[81];
    UINT n = 0;
    r = f_read(&f, buf, sizeof buf - 1, &n);
    f_close(&f);
    if (r != FR_OK) {
        m_Logger.Write(FromKernel, LogWarning, "%s read failed (FRESULT %d)", (const char*)path, (int)r);
        return;
    }
    buf[n] = 0;
    for (UINT i = 0; i < n; i++)
        if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';
    m_Logger.Write(FromKernel, LogNotice, "pix68k/hello.txt: %s", buf);
}

// deploy_pi.sh から起動されたとき: ログを boot パーティションの /pix68k/log.txt に書く (このあと再起動する)。
// timeUp は決めた秒数がたったとき (false は Ctrl + Alt + Delete のとき)
void CKernel::FinishDeploy(bool timeUp) {
    if (timeUp)
        m_Logger.Write(FromKernel, LogNotice, "deploy: %u seconds passed, saving log and rebooting", m_DeploySeconds);
    else
        m_Logger.Write(FromKernel, LogNotice, "deploy: saving log and rebooting (Ctrl+Alt+Delete)");
    if (m_BootDrive == nullptr) return;
    MakeBootDir();
    CString path;
    path.Format("%s/pix68k/log.txt", m_BootDrive);
    // USB メモリでは、ログ (最大 LOGGER_BUFSIZE) を書いても空きが残るときだけ書く
    if (!CanWriteNewFile(path, LOGGER_BUFSIZE)) {
        m_Logger.Write(FromKernel, LogWarning, "deploy: %s not written (not enough free space on the USB drive)",
                       (const char*)path);
        return;
    }
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    // CLogger はログの文字列を 16KB のリングバッファに持っている
    static char buf[LOGGER_BUFSIZE];
    int n = m_Logger.Read(buf, sizeof buf, FALSE);
    UINT written;
    if (n > 0) f_write(&f, buf, (UINT)n, &written);
    f_close(&f);
    f_unmount(m_BootDrive);
}

// コア 0 のタイマの割り込みから 100Hz で呼ばれ、すべてのコアに sev を送る (StartEventStream())
static void SevFromTimer() { asm volatile("dsb ish; sev" ::: "memory"); }

// コア 0 でイベントストリームを有効にし、届くかを確かめる (ほかのコアを起動する前に呼ぶ)。
// 2ms 眠って戻った回数を数え、周期から見込んだ回数の半分に届かなければ使わない (待つ間は前と同じく空回りする)。
// 周期が 2ms より長いと見込みが 0 回になり確かめられないので、そのときも使わない (platform::streamProbeOk())。
// コア 1 も、ここで届いたときだけイベントストリームを使う (Emulator::setEventStream())。
// カーネルのオプション pix68k_wfe=0 なら使わない (前と同じく空回りで待つ。温度や速さを比べるため)
void CKernel::StartEventStream() {
    if (m_Options.GetAppOptionDecimal("pix68k_wfe", 1) == 0) {
        m_Logger.Write(FromKernel, LogNotice, "event stream: off (pix68k_wfe=0); waiting cores spin");
        return;
    }
    const unsigned period = event_wait::enableStream();
    if (period == 0) {
        m_Logger.Write(FromKernel, LogWarning, "event stream: CNTFRQ is 0; waiting cores spin");
        return;
    }
    const unsigned window = 2000, expected = window / period;
    const unsigned wakes = expected != 0 ? event_wait::countWakes(window) : 0;
    if (!platform::streamProbeOk(wakes, window, period)) {
        event_wait::disableStream();
        m_Logger.Write(FromKernel, LogWarning,
                       "event stream: every %u us, %u wakes in %u us (expected about %u); waiting cores spin", period,
                       wakes, window, expected);
        return;
    }
    m_EventStreamUs = period;
    // Circle の 100Hz の定期割り込み (コア 0) で sev を送る。イベントストリームとは別に、どのコアの wfe も
    // 10ms 以内に必ず戻るようにする (コア 1 の起動時の確認 Emulator::run() が、届かなくても終わるように)。
    // コア 2 / 3 の空起きは 1 秒に 100 回増えるだけ
    m_Timer.RegisterPeriodicHandler(SevFromTimer);
    m_Logger.Write(FromKernel, LogNotice, "event stream: every %u us, %u wakes in %u us; waiting cores sleep (wfe)",
                   period, wakes, window);
}

// コア 0 が時刻 deadline まで待つ。イベントストリームが届けば wfe で眠る (割り込みとほかのコアの sev でも起きる)
void CKernel::IdleUntil(u64 deadline) {
    if (m_EventStreamUs != 0)
        event_wait::sleepUntil(deadline);
    else
        while (CTimer::GetClockTicks64() < deadline) {
        }
}

// SoC の温度 (1/1000 度)。読めなければ (QEMU など) 0
unsigned CKernel::ReadMilliC() {
    CBcmPropertyTags tags;
    TPropertyTagTemperature t;
    t.nTemperatureId = TEMPERATURE_ID;
    if (!tags.GetTag(PROPTAG_GET_TEMPERATURE, &t, sizeof t, 4)) return 0;
    return t.nValue;
}

void CKernel::ControlFan() {
#if RASPPI >= 5
    if (m_Fan.drive() == CPi5Fan::Drive::None) return;
    const u64 now = CTimer::GetClockTicks64();
    if (m_FanTicks != 0 && now - m_FanTicks < kFanSeconds * (u64)CLOCKHZ) return;
    m_FanTicks = now;
    // メールボックスを 1 回読む。ログは出さない (UART に 1 行出すと数 ms かかり、コア 0 が画面を写すのが遅れる)。
    // 段階は、10 秒ごとの cpu: の行 (ReportCpu()) に出る
    m_Fan.SetTemperature(ReadMilliC());
#endif
}

// メモリとヒープの大きさ。Circle のヒープ (CHeapAllocator) は、まだ切り出していない残りを先頭から切り出し、解放した
// ブロックは大きさごとの空きリストに戻して次の確保に使い回す。GetHeapFreeSpace() が返すのはまだ切り出していない残り
// だけで、解放したブロックは数えない。そこで「空き」ではなく「一度も使っていない部分」として出す
// (使った量は、起動してからいちばん多く使ったときの量に近い。解放して使い回せる分を含む)。
// 1GB より下のヒープは、Circle の CMemorySystem と同じく、ARM のメモリから MEM_HEAP_START と PAGE_RESERVE を引いた大きさ。
// 1GB より上のヒープは、メモリ全体から ARM のメモリを引いた大きさ (Circle の SetupHighMem())
CKernel::HeapStatus CKernel::ReadHeap() const {
    HeapStatus h;
    CMemorySystem* mem = CMemorySystem::Get();
    h.memBytes = mem->GetMemSize();
    if (m_ArmMemBytes > MEM_HEAP_START + PAGE_RESERVE) h.lowBytes = m_ArmMemBytes - MEM_HEAP_START - PAGE_RESERVE;
    h.lowUnused = mem->GetHeapFreeSpace(HEAP_LOW);
#if RASPPI >= 4
    if (m_ArmMemBytes != 0 && h.memBytes > m_ArmMemBytes) h.highBytes = h.memBytes - m_ArmMemBytes;
    h.highUnused = mem->GetHeapFreeSpace(HEAP_HIGH);
#endif
    return h;
}

static unsigned Mb(size_t bytes) { return (unsigned)((bytes + 512 * 1024) / (1024 * 1024)); }

// ヒープの使った量を「heap low 使った/大きさ MB」の形で out に足す (大きさが分からなければ一度も使っていない部分を出す)
void CKernel::FormatHeap(CString& out, const HeapStatus& h) {
    CString s;
    if (h.lowBytes != 0)
        s.Format("mem %u MB, heap low %u/%u MB used", Mb(h.memBytes), Mb(h.lowBytes - h.lowUnused), Mb(h.lowBytes));
    else
        s.Format("mem %u MB, heap low %u MB never used", Mb(h.memBytes), Mb(h.lowUnused));
    out.Append(s);
    if (h.highBytes != 0) {
        s.Format(", high %u/%u MB used", Mb(h.highBytes - h.highUnused), Mb(h.highBytes));
        out.Append(s);
    }
}

void CKernel::ReportHeap(const char* from, const char* when) {
    CString line;
    FormatHeap(line, ReadHeap());
    // フレームバッファはファームウェアが GPU のメモリに置くので、ヒープには入らない
    CBcmFrameBuffer* fb = m_Screen.GetFrameBuffer();
    CString fbs;
    if (fb != nullptr) fbs.Format("; framebuffer %u KB (not in the heap)", fb->GetSize() / 1024);
    m_Logger.Write(from, LogNotice, "mem: %s: %s%s", when, (const char*)line, (const char*)fbs);
    // フレームバッファの番地と、ARM のメモリ (キャッシュを通る) と 1GB より下のヒープの範囲。フレームバッファが
    // ARM のメモリの中やヒープと重なっていないかを見るため (Pi 5 の HDMI だけで絵が崩れる件の調べ)。
    // ヒープの範囲は Circle の CMemorySystem と同じ計算 (MEM_HEAP_START から、ARM のメモリの終わりの PAGE_RESERVE の手前まで)
    if (fb != nullptr) {
        const u64 fbStart = fb->GetBuffer(), fbEnd = fbStart + fb->GetSize();
        const u64 arm = m_ArmMemBytes;
        const u64 heapStart = MEM_HEAP_START, heapEnd = arm > MEM_HEAP_START + PAGE_RESERVE ? arm - PAGE_RESERVE : 0;
        const bool inArm = arm != 0 && fbStart < arm;
        const bool inHeap = heapEnd != 0 && fbStart < heapEnd && heapStart < fbEnd;
        m_Logger.Write(from, LogNotice,
                       "mem: framebuffer 0x%lX-0x%lX (%u KB, pitch %u), ARM memory 0x0-0x%lX (%u MB; total %u MB), "
                       "heap low 0x%lX-0x%lX; framebuffer %s the ARM memory%s",
                       (unsigned long)fbStart, (unsigned long)fbEnd, fb->GetSize() / 1024, fb->GetPitch(),
                       (unsigned long)arm, Mb(arm), Mb(CMemorySystem::Get()->GetMemSize()), (unsigned long)heapStart,
                       (unsigned long)heapEnd, arm == 0 ? "maybe in" : inArm ? "INSIDE" : "outside",
                       inHeap ? " and OVERLAPS the heap" : "");
    }
}

void CKernel::ReportCpu(const char* from) {
    if (m_pThrottle == nullptr) return;
    // GetClockRate() は要求した値を返すので、ファームウェアが温度などでクロックを下げても変わらない。
    // 実際のクロック (PROPTAG_GET_CLOCK_RATE_MEASURED) と、ファームウェアが下げているかどうか
    // (PROPTAG_GET_THROTTLED。bit0 電圧不足、bit1 クロックの上限を下げている、bit2 クロックを下げている、
    // bit3 温度の上限に達している。bit16-19 は同じことが起きたか) も出す。読めなければ (QEMU など) 「?」。
    // 0xFFFF を渡すと読んだあとに bit16-19 が消えるので、bit16-19 は「前の報告から起きたか」になる
    CBcmPropertyTags tags;
    TPropertyTagClockRate rate;
    rate.nClockId = CLOCK_ID_ARM;
    CString measured("?");
    m_CpuStatus.measuredMHz = 0;
    if (tags.GetTag(PROPTAG_GET_CLOCK_RATE_MEASURED, &rate, sizeof rate, 4) && rate.nRate != 0) {
        m_CpuStatus.measuredMHz = (rate.nRate + 500000) / 1000000;
        measured.Format("%u", m_CpuStatus.measuredMHz);
    }
    TPropertyTagSimple thr;
    thr.nValue = 0xFFFF; // CCPUThrottle::CheckThrottledState() と同じ値で読む (bit16-19 を消す)
    CString throttled("?");
    const bool thrOk = tags.GetTag(PROPTAG_GET_THROTTLED, &thr, sizeof thr, 4);
    if (thrOk) {
        throttled.Format("%X", thr.nValue);
        m_CpuStatus.throttled = thr.nValue;
        m_CpuStatus.throttledEver |= thr.nValue;
    }
    m_CpuStatus.throttledOk = thrOk;
    m_CpuStatus.setMHz = m_pThrottle->GetClockRate() / 1000000;
    m_CpuStatus.ticks = CTimer::GetClockTicks64();
    const unsigned temperature = m_pThrottle->GetTemperature();
    // Pi 5 はファンの段階 (0 から 4)、速さ (255 が全開)、RP1 が測った回転数も出す
    CString fan;
#if RASPPI >= 5
    if (m_Fan.drive() != CPi5Fan::Drive::None)
        fan.Format(", fan level %d (%u/255), %u rpm", m_Fan.level(), m_Fan.duty(), m_Fan.rpm());
#endif
    // メモリとヒープの使った量 (Pi 5 の 2GB のモデルで足りるかを見るため)
    CString heap(", ");
    FormatHeap(heap, ReadHeap());
    m_Logger.Write(from, LogNotice, "cpu: %u MHz set, %s MHz measured, throttled %s, %u C%s%s", m_CpuStatus.setMHz,
                   (const char*)measured, (const char*)throttled, temperature, (const char*)fan, (const char*)heap);
    // 電圧不足か、温度でクロックを下げた (下げている) ときは警告にする (ログを grep で探しやすいように)
    if (thrOk && (thr.nValue & 0xF000Fu) != 0)
        m_Logger.Write(from, LogWarning, "cpu: throttled %X (%s%s%s%s), %s MHz measured, %u C", thr.nValue,
                       (thr.nValue & 0x10001u) ? "under-voltage " : "", (thr.nValue & 0x20002u) ? "clock capped " : "",
                       (thr.nValue & 0x40004u) ? "clock lowered " : "", (thr.nValue & 0x80008u) ? "temperature limit" : "",
                       (const char*)measured, temperature);
}

// ネットワークの口の GET /thermal。名前=値 を 1 行ずつ返す。温度はここで読む (メールボックスを 1 回)。
// クロックと throttled は ReportCpu() が最後に読んだ値 (throttled を読むと「起きたか」のビットが消えるので、ここでは読まない)
void CKernel::netThermal(CString& out) {
    CString line;
    const unsigned milliC = ReadMilliC();
    if (milliC != 0) {
        line.Format("temp_c=%u.%u\n", milliC / 1000, milliC % 1000 / 100);
        out.Append(line);
    } else {
        out.Append("temp_c=?\n");
    }
    if (m_CpuStatus.ticks != 0) {
        line.Format("clock_set_mhz=%u\n", m_CpuStatus.setMHz);
        out.Append(line);
        if (m_CpuStatus.measuredMHz != 0) {
            line.Format("clock_measured_mhz=%u\n", m_CpuStatus.measuredMHz);
            out.Append(line);
        }
        if (m_CpuStatus.throttledOk) {
            line.Format("throttled=%X\n", m_CpuStatus.throttled);
            out.Append(line);
        }
        line.Format("throttled_since_boot=%X\nreport_age_s=%u\n", m_CpuStatus.throttledEver,
                    (unsigned)((CTimer::GetClockTicks64() - m_CpuStatus.ticks) / CLOCKHZ));
        out.Append(line);
    }
    // メモリとヒープ (KB)。*_unused は一度も使っていない部分で、解放したブロックは数えない (ReadHeap())
    const HeapStatus h = ReadHeap();
    line.Format("mem_total_kb=%u\nheap_low_unused_kb=%u\n", (unsigned)(h.memBytes / 1024), (unsigned)(h.lowUnused / 1024));
    out.Append(line);
    if (h.lowBytes != 0) {
        line.Format("heap_low_kb=%u\n", (unsigned)(h.lowBytes / 1024));
        out.Append(line);
    }
    if (h.highBytes != 0) {
        line.Format("heap_high_kb=%u\nheap_high_unused_kb=%u\n", (unsigned)(h.highBytes / 1024),
                    (unsigned)(h.highUnused / 1024));
        out.Append(line);
    }
#if RASPPI >= 5
    const CPi5Fan::Drive drive = m_Fan.drive();
    line.Format("fan=%s\n", drive == CPi5Fan::Drive::Pwm ? "pwm" : drive == CPi5Fan::Drive::OnOff ? "onoff" : "none");
    out.Append(line);
    if (drive != CPi5Fan::Drive::None) {
        line.Format("fan_level=%d\nfan_duty=%u\nfan_rpm=%u\n", m_Fan.level(), m_Fan.duty(), m_Fan.rpm());
        out.Append(line);
    }
#endif
}

TShutdownMode CKernel::Run() {
    m_Logger.Write(FromKernel, LogNotice, "pix68k bare metal on %s, compiled " __DATE__ " " __TIME__,
                   CMachineInfo::Get()->GetMachineName());
    m_Logger.Write(FromKernel, LogNotice, "screen %ux%u, pixel order %s", m_Screen.GetWidth(), m_Screen.GetHeight(),
                   m_PixelOrderResult < 0 ? "not set" : (m_PixelOrderResult == 0 ? "BGR" : "RGB"));
    LogOverscan();
    LogGpuScale();

    LogBootTime(BootKernel, "kernel started");
    LogBootTime(BootDevices, "screen, serial and timer ready");
    LogBootTime(BootMounted, "boot drive mounted");
    StartEventStream();
    FinishBootDrive();
    ListBootDrive();
    if (StartMachine()) {
        const TShutdownMode mode = RunMachine();
        // midi_out=uart なら、実機の音源が鳴り続けないよう、鳴っている音を止めてから再起動する (300ms まで待つ)
        SilenceMidiUart(300);
        return mode;
    }
    return RunM0();
}

// ROM がないとき (M0 の動作): コア 1 で速度計測をし、USB の入力をログに出し、テストパターンを描く
TShutdownMode CKernel::RunM0() {
    // 2 番目以降のコアを起動する。コア 1 が速度計測を走らせている間も、コア 0 は USB を処理する
    s_benchMemory = new bench::Memory;
    if (s_benchMemory == nullptr) m_Logger.Write(FromKernel, LogError, "benchmark: out of memory");
    if (!m_Cores.Initialize()) m_Logger.Write(FromKernel, LogError, "cannot start secondary cores");
    m_Logger.Write(FromKernel, LogNotice, "USB keyboard / mouse / gamepad input is logged");
    ScanUsb(false);
    // ROM がなくても、ネットワークの口は使える (ROM やカーネルを送れるように)
    StartHostNet();

    bool reported = false;
    unsigned lastMask = 0;
    const unsigned deployTicks = m_DeploySeconds * 100; // 10ms 単位
    for (unsigned count = 0;; count++) {
        UpdateUsbDevices();
        // Circle のスピンロックは wfe で待ち、解放するときに sev を出さない。QEMU では待っているコアが
        // 起きないことがあるので (実機は排他モニタの解除で起きる)、ここで定期的に起こす
        asm volatile("sev");
        unsigned mask = __atomic_load_n(&m_Cores.startedMask, __ATOMIC_RELAXED);
        if (mask != lastMask) {
            m_Logger.Write(FromKernel, LogNotice, "secondary cores started: mask=%X", mask);
            lastMask = mask;
        }
        if (!reported && __atomic_load_n(&m_Cores.benchDone, __ATOMIC_ACQUIRE)) {
            ReportBenchmark();
            reported = true;
        }
        // ログが流れるとカラーバーが消えるので、1 秒ごとに描き直す
        if (count % 100 == 0) DrawTestPattern();
        m_Screen.Rotor(0, count);
        // Ctrl + Alt + Delete で再起動する (X68000 を動かしていないので、書き戻すものはない)
        // POST /reboot?profile= の書き込みに失敗して once が残っているかもしれず、取り消せなければ再起動しない
        if (m_RebootRequest.take() && CancelProfileChoiceForRestart("Ctrl+Alt+Delete")) {
            m_Logger.Write(FromKernel, LogNotice, "restarting (Ctrl+Alt+Delete)");
            if (m_DeploySeconds != 0) FinishDeploy(false);
            return ShutdownReboot;
        }
        if (m_DeploySeconds != 0) {
            if (count % 100 == 0) KeepAlive();
            if (count >= deployTicks) {
                CancelProfileChoice();
                FinishDeploy();
                return ShutdownReboot;
            }
        }
        if (NetRebootDue()) {
            m_Logger.Write(FromKernel, LogNotice, "restarting (requested over the network)");
            return ShutdownReboot;
        }
        ControlFan();
        // 10ms 待つ。ネットワークの口を動かしていれば、待つ間にそのタスクを動かす (なければ IdleUntil() で眠る)
        ServiceHostNet(10000);
    }
    return ShutdownHalt;
}

void CKernel::UpdateUsbDevices() {
    if (!m_UsbOk) return;
    // FinishBootDrive() が USB メモリを待つ間に処理した分 (m_UsbDevicesPending) も、ここで装置を登録する
    const bool changed = m_USBHCI.UpdatePlugAndPlay();
    if (!changed && !m_UsbDevicesPending) return;
    m_UsbDevicesPending = false;
    // キーボード: ukbd1 から ukbd8 のうち、まだ使っていないものを空いている枠に入れる (4 台まで)。
    // Circle は外したキーボードの番号を次につないだものに使い回すので、番号ではなく装置で見分ける
    for (unsigned n = 1; n <= 8; n++) {
        CString name;
        name.Format("ukbd%u", n);
        CUSBKeyboardDevice* kb = (CUSBKeyboardDevice*)m_DeviceNameService.GetDevice(name, FALSE);
        if (kb == nullptr) continue;
        int slot = -1;
        bool used = false;
        for (int s = 0; s < MaxKeyboards; s++) {
            if (m_pKeyboard[s] == kb) used = true;
            if (slot < 0 && m_pKeyboard[s] == nullptr) slot = s;
        }
        if (used) continue;
        if (slot < 0) {
            static bool warned = false;
            if (!warned)
                m_Logger.Write(FromKernel, LogWarning, "%s: more than %d keyboards; not used", (const char*)name,
                               MaxKeyboards);
            warned = true;
            break;
        }
        m_pKeyboard[slot] = kb;
        SetupKeyLayout(slot, name);
        // ハンドラの引数は枠の番号 + 1 (0 は nullptr と見分けられないので使わない)。外したときのハンドラは、コア 0 の
        // タスク (UpdatePlugAndPlay() の中の装置の削除) からしか呼ばれないので、割り込みと重ならない
        kb->RegisterRemovedHandler(KeyboardRemovedHandler, (void*)(uintptr)(slot + 1));
        // Circle はハンドラの関数と引数を別々に書くので、その間にレポートの割り込みが来ないよう、割り込みを止めて登録する
        EnterCritical();
        kb->RegisterKeyStatusHandlerRaw(KeyStatusHandlerRaw, FALSE, (void*)(uintptr)(slot + 1));
        LeaveCritical();
    }
    if (m_pMouse == nullptr) {
        m_pMouse = (CMouseDevice*)m_DeviceNameService.GetDevice("mouse1", FALSE);
        if (m_pMouse != nullptr) {
            m_pMouse->RegisterRemovedHandler(MouseRemovedHandler);
            m_pMouse->RegisterStatusHandler(MouseStatusHandler);
            m_Logger.Write(FromKernel, LogNotice, "mouse attached");
        }
    }
    // ゲームパッド: upad1 から upad4 のうち、まだ使っていないものを空いている枠 (ジョイスティック 1、2) に入れる
    for (unsigned n = 1; n <= 4; n++) {
        CString name;
        name.Format("upad%u", n);
        CUSBGamePadDevice* pad = (CUSBGamePadDevice*)m_DeviceNameService.GetDevice(name, FALSE);
        if (pad == nullptr || pad == m_pGamePad[0] || pad == m_pGamePad[1]) continue;
        int slot = m_pGamePad[0] == nullptr ? 0 : m_pGamePad[1] == nullptr ? 1 : -1;
        if (slot < 0) break;
        m_PadIndex[slot] = n - 1;
        m_PadKnown[slot] = (pad->GetProperties() & GamePadPropertyIsKnown) != 0;
        unsigned vid = 0, pid = 0;
        CUSBDevice* dev = pad->GetDevice();
        const TUSBDeviceDescriptor* desc = dev ? dev->GetDeviceDescriptor() : nullptr;
        if (desc != nullptr) {
            vid = desc->idVendor;
            pid = desc->idProduct;
        }
        // 状態の通知 (USB の割り込み) が読むので、登録する前に決める
        m_PadLayout[slot] = m_PadKnown[slot] ? nullptr : platform::findPadLayout((uint16_t)vid, (uint16_t)pid);
        m_LastPadButtons[slot] = 0;
        m_LastPadHat[slot] = -2;
        m_PadWant[slot] = 0;
        m_PadSent[slot] = -1;
        m_PadHat[slot].reset(HatBase(pad, m_PadKnown[slot]));
        m_PadReset[slot].reset();
        m_PadMenu[slot].reset();
        m_pGamePad[slot] = pad;
        m_PadAttachTicks[slot] = CTimer::GetClockTicks64();
        m_BootPadBits[slot] = 0; // 起動の組み合わせのメニュー (boot_menu.cpp の PadStartFilter)
        m_BootPadReports[slot] = 0;
        pad->RegisterRemovedHandler(GamePadRemovedHandler);
        pad->RegisterStatusHandler(GamePadStatusHandler);
        m_Logger.Write(FromKernel, LogNotice, "gamepad attached (%s, %04x:%04x, joystick %d%s%s)", (const char*)name, vid,
                       pid, slot + 1, m_PadKnown[slot] ? "" : m_PadLayout[slot] ? ", layout " : ", unknown layout",
                       m_PadLayout[slot] ? m_PadLayout[slot]->name : "");
    }
}

// つないだキーボードの VID:PID と HID 記述子の bCountryCode を読み、キーの書き換え方を決めてログに出す
// (pix68k.cfg の keyboard= が auto でなければ、その種類にする)。Pi 400 の本体のキーボードは 04d9:0007。
// キーボードごとに決めるので、Pi 400 に外付けの USB キーボードをつないでも、外付けのほうは普通のキーボードとして扱う
void CKernel::SetupKeyLayout(int slot, const char* name) {
    CUSBKeyboardDevice* kb = m_pKeyboard[slot];
    unsigned vid = 0, pid = 0;
    uint8_t country = 0;
    CUSBDevice* dev = kb->GetDevice();
    const TUSBDeviceDescriptor* desc = dev ? dev->GetDeviceDescriptor() : nullptr;
    if (desc != nullptr) {
        vid = desc->idVendor;
        pid = desc->idProduct;
    }
    const TUSBConfigurationDescriptor* config = dev ? dev->GetConfigurationDescriptor() : nullptr;
    if (config != nullptr)
        platform::hidCountryCode((const uint8_t*)config, config->wTotalLength, kb->GetInterfaceNumber(), country);
    // 自動のときは、Pi 400 の上でだけ Pi 400 のキーボードとみなす (04d9 はほかのキーボードにもある VID のため)
    const bool pi400 = CMachineInfo::Get()->GetMachineModel() == MachineModel400;
    platform::KeyLayout layout =
        m_KeyLayoutAuto ? platform::detectKeyLayout((uint16_t)vid, (uint16_t)pid, country, pi400) : m_KeyLayoutCfg;
    // 書き換え方は USB の割り込みからも読むので、割り込みを止めて変える
    EnterCritical();
    m_KeyRemap[slot].setLayout(layout);
    m_KeyMerge.clear(slot);
    LeaveCritical();
    m_Leds[slot].reset();
    m_Logger.Write(FromKernel, LogNotice, "keyboard attached (%04x:%04x, country %u, layout %s%s, %s, keyboard %d)", vid,
                   pid, (unsigned)country, platform::keyLayoutName(layout), m_KeyLayoutAuto ? ", auto" : "", name,
                   slot + 1);
    if (m_BootTicks[BootKeyboard] == 0) LogBootTime(BootKeyboard, "keyboard attached");
}

// ゲームパッドのハットの論理最小値 (0 か 1。分からなければ -1 で、値から決める)。pix68k.cfg の hat= で
// 決めていればそれ、Circle が配置を知っている機種 (専用のドライバ) は 0-7 で渡すので 0、そのほかは
// HID のレポート記述子を読み直して調べる。Circle の CUSBGamePadStandardDevice は読んだ記述子を private に
// 持っていて外から参照できないので、Circle が列挙のときに成功させたのと同じ要求 (HID 記述子に書かれた
// 正確な長さ) を送る。長さを多めに頼むと、Pi 3 のハブの下のスプリット転送で短いパケットで終われず、
// コア 0 が止まることがある (このバージョンの Circle の DWHCI は制御転送にタイムアウトがない)
int CKernel::HatBase(CUSBGamePadDevice* pad, bool known) {
    if (m_HatBase >= 0) return m_HatBase;
    if (known) return 0;
    CUSBDevice* dev = pad->GetDevice();
    const TUSBConfigurationDescriptor* config = dev ? dev->GetConfigurationDescriptor() : nullptr;
    uint8_t type = 0;
    unsigned length = 0;
    if (config == nullptr ||
        !platform::hidReportDescriptorInfo((const uint8_t*)config, config->wTotalLength, pad->GetInterfaceNumber(),
                                           type, length)) {
        m_Logger.Write(FromKernel, LogNotice, "gamepad hat: no HID descriptor; deciding from values");
        return -1;
    }
    u8* desc = new u8[length];
    if (desc == nullptr) return -1;
    const int got = pad->GetHost()->GetDescriptor(pad->GetEndpoint0(), type, DESCRIPTOR_INDEX_DEFAULT, desc, length,
                                                  REQUEST_IN | REQUEST_TO_INTERFACE, pad->GetInterfaceNumber());
    int lmin = 0, lmax = 0, base = -1;
    if (got == (int)length && platform::hidHatLogicalRange(desc, length, lmin, lmax) && (lmin == 0 || lmin == 1)) {
        base = lmin;
        m_Logger.Write(FromKernel, LogNotice, "gamepad hat: logical %d..%d", lmin, lmax);
    } else {
        m_Logger.Write(FromKernel, LogNotice, "gamepad hat: cannot read the logical range (%d of %u bytes); "
                       "deciding from values", got, length);
    }
    delete[] desc;
    return base;
}

// USB キーボードのレポート (USB の割り込み)。arg はキーボードの枠の番号 + 1 (nullptr か範囲外なら捨てる)
void CKernel::KeyStatusHandlerRaw(unsigned char modifiers, const unsigned char rawKeys[6], void* arg) {
    const int slot = (int)(uintptr)arg - 1;
    if (slot < 0 || slot >= MaxKeyboards) return;
    s_pThis->KeyReport(slot, modifiers, rawKeys);
    if (s_pThis->m_pEmulator != nullptr) return;
    CString msg;
    msg.Format("key: mod=%02X", (unsigned)modifiers);
    for (unsigned i = 0; i < 6; i++) {
        if (rawKeys[i] != 0) {
            CString k;
            k.Format(" %02X", (unsigned)rawKeys[i]);
            msg.Append(k);
        }
    }
    s_pThis->m_Logger.Write(FromKernel, LogNotice, "%s (keyboard %d)", (const char*)msg, slot + 1);
}

// キーボードを外した。そのキーボードで押していたキーだけを離したことにする (ほかのキーボードで押しているキーはそのまま)
void CKernel::KeyboardRemovedHandler(CDevice* device, void* context) {
    CKernel* k = s_pThis;
    const int slot = (int)(uintptr)context - 1; // 枠の番号 + 1
    if (slot < 0 || slot >= MaxKeyboards || k->m_pKeyboard[slot] != (CUSBKeyboardDevice*)device) return;
    k->m_Logger.Write(FromKernel, LogNotice, "keyboard removed (keyboard %d)", slot + 1);
    k->m_Leds[slot].reset(); // 次にこの枠に入れたキーボードに LED を送り直す
    // キューの書き手を 1 つにするため、USB の割り込みを止めて送る
    EnterCritical();
    k->m_pKeyboard[slot] = nullptr;
    k->m_KeyRemap[slot].reset(); // 押していたキーの置き換え先と Num Lock の状態を忘れる
    k->m_KeyMerge.clear(slot);
    k->DispatchKeys();
    LeaveCritical();
}

// USB マウス (USB の割り込み)。X68000 を動かしていれば、移動量とボタンをコア 1 に送る
// (Circle のボタンのビット: bit0 左、bit1 右、bit2 中。InputSink::MOUSE_* と同じ)。
// キューが満杯で送れなかった分は m_Mouse が溜めていて、コア 0 の主ループで送り直す
void CKernel::MouseStatusHandler(unsigned buttons, int dx, int dy, int wheel) {
    if (s_pThis->m_pEmulator != nullptr) {
        if (s_pThis->m_MenuActive) return; // 設定メニューを開いている間は送らない
        Emulator* emu = s_pThis->m_pEmulator;
        s_pThis->m_Mouse.report(dx, dy, (uint8_t)(buttons & 7),
                                [emu](int x, int y, uint8_t b) { return emu->mouseEvent(x, y, b); });
        return;
    }
    s_pThis->m_Logger.Write(FromKernel, LogNotice, "mouse: buttons=%X dx=%d dy=%d wheel=%d", buttons, dx, dy, wheel);
}

void CKernel::MouseRemovedHandler(CDevice*, void*) {
    s_pThis->m_Logger.Write(FromKernel, LogNotice, "mouse removed");
    if (s_pThis->m_pEmulator != nullptr) {
        // 押したままのボタンを離す (キューの書き手を 1 つにするため、USB の割り込みを止めて送る)
        Emulator* emu = s_pThis->m_pEmulator;
        EnterCritical();
        s_pThis->m_Mouse.releaseAll([emu](int x, int y, uint8_t b) { return emu->mouseEvent(x, y, b); });
        LeaveCritical();
    }
    s_pThis->m_pMouse = nullptr;
}

// USB ゲームパッド (USB の割り込み)。X68000 を動かしていればジョイスティックのビットに直してコア 1 に送り、
// 動かしていなければボタンが変わったときだけログに出す (アナログ軸は値が細かく揺れるので出さない)
void CKernel::GamePadStatusHandler(unsigned index, const TGamePadState* state) {
    CKernel* k = s_pThis;
    int slot = -1;
    for (int s = 0; s < 2; s++)
        if (k->m_pGamePad[s] != nullptr && k->m_PadIndex[s] == index) slot = s;
    if (slot < 0) return;
    // pad_log=on: ボタンとハットの生の値が変わったらログに出す (配置の分からないゲームパッドの並びを調べるため)
    const int rawHat = state->nhats > 0 ? state->hats[0] : -1;
    if (k->m_PadLog && (state->buttons != k->m_LastPadButtons[slot] || rawHat != k->m_LastPadHat[slot])) {
        k->m_LastPadButtons[slot] = state->buttons;
        k->m_LastPadHat[slot] = rawHat;
        k->m_Logger.Write(FromKernel, LogNotice, "gamepad %d: buttons=%X hat=%d", slot + 1, state->buttons, rawHat);
    }
    // VID:PID で並びの分かるゲームパッド (platform::findPadLayout()) は、Circle が配置を知っているものと同じビットに並べ直す
    const platform::PadLayout* layout = k->m_PadLayout[slot];
    platform::GamePadInput in;
    in.known = k->m_PadKnown[slot] || layout != nullptr;
    in.buttons = layout != nullptr ? platform::remapPadButtons(*layout, state->buttons) : state->buttons;
    // Start + Select (X68000 のリセット)。X68000 を動かす前と、設定メニューを開いている間は扱わない。その間も状態は見ておき、
    // 押したまま X68000 が動き出したり、メニューを閉じたりしても、両方を離して押し直すまではリセットしない
    const bool reset =
        k->m_PadReset[slot].update(in.known, in.buttons) && k->m_pEmulator != nullptr && !k->m_MenuActive;
    if (reset) __atomic_store_n(&k->m_PadResetRequested, true, __ATOMIC_RELEASE);
    in.nhats = state->nhats;
    in.hat = state->nhats > 0 ? k->m_PadHat[slot].decode(state->hats[0]) : 8;
    in.naxes = state->naxes;
    for (int a = 0; a < 2 && a < state->naxes; a++) {
        in.value[a] = state->axes[a].value;
        in.minimum[a] = state->axes[a].minimum;
        in.maximum[a] = state->axes[a].maximum;
    }
    const u8 bits = platform::joystickBits(in);
    // 設定メニューを開く / 操作する (platform::PadMenuInput)。X68000 を動かす前も、押しているボタンは追っておく
    // (押したまま X68000 が動き出しても、離して押し直すまでは開かない)
    u8 out = 0, events[platform::PadMenuInput::MAX_EVENTS];
    int nEvents = 0;
    const platform::PadMenuInput::Result r =
        k->m_PadMenu[slot].update(in.known, in.buttons, bits, k->m_pEmulator == nullptr || k->m_MenuActive, out, events,
                                  nEvents);
    if (k->m_pEmulator == nullptr) k->m_PadMenu[slot].forgetHeld(); // 設定メニューの繰り返しには使わない
    if (k->m_pEmulator != nullptr) {
        switch (r) {
        case platform::PadMenuInput::OPEN: {
            // 同じレポートで Start + Select も押していたら、リセットだけにする (ホスト版の platform::PadPairInput と同じ)
            if (reset) return;
            // キーボードのメニューのキーと同じく、押しているキーを離したことにしてから、主ループがコア 1 を止めて開く。
            // ジョイスティックは OpenMenu() が離したことにする
            Emulator* emu = k->m_pEmulator;
            k->m_PadWant[slot] = 0;
            k->m_Keys.releaseAll([emu](uint8_t u, bool d) { return emu->keyEvent(u, d); });
            k->m_MenuActive = true;
            return;
        }
        case platform::PadMenuInput::MENU:
            // 設定メニューを開いている間は X68000 に送らない。閉じたときに押していたものは、離すまで送らない
            k->m_PadWant[slot] = 0;
            for (int i = 0; i < nEvents; i++) k->PushMenuKey(events[i]);
            return;
        case platform::PadMenuInput::PASS:
            k->m_PadWant[slot] = out;
            k->SendGamePad(slot);
            return;
        }
        return;
    }
    if (k->m_BootMenuActive) { // 起動の組み合わせのメニュー (boot_menu.cpp)
        k->m_BootPadBits[slot] = bits;
        k->m_BootPadReports[slot] = k->m_BootPadReports[slot] + 1;
        return;
    }
    if (k->m_PadLog || state->buttons == k->m_LastPadButtons[slot]) return;
    k->m_LastPadButtons[slot] = state->buttons;
    k->m_Logger.Write(FromKernel, LogNotice, "gamepad %u: buttons=%X", index, state->buttons);
}

void CKernel::GamePadRemovedHandler(CDevice* device, void*) {
    CKernel* k = s_pThis;
    k->m_Logger.Write(FromKernel, LogNotice, "gamepad removed");
    for (int s = 0; s < 2; s++) {
        if (k->m_pGamePad[s] != (CUSBGamePadDevice*)device) continue;
        k->m_pGamePad[s] = nullptr;
        k->m_PadReset[s].reset();
        k->m_PadMenu[s].reset();
        k->m_BootPadBits[s] = 0; // 起動の組み合わせのメニューで、離したことにする
        if (k->m_pEmulator != nullptr) {
            // ジョイスティックを離したことにする
            EnterCritical();
            k->m_PadWant[s] = 0;
            k->SendGamePad(s);
            LeaveCritical();
        }
    }
}
