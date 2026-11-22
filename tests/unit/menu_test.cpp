// src/platform/menu.h のテスト: メニューの操作 (項目の移動、値の変更、ディスクの入れ替え、保存、再起動)、
// 開いている間と閉じたあとのキーの扱い (MenuKeyFilter)、小さな画面に描いても枠の外に書かないこと
#include "platform/menu.h"
#include "test_check.h"

#include <stdio.h>
#include <string.h>
#include "x68k/keyboard.h"

namespace {

using namespace platform;
namespace mk = platform::menu_key;

class FakeHost : public MenuHost {
public:
    char disk[2][96] = {"HUMAN302.XDF", ""};
    int inserts = 0, resets = 0, saves = 0, loads = 0, settingsSaves = 0, restarts = 0;
    int lastSlot = -1;
    Settings saved;
    bool cpu020 = true;
    // 書き戻せないことにする (機械の側と同じく、続けてもう一度選んだら書き戻さずに進める)
    bool cannotWrite = false;
    MenuDiscardConfirm discard;
    int discarded = 0;
    void beginAction(int action) override { discard.begin(action); }
    void listFiles(MenuFileKind kind, MenuFileList& list) override {
        static const char* const names[] = {"b.xdf", "A.XDF", "game.d88", "hd.hdf", "readme.txt", "C.DIM", "game.iso", "big.hds",
                                            "music.cue", "music.bin"};
        for (const char* n : names)
            if (MenuFileList::matches(n, kind)) list.add(n);
    }
    bool insertDisk(int drive, const char* name, MenuText& msg) override {
        if (!name[0] && cannotWrite) {
            if (!discard.confirmed()) {
                msg.add("cannot write back");
                discard.arm(MENU_ACTION_EJECT + drive);
                return false;
            }
            discarded++;
        }
        if (strcmp(name, "game.d88") == 0) {
            msg.add("cannot read game.d88");
            return false;
        }
        copyString(disk[drive], sizeof disk[drive], name);
        inserts++;
        return true;
    }
    const char* diskName(int drive) override { return disk[drive]; }
    // CD-ROM 装置: cdResult が 0 ならつないでいない、1 なら入れ替える、-1 なら読めない
    int cdResult = 0, cdChanges = 0;
    bool cdLocked = false; // X68000 が取り出しを禁止していた
    char cd[96] = "";
    int changeCd(const char* name, MenuText& msg) override {
        if (cdResult > 0 && !cdLocked) {
            copyString(cd, sizeof cd, name);
            cdChanges++;
        } else if (cdResult == -2) {
            cd[0] = 0; // 前の媒体を外したあとで失敗した
            menuCdEmptyText(msg, name);
        } else if (cdResult > 0 && cdLocked) {
            copyString(cd, sizeof cd, name);
            cdChanges++;
            menuCdChangedText(msg, name[0] != 0, true);
        } else if (cdResult < 0) {
            msg.add("cannot load ").add(name);
        }
        return cdResult;
    }
    void resetMachine() override { resets++; }
    bool saveState(int slot, MenuText&) override {
        saves++;
        lastSlot = slot;
        return true;
    }
    bool loadState(int slot, MenuText& msg) override {
        loads++;
        lastSlot = slot;
        msg.add("version mismatch");
        return false;
    }
    bool saveSettings(const Settings& s, MenuText&) override {
        saved = s;
        settingsSaves++;
        return true;
    }
    bool restart(const Settings& s, MenuText& msg) override {
        if (cannotWrite) {
            if (!discard.confirmed()) {
                msg.add("cannot write back");
                discard.arm(MENU_ACTION_RESTART);
                return false;
            }
            discarded++;
        }
        saved = s;
        restarts++;
        return true;
    }
    bool has020() const override { return cpu020; }
};

// label の項目までカーソルを動かす
bool moveTo(Menu& m, const char* label) {
    for (int i = 0; i < 32; i++) {
        if (strcmp(m.currentLabel(), label) == 0) return true;
        m.key(mk::DOWN);
    }
    printf("  no item %s\n", label);
    failures++;
    return false;
}

void testNavigate() {
    printf("navigate\n");
    FakeHost host;
    Menu m(host);
    Settings s;
    m.open(s, s);
    CHECK_EQ(m.isOpen(), 1);
    CHECK_EQ(strcmp(m.currentLabel(), "FD0"), 0);
    m.key(mk::UP); // 一番上から上で一番下へ
    CHECK_EQ(strcmp(m.currentLabel(), "Close"), 0);
    m.key(mk::DOWN);
    CHECK_EQ(strcmp(m.currentLabel(), "FD0"), 0);
    // 68000 ではクロックがあり、ハイメモリとローカルメモリはない
    moveTo(m, "Clock");
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().clockMHz, 10);
    moveTo(m, "CPU");
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().cpu, 68030);
    moveTo(m, "High memory");
    m.key(mk::RIGHT);
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().highMemMB, 2);
    moveTo(m, "CPU");
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().cpu, 68060);
    moveTo(m, "Local memory");
    m.key(mk::LEFT); // 既定の 128MB から 16MB ずつ
    CHECK_EQ(m.pending().localMemMB, 112);
    // 060turbo のクロックは 50MHz と 75MHz (68000 で選んだ 10 は 68060 では 50 と読む)
    CHECK_EQ(m.pending().effectiveClockMHz(), 50);
    moveTo(m, "Clock");
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().clockMHz, 75);
    CHECK_EQ(m.pending().effectiveClockMHz(), 75);
    m.key(mk::LEFT);
    CHECK_EQ(m.pending().clockMHz, 50);
    m.key(mk::RIGHT);
    moveTo(m, "CPU");
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().cpu, 68000);
    CHECK_EQ(m.pending().effectiveClockMHz(), 16); // 75 は 68000 では使わない (既定の 16MHz)
    moveTo(m, "RAM");
    for (int i = 0; i < 20; i++) m.key(mk::RIGHT);
    CHECK_EQ(m.pending().ramMB, 12);
    // Esc で閉じる
    m.key(mk::ESC);
    CHECK_EQ(m.isOpen(), 0);
    // 68030 / 68060 のないビルド (Pi 3) では CPU は 68000 のまま
    FakeHost h3;
    h3.cpu020 = false;
    Menu m3(h3);
    m3.open(s, s);
    moveTo(m3, "CPU");
    m3.key(mk::RIGHT);
    CHECK_EQ(m3.pending().cpu, 68000);
    // メニューのキーでも閉じる
    m3.key(mk::SCROLL_LOCK);
    CHECK_EQ(m3.isOpen(), 0);
}

void testDisks() {
    printf("disks\n");
    FakeHost host;
    Menu m(host);
    Settings s;
    m.open(s, s);
    CHECK_EQ(strcmp(m.pending().fd[0], "HUMAN302.XDF"), 0); // いま入っているもの
    // FD1 に A.XDF を入れる (一覧は名前の順: [Eject] A.XDF b.xdf C.DIM game.d88)
    moveTo(m, "FD1");
    m.key(mk::ENTER);
    CHECK_EQ(m.inFileList(), 1);
    m.key(mk::DOWN);
    m.key(mk::ENTER);
    CHECK_EQ(m.inFileList(), 0);
    CHECK_EQ(strcmp(host.disk[1], "A.XDF"), 0);
    CHECK_EQ(strcmp(m.pending().fd[1], "A.XDF"), 0);
    CHECK_EQ(strcmp(m.status(), "FD1: inserted"), 0);
    // 読めないイメージは入れ替えず、理由を出す
    m.key(mk::ENTER);
    CHECK_EQ(m.inFileList(), 1);
    for (int i = 0; i < 3; i++) m.key(mk::DOWN); // A.XDF (今のもの) から 3 つ下が game.d88
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(host.disk[1], "A.XDF"), 0);
    CHECK_EQ(strcmp(m.status(), "cannot read game.d88"), 0);
    // 取り出す
    m.key(mk::ENTER);
    m.key(mk::HOME);
    m.key(mk::ENTER);
    CHECK_EQ(host.disk[1][0], 0);
    CHECK_EQ(m.pending().fd[1][0], 0);
    // ESC で一覧から戻る (メニューは閉じない)
    m.key(mk::ENTER);
    m.key(mk::ESC);
    CHECK_EQ(m.inFileList(), 0);
    CHECK_EQ(m.isOpen(), 1);
    // HD0 は一覧から選ぶと、次の起動からになる
    moveTo(m, "HD0 (SASI)");
    m.key(mk::ENTER);
    m.key(mk::DOWN);
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.pending().hd0, "hd.hdf"), 0);
    CHECK_EQ(host.inserts, 2); // A.XDF を入れたのと取り出したの
    // CD-ROM も一覧から選び、次の起動からになる。68000 で外付け SCSI ボードがなければそう出す
    moveTo(m, "CD-ROM");
    m.key(mk::ENTER);
    m.key(mk::DOWN);
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.pending().cd, "game.iso"), 0);
    CHECK_EQ(strcmp(m.status(), "CD-ROM needs scsi_ex=on or X68030"), 0);
    CHECK_EQ(MenuFileList::matches("GAME.ISO", MenuFileKind::CDROM), 1);
    CHECK_EQ(MenuFileList::matches("GAME.ISO", MenuFileKind::HARD_DISK), 0);
    // CUE シートも一覧に出る (BIN は出ない)
    CHECK_EQ(MenuFileList::matches("MUSIC.CUE", MenuFileKind::CDROM), 1);
    CHECK_EQ(MenuFileList::matches("music.bin", MenuFileKind::CDROM), 0);
    // CD-ROM 装置をつないでいれば、媒体をすぐ入れ替える (次の起動を待たない)
    host.cdResult = 1;
    m.key(mk::ENTER); // 一覧は (none)、game.iso、music.cue。いま選んでいる game.iso にいる
    m.key(mk::DOWN);
    m.key(mk::ENTER);
    CHECK_EQ(host.cdChanges, 1);
    CHECK_EQ(strcmp(host.cd, "music.cue"), 0);
    CHECK_EQ(strcmp(m.pending().cd, "music.cue"), 0);
    CHECK_EQ(strcmp(m.status(), "CD-ROM: inserted"), 0);
    // 読めなければ、設定は変えない
    host.cdResult = -1;
    m.key(mk::ENTER);
    m.key(mk::UP);
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.pending().cd, "music.cue"), 0);
    CHECK_EQ(strcmp(m.status(), "cannot load game.iso"), 0);
    // 前の媒体を外したあとで失敗したら (装置は空)、設定も媒体なしにする
    host.cdResult = -2;
    m.key(mk::ENTER);
    m.key(mk::UP);
    m.key(mk::ENTER);
    CHECK_EQ(m.pending().cd[0], 0);
    CHECK_EQ(strcmp(m.status(), "drive is empty: cannot load game.iso"), 0);
    // 環境が実際に出す文 (ホスト版とベアメタル版で共通の関数) が、状態の行 (COLS 文字) に切れずに入る
    {
        char line[Menu::COLS + 1], full[200];
        for (int inserted = 0; inserted < 2; inserted++) {
            for (int locked = 0; locked < 2; locked++) {
                MenuText a(line, sizeof line), b(full, sizeof full);
                menuCdChangedText(a, inserted != 0, locked != 0);
                menuCdChangedText(b, inserted != 0, locked != 0);
                CHECK_EQ(strcmp(line, full), 0);
            }
        }
        // 装置が空になったときの文は、名前が長くて後ろが切れても、「装置は空」が残る
        MenuText a(line, sizeof line);
        menuCdEmptyText(a, "A_VERY_LONG_FILE_NAME_OF_A_CD_ROM_IMAGE_0123456789.CUE");
        CHECK_EQ(strlen(line), (size_t)Menu::COLS);
        CHECK_EQ(strncmp(line, "drive is empty: cannot load A_VERY_LONG", 39), 0);
    }
    // X68000 が取り出しを禁止していたときに入れ替えると、そのことが状態の行に出る (切れない)
    host.cdResult = 1;
    host.cdLocked = true;
    m.key(mk::ENTER); // 一覧の先頭 ((none)) にいる
    m.key(mk::DOWN);
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.pending().cd, "game.iso"), 0);
    CHECK_EQ(strcmp(m.status(), "CD-ROM: inserted (was locked by the X68000)"), 0);
    m.key(mk::ENTER);
    m.key(mk::UP);
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.status(), "CD-ROM: ejected (was locked by the X68000)"), 0);
    host.cdLocked = false;
    host.cdResult = 1;
    m.key(mk::ENTER); // 一覧の先頭 ((none)) にいる
    m.key(mk::DOWN);
    m.key(mk::DOWN);
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.pending().cd, "music.cue"), 0);
    // 取り出す
    m.key(mk::ENTER);
    m.key(mk::UP);
    m.key(mk::UP);
    m.key(mk::ENTER);
    CHECK_EQ(host.cdChanges, 5);
    CHECK_EQ(host.cd[0], 0);
    CHECK_EQ(m.pending().cd[0], 0);
    CHECK_EQ(strcmp(m.status(), "CD-ROM: ejected"), 0);
    // 後ろの試験のために、次の起動からの選び方で game.iso に戻す
    host.cdResult = 0;
    m.key(mk::ENTER);
    m.key(mk::DOWN);
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.pending().cd, "game.iso"), 0);
    // SCSI のハードディスク (SC0〜SC6) も一覧 (.hds) から選び、次の起動からになる
    CHECK_EQ(MenuFileList::matches("BIG.HDS", MenuFileKind::SCSI_DISK), 1);
    CHECK_EQ(MenuFileList::matches("hd.hdf", MenuFileKind::SCSI_DISK), 0);
    moveTo(m, "SC2");
    m.key(mk::ENTER);
    CHECK_EQ(m.inFileList(), 1);
    m.key(mk::DOWN);
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.pending().sc[2], "big.hds"), 0);
    CHECK_EQ(strcmp(m.status(), "SC2 needs scsi_ex=on or X68030"), 0);
    moveTo(m, "SCSI board");
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().scsiEx, 1);
    moveTo(m, "SC6"); // CD-ROM と同じ ID (既定の 6)
    m.key(mk::ENTER);
    m.key(mk::DOWN);
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.status(), "SC6 is used by the CD-ROM"), 0);
    moveTo(m, "SC2");
    m.key(mk::ENTER);
    m.key(mk::HOME);
    m.key(mk::ENTER);
    CHECK_EQ(m.pending().sc[2][0], 0);
    // X68030 には外付け SCSI ボードの項目はない
    moveTo(m, "CPU");
    m.key(mk::RIGHT);
    for (int i = 0; i < 30; i++) {
        CHECK_EQ(strcmp(m.currentLabel(), "SCSI board") != 0, 1);
        m.key(mk::DOWN);
    }
}

void testActions() {
    printf("actions\n");
    FakeHost host;
    Menu m(host);
    Settings running;
    m.open(running, running);
    moveTo(m, "State slot");
    m.key(mk::LEFT); // 0 → 9
    CHECK_EQ(m.stateSlot(), 9);
    moveTo(m, "Save state");
    m.key(mk::ENTER);
    CHECK_EQ(host.saves, 1);
    CHECK_EQ(host.lastSlot, 9);
    CHECK_EQ(strcmp(m.status(), "state saved"), 0);
    CHECK_EQ(m.isOpen(), 1);
    moveTo(m, "Load state");
    m.key(mk::ENTER);
    CHECK_EQ(host.loads, 1);
    CHECK_EQ(strcmp(m.status(), "version mismatch"), 0);
    CHECK_EQ(m.isOpen(), 1); // 読めなければ開いたまま
    // 設定を変えて保存する
    moveTo(m, "Clock");
    m.key(mk::RIGHT);
    moveTo(m, "Save settings");
    m.key(mk::ENTER);
    CHECK_EQ(host.settingsSaves, 1);
    CHECK_EQ(host.saved.clockMHz, 10);
    CHECK_EQ(strcmp(host.saved.fd[0], "HUMAN302.XDF"), 0);
    // 再起動を選ぶと閉じて、restartRequested() が true
    moveTo(m, "Restart");
    m.key(mk::ENTER);
    CHECK_EQ(host.restarts, 1);
    CHECK_EQ(m.isOpen(), 0);
    CHECK_EQ(m.restartRequested(), 1);
    // リセットで閉じる
    m.open(running, running);
    CHECK_EQ(m.restartRequested(), 0);
    moveTo(m, "Reset");
    m.key(mk::ENTER);
    CHECK_EQ(host.resets, 1);
    CHECK_EQ(m.isOpen(), 0);
}

void testKeyFilter() {
    printf("key filter\n");
    MenuKeyFilter f;
    uint8_t mods, out[6], ev[6];
    int n;
    const uint8_t a[6] = {0x04, 0, 0, 0, 0, 0};
    CHECK_EQ(f.filter(0x02, a, false, mods, out, ev, n), MenuKeyFilter::PASS);
    CHECK_EQ(out[0], 0x04);
    CHECK_EQ(mods, 0x02);
    // A を押したままメニューのキー: 開く (呼んだ側が A と SHIFT を離したことにする)
    const uint8_t aMenu[6] = {0x04, mk::SCROLL_LOCK, 0, 0, 0, 0};
    CHECK_EQ(f.filter(0x02, aMenu, false, mods, out, ev, n), MenuKeyFilter::OPEN);
    // 開いている間は、新しく押したキーだけをメニューに渡す
    const uint8_t down[6] = {0x04, mk::SCROLL_LOCK, mk::DOWN, 0, 0, 0};
    CHECK_EQ(f.filter(0, down, true, mods, out, ev, n), MenuKeyFilter::MENU);
    CHECK_EQ(n, 1);
    CHECK_EQ(ev[0], mk::DOWN);
    const uint8_t enter[6] = {mk::ENTER, 0, 0, 0, 0, 0};
    CHECK_EQ(f.filter(0, enter, true, mods, out, ev, n), MenuKeyFilter::MENU);
    CHECK_EQ(n, 1);
    CHECK_EQ(ev[0], mk::ENTER);
    // Enter で閉じた。押したままの Enter は X68000 に送らない
    f.closed(0);
    const uint8_t enterB[6] = {mk::ENTER, 0x05, 0, 0, 0, 0};
    CHECK_EQ(f.filter(0, enterB, false, mods, out, ev, n), MenuKeyFilter::PASS);
    CHECK_EQ(out[0], 0x05);
    CHECK_EQ(out[1], 0);
    // Enter を離して押し直せば送る
    const uint8_t none[6] = {0, 0, 0, 0, 0, 0};
    f.filter(0, none, false, mods, out, ev, n);
    CHECK_EQ(f.filter(0, enter, false, mods, out, ev, n), MenuKeyFilter::PASS);
    CHECK_EQ(out[0], mk::ENTER);
    // メニューのキーは X68000 のキーではない
    CHECK_EQ(x68k::Keyboard::fromHid(mk::SCROLL_LOCK), 0);
    CHECK_EQ(x68k::Keyboard::fromHid(mk::APPLICATION), 0);
    CHECK_EQ(x68k::Keyboard::fromHid(mk::MENU), 0);
    CHECK_EQ(isMenuKey(mk::F12), 0);
    CHECK_EQ(isMenuKey(mk::F12, true), 1);

    MenuKeyRepeat r;
    CHECK_EQ(r.update(mk::DOWN, 1000), 0);
    CHECK_EQ(r.update(mk::DOWN, 1399), 0);
    CHECK_EQ(r.update(mk::DOWN, 1400), mk::DOWN);
    CHECK_EQ(r.update(mk::DOWN, 1430), 0);
    CHECK_EQ(r.update(mk::DOWN, 1460), mk::DOWN);
    CHECK_EQ(r.update(0, 1500), 0);
}

// 画面の大きさごとに描き、枠の外 (番兵) を書き換えないことと、何か描いたことを見る
void testDraw() {
    printf("draw\n");
    FakeHost host;
    Menu m(host);
    Settings s;
    m.open(s, s);
    static const int sizes[][2] = {{768, 512}, {512, 512}, {256, 256}, {256, 240}, {64, 32}, {1920, 1080}};
    for (const auto& sz : sizes) {
        const int w = sz[0], h = sz[1], pitch = w + 8;
        static uint32_t buf[(1920 + 8) * 1082];
        for (size_t i = 0; i < sizeof buf / 4; i++) buf[i] = 0x12345678u;
        uint32_t* px = buf + pitch; // 上に 1 行の番兵
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) px[y * pitch + x] = 0;
        m.draw(px, pitch, w, h);
        int bad = 0, drawn = 0;
        for (int y = -1; y <= h; y++)
            for (int x = 0; x < pitch; x++) {
                const uint32_t v = px[y * pitch + x];
                const bool inside = y >= 0 && y < h && x < w;
                if (!inside && v != 0x12345678u) bad++;
                if (inside && v != 0) drawn++;
            }
        CHECK_EQ(bad, 0);
        CHECK_EQ(drawn > 0, 1);
    }
    // 一覧が長くても、選んでいる行が見える範囲を描く (落ちないこと)
    m.key(mk::ENTER);
    for (int i = 0; i < 4; i++) m.key(mk::DOWN);
    static uint32_t small[256 * 64];
    m.draw(small, 256, 256, 64);

    // 行の上限 (40) より多いイメージがあっても、選んでいる行 (青) を描く
    struct ManyHost : FakeHost {
        void listFiles(MenuFileKind kind, MenuFileList& list) override {
            if (kind != MenuFileKind::FLOPPY) return;
            for (int i = 0; i < 60; i++) {
                char name[16] = "DISK00.XDF";
                name[4] = (char)('0' + i / 10);
                name[5] = (char)('0' + i % 10);
                list.add(name);
            }
        }
    } many;
    Menu mm(many);
    mm.open(s, s);
    mm.key(mk::ENTER);
    static uint32_t big[768 * 1024];
    for (int k = 0; k < 3; k++) {
        if (k == 1) mm.key(mk::END);
        if (k == 2) for (int i = 0; i < 25; i++) mm.key(mk::UP); // 真ん中のあたり
        for (size_t i = 0; i < sizeof big / 4; i++) big[i] = 0;
        mm.draw(big, 768, 768, 1024);
        int sel = 0;
        for (size_t i = 0; i < sizeof big / 4; i++) sel += big[i] == 0x2050b0u;
        CHECK_EQ(sel > 0, 1);
    }
}

// 書き戻せずに断った操作は、ほかの操作をはさまずに続けて同じ操作を選んだときだけ、書き戻さずに進める
void testDiscardConfirm() {
    printf("discard confirmation\n");
    FakeHost host;
    host.cannotWrite = true;
    Menu m(host);
    Settings s;
    m.open(s, s);
    // Restart → 断る → Save settings → Restart: まだ断る (間に別の操作をした)
    moveTo(m, "Restart");
    m.key(mk::ENTER);
    CHECK_EQ(host.restarts, 0);
    CHECK_EQ(m.isOpen(), 1);
    moveTo(m, "Save settings");
    m.key(mk::ENTER);
    moveTo(m, "Restart");
    m.key(mk::ENTER);
    CHECK_EQ(host.restarts, 0);
    CHECK_EQ(host.discarded, 0);
    // 値を変えても忘れる
    moveTo(m, "RAM");
    m.key(mk::RIGHT);
    moveTo(m, "Restart");
    m.key(mk::ENTER);
    CHECK_EQ(host.restarts, 0);
    // 続けてもう一度 (カーソルを動かすのは操作に数えない)
    m.key(mk::UP);
    m.key(mk::DOWN);
    m.key(mk::ENTER);
    CHECK_EQ(host.restarts, 1);
    CHECK_EQ(host.discarded, 1);

    // FD の取り出し: 同じドライブで続けたときだけ。別のドライブの取り出しや、別の FD を入れるのをはさむと忘れる
    FakeHost h2;
    h2.cannotWrite = true;
    copyString(h2.disk[1], sizeof h2.disk[1], "A.XDF");
    Menu m2(h2);
    m2.open(s, s);
    auto eject = [&](const char* drive) {
        moveTo(m2, drive);
        m2.key(mk::ENTER);
        m2.key(mk::HOME);
        m2.key(mk::ENTER);
    };
    eject("FD0");
    CHECK_EQ(h2.disk[0][0] != 0, true);
    eject("FD1"); // 別のドライブ: FD1 も断る (FD0 の確認は忘れる)
    CHECK_EQ(h2.disk[1][0] != 0, true);
    eject("FD0");
    CHECK_EQ(h2.disk[0][0] != 0, true);
    eject("FD0"); // 続けて同じドライブ: 書き戻さずに取り出す
    CHECK_EQ(h2.disk[0][0], 0);
    CHECK_EQ(h2.discarded, 1);
    // メニューを閉じても忘れる (機械の側は CloseMenu() で clear() する。ここでは beginAction の並びだけを見る)
    eject("FD1");
    CHECK_EQ(h2.disk[1][0] != 0, true);
    moveTo(m2, "FD1");
    m2.key(mk::ENTER);
    m2.key(mk::DOWN); // A.XDF を入れ直す (別の操作)
    m2.key(mk::ENTER);
    eject("FD1");
    CHECK_EQ(h2.disk[1][0] != 0, true);
    CHECK_EQ(h2.discarded, 1);
}

} // namespace

// 画面の出し方 (Display のページ)。hasVideo() が true の環境だけに出す。Scale (video_scale=) と、効果 (video_filter=) の
// Scanlines、Curve、Mask。開いたときはいまの値を出し、変えたらすぐに知らせる。選べない組み合わせ (videoAllowed()) は選ばない
void testScale() {
    printf("video scale and filter\n");
    using namespace platform;
    // 選べない環境では、Display の行を出さない
    {
        FakeHost host;
        Menu m(host);
        Settings s;
        m.open(s, s);
        for (int i = 0; i < 30; i++) {
            CHECK_EQ(strcmp(m.currentLabel(), "Display") != 0, 1);
            CHECK_EQ(strcmp(m.currentLabel(), "Scale") != 0, 1);
            m.key(mk::DOWN);
        }
    }
    // pi3 なら curve と、full + mask を選べない (試験用の決まり)
    struct VideoHost : FakeHost {
        int scale = -1, filter = -1, calls = 0;
        bool pi3 = false;
        bool hasVideo() const override { return true; }
        bool videoAllowed(int sc, int f) const override {
            return !pi3 || ((f & CRT_CURVE) == 0 && !(sc == VIDEO_SCALE_FULL && (f & CRT_MASK)));
        }
        void videoChanged(int sc, int f) override {
            scale = sc;
            filter = f;
            calls++;
        }
    } vh;
    Menu ms(vh);
    Settings running, saved;
    running.videoScale = VIDEO_SCALE_OFF; // メニューで off にしたが、保存していない
    saved.videoScale = VIDEO_SCALE_INTEGER;
    ms.open(running, saved);
    char buf[64];
    auto openDisplay = [&](Menu& m) {
        if (!moveTo(m, "Display")) return false;
        m.key(mk::ENTER);
        return true;
    };
    if (moveTo(ms, "Display")) {
        CHECK_EQ(strcmp(ms.currentValue(buf, sizeof buf), "off"), 0);
        ms.key(mk::ENTER);
        static const char* const kRows[] = {"Scale", "Scanlines", "Curve", "Mask", "Back"};
        for (int i = 0; i < 5; i++) {
            CHECK_EQ(strcmp(ms.currentLabel(), kRows[i]), 0);
            ms.key(mk::DOWN);
        }
        CHECK_EQ(strcmp(ms.currentLabel(), "Scale"), 0); // 一周する
        CHECK_EQ(strcmp(ms.currentValue(buf, sizeof buf), "off"), 0);
        // off → integer → aspect → full → off。左と Enter
        static const char* const kScales[] = {"integer", "aspect", "full", "off"};
        for (int i = 0; i < 4; i++) {
            ms.key(mk::RIGHT);
            CHECK_EQ(ms.pending().videoScale, (i + 1) % 4);
            CHECK_EQ(vh.scale, (i + 1) % 4); // 変えたらすぐに環境に知らせる (描き直しを待たない)
            CHECK_EQ(vh.filter, 0);
            CHECK_EQ(strcmp(ms.currentValue(buf, sizeof buf), kScales[i]), 0);
        }
        ms.key(mk::LEFT);
        CHECK_EQ(ms.pending().videoScale, (int)VIDEO_SCALE_FULL);
        ms.key(mk::ENTER); // Enter は右と同じ
        CHECK_EQ(ms.pending().videoScale, (int)VIDEO_SCALE_OFF);
        ms.key(mk::RIGHT);
        CHECK_EQ(vh.calls, 7);
        // 効果は 1 つずつ on / off
        moveTo(ms, "Scanlines");
        ms.key(mk::RIGHT);
        CHECK_EQ(ms.pending().videoFilter, (int)CRT_SCANLINE);
        CHECK_EQ(vh.filter, (int)CRT_SCANLINE);
        CHECK_EQ(vh.scale, (int)VIDEO_SCALE_INTEGER);
        CHECK_EQ(strcmp(ms.currentValue(buf, sizeof buf), "on"), 0);
        moveTo(ms, "Curve");
        ms.key(mk::RIGHT);
        moveTo(ms, "Mask");
        ms.key(mk::ENTER);
        CHECK_EQ(vh.filter, (int)CRT_FULL);
        moveTo(ms, "Scanlines");
        ms.key(mk::LEFT);
        CHECK_EQ(vh.filter, (int)(CRT_CURVE | CRT_MASK));
        CHECK_EQ(vh.calls, 11);
        // Esc で主のページの Display に戻る。値はまとめて出す
        ms.key(mk::ESC);
        CHECK_EQ(ms.isOpen(), 1);
        CHECK_EQ(strcmp(ms.currentLabel(), "Display"), 0);
        CHECK_EQ(strcmp(ms.currentValue(buf, sizeof buf), "integer + curve,mask"), 0);
        // 切り替えてすぐに Esc で閉じても、閉じる前に知らせてある
        ms.key(mk::ENTER);
        ms.key(mk::RIGHT); // Scale を aspect
        ms.key(mk::ESC);
        CHECK_EQ(strcmp(ms.currentValue(buf, sizeof buf), "aspect + curve,mask"), 0);
        ms.key(mk::ESC);
        CHECK_EQ(ms.isOpen(), 0);
        CHECK_EQ(vh.scale, (int)VIDEO_SCALE_ASPECT);
        CHECK_EQ(vh.calls, 12);
    }
    // 開いたときは、いま動いている値を出す
    running.videoScale = VIDEO_SCALE_FULL;
    running.videoFilter = CRT_SCANLINE;
    ms.open(running, saved);
    if (moveTo(ms, "Display")) CHECK_EQ(strcmp(ms.currentValue(buf, sizeof buf), "full + scanline"), 0);
    // 選べない組み合わせ: Scale は飛ばし、効果は変えずに状態の行で知らせる
    {
        vh.pi3 = true;
        vh.calls = 0;
        running.videoScale = VIDEO_SCALE_ASPECT;
        running.videoFilter = CRT_MASK;
        ms.open(running, saved);
        if (openDisplay(ms)) {
            ms.key(mk::RIGHT); // aspect → full は full + mask が選べないので飛ばして off
            CHECK_EQ(ms.pending().videoScale, (int)VIDEO_SCALE_OFF);
            ms.key(mk::LEFT); // off → full も飛ばして aspect
            CHECK_EQ(ms.pending().videoScale, (int)VIDEO_SCALE_ASPECT);
            CHECK_EQ(vh.calls, 2);
            moveTo(ms, "Curve");
            ms.key(mk::RIGHT);
            CHECK_EQ(ms.pending().videoFilter, (int)CRT_MASK); // 変えない
            CHECK_EQ(strcmp(ms.status(), "not available on this machine"), 0);
            CHECK_EQ(strcmp(ms.currentValue(buf, sizeof buf), "off"), 0);
            CHECK_EQ(vh.calls, 2);
            moveTo(ms, "Mask");
            ms.key(mk::RIGHT);
            CHECK_EQ(ms.pending().videoFilter, (int)CRT_OFF);
            moveTo(ms, "Scale");
            ms.key(mk::RIGHT); // mask がなければ full を選べる
            CHECK_EQ(ms.pending().videoScale, (int)VIDEO_SCALE_FULL);
            moveTo(ms, "Mask");
            ms.key(mk::RIGHT); // full + mask は選べない
            CHECK_EQ(ms.pending().videoFilter, (int)CRT_OFF);
            CHECK_EQ(vh.calls, 4);
        }
        vh.pi3 = false;
    }
    // モニタの枠 (Bezel)。hasBezel() が true の環境だけに出す。off → cz-600de → cz-600db、効果のビットは変えない。
    // 効果を切り替えても枠は残る。選べない枠 (絵のファイルがない) は飛ばす
    {
        struct BezelHost : VideoHost {
            bool noDe = false, noDb = false; // 絵が読めない枠
            mutable int asked = 0;            // bezelAvailable() を呼んだ回数
            bool hasBezel() const override { return true; }
            bool bezelAvailable(int f) const override {
                asked++;
                return !(noDe && (f & CRT_BEZEL_DE)) && !(noDb && (f & CRT_BEZEL_DB));
            }
        } bh;
        Menu mb(bh);
        running = Settings();
        running.videoFilter = CRT_SCANLINE;
        mb.open(running, saved);
        if (openDisplay(mb)) {
            static const char* const kRows[] = {"Scale", "Scanlines", "Curve", "Mask", "Bezel", "Back"};
            for (int i = 0; i < 6; i++) {
                CHECK_EQ(strcmp(mb.currentLabel(), kRows[i]), 0);
                mb.key(mk::DOWN);
            }
            moveTo(mb, "Bezel");
            CHECK_EQ(strcmp(mb.currentValue(buf, sizeof buf), "off"), 0);
            mb.key(mk::RIGHT);
            CHECK_EQ(mb.pending().videoFilter, (int)(CRT_SCANLINE | CRT_BEZEL_DE));
            CHECK_EQ(bh.filter, (int)(CRT_SCANLINE | CRT_BEZEL_DE));
            CHECK_EQ(strcmp(mb.currentValue(buf, sizeof buf), "cz-600de"), 0);
            mb.key(mk::RIGHT);
            CHECK_EQ(mb.pending().videoFilter, (int)(CRT_SCANLINE | CRT_BEZEL_DB));
            mb.key(mk::RIGHT);
            CHECK_EQ(mb.pending().videoFilter, (int)CRT_SCANLINE);
            mb.key(mk::LEFT);
            CHECK_EQ(mb.pending().videoFilter, (int)(CRT_SCANLINE | CRT_BEZEL_DB));
            // 効果を切り替えても枠は残る
            moveTo(mb, "Curve");
            mb.key(mk::RIGHT);
            CHECK_EQ(mb.pending().videoFilter, (int)(CRT_GEOM | CRT_BEZEL_DB));
            // 選べない枠は飛ばす (cz-600de → off)
            bh.noDb = true;
            moveTo(mb, "Bezel");
            mb.key(mk::LEFT); // cz-600db → cz-600de
            CHECK_EQ(mb.pending().videoFilter, (int)(CRT_GEOM | CRT_BEZEL_DE));
            mb.key(mk::RIGHT); // cz-600de → (cz-600db は選べない) → off
            CHECK_EQ(mb.pending().videoFilter, (int)CRT_GEOM);
            // 選べる枠がほかにない: 変えずに知らせる
            bh.noDb = false;
            mb.key(mk::ESC);
            CHECK_EQ(strcmp(mb.currentValue(buf, sizeof buf), "integer + scanline,curve"), 0);
        }
        // 枠があれば、まとめた値は拡大の代わりに枠の名前 (枠だけなら名前だけ)
        running.videoFilter = CRT_FULL | CRT_BEZEL_DE;
        mb.open(running, saved);
        if (moveTo(mb, "Display"))
            CHECK_EQ(strcmp(mb.currentValue(buf, sizeof buf), "cz-600de + scanline,curve,mask"), 0);
        running.videoFilter = CRT_BEZEL_DB;
        mb.open(running, saved);
        if (moveTo(mb, "Display")) CHECK_EQ(strcmp(mb.currentValue(buf, sizeof buf), "cz-600db"), 0);
        // 設定に枠があって絵が読めない: 枠なしで写しているので、Scale と効果は変えられ、絵が読めるかは聞かない。
        // Display の行は拡大と効果だけ
        bh.noDe = true;
        bh.pi3 = true;
        running = Settings();
        running.videoFilter = CRT_SCANLINE | CRT_BEZEL_DE;
        mb.open(running, saved);
        if (moveTo(mb, "Display"))
            CHECK_EQ(strcmp(mb.currentValue(buf, sizeof buf), "integer + scanline"), 0);
        if (openDisplay(mb)) {
            bh.asked = 0;
            moveTo(mb, "Scale");
            mb.key(mk::RIGHT); // integer → aspect
            CHECK_EQ(mb.pending().videoScale, (int)VIDEO_SCALE_ASPECT);
            CHECK_EQ(bh.scale, (int)VIDEO_SCALE_ASPECT);
            moveTo(mb, "Mask");
            mb.key(mk::RIGHT);
            CHECK_EQ(mb.pending().videoFilter, (int)(CRT_SCANLINE | CRT_MASK | CRT_BEZEL_DE));
            CHECK_EQ(bh.filter, (int)(CRT_SCANLINE | CRT_MASK | CRT_BEZEL_DE));
            moveTo(mb, "Scanlines");
            mb.key(mk::RIGHT);
            CHECK_EQ(mb.pending().videoFilter, (int)(CRT_MASK | CRT_BEZEL_DE));
            CHECK_EQ(bh.asked, 0);
            // Pi 3 の制限は、枠があっても今までどおり (aspect → full は mask があるので飛ばす)
            moveTo(mb, "Scale");
            mb.key(mk::RIGHT);
            CHECK_EQ(mb.pending().videoScale != (int)VIDEO_SCALE_FULL, 1);
            // Bezel を替えるときだけ聞く: cz-600de (読めない) → cz-600db
            moveTo(mb, "Bezel");
            mb.key(mk::RIGHT);
            CHECK_EQ(bh.asked > 0, 1);
            CHECK_EQ(mb.pending().videoFilter, (int)(CRT_MASK | CRT_BEZEL_DB));
            mb.key(mk::ESC);
        }
        bh.noDe = false;
        bh.pi3 = false;
        // hasBezel() が false の環境 (ホスト版) では出さず、Display の行にも枠の名前を出さない (枠なしで写している)
        running = Settings();
        running.videoFilter = CRT_SCANLINE | CRT_BEZEL_DB;
        ms.open(running, saved);
        if (moveTo(ms, "Display")) CHECK_EQ(strcmp(ms.currentValue(buf, sizeof buf), "integer + scanline"), 0);
        ms.open(running, saved);
        if (openDisplay(ms))
            for (int i = 0; i < 6; i++) {
                CHECK_EQ(strcmp(ms.currentLabel(), "Bezel") != 0, 1);
                ms.key(mk::DOWN);
            }
    }
    // どちらもすぐに反映する項目なので、「*」(Restart で反映) は付けない。ホスト版でも使う
    static const char* const kVideoKeys[] = {"video_scale", "video_filter"};
    for (const char* key : kVideoKeys) {
        const SettingKey* k = findSettingKey(key);
        CHECK_EQ(k != nullptr && !k->restart && k->host, 1);
    }
    // 「Save settings」は Display の項目があっても、最後から 3 つ目のまま
    running = Settings();
    ms.open(running, saved);
    ms.key(mk::END);
    ms.key(mk::UP);
    ms.key(mk::UP);
    CHECK_EQ(strcmp(ms.currentLabel(), "Save settings"), 0);
}

// 画面の隅の数字 (osd=) の項目は、どちらの環境にも出し、開いたときはいまの値を出す。変えたらすぐに知らせる
void testOsd() {
    printf("osd\n");
    struct OsdHost : FakeHost {
        int osd = -1, calls = 0;
        void osdChanged(int v) override {
            osd = v;
            calls++;
        }
    } oh;
    Menu m(oh);
    Settings running, saved;
    running.osd = 1; // メニューで fps にしたが、保存していない
    saved.osd = 0;
    m.open(running, saved);
    if (moveTo(m, "OSD")) {
        CHECK_EQ(m.pending().osd, 1);
        char v[Menu::COLS + 1];
        CHECK_EQ(strcmp(m.currentValue(v, sizeof v), "fps"), 0);
        // off (0) → on (2) → fps (1) → off の順に変わる
        m.key(mk::RIGHT);
        CHECK_EQ(m.pending().osd, 0);
        CHECK_EQ(oh.osd, 0);
        CHECK_EQ(strcmp(m.currentValue(v, sizeof v), "off"), 0);
        m.key(mk::RIGHT);
        CHECK_EQ(oh.osd, 2);
        CHECK_EQ(strcmp(m.currentValue(v, sizeof v), "on"), 0);
        m.key(mk::RIGHT);
        CHECK_EQ(oh.osd, 1);
        m.key(mk::LEFT);
        CHECK_EQ(oh.osd, 2);
        m.key(mk::LEFT);
        CHECK_EQ(oh.osd, 0);
        // 切り替えてすぐに Esc で閉じても、閉じる前に知らせてある
        m.key(mk::LEFT);
        m.key(mk::ESC);
        CHECK_EQ(m.isOpen(), 0);
        CHECK_EQ(oh.osd, 1);
        CHECK_EQ(oh.calls, 6);
    }
    const SettingKey* k = findSettingKey("osd");
    CHECK_EQ(k != nullptr, 1);
    if (k) {
        CHECK_EQ(k->restart, 0);
        CHECK_EQ(k->host, 1);
    }
    m.open(running, saved);
    m.key(mk::END);
    m.key(mk::UP);
    m.key(mk::UP);
    CHECK_EQ(strcmp(m.currentLabel(), "Save settings"), 0);
}

// 拡張ボードのページに入る (主のページの「Boards」で Enter)
bool openBoards(Menu& m) {
    if (!moveTo(m, "Boards")) return false;
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.currentLabel(), "FPU board"), 0);
    return strcmp(m.currentLabel(), "FPU board") == 0;
}

// 拡張ボードの項目 (fpu_board、mercury、neptune、midi、midi_out)。主のページの「Boards」から入る別のページにある
void testBoards() {
    printf("boards\n");
    FakeHost host;
    Menu m(host);
    Settings s;
    m.open(s, s);
    // 主のページには出さない
    for (int i = 0; i < 40; i++) {
        CHECK_EQ(strcmp(m.currentLabel(), "FPU board") != 0 && strcmp(m.currentLabel(), "MIDI board") != 0, 1);
        m.key(mk::DOWN);
    }
    if (!openBoards(m)) return;
    // MIDI ボードをつながないときは、MIDI の行き先を出さない (FPU board、Mercury Unit、Neptune-X、MIDI board、Back)
    m.key(mk::UP);
    CHECK_EQ(strcmp(m.currentLabel(), "Back"), 0);
    m.key(mk::UP);
    CHECK_EQ(strcmp(m.currentLabel(), "MIDI board"), 0);
    m.key(mk::HOME);
    // on / off を切り替える
    static const char* const kBoards[] = {"FPU board", "Mercury Unit", "Neptune-X", "MIDI board"};
    for (const char* b : kBoards) {
        if (!moveTo(m, b)) return;
        m.key(mk::RIGHT);
    }
    CHECK_EQ(m.pending().fpuBoard, 1);
    CHECK_EQ(m.pending().mercury, 1);
    CHECK_EQ(m.pending().neptune, 1);
    CHECK_EQ(m.pending().midi, 1);
    // FPU board は off、on、2 (2 枚) の順に回る
    moveTo(m, "FPU board");
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().fpuBoard, 2);
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().fpuBoard, 0);
    m.key(mk::LEFT);
    CHECK_EQ(m.pending().fpuBoard, 2);
    m.key(mk::LEFT);
    CHECK_EQ(m.pending().fpuBoard, 1);
    moveTo(m, "Mercury Unit");
    m.key(mk::ENTER); // Enter でも切り替わる
    CHECK_EQ(m.pending().mercury, 0);
    m.key(mk::LEFT);
    CHECK_EQ(m.pending().mercury, 1);
    // MIDI の行き先は pix68k.cfg の選択肢の順 (sf2、uart、none、mt32、sc55) に回る
    if (!moveTo(m, "MIDI out")) return;
    CHECK_EQ(m.pending().midiOut, 0);
    // ファイル名を書いていない行き先では、要るキーを知らせる
    m.key(mk::LEFT);
    CHECK_EQ(m.pending().midiOut, 4);
    CHECK_EQ(strcmp(m.status(), "SC-55 needs midi_sc55= in pix68k.cfg"), 0);
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().midiOut, 0);
    CHECK_EQ(strcmp(m.status(), "SoundFont needs midi_sf2= in pix68k.cfg"), 0);
    m.key(mk::RIGHT); // uart は要るファイルがないので、前の案内を消す
    CHECK_EQ(m.pending().midiOut, 1);
    CHECK_EQ(m.status()[0], 0);
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().midiOut, 2);
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().midiOut, 3);
    CHECK_EQ(strcmp(m.status(), "MT-32 needs midi_mt32_ctrl= and midi_mt32_pcm="), 0);
    // Esc で主のページの「Boards」に戻る (メニューは閉じない)。案内は残る
    m.key(mk::ESC);
    CHECK_EQ(m.isOpen(), 1);
    CHECK_EQ(strcmp(m.currentLabel(), "Boards"), 0);
    CHECK_EQ(strcmp(m.status(), "MT-32 needs midi_mt32_ctrl= and midi_mt32_pcm="), 0);
    // 「Back」でも戻る
    m.key(mk::ENTER);
    m.key(mk::END);
    CHECK_EQ(strcmp(m.currentLabel(), "Back"), 0);
    m.key(mk::ENTER);
    CHECK_EQ(strcmp(m.currentLabel(), "Boards"), 0);
    // 68030 / 68060 でも出す
    moveTo(m, "CPU");
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().cpu, 68030);
    CHECK_EQ(openBoards(m), 1);
    CHECK_EQ(moveTo(m, "MIDI out"), 1);
    m.key(mk::BACKSPACE);
    CHECK_EQ(strcmp(m.currentLabel(), "Boards"), 0);
    // Save settings と Restart に値が渡る
    moveTo(m, "Save settings");
    m.key(mk::ENTER);
    CHECK_EQ(host.settingsSaves, 1);
    CHECK_EQ(host.saved.fpuBoard, 1);
    CHECK_EQ(host.saved.mercury, 1);
    CHECK_EQ(host.saved.neptune, 1);
    CHECK_EQ(host.saved.midi, 1);
    CHECK_EQ(host.saved.midiOut, 3);
    openBoards(m);
    moveTo(m, "MIDI board");
    m.key(mk::RIGHT);
    m.key(mk::ESC);
    moveTo(m, "Restart");
    m.key(mk::ENTER);
    CHECK_EQ(host.restarts, 1);
    CHECK_EQ(host.saved.midi, 0);
    CHECK_EQ(host.saved.fpuBoard, 1);
    CHECK_EQ(m.restartRequested(), 1);

    // 拡張ボードのページでも、メニューのキーはメニューを閉じる。次に開いたときは主のページ
    m.open(s, s);
    openBoards(m);
    m.key(mk::SCROLL_LOCK);
    CHECK_EQ(m.isOpen(), 0);
    m.open(s, s);
    CHECK_EQ(strcmp(m.currentLabel(), "FD0"), 0);
    // 主のページの Clock は上から 6 つ目、「Save settings」は最後から 3 つ目のまま (試験のキーの列が数えている)。
    // 「Boards」は OSD の 1 つ上 (ホスト版。tests/host/menu_test.sh が下から数える)
    for (int i = 0; i < 5; i++) m.key(mk::DOWN);
    CHECK_EQ(strcmp(m.currentLabel(), "Clock"), 0);
    m.key(mk::END);
    m.key(mk::UP);
    m.key(mk::UP);
    CHECK_EQ(strcmp(m.currentLabel(), "Save settings"), 0);
    for (int i = 0; i < 5; i++) m.key(mk::UP);
    CHECK_EQ(strcmp(m.currentLabel(), "OSD"), 0);
    m.key(mk::UP);
    CHECK_EQ(strcmp(m.currentLabel(), "Boards"), 0);

    // 案内が要らないときは、状態の行 (操作の説明や「settings saved」) を変えない
    Settings withSf2;
    platform::copyString(withSf2.midiSf2, sizeof withSf2.midiSf2, "gm.sf2");
    m.open(withSf2, withSf2);
    char before[Menu::COLS + 1];
    platform::copyString(before, sizeof before, m.status());
    CHECK_EQ(before[0] != 0, 1);
    openBoards(m);
    moveTo(m, "MIDI board");
    m.key(mk::RIGHT);
    CHECK_EQ(m.pending().midi, 1);
    CHECK_EQ(strcmp(m.status(), before), 0);
    moveTo(m, "MIDI out");
    m.key(mk::RIGHT); // uart
    m.key(mk::RIGHT); // none
    CHECK_EQ(m.pending().midiOut, 2);
    CHECK_EQ(strcmp(m.status(), before), 0);
    // MIDI board を on にしたあとも、主のページの OSD / Save settings に動ける
    m.key(mk::ESC);
    CHECK_EQ(moveTo(m, "OSD"), 1);
    CHECK_EQ(moveTo(m, "Save settings"), 1);
    m.key(mk::ENTER);
    CHECK_EQ(host.saved.midi, 1);
    CHECK_EQ(host.saved.midiOut, 2);
    // Scale のある環境 (ベアメタル版) でも、Boards の下は Display、OSD
    struct ScaleHost : FakeHost {
        bool hasVideo() const override { return true; }
    } sh;
    Menu ms(sh);
    ms.open(withSf2, withSf2);
    openBoards(ms);
    moveTo(ms, "MIDI board");
    ms.key(mk::RIGHT);
    ms.key(mk::ESC);
    ms.key(mk::DOWN);
    CHECK_EQ(strcmp(ms.currentLabel(), "Display"), 0);
    ms.key(mk::DOWN);
    CHECK_EQ(strcmp(ms.currentLabel(), "OSD"), 0);
    ms.key(mk::END);
    ms.key(mk::UP);
    ms.key(mk::UP);
    CHECK_EQ(strcmp(ms.currentLabel(), "Save settings"), 0);

    // この環境で選べない行き先 (Pi 3 の sc55、ホスト版の uart) は飛ばす。設定ファイルに書いてあった値は開いたときはそのまま
    struct NoHost : FakeHost {
        bool none = false;
        bool hasMidiOut(int out) const override { return !none && out != 1 && out != 4; }
    } nh;
    Menu mn(nh);
    Settings saved = withSf2;
    saved.midi = 1;
    saved.midiOut = 4;
    mn.open(saved, saved);
    if (!openBoards(mn) || !moveTo(mn, "MIDI out")) return;
    CHECK_EQ(mn.pending().midiOut, 4);
    mn.key(mk::RIGHT); // sc55 の次は sf2
    CHECK_EQ(mn.pending().midiOut, 0);
    mn.key(mk::RIGHT); // uart を飛ばして none
    CHECK_EQ(mn.pending().midiOut, 2);
    mn.key(mk::RIGHT);
    CHECK_EQ(mn.pending().midiOut, 3);
    mn.key(mk::RIGHT); // sc55 を飛ばして sf2
    CHECK_EQ(mn.pending().midiOut, 0);
    mn.key(mk::LEFT);
    CHECK_EQ(mn.pending().midiOut, 3);
    mn.key(mk::LEFT);
    mn.key(mk::LEFT);
    CHECK_EQ(mn.pending().midiOut, 0);
    // どれも選べなければ、値は変わらない
    nh.none = true;
    mn.key(mk::RIGHT);
    CHECK_EQ(mn.pending().midiOut, 0);
    mn.key(mk::LEFT);
    mn.key(mk::ENTER);
    CHECK_EQ(mn.pending().midiOut, 0);

    // どのキーも Restart で反映する項目 (動いている値と違えば「*」を付ける) で、ホスト版でも使う
    static const char* const kKeys[] = {"fpu_board", "mercury", "neptune", "midi", "midi_out"};
    for (const char* key : kKeys) {
        const SettingKey* k = findSettingKey(key);
        CHECK_EQ(k != nullptr && k->restart && k->host, 1);
    }
}

//FIT-BEGIN
// buf (w x h、下の画面は 0) に描いたメニューの中に、text を scale 倍、color で描いたものがあるか。
// メニューの中の地の色は、下の画面が 0 なら 0x101028 (draw() の暗くする式)
bool findText(const uint32_t* buf, int w, int h, const char* text, int scale, uint32_t color) {
    const int tw = (int)strlen(text) * 8 * scale, th = 16 * scale;
    if (tw > w || th > h) return false;
    static uint32_t ref[Menu::COLS * 8 * 3 * 16 * 3];
    for (int i = 0; i < tw * th; i++) ref[i] = 0x101028u;
    Menu::drawText(ref, tw, tw, th, 0, 0, text, Menu::COLS, scale, color);
    for (int y = 0; y + th <= h; y++)
        for (int x = 0; x + tw <= w; x++) {
            if (buf[y * w + x] != ref[0]) continue;
            bool same = true;
            for (int ty = 0; ty < th && same; ty++)
                same = memcmp(buf + (y + ty) * w + x, ref + ty * tw, (size_t)tw * 4) == 0;
            if (same) return true;
        }
    return false;
}

// buf (w x h、下の画面は 0) に描いたメニューの中に、升目 cells (n 桁) を scale 倍、color、cgrom で描いたものがあるか。
// findText() と同じく、1 行の左から右までが一致する所を探す
bool findCells(const uint32_t* buf, int w, int h, const uint16_t* cells, int n, int scale, uint32_t color,
               const uint8_t* cgrom, size_t cgromSize) {
    const int tw = n * 8 * scale, th = 16 * scale;
    if (tw > w || th > h) return false;
    static uint32_t ref[Menu::COLS * 8 * 3 * 16 * 3];
    for (int i = 0; i < tw * th; i++) ref[i] = 0x101028u;
    drawCells(ref, tw, tw, th, 0, 0, cells, n, scale, color, cgrom, cgromSize);
    for (int y = 0; y + th <= h; y++)
        for (int x = 0; x + tw <= w; x++) {
            bool same = true;
            for (int ty = 0; ty < th && same; ty++)
                same = memcmp(buf + (y + ty) * w + x, ref + ty * tw, (size_t)tw * 4) == 0;
            if (same) return true;
        }
    return false;
}

// 升目を作る小さな道具: ASCII の文字列、全角の区点 (count 字)、「…」を足す
struct Cells {
    uint16_t c[Menu::COLS + 2];
    int n = 0;
    Cells& ascii(const char* s) {
        while (*s) c[n++] = (uint8_t)*s++;
        return *this;
    }
    Cells& wide(uint16_t code, int count = 1) {
        for (int i = 0; i < count; i++) {
            c[n++] = code;
            c[n++] = MSG_CELL_RIGHT;
        }
        return *this;
    }
};

// 日本語のファイル名 (#275): UTF-8 の名前を切らずに行にし、全角と半角カタカナを CGROM の字で描く。桁があふれたら
// 最後を「…」にする。CGROM がなければ全角は豆腐、半角カタカナは「?」。NAME バイトに入らない名前は一覧に出さない
void testJapaneseNames() {
    printf("japanese names\n");
    // 「あ」20 字と .XDF (64 バイト。前は 1 行 48 バイトで切れていた。先頭の空白と合わせて 45 桁で入る)、
    // 「あ」25 字と .XDF (先頭の空白と合わせて 55 桁で、48 桁に入らない)、半角カタカナ、長すぎる名前 (100 バイト)
    static char kLong[128], kWide[80], kOver[96];
    kLong[0] = kWide[0] = kOver[0] = 0;
    for (int i = 0; i < 32; i++) strcat(kLong, "あ");
    strcat(kLong, ".XDF");
    for (int i = 0; i < 20; i++) strcat(kWide, "あ");
    strcat(kWide, ".XDF");
    for (int i = 0; i < 25; i++) strcat(kOver, "あ");
    strcat(kOver, ".XDF");
    static const char kKana[] = "\xef\xbd\xb8\xef\xbe\x9e\xef\xbe\x97.XDF"; // ｸﾞﾗ.XDF
    struct JaHost : FakeHost {
        void listFiles(MenuFileKind, MenuFileList& list) override {
            list.add(kWide);
            list.add(kOver);
            list.add(kKana);
            list.add(kLong);
        }
    } host;
    MenuFileList l;
    host.listFiles(MenuFileKind::FLOPPY, l);
    CHECK_EQ(l.count(), 3); // 長すぎる名前は出さない
    // 95 バイトの名前は出し、96 バイトの名前は出さない
    char n95[100], n96[100];
    memset(n95, 'a', 91);
    memcpy(n95 + 91, ".XDF", 5);
    memset(n96, 'a', 92);
    memcpy(n96 + 92, ".XDF", 5);
    CHECK_EQ(strlen(n95), 95u);
    MenuFileList edge;
    edge.add(n95);
    edge.add(n96);
    CHECK_EQ(edge.count(), 1);
    CHECK_EQ(strcmp(edge.name(0), n95), 0);
    // 日本語の名前は英数字の名前のあとに並ぶ
    MenuFileList order;
    order.add(kWide);
    order.add("ZZZ.XDF");
    CHECK_EQ(strcmp(order.name(0), "ZZZ.XDF"), 0);

    Menu m(host);
    Settings s;
    m.open(s, s);
    moveTo(m, "FD0");
    m.key(menu_key::ENTER);
    CHECK_EQ(m.inFileList(), 1);
    // 字形を見分けられるようにした CGROM:「あ」(0x2422) は縞、「…」(0x2144) は塗りつぶし、ｸ ﾞ ﾗ は別の縞
    static uint8_t cg[x68k::Machine::CG_SIZE];
    memset(cg, 0, sizeof cg);
    memset(cg + cgromFont16Offset(0x2422), 0xaa, 32);
    memset(cg + cgromFont16Offset(MSG_JIS_ELLIPSIS), 0xff, 32);
    static const int kKanaCodes[] = {0xb8, 0xde, 0xd7};
    for (int k : kKanaCodes) memset(cg + CGROM_FONT8X16 + k * 16, 0x55, 16);
    static uint32_t buf[768 * 512];
    memset(buf, 0, sizeof buf);
    m.draw(buf, 768, 768, 512, 0, cg, sizeof cg);
    const uint32_t white = 0xffffffu;
    // 「あ」20 字の行は、最後の .XDF まで 1 行に描かれている
    Cells wide;
    wide.ascii(" ").wide(0x2422, 20).ascii(".XDF");
    CHECK_EQ(findCells(buf, 768, 512, wide.c, wide.n, 1, white, cg, sizeof cg), 1);
    // 「あ」25 字の行は 48 桁に入らないので、「あ」22 字のあとを「…」にする (全角の途中で切らない)
    Cells over;
    over.ascii(" ").wide(0x2422, 22).wide(MSG_JIS_ELLIPSIS);
    CHECK_EQ(findCells(buf, 768, 512, over.c, over.n, 1, white, cg, sizeof cg), 1);
    // 半角カタカナの行
    Cells kana;
    kana.ascii(" ");
    kana.c[kana.n++] = 0xb8;
    kana.c[kana.n++] = 0xde;
    kana.c[kana.n++] = 0xd7;
    kana.ascii(".XDF");
    CHECK_EQ(findCells(buf, 768, 512, kana.c, kana.n, 1, white, cg, sizeof cg), 1);

    // CGROM がなければ、全角は豆腐 (「…」は点 3 つ)、半角カタカナは「?」
    memset(buf, 0, sizeof buf);
    m.draw(buf, 768, 768, 512, 0, nullptr, 0);
    Cells tofu;
    tofu.ascii(" ").wide(MSG_CELL_TOFU, 20).ascii(".XDF");
    CHECK_EQ(findCells(buf, 768, 512, tofu.c, tofu.n, 1, white, nullptr, 0), 1);
    Cells tofuOver;
    tofuOver.ascii(" ").wide(MSG_CELL_TOFU, 22).wide(MSG_JIS_ELLIPSIS);
    CHECK_EQ(findCells(buf, 768, 512, tofuOver.c, tofuOver.n, 1, white, nullptr, 0), 1);
    CHECK_EQ(findText(buf, 768, 512, " ???.XDF", 1, white), 1);

    // 選ぶと、切れていない名前で入れる (一覧は [Eject]、「あ」20 字、「あ」25 字、ｸﾞﾗ.XDF の順)
    m.key(menu_key::DOWN);
    m.key(menu_key::ENTER);
    CHECK_EQ(strcmp(host.disk[0], kWide), 0);
    m.key(menu_key::ENTER);
    m.key(menu_key::DOWN); // 今の「あ」20 字から 1 つ下
    m.key(menu_key::ENTER);
    CHECK_EQ(strcmp(host.disk[0], kOver), 0);
}

// 768x512 (ホスト版がメニューを描く大きさ) では、どの項目を選んでいても、下の「*」の説明と状態の行が描かれる。
// 1920x1080 では文字が 2 倍のまま。68000 / 68030 / 68060 を、Scale の項目のある環境 (ベアメタル版) とない環境
// (ホスト版) で見る。MIDI board を on にした場合 (前の版では MIDI out の行が増えた) も見る
void testFit() {
    printf("fit\n");
    static const char kStatus[] = "Arrows: select/change  Enter: OK  Esc: close";
    static const char kLegend[] = "* = applied after Restart";
    static const char kTitle[] = "pix68k menu  (X68000 is paused)";
    static uint32_t buf[1920 * 1080];
    struct FitHost : FakeHost {
        bool scale = false;
        bool hasVideo() const override { return scale; }
    } host;
    static const int kCpus[] = {68000, 68030, 68060};
    for (int cpu : kCpus)
        for (int withScale = 0; withScale < 2; withScale++)
            for (int midi = 0; midi < 2; midi++) {
                host.scale = withScale != 0;
                Menu m(host);
                Settings running, saved;
                running.cpu = saved.cpu = cpu;
                saved.ramMB = 8; // 動いている値と違う (「*」の説明が出る)
                saved.midi = midi;
                m.open(running, saved);
                // 一番下の項目 (Close) に行くまで、項目を 1 つずつ選んで描く
                int missingStatus = 0, missingLegend = 0, items = 0;
                for (int i = 0; i < 60; i++) {
                    memset(buf, 0, (size_t)768 * 512 * 4);
                    m.draw(buf, 768, 768, 512);
                    if (!findText(buf, 768, 512, kStatus, 1, 0xffffffu)) missingStatus++;
                    if (!findText(buf, 768, 512, kLegend, 1, 0xffffffu)) missingLegend++;
                    items++;
                    if (strcmp(m.currentLabel(), "Close") == 0) break;
                    m.key(mk::DOWN);
                }
                if (missingStatus || missingLegend)
                    printf("  cpu %d scale %d midi %d: %d items, status missing %d, legend missing %d\n", cpu, withScale,
                           midi, items, missingStatus, missingLegend);
                CHECK_EQ(missingStatus, 0);
                CHECK_EQ(missingLegend, 0);
                // 1920x1080 では 2 倍 (見出しは 0xffe080)。一番下と一番上の項目を選んで見る
                for (int k = 0; k < 2; k++) {
                    memset(buf, 0, sizeof buf);
                    m.draw(buf, 1920, 1920, 1080);
                    CHECK_EQ(findText(buf, 1920, 1080, kTitle, 2, 0xffe080u), 1);
                    CHECK_EQ(findText(buf, 1920, 1080, kStatus, 2, 0xffffffu), 1);
                    CHECK_EQ(findText(buf, 1920, 1080, kLegend, 2, 0xffffffu), 1);
                    m.key(mk::HOME);
                }
            }
}
//FIT-END

// 主のページの項目の数と、拡張ボードのページ。項目は 68000 のベアメタル版が一番多く 26 (768x512 に出せるのは 30 行で、
// 見出し、空行、「*」の説明、状態の 4 行を足すとちょうど入る)
void testFitPages() {
    printf("fit pages\n");
    static uint32_t buf[768 * 512];
    struct FitHost : FakeHost {
        bool scale = false;
        bool hasVideo() const override { return scale; }
    } host;
    static const int kCpus[] = {68000, 68030, 68060};
    for (int cpu : kCpus)
        for (int withScale = 0; withScale < 2; withScale++) {
            host.scale = withScale != 0;
            Menu m(host);
            Settings running, saved;
            running.cpu = saved.cpu = cpu;
            saved.midi = 1; // 拡張ボードのページの項目が一番多い (「*」の説明も出る)
            m.open(running, saved);
            m.key(mk::END);
            int items = 1;
            while (strcmp(m.currentLabel(), "FD0") != 0 && items < 60) {
                m.key(mk::UP);
                items++;
            }
            CHECK_EQ(items, (cpu == 68030 ? 24 : 25) + withScale); // クロックの項目は 68000 と 68060
            // 拡張ボードのページ: 6 項目。どれを選んでも、見出し、「*」の説明、状態が描かれる
            if (!openBoards(m)) continue;
            int boards = 0;
            for (int i = 0; i < 10; i++) {
                memset(buf, 0, sizeof buf);
                m.draw(buf, 768, 768, 512);
                CHECK_EQ(findText(buf, 768, 512, "Expansion boards  (Esc: back)", 1, 0xffe080u), 1);
                CHECK_EQ(findText(buf, 768, 512, "* = applied after Restart", 1, 0xffffffu), 1);
                // 拡張ボードのページでは Esc は戻るなので、操作の説明も「Esc: back」
                CHECK_EQ(findText(buf, 768, 512, Menu::HELP_BOARDS, 1, 0xffffffu), 1);
                CHECK_EQ(findText(buf, 768, 512, Menu::HELP, 1, 0xffffffu), 0);
                boards++;
                if (strcmp(m.currentLabel(), "Back") == 0) break;
                m.key(mk::DOWN);
            }
            CHECK_EQ(boards, 6);
        }
}

// 主のページの「Boards」の行: 値はつなぐボードの名前 (なければ (none))。ボードだけを変えても、「Boards」の行に「*」が
// 付き、下に「*」の説明が出る。68000 / 68030 / 68060 で見る
// 項目の行の文字列 (「*」か空白と名前を 16 文字に詰め、値を続ける。Menu::buildLines() と同じ)
const char* row(const char* label, const char* value) {
    static char b[Menu::COLS + 1];
    snprintf(b, sizeof b, "%-16s%s", label, value);
    return b;
}

void testBoardsMainRow() {
    printf("boards row on the main page\n");
    static uint32_t buf[768 * 512];
    static const int kCpus[] = {68000, 68030, 68060};
    for (int cpu : kCpus) {
        FakeHost host;
        Menu m(host);
        Settings s;
        s.cpu = cpu;
        m.open(s, s);
        if (!moveTo(m, "Boards")) return;
        memset(buf, 0, sizeof buf);
        m.draw(buf, 768, 768, 512);
        CHECK_EQ(findText(buf, 768, 512, "* = applied after Restart", 1, 0xffffffu), 0);
        m.key(mk::DOWN); // 選んでいる行は地の色が違うので、「Boards」から外して描く
        memset(buf, 0, sizeof buf);
        m.draw(buf, 768, 768, 512);
        CHECK_EQ(findText(buf, 768, 512, row(" Boards", "(none)"), 1, 0xffffffu), 1);
        m.key(mk::UP);
        // FPU board と MIDI board だけを on にする
        openBoards(m);
        moveTo(m, "FPU board");
        m.key(mk::RIGHT);
        moveTo(m, "MIDI board");
        m.key(mk::RIGHT);
        m.key(mk::ESC);
        CHECK_EQ(strcmp(m.currentLabel(), "Boards"), 0);
        m.key(mk::DOWN);
        memset(buf, 0, sizeof buf);
        m.draw(buf, 768, 768, 512);
        CHECK_EQ(findText(buf, 768, 512, row("*Boards", "FPU MIDI"), 1, 0xffffffu), 1);
        CHECK_EQ(findText(buf, 768, 512, "* = applied after Restart", 1, 0xffffffu), 1);
        // 4 つともつなぐと、名前を全部並べる
        m.key(mk::UP);
        openBoards(m);
        moveTo(m, "Mercury Unit");
        m.key(mk::RIGHT);
        moveTo(m, "Neptune-X");
        m.key(mk::RIGHT);
        m.key(mk::ESC);
        m.key(mk::DOWN);
        memset(buf, 0, sizeof buf);
        m.draw(buf, 768, 768, 512);
        CHECK_EQ(findText(buf, 768, 512, row("*Boards", "FPU Mercury Neptune MIDI"), 1, 0xffffffu), 1);
        // 元に戻すと「*」も説明も消える
        m.key(mk::UP);
        openBoards(m);
        static const char* const kAll[] = {"FPU board", "Mercury Unit", "Neptune-X", "MIDI board"};
        for (const char* b : kAll) {
            moveTo(m, b);
            m.key(mk::LEFT);
        }
        m.key(mk::ESC);
        m.key(mk::DOWN);
        memset(buf, 0, sizeof buf);
        m.draw(buf, 768, 768, 512);
        CHECK_EQ(findText(buf, 768, 512, row(" Boards", "(none)"), 1, 0xffffffu), 1);
        CHECK_EQ(findText(buf, 768, 512, "* = applied after Restart", 1, 0xffffffu), 0);
    }
}

// 高さ 480 のフレームバッファ (640x480、720x480) では主のページの行 (68000 のベアメタル版で 30 行) が入りきらない
// (28 行)。どの項目を選んでいても、選んでいる項目、「*」の説明、状態の行が描かれる
void testFit480() {
    printf("fit 480\n");
    static uint32_t buf[720 * 480];
    struct FitHost : FakeHost {
        bool hasVideo() const override { return true; }
    } host;
    static const int kWidths[] = {640, 720};
    static const int kCpus[] = {68000, 68030, 68060};
    for (int w : kWidths)
        for (int cpu : kCpus) {
            Menu m(host);
            Settings running, saved;
            running.cpu = saved.cpu = cpu;
            saved.ramMB = 8; // 「*」の説明を出す
            m.open(running, saved);
            int missing = 0, items = 0;
            for (int i = 0; i < 60; i++) {
                memset(buf, 0, sizeof buf);
                m.draw(buf, w, w, 480);
                if (!findText(buf, w, 480, Menu::HELP, 1, 0xffffffu)) missing++;
                if (!findText(buf, w, 480, "* = applied after Restart", 1, 0xffffffu)) missing++;
                items++;
                if (strcmp(m.currentLabel(), "Close") == 0) break;
                m.key(mk::DOWN);
            }
            // 一番下の項目 (Close) を選んでいても、選んでいる行が出る (地の色 0x2050b0 の行がある)
            bool sel = false;
            for (int i = 0; i < w * 480 && !sel; i++) sel = buf[i] == 0x2050b0u;
            if (missing) printf("  %dx480 cpu %d: %d items, %d missing\n", w, cpu, items, missing);
            CHECK_EQ(missing, 0);
            CHECK_EQ(sel, true);
            // 一番上 (FD0) に戻すと見出しも出る
            m.key(mk::HOME);
            memset(buf, 0, sizeof buf);
            m.draw(buf, w, w, 480);
            CHECK_EQ(findText(buf, w, 480, "pix68k menu  (X68000 is paused)", 1, 0xffe080u), 1);
            CHECK_EQ(findText(buf, w, 480, Menu::HELP, 1, 0xffffffu), 1);
        }
}

// 文字の倍率はページを移っても変わらない (主のページの行の数で決める)。1920x1080 では、主のページ (30 行前後) が 2 倍
// なので、行の少ない拡張ボードのページとファイルの一覧も 2 倍
void testScalePages() {
    printf("scale pages\n");
    static uint32_t buf[1920 * 1080];
    FakeHost host;
    Menu m(host);
    Settings s;
    m.open(s, s);
    memset(buf, 0, sizeof buf);
    m.draw(buf, 1920, 1920, 1080);
    CHECK_EQ(findText(buf, 1920, 1080, "pix68k menu  (X68000 is paused)", 2, 0xffe080u), 1);
    openBoards(m);
    memset(buf, 0, sizeof buf);
    m.draw(buf, 1920, 1920, 1080);
    CHECK_EQ(findText(buf, 1920, 1080, "Expansion boards  (Esc: back)", 2, 0xffe080u), 1);
    CHECK_EQ(findText(buf, 1920, 1080, Menu::HELP_BOARDS, 2, 0xffffffu), 1);
    CHECK_EQ(findText(buf, 1920, 1080, "Expansion boards  (Esc: back)", 3, 0xffe080u), 0);
    m.key(mk::ESC);
    // ファイルの一覧 (FD0)
    m.key(mk::HOME);
    CHECK_EQ(strcmp(m.currentLabel(), "FD0"), 0);
    m.key(mk::ENTER);
    CHECK_EQ(m.inFileList(), true);
    memset(buf, 0, sizeof buf);
    m.draw(buf, 1920, 1920, 1080);
    CHECK_EQ(findText(buf, 1920, 1080, "FD0: choose an image", 2, 0xffe080u), 1);
    CHECK_EQ(findText(buf, 1920, 1080, "FD0: choose an image", 3, 0xffe080u), 0);
}

int main() {
    testBoards();
    testBoardsMainRow();
    testFit480();
    testScalePages();
    testFit();
    testFitPages();
    testScale();
    testOsd();
    testDiscardConfirm();
    testNavigate();
    testDisks();
    testJapaneseNames();
    testActions();
    testKeyFilter();
    testDraw();
    return finish();
}
