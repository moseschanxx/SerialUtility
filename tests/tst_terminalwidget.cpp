// GUI test suite for TerminalWidget (src/terminal/TerminalWidget.h) and HexDumpView
// (src/ui/HexDumpView.h). Runs offscreen (QT_QPA_PLATFORM=offscreen) against the real widgets:
// key presses, mouse selection, clipboard, drag & drop, the screen model behind the view and the
// painting path. Contracts under test: the header doc comments, docs/TERMINAL_EMULATION.md §11 and
// docs/DESIGN.md §4.7.
#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetricsF>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include "app/AppSettings.h"
#include "core/HexUtils.h"
#include "terminal/AnsiParser.h"
#include "terminal/TerminalScreen.h"
#include "terminal/TerminalWidget.h"
#include "ui/HexDumpView.h"

namespace {

/// The offscreen platform on Windows uses a FreeType font database that only scans
/// <Qt>/lib/fonts, a directory Qt no longer ships. Without any font every glyph - spaces
/// included - is drawn by QFontEngineBox as a rectangle, which would make the pixel-level
/// rendering checks below meaningless. Point the database at the system fonts unless the
/// environment already says otherwise. Runs before QApplication exists (static initialiser).
const bool s_fontDirConfigured = [] {
    if (!qEnvironmentVariableIsSet("QT_QPA_FONTDIR")) {
        const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::FontsLocation);
        for (const QString& dir : dirs) {
            if (QDir(dir).exists()) {
                qputenv("QT_QPA_FONTDIR", QDir::toNativeSeparators(dir).toLocal8Bit());
                break;
            }
        }
    }
    return true;
}();

/// ESC followed by `tail` ("\x1b" followed by a hex digit would be mis-parsed in a C literal).
QByteArray esc(const char* tail)
{
    return QByteArray("\x1b") + tail;
}

/// Every payload emitted through sendData(), concatenated in order.
QByteArray sentBytes(const QSignalSpy& spy)
{
    QByteArray all;
    for (const QList<QVariant>& args : spy) {
        all += args.at(0).toByteArray();
    }
    return all;
}

/// "line 0\r\nline 1\r\n..." for `count` lines.
QByteArray numberedLines(int count, const char* prefix = "line ")
{
    QByteArray out;
    for (int i = 0; i < count; ++i) {
        out += prefix;
        out += QByteArray::number(i);
        out += "\r\n";
    }
    return out;
}

/// True when every pixel of `image` has the colour `color`.
bool isUniform(const QImage& image, const QColor& color)
{
    const QRgb rgb = color.rgb();
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if ((image.pixel(x, y) | 0xFF000000u) != (rgb | 0xFF000000u)) {
                return false;
            }
        }
    }
    return true;
}

/// Foreground colour of the first text fragment of a QPlainTextEdit block.
QColor firstFragmentColor(const QTextBlock& block)
{
    QTextBlock::iterator it = block.begin();
    if (it.atEnd()) {
        return QColor();
    }
    return it.fragment().charFormat().foreground().color();
}

/// Counts the paint events delivered to the widget it is installed on.
class PaintCounter : public QObject
{
public:
    int paints = 0;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Paint) {
            ++paints;
        }
        return QObject::eventFilter(watched, event);
    }
};

/// show() + exposure + activation, so that key events and focus behave like in the app.
bool showAndActivate(QWidget* widget)
{
    widget->show();
    if (!QTest::qWaitForWindowExposed(widget)) {
        return false;
    }
    widget->activateWindow();
    return QTest::qWaitForWindowActive(widget);
}

/// A window-wide QAction on `window` with the shortcut `key`+`mods`, the way the main window's
/// menu actions compete with a focused terminal for key presses (via QEvent::ShortcutOverride).
QAction* addWindowAction(QWidget* window, Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    auto* action = new QAction(window);
    action->setShortcut(QKeySequence(QKeyCombination(mods, key)));
    action->setShortcutContext(Qt::WindowShortcut);
    window->addAction(action);
    return action;
}

/// "Ctrl+Shift+H" etc. for failure messages.
QString keyName(Qt::Key key, Qt::KeyboardModifiers mods)
{
    return QKeySequence(QKeyCombination(mods, key)).toString(QKeySequence::PortableText);
}

} // namespace

class Tst_terminalwidget : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // ---- Key mapping ----------------------------------------------------------------
    void enterSendsCr();
    void enterSendsLf();
    void enterSendsCrlf();
    void enterModeNoneFallsBackToCr();
    void shiftEnterSendsLf();
    void backspaceSendsDelete();
    void backspaceSendsBackspace();
    void tabSendsHtAndKeepsFocus();
    void tabKeepsFocusWhenInputDisabled();
    void escapeSendsEsc();
    void editingKeys();
    void arrowsNormalMode();
    void arrowsApplicationMode();
    void functionKeys();
    void ctrlLetters();
    void ctrlPunctuation();
    void ctrlCWithoutSelectionSendsEtx();
    void altKeySendsEscPrefix();
    void plainText();
    void unicodeUtf8();
    void unicodeGb18030();
    void unsupportedEncodingFallsBackToUtf8();
    void imeCommitIsEncoded();
    void inputDisabledSwallowsKeys();
    void inputDisabledSuppressesDsrReplies();
    void shiftPageKeysScrollInsteadOfSending();
    void shortcutOverrideClaimsTerminalKeysWhileConnected();
    void shortcutOverrideWhileDisconnected();
    void ctrlShiftLetterWithoutActionSendsControlByte();

    // ---- Selection / clipboard --------------------------------------------------------
    void cellMetricsMatchGrid();
    void dragSelectionAndCtrlShiftCCopies();
    void dragSelectionAcrossLines();
    void selectAllCopiesText();
    void ctrlCWithSelectionCopiesInsteadOfSending();
    void pasteConvertsNewlinesToEnterBytes();
    void pasteFollowsEnterMode();
    void bracketedPasteWrapsPayload();
    void shiftInsertPastes();
    void ctrlInsertCopies();
    void doubleClickSelectsWord();
    void tripleClickSelectsLine();
    void shiftClickExtendsSelection();
    void middleClickPastes();
    void selectionSurvivesScrollbackGrowth();
    void dragAutoScrollRepeatsOnTimer();
    void dragAutoScrollStopsInsideViewport();
    void dropTextPastes();
    void dropFileEmitsFileDropped();
    void pasteIgnoredWhenInputDisabled();
    void clearSelectionEmitsSignal();

    // ---- Model / rendering ------------------------------------------------------------
    void colouredBootLog();
    void burstFillsScrollback();
    void scrollingUpFreezesView();
    void fullScrollbackKeepsFrozenViewAndSelection();
    void keyPressReturnsToBottom();
    void wheelScrollsAndCtrlWheelZooms();
    void zoomBoundsAndSignal();
    void gridSizeChangedOnResize();
    void clearScreenKeepsScrollback();
    void clearScrollbackResetsScrollBar();
    void resetTerminalClearsEverything();
    void localEchoShowsTypedText();
    void dsrReply();
    void daReply();
    void titleChangedOnOsc();
    void bellRangAndFlash();
    void bellBurstIsThrottled();
    void findNextWrapsAndSelects();
    void findPreviousAndCaseSensitivity();
    void paintingProducesNonUniformImage();
    void paintWideCharsAndAllAttributes();
    void colorPaletteAppliesToPainting();
    void cursorBlockWhenConnectedHollowWhenNot();
    void selectionIsPainted();
    void feedPerformance();
    void repaintsAreCoalescedWhileFollowing();
    void partialRepaintConsumesDirtyState();
    void scrollbackMaxTrims();
    void cursorPositionChangedSignal();

    // ---- HexDumpView --------------------------------------------------------------------
    void hexRxTxMarkers();
    void hexColoursDifferForRxTx();
    void hexMaxLinesTrimming();
    void hexClearAll();
    void hexBytesPerLine();
    void hexTimestampsToggle();
    void hexEmptyChunkIgnored();
    void hexTrimKeepsScrolledUpContent();

private:
    QByteArray bytesFor(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    QPoint cellCenter(int row, int col) const;
    void dragSelect(int row0, int col0, int row1, int col1);
    void sendWheel(int angleDeltaY, Qt::KeyboardModifiers modifiers);

    QTemporaryDir m_tempDir;
    TerminalWidget* m_term = nullptr;
    int m_cellW = 1;
    int m_cellH = 1;
};

// -------------------------------------------------------------------------------------------
// Fixture
// -------------------------------------------------------------------------------------------

void Tst_terminalwidget::initTestCase()
{
    QVERIFY(s_fontDirConfigured);
    QStandardPaths::setTestModeEnabled(true);
    // AppSettings uses the default QSettings() constructor, which picks these names up. INI
    // format keeps the test settings in the test-mode config directory instead of the registry.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-terminalwidget"));
    QSettings settings;
    QVERIFY(settings.fileName().contains(QStringLiteral("SerialUtilityTest-terminalwidget")));
    settings.clear();
    settings.sync();
    QVERIFY(m_tempDir.isValid());
    AppSettings::instance().setLogDirectory(m_tempDir.path());
    QCOMPARE(AppSettings::instance().logDirectory(), m_tempDir.path());
}

void Tst_terminalwidget::init()
{
    m_term = new TerminalWidget;
    m_term->setBellEnabled(false); // QApplication::beep() is pointless offscreen
    m_term->setCursorBlink(false); // deterministic painting
    m_term->resize(800, 480);
    QVERIFY(showAndActivate(m_term));
    m_term->setFocus();
    QTRY_VERIFY(m_term->hasFocus());
    m_term->setInputEnabled(true);

    // Cell metrics exactly as the widget computes them (see TerminalWidget::updateCellMetrics).
    const QFontMetricsF fm(m_term->terminalFont(), m_term->viewport());
    m_cellW = qMax(1, qCeil(fm.horizontalAdvance(QLatin1Char('M'))));
    m_cellH = qMax(1, qCeil(fm.lineSpacing()));
}

void Tst_terminalwidget::cleanup()
{
    delete m_term;
    m_term = nullptr;
    QApplication::clipboard()->clear();
}

QByteArray Tst_terminalwidget::bytesFor(Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::keyClick(m_term, key, modifiers);
    return sentBytes(spy);
}

QPoint Tst_terminalwidget::cellCenter(int row, int col) const
{
    return QPoint(col * m_cellW + m_cellW / 2, row * m_cellH + m_cellH / 2);
}

void Tst_terminalwidget::dragSelect(int row0, int col0, int row1, int col1)
{
    QWidget* vp = m_term->viewport();
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(row0, col0));
    QTest::mouseMove(vp, cellCenter(row1, col1));
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(row1, col1));
}

void Tst_terminalwidget::sendWheel(int angleDeltaY, Qt::KeyboardModifiers modifiers)
{
    QWidget* vp = m_term->viewport();
    const QPointF pos(vp->width() / 2.0, vp->height() / 2.0);
    QWheelEvent event(pos, vp->mapToGlobal(pos), QPoint(), QPoint(0, angleDeltaY), Qt::NoButton, modifiers,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(vp, &event);
}

// -------------------------------------------------------------------------------------------
// Key mapping
// -------------------------------------------------------------------------------------------

void Tst_terminalwidget::enterSendsCr()
{
    QVERIFY(m_term->enterSends() == LineEnding::Mode::CR); // default
    QCOMPARE(bytesFor(Qt::Key_Return), QByteArray("\r"));
    QCOMPARE(bytesFor(Qt::Key_Enter), QByteArray("\r"));
}

void Tst_terminalwidget::enterSendsLf()
{
    m_term->setEnterSends(LineEnding::Mode::LF);
    QVERIFY(m_term->enterSends() == LineEnding::Mode::LF);
    QCOMPARE(bytesFor(Qt::Key_Return), QByteArray("\n"));
}

void Tst_terminalwidget::enterSendsCrlf()
{
    m_term->setEnterSends(LineEnding::Mode::CRLF);
    QCOMPARE(bytesFor(Qt::Key_Return), QByteArray("\r\n"));
    QCOMPARE(bytesFor(Qt::Key_Enter), QByteArray("\r\n"));
}

void Tst_terminalwidget::enterModeNoneFallsBackToCr()
{
    // LineEnding::Mode::None means "append nothing" for the line-mode CommandInput. For the
    // terminal's Enter key an empty payload would make Enter a no-op, so the widget falls back
    // to CR (documented in TerminalWidget.h).
    m_term->setEnterSends(LineEnding::Mode::None);
    QVERIFY(LineEnding::bytes(LineEnding::Mode::None).isEmpty());
    QCOMPARE(bytesFor(Qt::Key_Return), QByteArray("\r"));
}

void Tst_terminalwidget::shiftEnterSendsLf()
{
    m_term->setEnterSends(LineEnding::Mode::CRLF);
    QCOMPARE(bytesFor(Qt::Key_Return, Qt::ShiftModifier), QByteArray("\n"));
    m_term->setEnterSends(LineEnding::Mode::CR);
    QCOMPARE(bytesFor(Qt::Key_Enter, Qt::ShiftModifier), QByteArray("\n"));
}

void Tst_terminalwidget::backspaceSendsDelete()
{
    QVERIFY(m_term->backspaceSendsDelete()); // default
    QCOMPARE(bytesFor(Qt::Key_Backspace), QByteArray(1, '\x7f'));
    QCOMPARE(bytesFor(Qt::Key_Backspace, Qt::ShiftModifier), QByteArray(1, '\x08'));
}

void Tst_terminalwidget::backspaceSendsBackspace()
{
    m_term->setBackspaceSendsDelete(false);
    QVERIFY(!m_term->backspaceSendsDelete());
    QCOMPARE(bytesFor(Qt::Key_Backspace), QByteArray(1, '\x08'));
    QCOMPARE(bytesFor(Qt::Key_Backspace, Qt::ShiftModifier), QByteArray(1, '\x7f'));
}

void Tst_terminalwidget::tabSendsHtAndKeepsFocus()
{
    // A sibling widget that would take focus if Tab were treated as focus navigation.
    QWidget container;
    auto* layout = new QVBoxLayout(&container);
    auto* term = new TerminalWidget;
    auto* edit = new QLineEdit;
    layout->addWidget(term);
    layout->addWidget(edit);
    container.resize(640, 400);
    QVERIFY(showAndActivate(&container));
    term->setFocus();
    QTRY_VERIFY(term->hasFocus());
    term->setInputEnabled(true);

    QSignalSpy spy(term, &TerminalWidget::sendData);
    QTest::keyClick(term, Qt::Key_Tab);
    QCOMPARE(sentBytes(spy), QByteArray("\t"));
    QCOMPARE(QApplication::focusWidget(), term);
    QVERIFY(term->hasFocus());
    QVERIFY(!edit->hasFocus());

    spy.clear();
    QTest::keyClick(term, Qt::Key_Tab); // a second Tab in a row: still no focus change
    QCOMPARE(sentBytes(spy), QByteArray("\t"));
    QCOMPARE(QApplication::focusWidget(), term);

    spy.clear();
    QTest::keyClick(term, Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(sentBytes(spy), esc("[Z"));
    QCOMPARE(QApplication::focusWidget(), term);
    QVERIFY(!edit->hasFocus());
}

void Tst_terminalwidget::tabKeepsFocusWhenInputDisabled()
{
    // DESIGN.md 4.7: Tab never moves focus away from the terminal, connected or not.
    QWidget container;
    auto* layout = new QVBoxLayout(&container);
    auto* term = new TerminalWidget;
    auto* edit = new QLineEdit;
    layout->addWidget(term);
    layout->addWidget(edit);
    container.resize(640, 400);
    QVERIFY(showAndActivate(&container));
    term->setFocus();
    QTRY_VERIFY(term->hasFocus());
    QVERIFY(!term->inputEnabled());

    // Focus is checked after every single key: Tab followed by Shift+Tab would move the focus
    // away and back again and hide the defect.
    QSignalSpy spy(term, &TerminalWidget::sendData);
    QTest::keyClick(term, Qt::Key_Tab);
    QCOMPARE(QApplication::focusWidget(), term);
    QVERIFY(!edit->hasFocus());
    QTest::keyClick(term, Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(QApplication::focusWidget(), term);
    QTest::keyClick(term, Qt::Key_Tab);
    QCOMPARE(QApplication::focusWidget(), term);
    QCOMPARE(spy.count(), qsizetype(0));
    QVERIFY(term->hasFocus());
    QVERIFY(!edit->hasFocus());

    // ... and the same Tab reaches the device once connected.
    term->setInputEnabled(true);
    QTest::keyClick(term, Qt::Key_Tab);
    QCOMPARE(sentBytes(spy), QByteArray("\t"));
    QCOMPARE(QApplication::focusWidget(), term);
}

void Tst_terminalwidget::escapeSendsEsc()
{
    QCOMPARE(bytesFor(Qt::Key_Escape), QByteArray("\x1b"));
}

void Tst_terminalwidget::editingKeys()
{
    QCOMPARE(bytesFor(Qt::Key_Delete), esc("[3~"));
    QCOMPARE(bytesFor(Qt::Key_Insert), esc("[2~"));
    QCOMPARE(bytesFor(Qt::Key_Home), esc("[H"));
    QCOMPARE(bytesFor(Qt::Key_End), esc("[F"));
    QCOMPARE(bytesFor(Qt::Key_PageUp), esc("[5~"));
    QCOMPARE(bytesFor(Qt::Key_PageDown), esc("[6~"));
}

void Tst_terminalwidget::arrowsNormalMode()
{
    QVERIFY(!m_term->parser()->cursorKeyApplicationMode());
    QCOMPARE(bytesFor(Qt::Key_Up), esc("[A"));
    QCOMPARE(bytesFor(Qt::Key_Down), esc("[B"));
    QCOMPARE(bytesFor(Qt::Key_Right), esc("[C"));
    QCOMPARE(bytesFor(Qt::Key_Left), esc("[D"));
}

void Tst_terminalwidget::arrowsApplicationMode()
{
    m_term->feedData(esc("[?1h")); // DECCKM set
    QVERIFY(m_term->parser()->cursorKeyApplicationMode());
    QCOMPARE(bytesFor(Qt::Key_Up), esc("OA"));
    QCOMPARE(bytesFor(Qt::Key_Down), esc("OB"));
    QCOMPARE(bytesFor(Qt::Key_Right), esc("OC"));
    QCOMPARE(bytesFor(Qt::Key_Left), esc("OD"));

    m_term->feedData(esc("[?1l")); // DECCKM reset
    QVERIFY(!m_term->parser()->cursorKeyApplicationMode());
    QCOMPARE(bytesFor(Qt::Key_Up), esc("[A"));
}

void Tst_terminalwidget::functionKeys()
{
    struct Mapping
    {
        Qt::Key key;
        const char* tail;
    };
    const Mapping table[] = {{Qt::Key_F1, "OP"},   {Qt::Key_F2, "OQ"},    {Qt::Key_F3, "OR"},    {Qt::Key_F4, "OS"},
                             {Qt::Key_F5, "[15~"}, {Qt::Key_F6, "[17~"},  {Qt::Key_F7, "[18~"},  {Qt::Key_F8, "[19~"},
                             {Qt::Key_F9, "[20~"}, {Qt::Key_F10, "[21~"}, {Qt::Key_F11, "[23~"}, {Qt::Key_F12, "[24~"}};
    for (const Mapping& m : table) {
        const QByteArray actual = bytesFor(m.key);
        const QByteArray expected = esc(m.tail);
        QVERIFY2(actual == expected, qPrintable(QStringLiteral("key 0x%1: got %2, expected %3")
                                                    .arg(int(m.key), 0, 16)
                                                    .arg(QString::fromLatin1(actual.toHex(' ')))
                                                    .arg(QString::fromLatin1(expected.toHex(' ')))));
    }
}

void Tst_terminalwidget::ctrlLetters()
{
    for (int i = 0; i < 26; ++i) {
        const Qt::Key key = static_cast<Qt::Key>(Qt::Key_A + i);
        const QByteArray actual = bytesFor(key, Qt::ControlModifier);
        const QByteArray expected(1, static_cast<char>(i + 1));
        QVERIFY2(actual == expected, qPrintable(QStringLiteral("Ctrl+%1: got %2")
                                                    .arg(QChar(static_cast<char16_t>(u'A' + i)))
                                                    .arg(QString::fromLatin1(actual.toHex(' ')))));
    }
}

void Tst_terminalwidget::ctrlPunctuation()
{
    QCOMPARE(bytesFor(Qt::Key_BracketLeft, Qt::ControlModifier), QByteArray(1, '\x1b'));
    QCOMPARE(bytesFor(Qt::Key_Backslash, Qt::ControlModifier), QByteArray(1, '\x1c'));
    QCOMPARE(bytesFor(Qt::Key_BracketRight, Qt::ControlModifier), QByteArray(1, '\x1d'));
    QCOMPARE(bytesFor(Qt::Key_Space, Qt::ControlModifier), QByteArray(1, '\0'));
}

void Tst_terminalwidget::ctrlCWithoutSelectionSendsEtx()
{
    QVERIFY(!m_term->hasSelection());
    QCOMPARE(bytesFor(Qt::Key_C, Qt::ControlModifier), QByteArray(1, '\x03'));
    QVERIFY(QApplication::clipboard()->text().isEmpty());
}

void Tst_terminalwidget::altKeySendsEscPrefix()
{
    QCOMPARE(bytesFor(Qt::Key_X, Qt::AltModifier), esc("x"));
    QCOMPARE(bytesFor(Qt::Key_Return, Qt::AltModifier), esc("\r"));
    QCOMPARE(bytesFor(Qt::Key_Backspace, Qt::AltModifier), esc("\x7f"));
    QCOMPARE(bytesFor(Qt::Key_Escape, Qt::AltModifier), QByteArray("\x1b\x1b"));
}

void Tst_terminalwidget::plainText()
{
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::keyClicks(m_term, QStringLiteral("ls -la"));
    QCOMPARE(spy.count(), qsizetype(6)); // one write per key press
    QCOMPARE(sentBytes(spy), QByteArray("ls -la"));
    // Typed text is not shown locally unless local echo is on.
    QVERIFY(m_term->screen()->lineText(0).isEmpty());
}

void Tst_terminalwidget::unicodeUtf8()
{
    QCOMPARE(m_term->encoding(), QStringLiteral("UTF-8"));
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::sendKeyEvent(QTest::Click, m_term, Qt::Key_unknown, QStringLiteral("你"), Qt::NoModifier); // 你
    QTest::sendKeyEvent(QTest::Click, m_term, Qt::Key_unknown, QStringLiteral("好"), Qt::NoModifier); // 好
    QCOMPARE(sentBytes(spy), QByteArray::fromHex("e4bda0e5a5bd"));
}

void Tst_terminalwidget::unicodeGb18030()
{
    QStringEncoder probe("GB18030");
    if (!probe.isValid()) {
        QSKIP("QStringEncoder has no GB18030 codec in this Qt build (needs ICU)");
    }
    QVERIFY(m_term->setEncoding(QStringLiteral("GB18030")));
    QCOMPARE(m_term->encoding(), QStringLiteral("GB18030"));
    QCOMPARE(m_term->parser()->encoding(), QStringLiteral("GB18030"));
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::sendKeyEvent(QTest::Click, m_term, Qt::Key_unknown, QStringLiteral("你好"), Qt::NoModifier);
    QCOMPARE(sentBytes(spy), QByteArray::fromHex("c4e3bac3"));
}

void Tst_terminalwidget::unsupportedEncodingFallsBackToUtf8()
{
    QVERIFY(!m_term->setEncoding(QStringLiteral("no-such-codec-42")));
    QCOMPARE(m_term->encoding(), QStringLiteral("UTF-8"));
    QCOMPARE(m_term->parser()->encoding(), QStringLiteral("UTF-8"));
    QVERIFY(m_term->setEncoding(QStringLiteral("ISO-8859-1")));
    QCOMPARE(m_term->encoding(), QStringLiteral("ISO-8859-1"));
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::sendKeyEvent(QTest::Click, m_term, Qt::Key_unknown, QStringLiteral("é"), Qt::NoModifier); // é
    QCOMPARE(sentBytes(spy), QByteArray::fromHex("e9"));
}

void Tst_terminalwidget::imeCommitIsEncoded()
{
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QInputMethodEvent event;
    event.setCommitString(QStringLiteral("好"));
    QApplication::sendEvent(m_term, &event);
    QCOMPARE(sentBytes(spy), QByteArray::fromHex("e5a5bd"));

    // Preedit-only events (composition in progress) send nothing.
    QInputMethodEvent preedit(QStringLiteral("ni"), {});
    QApplication::sendEvent(m_term, &preedit);
    QCOMPARE(spy.count(), qsizetype(1));

    // The IME is enabled and positioned at the cursor cell (queried the way Qt's input context does).
    QVERIFY(m_term->testAttribute(Qt::WA_InputMethodEnabled));
    QInputMethodQueryEvent query(Qt::ImEnabled | Qt::ImCursorRectangle | Qt::ImFont);
    QApplication::sendEvent(m_term, &query);
    QVERIFY(query.value(Qt::ImEnabled).toBool());
    QCOMPARE(query.value(Qt::ImCursorRectangle).toRect().size(), QSize(m_cellW, m_cellH));
    QCOMPARE(query.value(Qt::ImFont).value<QFont>().pointSize(), m_term->terminalFont().pointSize());
}

void Tst_terminalwidget::inputDisabledSwallowsKeys()
{
    m_term->feedData("some text\r\n");
    m_term->setInputEnabled(false);
    QVERIFY(!m_term->inputEnabled());

    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::keyClicks(m_term, QStringLiteral("abc"));
    QTest::keyClick(m_term, Qt::Key_Return);
    QTest::keyClick(m_term, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(m_term, Qt::Key_F1);
    QTest::keyClick(m_term, Qt::Key_Up);
    QTest::keyClick(m_term, Qt::Key_Tab);
    QTest::keyClick(m_term, Qt::Key_Escape);
    QInputMethodEvent ime;
    ime.setCommitString(QStringLiteral("x"));
    QApplication::sendEvent(m_term, &ime);
    QCOMPARE(spy.count(), qsizetype(0));

    // Copy still works while disconnected (navigation/copy shortcuts are exempt).
    m_term->selectAll();
    QTest::keyClick(m_term, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(QApplication::clipboard()->text().startsWith(QStringLiteral("some text")));
    QCOMPARE(spy.count(), qsizetype(0));

    // Re-enabling restores normal operation.
    m_term->setInputEnabled(true);
    QTest::keyClick(m_term, Qt::Key_A);
    QCOMPARE(sentBytes(spy), QByteArray("a"));
}

void Tst_terminalwidget::inputDisabledSuppressesDsrReplies()
{
    m_term->setInputEnabled(false);
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    m_term->feedData(esc("[6n"));
    m_term->feedData(esc("[c"));
    m_term->feedData(esc("[5n"));
    QCOMPARE(spy.count(), qsizetype(0));

    m_term->setInputEnabled(true);
    m_term->feedData(esc("[6n"));
    QCOMPARE(spy.count(), qsizetype(1));
}

void Tst_terminalwidget::shiftPageKeysScrollInsteadOfSending()
{
    m_term->feedData(numberedLines(200));
    QScrollBar* bar = m_term->verticalScrollBar();
    const int max = bar->maximum();
    QVERIFY(max > 2 * m_term->visibleRows());
    QCOMPARE(bar->value(), max);

    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::keyClick(m_term, Qt::Key_PageUp, Qt::ShiftModifier);
    QCOMPARE(spy.count(), qsizetype(0));
    QCOMPARE(bar->value(), max - m_term->visibleRows());
    QVERIFY(!m_term->isAtBottom());

    QTest::keyClick(m_term, Qt::Key_PageDown, Qt::ShiftModifier);
    QCOMPARE(spy.count(), qsizetype(0));
    QCOMPARE(bar->value(), max);
    QVERIFY(m_term->isAtBottom());

    // Plain PgUp/PgDn go to the device.
    QCOMPARE(bytesFor(Qt::Key_PageUp), esc("[5~"));
}

void Tst_terminalwidget::shortcutOverrideClaimsTerminalKeysWhileConnected()
{
    // Header "Input mapping": while the terminal is connected and focused, the keys it maps to
    // bytes (Ctrl+<letter>, Tab, F1, Esc, ...) are claimed in event(QEvent::ShortcutOverride) so
    // an ancestor's QAction with the same shortcut cannot steal them; the main window's
    // shortcuts (Ctrl+Shift+<letter>, Ctrl+T, Ctrl+W, Ctrl+Tab, Ctrl+, F2/F3/F5) pass through.
    QWidget container;
    auto* layout = new QVBoxLayout(&container);
    auto* term = new TerminalWidget;
    layout->addWidget(term);
    container.resize(640, 400);
    QVERIFY(showAndActivate(&container));
    term->setFocus();
    QTRY_VERIFY(term->hasFocus());
    term->setInputEnabled(true);
    QVERIFY(!term->hasSelection());

    struct Probe
    {
        Qt::Key key;
        Qt::KeyboardModifiers mods;
        QByteArray expected;   ///< bytes the device receives (claimed keys only)
    };
    const Probe claimed[] = {{Qt::Key_L, Qt::ControlModifier, QByteArray(1, '\x0c')},
                             {Qt::Key_C, Qt::ControlModifier, QByteArray(1, '\x03')},   // no selection: ETX
                             {Qt::Key_Tab, Qt::NoModifier, QByteArray("\t")},
                             {Qt::Key_F1, Qt::NoModifier, esc("OP")},
                             {Qt::Key_Escape, Qt::NoModifier, QByteArray("\x1b")}};
    // F2 is listed in functionKeys() as ESC O Q: that mapping only applies when no action owns it.
    const Probe passThrough[] = {{Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier, {}},
                                 {Qt::Key_T, Qt::ControlModifier, {}},
                                 {Qt::Key_W, Qt::ControlModifier, {}},
                                 {Qt::Key_Comma, Qt::ControlModifier, {}},
                                 {Qt::Key_Tab, Qt::ControlModifier, {}},
                                 {Qt::Key_F2, Qt::NoModifier, {}},
                                 {Qt::Key_F3, Qt::NoModifier, {}},
                                 {Qt::Key_F5, Qt::NoModifier, {}}};

    for (const Probe& p : claimed) {
        QAction* action = addWindowAction(&container, p.key, p.mods);
        QSignalSpy triggered(action, &QAction::triggered);
        QSignalSpy send(term, &TerminalWidget::sendData);
        QTest::keyClick(term, p.key, p.mods);
        QVERIFY2(triggered.count() == 0, qPrintable(keyName(p.key, p.mods) + QStringLiteral(" reached the action")));
        QVERIFY2(sentBytes(send) == p.expected, qPrintable(keyName(p.key, p.mods) + QStringLiteral(": got ") +
                                                           QString::fromLatin1(sentBytes(send).toHex(' '))));
        QCOMPARE(QApplication::focusWidget(), term);   // in particular after Tab
    }

    for (const Probe& p : passThrough) {
        QAction* action = addWindowAction(&container, p.key, p.mods);
        QSignalSpy triggered(action, &QAction::triggered);
        QSignalSpy send(term, &TerminalWidget::sendData);
        QTest::keyClick(term, p.key, p.mods);
        QVERIFY2(triggered.count() == 1,
                 qPrintable(keyName(p.key, p.mods) + QStringLiteral(" did not reach the action")));
        QVERIFY2(send.count() == 0, qPrintable(keyName(p.key, p.mods) + QStringLiteral(" was sent to the device")));
    }
}

void Tst_terminalwidget::shortcutOverrideWhileDisconnected()
{
    // Header setInputEnabled(): disconnected, key presses are swallowed except the copy /
    // navigation shortcuts. event() therefore only claims the widget-local actions - and only
    // when they apply: an ancestor's Ctrl+L fires (nothing to send), Ctrl+Insert / Ctrl+C with
    // a selection copy locally instead of triggering the ancestor, Ctrl+C without a selection
    // goes to the ancestor, and Ctrl+Shift+C passes through like every Ctrl+Shift+<letter>.
    QWidget container;
    auto* layout = new QVBoxLayout(&container);
    auto* term = new TerminalWidget;
    layout->addWidget(term);
    container.resize(640, 400);
    QVERIFY(showAndActivate(&container));
    term->setFocus();
    QTRY_VERIFY(term->hasFocus());
    QVERIFY(!term->inputEnabled());
    term->feedData("some text\r\n");
    QSignalSpy send(term, &TerminalWidget::sendData);

    QAction* ctrlL = addWindowAction(&container, Qt::Key_L, Qt::ControlModifier);
    QSignalSpy ctrlLSpy(ctrlL, &QAction::triggered);
    QTest::keyClick(term, Qt::Key_L, Qt::ControlModifier);
    QCOMPARE(ctrlLSpy.count(), qsizetype(1));

    // Tab is swallowed (no action owns it): no bytes, the focus stays on the terminal.
    QTest::keyClick(term, Qt::Key_Tab);
    QCOMPARE(QApplication::focusWidget(), term);
    QVERIFY(term->hasFocus());

    QAction* ctrlInsert = addWindowAction(&container, Qt::Key_Insert, Qt::ControlModifier);
    QSignalSpy ctrlInsertSpy(ctrlInsert, &QAction::triggered);
    term->selectAll();
    QVERIFY(term->hasSelection());
    QTest::keyClick(term, Qt::Key_Insert, Qt::ControlModifier);
    QCOMPARE(ctrlInsertSpy.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("some text"));
    QVERIFY(term->hasSelection());   // Ctrl+Insert keeps the selection

    QAction* ctrlC = addWindowAction(&container, Qt::Key_C, Qt::ControlModifier);
    QSignalSpy ctrlCSpy(ctrlC, &QAction::triggered);
    QApplication::clipboard()->clear();
    QTest::keyClick(term, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(ctrlCSpy.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("some text"));
    QVERIFY(!term->hasSelection());   // Ctrl+C clears the selection ...
    QTest::keyClick(term, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(ctrlCSpy.count(), qsizetype(1));   // ... so the next Ctrl+C is the ancestor's

    // Ctrl+Shift+C belongs to the main window's Copy action whenever one exists (pass-through);
    // the terminal itself copies on Ctrl+Shift+C only when no action claims it (see
    // inputDisabledSwallowsKeys()).
    QAction* ctrlShiftC = addWindowAction(&container, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QSignalSpy ctrlShiftCSpy(ctrlShiftC, &QAction::triggered);
    QApplication::clipboard()->clear();
    term->selectAll();
    QTest::keyClick(term, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(ctrlShiftCSpy.count(), qsizetype(1));
    QVERIFY(QApplication::clipboard()->text().isEmpty());

    QCOMPARE(send.count(), qsizetype(0));
    QCOMPARE(QApplication::focusWidget(), term);
}

void Tst_terminalwidget::ctrlShiftLetterWithoutActionSendsControlByte()
{
    // Header: a Ctrl+Shift+<letter> that no action uses comes back from the shortcut map and is
    // sent as the Ctrl+<letter> control byte. The fixture has no QActions at all.
    QCOMPARE(bytesFor(Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier), QByteArray(1, '\x01'));
    QCOMPARE(bytesFor(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier), QByteArray(1, '\x1a'));
}

// -------------------------------------------------------------------------------------------
// Selection / clipboard
// -------------------------------------------------------------------------------------------

void Tst_terminalwidget::cellMetricsMatchGrid()
{
    // The grid the widget derives from its viewport must match the metrics this suite uses
    // to compute mouse coordinates.
    const QSize vp = m_term->viewport()->size();
    QCOMPARE(m_term->columns(), qMax(2, vp.width() / m_cellW));
    QCOMPARE(m_term->visibleRows(), qMax(2, vp.height() / m_cellH));
    QCOMPARE(m_term->screen()->cols(), m_term->columns());
    QCOMPARE(m_term->screen()->rows(), m_term->visibleRows());
    QVERIFY(m_term->columns() >= 40);
    QVERIFY(m_term->visibleRows() >= 10);
}

void Tst_terminalwidget::dragSelectionAndCtrlShiftCCopies()
{
    m_term->feedData("hello world\r\n");
    QSignalSpy selectionSpy(m_term, &TerminalWidget::selectionChanged);
    QVERIFY(!m_term->hasSelection());

    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->hasSelection());
    QCOMPARE(m_term->selectedText(), QStringLiteral("hello"));
    QVERIFY(selectionSpy.count() >= 1);

    QSignalSpy sendSpy(m_term, &TerminalWidget::sendData);
    QTest::keyClick(m_term, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello"));
    QCOMPARE(sendSpy.count(), qsizetype(0));
    QVERIFY(m_term->hasSelection()); // Ctrl+Shift+C keeps the selection
}

void Tst_terminalwidget::dragSelectionAcrossLines()
{
    m_term->feedData("hello world\r\nsecond line\r\n");
    dragSelect(0, 6, 1, 5);
    QCOMPARE(m_term->selectedText(), QStringLiteral("world\nsecond"));

    // Backwards drag covers the same cells.
    dragSelect(1, 5, 0, 6);
    QCOMPARE(m_term->selectedText(), QStringLiteral("world\nsecond"));

    // Trailing blanks are trimmed; a click without a drag leaves no selection.
    QTest::mouseClick(m_term->viewport(), Qt::LeftButton, Qt::NoModifier, cellCenter(3, 3));
    QVERIFY(!m_term->hasSelection());
    QCOMPARE(m_term->selectedText(), QString());
}

void Tst_terminalwidget::selectAllCopiesText()
{
    m_term->feedData("alpha\r\nbeta");
    m_term->selectAll();
    QVERIFY(m_term->hasSelection());
    QCOMPARE(m_term->selectedText(), QStringLiteral("alpha\nbeta")); // no trailing blank rows
    m_term->copySelection();
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("alpha\nbeta"));

    // A terminated last line (cursor on a blank row) still copies without trailing newlines.
    m_term->feedData("\r\n");
    m_term->selectAll();
    QCOMPARE(m_term->selectedText(), QStringLiteral("alpha\nbeta"));

    // Select All on a blank buffer selects nothing.
    m_term->resetTerminal();
    m_term->selectAll();
    QVERIFY(!m_term->hasSelection());
}

void Tst_terminalwidget::ctrlCWithSelectionCopiesInsteadOfSending()
{
    m_term->feedData("copy me\r\n");
    dragSelect(0, 0, 0, 3);
    QCOMPARE(m_term->selectedText(), QStringLiteral("copy"));

    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::keyClick(m_term, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(spy.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("copy"));
    QVERIFY(!m_term->hasSelection()); // Ctrl+C clears the selection

    // ... so the next Ctrl+C is an interrupt again.
    QTest::keyClick(m_term, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(sentBytes(spy), QByteArray(1, '\x03'));
}

void Tst_terminalwidget::pasteConvertsNewlinesToEnterBytes()
{
    QApplication::clipboard()->setText(QStringLiteral("a\nb\r\nc"));
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::keyClick(m_term, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(spy.count(), qsizetype(1)); // one write
    QCOMPARE(sentBytes(spy), QByteArray("a\rb\rc"));
}

void Tst_terminalwidget::pasteFollowsEnterMode()
{
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    m_term->setEnterSends(LineEnding::Mode::LF);
    m_term->pasteText(QStringLiteral("a\nb\r\nc"));
    QCOMPARE(sentBytes(spy), QByteArray("a\nb\nc"));

    spy.clear();
    m_term->setEnterSends(LineEnding::Mode::CRLF);
    m_term->pasteText(QStringLiteral("a\r\nb\n"));
    QCOMPARE(sentBytes(spy), QByteArray("a\r\nb\r\n"));

    spy.clear();
    m_term->pasteText(QString());
    QCOMPARE(spy.count(), qsizetype(0));
}

void Tst_terminalwidget::bracketedPasteWrapsPayload()
{
    m_term->feedData(esc("[?2004h"));
    QVERIFY(m_term->parser()->bracketedPasteMode());
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    m_term->pasteText(QStringLiteral("x\ny"));
    QCOMPARE(sentBytes(spy), esc("[200~") + "x\ry" + esc("[201~"));

    spy.clear();
    m_term->feedData(esc("[?2004l"));
    QVERIFY(!m_term->parser()->bracketedPasteMode());
    m_term->pasteText(QStringLiteral("x"));
    QCOMPARE(sentBytes(spy), QByteArray("x"));
}

void Tst_terminalwidget::shiftInsertPastes()
{
    QApplication::clipboard()->setText(QStringLiteral("ins"));
    QCOMPARE(bytesFor(Qt::Key_Insert, Qt::ShiftModifier), QByteArray("ins"));
}

void Tst_terminalwidget::ctrlInsertCopies()
{
    m_term->feedData("word here\r\n");
    dragSelect(0, 0, 0, 3);
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::keyClick(m_term, Qt::Key_Insert, Qt::ControlModifier);
    QCOMPARE(spy.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("word"));
}

void Tst_terminalwidget::doubleClickSelectsWord()
{
    m_term->feedData("foo bar-baz/qux.txt end\r\n");
    QWidget* vp = m_term->viewport();
    QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 5));
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 5));
    QCOMPARE(m_term->selectedText(), QStringLiteral("bar-baz/qux.txt"));

    // Double-clicking a blank cell (not a word character) selects just that cell.
    QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 3));
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 3));
    QVERIFY(m_term->hasSelection());
    QVERIFY(m_term->selectedText().trimmed().isEmpty());
    QVERIFY(m_term->selectedText().size() <= 1);
}

void Tst_terminalwidget::tripleClickSelectsLine()
{
    m_term->feedData("the whole line here\r\nnext line\r\n");
    QWidget* vp = m_term->viewport();
    const QPoint p = cellCenter(0, 4);
    // Qt delivers a real triple click as Press Release Press DblClick Release Press Release.
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, p);
    QCOMPARE(m_term->selectedText(), QStringLiteral("whole"));
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, p);
    QCOMPARE(m_term->selectedText(), QStringLiteral("the whole line here"));
}

void Tst_terminalwidget::shiftClickExtendsSelection()
{
    m_term->feedData("hello world\r\n");
    dragSelect(0, 0, 0, 2);
    QCOMPARE(m_term->selectedText(), QStringLiteral("hel"));
    QWidget* vp = m_term->viewport();
    QTest::mousePress(vp, Qt::LeftButton, Qt::ShiftModifier, cellCenter(0, 10));
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::ShiftModifier, cellCenter(0, 10));
    QCOMPARE(m_term->selectedText(), QStringLiteral("hello world"));
}

void Tst_terminalwidget::middleClickPastes()
{
    QApplication::clipboard()->setText(QStringLiteral("mid"));
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::mouseClick(m_term->viewport(), Qt::MiddleButton, Qt::NoModifier, cellCenter(0, 0));
    QCOMPARE(sentBytes(spy), QByteArray("mid"));
}

void Tst_terminalwidget::selectionSurvivesScrollbackGrowth()
{
    m_term->feedData("anchor text\r\n");
    dragSelect(0, 0, 0, 5);
    QCOMPARE(m_term->selectedText(), QStringLiteral("anchor"));

    m_term->feedData(numberedLines(m_term->visibleRows() + 20));
    QVERIFY(m_term->screen()->scrollbackSize() > 0);
    QCOMPARE(m_term->screen()->lineText(0), QStringLiteral("anchor text")); // now in scrollback
    QCOMPARE(m_term->selectedText(), QStringLiteral("anchor"));             // anchors are absolute
}

void Tst_terminalwidget::dragAutoScrollRepeatsOnTimer()
{
    m_term->feedData(numberedLines(200)); // several pages of scrollback
    m_term->scrollToBottom();
    QScrollBar* bar = m_term->verticalScrollBar();
    QVERIFY(bar->maximum() > 4 * m_term->visibleRows());
    QWidget* vp = m_term->viewport();
    const TerminalScreen* screen = m_term->screen();

    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(5, 0));
    QTest::mouseMove(vp, QPoint(cellCenter(0, 0).x(), -10)); // above the viewport: one line right away
    const int v0 = bar->value();
    QVERIFY(v0 < bar->maximum());
    QVERIFY(!m_term->isAtBottom());

    // Without any further mouse event the timer keeps scrolling ...
    QTRY_VERIFY_WITH_TIMEOUT(bar->value() <= v0 - 3, 1000);
    // ... and the selection follows: it starts on the first visible line.
    QVERIFY(m_term->hasSelection());
    QVERIFY2(m_term->selectedText().startsWith(screen->lineText(bar->value())), qPrintable(m_term->selectedText()));

    // Releasing the button stops the timer.
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, QPoint(cellCenter(0, 0).x(), -10));
    const int released = bar->value();
    QTest::qWait(200);
    QCOMPARE(bar->value(), released);
    QVERIFY(m_term->hasSelection());
    QVERIFY(m_term->selectedText().startsWith(screen->lineText(released)));
}

void Tst_terminalwidget::dragAutoScrollStopsInsideViewport()
{
    m_term->feedData(numberedLines(200));
    m_term->scrollToBottom();
    QScrollBar* bar = m_term->verticalScrollBar();
    QWidget* vp = m_term->viewport();

    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(5, 0));
    QTest::mouseMove(vp, QPoint(cellCenter(0, 0).x(), -10));
    const int v0 = bar->value();
    QTRY_VERIFY_WITH_TIMEOUT(bar->value() <= v0 - 2, 1000);

    // Back inside the viewport: no more scrolling, the selection ends at the pointer cell.
    QTest::mouseMove(vp, cellCenter(2, 0));
    const int inside = bar->value();
    QTest::qWait(200);
    QCOMPARE(bar->value(), inside);
    QVERIFY(m_term->selectedText().startsWith(m_term->screen()->lineText(inside + 2)));

    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(2, 0));
    QTest::qWait(100);
    QCOMPARE(bar->value(), inside);
}

void Tst_terminalwidget::dropTextPastes()
{
    // Like a real drag: the events land on the viewport (the visible surface); QApplication
    // delivers the Drop to the widget whose DragEnter was accepted.
    QWidget* vp = m_term->viewport();
    QMimeData mime;
    mime.setText(QStringLiteral("dropped\nline"));
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &enter);
    QVERIFY(enter.isAccepted());

    QSignalSpy sendSpy(m_term, &TerminalWidget::sendData);
    QSignalSpy fileSpy(m_term, &TerminalWidget::fileDropped);
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(sentBytes(sendSpy), QByteArray("dropped\rline"));
    QCOMPARE(fileSpy.count(), qsizetype(0));
}

void Tst_terminalwidget::dropFileEmitsFileDropped()
{
    const QString path = m_tempDir.path() + QStringLiteral("/dropped.bin");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("payload");
    }
    QWidget* vp = m_term->viewport();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    mime.setText(path); // file managers also provide the path as text: the URL wins
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &enter);
    QVERIFY(enter.isAccepted());

    QSignalSpy sendSpy(m_term, &TerminalWidget::sendData);
    QSignalSpy fileSpy(m_term, &TerminalWidget::fileDropped);
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(fileSpy.count(), qsizetype(1));
    QCOMPARE(QDir::cleanPath(fileSpy.at(0).at(0).toString()), QDir::cleanPath(path));
    QCOMPARE(sendSpy.count(), qsizetype(0));

    // Something that is neither text nor a URL is refused.
    QMimeData other;
    other.setData(QStringLiteral("application/x-unknown"), QByteArray("?"));
    QDragEnterEvent refused(QPoint(10, 10), Qt::CopyAction, &other, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &refused);
    QVERIFY(!refused.isAccepted());
}

void Tst_terminalwidget::pasteIgnoredWhenInputDisabled()
{
    m_term->setInputEnabled(false);
    QApplication::clipboard()->setText(QStringLiteral("x"));
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    m_term->paste();
    m_term->pasteText(QStringLiteral("y"));
    QTest::keyClick(m_term, Qt::Key_Insert, Qt::ShiftModifier);
    QTest::mouseClick(m_term->viewport(), Qt::MiddleButton, Qt::NoModifier, cellCenter(0, 0));
    QCOMPARE(spy.count(), qsizetype(0));
}

void Tst_terminalwidget::clearSelectionEmitsSignal()
{
    m_term->feedData("abc");
    QSignalSpy spy(m_term, &TerminalWidget::selectionChanged);
    m_term->selectAll();
    QCOMPARE(spy.count(), qsizetype(1));
    QVERIFY(m_term->hasSelection());
    m_term->clearSelection();
    QCOMPARE(spy.count(), qsizetype(2));
    QVERIFY(!m_term->hasSelection());
    m_term->clearSelection(); // nothing to clear: no signal
    QCOMPARE(spy.count(), qsizetype(2));
}

// -------------------------------------------------------------------------------------------
// Model / rendering
// -------------------------------------------------------------------------------------------

void Tst_terminalwidget::colouredBootLog()
{
    m_term->feedData(esc("[0;32m[    0.000000]") + esc("[0m Booting Linux on physical CPU 0x0\r\n") +
                     esc("[1;33m[    0.123456] rockchip-pinctrl") + esc("[0m: probed\r\n") + esc("[31mERROR") +
                     esc("[39m: ") + esc("[4munderlined") + esc("[24m done\r\n") + esc("[38;5;208m256-colour") +
                     esc("[0m and ") + esc("[38;2;10;20;30mtruecolor") + esc("[0m\r\n"));
    const TerminalScreen* screen = m_term->screen();
    QCOMPARE(screen->lineText(0), QStringLiteral("[    0.000000] Booting Linux on physical CPU 0x0"));
    QCOMPARE(screen->lineText(1), QStringLiteral("[    0.123456] rockchip-pinctrl: probed"));
    QCOMPARE(screen->lineText(2), QStringLiteral("ERROR: underlined done"));
    QCOMPARE(screen->lineText(3), QStringLiteral("256-colour and truecolor"));
    QCOMPARE(screen->lineText(4), QString());
    QCOMPARE(screen->cursor().row, 4);
    QCOMPARE(screen->cursor().col, 0);

    QVERIFY(screen->line(0).cells.at(0).attr.fg == Terminal::Color::indexed(2));
    QVERIFY(screen->line(0).cells.at(15).attr.fg.isDefault());
    QVERIFY(screen->line(1).cells.at(0).attr.has(Terminal::Bold));
    QVERIFY(screen->line(1).cells.at(0).attr.fg == Terminal::Color::indexed(3));
    QVERIFY(screen->line(2).cells.at(7).attr.has(Terminal::Underline));
    QVERIFY(!screen->line(2).cells.at(18).attr.has(Terminal::Underline));
    QVERIFY(screen->line(3).cells.at(0).attr.fg == Terminal::Color::indexed(208));
    QVERIFY(screen->line(3).cells.at(15).attr.fg == Terminal::Color::rgb(10, 20, 30));

    // The coalesced repaint fires without further input.
    QTest::qWait(40);
    const QImage image = m_term->viewport()->grab().toImage();
    QVERIFY(!isUniform(image, m_term->colorPalette().background));
}

void Tst_terminalwidget::burstFillsScrollback()
{
    const int rows = m_term->visibleRows();
    m_term->feedData(numberedLines(3000));
    const TerminalScreen* screen = m_term->screen();
    QVERIFY(screen->scrollbackSize() > 0);
    QCOMPARE(screen->scrollbackSize(), 3001 - rows); // 3000 lines + the cursor line
    QCOMPARE(m_term->verticalScrollBar()->maximum(), screen->scrollbackSize());
    QCOMPARE(m_term->verticalScrollBar()->value(), screen->scrollbackSize());
    QVERIFY(m_term->isAtBottom());
    QCOMPARE(screen->lineText(0), QStringLiteral("line 0"));
    QCOMPARE(screen->lineText(screen->scrollbackSize() + screen->cursor().row - 1), QStringLiteral("line 2999"));
}

void Tst_terminalwidget::scrollingUpFreezesView()
{
    m_term->feedData(numberedLines(500));
    QScrollBar* bar = m_term->verticalScrollBar();
    const int max = bar->maximum();
    QVERIFY(max > 20);

    m_term->scrollLines(-10);
    QCOMPARE(bar->value(), max - 10);
    QVERIFY(!m_term->isAtBottom());

    // New output grows the range but must not move the frozen view.
    m_term->feedData(numberedLines(20, "more "));
    QCOMPARE(bar->maximum(), max + 20);
    QCOMPARE(bar->value(), max - 10);
    QVERIFY(!m_term->isAtBottom());

    m_term->scrollPages(-1);
    QCOMPARE(bar->value(), max - 10 - m_term->visibleRows());

    m_term->scrollToBottom();
    QCOMPARE(bar->value(), bar->maximum());
    QVERIFY(m_term->isAtBottom());
    // ... and it follows output again.
    m_term->feedData(numberedLines(5, "tail "));
    QCOMPARE(bar->value(), bar->maximum());
}

void Tst_terminalwidget::fullScrollbackKeepsFrozenViewAndSelection()
{
    m_term->setScrollbackMax(100);
    m_term->feedData(numberedLines(300)); // the scrollback is full: every new line drops the oldest
    const TerminalScreen* screen = m_term->screen();
    QScrollBar* bar = m_term->verticalScrollBar();
    QCOMPARE(bar->maximum(), 100);

    m_term->scrollLines(-10);
    const int v = bar->value();
    QCOMPARE(v, 90);
    const QString top = screen->lineText(v);
    QVERIFY(!top.isEmpty());

    // Select a word on viewport row 2 (absolute line v + 2).
    QWidget* vp = m_term->viewport();
    QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(2, 0));
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(2, 0));
    const QString sel = m_term->selectedText();
    QVERIFY(!sel.isEmpty());

    // Seven more lines: the range stays 0..100, the view and the selection keep their text.
    m_term->feedData(numberedLines(7, "more "));
    QCOMPARE(bar->maximum(), 100);
    QCOMPARE(bar->value(), v - 7);
    QCOMPARE(screen->lineText(bar->value()), top);
    QCOMPARE(m_term->selectedText(), sel);
    QVERIFY(!m_term->isAtBottom());

    // A flood pushes both the view and the selection off the top.
    m_term->feedData(numberedLines(200, "flood "));
    QCOMPARE(bar->value(), 0);
    QVERIFY(!m_term->hasSelection());

    // Follow mode is unchanged.
    m_term->scrollToBottom();
    m_term->feedData(numberedLines(5, "tail "));
    QVERIFY(m_term->isAtBottom());
    QCOMPARE(bar->value(), bar->maximum());
}

void Tst_terminalwidget::keyPressReturnsToBottom()
{
    m_term->feedData(numberedLines(300));
    QScrollBar* bar = m_term->verticalScrollBar();
    m_term->scrollLines(-25);
    QVERIFY(!m_term->isAtBottom());
    const int frozen = bar->value();
    m_term->feedData(numberedLines(10, "x "));
    QCOMPARE(bar->value(), frozen);

    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::keyClick(m_term, Qt::Key_A);
    QCOMPARE(sentBytes(spy), QByteArray("a"));
    QVERIFY(m_term->isAtBottom());
    QCOMPARE(bar->value(), bar->maximum());
}

void Tst_terminalwidget::wheelScrollsAndCtrlWheelZooms()
{
    m_term->feedData(numberedLines(200));
    QScrollBar* bar = m_term->verticalScrollBar();
    const int max = bar->maximum();
    sendWheel(120, Qt::NoModifier); // one notch up = 3 lines
    QCOMPARE(bar->value(), max - 3);
    sendWheel(-120, Qt::NoModifier);
    QCOMPARE(bar->value(), max);
    // Touchpad-style sub-notch deltas accumulate.
    sendWheel(40, Qt::NoModifier);
    QCOMPARE(bar->value(), max - 1);
    sendWheel(20, Qt::NoModifier);
    QCOMPARE(bar->value(), max - 1);
    sendWheel(20, Qt::NoModifier);
    QCOMPARE(bar->value(), max - 2);
    m_term->scrollToBottom();

    QSignalSpy spy(m_term, &TerminalWidget::fontZoomed);
    const int base = m_term->terminalFont().pointSize();
    sendWheel(120, Qt::ControlModifier);
    QCOMPARE(m_term->terminalFont().pointSize(), base + 1);
    QCOMPARE(spy.count(), qsizetype(1));
    QCOMPARE(bar->value(), bar->maximum()); // Ctrl+wheel does not scroll
    sendWheel(-120, Qt::ControlModifier);
    QCOMPARE(m_term->terminalFont().pointSize(), base);
    QCOMPARE(spy.count(), qsizetype(2));
}

void Tst_terminalwidget::zoomBoundsAndSignal()
{
    QSignalSpy spy(m_term, &TerminalWidget::fontZoomed);
    const int base = m_term->terminalFont().pointSize();
    QCOMPARE(base, 10);

    m_term->zoomIn();
    QCOMPARE(m_term->terminalFont().pointSize(), 11);
    QCOMPARE(spy.count(), qsizetype(1));
    QCOMPARE(spy.at(0).at(0).value<QFont>().pointSize(), 11);
    m_term->zoomOut();
    QCOMPARE(m_term->terminalFont().pointSize(), 10);
    QCOMPARE(spy.count(), qsizetype(2));

    for (int i = 0; i < 20; ++i) {
        m_term->zoomOut();
    }
    QCOMPARE(m_term->terminalFont().pointSize(), 6); // lower bound
    QCOMPARE(spy.count(), qsizetype(2 + 4));         // no signal once clamped
    for (int i = 0; i < 60; ++i) {
        m_term->zoomIn();
    }
    QCOMPARE(m_term->terminalFont().pointSize(), 40); // upper bound
    QCOMPARE(spy.count(), qsizetype(6 + 34));
    m_term->resetZoom();
    QCOMPARE(m_term->terminalFont().pointSize(), 10);
    QCOMPARE(spy.count(), qsizetype(41));
    m_term->resetZoom();
    QCOMPARE(spy.count(), qsizetype(41));

    // Keyboard zoom: Ctrl+'+' / Ctrl+'-' / Ctrl+0, never sent to the device.
    QSignalSpy sendSpy(m_term, &TerminalWidget::sendData);
    QTest::keyClick(m_term, Qt::Key_Plus, Qt::ControlModifier);
    QCOMPARE(m_term->terminalFont().pointSize(), 11);
    QTest::keyClick(m_term, Qt::Key_Minus, Qt::ControlModifier);
    QTest::keyClick(m_term, Qt::Key_Minus, Qt::ControlModifier);
    QCOMPARE(m_term->terminalFont().pointSize(), 9);
    QTest::keyClick(m_term, Qt::Key_0, Qt::ControlModifier);
    QCOMPARE(m_term->terminalFont().pointSize(), 10);
    QCOMPARE(sendSpy.count(), qsizetype(0));

    // setTerminalFont() defines a new base size for resetZoom().
    QFont bigger = m_term->terminalFont();
    bigger.setPointSize(14);
    m_term->setTerminalFont(bigger);
    QCOMPARE(m_term->terminalFont().pointSize(), 14);
    m_term->zoomIn();
    QCOMPARE(m_term->terminalFont().pointSize(), 15);
    m_term->resetZoom();
    QCOMPARE(m_term->terminalFont().pointSize(), 14);
}

void Tst_terminalwidget::gridSizeChangedOnResize()
{
    QSignalSpy spy(m_term, &TerminalWidget::gridSizeChanged);
    const int cols = m_term->columns();
    const int rows = m_term->visibleRows();

    m_term->resize(m_term->width() + 10 * m_cellW, m_term->height());
    QTRY_VERIFY(spy.count() >= 1);
    QCOMPARE(m_term->columns(), cols + 10);
    QCOMPARE(m_term->visibleRows(), rows);
    QCOMPARE(spy.last().at(0).toInt(), m_term->visibleRows());
    QCOMPARE(spy.last().at(1).toInt(), m_term->columns());

    spy.clear();
    m_term->resize(m_term->width(), m_term->height() - 5 * m_cellH);
    QTRY_VERIFY(spy.count() >= 1);
    QCOMPARE(m_term->visibleRows(), rows - 5);
    QCOMPARE(spy.last().at(0).toInt(), rows - 5);

    // A resize that does not cross a cell boundary is not a grid change.
    const int slack = m_cellW - 1 - (m_term->viewport()->width() % m_cellW);
    if (slack > 0) {
        spy.clear();
        m_term->resize(m_term->width() + slack, m_term->height());
        QTest::qWait(20);
        QCOMPARE(spy.count(), qsizetype(0));
        QCOMPARE(m_term->columns(), cols + 10);
    }

    // Fewer rows with content: lines above the cursor move into the scrollback (no data loss).
    const int rows2 = m_term->visibleRows();
    m_term->feedData(numberedLines(rows2 - 1)); // fills every row but the last (cursor row)
    QCOMPARE(m_term->screen()->scrollbackSize(), 0);
    QCOMPARE(m_term->screen()->cursor().row, rows2 - 1);
    m_term->resize(m_term->width(), m_term->height() - 5 * m_cellH);
    QTRY_COMPARE(m_term->visibleRows(), rows2 - 5);
    QVERIFY(m_term->screen()->scrollbackSize() >= 5);
    QCOMPARE(m_term->screen()->lineText(0), QStringLiteral("line 0"));
    QCOMPARE(m_term->verticalScrollBar()->maximum(), m_term->screen()->scrollbackSize());
    QVERIFY(m_term->isAtBottom());
}

void Tst_terminalwidget::clearScreenKeepsScrollback()
{
    m_term->feedData("one\r\ntwo\r\nthree");
    TerminalScreen* screen = m_term->screen();
    QCOMPARE(screen->scrollbackSize(), 0);
    QCOMPARE(screen->lineText(2), QStringLiteral("three"));

    m_term->clearScreen();
    QCOMPARE(screen->scrollbackSize(), 3);
    QCOMPARE(screen->scrollbackLine(0).text(), QStringLiteral("one"));
    QCOMPARE(screen->scrollbackLine(2).text(), QStringLiteral("three"));
    for (int r = 0; r < screen->rows(); ++r) {
        QVERIFY2(screen->line(r).text().isEmpty(), qPrintable(QStringLiteral("row %1 not blank").arg(r)));
    }
    QCOMPARE(screen->cursor().row, 0);
    QCOMPARE(screen->cursor().col, 0);
    QCOMPARE(m_term->verticalScrollBar()->maximum(), 3);
    QVERIFY(m_term->isAtBottom());
}

void Tst_terminalwidget::clearScrollbackResetsScrollBar()
{
    m_term->feedData(numberedLines(200));
    TerminalScreen* screen = m_term->screen();
    QVERIFY(screen->scrollbackSize() > 0);
    m_term->selectAll();

    m_term->clearScrollback();
    QCOMPARE(screen->scrollbackSize(), 0);
    QCOMPARE(m_term->verticalScrollBar()->maximum(), 0);
    QCOMPARE(m_term->verticalScrollBar()->value(), 0);
    QVERIFY(m_term->isAtBottom());
    QVERIFY(!m_term->hasSelection()); // a selection into vanished lines is dropped
    // The visible screen is untouched.
    QCOMPARE(screen->lineText(screen->cursor().row - 1), QStringLiteral("line 199"));
}

void Tst_terminalwidget::resetTerminalClearsEverything()
{
    m_term->feedData(numberedLines(100));
    m_term->feedData(esc("[1;31m") + esc("[?1h") + esc("[?2004h") + "red");
    TerminalScreen* screen = m_term->screen();
    QVERIFY(screen->scrollbackSize() > 0);
    QVERIFY(m_term->parser()->cursorKeyApplicationMode());
    QVERIFY(m_term->parser()->bracketedPasteMode());
    m_term->selectAll();

    m_term->resetTerminal();
    QCOMPARE(screen->scrollbackSize(), 0);
    QCOMPARE(screen->totalLines(), screen->rows());
    for (int r = 0; r < screen->rows(); ++r) {
        QVERIFY(screen->line(r).text().isEmpty());
    }
    QCOMPARE(screen->cursor().row, 0);
    QCOMPARE(screen->cursor().col, 0);
    QVERIFY(screen->currentAttributes() == Terminal::Attributes());
    QVERIFY(!m_term->parser()->cursorKeyApplicationMode());
    QVERIFY(!m_term->parser()->bracketedPasteMode());
    QVERIFY(!m_term->hasSelection());
    QCOMPARE(m_term->verticalScrollBar()->maximum(), 0);
    QVERIFY(m_term->isAtBottom());
    // Fully functional afterwards.
    m_term->feedData("back");
    QCOMPARE(screen->lineText(0), QStringLiteral("back"));
}

void Tst_terminalwidget::localEchoShowsTypedText()
{
    QVERIFY(!m_term->localEcho()); // default off
    m_term->setLocalEcho(true);
    QVERIFY(m_term->localEcho());
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    QTest::keyClicks(m_term, QStringLiteral("hi"));
    QCOMPARE(m_term->screen()->lineText(0), QStringLiteral("hi"));
    QTest::keyClick(m_term, Qt::Key_Return); // CR is echoed as a new line
    QCOMPARE(m_term->screen()->cursor().row, 1);
    QCOMPARE(m_term->screen()->cursor().col, 0);
    QTest::keyClicks(m_term, QStringLiteral("ok"));
    QCOMPARE(m_term->screen()->lineText(1), QStringLiteral("ok"));
    QCOMPARE(sentBytes(spy), QByteArray("hi\rok")); // the echo never changes what is sent

    m_term->setLocalEcho(false);
    QTest::keyClicks(m_term, QStringLiteral("zz"));
    QCOMPARE(m_term->screen()->lineText(1), QStringLiteral("ok"));
}

void Tst_terminalwidget::dsrReply()
{
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    m_term->feedData(esc("[6n"));
    QCOMPARE(sentBytes(spy), esc("[1;1R"));

    spy.clear();
    m_term->feedData("abc" + esc("[6n"));
    QCOMPARE(sentBytes(spy), esc("[1;4R"));

    spy.clear();
    m_term->feedData(esc("[5;7H") + esc("[6n"));
    QCOMPARE(sentBytes(spy), esc("[5;7R"));

    spy.clear();
    m_term->feedData(esc("[5n"));
    QCOMPARE(sentBytes(spy), esc("[0n"));
    QVERIFY(m_term->screen()->lineText(0).startsWith(QStringLiteral("abc"))); // replies are not echoed
}

void Tst_terminalwidget::daReply()
{
    QSignalSpy spy(m_term, &TerminalWidget::sendData);
    m_term->feedData(esc("[c"));
    QCOMPARE(sentBytes(spy), esc("[?1;2c"));
    spy.clear();
    m_term->feedData(esc("[0c"));
    QCOMPARE(sentBytes(spy), esc("[?1;2c"));
    spy.clear();
    m_term->feedData(esc("Z"));
    QCOMPARE(sentBytes(spy), esc("[?1;2c"));
}

void Tst_terminalwidget::titleChangedOnOsc()
{
    QSignalSpy spy(m_term, &TerminalWidget::titleChanged);
    m_term->feedData(esc("]0;My Title\x07"));
    QCOMPARE(spy.count(), qsizetype(1));
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("My Title"));
    QCOMPARE(m_term->screen()->title(), QStringLiteral("My Title"));

    m_term->feedData(esc("]2;root@rv1106:~") + esc("\\")); // OSC 2 terminated by ST
    QCOMPARE(spy.count(), qsizetype(2));
    QCOMPARE(spy.at(1).at(0).toString(), QStringLiteral("root@rv1106:~"));
}

void Tst_terminalwidget::bellRangAndFlash()
{
    QSignalSpy spy(m_term, &TerminalWidget::bellRang);
    const QColor background = m_term->colorPalette().background;
    m_term->feedData(esc("[?25l")); // hide the cursor so the corner pixel is pure background
    const QPoint corner(m_term->viewport()->width() - 2, m_term->viewport()->height() - 2);

    m_term->feedData("\x07"); // bell disabled in init(): signal only, no flash
    QCOMPARE(spy.count(), qsizetype(1));
    QCOMPARE(QColor(m_term->viewport()->grab().toImage().pixel(corner)), background);

    m_term->setBellEnabled(true);
    m_term->feedData("\x07");
    QCOMPARE(spy.count(), qsizetype(2));
    QVERIFY(QColor(m_term->viewport()->grab().toImage().pixel(corner)) != background); // flashing
    QTRY_VERIFY_WITH_TIMEOUT(QColor(m_term->viewport()->grab().toImage().pixel(corner)) == background, 3000);
}

void Tst_terminalwidget::bellBurstIsThrottled()
{
    m_term->setBellEnabled(true);
    QSignalSpy spy(m_term, &TerminalWidget::bellRang);
    const QColor background = m_term->colorPalette().background;
    m_term->feedData(esc("[?25l"));
    const QPoint corner(m_term->viewport()->width() - 2, m_term->viewport()->height() - 2);
    auto cornerColor = [this, corner]() { return QColor(m_term->viewport()->grab().toImage().pixel(corner)); };

    // A burst of BELs in one chunk: the signal is still per BEL, but only one flash is started
    // and the background returns to normal even though the BELs kept coming.
    m_term->feedData(QByteArray(20, '\x07'));
    QCOMPARE(spy.count(), qsizetype(20));
    QVERIFY(cornerColor() != background);
    QTRY_VERIFY_WITH_TIMEOUT(cornerColor() == background, 3000);

    // A stream of BELs spaced closer than the flash duration (30 ms < 120 ms) must not keep the
    // background tinted: only one bell per 250 ms window is accepted, so the tint is gone for at
    // least 130 ms between flashes. Without suppression every BEL restarts the flash timer and
    // the corner never shows the background while the stream flows.
    QTest::qWait(300); // leave the previous suppression window
    int sawBackground = 0;
    for (int i = 0; i < 14; ++i) {
        m_term->feedData("\x07");
        if (i == 0) {
            QVERIFY(cornerColor() != background); // the first bell of the stream flashes
        }
        QTest::qWait(30);
        if (cornerColor() == background) {
            ++sawBackground;
        }
    }
    QCOMPARE(spy.count(), qsizetype(34));
    QVERIFY2(sawBackground >= 1, "a stream of bells kept the background tinted");
    QTRY_VERIFY_WITH_TIMEOUT(cornerColor() == background, 3000);
}

void Tst_terminalwidget::findNextWrapsAndSelects()
{
    m_term->feedData("alpha\r\nbeta\r\ngamma\r\nalpha again\r\n");
    QVERIFY(!m_term->findNext(QString()));
    QVERIFY(!m_term->findNext(QStringLiteral("nothing here")));
    QVERIFY(!m_term->hasSelection());

    // From the cursor (row 4) the search wraps around to line 0.
    QVERIFY(m_term->findNext(QStringLiteral("alpha")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("alpha"));
    // Continues after the current match: line 3.
    QVERIFY(m_term->findNext(QStringLiteral("alpha")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("alpha"));
    QVERIFY(m_term->findNext(QStringLiteral("again"))); // same line, after the match
    QCOMPARE(m_term->selectedText(), QStringLiteral("again"));
    // Wraps: from line 3 back to line 0.
    QVERIFY(m_term->findNext(QStringLiteral("alpha")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("alpha"));
    QVERIFY(m_term->findNext(QStringLiteral("beta")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("beta"));

    // A miss keeps the current selection.
    QVERIFY(!m_term->findNext(QStringLiteral("delta")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("beta"));
    QVERIFY(QApplication::clipboard()->text().isEmpty()); // find never touches the clipboard
}

void Tst_terminalwidget::findPreviousAndCaseSensitivity()
{
    m_term->feedData("Alpha\r\nbeta\r\nALPHA\r\n");
    QVERIFY(!m_term->findNext(QStringLiteral("alpha"), true)); // case-sensitive: no lowercase match
    QVERIFY(m_term->findNext(QStringLiteral("alpha"), false));
    QCOMPARE(m_term->selectedText(), QStringLiteral("Alpha")); // wrapped to line 0
    QVERIFY(m_term->findNext(QStringLiteral("ALPHA"), true));
    QCOMPARE(m_term->selectedText(), QStringLiteral("ALPHA")); // line 2

    QVERIFY(m_term->findPrevious(QStringLiteral("alpha")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("Alpha")); // back to line 0
    QVERIFY(m_term->findPrevious(QStringLiteral("alpha")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("ALPHA")); // wrapped backwards to line 2
    QVERIFY(m_term->findPrevious(QStringLiteral("beta")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("beta"));
    QVERIFY(!m_term->findPrevious(QStringLiteral("Beta"), true));
    QVERIFY(!m_term->findPrevious(QString()));
}

void Tst_terminalwidget::paintingProducesNonUniformImage()
{
    const QColor background = m_term->colorPalette().background;
    m_term->feedData(esc("[?25l")); // no cursor: an empty screen is a uniform background
    QImage empty = m_term->viewport()->grab().toImage();
    QVERIFY(empty.width() > 0 && empty.height() > 0);
    QVERIFY(isUniform(empty, background));

    m_term->feedData("Hello, terminal! ##########");
    const QImage painted = m_term->viewport()->grab().toImage();
    QVERIFY(!isUniform(painted, background));

    // The cursor is painted too (hollow when unfocused, block when focused).
    m_term->feedData(esc("[?25h") + esc("[2J") + esc("[H"));
    QVERIFY(!isUniform(m_term->viewport()->grab().toImage(), background));
}

void Tst_terminalwidget::paintWideCharsAndAllAttributes()
{
    m_term->feedData(QStringLiteral("你好世界 wide\r\n").toUtf8());
    m_term->feedData(esc("[1mbold ") + esc("[0m") + esc("[2mdim ") + esc("[0m") + esc("[3mitalic ") + esc("[0m") +
                     esc("[4munder ") + esc("[0m") + esc("[5mblink ") + esc("[0m") + esc("[7minverse ") + esc("[0m") +
                     esc("[8mhidden ") + esc("[0m") + esc("[9mstrike ") + esc("[0m") + esc("[1;2;3;4;5;7;8;9mall") +
                     esc("[0m\r\n"));
    m_term->feedData(esc("[31;42mfg/bg ") + esc("[0m") + esc("[38;5;196;48;5;21m256 ") + esc("[0m") +
                     esc("[38;2;255;0;0;48;2;0;0;255mtruecolor ") + esc("[0m") + esc("[1;34mbold-is-bright") +
                     esc("[0m\r\n"));
    m_term->feedData(QStringLiteral("中文 mixed 文字 end\r\n").toUtf8());

    const TerminalScreen* screen = m_term->screen();
    QCOMPARE(screen->scrollbackSize(), 0);
    QCOMPARE(screen->line(0).text(), QStringLiteral("你好世界 wide"));
    QVERIFY(screen->line(0).cells.at(0).attr.has(Terminal::WideLead));
    QVERIFY(screen->line(0).cells.at(1).attr.has(Terminal::WideTrail));
    QCOMPARE(screen->line(1).text(), QStringLiteral("bold dim italic under blink inverse hidden strike all"));
    QVERIFY(screen->line(1).cells.at(0).attr.has(Terminal::Bold));
    QVERIFY(screen->line(1).cells.at(5).attr.has(Terminal::Dim));
    QVERIFY(screen->line(1).cells.at(48).attr.has(Terminal::Strike));

    // Paint with a selection covering everything, the cursor on a wide char and every attribute.
    m_term->selectAll();
    m_term->feedData(esc("[1;1H"));
    QImage image = m_term->viewport()->grab().toImage();
    QVERIFY(!isUniform(image, m_term->colorPalette().background));
    QTest::qWait(40); // let the coalesced repaint timer fire as well
    m_term->viewport()->repaint();
    m_term->clearSelection();
    m_term->feedData(esc("[4;1H") + esc("[?25l"));
    image = m_term->viewport()->grab().toImage();
    QVERIFY(!isUniform(image, m_term->colorPalette().background));

    // A wide character wraps as a whole when only one column is left.
    m_term->feedData(esc("[2J") + esc("[H"));
    m_term->feedData(QByteArray(m_term->columns() - 1, 'x') + QStringLiteral("中").toUtf8());
    QCOMPARE(screen->line(0).text(), QString(m_term->columns() - 1, QLatin1Char('x')));
    QCOMPARE(screen->line(1).text(), QStringLiteral("中"));
    QVERIFY(!isUniform(m_term->viewport()->grab().toImage(), m_term->colorPalette().background));
}

void Tst_terminalwidget::colorPaletteAppliesToPainting()
{
    Terminal::Palette palette = m_term->colorPalette();
    QCOMPARE(palette.name, QStringLiteral("dark")); // built-in default until SessionWidget applies the theme
    palette.name = QStringLiteral("test");
    palette.background = QColor(0x10, 0x20, 0x30);
    palette.foreground = QColor(0xF0, 0xE0, 0xD0);
    m_term->setColorPalette(palette);
    QCOMPARE(m_term->colorPalette().name, QStringLiteral("test"));
    QCOMPARE(m_term->colorPalette().background, QColor(0x10, 0x20, 0x30));

    m_term->feedData(esc("[?25l"));
    const QImage image = m_term->viewport()->grab().toImage();
    QVERIFY(isUniform(image, QColor(0x10, 0x20, 0x30)));
    // The viewport palette follows so Qt never flashes another colour before paintEvent.
    QCOMPARE(m_term->viewport()->palette().color(QPalette::Window), QColor(0x10, 0x20, 0x30));

    m_term->feedData("text");
    QVERIFY(!isUniform(m_term->viewport()->grab().toImage(), QColor(0x10, 0x20, 0x30)));
}

void Tst_terminalwidget::cursorBlockWhenConnectedHollowWhenNot()
{
    const Terminal::Palette palette = m_term->colorPalette();
    const QRect cell(0, 0, m_cellW, m_cellH); // cursor at home on a blank screen
    const QPoint center = cell.center();
    QVERIFY(m_term->hasFocus());
    QVERIFY(m_term->inputEnabled());

    // Connected + focused: a filled block in the cursor colour.
    QImage image = m_term->viewport()->grab().toImage();
    QCOMPARE(QColor(image.pixel(center)), palette.cursor);
    QCOMPARE(QColor(image.pixel(cell.topLeft())), palette.cursor);

    // Disconnected: hollow - the outline is drawn, the inside is background.
    m_term->setInputEnabled(false);
    image = m_term->viewport()->grab().toImage();
    QCOMPARE(QColor(image.pixel(center)), palette.background);
    QVERIFY(!isUniform(image.copy(cell), palette.background));

    // DECTCEM off: nothing at all.
    m_term->setInputEnabled(true);
    m_term->feedData(esc("[?25l"));
    image = m_term->viewport()->grab().toImage();
    QVERIFY(isUniform(image.copy(cell), palette.background));
    m_term->feedData(esc("[?25h"));
    image = m_term->viewport()->grab().toImage();
    QCOMPARE(QColor(image.pixel(center)), palette.cursor);

    // The cursor follows the text and is drawn over the character there.
    m_term->feedData("ab" + esc("[1;1H"));
    image = m_term->viewport()->grab().toImage();
    QVERIFY(!isUniform(image.copy(cell), palette.cursor)); // glyph 'a' in cursorText colour
    QCOMPARE(QColor(image.pixel(cell.topLeft())), palette.cursor);
}

void Tst_terminalwidget::selectionIsPainted()
{
    const Terminal::Palette palette = m_term->colorPalette();
    m_term->feedData("ab   cd\r\n" + esc("[?25l"));
    const QRect blankCell(3 * m_cellW, 0, m_cellW, m_cellH); // the blank between "ab" and "cd"
    QImage image = m_term->viewport()->grab().toImage();
    QVERIFY(isUniform(image.copy(blankCell), palette.background));

    dragSelect(0, 0, 0, 6);
    QCOMPARE(m_term->selectedText(), QStringLiteral("ab   cd"));
    image = m_term->viewport()->grab().toImage();
    QVERIFY(isUniform(image.copy(blankCell), palette.selection));
    // Unselected cells on the same row keep the background.
    const QRect afterCell(9 * m_cellW, 0, m_cellW, m_cellH);
    QVERIFY(isUniform(image.copy(afterCell), palette.background));

    m_term->clearSelection();
    image = m_term->viewport()->grab().toImage();
    QVERIFY(isUniform(image.copy(blankCell), palette.background));
}

void Tst_terminalwidget::feedPerformance()
{
    QByteArray chunk;
    while (chunk.size() < 1024) {
        chunk += esc("[32m[   12.345678]") + esc("[0m usb 1-1: new high-speed USB device number 3 using dwc2\r\n");
    }
    chunk.truncate(1024); // chunk boundaries split escape sequences and lines

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 1000; ++i) {
        m_term->feedData(chunk);
    }
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed < 5000, qPrintable(QStringLiteral("1000 x 1 KB took %1 ms").arg(elapsed)));
    qInfo("feedPerformance: 1000 x 1 KB in %lld ms", static_cast<long long>(elapsed));

    QVERIFY(m_term->screen()->scrollbackSize() > 1000);
    QVERIFY(m_term->screen()->scrollbackSize() <= 10000); // default scrollback cap
    QVERIFY(m_term->isAtBottom());
    QCoreApplication::processEvents();
    QVERIFY(!isUniform(m_term->viewport()->grab().toImage(), m_term->colorPalette().background));
}

void Tst_terminalwidget::repaintsAreCoalescedWhileFollowing()
{
    // Flush whatever the fixture left pending so only the paints below are counted.
    m_term->resetTerminal();
    QCoreApplication::processEvents();
    QTest::qWait(40);

    PaintCounter counter;
    QWidget* vp = m_term->viewport();
    vp->installEventFilter(&counter);

    // 200 chunks, each scrolling the (full) screen by three lines, with the event loop spun in
    // between like QSerialPort::readyRead would. Every chunk moves the scrollbar; the repaint
    // must still go through the 16 ms coalescer instead of one full paint per chunk.
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 200; ++i) {
        m_term->feedData(QByteArrayLiteral("line ") + QByteArray::number(i) + "\r\nline b\r\nline c\r\n");
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
    QTest::qWait(50);
    const qint64 elapsed = t.elapsed();
    vp->removeEventFilter(&counter);

    qInfo("repaintsAreCoalescedWhileFollowing: %d paints for 200 chunks in %lld ms", counter.paints,
          static_cast<long long>(elapsed));
    QVERIFY(counter.paints >= 1);
    QVERIFY2(counter.paints <= static_cast<int>(elapsed / 16) + 3,
             qPrintable(QStringLiteral("%1 paints in %2 ms").arg(counter.paints).arg(elapsed)));
    // Follow-output still works with the deferred repaint.
    QVERIFY(m_term->isAtBottom());
    QCOMPARE(m_term->verticalScrollBar()->value(), m_term->verticalScrollBar()->maximum());
    QVERIFY(!isUniform(vp->grab().toImage(), m_term->colorPalette().background));
}

void Tst_terminalwidget::partialRepaintConsumesDirtyState()
{
    // Baseline: the initial all-dirty paint has happened.
    m_term->feedData("x");
    QTest::qWait(40);
    QVERIFY(!m_term->screen()->isDirty());

    // A single-row change is painted partially and still consumes the dirty state.
    m_term->feedData("y");
    QVERIFY(m_term->screen()->isDirty());
    QVERIFY(!m_term->screen()->allDirty());
    QTest::qWait(40);
    QVERIFY(!m_term->screen()->isDirty());

    // Two mutations before the timer fires: both rows are painted, nothing is left dirty.
    m_term->feedData("a");
    m_term->feedData(esc("[10;1Hb"));
    QVERIFY(m_term->screen()->isDirty());
    QTest::qWait(40);
    QVERIFY(!m_term->screen()->isDirty());
    const TerminalScreen* screen = m_term->screen();
    QCOMPARE(screen->lineText(screen->scrollbackSize() + 9).trimmed(), QStringLiteral("b"));
    QCOMPARE(screen->lineText(screen->scrollbackSize()).trimmed(), QStringLiteral("xya"));
    // The rows are really on screen (grab renders from the model, so also check a pixel of row 9).
    const QImage image = m_term->viewport()->grab().toImage();
    QVERIFY(!isUniform(image.copy(QRect(0, 9 * m_cellH, m_cellW, m_cellH)), m_term->colorPalette().background));
}

void Tst_terminalwidget::scrollbackMaxTrims()
{
    m_term->feedData(numberedLines(500));
    TerminalScreen* screen = m_term->screen();
    QVERIFY(screen->scrollbackSize() > 100);

    m_term->setScrollbackMax(50);
    QCOMPARE(screen->scrollbackMax(), 50);
    QCOMPARE(screen->scrollbackSize(), 50);
    QCOMPARE(m_term->verticalScrollBar()->maximum(), 50);
    QVERIFY(m_term->isAtBottom());
    // The newest history survives.
    QCOMPARE(screen->lineText(49), QStringLiteral("line %1").arg(500 - m_term->visibleRows()));

    m_term->feedData(numberedLines(10, "new "));
    QCOMPARE(screen->scrollbackSize(), 50);
    QCOMPARE(m_term->verticalScrollBar()->maximum(), 50);

    m_term->setScrollbackMax(0);
    QCOMPARE(screen->scrollbackSize(), 0);
    QCOMPARE(m_term->verticalScrollBar()->maximum(), 0);
    m_term->feedData(numberedLines(10, "gone "));
    QCOMPARE(screen->scrollbackSize(), 0);
}

void Tst_terminalwidget::cursorPositionChangedSignal()
{
    QSignalSpy spy(m_term, &TerminalWidget::cursorPositionChanged);
    m_term->feedData("abc");
    QVERIFY(spy.count() >= 1);
    QCOMPARE(spy.last().at(0).toInt(), 0);
    QCOMPARE(spy.last().at(1).toInt(), 3);
    m_term->feedData("\r\n");
    QCOMPARE(spy.last().at(0).toInt(), 1);
    QCOMPARE(spy.last().at(1).toInt(), 0);
    m_term->feedData(esc("[10;20H"));
    QCOMPARE(spy.last().at(0).toInt(), 9);
    QCOMPARE(spy.last().at(1).toInt(), 19);
}

// -------------------------------------------------------------------------------------------
// HexDumpView
// -------------------------------------------------------------------------------------------

void Tst_terminalwidget::hexRxTxMarkers()
{
    HexDumpView view;
    QVERIFY(view.isReadOnly());
    QVERIFY(view.showTimestamps());
    QCOMPARE(view.bytesPerLine(), 16);
    QCOMPARE(view.maxLines(), 5000);

    view.appendReceived("Hello");
    QStringList lines = view.toPlainText().split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), qsizetype(2));
    const QRegularExpression rxHeader(QStringLiteral("^\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] RX 5 bytes$"));
    QVERIFY2(rxHeader.match(lines.at(0)).hasMatch(), qPrintable(lines.at(0)));
    QCOMPARE(lines.at(1), HexUtils::hexDump("Hello", 0, 16));
    QVERIFY(lines.at(1).startsWith(QStringLiteral("00000000  48 65 6C 6C 6F")));
    QVERIFY(lines.at(1).endsWith(QStringLiteral("|Hello|")));

    view.appendSent("\r\nOK");
    lines = view.toPlainText().split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), qsizetype(4));
    const QRegularExpression txHeader(QStringLiteral("^\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] TX 4 bytes$"));
    QVERIFY2(txHeader.match(lines.at(2)).hasMatch(), qPrintable(lines.at(2)));
    QCOMPARE(lines.at(3), HexUtils::hexDump("\r\nOK", 0, 16));
    QVERIFY(lines.at(3).contains(QStringLiteral("0D 0A 4F 4B")));
    QVERIFY(lines.at(3).endsWith(QStringLiteral("|..OK|"))); // non-printables as '.'
    QCOMPARE(view.document()->blockCount(), 4);

    // Offsets restart at 0 for every chunk and continue within a chunk.
    view.appendReceived(QByteArray(40, 'A'));
    lines = view.toPlainText().split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), qsizetype(8));
    QVERIFY(lines.at(5).startsWith(QStringLiteral("00000000  41 41")));
    QVERIFY(lines.at(6).startsWith(QStringLiteral("00000010  41 41")));
    QVERIFY(lines.at(7).startsWith(QStringLiteral("00000020  41 41 41 41 41 41 41 41 ")));
    QVERIFY(lines.at(7).endsWith(QStringLiteral("|AAAAAAAA|")));
}

void Tst_terminalwidget::hexColoursDifferForRxTx()
{
    HexDumpView view;
    view.appendReceived("rx");
    view.appendSent("tx");
    const QTextDocument* doc = view.document();
    QCOMPARE(doc->blockCount(), 4);
    const QColor rxHeader = firstFragmentColor(doc->findBlockByNumber(0));
    const QColor rxBytes = firstFragmentColor(doc->findBlockByNumber(1));
    const QColor txHeader = firstFragmentColor(doc->findBlockByNumber(2));
    const QColor txBytes = firstFragmentColor(doc->findBlockByNumber(3));
    QVERIFY(rxBytes.isValid() && txBytes.isValid());
    QVERIFY(rxBytes != txBytes);
    QCOMPARE(rxHeader, txHeader); // headers share one dim colour
    QVERIFY(rxHeader != rxBytes);
    QVERIFY(txHeader != txBytes);
}

void Tst_terminalwidget::hexMaxLinesTrimming()
{
    HexDumpView view;
    view.setShowTimestamps(false);
    view.setMaxLines(40);
    QCOMPARE(view.maxLines(), 40);
    for (int i = 0; i < 100; ++i) {
        view.appendReceived(QByteArray(16, static_cast<char>(i))); // header + one hex line
    }
    const int blocks = view.document()->blockCount();
    QVERIFY2(blocks <= 40 + 2, qPrintable(QStringLiteral("%1 blocks").arg(blocks)));
    QVERIFY(blocks >= 38);
    // Oldest chunks are gone, the newest is intact.
    QVERIFY(!view.toPlainText().contains(QStringLiteral("00 00 00 00 00 00 00 00")));
    QVERIFY(view.toPlainText().contains(QStringLiteral("63 63 63 63 63 63 63 63"))); // byte 99
    QVERIFY(view.document()->firstBlock().text().startsWith(QStringLiteral("RX 16 bytes")));

    view.setMaxLines(10);
    QCOMPARE(view.maxLines(), 10);
    QVERIFY(view.document()->blockCount() <= 12);
    QVERIFY(view.toPlainText().contains(QStringLiteral("63 63 63 63 63 63 63 63")));

    // A single chunk larger than the limit is trimmed to its tail.
    view.setMaxLines(3);
    view.appendSent(QByteArray(160, 'Z')); // header + 10 hex lines
    QVERIFY(view.document()->blockCount() <= 3 + 2);
    QVERIFY(view.document()->lastBlock().text().startsWith(QStringLiteral("00000090")));

    view.setMaxLines(0); // clamped to at least one block
    QCOMPARE(view.maxLines(), 1);
    QCOMPARE(view.document()->blockCount(), 1);
}

void Tst_terminalwidget::hexClearAll()
{
    HexDumpView view;
    view.appendReceived("abc");
    view.appendSent("def");
    QCOMPARE(view.document()->blockCount(), 4);
    view.clearAll();
    QVERIFY(view.toPlainText().isEmpty());
    QCOMPARE(view.document()->blockCount(), 1);
    // No leading empty block after a clear.
    view.setShowTimestamps(false);
    view.appendReceived("xy");
    QCOMPARE(view.document()->blockCount(), 2);
    QCOMPARE(view.document()->firstBlock().text(), QStringLiteral("RX 2 bytes"));
}

void Tst_terminalwidget::hexBytesPerLine()
{
    HexDumpView view;
    view.setShowTimestamps(false);
    const QByteArray data = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");

    view.setBytesPerLine(8);
    QCOMPARE(view.bytesPerLine(), 8);
    view.appendReceived(data);
    QStringList lines = view.toPlainText().split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), qsizetype(3));
    QCOMPARE(lines.mid(1).join(QLatin1Char('\n')), HexUtils::hexDump(data, 0, 8));
    QVERIFY(lines.at(2).startsWith(QStringLiteral("00000008  08 09 0A 0B 0C 0D 0E 0F")));

    view.clearAll();
    view.setBytesPerLine(32);
    QCOMPARE(view.bytesPerLine(), 32);
    view.appendReceived(data + data);
    lines = view.toPlainText().split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), qsizetype(2));
    QCOMPARE(lines.at(1), HexUtils::hexDump(data + data, 0, 32));

    // Only 8, 16 and 32 are supported: other values snap.
    view.setBytesPerLine(12);
    QCOMPARE(view.bytesPerLine(), 16);
    view.setBytesPerLine(5);
    QCOMPARE(view.bytesPerLine(), 8);
    view.setBytesPerLine(100);
    QCOMPARE(view.bytesPerLine(), 32);
    view.setBytesPerLine(0);
    QCOMPARE(view.bytesPerLine(), 8);
}

void Tst_terminalwidget::hexTimestampsToggle()
{
    HexDumpView view;
    view.setShowTimestamps(false);
    QVERIFY(!view.showTimestamps());
    view.appendReceived("AB");
    QCOMPARE(view.document()->firstBlock().text(), QStringLiteral("RX 2 bytes"));
    view.setShowTimestamps(true);
    view.appendSent("CD");
    const QString header = view.document()->findBlockByNumber(2).text();
    QVERIFY2(header.startsWith(QLatin1Char('[')), qPrintable(header));
    QVERIFY(header.endsWith(QStringLiteral("] TX 2 bytes")));
}

void Tst_terminalwidget::hexEmptyChunkIgnored()
{
    HexDumpView view;
    view.appendReceived(QByteArray());
    view.appendSent(QByteArray());
    QVERIFY(view.toPlainText().isEmpty());
    QCOMPARE(view.document()->blockCount(), 1);
}

void Tst_terminalwidget::hexTrimKeepsScrolledUpContent()
{
    HexDumpView view;
    view.setShowTimestamps(false);
    view.setMaxLines(40);
    view.resize(600, 200);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    for (int i = 0; i < 20; ++i) {
        view.appendReceived(QByteArray(16, static_cast<char>(i))); // header + 1 hex line = 40 blocks
    }
    QCOMPARE(view.document()->blockCount(), 40);
    QScrollBar* bar = view.verticalScrollBar();
    bar->setValue(5);
    QCoreApplication::processEvents();
    // firstVisibleBlock() is protected; the block under the viewport's top-left is the same thing.
    const QString topText = view.cursorForPosition(QPoint(2, 2)).block().text();
    QVERIFY2(topText.contains(QStringLiteral("02 02 02 02")), qPrintable(topText));

    view.appendReceived(QByteArray(48, 'Z')); // header + 3 hex lines -> 4 blocks trimmed
    QCoreApplication::processEvents();
    QCOMPARE(view.document()->blockCount(), 40);
    QCOMPARE(view.cursorForPosition(QPoint(2, 2)).block().text(), topText); // no jump
    QCOMPARE(bar->value(), 1);

    // Shrinking the cap must not jump either. Scroll deep enough that the visible block survives
    // the 10-block trim (a block that is itself trimmed away can only clamp to the top).
    bar->setValue(15);
    QCoreApplication::processEvents();
    const QString deeperText = view.cursorForPosition(QPoint(2, 2)).block().text();
    QVERIFY2(deeperText.contains(QStringLiteral("09 09 09 09")), qPrintable(deeperText));
    view.setMaxLines(30);
    QCoreApplication::processEvents();
    QCOMPARE(view.document()->blockCount(), 30);
    QCOMPARE(view.cursorForPosition(QPoint(2, 2)).block().text(), deeperText);
    QCOMPARE(bar->value(), 5);

    // Following the tail is unaffected.
    bar->setValue(bar->maximum());
    view.appendReceived(QByteArray(16, 'Q'));
    QCoreApplication::processEvents();
    QCOMPARE(bar->value(), bar->maximum());
}

QTEST_MAIN(Tst_terminalwidget)
#include "tst_terminalwidget.moc"
