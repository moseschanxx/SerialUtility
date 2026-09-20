#pragma once

#include <cstdint>

namespace Terminal {

/**
 * Number of terminal cells a code point occupies: 0 (combining marks, zero-width joiners
 * and other format characters incl. U+FEFF, variation selectors, C0/C1 controls),
 * 2 (East Asian Wide / Fullwidth: CJK ideographs, Hangul, fullwidth forms, most emoji),
 * otherwise 1.
 *
 * Implemented with Unicode 15 range tables (a compact subset is fine: the goal is correct
 * rendering of Chinese boot logs and prompts, not perfect emoji coverage).
 * Unit-tested in tests/tst_charwidth.cpp.
 */
int charWidth(char32_t codePoint);

/// True for U+0000..U+001F, U+007F..U+009F.
bool isControl(char32_t codePoint);

} // namespace Terminal
