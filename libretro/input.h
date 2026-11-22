// libretro のキーボードは SDL2 の scancode (USB HID) とは別の値。
#pragma once

#include "libretro.h"
#include <stdint.h>

namespace pix68k_retro {
inline uint8_t keyUsage(unsigned key) {
    if (key >= RETROK_a && key <= RETROK_z) return (uint8_t)(4 + key - RETROK_a);
    if (key >= RETROK_1 && key <= RETROK_9) return (uint8_t)(0x1e + key - RETROK_1);
    if (key >= RETROK_F1 && key <= RETROK_F12) return (uint8_t)(0x3a + key - RETROK_F1);
    if (key >= RETROK_F13 && key <= RETROK_F15) return (uint8_t)(0x68 + key - RETROK_F13);
    if (key >= RETROK_KP1 && key <= RETROK_KP9) return (uint8_t)(0x59 + key - RETROK_KP1);
    switch (key) {
    case RETROK_0: return 0x27;
    case RETROK_RETURN: return 0x28;
    case RETROK_ESCAPE: return 0x29;
    case RETROK_BACKSPACE: return 0x2a;
    case RETROK_TAB: return 0x2b;
    case RETROK_SPACE: return 0x2c;
    case RETROK_MINUS: return 0x2d;
    case RETROK_EQUALS: return 0x2e;
    case RETROK_LEFTBRACKET: return 0x2f;
    case RETROK_RIGHTBRACKET: return 0x30;
    case RETROK_BACKSLASH: return 0x31;
    case RETROK_SEMICOLON: return 0x33;
    case RETROK_QUOTE: return 0x34;
    case RETROK_BACKQUOTE: return 0x35;
    case RETROK_COMMA: return 0x36;
    case RETROK_PERIOD: return 0x37;
    case RETROK_SLASH: return 0x38;
    case RETROK_CAPSLOCK: return 0x39;
    case RETROK_PRINT: return 0x46;
    case RETROK_SCROLLOCK: return 0x47;
    case RETROK_PAUSE: return 0x48;
    case RETROK_INSERT: return 0x49;
    case RETROK_HOME: return 0x4a;
    case RETROK_PAGEUP: return 0x4b;
    case RETROK_DELETE: return 0x4c;
    case RETROK_END: return 0x4d;
    case RETROK_PAGEDOWN: return 0x4e;
    case RETROK_RIGHT: return 0x4f;
    case RETROK_LEFT: return 0x50;
    case RETROK_DOWN: return 0x51;
    case RETROK_UP: return 0x52;
    case RETROK_NUMLOCK: return 0x53;
    case RETROK_KP_DIVIDE: return 0x54;
    case RETROK_KP_MULTIPLY: return 0x55;
    case RETROK_KP_MINUS: return 0x56;
    case RETROK_KP_PLUS: return 0x57;
    case RETROK_KP_ENTER: return 0x58;
    case RETROK_KP0: return 0x62;
    case RETROK_KP_PERIOD: return 0x63;
    case RETROK_KP_EQUALS: return 0x67;
    case RETROK_HELP: return 0x75;
    case RETROK_BREAK: return 0x78;
    case RETROK_UNDO: return 0x7a;
    case RETROK_MODE: return 0x6b; // F16 相当: かな (International2 は XF4 なので使わない)
    case RETROK_LCTRL: return 0xe0;
    case RETROK_LSHIFT: return 0xe1;
    case RETROK_LALT: return 0xe2;
    case RETROK_LMETA:
    case RETROK_LSUPER: return 0xe3;
    case RETROK_RCTRL: return 0xe4;
    case RETROK_RSHIFT: return 0xe5;
    case RETROK_RALT: return 0xe6;
    case RETROK_RMETA:
    case RETROK_RSUPER: return 0xe7;
    default: return 0;
    }
}
} // namespace pix68k_retro
