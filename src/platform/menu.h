// 設定メニュー: X68000 の画面の上に重ねて出す。ベアメタル版とホスト版で同じものを使う。
//
// 開き方: メニューのキー (isMenuKey(): Scroll Lock、Application、Menu。Pi 400 はラズベリー + M、ホスト版は F12 も)。
// ゲームパッドの Home か Select + 上でも開き、メニューの中のボタンはこれらのキーに置き換えて渡す (platform/input_map.h の PadMenuInput)。
// どれも X68000 のキーに対応していない usage なので、X68000 のキーを潰さない。
// 開いている間、キーは X68000 に送らない (MenuKeyFilter)。開くときに押しているキーはすべて離したことにし、
// 閉じるときに押したままのキー (閉じるのに押した Enter など) は、一度離すまで X68000 に送らない。
// エミュレーションは止める (止め方はそれぞれの環境)。
//
// 操作: ↑↓ で項目を選ぶ、←→ で値を変える、Enter で決める、Esc (か BS) で戻る / 閉じる。
// 値を変えただけでは保存しない。「Save settings」で pix68k.cfg に書く。電源を入れ直さないと反映されない項目
// (SettingKey::restart) は、動いている値と違えば「*」を付け、「Restart」で反映する。
// 拡張ボードと画面の出し方は別のページにある (主のページの「Boards」「Display」で Enter。Esc か「Back」で戻る)。主のページの行を増やすと
// 768x512 に入りきらず、下の状態の行が出なくなるため。FPU board、Mercury Unit、Neptune-X、MIDI board は on / off を
// 切り替える。MIDI の行き先 (MIDI out) は、MIDI ボードをつないだときだけ出し、この環境で選べないもの (MenuHost::hasMidiOut()) は飛ばす。
// 音源の ROM や SoundFont のファイル名はメニューでは変えない (pix68k.cfg に書く)。
// ディスクの入れ替えと取り出し、リセット、ステートの保存と読み込みは、選んだときに MenuHost に頼む。
// 画面の出し方は別のページにある (主のページの「Display」で Enter。値は「aspect + scanline,curve」のようにまとめて出す)。
// Scale (video_scale=。off、integer、aspect、full) と、ブラウン管のように見せる効果 (video_filter=。platform/crt_effect.h) の
// Scanlines、Curve、Mask (それぞれ on / off) を切り替える。この環境で選べない組み合わせ (MenuHost::videoAllowed()。
// Pi 3 の curve と、full と mask の組) は選ばない (Scale は飛ばし、効果は変えずに状態の行で知らせる)。
// モニタの枠 (Bezel。video_filter= の cz-600de / cz-600db。platform/bezel.h) は、off → cz-600de → cz-600db の順に替える
// (MenuHost::hasBezel() が true のときだけ出す。選べない枠 (絵のファイルがないなど) は飛ばす)。
// 変えたときは MenuHost::videoChanged() を呼び、すぐに反映する。
// 画面の隅の数字 (OSD) も、変えたときに MenuHost::osdChanged() を呼び、すぐに反映する。
//
// 文字は UTF-8 で、ASCII を platform/menu_font.h の 8x16、半角カタカナと全角 (日本語のファイル名) を CGROM の字で描く
// (platform/text_cells.h)。描く先は XRGB8888 の画素の並び (draw())。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/menu_font.h"
#include "platform/settings.h"
#include "platform/text_cells.h"

namespace platform {

// ---------------------------------------------------------------- キー (USB HID の usage)

namespace menu_key {
static constexpr uint8_t ENTER = 0x28, ESC = 0x29, BACKSPACE = 0x2a, RIGHT = 0x4f, LEFT = 0x50, DOWN = 0x51, UP = 0x52;
static constexpr uint8_t PAGE_UP = 0x4b, PAGE_DOWN = 0x4e, HOME = 0x4a, END = 0x4d, KP_ENTER = 0x58;
static constexpr uint8_t F12 = 0x45, SCROLL_LOCK = 0x47, APPLICATION = 0x65, MENU = 0x76;
} // namespace menu_key

// メニューを開く / 閉じるキーか。F12 (X68000 の XF2) は、f12 が true (ホスト版) のときだけ
inline bool isMenuKey(uint8_t usage, bool f12 = false) {
    return usage == menu_key::SCROLL_LOCK || usage == menu_key::APPLICATION || usage == menu_key::MENU ||
           (f12 && usage == menu_key::F12);
}

// USB キーボードのレポートを、X68000 に送る分とメニューに渡す分に分ける (ベアメタル版。USB の割り込みから呼ぶ)
class MenuKeyFilter {
public:
    enum Result {
        PASS, // X68000 に送る (out のレポート)
        OPEN, // メニューのキーが押された。押しているキーをすべて離してからメニューを開く
        MENU, // メニューが開いている。events の usage (新しく押したキー) をメニューに渡す
    };
    // menuOpen: いまメニューが開いているか。events は 6 個分
    Result filter(uint8_t mods, const uint8_t keys[6], bool menuOpen, uint8_t& outMods, uint8_t outKeys[6],
                  uint8_t events[6], int& nEvents) {
        nEvents = 0;
        bool errorRollOver = false;
        for (int i = 0; i < 6; i++) errorRollOver = errorRollOver || (keys[i] >= 1 && keys[i] <= 3);
        uint8_t down[32] = {};
        if (!errorRollOver)
            for (int i = 0; i < 6; i++)
                if (keys[i]) down[keys[i] >> 3] |= (uint8_t)(1u << (keys[i] & 7));
        // 新しく押したキー
        bool menuPressed = false;
        if (!errorRollOver) {
            for (int i = 0; i < 6; i++) {
                const uint8_t u = keys[i];
                if (u == 0 || get(held_, u)) continue;
                if (isMenuKey(u)) menuPressed = true;
                if (nEvents < 6) events[nEvents++] = u;
            }
            memcpy(held_, down, sizeof held_);
            // 離したキーは隠すのをやめる
            for (int b = 0; b < 32; b++) masked_[b] &= down[b];
            maskedMods_ &= mods;
        }
        if (menuOpen) return MENU;
        if (menuPressed) {
            nEvents = 0;
            return OPEN;
        }
        nEvents = 0;
        // 閉じたときに押したままだったキーを除いて送る
        outMods = (uint8_t)(mods & ~maskedMods_);
        int n = 0;
        for (int i = 0; i < 6; i++) outKeys[i] = 0;
        for (int i = 0; i < 6; i++) {
            const uint8_t u = keys[i];
            if (u == 0) continue;
            if (!errorRollOver && (get(masked_, u) || isMenuKey(u))) continue;
            outKeys[n++] = u;
        }
        return PASS;
    }
    // filter() と同じことを、押しているキーを usage ごとの 1 ビット (32 バイト) で受け取って行う (何台かのキーボードを
    // 合わせたあと。platform/key_merge.h)。ErrorRollOver などは来ない (合わせる前に除いてある)。
    // out は X68000 に送る分 (PASS のとき)。events は 6 個分で、新しく押したキーが 7 個以上なら 6 個まで
    Result filterBits(uint8_t mods, const uint8_t down[32], bool menuOpen, uint8_t& outMods, uint8_t outDown[32],
                      uint8_t events[6], int& nEvents) {
        nEvents = 0;
        bool menuPressed = false;
        for (int u = 4; u < 256; u++) {
            if (!get(down, u) || get(held_, u)) continue;
            if (isMenuKey((uint8_t)u)) menuPressed = true;
            if (nEvents < 6) events[nEvents++] = (uint8_t)u;
        }
        memcpy(held_, down, sizeof held_);
        for (int b = 0; b < 32; b++) masked_[b] &= down[b];
        maskedMods_ &= mods;
        if (menuOpen) return MENU;
        nEvents = 0;
        if (menuPressed) return OPEN;
        outMods = (uint8_t)(mods & ~maskedMods_);
        for (int b = 0; b < 32; b++) outDown[b] = (uint8_t)(down[b] & ~masked_[b]);
        static constexpr uint8_t kMenuKeys[3] = {menu_key::SCROLL_LOCK, menu_key::APPLICATION, menu_key::MENU};
        for (uint8_t u : kMenuKeys) outDown[u >> 3] &= (uint8_t)~(1u << (u & 7));
        return PASS;
    }
    // メニューを閉じた (コア 0 から。USB の割り込みを止めて呼ぶ)。いま押しているキーは、一度離すまで送らない
    void closed(uint8_t mods) {
        memcpy(masked_, held_, sizeof masked_);
        maskedMods_ = mods;
    }
    void reset() {
        memset(held_, 0, sizeof held_);
        memset(masked_, 0, sizeof masked_);
        maskedMods_ = 0;
    }
    // いま押しているか (キーの繰り返し用)
    bool isHeld(uint8_t u) const { return get(held_, u); }

private:
    static bool get(const uint8_t* bits, int u) { return (bits[u >> 3] >> (u & 7)) & 1; }
    uint8_t held_[32] = {};   // 押している物理キー (書き換えたあと)
    uint8_t masked_[32] = {}; // 閉じたときに押していて、まだ離していないキー
    uint8_t maskedMods_ = 0;
};

// メニューの ↑↓←→ を押し続けたときの繰り返し (最初は 400ms、そのあと 60ms ごと)
class MenuKeyRepeat {
public:
    // key: いま押し続けているキー (0 ならなし)。繰り返すときはそのキーを返す (0 なら何もしない)
    uint8_t update(uint8_t key, uint32_t nowMs) {
        if (key != key_) {
            key_ = key;
            next_ = nowMs + 400;
            return 0;
        }
        if (key == 0 || (int32_t)(nowMs - next_) < 0) return 0;
        next_ = nowMs + 60;
        return key;
    }
    static bool repeats(uint8_t u) {
        return u == menu_key::UP || u == menu_key::DOWN || u == menu_key::LEFT || u == menu_key::RIGHT ||
               u == menu_key::PAGE_UP || u == menu_key::PAGE_DOWN;
    }

private:
    uint8_t key_ = 0;
    uint32_t next_ = 0;
};

// ---------------------------------------------------------------- 文字列

// 固定長の文字列に足していく (stdio を使わない)
class MenuText {
public:
    MenuText(char* buf, size_t size) : buf_(buf), size_(size) { clear(); }
    void clear() {
        len_ = 0;
        if (size_) buf_[0] = 0;
    }
    MenuText& add(const char* s) {
        while (*s && len_ + 1 < size_) buf_[len_++] = *s++;
        if (size_) buf_[len_] = 0;
        return *this;
    }
    MenuText& add(long v) {
        char tmp[24];
        formatDecimal(v, tmp, sizeof tmp);
        return add(tmp);
    }
    const char* str() const { return buf_; }

private:
    char* buf_;
    size_t size_, len_ = 0;
};

// ---------------------------------------------------------------- ファイルの一覧

enum class MenuFileKind { FLOPPY, HARD_DISK, CDROM, SCSI_DISK };

// ファイル名の一覧 (名前で並べる)。メモリは使うときに new で確保する
class MenuFileList {
public:
    static constexpr int MAX = 512;
    static constexpr size_t NAME = 96;
    ~MenuFileList() { delete[] names_; }
    void clear() { count_ = 0; }
    // 足す。いっぱいなら (メモリが足りなければ) false。NAME バイトに入らない長い名前は、切ると別のファイルを指すので
    // 足さずに true を返す (UTF-8 の日本語の名前は 1 字 3 バイトなので、31 字まで)
    bool add(const char* name) {
        if (strlen(name) >= NAME) return true;
        if (names_ == nullptr) {
            names_ = new char[MAX][NAME];
            if (names_ == nullptr) return false;
        }
        if (count_ >= MAX) return false;
        copyString(names_[count_], NAME, name);
        // 大文字と小文字を区別せずに並べる (挿入ソート)
        int i = count_++;
        while (i > 0 && less(names_[i], names_[i - 1])) {
            char tmp[NAME];
            memcpy(tmp, names_[i], NAME);
            memcpy(names_[i], names_[i - 1], NAME);
            memcpy(names_[i - 1], tmp, NAME);
            i--;
        }
        return true;
    }
    int count() const { return count_; }
    const char* name(int i) const { return names_[i]; }
    bool full() const { return count_ >= MAX; }

    // 名前がこの種類のイメージの拡張子か (大文字と小文字は区別しない)
    static bool matches(const char* name, MenuFileKind kind) {
        static const char* const kFloppy[] = {".xdf", ".dim", ".d88", ".hdm", ".2hd", nullptr};
        static const char* const kHard[] = {".hdf", nullptr};
        static const char* const kCd[] = {".iso", ".cue", nullptr};
        static const char* const kScsi[] = {".hds", nullptr};
        const char* const* exts = kind == MenuFileKind::FLOPPY      ? kFloppy
                                  : kind == MenuFileKind::CDROM     ? kCd
                                  : kind == MenuFileKind::SCSI_DISK ? kScsi
                                                                    : kHard;
        const size_t n = strlen(name);
        for (int i = 0; exts[i]; i++) {
            const size_t e = strlen(exts[i]);
            if (n > e && lowerEq(name + n - e, exts[i])) return true;
        }
        return false;
    }

private:
    static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
    static bool lowerEq(const char* a, const char* b) {
        for (; *a && *b; a++, b++)
            if (lower(*a) != lower(*b)) return false;
        return *a == *b;
    }
    // バイトは符号なしで比べる (UTF-8 の日本語の名前を、英数字の名前のあとに並べる)
    static bool less(const char* a, const char* b) {
        for (; *a && *b; a++, b++)
            if (lower(*a) != lower(*b)) return (uint8_t)lower(*a) < (uint8_t)lower(*b);
        return *a == 0 && *b != 0;
    }
    char (*names_)[NAME] = nullptr;
    int count_ = 0;
};

// ---------------------------------------------------------------- 環境に頼むこと

// メニューの操作の種類 (MenuHost::beginAction())。書き戻せずに断った操作を、続けてもう一度選んだかを見るため
enum MenuAction { MENU_ACTION_OTHER = 0, MENU_ACTION_RESTART = 1, MENU_ACTION_EJECT = 2 /* + ドライブの番号 */ };

// 書き戻せずに断った操作を、ほかの操作をはさまずに続けてもう一度選んだら、書き戻さずに進める (データを捨てて抜ける)。
// 環境 (MenuHost) が持ち、beginAction() で begin() を、断ったときに arm() を呼ぶ
class MenuDiscardConfirm {
public:
    // 操作を始める。断った操作と同じなら confirmed() が true。ほかの操作なら確かめたことを忘れる
    void begin(int action) {
        confirmed_ = action != MENU_ACTION_OTHER && action == armed_;
        armed_ = MENU_ACTION_OTHER;
    }
    bool confirmed() const { return confirmed_; }
    // 書き戻せずに断った (次に同じ操作を選んだら confirmed())
    void arm(int action) {
        armed_ = action;
        confirmed_ = false;
    }
    void clear() {
        armed_ = MENU_ACTION_OTHER;
        confirmed_ = false;
    }

private:
    int armed_ = MENU_ACTION_OTHER;
    bool confirmed_ = false;
};

// CD-ROM の媒体を入れ替えたあとに、メニューの状態の行に出す文 (ホスト版とベアメタル版で同じ文にする)。
// 状態の行は Menu::COLS (48) 文字までなので、それに入る長さにする (menu_test で確かめる)。
// locked は、X68000 が取り出しを禁止していた (PREVENT ALLOW MEDIUM REMOVAL) のに入れ替えたとき
inline void menuCdChangedText(MenuText& msg, bool inserted, bool locked) {
    msg.add(inserted ? "CD-ROM: inserted" : "CD-ROM: ejected");
    if (locked) msg.add(" (was locked by the X68000)");
}
// 前の媒体を外したあとで失敗して、装置が空になったとき (MenuHost::changeCd() の -2)。名前が長くても
// 「装置は空」が切れないよう、先に書く
inline void menuCdEmptyText(MenuText& msg, const char* name) { msg.add("drive is empty: cannot load ").add(name); }

class MenuHost {
public:
    virtual ~MenuHost() {}
    // 操作 (値の変更、ディスクの入れ替え、保存、Restart など) の前に呼ぶ。action は MenuAction。
    // カーソルの移動と、ファイルの一覧を開く / 閉じるのは操作に数えない
    virtual void beginAction(int action) { (void)action; }
    // kind のイメージのファイルを list に足す
    virtual void listFiles(MenuFileKind kind, MenuFileList& list) = 0;
    // フロッピーディスクを入れ替える (name が空なら取り出す)。失敗したら false と msg (英語の短い文)
    virtual bool insertDisk(int drive, const char* name, MenuText& msg) = 0;
    // いまドライブに入っているイメージの名前 (なければ空)
    virtual const char* diskName(int drive) = 0;
    // CD-ROM 装置の媒体を、動かしたまま入れ替える (name が空なら取り出す)。1 なら入れ替えた、0 なら CD-ROM 装置を
    // つないでいない (次の起動から)、-1 なら失敗 (msg に英語の短い文。入っている媒体はそのまま)、
    // -2 なら、前の媒体を外したあとで失敗した (msg に英語の短い文。装置は空になった)。
    // X68000 が取り出しを禁止していても入れ替える。そのときは、禁止されていたことを msg に入れる
    virtual int changeCd(const char* name, MenuText& msg) {
        (void)name;
        (void)msg;
        return 0;
    }
    // X68000 をリセットする (電源を入れ直したのと同じ)
    virtual void resetMachine() = 0;
    virtual bool saveState(int slot, MenuText& msg) = 0;
    virtual bool loadState(int slot, MenuText& msg) = 0;
    // 設定を保存する
    virtual bool saveSettings(const Settings& s, MenuText& msg) = 0;
    // 電源を入れ直して設定を反映する。戻ったら (ホスト版はメニューを閉じたあとに作り直す) true
    virtual bool restart(const Settings& s, MenuText& msg) = 0;
    // この環境の CPU (68030 / 68060 が使えるか)
    virtual bool has020() const { return x68k::Config::has020(); }
    // 画面の出し方 (video_scale=、video_filter=) をメニューで変えられるか
    virtual bool hasVideo() const { return false; }
    // モニタの枠 (video_filter= の cz-600de / cz-600db) をメニューで選べるか (ホスト版は使わない)
    virtual bool hasBezel() const { return false; }
    // 枠 filter (CRT_BEZEL_DE か CRT_BEZEL_DB を含む) の絵を使えるか (ベアメタル版は初めて聞かれたときに読む)。
    // 聞くのは Bezel で枠を替えるときと、Display の行に枠の名前を出すかを決めるときだけ。ほかの項目 (Scale や効果) は
    // 聞かない (設定に枠があって絵が読めなくても、枠なしで写しているので、変えられるようにする)
    virtual bool bezelAvailable(int filter) const {
        (void)filter;
        return true;
    }
    // video_scale= の scale と video_filter= の filter の組み合わせを、この環境で選べるか
    virtual bool videoAllowed(int scale, int filter) const {
        (void)scale;
        (void)filter;
        return true;
    }
    // メニューで画面の出し方を変えた (すぐに反映する。同じキーの列でメニューが閉じても、閉じる前に呼ぶ)
    virtual void videoChanged(int scale, int filter) {
        (void)scale;
        (void)filter;
    }
    // メニューで画面の隅の数字 (osd=) を変えた (すぐに反映する。呼ぶ時機は videoChanged() と同じ)
    virtual void osdChanged(int osd) { (void)osd; }
    // MIDI の行き先 (Settings::midiOut の値) をこの環境で選べるか (Pi 3 の sc55、ホスト版の uart は選べない)
    virtual bool hasMidiOut(int out) const {
        (void)out;
        return true;
    }
};

// ---------------------------------------------------------------- メニュー

class Menu {
public:
    static constexpr int COLS = 48; // メニューの枠の幅 (文字)
    // 1 行の文字列のバイト数の上限 (0 を含む)。ファイル名は UTF-8 なので、1 桁に 3 バイト (半角カタカナ) まで入るようにする
    static constexpr int LINE_BYTES = COLS * 3 + 1;
    // 開いたときの状態の行 (操作の説明)。拡張ボードのページでは、Esc が主のページに戻るので、HELP_BOARDS に替えて出す
    static constexpr const char* HELP = "Arrows: select/change  Enter: OK  Esc: close";
    static constexpr const char* HELP_BOARDS = "Arrows: select/change  Enter: OK  Esc: back";

    explicit Menu(MenuHost& host) : host_(host), status_(statusBuf_, sizeof statusBuf_) {}

    // 開く。running はいま動いている設定、saved は保存してある (次に起動したときの) 設定
    void open(const Settings& running, const Settings& saved) {
        running_ = running;
        pending_ = saved;
        // フロッピーディスクはいま入っているものを出す (メニューの外で替わることはない)
        for (int d = 0; d < 2; d++) copyString(pending_.fd[d], Settings::PATH_SIZE, host_.diskName(d));
        // 画面の拡大もすぐに反映する項目なので、いまの値を出す
        pending_.videoScale = running.videoScale;
        pending_.videoFilter = running.videoFilter;
        pending_.osd = running.osd;
        open_ = true;
        page_ = MAIN;
        cursor_ = 0;
        status_.clear();
        status_.add(HELP);
        restartPending_ = false;
        midiHint_ = nullptr;
    }
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    // 「Restart」が選ばれた (ホスト版は閉じたあとに作り直す)
    bool restartRequested() const { return restartPending_; }
    const Settings& pending() const { return pending_; }
    const char* status() const { return status_.str(); }

    // キーを押した (USB HID の usage)。閉じたら false
    bool key(uint8_t u) {
        if (!open_) return false;
        if (page_ == FILES) {
            fileKey(u);
            return open_;
        }
        // 拡張ボードと画面の出し方のページでは、Esc (か BS) は主のページに戻る
        if (page_ != MAIN && (u == menu_key::ESC || u == menu_key::BACKSPACE)) {
            leavePage();
            return open_;
        }
        const int n = itemCount();
        switch (u) {
        case menu_key::UP:
            cursor_ = (cursor_ + n - 1) % n;
            break;
        case menu_key::DOWN:
            cursor_ = (cursor_ + 1) % n;
            break;
        case menu_key::HOME:
        case menu_key::PAGE_UP:
            cursor_ = 0;
            break;
        case menu_key::END:
        case menu_key::PAGE_DOWN:
            cursor_ = n - 1;
            break;
        case menu_key::LEFT:
            change(item(cursor_), -1);
            break;
        case menu_key::RIGHT:
            change(item(cursor_), 1);
            break;
        case menu_key::ENTER:
        case menu_key::KP_ENTER:
            activate(item(cursor_));
            break;
        case menu_key::ESC:
        case menu_key::BACKSPACE:
            open_ = false;
            break;
        default:
            if (isMenuKey(u, true)) open_ = false;
            break;
        }
        return open_;
    }

    // ---- 描く ----
    // pixels (XRGB8888、1 行 pitch 画素) の width x height の真ん中に描く。下の画面は暗くする。
    // 入りきらなければ文字の数を減らす (幅) か、選んでいる行が見えるように一部だけ出す (高さ。最後の 2 行の「*」の説明
    // (か空行) と状態の行は残す)。文字の倍率は、どのページでも主のページの行の数で決める (ページを移っても変わらない)。
    // 書く画素には opaque を OR する (ベアメタルのフレームバッファはアルファを 0xff にする)。
    // 文字列は UTF-8。半角カタカナと全角 (ファイル名の日本語) は cgrom (CGROM の写し、cgromSize バイト) の字で描く
    // (platform/text_cells.h。cgrom がなければ半角カタカナは「?」、全角は豆腐)
    void draw(uint32_t* pixels, int pitch, int width, int height, uint32_t opaque = 0, const uint8_t* cgrom = nullptr,
              size_t cgromSize = 0) const {
        if (!open_ || width < 16 || height < 16) return;
        char lines[MAX_ROWS][LINE_BYTES];
        int hl = -1;
        int rows = buildLines(lines, hl);
        // 大きな画面では文字を 2 倍、3 倍にする
        const int scaleRows = mainRows();
        int scale = 1;
        while (scale < 3 && (COLS + 2) * 8 * (scale + 1) <= width && (scaleRows + 2) * 16 * (scale + 1) <= height)
            scale++;
        int cols = COLS;
        if ((cols + 2) * 8 * scale > width) cols = width / (8 * scale) - 2;
        if (cols < 4) cols = width / (8 * scale);
        int fit = height / (16 * scale) - 2;
        if (fit < 1) fit = height / (16 * scale);
        if (fit < 1) return;
        // 高さが足りなければ、最後の FOOTER 行は残し、その上の行は、選んでいる行が入るように出す範囲を動かす
        // (主のページは 68000 のベアメタル版で 30 行あり、640x480 や 720x480 では 28 行しか出せない)
        const int footer = rows > fit && fit > FOOTER ? FOOTER : 0;
        const int body = rows - footer, bodyFit = fit - footer;
        int first = 0, shownBody = body;
        if (body > bodyFit) {
            shownBody = bodyFit;
            if (hl >= 0 && hl >= bodyFit) first = hl - bodyFit + 1;
            if (first + shownBody > body) first = body - shownBody;
        }
        const int shown = shownBody + footer;
        const int boxW = (cols + 2) * 8 * scale, boxH = (shown + 1) * 16 * scale;
        const int x0 = (width - boxW) / 2 > 0 ? (width - boxW) / 2 : 0;
        const int y0 = (height - boxH) / 2 > 0 ? (height - boxH) / 2 : 0;
        const int x1 = x0 + boxW < width ? x0 + boxW : width, y1 = y0 + boxH < height ? y0 + boxH : height;
        // 下の画面を暗くし、枠を描く
        for (int y = y0; y < y1; y++) {
            uint32_t* row = pixels + (size_t)y * pitch;
            for (int x = x0; x < x1; x++) {
                const bool border = y == y0 || y == y1 - 1 || x == x0 || x == x1 - 1;
                row[x] = (border ? 0xc0c0c0u : ((row[x] >> 3) & 0x1f1f1fu) + 0x101028u) | opaque;
            }
        }
        for (int r = 0; r < shown; r++) {
            const int li = r < shownBody ? first + r : body + (r - shownBody);
            const int ty = y0 + (8 + r * 16) * scale;
            const bool sel = li == hl;
            if (sel) {
                for (int y = ty; y < ty + 16 * scale && y < y1 - 1; y++) {
                    uint32_t* row = pixels + (size_t)y * pitch;
                    for (int x = x0 + 4 * scale; x < x1 - 4 * scale; x++) row[x] = 0x2050b0u | opaque;
                }
            }
            const uint32_t color = li == 0 ? 0xffe080u : (lines[li][0] == '-' ? 0x909090u : 0xffffffu);
            drawText(pixels, pitch, x1, y1, x0 + 8 * scale, ty, lines[li], cols, scale, color | opaque, cgrom, cgromSize);
        }
    }

    // UTF-8 の文字列 s を maxChars 桁 (全角は 2 桁) まで描く。入りきらなければ最後を「…」にする (layoutLine())。
    // x の右端と y の下端 (xmax、ymax) を超える分は描かない
    static void drawText(uint32_t* pixels, int pitch, int xmax, int ymax, int x, int y, const char* s, int maxChars,
                         int scale, uint32_t color, const uint8_t* cgrom = nullptr, size_t cgromSize = 0) {
        uint16_t cells[COLS + 2];
        if (maxChars > COLS) maxChars = COLS;
        const int n = layoutLine(s, cells, maxChars);
        drawCells(pixels, pitch, xmax, ymax, x, y, cells, n, scale, color, cgrom, cgromSize);
    }

    // テスト用: 選んでいる項目の名前
    const char* currentLabel() const {
        if (page_ == FILES) return "files";
        return label(item(cursor_));
    }
    // テスト用: 選んでいる項目の値の文字 (メニューの行の、名前の右に出すもの)。buf に入れて返す
    const char* currentValue(char* buf, size_t size) const {
        MenuText t(buf, size);
        if (page_ != FILES) value(item(cursor_), t);
        return t.str();
    }
    bool inFileList() const { return page_ == FILES; }
    int stateSlot() const { return slot_; }

private:
    enum Page { MAIN, FILES, BOARDS, DISPLAY };
    enum Item {
        I_FD0, I_FD1, I_HD0, I_CD, I_CPU, I_CLOCK, I_RAM, I_HIGH_MEM, I_LOCAL_MEM, I_MAX_SPEED,
        I_SCSI_EX, I_SC0, I_SC1, I_SC2, I_SC3, I_SC4, I_SC5, I_SC6, I_BOARDS, I_DISPLAY, I_OSD,
        I_SLOT, I_SAVE_STATE, I_LOAD_STATE, I_RESET, I_SAVE_SETTINGS, I_RESTART, I_CLOSE,
        // ここから拡張ボードのページの項目
        I_FPU_BOARD, I_MERCURY, I_NEPTUNE, I_MIDI, I_MIDI_OUT, I_BACK,
        // ここから画面の出し方のページの項目
        I_SCALE, I_SCANLINE, I_CURVE, I_MASK, I_BEZEL, I_DISPLAY_BACK, I_COUNT
    };
    static constexpr int MAX_ROWS = 40;
    static constexpr int FOOTER = 2; // どのページも、最後の 2 行は「*」の説明 (か空行) と状態の行

    // 主のページの行の数 (見出し、空行、項目、「*」の説明、状態)。文字の倍率を決める (draw())
    int mainRows() const {
        int n = 4;
        for (int i = 0; i < I_COUNT; i++) n += usable((Item)i) && pageOf((Item)i) == MAIN ? 1 : 0;
        return n;
    }

    // CPU によって出さない項目がある (クロックは 68000 と 68060、外付け SCSI ボードは 68000、ハイメモリは 68030、
    // ローカルメモリは 68060 だけ)。
    // MIDI の行き先は、MIDI ボードをつなぐときだけ出す
    // 設定の値から決まる分 (どのページを開いているかは見ない)
    bool usable(Item it) const {
        switch (it) {
        case I_CLOCK: return pending_.cpu == 68000 || pending_.cpu == 68060;
        case I_SCSI_EX: return pending_.cpu == 68000;
        case I_HIGH_MEM: return pending_.cpu == 68030;
        case I_LOCAL_MEM: return pending_.cpu == 68060;
        case I_MIDI_OUT: return pending_.midi != 0;
        case I_DISPLAY:
        case I_SCALE:
        case I_SCANLINE:
        case I_CURVE:
        case I_MASK: return host_.hasVideo();
        case I_BEZEL: return host_.hasVideo() && host_.hasBezel();
        default: return true;
        }
    }
    // 項目を出すページ
    static Page pageOf(Item it) {
        return it >= I_SCALE ? DISPLAY : it >= I_FPU_BOARD ? BOARDS : MAIN;
    }
    // いま開いているページに出す項目か
    bool shown(Item it) const { return usable(it) && pageOf(it) == page_; }
    int itemCount() const {
        int n = 0;
        for (int i = 0; i < I_COUNT; i++) n += shown((Item)i) ? 1 : 0;
        return n;
    }
    Item item(int index) const {
        for (int i = 0; i < I_COUNT; i++) {
            if (!shown((Item)i)) continue;
            if (index-- == 0) return (Item)i;
        }
        return page_ == BOARDS ? I_BACK : page_ == DISPLAY ? I_DISPLAY_BACK : I_CLOSE;
    }
    const char* label(Item it) const {
        static const char* const kLabels[I_COUNT] = {
            "FD0", "FD1", "HD0 (SASI)", "CD-ROM", "CPU", "Clock", "RAM", "High memory", "Local memory", "Max speed",
            "SCSI board", "SC0", "SC1", "SC2", "SC3", "SC4", "SC5", "SC6", "Boards", "Display", "OSD",
            "State slot", "Save state", "Load state", "Reset", "Save settings", "Restart", "Close",
            "FPU board", "Mercury Unit", "Neptune-X", "MIDI board", "MIDI out", "Back",
            "Scale", "Scanlines", "Curve", "Mask", "Bezel", "Back",
        };
        return kLabels[it];
    }
    // その項目に対応する pix68k.cfg のキー (なければ nullptr)
    static const char* cfgKey(Item it) {
        switch (it) {
        case I_HD0: return "hd0";
        case I_CD: return "cd";
        case I_CPU: return "cpu";
        case I_CLOCK: return "clock";
        case I_RAM: return "ram";
        case I_HIGH_MEM: return "high_mem";
        case I_LOCAL_MEM: return "local_mem";
        case I_MAX_SPEED: return "max_speed";
        case I_SCSI_EX: return "scsi_ex";
        case I_SC0: return "sc0";
        case I_SC1: return "sc1";
        case I_SC2: return "sc2";
        case I_SC3: return "sc3";
        case I_SC4: return "sc4";
        case I_SC5: return "sc5";
        case I_SC6: return "sc6";
        case I_FPU_BOARD: return "fpu_board";
        case I_MERCURY: return "mercury";
        case I_NEPTUNE: return "neptune";
        case I_MIDI: return "midi";
        case I_MIDI_OUT: return "midi_out";
        case I_SCALE: return "video_scale";
        case I_SCANLINE:
        case I_CURVE:
        case I_MASK:
        case I_BEZEL: return "video_filter";
        case I_OSD: return "osd";
        default: return nullptr;
        }
    }
    // 電源を入れ直さないと反映されない値を変えたか
    bool changed(Item it) const {
        if (it == I_BOARDS) {
            // 拡張ボードのページのどれかを変えた
            for (int i = I_FPU_BOARD; i < I_BACK; i++)
                if (usable((Item)i) && changed((Item)i)) return true;
            return false;
        }
        const char* k = cfgKey(it);
        if (k == nullptr) return false;
        const SettingKey* sk = findSettingKey(k);
        if (it == I_LOCAL_MEM) return localMem(pending_) != localMem(running_);
        if (it == I_CLOCK) return pending_.effectiveClockMHz() != running_.effectiveClockMHz();
        return sk && sk->restart && !sameSetting(pending_, running_, *sk);
    }
    bool anyChanged() const {
        for (int i = 0; i < I_COUNT; i++)
            if (changed((Item)i) && usable((Item)i)) return true;
        return pending_.cpu != running_.cpu;
    }
    static int localMem(const Settings& s) { return s.localMemMB >= 0 ? s.localMemMB : (s.cpu == 68060 ? 128 : 0); }

    void value(Item it, MenuText& t) const {
        switch (it) {
        case I_FD0:
        case I_FD1: {
            const char* n = pending_.fd[it - I_FD0];
            t.add(n[0] ? baseName(n) : "(empty)");
            break;
        }
        case I_HD0:
            t.add(pending_.hd0[0] ? baseName(pending_.hd0) : "(none)");
            break;
        case I_CD:
            t.add(pending_.cd[0] ? baseName(pending_.cd) : "(none)");
            if (pending_.cd[0]) t.add(" ID ").add((long)pending_.cdId);
            break;
        case I_CPU:
            t.add((long)pending_.cpu);
            t.add(pending_.cpu == 68030 ? " (X68030)" : pending_.cpu == 68060 ? " (060turbo)" : "");
            break;
        case I_CLOCK:
            t.add((long)pending_.effectiveClockMHz()).add(" MHz");
            if (pending_.cpu == 68060 && pending_.effectiveClockMHz() == 75) t.add(" (overclock)");
            break;
        case I_RAM:
            t.add((long)pending_.ramMB).add(" MB");
            break;
        case I_HIGH_MEM:
            t.add((long)pending_.highMemMB).add(" MB");
            break;
        case I_LOCAL_MEM:
            t.add((long)localMem(pending_)).add(" MB");
            break;
        case I_MAX_SPEED:
            t.add(pending_.maxSpeed ? "on" : "off");
            break;
        case I_SCSI_EX:
            t.add(pending_.scsiEx ? "on" : "off");
            break;
        case I_SC0:
        case I_SC1:
        case I_SC2:
        case I_SC3:
        case I_SC4:
        case I_SC5:
        case I_SC6: {
            const char* n = pending_.sc[it - I_SC0];
            t.add(n[0] ? baseName(n) : "(none)");
            break;
        }
        case I_BOARDS: {
            // つなぐボードの名前を並べる
            const int before = (int)strlen(t.str());
            if (pending_.fpuBoard) t.add(pending_.fpuBoard >= 2 ? "FPUx2 " : "FPU ");
            if (pending_.mercury) t.add("Mercury ");
            if (pending_.neptune) t.add("Neptune ");
            if (pending_.midi) t.add("MIDI ");
            if ((int)strlen(t.str()) == before) t.add("(none)");
            break;
        }
        case I_FPU_BOARD: // off、on (1 枚)、2 (2 枚。$E9E080 にも)
            t.add(pending_.fpuBoard >= 2 ? "2" : pending_.fpuBoard ? "on" : "off");
            break;
        case I_MERCURY:
            t.add(pending_.mercury ? "on" : "off");
            break;
        case I_NEPTUNE:
            t.add(pending_.neptune ? "on" : "off");
            break;
        case I_MIDI:
            t.add(pending_.midi ? "on" : "off");
            break;
        case I_MIDI_OUT:
            t.add(midiOutName(pending_.midiOut));
            break;
        case I_DISPLAY:
            // 拡大と効果をまとめて出す (「integer」「aspect + scanline,curve」など)。枠があれば、拡大の代わりに枠の名前
            // (枠はガラスの矩形に写し、video_scale= を使わない。「cz-600de + scanline,curve,mask」)
            // 枠を使えないとき (ホスト版、絵が読めない) は、枠なしで写しているので、拡大と効果だけを出す
            if (const char* bezel = videoBezelName(pending_.videoFilter);
                bezel != nullptr && host_.hasBezel() && host_.bezelAvailable(pending_.videoFilter)) {
                t.add(bezel);
                if ((pending_.videoFilter & CRT_EFFECT_BITS) != 0)
                    t.add(" + ").add(videoFilterName(pending_.videoFilter & CRT_EFFECT_BITS));
            } else {
                t.add(videoScaleName(pending_.videoScale));
                const int effect = pending_.videoFilter & CRT_EFFECT_BITS;
                if (effect != CRT_OFF) t.add(" + ").add(videoFilterName(effect));
            }
            break;
        case I_SCALE:
            t.add(videoScaleName(pending_.videoScale));
            break;
        case I_SCANLINE:
        case I_CURVE:
        case I_MASK:
            t.add((pending_.videoFilter & filterBit(it)) ? "on" : "off");
            break;
        case I_BEZEL: {
            const char* b = videoBezelName(pending_.videoFilter);
            t.add(b != nullptr ? b : "off");
            break;
        }
        case I_OSD:
            // on: 余白に出す。ベアメタル版は余白が足りなければ X68000 の画面に重ねる (IP アドレスをいつも出すため)。
            // ホスト版は重ねない。fps: どちらも重ねる (platform/osd.h の chooseOsd())
            t.add(pending_.osd == 0 ? "off" : pending_.osd == 1 ? "fps" : "on");
            break;
        case I_SLOT:
            t.add((long)slot_);
            break;
        default:
            break;
        }
    }

    int buildLines(char (*lines)[LINE_BYTES], int& hl) const {
        int n = 0;
        auto line = [&](const char* s) {
            if (n < MAX_ROWS) copyString(lines[n++], LINE_BYTES, s);
        };
        char buf[LINE_BYTES];
        MenuText t(buf, sizeof buf);
        if (page_ != FILES) {
            line(page_ == BOARDS    ? "Expansion boards  (Esc: back)"
                 : page_ == DISPLAY ? "Display  (Esc: back)"
                                    : "pix68k menu  (X68000 is paused)");
            line("");
            const int count = itemCount();
            for (int i = 0; i < count; i++) {
                const Item it = item(i);
                t.clear();
                t.add(changed(it) ? "*" : " ").add(label(it));
                char pad[LINE_BYTES];
                MenuText v(pad, sizeof pad);
                value(it, v);
                if (v.str()[0]) {
                    while (strlen(t.str()) < 16) t.add(" ");
                    t.add(v.str());
                }
                if (i == cursor_) hl = n;
                line(t.str());
            }
            // 項目のすぐ下に「*」の説明 (なければ空行)、その下に状態。主のページは 68000 のベアメタル版で 26 項目あり、
            // 768x512 に出せる 30 行 (draw() の fit) に入れるため、ここに空行をもう 1 つは置かない
            line(anyChanged() ? "* = applied after Restart" : "");
            line(page_ != MAIN && strcmp(status_.str(), HELP) == 0 ? HELP_BOARDS : status_.str());
        } else {
            t.clear();
            if (fileKind_ == MenuFileKind::FLOPPY) t.add(fileDrive_ ? "FD1: " : "FD0: ");
            else if (fileKind_ == MenuFileKind::CDROM) t.add("CD-ROM: ");
            else if (fileKind_ == MenuFileKind::SCSI_DISK) t.add("SC").add((long)fileDrive_).add(": ");
            else t.add("HD0: ");
            t.add("choose an image");
            line(t.str());
            line("");
            // 行の数には上限 (MAX_ROWS) があるので、選んでいる名前が入る範囲だけを行にする
            // (見出し 2 行と、後ろの空行、状態、「no image files」の 3 行を除いた分)
            const int total = files_.count() + 1;
            const int cap = MAX_ROWS - 5;
            int first = 0;
            if (total > cap) {
                first = fileCursor_ - cap / 2;
                if (first > total - cap) first = total - cap;
                if (first < 0) first = 0;
            }
            const int last = first + cap < total ? first + cap : total;
            for (int i = first; i < last; i++) {
                t.clear();
                if (i == 0) t.add(fileKind_ == MenuFileKind::FLOPPY ? " [Eject]" : " [None]");
                else t.add(" ").add(files_.name(i - 1));
                if (i == fileCursor_) hl = n;
                line(t.str());
            }
            if (files_.count() == 0) line("- no image files found");
            line("");
            line(status_.str());
        }
        return n;
    }

    void change(Item it, int dir) {
        host_.beginAction(MENU_ACTION_OTHER);
        switch (it) {
        case I_CPU: {
            static const int kCpus[] = {68000, 68030, 68060};
            const int count = host_.has020() ? 3 : 1;
            int i = 0;
            while (i < 3 && kCpus[i] != pending_.cpu) i++;
            i = ((i % 3) + dir + count) % count;
            pending_.cpu = kCpus[i];
            break;
        }
        case I_CLOCK:
            // 68000 は 10 と 16、68060 は 50 と 75 を切り替える (clock= は CPU ごとに読み替える。Settings::clockFor())
            if (pending_.cpu == 68060) pending_.clockMHz = pending_.effectiveClockMHz() == 50 ? 75 : 50;
            else pending_.clockMHz = pending_.effectiveClockMHz() == 10 ? 16 : 10;
            break;
        case I_RAM:
            pending_.ramMB = clamp(pending_.ramMB + dir, 1, 12);
            break;
        case I_HIGH_MEM:
            pending_.highMemMB = clamp(pending_.highMemMB + dir, 0, 16);
            break;
        case I_LOCAL_MEM:
            pending_.localMemMB = clamp(localMem(pending_) + dir * 16, 0, 256);
            break;
        case I_MAX_SPEED:
            pending_.maxSpeed = !pending_.maxSpeed;
            break;
        case I_SCSI_EX:
            pending_.scsiEx = !pending_.scsiEx;
            break;
        case I_FPU_BOARD: // off、on、2 の順に回る
            pending_.fpuBoard = (pending_.fpuBoard + dir + 3) % 3;
            break;
        case I_MERCURY:
            pending_.mercury = !pending_.mercury;
            break;
        case I_NEPTUNE:
            pending_.neptune = !pending_.neptune;
            break;
        case I_MIDI:
            pending_.midi = !pending_.midi;
            midiHint();
            break;
        case I_MIDI_OUT: {
            // pix68k.cfg の選択肢の順に回し、この環境で選べないものは飛ばす。いまの値が選べないもの
            // (設定ファイルに書いてあった) でも、そこから次の選べるものへ動く
            const SettingKey* sk = findSettingKey("midi_out");
            int n = 0;
            while (sk->choices[n]) n++;
            int i = 0;
            while (i < n && sk->choiceValues[i] != pending_.midiOut) i++;
            if (i == n) i = 0;
            for (int k = 0; k < n; k++) {
                i = (i + dir + n) % n;
                if (!host_.hasMidiOut(sk->choiceValues[i])) continue;
                pending_.midiOut = sk->choiceValues[i];
                break;
            }
            midiHint();
            break;
        }
        case I_SCALE: {
            // off → integer → aspect → full の順 (戻るキーは逆)。この環境で選べない組み合わせは飛ばす
            int v = pending_.videoScale;
            for (int k = 0; k < VIDEO_SCALE_COUNT - 1; k++) {
                v = (v + dir + VIDEO_SCALE_COUNT) % VIDEO_SCALE_COUNT;
                if (host_.videoAllowed(v, pending_.videoFilter)) break;
            }
            if (v == pending_.videoScale || !host_.videoAllowed(v, pending_.videoFilter)) {
                notAvailable();
                break;
            }
            pending_.videoScale = v;
            host_.videoChanged(pending_.videoScale, pending_.videoFilter);
            break;
        }
        case I_SCANLINE:
        case I_CURVE:
        case I_MASK: {
            const int f = pending_.videoFilter ^ filterBit(it);
            if (!host_.videoAllowed(pending_.videoScale, f)) {
                notAvailable();
                break;
            }
            pending_.videoFilter = f;
            host_.videoChanged(pending_.videoScale, pending_.videoFilter);
            break;
        }
        case I_BEZEL: {
            // off → cz-600de → cz-600db の順 (戻るキーは逆)。選べない枠 (組み合わせが選べない、絵が読めない) は飛ばす
            static const int kBezels[3] = {0, CRT_BEZEL_DE, CRT_BEZEL_DB};
            const int cur = videoBezel(pending_.videoFilter), effect = pending_.videoFilter & ~CRT_BEZEL_BITS;
            auto usable = [&](int f) {
                return host_.videoAllowed(pending_.videoScale, f) && ((f & CRT_BEZEL_BITS) == 0 || host_.bezelAvailable(f));
            };
            int i = cur, f = pending_.videoFilter;
            for (int k = 0; k < 2; k++) {
                i = (i + dir + 3) % 3;
                f = effect | kBezels[i];
                if (usable(f)) break;
            }
            if (i == cur || !usable(f)) {
                notAvailable();
                break;
            }
            pending_.videoFilter = f;
            host_.videoChanged(pending_.videoScale, pending_.videoFilter);
            break;
        }
        case I_OSD:
            // off (0) → on (2) → fps (1) の順。戻るキーは逆の順
            pending_.osd = dir < 0 ? (pending_.osd == 0 ? 1 : pending_.osd == 1 ? 2 : 0)
                                   : (pending_.osd == 0 ? 2 : pending_.osd == 2 ? 1 : 0);
            host_.osdChanged(pending_.osd);
            break;
        case I_SLOT:
            slot_ = (slot_ + dir + 10) % 10;
            break;
        default:
            break;
        }
    }
    // Scanlines、Curve、Mask の項目が切り替える効果のビット
    static int filterBit(Item it) { return it == I_SCANLINE ? CRT_SCANLINE : it == I_CURVE ? CRT_CURVE : CRT_MASK; }
    // 画面の出し方で、選べない組み合わせを選ぼうとした
    void notAvailable() {
        status_.clear();
        status_.add("not available on this machine");
    }
    // MIDI の行き先の名前 (Settings::midiOut の値)
    static const char* midiOutName(int out) {
        return out == 0 ? "SoundFont" : out == 1 ? "UART" : out == 2 ? "none" : out == 3 ? "MT-32" : out == 4 ? "SC-55" : "?";
    }
    // MIDI の行き先に要るファイル名 (メニューでは変えられない) を書いていなければ、そのキーを知らせる。
    // 知らせることがなければ状態の行は変えない (操作の説明や「settings saved」を残す)。前に出した案内だけは消す
    void midiHint() {
        const char* hint = nullptr;
        if (!pending_.midi) hint = nullptr;
        else if (pending_.midiOut == 0 && !pending_.midiSf2[0]) hint = "SoundFont needs midi_sf2= in pix68k.cfg";
        else if (pending_.midiOut == 3 && (!pending_.midiMt32Ctrl[0] || !pending_.midiMt32Pcm[0]))
            hint = "MT-32 needs midi_mt32_ctrl= and midi_mt32_pcm=";
        else if (pending_.midiOut == 4 && !pending_.midiSc55[0]) hint = "SC-55 needs midi_sc55= in pix68k.cfg";
        if (hint == nullptr && (midiHint_ == nullptr || strcmp(status_.str(), midiHint_) != 0)) return;
        status_.clear();
        if (hint) status_.add(hint);
        midiHint_ = hint;
    }
    void enterPage(Page page) {
        mainCursor_ = cursor_;
        page_ = page;
        cursor_ = 0;
    }
    void leavePage() {
        page_ = MAIN;
        cursor_ = mainCursor_;
    }
    static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

    void activate(Item it) {
        // ページを移るだけなら状態の行は残す (拡張ボードのページで出した案内を、戻ったあとも読めるように)
        if (it != I_BOARDS && it != I_BACK && it != I_DISPLAY && it != I_DISPLAY_BACK) status_.clear();
        // ファイルの一覧を開くのは操作に数えない (FD の取り出しは一覧の中で選ぶ)。値を変える項目は change() が呼ぶ
        if (it == I_SAVE_STATE || it == I_LOAD_STATE || it == I_RESET || it == I_SAVE_SETTINGS || it == I_CLOSE)
            host_.beginAction(MENU_ACTION_OTHER);
        else if (it == I_RESTART)
            host_.beginAction(MENU_ACTION_RESTART);
        switch (it) {
        case I_FD0:
        case I_FD1:
            openFiles(MenuFileKind::FLOPPY, it - I_FD0);
            break;
        case I_HD0:
            openFiles(MenuFileKind::HARD_DISK, 0);
            break;
        case I_CD:
            openFiles(MenuFileKind::CDROM, 0);
            break;
        case I_SC0:
        case I_SC1:
        case I_SC2:
        case I_SC3:
        case I_SC4:
        case I_SC5:
        case I_SC6:
            openFiles(MenuFileKind::SCSI_DISK, it - I_SC0);
            break;
        case I_BOARDS:
            enterPage(BOARDS);
            break;
        case I_DISPLAY:
            enterPage(DISPLAY);
            break;
        case I_BACK:
        case I_DISPLAY_BACK:
            leavePage();
            break;
        case I_SAVE_STATE:
            if (host_.saveState(slot_, status_) && status_.str()[0] == 0) status_.add("state saved");
            break;
        case I_LOAD_STATE:
            if (host_.loadState(slot_, status_)) {
                if (status_.str()[0] == 0) status_.add("state loaded");
                open_ = false;
            }
            break;
        case I_RESET:
            host_.resetMachine();
            open_ = false;
            break;
        case I_SAVE_SETTINGS:
            if (host_.saveSettings(pending_, status_) && status_.str()[0] == 0) status_.add("settings saved");
            break;
        case I_RESTART:
            if (host_.restart(pending_, status_)) {
                restartPending_ = true;
                open_ = false;
            }
            break;
        case I_CLOSE:
            open_ = false;
            break;
        default:
            change(it, 1);
            break;
        }
    }

    void openFiles(MenuFileKind kind, int drive) {
        fileKind_ = kind;
        fileDrive_ = drive;
        files_.clear();
        host_.listFiles(kind, files_);
        fileCursor_ = 0;
        // いま選んでいるイメージに合わせる
        const char* cur = kind == MenuFileKind::FLOPPY      ? pending_.fd[drive]
                          : kind == MenuFileKind::CDROM     ? pending_.cd
                          : kind == MenuFileKind::SCSI_DISK ? pending_.sc[drive]
                                                            : pending_.hd0;
        for (int i = 0; i < files_.count(); i++)
            if (strcmp(files_.name(i), baseName(cur)) == 0) fileCursor_ = i + 1;
        page_ = FILES;
        status_.clear();
        if (files_.full()) status_.add("too many files; some are not listed");
    }

    void fileKey(uint8_t u) {
        const int total = files_.count() + 1;
        switch (u) {
        case menu_key::UP:
            fileCursor_ = (fileCursor_ + total - 1) % total;
            break;
        case menu_key::DOWN:
            fileCursor_ = (fileCursor_ + 1) % total;
            break;
        case menu_key::PAGE_UP:
            fileCursor_ = fileCursor_ > 10 ? fileCursor_ - 10 : 0;
            break;
        case menu_key::PAGE_DOWN:
            fileCursor_ = fileCursor_ + 10 < total ? fileCursor_ + 10 : total - 1;
            break;
        case menu_key::HOME:
            fileCursor_ = 0;
            break;
        case menu_key::END:
            fileCursor_ = total - 1;
            break;
        case menu_key::ENTER:
        case menu_key::KP_ENTER:
            chooseFile();
            break;
        case menu_key::ESC:
        case menu_key::BACKSPACE:
        case menu_key::LEFT:
            page_ = MAIN;
            status_.clear();
            break;
        default:
            if (isMenuKey(u, true)) open_ = false;
            break;
        }
    }

    void chooseFile() {
        const char* name = fileCursor_ == 0 ? "" : files_.name(fileCursor_ - 1);
        status_.clear();
        host_.beginAction(fileKind_ == MenuFileKind::FLOPPY && !name[0] ? MENU_ACTION_EJECT + fileDrive_
                                                                         : MENU_ACTION_OTHER);
        if (fileKind_ == MenuFileKind::FLOPPY) {
            if (host_.insertDisk(fileDrive_, name, status_)) {
                copyString(pending_.fd[fileDrive_], Settings::PATH_SIZE, host_.diskName(fileDrive_));
                copyString(running_.fd[fileDrive_], Settings::PATH_SIZE, host_.diskName(fileDrive_));
                if (status_.str()[0] == 0) {
                    status_.add(fileDrive_ ? "FD1: " : "FD0: ");
                    status_.add(name[0] ? "inserted" : "ejected");
                }
            }
        } else if (fileKind_ == MenuFileKind::CDROM) {
            // CD-ROM 装置をつないでいれば、媒体をすぐ入れ替える (X68000 には、媒体が替わったことが伝わる)。
            // つないでいなければ、次の起動から
            const int r = host_.changeCd(name, status_);
            if (r > 0) {
                copyString(pending_.cd, Settings::PATH_SIZE, name);
                copyString(running_.cd, Settings::PATH_SIZE, name);
                if (status_.str()[0] == 0) status_.add(name[0] ? "CD-ROM: inserted" : "CD-ROM: ejected");
            } else if (r == -2) {
                // 装置は空になった: 設定も、媒体なしにする
                pending_.cd[0] = 0;
                running_.cd[0] = 0;
            } else if (r == 0) {
                copyString(pending_.cd, Settings::PATH_SIZE, name);
                // CD-ROM は SCSI につなぐ (68000 の機種は外付け SCSI ボードが要る)
                if (name[0] && pending_.cpu == 68000 && !pending_.scsiEx) status_.add("CD-ROM needs scsi_ex=on or X68030");
                else status_.add("CD-ROM is changed after Restart");
            }
        } else if (fileKind_ == MenuFileKind::SCSI_DISK) {
            copyString(pending_.sc[fileDrive_], Settings::PATH_SIZE, name);
            // SCSI のハードディスクも、68000 の機種は外付け SCSI ボードが要る。CD-ROM と同じ ID なら CD-ROM を使う
            if (name[0] && pending_.cpu == 68000 && !pending_.scsiEx)
                status_.add("SC").add((long)fileDrive_).add(" needs scsi_ex=on or X68030");
            else if (name[0] && pending_.cd[0] && pending_.cdId == fileDrive_)
                status_.add("SC").add((long)fileDrive_).add(" is used by the CD-ROM");
            else status_.add("SC").add((long)fileDrive_).add(" is changed after Restart");
        } else {
            copyString(pending_.hd0, Settings::PATH_SIZE, name);
            status_.add("HD0 is changed after Restart");
        }
        page_ = MAIN;
    }

    MenuHost& host_;
    bool open_ = false;
    bool restartPending_ = false;
    Page page_ = MAIN;
    int cursor_ = 0;
    int mainCursor_ = 0;               // 拡張ボードか画面の出し方のページに入る前のカーソル
    const char* midiHint_ = nullptr;   // 状態の行に出した midiHint() の案内 (出していなければ nullptr)
    int slot_ = 0;
    Settings running_, pending_;
    MenuFileList files_;
    MenuFileKind fileKind_ = MenuFileKind::FLOPPY;
    int fileDrive_ = 0;
    int fileCursor_ = 0;
    char statusBuf_[LINE_BYTES];
    MenuText status_;
};

} // namespace platform
