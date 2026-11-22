// 起動の組み合わせ (pix68k-<名前>.cfg) のベアメタル版の部分 (#231。決まりは platform/boot_profile.h)。
//
// 流れ (ReadConfig() から ChooseProfile() を呼ぶ。X68000 を用意する前):
//   1. /pix68k の pix68k-*.cfg を一覧にする (ListProfiles())。1 つもなければ何もしない (今までどおり pix68k.cfg だけ)
//   2. /pix68k/profile.sel を読む。2 行目が once (POST /reboot?profile= で頼まれた) なら、メニューを出さずに
//      その組み合わせにし、once を消して書き直す
//   3. そうでなければ、HDMI にメニューを出す (RunBootMenu())。USB の装置は、ふだんは X68000 を動かし始めてから探すが、
//      メニューのキーボードとゲームパッドのため、ここで探す。メニューの間はログを画面に出さない
//   4. 前回と違うものを選んだら profile.sel を書く (SD カードに書くのは、選び直したときと HTTP で頼まれたときだけ)
// autotype= は、X68000 を動かし始めてから主ループで打つ (StartAutoType()、ServiceAutoType())。
#include <circle/bcmframebuffer.h>
#include <circle/startup.h>
#include <circle/string.h>
#include <circle/util.h>

#include "kernel.h"

static const char FromProfile[] = "profile";

static uint32_t NowMs() { return (uint32_t)(CTimer::GetClockTicks64() / (CLOCKHZ / 1000)); }

// 組み合わせのファイルがあるか (pix68k-<名前>.cfg か、保存の途中で止まって残った pix68k-<名前>.new。
// LoadConfigText() は .cfg がなければ .new を読む)。一覧の上限 (PROFILE_MAX) とは関係なく、ファイルを直接見る
bool CKernel::ProfileFileExists(const char* name) {
    for (int tmp = 0; tmp < 2; tmp++) {
        char file[64];
        if (!platform::profileFileName(name, file, sizeof file, tmp != 0)) return false;
        CString path;
        path.Format("%s/pix68k/%s", m_BootDrive, file);
        FILINFO fi;
        if (f_stat(path, &fi) == FR_OK && !(fi.fattrib & AM_DIR)) return true;
    }
    return false;
}

// /pix68k の pix68k-<名前>.cfg を list に入れる (題はファイルの 1 行目)。保存の途中で止まって
// pix68k-<名前>.new だけが残っている組み合わせも入れる (選べば .new を読み、次に保存したときに .cfg に戻る)
void CKernel::ListProfiles(platform::ProfileList& list) {
    CString dirPath;
    dirPath.Format("%s/pix68k", m_BootDrive);
    static const char* const kPatterns[] = {"pix68k-*.cfg", "pix68k-*.new"};
    bool full = false;
    for (const char* pattern : kPatterns) {
        DIR dir;
        FILINFO info;
        FRESULT r = f_findfirst(&dir, &info, dirPath, pattern);
        for (; !full && r == FR_OK && info.fname[0] != 0; r = f_findnext(&dir, &info)) {
            if (info.fattrib & AM_DIR) continue;
            char name[platform::PROFILE_NAME_MAX + 1];
            bool tmp = false;
            if (!platform::profileNameFromFile(info.fname, name, sizeof name, &tmp)) {
                m_Logger.Write(FromProfile, LogWarning, "%s: not used (the name is letters, digits, - and _, up to %d)",
                               info.fname, platform::PROFILE_NAME_MAX);
                continue;
            }
            CString path;
            path.Format("%s/%s", (const char*)dirPath, info.fname);
            if (tmp) {
                // .cfg もあれば .new は書きかけなので見ない
                char cfg[64];
                CString cfgPath;
                platform::profileFileName(name, cfg, sizeof cfg);
                cfgPath.Format("%s/%s", (const char*)dirPath, cfg);
                FILINFO fi;
                if (f_stat(cfgPath, &fi) == FR_OK) continue;
                m_Logger.Write(FromProfile, LogWarning, "%s: only the file being rewritten is left; using it",
                               (const char*)path);
            }
            // 題は 1 行目だけなので、頭の 512 バイトを読む
            char head[512], title[platform::PROFILE_TITLE_MAX + 1] = "";
            bool sjis = false;
            FIL f;
            if (f_open(&f, path, FA_READ) == FR_OK) {
                UINT n = 0;
                // ファイルが読んだ分より長ければ、1 行目が頭の終わりで切れていることがある
                if (f_read(&f, head, sizeof head, &n) == FR_OK)
                    platform::profileTitle(head, n, title, sizeof title, &sjis, f_size(&f) > n);
                f_close(&f);
            }
            if (!list.add(name, title, sjis)) {
                m_Logger.Write(FromProfile, LogWarning, "out of memory; profiles not listed");
                full = true;
            }
        }
        f_closedir(&dir);
    }
    if (list.dropped() != 0)
        m_Logger.Write(FromProfile, LogWarning, "more than %d profiles; %d not listed", platform::PROFILE_MAX,
                       list.dropped());
}

// /pix68k/profile.sel を書く (platform/boot_profile.h の formatProfileChoice())
bool CKernel::WriteProfileChoice(const char* name, bool once) {
    char data[platform::PROFILE_NAME_MAX + 16];
    const size_t n = platform::formatProfileChoice(name, once, data, sizeof data);
    if (n == 0) return false;
    MakeBootDir();
    CString path, tmp;
    path.Format("%s/pix68k/%s", m_BootDrive, platform::PROFILE_CHOICE_FILE);
    tmp.Format("%s/pix68k/%s", m_BootDrive, platform::PROFILE_CHOICE_TMP);
    const FRESULT r = WriteFileReplacing(path, tmp, data, (unsigned)n);
    if (r != FR_OK) {
        m_Logger.Write(FromProfile, LogWarning, "%s: write failed (FRESULT %d)", (const char*)path, (int)r);
        return false;
    }
    return true;
}

// 起動の組み合わせを決めて m_Profile に入れる (空なら pix68k.cfg だけ)。m_Settings は pix68k.cfg を読んだもの
void CKernel::ChooseProfile() {
    m_Profile[0] = 0;
    // 前回の選択
    char last[platform::PROFILE_NAME_MAX + 1] = "";
    bool once = false;
    {
        // 書き直しの途中で止まって profile.sel がなければ profile.new を読む
        CString path, tmp;
        path.Format("%s/pix68k/%s", m_BootDrive, platform::PROFILE_CHOICE_FILE);
        tmp.Format("%s/pix68k/%s", m_BootDrive, platform::PROFILE_CHOICE_TMP);
        u8* data = nullptr;
        unsigned size = 0;
        bool usedTmp = false;
        if (ReadWithFallback(path, tmp, data, size, usedTmp) == FR_OK &&
            !platform::parseProfileChoice(data != nullptr ? (const char*)data : "", size, last, sizeof last, once))
            m_Logger.Write(FromProfile, LogWarning, "%s: not a profile name; ignored", (const char*)path);
        delete[] data;
    }
    m_ProfileChoice.setSaved(last);
    // 前回の選択は、組み合わせが PROFILE_MAX を超えても一覧に残す
    platform::ProfileList list;
    list.keep(last);
    ListProfiles(list);
    // once を消せなければ (SD に書けない)、毎回 once の起動になってメニューが出なくなるので、この起動はメニューを出す
    if (once && !WriteProfileChoice(last, false)) {
        m_Logger.Write(FromProfile, LogWarning, "cannot clear the network request in %s; showing the menu",
                       platform::PROFILE_CHOICE_FILE);
        once = false;
    }
    if (once) {
        // POST /reboot?profile= で頼まれた起動。メニューを出さずにその組み合わせにする。
        // 一覧 (名前の順で PROFILE_MAX まで) に入っていなくても、ファイルがあれば使う (HTTP もファイルの有無で受けた)
        if (last[0] != 0 && !ProfileFileExists(last)) {
            m_Logger.Write(FromProfile, LogWarning, "pix68k-%s.cfg (requested over the network) not found; using pix68k.cfg",
                           last);
            return;
        }
        const int i = last[0] != 0 ? list.find(last) : -1;
        platform::copyString(m_Profile, sizeof m_Profile, i >= 0 ? list.at(i).name : last);
        m_Logger.Write(FromProfile, LogNotice, "%s (requested over the network)", m_Profile[0] ? m_Profile : "pix68k.cfg");
        return;
    }
    if (list.count() == 0) return;
    int initial = 0;
    if (last[0] != 0) {
        const int i = list.find(last);
        if (i >= 0) initial = i + 1;
        else m_Logger.Write(FromProfile, LogWarning, "last profile pix68k-%s.cfg not found; using pix68k.cfg", last);
    }
    m_Logger.Write(FromProfile, LogNotice, "%d profile(s); last: %s", list.count(),
                   initial ? list.at(initial - 1).name : "pix68k.cfg");
    const int row = m_Settings.profileWait > 0 ? RunBootMenu(list, initial) : initial;
    const char* chosen = row > 0 ? list.at(row - 1).name : "";
    platform::copyString(m_Profile, sizeof m_Profile, chosen);
    // 前回と違うときだけ書く (大文字と小文字だけの違いも書かない)
    if ((row != initial || (last[0] != 0 && initial == 0)) && WriteProfileChoice(chosen, false))
        m_ProfileChoice.setSaved(chosen);
}

// メニューを出して、選んだ行 (0 は pix68k.cfg、i + 1 は list の i 番目) を返す。
// Ctrl + Alt + Delete なら再起動する (X68000 は動いていないので、書き戻すものはない)
int CKernel::RunBootMenu(const platform::ProfileList& list, int initial) {
    LogBootTime(BootProfile, "boot profile menu");
    // メニューの間はログを画面に出さない (UART と /log には出る)。題の字のために CGROM を先に読む (StartMachine() が使う)
    m_Tee.EnableSecond(false);
    if (m_pBootCgrom == nullptr) m_pBootCgrom = ReadRom("CGROM.DAT", x68k::Machine::CG_SIZE);
    platform::BootMenu menu;
    menu.open(&list, initial, NowMs(), (uint32_t)m_Settings.profileWait * 1000);
    DrawBootMenu(menu);
    m_MenuKeyHead = m_MenuKeyTail = 0;
    memset(m_BootMenuHeld, 0, sizeof m_BootMenuHeld);
    m_BootPadBits[0] = m_BootPadBits[1] = 0;
    m_BootPadReports[0] = m_BootPadReports[1] = 0;
    m_BootMenuActive = true;
    // キーボードとゲームパッドのために USB の装置を探す (Pi 3B+ で 2 秒ほど。X68000 を動かし始めてから探すのと同じ
    // ScanUsb()。2 回目からは何もしない)。探している間に過ぎた分は待つ時間に足さないが、探し終えてから 2 秒は待つ
    ScanUsb(false);
    menu.holdUntil(NowMs() + 2000);
    // ゲームパッドは、つないで 0.3 秒以内の最初のレポートを押す前の状態にする (platform::PadStartFilter)
    platform::PadStartFilter pads[2] = {platform::PadStartFilter(CLOCKHZ * 3 / 10),
                                        platform::PadStartFilter(CLOCKHZ * 3 / 10)};
    for (auto& p : pads) p.start(CTimer::GetClockTicks64());
    int shownSeconds = -2, shownCursor = -1, row = -1;
    while (row < 0) {
        UpdateUsbDevices();
        KeepAlive();
        if (m_RebootRequest.take()) {
            m_Logger.Write(FromProfile, LogNotice, "restarting (Ctrl+Alt+Delete)");
            if (m_DeploySeconds != 0) FinishDeploy(false);
            reboot();
        }
        for (int u; row < 0 && (u = PopMenuKey()) >= 0;) row = menu.key((u8)u);
        for (int s = 0; s < 2 && row < 0; s++) {
            // レポートの数を先に読む (間に最初のレポートが来ても、次の周で基準にする)。つなぎ直すと
            // m_PadAttachTicks が変わり、数とビットは 0 に戻る (kernel.cpp の UpdateUsbDevices()、同じコア 0)
            const unsigned reports = m_BootPadReports[s];
            const uint8_t bits = m_BootPadBits[s];
            uint8_t keys[3];
            const int n = pads[s].keys(m_PadAttachTicks[s], reports, bits, CTimer::GetClockTicks64(), keys);
            for (int i = 0; i < n && row < 0; i++) row = menu.key(keys[i]);
        }
        const uint32_t now = NowMs();
        if (row < 0) row = menu.tick(now);
        if (row < 0 && (menu.secondsLeft(now) != shownSeconds || menu.cursor() != shownCursor)) {
            shownSeconds = menu.secondsLeft(now);
            shownCursor = menu.cursor();
            DrawBootMenu(menu);
        }
        if (row < 0) IdleUntil(CTimer::GetClockTicks64() + CLOCKHZ / 100);
    }
    m_Logger.Write(FromProfile, LogNotice, "chosen: %s%s", menu.rowName(row), menu.waiting() ? " (timeout)" : "");
    // 決めるのに押したキー (Enter) とボタンを離すまで待つ (X68000 に送らないように。長くても 2 秒)。
    // m_BootPadBits はメニューの間だけ更新されるので、待ち終わるまで m_BootMenuActive を下ろさない
    const u64 t0 = CTimer::GetClockTicks64();
    for (;;) {
        u8 mods, down[32];
        EnterCritical();
        m_KeyMerge.merged(mods, down);
        LeaveCritical();
        bool any = mods != 0 || m_BootPadBits[0] != 0 || m_BootPadBits[1] != 0;
        for (int i = 0; i < 32; i++) any = any || down[i] != 0;
        if (!any || CTimer::GetClockTicks64() - t0 > 2 * CLOCKHZ) break;
        UpdateUsbDevices();
        IdleUntil(CTimer::GetClockTicks64() + CLOCKHZ / 100);
    }
    m_BootMenuActive = false;
    m_MenuKeyHead = m_MenuKeyTail = 0;
    // メニューを消して、ログを画面に戻す
    m_Screen.Write("\x1b[H\x1b[J", 6);
    m_Tee.EnableSecond(true);
    return row;
}

void CKernel::DrawBootMenu(const platform::BootMenu& menu) {
    CBcmFrameBuffer* fb = m_Screen.GetFrameBuffer();
    if (fb == nullptr || fb->GetDepth() != 32) return;
    menu.draw((u32*)(uintptr)fb->GetBuffer(), (int)(fb->GetPitch() / 4), (int)m_Screen.GetWidth(),
              (int)m_Screen.GetHeight(), NowMs(), m_pBootCgrom, m_pBootCgrom ? x68k::Machine::CG_SIZE : 0, 0xff000000u);
}

// メニューの間のキーボード (USB の割り込みか、割り込みを止めたコア 0)。新しく押したキーをメニューに渡す
void CKernel::BootMenuKeys(u8, const u8 down[32]) {
    for (int b = 0; b < 32; b++) {
        const u8 pressed = (u8)(down[b] & ~m_BootMenuHeld[b]);
        for (int i = 0; i < 8; i++)
            if (pressed & (1u << i)) PushMenuKey((u8)(b * 8 + i));
        m_BootMenuHeld[b] = down[b];
    }
}

// 組み合わせで keyboard= か hat= が変わったら、メニューのためにもうつないだキーボードとゲームパッドに当て直す
void CKernel::ReapplyInputSettings(int keyboard, int hat) {
    if (!m_UsbStarted) return;
    if (m_Settings.keyboard != keyboard)
        for (int s = 0; s < MaxKeyboards; s++)
            if (m_pKeyboard[s] != nullptr) SetupKeyLayout(s, "keyboard= of the profile");
    if (m_Settings.hat != hat)
        for (int s = 0; s < 2; s++)
            if (m_pGamePad[s] != nullptr) m_PadHat[s].reset(HatBase(m_pGamePad[s], m_PadKnown[s]));
}

// POST /reboot?profile= (HTTP のタスク)。次の起動の組み合わせを、この場で profile.sel に once 付きで書く
// (書けなければ WRITE_FAILED を返し、HTTP は再起動せずに 500 を返す)。
// name が nullptr (profile= なしの POST /reboot) なら、前に頼まれて、ディスクを書き戻せずに再起動を断った組み合わせを
// 忘れる (profile.sel を前回の選択に書き戻す)
platform::ProfileRequest CKernel::netBootProfile(const char* name) {
    if (name == nullptr) {
        return CancelProfileChoice() ? platform::ProfileRequest::OK : platform::ProfileRequest::WRITE_FAILED;
    }
    if (m_BootDrive == nullptr) return platform::ProfileRequest::NO_DRIVE;
    if (name[0] != 0) {
        if (!platform::validProfileName(name)) return platform::ProfileRequest::BAD_NAME;
        if (!ProfileFileExists(name)) return platform::ProfileRequest::NOT_FOUND;
    }
    if (!RequestProfileChoice(name)) return platform::ProfileRequest::WRITE_FAILED;
    m_Logger.Write(FromProfile, LogNotice, "next boot: %s (requested over the network)",
                   name[0] ? name : "pix68k.cfg");
    return platform::ProfileRequest::OK;
}

// autotype= があれば、autotype_wait= 秒たったら打つ (X68000 を動かし始めたときに呼ぶ)
void CKernel::StartAutoType() {
    if (m_Settings.autotype[0] == 0) return;
    char text[platform::AutoTyper::MAX_TEXT];
    const size_t n = platform::unescapeTyped(m_Settings.autotype, strlen(m_Settings.autotype), text, sizeof text);
    m_AutoTyper.start(text, n, NowMs(), (uint32_t)m_Settings.autotypeWait * 1000);
    m_Logger.Write(FromProfile, LogNotice, "autotype: %u characters in %d s", (unsigned)n, m_Settings.autotypeWait);
}

// 主ループから呼ぶ。設定メニューを開いている間は打たない
void CKernel::ServiceAutoType() {
    if (!m_AutoTyper.active() || m_MenuActive) return;
    const uint32_t now = NowMs();
    u8 mods, usage;
    while (m_AutoTyper.poll(now, mods, usage)) {
        const u8 keys[6] = {usage, 0, 0, 0, 0, 0};
        if (!netKeyReport(mods, keys)) return;
        m_AutoTyper.ack(now);
    }
    if (!m_AutoTyper.active())
        m_Logger.Write(FromProfile, LogNotice, "autotype: typed %u keys (%u characters skipped)", m_AutoTyper.typed(),
                       m_AutoTyper.skipped());
}
