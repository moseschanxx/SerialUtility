// Unit tests for Terminal::charWidth / Terminal::isControl (src/terminal/CharWidth.h).
#include <QtTest>

#include "terminal/CharWidth.h"

using Terminal::charWidth;
using Terminal::isControl;

class Tst_charwidth : public QObject
{
    Q_OBJECT
private slots:
    void asciiIsNarrow();
    void controlsAreZero();
    void isControlRanges();
    void latinAndSymbolsAreNarrow();
    void cjkIsWide();
    void hangulIsWide();
    void fullwidthFormsAreWide();
    void emojiAreWide();
    void combiningMarksAreZero();
    void formatCharactersAreZero();
    void hangulJamoMedialFinalAreZero();
    void replacementCharacterIsNarrow();
    void miscSymbolsBlockIsNarrow();
    void boundaries_data();
    void boundaries();
};

void Tst_charwidth::asciiIsNarrow()
{
    for (char32_t cp = 0x20; cp < 0x7F; ++cp) {
        QCOMPARE(charWidth(cp), 1);
    }
}

void Tst_charwidth::controlsAreZero()
{
    for (char32_t cp = 0; cp < 0x20; ++cp) {
        QCOMPARE(charWidth(cp), 0);
    }
    QCOMPARE(charWidth(0x7F), 0);
    for (char32_t cp = 0x80; cp <= 0x9F; ++cp) {
        QCOMPARE(charWidth(cp), 0);
    }
}

void Tst_charwidth::isControlRanges()
{
    QVERIFY(isControl(0x00));
    QVERIFY(isControl(0x1B));
    QVERIFY(isControl(0x1F));
    QVERIFY(!isControl(0x20));
    QVERIFY(!isControl(U'A'));
    QVERIFY(!isControl(0x7E));
    QVERIFY(isControl(0x7F));
    QVERIFY(isControl(0x80));
    QVERIFY(isControl(0x9F));
    QVERIFY(!isControl(0xA0));
    QVERIFY(!isControl(0x4E2D));
}

void Tst_charwidth::latinAndSymbolsAreNarrow()
{
    QCOMPARE(charWidth(0xA0), 1);   // NBSP
    QCOMPARE(charWidth(0xE9), 1);   // é
    QCOMPARE(charWidth(0x3B1), 1);  // α
    QCOMPARE(charWidth(0x416), 1);  // Ж
    QCOMPARE(charWidth(0x2500), 1); // box drawing ─
    QCOMPARE(charWidth(0x2588), 1); // full block █
    QCOMPARE(charWidth(0x2192), 1); // →
    QCOMPARE(charWidth(0x20AC), 1); // €
}

void Tst_charwidth::cjkIsWide()
{
    QCOMPARE(charWidth(0x4E2D), 2); // 中
    QCOMPARE(charWidth(0x6587), 2); // 文
    QCOMPARE(charWidth(0x9FFF), 2); // last of the main CJK block
    QCOMPARE(charWidth(0x3400), 2); // Extension A
    QCOMPARE(charWidth(0x3042), 2); // Hiragana あ
    QCOMPARE(charWidth(0x30AB), 2); // Katakana カ
    QCOMPARE(charWidth(0x3001), 2); // 、
    QCOMPARE(charWidth(0x3000), 2); // ideographic space
    QCOMPARE(charWidth(0xF900), 2); // compatibility ideograph
    QCOMPARE(charWidth(0x20000), 2); // Extension B
    QCOMPARE(charWidth(0x2A6D6), 2);
    QCOMPARE(charWidth(0x30000), 2); // Extension G
}

void Tst_charwidth::hangulIsWide()
{
    QCOMPARE(charWidth(0xAC00), 2); // 가
    QCOMPARE(charWidth(0xD55C), 2); // 한
    QCOMPARE(charWidth(0xD7A3), 2);
    QCOMPARE(charWidth(0x1100), 2); // initial Jamo
    QCOMPARE(charWidth(0x115F), 2);
}

void Tst_charwidth::fullwidthFormsAreWide()
{
    QCOMPARE(charWidth(0xFF01), 2); // ！
    QCOMPARE(charWidth(0xFF21), 2); // Ａ
    QCOMPARE(charWidth(0xFF60), 2);
    QCOMPARE(charWidth(0xFFE0), 2); // ￠
    QCOMPARE(charWidth(0xFFE6), 2);
    QCOMPARE(charWidth(0xFF61), 1); // halfwidth ｡
    QCOMPARE(charWidth(0xFF9F), 1); // halfwidth katakana
    QCOMPARE(charWidth(0xFE30), 2); // CJK compatibility forms
}

void Tst_charwidth::emojiAreWide()
{
    QCOMPARE(charWidth(0x1F600), 2); // 😀
    QCOMPARE(charWidth(0x1F44D), 2); // 👍
    QCOMPARE(charWidth(0x1F680), 2); // 🚀
    QCOMPARE(charWidth(0x1F9E9), 2); // 🧩
}

void Tst_charwidth::combiningMarksAreZero()
{
    QCOMPARE(charWidth(0x0300), 0); // combining grave
    QCOMPARE(charWidth(0x0301), 0); // combining acute
    QCOMPARE(charWidth(0x036F), 0);
    QCOMPARE(charWidth(0x1AB0), 0);
    QCOMPARE(charWidth(0x1DC0), 0);
    QCOMPARE(charWidth(0x20D0), 0);
    QCOMPARE(charWidth(0x20E3), 0); // combining enclosing keycap
    QCOMPARE(charWidth(0xFE0F), 0); // variation selector 16
    QCOMPARE(charWidth(0xFE20), 0);
    QCOMPARE(charWidth(0xE0100), 0);
    QCOMPARE(charWidth(0xE01EF), 0);
}

void Tst_charwidth::formatCharactersAreZero()
{
    QCOMPARE(charWidth(0x200B), 0); // zero width space
    QCOMPARE(charWidth(0x200D), 0); // zero width joiner
    QCOMPARE(charWidth(0x200F), 0); // RLM
    QCOMPARE(charWidth(0x2028), 0); // line separator
    QCOMPARE(charWidth(0x202E), 0);
    QCOMPARE(charWidth(0x2060), 0); // word joiner
    QCOMPARE(charWidth(0x2064), 0);
    QCOMPARE(charWidth(0x2010), 1); // hyphen stays narrow
    QCOMPARE(charWidth(0x2065), 1);
}

void Tst_charwidth::hangulJamoMedialFinalAreZero()
{
    QCOMPARE(charWidth(0x1160), 0);
    QCOMPARE(charWidth(0x1175), 0);
    QCOMPARE(charWidth(0x11FF), 0);
}

void Tst_charwidth::replacementCharacterIsNarrow()
{
    QCOMPARE(charWidth(0xFFFD), 1);
    QCOMPARE(charWidth(0xFFFC), 1);
}

void Tst_charwidth::miscSymbolsBlockIsNarrow()
{
    // Documented simplification: U+2600-26FF is width 1 throughout.
    QCOMPARE(charWidth(0x2600), 1);
    QCOMPARE(charWidth(0x2615), 1);
    QCOMPARE(charWidth(0x26A1), 1);
    QCOMPARE(charWidth(0x26FF), 1);
}

void Tst_charwidth::boundaries_data()
{
    QTest::addColumn<uint>("codePoint");
    QTest::addColumn<int>("width");

    QTest::newRow("before 1100") << 0x10FFu << 1;
    QTest::newRow("1100") << 0x1100u << 2;
    QTest::newRow("115F") << 0x115Fu << 2;
    QTest::newRow("1160 (zero)") << 0x1160u << 0;
    QTest::newRow("11FF (zero)") << 0x11FFu << 0;
    QTest::newRow("1200") << 0x1200u << 1;
    QTest::newRow("2E7F") << 0x2E7Fu << 1;
    QTest::newRow("2E80") << 0x2E80u << 2;
    QTest::newRow("303E") << 0x303Eu << 2;
    QTest::newRow("303F") << 0x303Fu << 1;
    QTest::newRow("3040") << 0x3040u << 1;
    QTest::newRow("3041") << 0x3041u << 2;
    QTest::newRow("33FF") << 0x33FFu << 2;
    QTest::newRow("4DBF") << 0x4DBFu << 2;
    QTest::newRow("4DC0") << 0x4DC0u << 1;
    QTest::newRow("4DFF") << 0x4DFFu << 1;
    QTest::newRow("4E00") << 0x4E00u << 2;
    QTest::newRow("A000") << 0xA000u << 2;
    QTest::newRow("A4CF") << 0xA4CFu << 2;
    QTest::newRow("A4D0") << 0xA4D0u << 1;
    QTest::newRow("ABFF") << 0xABFFu << 1;
    QTest::newRow("D7A4") << 0xD7A4u << 1;
    QTest::newRow("F8FF") << 0xF8FFu << 1;
    QTest::newRow("FAFF") << 0xFAFFu << 2;
    QTest::newRow("FB00") << 0xFB00u << 1;
    QTest::newRow("FE4F") << 0xFE4Fu << 2;
    QTest::newRow("FE50") << 0xFE50u << 1;
    QTest::newRow("FEFF") << 0xFEFFu << 1;
    QTest::newRow("FF00") << 0xFF00u << 2;
    QTest::newRow("FFE7") << 0xFFE7u << 1;
    QTest::newRow("1F2FF") << 0x1F2FFu << 1;
    QTest::newRow("1F300") << 0x1F300u << 2;
    QTest::newRow("1F64F") << 0x1F64Fu << 2;
    QTest::newRow("1F650") << 0x1F650u << 1;
    QTest::newRow("1F67F") << 0x1F67Fu << 1;
    QTest::newRow("1F6FF") << 0x1F6FFu << 2;
    QTest::newRow("1F700") << 0x1F700u << 1;
    QTest::newRow("1F8FF") << 0x1F8FFu << 1;
    QTest::newRow("1F9FF") << 0x1F9FFu << 2;
    QTest::newRow("1FA00") << 0x1FA00u << 1;
    QTest::newRow("1FFFF") << 0x1FFFFu << 1;
    QTest::newRow("2FFFD") << 0x2FFFDu << 2;
    QTest::newRow("2FFFE") << 0x2FFFEu << 1;
    QTest::newRow("3FFFD") << 0x3FFFDu << 2;
    QTest::newRow("3FFFE") << 0x3FFFEu << 1;
    QTest::newRow("E00FF") << 0xE00FFu << 1;
    QTest::newRow("E01F0") << 0xE01F0u << 1;
    QTest::newRow("10FFFF") << 0x10FFFFu << 1;
}

void Tst_charwidth::boundaries()
{
    QFETCH(uint, codePoint);
    QFETCH(int, width);
    QCOMPARE(charWidth(static_cast<char32_t>(codePoint)), width);
}

QTEST_GUILESS_MAIN(Tst_charwidth)
#include "tst_charwidth.moc"
