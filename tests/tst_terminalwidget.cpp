// GUI test suite for TerminalWidget (src/terminal/TerminalWidget.h) and HexDumpView
// (src/ui/HexDumpView.h). Runs offscreen (QT_QPA_PLATFORM=offscreen) against the real widgets:
// key presses, mouse selection, clipboard, drag & drop, the screen model behind the view and the
// painting path. Contracts under test: the header doc comments, docs/TERMINAL_EMULATION.md §11 and
// docs/DESIGN.md §4.7.
#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
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
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
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

/// The terminal's context menu runs QMenu::exec(), a nested event loop. Timers keep firing inside
/// it, so this closes the popup as soon as it shows and records what it offered: a test can send
/// the real QContextMenuEvent without blocking. count() == 0 afterwards means no menu appeared.
class PopupCloser : public QObject
{
public:
    explicit PopupCloser(QObject* parent = nullptr)
        : QObject(parent)
    {
        m_timer.setInterval(10);
        connect(&m_timer, &QTimer::timeout, this, &PopupCloser::poll);
        m_timer.start();
    }

    int count() const { return m_count; }
    QStringList actionTexts() const { return m_texts; }   ///< of the last menu, separators skipped
    QList<bool> actionEnabled() const { return m_enabled; }

private:
    void poll()
    {
        QWidget* popup = QApplication::activePopupWidget();
        if (!popup) {
            // Belt and braces: a shown QMenu is a Qt::Popup top-level window whatever the platform.
            const QList<QWidget*> tops = QApplication::topLevelWidgets();
            for (QWidget* top : tops) {
                if (qobject_cast<QMenu*>(top) && top->isVisible()) {
                    popup = top;
                    break;
                }
            }
        }
        if (!popup) {
            return;
        }
        ++m_count;
        m_texts.clear();
        m_enabled.clear();
        if (auto* menu = qobject_cast<QMenu*>(popup)) {
            const QList<QAction*> actions = menu->actions();
            for (const QAction* entry : actions) {
                if (!entry->isSeparator()) {
                    m_texts.append(entry->text());
                    m_enabled.append(entry->isEnabled());
                }
            }
        }
        popup->close();   // QMenu::hideEvent() quits the exec() loop
    }

    QTimer m_timer;
    int m_count = 0;
    QStringList m_texts;
    QList<bool> m_enabled;
};

/// What the platform window generates for a right click (reason Mouse) or for the Menu key /
/// Shift+F10 (reason Keyboard). QTest::mouseClick() delivers the QMouseEvent alone, never this,
/// so the two halves of a right click are exercised separately. Returns whether it was accepted.
bool sendContextMenu(QWidget* viewport, QContextMenuEvent::Reason reason, const QPoint& pos,
                     Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QContextMenuEvent event(reason, pos, viewport->mapToGlobal(pos), mods);
    QApplication::sendEvent(viewport, &event);
    return event.isAccepted();
}

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

    // ---- Pause output while selecting (cmd.exe mark mode) ------------------------------
    void pauseDefaultsAndApi();
    void dragSelectionPausesAndEnterCopiesResumes();
    void escapeResumesWithoutCopying();
    void copyShortcutsCopyAndResume();
    void clickWithoutDragResumes();
    void everySelectionKindPauses();
    void keysAndPastesSwallowedWhilePaused();
    void viewScrollingWorksWhilePaused();
    void pauseFeatureOff();
    void pauseBufferOverflowResumes();
    void clearAndResetFlushPendingAfterwards();
    void resumeOutputKeepsSelection();
    void newSelectionWhilePausedReplacesOld();
    void dsrReplyDeferredWhilePaused();
    void pauseBadgeIsPainted();
    void pauseBadgeRepaintIsCoalesced();
    void pauseSurvivesResize();
    void pauseBufferLimitBoundaries();
    void selectAllThenEnterCopiesEverything();
    void findWhilePausedReplacesSelection();
    void pauseBadgeInTinyViewportAndFeatureOff();
    void destroyedWhilePaused();

    // ---- cmd.exe right click: paste / copy selection, Shift+right click = menu ----------
    void rightClickDefaultsAndApi();
    void rightClickPastesClipboard();
    void rightClickCopiesSelection();
    void shiftRightClickOpensMenuNotPaste();
    void rightClickSettingOff();
    void rightClickWhileInputDisabled();
    void rightClickNeverTouchesLeftSelection();
    void rightClickAfterLostLeftRelease();

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
    void clearAllWipesScreenAndScrollback();
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
    void hexHiddenViewDefersRendering();
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
    // Mark mode is covered by its own tests: with it on, Ctrl+Insert would also end the selection.
    term->setPauseWhileSelecting(false);
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
    // The legacy copy semantics (the selection survives Ctrl+Shift+C) apply while the display is
    // not paused; copyShortcutsCopyAndResume() covers the mark-mode variant.
    m_term->setPauseWhileSelecting(false);
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
    // With mark mode on the output would simply queue up; this test is about the absolute anchors
    // when the screen keeps moving under a selection (feature off, or after an overflow resume).
    m_term->setPauseWhileSelecting(false);
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
// Pause output while selecting (cmd.exe QuickEdit / mark mode; header "Pause output while
// selecting", DESIGN.md 4.7, TERMINAL_EMULATION.md §11)
// -------------------------------------------------------------------------------------------

void Tst_terminalwidget::pauseDefaultsAndApi()
{
    QVERIFY(m_term->pauseWhileSelecting());   // on by default
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QCOMPARE(m_term->pauseBufferLimit(), qint64(64) * 1024 * 1024);

    m_term->setPauseBufferLimit(100);
    QCOMPARE(m_term->pauseBufferLimit(), qint64(100));
    m_term->setPauseBufferLimit(-5);
    QCOMPARE(m_term->pauseBufferLimit(), qint64(0));   // clamped, never negative

    // resumeOutput() is a public slot (SessionWidget / MainWindow may wire it) and a no-op when
    // nothing is paused: no transition signal.
    QVERIFY(TerminalWidget::staticMetaObject.indexOfSlot("resumeOutput()") >= 0);
    QSignalSpy pausedSpy(m_term, &TerminalWidget::outputPausedChanged);
    m_term->resumeOutput();
    QCOMPARE(pausedSpy.count(), qsizetype(0));
    QVERIFY(!m_term->isOutputPaused());
}

void Tst_terminalwidget::dragSelectionPausesAndEnterCopiesResumes()
{
    m_term->feedData("hello world\r\n");
    const TerminalScreen* screen = m_term->screen();
    QSignalSpy pausedSpy(m_term, &TerminalWidget::outputPausedChanged);
    QSignalSpy sendSpy(m_term, &TerminalWidget::sendData);

    // Press, drag to another cell, release: the selection becomes non-empty -> paused.
    dragSelect(0, 0, 0, 4);
    QCOMPARE(m_term->selectedText(), QStringLiteral("hello"));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(pausedSpy.count(), qsizetype(1));
    QVERIFY(pausedSpy.at(0).at(0).toBool());

    // Incoming bytes are queued: screen, cursor and selection do not move.
    m_term->feedData("NEW LINE\r\n");
    QVERIFY(screen->lineText(1).isEmpty());
    QCOMPARE(screen->cursor().row, 1);
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(10));
    QCOMPARE(m_term->selectedText(), QStringLiteral("hello"));
    QCOMPARE(pausedSpy.count(), qsizetype(1));   // emitted once per transition, not per chunk

    // Enter: clipboard, selection cleared, display resumes with the queued output.
    QTest::keyClick(m_term, Qt::Key_Return);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello"));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QCOMPARE(screen->lineText(1), QStringLiteral("NEW LINE"));
    QCOMPARE(screen->cursor().row, 2);
    QCOMPARE(pausedSpy.count(), qsizetype(2));
    QVERIFY(!pausedSpy.at(1).at(0).toBool());
    QCOMPARE(sendSpy.count(), qsizetype(0));   // the Enter never reached the device

    // Keypad Enter behaves the same.
    dragSelect(0, 6, 0, 10);
    QCOMPARE(m_term->selectedText(), QStringLiteral("world"));
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("more\r\n");
    QVERIFY(screen->lineText(2).isEmpty());
    QTest::keyClick(m_term, Qt::Key_Enter, Qt::KeypadModifier);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("world"));
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(screen->lineText(2), QStringLiteral("more"));
    QCOMPARE(sendSpy.count(), qsizetype(0));
    QCOMPARE(pausedSpy.count(), qsizetype(4));
}

void Tst_terminalwidget::escapeResumesWithoutCopying()
{
    m_term->feedData("keep\r\n");
    QApplication::clipboard()->setText(QStringLiteral("untouched"));
    QSignalSpy sendSpy(m_term, &TerminalWidget::sendData);

    dragSelect(0, 0, 0, 3);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("later\r\n");
    QVERIFY(m_term->screen()->lineText(1).isEmpty());

    QTest::keyClick(m_term, Qt::Key_Escape);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("untouched"));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QCOMPARE(m_term->screen()->lineText(1), QStringLiteral("later"));
    QCOMPARE(sendSpy.count(), qsizetype(0));   // no ESC byte either
}

void Tst_terminalwidget::copyShortcutsCopyAndResume()
{
    m_term->feedData("hello world\r\n");
    const TerminalScreen* screen = m_term->screen();
    QSignalSpy sendSpy(m_term, &TerminalWidget::sendData);

    // Ctrl+Shift+C
    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("one\r\n");
    QTest::keyClick(m_term, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello"));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(screen->lineText(1), QStringLiteral("one"));

    // Ctrl+C with a selection copies instead of sending ETX, and resumes.
    dragSelect(1, 0, 1, 2);
    QCOMPARE(m_term->selectedText(), QStringLiteral("one"));
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("two\r\n");
    QTest::keyClick(m_term, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(sendSpy.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("one"));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(screen->lineText(2), QStringLiteral("two"));

    // Ctrl+Insert
    dragSelect(2, 0, 2, 2);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("three\r\n");
    QTest::keyClick(m_term, Qt::Key_Insert, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("two"));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(screen->lineText(3), QStringLiteral("three"));

    // copySelection() itself (Edit > Copy, the context menu's Copy) finishes the mark mode too.
    dragSelect(3, 0, 3, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("four\r\n");
    m_term->copySelection();
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("three"));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(screen->lineText(4), QStringLiteral("four"));
    QCOMPARE(sendSpy.count(), qsizetype(0));

    // Not paused (feature off): copying keeps the selection, as it always did.
    m_term->setPauseWhileSelecting(false);
    dragSelect(4, 0, 4, 3);
    QVERIFY(!m_term->isOutputPaused());
    m_term->copySelection();
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("four"));
    QVERIFY(m_term->hasSelection());
    QTest::keyClick(m_term, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(m_term->hasSelection());
    QTest::keyClick(m_term, Qt::Key_Insert, Qt::ControlModifier);
    QVERIFY(m_term->hasSelection());
}

void Tst_terminalwidget::clickWithoutDragResumes()
{
    m_term->feedData("click\r\n");
    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("x\r\n");
    QVERIFY(m_term->screen()->lineText(1).isEmpty());

    QTest::mouseClick(m_term->viewport(), Qt::LeftButton, Qt::NoModifier, cellCenter(5, 5));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->screen()->lineText(1), QStringLiteral("x"));

    // clearSelection() from any caller resumes as well.
    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("y\r\n");
    m_term->clearSelection();
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->screen()->lineText(2), QStringLiteral("y"));
}

void Tst_terminalwidget::everySelectionKindPauses()
{
    m_term->feedData("foo bar-baz qux\r\nsecond line\r\n");
    QWidget* vp = m_term->viewport();
    // One byte queued per kind; Esc flushes it onto the cursor line.
    auto pausedHolds = [this]() {
        const qint64 before = m_term->pendingPausedBytes();
        m_term->feedData("z");
        return m_term->isOutputPaused() && m_term->pendingPausedBytes() == before + 1;
    };

    // Double-click: word.
    QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 5));
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 5));
    QCOMPARE(m_term->selectedText(), QStringLiteral("bar-baz"));
    QVERIFY(pausedHolds());
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());

    // Triple-click: line (Press Release Press DblClick Release Press Release).
    const QPoint p = cellCenter(0, 1);
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, p);
    QCOMPARE(m_term->selectedText(), QStringLiteral("foo bar-baz qux"));
    QVERIFY(pausedHolds());
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());

    // selectAll()
    m_term->selectAll();
    QVERIFY(m_term->hasSelection());
    QVERIFY(pausedHolds());
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());

    // findNext() / findPrevious() select their match.
    QVERIFY(m_term->findNext(QStringLiteral("second")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("second"));
    QVERIFY(pausedHolds());
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());
    QVERIFY(m_term->findPrevious(QStringLiteral("foo")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("foo"));
    QVERIFY(pausedHolds());
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());

    // Shift+click extending a selection (made with the feature off, so the extension itself is
    // the first freeze; enabling the feature does not freeze an existing selection retroactively).
    m_term->setPauseWhileSelecting(false);
    dragSelect(1, 0, 1, 2);
    QCOMPARE(m_term->selectedText(), QStringLiteral("sec"));
    QVERIFY(!m_term->isOutputPaused());
    m_term->setPauseWhileSelecting(true);
    QVERIFY(!m_term->isOutputPaused());
    QTest::mousePress(vp, Qt::LeftButton, Qt::ShiftModifier, cellCenter(1, 10));
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::ShiftModifier, cellCenter(1, 10));
    QCOMPARE(m_term->selectedText(), QStringLiteral("second line"));
    QVERIFY(pausedHolds());
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());

    // Every flushed byte landed on the cursor line, in order.
    QCOMPARE(m_term->screen()->lineText(2), QStringLiteral("zzzzzz"));
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
}

void Tst_terminalwidget::keysAndPastesSwallowedWhilePaused()
{
    m_term->feedData("text\r\n");
    m_term->setLocalEcho(true);
    dragSelect(0, 0, 0, 3);
    QVERIFY(m_term->isOutputPaused());
    QSignalSpy send(m_term, &TerminalWidget::sendData);

    // Ordinary keys: nothing is sent, nothing is echoed, the focus stays (Tab included).
    QTest::keyClicks(m_term, QStringLiteral("abc"));
    QTest::keyClick(m_term, Qt::Key_Tab);
    QCOMPARE(QApplication::focusWidget(), m_term);
    QTest::keyClick(m_term, Qt::Key_F1);
    QTest::keyClick(m_term, Qt::Key_Up);
    QTest::keyClick(m_term, Qt::Key_Home);
    QTest::keyClick(m_term, Qt::Key_Backspace);
    QTest::keyClick(m_term, Qt::Key_Delete);
    QTest::keyClick(m_term, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(m_term, Qt::Key_Space, Qt::ControlModifier);
    QTest::keyClick(m_term, Qt::Key_X, Qt::AltModifier);
    QTest::keyClick(m_term, Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);   // no action owns it
    QCOMPARE(send.count(), qsizetype(0));
    QVERIFY(m_term->screen()->lineText(1).isEmpty());   // no local echo
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));  // ... not even a queued one
    QVERIFY(m_term->isOutputPaused());

    // IME commit
    QInputMethodEvent ime;
    ime.setCommitString(QStringLiteral("好"));
    QApplication::sendEvent(m_term, &ime);
    QCOMPARE(send.count(), qsizetype(0));

    // Pastes: keyboard, middle click, programmatic and drop.
    QApplication::clipboard()->setText(QStringLiteral("paste"));
    QTest::keyClick(m_term, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::keyClick(m_term, Qt::Key_Insert, Qt::ShiftModifier);
    QTest::mouseClick(m_term->viewport(), Qt::MiddleButton, Qt::NoModifier, cellCenter(3, 3));
    m_term->paste();
    m_term->pasteText(QStringLiteral("y"));
    QMimeData mime;
    mime.setText(QStringLiteral("dropped"));
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_term->viewport(), &enter);
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_term->viewport(), &drop);
    QCOMPARE(send.count(), qsizetype(0));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->selectedText(), QStringLiteral("text"));

    // Application shortcuts still pass through to the window's actions.
    QAction* hexView = addWindowAction(m_term, Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
    QSignalSpy hexSpy(hexView, &QAction::triggered);
    QTest::keyClick(m_term, Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(hexSpy.count(), qsizetype(1));
    QAction* refresh = addWindowAction(m_term, Qt::Key_F5);
    QSignalSpy refreshSpy(refresh, &QAction::triggered);
    QTest::keyClick(m_term, Qt::Key_F5);
    QCOMPARE(refreshSpy.count(), qsizetype(1));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(send.count(), qsizetype(0));

    // Esc ends the mark mode; typing reaches the device (and the local echo) again.
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());
    QTest::keyClicks(m_term, QStringLiteral("ok"));
    QCOMPARE(sentBytes(send), QByteArray("ok"));
    QCOMPARE(m_term->screen()->lineText(1), QStringLiteral("ok"));
}

void Tst_terminalwidget::viewScrollingWorksWhilePaused()
{
    m_term->feedData(numberedLines(200));
    QScrollBar* bar = m_term->verticalScrollBar();
    const int max = bar->maximum();
    const int rows = m_term->visibleRows();
    QVERIFY(max > 3 * rows);

    dragSelect(5, 0, 5, 3);
    QCOMPARE(m_term->selectedText(), QStringLiteral("line"));
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData(numberedLines(10, "more "));
    QCOMPARE(bar->maximum(), max);   // nothing parsed

    // Wheel, Shift+PgUp/PgDn and the scrollbar keep working; the selection stays put.
    sendWheel(120, Qt::NoModifier);
    QCOMPARE(bar->value(), max - 3);
    QVERIFY(m_term->isOutputPaused());
    QTest::keyClick(m_term, Qt::Key_PageUp, Qt::ShiftModifier);
    QCOMPARE(bar->value(), max - 3 - rows);
    QTest::keyClick(m_term, Qt::Key_PageDown, Qt::ShiftModifier);
    QCOMPARE(bar->value(), max - 3);
    bar->setValue(0);
    QCOMPARE(bar->value(), 0);
    m_term->scrollLines(7);
    QCOMPARE(bar->value(), 7);
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->selectedText(), QStringLiteral("line"));
    QCOMPARE(m_term->pendingPausedBytes(), qint64(numberedLines(10, "more ").size()));

    // Resume while following: the view follows the flushed output.
    m_term->scrollToBottom();
    QVERIFY(m_term->isAtBottom());
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(bar->maximum(), max + 10);
    QVERIFY(m_term->isAtBottom());

    // Resume while scrolled up: the frozen view stays where it is.
    dragSelect(3, 0, 3, 3);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData(numberedLines(5, "tail "));
    m_term->scrollLines(-10);
    const int frozen = bar->value();
    QVERIFY(!m_term->isAtBottom());
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(bar->maximum(), max + 15);
    QCOMPARE(bar->value(), frozen);
    QVERIFY(!m_term->isAtBottom());

    // Zoom is a view operation too (Ctrl+wheel and the keys stay usable while paused).
    m_term->scrollToBottom();
    dragSelect(2, 0, 2, 3);
    QVERIFY(m_term->isOutputPaused());
    const int pointSize = m_term->terminalFont().pointSize();
    sendWheel(120, Qt::ControlModifier);
    QCOMPARE(m_term->terminalFont().pointSize(), pointSize + 1);
    QVERIFY(m_term->isOutputPaused());
    QTest::keyClick(m_term, Qt::Key_Minus, Qt::ControlModifier);
    QCOMPARE(m_term->terminalFont().pointSize(), pointSize);
    QVERIFY(m_term->isOutputPaused());
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());
}

void Tst_terminalwidget::pauseFeatureOff()
{
    m_term->setPauseWhileSelecting(false);
    QVERIFY(!m_term->pauseWhileSelecting());
    const TerminalScreen* screen = m_term->screen();
    QSignalSpy pausedSpy(m_term, &TerminalWidget::outputPausedChanged);
    QSignalSpy sendSpy(m_term, &TerminalWidget::sendData);

    m_term->feedData("hello\r\n");
    dragSelect(0, 0, 0, 4);
    QVERIFY(!m_term->isOutputPaused());
    m_term->feedData("world\r\n");
    QCOMPARE(screen->lineText(1), QStringLiteral("world"));   // rendered immediately
    QVERIFY(m_term->hasSelection());
    QCOMPARE(m_term->selectedText(), QStringLiteral("hello"));
    QCOMPARE(pausedSpy.count(), qsizetype(0));
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));

    // Enter is a plain key again: sent to the device, the selection is untouched.
    QTest::keyClick(m_term, Qt::Key_Return);
    QCOMPARE(sentBytes(sendSpy), QByteArray("\r"));
    QVERIFY(m_term->hasSelection());
    QVERIFY(QApplication::clipboard()->text().isEmpty());

    // On again: the existing selection does not freeze the display; the next one does.
    m_term->setPauseWhileSelecting(true);
    QVERIFY(!m_term->isOutputPaused());
    m_term->feedData("x\r\n");
    QCOMPARE(screen->lineText(2), QStringLiteral("x"));
    dragSelect(1, 0, 1, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("y\r\n");
    QVERIFY(screen->lineText(3).isEmpty());

    // Off while paused: resumes and keeps the selection.
    m_term->setPauseWhileSelecting(false);
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(screen->lineText(3), QStringLiteral("y"));
    QVERIFY(m_term->hasSelection());
    QCOMPARE(m_term->selectedText(), QStringLiteral("world"));
    QCOMPARE(pausedSpy.count(), qsizetype(2));
    QVERIFY(!pausedSpy.last().at(0).toBool());
}

void Tst_terminalwidget::pauseBufferOverflowResumes()
{
    m_term->setPauseBufferLimit(100);
    m_term->feedData("hello\r\n");
    const TerminalScreen* screen = m_term->screen();
    QSignalSpy overflow(m_term, &TerminalWidget::pauseBufferOverflow);
    QSignalSpy pausedSpy(m_term, &TerminalWidget::outputPausedChanged);

    QByteArray first;
    for (int i = 0; i < 6; ++i) {
        first += "abcdefgh\r\n";   // 60 bytes
    }
    QByteArray second;
    for (int i = 0; i < 9; ++i) {
        second += "ijklmnop\r\n";   // 90 bytes
    }

    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData(first);
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(60));
    QCOMPARE(overflow.count(), qsizetype(0));

    // 60 + 90 > 100: everything is flushed, the display flows again, the selection survives.
    m_term->feedData(second);
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QCOMPARE(overflow.count(), qsizetype(1));
    QCOMPARE(overflow.at(0).at(0).toLongLong(), qint64(150));
    QCOMPARE(screen->lineText(1), QStringLiteral("abcdefgh"));
    QCOMPARE(screen->lineText(6), QStringLiteral("abcdefgh"));
    QCOMPARE(screen->lineText(7), QStringLiteral("ijklmnop"));
    QCOMPARE(screen->lineText(15), QStringLiteral("ijklmnop"));
    QCOMPARE(screen->cursor().row, 16);
    QVERIFY(m_term->hasSelection());
    QCOMPARE(m_term->selectedText(), QStringLiteral("hello"));
    QCOMPARE(pausedSpy.count(), qsizetype(2));
    QVERIFY(pausedSpy.at(0).at(0).toBool());
    QVERIFY(!pausedSpy.at(1).at(0).toBool());

    // Not paused any more: output renders immediately, the kept selection is still copyable.
    m_term->feedData("after\r\n");
    QCOMPARE(screen->lineText(16), QStringLiteral("after"));
    QTest::keyClick(m_term, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello"));

    // A single chunk larger than the limit overflows at once.
    dragSelect(1, 0, 1, 7);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData(first + second);
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(overflow.count(), qsizetype(2));
    QCOMPARE(overflow.at(1).at(0).toLongLong(), qint64(150));
    QCOMPARE(screen->lineText(17), QStringLiteral("abcdefgh"));
    QCOMPARE(screen->lineText(31), QStringLiteral("ijklmnop"));
    QCOMPARE(m_term->selectedText(), QStringLiteral("abcdefgh"));
}

void Tst_terminalwidget::clearAndResetFlushPendingAfterwards()
{
    TerminalScreen* screen = m_term->screen();

    // clearScreen(): selection dropped, screen pushed into the scrollback, *then* the queued
    // output continues on the cleared screen.
    m_term->feedData("old\r\n");
    dragSelect(0, 0, 0, 2);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("new\r\n");
    QCOMPARE(m_term->pendingPausedBytes(), qint64(5));
    m_term->clearScreen();
    QVERIFY(!m_term->isOutputPaused());
    QVERIFY(!m_term->hasSelection());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QVERIFY(screen->scrollbackSize() >= 1);
    QCOMPARE(screen->scrollbackLine(0).text(), QStringLiteral("old"));
    QCOMPARE(screen->line(0).text(), QStringLiteral("new"));
    QCOMPARE(screen->cursor().row, 1);

    // resetTerminal(): everything gone, the queued output starts on the blank screen.
    dragSelect(0, 0, 0, 2);
    QCOMPARE(m_term->selectedText(), QStringLiteral("new"));
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("after\r\n");
    m_term->resetTerminal();
    QVERIFY(!m_term->isOutputPaused());
    QVERIFY(!m_term->hasSelection());
    QCOMPARE(screen->scrollbackSize(), 0);
    QCOMPARE(screen->line(0).text(), QStringLiteral("after"));
    QCOMPARE(screen->cursor().row, 1);

    // clearScrollback(): the history goes, the visible screen and the queued output stay.
    m_term->feedData(numberedLines(m_term->visibleRows() + 5));
    QVERIFY(screen->scrollbackSize() > 0);
    dragSelect(0, 0, 0, 3);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("tail\r\n");
    m_term->clearScrollback();
    QVERIFY(!m_term->isOutputPaused());
    QVERIFY(!m_term->hasSelection());
    QVERIFY(screen->scrollbackSize() <= 1);   // "tail" may have scrolled one line out
    QCOMPARE(screen->lineText(screen->scrollbackSize() + screen->cursor().row - 1), QStringLiteral("tail"));
}

void Tst_terminalwidget::resumeOutputKeepsSelection()
{
    m_term->feedData("abc def\r\n");
    const TerminalScreen* screen = m_term->screen();
    dragSelect(0, 0, 0, 2);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("ghi\r\n");

    m_term->resumeOutput();
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QVERIFY(m_term->hasSelection());
    QCOMPARE(m_term->selectedText(), QStringLiteral("abc"));
    QCOMPARE(screen->lineText(1), QStringLiteral("ghi"));

    // No new selection: the display keeps flowing ...
    m_term->feedData("jkl\r\n");
    QCOMPARE(screen->lineText(2), QStringLiteral("jkl"));
    QVERIFY(!m_term->isOutputPaused());
    // ... the kept selection copies with the legacy semantics (not in mark mode: it survives) ...
    QTest::keyClick(m_term, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("abc"));
    QVERIFY(m_term->hasSelection());
    // ... and a fresh selection pauses again.
    dragSelect(1, 0, 1, 2);
    QVERIFY(m_term->isOutputPaused());
}

void Tst_terminalwidget::newSelectionWhilePausedReplacesOld()
{
    m_term->feedData("hello world\r\n");
    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("queued\r\n");
    QCOMPARE(m_term->pendingPausedBytes(), qint64(8));
    QSignalSpy pausedSpy(m_term, &TerminalWidget::outputPausedChanged);

    dragSelect(0, 6, 0, 10);
    QCOMPARE(m_term->selectedText(), QStringLiteral("world"));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(8));
    QCOMPARE(pausedSpy.count(), qsizetype(0));   // no transition: still paused

    QWidget* vp = m_term->viewport();
    QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 1));
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 1));
    QCOMPARE(m_term->selectedText(), QStringLiteral("hello"));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(8));
    QCOMPARE(pausedSpy.count(), qsizetype(0));

    QTest::keyClick(m_term, Qt::Key_Return);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello"));
    QCOMPARE(m_term->screen()->lineText(1), QStringLiteral("queued"));
    QCOMPARE(pausedSpy.count(), qsizetype(1));
}

void Tst_terminalwidget::dsrReplyDeferredWhilePaused()
{
    // Replies are produced when the bytes are parsed: queued with the query, sent on resume.
    m_term->feedData("xy\r\n");
    dragSelect(0, 0, 0, 1);
    QVERIFY(m_term->isOutputPaused());
    QSignalSpy send(m_term, &TerminalWidget::sendData);
    m_term->feedData(esc("[6n"));
    QCOMPARE(send.count(), qsizetype(0));
    QCOMPARE(m_term->pendingPausedBytes(), qint64(4));

    QTest::keyClick(m_term, Qt::Key_Escape);
    QCOMPARE(send.count(), qsizetype(1));
    QCOMPARE(sentBytes(send), esc("[2;1R"));
}

void Tst_terminalwidget::pauseBadgeIsPainted()
{
    const QColor background = m_term->colorPalette().background;
    m_term->feedData("badge test\r\n" + esc("[?25l"));   // no cursor: the corner is pure background
    QWidget* vp = m_term->viewport();
    // Inside the pill (right-aligned, 6 px margin) and the background just above it.
    const QRect corner(vp->width() - 60, 4, 50, 14);
    const QImage before = vp->grab().toImage();
    QVERIFY(isUniform(before.copy(corner), background));

    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->isOutputPaused());
    const QImage paused = vp->grab().toImage();
    QVERIFY(!isUniform(paused.copy(corner), background));
    QVERIFY(paused.copy(corner) != before.copy(corner));
    // The badge lives in the paint path only: not in the model, not in the copied text.
    QCOMPARE(m_term->screen()->lineText(0), QStringLiteral("badge test"));
    QCOMPARE(m_term->selectedText(), QStringLiteral("badge"));

    // The pending size is part of the badge: once the (coalesced) repaint ran, the pixels differ.
    m_term->feedData(QByteArray(1500, 'q'));
    QTest::qWait(150);
    const QImage grown = vp->grab().toImage();
    QVERIFY(!isUniform(grown.copy(corner), background));
    QVERIFY(grown != paused);
    QCOMPARE(m_term->screen()->lineText(0), QStringLiteral("badge test"));

    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());
    const QImage after = vp->grab().toImage();
    QVERIFY(isUniform(after.copy(corner), background));
}

void Tst_terminalwidget::pauseBadgeRepaintIsCoalesced()
{
    m_term->feedData("coalesce\r\n");
    dragSelect(0, 0, 0, 3);
    QVERIFY(m_term->isOutputPaused());
    QTest::qWait(150);   // the pause transition's own repaint

    PaintCounter counter;
    QWidget* vp = m_term->viewport();
    vp->installEventFilter(&counter);
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 100; ++i) {
        m_term->feedData("chunk\r\n");
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
    // One badge repaint per 100 ms window, however many chunks arrived in it. Wait for that
    // repaint (generously: a loaded machine may delay the coarse timer and the paint) and then
    // through one more window, and bound the count by the wall time rather than by the chunks.
    QTRY_VERIFY_WITH_TIMEOUT(counter.paints >= 1, 5000);
    QTest::qWait(150);
    const qint64 elapsed = t.elapsed();
    vp->removeEventFilter(&counter);

    qInfo("pauseBadgeRepaintIsCoalesced: %d paints for 100 chunks in %lld ms", counter.paints,
          static_cast<long long>(elapsed));
    QVERIFY2(counter.paints <= static_cast<int>(elapsed / 100) + 2,
             qPrintable(QStringLiteral("%1 paints in %2 ms").arg(counter.paints).arg(elapsed)));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(700));
    QVERIFY(m_term->screen()->lineText(1).isEmpty());
}

void Tst_terminalwidget::pauseSurvivesResize()
{
    // A resize while paused (fewer rows: lines above the cursor move into the scrollback; more
    // columns) must neither parse the queue nor move the selection off its text, and the flush
    // afterwards continues on the resized grid right where the frozen cursor was.
    const int rows = m_term->visibleRows();
    const int cols = m_term->columns();
    QVERIFY(rows > 8);
    m_term->feedData(numberedLines(rows - 2));   // the cursor sits on row rows-2, nothing scrolled yet
    const TerminalScreen* screen = m_term->screen();
    QCOMPARE(screen->scrollbackSize(), 0);
    QCOMPARE(screen->cursor().row, rows - 2);

    dragSelect(3, 0, 3, 5);
    QCOMPARE(m_term->selectedText(), QStringLiteral("line 3"));
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("after resize\r\n");
    QCOMPARE(m_term->pendingPausedBytes(), qint64(14));
    QSignalSpy pausedSpy(m_term, &TerminalWidget::outputPausedChanged);

    // Six rows fewer, ten columns more.
    m_term->resize(m_term->width() + 10 * m_cellW, m_term->height() - 6 * m_cellH);
    QTRY_COMPARE(m_term->visibleRows(), rows - 6);
    QCOMPARE(m_term->columns(), cols + 10);
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(pausedSpy.count(), qsizetype(0));
    QCOMPARE(m_term->pendingPausedBytes(), qint64(14));
    // Five lines moved above the cursor into the scrollback; absolute anchors keep the same text.
    QCOMPARE(screen->scrollbackSize(), 5);
    QCOMPARE(screen->cursor().row, rows - 7);
    QCOMPARE(m_term->selectedText(), QStringLiteral("line 3"));
    QVERIFY(screen->lineText(rows - 2).isEmpty());   // the frozen cursor line: nothing parsed

    // Rows back up by three: lines return from the scrollback, still paused, still "line 3".
    m_term->resize(m_term->width(), m_term->height() + 3 * m_cellH);
    QTRY_COMPARE(m_term->visibleRows(), rows - 3);
    QCOMPARE(screen->scrollbackSize(), 2);
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->selectedText(), QStringLiteral("line 3"));
    QCOMPARE(m_term->pendingPausedBytes(), qint64(14));

    // Enter: the selection is copied, the queue lands on the (old) cursor line of the new grid.
    QTest::keyClick(m_term, Qt::Key_Return);
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("line 3"));
    QCOMPARE(screen->lineText(rows - 2), QStringLiteral("after resize"));
    QCOMPARE(screen->lineText(3), QStringLiteral("line 3"));
    QCOMPARE(screen->totalLines(), rows - 3 + screen->scrollbackSize());
    QCOMPARE(screen->cursor().col, 0);
    QCOMPARE(screen->scrollbackSize() + screen->cursor().row, rows - 1);   // one line below the flushed text
}

void Tst_terminalwidget::pauseBufferLimitBoundaries()
{
    m_term->setPauseBufferLimit(100);
    m_term->feedData("hello\r\n");
    const TerminalScreen* screen = m_term->screen();
    QSignalSpy overflow(m_term, &TerminalWidget::pauseBufferOverflow);
    // The rows the flushed runs land on, joined (a run may wrap over several rows).
    auto rowsText = [screen](int from, int to) {
        QString text;
        for (int i = from; i <= to; ++i) {
            text += screen->lineText(i);
        }
        return text;
    };

    // Exactly the limit still fits ...
    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData(QByteArray(100, 'a'));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(100));
    QCOMPARE(overflow.count(), qsizetype(0));
    QVERIFY(screen->lineText(1).isEmpty());
    // ... one byte more overflows: all 101 bytes are parsed in order, the selection survives.
    m_term->feedData("b");
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QCOMPARE(overflow.count(), qsizetype(1));
    QCOMPARE(overflow.at(0).at(0).toLongLong(), qint64(101));
    const int wrappedRows = (101 + m_term->columns() - 1) / m_term->columns();
    QCOMPARE(rowsText(1, wrappedRows), QString(100, QLatin1Char('a')) + QLatin1Char('b'));
    QCOMPARE(m_term->selectedText(), QStringLiteral("hello"));
    m_term->feedData("\r\n");
    const int next = 1 + wrappedRows;   // first free row

    // A single chunk of exactly the limit does not overflow; one of limit + 1 does at once.
    m_term->clearSelection();
    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData(QByteArray(100, 'c'));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(100));
    QCOMPARE(overflow.count(), qsizetype(1));
    QTest::keyClick(m_term, Qt::Key_Escape);
    QCOMPARE(rowsText(next, next + wrappedRows), QString(100, QLatin1Char('c')));
    m_term->feedData("\r\n");
    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData(QByteArray(101, 'd'));
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(overflow.count(), qsizetype(2));
    QCOMPARE(overflow.at(1).at(0).toLongLong(), qint64(101));
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QVERIFY(m_term->hasSelection());
    m_term->feedData("\r\n");

    // Limit 0: the very first byte overflows (the display never freezes, nothing is lost).
    m_term->setPauseBufferLimit(0);
    m_term->clearSelection();
    dragSelect(0, 0, 0, 4);
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("e");
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(overflow.count(), qsizetype(3));
    QCOMPARE(overflow.at(2).at(0).toLongLong(), qint64(1));
    QVERIFY(screen->lineText(screen->scrollbackSize() + screen->cursor().row).endsWith(QLatin1Char('e')));
    QCOMPARE(m_term->selectedText(), QStringLiteral("hello"));
}

void Tst_terminalwidget::selectAllThenEnterCopiesEverything()
{
    m_term->feedData("alpha\r\nbeta\r\ngamma\r\n");
    const TerminalScreen* screen = m_term->screen();
    QSignalSpy send(m_term, &TerminalWidget::sendData);

    // Edit > Select All (or the context menu) pauses like a mouse selection ...
    m_term->selectAll();
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->selectedText(), QStringLiteral("alpha\nbeta\ngamma"));
    m_term->feedData("delta\r\n");
    QVERIFY(screen->lineText(3).isEmpty());
    // ... bare Ctrl+A is swallowed in mark mode (it is 0x01 for the device otherwise) ...
    QTest::keyClick(m_term, Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(send.count(), qsizetype(0));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->selectedText(), QStringLiteral("alpha\nbeta\ngamma"));
    // ... and Enter copies the whole buffer, then lets the queued line through.
    QTest::keyClick(m_term, Qt::Key_Return);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("alpha\nbeta\ngamma"));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(screen->lineText(3), QStringLiteral("delta"));
    QCOMPARE(send.count(), qsizetype(0));

    // Select All again now includes the flushed line.
    m_term->selectAll();
    QVERIFY(m_term->isOutputPaused());
    QTest::keyClick(m_term, Qt::Key_Return);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("alpha\nbeta\ngamma\ndelta"));
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(send.count(), qsizetype(0));
}

void Tst_terminalwidget::findWhilePausedReplacesSelection()
{
    m_term->feedData("one two\r\nthree two\r\n");
    const TerminalScreen* screen = m_term->screen();
    dragSelect(0, 0, 0, 2);
    QCOMPARE(m_term->selectedText(), QStringLiteral("one"));
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("two again\r\n");
    QCOMPARE(m_term->pendingPausedBytes(), qint64(11));
    QSignalSpy pausedSpy(m_term, &TerminalWidget::outputPausedChanged);

    // Each match replaces the selection; the widget stays paused, the queue untouched.
    QVERIFY(m_term->findNext(QStringLiteral("two")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("two"));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(pausedSpy.count(), qsizetype(0));
    QVERIFY(m_term->findNext(QStringLiteral("two")));   // the second line's match
    QCOMPARE(m_term->selectedText(), QStringLiteral("two"));
    QVERIFY(m_term->isOutputPaused());
    // The queued "two again" is not on the screen yet, so it is not found: the search wraps to the
    // first match instead.
    QVERIFY(!m_term->findNext(QStringLiteral("again")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("two"));
    QVERIFY(m_term->findPrevious(QStringLiteral("one")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("one"));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(11));
    QCOMPARE(pausedSpy.count(), qsizetype(0));
    QVERIFY(screen->lineText(2).isEmpty());

    // Esc resumes; the flushed line is searchable and a find pauses again.
    QTest::keyClick(m_term, Qt::Key_Escape);
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(screen->lineText(2), QStringLiteral("two again"));
    QVERIFY(m_term->findNext(QStringLiteral("again")));
    QCOMPARE(m_term->selectedText(), QStringLiteral("again"));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(pausedSpy.count(), qsizetype(2));
}

void Tst_terminalwidget::pauseBadgeInTinyViewportAndFeatureOff()
{
    // A viewport narrower than the badge text: the pill is shrunk and its text elided (never
    // wider than the viewport, never a crash) and it still shows; turning the feature off from
    // Preferences while it is on screen removes it with the pause.
    const QColor background = m_term->colorPalette().background;
    m_term->feedData("ab\r\n" + esc("[?25l"));
    m_term->resize(60, 80);
    QTRY_VERIFY(m_term->viewport()->width() < 60);
    QVERIFY(m_term->viewport()->width() < 120);   // far narrower than "Output paused ..."
    QWidget* vp = m_term->viewport();
    // Row 1 is blank and unselected: inside the badge (top margin 6 px) but below the selection.
    const QRect probe(8, m_cellH + 1, qMax(1, vp->width() - 16), 3);
    QVERIFY(isUniform(vp->grab().toImage().copy(probe), background));

    m_term->selectAll();
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(m_term->selectedText(), QStringLiteral("ab"));
    m_term->feedData(QByteArray(200, '\r'));   // grows the queue without ever printing anything
    QTest::qWait(150);
    const QImage paused = vp->grab().toImage();   // paints the elided badge
    QVERIFY(!isUniform(paused.copy(probe), background));

    // The minimum grid (2 x 2 cells) with a viewport smaller than the badge's padding.
    m_term->resize(24, 24);
    QTest::qWait(20);
    QVERIFY(!vp->grab().isNull());
    QVERIFY(m_term->isOutputPaused());
    m_term->resize(60, 80);
    QTest::qWait(20);

    m_term->setPauseWhileSelecting(false);
    QVERIFY(!m_term->isOutputPaused());
    QVERIFY(m_term->hasSelection());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    const QImage resumed = vp->grab().toImage();
    QVERIFY(isUniform(resumed.copy(probe), background));
}

void Tst_terminalwidget::destroyedWhilePaused()
{
    // A tab closed while its terminal is paused: the queue and the armed badge timer go with
    // the widget, no flush, no signal, no crash - for a direct delete and for deleteLater().
    for (const bool viaDeleteLater : {false, true}) {
        auto* term = new TerminalWidget;
        term->resize(400, 240);
        term->show();
        QVERIFY(QTest::qWaitForWindowExposed(term));
        term->feedData("gone\r\n");
        term->selectAll();
        QVERIFY(term->isOutputPaused());
        term->feedData(QByteArray(5000, 'x'));   // pending bytes + the 100 ms badge timer running
        QCOMPARE(term->pendingPausedBytes(), qint64(5000));
        QSignalSpy pausedSpy(term, &TerminalWidget::outputPausedChanged);
        if (viaDeleteLater) {
            term->deleteLater();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        } else {
            delete term;
        }
        QTest::qWait(150);   // past the badge interval: nothing may fire into freed memory
        QCOMPARE(pausedSpy.count(), qsizetype(0));
    }
}

// -------------------------------------------------------------------------------------------
// cmd.exe right click (header "Right click", DESIGN.md 4.7, TERMINAL_EMULATION.md §11): a plain
// right click copies the selection or pastes the clipboard, the menu moves to Shift+right click.
// -------------------------------------------------------------------------------------------

void Tst_terminalwidget::rightClickDefaultsAndApi()
{
    QVERIFY(m_term->rightClickPastes());   // on by default (AppSettings::rightClickPastes())
    m_term->setRightClickPastes(false);
    QVERIFY(!m_term->rightClickPastes());
    m_term->setRightClickPastes(true);
    QVERIFY(m_term->rightClickPastes());
}

void Tst_terminalwidget::rightClickPastesClipboard()
{
    QWidget* vp = m_term->viewport();
    QSignalSpy send(m_term, &TerminalWidget::sendData);
    QSignalSpy selectionSpy(m_term, &TerminalWidget::selectionChanged);
    QApplication::clipboard()->setText(QStringLiteral("abc\n"));

    // No selection: the clipboard goes out through the paste() path (newline -> Enter bytes, CR).
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(2, 3));
    QCOMPARE(send.count(), qsizetype(1));
    QCOMPARE(sentBytes(send), QByteArray("abc\r"));
    QVERIFY(!m_term->hasSelection());
    QCOMPARE(selectionSpy.count(), qsizetype(0));   // a right press never starts a selection
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("abc\n"));   // and never writes the clipboard

    // Enter mode and bracketed paste are honoured exactly like Ctrl+Shift+V.
    send.clear();
    m_term->setEnterSends(LineEnding::Mode::LF);
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(0, 0));
    QCOMPARE(sentBytes(send), QByteArray("abc\n"));
    send.clear();
    m_term->setEnterSends(LineEnding::Mode::CR);
    m_term->feedData(esc("[?2004h"));
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(0, 0));
    QCOMPARE(sentBytes(send), esc("[200~") + "abc\r" + esc("[201~"));
    m_term->feedData(esc("[?2004l"));

    // A right-button drag selects nothing either; the press pasted once.
    send.clear();
    QTest::mousePress(vp, Qt::RightButton, Qt::NoModifier, cellCenter(1, 1));
    QTest::mouseMove(vp, cellCenter(3, 8));
    QTest::mouseRelease(vp, Qt::RightButton, Qt::NoModifier, cellCenter(3, 8));
    QVERIFY(!m_term->hasSelection());
    QCOMPARE(selectionSpy.count(), qsizetype(0));
    QCOMPARE(sentBytes(send), QByteArray("abc\r"));

    // The context-menu event the platform generates for that click is swallowed: no menu.
    {
        PopupCloser popups;
        QVERIFY(sendContextMenu(vp, QContextMenuEvent::Mouse, cellCenter(0, 0)));
        QTest::qWait(40);
        QCOMPARE(popups.count(), 0);
    }

    // An empty clipboard: nothing is sent.
    send.clear();
    QApplication::clipboard()->clear();
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(0, 0));
    QCOMPARE(send.count(), qsizetype(0));
}

void Tst_terminalwidget::rightClickCopiesSelection()
{
    m_term->feedData("hello world\r\n");
    QWidget* vp = m_term->viewport();
    const TerminalScreen* screen = m_term->screen();
    QSignalSpy send(m_term, &TerminalWidget::sendData);
    QApplication::clipboard()->setText(QStringLiteral("clipboard"));

    // Mark mode: the selection froze the display. The right click copies it, clears it and
    // resumes (the queued bytes are flushed) - exactly Enter - and pastes nothing.
    dragSelect(0, 0, 0, 4);
    QCOMPARE(m_term->selectedText(), QStringLiteral("hello"));
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("queued\r\n");
    QVERIFY(screen->lineText(1).isEmpty());
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(5, 5));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello"));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QCOMPARE(screen->lineText(1), QStringLiteral("queued"));
    QCOMPARE(send.count(), qsizetype(0));

    // The next right click (no selection any more) pastes what was just copied.
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(5, 5));
    QCOMPARE(sentBytes(send), QByteArray("hello"));

    // Pause feature off: a selection is still copied and finished, never pasted over.
    send.clear();
    m_term->setPauseWhileSelecting(false);
    dragSelect(0, 6, 0, 10);
    QCOMPARE(m_term->selectedText(), QStringLiteral("world"));
    QVERIFY(!m_term->isOutputPaused());
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(0, 0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("world"));
    QVERIFY(!m_term->hasSelection());
    QCOMPARE(send.count(), qsizetype(0));

    // A Select All selection is copied the same way (multi-line text, then resumes).
    m_term->setPauseWhileSelecting(true);
    m_term->selectAll();
    QVERIFY(m_term->isOutputPaused());
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(0, 0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello world\nqueued"));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(send.count(), qsizetype(0));
}

void Tst_terminalwidget::shiftRightClickOpensMenuNotPaste()
{
    m_term->feedData("menu please\r\n");
    QWidget* vp = m_term->viewport();
    QSignalSpy send(m_term, &TerminalWidget::sendData);
    QApplication::clipboard()->setText(QStringLiteral("clip"));
    dragSelect(0, 0, 0, 3);
    QCOMPARE(m_term->selectedText(), QStringLiteral("menu"));
    QVERIFY(m_term->isOutputPaused());

    // QTest delivers the QMouseEvent only (never a QContextMenuEvent), so the press is checked
    // on its own: with Shift it is not consumed by the paste / copy path.
    QTest::mouseClick(vp, Qt::RightButton, Qt::ShiftModifier, cellCenter(3, 3));
    QCOMPARE(send.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clip"));
    QCOMPARE(m_term->selectedText(), QStringLiteral("menu"));   // kept, still paused
    QVERIFY(m_term->isOutputPaused());

    // The context-menu event that follows such a click opens the real menu. QMenu::exec() runs a
    // nested event loop, so PopupCloser closes the popup as soon as it appears and records its
    // entries: the usual commands, and the hint line at the very end.
    {
        PopupCloser popups;
        QVERIFY(sendContextMenu(vp, QContextMenuEvent::Mouse, cellCenter(3, 3), Qt::ShiftModifier));
        QCOMPARE(popups.count(), 1);
        const QStringList texts = popups.actionTexts();
        QVERIFY2(texts.size() >= 8, qPrintable(texts.join(QStringLiteral(" | "))));
        QCOMPARE(texts.first(), QStringLiteral("&Copy"));
        QCOMPARE(texts.at(1), QStringLiteral("&Paste"));
        QVERIFY(texts.contains(QStringLiteral("Select &All")));
        QVERIFY(texts.contains(QStringLiteral("Clear &Screen (keep scrollback)")));
        QVERIFY(texts.contains(QStringLiteral("Clear Scroll&back")));
        QVERIFY(texts.contains(QStringLiteral("&Reset Terminal")));
        QVERIFY(texts.contains(QStringLiteral("&Find...")));
        QCOMPARE(texts.last(), QStringLiteral("Right click: paste / copy selection - Shift+right click: this menu"));
        QVERIFY(!popups.actionEnabled().last());   // a hint, not a command
        QVERIFY(popups.actionEnabled().first());   // Copy: there is a selection
        QVERIFY(!popups.actionEnabled().at(1));    // Paste: disabled while paused
    }
    // Opening the menu changed nothing.
    QCOMPARE(send.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clip"));
    QCOMPARE(m_term->selectedText(), QStringLiteral("menu"));
    QVERIFY(m_term->isOutputPaused());

    // The Menu key / Shift+F10 (reason Keyboard, no Shift needed) open it as well ...
    {
        PopupCloser popups;
        QVERIFY(sendContextMenu(vp, QContextMenuEvent::Keyboard, cellCenter(0, 0)));
        QCOMPARE(popups.count(), 1);
        QVERIFY(popups.actionTexts().contains(QStringLiteral("&Copy")));
        QCOMPARE(popups.actionTexts().last(),
                 QStringLiteral("Right click: paste / copy selection - Shift+right click: this menu"));
    }
    // ... while the plain mouse-triggered one is swallowed: no menu at all.
    {
        PopupCloser popups;
        QVERIFY(sendContextMenu(vp, QContextMenuEvent::Mouse, cellCenter(0, 0)));
        QTest::qWait(40);
        QCOMPARE(popups.count(), 0);
    }
    QCOMPARE(send.count(), qsizetype(0));
    QCOMPARE(m_term->selectedText(), QStringLiteral("menu"));
    QVERIFY(m_term->isOutputPaused());
}

void Tst_terminalwidget::rightClickSettingOff()
{
    m_term->setRightClickPastes(false);
    m_term->feedData("legacy\r\n");
    QWidget* vp = m_term->viewport();
    QSignalSpy send(m_term, &TerminalWidget::sendData);
    QApplication::clipboard()->setText(QStringLiteral("clip"));

    // Without a selection nothing is pasted ...
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(2, 2));
    QCOMPARE(send.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clip"));
    QVERIFY(!m_term->hasSelection());
    // ... with one nothing is copied and the selection stays (still paused).
    dragSelect(0, 0, 0, 5);
    QCOMPARE(m_term->selectedText(), QStringLiteral("legacy"));
    QVERIFY(m_term->isOutputPaused());
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(2, 2));
    QCOMPARE(send.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clip"));
    QCOMPARE(m_term->selectedText(), QStringLiteral("legacy"));
    QVERIFY(m_term->isOutputPaused());

    // The plain right click opens the context menu as before, without the hint line.
    PopupCloser popups;
    QVERIFY(sendContextMenu(vp, QContextMenuEvent::Mouse, cellCenter(2, 2)));
    QCOMPARE(popups.count(), 1);
    const QStringList texts = popups.actionTexts();
    QCOMPARE(texts.first(), QStringLiteral("&Copy"));
    QCOMPARE(texts.last(), QStringLiteral("&Find..."));
    QCOMPARE(send.count(), qsizetype(0));
    QCOMPARE(m_term->selectedText(), QStringLiteral("legacy"));
}

void Tst_terminalwidget::rightClickWhileInputDisabled()
{
    m_term->setInputEnabled(false);
    m_term->feedData("offline\r\n");
    QWidget* vp = m_term->viewport();
    QSignalSpy send(m_term, &TerminalWidget::sendData);
    QApplication::clipboard()->setText(QStringLiteral("clip"));

    // Disconnected: the paste is dropped (nothing may be sent), the clipboard is untouched ...
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(3, 3));
    QCOMPARE(send.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clip"));
    // ... but copying a selection needs no connection: copied, cleared, resumed.
    dragSelect(0, 0, 0, 6);
    QCOMPARE(m_term->selectedText(), QStringLiteral("offline"));
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("later\r\n");
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(3, 3));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("offline"));
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->screen()->lineText(1), QStringLiteral("later"));
    QCOMPARE(send.count(), qsizetype(0));
    // Still nothing to paste while disconnected, even with the copied text on the clipboard.
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, cellCenter(3, 3));
    QCOMPARE(send.count(), qsizetype(0));
}

void Tst_terminalwidget::rightClickNeverTouchesLeftSelection()
{
    // A right press while the left button is held (a chorded click during a drag) neither
    // finishes the drag nor pastes; the drag goes on and ends as usual.
    m_term->feedData("chord test\r\n");
    QWidget* vp = m_term->viewport();
    QSignalSpy send(m_term, &TerminalWidget::sendData);
    QApplication::clipboard()->setText(QStringLiteral("clip"));

    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 0));
    QTest::mouseMove(vp, cellCenter(0, 4));
    QCOMPARE(m_term->selectedText(), QStringLiteral("chord"));
    QVERIFY(m_term->isOutputPaused());
    QTest::mousePress(vp, Qt::RightButton, Qt::NoModifier, cellCenter(0, 4));
    QTest::mouseRelease(vp, Qt::RightButton, Qt::NoModifier, cellCenter(0, 4));
    QCOMPARE(send.count(), qsizetype(0));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clip"));
    QCOMPARE(m_term->selectedText(), QStringLiteral("chord"));
    QVERIFY(m_term->isOutputPaused());
    QTest::mouseMove(vp, cellCenter(0, 9));
    QCOMPARE(m_term->selectedText(), QStringLiteral("chord test"));
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 9));
    QCOMPARE(m_term->selectedText(), QStringLiteral("chord test"));
    QVERIFY(m_term->isOutputPaused());
    QCOMPARE(send.count(), qsizetype(0));
}

void Tst_terminalwidget::rightClickAfterLostLeftRelease()
{
    // A popup can swallow a drag's left release (the Shift+right-click menu opened mid-drag,
    // dismissed with Esc): the widget would still believe the drag is in progress. The next
    // right press reports no left button - that is authoritative: the stale drag ends, the
    // selection it made stays, and the right click acts on it instead of being ignored until a
    // left click drops the selection.
    m_term->feedData("lost release\r\n");
    QWidget* vp = m_term->viewport();
    QSignalSpy send(m_term, &TerminalWidget::sendData);
    QApplication::clipboard()->setText(QStringLiteral("clip"));
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 0));
    QTest::mouseMove(vp, cellCenter(0, 3));
    QCOMPARE(m_term->selectedText(), QStringLiteral("lost"));
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("queued\r\n");

    // No left release ever arrives; the right press carries only the right button.
    const QPoint pos = cellCenter(4, 4);
    QMouseEvent press(QEvent::MouseButtonPress, pos, vp->mapToGlobal(pos), Qt::RightButton, Qt::RightButton,
                      Qt::NoModifier);
    QApplication::sendEvent(vp, &press);
    QVERIFY(press.isAccepted());
    QMouseEvent release(QEvent::MouseButtonRelease, pos, vp->mapToGlobal(pos), Qt::RightButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(vp, &release);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("lost"));   // copied, not ignored
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QCOMPARE(m_term->screen()->lineText(1), QStringLiteral("queued"));
    QCOMPARE(send.count(), qsizetype(0));

    // The pointer moving afterwards extends nothing; a late left release is harmless.
    QTest::mouseMove(vp, cellCenter(0, 9));
    QVERIFY(!m_term->hasSelection());
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 9));
    QVERIFY(!m_term->hasSelection());
    QCOMPARE(send.count(), qsizetype(0));

    // A genuine chord (left really held) is still ignored and the drag goes on.
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(1, 0));
    QTest::mouseMove(vp, cellCenter(1, 5));
    QCOMPARE(m_term->selectedText(), QStringLiteral("queued"));
    QTest::mousePress(vp, Qt::RightButton, Qt::NoModifier, cellCenter(1, 5));
    QTest::mouseRelease(vp, Qt::RightButton, Qt::NoModifier, cellCenter(1, 5));
    QCOMPARE(m_term->selectedText(), QStringLiteral("queued"));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("lost"));
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, cellCenter(1, 5));
    QCOMPARE(m_term->selectedText(), QStringLiteral("queued"));
    QCOMPARE(send.count(), qsizetype(0));
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
    m_term->setPauseWhileSelecting(false);   // the selection must be shifted by real scrollback drops here
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

void Tst_terminalwidget::clearAllWipesScreenAndScrollback()
{
    // The toolbar's Clear (SessionWidget::clearTerminal()): screen and scrollback gone, cursor
    // home, attributes / modes / parser state untouched - resetTerminal() is the RIS. While
    // paused the clear applies first and the queue continues on the empty screen.
    m_term->feedData(numberedLines(m_term->visibleRows() + 40));
    m_term->feedData(esc("[1;31m") + esc("[?1h") + esc("[?2004h") + "red");
    TerminalScreen* screen = m_term->screen();
    QVERIFY(screen->scrollbackSize() > 0);
    const Terminal::Attributes attributes = screen->currentAttributes();
    QVERIFY(attributes != Terminal::Attributes());
    m_term->selectAll();
    QVERIFY(m_term->isOutputPaused());
    m_term->feedData("after\r\n");

    m_term->clearAll();
    QCOMPARE(screen->scrollbackSize(), 0);
    QCOMPARE(screen->totalLines(), screen->rows());
    QCOMPARE(screen->lineText(0), QStringLiteral("after"));   // the queue landed on the empty screen
    for (int r = 1; r < screen->rows(); ++r) {
        QVERIFY2(screen->line(r).text().isEmpty(), qPrintable(QStringLiteral("row %1 not blank").arg(r)));
    }
    QCOMPARE(screen->cursor().row, 1);
    QCOMPARE(screen->cursor().col, 0);
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    QCOMPARE(m_term->pendingPausedBytes(), qint64(0));
    QVERIFY(screen->currentAttributes() == attributes);     // bold red still active
    QVERIFY(m_term->parser()->cursorKeyApplicationMode());   // DECCKM kept
    QVERIFY(m_term->parser()->bracketedPasteMode());         // bracketed paste kept
    QCOMPARE(m_term->verticalScrollBar()->maximum(), 0);
    QVERIFY(m_term->isAtBottom());

    // Not paused: a plain wipe, cursor home, nothing in the scrollback, fully usable afterwards.
    m_term->feedData(numberedLines(m_term->visibleRows() + 5));
    QVERIFY(screen->scrollbackSize() > 0);
    m_term->clearAll();
    QCOMPARE(screen->scrollbackSize(), 0);
    QCOMPARE(screen->cursor().row, 0);
    QCOMPARE(screen->cursor().col, 0);
    for (int r = 0; r < screen->rows(); ++r) {
        QVERIFY(screen->line(r).text().isEmpty());
    }
    QCOMPARE(m_term->verticalScrollBar()->maximum(), 0);
    m_term->feedData("back");
    QCOMPARE(screen->lineText(0), QStringLiteral("back"));
    QVERIFY(screen->line(0).cells.at(0).attr == attributes);   // printed with the kept attributes

    // Alternate screen (top / vi / menuconfig): the visible grid is wiped and so is the primary
    // grid saved behind it - leaving the alternate screen brings back a blank primary, not
    // "back". Clear itself never leaves the alternate screen (the program owns it) and a second
    // Clear right away is harmless.
    m_term->feedData(esc("[?1049h") + esc("[H") + "inside top");   // ?1049 keeps the cursor: home it
    QVERIFY(screen->alternateScreenActive());
    QCOMPARE(screen->lineText(0), QStringLiteral("inside top"));
    m_term->selectAll();
    QVERIFY(m_term->isOutputPaused());
    m_term->clearAll();
    m_term->clearAll();
    QVERIFY(screen->alternateScreenActive());
    QVERIFY(!m_term->hasSelection());
    QVERIFY(!m_term->isOutputPaused());
    for (int r = 0; r < screen->rows(); ++r) {
        QVERIFY(screen->line(r).text().isEmpty());
    }
    QCOMPARE(screen->cursor().row, 0);
    QCOMPARE(screen->cursor().col, 0);
    QCOMPARE(m_term->verticalScrollBar()->maximum(), 0);
    m_term->feedData(esc("[?1049l"));
    QVERIFY(!screen->alternateScreenActive());
    QCOMPARE(screen->scrollbackSize(), 0);
    for (int r = 0; r < screen->rows(); ++r) {
        QVERIFY2(screen->line(r).text().isEmpty(), qPrintable(QStringLiteral("primary row %1 not blank").arg(r)));
    }
    m_term->feedData("\r\nprompt$ ");
    QVERIFY(screen->lineText(screen->cursor().row).startsWith(QStringLiteral("prompt$")));
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
    m_term->setPauseWhileSelecting(false);   // the cursor is moved onto a wide char *while* everything is selected
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
    // Baseline: the initial all-dirty paint has happened. The coalescing window is at least as
    // long as the previous paint took, so on a loaded machine a fixed 40 ms wait is not enough:
    // poll for the state instead (the checks below are the same).
    m_term->feedData("x");
    QTRY_VERIFY_WITH_TIMEOUT(!m_term->screen()->isDirty(), 2000);

    // A single-row change is painted partially and still consumes the dirty state.
    m_term->feedData("y");
    QVERIFY(m_term->screen()->isDirty());
    QVERIFY(!m_term->screen()->allDirty());
    QTRY_VERIFY_WITH_TIMEOUT(!m_term->screen()->isDirty(), 2000);

    // Two mutations before the timer fires: both rows are painted, nothing is left dirty.
    m_term->feedData("a");
    m_term->feedData(esc("[10;1Hb"));
    QVERIFY(m_term->screen()->isDirty());
    QTRY_VERIFY_WITH_TIMEOUT(!m_term->screen()->isDirty(), 2000);
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
    view.flushPending();   // the view is hidden: chunks are queued until shown / flushed
    QStringList lines = view.toPlainText().split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), qsizetype(2));
    const QRegularExpression rxHeader(QStringLiteral("^\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] RX 5 bytes$"));
    QVERIFY2(rxHeader.match(lines.at(0)).hasMatch(), qPrintable(lines.at(0)));
    QCOMPARE(lines.at(1), HexUtils::hexDump("Hello", 0, 16));
    QVERIFY(lines.at(1).startsWith(QStringLiteral("00000000  48 65 6C 6C 6F")));
    QVERIFY(lines.at(1).endsWith(QStringLiteral("|Hello|")));

    view.appendSent("\r\nOK");
    view.flushPending();
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
    view.flushPending();
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
    view.flushPending();
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
    view.flushPending();
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
    view.flushPending();
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
    view.flushPending();
    QCOMPARE(view.document()->blockCount(), 4);
    view.clearAll();
    QVERIFY(view.toPlainText().isEmpty());
    QCOMPARE(view.document()->blockCount(), 1);
    // No leading empty block after a clear.
    view.setShowTimestamps(false);
    view.appendReceived("xy");
    view.flushPending();
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
    view.flushPending();
    QStringList lines = view.toPlainText().split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), qsizetype(3));
    QCOMPARE(lines.mid(1).join(QLatin1Char('\n')), HexUtils::hexDump(data, 0, 8));
    QVERIFY(lines.at(2).startsWith(QStringLiteral("00000008  08 09 0A 0B 0C 0D 0E 0F")));

    view.clearAll();
    view.setBytesPerLine(32);
    QCOMPARE(view.bytesPerLine(), 32);
    view.appendReceived(data + data);
    view.flushPending();
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
    view.flushPending();
    QCOMPARE(view.document()->firstBlock().text(), QStringLiteral("RX 2 bytes"));
    view.setShowTimestamps(true);
    view.appendSent("CD");
    view.flushPending();
    const QString header = view.document()->findBlockByNumber(2).text();
    QVERIFY2(header.startsWith(QLatin1Char('[')), qPrintable(header));
    QVERIFY(header.endsWith(QStringLiteral("] TX 2 bytes")));
}

void Tst_terminalwidget::hexEmptyChunkIgnored()
{
    HexDumpView view;
    view.appendReceived(QByteArray());
    view.appendSent(QByteArray());
    view.flushPending();
    QVERIFY(view.toPlainText().isEmpty());
    QCOMPARE(view.document()->blockCount(), 1);
}

void Tst_terminalwidget::hexHiddenViewDefersRendering()
{
    // A hidden view queues chunks instead of touching its document (DESIGN.md 4.7): during a boot
    // log the terminal page is in front, and a QTextDocument insertion per 4 KB chunk costs more
    // than the whole terminal pipeline. The queue keeps only what maxLines() can show.
    HexDumpView view;
    view.setShowTimestamps(false);
    view.setMaxLines(6);
    QVERIFY(!view.isVisible());
    for (int i = 0; i < 50; ++i) {
        view.appendReceived(QByteArray(16, static_cast<char>('A' + i % 26)));   // header + one hex line
    }
    QCOMPARE(view.document()->blockCount(), 1);   // untouched
    QVERIFY(view.toPlainText().isEmpty());

    // flushPending() renders exactly what rendering every chunk on arrival would have left:
    // the last three chunks (i = 47, 48, 49 -> 'V', 'W', 'X').
    view.flushPending();
    QCOMPARE(view.document()->blockCount(), 6);
    QCOMPARE(view.document()->firstBlock().text(), QStringLiteral("RX 16 bytes"));
    QString text = view.toPlainText();
    QVERIFY2(text.contains(QStringLiteral("|VVVVVVVVVVVVVVVV|")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("|XXXXXXXXXXXXXXXX|")), qPrintable(text));
    QVERIFY2(!text.contains(QStringLiteral("|UUUUUUUUUUUUUUUU|")), qPrintable(text));   // i = 46: trimmed
    view.flushPending();   // nothing queued: no-op
    QCOMPARE(view.document()->blockCount(), 6);

    // Formatting is captured on arrival: a chunk queued without timestamps stays that way even
    // when the option changes before it is rendered.
    view.appendSent("late");
    view.setShowTimestamps(true);
    view.appendSent("later");
    QCOMPARE(view.document()->blockCount(), 6);
    // Showing the view renders the queue before the first paint (4 blocks in, trimmed back to 6).
    view.resize(600, 200);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QCOMPARE(view.document()->blockCount(), 6);
    text = view.toPlainText();
    QVERIFY2(text.contains(QStringLiteral("\nTX 4 bytes\n")), qPrintable(text));   // no timestamp
    QVERIFY2(text.contains(QStringLiteral("] TX 5 bytes\n")), qPrintable(text));   // timestamped
    QVERIFY(view.verticalScrollBar()->value() == view.verticalScrollBar()->maximum());

    // Shown: rendered on arrival, as before.
    view.appendReceived("now");
    QVERIFY(view.toPlainText().contains(QStringLiteral("RX 3 bytes")));

    // clearAll() drops the queue as well as the document.
    view.hide();
    view.appendReceived("gone");
    view.clearAll();
    view.flushPending();
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
