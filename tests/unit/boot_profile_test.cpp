// src/platform/boot_profile.h のテスト: 名前の取り出し、題、pix68k.cfg との重ね方、保存の振り分け、一覧、
// 前回の選択 (profile.sel)、起動時のメニューの操作と描画、autotype (platform/key_type.h の AutoTyper)
#include "platform/boot_profile.h"
#include "platform/key_type.h"
#include "platform/net_http.h"
#include "test_check.h"

#include <stdio.h>
#include <string.h>

namespace {

using namespace platform;
namespace mk = platform::menu_key;

void testNames() {
    char name[64];
    CHECK_EQ(profileNameFromFile("pix68k-gr2.cfg", name, sizeof name), 1);
    CHECK_EQ(strcmp(name, "gr2"), 0);
    CHECK_EQ(profileNameFromFile("PIX68K-CBX.CFG", name, sizeof name), 1); // FAT の短い名前
    CHECK_EQ(strcmp(name, "CBX"), 0);
    CHECK_EQ(profileNameFromFile("pix68k-x68030.cfg", name, sizeof name), 1);
    CHECK_EQ(profileNameFromFile("pix68k-out_run-a.cfg", name, sizeof name), 1);
    CHECK_EQ(strcmp(name, "out_run-a"), 0);
    static const char* const kBad[] = {"pix68k.cfg", "pix68k-.cfg", "pix68k-gr2.new", "pix68k-gr2.cfg.old",
                                       "pix68k.cfg.human", "pix68k-a b.cfg", "pix68k-a.b.cfg", "xpix68k-a.cfg",
                                       "pix68k-123456789012345678901234567890123.cfg", "pix68k-ぐら.cfg"};
    for (const char* f : kBad) CHECK_EQ(profileNameFromFile(f, name, sizeof name), 0);
    // 保存の途中で止まって残った pix68k-<名前>.new も、tmp を渡せば受け付ける (一覧に出して復旧できるように)
    bool tmp = true;
    CHECK_EQ(profileNameFromFile("pix68k-gr2.cfg", name, sizeof name, &tmp), 1);
    CHECK_EQ(tmp, false);
    CHECK_EQ(profileNameFromFile("pix68k-gr2.new", name, sizeof name, &tmp), 1);
    CHECK_EQ(strcmp(name, "gr2"), 0);
    CHECK_EQ(tmp, true);
    CHECK_EQ(profileNameFromFile("PIX68K-CBX.NEW", name, sizeof name, &tmp), 1);
    CHECK_EQ(strcmp(name, "CBX"), 0);
    static const char* const kBadNew[] = {"pix68k.new", "pix68k-.new", "pix68k-gr2.old", "pix68k-a b.new", "pix68k-gr2.new.cfg~"};
    for (const char* f : kBadNew) CHECK_EQ(profileNameFromFile(f, name, sizeof name, &tmp), 0);
    CHECK_EQ(validProfileName("12345678901234567890123456789012"), 1); // 32 字
    CHECK_EQ(validProfileName("123456789012345678901234567890123"), 0);
    CHECK_EQ(validProfileName(""), 0);
    CHECK_EQ(validProfileName("../x"), 0);
    CHECK_EQ(profileFileName("gr2", name, sizeof name), 1);
    CHECK_EQ(strcmp(name, "pix68k-gr2.cfg"), 0);
    CHECK_EQ(profileFileName("gr2", name, sizeof name, true), 1);
    CHECK_EQ(strcmp(name, "pix68k-gr2.new"), 0);
    CHECK_EQ(profileFileName("a/b", name, sizeof name), 0);
    char small[8];
    CHECK_EQ(profileFileName("gr2", small, sizeof small), 0); // 入りきらない
}

void testTitle() {
    char t[PROFILE_TITLE_MAX + 1];
    const char* a = "# グラディウス II  \r\nfd0 = GR2.XDF\r\n";
    CHECK_EQ(profileTitle(a, strlen(a), t, sizeof t), 1);
    CHECK_EQ(strcmp(t, "グラディウス II"), 0);
    const char* b = "\xef\xbb\xbf#OutRun\n";
    CHECK_EQ(profileTitle(b, strlen(b), t, sizeof t), 1);
    CHECK_EQ(strcmp(t, "OutRun"), 0);
    const char* c = "fd0 = A.XDF\n# not a title\n";
    CHECK_EQ(profileTitle(c, strlen(c), t, sizeof t), 0);
    CHECK_EQ(t[0], 0);
    CHECK_EQ(profileTitle("#   \n", 5, t, sizeof t), 0);
    CHECK_EQ(profileTitle("", 0, t, sizeof t), 0);
    // 長い題は字の切れ目で切る
    char longTitle[400] = "# ";
    for (int i = 0; i < 100; i++) strcat(longTitle, "あ");
    CHECK_EQ(profileTitle(longTitle, strlen(longTitle), t, sizeof t), 1);
    CHECK_EQ(strlen(t), 120u);
    char tiny[5];
    CHECK_EQ(profileTitle(longTitle, strlen(longTitle), tiny, sizeof tiny), 1);
    CHECK_EQ(strlen(tiny), 3u); // 「あ」1 字 (3 バイト)
}

// Shift_JIS の題 (#272)。期待する区点は Python の cp932 と euc_jp で確かめた値
void testTitleSjis() {
    char t[PROFILE_TITLE_MAX + 1];
    bool sjis = true;
    // UTF-8 と ASCII だけの行は UTF-8
    const char* u = "# グラディウス II\n";
    CHECK_EQ(profileTitle(u, strlen(u), t, sizeof t, &sjis), 1);
    CHECK_EQ(sjis, 0);
    sjis = true;
    CHECK_EQ(profileTitle("# OutRun\n", 9, t, sizeof t, &sjis), 1);
    CHECK_EQ(sjis, 0);
    // Shift_JIS: 「グラディウス II」(全角カタカナと ASCII)
    const char* s = "# \x83\x4f\x83\x89\x83\x66\x83\x42\x83\x45\x83\x58 II \r\nfd0 = GR2.XDF\r\n";
    CHECK_EQ(profileTitle(s, strlen(s), t, sizeof t, &sjis), 1);
    CHECK_EQ(sjis, 1);
    CHECK_EQ(strcmp(t, "\x83\x4f\x83\x89\x83\x66\x83\x42\x83\x45\x83\x58 II"), 0);
    // 2 バイト目が 0x5C の字 (「表」「ソ」) で行を読み違えない
    const char* h = "#\x95\x5c\x83\x5c\n";
    CHECK_EQ(profileTitle(h, strlen(h), t, sizeof t, &sjis), 1);
    CHECK_EQ(sjis, 1);
    CHECK_EQ(strlen(t), 4u);
    // 半角カタカナ (ｱｲｳ) は 1 バイト
    const char* k = "# \xb1\xb2\xb3\n";
    CHECK_EQ(profileTitle(k, strlen(k), t, sizeof t, &sjis), 1);
    CHECK_EQ(sjis, 1);
    CHECK_EQ(strcmp(t, "\xb1\xb2\xb3"), 0);
    // 半角カタカナの並びの一部は正しい UTF-8 にもなるので UTF-8 とみなす (ﾃﾞ = 0xC3 0xDE は違うが、ﾂｰ = 0xC2 0xB0 は U+00B0)
    const char* amb = "# \xc2\xb0\n";
    CHECK_EQ(profileTitle(amb, strlen(amb), t, sizeof t, &sjis), 1);
    CHECK_EQ(sjis, 0);
    // BOM があれば UTF-8 (壊れていても)
    const char* bom = "\xef\xbb\xbf# \x82\xa0\n";
    CHECK_EQ(profileTitle(bom, strlen(bom), t, sizeof t, &sjis), 1);
    CHECK_EQ(sjis, 0);
    // 長い Shift_JIS の題は 2 バイト文字の途中で切らない (ASCII 1 字のあとに「あ」を並べ、120 バイト目が字の途中になる)
    char longTitle[400] = "# a";
    for (int i = 0; i < 100; i++) strcat(longTitle, "\x82\xa0");
    CHECK_EQ(profileTitle(longTitle, strlen(longTitle), t, sizeof t, &sjis), 1);
    CHECK_EQ(sjis, 1);
    CHECK_EQ(strlen(t), 119u); // a と「あ」59 字
    char tiny[5];
    CHECK_EQ(profileTitle(longTitle, strlen(longTitle), tiny, sizeof tiny, &sjis), 1);
    CHECK_EQ(strlen(tiny), 3u); // a と「あ」1 字
    // 1 行目が読む上限 (partial) で切れた UTF-8 は、最後の字が欠けていても UTF-8
    char cut[64] = "# ";
    for (int i = 0; i < 5; i++) strcat(cut, "あ");
    const size_t cutLen = strlen(cut) - 1; // 最後の「あ」の 3 バイト目を落とす
    CHECK_EQ(profileTitle(cut, cutLen, t, sizeof t, &sjis, true), 1);
    CHECK_EQ(sjis, 0);
    // ファイルの終わりで欠けていれば (partial でなければ) UTF-8 ではない
    CHECK_EQ(profileTitle(cut, cutLen, t, sizeof t, &sjis), 1);
    CHECK_EQ(sjis, 1);
    // 改行のないファイルの終わりの半角カタカナ「ﾂ」(0xC2) は Shift_JIS
    CHECK_EQ(profileTitle("# \xc2", 3, t, sizeof t, &sjis), 1);
    CHECK_EQ(sjis, 1);
    CHECK_EQ(strcmp(t, "\xc2"), 0);
    // 行が終わっていれば、partial でも欠けた字は UTF-8 ではない
    char cutLine[64];
    memcpy(cutLine, cut, cutLen);
    cutLine[cutLen] = '\n';
    CHECK_EQ(profileTitle(cutLine, cutLen + 1, t, sizeof t, &sjis, true), 1);
    CHECK_EQ(sjis, 1);
    // partial で読む上限まで行が続いていても、欠けた字のあとに空白があれば、その字は UTF-8 の字の途中ではない
    // (「ﾂ」(0xC2) と空白 509 個で 512 バイト。続きは改行)
    char spaced[512];
    memset(spaced, ' ', sizeof spaced);
    spaced[0] = '#';
    spaced[2] = '\xc2';
    CHECK_EQ(profileTitle(spaced, sizeof spaced, t, sizeof t, &sjis, true), 1);
    CHECK_EQ(sjis, 1);
    CHECK_EQ(strcmp(t, "\xc2"), 0);
    // partial でも、続くバイトがどうであっても正しい UTF-8 にならない終わりは Shift_JIS:
    // 長すぎる形 (C0、C1、E0 80、F0 80)、サロゲート (ED A0)、0x10FFFF より大きい (F4 90、F5)、続きのバイトから始まる (80)
    static const char* const kBad[] = {"# a\xc0", "# a\xc1", "# a\xe0\x80", "# a\xf0\x80", "# a\xed\xa0",
                                       "# a\xf4\x90", "# a\xf5", "# a\x80", "# a\xe1\x80\x41"};
    for (const char* b : kBad) {
        CHECK_EQ(profileTitle(b, strlen(b), t, sizeof t, &sjis, true), 1);
        CHECK_EQ(sjis, 1);
    }
    // 続くバイト次第で正しい UTF-8 になる終わりは、partial なら UTF-8
    static const char* const kGood[] = {"# a\xc2", "# a\xe0\xa0", "# a\xed\x9f", "# a\xef\xbf", "# a\xf0\x90\x80",
                                        "# a\xf4\x8f\xbf", "# a\xe3"};
    for (const char* g : kGood) {
        CHECK_EQ(profileTitle(g, strlen(g), t, sizeof t, &sjis, true), 1);
        CHECK_EQ(sjis, 0);
    }
    // 題がなければ sjis は false
    sjis = true;
    CHECK_EQ(profileTitle("fd0 = A.XDF\n", 12, t, sizeof t, &sjis), 0);
    CHECK_EQ(sjis, 0);
}

// 文字列から ConfigText を作る
void load(ConfigText& t, const char* s) { t.load(s, strlen(s)); }
// ConfigText の中身が want と同じか (data() は 0 で終わらない)
bool same(const ConfigText& t, const char* want) {
    const bool ok = t.size() == strlen(want) && memcmp(t.data(), want, t.size()) == 0;
    if (!ok) printf("  got:\n%.*s\n  want:\n%s\n", (int)t.size(), t.data(), want);
    return ok;
}

void testMerge() {
    ConfigText base, prof;
    load(base, "# 共通\nfd0 = HUMAN302.XDF\nclock = 10\nnet = on\nnet_token = secret\nkeyboard = pi400jp\n"
               "video_scale = off\nvideo_filter = curve,mask\nautotype = dir\\n\nprofile_wait = 3\nbogus = 1\n");
    load(prof, "# x68060\ncpu = 68060\nsc0 = CBXNEW.HDS\nclock = 75\nvideo_scale = integer\nautotype = cbx68p 0 30\\n\n");
    int warnBase = 0, warnProf = 0;
    auto warn = [&](bool fromProfile, const char*, const char*, SettingResult) { (fromProfile ? warnProf : warnBase)++; };
    auto bad = [&](bool fromProfile, const char*) { (fromProfile ? warnProf : warnBase)++; };
    // 組み合わせなし: pix68k.cfg のすべて
    Settings s;
    readProfileSettings(base, nullptr, s, true, warn, bad);
    CHECK_EQ(strcmp(s.fd[0], "HUMAN302.XDF"), 0);
    CHECK_EQ(s.clockMHz, 10);
    CHECK_EQ(s.profileWait, 3);
    CHECK_EQ(strcmp(s.autotype, "dir\\n"), 0);
    CHECK_EQ(warnBase, 1); // bogus
    // 組み合わせあり: pix68k.cfg からは共通の項目だけ
    warnBase = 0;
    readProfileSettings(base, &prof, s, false, warn, bad);
    CHECK_EQ(warnBase, 0); // 2 回目は黙る
    // ホスト版は 1 回で読む: 受け継がない項目でも、知らない名前は知らせる (bogus)。値の誤りは知らせない
    Settings once;
    readProfileSettings(base, &prof, once, true, warn, bad);
    CHECK_EQ(warnBase, 1);
    warnBase = 0;
    CHECK_EQ(warnProf, 0);
    CHECK_EQ(s.fd[0][0], 0);   // pix68k.cfg の fd0 は受け継がない
    CHECK_EQ(s.cpu, 68060);
    CHECK_EQ(s.clockMHz, 75);
    CHECK_EQ(strcmp(s.sc[0], "CBXNEW.HDS"), 0);
    CHECK_EQ(s.net, 1);        // 共通の項目は受け継ぐ
    CHECK_EQ(strcmp(s.netToken, "secret"), 0);
    CHECK_EQ(s.keyboard, 3);
    CHECK_EQ(s.videoScale, (int)VIDEO_SCALE_INTEGER); // 組み合わせに書けば、そちらが勝つ
    CHECK_EQ(s.videoFilter, (int)(CRT_CURVE | CRT_MASK)); // video_filter= も画面の出し方なので共通の項目
    CHECK_EQ(s.profileWait, 3);
    CHECK_EQ(strcmp(s.autotype, "cbx68p 0 30\\n"), 0); // autotype は組み合わせのもの
    // 組み合わせに autotype がなければ、pix68k.cfg の autotype は打たない
    ConfigText prof2;
    load(prof2, "fd0 = GR2.XDF\nbad line\n");
    readProfileSettings(base, &prof2, s, false, warn, bad);
    CHECK_EQ(s.autotype[0], 0);
    CHECK_EQ(s.clockMHz, 16); // 既定の値
    CHECK_EQ(warnProf, 1);
    // 控えのファイル (pix68k.cfg に写して使っていたもの) は、写したときと同じ構成になる
    ConfigText copy;
    const char* backup = "# Pi 3 用\nfd0 = HUMAN302.XDF\nclock = 16\nram = 4\n#hd0 = HUMAN302.HDF\n";
    load(copy, backup);
    Settings asBase, asProfile;
    readProfileSettings(copy, nullptr, asBase, false, warn, bad);
    ConfigText other;
    load(other, "cpu = 68060\nsc0 = CBXPAD.HDS\nclock = 75\nram = 12\n");
    readProfileSettings(other, &copy, asProfile, false, warn, bad);
    for (const SettingKey& k : kSettingKeys) CHECK_EQ(sameSetting(asBase, asProfile, k), 1);
}

void testWriteSplit() {
    ConfigText base, prof;
    load(base, "net = on\nvideo_scale = integer\n");
    load(prof, "# GR2\nfd0 = GR2A.XDF\n");
    Settings saved;
    readProfileSettings(base, &prof, saved, false, [](bool, const char*, const char*, SettingResult) {},
                        [](bool, const char*) {});
    Settings s = saved;
    copyString(s.fd[0], sizeof s.fd[0], "GR2B.XDF");
    s.videoScale = VIDEO_SCALE_OFF;
    bool cb = false, cp = false;
    CHECK_EQ(writeProfileSettings(base, prof, s, saved, cb, cp), 1);
    CHECK_EQ(cb, 1);
    CHECK_EQ(cp, 1);
    CHECK_EQ(same(base, "net = on\nvideo_scale = off\n"), 1);
    CHECK_EQ(same(prof, "# GR2\nfd0 = GR2B.XDF\n"), 1);
    // 組み合わせの側だけ変えたら、pix68k.cfg は触らない
    Settings s2 = s;
    copyString(s2.fd[1], sizeof s2.fd[1], "B.XDF");
    CHECK_EQ(writeProfileSettings(base, prof, s2, s, cb, cp), 1);
    CHECK_EQ(cb, 0);
    CHECK_EQ(cp, 1);
    CHECK_EQ(same(prof, "# GR2\nfd0 = GR2B.XDF\nfd1=B.XDF\n"), 1);
    // 共通の項目でも、組み合わせに行があれば組み合わせに書く (次の起動ではそちらが勝つ)
    ConfigText base3, prof3;
    load(base3, "video_scale = integer\n");
    load(prof3, "video_scale = off\n");
    Settings saved3;
    readProfileSettings(base3, &prof3, saved3, false, [](bool, const char*, const char*, SettingResult) {},
                        [](bool, const char*) {});
    CHECK_EQ(saved3.videoScale, (int)VIDEO_SCALE_OFF);
    Settings s3 = saved3;
    s3.videoScale = VIDEO_SCALE_ASPECT;
    CHECK_EQ(writeProfileSettings(base3, prof3, s3, saved3, cb, cp), 1);
    CHECK_EQ(cb, 0);
    CHECK_EQ(cp, 1);
    CHECK_EQ(same(base3, "video_scale = integer\n"), 1);
    CHECK_EQ(same(prof3, "video_scale = aspect\n"), 1);
}

// video_filter= と video_scale= は共通の項目: 組み合わせに書かなければ pix68k.cfg の値を受け継ぎ、書けば (none でも)
// そちらを使う。v0.1.0 の書き方 (scale=、scanline=) も受け継ぎ (pix68k.cfg に残っていても見た目が変わらないように)、
// 保存するときはどちらのファイルでも新しい 2 つの行にする。pix68k.cfg は pix68k.cfg だけを読んだ値で、組み合わせのファイルは
// 重ねて読んだ値で書く
void testVideoProfile() {
    auto quiet = [](bool, const char*, const char*, SettingResult) {};
    auto quietBad = [](bool, const char*) {};
    {
        ConfigText base, prof;
        load(base, "video_filter = scanline,curve,mask\nvideo_scale = aspect\n");
        load(prof, "fd0 = A.XDF\n");
        Settings s;
        readProfileSettings(base, &prof, s, false, quiet, quietBad);
        CHECK_EQ(s.videoFilter, (int)CRT_FULL);
        CHECK_EQ(s.videoScale, (int)VIDEO_SCALE_ASPECT);
        load(prof, "fd0 = A.XDF\nvideo_filter = none\n");
        Settings s2;
        readProfileSettings(base, &prof, s2, false, quiet, quietBad);
        CHECK_EQ(s2.videoFilter, (int)CRT_OFF);
        CHECK_EQ(s2.videoScale, (int)VIDEO_SCALE_ASPECT);
    }
    {
        // pix68k.cfg に残っている scanline= も受け継ぐ。組み合わせの scanline=off はあとの行なので勝つ
        ConfigText base, prof;
        load(base, "scale = off\nscanline = on\n");
        load(prof, "scanline = off\n");
        Settings s;
        int legacy = 0;
        readProfileSettings(base, &prof, s, true,
                            [&](bool, const char*, const char*, SettingResult r) { legacy += r == SettingResult::LEGACY; },
                            quietBad);
        CHECK_EQ(legacy, 3);
        CHECK_EQ(s.videoFilter, (int)CRT_OFF);
        CHECK_EQ(s.videoScale, (int)VIDEO_SCALE_OFF);
        // 何も変えずに保存しても、どちらのファイルも新しい書き方にする。pix68k.cfg は pix68k.cfg だけの値、
        // 組み合わせは重ねた値 (scanline=off だけだったので video_filter= だけ)
        bool cb = false, cp = false;
        CHECK_EQ(writeProfileSettings(base, prof, s, s, cb, cp), 1);
        CHECK_EQ(cb, 1);
        CHECK_EQ(cp, 1);
        CHECK_EQ(same(base, "video_scale=off\nvideo_filter=scanline\n"), 1);
        CHECK_EQ(same(prof, "video_filter=none\n"), 1);
        Settings again;
        readProfileSettings(base, &prof, again, false, quiet, quietBad);
        CHECK_EQ(again.videoFilter, (int)CRT_OFF);
        CHECK_EQ(again.videoScale, (int)VIDEO_SCALE_OFF);
        // pix68k.cfg だけで起動しても前と同じ
        Settings alone;
        readProfileSettings(base, nullptr, alone, false, quiet, quietBad);
        CHECK_EQ(alone.videoFilter, (int)CRT_SCANLINE);
    }
    {
        // 選んでいない組み合わせの見た目は、ほかの組み合わせで保存しても変わらない (pix68k.cfg を書き直すのは、
        // pix68k.cfg だけを読んだ値なので)。B は pix68k.cfg の scale=off と scanline=on を、scanline=off と
        // video_scale=full で上書きする。C は video_filter=mask を書いた行が pix68k.cfg のあと。A は何も書かない
        const char* kBase = "scanline=on\nvideo_filter=mask\nscanline=on\nscale=off\n";
        const char* kProfiles[] = {"scanline=off\nvideo_scale=full\n", "video_filter=mask\n", "fd0=A.XDF\n",
                                   "scale=auto\n"};
        for (const char* other : kProfiles) {
            ConfigText base, a, b;
            load(base, kBase);
            load(a, "fd0=A.XDF\n");
            load(b, other);
            Settings before;
            readProfileSettings(base, &b, before, false, quiet, quietBad);
            Settings sa;
            readProfileSettings(base, &a, sa, false, quiet, quietBad);
            bool cb = false, cp = false;
            CHECK_EQ(writeProfileSettings(base, a, sa, sa, cb, cp), 1);
            CHECK_EQ(cb, 1); // pix68k.cfg の scale= と scanline= を書き直した
            CHECK_EQ(cp, 0);
            Settings after;
            int legacy = 0;
            readProfileSettings(base, &b, after, true,
                                [&](bool fromProfile, const char*, const char*, SettingResult r) {
                                    legacy += !fromProfile && r == SettingResult::LEGACY;
                                },
                                quietBad);
            CHECK_EQ(legacy, 0);
            CHECK_EQ(after.videoFilter, before.videoFilter);
            CHECK_EQ(after.videoScale, before.videoScale);
        }
        // pix68k.cfg は、あとの scanline=on が video_filter=mask より勝つので scanline、scale=off で 1 倍
        ConfigText base;
        load(base, kBase);
        Settings s;
        readProfileSettings(base, nullptr, s, false, quiet, quietBad);
        CHECK_EQ(s.videoFilter, (int)CRT_SCANLINE);
        CHECK_EQ(s.videoScale, (int)VIDEO_SCALE_OFF);
    }
    {
        // crt= は読み替えずに無視して知らせる (pix68k.cfg にあっても組み合わせにあっても)。保存しても行は残す
        ConfigText base, prof;
        load(base, "crt=full\n");
        load(prof, "crt=geom\n");
        Settings s;
        int removed = 0;
        readProfileSettings(base, &prof, s, true,
                            [&](bool, const char*, const char*, SettingResult r) { removed += r == SettingResult::REMOVED; },
                            quietBad);
        CHECK_EQ(removed, 2);
        CHECK_EQ(s.videoFilter, (int)CRT_OFF);
        CHECK_EQ(s.videoScale, (int)VIDEO_SCALE_INTEGER);
        bool cb = false, cp = false;
        CHECK_EQ(writeProfileSettings(base, prof, s, s, cb, cp), 1);
        CHECK_EQ(cb, 0);
        CHECK_EQ(cp, 0);
        CHECK_EQ(same(base, "crt=full\n"), 1);
    }
    {
        // Save: 組み合わせに行がなければ pix68k.cfg に、あれば組み合わせに書く
        ConfigText base, prof;
        load(base, "video_filter = curve\n");
        load(prof, "fd0 = A.XDF\nvideo_scale = full\n");
        Settings saved;
        readProfileSettings(base, &prof, saved, false, quiet, quietBad);
        Settings s = saved;
        s.videoFilter = CRT_SCANLINE;
        s.videoScale = VIDEO_SCALE_INTEGER;
        bool cb = false, cp = false;
        CHECK_EQ(writeProfileSettings(base, prof, s, saved, cb, cp), 1);
        CHECK_EQ(cb, 1);
        CHECK_EQ(cp, 1);
        CHECK_EQ(same(base, "video_filter = scanline\n"), 1);
        CHECK_EQ(same(prof, "fd0 = A.XDF\nvideo_scale = integer\n"), 1);
        // 組み合わせの scanline= は、そこで新しい行になるので、変えた値も組み合わせに書く
        ConfigText base2, prof2;
        load(base2, "video_filter = curve\n");
        load(prof2, "scanline = on\n");
        Settings saved2;
        readProfileSettings(base2, &prof2, saved2, false, quiet, quietBad);
        CHECK_EQ(saved2.videoFilter, (int)CRT_SCANLINE);
        Settings s2 = saved2;
        s2.videoFilter = CRT_MASK;
        CHECK_EQ(writeProfileSettings(base2, prof2, s2, saved2, cb, cp), 1);
        CHECK_EQ(cb, 0);
        CHECK_EQ(cp, 1);
        CHECK_EQ(same(base2, "video_filter = curve\n"), 1);
        CHECK_EQ(same(prof2, "video_filter=mask\n"), 1);
    }
    {
        // ホスト版 (既定は video_scale=aspect、legacyVideo=0) は、v0.1.0 のホスト版と同じく scale= と scanline= を読まず、
        // 保存でも書き直さない (ベアメタル版と共用の pix68k.cfg の見た目を変えない)
        Settings hostDefaults;
        hostDefaults.videoScale = VIDEO_SCALE_ASPECT;
        hostDefaults.legacyVideo = 0;
        ConfigText base, prof;
        load(base, "scale = off\nscanline = on\n");
        load(prof, "fd0 = A.XDF\n");
        Settings s;
        int warned = 0;
        readProfileSettings(base, &prof, s, true, [&](bool, const char*, const char*, SettingResult) { warned++; },
                            quietBad, hostDefaults);
        CHECK_EQ(warned, 0);
        CHECK_EQ(s.videoScale, (int)VIDEO_SCALE_ASPECT);
        CHECK_EQ(s.videoFilter, (int)CRT_OFF);
        Settings none;
        readProfileSettings(prof, nullptr, none, false, quiet, quietBad, hostDefaults);
        CHECK_EQ(none.videoScale, (int)VIDEO_SCALE_ASPECT);
        bool cb = false, cp = false;
        CHECK_EQ(writeProfileSettings(base, prof, s, s, cb, cp, hostDefaults), 1);
        CHECK_EQ(cb, 0);
        CHECK_EQ(same(base, "scale = off\nscanline = on\n"), 1);
        // メニューで変えて保存すると、pix68k.cfg のあとの行に足す (ベアメタル版でも、あとの行なので勝つ)
        Settings changed = s;
        changed.videoFilter = CRT_MASK;
        CHECK_EQ(writeProfileSettings(base, prof, changed, s, cb, cp, hostDefaults), 1);
        CHECK_EQ(same(base, "scale = off\nscanline = on\nvideo_filter=mask\n"), 1);
        Settings pi;
        readProfileSettings(base, nullptr, pi, false, quiet, quietBad);
        CHECK_EQ(pi.videoFilter, (int)CRT_MASK);
        CHECK_EQ(pi.videoScale, (int)VIDEO_SCALE_OFF);
    }
}

void testList() {
    ProfileList l;
    CHECK_EQ(l.count(), 0);
    CHECK_EQ(l.add("gr2", "グラディウス II"), 1);
    CHECK_EQ(l.add("cbx", "OutRun"), 1);
    CHECK_EQ(l.add("Human", ""), 1);
    CHECK_EQ(l.add("CBX", "dup"), 1); // 同じ名前は捨てる
    CHECK_EQ(l.add("a/b", ""), 0);
    CHECK_EQ(l.count(), 3);
    CHECK_EQ(strcmp(l.at(0).name, "cbx"), 0);
    CHECK_EQ(strcmp(l.at(0).title, "OutRun"), 0);
    CHECK_EQ(strcmp(l.at(1).name, "gr2"), 0);
    CHECK_EQ(strcmp(l.at(2).name, "Human"), 0);
    CHECK_EQ(l.find("HUMAN"), 2);
    CHECK_EQ(l.find("x"), -1);
    // 上限を超えたら、名前の順で後ろのものを捨てる
    ProfileList big;
    char n[8];
    for (int i = PROFILE_MAX + 4; i >= 0; i--) {
        snprintf(n, sizeof n, "p%02d", i);
        big.add(n, "");
    }
    CHECK_EQ(big.count(), PROFILE_MAX);
    CHECK_EQ(big.dropped(), 5);
    CHECK_EQ(strcmp(big.at(0).name, "p00"), 0);
    snprintf(n, sizeof n, "p%02d", PROFILE_MAX - 1);
    CHECK_EQ(strcmp(big.at(PROFILE_MAX - 1).name, n), 0);
    // 前回の選択 (keep()) は、名前の順で後ろでも捨てない。足す順 (前、途中、後ろ) によらない
    for (int pos = 0; pos < 3; pos++) {
        ProfileList kept;
        kept.keep("ZZ");
        const int total = PROFILE_MAX + 3, keepAt = pos == 0 ? 0 : pos == 1 ? total / 2 : total;
        for (int i = 0; i <= total; i++) {
            if (i == keepAt) kept.add("zz", "last");
            if (i < total) {
                snprintf(n, sizeof n, "p%02d", i);
                kept.add(n, "");
            }
        }
        CHECK_EQ(kept.count(), PROFILE_MAX);
        CHECK_EQ(kept.dropped(), 4);
        CHECK_EQ(kept.find("zz"), PROFILE_MAX - 1);
        snprintf(n, sizeof n, "p%02d", PROFILE_MAX - 2);
        CHECK_EQ(kept.find(n), PROFILE_MAX - 2);
        CHECK_EQ(strcmp(kept.at(0).name, "p00"), 0);
    }
}

void testChoice() {
    char name[PROFILE_NAME_MAX + 1], buf[64];
    bool once = true;
    CHECK_EQ(parseProfileChoice("gr2\n", 4, name, sizeof name, once), 1);
    CHECK_EQ(strcmp(name, "gr2"), 0);
    CHECK_EQ(once, 0);
    CHECK_EQ(parseProfileChoice("cbx\r\nonce\r\n", 11, name, sizeof name, once), 1);
    CHECK_EQ(strcmp(name, "cbx"), 0);
    CHECK_EQ(once, 1);
    CHECK_EQ(parseProfileChoice("\nonce\n", 6, name, sizeof name, once), 1); // pix68k.cfg だけ、1 回
    CHECK_EQ(name[0], 0);
    CHECK_EQ(once, 1);
    CHECK_EQ(parseProfileChoice("", 0, name, sizeof name, once), 1);
    CHECK_EQ(name[0], 0);
    CHECK_EQ(parseProfileChoice("../x\n", 5, name, sizeof name, once), 0);
    CHECK_EQ(name[0], 0);
    CHECK_EQ(parseProfileChoice("gr2", 3, name, sizeof name, once), 1); // 改行なし
    CHECK_EQ(strcmp(name, "gr2"), 0);
    size_t n = formatProfileChoice("gr2", true, buf, sizeof buf);
    CHECK_EQ(n, 9u);
    CHECK_EQ(strcmp(buf, "gr2\nonce\n"), 0);
    CHECK_EQ(parseProfileChoice(buf, n, name, sizeof name, once), 1);
    CHECK_EQ(once, 1);
    CHECK_EQ(formatProfileChoice("", false, buf, sizeof buf), 1u);
    CHECK_EQ(formatProfileChoice("a b", false, buf, sizeof buf), 0u);
    // POST /reboot?profile= のクエリ
    CHECK_EQ(queryHas("profile=gr2", "profile"), 1);
    CHECK_EQ(queryHas("a=1&profile=", "profile"), 1);
    CHECK_EQ(queryHas("profile", "profile"), 1);
    CHECK_EQ(queryHas("profiles=1", "profile"), 0);
    CHECK_EQ(queryHas("", "profile"), 0);
    CHECK_EQ(queryValue("a=1&profile=", "profile", buf, sizeof buf), 1);
    CHECK_EQ(buf[0], 0);
}

void testLayout() {
    uint16_t cells[16];
    CHECK_EQ(layoutLine("abc", cells, 10), 3);
    CHECK_EQ(cells[0], 'a');
    // 全角は 2 桁
    CHECK_EQ(layoutLine("あい", cells, 10), 4);
    CHECK_EQ(cells[1], MSG_CELL_RIGHT);
    // 入りきらなければ最後を「…」に
    CHECK_EQ(layoutLine("abcdefghijkl", cells, 8), 8);
    CHECK_EQ(cells[5], 'f');
    CHECK_EQ(cells[6], MSG_JIS_ELLIPSIS);
    CHECK_EQ(cells[7], MSG_CELL_RIGHT);
    // 全角の途中で切らない
    CHECK_EQ(layoutLine("aあいうえ", cells, 6), 5);
    CHECK_EQ(cells[1] != MSG_CELL_RIGHT, 1);
    CHECK_EQ(cells[3], MSG_JIS_ELLIPSIS);
    // 半角カタカナ
    CHECK_EQ(layoutLine("\xef\xbd\xb1", cells, 4), 1); // ｱ
    CHECK_EQ(cells[0], 0xb1);
}

// Shift_JIS の行を並べる (#272)。区点は Python の cp932 と euc_jp で確かめた値
void testLayoutSjis() {
    uint16_t cells[32];
    // ASCII、全角、半角カタカナの混じり: a あ ｱ
    CHECK_EQ(layoutLine("a\x82\xa0\xb1", cells, 10, true), 4);
    CHECK_EQ(cells[0], 'a');
    CHECK_EQ(cells[1], 0x2422);
    CHECK_EQ(cells[2], MSG_CELL_RIGHT);
    CHECK_EQ(cells[3], 0xb1);
    // 区と点の境目: × (817E) ÷ (8180) ◇ (819E) ◆ (819F)
    CHECK_EQ(layoutLine("\x81\x7e\x81\x80\x81\x9e\x81\x9f", cells, 10, true), 8);
    CHECK_EQ(cells[0], 0x215f);
    CHECK_EQ(cells[2], 0x2160);
    CHECK_EQ(cells[4], 0x217e);
    CHECK_EQ(cells[6], 0x2221);
    // 1 バイト目の境目: 檗 (9F40) 滌 (9FFC) 漾 (E040) 熙 (EAA4)
    CHECK_EQ(layoutLine("\x9f\x40\x9f\xfc\xe0\x40\xea\xa4", cells, 10, true), 8);
    CHECK_EQ(cells[0], 0x5d21);
    CHECK_EQ(cells[2], 0x5e7e);
    CHECK_EQ(cells[4], 0x5f21);
    CHECK_EQ(cells[6], 0x7426);
    // 2 バイト目が 0x5C: 表 (955C) ソ (835C)
    CHECK_EQ(layoutLine("\x95\x5c\x83\x5c", cells, 10, true), 4);
    CHECK_EQ(cells[0], 0x493d);
    CHECK_EQ(cells[2], 0x253d);
    // 壊れたバイト列: 2 バイト目が違う、使わないバイト、終わりで切れた 1 バイト目は「?」で 1 バイトずつ進む
    CHECK_EQ(layoutLine("\x82" "0\x80\xa0\xfd\x82", cells, 10, true), 6);
    CHECK_EQ(cells[0], '?');
    CHECK_EQ(cells[1], '0');
    CHECK_EQ(cells[2], '?');
    CHECK_EQ(cells[3], '?');
    CHECK_EQ(cells[4], '?');
    CHECK_EQ(cells[5], '?');
    // 2 バイト目が 0x7F なら 2 バイト文字ではない (「?」で 1 バイト進み、0x7F は捨てる)
    CHECK_EQ(layoutLine("\x82\x7f" "a", cells, 10, true), 2);
    CHECK_EQ(cells[0], '?');
    CHECK_EQ(cells[1], 'a');
    // 1 バイト目の上限 0xFC (IBM 拡張文字の範囲) も 2 バイト文字として読み、区点は 0x7E 区より先のまま (豆腐になる)
    CHECK_EQ(layoutLine("\xfc\x40\xfc\xfc", cells, 10, true), 4);
    CHECK_EQ(cells[0], 0x9721);
    CHECK_EQ(cells[2], 0x987e);
    CHECK_EQ(cgromFont16Offset(cells[0]), -1);
    // 外字 (F040) は区点のまま (CGROM にないので drawCells() が豆腐にする)
    CHECK_EQ(layoutLine("\xf0\x40", cells, 10, true), 2);
    CHECK_EQ(cells[0], 0x7f21);
    // TAB は空白、ほかの制御文字は捨てる
    CHECK_EQ(layoutLine("a\tb\x01", cells, 10, true), 3);
    CHECK_EQ(cells[1], ' ');
    // 入りきらなければ全角の途中で切らずに「…」
    CHECK_EQ(layoutLine("a\x82\xa0\x82\xa2\x82\xa4\x82\xa6", cells, 6, true), 5);
    CHECK_EQ(cells[1], 0x2422);
    CHECK_EQ(cells[3], MSG_JIS_ELLIPSIS);
    // 同じバイト列でも UTF-8 として読めば違う
    CHECK_EQ(layoutLine("\x82\xa0", cells, 10), 2);
    CHECK_EQ(cells[0], '?');
}

void testMenuSjis() {
    ProfileList l;
    CHECK_EQ(l.add("gr2", "\x83\x4f\x83\x89", true), 1);
    CHECK_EQ(l.add("cbx", "OutRun"), 1);
    CHECK_EQ(l.add("empty", "", true), 1);
    CHECK_EQ(l.at(0).sjis, 0);
    CHECK_EQ(l.at(2).sjis, 1);
    CHECK_EQ(l.at(1).sjis, 0); // 題が空なら false
    // Shift_JIS の題の行も描ける (CGROM がないので全角は豆腐)
    BootMenu m;
    m.open(&l, 3, 0, 5000);
    static uint32_t px[640 * 480];
    m.draw(px, 640, 640, 480, 0, nullptr, 0, 0xff000000u);
}

void testMenu() {
    ProfileList l;
    l.add("cbx", "OutRun");
    l.add("gr2", "グラディウス II");
    BootMenu m;
    m.open(&l, 2, 1000, 5000);
    CHECK_EQ(m.rows(), 3);
    CHECK_EQ(m.cursor(), 2);
    CHECK_EQ(m.secondsLeft(1000), 5);
    CHECK_EQ(m.secondsLeft(5001), 1);
    CHECK_EQ(m.tick(5999), -1);
    // USB を探していた間に過ぎた分を延ばす
    m.holdUntil(7000);
    CHECK_EQ(m.tick(6000), -1);
    CHECK_EQ(m.tick(7000), 2);
    m.holdUntil(3000); // 縮めはしない
    CHECK_EQ(m.tick(6999), -1);
    CHECK_EQ(strcmp(m.rowName(0), "pix68k.cfg"), 0);
    CHECK_EQ(strcmp(m.rowName(2), "gr2"), 0);
    // キーを押したら待つのをやめる
    CHECK_EQ(m.key(mk::UP), -1);
    CHECK_EQ(m.waiting(), 0);
    CHECK_EQ(m.secondsLeft(7000), -1);
    CHECK_EQ(m.tick(100000), -1);
    CHECK_EQ(m.cursor(), 1);
    CHECK_EQ(m.key(mk::UP), -1);
    CHECK_EQ(m.key(mk::UP), -1); // 先頭から最後へ回る
    CHECK_EQ(m.cursor(), 2);
    CHECK_EQ(m.key(mk::DOWN), -1);
    CHECK_EQ(m.cursor(), 0);
    CHECK_EQ(m.key(mk::END), -1);
    CHECK_EQ(m.key(mk::ENTER), 2);
    CHECK_EQ(m.key(mk::HOME), -1);
    CHECK_EQ(m.key(mk::KP_ENTER), 0);
    // 範囲の外の最初の行は pix68k.cfg に
    m.open(&l, 7, 0, 0);
    CHECK_EQ(m.cursor(), 0);
    CHECK_EQ(m.tick(0), 0);
    // ゲームパッド
    uint8_t keys[3];
    CHECK_EQ(padMenuKeys(0, 0x01, keys), 1);
    CHECK_EQ(keys[0], mk::UP);
    CHECK_EQ(padMenuKeys(0x01, 0x01, keys), 0); // 押し続けても 1 回
    CHECK_EQ(padMenuKeys(0x01, 0x12, keys), 2);
    CHECK_EQ(keys[0], mk::DOWN);
    CHECK_EQ(keys[1], mk::ENTER);
    CHECK_EQ(padMenuKeys(0, 0x20, keys), 1);
    CHECK_EQ(keys[0], mk::ENTER);
}

// 描く範囲の外 (番兵) に書かないこと。小さな画面と大きな画面
void testDraw() {
    ProfileList l;
    for (int i = 0; i < PROFILE_MAX; i++) {
        char n[8];
        snprintf(n, sizeof n, "p%02d", i);
        l.add(n, "とても長い題とても長い題とても長い題とても長い題とても長い題とても長い題");
    }
    static const int kSizes[][2] = {{640, 480}, {1920, 1080}, {100, 40}, {16, 16}, {720, 480}};
    for (const auto& sz : kSizes) {
        const int w = sz[0], h = sz[1], pitch = w + 8;
        uint32_t* px = new uint32_t[(size_t)pitch * (h + 2)];
        for (size_t i = 0; i < (size_t)pitch * (h + 2); i++) px[i] = 0xdeadbeef;
        BootMenu m;
        m.open(&l, PROFILE_MAX, 0, 5000);
        m.draw(px + pitch, pitch, w, h, 0, nullptr, 0, 0xff000000u);
        bool guard = true;
        for (int x = 0; x < pitch; x++) guard = guard && px[x] == 0xdeadbeef && px[(size_t)pitch * (h + 1) + x] == 0xdeadbeef;
        for (int y = 1; y <= h; y++)
            for (int x = w; x < pitch; x++) guard = guard && px[(size_t)pitch * y + x] == 0xdeadbeef;
        CHECK_EQ(guard, 1);
        // 選んでいる行 (最後の行) の色が描いてある
        bool sel = false;
        if (w >= 640)
            for (size_t i = pitch; i < (size_t)pitch * (h + 1); i++) sel = sel || px[i] == (0x2050b0u | 0xff000000u);
        CHECK_EQ(sel, w >= 640);
        delete[] px;
    }
}

void testAutoTyper() {
    char text[64];
    const char* in = "Ab\\n\x01";
    const size_t n = unescapeTyped(in, strlen(in), text, sizeof text);
    AutoTyper t;
    t.start(text, n, 1000, 10000);
    CHECK_EQ(t.active(), 1);
    uint8_t mods = 0xff, usage = 0xff;
    CHECK_EQ(t.poll(10999, mods, usage), 0); // まだ
    CHECK_EQ(t.poll(11000, mods, usage), 1);
    CHECK_EQ(usage, 0x04); // A
    CHECK_EQ(mods, HID_MOD_LSHIFT);
    // 送れなかったら、次も同じもの
    CHECK_EQ(t.poll(11001, mods, usage), 1);
    CHECK_EQ(usage, 0x04);
    t.ack(11001);
    CHECK_EQ(t.poll(11040, mods, usage), 0); // 押して 40ms
    CHECK_EQ(t.poll(11041, mods, usage), 1);
    CHECK_EQ(usage, 0);
    CHECK_EQ(mods, 0);
    t.ack(11041);
    CHECK_EQ(t.poll(11081, mods, usage), 1);
    CHECK_EQ(usage, 0x05); // b
    CHECK_EQ(mods, 0);
    t.ack(11081);
    CHECK_EQ(t.poll(11121, mods, usage), 1);
    t.ack(11121);
    CHECK_EQ(t.poll(11161, mods, usage), 1);
    CHECK_EQ(usage, 0x28); // Enter
    t.ack(11161);
    CHECK_EQ(t.poll(11201, mods, usage), 1);
    CHECK_EQ(usage, 0);
    t.ack(11201);
    // 打てない字 (0x01) を飛ばして終わる
    CHECK_EQ(t.poll(11241, mods, usage), 0);
    CHECK_EQ(t.active(), 0);
    CHECK_EQ(t.typed(), 3u);
    CHECK_EQ(t.skipped(), 1u);
    // 空なら何もしない
    AutoTyper e;
    e.start("", 0, 0, 0);
    CHECK_EQ(e.active(), 0);
    CHECK_EQ(e.poll(0, mods, usage), 0);
}

} // namespace

// profile.sel (sel) と profile.new (new) だけのファイルシステム。操作ごとに「n 回目だけ失敗」か「n 回目からずっと失敗」を
// 選べる。rename には「失敗を返すが、実は済んでいた」もある
struct ChoiceFs {
    enum Op { EXISTS, WRITE, REMOVE, RENAME, READ, OPS };
    struct File {
        bool exists = false;
        char data[64] = {};
    } sel, tmp;
    struct Fail {
        int at = 0;       // 0 なら失敗させない。1 から数えて at 回目に失敗
        bool from = false; // true なら at 回目からずっと
        bool done = false; // rename だけ: 失敗を返すが、実は済ませる
        int calls = 0;
        bool hit() {
            calls++;
            return at != 0 && (from ? calls >= at : calls == at);
        }
    } fail[OPS];
    File& f(const char* n) { return strcmp(n, "sel") == 0 ? sel : tmp; }
    void clearFailures() {
        for (Fail& x : fail) x = Fail();
    }
    FsResult exists(const char* n) {
        if (fail[EXISTS].hit()) return FsResult::ERROR;
        return f(n).exists ? FsResult::OK : FsResult::NO_FILE;
    }
    FsResult write(const char* n, const void* d, unsigned size) {
        File& x = f(n);
        x.exists = true; // 作り直す (失敗しても空のファイルが残る)
        memset(x.data, 0, sizeof x.data);
        if (fail[WRITE].hit()) return FsResult::ERROR;
        memcpy(x.data, d, size);
        return FsResult::OK;
    }
    FsResult remove(const char* n) {
        if (fail[REMOVE].hit()) return FsResult::ERROR;
        File& x = f(n);
        if (!x.exists) return FsResult::NO_FILE;
        x.exists = false;
        return FsResult::OK;
    }
    FsResult rename(const char* from, const char* to) {
        const bool failed = fail[RENAME].hit();
        if (failed && !fail[RENAME].done) return FsResult::ERROR;
        File& a = f(from);
        File& b = f(to);
        if (!a.exists || b.exists) return FsResult::ERROR;
        b = a;
        a.exists = false;
        return failed ? FsResult::ERROR : FsResult::OK;
    }
    FsResult read(const char* n, char* buf, unsigned cap, unsigned& got) {
        got = 0;
        if (fail[READ].hit()) return FsResult::ERROR;
        const File& x = f(n);
        if (!x.exists) return FsResult::NO_FILE;
        got = (unsigned)strlen(x.data);
        if (got > cap) got = cap;
        memcpy(buf, x.data, got);
        return FsResult::OK;
    }
    // 次の起動が実際に読むもの (失敗させずに、ChooseProfile() と同じ読み方)。once が入っていれば true
    bool nextBootOnce(char* name, size_t size) {
        const File* x = sel.exists ? &sel : tmp.exists ? &tmp : nullptr;
        bool once = false;
        name[0] = 0;
        if (x != nullptr && !parseProfileChoice(x->data, strlen(x->data), name, size, once)) {
            name[0] = 0;
            once = false;
        }
        return once;
    }
    void put(File& x, const char* s) {
        x.exists = true;
        memset(x.data, 0, sizeof x.data);
        strcpy(x.data, s);
    }
};

// POST /reboot?profile= の要求の書き込みと取り消し (ProfileChoiceStore)。操作の失敗を 1 つか 2 つ組み合わせて総当たりし、
// どの場合も次を確かめる:
//   - request() が失敗を返したら、次の起動で once として読まれないか、pending() が残っている
//   - cancel() が true なら、次の起動で once として読まれない。false なら pending() が残っている
//   - 失敗しなくなれば、cancel() は true になり、前回の選択に戻る
int g_choiceCases = 0;

void checkChoiceStore(int initial, const ChoiceFs::Fail* plan, int planOps[2], int nPlan) {
    ChoiceFs fs;
    if (initial == 1) fs.put(fs.sel, "gr2\n");
    if (initial == 2) fs.put(fs.tmp, "gr2\n"); // 前の置き換えの途中で止まった
    for (int i = 0; i < nPlan; i++) fs.fail[planOps[i]] = plan[i];
    ProfileChoiceStore st;
    st.setSaved(initial ? "gr2" : "");
    char name[PROFILE_NAME_MAX + 1];
    g_choiceCases++;
    const bool ok = st.request(fs, "sel", "new", "cbx");
    const bool once = fs.nextBootOnce(name, sizeof name);
    bool good = true;
    if (ok) good = good && st.pending() && once && strcmp(name, "cbx") == 0;
    else good = good && (!once || st.pending());
    // 再起動を断ったので取り消す (失敗はそのまま続く)
    const bool c1 = st.cancel(fs, "sel", "new");
    const bool once1 = fs.nextBootOnce(name, sizeof name);
    good = good && (c1 ? !once1 && !st.pending() : st.pending());
    // 失敗しなくなったら取り消せる
    fs.clearFailures();
    const bool c2 = st.cancel(fs, "sel", "new");
    const bool once2 = fs.nextBootOnce(name, sizeof name);
    good = good && c2 && !once2 && !st.pending();
    if (!good) {
        printf("  NG: initial %d,", initial);
        for (int i = 0; i < nPlan; i++)
            printf(" op %d at %d%s%s", planOps[i], plan[i].at, plan[i].from ? "+" : "", plan[i].done ? " (done)" : "");
        printf(": request %d once %d, cancel %d once %d, cancel %d once %d pending %d\n", ok, once, c1, once1, c2, once2,
               st.pending());
    }
    CHECK_EQ(good, true);
}

void testChoiceStore() {
    // 失敗のしかた: 操作 × (1〜5 回目だけ、1〜5 回目からずっと) と、rename の「失敗を返すが済んでいた」
    struct Way {
        int op;
        ChoiceFs::Fail fail;
    } ways[64];
    int nWays = 0;
    for (int op = 0; op < ChoiceFs::OPS; op++)
        for (int at = 1; at <= 5; at++)
            for (int from = 0; from < 2; from++) {
                Way w;
                w.op = op;
                w.fail.at = at;
                w.fail.from = from != 0;
                ways[nWays++] = w;
                if (op == ChoiceFs::RENAME && !from) {
                    w.fail.done = true;
                    ways[nWays++] = w;
                }
            }
    for (int initial = 0; initial < 3; initial++) {
        int ops[2] = {0, 0};
        ChoiceFs::Fail plan[2];
        checkChoiceStore(initial, plan, ops, 0);
        for (int i = 0; i < nWays; i++) {
            ops[0] = ways[i].op;
            plan[0] = ways[i].fail;
            checkChoiceStore(initial, plan, ops, 1);
            for (int j = 0; j < nWays; j++) {
                if (ways[j].op == ways[i].op) continue;
                ops[1] = ways[j].op;
                plan[1] = ways[j].fail;
                checkChoiceStore(initial, plan, ops, 2);
            }
        }
    }
    CHECK_EQ(g_choiceCases > 5000, true);
    // 普通の場合: 書いて、取り消すと前回の選択に戻る
    ChoiceFs fs;
    fs.put(fs.sel, "gr2\n");
    ProfileChoiceStore st;
    st.setSaved("gr2");
    char name[PROFILE_NAME_MAX + 1];
    CHECK_EQ(st.request(fs, "sel", "new", "cbx"), 1);
    CHECK_EQ(fs.nextBootOnce(name, sizeof name), 1);
    CHECK_EQ(strcmp(name, "cbx"), 0);
    CHECK_EQ(st.cancel(fs, "sel", "new"), 1);
    CHECK_EQ(fs.nextBootOnce(name, sizeof name), 0);
    CHECK_EQ(strcmp(name, "gr2"), 0);
    // どちらのファイルもなく書けない: 失敗を返すが、once は残らないので取り消すものはない
    // (profile= なしの POST /reboot を 500 にしない)
    ChoiceFs ro;
    ro.fail[ChoiceFs::WRITE].at = 1;
    ro.fail[ChoiceFs::WRITE].from = true;
    ProfileChoiceStore st2;
    CHECK_EQ(st2.request(ro, "sel", "new", "cbx"), 0);
    CHECK_EQ(st2.pending(), 0);
    CHECK_EQ(st2.cancel(ro, "sel", "new"), 1);
}

// メニューのゲームパッドの枠 (PadStartFilter)。つないだときから押したままのボタンを数えないこと
void testPadStart() {
    uint8_t keys[3];
    const uint8_t A = 0x10, DOWN = 0x02;
    // つないで 300 以内の最初のレポートで A を押したまま: 数えない。離して押し直すと Enter
    {
        PadStartFilter p(300);
        p.start(1000);
        CHECK_EQ(p.keys(0, 0, 0, 1000, keys), 0); // つないでいない
        CHECK_EQ(p.keys(900, 1, A, 1200, keys), 0); // 前につないだもの: 探し終えた 1000 から 300 以内
        CHECK_EQ(p.keys(900, 2, A, 1400, keys), 0);
        CHECK_EQ(p.keys(900, 3, 0, 1500, keys), 0);
        CHECK_EQ(p.keys(900, 4, A, 1600, keys), 1);
        CHECK_EQ(keys[0], menu_key::ENTER);
    }
    // 300 を過ぎてから最初のレポート (変わったときだけ送るパッド): 押したとみなす
    {
        PadStartFilter p(300);
        p.start(1000);
        CHECK_EQ(p.keys(1100, 0, 0, 1200, keys), 0);
        CHECK_EQ(p.keys(1100, 1, DOWN, 1401, keys), 1);
        CHECK_EQ(keys[0], menu_key::DOWN);
    }
    // ちょうど 300 は数えない (つないだ時刻が探し終えたあとなら、つないだ時刻から数える)
    {
        PadStartFilter p(300);
        p.start(1000);
        CHECK_EQ(p.keys(2000, 1, A, 2300, keys), 0);
    }
    // 抜いて、A を押したままのパッドを同じ枠につなぎ直す: つないだ時刻から数え直す
    {
        PadStartFilter p(300);
        p.start(1000);
        CHECK_EQ(p.keys(900, 1, 0, 1100, keys), 0);
        CHECK_EQ(p.keys(900, 2, DOWN, 2000, keys), 1); // 普通に使う
        CHECK_EQ(p.keys(900, 3, 0, 2100, keys), 0);
        CHECK_EQ(p.keys(5000, 0, 0, 5000, keys), 0);   // つなぎ直した (数とビットは 0 に戻る)
        CHECK_EQ(p.keys(5000, 1, A, 5100, keys), 0);   // 押したまま: 数えない
        CHECK_EQ(p.keys(5000, 2, 0, 5200, keys), 0);
        CHECK_EQ(p.keys(5000, 3, A, 5300, keys), 1);
        CHECK_EQ(keys[0], menu_key::ENTER);
    }
}

int main() {
    testNames();
    testTitle();
    testTitleSjis();
    testMerge();
    testWriteSplit();
    testVideoProfile();
    testList();
    testChoice();
    testChoiceStore();
    testPadStart();
    testLayout();
    testLayoutSjis();
    testMenuSjis();
    testMenu();
    testDraw();
    testAutoTyper();
    return finish();
}
