// ベアメタル版のネットワークの口 (説明は host_net.h)
#include "host_net.h"

#include <circle/net/error.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/net/socket.h>
#include <circle/sched/task.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/sysconfig.h>
#include <fatfs/ff.h>

// PutFile() は、使っているイメージとの重なりを最後に見てから入れ替え終わるまで、主ループに戻らない (Yield() しない)
// ことを前提にしている。NO_BUSY_WAIT を定義すると、SD カードと NVMe のドライバが待つ間に譲るようになり、その間に
// 設定メニューがイメージをつなぎ替えられてしまう
#ifdef NO_BUSY_WAIT
#error "host_net.cpp assumes the SD card and NVMe drivers do not yield (NO_BUSY_WAIT must not be defined)"
#endif

#include "platform/key_type.h"
#include "platform/net_config.h"
#include "platform/boot_drive.h"
#include "platform/net_files.h"
#include "platform/osd_message.h"
#include "platform/net_http.h"
#include "platform/serial_ring.h"

static const char FromNet[] = "net";
static const char FromHttp[] = "httpd";
static const char FromRs232c[] = "rs232c";

// ---- ログのリング ----

char CLogRing::s_Buf[CLogRing::SIZE];

int CLogRing::Write(const void* buffer, size_t count) {
    // ログはコア 0 のタスクと割り込みから書く (コア 1〜3 は書かない)。割り込みを止めて、書く位置を進める
    const char* p = (const char*)buffer;
    EnterCritical();
    for (size_t i = 0; i < count; i++) {
        if (p[i] == '\r') continue;
        m_Buf[m_nHead] = p[i];
        m_nTotal++;
        if (++m_nHead == SIZE) {
            m_nHead = 0;
            m_bWrapped = true;
        }
    }
    LeaveCritical();
    return (int)count;
}

unsigned CLogRing::Snapshot(char* out) {
    EnterCritical();
    unsigned n = 0;
    if (m_bWrapped) {
        memcpy(out, m_Buf + m_nHead, SIZE - m_nHead);
        n = SIZE - m_nHead;
    }
    memcpy(out + n, m_Buf, m_nHead);
    n += m_nHead;
    LeaveCritical();
    return n;
}

unsigned CLogRing::ReadFrom(u64& pos, char* out, unsigned max, u64& lost) {
    EnterCritical();
    if (pos > m_nTotal) pos = m_nTotal;
    if (m_nTotal - pos > SIZE) {
        lost += m_nTotal - SIZE - pos;
        pos = m_nTotal - SIZE;
    }
    u64 avail = m_nTotal - pos;
    const unsigned n = avail < max ? (unsigned)avail : max;
    // m_nHead は m_nTotal % SIZE と同じ
    unsigned at = (unsigned)(pos % SIZE);
    for (unsigned i = 0; i < n; i++) {
        out[i] = m_Buf[at];
        if (++at == SIZE) at = 0;
    }
    pos += n;
    LeaveCritical();
    return n;
}

// ---- syslog ----

// CLogRing に溜まったログ (UART と同じ行) を、1 行ずつ UDP の syslog (RFC 5424) で送る。初めは Circle の CSysLogDaemon
// (CLogger のイベントを送る) を使ったが、実機 (Pi 3B+) で何も届かなかった (原因は分かっていない)。送った数と失敗した数を
// GET / に出し、送れなければログに出す
class CSysLogTask : public CTask {
public:
    CSysLogTask(CNetSubSystem* net, CLogger& logger, CHostNetEnv& env, const u8 ip[4], u16 port, const char* host)
        : m_pNet(net), m_Logger(logger), m_Env(env), m_Port(port), m_Host(host) {
        memcpy(m_Ip, ip, 4);
        SetName("syslog");
    }
    void Run() override;
    unsigned sent() const { return m_nSent; }
    unsigned failed() const { return m_nFailed; }
    unsigned unreachable() const { return m_nUnreachable; }
    u64 lost() const { return m_nLost; }

private:
    void SendLine(CSocket& s, const char* line, unsigned len);
    CNetSubSystem* m_pNet;
    CLogger& m_Logger;
    CHostNetEnv& m_Env;
    u8 m_Ip[4];
    u16 m_Port;
    const char* m_Host;
    u64 m_Pos = 0;
    u64 m_nLost = 0;
    unsigned m_nSent = 0, m_nFailed = 0;
    unsigned m_nUnreachable = 0; // 送り先から ICMP の到達不能が返った回数 (送り直した)
    int m_LastError = 0;
    static constexpr unsigned MAX_LINE = 1024;
    char m_Line[MAX_LINE];
    unsigned m_nLine = 0;
    char m_Chunk[2048];
    char m_Msg[MAX_LINE + 128];
};

void CSysLogTask::SendLine(CSocket& s, const char* line, unsigned len) {
    // <13> = facility user (1) × 8 + severity notice (5)。時刻は持たないので "-"
    CString head;
    head.Format("<13>1 - %s pix68k - - - ", m_Host);
    const unsigned h = head.GetLength() < 128 ? head.GetLength() : 128;
    memcpy(m_Msg, (const char*)head, h);
    if (len > sizeof m_Msg - h) len = sizeof m_Msg - h;
    memcpy(m_Msg + h, line, len);
    int r = s.Send(m_Msg, h + len, MSG_DONTWAIT);
    // 送り先が ICMP の到達不能 (受け手の syslogd が動いていないなど) を返すと、Circle の UDP は次の Send() で 1 回だけ
    // そのエラーを返し、その行を送らない。エラーは返したときに消えるので、もう一度送る
    if (r == -NET_ERROR_DESTINATION_UNREACHABLE || r == -NET_ERROR_PROTOCOL_ERROR) {
        m_nUnreachable++;
        r = s.Send(m_Msg, h + len, MSG_DONTWAIT);
    }
    if (r == (int)(h + len)) {
        if (m_nSent++ == 0)
            m_Logger.Write("net", LogNotice, "syslog: sending to %u.%u.%u.%u:%u", m_Ip[0], m_Ip[1], m_Ip[2], m_Ip[3],
                           m_Port);
        return;
    }
    // 失敗は、エラーの値が変わったときだけログに出す (出した行も送るので、毎回は出さない)
    m_nFailed++;
    if (r != m_LastError) {
        m_LastError = r;
        m_Logger.Write("net", LogWarning, "syslog: cannot send (%d)", r);
    }
}

void CSysLogTask::Run() {
    CSocket s(m_pNet, IPPROTO_UDP);
    const int rc = s.Connect(CIPAddress(m_Ip), m_Port);
    if (rc < 0) {
        m_Logger.Write("net", LogError, "syslog: cannot open a UDP socket (%d)", rc);
        return;
    }
    for (;;) {
        // 1 回に送るのは 2KB 分まで (ほかのタスクを待たせない)
        const unsigned n = m_Env.netLogRead(m_Pos, m_Chunk, sizeof m_Chunk, m_nLost);
        for (unsigned i = 0; i < n; i++) {
            const char c = m_Chunk[i];
            if (c == '\n') {
                if (m_nLine != 0) SendLine(s, m_Line, m_nLine);
                m_nLine = 0;
            } else if (m_nLine < MAX_LINE) {
                m_Line[m_nLine++] = c;
            }
        }
        if (n < sizeof m_Chunk) CScheduler::Get()->MsSleep(100);
        else CScheduler::Get()->Yield();
    }
}

// ---- RS-232C (SCC のチャネル A) の TCP ----
// rs232c=tcp:PORT のポートで待ち受け、つないできた相手 1 つとバイトをそのままやり取りする。X68000 (コア 1) とは
// NetSerialLink で受け渡す。つないでいる間は DCD と CTS を出す。相手を切るまで次の接続は受けない
// (Circle の待ち受けのキューで待つ)。net_allow= があれば、その送り元からだけ受ける (トークンは見ない)

class CSerialTask : public CTask {
public:
    CSerialTask(CNetSubSystem* net, CLogger& logger, NetSerialLink* link, u16 port, const char* allow)
        : m_pNet(net), m_Logger(logger), m_pLink(link), m_Port(port), m_Allow(allow) {
        SetName(FromRs232c);
    }
    void Run() override;

private:
    void Serve(CSocket* c);
    CNetSubSystem* m_pNet;
    CLogger& m_Logger;
    NetSerialLink* m_pLink;
    u16 m_Port;
    const char* m_Allow;
    // Circle の CSocket::Receive() は届いた 1 つの区切り (セグメント) を渡し、渡す場所が短いと残りを捨てる。
    // セグメントは 1 フレームに収まるので、FRAME_BUFFER_SIZE 以上の場所で受け取る
    u8 m_Rx[FRAME_BUFFER_SIZE];
    u8 m_Tx[1024];
};

void CSerialTask::Run() {
    CSocket listener(m_pNet, IPPROTO_TCP);
    if (listener.Bind(m_Port) < 0 || listener.Listen(1) < 0) {
        m_Logger.Write(FromRs232c, LogError, "cannot listen on port %u", m_Port);
        return;
    }
    m_Logger.Write(FromRs232c, LogNotice, "listening on port %u (RS-232C)", m_Port);
    for (;;) {
        CIPAddress from;
        u16 port = 0;
        CSocket* c = listener.Accept(&from, &port);
        if (c == nullptr) {
            CScheduler::Get()->MsSleep(100);
            continue;
        }
        u8 ip[4];
        from.CopyTo(ip);
        if (m_Allow[0] != 0 && !platform::allowListHas(m_Allow, ip)) {
            m_Logger.Write(FromRs232c, LogWarning, "refused %u.%u.%u.%u (not in net_allow=)", ip[0], ip[1], ip[2], ip[3]);
            delete c;
            continue;
        }
        m_Logger.Write(FromRs232c, LogNotice, "connected from %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
        Serve(c);
        delete c;
        m_Logger.Write(FromRs232c, LogNotice, "disconnected");
    }
}

void CSerialTask::Serve(CSocket* c) {
    // つながる前に X68000 が送ったバイトは捨てる (相手のいない線に送ったのと同じ)
    while (m_pLink->popTx() >= 0) {
    }
    // 前の相手から届いて、まだ X68000 が読んでいないバイトも捨てる (次の相手のバイトと混ぜない)。
    // 切れたときではなく今捨てるのは、送ってすぐ閉じる相手の最後まで X68000 に渡すため
    m_pLink->dropRx();
    m_pLink->setCarrier(true);
    unsigned dropped = 0;
    for (;;) {
        bool busy = false;
        // 受け取る: リングの空きに関係なく読む。Circle は TCP の受信の窓を狭めないので、読まずにいても相手は待たず、
        // Circle の受信のキューが伸び続ける。また、キューを空にしないと、相手が切れたこと (CLOSE_WAIT) が
        // Receive() の負の値で分からない (GetStatus() の bConnected は CLOSE_WAIT でも 1)。
        // リングに入らない分は捨てる。X68000 が受信を止めているときだけでなく、RTS が上がっていても、X68000 は
        // ボーレートの速さでしか読まないので、相手が一度に 64KB を超えて送れば超えた分を捨てる (docs/expansion.md の RS-232C の節。
        // 相手はボーレートより速く送らないか、一度に 64KB までにする)
        const int n = platform::pumpRx(*m_pLink, m_Rx, sizeof m_Rx,
                                       [c](u8* buf, unsigned size) { return c->Receive(buf, size, MSG_DONTWAIT); }, dropped);
        if (n < 0) break;
        busy = n > 0;
        // 送る: Circle の送信待ちが上限 (TCP_CONFIG_TX_THRESHOLD) より少ないときだけ。MSG_DONTWAIT の Send() は
        // 上限を見ずに積むので、相手が読まないと送信待ちが際限なく増える (上限の判定は積む前なので、上限を最大で
        // m_Tx の 1024 バイト弱超える)。送れない間は送信のリング (4KB) にためて、満ちたら X68000 の送ったバイトを捨てる
        const CSocket::TStatus st = c->GetStatus();
        if (!st.bConnected) break;
        const int w = platform::pumpTx(*m_pLink, m_Tx, sizeof m_Tx, st.bTxReady,
                                       [c](u8* buf, unsigned len) { return c->Send(buf, len, MSG_DONTWAIT); });
        if (w < 0) break;
        busy = busy || w > 0;
        // 9600bps で 1 バイトは約 1ms。届いたものがあれば続けて見る
        if (busy) CScheduler::Get()->Yield();
        else CScheduler::Get()->MsSleep(1);
    }
    if (dropped) m_Logger.Write(FromRs232c, LogWarning, "dropped %u received bytes (the X68000 was not reading)", dropped);
    m_pLink->setCarrier(false);
}

// ---- HTTP ----

class CHttpTask : public CTask {
public:
    CHttpTask(CNetSubSystem* net, CLogger& logger, CHostNetEnv& env, const char* allow, const char* token,
              const char* name, const CSysLogTask* syslog)
        : m_pNet(net), m_Logger(logger), m_Env(env), m_Allow(allow), m_Token(token), m_Name(name), m_pSysLog(syslog) {
        SetName(FromHttp);
    }
    void Run() override;

private:
    static constexpr unsigned BUF_SIZE = 32 * 1024; // ファイルの読み書きの単位 (SD カードに 1 回で書く量)。確保はこれに FRAME_BUFFER_SIZE を足す
    static constexpr unsigned MAX_TYPE = 4096;      // /type の本体の上限
    void Handle(CSocket* c, const u8 ip[4]);
    bool SendAll(const void* data, unsigned n);
    bool SendHead(int status, const char* type, long long length);
    void SendText(int status, const char* text);
    bool Receive(unsigned& n);
    bool ReceiveBefore(u64 deadline, unsigned& n); // m_pBuf に受け取る。切れたら false
    void GetFile(const char* path, bool head);
    void ListDir(const char* drivePath, const char* path, bool head);
    void PutFile(const char* path);
    void DeleteFile(const char* path);
    platform::ImageCheck TouchesImage(const char* path, bool upload);
    bool RefuseImage(const char* path, bool upload);
    void GetLog(bool head);
    void GetScreen(bool head, unsigned source = 0);
    void Type();
    void Key();
    void Message();
    bool ReadBody(char* out, unsigned max, unsigned& n);
    bool KeyState(); // 押しているキー (m_KeyMods、m_Keys) を送る

    CNetSubSystem* m_pNet;
    CLogger& m_Logger;
    CHostNetEnv& m_Env;
    const char* m_Allow;
    const char* m_Token;
    const char* m_Name; // net_name= (Host: に来てよい名前)
    const CSysLogTask* m_pSysLog; // syslog= がなければ nullptr
    CSocket* m_pConn = nullptr;
    platform::HttpHeadReader m_Head;
    platform::HttpRequest m_Req;
    u8* m_pBuf = nullptr;     // BUF_SIZE
    char* m_pLog = nullptr;   // CLogRing::SIZE (/log)
    u8* m_pScreen = nullptr;  // 画面の写し (/screen.ppm。初めて頼まれたときに確保する)
    unsigned m_ScreenCapacity = 0;
    char* m_pType = nullptr;  // MAX_TYPE + 1 (/type)
    unsigned m_nLeft = 0;     // m_pBuf の先頭に残っている本体のバイト数 (頭と一緒に届いた分)
    int m_Status = 0;
    long long m_nBytes = 0;   // 応答か受け取った本体の長さ (ログ用)
    FIL m_File;
    DIR m_Dir;
    FILINFO m_Info;
    u8 m_KeyMods = 0, m_Keys[6] = {};
};

static const unsigned kScreenBytes = 1024u * 1024u * 3u; // x68k::Renderer::MAX_WIDTH × MAX_HEIGHT × RGB

void CHttpTask::Run() {
    m_pBuf = new u8[BUF_SIZE + FRAME_BUFFER_SIZE];
    m_pLog = new char[CLogRing::SIZE];
    m_pType = new char[MAX_TYPE + 1];
    CSocket listener(m_pNet, IPPROTO_TCP);
    if (m_pBuf == nullptr || m_pLog == nullptr || m_pType == nullptr || listener.Bind(80) < 0 ||
        listener.Listen(4) < 0) {
        m_Logger.Write(FromHttp, LogError, "cannot listen on port 80");
        return;
    }
    m_Logger.Write(FromHttp, LogNotice, "listening on port 80");
    for (;;) {
        CIPAddress from;
        u16 port = 0;
        CSocket* c = listener.Accept(&from, &port);
        if (c == nullptr) {
            CScheduler::Get()->MsSleep(100);
            continue;
        }
        u8 ip[4];
        from.CopyTo(ip);
        Handle(c, ip);
        delete c; // 送った分を送り終えてから閉じる (Circle の TCP)
        m_pConn = nullptr;
    }
}

bool CHttpTask::SendAll(const void* data, unsigned n) {
    const u8* p = (const u8*)data;
    while (n > 0) {
        const unsigned chunk = n > 8192 ? 8192 : n;
        const int r = m_pConn->Send(p, chunk, 0);
        if (r <= 0) return false;
        p += r;
        n -= (unsigned)r;
    }
    return true;
}

bool CHttpTask::SendHead(int status, const char* type, long long length) {
    m_Status = status;
    CString h;
    h.Format("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\n", status, platform::httpReason(status), type);
    if (length >= 0) {
        CString l;
        l.Format("Content-Length: %u\r\n", (unsigned)length);
        h.Append(l);
    }
    h.Append("\r\n");
    return SendAll((const char*)h, h.GetLength());
}

void CHttpTask::SendText(int status, const char* text) {
    const unsigned n = strlen(text);
    if (SendHead(status, "text/plain; charset=utf-8", n) && m_Req.method != platform::HttpRequest::HEAD && n > 0)
        SendAll(text, n);
}

bool CHttpTask::Receive(unsigned& n) {
    const int r = m_pConn->Receive(m_pBuf, FRAME_BUFFER_SIZE, 0);
    if (r <= 0) return false;
    n = (unsigned)r;
    return true;
}

// 時刻 deadline (CTimer::GetClockTicks64()) までに届いた分を受け取る。届かずに過ぎれば false。
// 少しずつ送り続けて 1 つの接続を長く占めないよう、要求の頭と /type などの小さな本体は、全体を 10 秒で区切る
bool CHttpTask::ReceiveBefore(u64 deadline, unsigned& n) {
    const u64 now = CTimer::GetClockTicks64();
    if (now >= deadline) return false;
    m_pConn->SetOptionReceiveTimeout((unsigned)((deadline - now) / (CLOCKHZ / 1000000)) + 1);
    return Receive(n);
}

void CHttpTask::Handle(CSocket* c, const u8 ip[4]) {
    m_pConn = c;
    m_Status = 0;
    m_nBytes = 0;
    m_nLeft = 0;
    m_Req = platform::HttpRequest();
    c->SetOptionReceiveTimeout(10 * 1000000); // 10 秒届かなければ切る (ほかの接続を待たせないため)
    const u64 t0 = CTimer::GetClockTicks64();
    m_Head.reset();
    const u64 headDeadline = t0 + 10 * (u64)CLOCKHZ; // 頭は全体で 10 秒まで
    for (;;) {
        unsigned n = 0;
        if (!ReceiveBefore(headDeadline, n)) return;
        size_t used = 0;
        const platform::HttpHeadReader::State st = m_Head.feed((const char*)m_pBuf, n, used);
        if (st == platform::HttpHeadReader::TOO_LARGE) {
            SendText(400, "request header too large\n");
            return;
        }
        if (st == platform::HttpHeadReader::BAD) {
            SendText(400, "control character in the request header\n");
            return;
        }
        if (st == platform::HttpHeadReader::DONE) {
            m_nLeft = n - (unsigned)used;
            memmove(m_pBuf, m_pBuf + used, m_nLeft);
            break;
        }
    }
    c->SetOptionReceiveTimeout(10 * 1000000);
    if (!platform::parseHttpRequest(m_Head.text(), m_Req)) {
        SendText(400, m_Req.error);
        return;
    }
    using M = platform::HttpRequest;
    const M::Method m = m_Req.method;
    const bool write = m == M::PUT || m == M::POST || m == M::DELETE;
    const char* p = m_Req.path;
    const bool files = strncmp(p, "/files", 6) == 0 && (p[6] == 0 || p[6] == '/');
    const platform::NetOp op = write ? platform::NetOp::WRITE : files ? platform::NetOp::READ_FILES : platform::NetOp::READ;
    // Host: が自分のアドレスか net_name= でなければ、読むだけの要求も断る (DNS リバインディング対策。
    // platform::checkHost())。アドレスは DHCP で変わることがあるので、要求ごとに読む
    {
        u8 self[4];
        m_pNet->GetConfig()->GetIPAddress()->CopyTo(self);
        switch (platform::checkHost(m_Req, self, m_Name)) {
        case platform::HostCheck::OK: break;
        case platform::HostCheck::MISSING: SendText(400, "Host: is required\n"); goto done;
        case platform::HostCheck::OTHER:
            SendText(403, "Host: must be this device's address or net_name=\n");
            goto done;
        }
    }
    // ブラウザが送る書き込み (Origin: が付く) は断る。net_allow= だけでトークンなしの設定だと、ほかのサイトの
    // ページから、プリフライトなしで送れる本体付きの POST で /files、/reboot、/type を叩けてしまうため
    if (write && m_Req.hasOrigin) {
        SendText(403, "requests from web pages (with Origin:) cannot write; use curl\n");
        goto done;
    }
    switch (platform::checkNetAccess(m_Allow, m_Token, ip, m_Req.token, op)) {
    case platform::NetAccess::OK: break;
    case platform::NetAccess::DENIED_SOURCE: SendText(403, "not allowed from this address (net_allow=)\n"); goto done;
    case platform::NetAccess::DENIED_TOKEN: SendText(403, "bad token (X-Pix68k-Token, net_token=)\n"); goto done;
    case platform::NetAccess::NO_WRITE_RULE:
        SendText(403, "writing is disabled: set net_allow= or net_token= in pix68k.cfg\n");
        goto done;
    }
    if (files) {
        char decoded[256], path[256];
        if (!platform::urlDecode(p + 6, decoded, sizeof decoded) ||
            !platform::normalizeFilePath(decoded, path, sizeof path)) {
            SendText(400, "bad path\n");
        } else if (m == M::GET || m == M::HEAD) {
            GetFile(path, m == M::HEAD);
        } else if (m == M::DELETE) {
            DeleteFile(path);
        } else if (write) {
            PutFile(path);
        } else {
            SendText(405, "use GET, PUT, POST or DELETE\n");
        }
    } else if (strcmp(p, "/log") == 0 && (m == M::GET || m == M::HEAD)) {
        GetLog(m == M::HEAD);
    } else if (strcmp(p, "/thermal") == 0 && (m == M::GET || m == M::HEAD)) {
        CString text;
        m_Env.netThermal(text);
        SendText(200, text);
    } else if (strcmp(p, "/screen.ppm") == 0 && (m == M::GET || m == M::HEAD)) {
        GetScreen(m == M::HEAD);
    } else if ((strcmp(p, "/ts6bga.ppm") == 0 || strcmp(p, "/screen2.ppm") == 0) &&
               (m == M::GET || m == M::HEAD)) {
        GetScreen(m == M::HEAD, strcmp(p, "/ts6bga.ppm") == 0 ? 1 : 2);
    } else if (strcmp(p, "/reboot") == 0 && m == M::POST) {
        // ?profile=名前 なら、次の起動をその組み合わせ (pix68k-<名前>.cfg) にする (platform/boot_profile.h)
        char profile[64];
        const bool hasProfile = platform::queryHas(m_Req.query, "profile");
        platform::ProfileRequest pr = platform::ProfileRequest::OK;
        // 頼まれた組み合わせは、ここで profile.sel に書く。書けなければ再起動しない
        m_Env.netBeginHeavy();
        if (hasProfile)
            pr = platform::queryValue(m_Req.query, "profile", profile, sizeof profile) ? m_Env.netBootProfile(profile)
                                                                                     : platform::ProfileRequest::BAD_NAME;
        else
            pr = m_Env.netBootProfile(nullptr); // 前に頼まれて、書き戻せずに断った組み合わせを忘れる
        m_Env.netEndHeavy();
        switch (pr) {
        case platform::ProfileRequest::OK:
            SendText(202, "rebooting\n");
            m_Env.netRequestReboot();
            break;
        case platform::ProfileRequest::BAD_NAME:
            SendText(400, "profile= is a name of letters, digits, - and _ (pix68k-<name>.cfg), or empty for pix68k.cfg\n");
            break;
        case platform::ProfileRequest::NOT_FOUND: SendText(404, "no such profile (pix68k-<name>.cfg in /pix68k)\n"); break;
        case platform::ProfileRequest::NO_DRIVE: SendText(503, "no boot drive\n"); break;
        case platform::ProfileRequest::WRITE_FAILED:
            SendText(500, "cannot write /pix68k/profile.sel; not rebooting\n");
            break;
        }
    } else if (strcmp(p, "/type") == 0 && m == M::POST) {
        Type();
    } else if (strcmp(p, "/key") == 0 && m == M::POST) {
        Key();
    } else if (strcmp(p, "/message") == 0 && m == M::POST) {
        Message();
    } else if (strcmp(p, "/") == 0 && (m == M::GET || m == M::HEAD)) {
        CString help;
        help = "pix68k\n"
                      "GET  /files/<path>    read a file (a directory gives a list)\n"
                      "PUT  /files/<path>    replace a file (curl -T FILE URL)\n"
                      "DELETE /files/<path>  delete a file (curl -X DELETE URL)\n"
                      "GET  /log             log since boot (last 64KB)\n"
                      "GET  /thermal         temperature, ARM clock, throttling and the fan (Pi 5)\n"
                      "GET  /screen.ppm      the X68000 screen\n"
                      "GET  /ts6bga.ppm      the TS-6BGA framebuffer (unscaled)\n"
                      "GET  /screen2.ppm     selected HDMI 2 source (unfiltered, unscaled)\n"
                      "POST /reboot          write back disks and SRAM, then reboot\n"
                      "POST /reboot?profile=NAME   reboot with /pix68k/pix68k-NAME.cfg (empty: pix68k.cfg only)\n"
                      "POST /type            type the body as X68000 keys (\\n is Enter)\n"
                      "POST /key?down=XX&mod=YY, /key?up=XX, /key?up   hold / release USB keys\n"
                      "POST /message?sec=N   show the body (UTF-8) at the top left for N s (10; 0 = until cleared);\n"
                      "                      an empty body clears it\n"
                      "With net_token=, /files (even GET), /reboot, /type, /key and /message need X-Pix68k-Token.\n";
        if (m_pSysLog != nullptr) {
            CString st;
            st.Format("syslog: %u lines sent, %u failed, %u bytes skipped, %u ICMP unreachable (no syslogd listening?)\n",
                      m_pSysLog->sent(), m_pSysLog->failed(), (unsigned)m_pSysLog->lost(), m_pSysLog->unreachable());
            help.Append(st);
        } else {
            help.Append("syslog: off\n");
        }
        SendText(200, help);
    } else {
        SendText(404, "not found (GET / for the list)\n");
    }
done:
    const unsigned ms = (unsigned)((CTimer::GetClockTicks64() - t0) / (CLOCKHZ / 1000));
    static const char* const kMethods[] = {"GET", "HEAD", "PUT", "POST", "DELETE", "?"};
    m_Logger.Write(FromHttp, LogNotice, "%u.%u.%u.%u %s %s: %d (%u bytes, %u ms)", ip[0], ip[1], ip[2], ip[3],
                   kMethods[m], p, m_Status, (unsigned)m_nBytes, ms);
}

void CHttpTask::GetFile(const char* path, bool head) {
    const char* drive = m_Env.netDrive();
    if (drive == nullptr) {
        SendText(503, "no boot drive\n");
        return;
    }
    CString full;
    full.Format("%s%s", drive, path);
    FRESULT r = FR_OK;
    if (strcmp(path, "/") != 0) {
        m_Env.netBeginHeavy();
        r = f_stat(full, &m_Info);
        m_Env.netEndHeavy();
        if (r != FR_OK) {
            SendText(404, "no such file\n");
            return;
        }
        if (m_Info.fattrib & AM_DIR) {
            ListDir(full, path, head);
            return;
        }
    } else {
        ListDir(full, path, head);
        return;
    }
    m_Env.netBeginHeavy();
    r = f_open(&m_File, full, FA_READ | FA_OPEN_EXISTING);
    m_Env.netEndHeavy();
    if (r != FR_OK) {
        SendText(404, "cannot open\n");
        return;
    }
    const FSIZE_t size = f_size(&m_File);
    if (SendHead(200, "application/octet-stream", (long long)size) && !head) {
        FSIZE_t done = 0;
        while (done < size) {
            UINT got = 0;
            const UINT want = size - done > BUF_SIZE ? BUF_SIZE : (UINT)(size - done);
            m_Env.netBeginHeavy();
            r = f_read(&m_File, m_pBuf, want, &got);
            m_Env.netEndHeavy();
            if (r != FR_OK || got == 0) break;
            if (!SendAll(m_pBuf, got)) break;
            done += got;
            m_nBytes = (long long)done;
            CScheduler::Get()->Yield();
        }
    }
    m_Env.netBeginHeavy();
    f_close(&m_File);
    m_Env.netEndHeavy();
}

// ディレクトリの一覧 (1 行に 1 つ。「名前<TAB>大きさ」、ディレクトリは名前の後ろに /)。BUF_SIZE に入る分まで
void CHttpTask::ListDir(const char* full, const char* path, bool head) {
    CString dir(full);
    if (strcmp(path, "/") != 0) dir.Append("/");
    m_Env.netBeginHeavy();
    FRESULT r = f_findfirst(&m_Dir, &m_Info, dir, "*");
    unsigned n = 0;
    while (r == FR_OK && m_Info.fname[0]) {
        CString line;
        line.Format("%s%s\t%u\n", m_Info.fname, (m_Info.fattrib & AM_DIR) ? "/" : "", (unsigned)m_Info.fsize);
        if (n + line.GetLength() + 32 >= BUF_SIZE) {
            memcpy(m_pBuf + n, "...\n", 4);
            n += 4;
            break;
        }
        memcpy(m_pBuf + n, (const char*)line, line.GetLength());
        n += line.GetLength();
        r = f_findnext(&m_Dir, &m_Info);
    }
    f_closedir(&m_Dir);
    m_Env.netEndHeavy();
    m_nBytes = n;
    if (SendHead(200, "text/plain; charset=utf-8", n) && !head && n > 0) SendAll(m_pBuf, n);
}

// FatFs で置き換える (platform/net_files.h の commitUpload() の Fs)
namespace {
struct CUploadFs {
    platform::FsResult conv(FRESULT r) {
        if (r == FR_OK) return platform::FsResult::OK;
        if (r == FR_NO_FILE || r == FR_NO_PATH) return platform::FsResult::NO_FILE;
        return platform::FsResult::ERROR;
    }
    platform::FsResult exists(const char* p) {
        FILINFO fi;
        return conv(f_stat(p, &fi));
    }
    bool isDirectory(const char* p) {
        FILINFO fi;
        return f_stat(p, &fi) == FR_OK && (fi.fattrib & AM_DIR) != 0;
    }
    platform::FsResult remove(const char* p) { return conv(f_unlink(p)); }
    platform::FsResult rename(const char* a, const char* b) {
        // FatFs の f_rename の新しい名前にはドライブ名を付けない
        const char* colon = strchr(b, ':');
        return conv(f_rename(a, colon ? colon + 1 : b));
    }
};
} // namespace

// path (書き換えるなら upload。<パス>.part と <パス>.old も) が、X68000 が使っているイメージと同じファイルか
platform::ImageCheck CHttpTask::TouchesImage(const char* path, bool upload) {
    char part[300], old[300];
    if (!platform::withSuffix(path, ".part", part, sizeof part) || !platform::withSuffix(path, ".old", old, sizeof old))
        return platform::ImageCheck::UNKNOWN;
    const char* const paths[] = {path, part, old};
    m_Env.netBeginHeavy();
    const platform::ImageCheck c = m_Env.netImageInUse(paths, upload ? 3 : 1);
    m_Env.netEndHeavy();
    return c;
}

// 使っているイメージに当たるか、決められなければ断りの応答を送って true
bool CHttpTask::RefuseImage(const char* path, bool upload) {
    switch (TouchesImage(path, upload)) {
    case platform::ImageCheck::FREE: return false;
    case platform::ImageCheck::IN_USE:
        SendText(409, upload ? "the X68000 is using this disk image (or its .part / .old); change it in pix68k.cfg or "
                               "the menu and reboot first\n"
                             : "the X68000 is using this disk image\n");
        return true;
    default: SendText(503, "cannot check whether the X68000 is using this file (SD card read error?)\n"); return true;
    }
}

// ファイルを消す (DELETE)。ディレクトリと、X68000 が使っているディスクのイメージは断る
void CHttpTask::DeleteFile(const char* path) {
    const char* drive = m_Env.netDrive();
    if (drive == nullptr) {
        SendText(503, "no boot drive\n");
        return;
    }
    if (strcmp(path, "/") == 0) {
        SendText(400, "no file name\n");
        return;
    }
    const char* why = nullptr;
    if (!m_Env.netCanWrite(path, -1, why)) {
        SendText(403, why);
        return;
    }
    if (RefuseImage(path, false)) return;
    if (m_Env.netBusy()) {
        SendText(503, "the settings menu is open; try again later\n");
        return;
    }
    CString full;
    full.Format("%s%s", drive, path);
    m_Env.netBeginHeavy();
    FRESULT r = f_stat(full, &m_Info);
    const bool dir = r == FR_OK && (m_Info.fattrib & AM_DIR) != 0;
    if (r == FR_OK && !dir) r = f_unlink(full);
    m_Env.netEndHeavy();
    if (r == FR_NO_FILE || r == FR_NO_PATH) {
        SendText(404, "no such file\n");
    } else if (dir) {
        SendText(400, "this is a directory\n");
    } else if (r != FR_OK) {
        SendText(500, "cannot delete\n");
    } else {
        m_Logger.Write(FromHttp, LogNotice, "%s deleted", path);
        SendText(200, "deleted\n");
    }
}

void CHttpTask::PutFile(const char* path) {
    const char* drive = m_Env.netDrive();
    if (m_Req.contentLength < 0) {
        SendText(411, "Content-Length is required (use curl -T FILE URL)\n");
        return;
    }
    if (drive == nullptr) {
        SendText(503, "no boot drive\n");
        return;
    }
    if (strcmp(path, "/") == 0) {
        SendText(400, "no file name\n");
        return;
    }
    const char* refused = nullptr;
    if (!m_Env.netCanWrite(path, -1, refused)) { // 場所だけを先に見る (空きは、書き始める直前に見る)
        SendText(403, refused);
        return;
    }
    // 書く <パス>.part と、消す <パス>.old も、使っているイメージと重ならないか見る (書き始める前に)
    if (RefuseImage(path, true)) return;
    if (m_Env.netBusy()) {
        SendText(503, "the settings menu is open; try again later\n");
        return;
    }
    char full[300], part[310], old[310];
    CString f;
    f.Format("%s%s", drive, path);
    if (f.GetLength() + 6 >= sizeof full) {
        SendText(400, "path too long\n");
        return;
    }
    strcpy(full, f);
    platform::withSuffix(full, ".part", part, sizeof part);
    platform::withSuffix(full, ".old", old, sizeof old);
    // 空きを見て、受け取る大きさを書く予定として覚える。この関数から出るときに、書いていない残りを返す
    if (!m_Env.netCanWrite(path, m_Req.contentLength, refused)) {
        SendText(403, refused);
        return;
    }
    struct Planned {
        CHostNetEnv& env;
        platform::UploadPlan plan;
        ~Planned() { env.netWriteDone(plan.finish()); }
    } planned{m_Env, platform::UploadPlan((unsigned long long)m_Req.contentLength, m_Env.netClusterBytes())};
    // ディレクトリには書かない (/files/pix68k に送ると、/pix68k を /pix68k.old に改名してしまう)
    m_Env.netBeginHeavy();
    const bool dir = CUploadFs().isDirectory(full);
    FRESULT r = dir ? FR_OK : f_open(&m_File, part, FA_WRITE | FA_CREATE_ALWAYS);
    m_Env.netEndHeavy();
    if (dir) {
        SendText(400, "this is a directory; add the file name to the URL\n");
        return;
    }
    if (r != FR_OK) {
        CString msg;
        msg.Format("cannot create %s (FRESULT %d; does the directory exist?)\n", part, (int)r);
        SendText(r == FR_NO_PATH ? 404 : 500, msg);
        return;
    }
    if (m_Req.expectContinue) SendAll("HTTP/1.1 100 Continue\r\n\r\n", 25);
    // 受け取りながら BUF_SIZE ずつ書く。m_pBuf の先頭には頭と一緒に届いた分 (m_nLeft) がある。
    // Receive() は渡した長さを超える分を捨てるので、いつも FRAME_BUFFER_SIZE の空きを渡す (m_pBuf はその分大きい)
    const unsigned long long total = (unsigned long long)m_Req.contentLength;
    unsigned fill = m_nLeft > total ? (unsigned)total : m_nLeft;
    unsigned long long received = fill, written = 0;
    bool ok = true;
    const char* why = "";
    // 進みを見る。10 秒ごとに区切り、その間に kMinBytes 届かなければ切る (少しずつ送り続けて接続を占めないように。
    // QEMU の usb-net でも数十 KB/秒は届く)。SD カードに書いている間は受け取れないので、その時間は区切りを先に延ばす
    // (SD カードが遅いときに、送る側ではなくこちらの書き込みのせいで切らないように)
    static const unsigned kMinBytes = 4096;
    u64 windowEnd = CTimer::GetClockTicks64() + 10 * (u64)CLOCKHZ;
    unsigned long long windowStart = received;
    for (;;) {
        if (fill >= BUF_SIZE || (received == total && fill > 0)) {
            UINT done = 0;
            const u64 w0 = CTimer::GetClockTicks64();
            m_Env.netBeginHeavy();
            r = f_write(&m_File, m_pBuf, fill, &done);
            m_Env.netEndHeavy();
            windowEnd += CTimer::GetClockTicks64() - w0;
            if (r != FR_OK || done != fill) {
                ok = false;
                why = r != FR_OK ? "write failed" : "the drive is full";
                break;
            }
            written += fill;
            m_Env.netWriteDone(planned.plan.wrote(fill));
            fill = 0;
            m_nBytes = (long long)written;
            CScheduler::Get()->Yield(); // Receive() はデータが溜まっていれば譲らないので、ここで主ループに戻す
        }
        if (received == total) break;
        u64 now = CTimer::GetClockTicks64();
        if (now >= windowEnd) {
            if (received - windowStart < kMinBytes) {
                ok = false;
                why = "too slow (less than 4 KB in 10 s)";
                break;
            }
            windowEnd = now + 10 * (u64)CLOCKHZ;
            windowStart = received;
        }
        m_pConn->SetOptionReceiveTimeout((unsigned)((windowEnd - now) / (CLOCKHZ / 1000000)) + 1);
        const int n = m_pConn->Receive(m_pBuf + fill, FRAME_BUFFER_SIZE, 0);
        if (n == -NET_ERROR_WOULD_BLOCK) continue; // 区切りの時刻まで届かなかった。上で進みを見る
        if (n <= 0) {
            ok = false;
            why = n == -NET_ERROR_CONNECTION_RESET ? "connection closed before the end" : "receive error";
            break;
        }
        unsigned got = (unsigned)n;
        if (got > total - received) got = (unsigned)(total - received); // 後ろの余分は捨てる
        fill += got;
        received += got;
    }
    m_Env.netBeginHeavy();
    if (ok) r = f_sync(&m_File);
    const FRESULT rc = f_close(&m_File);
    if (ok && (r != FR_OK || rc != FR_OK)) {
        ok = false;
        why = "sync failed";
    }
    // 受け取っている間も主ループは動くので、その間にメニューを開いたり、メニューでこのファイルをつないだりしたかもしれない。
    // 置き換える直前にもう一度見る。ここから置き換え終わるまで Yield() しないので (NO_BUSY_WAIT を定義していないので、
    // SD カードと NVMe のドライバは待つ間も譲らない)、そのあいだに主ループがイメージをつなぎ替えることはない
    int status = 500;
    if (ok && m_Env.netBusy()) {
        ok = false, status = 503;
        why = "the settings menu was opened during the upload; try again later";
    } else if (ok && CUploadFs().isDirectory(full)) {
        // 受け取っている間に、同じ名前のディレクトリが作られた (commitUpload() も断るが、400 で答える)
        ok = false, status = 400;
        why = "a directory with this name was created during the upload";
    } else if (ok) {
        const platform::ImageCheck c = TouchesImage(path, true);
        if (c == platform::ImageCheck::IN_USE) {
            ok = false, status = 409;
            why = "the X68000 started using this disk image during the upload";
        } else if (c != platform::ImageCheck::FREE) {
            ok = false, status = 503;
            why = "cannot check whether the X68000 is using this file (SD card read error?)";
        }
    }
    platform::FsResult cr = platform::FsResult::ERROR;
    if (ok) {
        CUploadFs fs;
        cr = platform::commitUpload(fs, full, part, old);
    } else {
        // 受け取りを捨てる。ただし <パス>.part を X68000 が使っている (か決められない) なら消さない。
        // 設定メニューは .part をつながないので起きないはずだが、使っているファイルを消さないよう念のため見る
        char rel[300];
        const char* const parts[] = {rel};
        if (platform::withSuffix(path, ".part", rel, sizeof rel) &&
            m_Env.netImageInUse(parts, 1) == platform::ImageCheck::FREE) {
            f_unlink(part);
        } else {
            m_Logger.Write(FromHttp, LogWarning, "%s.part is in use (or cannot be checked); not deleted", path);
        }
    }
    m_Env.netEndHeavy();
    CString msg;
    if (!ok) {
        msg.Format("%s (%u of %u bytes); %s is unchanged\n", why, (unsigned)received, (unsigned)total, path);
        SendText(status, msg);
        m_Logger.Write(FromHttp, LogWarning, "%s: %s", path, (const char*)msg);
        return;
    }
    if (cr != platform::FsResult::OK) {
        msg.Format("cannot replace %s (the old file may be %s.old)\n", path, path);
        SendText(500, msg);
        m_Logger.Write(FromHttp, LogWarning, "%s", (const char*)msg);
        return;
    }
    msg.Format("%u bytes written to %s\n", (unsigned)written, path);
    m_Logger.Write(FromHttp, LogNotice, "%s replaced (%u bytes)", path, (unsigned)written);
    SendText(201, msg);
    m_nBytes = (long long)written;
}

void CHttpTask::GetLog(bool head) {
    const unsigned n = m_Env.netLog(m_pLog);
    m_nBytes = n;
    if (SendHead(200, "text/plain; charset=utf-8", n) && !head && n > 0) SendAll(m_pLog, n);
}

void CHttpTask::GetScreen(bool head, unsigned source) {
    const unsigned capacity = source ? 2048u * 1024u * 3u : kScreenBytes;
    if (m_ScreenCapacity < capacity) {
        u8* replacement = new u8[capacity];
        if (replacement) {
            delete[] m_pScreen; m_pScreen = replacement; m_ScreenCapacity = capacity;
        }
    }
    int w = 0, h = 0;
    const bool ok = m_pScreen && (source
        ? m_Env.netSnapshotTs6bga(m_pScreen, m_ScreenCapacity, w, h, source == 2)
        : m_Env.netSnapshotScreen(m_pScreen, m_ScreenCapacity, w, h));
    if (!ok) {
        SendText(503, "no screen yet\n");
        return;
    }
    char ppm[32];
    const int hn = platform::ppmHeader(ppm, sizeof ppm, w, h);
    const unsigned bytes = (unsigned)w * (unsigned)h * 3;
    m_nBytes = bytes;
    if (SendHead(200, "image/x-portable-pixmap", hn + bytes) && !head) {
        SendAll(ppm, (unsigned)hn);
        for (unsigned off = 0; off < bytes; off += BUF_SIZE) {
            if (!SendAll(m_pScreen + off, bytes - off > BUF_SIZE ? BUF_SIZE : bytes - off)) break;
            CScheduler::Get()->Yield();
        }
    }
}

// 本体を out に読む (max バイトまで)。長すぎれば false
bool CHttpTask::ReadBody(char* out, unsigned max, unsigned& n) {
    n = 0;
    if (m_Req.contentLength < 0) return true; // 本体なし
    if ((unsigned long long)m_Req.contentLength > max) return false;
    const unsigned total = (unsigned)m_Req.contentLength;
    n = m_nLeft > total ? total : m_nLeft;
    memcpy(out, m_pBuf, n);
    const u64 deadline = CTimer::GetClockTicks64() + 10 * (u64)CLOCKHZ; // 小さな本体は全体で 10 秒まで
    while (n < total) {
        unsigned got = 0;
        if (!ReceiveBefore(deadline, got)) return false;
        if (got > total - n) got = total - n;
        memcpy(out + n, m_pBuf, got);
        n += got;
    }
    return true;
}

bool CHttpTask::KeyState() { return m_Env.netKeyReport(m_KeyMods, m_Keys); }

// 本体の文字を 1 文字ずつ、押して 40ms、離して 40ms で打つ (X68000 のキーボードの受け取りに間に合う間隔)。
// 打つ間、/key で押したままのキーは離しておく
void CHttpTask::Type() {
    unsigned n = 0;
    if (!ReadBody(m_pType, MAX_TYPE, n)) {
        SendText(413, "the body is too long (4096 bytes at most)\n");
        return;
    }
    // 戻した文字列は元より長くならないので、同じバッファに書く
    char* text = m_pType;
    const unsigned len = (unsigned)platform::unescapeTyped(m_pType, n, text, MAX_TYPE + 1);
    memset(m_Keys, 0, sizeof m_Keys);
    m_KeyMods = 0;
    if (!KeyState()) {
        SendText(503, "the X68000 is not running\n");
        return;
    }
    unsigned typed = 0, skipped = 0;
    for (unsigned i = 0; i < len; i++) {
        u8 usage = 0;
        bool shift = false;
        if (!platform::asciiToUsage(text[i], usage, shift)) {
            skipped++;
            continue;
        }
        u8 keys[6] = {usage, 0, 0, 0, 0, 0};
        const u8 none[6] = {};
        m_Env.netKeyReport(shift ? platform::HID_MOD_LSHIFT : 0, keys);
        CScheduler::Get()->MsSleep(40);
        m_Env.netKeyReport(0, none);
        CScheduler::Get()->MsSleep(40);
        typed++;
    }
    m_nBytes = typed;
    CString msg;
    msg.Format("typed %u keys (%u characters skipped)\n", typed, skipped);
    SendText(200, msg);
}

static bool ParseHexByte(const char* s, u8& v) {
    unsigned x = 0;
    if (*s == 0) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    int digits = 0;
    for (; *s; s++) {
        const char c = *s;
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0 || ++digits > 2) return false;
        x = x * 16 + (unsigned)d;
    }
    v = (u8)x;
    return digits > 0;
}

void CHttpTask::Key() {
    char v[16];
    u8 usage = 0;
    bool changed = false;
    if (platform::queryValue(m_Req.query, "mod", v, sizeof v)) {
        if (!ParseHexByte(v, m_KeyMods)) {
            SendText(400, "mod= is a hex byte (01 LCtrl, 02 LShift, 04 LAlt, 08 LGUI, 10 RCtrl ...)\n");
            return;
        }
        changed = true;
    }
    if (platform::queryValue(m_Req.query, "down", v, sizeof v)) {
        if (!ParseHexByte(v, usage) || usage < 4) {
            SendText(400, "down= is a USB usage in hex (4c is Delete)\n");
            return;
        }
        bool held = false;
        for (u8 k : m_Keys) held = held || k == usage;
        for (u8& k : m_Keys)
            if (!held && k == 0) {
                k = usage;
                held = true;
            }
        if (!held) {
            SendText(409, "six keys are already held\n");
            return;
        }
        changed = true;
    }
    if (platform::queryValue(m_Req.query, "up", v, sizeof v)) {
        if (v[0] == 0) {
            memset(m_Keys, 0, sizeof m_Keys);
            m_KeyMods = 0;
        } else if (ParseHexByte(v, usage)) {
            for (u8& k : m_Keys)
                if (k == usage) k = 0;
        } else {
            SendText(400, "up= is a USB usage in hex, or empty to release all\n");
            return;
        }
        changed = true;
    }
    if (!changed) {
        SendText(400, "use /key?down=XX&mod=YY, /key?up=XX or /key?up\n");
        return;
    }
    if (!KeyState()) {
        SendText(503, "the X68000 is not running\n");
        return;
    }
    CString msg;
    msg.Format("mod=%02x keys=%02x %02x %02x %02x %02x %02x\n", m_KeyMods, m_Keys[0], m_Keys[1], m_Keys[2], m_Keys[3],
               m_Keys[4], m_Keys[5]);
    SendText(200, msg);
}

// 本体の文字列 (UTF-8。\n などのエスケープを使える) を画面の左上に重ねて出す (platform/osd_message.h)。
// ?sec=N で出す秒数 (既定 10、0 なら消すまで)。本体が空 (空白と改行だけ) なら消す。ログに 1 行残す
void CHttpTask::Message() {
    unsigned n = 0;
    if (!ReadBody(m_pType, platform::MSG_BODY_MAX, n)) {
        SendText(413, "the body is too long (1024 bytes at most)\n");
        return;
    }
    // 戻した文字列は元より長くならないので、同じバッファに書く
    const unsigned len = (unsigned)platform::unescapeTyped(m_pType, n, m_pType, MAX_TYPE + 1);
    platform::MessageRequest req;
    if (!platform::parseMessageRequest(m_Req.query, m_pType, len, req)) {
        SendText(400, req.error);
        return;
    }
    if (!m_Env.netMessage(m_pType, req.clear ? 0 : len, req.sec)) {
        SendText(503, "out of memory for the message\n");
        return;
    }
    char line[256];
    platform::formatMessageLog(line, sizeof line, m_pType, len, req.sec, req.clear);
    m_Logger.Write(FromHttp, LogNotice, "%s", line);
    m_nBytes = n;
    CString msg;
    if (req.clear) msg = "cleared\n";
    else if (req.sec == 0) msg = "shown until cleared\n";
    else msg.Format("shown for %u s\n", req.sec);
    SendText(200, msg);
}

// ---- CHostNet ----

CHostNet::~CHostNet() {}

bool CHostNet::Configure(const platform::Settings& s, CKernelOptions& options) {
    // cmdline.txt の pix68k_<キー> が pix68k.cfg の <キー> に勝つ
    auto get = [&](const char* opt, const char* cfg) { return options.GetAppOptionString(opt, cfg); };
    const char* net = get("pix68k_net", s.net ? "on" : "off");
    if (strcmp(net, "on") != 0) {
        if (strcmp(net, "off") != 0) m_Logger.Write(FromNet, LogWarning, "pix68k_net=%s: use on or off", net);
        return false;
    }
    const char* ip = get("pix68k_net_ip", s.netIp);
    if (ip[0] != 0) {
        int prefix = -1;
        if (!platform::parseIpv4Cidr(ip, m_Ip, prefix)) {
            m_Logger.Write(FromNet, LogWarning, "net_ip=%s: not an address like 192.168.1.50/24; using DHCP", ip);
        } else {
            m_bDhcp = false;
            platform::prefixToMask(prefix < 0 ? 24 : prefix, m_Mask);
            const char* gw = get("pix68k_net_gw", s.netGw);
            const char* dns = get("pix68k_net_dns", s.netDns);
            m_bGw = gw[0] != 0 && platform::parseIpv4(gw, m_Gw);
            m_bDns = dns[0] != 0 && platform::parseIpv4(dns, m_Dns);
            if (gw[0] != 0 && !m_bGw) m_Logger.Write(FromNet, LogWarning, "net_gw=%s: not an address; ignored", gw);
            if (dns[0] != 0 && !m_bDns) m_Logger.Write(FromNet, LogWarning, "net_dns=%s: not an address; ignored", dns);
        }
    }
    const char* name = get("pix68k_net_name", s.netName);
    if (name[0] != 0) platform::copyString(m_Name, sizeof m_Name, name);
    platform::copyString(m_Allow, sizeof m_Allow, get("pix68k_net_allow", s.netAllow));
    platform::copyString(m_Token, sizeof m_Token, get("pix68k_net_token", s.netToken));
    if (m_Allow[0] != 0 && !platform::allowListValid(m_Allow)) {
        // 読めない並びは誰も通さない (allowListHas())。書いた人が気づくように警告する
        m_Logger.Write(FromNet, LogWarning, "net_allow=%s: cannot read; nobody is allowed", m_Allow);
    }
    // RS-232C の TCP (rs232c=tcp:PORT)。リングは X68000 を動かす前に CKernel が作ってつないでいる
    m_pSerialLink = m_Env.netSerialLink();
    if (m_pSerialLink != nullptr) {
        unsigned port = 0;
        char addr[64];
        platform::parseRs232c(s.rs232c, port, addr, sizeof addr);
        if (addr[0] != 0) m_Logger.Write(FromNet, LogNotice, "rs232c=%s: the address is not used on the bare metal build", s.rs232c);
        m_SerialPort = (u16)port;
        if (m_SerialPort == 80) {
            m_Logger.Write(FromNet, LogWarning, "rs232c=%s: port 80 is used by HTTP; RS-232C is off", s.rs232c);
            m_pSerialLink = nullptr;
        }
    }
    const char* sl = get("pix68k_syslog", s.syslog);
    if (sl[0] != 0) {
        m_bSysLog = platform::parseIpv4Port(sl, m_SysLogIp, m_SysLogPort, 514);
        if (!m_bSysLog) m_Logger.Write(FromNet, LogWarning, "syslog=%s: not an address like 192.168.1.2:514", sl);
    }
    return true;
}

bool CHostNet::Start() {
    m_pScheduler = new CScheduler;
    if (m_bDhcp) {
        m_pNet = new CNetSubSystem(0, 0, 0, 0, m_Name);
    } else {
        m_pNet = new CNetSubSystem(m_Ip, m_Mask, m_bGw ? m_Gw : nullptr, m_bDns ? m_Dns : nullptr, m_Name);
    }
    // 待たない: USB の Ethernet は、つながったら CNetSubSystem のタスクが見つける
    if (m_pScheduler == nullptr || m_pNet == nullptr || !m_pNet->Initialize(FALSE)) {
        m_Logger.Write(FromNet, LogError, "cannot start TCP/IP (no Ethernet controller?)");
        return false;
    }
    m_StartTicks = CTimer::GetClockTicks64();
    if (m_bDhcp)
        m_Logger.Write(FromNet, LogNotice, "TCP/IP started (DHCP, host name %s)", m_Name);
    else
        m_Logger.Write(FromNet, LogNotice, "TCP/IP started (%u.%u.%u.%u/%u.%u.%u.%u)", m_Ip[0], m_Ip[1], m_Ip[2],
                       m_Ip[3], m_Mask[0], m_Mask[1], m_Mask[2], m_Mask[3]);
    if (m_Allow[0] == 0 && m_Token[0] == 0)
        m_Logger.Write(FromNet, LogWarning, "neither net_allow= nor net_token= is set; files cannot be written");
    return true;
}

// アドレスが決まったら、HTTP と syslog を始める
void CHostNet::CheckRunning() {
    if (m_bRunning || m_pNet == nullptr) return;
    if (!m_pNet->IsRunning()) {
        if (!m_bWaitLogged && CTimer::GetClockTicks64() - m_StartTicks > 10 * (u64)CLOCKHZ) {
            m_Logger.Write(FromNet, LogWarning, "no link or no DHCP answer yet (still trying)");
            m_bWaitLogged = true;
        }
        return;
    }
    m_bRunning = true;
    CString ip;
    m_pNet->GetConfig()->GetIPAddress()->Format(&ip);
    const unsigned ms = (unsigned)((CTimer::GetClockTicks64() - m_StartTicks) / (CLOCKHZ / 1000));
    m_Logger.Write(FromNet, LogNotice, "address %s (%u ms after start)", (const char*)ip, ms);
    if (m_bSysLog) {
        m_Logger.Write(FromNet, LogNotice, "syslog to %u.%u.%u.%u:%u", m_SysLogIp[0], m_SysLogIp[1], m_SysLogIp[2],
                       m_SysLogIp[3], m_SysLogPort);
        m_pSysLog = new CSysLogTask(m_pNet, m_Logger, m_Env, m_SysLogIp, m_SysLogPort, m_Name);
    } else {
        m_Logger.Write(FromNet, LogNotice, "syslog is off (syslog= in pix68k.cfg or pix68k_syslog= in cmdline.txt)");
    }
    m_pHttp = new CHttpTask(m_pNet, m_Logger, m_Env, m_Allow, m_Token, m_Name, m_pSysLog);
    if (m_pSerialLink != nullptr) m_pSerial = new CSerialTask(m_pNet, m_Logger, m_pSerialLink, m_SerialPort, m_Allow);
}

u32 CHostNet::IPv4() const {
    // m_bRunning は一度つながったら戻らないので、リンクが切れたときやアドレスが変わったときのために毎回見る
    // (IsRunning() はリンクと DHCP の状態を見るだけ。Pi 5 の MACB は PHY を読む)
    if (m_pNet == nullptr || !m_pNet->IsRunning()) return 0;
    const CIPAddress* ip = m_pNet->GetConfig()->GetIPAddress();
    if (ip == nullptr || ip->IsNull()) return 0;
    u8 a[4];
    ip->CopyTo(a);
    return (u32)a[0] << 24 | (u32)a[1] << 16 | (u32)a[2] << 8 | a[3];
}

void CHostNet::Service(unsigned waitUs) {
    const u64 end = CTimer::GetClockTicks64() + (u64)waitUs * (CLOCKHZ / 1000000);
    if (m_pScheduler == nullptr) {
        if (waitUs != 0) m_Env.netIdleUntil(end);
        return;
    }
    for (;;) {
        m_pScheduler->Yield();
        CheckRunning();
        const u64 now = CTimer::GetClockTicks64();
        if (now >= end) break;
        // 0.1ms ごとにタスクを見る。ネットワークのタスクを見る合間は wfe で眠らせず、空回りで待つ
        // (USB を探す間の UsbScanDelay() などほかの待ちは眠る)。
        // 前は、ここを CKernel::IdleUntil() (wfe) で待つと、QEMU の usb-net で DHCP のあとフレームを 1 つも受け取れ
        // なくなった (#117 の直しの前。2 回とも)。#117 の直し (QEMU の DWC2 が止めたチャネルでフレームを受け取ってしまう
        // のを避ける) のあとのビルドでは、ここを m_Env.netIdleUntil() にしても受け取れた (#136。Mac の QEMU 11.1 で 6 回。
        // ログに "waiting cores sleep (wfe)" が出て、6 回とも GET と 1MB の PUT / GET の要求には答えが返った。
        // 読み戻した中身は 5 回一致し、1 回は一致しなかった。その回は QEMU の時間切れで GET が途中で切れたと見ているが、
        // 確かめてはいない)。前に受け取れなかったのは #117 と同じことが起きていたためかもしれないが、確かめてはいない。
        // 実機でどう待つかは決めていないので、今は空回りのまま
        const u64 left = (end - now) / (CLOCKHZ / 1000000);
        CTimer::SimpleusDelay(left < 100 ? (unsigned)left + 1 : 100);
    }
}
