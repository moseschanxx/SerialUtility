#include "terminal/AnsiParser.h"

#include "app/Logging.h"
#include "terminal/TerminalScreen.h"

#include <QStringList>

#include <algorithm>
#include <utility>

namespace {

constexpr int kMaxParams = 32;
constexpr int kMaxSubParams = 8;
constexpr int kMaxParamValue = 65535;
constexpr qsizetype kMaxOscLength = 4096;
/// Code points a DCS/SOS/PM/APC string may consume before the parser gives up and returns to
/// Ground: a stray introducer from line noise must not silence the console forever.
constexpr qsizetype kMaxControlStringLength = 4096;

/// DEC Special Graphics (ESC ( 0): the glyphs for 0x5F..0x7E while that set is GL.
constexpr char32_t kDecSpecialGraphics[32] = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, // _ ` a b c d e f
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, // g h i j k l m n
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, // o p q r s t u v
    0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7, // w x y z { | } ~
};
constexpr char kCharsetAscii = 'B';
constexpr char kCharsetGraphics = '0';

const char kUtf8Name[] = "UTF-8";
const char kSystemName[] = "System";

bool isC0(char32_t cp)
{
    return cp < 0x20;
}

bool isIntermediate(char32_t cp)
{
    return cp >= 0x20 && cp <= 0x2F;
}

bool isParamByte(char32_t cp)
{
    return cp >= 0x30 && cp <= 0x3F;
}

bool isDigit(char32_t cp)
{
    return cp >= U'0' && cp <= U'9';
}

bool isFinalByte(char32_t cp)
{
    return cp >= 0x40 && cp <= 0x7E;
}

void appendCodePoint(QString& out, char32_t cp)
{
    if (QChar::requiresSurrogates(cp)) {
        out.append(QChar(QChar::highSurrogate(cp)));
        out.append(QChar(QChar::lowSurrogate(cp)));
    } else {
        out.append(QChar(static_cast<char16_t>(cp)));
    }
}

int clamp255(int v)
{
    return std::max(0, std::min(v, 255));
}

QStringDecoder makeDecoder(const QString& name)
{
    if (name.compare(QLatin1StringView(kSystemName), Qt::CaseInsensitive) == 0) {
        return QStringDecoder(QStringConverter::System);
    }
    return QStringDecoder(name.toLatin1().constData());
}

bool isUtf8Decoder(const QStringDecoder& decoder)
{
    return decoder.isValid() && qstrcmp(decoder.name(), kUtf8Name) == 0;
}

/// Length of the UTF-8 sequence introduced by `lead`, or 0 when `lead` cannot start one
/// (ASCII, continuation bytes and the always-invalid C0/C1/F5-FF).
int utf8SequenceLength(uchar lead)
{
    if (lead >= 0xC2 && lead <= 0xDF) {
        return 2;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        return 4;
    }
    return 0;
}

bool isUtf8Continuation(uchar byte)
{
    return (byte & 0xC0) == 0x80;
}

} // namespace

// ---- Construction / options ---------------------------------------------------------------

AnsiParser::AnsiParser(TerminalScreen* screen, QObject* parent)
    : QObject(parent)
    , m_screen(screen)
    , m_decoder(QStringConverter::Utf8)
    , m_encoding(QLatin1StringView(kUtf8Name))
{
    Q_ASSERT(screen);
    m_params.reserve(kMaxParams);
    m_subParams.reserve(kMaxParams);
}

AnsiParser::~AnsiParser() = default;

TerminalScreen* AnsiParser::screen() const
{
    return m_screen;
}

bool AnsiParser::setEncoding(const QString& name)
{
    flushText();
    const QString trimmed = name.trimmed();
    QStringDecoder decoder = trimmed.isEmpty() ? QStringDecoder() : makeDecoder(trimmed);
    m_pendingHighSurrogate.clear();
    m_pendingBytes.clear();
    if (!decoder.isValid()) {
        qCWarning(lcTerminal) << "unknown text encoding" << name << "- falling back to UTF-8";
        m_decoder = QStringDecoder(QStringConverter::Utf8);
        m_encoding = QLatin1StringView(kUtf8Name);
        m_utf8 = true;
        return false;
    }
    m_decoder = std::move(decoder);
    m_encoding = trimmed;
    m_utf8 = isUtf8Decoder(m_decoder);
    return true;
}

QString AnsiParser::encoding() const
{
    return m_encoding;
}

QStringList AnsiParser::availableEncodings()
{
    QStringList list{QStringLiteral("UTF-8"), QStringLiteral("ISO-8859-1")};
    if (QStringDecoder("GB18030").isValid()) {
        list.append(QStringLiteral("GB18030"));
    }
    list.append(QLatin1StringView(kSystemName));
    return list;
}

bool AnsiParser::implicitCr() const
{
    return m_implicitCr;
}

void AnsiParser::setImplicitCr(bool on)
{
    m_implicitCr = on;
}

bool AnsiParser::cursorKeyApplicationMode() const
{
    return m_cursorKeyApp;
}

bool AnsiParser::bracketedPasteMode() const
{
    return m_bracketedPaste;
}

void AnsiParser::setCursorKeyApplicationMode(bool on)
{
    if (m_cursorKeyApp == on) {
        return;
    }
    m_cursorKeyApp = on;
    emit cursorKeyModeChanged(on);
}

void AnsiParser::setBracketedPaste(bool on)
{
    if (m_bracketedPaste == on) {
        return;
    }
    m_bracketedPaste = on;
    emit bracketedPasteChanged(on);
}

// ---- Input --------------------------------------------------------------------------------

void AnsiParser::feed(const QByteArray& data)
{
    if (!m_screen || data.isEmpty()) {
        return;
    }
    QString text = decodeChunk(data);
    if (!m_pendingHighSurrogate.isEmpty()) {
        text.prepend(m_pendingHighSurrogate);
        m_pendingHighSurrogate.clear();
    }
    // One batch per chunk: the screen emits contentChanged/scrollbackChanged/cursorMoved once at
    // the end instead of after every text run and control (TerminalScreen::beginBatch()).
    m_screen->beginBatch();
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t unit = text[i].unicode();
        char32_t cp = unit;
        if (QChar::isHighSurrogate(unit)) {
            if (i + 1 >= n) {
                m_pendingHighSurrogate = QString(QChar(unit));
                break;
            }
            const char16_t low = text[i + 1].unicode();
            if (QChar::isLowSurrogate(low)) {
                cp = QChar::surrogateToUcs4(unit, low);
                ++i;
            } else {
                cp = 0xFFFD;
            }
        } else if (QChar::isLowSurrogate(unit)) {
            cp = 0xFFFD;
        }
        processCodePoint(cp);
    }
    flushText();
    m_screen->endBatch();
}

void AnsiParser::reset()
{
    m_state = State::Ground;
    clearSequence();
    m_textRun.clear();
    m_pendingHighSurrogate.clear();
    m_pendingBytes.clear();
    m_decoder.resetState();
    m_lineFeedNewLine = false;
    resetCharsets();
    setCursorKeyApplicationMode(false);
    setBracketedPaste(false);
}

QString AnsiParser::decodeChunk(const QByteArray& data)
{
    if (!m_utf8) {
        return m_decoder.decode(data);
    }

    // QStringDecoder carries an unterminated multi-byte sequence over to the next call, but
    // when that sequence turns out to be invalid it drops every byte it collected after the
    // lead byte *and* the first byte of the new chunk - which on a serial console is often
    // the ESC of the next control sequence. Line noise and baud-rate mismatches produce exactly
    // this damage, so the chunk boundary is handled here: only a sequence that is valid so far
    // is carried over (m_pendingBytes), and an invalid lead byte becomes U+FFFD without
    // touching the bytes that follow it. The decoder itself never sees a truncated sequence.
    QByteArray joined;
    QByteArrayView bytes(data);
    if (!m_pendingBytes.isEmpty()) {
        joined = m_pendingBytes + data;
        m_pendingBytes.clear();
        bytes = joined;
    }

    QString out;
    while (!bytes.isEmpty()) {
        const qsizetype n = bytes.size();
        // A sequence can only run past the end of the chunk if its lead byte is among the
        // last three bytes; every C2..F4 byte is processed as a lead (it is never a valid
        // continuation), so the first such overrunning byte is where the decoder would stop.
        // The split is then moved back over any sequence it would cut.
        qsizetype lead = -1;
        for (qsizetype i = std::max<qsizetype>(0, n - 3); i < n; ++i) {
            const int length = utf8SequenceLength(static_cast<uchar>(bytes[i]));
            if (length > 0 && n - i < length) {
                lead = i;
                break;
            }
        }
        if (lead < 0) {
            out.append(QString(m_decoder.decode(bytes)));
            break;
        }
        // The prefix handed to the decoder must not itself end inside a multi-byte sequence:
        // a lead byte within three bytes before `lead` whose sequence is cut off by `lead`
        // would be carried over as decoder state, and QStringDecoder then swallows the first
        // byte of the next call - the pending lead byte. Move the split back over every such
        // byte (repeat until stable: the new prefix can end in another cut-off sequence).
        for (bool moved = true; moved && lead > 0;) {
            moved = false;
            for (qsizetype i = std::max<qsizetype>(0, lead - 3); i < lead; ++i) {
                const int length = utf8SequenceLength(static_cast<uchar>(bytes[i]));
                if (length > 0 && i + length > lead) { // i + length == lead is a complete character
                    lead = i;
                    moved = true;
                    break;
                }
            }
        }
        if (lead > 0) {
            out.append(QString(m_decoder.decode(bytes.first(lead))));
        }
        bool validSoFar = true;
        for (qsizetype i = lead + 1; i < n; ++i) {
            if (!isUtf8Continuation(static_cast<uchar>(bytes[i]))) {
                validSoFar = false;
                break;
            }
        }
        if (validSoFar) {
            m_pendingBytes = bytes.sliced(lead).toByteArray(); // wait for the rest of the character
            break;
        }
        out.append(QChar(QChar::ReplacementCharacter)); // the lead byte alone is invalid
        bytes = bytes.sliced(lead + 1);
    }
    return out;
}

void AnsiParser::flushText()
{
    if (m_textRun.isEmpty()) {
        return;
    }
    m_screen->putText(m_textRun);
    m_textRun.resize(0);
}

// ---- State machine (Paul Williams' VT500 parser) ------------------------------------------

void AnsiParser::processCodePoint(char32_t cp)
{
    // "Anywhere" transitions -------------------------------------------------------------
    if (cp == 0x1B) {
        if (m_state == State::OscString) {
            dispatchOsc();
        } else if (m_state == State::Ground) {
            flushText();
        }
        clearSequence();
        m_state = State::Escape;
        return;
    }
    if (cp == 0x18 || cp == 0x1A) { // CAN / SUB abort any sequence
        flushText();
        clearSequence();
        m_state = State::Ground;
        return;
    }
    if (cp >= 0x80 && cp <= 0x9F) { // C1 controls
        flushText();
        if (m_state == State::OscString && cp == 0x9C) {
            dispatchOsc();
        }
        clearSequence();
        switch (cp) {
        case 0x9B:
            m_state = State::CsiEntry;
            break;
        case 0x9D:
            m_state = State::OscString;
            break;
        case 0x90:
            m_state = State::DcsEntry;
            break;
        case 0x98:
        case 0x9E:
        case 0x9F:
            m_state = State::SosPmApcString;
            break;
        default: // ST and the remaining C1 controls are ignored
            m_state = State::Ground;
            break;
        }
        return;
    }

    // Printable non-ASCII text inside a control sequence or a DCS/SOS/PM/APC string is garbage:
    // abandon it and print the character instead of swallowing the text that follows (ECMA-48
    // command strings are 7-bit; OSC is excluded because titles may contain non-ASCII text).
    if (cp >= 0xA0) {
        switch (m_state) {
        case State::Escape:
        case State::EscapeIntermediate:
        case State::CsiEntry:
        case State::CsiParam:
        case State::CsiIntermediate:
        case State::CsiIgnore:
        case State::DcsEntry:
        case State::DcsParam:
        case State::DcsIntermediate:
        case State::DcsPassthrough:
        case State::DcsIgnore:
        case State::SosPmApcString:
            clearSequence();
            m_state = State::Ground;
            break;
        default:
            break;
        }
    }

    // A stray introducer from line noise must not swallow the console until an ST arrives:
    // give up on an over-long string and print from here on.
    if (m_state == State::DcsEntry || m_state == State::DcsParam || m_state == State::DcsIntermediate ||
        m_state == State::DcsPassthrough || m_state == State::DcsIgnore || m_state == State::SosPmApcString) {
        if (++m_stringLength > kMaxControlStringLength) {
            qCDebug(lcTerminal) << "abandoning over-long control string";
            clearSequence();
            m_state = State::Ground; // cp is then handled by the Ground case below and printed
        }
    }

    switch (m_state) {
    case State::Ground:
        if (isC0(cp)) {
            flushText();
            executeC0(cp);
        } else if (cp != 0x7F) {
            appendCodePoint(m_textRun, mapCharset(cp));
        }
        break;

    case State::Escape:
        if (isC0(cp)) {
            executeC0(cp);
        } else if (cp == 0x7F) {
            // ignored
        } else if (isIntermediate(cp)) {
            m_intermediates.append(QChar(static_cast<char16_t>(cp)));
            m_state = State::EscapeIntermediate;
        } else if (cp == U'[') {
            m_state = State::CsiEntry;
        } else if (cp == U']') {
            m_state = State::OscString;
        } else if (cp == U'P') {
            m_state = State::DcsEntry;
        } else if (cp == U'X' || cp == U'^' || cp == U'_') {
            m_state = State::SosPmApcString;
        } else {
            m_state = State::Ground;
            dispatchEscape(cp);
        }
        break;

    case State::EscapeIntermediate:
        if (isC0(cp)) {
            executeC0(cp);
        } else if (cp == 0x7F) {
            // ignored
        } else if (isIntermediate(cp)) {
            m_intermediates.append(QChar(static_cast<char16_t>(cp)));
        } else {
            m_state = State::Ground;
            dispatchEscape(cp);
        }
        break;

    case State::CsiEntry:
        if (isC0(cp)) {
            executeC0(cp);
        } else if (cp == 0x7F) {
            // ignored
        } else if (isDigit(cp) || cp == U';' || cp == U':') {
            collectParam(cp);
            m_state = State::CsiParam;
        } else if (cp >= 0x3C && cp <= 0x3F) {
            m_privateMarker = true;
            m_privateChar = cp;
            m_state = State::CsiParam;
        } else if (isIntermediate(cp)) {
            m_intermediates.append(QChar(static_cast<char16_t>(cp)));
            m_state = State::CsiIntermediate;
        } else {
            m_state = State::Ground;
            dispatchCsi(cp);
        }
        break;

    case State::CsiParam:
        if (isC0(cp)) {
            executeC0(cp);
        } else if (cp == 0x7F) {
            // ignored
        } else if (isDigit(cp) || cp == U';' || cp == U':') {
            collectParam(cp);
        } else if (cp >= 0x3C && cp <= 0x3F) {
            m_state = State::CsiIgnore;
        } else if (isIntermediate(cp)) {
            m_intermediates.append(QChar(static_cast<char16_t>(cp)));
            m_state = State::CsiIntermediate;
        } else {
            m_state = State::Ground;
            dispatchCsi(cp);
        }
        break;

    case State::CsiIntermediate:
        if (isC0(cp)) {
            executeC0(cp);
        } else if (cp == 0x7F) {
            // ignored
        } else if (isIntermediate(cp)) {
            m_intermediates.append(QChar(static_cast<char16_t>(cp)));
        } else if (isParamByte(cp)) {
            m_state = State::CsiIgnore;
        } else {
            m_state = State::Ground;
            dispatchCsi(cp);
        }
        break;

    case State::CsiIgnore:
        if (isC0(cp)) {
            executeC0(cp);
        } else if (isFinalByte(cp)) {
            qCDebug(lcTerminal) << "ignoring malformed CSI sequence ending in"
                                << QString(QChar(static_cast<char16_t>(cp)));
            clearSequence();
            m_state = State::Ground;
        }
        break;

    case State::OscString:
        if (cp == 0x07) {
            dispatchOsc();
            clearSequence();
            m_state = State::Ground;
        } else if (!isC0(cp) && cp != 0x7F && m_oscString.size() < kMaxOscLength) {
            appendCodePoint(m_oscString, cp);
        }
        break;

    case State::DcsEntry:
        if (isC0(cp) || cp == 0x7F) {
            // ignored
        } else if (cp == U':') {
            m_state = State::DcsIgnore;
        } else if (isParamByte(cp)) {
            m_state = State::DcsParam;
        } else if (isIntermediate(cp)) {
            m_state = State::DcsIntermediate;
        } else if (isFinalByte(cp)) {
            m_state = State::DcsPassthrough;
        } else {
            m_state = State::DcsIgnore;
        }
        break;

    case State::DcsParam:
        if (isC0(cp) || cp == 0x7F) {
            // ignored
        } else if (isDigit(cp) || cp == U';') {
            // parameters are not needed: every DCS is consumed
        } else if (isParamByte(cp)) {
            m_state = State::DcsIgnore;
        } else if (isIntermediate(cp)) {
            m_state = State::DcsIntermediate;
        } else if (isFinalByte(cp)) {
            m_state = State::DcsPassthrough;
        } else {
            m_state = State::DcsIgnore;
        }
        break;

    case State::DcsIntermediate:
        if (isC0(cp) || cp == 0x7F || isIntermediate(cp)) {
            // ignored
        } else if (isParamByte(cp)) {
            m_state = State::DcsIgnore;
        } else if (isFinalByte(cp)) {
            m_state = State::DcsPassthrough;
        } else {
            m_state = State::DcsIgnore;
        }
        break;

    case State::DcsPassthrough:
    case State::DcsIgnore:
    case State::SosPmApcString:
        // Consumed until ST (ESC \ or 0x9C), which the "anywhere" transitions handle.
        break;
    }
}

void AnsiParser::executeC0(char32_t cp)
{
    switch (cp) {
    case 0x07: // BEL
        m_screen->bell();
        break;
    case 0x08: // BS
        m_screen->backspace();
        break;
    case 0x09: // HT
        m_screen->tab();
        break;
    case 0x0A: // LF
    case 0x0B: // VT
    case 0x0C: // FF
        if (m_implicitCr || m_lineFeedNewLine) {
            m_screen->carriageReturn();
        }
        m_screen->lineFeed();
        break;
    case 0x0D: // CR
        m_screen->carriageReturn();
        break;
    case 0x0E: // SO (LS1): G1 becomes the active set
        m_glCharset = 1;
        break;
    case 0x0F: // SI (LS0): back to G0
        m_glCharset = 0;
        break;
    default: // NUL, ENQ, XON, XOFF and the rest are ignored
        break;
    }
}

char32_t AnsiParser::mapCharset(char32_t cp) const
{
    if (m_charset[m_glCharset] == kCharsetGraphics && cp >= 0x5F && cp <= 0x7E) {
        return kDecSpecialGraphics[cp - 0x5F];
    }
    return cp;
}

void AnsiParser::resetCharsets()
{
    m_charset[0] = m_charset[1] = kCharsetAscii;
    m_glCharset = 0;
    m_savedCharset[0] = m_savedCharset[1] = kCharsetAscii;
    m_savedGl = 0;
}

// ---- ESC sequences ------------------------------------------------------------------------

void AnsiParser::dispatchEscape(char32_t final)
{
    if (m_intermediates.isEmpty()) {
        switch (final) {
        case U'7': // DECSC: the charset state is saved in a single parser-side slot (not per screen)
            m_screen->saveCursor();
            m_savedCharset[0] = m_charset[0];
            m_savedCharset[1] = m_charset[1];
            m_savedGl = m_glCharset;
            break;
        case U'8': // DECRC
            m_screen->restoreCursor();
            m_charset[0] = m_savedCharset[0];
            m_charset[1] = m_savedCharset[1];
            m_glCharset = m_savedGl;
            break;
        case U'D': // IND
            m_screen->index();
            break;
        case U'E': // NEL
            m_screen->nextLine();
            break;
        case U'H': // HTS
            m_screen->setTabStop();
            break;
        case U'M': // RI
            m_screen->reverseIndex();
            break;
        case U'Z': // DECID: answer like DA
            emit responseRequested(QByteArrayLiteral("\x1b[?1;2c"));
            break;
        case U'c': // RIS
            m_screen->reset();
            m_lineFeedNewLine = false;
            resetCharsets();
            setCursorKeyApplicationMode(false);
            setBracketedPaste(false);
            break;
        case U'=': // DECKPAM
        case U'>': // DECKPNM
        case U'\\': // ST closing a string
        case U'N': // SS2
        case U'O': // SS3
        case U'n': // LS2
        case U'o': // LS3
        case U'|': // LS3R
        case U'}': // LS2R
        case U'~': // LS1R
            break;
        default:
            qCDebug(lcTerminal) << "ignoring unknown escape sequence"
                                << describeSequence(QStringLiteral("ESC "), final);
            break;
        }
        clearSequence();
        return;
    }

    const QChar inter = m_intermediates.at(0);
    if (inter == QLatin1Char('#')) {
        if (final == U'8') {
            alignmentPattern(); // DECALN
        }
        // ESC # 3 / 4 / 5 / 6 (double-height / double-width lines) are ignored.
    } else if (inter == QLatin1Char('(') || inter == QLatin1Char(')')) {
        // SCS for G0 / G1: '0' and '2' (alternate ROM) select DEC Special Graphics; every other
        // set (ASCII 'B', UK 'A', national replacement sets, `ESC ( % 5` ...) is treated as ASCII.
        const bool graphics = m_intermediates.size() == 1 && (final == U'0' || final == U'2');
        m_charset[inter == QLatin1Char('(') ? 0 : 1] = graphics ? kCharsetGraphics : kCharsetAscii;
    } else if (inter == QLatin1Char('*') || inter == QLatin1Char('+') || inter == QLatin1Char('-') ||
               inter == QLatin1Char('.') || inter == QLatin1Char('/')) {
        // G2 / G3 designations and 96-character sets: ignored (never selected into GL).
    } else if (inter == QLatin1Char(' ') || inter == QLatin1Char('%')) {
        // S7C1T / S8C1T / ANSI conformance level, ESC % G / ESC % @ (UTF-8 selection): ignored.
    } else {
        qCDebug(lcTerminal) << "ignoring unknown escape sequence" << describeSequence(QStringLiteral("ESC "), final);
    }
    clearSequence();
}

void AnsiParser::alignmentPattern()
{
    const int rows = m_screen->rows();
    const int cols = m_screen->cols();
    m_screen->setOriginMode(false);
    m_screen->setScrollRegion(0, rows - 1);
    const QString fill(cols, QLatin1Char('E'));
    for (int r = 0; r < rows; ++r) {
        m_screen->moveCursorTo(r, 0);
        m_screen->putText(fill);
    }
    m_screen->moveCursorTo(0, 0);
}

// ---- CSI sequences ------------------------------------------------------------------------

void AnsiParser::dispatchCsi(char32_t final)
{
    const int count = static_cast<int>(m_params.size());

    // -- private-marker sequences: CSI ? ... , CSI > ... , CSI = ... , CSI < ... ------------
    if (m_privateMarker) {
        if (m_privateChar == U'?' && m_intermediates.isEmpty()) {
            switch (final) {
            case U'h': // DECSET
            case U'l': // DECRST
                for (int i = 0; i < count; ++i) {
                    setMode(m_params[i], final == U'h', true);
                }
                break;
            case U'J': // DECSED, treated like ED
                m_screen->eraseInDisplay(param(0));
                break;
            case U'K': // DECSEL, treated like EL
                m_screen->eraseInLine(param(0));
                break;
            case U'n': // DECXCPR
                if (param(0) == 6) {
                    const Terminal::Cursor c = m_screen->cursor();
                    const int row = (m_screen->originMode() ? c.row - m_screen->scrollTop() : c.row) + 1;
                    emit responseRequested(QStringLiteral("\x1b[?%1;%2R").arg(row).arg(c.col + 1).toLatin1());
                }
                break;
            default:
                qCDebug(lcTerminal) << "ignoring unknown CSI sequence"
                                    << describeSequence(QStringLiteral("CSI "), final);
                break;
            }
        } else {
            // Secondary/tertiary DA, XTMODKEYS, DECRQM and friends: consumed, not implemented.
            qCDebug(lcTerminal) << "ignoring unsupported CSI sequence"
                                << describeSequence(QStringLiteral("CSI "), final);
        }
        clearSequence();
        return;
    }

    // -- sequences with an intermediate byte ----------------------------------------------
    if (!m_intermediates.isEmpty()) {
        const QChar inter = m_intermediates.at(0);
        if (inter == QLatin1Char('!') && final == U'p') {
            softReset(); // DECSTR
        } else if (inter == QLatin1Char(' ') && final == U'q') {
            // DECSCUSR (cursor style): ignored
        } else if (inter == QLatin1Char('"') && (final == U'q' || final == U'p')) {
            // DECSCA (protection) / DECSCL (conformance level): ignored
        } else if (inter == QLatin1Char('$') || inter == QLatin1Char('\'') || inter == QLatin1Char('*')) {
            // DECRQM, rectangular-area operations, DECSACE and similar: ignored
        } else {
            qCDebug(lcTerminal) << "ignoring unknown CSI sequence" << describeSequence(QStringLiteral("CSI "), final);
        }
        clearSequence();
        return;
    }

    // -- standard sequences ----------------------------------------------------------------
    switch (final) {
    case U'@': // ICH
        m_screen->insertChars(paramOr(0, 1));
        break;
    case U'A': // CUU
        m_screen->moveCursorBy(-paramOr(0, 1), 0);
        break;
    case U'B': // CUD
        m_screen->moveCursorBy(paramOr(0, 1), 0);
        break;
    case U'C': // CUF
        m_screen->moveCursorBy(0, paramOr(0, 1));
        break;
    case U'D': // CUB
        m_screen->moveCursorBy(0, -paramOr(0, 1));
        break;
    case U'E': // CNL
        m_screen->cursorNextLine(paramOr(0, 1));
        break;
    case U'F': // CPL
        m_screen->cursorPreviousLine(paramOr(0, 1));
        break;
    case U'G': // CHA
    case U'`': // HPA
        m_screen->setCursorColumn(paramOr(0, 1) - 1);
        break;
    case U'H': // CUP
    case U'f': // HVP
        m_screen->moveCursorTo(paramOr(0, 1) - 1, paramOr(1, 1) - 1);
        break;
    case U'I': // CHT
        for (int i = paramOr(0, 1); i > 0; --i) {
            m_screen->tab();
        }
        break;
    case U'J': // ED
        m_screen->eraseInDisplay(param(0));
        break;
    case U'K': // EL
        m_screen->eraseInLine(param(0));
        break;
    case U'L': // IL
        m_screen->insertLines(paramOr(0, 1));
        break;
    case U'M': // DL
        m_screen->deleteLines(paramOr(0, 1));
        break;
    case U'P': // DCH
        m_screen->deleteChars(paramOr(0, 1));
        break;
    case U'S': // SU
        m_screen->scrollUp(paramOr(0, 1));
        break;
    case U'T': // SD (with more than one parameter it is XTHIMOUSE: ignored)
        if (count <= 1) {
            m_screen->scrollDown(paramOr(0, 1));
        }
        break;
    case U'X': // ECH
        m_screen->eraseChars(paramOr(0, 1));
        break;
    case U'Z': { // CBT: back to the previous tab stop (fixed 8-column stops)
        int col = m_screen->cursor().col;
        for (int i = paramOr(0, 1); i > 0 && col > 0; --i) {
            col = ((col - 1) / 8) * 8;
        }
        m_screen->setCursorColumn(col);
        break;
    }
    case U'a': // HPR
        m_screen->moveCursorBy(0, paramOr(0, 1));
        break;
    case U'b': // REP
        m_screen->repeatLastChar(paramOr(0, 1));
        break;
    case U'c': // DA: "VT100 with Advanced Video Option"
        if (param(0) == 0) {
            emit responseRequested(QByteArrayLiteral("\x1b[?1;2c"));
        }
        break;
    case U'd': // VPA
        m_screen->setCursorRow(paramOr(0, 1) - 1);
        break;
    case U'e': // VPR
        m_screen->moveCursorBy(paramOr(0, 1), 0);
        break;
    case U'g': // TBC
        if (param(0) == 0) {
            m_screen->clearTabStop();
        } else if (param(0) == 3) {
            m_screen->clearAllTabStops();
        }
        break;
    case U'h': // SM
    case U'l': // RM
        for (int i = 0; i < count; ++i) {
            setMode(m_params[i], final == U'h', false);
        }
        break;
    case U'm': // SGR
        applySgr();
        break;
    case U'n': // DSR
        if (param(0) == 5) {
            emit responseRequested(QByteArrayLiteral("\x1b[0n"));
        } else if (param(0) == 6) {
            const Terminal::Cursor c = m_screen->cursor();
            const int row = (m_screen->originMode() ? c.row - m_screen->scrollTop() : c.row) + 1;
            emit responseRequested(QStringLiteral("\x1b[%1;%2R").arg(row).arg(c.col + 1).toLatin1());
        }
        break;
    case U'r': // DECSTBM
        m_screen->setScrollRegion(paramOr(0, 1) - 1, paramOr(1, m_screen->rows()) - 1);
        break;
    case U's': // SCOSC
        m_screen->saveCursor();
        break;
    case U'u': // SCORC
        m_screen->restoreCursor();
        break;
    case U't': // XTWINOPS
    case U'q': // DECLL
    case U'x': // DECREQTPARM
        break;
    default:
        qCDebug(lcTerminal) << "ignoring unknown CSI sequence" << describeSequence(QStringLiteral("CSI "), final);
        break;
    }
    clearSequence();
}

void AnsiParser::setMode(int mode, bool on, bool isPrivate)
{
    if (!isPrivate) {
        switch (mode) {
        case 4: // IRM
            m_screen->setInsertMode(on);
            break;
        case 20: // LNM
            m_lineFeedNewLine = on;
            break;
        case 2:  // KAM
        case 12: // SRM
            break;
        default:
            qCDebug(lcTerminal) << "ignoring unsupported ANSI mode" << mode << (on ? "set" : "reset");
            break;
        }
        return;
    }
    switch (mode) {
    case 1: // DECCKM
        setCursorKeyApplicationMode(on);
        break;
    case 6: // DECOM
        m_screen->setOriginMode(on);
        break;
    case 7: // DECAWM
        m_screen->setAutoWrap(on);
        break;
    case 25: // DECTCEM
        m_screen->setCursorVisible(on);
        break;
    case 47:
    case 1047:
        m_screen->setAlternateScreen(on, false);
        break;
    case 1048:
        if (on) {
            m_screen->saveCursor();
        } else {
            m_screen->restoreCursor();
        }
        break;
    case 1049:
        // xterm (srm_ALTBUF_CURSOR) always runs CursorSave/ClearScreen on set and
        // CursorRestore on reset; only the buffer switch itself is guarded.
        if (on && m_screen->alternateScreenActive()) {
            m_screen->saveCursor();      // alternate-screen slot, like xterm's sc[whichBuf]
            m_screen->eraseInDisplay(2); // ClearScreen: erase cells, cursor stays put
        } else if (!on && !m_screen->alternateScreenActive()) {
            m_screen->restoreCursor(); // primary slot; homes + resets attrs when nothing saved
        } else {
            m_screen->setAlternateScreen(on, true);
        }
        break;
    case 2004:
        setBracketedPaste(on);
        break;
    case 3:    // DECCOLM
    case 4:    // DECSCLM
    case 5:    // DECSCNM
    case 8:    // DECARM
    case 9:    // X10 mouse
    case 12:   // cursor blink
    case 40:   // 80/132 switching
    case 45:   // reverse wraparound
    case 1000: // mouse tracking modes ...
    case 1001:
    case 1002:
    case 1003:
    case 1004: // focus events
    case 1005:
    case 1006:
    case 1007:
    case 1015:
    case 1016:
    case 1034: // meta sends escape
    case 1036:
    case 2026: // synchronized output
    case 7727: // application escape mode
    case 8452: // sixel cursor placement
        break;
    default:
        qCDebug(lcTerminal) << "ignoring unsupported DEC private mode" << mode << (on ? "set" : "reset");
        break;
    }
}

void AnsiParser::softReset()
{
    const Terminal::Cursor c = m_screen->cursor();
    m_screen->resetAttributes();
    m_screen->setCursorVisible(true);
    m_screen->setOriginMode(false);
    m_screen->setAutoWrap(true);
    m_screen->setInsertMode(false);
    m_screen->setScrollRegion(0, m_screen->rows() - 1);
    m_screen->moveCursorTo(c.row, c.col);
    m_lineFeedNewLine = false;
    resetCharsets(); // xterm: DECSTR designates ASCII into G0..G3 and selects G0
    setCursorKeyApplicationMode(false);
    setBracketedPaste(false);
}

// ---- SGR ----------------------------------------------------------------------------------

bool AnsiParser::extendedColor(int index, Terminal::Color& out, int& consumed) const
{
    consumed = 0;
    const QVector<int>& subs = m_subParams[index];
    if (!subs.isEmpty()) {
        // Colon form: 38:5:n  |  38:2::r:g:b  |  38:2:r:g:b (missing colour-space id)
        if (subs[0] == 5 && subs.size() >= 2) {
            out = Terminal::Color::indexed(clamp255(subs[1]));
            return true;
        }
        if (subs[0] == 2) {
            if (subs.size() >= 5) {
                out = Terminal::Color::rgb(clamp255(subs[2]), clamp255(subs[3]), clamp255(subs[4]));
                return true;
            }
            if (subs.size() == 4) {
                out = Terminal::Color::rgb(clamp255(subs[1]), clamp255(subs[2]), clamp255(subs[3]));
                return true;
            }
        }
        return false;
    }
    // Semicolon form: the following parameters belong to this colour specification.
    const int remaining = static_cast<int>(m_params.size()) - index - 1;
    if (remaining <= 0) {
        return false;
    }
    const int kind = param(index + 1);
    if (kind == 5) {
        consumed = std::min(2, remaining);
        out = Terminal::Color::indexed(clamp255(param(index + 2)));
        return true;
    }
    if (kind == 2) {
        consumed = std::min(4, remaining);
        out = Terminal::Color::rgb(clamp255(param(index + 2)), clamp255(param(index + 3)), clamp255(param(index + 4)));
        return true;
    }
    consumed = 1;
    return false;
}

void AnsiParser::applySgr()
{
    if (m_params.isEmpty()) {
        m_screen->resetAttributes();
        return;
    }
    Terminal::Attributes attr = m_screen->currentAttributes();
    const int count = static_cast<int>(m_params.size());
    for (int i = 0; i < count; ++i) {
        const int p = m_params[i];
        const QVector<int>& subs = m_subParams[i];
        switch (p) {
        case 0:
            attr = Terminal::Attributes();
            break;
        case 1:
            attr.set(Terminal::Bold, true);
            break;
        case 2:
            attr.set(Terminal::Dim, true);
            break;
        case 3:
            attr.set(Terminal::Italic, true);
            break;
        case 4: // 4:0 = off, 4 / 4:1..4:5 = on (underline styles are all drawn the same)
            attr.set(Terminal::Underline, subs.isEmpty() || subs[0] != 0);
            break;
        case 5:
        case 6:
            attr.set(Terminal::Blink, true);
            break;
        case 7:
            attr.set(Terminal::Inverse, true);
            break;
        case 8:
            attr.set(Terminal::Hidden, true);
            break;
        case 9:
            attr.set(Terminal::Strike, true);
            break;
        case 21: // double underline -> underline
            attr.set(Terminal::Underline, true);
            break;
        case 22:
            attr.set(Terminal::Bold, false);
            attr.set(Terminal::Dim, false);
            break;
        case 23:
            attr.set(Terminal::Italic, false);
            break;
        case 24:
            attr.set(Terminal::Underline, false);
            break;
        case 25:
            attr.set(Terminal::Blink, false);
            break;
        case 27:
            attr.set(Terminal::Inverse, false);
            break;
        case 28:
            attr.set(Terminal::Hidden, false);
            break;
        case 29:
            attr.set(Terminal::Strike, false);
            break;
        case 38:
        case 48:
        case 58: {
            Terminal::Color color;
            int consumed = 0;
            const bool ok = extendedColor(i, color, consumed);
            if (ok && p == 38) {
                attr.fg = color;
            } else if (ok && p == 48) {
                attr.bg = color;
            }
            i += consumed; // 58 (underline colour) is parsed and ignored
            break;
        }
        case 39:
            attr.fg = Terminal::Color();
            break;
        case 49:
            attr.bg = Terminal::Color();
            break;
        case 59: // default underline colour
            break;
        default:
            if (p >= 30 && p <= 37) {
                attr.fg = Terminal::Color::indexed(p - 30);
            } else if (p >= 40 && p <= 47) {
                attr.bg = Terminal::Color::indexed(p - 40);
            } else if (p >= 90 && p <= 97) {
                attr.fg = Terminal::Color::indexed(p - 90 + 8);
            } else if (p >= 100 && p <= 107) {
                attr.bg = Terminal::Color::indexed(p - 100 + 8);
            } else {
                qCDebug(lcTerminal) << "ignoring unsupported SGR parameter" << p;
            }
            break;
        }
    }
    m_screen->setCurrentAttributes(attr);
}

// ---- OSC ----------------------------------------------------------------------------------

void AnsiParser::dispatchOsc()
{
    const qsizetype sep = m_oscString.indexOf(QLatin1Char(';'));
    if (sep < 0) {
        m_oscString.clear();
        return;
    }
    bool ok = false;
    const int ps = m_oscString.left(sep).toInt(&ok);
    if (ok && (ps == 0 || ps == 2)) {
        m_screen->setTitle(m_oscString.mid(sep + 1));
    } else {
        qCDebug(lcTerminal) << "ignoring OSC" << m_oscString.left(sep);
    }
    m_oscString.clear();
}

// ---- Parameter handling -------------------------------------------------------------------

void AnsiParser::collectParam(char32_t cp)
{
    if (m_params.isEmpty()) {
        m_params.append(0);
        m_subParams.append(QVector<int>());
    }
    if (cp == U';') {
        m_inSubParam = false;
        if (m_paramOverflow || m_params.size() >= kMaxParams) {
            m_paramOverflow = true; // extra parameters are ignored, never synthesized as 0
            return;
        }
        m_params.append(0);
        m_subParams.append(QVector<int>());
        return;
    }
    if (m_paramOverflow) {
        return;
    }
    if (cp == U':') {
        QVector<int>& subs = m_subParams.last();
        if (subs.size() >= kMaxSubParams) {
            m_paramOverflow = true; // too many sub-parameters: ignore the rest of the sequence
            return;
        }
        subs.append(0);
        m_inSubParam = true;
        return;
    }
    const int digit = static_cast<int>(cp - U'0');
    int& value = m_inSubParam ? m_subParams.last().last() : m_params.last();
    value = std::min(value * 10 + digit, kMaxParamValue);
}

int AnsiParser::param(int index) const
{
    return (index >= 0 && index < m_params.size()) ? m_params[index] : 0;
}

int AnsiParser::paramOr(int index, int fallback) const
{
    const int v = param(index);
    return v == 0 ? fallback : v;
}

void AnsiParser::clearSequence()
{
    m_params.clear();
    m_subParams.clear();
    m_intermediates.clear();
    m_privateMarker = false;
    m_privateChar = 0;
    m_oscString.clear();
    m_inSubParam = false;
    m_paramOverflow = false;
    m_stringLength = 0;
}

QString AnsiParser::describeSequence(const QString& prefix, char32_t final) const
{
    QString s = prefix;
    if (m_privateMarker) {
        s.append(QChar(static_cast<char16_t>(m_privateChar)));
    }
    QStringList params;
    for (int i = 0; i < m_params.size(); ++i) {
        QString p = QString::number(m_params[i]);
        for (int sub : m_subParams[i]) {
            p.append(QLatin1Char(':'));
            p.append(QString::number(sub));
        }
        params.append(p);
    }
    s.append(params.join(QLatin1Char(';')));
    s.append(m_intermediates);
    s.append(QChar(static_cast<char16_t>(final)));
    return s;
}
