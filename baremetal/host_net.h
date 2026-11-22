// ベアメタル版のネットワークの口 (X68000 の外側。pix68k.cfg / cmdline.txt の net=on のときだけ動かす)。
// SD カードを抜かずに、カーネルや設定を書き換え、ログと画面を見て、キーを打ち、再起動できるようにする。
//
//   TCP/IP   Circle の CNetSubSystem (DHCP か、net_ip= の固定の IP)。有線の Ethernet: Pi 3B+ は USB の LAN7800
//            (USB の装置を探し終えると Circle が見つける)、Pi 4 / Pi 400 は GENET、Pi 5 は RP1 の MACB
//            (CNetSubSystem が作る)。QEMU の raspi3b では -device usb-net
//   HTTP     ポート 80。1 つの接続で 1 つの要求 (応答は Connection: close)。接続は 1 つずつ順に受ける
//              GET  /files/<パス>        ファイルを読む (ディレクトリなら一覧)
//              PUT  /files/<パス>        ファイルを書き換える (POST も同じ。<パス>.part に書き終えてから置き換える。
//                                        platform/net_files.h)。X68000 が使っているディスクのイメージは断る (409)
//              GET  /log                 起動からのログ (最新 64KB。UART と同じ内容)
//              GET  /ts6bga.ppm         TS-6BGA の仮想VRAM (PPM)
//              GET  /screen2.ppm        第2 HDMI の選択中ソース (PPM)
//              GET  /screen.ppm          いま出している X68000 の画面 (PPM)
//              POST /reboot              ディスクと SRAM を書き戻してから再起動する
//              POST /reboot?profile=名前  起動の組み合わせ pix68k-<名前>.cfg で再起動する (空なら pix68k.cfg だけ)
//              POST /type                本体の文字列を X68000 のキーとして打つ (\n などのエスケープを使える)
//              POST /key?down=XX&mod=YY  USB のキー XX (16 進の usage) を押したままにする。mod は修飾キーのビット
//              POST /key?up=XX / /key?up キー XX を離す / すべて離す
//              POST /message?sec=N       本体の文字列を画面の左上に N 秒 (既定 10、0 なら消すまで) 重ねて出す。
//                                        本体が空なら消す (platform/osd_message.h)
//   syslog   syslog= の送り先に、UART と同じログを 1 行ずつ UDP で送る (CLogRing から読む。RFC 5424 の形)
//   RS-232C  rs232c=tcp:PORT のポートで待ち受け、X68000 の RS-232C (SCC のチャネル A) とバイトをやり取りする
//            (CSerialTask。つないでいる間は DCD と CTS を出す。net_allow= は見るが、トークンは見ない)
// 書く操作 (PUT、POST、DELETE) は、net_allow= の送り元か net_token= のトークン (X-Pix68k-Token か
// Authorization: Bearer) が要る (platform/net_config.h の checkNetAccess())。ファイルを読む (GET /files) のも、
// net_token= があればトークンが要る (トークンを書いた pix68k.cfg や cmdline.txt を読めないように)。
// どの操作も、net_allow= があればその送り元からだけ受ける。
//
// コア 0 だけで動かす。Circle の協調型のスケジューラ (CScheduler) を作り、主ループが Service() で Yield() したときだけ
// ネットワークのタスク (TCP/IP、DHCP、HTTP、syslog) が動く。SD カードの読み書きなど時間のかかる処理は、
// CHostNetEnv::netBeginHeavy() / netEndHeavy() で囲み、その間は合成するコアが画面を写す (フレームは飛ばさない)。
#pragma once

#include <circle/device.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/net/netsubsystem.h>
#include <circle/sched/scheduler.h>
#include <circle/string.h>
#include <circle/types.h>

#include "platform/boot_profile.h"
#include "platform/net_files.h"
#include "platform/serial_ring.h"
#include "platform/settings.h"

// RS-232C (rs232c=tcp:PORT) で X68000 と CSerialTask をつなぐリング。Circle は TCP の受信の窓を狭めない (読まなくても相手は
// 送り続ける) ので、X68000 が受信を止めている間のバイトを受信のリングにためる (64KB。あふれた分は捨てる)
using NetSerialLink = platform::RingSerialLinkT<65536, 4096>;

// ログを溜めるリング (最新 64KB)。CTeeDevice がすべてのログを書く (net=off でも溜める。軽い)
class CLogRing : public CDevice {
public:
    static constexpr unsigned SIZE = 64 * 1024;
    int Write(const void* buffer, size_t count) override;
    // 溜まっている分を古い順に out に写し、長さを返す (out は SIZE バイト)
    unsigned Snapshot(char* out);
    // pos (起動してから書いたバイト数で数えた位置) から max バイトまでを out に写し、長さを返す。pos は進める。
    // pos の分がもう上書きされていれば、残っているいちばん古いところから写し、lost に飛ばしたバイト数を足す
    unsigned ReadFrom(u64& pos, char* out, unsigned max, u64& lost);

private:
    char* const m_Buf = s_Buf; // CKernel は main() のスタックに置くので、バッファは .bss に置く
    static char s_Buf[SIZE];
    unsigned m_nHead = 0;   // 次に書く位置
    u64 m_nTotal = 0;       // 起動してから書いたバイト数
    bool m_bWrapped = false; // 一周した
};

// ネットワークの口から CKernel に頼むこと (コア 0 のタスクから呼ぶ)
class CHostNetEnv {
public:
    virtual ~CHostNetEnv() {}
    virtual const char* netDrive() = 0;                     // ファイルを読み書きするドライブ ("SD:" など)。なければ nullptr
    // paths (ドライブの中のパス) のどれかが、X68000 が使っているイメージ (か、その置き換えに使う名前) と同じファイルか。
    // SD カードを読めず決められなければ UNKNOWN
    virtual platform::ImageCheck netImageInUse(const char* const* paths, int count) = 0;
    // path (ドライブの中のパス) を消してよいか (bytes が負)、bytes バイト (0 でもよい) のファイルを書いてよいか。
    // boot ドライブが USB メモリのときは、/pix68k の下だけで、空きが残るときだけ。だめなら why に理由を入れる。
    // bytes が 0 以上で true を返したら、使う大きさ (クラスタに切り上げ、ディレクトリが伸びる分を足す) を書く予定として
    // 覚える (ほかの保存が空きを使い切らないように)。呼んだ側は platform::UploadPlan(bytes, netClusterBytes()) で残りを
    // 数え、書いて減った分と、やめたときの残りを netWriteDone() で返す
    virtual bool netCanWrite(const char* path, long long bytes, const char*& why) = 0;
    virtual void netWriteDone(unsigned long long bytes) = 0;
    virtual unsigned long long netClusterBytes() = 0;       // 上の計算に使うクラスタの大きさ (制限がなければ 1)
    virtual bool netBusy() = 0;                             // 書き換えを断る (設定メニューを開いている)
    virtual void netBeginHeavy() = 0;                       // SD カードの読み書きの前 (画面を合成するコアに任せる)
    virtual void netEndHeavy() = 0;                         // その後
    virtual void netIdleUntil(u64 deadline) = 0;            // 時刻 deadline (CTimer::GetClockTicks64()) まで眠る
    virtual bool netSnapshotScreen(u8* rgb, unsigned capacity, int& width, int& height) = 0;
    virtual bool netSnapshotTs6bga(u8* rgb, unsigned capacity, int& width, int& height, bool output) = 0;
    virtual void netRequestReboot() = 0;                    // 少し後に再起動する (応答を送り終えるため)
    // 次の再起動で使う起動の組み合わせ (POST /reboot?profile=。空なら pix68k.cfg だけ)。名前とファイルを確かめ、
    // profile.sel に書く。nullptr なら前に頼まれたものを取り消す
    virtual platform::ProfileRequest netBootProfile(const char* name) = 0;
    virtual bool netKeyReport(u8 modifiers, const u8 keys[6]) = 0; // USB キーボードのレポートと同じ経路で送る
    virtual unsigned netLog(char* out) = 0;                 // CLogRing::Snapshot()
    virtual unsigned netLogRead(u64& pos, char* out, unsigned max, u64& lost) = 0; // CLogRing::ReadFrom()
    virtual void netThermal(CString& out) = 0;              // 温度、クロック、ファン (名前=値 を 1 行ずつ)
    // RS-232C の相手のリング (rs232c=tcp:PORT のとき、X68000 を動かす前に作ってつないだもの)。なければ nullptr
    virtual NetSerialLink* netSerialLink() = 0;
    // 画面の左上に text (エスケープを戻した UTF-8、n バイト) を sec 秒 (0 なら消すまで) 出す。n が 0 なら消す
    // (platform/osd_message.h)。メモリを確保できなければ false
    virtual bool netMessage(const char* text, unsigned n, unsigned sec) = 0;
};

class CHttpTask;
class CSysLogTask;
class CSerialTask;

class CHostNet {
public:
    CHostNet(CLogger& logger, CHostNetEnv& env) : m_Logger(logger), m_Env(env) {}
    ~CHostNet();

    // pix68k.cfg の設定と cmdline.txt (こちらが勝つ) から決める。net=on なら true
    bool Configure(const platform::Settings& s, CKernelOptions& options);
    // スケジューラと TCP/IP を作る (待たない。DHCP はあとで Service() の中で進む)。Pi 4 / Pi 5 では Ethernet の
    // コントローラを初期化する (時間がかかるので、呼ぶ側で合成するコアに画面を任せる)
    bool Start();
    // コア 0 の主ループから呼ぶ。ネットワークのタスクを動かす (waitUs の間、Yield() を繰り返す。0 なら 1 回)
    void Service(unsigned waitUs);
    // いまの IP アドレス (192.168.1.27 なら 0xc0a8011b)。リンクが切れているか、DHCP の答えを待っているときは 0
    // (画面の隅の数字に出す。コア 0 から呼ぶ。ネットワークのタスクと同じコアなので、読む間に書き換わらない)
    u32 IPv4() const;

private:
    void CheckRunning();

    CLogger& m_Logger;
    CHostNetEnv& m_Env;
    CScheduler* m_pScheduler = nullptr;
    CNetSubSystem* m_pNet = nullptr;
    CSysLogTask* m_pSysLog = nullptr;
    CHttpTask* m_pHttp = nullptr;
    CSerialTask* m_pSerial = nullptr;
    NetSerialLink* m_pSerialLink = nullptr; // RS-232C (rs232c=tcp:PORT のとき)
    u16 m_SerialPort = 0;
    bool m_bRunning = false;
    u64 m_StartTicks = 0;
    bool m_bWaitLogged = false;
    // 決めた設定
    bool m_bDhcp = true;
    u8 m_Ip[4] = {}, m_Mask[4] = {}, m_Gw[4] = {}, m_Dns[4] = {};
    bool m_bGw = false, m_bDns = false;
    char m_Name[64] = "pix68k";
    char m_Allow[platform::Settings::PATH_SIZE] = {};
    char m_Token[platform::Settings::PATH_SIZE] = {};
    bool m_bSysLog = false;
    u8 m_SysLogIp[4] = {};
    u16 m_SysLogPort = 514;
};
