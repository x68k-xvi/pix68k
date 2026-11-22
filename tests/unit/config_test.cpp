// src/platform/config_text.h と settings.h のテスト: pix68k.cfg の読み方、書き換えたときに注釈・知らない行・
// 改行の形が残ること、項目の表 (範囲、選択肢)
#include "platform/config_text.h"
#include "platform/settings.h"
#include "platform/settings_file.h"
#include "platform/setting_warning.h"
#include "test_check.h"

#include <string.h>

namespace {

using platform::ConfigText;
using platform::Settings;
using platform::SettingResult;

bool same(const ConfigText& c, const char* want) {
    bool ok = c.size() == strlen(want) && memcmp(c.data(), want, c.size()) == 0;
    if (!ok) printf("  got:\n%.*s\n  want:\n%s\n", (int)c.size(), c.data(), want);
    return ok;
}

void testRead() {
    printf("read\n");
    const char* text = "# pix68k の設定\n"
                       "fd0 = HUMAN302.XDF   # 起動ディスク\n"
                       "  clock=10\n"
                       "garbage line\n"
                       "\n"
                       "clock = 16\r\n"
                       "sc0=HUMAN.HDS\n"
                       "cpu=68030";
    ConfigText c;
    CHECK_EQ(c.load(text, strlen(text)), 1);
    int entries = 0, bad = 0;
    char last[64] = {};
    c.forEach(
        [&](const ConfigText::Entry& e) {
            entries++;
            platform::copyString(last, sizeof last, e.key);
        },
        [&](const char* line) {
            bad++;
            CHECK_EQ(strcmp(line, "garbage line"), 0);
        });
    CHECK_EQ(entries, 5);
    CHECK_EQ(bad, 1);
    CHECK_EQ(strcmp(last, "cpu"), 0);
    char v[64];
    CHECK_EQ(c.get("fd0", v, sizeof v), 1);
    CHECK_EQ(strcmp(v, "HUMAN302.XDF"), 0);
    CHECK_EQ(c.get("clock", v, sizeof v), 1); // 後の行が勝つ
    CHECK_EQ(strcmp(v, "16"), 0);
    CHECK_EQ(c.get("cpu", v, sizeof v), 1); // 最後の行に改行がなくても読む
    CHECK_EQ(strcmp(v, "68030"), 0);
    CHECK_EQ(c.get("hd0", v, sizeof v), 0);
}

void testWrite() {
    printf("write\n");
    // 注釈、知らない行 (PR #38 のキーなど)、空行、値の後ろの注釈が残る。最後の行の値だけが変わる
    const char* text = "# pix68k の設定\n"
                       "fd0 = HUMAN302.XDF   # 起動ディスク\n"
                       "clock=10\n"
                       "ram=4\n"
                       "scsi_ex=on\n"
                       "sc0=HUMAN.HDS\n"
                       "cd=GAME.ISO\n"
                       "cd_id=6\n"
                       "\n"
                       "clock = 16 # 後の行\n"
                       "mystery=1\n";
    ConfigText c;
    c.load(text, strlen(text));
    CHECK_EQ(c.set("clock", "10"), 1);
    CHECK_EQ(c.set("fd0", "GAME.XDF"), 1);
    CHECK_EQ(c.set("cpu", "68030"), 1); // ない行は最後に足す
    CHECK_EQ(c.set("fd1", ""), 1);
    CHECK_EQ(same(c, "# pix68k の設定\n"
                     "fd0 = GAME.XDF   # 起動ディスク\n"
                     "clock=10\n"
                     "ram=4\n"
                     "scsi_ex=on\n"
                     "sc0=HUMAN.HDS\n"
                     "cd=GAME.ISO\n"
                     "cd_id=6\n"
                     "\n"
                     "clock = 10 # 後の行\n"
                     "mystery=1\n"
                     "cpu=68030\n"
                     "fd1=\n"),
             1);
    // 値に # や改行は書けない
    CHECK_EQ(c.set("fd0", "A#B"), 0);
    CHECK_EQ(c.set("fd0", "A\nB"), 0);
    // 空の値の行も書き換えられる
    CHECK_EQ(c.set("fd1", "X.XDF"), 1);
    char v[64];
    CHECK_EQ(c.get("fd1", v, sizeof v), 1);
    CHECK_EQ(strcmp(v, "X.XDF"), 0);
}

void testCrlf() {
    printf("crlf\n");
    // CRLF のファイルには CRLF で足す。最後の行に改行がなければ先に足す
    const char* text = "fd0=A.XDF\r\nclock=16";
    ConfigText c;
    c.load(text, strlen(text));
    CHECK_EQ(c.set("clock", "10"), 1);
    CHECK_EQ(c.set("cpu", "68000"), 1);
    CHECK_EQ(same(c, "fd0=A.XDF\r\nclock=10\r\ncpu=68000\r\n"), 1);
    // 空のファイル
    ConfigText e;
    e.load("", 0);
    CHECK_EQ(e.set("fd0", "B.XDF"), 1);
    CHECK_EQ(same(e, "fd0=B.XDF\n"), 1);
    // 注釈の中の = や、注釈にしたキーは行とみなさない
    const char* t2 = "#fd0=OLD.XDF\nfd1=C.XDF # a=b\n";
    ConfigText d;
    d.load(t2, strlen(t2));
    CHECK_EQ(d.set("fd0", "NEW.XDF"), 1);
    CHECK_EQ(same(d, "#fd0=OLD.XDF\nfd1=C.XDF # a=b\nfd0=NEW.XDF\n"), 1);
}

void testSettings() {
    printf("settings\n");
    Settings s;
    CHECK_EQ((int)applySetting(s, "clock", "10"), (int)SettingResult::OK);
    CHECK_EQ(s.clockMHz, 10);
    CHECK_EQ((int)applySetting(s, "clock", "12"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ(s.clockMHz, 10);
    CHECK_EQ((int)applySetting(s, "cpu", "68020"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "cpu", "68060"), (int)SettingResult::OK);
    CHECK_EQ((int)applySetting(s, "ram", "13"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "ram", "12"), (int)SettingResult::OK);
    CHECK_EQ((int)applySetting(s, "local_mem", "x"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "max_speed", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.maxSpeed, 1);
    CHECK_EQ((int)applySetting(s, "max_speed", "yes"), (int)SettingResult::BAD_VALUE);
    // 060turbo の命令の速さと、最高速モードの上限 (実機の 68060 を 100 とした %)
    CHECK_EQ(s.speed060, 100);
    CHECK_EQ(s.maxSpeedLimit, 0);
    CHECK_EQ((int)applySetting(s, "speed060", "30"), (int)SettingResult::OK);
    CHECK_EQ(s.speed060, 30);
    CHECK_EQ((int)applySetting(s, "speed060", "4"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "speed060", "3201"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ(s.speed060, 30);
    CHECK_EQ(s.busSyncSprite, 0); // 既定はそろえない (#218)
    CHECK_EQ((int)applySetting(s, "bus_sync_sprite", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.busSyncSprite, 1);
    CHECK_EQ((int)applySetting(s, "bus_sync_sprite", "1600"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "bus_sync_sprite", "off"), (int)SettingResult::OK);
    CHECK_EQ(s.busSyncSprite, 0);
    CHECK_EQ((int)applySetting(s, "max_speed_limit", "100"), (int)SettingResult::OK);
    CHECK_EQ(s.maxSpeedLimit, 100);
    CHECK_EQ((int)applySetting(s, "max_speed_limit", "-1"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "keyboard", "pi400"), (int)SettingResult::OK);
    CHECK_EQ(s.keyboard, 2);
    CHECK_EQ((int)applySetting(s, "hat", "1"), (int)SettingResult::OK);
    CHECK_EQ(s.hat, 1);
    CHECK_EQ(s.padLog, 0);
    CHECK_EQ((int)applySetting(s, "pad_log", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.padLog, 1);
    CHECK_EQ((int)applySetting(s, "sram_memsize", "keep"), (int)SettingResult::OK);
    CHECK_EQ(s.fixSramMemsize, 0);
    // video_scale= と video_filter= (画面の出し方)
    {
        using namespace platform;
        Settings v;
        CHECK_EQ(v.videoScale, VIDEO_SCALE_INTEGER); // 既定は整数倍
        CHECK_EQ(v.videoFilter, CRT_OFF);            // 既定は効果なし
        static const char* const kScales[] = {"off", "integer", "aspect", "full"};
        for (int i = 0; i < 4; i++) {
            CHECK_EQ((int)applySetting(v, "video_scale", kScales[i]), (int)SettingResult::OK);
            CHECK_EQ(v.videoScale, i);
            CHECK_EQ(strcmp(videoScaleName(i), kScales[i]), 0);
        }
        CHECK_EQ((int)applySetting(v, "video_scale", "auto"), (int)SettingResult::BAD_VALUE);
        CHECK_EQ(v.videoScale, VIDEO_SCALE_FULL); // 変えない
        // video_filter= は , で並べる。順番は問わず、前後の空白と空の項目は無視する。「none」か空なら効果なし。
        // 知らない名前 (後の版で増える名前) は飛ばして、知っている名前だけを使い、IGNORED_NAME を返す。
        // 枠 (cz-600de、cz-600db) はどちらか 1 つで、両方を書いたらあとのものを使い、EXCLUSIVE_NAME を返す
        const int OK = (int)SettingResult::OK, IGN = (int)SettingResult::IGNORED_NAME,
                  EXC = (int)SettingResult::EXCLUSIVE_NAME;
        static const struct {
            const char* value;
            int bits, result;
        } kFilters[] = {
            {"scanline", CRT_SCANLINE, OK},
            {"curve", CRT_CURVE, OK},
            {"mask", CRT_MASK, OK},
            {"scanline,curve,mask", CRT_FULL, OK},
            {"mask, scanline", CRT_GRILLE, OK},
            {" curve ,scanline ", CRT_GEOM, OK},
            {"curve,curve", CRT_CURVE, OK},
            {"none", CRT_OFF, OK},
            {"", CRT_OFF, OK},
            {"scanline,", CRT_SCANLINE, OK},
            {",scanline", CRT_SCANLINE, OK},
            {"scanline,,mask", CRT_GRILLE, OK},
            {"scanline,none", CRT_SCANLINE, OK},
            {"scanline,cz-600xx", CRT_SCANLINE, IGN},
            {"scanline,cz-600de", CRT_SCANLINE | CRT_BEZEL_DE, OK},
            {"cz-600db", CRT_BEZEL_DB, OK},
            {"cz-600db,curve,mask", CRT_CURVE | CRT_MASK | CRT_BEZEL_DB, OK},
            {"cz-600de,cz-600de", CRT_BEZEL_DE, OK},
            {"cz-600de,scanline,cz-600db", CRT_SCANLINE | CRT_BEZEL_DB, EXC},
            {"cz-600db,cz-600de", CRT_BEZEL_DE, EXC},
            {"cz-600db,cz-600de,bezel", CRT_BEZEL_DE, IGN},
            {"grille", CRT_OFF, IGN},
            {"Scanline", CRT_OFF, IGN},
        };
        for (const auto& f : kFilters) {
            v.videoFilter = CRT_MASK;
            CHECK_EQ((int)applySetting(v, "video_filter", f.value), f.result);
            CHECK_EQ(v.videoFilter, f.bits);
        }
        {
            char msg[120];
            formatSettingWarning(msg, sizeof msg, "video_filter", "scanline,cz-600xx", SettingResult::IGNORED_NAME);
            CHECK_EQ(strcmp(msg, "video_filter=scanline,cz-600xx: unknown names ignored"), 0);
            formatSettingWarning(msg, sizeof msg, "video_filter", "cz-600de,cz-600db", SettingResult::EXCLUSIVE_NAME);
            CHECK_EQ(strcmp(msg, "video_filter=cz-600de,cz-600db: only one bezel can be used; using the last one"), 0);
        }
        // 書く形は「scanline,curve,mask」の順。効果がなければ「none」
        const SettingKey* fk = findSettingKey("video_filter");
        const SettingKey* sk = findSettingKey("video_scale");
        CHECK_EQ(fk != nullptr && sk != nullptr, 1);
        // どちらもホスト版でも使い、すぐに反映する (「*」を付けない)
        CHECK_EQ(fk && sk && !fk->restart && fk->host && !sk->restart && sk->host, 1);
        if (fk && sk) {
            // 枠のビットも書いて読み戻す (書く形は効果のあとに枠の名前)
            for (int i = 0; i < CRT_PRESET_COUNT * 3; i++) {
                const int b = (i & CRT_EFFECT_BITS) | (i / CRT_PRESET_COUNT == 1 ? CRT_BEZEL_DE : i / CRT_PRESET_COUNT == 2 ? CRT_BEZEL_DB : 0);
                char out[40];
                v.videoFilter = b;
                CHECK_EQ(formatSetting(v, *fk, out, sizeof out), 1);
                CHECK_EQ(strcmp(out, videoFilterName(b)), 0);
                // 書いた値を読むと同じになる
                Settings w;
                CHECK_EQ((int)applySetting(w, "video_filter", out), (int)SettingResult::OK);
                CHECK_EQ(w.videoFilter, b);
            }
            char small[6];
            v.videoFilter = CRT_FULL;
            CHECK_EQ(formatSetting(v, *fk, small, sizeof small), 0); // 入りきらない
        }
        CHECK_EQ(strcmp(videoFilterName(CRT_GRILLE), "scanline,mask"), 0);
        CHECK_EQ(strcmp(videoFilterName(CRT_FULL | CRT_BEZEL_DB), "scanline,curve,mask,cz-600db"), 0);
        CHECK_EQ(strcmp(videoFilterName(CRT_BEZEL_DE), "cz-600de"), 0);
        CHECK_EQ(strcmp(videoFilterName(CRT_BEZEL_BITS), "?"), 0);
        CHECK_EQ(videoBezel(CRT_SCANLINE | CRT_BEZEL_DB), 2);
        CHECK_EQ(videoBezelName(CRT_FULL) == nullptr, true);
        CHECK_EQ(strcmp(videoFilterName(CRT_OFF), "none"), 0);
        // 前の書き方 (scale=、scanline=、crt=) は読み替えて LEGACY を返す (知らせる)。
        // 項目の表にはない (保存では書かない)
        CHECK_EQ(findSettingKey("scale") == nullptr && findSettingKey("scanline") == nullptr &&
                     findSettingKey("crt") == nullptr && findSettingKey("curve") == nullptr &&
                     findSettingKey("mask") == nullptr,
                 1);
        CHECK_EQ(isLegacySettingKey("scale") && isLegacySettingKey("scanline") && !isLegacySettingKey("crt") &&
                     !isLegacySettingKey("video_scale"),
                 1);
        // 行を順に読んだあとの値 (公開版 v0.1.0 のベアメタル版と同じ見た目になる。scale=off は 1 倍、auto は整数倍、
        // scanline=on はスキャンライン)。legacy は前の書き方の行の数、removed は crt= の行の数
        static const struct {
            const char* lines;
            int scale, filter, legacy, removed;
        } kLegacy[] = {
            {"scale=off", VIDEO_SCALE_OFF, CRT_OFF, 1, 0},
            {"scale=auto", VIDEO_SCALE_INTEGER, CRT_OFF, 1, 0},
            {"scanline=on", VIDEO_SCALE_INTEGER, CRT_SCANLINE, 1, 0},
            {"scanline=on\nscanline=off", VIDEO_SCALE_INTEGER, CRT_OFF, 2, 0},
            {"scale=off\nscanline=on", VIDEO_SCALE_OFF, CRT_SCANLINE, 2, 0},
            {"scale=off\nscale=auto", VIDEO_SCALE_INTEGER, CRT_OFF, 2, 0},
            // いまの書き方とまざったら、キーごとにあとの行が勝つ
            {"video_scale=full\nscale=auto", VIDEO_SCALE_INTEGER, CRT_OFF, 1, 0},
            {"scale=off\nvideo_scale=full", VIDEO_SCALE_FULL, CRT_OFF, 1, 0},
            {"video_filter=mask\nscanline=on", VIDEO_SCALE_INTEGER, CRT_SCANLINE, 1, 0},
            {"scanline=on\nvideo_filter=mask", VIDEO_SCALE_INTEGER, CRT_MASK, 1, 0},
            {"scanline=on\nvideo_filter=mask\nscanline=off", VIDEO_SCALE_INTEGER, CRT_OFF, 2, 0},
            // scale= と video_filter=、scanline= と video_scale= は互いに関係しない
            {"scale=off\nvideo_filter=curve", VIDEO_SCALE_OFF, CRT_CURVE, 1, 0},
            {"video_scale=aspect\nscanline=on", VIDEO_SCALE_ASPECT, CRT_SCANLINE, 1, 0},
            // 受け付けなかった video_scale= / video_filter= の行は、前の scale= / scanline= を消さない
            {"scale=off\nvideo_scale=typo", VIDEO_SCALE_OFF, CRT_OFF, 1, 0},
            {"scanline=on\nvideo_filter=scanline,", VIDEO_SCALE_INTEGER, CRT_SCANLINE, 1, 0},
            {"scanline=on\nvideo_filter=cz-600xx", VIDEO_SCALE_INTEGER, CRT_OFF, 1, 0}, // 知らない名前だけでも受け付けた行
            {"scanline=on\nvideo_filter=cz-600de", VIDEO_SCALE_INTEGER, CRT_BEZEL_DE, 1, 0}, // 枠だけの行も、前の scanline= に勝つ
            // crt= (公開版に出ていない) は読み替えずに無視する
            {"crt=full", VIDEO_SCALE_INTEGER, CRT_OFF, 0, 1},
            {"crt=full\nscanline=off", VIDEO_SCALE_INTEGER, CRT_OFF, 1, 1},
            {"scale=off\ncrt=grille", VIDEO_SCALE_OFF, CRT_OFF, 1, 1},
            {"video_filter=mask\ncrt=off", VIDEO_SCALE_INTEGER, CRT_MASK, 0, 1},
        };
        for (const auto& l : kLegacy) {
            ConfigText c;
            c.load(l.lines, strlen(l.lines));
            Settings t;
            int legacy = 0, removed = 0;
            readSettings(
                c, t,
                [&](const char*, const char*, SettingResult r) {
                    legacy += r == SettingResult::LEGACY;
                    removed += r == SettingResult::REMOVED;
                },
                [](const char*) {});
            CHECK_EQ(t.videoScale, l.scale);
            CHECK_EQ(t.videoFilter, l.filter);
            CHECK_EQ(legacy, l.legacy);
            CHECK_EQ(removed, l.removed);
        }
        {
            // ホスト版 (legacyVideo が 0) は、v0.1.0 のホスト版と同じく scale= と scanline= を読まず、知らせもしない
            const char* text = "scale=off\nscanline=on\nscale=typo\nscanline=typo\n";
            ConfigText c;
            c.load(text, strlen(text));
            Settings h;
            h.videoScale = VIDEO_SCALE_ASPECT;
            h.legacyVideo = 0;
            int warned = 0;
            readSettings(c, h, [&](const char*, const char*, SettingResult) { warned++; }, [](const char*) {});
            CHECK_EQ(warned, 0);
            CHECK_EQ(h.videoScale, (int)VIDEO_SCALE_ASPECT);
            CHECK_EQ(h.videoFilter, (int)CRT_OFF);
        }
        Settings b;
        CHECK_EQ((int)applySetting(b, "crt", "on"), (int)SettingResult::REMOVED);
        CHECK_EQ((int)applySetting(b, "scale", "2"), (int)SettingResult::BAD_VALUE);
        CHECK_EQ((int)applySetting(b, "scanline", "50"), (int)SettingResult::BAD_VALUE);
        CHECK_EQ(b.videoScale == VIDEO_SCALE_INTEGER && b.videoFilter == CRT_OFF, 1); // 変えない
        CHECK_EQ((int)applySetting(b, "curve", "on"), (int)SettingResult::UNKNOWN_KEY); // 出していない書き方
        // ログの文言
        char msg[160];
        formatSettingWarning(msg, sizeof msg, "crt", "full", SettingResult::REMOVED);
        CHECK_EQ(strcmp(msg, "crt=full is no longer supported; use video_filter= and video_scale= (ignored)"), 0);
        formatSettingWarning(msg, sizeof msg, "scale", "auto", SettingResult::LEGACY);
        CHECK_EQ(strcmp(msg, "scale=auto is old; use video_scale= (Save settings rewrites it)"), 0);
        formatSettingWarning(msg, sizeof msg, "scanline", "on", SettingResult::LEGACY);
        CHECK_EQ(strcmp(msg, "scanline=on is old; use video_filter= (Save settings rewrites it)"), 0);
    }
    CHECK_EQ(s.osd, 2); // 既定は on (余白に入るときだけ出す)
    CHECK_EQ((int)applySetting(s, "osd", "fps"), (int)SettingResult::OK);
    CHECK_EQ(s.osd, 1);
    CHECK_EQ((int)applySetting(s, "osd", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.osd, 2);
    CHECK_EQ((int)applySetting(s, "osd", "yes"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "osd", "off"), (int)SettingResult::OK);
    CHECK_EQ(s.osd, 0);
    CHECK_EQ((int)applySetting(s, "fd1", "DISK.XDF"), (int)SettingResult::OK);
    CHECK_EQ(strcmp(s.fd[1], "DISK.XDF"), 0);
    CHECK_EQ((int)applySetting(s, "scsi_ex", "on"), (int)SettingResult::OK); // PR #38 の項目
    CHECK_EQ((int)applySetting(s, "sc7", "X.HDS"), (int)SettingResult::UNKNOWN_KEY);
    CHECK_EQ((int)applySetting(s, "cd_id", "7"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "mystery", "1"), (int)SettingResult::UNKNOWN_KEY);
    CHECK_EQ((int)applySetting(s, "neptune", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.neptune, 1);
    CHECK_EQ((int)applySetting(s, "neptune_mac", "02:68:6B:00:00:2a"), (int)SettingResult::OK);
    uint8_t mac[6];
    CHECK_EQ(platform::parseMac(s.neptuneMac, mac), 1);
    CHECK_EQ(mac[3] << 16 | mac[4] << 8 | mac[5], 0x00002a);
    CHECK_EQ(mac[2], 0x6b);
    CHECK_EQ(platform::parseMac("02-00-00-00-00-01", mac), 1);
    CHECK_EQ(platform::parseMac("03:00:00:00:00:01", mac), 0); // マルチキャスト
    CHECK_EQ(platform::parseMac("02:00:00:00:00", mac), 0);
    CHECK_EQ(platform::parseMac("02:00:00:00:00:011", mac), 0);
    CHECK_EQ(platform::parseMac("02:00:00:00:0g:01", mac), 0);
    // 数値演算プロセッサボード: on / off と、2 枚の 2
    CHECK_EQ(s.fpuBoard, 0); // 既定は off
    CHECK_EQ((int)applySetting(s, "fpu_board", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.fpuBoard, 1);
    CHECK_EQ((int)applySetting(s, "fpu_board", "2"), (int)SettingResult::OK);
    CHECK_EQ(s.fpuBoard, 2);
    CHECK_EQ((int)applySetting(s, "fpu_board", "3"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "fpu_board", "yes"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ(s.fpuBoard, 2);
    {
        char fv[8];
        const platform::SettingKey* fk = platform::findSettingKey("fpu_board");
        CHECK_EQ(fk != nullptr && fk->restart && fk->host, 1);
        CHECK_EQ(formatSetting(s, *fk, fv, sizeof fv), 1);
        CHECK_EQ(strcmp(fv, "2"), 0); // 2 枚は 2 のまま書く (on に丸めない)
        Settings f;
        CHECK_EQ((int)applySetting(f, "fpu_board", "1"), (int)SettingResult::OK);
        CHECK_EQ(f.fpuBoard, 1);
        CHECK_EQ(formatSetting(f, *fk, fv, sizeof fv), 1);
        CHECK_EQ(strcmp(fv, "on"), 0);
        CHECK_EQ((int)applySetting(f, "fpu_board", "0"), (int)SettingResult::OK);
        CHECK_EQ(formatSetting(f, *fk, fv, sizeof fv), 1);
        CHECK_EQ(strcmp(fv, "off"), 0);
    }
    // MIDI ボード
    CHECK_EQ(s.midi, 0); // 既定は off
    CHECK_EQ(s.midiOut, 0);
    CHECK_EQ(s.midiVoices, 48);
    CHECK_EQ((int)applySetting(s, "midi", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.midi, 1);
    CHECK_EQ((int)applySetting(s, "midi_out", "uart"), (int)SettingResult::OK);
    CHECK_EQ(s.midiOut, 1);
    CHECK_EQ((int)applySetting(s, "midi_out", "usb"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "midi_sf2", "GeneralUser.sf2"), (int)SettingResult::OK);
    CHECK_EQ(strcmp(s.midiSf2, "GeneralUser.sf2"), 0);
    CHECK_EQ((int)applySetting(s, "midi_voices", "3"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "midi_voices", "32"), (int)SettingResult::OK);
    CHECK_EQ(s.midiVoices, 32);
    CHECK_EQ((int)applySetting(s, "midi_out", "mt32"), (int)SettingResult::OK);
    CHECK_EQ(s.midiOut, 3);
    CHECK_EQ(s.midiMt32Ctrl[0], 0);
    CHECK_EQ((int)applySetting(s, "midi_mt32_ctrl", "MT32_CONTROL.ROM"), (int)SettingResult::OK);
    CHECK_EQ(strcmp(s.midiMt32Ctrl, "MT32_CONTROL.ROM"), 0);
    CHECK_EQ((int)applySetting(s, "midi_mt32_pcm", "/roms/MT32_PCM.ROM"), (int)SettingResult::OK);
    CHECK_EQ(strcmp(s.midiMt32Pcm, "/roms/MT32_PCM.ROM"), 0);
    CHECK_EQ((int)applySetting(s, "midi_out", "sc55"), (int)SettingResult::OK);
    CHECK_EQ(s.midiOut, 4);
    CHECK_EQ(s.midiSc55[0], 0);
    CHECK_EQ((int)applySetting(s, "midi_sc55", "SC55MK2"), (int)SettingResult::OK);
    CHECK_EQ(strcmp(s.midiSc55, "SC55MK2"), 0);
    CHECK_EQ(s.midiUart, 0);
    CHECK_EQ((int)applySetting(s, "midi_uart", "3"), (int)SettingResult::OK);
    CHECK_EQ(s.midiUart, 3);
    CHECK_EQ((int)applySetting(s, "midi_uart", "10"), (int)SettingResult::OK);
    CHECK_EQ(s.midiUart, 10);
    CHECK_EQ((int)applySetting(s, "midi_uart", "6"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "midi_uart", "11"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ(s.midiUart, 10);
    CHECK_EQ(s.midiUartFamily, 0);
    CHECK_EQ((int)applySetting(s, "midi_uart_family", "mt32"), (int)SettingResult::OK);
    CHECK_EQ(s.midiUartFamily, 1);
    CHECK_EQ((int)applySetting(s, "midi_uart_family", "gm"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "midi_uart_family", "gs"), (int)SettingResult::OK);
    CHECK_EQ(s.midiUartFamily, 0);
    const char* arg = nullptr;
    CHECK_EQ((int)platform::parseNeptuneLink("", arg), (int)platform::NeptuneLink::DEFAULT);
    CHECK_EQ((int)platform::parseNeptuneLink("echo", arg), (int)platform::NeptuneLink::ECHO);
    CHECK_EQ((int)platform::parseNeptuneLink("pcap:en0", arg), (int)platform::NeptuneLink::PCAP);
    CHECK_EQ(strcmp(arg, "en0"), 0);
    CHECK_EQ((int)platform::parseNeptuneLink("pcap", arg), (int)platform::NeptuneLink::BAD);
    CHECK_EQ((int)platform::parseNeptuneLink("echo:x", arg), (int)platform::NeptuneLink::BAD);
    CHECK_EQ((int)platform::parseNeptuneLink("tap:tap0", arg), (int)platform::NeptuneLink::TAP);

    // 書く形
    char v[32];
    const platform::SettingKey* k = platform::findSettingKey("keyboard");
    CHECK_EQ(formatSetting(s, *k, v, sizeof v), 1);
    CHECK_EQ(strcmp(v, "pi400us"), 0);
    k = platform::findSettingKey("local_mem");
    CHECK_EQ(formatSetting(Settings(), *k, v, sizeof v), 0); // 書いていない (-1)
    k = platform::findSettingKey("hat");
    CHECK_EQ(formatSetting(Settings(), *k, v, sizeof v), 1);
    CHECK_EQ(strcmp(v, "auto"), 0);
    // rs232c= (RS-232C の相手)
    {
        unsigned port = 1;
        char addr[16] = "x";
        CHECK_EQ((int)platform::parseRs232c("", port, addr, sizeof addr), (int)platform::Rs232cLink::OFF);
        CHECK_EQ((int)platform::parseRs232c("off", port, addr, sizeof addr), (int)platform::Rs232cLink::OFF);
        CHECK_EQ((int)platform::parseRs232c("tcp:2324", port, addr, sizeof addr), (int)platform::Rs232cLink::TCP);
        CHECK_EQ(port, 2324u);
        CHECK_EQ(addr[0], 0);
        CHECK_EQ((int)platform::parseRs232c("tcp:0.0.0.0:23", port, addr, sizeof addr), (int)platform::Rs232cLink::TCP);
        CHECK_EQ(port, 23u);
        CHECK_EQ(strcmp(addr, "0.0.0.0"), 0);
        static const char* const kBad[] = {"tcp:", "tcp:0", "tcp:65536", "tcp:12x", "udp:1", "tcp::5", "tcp:1:",
                                           "tcp:255.255.255.255.1:5"};
        for (const char* v : kBad)
            CHECK_EQ((int)platform::parseRs232c(v, port, addr, sizeof addr), (int)platform::Rs232cLink::BAD);
        Settings r;
        CHECK_EQ((int)applySetting(r, "rs232c", "tcp:2324"), (int)SettingResult::OK);
        CHECK_EQ(strcmp(r.rs232c, "tcp:2324"), 0);
    }
    // 表のすべての項目で、既定の値を書いて読むと同じになる
    for (const platform::SettingKey& key : platform::kSettingKeys) {
        Settings a, b;
        if (!formatSetting(a, key, v, sizeof v)) continue;
        CHECK_EQ((int)applySetting(b, key.key, v), (int)SettingResult::OK);
        CHECK_EQ(sameSetting(a, b, key), 1);
    }

    // Config に写す: 68030 のときは clock を使わない、060turbo のローカルメモリの既定は 128MB
    Settings c;
    applySetting(c, "cpu", "68060");
    x68k::Config cfg;
    c.toConfig(cfg);
    if (x68k::Config::has020()) {
        CHECK_EQ(cfg.cpuHz, 50000000u);
        CHECK_EQ(cfg.localMemSize, 128u << 20);
    }
    CHECK_EQ(cfg.speed060, 100);
    CHECK_EQ(cfg.maxSpeedLimit060, 0);
    // スプライトの読み書きは、既定ではそろえない。bus_sync_sprite=on で I/O と同じ 1600ns になる (#218)
    if (x68k::Config::has020()) {
        CHECK_EQ(cfg.busArea[x68k::Config::BUS_AREA_RTC].read.period, 1600);
        CHECK_EQ(cfg.busArea[x68k::Config::BUS_AREA_SPRITE].write.period, 0);
        applySetting(c, "bus_sync_sprite", "on");
        c.toConfig(cfg);
        CHECK_EQ(cfg.busArea[x68k::Config::BUS_AREA_SPRITE].write.period, 1600);
        CHECK_EQ(cfg.busArea[x68k::Config::BUS_AREA_PCG].write.period, 1600);
        applySetting(c, "bus_sync_sprite", "off");
        c.toConfig(cfg);
        CHECK_EQ(cfg.busArea[x68k::Config::BUS_AREA_SPRITE].write.period, 0);
    }
    applySetting(c, "speed060", "40");
    applySetting(c, "max_speed_limit", "150");
    c.toConfig(cfg);
    CHECK_EQ(cfg.speed060, 40);
    CHECK_EQ(cfg.maxSpeedLimit060, 150);
    Settings d;
    applySetting(d, "clock", "10");
    x68k::Config cfg2;
    d.toConfig(cfg2);
    CHECK_EQ(cfg2.cpuHz, 10000000u);
    CHECK_EQ(cfg2.localMemSize, 0u);

    // clock= は CPU ごとに読む: 68060 は 50 (既定) か 75、68000 は 10 か 16 (既定)。合わない値はその CPU の既定
    Settings e;
    CHECK_EQ((int)applySetting(e, "clock", "75"), (int)SettingResult::OK);
    CHECK_EQ((int)applySetting(e, "clock", "50"), (int)SettingResult::OK);
    CHECK_EQ((int)applySetting(e, "clock", "60"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(e, "clock", "25"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ(Settings::clockFor(68000, 75), 16);
    CHECK_EQ(Settings::clockFor(68000, 10), 10);
    CHECK_EQ(Settings::clockFor(68030, 10), 25);
    CHECK_EQ(Settings::clockFor(68060, 16), 50); // 既定の clock=16 のままの cpu=68060 は 50MHz
    CHECK_EQ(Settings::clockFor(68060, 75), 75);
    if (x68k::Config::has020()) {
        Settings f;
        applySetting(f, "cpu", "68060");
        x68k::Config cfg3;
        f.toConfig(cfg3);
        CHECK_EQ(cfg3.cpuHz, 50000000u);
        CHECK_EQ(cfg3.busWait060, 6);
        applySetting(f, "clock", "75");
        x68k::Config cfg4;
        f.toConfig(cfg4);
        CHECK_EQ(cfg4.cpuHz, 75000000u);
        CHECK_EQ(cfg4.busWait060, 9);
        applySetting(f, "cpu", "68000");
        x68k::Config cfg5;
        f.toConfig(cfg5);
        CHECK_EQ(cfg5.cpuHz, 16000000u); // 75 は 68000 では使わない
    }
}

// メニューで取り出して保存すると「fd0=」を書く (ベアメタル版は空の fd0= を「ドライブを空にする」と読む)
void testEject() {
    printf("eject\n");
    const char* text = "fd0=GAME.XDF # 起動\nclock=10\n";
    ConfigText c;
    c.load(text, strlen(text));
    Settings base;
    platform::readSettings(c, base, [](const char*, const char*, SettingResult) {}, [](const char*) {});
    Settings s = base;
    s.fd[0][0] = 0;
    CHECK_EQ(platform::writeSettings(c, s, base), 1);
    CHECK_EQ(same(c, "fd0= # 起動\nclock=10\n"), 1);
    char v[8];
    CHECK_EQ(c.get("fd0", v, sizeof v), 1); // 行はある (空の値)
    CHECK_EQ(v[0], 0);
}

// bus_sync_sprite を変えて保存し、読み直すと同じ値になる (on にする場合と、off に戻す場合。#218)
void testBusSyncSpriteSave() {
    printf("bus_sync_sprite: save and reload\n");
    const auto read = [](const ConfigText& c) {
        Settings s;
        platform::readSettings(c, s, [](const char*, const char*, SettingResult) {}, [](const char*) {});
        return s;
    };
    const char* text = "cpu=68060\n";
    ConfigText c;
    c.load(text, strlen(text));
    const Settings base = read(c);
    CHECK_EQ(base.busSyncSprite, 0);
    // 変えていなければ行を足さない
    CHECK_EQ(platform::writeSettings(c, base, base), 1);
    CHECK_EQ(same(c, "cpu=68060\n"), 1);
    // off (既定) から on
    Settings on = base;
    on.busSyncSprite = 1;
    CHECK_EQ(platform::writeSettings(c, on, base), 1);
    CHECK_EQ(same(c, "cpu=68060\nbus_sync_sprite=on\n"), 1);
    const Settings reloaded = read(c);
    CHECK_EQ(reloaded.busSyncSprite, 1);
    x68k::Config cfg;
    reloaded.toConfig(cfg);
    if (x68k::Config::has020()) CHECK_EQ(cfg.busArea[x68k::Config::BUS_AREA_SPRITE].write.period, x68k::Config::BUS_SYNC_NS);
    // on から off
    Settings off = reloaded;
    off.busSyncSprite = 0;
    CHECK_EQ(platform::writeSettings(c, off, reloaded), 1);
    CHECK_EQ(same(c, "cpu=68060\nbus_sync_sprite=off\n"), 1);
    const Settings again = read(c);
    CHECK_EQ(again.busSyncSprite, 0);
    x68k::Config cfg2;
    again.toConfig(cfg2);
    if (x68k::Config::has020()) CHECK_EQ(cfg2.busArea[x68k::Config::BUS_AREA_SPRITE].write.period, 0);
}

// 前の書き方の行 (v0.1.0 の scale=、scanline=) は、保存するときに消し、読んだときの値で video_scale= と video_filter= を
// 書く (行があれば最後の行を書き換え、なければ足す)。変えた項目がなくても書き直す。crt= の行は触らない
void testLegacySave() {
    printf("legacy video settings: rewritten on save\n");
    using namespace platform;
    const auto read = [](const ConfigText& c) {
        Settings s;
        readSettings(c, s, [](const char*, const char*, SettingResult) {}, [](const char*) {});
        return s;
    };
    {
        const char* text = "clock=10\r\n  scale = off  # 1 倍\r\nscanline=on\r\nosd=on\r\n";
        ConfigText c;
        c.load(text, strlen(text));
        const Settings base = read(c);
        CHECK_EQ(base.videoFilter, CRT_SCANLINE);
        CHECK_EQ(base.videoScale, VIDEO_SCALE_OFF);
        CHECK_EQ(writeSettings(c, base, base), 1);
        CHECK_EQ(same(c, "clock=10\r\nosd=on\r\nvideo_scale=off\r\nvideo_filter=scanline\r\n"), 1);
        const Settings again = read(c);
        CHECK_EQ(again.videoFilter == base.videoFilter && again.videoScale == base.videoScale, 1);
        // 変えた値は、書いた行を書き換える
        Settings s = base;
        s.videoFilter = CRT_MASK;
        CHECK_EQ(writeSettings(c, s, again), 1);
        CHECK_EQ(same(c, "clock=10\r\nosd=on\r\nvideo_scale=off\r\nvideo_filter=mask\r\n"), 1);
    }
    {
        // scale= だけなら video_scale= だけ、scanline= だけなら video_filter= だけを書く。値が正しくない行も消す。
        // crt= の行は残す (読み替えないので、ログの知らせを見て直してもらう)
        const char* text = "scale=off\n";
        ConfigText c;
        c.load(text, strlen(text));
        const Settings base = read(c);
        CHECK_EQ(writeSettings(c, base, base), 1);
        CHECK_EQ(same(c, "video_scale=off\n"), 1);
        const char* text2 = "scanline=on\ncrt=full\nscanline=bogus\n";
        ConfigText c2;
        c2.load(text2, strlen(text2));
        const Settings base2 = read(c2);
        CHECK_EQ(migrateLegacySettings(c2, base2), 1);
        CHECK_EQ(same(c2, "crt=full\nvideo_filter=scanline\n"), 1);
        // crt= だけなら何もしない
        const char* text3 = "crt=full\n";
        ConfigText c3;
        c3.load(text3, strlen(text3));
        CHECK_EQ(migrateLegacySettings(c3, read(c3)), 0);
        CHECK_EQ(same(c3, "crt=full\n"), 1);
        // ホスト版は scale= と scanline= を読まないので、行も書き直さない
        Settings host = read(c);
        host.legacyVideo = 0;
        ConfigText c4;
        c4.load(text, strlen(text));
        CHECK_EQ(migrateLegacySettings(c4, host), 0);
        CHECK_EQ(same(c4, "scale=off\n"), 1);
    }
    {
        // いまの書き方の行があれば、そこを書き換える (あとの scanline=on が勝つので scanline だけ)
        const char* text = "video_filter=mask\nscanline=on\n";
        ConfigText c;
        c.load(text, strlen(text));
        CHECK_EQ(migrateLegacySettings(c, read(c)), 1);
        CHECK_EQ(same(c, "video_filter=scanline\n"), 1);
    }
    {
        // 前の書き方の行がなければ何も変えない
        const char* text = "video_filter=curve\n";
        ConfigText c;
        c.load(text, strlen(text));
        CHECK_EQ(migrateLegacySettings(c, read(c)), 0);
        CHECK_EQ(same(c, "video_filter=curve\n"), 1);
    }
    {
        // removeLines() は key の行をすべて消す (注釈の行、ほかのキーは残す)
        const char* text = "# crt=full\ncrt=full\ncrtx=1\n crt = off # x\n";
        ConfigText c;
        c.load(text, strlen(text));
        CHECK_EQ(c.removeLines("crt"), 2);
        CHECK_EQ(same(c, "# crt=full\ncrtx=1\n"), 1);
    }
}

} // namespace

int main() {
    testRead();
    testWrite();
    testCrlf();
    testSettings();
    testEject();
    testBusSyncSpriteSave();
    testLegacySave();
    return finish();
}
