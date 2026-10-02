// Text handling shared by rejseplanenClient (API text) and the main sketch (station names typed into
// the web config): converting the UTF-8 the outside world uses into the single-byte form the board's
// strings are kept in, and the private codes used for letters that single-byte Latin-1 can't hold.
#pragma once
#include <Arduino.h>

// Czech/Slovak letters. Latin-1 (what the board's strings are stored as - one byte per character) has
// every German letter and most of the Czech ones with just an acute (á é í ó ú ý), but NOT the caron
// letters č ď ě ň ř š ť ž or ů - they live in Unicode "Latin Extended-A" (U+0100-017F), which doesn't
// fit in a byte. Rather than widening every string buffer in the firmware, each of these 18 letters
// is stored as one of the otherwise-unused control codes 14..31 (nothing in this firmware's text ever
// uses codes below 32; 14 up, specifically, so none of them can be mistaken for tab/newline/return),
// and expanded back to its real Unicode code point only at the moment it is drawn - see
// boardExtCodepointFor() and drawMixedStr()/getMixedStringWidth() in "Departures Board.cpp".
//
// Sorted by code point, so index + BOARD_EXT_FIRST is the private code. Upper/lower alternate, upper
// first - the drawing code relies on that (an even code is a capital) to pick the right baseline
// reference letter.
#define BOARD_EXT_COUNT 18
#define BOARD_EXT_FIRST 14
static const uint16_t boardExtCodepoints[BOARD_EXT_COUNT] = {
    0x010C, 0x010D,   // Č č
    0x010E, 0x010F,   // Ď ď
    0x011A, 0x011B,   // Ě ě
    0x0147, 0x0148,   // Ň ň
    0x0158, 0x0159,   // Ř ř
    0x0160, 0x0161,   // Š š
    0x0164, 0x0165,   // Ť ť
    0x016E, 0x016F,   // Ů ů
    0x017D, 0x017E    // Ž ž
};

// Every other Latin Extended-A letter (Polish, Hungarian, Croatian, Romanian, ...) has no glyph here;
// it is folded to its plain ASCII base letter instead, so the name stays readable (Łódź -> Lodz)
// rather than turning into garbage. Index = code point - 0x0100.
static const char boardExtAFold[129] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

// The Unicode code point behind a private code, or 0 if c isn't one.
static inline uint16_t boardExtCodepointFor(unsigned char c) {
    return (c >= BOARD_EXT_FIRST && c < BOARD_EXT_FIRST + BOARD_EXT_COUNT) ? boardExtCodepoints[c - BOARD_EXT_FIRST] : 0;
}

// Capital or lowercase? (see the ordering note above: an even code is a capital) - only meaningful for a valid private code.
static inline bool boardExtIsUpper(unsigned char c) {
    return (c & 1) == 0;
}

// Converts, in place, the UTF-8 sequences this board can show into its single-byte form:
//  - U+00C0..U+00FF (all of æøåäöü ß é ... - 2 bytes, lead 0xC3) -> the matching Latin-1 byte
//  - the 18 Czech/Slovak letters above (U+0100..U+017F, lead 0xC4/0xC5) -> private code 14..31
//  - any other Latin Extended-A letter -> its plain ASCII base letter
// Every conversion turns 2 bytes into 1, so the result is never longer than the input and no scratch
// buffer is needed; maxLen is accepted for call-site compatibility and not needed. Safe to run twice
// over the same text: the private codes are below 0x20, and a converted Latin-1 letter is only ever
// mistaken for a UTF-8 lead byte if it is followed directly by a byte in 0x80-0xBF, which letters and
// spaces never are (the previous version of this function relied on the same thing for 0xC3).
static inline void convertUtf8ToBoardText(char *input, size_t maxLen) {
    (void)maxLen;
    if (!input || !input[0]) return;
    size_t len = strlen(input);
    size_t out = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)input[i];
        if ((c == 0xC3 || c == 0xC4 || c == 0xC5) && i + 1 < len) {
            unsigned char c2 = (unsigned char)input[i + 1];
            if (c2 >= 0x80 && c2 <= 0xBF) {
                if (c == 0xC3) {
                    input[out++] = (char)(0xC0 | (c2 & 0x3F));          // U+00C0..U+00FF -> Latin-1
                } else {
                    uint16_t cp = (uint16_t)(0x0100 + ((c - 0xC4) << 6) + (c2 & 0x3F));
                    uint8_t code = 0;
                    for (uint8_t k = 0; k < BOARD_EXT_COUNT; k++) {
                        if (boardExtCodepoints[k] == cp) { code = k + BOARD_EXT_FIRST; break; }
                    }
                    input[out++] = code ? (char)code : boardExtAFold[cp - 0x0100];
                }
                i += 2;
                continue;
            }
        }
        input[out++] = input[i++];
    }
    input[out] = 0;
}
