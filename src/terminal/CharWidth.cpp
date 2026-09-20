#include "terminal/CharWidth.h"

#include <algorithm>
#include <iterator>

namespace Terminal {

namespace {

struct Range
{
    char32_t first;
    char32_t last;
};

// Code points that occupy no cell of their own (combining marks, joiners, format
// characters incl. the BOM, variation selectors, Hangul medial/final Jamo). Sorted by
// `first`.
constexpr Range kZeroWidth[] = {
    {0x0300, 0x036F},   // Combining Diacritical Marks
    {0x1160, 0x11FF},   // Hangul Jamo medial vowels + final consonants
    {0x1AB0, 0x1AFF},   // Combining Diacritical Marks Extended
    {0x1DC0, 0x1DFF},   // Combining Diacritical Marks Supplement
    {0x200B, 0x200F},   // ZWSP, ZWNJ, ZWJ, LRM, RLM
    {0x2028, 0x202E},   // line/paragraph separators, bidi embeddings
    {0x2060, 0x2064},   // word joiner, invisible operators
    {0x20D0, 0x20FF},   // Combining Diacritical Marks for Symbols
    {0xFE00, 0xFE0F},   // Variation Selectors
    {0xFE20, 0xFE2F},   // Combining Half Marks
    {0xFEFF, 0xFEFF},   // ZWNBSP / byte order mark
    {0xFFF9, 0xFFFB},   // Interlinear annotation anchors
    {0xE0100, 0xE01EF}, // Variation Selectors Supplement
};

// East Asian Wide / Fullwidth code points plus the common emoji blocks. Sorted by
// `first`. U+2600-26FF (miscellaneous symbols) is deliberately width 1: most of that
// block is narrow and boot logs use none of it.
constexpr Range kWide[] = {
    {0x1100, 0x115F},   // Hangul Jamo initial consonants
    {0x2E80, 0x303E},   // CJK Radicals .. CJK Symbols and Punctuation (excl. U+303F)
    {0x3041, 0x33FF},   // Hiragana, Katakana, Bopomofo, Hangul Compat Jamo, CJK compat
    {0x3400, 0x4DBF},   // CJK Unified Ideographs Extension A
    {0x4E00, 0x9FFF},   // CJK Unified Ideographs
    {0xA000, 0xA4CF},   // Yi Syllables and Radicals
    {0xAC00, 0xD7A3},   // Hangul Syllables
    {0xF900, 0xFAFF},   // CJK Compatibility Ideographs
    {0xFE30, 0xFE4F},   // CJK Compatibility Forms
    {0xFF00, 0xFF60},   // Fullwidth Forms
    {0xFFE0, 0xFFE6},   // Fullwidth signs
    {0x1F300, 0x1F64F}, // Misc Symbols and Pictographs, Emoticons
    {0x1F680, 0x1F6FF}, // Transport and Map Symbols
    {0x1F900, 0x1F9FF}, // Supplemental Symbols and Pictographs
    {0x20000, 0x2FFFD}, // CJK Unified Ideographs Extension B..F
    {0x30000, 0x3FFFD}, // CJK Unified Ideographs Extension G..
};

template <std::size_t N>
bool inRanges(const Range (&table)[N], char32_t cp)
{
    // Binary search for the last range whose `first` is <= cp, then test its `last`.
    const Range* begin = std::begin(table);
    const Range* end = std::end(table);
    const Range* it = std::upper_bound(begin, end, cp, [](char32_t value, const Range& r) { return value < r.first; });
    if (it == begin) {
        return false;
    }
    --it;
    return cp <= it->last;
}

} // namespace

bool isControl(char32_t codePoint)
{
    return codePoint < 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F);
}

int charWidth(char32_t codePoint)
{
    if (isControl(codePoint)) {
        return 0;
    }
    if (inRanges(kZeroWidth, codePoint)) {
        return 0;
    }
    if (inRanges(kWide, codePoint)) {
        return 2;
    }
    return 1;
}

} // namespace Terminal
