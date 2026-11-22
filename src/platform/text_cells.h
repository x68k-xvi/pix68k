// 1 行の字を升目に並べて描く。起動時のメニュー (platform/boot_profile.h) と設定メニュー (platform/menu.h) が使う。
//
// 升目のコードは platform/osd_message.h の升目と同じ: ASCII、半角カタカナ 0xA1-0xDF、全角の区点か MSG_CELL_TOFU、
// その右の桁は MSG_CELL_RIGHT。ASCII は platform/menu_font.h、半角カタカナと全角は CGROM の字で描く
// (CGROM がなければ、半角カタカナは「?」、全角は豆腐)。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/menu_font.h"
#include "platform/osd_message.h"

namespace platform {

namespace text_detail {
// Shift_JIS の 2 バイト文字の 1 バイト目と 2 バイト目か
inline bool sjisLead(uint8_t c) { return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc); }
inline bool sjisTrail(uint8_t c) { return c >= 0x40 && c <= 0xfc && c != 0x7f; }

// Shift_JIS の s[i] から 1 字を読み、i を進める。終わりなら false。code は升目のコード (ASCII、半角カタカナ
// 0xA1-0xDF、全角の区点)、width は桁数。2 バイト目が正しくない、終わりで切れた、使わないバイト (0x80、0xA0、
// 0xFD-0xFF) は、1 バイトだけ進めて「?」にする。区点は 0x7E 区より先 (0xF0 から先の外字) もそのまま返す
// (CGROM にない区点は drawCells() が豆腐にする)
inline bool sjisNext(const char* s, size_t n, size_t& i, uint16_t& code, int& width) {
    if (i >= n) return false;
    const uint8_t c = (uint8_t)s[i];
    width = 1;
    if (sjisLead(c) && i + 1 < n && sjisTrail((uint8_t)s[i + 1])) {
        const uint8_t d = (uint8_t)s[i + 1];
        int row = (c - (c >= 0xe0 ? 0xc1 : 0x81)) * 2 + 0x21, col;
        if (d >= 0x9f) {
            row++;
            col = d - 0x7e;
        } else {
            col = d - (d >= 0x80 ? 0x20 : 0x1f);
        }
        code = (uint16_t)(row << 8 | col);
        width = 2;
        i += 2;
        return true;
    }
    code = c < 0x80 || (c >= 0xa1 && c <= 0xdf) ? c : '?';
    i++;
    return true;
}

} // namespace text_detail

// UTF-8 (sjis なら Shift_JIS) の s を、maxCols 桁 (半角の桁。全角は 2 桁) の升目に並べる (コードは
// platform/osd_message.h の升目と同じ: ASCII、半角カタカナ 0xA1-0xDF、全角の区点か MSG_CELL_TOFU、その右の桁は
// MSG_CELL_RIGHT)。Shift_JIS は Unicode を経ずに区点に直す (text_detail::sjisNext())。
// 入りきらなければ、最後を「…」(全角) にする。並べた桁数を返す
inline int layoutLine(const char* s, uint16_t* cells, int maxCols, bool sjis = false) {
    int col = 0;
    size_t i = 0;
    const size_t n = strlen(s);
    bool cut = false;
    for (;;) {
        uint16_t code;
        int width = 1;
        if (sjis) {
            if (!text_detail::sjisNext(s, n, i, code, width)) break;
            if (code == '\t') code = ' ';
            if (code < 0x20 || code == 0x7f) continue;
        } else {
            uint32_t cp;
            if (!utf8Next(s, n, i, cp)) break;
            if (cp == '\t') cp = ' ';
            if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0)) continue;
            if (cp < 0x80) code = (uint16_t)cp;
            else if (cp == 0xfffd) code = '?';
            else if (cp >= 0xff61 && cp <= 0xff9f) code = (uint16_t)(cp - 0xff61 + 0xa1);
            else if (const uint16_t jis = unicodeToJis(cp)) {
                code = jis;
                width = 2;
            } else if (cp >= 0x1100) {
                code = MSG_CELL_TOFU;
                width = 2;
            } else {
                code = '?';
            }
        }
        if (col + width > maxCols) {
            cut = true;
            break;
        }
        cells[col++] = code;
        if (width == 2) cells[col++] = MSG_CELL_RIGHT;
    }
    if (!cut || maxCols < 2) return col;
    int at = col > maxCols - 2 ? maxCols - 2 : col;
    if (at > 0 && at < col && cells[at] == MSG_CELL_RIGHT) at--;
    cells[at] = MSG_JIS_ELLIPSIS;
    cells[at + 1] = MSG_CELL_RIGHT;
    return at + 2;
}

// 升目の字を描く (8 * scale x 16 * scale の桁)。xmax と ymax を超える分は描かない。cgrom がなければ全角は豆腐
inline void drawCells(uint32_t* pixels, int pitch, int xmax, int ymax, int x, int y, const uint16_t* cells, int cols,
                      int scale, uint32_t color, const uint8_t* cgrom, size_t cgromSize) {
    for (int c = 0; c < cols; c++) {
        const uint16_t code = cells[c];
        if (code == MSG_CELL_RIGHT) continue;
        uint8_t g[2][16] = {};
        int width = 1;
        if (code >= MENU_FONT_FIRST && code <= MENU_FONT_LAST) {
            memcpy(g[0], kMenuFont[code - MENU_FONT_FIRST], 16);
        } else if (code >= 0xa1 && code <= 0xdf) {
            const size_t off = CGROM_FONT8X16 + (size_t)code * 16;
            if (cgrom != nullptr && off + 16 <= cgromSize) memcpy(g[0], cgrom + off, 16);
            else memcpy(g[0], kMenuFont['?' - MENU_FONT_FIRST], 16);
        } else {
            width = 2;
            const long off = code == MSG_CELL_TOFU ? -1 : cgromFont16Offset(code);
            bool drawn = false;
            if (cgrom != nullptr && off >= 0 && (size_t)off + 32 <= cgromSize) {
                uint8_t any = 0;
                for (int k = 0; k < 16; k++) {
                    g[0][k] = cgrom[off + 2 * k];
                    g[1][k] = cgrom[off + 2 * k + 1];
                    any |= g[0][k] | g[1][k];
                }
                drawn = any != 0 || code == 0x2121;
            }
            if (!drawn && code == MSG_JIS_ELLIPSIS) message_detail::ellipsis(g[0], g[1]);
            else if (!drawn) message_detail::tofu(g[0], g[1]);
        }
        for (int h = 0; h < width; h++) {
            const int x0 = x + (c + h) * 8 * scale;
            for (int gy = 0; gy < 16 * scale; gy++) {
                const int py = y + gy;
                if (py < 0 || py >= ymax) continue;
                const uint8_t bits = g[h][gy / scale];
                uint32_t* row = pixels + (size_t)py * pitch;
                for (int gx = 0; gx < 8 * scale; gx++) {
                    const int px = x0 + gx;
                    if (px >= 0 && px < xmax && (bits & (0x80 >> (gx / scale)))) row[px] = color;
                }
            }
        }
    }
}

} // namespace platform
