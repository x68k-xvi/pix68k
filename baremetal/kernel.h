// ベアメタル版のカーネル。boot パーティションに ROM があれば X68000 をコア 1 で動かし、
// なければ M0 の動作 (画面のテストパターン、USB 入力のログ、CPU コアの速度計測) をする
#pragma once

#include <SDCard/emmc.h>
#include <circle/actled.h>
#include <circle/bcmwatchdog.h>
#include <circle/cputhrottle.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/multicore.h>
#include <circle/screen.h>
#include <circle/string.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbgamepad.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <fatfs/ff.h>

#include "emulator.h"
#include "host_net.h"
#include "net_bridge.h"
#include "pi5_fan.h"
#include "sound_out.h"
#include "platform/boot_drive.h"
#include "platform/bezel.h"
#include "platform/crt_effect.h"
#include "platform/boot_profile.h"
#include "platform/frame_scale.h"
#include "platform/snapshot_wait.h"
#include "platform/input_map.h"
#include "platform/key_merge.h"
#include "platform/key_remap.h"
#include "platform/key_tracker.h"
#include "platform/key_type.h"
#include "platform/menu.h"
#include "platform/osd.h"
#include "platform/osd_message.h"
#include "platform/reboot_keys.h"
#include "platform/settings_file.h"
#include "platform/sample_clock.h"
#include "platform/uart_midi.h"
#include "x68k/sram_file.h"

namespace platform {
class Mt32Synth; // platform/mt32_synth.h (machine_run.cpp が読む)
}
#if RASPPI >= 5
#include <nvme/nvme.h>
#endif

enum TShutdownMode { ShutdownNone, ShutdownHalt, ShutdownReboot };

// ディスクのイメージの書き戻し (machine_run.cpp。platform/disk_sync.h をコア 0 で動かす)
struct CDiskWriter;

// ログを UART と画面の両方に出すための出力先。X68000 の画面を出し始めたら、画面には出さない。
// ネットワークの口の /log のため、リング (ring) にも溜める
class CTeeDevice : public CDevice {
public:
    CTeeDevice(CDevice* a, CDevice* b, CDevice* ring) : m_a(a), m_b(b), m_ring(ring) {}
    int Write(const void* buffer, size_t count) override {
        if (__atomic_load_n(&m_bEnabled, __ATOMIC_RELAXED)) m_b->Write(buffer, count);
        m_ring->Write(buffer, count);
        if (!__atomic_load_n(&m_bFirst, __ATOMIC_ACQUIRE)) return (int)count;
        return m_a->Write(buffer, count);
    }
    void EnableSecond(bool on) { __atomic_store_n(&m_bEnabled, on, __ATOMIC_RELAXED); }
    // 1 番目 (UART) に出すのをやめる (UART を MIDI の出力に使うとき)
    void DisableFirst() { __atomic_store_n(&m_bFirst, false, __ATOMIC_RELEASE); }

private:
    CDevice* m_a;
    CDevice* m_b;
    CDevice* m_ring;
    bool m_bEnabled = true;
    bool m_bFirst = true;
};

// MIDI ボードが送ったバイトを UART から 31.25kbps で出す (pix68k.cfg の midi_out=uart。使う UART は midi_uart=)。
// 溜め方と送る速さの配分は platform/uart_midi.h。ここは UART に 1 バイト書くことと時刻だけ。
// 送るのは、音をコア 3 で作るときはコア 3 (記録を処理するたびと、空いたとき。送るものが残っている間は眠らない。SELF)、
// そうでなければコア 0 の主ループ
// (OTHER。CKernel::ServiceMidiUart())。どちらも FIFO に空きがあるときだけ書くので、Write() は待たない
class CUartMidiOut : public platform::UartMidi {
public:
    CUartMidiOut(CSerialDevice* serial, Family family) : UartMidi(family), m_pSerial(serial) {}

protected:
    void put(u8 b) override { m_pSerial->Write(&b, 1); }
    uint64_t nowUs() override { return CTimer::GetClockTicks64(); } // CLOCKHZ は 1MHz

private:
    CSerialDevice* m_pSerial;
};

// 2 番目以降のコアの処理。コア 1 は X68000 を動かす (ROM がなければ CPU コアの速度計測を走らせる)。
// コア 2 は画面を合成し (render_core=off なら何もしない)、コア 3 は音を合成する (sound_core=off なら何もしない)
class CCoreRunner : public CMultiCoreSupport {
public:
    CCoreRunner() : CMultiCoreSupport(CMemorySystem::Get()) {}
    void Run(unsigned core) override;

    // コア間で受け渡すデータ。読み書きはすべて __atomic_* で行う
    unsigned startedMask = 0; // 起動したコアのビット
    bool benchDone = false;   // 計測結果 (下の 2 つ) を書き終えたら true にする
    unsigned benchCycles = 0;
    unsigned benchMicros = 0;
    Emulator* emulator = nullptr; // コア 1 を起動する前に設定する。nullptr なら速度計測
    // 試験用 (カーネルの pix68k_fail_core=N): N 番以降のコアは起動しなかったものとして扱う。
    // Pi 3 の Circle の Initialize() は起動に失敗したコアで止まり、それより後のコアを起動しないので、それに合わせる
    // (Pi 5 では先にすべてのコアを起動するので、後のコアが動いていることもある。コアごとの受け渡しはどちらでも働く)
    unsigned failFrom = 0;
};

// 設定メニュー (platform/menu.h) とネットワークの口 (host_net.h) から頼まれたことも、このクラスが行う
// (machine_menu.cpp、machine_run.cpp)
class CKernel : public platform::MenuHost, public CHostNetEnv {
public:
    CKernel();
    ~CKernel();

    boolean Initialize();
    TShutdownMode Run();
    // path に size バイトの新しいファイルを書いてよいか (boot ドライブが USB メモリのときの制限。kernel.cpp)
    static bool CanWriteNewFile(const char* path, unsigned size);
    static bool CanRenameFile(const char* path);

private:
    void ReportBenchmark();
    void KeepAlive();
    void SetPixelOrderBgr();
    void MountBootDrive();
    void ProbeBootDrive(int drive);
    void SelectBootDrive();
    void FinishBootDrive();
    bool MountUsbDrive(int k, unsigned ms);
    void MakeBootDir();
    bool UsbFreeBytes(unsigned long long& freeBytes);
    bool UsbPathWritable(const char* path);
    platform::UsbWriteCheck CheckUsbWrite(const char* pathInDrive, platform::UsbWriteKind kind, unsigned long long bytes);
    unsigned ReadDeployId(const char* drive);
    void ListBootDrive();
    void FinishDeploy(bool timeUp = true);
    void DrawTestPattern();
    TShutdownMode RunM0();
    bool StartMachine();
    TShutdownMode RunMachine();
    void ReadConfig();
    void ApplySettings();
    bool SaveConfig(const platform::Settings& s);
    // 起動の組み合わせ (platform/boot_profile.h。machine_run.cpp)
    FRESULT LoadConfigText(const char* name, const char* tmpName, platform::ConfigText& text, CString& path);
    bool LoadSavedConfigText(const char* name, const char* tmpName, platform::ConfigText& text);
    void ChooseProfile();
    void ListProfiles(platform::ProfileList& list);
    bool ProfileFileExists(const char* name);
    int RunBootMenu(const platform::ProfileList& list, int initial);
    void DrawBootMenu(const platform::BootMenu& menu);
    void BootMenuKeys(u8 mods, const u8 down[32]);
    bool WriteProfileChoice(const char* name, bool once);
    bool RequestProfileChoice(const char* name);
    bool CancelProfileChoice();
    bool CancelProfileChoiceForRestart(const char* who);
    void ReapplyInputSettings(int keyboard, int hat);
    void StartAutoType();
    void ServiceAutoType();
    bool EjectDisk(int drive, bool discard = false);
    // ディスクの書き戻し (machine_run.cpp)
    int OpenHardDiskImage(int disk, const char* name, bool cdrom, x68k::HardDiskImage*& image, u32 cdBlockSize = 2048);
    bool ResolveCdImage(const char* name, CString& data, x68k::CdToc*& toc, const char*& why);
    bool DiskReadOnly(int disk, const char* path, const char*& why);
    bool FlushDisks(int only = -1); // X68000 を止めている間に呼ぶ
    bool PauseAndFlushDisks();      // 動いている X68000 を止めてから書き戻す (再起動の前)
    void CloseDiskFiles();
    void SetupFastSeek(FIL* f, const char* label);
    void ReportDisks();
    void ReportSoundLevels(); // 音の大きさと、MT-32 の音源が捨てたメッセージの数 (machine_run.cpp)
    // 起動の時間 (LogBootTime())。段階の番号は時刻を入れる m_BootTicks の添字
    enum BootStage {
        BootKernel,     // カーネルに入った (CKernel のコンストラクタ)
        BootDevices,    // 画面、UART、割り込み、タイマを用意した
        BootMounted,    // SD カード (Pi 5 は NVMe も) をマウントした
        BootConfig,     // pix68k.cfg を読み始める
        BootProfile,    // 起動の組み合わせのメニューを出した
        BootRoms,       // ROM を読み終えた
        BootSetUp,      // X68000 を用意した
        BootDisks,      // ディスクのイメージを読み終えた (大きなイメージは開いただけ)
        BootCore1,      // コア 1 を起動した (X68000 が IPL ROM を動かし始める)
        BootFrame,      // 最初の画面を出した
        BootUsb,        // USB の装置を探し終えた
        BootKeyboard,   // キーボードが見つかった
        BootStages
    };
    void LogBootTime(BootStage stage, const char* what);
    void ScanUsb(bool serviceMachine);
    static void UsbScanDelay(unsigned ms);
    void ServiceDuringUsbScan(bool insideUsbTransfer = false);
    void ShowNextFrame();
    void ShowTakenFrame(const Emulator::Frame* frame);
    void WaitSnapshotFrames();
    static bool WaitSnapshotFrame(void* ctx);
    static void ShowSnapshotFrame(void* ctx, const Emulator::Frame* frame);
    void LogShownScreen();
    void LogOverscan();
    boolean InitScreen();
    void LogGpuScale();
    static void PresentFromComposer(void* ctx, const Emulator::Frame& frame);
    static void WaitComposerPresent(void* ctx);
    // f() (USB の転送) を呼ぶ間、画面を写す役を合成するコアに任せる。コア 0 は、その前に取れるフレームを写してから
    // 任せ、f() から戻ったら役を戻す (platform/frame_handoff.h を参照)。f() の中の CTimer::MsDelay() の待ちでは、
    // ハードディスクのイメージのチャンクを読み、書き戻す (UsbScanDelay())。m_pEmulator が動いているときだけ使う
    template <class F> void WithComposerPresenting(F f) {
        ShowNextFrame();
        m_pEmulator->beginComposerPresent();
        m_ComposerPresenting = true;
        void (*const prevHook)(unsigned) = pix68k_msdelay_hook;
        pix68k_msdelay_hook = UsbScanDelay;
        f();
        pix68k_msdelay_hook = prevHook;
        m_ComposerPresenting = false;
        m_pEmulator->endComposerPresent(WaitComposerPresent, this);
        m_LastPollTicks = CTimer::GetClockTicks64(); // 任せていた間は、takeFrame() を見に行く間隔に数えない
        LogShownScreen();
    }
    // 主ループで、長くかかりうる処理 f() (UART にログを書く、SD カードを読み書きする) を呼ぶ間、画面を写す役を
    // 合成するコアに任せる。UART は 115200bps で、割り込みを使わずに書き終わるまで待つので、10 秒ごとのログ
    // (約 1KB) でコア 0 が 70〜90ms 止まり、その間に合成したフレームが置き換わっていた (Pi 3B+ の実機)。
    // メニューを開いている間 (開くところを含む) は、コア 0 がメニューの下に最後のフレームを描くので任せずに呼ぶ。
    // 任せている間 (入れ子) もそのまま呼ぶ。f() の中で画面を写さないこと
    template <class F> void WhileComposerPresents(F f) {
        if (m_pEmulator == nullptr || m_MenuActive || m_ComposerPresenting) {
            f();
            return;
        }
        ShowNextFrame();
        m_pEmulator->beginComposerPresent();
        m_ComposerPresenting = true;
        f();
        m_ComposerPresenting = false;
        m_pEmulator->endComposerPresent(WaitComposerPresent, this);
        m_LastPollTicks = CTimer::GetClockTicks64();
        LogShownScreen();
    }
    unsigned ServiceDisks(u64 now, bool io = true);
    u8* ReadWholeFile(const char* path, unsigned& size);
    static FRESULT ReadFileChecked(const char* path, u8*& data, unsigned& size);
    static FRESULT WriteFileReplacing(const char* path, const char* tmp, const void* data, unsigned size);
    static FRESULT ReadWithFallback(const char* path, const char* tmp, u8*& data, unsigned& size, bool& usedTmp);
    u8* ReadRom(const char* name, unsigned size);
    bool LoadDisk(int drive, const char* name);
    bool LoadHardDisk(const char* name);
    void LoadScsi();
    void LoadSram();
    bool SaveSram(const u8* data);
    void ShowFrame(const Emulator::Frame& frame, bool stopped = false);
    void DumpText();
    void ReportSpeed();
    void ReportMaxSpeed(); // 最高速モードの CPU の速さをログに出す (2 秒ごと)
    // ARM のクロック、ファームウェアがクロックを下げているか、温度 (Pi 5 はファンも) をログに出す (kernel.cpp)。
    // ReportSpeed() が 10 秒ごとに呼ぶ。from はログの行の先頭に付く名前
    void ReportCpu(const char* from);
    // Pi 5: 2 秒ごとに温度を読んで、ファンの速さを決める。主ループから毎回呼ぶ。ほかの機種では何もしない
    void ControlFan();
    static unsigned ReadMilliC(); // SoC の温度 (1/1000 度)。メールボックスを 1 回読む。読めなければ (QEMU など) 0
    // メモリとヒープの大きさ (kernel.cpp。ReadHeap() を参照)
    struct HeapStatus {
        size_t memBytes = 0;   // メモリ全体 (CMemorySystem::GetMemSize()。1GB より上を含む)
        size_t lowBytes = 0;   // 1GB より下のヒープ (HEAP_LOW。new と malloc が使う) の大きさ。0 なら分からない
        size_t lowUnused = 0;  // その中で一度も使っていない部分 (GetHeapFreeSpace(HEAP_LOW))
        size_t highBytes = 0;  // 1GB より上のヒープ (HEAP_HIGH。Pi 4 / Pi 5 でメモリが 1GB より多いときだけ)
        size_t highUnused = 0; // その中で一度も使っていない部分 (GetHeapFreeSpace(HEAP_HIGH))
    };
    HeapStatus ReadHeap() const;
    static void FormatHeap(CString& out, const HeapStatus& h); // 「mem 8192 MB, heap low 150/958 MB used, ...」を out に足す
    void ReportHeap(const char* from, const char* when); // 起動のときに 1 回、ヒープの使った量をログに出す
    void StartSound();
    CSoundBaseDevice* NewI2sSound(); // pix68k_sound=i2s の音のデバイス (StartSound() から。作れなければ nullptr)
    void StartMidi(); // MIDI ボードをつなぎ、音源 (SoundFont、MT-32、SC-55 か UART) を用意する (midi=on のとき)
    void StartMidiMt32(); // midi_out=mt32: ROM の場所と大きさを確かめる
    void StartMidiUart();   // midi_out=uart: UART を 31.25kbps にして音源にする
    void ServiceMidiUart(); // 主ループから: midi_out=uart のバイトを送るコアを決め、コア 0 が送るなら送る
    void SilenceMidiUart(unsigned waitMs); // midi_out=uart: 鳴っている音を止め、送り終えるまで waitMs まで待つ
    void LoadMidiSoundFont(); // StartMidi() が決めた SoundFont を、X68000 を動かし始めてから読む
    void LoadMidiMt32();      // StartMidiMt32() が決めた ROM を、X68000 を動かし始めてから読む
    void StartMidiSc55();     // midi_out=sc55: ROM の場所と大きさを確かめる
    void LoadMidiSc55();      // StartMidiSc55() が決めた ROM を、X68000 を動かし始めてから読み、SC-55mkII を起動する
    friend class CMidiFileSource; // SoundFont や ROM を読む合間に ServiceDuringUsbScan() と IdleUntil() を呼ぶ
    void DrainNullAudio();
    void KeyReport(int slot, unsigned char modifiers, const unsigned char rawKeys[6]);
    void DispatchKeys();
    void SendKeyReport(u8 mods, const u8 down[32]);
    bool RebootFromKeyboard() { return RebootWritingBack("Ctrl+Alt+Delete", m_RebootRequest); }
    bool RebootWritingBack(const char* who, platform::RebootRequest& request);
    void ResendKeys();
    void ResendInput();
    bool InputPending() const;
    // 設定メニュー (machine_menu.cpp)
    void ServiceMenu();
    void OpenMenu();
    void CloseMenu();
    void DrawMenu();
    bool PushMenuKey(u8 usage);
    int PopMenuKey();
    void listFiles(platform::MenuFileKind kind, platform::MenuFileList& list) override;
    bool insertDisk(int drive, const char* name, platform::MenuText& msg) override;
    const char* diskName(int drive) override { return m_Settings.fd[drive]; }
    int changeCd(const char* name, platform::MenuText& msg) override;
    void resetMachine() override;
    bool saveState(int slot, platform::MenuText& msg) override;
    bool loadState(int slot, platform::MenuText& msg) override;
    bool saveSettings(const platform::Settings& s, platform::MenuText& msg) override;
    bool restart(const platform::Settings& s, platform::MenuText& msg) override;
    void beginAction(int action) override { m_Discard.begin(action); }
    // ネットワークの口 (machine_run.cpp)
    void StartHostNet();
    void ServiceHostNet(unsigned waitUs);
    bool NetRebootDue();
    const char* netDrive() override { return m_BootDrive; }
    platform::ImageCheck netImageInUse(const char* const* paths, int count) override;
    bool netBusy() override { return m_MenuActive; }
    bool netCanWrite(const char* path, long long bytes, const char*& why) override;
    void netWriteDone(unsigned long long bytes) override;
    unsigned long long netClusterBytes() override;
    void netBeginHeavy() override;
    void netEndHeavy() override;
    void netIdleUntil(u64 deadline) override { IdleUntil(deadline); }
    bool netSnapshotScreen(u8* rgb, unsigned capacity, int& width, int& height) override;
    bool netSnapshotTs6bga(u8* rgb, unsigned capacity, int& width, int& height, bool output) override;
    void ShowTsFrame();
    void netRequestReboot() override;
    platform::ProfileRequest netBootProfile(const char* name) override;
    bool netKeyReport(u8 modifiers, const u8 keys[6]) override;
    unsigned netLog(char* out) override { return m_LogRing.Snapshot(out); }
    void netThermal(CString& out) override;
    unsigned netLogRead(u64& pos, char* out, unsigned max, u64& lost) override {
        return m_LogRing.ReadFrom(pos, out, max, lost);
    }
    NetSerialLink* netSerialLink() override { return m_pSerialLink; }
    bool netMessage(const char* text, unsigned n, unsigned sec) override;
    // Pi 3 では SC-55 の音源を使わない (StartMidiSc55())
    bool hasMidiOut(int out) const override {
#if RASPPI <= 3
        return out != 4;
#else
        (void)out;
        return true;
#endif
    }
    // メニューを開いている間は合成するコアに写す役を任せないので、コア 0 が書き換えてよい。次に写すときから反映する
    // 画面の出し方 (video_scale=、video_filter=)。変わったら ShowFrame() がすべての行を写し直す。
    // この機種で選べない組み合わせは使わない (メニューは選ばないが、念のため)
    bool hasVideo() const override { return true; }
    bool hasBezel() const override { return true; }
    bool videoAllowed(int scale, int filter) const override { return VideoAllowed(scale, filter); }
    // 枠 (video_filter= の cz-600de / cz-600db) は、絵を読めたときだけ選べる (初めて聞かれたときに起動したドライブから読む。
    // LoadBezel())。メニューが聞くのは、Bezel で枠を替えるときだけ (platform::MenuHost::bezelAvailable())
    bool bezelAvailable(int filter) const override { return const_cast<CKernel*>(this)->LoadBezel(filter) != nullptr; }
    void videoChanged(int scale, int filter) override {
        if (!VideoAllowed(scale, filter)) return;
        m_Settings.videoScale = scale;
        m_Settings.videoFilter = filter;
        __atomic_store_n(&m_Video, scale << 16 | filter, __ATOMIC_RELAXED);
    }
    // 画面の隅の数字も同じ。数字は数え直す (前に出していた数字は出さない)
    void osdChanged(int osd) override {
        m_Settings.osd = osd;
        PublishOsd(platform::OsdValue());
        m_OsdTicks = 0;
    }
    void UpdateOsd();
    static int OsdPiModel();
    void ExpireOsd();
    void ServiceMessage();
    void SendGamePad(int slot);
    void ResetFromGamePad();
    void ClosedPadMenu();
    void UpdateKeyboardLeds();
    void ReportMouse();
    void UpdateUsbDevices();
    void SetupKeyLayout(int slot, const char* name);
    int HatBase(CUSBGamePadDevice* pad, bool known);
    void ReportFrameTiming();
    void ReportCoreWork();
    static void KeyStatusHandlerRaw(unsigned char modifiers, const unsigned char rawKeys[6], void* arg);
    static void KeyboardRemovedHandler(CDevice* device, void* context);
    static void MouseStatusHandler(unsigned buttons, int dx, int dy, int wheel);
    static void MouseRemovedHandler(CDevice* device, void* context);
    static void GamePadStatusHandler(unsigned index, const TGamePadState* state);
    static void GamePadRemovedHandler(CDevice* device, void* context);

    // 初期化の順番どおりに並べる (Circle の決まり)
    CLogRing m_LogRing; // m_Tee より前 (ログを溜める先)
    CActLED m_ActLED;
    CKernelOptions m_Options;
    CDeviceNameService m_DeviceNameService;
    CScreenDevice m_Screen;
    CBcmFrameBuffer* m_TsDisplay = nullptr;
    const x68k::Ts6bga* m_TsFrame = nullptr; // 次に takeTs6bga() するまで読み手が所有
    uint32_t m_TsMainShown = ~0u;
    bool m_TsChanged = false;
    CSerialDevice m_Serial;
    CTeeDevice m_Tee;
    CExceptionHandler m_ExceptionHandler;
    CInterruptSystem m_Interrupt;
    CTimer m_Timer;
    CLogger m_Logger;
    CUSBHCIDevice m_USBHCI;
    CEMMCDevice m_EMMC;
#if RASPPI >= 5
    CNVMeDevice m_NVMe;
#endif
    CBcmWatchdog m_Watchdog;
    FATFS m_SdFs;
#if RASPPI >= 5
    FATFS m_NvmeFs;
#endif
    FATFS m_UsbFs[platform::BOOT_USB_DRIVES]; // umsd1〜umsd3 ("USB:"、"USB2:"、"USB3:")
    // 読み書きするドライブ ("NVME:"、"SD:"、"USB:"、"USB2:"、"USB3:" のどれか。platform/boot_drive.h)。なければ nullptr
    const char* m_BootDrive = nullptr;
    bool m_BootOnUsb = false;         // m_BootDrive が USB メモリ
    FATFS* m_BootUsbFs = nullptr;     // m_BootDrive が USB メモリのとき、その FATFS (m_UsbFs のどれか)
    platform::UsbWriteBudget m_UsbBudget; // ネットワークの口から受け取っている途中のファイルの残り (USB メモリのとき)
    platform::BootDriveState m_Drives[platform::BOOT_DRIVES];
    platform::UsbSlotWatch m_UsbWatch[platform::BOOT_USB_DRIVES]; // FinishBootDrive() の umsdN の見張り (削除の通知が書く)
    platform::BootDriveOption m_DriveOption = platform::BootDriveOption::AUTO; // cmdline.txt の pix68k_drive
    unsigned m_WantId = 0;             // cmdline.txt の pix68k_id (deploy のとき。なければ 0)
    bool m_UsbDevicesPending = false;  // FinishBootDrive() が処理した USB の装置を、UpdateUsbDevices() がまだ登録していない
    int m_PixelOrderResult = -1;       // 画素の並びの設定の結果 (0 BGR、1 RGB、-1 失敗)
    // pix68k_gpu_scale (InitScreen())。GPU がフレームバッファを表示に拡大する横と縦の倍率 (使わなければ 1)
    int m_GpuScaleX = 1, m_GpuScaleY = 1;
    unsigned m_GpuDisplayW = 0, m_GpuDisplayH = 0; // 表示の大きさ
    enum GpuScaleNote { GpuScaleOff, GpuScaleOn, GpuScaleBadOption, GpuScaleNoDisplay, GpuScaleDoesNotFit, GpuScaleFailed };
    GpuScaleNote m_GpuScaleNote = GpuScaleOff;
    unsigned m_DeploySeconds = 0;      // tools/deploy_pi.sh から起動されたときの動作秒数 (0 なら普通の起動)
    CCoreRunner m_Cores;

    // キーボードは 4 台まで (ukbd1 から ukbd8 のうち、つないだ順に空いている枠に入れる)。すべての入力を合わせて X68000 に送る
    static constexpr int MaxKeyboards = 4;
    CUSBKeyboardDevice* volatile m_pKeyboard[MaxKeyboards] = {};
    CMouseDevice* volatile m_pMouse = nullptr;
    // ゲームパッドは 2 台まで (つないだ順にジョイスティック 1、2)
    CUSBGamePadDevice* volatile m_pGamePad[2] = {nullptr, nullptr};
    unsigned m_PadIndex[2] = {0, 0};     // 状態の通知の nDeviceIndex (upadN の N - 1)
    bool m_PadKnown[2] = {false, false}; // Circle が配置を知っているゲームパッドか
    const platform::PadLayout* m_PadLayout[2] = {nullptr, nullptr}; // VID:PID で並びの分かるゲームパッド (platform::findPadLayout())
    int m_LastPadHat[2] = {-2, -2};      // pad_log=on: 前にログに出したハットの生の値
    bool m_PadLog = false;               // pix68k.cfg の pad_log=
    unsigned m_LastPadButtons[2] = {0, 0};
    u8 m_PadWant[2] = {0, 0};            // ゲームパッドのジョイスティックのビット
    int m_PadSent[2] = {0, 0};           // コア 1 に送ったビット (-1 はまだ送っていない)
    platform::HatDecoder m_PadHat[2];    // ハットの値の直し方 (論理範囲が 0-7 か 1-8 か)
    platform::PadResetCombo m_PadReset[2]; // Start + Select (X68000 のリセット) を見つける
    platform::PadMenuInput m_PadMenu[2]; // Home か Select + 上で設定メニューを開き、メニューのキーに置き換える
    bool m_PadResetRequested = false;    // Start + Select を押した (USB の割り込みが立て、コア 0 の主ループが受け取る)
    bool m_FrameTimingPending = false;   // フレームの時間の区間を頼んで、まだ受け取っていない
    int m_HatBase = -1;                  // pix68k.cfg の hat=: 0 (0-7)、1 (1-8)、-1 (auto)

    // X68000 (ROM がなければ nullptr)
    Emulator* m_pEmulator = nullptr;
    CNetBridge* m_pNetBridge = nullptr;  // Neptune-X のフレームの送受信 (neptune=on のときだけ)
    NetSerialLink* m_pSerialLink = nullptr; // RS-232C の相手 (rs232c=tcp:PORT のときだけ。host_net.cpp の CSerialTask)
    x68k::MidiOut* m_pMidiOut = nullptr; // MIDI ボードの音源 (midi=on のときだけ。SoundFont、MT-32 か UART)
    CUartMidiOut* m_pUartMidi = nullptr; // midi_out=uart のときの m_pMidiOut
    platform::Mt32Synth* m_pMt32 = nullptr; // midi_out=mt32 のときの m_pMidiOut (捨てたメッセージの数をログに出す)
    uint32_t m_Mt32DroppedLogged[2] = {0, 0}; // ログに出したときの droppedMessages() と droppedSysex()
    CSerialDevice* m_pMidiSerial = nullptr; // midi_out=uart で、ログと別の UART を使うとき
    unsigned m_UartMidiDevice = 0;          // midi_out=uart で使う UART (CSerialDevice の番号)
    CString m_MidiSf2Path;               // X68000 を動かし始めてから読む SoundFont (空なら読まない)
    CString m_MidiMt32Path[2];           // X68000 を動かし始めてから読む MT-32 の CONTROL と PCM の ROM (空なら読まない)
    CString m_MidiSc55Path[5];           // X68000 を動かし始めてから読む SC-55mkII の 5 つの ROM (空なら読まない)
    CHostNet* m_pHostNet = nullptr;      // ネットワークの口 (net=on のときだけ)
    // netSnapshotScreen() で、写す役を戻してフレームを待つ時間の上限 (ms)。数えるのはフレームを待つ間だけで、
    // 合成するコアが写し終えるのを待つ間と、受け取ったフレームを写す時間と、待つ間のディスクの読み書きの時間は数えない
    // (platform/snapshot_wait.h)
    static const unsigned kSnapshotWaitMs = 200;
    // ディスクの読み書きが残っていれば、この間 (ms) 待ってもフレームが来ないときに区切って読み書きする。
    // X68000 の 1 フレーム (約 18ms) より長くして、フレームが来ているときは区切らずに受け取れるようにする
    static const unsigned kSnapshotSliceMs = 40;
    platform::SnapshotWait* m_pSnapshotWait = nullptr; // WaitSnapshotFrames() の間だけ、その中の変数を指す
    // フレームが来なかったときの警告は、この間 (秒) に 1 回だけ出し、出さなかった回数を次の警告に添える
    static const unsigned kSnapshotWarnSeconds = 10;
    u64 m_SnapshotWarnTicks = 0;
    unsigned m_SnapshotWarnSuppressed = 0;
    bool m_NetHeavy = false;             // netBeginHeavy() で画面を合成するコアに任せている
    unsigned m_NetHeavyDepth = 0;        // netBeginHeavy() の入れ子の深さ
    u64 m_NetRebootAt = 0;               // ネットワークから頼まれた再起動の時刻 (0 なら頼まれていない)
    platform::RebootRequest m_NetReboot{60 * (uint64_t)CLOCKHZ}; // POST /reboot で書けずに断ったあと 60 秒以内の 2 回目の扱い
    CString m_InUsePath[10];             // X68000 につないだディスクのイメージのファイル (書き換えを断る)
    x68k::Config m_Config;               // pix68k.cfg の clock= で変わる
    bool m_RenderCore = true;            // pix68k.cfg の render_core= (画面の合成をコア 2 で行う)
    bool m_SoundCore = true;             // pix68k.cfg の sound_core= (音の合成をコア 3 で行う)
    bool m_MaxSpeed = false;             // pix68k.cfg の max_speed= (x68k/max_speed.h)
    CCPUThrottle* m_pThrottle = nullptr; // ARM のクロック (最大にする)
    // ReportCpu() が最後に読んだ値 (ネットワークの口の /thermal が返す)。PROPTAG_GET_THROTTLED は読むと
    // 「起きたか」のビットが消えるので、読むのは ReportCpu() だけにして、ほかはここから読む
    struct CpuStatus {
        u64 ticks = 0;            // 読んだ時刻 (0 ならまだ読んでいない)
        unsigned setMHz = 0;      // 要求したクロック
        unsigned measuredMHz = 0; // 実測のクロック (0 なら読めなかった)
        bool throttledOk = false; // throttled を読めた
        u32 throttled = 0;        // PROPTAG_GET_THROTTLED の値
        u32 throttledEver = 0;    // 起動してから読んだ値をすべて OR したもの
    } m_CpuStatus;
    size_t m_ArmMemBytes = 0; // ファームウェアが ARM に渡した 1GB より下のメモリ (PROPTAG_GET_ARM_MEMORY。0 なら読めなかった)
#if RASPPI >= 5
    static constexpr unsigned kFanSeconds = 2;       // ファンの速さを決め直す間隔
    CPi5Fan m_Fan;
    u64 m_FanTicks = 0; // ControlFan() が最後に温度を読んだ時刻 (0 ならまだ)
#endif
    CSoundBaseDevice* m_pSound = nullptr; // 音の出力 (pix68k_sound=off なら nullptr)
    CI2CMaster* m_pI2CMaster = nullptr;   // pix68k_sound=i2s で DAC を I2C で設定するとき (それ以外は作らない。GPIO 2 / 3 を触らない)
    bool m_NullAudio = false;             // pix68k_sound=null (音を取り出して捨てる。試験用)
    platform::SampleClock m_NullAudioClock{CLOCKHZ, pix68k_sound::RATE}; // 取り出す数を実時間に合わせる
    x68k::FloppyImage* m_pDisks[2] = {nullptr, nullptr};
    u8* m_pDiskData[2] = {nullptr, nullptr};     // m_pDisks のイメージのバイト列 (取り出したら解放する)
    x68k::HardDiskImage* m_pHardDisk = nullptr; // SASI ID 0
    x68k::HardDiskImage* m_pCdImage = nullptr;  // CD-ROM の媒体 (メニューで入れ替えるときに使い回す)
    x68k::DiskCache* m_pCdCache = nullptr;      // その読み込みのキャッシュ (全体を読んだイメージなら nullptr)
    // 書き戻し。ディスクの番号は platform::DiskChannel のもの (0-1 FD、2 SASI ID 0、3-9 SCSI ID 0-6)
    CDiskWriter* m_pDiskWriter = nullptr;  // 書き戻し (write_back=on のとき) と、大きなイメージの読み込み
    u64 m_StreamMinBytes = 0;              // これより大きいハードディスクのイメージは、使うチャンクだけを読む
    bool m_DeployHeld = false;             // deploy の再起動を、書き戻せないので待たせている (X68000 は止めたまま)
    platform::MenuDiscardConfirm m_Discard; // 書き戻せずに断ったメニューの操作 (続けてもう一度選んだら書き戻さずに進める)
    FIL* m_pDiskFile[10] = {};             // 書き戻すか、使うチャンクだけを読むハードディスクの開いたままのファイル
    CString m_DiskPath[10];                // 書き戻すディスクのファイル (同じファイルを 2 つのドライブで書かないように見る)
    CString m_DiskTmp[2];                  // フロッピーディスクを置き換えるときの一時ファイル (<名前>.new)
    u8* m_pFdShadow[2] = {nullptr, nullptr}; // フロッピーディスクのファイルの写し (一度確保したら使い回す)
    // pix68k.cfg の設定 (platform/settings.h)。m_Settings はいま動いている設定 (fd0= / fd1= はメニューで入れ替えると
    // 書き換える)、m_FileSettings は pix68k.cfg に書いてある設定 (メニューの「Save settings」はこれと違う項目だけを書く)
    platform::Settings m_Settings, m_FileSettings;
    bool m_FdExplicit[2] = {false, false}; // pix68k.cfg に fd0= / fd1= の行がある (空なら、既定の名前を探さない)
    // 起動の組み合わせ (platform/boot_profile.h)。m_Profile は動いている組み合わせの名前 (空なら pix68k.cfg だけ)。
    // 組み合わせで動いているときは、m_FileSettings は pix68k.cfg と組み合わせのファイルを重ねた値
    char m_Profile[platform::PROFILE_NAME_MAX + 1] = {};
    // profile.sel に書いてある前回の選択と、POST /reboot?profile= で once 付きで書いたか。再起動を断ったときと、
    // そのあと profile= なしの POST /reboot が来たときは、前回の選択に書き戻す
    platform::ProfileChoiceStore m_ProfileChoice;
    u8* m_pBootCgrom = nullptr;            // メニューの字に使うために先に読んだ CGROM (StartMachine() がそのまま使う)
    volatile bool m_BootMenuActive = false; // 起動の組み合わせのメニューを出している (キーとゲームパッドをメニューに渡す)
    u8 m_BootMenuHeld[32] = {};            // メニューの間に押していたキー (USB の割り込みだけが触る)
    volatile u8 m_BootPadBits[2] = {};     // メニューの間のゲームパッドのジョイスティックのビット
    volatile unsigned m_BootPadReports[2] = {}; // メニューの間に来たゲームパッドのレポートの数
    u64 m_PadAttachTicks[2] = {};          // ゲームパッドをつないだ時刻 (CTimer::GetClockTicks64())
    platform::AutoTyper m_AutoTyper;       // autotype= (起動したあとに打つ文字列)
    u64 m_BootTicks[BootStages] = {};  // 起動の各段階の時刻 (BootStage。0 はまだ)
    bool m_UsbStarted = false;         // USB のホストコントローラを初期化した (ScanUsb())
    bool m_UsbOk = false;              // 初期化に成功した (失敗したら UpdatePlugAndPlay() を呼ばない)
    bool m_InScanDelay = false;        // UsbScanDelay() の中 (入れ子にしない)
    unsigned m_EventStreamUs = 0;      // コア 0 のイベントストリームの周期 (us)。0 なら届かないので、待つ間は空回りする
    void IdleUntil(u64 deadline);      // 時刻 deadline まで待つ (event_wait.h。届かなければ空回り)
    void StartEventStream();
    u64 m_ScanLastAlive = 0;           // ServiceDuringUsbScan() がウォッチドッグを延ばした時刻
    bool m_FrameLogged = false;        // 最初の画面を出した時刻をログに出した
    bool m_ScreenModeChanged = false;  // ShowFrame() が画面の大きさの変化を見つけた (コア 0 がログに出す)
    uint32_t m_FramesShownByCore0 = 0; // コア 0 が写したフレームの数 (ログ用)

    // 設定メニュー。USB の割り込みがメニューのキーを見つけたら m_MenuActive を立て、主ループが X68000 を止めて開く。
    // 開いている間に押したキーは、割り込みが m_MenuKeyRing に入れ、主ループがメニューに渡す
    platform::Menu m_Menu{*this};
    platform::MenuKeyFilter m_MenuKeys;   // USB の割り込みだけが触る (閉じるときは割り込みを止めて触る)
    platform::MenuKeyRepeat m_MenuRepeat;
    volatile bool m_MenuActive = false;   // メニューを開いている (開くところ、閉じるところを含む)
    u8 m_MenuKeyRing[32] = {};
    volatile unsigned m_MenuKeyHead = 0, m_MenuKeyTail = 0;
    u8 m_MenuLastKey = 0;                 // 最後にメニューに渡したキー (押し続けたときの繰り返し用)
    volatile u8 m_LastMods = 0;           // 最後のレポートの修飾キー (すべてのキーボードを合わせたもの)
    const Emulator::Frame* m_pLastFrame = nullptr; // 最後に出した X68000 の画面 (メニューの下に描き直す)
    bool m_RebootRequested = false;       // メニューの「Restart」
    unsigned m_TextDumpSeconds = 0; // pix68k_textdump (0 なら出さない)
    char (*m_pLastDump)[x68k::TEXT_COLS + 1] = nullptr;
    int m_ShownWidth = -1, m_ShownHeight = -1;
    // ShowFrame() が使う (コア 0 か、写す役を任された合成するコアのどちらか一方だけが触る。platform/frame_handoff.h)
    platform::FrameScale m_ShownScale;      // 最後に写したときの倍率と位置 (変われば枠の外を塗り直す)
    // 最後に写したときの写し方 (表で写したら kCrtShownMap | video_filter、スキャンラインだけなら CRT_SCANLINE、
    // ほかは 0。ShowFrame() が書き、コア 0 がログに使う)
    int m_CrtShown = 0;
    static constexpr int kCrtShownMap = 0x10;
    alignas(16) u32 m_ScaleLine[4096];      // 拡大した 1 行 (platform/frame_scale.h の presentScaled())
    alignas(16) u32 m_ScaleLine2[4096];     // もう 1 行 (platform/crt_effect.h の presentCrt() がにじませるときに使う)
    // 画面の出し方 (video_scale=、video_filter=。platform/crt_effect.h)
    int m_Video = platform::VIDEO_SCALE_INTEGER << 16; // video_scale << 16 | video_filter (コア 0 が書き、写す役のコアが読む)
    bool m_VideoAll = false;                // pix68k_crt_all=1 (Pi 3 でもどの組み合わせも選べる。VideoAllowed())
    platform::CrtMap m_CrtMap;              // 表 (写す役のコアだけが触る。画面の大きさか組み合わせが変わると作り直す)
    int m_CrtLast = 0;                      // 前に写したときの写し方 (m_CrtShown と同じ形。変わったらすべての行を写す。写す役のコアだけが触る)
    uint32_t m_CrtSerialShown = 0;          // 最後に効果を掛けて写した表 (CrtMap::serial。変わったらすべての行を写す)
    // 表は、フレームを写すたびに kCrtBuildSliceUs だけ作り進める (ShowFrame()。1 回で作ると、Pi 3 では 39ms かかり、
    // その間に来たフレームが写されずに置き換わる)。Pi 3 の 1920x1080 で 20 フレームほどかかる
    static constexpr unsigned kCrtBuildSliceUs = 2000;
    // ログ用 (写す役のコアが書き、コア 0 が読む)
    uint32_t m_CrtBuildUs = 0;              // 1 フレームで表を作るのに使った時間の最大 (コア 0 が読んで 0 に戻す)
    uint32_t m_CrtBuildSlices = 0;          // 表を作り進めたフレームの数
    uint32_t m_CrtBuildTotalUs = 0;         // 表を作るのに使った時間の合計
    uint32_t m_CrtBuildsDone = 0;           // 作り終えた表の数
    uint32_t m_CrtBuildSlicesLogged = 0, m_CrtBuildTotalUsLogged = 0, m_CrtBuildsDoneLogged = 0; // コア 0 だけが触る
    void StartCrt();
    bool VideoAllowed(int scale, int filter) const;
    // モニタの枠 (video_filter= の cz-600de / cz-600db。platform/bezel.h)。絵は初めて使うときに起動したドライブの /pix68k/ から読み、
    // 読めたものは持っておく (2 つで 16MB)。読むのはコア 0 だけ (StartCrt() とメニュー。メニューを開いている間は、
    // 合成するコアに写す役を任せない)。読めなかった枠は読み直さない (ログは 1 回)
    platform::BezelImage m_Bezel[2];
    bool m_BezelTried[2] = {false, false};
    const platform::BezelImage* LoadBezel(int filter);
    // ShowFrame() が使う (写す役のコアだけが触る)。いまの枠の外は枠の絵か (どの枠か。0 なら枠なし)
    int m_BezelShown = 0;
    int m_BezelSkip = 0;               // 枠を出さない理由 (platform::bezelSkipCode())。ShowFrame() が毎フレーム書き、コア 0 が読む
    platform::BezelSkipLog m_BezelSkipLog; // コア 0 だけが触る
    platform::OsdState m_OsdState;          // 画面の隅の数字の、いま出ているもの (platform/osd.h)
    // 画面の左上のメッセージ (POST /message。platform/osd_message.h)。字形はコア 0 が作って s_MsgBox (machine_run.cpp)
    // に置き、写す役のコアが ShowFrame() で s_MsgShown に写す。ここの 3 つは写す役のコアだけが触る
    platform::MessageState m_MsgState;      // いま出ているもの
    unsigned m_MsgSeqLoaded = 0;            // s_MsgShown に写した版の番号
    u64 m_MsgDeadline = 0;                  // s_MsgShown を消す時刻 (CTimer::GetClockTicks64()。0 なら消すまで出す)
    // コア 0 だけが触る: 消す時刻 (X68000 が止まっている間に、時刻が来たら最後の画面を写し直すため。0 ならない) と、
    // 新しいメッセージを置いたので止まっている間でも写し直す
    u64 m_MsgExpireTicks = 0;
    bool m_MsgRedraw = false;
    static constexpr int ScaleLinePixels = 4096;
    // 前に写した X68000 の画面の写し。変わっていない行はフレームバッファに書かない (platform/frame_scale.h)。
    // フレームバッファを ShowFrame() 以外で書いたとき (メニュー) は valid を false にする
    platform::FrameRowCache m_RowCache;
    void ForceRows(int y0, int y1);
    uint32_t m_RowsCopied = 0, m_RowsSkipped = 0; // m_RowCache の数の写し (ログ用。コア 0 が読む)
    uint32_t m_RowsLogged[2] = {};                // 前のログのときの数 (コア 0 だけが使う)
    // 1 フレームを写すのにかかった時間 (ログ用。画面の隅の数字を描く時間は含め、枠の外を塗る時間は含めない)。最大はログに出すたびに 0 に戻す
    uint32_t m_CopyCount = 0, m_CopyMaxUs = 0;
    uint64_t m_CopyTotalUs = 0;
    uint32_t m_CopyCountLogged = 0; // 前のログのときの m_CopyCount と m_CopyTotalUs (コア 0 だけが使う)
    uint64_t m_CopyUsLogged = 0;
    // コア 2 / コア 3 が合成に使った時間 (Emulator::takeCoreWork()) の区間を頼んで、まだ受け取っていない (0 が画面、1 が音)
    bool m_CoreWorkPending[2] = {};
    // 10 秒ごとのログで差を出すための、前のログのときの数 (合成した、置き換わった、コア 0 が写した、合成するコアが写した)
    uint32_t m_ScreenLogged[4] = {};
    uint32_t m_RenderWaitsLogged = 0, m_SoundWaitsLogged = 0;
    u64 m_LastPollTicks = 0, m_PollGapMax = 0; // 主ループが takeFrame() を見に行った時刻と、その間隔の最大
    // 画面を写す役を合成するコアに任せている (WithComposerPresenting() か WhileComposerPresents() の中)
    bool m_ComposerPresenting = false;
    uint64_t m_SpeedCyclesLogged = 0, m_SpeedUsLogged = 0; // 前の speed のログのときの数 (区間の速さを出す)
    // 画面の隅の数字 (osd=on / fps)。コア 0 が 1 秒ごとに数え (UpdateOsd())、platform::packOsd() で詰めて PublishOsd() で
    // m_OsdValue (64 ビット 3 つ) に置く。写す役のコアが ShowFrame() で LoadOsd() で読む。3 つの語の一部だけ新しい値で
    // 読まないよう、版の番号 m_OsdSeq で受け渡す (seqlock。書くのはコア 0 だけ。書いている間は奇数)。
    // ログの数 (m_ScreenLogged など) とは別に、前に数えたときの数を持つ
    void PublishOsd(const platform::OsdValue& v) {
        const unsigned seq = __atomic_load_n(&m_OsdSeq, __ATOMIC_RELAXED);
        __atomic_store_n(&m_OsdSeq, seq + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&m_OsdValue.a, v.a, __ATOMIC_RELAXED);
        __atomic_store_n(&m_OsdValue.b, v.b, __ATOMIC_RELAXED);
        __atomic_store_n(&m_OsdValue.c, v.c, __ATOMIC_RELAXED);
        __atomic_store_n(&m_OsdSeq, seq + 2, __ATOMIC_RELEASE);
    }
    platform::OsdValue LoadOsd() const {
        platform::OsdValue v;
        for (;;) {
            const unsigned seq = __atomic_load_n(&m_OsdSeq, __ATOMIC_ACQUIRE);
            v.a = __atomic_load_n(&m_OsdValue.a, __ATOMIC_RELAXED);
            v.b = __atomic_load_n(&m_OsdValue.b, __ATOMIC_RELAXED);
            v.c = __atomic_load_n(&m_OsdValue.c, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (!(seq & 1) && __atomic_load_n(&m_OsdSeq, __ATOMIC_RELAXED) == seq) return v;
        }
    }
    platform::OsdValue m_OsdValue;
    unsigned m_OsdSeq = 0;
    int m_OsdRpmShown = -1;            // OSD に出しているファンの回転数 (platform::osdRpmShown())
    unsigned m_OsdEffMHzShown = 0;     // OSD に出している最高速モードの実効のクロック (platform::osdEffMHzShown())
    u64 m_OsdTicks = 0;                // 前に数えた時刻 (0 なら次に数え始める)
    bool m_OsdPlaced = false;          // 最後に写したときに、数字を出す場所があったか (ShowFrame() が書き、UpdateOsd() が読む)
    uint32_t m_OsdFrames[3] = {};      // 前に数えたときの、合成した数、置き換わった数、写した数
    uint64_t m_OsdCycles = 0, m_OsdUs = 0; // 前に数えたときの X68000 のサイクルと時間 (速さを出す)
    uint64_t m_OsdEmuFrames = 0;           // 前に数えたときの X68000 のフレームの数 (Emulator::stats()。fps を出す)
    uint32_t m_OsdPublished = 0;       // ExpireOsd() が前に見た、合成したフレームの数
    u64 m_OsdPublishedTicks = 0;       // その数が変わった時刻
    platform::KeyRemap m_KeyRemap[MaxKeyboards]; // キーボードごとの、種類に合わせたレポートの書き換え (Pi 400 の切り替えのキーなど)
    // キーボードごとの押しているキー (書き換えたあと) を合わせる。最後の枠 (VirtualKeyboard) はネットワークからのキー入力
    static constexpr int VirtualKeyboard = MaxKeyboards;
    platform::KeyMerger<MaxKeyboards + 1> m_KeyMerge;
    platform::RebootKeyFilter m_RebootKeys; // Ctrl + Alt + Delete (Raspberry Pi の再起動) を見つけ、X68000 に送らない
    // Ctrl + Alt + Delete を押したこと (USB の割り込みから主ループへ) と、書けずに断ったあと 60 秒以内の 2 回目の扱い
    platform::RebootRequest m_RebootRequest{60 * (uint64_t)CLOCKHZ};
    bool m_KeyLayoutAuto = true;       // pix68k.cfg の keyboard=auto (つないだキーボードの VID:PID から決める)
    platform::KeyLayout m_KeyLayoutCfg = platform::KeyLayout::STANDARD; // keyboard= で決めた種類 (auto でないとき)
    platform::KeyTracker m_Keys;       // USB キーボードの押しているキーと、X68000 に送ったキー
    platform::MouseTracker m_Mouse;    // USB マウスの、まだ X68000 に送っていない移動量とボタン
    // キーボードごとの LED (送れなければ 0.25 秒から間を空けて送り直す)
    platform::LedSender m_Leds[MaxKeyboards] = {platform::LedSender{CLOCKHZ / 4}, platform::LedSender{CLOCKHZ / 4},
                                                platform::LedSender{CLOCKHZ / 4}, platform::LedSender{CLOCKHZ / 4}};
    int m_LedNext = 0; // 次に LED を送るかを見るキーボードの枠 (1 回の主ループで送るのは 1 台まで)
    int32_t m_MouseShownDx = 0, m_MouseShownDy = 0; // ログに出したマウスの移動量の合計
    u64 m_MouseShownAt = 0;            // マウスの移動量をログに出した時刻 (1 秒に 1 回まで)
    x68k::SramSlots m_SramSlots;       // SRAM.DAT (スロット 0) と SRAM.BAK (スロット 1)
    u8* m_pSramFile = nullptr;         // 書き込むファイルの中身 (SramSlots::FILE_SIZE)
    static CKernel* s_pThis;
};
