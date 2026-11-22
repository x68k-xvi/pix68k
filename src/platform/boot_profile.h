// 起動の組み合わせ (プロファイル): /pix68k/pix68k-<名前>.cfg を起動時に選ぶ (#231)。
//
// ファイル: pix68k.cfg と同じ書式。1 行目が「#」で始まれば、その残りをメニューに出す題にする (なければ名前だけ)。
// 名前は英数字と - と _ だけ (PROFILE_NAME_MAX 字まで)。ほかの名前のファイルは一覧に出さない。
//
// pix68k.cfg との重ね方: 組み合わせを選んだときは、pix68k.cfg からは「共通の項目」(isCommonSettingKey()。ネットワーク、
// キーボード、ゲームパッド、画面の出し方、コアの割り当てなど、その Pi と周りの機器で決まるもの) だけを受け継ぎ、
// X68000 の構成 (CPU、メモリ、ディスク、拡張ボード、MIDI、autotype) は組み合わせのファイルだけから読む
// (書いていない項目は既定の値)。pix68k.cfg の fd0= などが組み合わせに紛れ込まないよう、また、今まで
// 「pix68k.cfg に写して使う」ために置いていた控えのファイルが、写したときと同じ構成になるようにするため。
// 組み合わせのファイルに共通の項目を書けば、そちらが勝つ。
//
// 前回の選択: /pix68k/profile.sel (1 行目に名前。空なら pix68k.cfg だけ)。メニューで前と違うものを選んだときと、
// HTTP の POST /reboot?profile= のときだけ書く。HTTP のときは 2 行目に once を書き、次の起動はメニューを出さずに
// その組み合わせで起動して、once を消す。
//
// 起動時のメニュー (BootMenu): 一覧の先頭は pix68k.cfg (組み合わせなし)。↑↓ で選び、Enter で決める。profile_wait= 秒
// 何も押さなければ前回のもの。何か押したら待つのをやめる。
// 題の字は UTF-8 か Shift_JIS (profileTitle())。ASCII を platform/menu_font.h、半角カタカナと全角を CGROM で描く
// (platform/text_cells.h)。
//
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/config_text.h"
#include "platform/file_replace.h"
#include "platform/menu.h"
#include "platform/osd_message.h"
#include "platform/settings.h"
#include "platform/settings_file.h"
#include "platform/text_cells.h"

namespace platform {

static constexpr int PROFILE_NAME_MAX = 32;   // 名前の長さの上限 (字)
static constexpr int PROFILE_TITLE_MAX = 120; // 題の長さの上限 (バイト。字の切れ目で切る)
static constexpr int PROFILE_MAX = 30;        // 一覧に出す数の上限 (名前の順で先のもの)
static constexpr const char* PROFILE_CHOICE_FILE = "profile.sel";
static constexpr const char* PROFILE_CHOICE_TMP = "profile.new";

// POST /reboot?profile= の名前を確かめた結果
// WRITE_FAILED: 次の起動の組み合わせを profile.sel に書けなかった (再起動しない)
enum class ProfileRequest { OK, BAD_NAME, NOT_FOUND, NO_DRIVE, WRITE_FAILED };

namespace profile_detail {
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
inline bool equalNoCase(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (lower(*a) != lower(*b)) return false;
    return *a == *b;
}
inline int compareNoCase(const char* a, const char* b) {
    for (;; a++, b++) {
        const char x = lower(*a), y = lower(*b);
        if (x != y || x == 0) return (unsigned char)x - (unsigned char)y;
    }
}
} // namespace profile_detail

// 組み合わせの名前として使えるか (英数字、-、_ だけで、1 字から PROFILE_NAME_MAX 字)
inline bool validProfileName(const char* s) {
    int n = 0;
    for (; s[n]; n++) {
        const char c = s[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok || n >= PROFILE_NAME_MAX) return false;
    }
    return n > 0;
}

// ファイル名 pix68k-<名前>.cfg (大文字と小文字は区別しない) から名前を取り出す。違う形か、名前が使えなければ false。
// tmp を渡すと、書き直しの一時ファイル pix68k-<名前>.new も受け付け、どちらだったかを *tmp に入れる
// (保存の途中で止まって .new だけが残った組み合わせも一覧に出すため)
inline bool profileNameFromFile(const char* file, char* name, size_t size, bool* tmp = nullptr) {
    static const char kPrefix[] = "pix68k-", kCfg[] = ".cfg", kNew[] = ".new";
    const size_t n = strlen(file), pn = sizeof kPrefix - 1, sn = sizeof kCfg - 1;
    if (n <= pn + sn) return false;
    for (size_t i = 0; i < pn; i++)
        if (profile_detail::lower(file[i]) != kPrefix[i]) return false;
    bool isCfg = true, isNew = tmp != nullptr;
    for (size_t i = 0; i < sn; i++) {
        const char c = profile_detail::lower(file[n - sn + i]);
        isCfg = isCfg && c == kCfg[i];
        isNew = isNew && c == kNew[i];
    }
    if (!isCfg && !isNew) return false;
    if (tmp != nullptr) *tmp = !isCfg;
    const size_t len = n - pn - sn;
    if (len + 1 > size) return false;
    memcpy(name, file + pn, len);
    name[len] = 0;
    return validProfileName(name);
}

// 名前から組み合わせのファイル名 (pix68k-<名前>.cfg) と、書き直すときの一時ファイル名 (pix68k-<名前>.new) を作る
inline bool profileFileName(const char* name, char* out, size_t size, bool tmp = false) {
    if (!validProfileName(name)) return false;
    MenuText t(out, size);
    t.add("pix68k-").add(name).add(tmp ? ".new" : ".cfg");
    return strlen(out) == strlen(name) + 11;
}

// ---------------------------------------------------------------- 題の字 (UTF-8 か Shift_JIS)

namespace profile_detail {
// s[0..n) が、続くバイト次第で正しい UTF-8 の 1 字になりうる途中までの並びか (n は 1 以上で、その字の長さより短い)。
// 1 バイト目ごとの 2 バイト目の範囲で、長すぎる形 (C0、C1、E0 80 など)、サロゲート (ED A0 から)、0x10FFFF より大きいもの
// (F4 90 から、F5 から) を除く
inline bool utf8Prefix(const uint8_t* s, size_t n) {
    const uint8_t c = s[0];
    size_t len;
    uint8_t lo = 0x80, hi = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) len = 2;
    else if (c >= 0xe0 && c <= 0xef) {
        len = 3;
        if (c == 0xe0) lo = 0xa0;
        if (c == 0xed) hi = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        len = 4;
        if (c == 0xf0) lo = 0x90;
        if (c == 0xf4) hi = 0x8f;
    } else {
        return false;
    }
    if (n >= len) return false;
    if (n >= 2 && (s[1] < lo || s[1] > hi)) return false;
    for (size_t k = 2; k < n; k++)
        if ((s[k] & 0xc0) != 0x80) return false;
    return true;
}

// s[0..n) が正しい UTF-8 か (utf8Next() が U+FFFD に置き換える並びがないか)。partial なら、終わりで切れた 1 字は、
// 続くバイト次第で正しい UTF-8 になりうるときだけ許す (ファイルの頭だけを読み、1 行目がその終わりで切れているとき)
inline bool utf8Valid(const char* s, size_t n, bool partial) {
    size_t i = 0;
    while (i < n) {
        const uint8_t c = (uint8_t)s[i];
        size_t at = i;
        uint32_t cp;
        utf8Next(s, n, at, cp);
        if (c >= 0x80 && at == i + 1) return partial && utf8Prefix((const uint8_t*)s + i, n - i);
        i = at;
    }
    return true;
}

// Shift_JIS の s の先頭 max バイト以内で、字の切れ目になるいちばん長い長さ (sjisNext() で読む切れ目)
inline size_t sjisTruncate(const char* s, size_t n, size_t max) {
    if (n <= max) return n;
    size_t i = 0, last = 0;
    uint16_t code;
    int width;
    while (text_detail::sjisNext(s, n, i, code, width) && i <= max) last = i;
    return last;
}
} // namespace profile_detail

// ファイルの 1 行目が「#」で始まれば、その残り (前後の空白を除く) を out に入れる。題がなければ false (out は空)。
// 字は UTF-8 か Shift_JIS: 1 行目が正しい UTF-8 ならそのまま、そうでなければ Shift_JIS とみなして *sjis を true にする
// (ASCII だけの行はどちらでも同じなので UTF-8 とする)。UTF-8 の BOM があれば飛ばして UTF-8 とする。
// 見分けは切り詰める前の行全体で行う (切ったあとの Shift_JIS が、たまたま正しい UTF-8 になることがあるため)。
// 半角カタカナ 2 字の並びの一部 (0xC2-0xDF のあとに 0xA1-0xBF) は正しい UTF-8 でもあるので、その字だけの題は
// UTF-8 とみなす。
// partial: data がファイルの頭だけで、続きがある (読む上限で止まった)。1 行目が data の終わりで切れていれば、最後の
// 欠けた 1 字を UTF-8 の誤りとみなさない。ファイルの終わりまで読んだときは false にする
inline bool profileTitle(const char* data, size_t n, char* out, size_t size, bool* sjis = nullptr,
                         bool partial = false) {
    if (sjis != nullptr) *sjis = false;
    if (size == 0) return false;
    out[0] = 0;
    size_t i = 0;
    bool bom = false;
    if (n >= 3 && (uint8_t)data[0] == 0xef && (uint8_t)data[1] == 0xbb && (uint8_t)data[2] == 0xbf) {
        i = 3;
        bom = true;
    }
    if (i >= n || data[i] != '#') return false;
    i++;
    while (i < n && (data[i] == ' ' || data[i] == '\t' || data[i] == '#')) i++;
    size_t e = i;
    while (e < n && data[e] != '\r' && data[e] != '\n') e++;
    const size_t lineEnd = e;
    while (e > i && (data[e - 1] == ' ' || data[e - 1] == '\t')) e--;
    // 1 行目が読む上限で切れていて、最後の字のあとに空白もないときだけ、その字が欠けているとみなせる
    // (空白が続いていれば、その前の字は欠けた UTF-8 の字ではない)
    const bool cutOff = partial && lineEnd == n && e == n;
    size_t len = e - i;
    const bool isSjis = !bom && !profile_detail::utf8Valid(data + i, len, cutOff);
    const size_t max = size - 1 < (size_t)PROFILE_TITLE_MAX ? size - 1 : (size_t)PROFILE_TITLE_MAX;
    len = isSjis ? profile_detail::sjisTruncate(data + i, len, max) : utf8Truncate(data + i, len, max);
    memcpy(out, data + i, len);
    out[len] = 0;
    if (sjis != nullptr) *sjis = isSjis && len > 0;
    return len > 0;
}

// 組み合わせを選んだときも pix68k.cfg から受け継ぐ「共通の項目」か (上の説明を参照)
inline bool isCommonSettingKey(const char* key) {
    static const char* const kCommon[] = {"net",       "net_ip",    "net_gw",           "net_dns",    "net_name",
                                          "net_allow", "net_token", "syslog",           "keyboard",   "hat",       "pad_log",
                                          "video_filter", "video_scale", "osd",       "render_core", "sound_core",
                                          "midi_uart", "midi_uart_family", "write_back", "profile_wait",
                                          // 前の書き方 (v0.1.0 の scale= と scanline= は読み替え、crt= は知らせて無視する)。
                                          // pix68k.cfg に残っていても受け継ぐ
                                          "scale",     "scanline",  "crt"};
    for (const char* k : kCommon)
        if (strcmp(k, key) == 0) return true;
    return false;
}

// 設定を読む。profile が nullptr なら base (pix68k.cfg) のすべての行、そうでなければ base の共通の項目だけを
// 読んでから profile のすべての行を読む。warn と bad は platform::readSettings() と同じ。fromProfile は、
// どちらのファイルの行かを知らせる (base の行は 1 回目に読んだときに知らせてあれば、warnBase = false で黙らせる)。
// defaults は書いていない項目の値 (ホスト版は video_scale の既定が aspect)
template <typename Warn, typename Bad>
void readProfileSettings(const ConfigText& base, const ConfigText* profile, Settings& s, bool warnBase, Warn&& warn,
                         Bad&& bad, const Settings& defaults = Settings()) {
    s = defaults;
    base.forEach(
        [&](const ConfigText::Entry& e) {
            if (profile != nullptr && !isCommonSettingKey(e.key)) {
                // 受け継がない項目も、知らない名前 (打ち間違い) は知らせる
                if (warnBase && findSettingKey(e.key) == nullptr)
                    warn(false, (const char*)e.key, (const char*)e.value, SettingResult::UNKNOWN_KEY);
                return;
            }
            const SettingResult r = applySetting(s, e.key, e.value);
            if (r != SettingResult::OK && warnBase) warn(false, (const char*)e.key, (const char*)e.value, r);
        },
        [&](const char* line) {
            if (warnBase) bad(false, line);
        });
    if (profile != nullptr)
        profile->forEach(
            [&](const ConfigText::Entry& e) {
                const SettingResult r = applySetting(s, e.key, e.value);
                if (r != SettingResult::OK) warn(true, (const char*)e.key, (const char*)e.value, r);
            },
            [&](const char* line) { bad(true, line); });
    // 前の書き方は、2 つのファイルを重ねて読んだあとで読み替える (pix68k.cfg の scanline=on と組み合わせの scanline=off なら効果なし)
    resolveLegacySettings(s);
}

// 組み合わせで動いているときの「Save settings」: s のうち saved (前に読んだか書いた値) と違う項目を、共通の項目は
// base (pix68k.cfg) に、そのほかは profile に書く (行があれば値を書き換え、なければ足す)。共通の項目でも profile に
// 行があれば profile に書く (次の起動では profile の行が勝つので)。書けなければ false。
// changedBase / changedProfile に、それぞれのファイルを書き換えたかを入れる。defaults は readProfileSettings() と同じ
inline bool writeProfileSettings(ConfigText& base, ConfigText& profile, const Settings& s, const Settings& saved,
                                 bool& changedBase, bool& changedProfile, const Settings& defaults = Settings()) {
    bool ok = true;
    changedBase = changedProfile = false;
    // v0.1.0 の書き方の行 (scale=、scanline=) は、どちらのファイルでもいまの書き方にする (platform/settings_file.h)。
    // pix68k.cfg は pix68k.cfg だけを読んだ値で、組み合わせのファイルは重ねて読んだ値 (saved) で書く (組み合わせの行は
    // pix68k.cfg の行に勝つので、重ねた値がそのまま次の起動の値になる)。組み合わせのファイルに書いた video_filter= などは、
    // 変えたときもそちらに書く (下の profile.get())
    Settings baseOnly = defaults;
    readSettings(base, baseOnly, [](const char*, const char*, SettingResult) {}, [](const char*) {});
    const int migratedBase = migrateLegacySettings(base, baseOnly), migratedProfile = migrateLegacySettings(profile, saved);
    if (migratedBase < 0 || migratedProfile < 0) ok = false;
    if (migratedBase > 0) changedBase = true;
    if (migratedProfile > 0) changedProfile = true;
    for (const SettingKey& k : kSettingKeys) {
        if (sameSetting(s, saved, k)) continue;
        char v[Settings::PATH_SIZE];
        if (!formatSetting(s, k, v, sizeof v)) continue;
        char cur[Settings::PATH_SIZE];
        const bool toBase = isCommonSettingKey(k.key) && !profile.get(k.key, cur, sizeof cur);
        ok = (toBase ? base : profile).set(k.key, v) && ok;
        (toBase ? changedBase : changedProfile) = true;
    }
    return ok;
}

// ---------------------------------------------------------------- 一覧

class ProfileList {
public:
    struct Entry {
        char name[PROFILE_NAME_MAX + 1];
        char title[PROFILE_TITLE_MAX + 1];
        bool sjis; // title が Shift_JIS (profileTitle() の *sjis)
    };
    ProfileList() {}
    ~ProfileList() { delete[] entries_; }
    ProfileList(const ProfileList&) = delete;
    ProfileList& operator=(const ProfileList&) = delete;

    // 上限を超えても捨てない名前 (前回の選択)。add() の前に呼ぶ。空なら何もしない
    void keep(const char* name) { copyString(keep_, sizeof keep_, name ? name : ""); }

    // 名前の順 (大文字と小文字を区別しない) に入れる。同じ名前はあとから足したものを捨てる。
    // PROFILE_MAX を超えたら、名前の順で後ろのものを捨てる (keep() の名前は捨てない)。メモリが足りなければ false。
    // sjis は title が Shift_JIS か
    bool add(const char* name, const char* title, bool sjis = false) {
        if (!validProfileName(name)) return false;
        if (entries_ == nullptr) {
            entries_ = new Entry[PROFILE_MAX];
            if (entries_ == nullptr) return false;
        }
        if (find(name) >= 0) return true;
        int at = count_;
        while (at > 0 && profile_detail::compareNoCase(entries_[at - 1].name, name) > 0) at--;
        if (count_ == PROFILE_MAX) {
            // 捨てるのは名前の順で最後のもの。ただし keep() の名前はその 1 つ前を捨てる
            int victim = count_ - 1;
            if (isKept(entries_[victim].name)) victim--;
            dropped_++;
            if (at > victim && !isKept(name)) return true; // 足すものが捨てる側
            for (int i = victim; i < count_ - 1; i++) entries_[i] = entries_[i + 1];
            count_--;
            if (at > victim) at--;
        }
        for (int i = count_; i > at; i--) entries_[i] = entries_[i - 1];
        copyString(entries_[at].name, sizeof entries_[at].name, name);
        copyString(entries_[at].title, sizeof entries_[at].title, title ? title : "");
        entries_[at].sjis = sjis && title != nullptr && title[0] != 0;
        count_++;
        return true;
    }
    int count() const { return count_; }
    int dropped() const { return dropped_; }
    const Entry& at(int i) const { return entries_[i]; }
    // 名前 (大文字と小文字を区別しない) の位置。なければ -1
    int find(const char* name) const {
        for (int i = 0; i < count_; i++)
            if (profile_detail::equalNoCase(entries_[i].name, name)) return i;
        return -1;
    }

private:
    bool isKept(const char* name) const { return keep_[0] != 0 && profile_detail::equalNoCase(keep_, name); }
    Entry* entries_ = nullptr;
    int count_ = 0, dropped_ = 0;
    char keep_[PROFILE_NAME_MAX + 1] = {};
};

// ---------------------------------------------------------------- 前回の選択 (profile.sel)

// profile.sel の中身を読む。1 行目が名前 (空なら pix68k.cfg だけ)、2 行目が once なら HTTP で頼まれた 1 回だけの起動。
// 名前が使えない形なら false (name は空、once は false)
inline bool parseProfileChoice(const char* data, size_t n, char* name, size_t size, bool& once) {
    once = false;
    if (size == 0) return false;
    name[0] = 0;
    size_t i = 0;
    if (n >= 3 && (uint8_t)data[0] == 0xef && (uint8_t)data[1] == 0xbb && (uint8_t)data[2] == 0xbf) i = 3;
    char lines[2][PROFILE_NAME_MAX + 2] = {};
    for (int l = 0; l < 2 && i < n; l++) {
        size_t e = i;
        while (e < n && data[e] != '\n') e++;
        size_t s = i, t = e;
        while (s < t && (data[s] == ' ' || data[s] == '\t')) s++;
        while (t > s && (data[t - 1] == ' ' || data[t - 1] == '\t' || data[t - 1] == '\r')) t--;
        if (t - s > PROFILE_NAME_MAX) {
            if (l == 0) return false;
        } else {
            memcpy(lines[l], data + s, t - s);
        }
        i = e + 1;
    }
    if (lines[0][0] != 0 && !validProfileName(lines[0])) return false;
    copyString(name, size, lines[0]);
    once = strcmp(lines[1], "once") == 0;
    return true;
}

// profile.sel に書く中身。out の長さを返す (名前が使えなければ 0)
inline size_t formatProfileChoice(const char* name, bool once, char* out, size_t size) {
    if (name[0] != 0 && !validProfileName(name)) return 0;
    MenuText t(out, size);
    t.add(name).add("\n");
    if (once) t.add("once\n");
    return strlen(out);
}

// 次の起動が読む前回の選択 (ChooseProfile() と同じ読み方: path があれば path、なければ tmp。どちらもなければ空)。
// known が false なら、読めなかったか、読んだ中身が分からない (once が入っているかもしれない)
struct ProfileChoiceRead {
    bool known = false;
    bool once = false;
    bool fromTmp = false; // tmp を読んだ (path がない)
    char name[PROFILE_NAME_MAX + 1] = {};
};

// Fs に FsResult read(const char* path, char* buf, unsigned cap, unsigned& n) が要る (ない: NO_FILE、
// 読めない: ERROR。cap を超えるファイルは n == cap で返してよい)
template <class Fs> ProfileChoiceRead readProfileChoice(Fs& fs, const char* path, const char* tmp) {
    ProfileChoiceRead c;
    char buf[128];
    unsigned n = 0;
    FsResult r = fs.read(path, buf, sizeof buf, n);
    if (r == FsResult::NO_FILE) {
        c.fromTmp = true;
        r = fs.read(tmp, buf, sizeof buf, n);
        if (r == FsResult::NO_FILE) {
            c.known = true; // どちらもない: 前回の選択なし
            return c;
        }
    }
    if (r != FsResult::OK || n >= sizeof buf) return c; // 読めないか、profile.sel にしては大きすぎる
    // ChooseProfile() と同じく、名前として読めなければ使わない (once もない)
    if (!parseProfileChoice(buf, n, c.name, sizeof c.name, c.once)) {
        c.name[0] = 0;
        c.once = false;
    }
    c.known = true;
    return c;
}

// POST /reboot?profile= の要求を profile.sel に once 付きで書くことと、その取り消し (ベアメタル版)。
// 書き方は platform/file_replace.h の replaceFile() (path = profile.sel、tmp = profile.new)。
// 書けたか、取り消せたかは、ファイルの操作の結果からは推し量らず、次の起動と同じ読み方で読み直して決める
// (readProfileChoice())。読み直して once が入っていない (取り消しなら、once のない中身) と分かったときだけ
// 「残っていない」とし、読めないか once が入っていれば pending() のまま残して、あとの cancel() でもう一度試す。
// Fs は replaceFile() のもの (exists、write、remove、rename) と read
class ProfileChoiceStore {
public:
    // profile.sel に書いてある前回の選択 (once のない形)。取り消すときはこれに戻す
    void setSaved(const char* name) { copyString(saved_, sizeof saved_, name ? name : ""); }
    const char* saved() const { return saved_; }
    // once 付きの要求が、次の起動で読まれるかもしれない (取り消すまで残る)
    bool pending() const { return pending_; }

    // once 付きで name を書く。書けなければ false (読み直して once が残っていそうなら、取り消してから返す)
    template <class Fs> bool request(Fs& fs, const char* path, const char* tmp, const char* name) {
        if (write(fs, path, tmp, name, true)) {
            pending_ = true;
            return true;
        }
        const ProfileChoiceRead c = readProfileChoice(fs, path, tmp);
        if (!c.known || c.once) {
            pending_ = true;
            cancel(fs, path, tmp);
        }
        return false;
    }
    // once のない前回の選択に戻す。読み直して once が残っていないと分かったら true。
    // 前回の選択に戻せず、once のない別の中身 (空など) が読まれるときも true (断った要求は使われない)
    template <class Fs> bool cancel(Fs& fs, const char* path, const char* tmp) {
        if (!pending_) return true;
        ProfileChoiceRead c = readProfileChoice(fs, path, tmp);
        if (!(c.known && !c.once && profile_detail::equalNoCase(c.name, saved_))) {
            // once の入った tmp だけが読まれるなら、先に消す (replaceFile() は tmp の名前を path に戻してから書くので、
            // そのあと書けないと once が path に残る)
            if (c.known && c.fromTmp) fs.remove(tmp);
            write(fs, path, tmp, saved_, false);
            c = readProfileChoice(fs, path, tmp);
        }
        if (!c.known || c.once) return false;
        pending_ = false;
        return true;
    }

private:
    template <class Fs> static bool write(Fs& fs, const char* path, const char* tmp, const char* name, bool once) {
        char data[PROFILE_NAME_MAX + 16];
        const size_t n = formatProfileChoice(name, once, data, sizeof data);
        return n != 0 && replaceFile(fs, path, tmp, data, (unsigned)n) == FsResult::OK;
    }
    char saved_[PROFILE_NAME_MAX + 1] = {};
    bool pending_ = false;
};

// ---------------------------------------------------------------- 起動時のメニュー

class BootMenu {
public:
    static constexpr int COLS = 60; // 枠の中の桁数

    // list の組み合わせを出す。initial は最初に選ぶ行 (0 は pix68k.cfg、i + 1 は list の i 番目)。
    // waitMs たったら選んでいる行で起動する
    void open(const ProfileList* list, int initial, uint32_t nowMs, uint32_t waitMs) {
        list_ = list;
        cursor_ = initial >= 0 && initial < rows() ? initial : 0;
        deadline_ = nowMs + waitMs;
        waiting_ = true;
    }
    int rows() const { return 1 + (list_ ? list_->count() : 0); }
    int cursor() const { return cursor_; }
    bool waiting() const { return waiting_; }
    // 待つ時間を、少なくとも untilMs までにする (USB の装置を探していた間に時間が過ぎたとき)
    void holdUntil(uint32_t untilMs) {
        if (waiting_ && (int32_t)(untilMs - deadline_) > 0) deadline_ = untilMs;
    }
    // 残りの秒数 (切り上げ)。待つのをやめていれば -1
    int secondsLeft(uint32_t nowMs) const {
        if (!waiting_) return -1;
        const int32_t left = (int32_t)(deadline_ - nowMs);
        return left <= 0 ? 0 : (int)((left + 999) / 1000);
    }
    // キー (USB の usage) を渡す。決めたら行の番号、まだなら -1。何か押したら待つのをやめる
    int key(uint8_t usage) {
        namespace mk = menu_key;
        waiting_ = false;
        const int last = rows() - 1;
        switch (usage) {
        case mk::UP: cursor_ = cursor_ > 0 ? cursor_ - 1 : last; break;
        case mk::DOWN: cursor_ = cursor_ < last ? cursor_ + 1 : 0; break;
        case mk::HOME:
        case mk::PAGE_UP: cursor_ = 0; break;
        case mk::END:
        case mk::PAGE_DOWN: cursor_ = last; break;
        case mk::ENTER:
        case mk::KP_ENTER: return cursor_;
        default: break;
        }
        return -1;
    }
    // 時間が来たら選んでいる行、まだなら -1
    int tick(uint32_t nowMs) const {
        if (waiting_ && (int32_t)(nowMs - deadline_) >= 0) return cursor_;
        return -1;
    }

    // 行の名前 (pix68k.cfg か、組み合わせの名前)
    const char* rowName(int r) const { return r == 0 ? "pix68k.cfg" : list_->at(r - 1).name; }

    // pixels の width x height に描く (全体を塗る)。opaque は書く画素に OR する
    void draw(uint32_t* pixels, int pitch, int width, int height, uint32_t nowMs, const uint8_t* cgrom,
              size_t cgromSize, uint32_t opaque = 0) const {
        if (width < 16 || height < 16) return;
        for (int y = 0; y < height; y++) {
            uint32_t* row = pixels + (size_t)y * pitch;
            for (int x = 0; x < width; x++) row[x] = opaque;
        }
        const int lines = rows() + 4; // 題、空行、行、空行、案内
        int scale = 1;
        while (scale < 3 && (COLS + 2) * 8 * (scale + 1) <= width && (lines + 2) * 16 * (scale + 1) <= height) scale++;
        int cols = COLS;
        if ((cols + 2) * 8 * scale > width) cols = width / (8 * scale) - 2;
        if (cols < 8) return;
        int fit = height / (16 * scale) - 1;
        // 行が入りきらなければ、選んでいる行が入るように出す範囲を動かす
        const int body = rows();
        const int bodyFit = fit - 4 < body ? (fit - 4 > 1 ? fit - 4 : 1) : body;
        int first = 0;
        if (cursor_ >= bodyFit) first = cursor_ - bodyFit + 1;
        const int shown = bodyFit + 4;
        const int boxW = (cols + 2) * 8 * scale, boxH = (shown + 1) * 16 * scale;
        const int x0 = (width - boxW) / 2 > 0 ? (width - boxW) / 2 : 0;
        const int y0 = (height - boxH) / 2 > 0 ? (height - boxH) / 2 : 0;
        const int x1 = x0 + boxW < width ? x0 + boxW : width, y1 = y0 + boxH < height ? y0 + boxH : height;
        for (int y = y0; y < y1; y++) {
            uint32_t* row = pixels + (size_t)y * pitch;
            for (int x = x0; x < x1; x++) {
                const bool border = y == y0 || y == y1 - 1 || x == x0 || x == x1 - 1;
                row[x] = (border ? 0xc0c0c0u : 0x101028u) | opaque;
            }
        }
        uint16_t cells[COLS + 2];
        auto text = [&](int line, const char* s, uint32_t color, bool sjis = false) {
            const int n = layoutLine(s, cells, cols, sjis);
            drawCells(pixels, pitch, x1, y1, x0 + 8 * scale, y0 + (8 + line * 16) * scale, cells, n, scale,
                      color | opaque, cgrom, cgromSize);
        };
        text(0, "pix68k: choose a boot profile", 0xffe080u);
        char buf[PROFILE_NAME_MAX + PROFILE_TITLE_MAX + 8];
        for (int i = 0; i < bodyFit && first + i < body; i++) {
            const int r = first + i, line = 2 + i;
            if (r == cursor_) {
                const int ty = y0 + (8 + line * 16) * scale;
                for (int y = ty; y < ty + 16 * scale && y < y1 - 1; y++) {
                    uint32_t* row = pixels + (size_t)y * pitch;
                    for (int x = x0 + 4 * scale; x < x1 - 4 * scale; x++) row[x] = 0x2050b0u | opaque;
                }
            }
            MenuText t(buf, sizeof buf);
            t.add(rowName(r));
            const char* title = r == 0 ? "" : list_->at(r - 1).title;
            const bool sjis = r != 0 && list_->at(r - 1).sjis; // 名前と空白は ASCII なので、行ごと題の字で読める
            if (title[0]) {
                for (int k = (int)strlen(buf); k < 12; k++) t.add(" ");
                t.add(" ").add(title);
            }
            text(line, buf, 0xffffffu, sjis);
        }
        MenuText t(buf, sizeof buf);
        t.add("Up/Down, Enter");
        const int left = secondsLeft(nowMs);
        if (left >= 0) t.add("   starting in ").add((long)left).add(" s");
        text(shown - 1, buf, 0x909090u);
    }

private:
    const ProfileList* list_ = nullptr;
    int cursor_ = 0;
    uint32_t deadline_ = 0;
    bool waiting_ = false;
};

// ゲームパッドのジョイスティックのビット (x68k::Ppi::JOY_*) の変化を、メニューのキーにする。
// 上 / 下は ↑ / ↓、A / B は Enter。押したときだけ (out は 3 個分)。キーの数を返す
inline int padMenuKeys(uint8_t prev, uint8_t now, uint8_t out[3]) {
    const uint8_t pressed = (uint8_t)(now & ~prev);
    int n = 0;
    if (pressed & 0x01) out[n++] = menu_key::UP;   // JOY_UP
    if (pressed & 0x02) out[n++] = menu_key::DOWN; // JOY_DOWN
    if (pressed & 0x30) out[n++] = menu_key::ENTER; // JOY_A、JOY_B
    return n;
}

// メニューの間の 1 つのゲームパッドの枠。つないだときから押したままのボタンで、待つのをやめたり決めたりしないように、
// つないで (メニューより前につないだものは、USB を探し終えて) window 以内に来た最初のレポートを、押す前の状態にする。
// 変わったときだけレポートを送るパッドでは最初のレポートが押したときなので、window より後の最初のレポートは、
// いつもどおり押したとみなす。つなぎ直したら (attach が変わったら) 初めから。
// reports はその枠に来たレポートの数 (つないだときに 0 に戻す)、bits はそのときのビット。時刻の単位は呼ぶ側が決める
class PadStartFilter {
public:
    explicit PadStartFilter(uint64_t window) : window_(window) {}
    // メニューを出し始めた (USB を探し終えた) 時刻
    void start(uint64_t scanEnd) {
        scanEnd_ = scanEnd;
        attach_ = 0;
        seen_ = false;
        prev_ = 0;
    }
    // 1 周ごとに呼ぶ。メニューのキー (padMenuKeys()) の数を返す
    int keys(uint64_t attach, unsigned reports, uint8_t bits, uint64_t now, uint8_t out[3]) {
        if (attach != attach_) {
            attach_ = attach;
            seen_ = false;
            prev_ = 0;
        }
        if (!seen_) {
            if (reports == 0) return 0;
            seen_ = true;
            const uint64_t from = attach > scanEnd_ ? attach : scanEnd_;
            if (now <= from + window_) {
                prev_ = bits;
                return 0;
            }
        }
        const int n = padMenuKeys(prev_, bits, out);
        prev_ = bits;
        return n;
    }

private:
    uint64_t window_, scanEnd_ = 0, attach_ = 0;
    bool seen_ = false;
    uint8_t prev_ = 0;
};

} // namespace platform
