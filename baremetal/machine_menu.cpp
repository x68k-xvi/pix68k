// 設定メニュー (platform/menu.h) のベアメタル版の部分。
//
// 開き方: USB の割り込み (SendKeyReport()) がメニューのキーを見つけたら、押していたキーを X68000 に離したと送り、
// m_MenuActive を立てる。コア 0 の主ループ (ServiceMenu()) がコア 1 に一時停止を頼み、止まってから開く。
// 開いている間:
//   - キーは X68000 に送らず、割り込みが m_MenuKeyRing に入れ、主ループがメニューに渡す
//   - マウスとゲームパッドも X68000 に送らない (開くときに離したことにする)
//   - X68000 は止まっている (コア 1 はフレームの切れ目で、コア 2 / 3 が記録を処理し終えるのを待ってから止まる)。
//     コア 0 がディスクの入れ替え、リセット、ステートの保存と読み込みで Machine を触る
//   - 画面は、最後に受け取った X68000 の画面をフレームバッファに写し、その上にメニューを描く (コア 0)。
//     コア 2 の合成とフレームの受け渡し (RenderLink / VideoJournal) には手を入れないので、フレームは飛ばさない
// 閉じるときは、押したままのキーを一度離すまで X68000 に送らないようにしてから、コア 1 を再開する
#include <circle/bcmframebuffer.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <circle/util.h>

#include "kernel.h"
#include "platform/net_files.h"
#include "x68k/state.h"

static const char FromMenu[] = "menu";

// メニューのキーを入れる (USB の割り込み)。いっぱいなら捨てる
bool CKernel::PushMenuKey(u8 usage) {
    const unsigned head = m_MenuKeyHead;
    const unsigned next = (head + 1) % sizeof m_MenuKeyRing;
    if (next == m_MenuKeyTail) return false;
    m_MenuKeyRing[head] = usage;
    DataMemBarrier();
    m_MenuKeyHead = next;
    return true;
}

// メニューのキーを取り出す (コア 0 の主ループ)。なければ -1
int CKernel::PopMenuKey() {
    const unsigned tail = m_MenuKeyTail;
    if (tail == m_MenuKeyHead) return -1;
    DataMemBarrier();
    const u8 u = m_MenuKeyRing[tail];
    m_MenuKeyTail = (tail + 1) % sizeof m_MenuKeyRing;
    return u;
}

// コア 0 の主ループから毎回呼ぶ
void CKernel::ServiceMenu() {
    if (!m_MenuActive) return;
    if (!m_Menu.isOpen()) OpenMenu();
    bool redraw = false;
    for (int u; (u = PopMenuKey()) >= 0;) {
        m_MenuLastKey = (u8)u;
        m_Menu.key((u8)u);
        redraw = true;
        if (!m_Menu.isOpen()) break;
    }
    // ↑↓←→ を押し続けたら繰り返す
    if (m_Menu.isOpen()) {
        const u8 held = platform::MenuKeyRepeat::repeats(m_MenuLastKey) &&
                                (m_MenuKeys.isHeld(m_MenuLastKey) || m_PadMenu[0].holds(m_MenuLastKey) ||
                                 m_PadMenu[1].holds(m_MenuLastKey))
                            ? m_MenuLastKey
                            : 0;
        if (const u8 r = m_MenuRepeat.update(held, (uint32_t)(CTimer::GetClockTicks64() / (CLOCKHZ / 1000)))) {
            m_Menu.key(r);
            redraw = true;
        }
    }
    if (!m_Menu.isOpen()) {
        CloseMenu();
        return;
    }
    if (redraw) DrawMenu();
}

// メニューを閉じた (割り込みを止めて呼ぶ)。ゲームパッドで押したままのボタンと方向は、一度離すまで X68000 に送らない
void CKernel::ClosedPadMenu() {
    for (int s = 0; s < 2; s++) {
        m_PadMenu[s].closed();
        m_PadWant[s] = 0;
    }
}

void CKernel::OpenMenu() {
    Emulator* emu = m_pEmulator;
    // マウスのボタンとジョイスティックを離したことにする (押していたキーは、割り込みがもう離している)
    // 入力のキューが満杯で送れなかった分は、コア 1 が読んで空くまで送り直す。すべて届けてから止める
    // (止めたあとはコア 1 がキューを読まないので、送れなかった解放はステートに残ってしまう)
    const u64 t0 = CTimer::GetClockTicks64();
    EnterCritical();
    m_Mouse.releaseAll([emu](int x, int y, uint8_t b) { return emu->mouseEvent(x, y, b); });
    LeaveCritical();
    for (;;) {
        EnterCritical();
        m_Keys.resync([emu](uint8_t u, bool d) { return emu->keyEvent(u, d); });
        m_Mouse.flush([emu](int x, int y, uint8_t b) { return emu->mouseEvent(x, y, b); });
        for (int s = 0; s < 2; s++)
            if (m_PadSent[s] != 0 && emu->joystickEvent(s, 0)) m_PadSent[s] = 0;
        const bool done = !m_Keys.pending() && !m_Mouse.pending() && m_PadSent[0] == 0 && m_PadSent[1] == 0;
        LeaveCritical();
        if (done) break;
        if (CTimer::GetClockTicks64() - t0 > 2 * CLOCKHZ) {
            m_Logger.Write(FromMenu, LogWarning, "input queue stays full; menu not opened");
            EnterCritical();
            m_MenuKeys.closed(m_LastMods);
            ClosedPadMenu();
            m_MenuKeyHead = m_MenuKeyTail = 0;
            m_MenuActive = false;
            LeaveCritical();
            return;
        }
        asm volatile("sev");
    }
    // コア 1 がフレームの切れ目で止まるのを待つ (コア 2 / 3 が記録を処理し終えるまでかかることがある)
    emu->requestPause(true);
    while (!emu->paused()) {
        asm volatile("sev");
        if (CTimer::GetClockTicks64() - t0 > 5 * CLOCKHZ) {
            // 止まらない (コア 1 が動いていない) なら、メニューを開かない
            m_Logger.Write(FromMenu, LogWarning, "core 1 did not pause; menu not opened");
            emu->requestPause(false);
            EnterCritical();
            m_MenuKeys.closed(m_LastMods);
            ClosedPadMenu();
            m_MenuActive = false;
            LeaveCritical();
            return;
        }
    }
    m_Logger.Write(FromMenu, LogNotice, "opened (X68000 paused, %u ms)",
                   (unsigned)((CTimer::GetClockTicks64() - t0) / (CLOCKHZ / 1000)));
    // midi_out=uart なら、止めている間に実機の音源が鳴り続けないよう、鳴っている音を止める (送るのは待たない)
    SilenceMidiUart(0);
    m_Discard.clear();
    m_Menu.open(m_Settings, m_FileSettings);
    m_MenuLastKey = 0;
    m_MenuRepeat.update(0, 0);
    DrawMenu();
}

void CKernel::CloseMenu() {
    m_Logger.Write(FromMenu, LogNotice, "closed%s%s%s", m_Menu.status()[0] ? " (" : "", m_Menu.status(),
                   m_Menu.status()[0] ? ")" : "");
    // 断った POST /reboot?profile= が残っていて前回の選択に戻せなければ、再起動せずに閉じる (X68000 を動かし直す)
    if (m_Menu.restartRequested() && CancelProfileChoiceForRestart("menu Restart")) {
        // pix68k.cfg は書いてあり、ディスクも書き戻してある (restart())。SRAM を書いてから再起動する
        // (主ループが ShutdownReboot を返す)
        if (m_pEmulator->machine().sramDirty() || m_pEmulator->pendingSram() != nullptr)
            SaveSram(m_pEmulator->machine().sram());
        CloseDiskFiles();
        m_RebootRequested = true;
        return;
    }
    // メニューを消す (X68000 の画面の外にも描いたので、全体を描き直す)
    m_ShownWidth = -1;
    // X68000 が止まっていれば次のフレームは来ないので、効果の表は最後まで作る (ShowFrame())
    if (m_pLastFrame != nullptr) ShowFrame(*m_pLastFrame, m_pEmulator->paused());
    EnterCritical();
    m_MenuKeys.closed(m_LastMods);
    ClosedPadMenu();
    m_MenuKeyHead = m_MenuKeyTail = 0;
    m_MenuActive = false;
    LeaveCritical();
    // 止めていた間は数えない。画面の隅の数字は数え直し、数え終えるまで出さない (ホスト版も同じ)
    PublishOsd(platform::OsdValue());
    m_OsdTicks = 0;
    m_Discard.clear();
    // deploy の再起動を待たせている間は、X68000 を止めたまま (machine_run.cpp)
    if (m_DeployHeld) return;
    // コア 1 が動き出すまで待つ (すぐに開き直したときに、前の「止まった」を見て Machine を触らないように)
    m_pEmulator->requestPause(false);
    const u64 t0 = CTimer::GetClockTicks64();
    while (m_pEmulator->paused() && CTimer::GetClockTicks64() - t0 < CLOCKHZ) asm volatile("sev");
}

// 最後の X68000 の画面を写し直し、その上にメニューを描く
void CKernel::DrawMenu() {
    CBcmFrameBuffer* fb = m_Screen.GetFrameBuffer();
    if (fb == nullptr || fb->GetDepth() != 32) return;
    // X68000 が止まっていれば次のフレームは来ないので、効果の表は最後まで作る (ShowFrame())
    if (m_pLastFrame != nullptr) ShowFrame(*m_pLastFrame, m_pEmulator->paused());
    u32* base = (u32*)(uintptr)fb->GetBuffer();
    // メニューの枠は X68000 の画面の外にもはみ出すので、前に描いた枠が残らないよう外側も塗り直す
    // 枠を写しているときは、枠の外を枠の絵にする (platform/bezel.h)
    if (m_pLastFrame != nullptr && m_BezelShown != 0)
        platform::drawBezelOutside((u8*)base, fb->GetPitch(), m_Bezel[m_BezelShown - 1]);
    else if (m_pLastFrame != nullptr)
        platform::fillOutside((u8*)base, fb->GetPitch(), (int)m_Screen.GetWidth(), (int)m_Screen.GetHeight(),
                              m_ShownScale, 0xff000000u);
    m_OsdState.invalidate(); // 画面の隅の数字も消えた (閉じたあとに描き直す)
    m_MsgState.invalidate(); // 左上のメッセージも同じ
    // 日本語のファイル名は CGROM の字で描く
    const uint8_t* cg = m_pEmulator != nullptr ? m_pEmulator->cgRom() : nullptr;
    m_Menu.draw(base, (int)(fb->GetPitch() / 4), (int)m_Screen.GetWidth(), (int)m_Screen.GetHeight(), 0xff000000u, cg,
                cg != nullptr ? x68k::Machine::CG_SIZE : 0);
    // X68000 の画面の上にメニューを描いたので、次に写すときはすべての行を写す
    m_RowCache.valid = false;
}

// ---------------------------------------------------------------- MenuHost (X68000 は止まっている)

// /pix68k/ にあるイメージの一覧
void CKernel::listFiles(platform::MenuFileKind kind, platform::MenuFileList& list) {
    CString dir;
    dir.Format("%s/pix68k", m_BootDrive);
    DIR d;
    if (f_opendir(&d, dir) != FR_OK) return;
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] != 0) {
        if (fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
        // 置き換えの一時ファイル (ネットワークの口が書いている <名前>.part など) は出さない
        if (platform::isReplaceTempName(fi.fname)) continue;
        if (platform::MenuFileList::matches(fi.fname, kind) && !list.add(fi.fname)) break;
    }
    f_closedir(&d);
}

bool CKernel::insertDisk(int drive, const char* name, platform::MenuText& msg) {
    if (name[0] == 0) {
        // 書き戻せなければ取り出さない (書いた内容を捨てないように)。続けてもう一度取り出しを選んだら、書き戻さずに取り出す
        // (SD カードに空きがないときなどに抜けられるように)
        if (!EjectDisk(drive, m_Discard.confirmed())) {
            msg.add("cannot write back FD").add((long)drive).add("; eject again to discard");
            m_Discard.arm(platform::MENU_ACTION_EJECT + drive);
            return false;
        }
        m_Settings.fd[drive][0] = 0;
        m_Logger.Write(FromMenu, LogNotice, "fd%d: ejected", drive);
        return true;
    }
    // 置き換えの一時ファイル (.new、.part、.old) はつながない。ネットワークの口が書いている途中の <名前>.part を
    // X68000 が使い、受け取りを捨てるときに消してしまわないように (一覧にも出さないが、ここでも断る)
    if (platform::isReplaceTempName(name)) {
        msg.add("cannot use a temporary file: ").add(name);
        return false;
    }
    // 読めなければ、入っているディスクはそのまま (LoadDisk() は読めてから入れ替える)
    if (!LoadDisk(drive, name)) {
        // 読めないか、入っているディスクを書き戻せなかった (どちらかはログに出る)
        msg.add("cannot change to ").add(name);
        return false;
    }
    platform::copyString(m_Settings.fd[drive], sizeof m_Settings.fd[drive], name);
    return true;
}

void CKernel::resetMachine() {
    // リセットしてもイメージはメモリに残るので、書けなくても続ける (あとで書き直す)
    FlushDisks();
    m_pEmulator->machine().powerOn();
    m_Logger.Write(FromMenu, LogNotice, "reset");
}

static void StatePath(CString& path, const char* drive, int slot, const char* ext = "p68") {
    path.Format("%s/pix68k/state%d.%s", drive, slot, ext);
}

// 書くのに失敗しても前のステートが残るよう、state<N>.new に書き終えてから置き換える
bool CKernel::saveState(int slot, platform::MenuText& msg) {
    x68k::Machine& m = m_pEmulator->machine();
    // ステートにディスクの中身は入らないので、イメージのファイルを今の内容にしておく
    const bool flushed = FlushDisks();
    m_pEmulator->prepareSave();
    const size_t cap = x68k::stateSize(m);
    u8* buf = new u8[cap];
    size_t n = 0;
    CString path, tmp;
    StatePath(path, m_BootDrive, slot);
    StatePath(tmp, m_BootDrive, slot, "new");
    bool ok = buf != nullptr && x68k::saveState(m, buf, cap, n);
    // USB メモリでは、書いたあとも空きが残るときだけ書く (CKernel::CanWriteNewFile()。足りなければ FR_DENIED で、
    // 前のステートは残る)
    FRESULT r = ok ? WriteFileReplacing(path, tmp, buf, (unsigned)n) : FR_NOT_ENOUGH_CORE;
    delete[] buf;
    if (r != FR_OK) {
        m_Logger.Write(FromMenu, LogWarning, "%s: cannot save the state (FRESULT %d, %u bytes)", (const char*)path,
                       (int)r, (unsigned)cap);
        msg.add("cannot save state ").add((long)slot);
        return false;
    }
    m_Logger.Write(FromMenu, LogNotice, "state saved to %s (%u bytes)", (const char*)path, (unsigned)n);
    msg.add("state ").add((long)slot).add(" saved");
    if (!flushed) msg.add("; disks not written");
    return true;
}

bool CKernel::loadState(int slot, platform::MenuText& msg) {
    CString path, tmp;
    StatePath(path, m_BootDrive, slot);
    StatePath(tmp, m_BootDrive, slot, "new");
    unsigned size = 0;
    u8* data = nullptr;
    bool usedTmp = false;
    // 置き換えの途中で止まったときは .new だけが残っている (.p68 がないときだけ .new を読む)
    const FRESULT r = ReadWithFallback(path, tmp, data, size, usedTmp);
    if (usedTmp) path = tmp;
    if (r != FR_OK || data == nullptr) {
        if (r == FR_NO_FILE || r == FR_NO_PATH) msg.add("no state ").add((long)slot);
        else msg.add("cannot read state ").add((long)slot);
        delete[] data;
        return false;
    }
    const char* error = nullptr;
    const bool ok = x68k::loadState(m_pEmulator->machine(), data, size, error);
    delete[] data;
    if (!ok) {
        m_Logger.Write(FromMenu, LogWarning, "%s: not loaded: %s", (const char*)path, error ? error : "?");
        msg.add(error ? error : "cannot load state");
        return false;
    }
    m_pEmulator->resyncAfterLoad();
    m_Logger.Write(FromMenu, LogNotice, "state loaded from %s", (const char*)path);
    msg.add("state ").add((long)slot).add(" loaded");
    return true;
}

bool CKernel::saveSettings(const platform::Settings& s, platform::MenuText& msg) {
    const bool flushed = FlushDisks();
    if (!SaveConfig(s)) {
        msg.add("cannot write pix68k.cfg");
        return false;
    }
    if (!flushed) msg.add("settings saved; disks not written");
    return true;
}

bool CKernel::restart(const platform::Settings& s, platform::MenuText& msg) {
    // 再起動すると書き戻していない内容は消えるので、書けなければ再起動しない。続けてもう一度 Restart を選んだら、
    // 書き戻さずに再起動する (SD カードに空きがないときや、deploy の再起動を待たせているときに抜けられるように)
    if (!FlushDisks()) {
        if (!m_Discard.confirmed()) {
            msg.add("cannot write back; Restart again to discard");
            m_Discard.arm(platform::MENU_ACTION_RESTART);
            return false;
        }
        m_Logger.Write(FromMenu, LogWarning, "restarting without writing back disks");
    }
    if (!SaveConfig(s)) {
        msg.add("cannot write pix68k.cfg");
        return false;
    }
    msg.add("restarting");
    return true;
}
