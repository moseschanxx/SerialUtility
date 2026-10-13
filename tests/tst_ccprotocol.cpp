// CcProtocol (COBS, CRC16, frames, FrameSplitter, INFO, names) and CcPackage (CRC32, parse, build)
// against the known answers the Python tools generated (tests/data/firmware/vectors.json, the small
// package and the 0.8.1 release package) plus every case of CC1310-Firmware tools/tests/
// test_fw_protocol.py and test_fw_package.py. The suite never runs Python.
#include <QtTest>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QtEndian>

#include "firmware/CcPackage.h"
#include "firmware/CcProtocol.h"

using namespace CcProtocol;

namespace {

QString dataPath(const QString& name)
{
    return QString::fromUtf8(SU_TEST_DATA_DIR) + QStringLiteral("/firmware/") + name;
}

QByteArray hex(const QJsonValue& value)
{
    return QByteArray::fromHex(value.toString().toLatin1());
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray le32(quint32 value)
{
    QByteArray bytes(4, '\0');
    qToLittleEndian<quint32>(value, bytes.data());
    return bytes;
}

QByteArray infoPayload(quint32 target, quint32 state, quint32 maxSize, quint32 imageSize, quint32 version,
                       std::optional<quint32> role = std::nullopt)
{
    QByteArray payload = le32(target) + le32(state) + le32(maxSize) + le32(imageSize) + le32(version);
    if (role)
        payload += le32(*role);
    return payload;
}

/// A wire frame whose raw bytes (header + payload + crc) are given, COBS-encoded and terminated.
QByteArray wireFromRaw(const QByteArray& raw)
{
    return cobsEncode(raw) + QByteArray(1, '\0');
}

/// The raw bytes of a valid HELLO frame, decoded from its wire form: tests flip a field and re-wrap.
QByteArray rawHello(quint16 sequence)
{
    QByteArray raw;
    cobsDecode(encodeFrame(Hello, sequence).chopped(1), &raw);
    return raw;
}

void fixCrc(QByteArray* raw)
{
    const quint16 crc = crc16Ccitt(raw->left(raw->size() - 2));
    qToLittleEndian<quint16>(crc, raw->data() + raw->size() - 2);
}

/// test_fw_package.py replace_header(): overwrite a header field, then recompute the header CRC32 so
/// the field check (not the CRC check) is what rejects the package.
QByteArray patchHeader(QByteArray package, int offset, int width, quint32 value)
{
    if (width == 2)
        qToLittleEndian<quint16>(static_cast<quint16>(value), package.data() + offset);
    else
        qToLittleEndian<quint32>(value, package.data() + offset);
    qToLittleEndian<quint32>(CcPackage::crc32(package.left(28)), package.data() + 28);
    return package;
}

constexpr quint32 kTarget = 0x43433152;   // test_fw_package.py TARGET_ID
constexpr int kOffMagic = 0, kOffFormat = 4, kOffHeaderSize = 6, kOffAddress = 12, kOffSize = 16, kOffVersion = 20;

} // namespace

class tst_ccprotocol : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // ---- CRC16 ----
    void crc16_data();
    void crc16();
    void crc16ReferenceVector();

    // ---- COBS ----
    void cobsEncode_data();
    void cobsEncode();
    void cobsDecode_data();
    void cobsDecode();
    void cobsRejectsInvalidInput_data();
    void cobsRejectsInvalidInput();
    void cobsRoundTripWithZeroesAndLongRuns();
    void cobsBlockBoundaries();
    void cobsNeverEmitsAZero();

    // ---- frames ----
    void frameEncode_data();
    void frameEncode();
    void frameDecode_data();
    void frameDecode();
    void frameDecodeStripsTerminators();
    void frameRoundTripEmptyAndMaxDataPayloads();
    void frameRejections_data();
    void frameRejections();
    void frameRejectsCorruptedCrc();
    void frameRejectsBadVersionAndLength();
    void frameRejectsShortAndInvalidCobs();
    void messageNames();
    void frameEquality();

    // ---- FrameSplitter ----
    void splitterReadsConcatenatedFrames();
    void splitterAtEverySplitPoint();
    void splitterByteByByte();
    void splitterDropsGarbageBetweenFrames();
    void splitterJoinsGarbageWithoutZeroToTheNextFrame();
    void splitterKeepsPartialFramesAndClears();
    void splitterDropsEndlessNoise();

    // ---- INFO and names ----
    void parseInfoVector();
    void parseInfo24And20Bytes();
    void parseInfoRejectsOtherLengths_data();
    void parseInfoRejectsOtherLengths();
    void deviceInfoText();
    void deviceInfoTextWithUnknownValues();
    void nameHelpers();
    void roleParsing();

    // ---- CRC32 ----
    void crc32_data();
    void crc32();
    void crc32ContinuesFromASeed();

    // ---- packages ----
    void packageSmallVector();
    void packageSmallLoadSetsThePath();
    void packagePadded();
    void packageInvalidVectors_data();
    void packageInvalidVectors();
    void packageReleaseVector();
    void packageBuildReproducesTheSmallPackage();
    void roundTripPreservesMetadataAndPadsTheImage();
    void packageFormatsPackedVersions();
    void packageRejectsReservedVersionByte();
    void packageRejectsShortInput();
    void packageRejectsCorruptedHeaderCrc();
    void packageRejectsCorruptedImageCrc();
    void packageRejectsInvalidHeaderFieldsWithValidCrc_data();
    void packageRejectsInvalidHeaderFieldsWithValidCrc();
    void packageChecksRunInThePythonOrder();
    void packageRejectsLengthMismatches();
    void buildRejectsEmptyAndOversizedImages();
    void buildAcceptsMinimumAndMaximumAlignedImages();
    void loadReportsAnUnreadableFile();
    void parseWithoutOutputObject();

private:
    QJsonObject m_vectors;
    QByteArray m_small;
};

void tst_ccprotocol::initTestCase()
{
    QFile file(dataPath(QStringLiteral("vectors.json")));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
    m_vectors = QJsonDocument::fromJson(file.readAll()).object();
    QVERIFY(!m_vectors.isEmpty());
    QCOMPARE(m_vectors.value(QStringLiteral("protocol_version")).toInt(), int(kProtocolVersion));
    m_small = readFile(dataPath(QStringLiteral("package_small.pkg")));
    QCOMPARE(m_small.size(), 1032);
}

// ---------------------------------------------------------------------------------- CRC16

void tst_ccprotocol::crc16_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<int>("crc");
    int row = 0;
    for (const QJsonValue& entry : m_vectors.value(QStringLiteral("crc16")).toArray())
        QTest::addRow("vector %d", row++) << hex(entry.toObject().value(QStringLiteral("data")))
                                          << entry.toObject().value(QStringLiteral("crc")).toInt();
}

void tst_ccprotocol::crc16()
{
    QFETCH(QByteArray, data);
    QFETCH(int, crc);
    QCOMPARE(int(crc16Ccitt(data)), crc);
}

void tst_ccprotocol::crc16ReferenceVector()
{
    QCOMPARE(crc16Ccitt("123456789"), quint16(0x29B1));
    QCOMPARE(crc16Ccitt(QByteArray()), quint16(0xFFFF));   // the init value, no final xor
}

// ----------------------------------------------------------------------------------- COBS

void tst_ccprotocol::cobsEncode_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<QByteArray>("encoded");
    int row = 0;
    for (const QJsonValue& entry : m_vectors.value(QStringLiteral("cobs")).toArray())
        QTest::addRow("vector %d", row++) << hex(entry.toObject().value(QStringLiteral("data")))
                                          << hex(entry.toObject().value(QStringLiteral("encoded")));
}

void tst_ccprotocol::cobsEncode()
{
    QFETCH(QByteArray, data);
    QFETCH(QByteArray, encoded);
    QCOMPARE(CcProtocol::cobsEncode(data).toHex(), encoded.toHex());
}

void tst_ccprotocol::cobsDecode_data()
{
    cobsEncode_data();
}

void tst_ccprotocol::cobsDecode()
{
    QFETCH(QByteArray, data);
    QFETCH(QByteArray, encoded);
    QByteArray out = "stale";
    QVERIFY(CcProtocol::cobsDecode(encoded, &out));
    QCOMPARE(out.toHex(), data.toHex());
}

void tst_ccprotocol::cobsRejectsInvalidInput_data()
{
    QTest::addColumn<QByteArray>("encoded");
    int row = 0;
    for (const QJsonValue& entry : m_vectors.value(QStringLiteral("cobs_invalid")).toArray())
        QTest::addRow("vector %d", row++) << hex(entry);
    // test_fw_protocol.py: b"\x00", b"\x02", b"\x03\x01" (the same three), plus a zero inside a block
    QTest::addRow("zero code byte after a block") << QByteArray::fromHex("0211" "00");
    QTest::addRow("block runs past the end") << QByteArray::fromHex("ff0102");
}

void tst_ccprotocol::cobsRejectsInvalidInput()
{
    QFETCH(QByteArray, encoded);
    QByteArray out = "untouched";
    QVERIFY(!CcProtocol::cobsDecode(encoded, &out));
    QCOMPARE(out, QByteArray("untouched"));            // *out is written only on success
}

void tst_ccprotocol::cobsRoundTripWithZeroesAndLongRuns()
{
    // test_fw_protocol.py: b"\x00" + bytes(range(1, 255)) + b"\x00" + 512 random bytes
    QByteArray payload(1, '\0');
    for (int i = 1; i < 255; ++i)
        payload.append(static_cast<char>(i));
    payload.append('\0');
    QRandomGenerator generator(0xC0B5);
    for (int i = 0; i < 512; ++i)
        payload.append(static_cast<char>(generator.bounded(256)));
    QByteArray decoded;
    QVERIFY(CcProtocol::cobsDecode(CcProtocol::cobsEncode(payload), &decoded));
    QCOMPARE(decoded.toHex(), payload.toHex());
}

void tst_ccprotocol::cobsBlockBoundaries()
{
    // A full 254-byte block (code 0xFF) carries no implicit zero: round trip around the boundary.
    for (int length : {0, 1, 253, 254, 255, 508, 509, 762}) {
        QByteArray data(length, '\x5A');
        QByteArray decoded;
        QVERIFY2(CcProtocol::cobsDecode(CcProtocol::cobsEncode(data), &decoded), qPrintable(QString::number(length)));
        QCOMPARE(decoded, data);
    }
    QCOMPARE(CcProtocol::cobsEncode(QByteArray()), QByteArray("\x01", 1));
    QCOMPARE(CcProtocol::cobsEncode(QByteArray(254, '\x01')).size(), 256);   // 0xFF + 254 + final 0x01
}

void tst_ccprotocol::cobsNeverEmitsAZero()
{
    QRandomGenerator generator(7);
    for (int round = 0; round < 50; ++round) {
        QByteArray data;
        const int length = static_cast<int>(generator.bounded(600));
        for (int i = 0; i < length; ++i)
            data.append(static_cast<char>(generator.bounded(4) == 0 ? 0 : generator.bounded(256)));
        QVERIFY(!CcProtocol::cobsEncode(data).contains('\0'));
    }
}

// --------------------------------------------------------------------------------- frames

void tst_ccprotocol::frameEncode_data()
{
    QTest::addColumn<int>("type");
    QTest::addColumn<int>("sequence");
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<QByteArray>("wire");
    int row = 0;
    for (const QJsonValue& entry : m_vectors.value(QStringLiteral("frames")).toArray()) {
        const QJsonObject frame = entry.toObject();
        QTest::addRow("%s #%d", qPrintable(frame.value(QStringLiteral("type")).toString()), row++)
            << frame.value(QStringLiteral("type_id")).toInt() << frame.value(QStringLiteral("sequence")).toInt()
            << hex(frame.value(QStringLiteral("payload"))) << hex(frame.value(QStringLiteral("wire")));
    }
}

void tst_ccprotocol::frameEncode()
{
    QFETCH(int, type);
    QFETCH(int, sequence);
    QFETCH(QByteArray, payload);
    QFETCH(QByteArray, wire);
    QCOMPARE(encodeFrame(static_cast<quint8>(type), static_cast<quint16>(sequence), payload).toHex(), wire.toHex());
}

void tst_ccprotocol::frameDecode_data()
{
    frameEncode_data();
}

void tst_ccprotocol::frameDecode()
{
    QFETCH(int, type);
    QFETCH(int, sequence);
    QFETCH(QByteArray, payload);
    QFETCH(QByteArray, wire);
    Frame frame;
    QString error = QStringLiteral("stale");
    QVERIFY2(decodeFrame(wire, &frame, &error), qPrintable(error));
    QCOMPARE(int(frame.type), type);
    QCOMPARE(int(frame.sequence), sequence);
    QCOMPARE(frame.payload.toHex(), payload.toHex());
}

void tst_ccprotocol::frameDecodeStripsTerminators()
{
    const QByteArray wire = encodeFrame(Ack, 6, le32(128));
    Frame frame;
    QVERIFY(decodeFrame(wire.chopped(1), &frame));              // no terminator at all
    QCOMPARE(frame.type, quint8(Ack));
    QVERIFY(decodeFrame(wire + QByteArray(3, '\0'), &frame));   // several (bytes.rstrip(b"\0"))
    QCOMPARE(frame.sequence, quint16(6));
    QVERIFY(decodeFrame(wire, nullptr));                        // a null result pointer is allowed
}

void tst_ccprotocol::frameRoundTripEmptyAndMaxDataPayloads()
{
    QByteArray full;
    for (int i = 0; i < kDataChunkSize; ++i)
        full.append(static_cast<char>(i));
    for (const QByteArray& payload : {QByteArray(), full}) {
        const QByteArray wire = encodeFrame(Data, 0xFFFF, payload);
        QCOMPARE(wire.right(1), QByteArray(1, '\0'));
        QCOMPARE(wire.chopped(1).count('\0'), 0);               // the only zero is the terminator
        Frame frame;
        QVERIFY(decodeFrame(wire, &frame));
        QCOMPARE(frame.type, quint8(Data));
        QCOMPARE(frame.sequence, quint16(0xFFFF));
        QCOMPARE(frame.payload, payload);
    }
}

void tst_ccprotocol::frameRejections_data()
{
    QTest::addColumn<QByteArray>("wire");
    QTest::addColumn<QString>("reason");
    int row = 0;
    for (const QJsonValue& entry : m_vectors.value(QStringLiteral("invalid_frames")).toArray()) {
        const QJsonObject invalid = entry.toObject();
        QTest::addRow("vector %d (%s)", row++, qPrintable(invalid.value(QStringLiteral("reason")).toString()))
            << hex(invalid.value(QStringLiteral("wire"))) << invalid.value(QStringLiteral("reason")).toString();
    }
}

void tst_ccprotocol::frameRejections()
{
    QFETCH(QByteArray, wire);
    QFETCH(QString, reason);
    Frame frame;
    QString error;
    QVERIFY(!decodeFrame(wire, &frame, &error));
    QVERIFY2(error.contains(reason), qPrintable(error));
}

void tst_ccprotocol::frameRejectsCorruptedCrc()
{
    QByteArray raw = rawHello(9);
    raw[raw.size() - 1] = static_cast<char>(raw.at(raw.size() - 1) ^ 0x01);
    Frame frame;
    QString error;
    QVERIFY(!decodeFrame(wireFromRaw(raw), &frame, &error));
    QCOMPARE(error, QStringLiteral("frame CRC16 mismatch"));
}

void tst_ccprotocol::frameRejectsBadVersionAndLength()
{
    Frame frame;
    QString error;
    QByteArray raw = rawHello(3);
    raw[0] = static_cast<char>(kProtocolVersion + 1);
    fixCrc(&raw);
    QVERIFY(!decodeFrame(wireFromRaw(raw), &frame, &error));
    QCOMPARE(error, QStringLiteral("invalid frame header"));

    raw = rawHello(3);
    qToLittleEndian<quint16>(1, raw.data() + 4);                // claims one payload byte that is not there
    fixCrc(&raw);
    QVERIFY(!decodeFrame(wireFromRaw(raw), &frame, &error));
    QCOMPARE(error, QStringLiteral("invalid frame header"));
}

void tst_ccprotocol::frameRejectsShortAndInvalidCobs()
{
    Frame frame;
    QString error;
    QVERIFY(!decodeFrame(QByteArray(), &frame, &error));        // nothing but terminators is a short frame
    QCOMPARE(error, QStringLiteral("short frame"));
    QVERIFY(!decodeFrame(QByteArray(3, '\0'), &frame, &error));
    QCOMPARE(error, QStringLiteral("short frame"));
    QVERIFY(!decodeFrame(wireFromRaw(QByteArray(7, '\x11')), &frame, &error));   // one byte short of header + crc
    QCOMPARE(error, QStringLiteral("short frame"));
    QVERIFY(!decodeFrame(QByteArray::fromHex("0301" "00"), &frame, &error));
    QCOMPARE(error, QStringLiteral("invalid COBS frame"));
    QVERIFY(!decodeFrame(QByteArray::fromHex("0402010901010376dd00"), nullptr, nullptr));   // no out pointers
}

void tst_ccprotocol::messageNames()
{
    const QJsonObject names = m_vectors.value(QStringLiteral("message_types")).toObject();
    QCOMPARE(names.size(), 11);
    for (auto it = names.constBegin(); it != names.constEnd(); ++it)
        QCOMPARE(messageTypeName(static_cast<quint8>(it.value().toInt())), it.key());
    QCOMPARE(messageTypeName(0), QStringLiteral("type 0"));
    QCOMPARE(messageTypeName(12), QStringLiteral("type 12"));
    QCOMPARE(messageTypeName(255), QStringLiteral("type 255"));
}

void tst_ccprotocol::frameEquality()
{
    Frame a;
    a.type = Info;
    a.sequence = 4;
    a.payload = QByteArray("xy");
    Frame b = a;
    QVERIFY(a == b);
    b.sequence = 5;
    QVERIFY(a != b);
    b = a;
    b.payload = QByteArray("xz");
    QVERIFY(a != b);
    b = a;
    b.type = Ack;
    QVERIFY(a != b);
}

// ------------------------------------------------------------------------------ splitter

namespace {

QList<Frame> vectorFrames(const QJsonObject& vectors, QByteArray* stream)
{
    QList<Frame> frames;
    for (const QJsonValue& entry : vectors.value(QStringLiteral("frames")).toArray()) {
        const QJsonObject frame = entry.toObject();
        Frame parsed;
        parsed.type = static_cast<quint8>(frame.value(QStringLiteral("type_id")).toInt());
        parsed.sequence = static_cast<quint16>(frame.value(QStringLiteral("sequence")).toInt());
        parsed.payload = hex(frame.value(QStringLiteral("payload")));
        frames.append(parsed);
        stream->append(hex(frame.value(QStringLiteral("wire"))));
    }
    return frames;
}

} // namespace

void tst_ccprotocol::splitterReadsConcatenatedFrames()
{
    QByteArray stream;
    const QList<Frame> frames = vectorFrames(m_vectors, &stream);
    QCOMPARE(frames.size(), 12);
    FrameSplitter splitter;
    QCOMPARE(splitter.feed(stream), frames);
    QCOMPARE(splitter.invalidFrames(), 0);
    QVERIFY(splitter.pending().isEmpty());
}

void tst_ccprotocol::splitterAtEverySplitPoint()
{
    QByteArray stream;
    const QList<Frame> frames = vectorFrames(m_vectors, &stream);
    for (int cut = 0; cut <= stream.size(); ++cut) {
        FrameSplitter splitter;
        QList<Frame> got = splitter.feed(stream.left(cut));
        got += splitter.feed(stream.mid(cut));
        if (got != frames)
            QFAIL(qPrintable(QStringLiteral("split at byte %1 changed the result").arg(cut)));
        QCOMPARE(splitter.invalidFrames(), 0);
    }
}

void tst_ccprotocol::splitterByteByByte()
{
    QByteArray stream;
    const QList<Frame> frames = vectorFrames(m_vectors, &stream);
    FrameSplitter splitter;
    QList<Frame> got;
    for (char byte : stream)
        got += splitter.feed(QByteArray(1, byte));
    QCOMPARE(got, frames);
}

void tst_ccprotocol::splitterDropsGarbageBetweenFrames()
{
    const QByteArray first = encodeFrame(Hello, 1);
    const QByteArray second = encodeFrame(Ack, 2, le32(128));
    const QByteArray third = encodeFrame(Complete, 3);
    FrameSplitter splitter;
    // frame, a garbage chunk ended by its own zero, frame, idle zeros, frame
    const QList<Frame> got = splitter.feed(first + QByteArray("\x7f\x13\x55\x01", 4) + QByteArray(1, '\0')
                                           + second + QByteArray(3, '\0') + third);
    QCOMPARE(got.size(), 3);
    QCOMPARE(got.at(0).type, quint8(Hello));
    QCOMPARE(got.at(1).type, quint8(Ack));
    QCOMPARE(got.at(2).sequence, quint16(3));
    QCOMPARE(splitter.invalidFrames(), 1);                      // the idle zeros are not counted
    QVERIFY(splitter.pending().isEmpty());
}

void tst_ccprotocol::splitterJoinsGarbageWithoutZeroToTheNextFrame()
{
    // Like _read_frame(): without a zero in between, noise and the frame that follows are one chunk,
    // which does not decode, and both are dropped; the frame after that is fine again.
    FrameSplitter splitter;
    const QList<Frame> got = splitter.feed(QByteArray("noise") + encodeFrame(Hello, 1) + encodeFrame(Hello, 2));
    QCOMPARE(got.size(), 1);
    QCOMPARE(got.at(0).sequence, quint16(2));
    QCOMPARE(splitter.invalidFrames(), 1);
}

void tst_ccprotocol::splitterKeepsPartialFramesAndClears()
{
    const QByteArray wire = encodeFrame(Nack, 8, le32(256));
    FrameSplitter splitter;
    QVERIFY(splitter.feed(wire.left(5)).isEmpty());
    QCOMPARE(splitter.pending(), wire.left(5));
    const QList<Frame> got = splitter.feed(wire.mid(5));
    QCOMPARE(got.size(), 1);
    QCOMPARE(got.at(0).payload, le32(256));
    QVERIFY(splitter.pending().isEmpty());

    QVERIFY(splitter.feed(QByteArray("abc")).isEmpty());        // bytes after the last zero are held
    QCOMPARE(splitter.pending(), QByteArray("abc"));
    splitter.clear();                                           // what a reset settle does
    QVERIFY(splitter.pending().isEmpty());
    QCOMPARE(splitter.feed(wire).size(), 1);
}

void tst_ccprotocol::splitterDropsEndlessNoise()
{
    FrameSplitter splitter;
    QVERIFY(splitter.feed(QByteArray(80000, '\x41')).isEmpty());
    QVERIFY(splitter.pending().isEmpty());                      // a run that long without a zero is noise
    QCOMPARE(splitter.invalidFrames(), 1);
    QCOMPARE(splitter.feed(encodeFrame(Hello, 1)).size(), 1);   // and the stream recovers
}

// ----------------------------------------------------------------------------- INFO, names

void tst_ccprotocol::parseInfoVector()
{
    for (const QJsonValue& entry : m_vectors.value(QStringLiteral("frames")).toArray()) {
        const QJsonObject frame = entry.toObject();
        if (frame.value(QStringLiteral("type")).toString() != QLatin1String("INFO"))
            continue;
        DeviceInfo info;
        QVERIFY(parseInfo(hex(frame.value(QStringLiteral("payload"))), &info));
        QCOMPARE(info.target, 0x4343314Du);
        QCOMPARE(info.state, 1u);
        QCOMPARE(info.maxSize, 94208u);
        QCOMPARE(info.imageSize, 78040u);
        QCOMPARE(info.version, 0x801u);
        QVERIFY(info.role.has_value());
        QCOMPARE(*info.role, 1u);
        QCOMPARE(info.toString(), QStringLiteral("INFO target=CC1M(0x4343314D) state=valid_application(1) "
                                                  "max_size=94208 image_size=78040 version=0.8.1(0x00000801) role=rx(1)"));
        return;
    }
    QFAIL("no INFO frame in the vectors");
}

void tst_ccprotocol::parseInfo24And20Bytes()
{
    DeviceInfo info;
    QVERIFY(parseInfo(infoPayload(0x4343314D, 2, 0x17000, 0, 0x000800, RoleTx), &info));
    QCOMPARE(info.state, 2u);
    QCOMPARE(info.maxSize, 0x17000u);
    QCOMPARE(info.imageSize, 0u);
    QCOMPARE(info.version, 0x800u);
    QVERIFY(info.role.has_value());
    QCOMPARE(*info.role, quint32(RoleTx));

    DeviceInfo old;
    QVERIFY(parseInfo(infoPayload(0x43433152, 3, 94208, 0, 0), &old));   // a 20 byte INFO has no role
    QVERIFY(!old.role.has_value());
    QCOMPARE(old.target, 0x43433152u);
    QCOMPARE(old.toString(), QStringLiteral("INFO target=unknown(0x43433152) state=update_in_progress(3) "
                                             "max_size=94208 image_size=0 version=0.0.0(0x00000000)"));
    QVERIFY(parseInfo(infoPayload(1, 1, 1, 1, 1), nullptr));             // a null output is allowed

    DeviceInfo again = info;
    QVERIFY(again == info);
    again.role = RoleRx;
    QVERIFY(!(again == info));
    again = info;
    again.imageSize = 1;
    QVERIFY(!(again == info));
    again = info;
    again.role.reset();
    QVERIFY(!(again == info));
}

void tst_ccprotocol::parseInfoRejectsOtherLengths_data()
{
    QTest::addColumn<int>("length");
    for (int length : {0, 4, 19, 21, 23, 25, 28})
        QTest::addRow("%d bytes", length) << length;
}

void tst_ccprotocol::parseInfoRejectsOtherLengths()
{
    QFETCH(int, length);
    DeviceInfo info;
    info.target = 77;
    QVERIFY(!parseInfo(QByteArray(length, '\x01'), &info));
    QCOMPARE(info.target, 77u);                                 // not touched on failure
}

void tst_ccprotocol::deviceInfoText()
{
    DeviceInfo info;
    info.target = 0x4343314D;
    info.state = 2;
    info.maxSize = 94208;
    info.imageSize = 65280;
    info.version = 0x702;
    info.role = RoleTx;
    // The runbook's Part B line for a board B in the updater.
    QCOMPARE(info.toString(), QStringLiteral("INFO target=CC1M(0x4343314D) state=update_requested(2) "
                                              "max_size=94208 image_size=65280 version=0.7.2(0x00000702) role=tx(2)"));
}

void tst_ccprotocol::deviceInfoTextWithUnknownValues()
{
    DeviceInfo info;
    info.target = 0x12345678;
    info.state = 7;
    info.maxSize = 1;
    info.imageSize = 2;
    info.version = 0x01000000;                                  // reserved byte set: no version text
    info.role = 9;
    QCOMPARE(info.toString(), QStringLiteral("INFO target=unknown(0x12345678) state=unknown(7) max_size=1 "
                                              "image_size=2 version=unknown(0x01000000) role=unknown(9)"));
    info.role = RoleUnset;
    QVERIFY(info.toString().endsWith(QStringLiteral("role=unset(0)")));
}

void tst_ccprotocol::nameHelpers()
{
    QCOMPARE(formatVersion(0x00000801), QStringLiteral("0.8.1"));
    QCOMPARE(formatVersion((2u << 16) | (17u << 8) | 255u), QStringLiteral("2.17.255"));   // test_fw_package.py
    QCOMPARE(formatVersion(0), QStringLiteral("0.0.0"));
    QCOMPARE(formatVersion(0x00FFFFFF), QStringLiteral("255.255.255"));
    QCOMPARE(formatVersion(0x01000000), QStringLiteral("unknown"));
    QCOMPARE(bootStateName(1), QStringLiteral("valid_application"));
    QCOMPARE(bootStateName(2), QStringLiteral("update_requested"));
    QCOMPARE(bootStateName(3), QStringLiteral("update_in_progress"));
    QCOMPARE(bootStateName(0), QStringLiteral("unknown"));
    QCOMPARE(bootStateName(4), QStringLiteral("unknown"));
    QCOMPARE(roleName(0), QStringLiteral("unset"));
    QCOMPARE(roleName(1), QStringLiteral("rx"));
    QCOMPARE(roleName(2), QStringLiteral("tx"));
    QCOMPARE(roleName(3), QStringLiteral("unknown"));
    QCOMPARE(targetName(0x4343314D), QStringLiteral("CC1M"));
    QCOMPARE(targetName(0x43433152), QStringLiteral("unknown"));
    QCOMPARE(formatEnum(1, QStringLiteral("valid_application")), QStringLiteral("valid_application(1)"));
    QCOMPARE(formatEnum(99, QStringLiteral("unknown")), QStringLiteral("unknown(99)"));
    QCOMPARE(formatTarget(0x4343314D), QStringLiteral("CC1M(0x4343314D)"));
    QCOMPARE(formatTarget(0xABC), QStringLiteral("unknown(0x00000ABC)"));   // the name keeps its case
}

void tst_ccprotocol::roleParsing()
{
    QCOMPARE(parseRole(QStringLiteral("rx")), std::optional<quint32>(1));
    QCOMPARE(parseRole(QStringLiteral("tx")), std::optional<quint32>(2));
    QCOMPARE(parseRole(QStringLiteral("RX")), std::optional<quint32>(1));
    QCOMPARE(parseRole(QStringLiteral("Tx")), std::optional<quint32>(2));
    QVERIFY(!parseRole(QStringLiteral("")).has_value());
    QVERIFY(!parseRole(QStringLiteral("both")).has_value());
    QVERIFY(!parseRole(QStringLiteral("unset")).has_value());
}

// ----------------------------------------------------------------------------------- CRC32

void tst_ccprotocol::crc32_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<qlonglong>("crc");
    int row = 0;
    for (const QJsonValue& entry : m_vectors.value(QStringLiteral("crc32")).toArray())
        QTest::addRow("vector %d", row++) << hex(entry.toObject().value(QStringLiteral("data")))
                                          << static_cast<qlonglong>(entry.toObject().value(QStringLiteral("crc")).toDouble());
}

void tst_ccprotocol::crc32()
{
    QFETCH(QByteArray, data);
    QFETCH(qlonglong, crc);
    QCOMPARE(static_cast<qlonglong>(CcPackage::crc32(data)), crc);
}

void tst_ccprotocol::crc32ContinuesFromASeed()
{
    QCOMPARE(CcPackage::crc32("123456789"), 0xCBF43926u);       // the standard check value
    // zlib.crc32(b, crc32(a)) == crc32(a + b)
    const QByteArray a("The quick brown fox ");
    const QByteArray b("jumps over the lazy dog");
    QCOMPARE(CcPackage::crc32(b, CcPackage::crc32(a)), CcPackage::crc32(a + b));
    QCOMPARE(CcPackage::crc32(QByteArray(), 0x12345678u), 0x12345678u);
}

// -------------------------------------------------------------------------------- packages

void tst_ccprotocol::packageSmallVector()
{
    const QJsonObject expected = m_vectors.value(QStringLiteral("package_small")).toObject();
    CcPackage package;
    QString error;
    QVERIFY2(CcPackage::parse(m_small, &package, &error), qPrintable(error));
    QVERIFY(package.isValid());
    QCOMPARE(m_small.left(4), QByteArray("FWPK"));
    QCOMPARE(CcPackage::kMagic, 0x4B505746u);
    QCOMPARE(package.target, static_cast<quint32>(expected.value(QStringLiteral("target")).toDouble()));
    QCOMPARE(package.address, static_cast<quint32>(expected.value(QStringLiteral("address")).toDouble()));
    QCOMPARE(package.address, CcPackage::kAppAddress);
    QCOMPARE(package.size, static_cast<quint32>(expected.value(QStringLiteral("size")).toDouble()));
    QCOMPARE(package.version, static_cast<quint32>(expected.value(QStringLiteral("version")).toDouble()));
    QCOMPARE(package.versionText(), expected.value(QStringLiteral("version_text")).toString());
    QCOMPARE(package.imageCrc, static_cast<quint32>(expected.value(QStringLiteral("image_crc")).toDouble()));
    QCOMPARE(package.headerCrc, static_cast<quint32>(expected.value(QStringLiteral("header_crc")).toDouble()));
    QCOMPARE(package.header, m_small.left(32));
    QCOMPARE(package.header.size(), 32);
    QCOMPARE(package.image, m_small.mid(32));
    QCOMPARE(package.image.size(), 1000);
    QCOMPARE(package.image.left(16).toHex(), expected.value(QStringLiteral("image_sha_prefix")).toString().toLatin1());
    QCOMPARE(package.targetText(), QStringLiteral("CC1M(0x4343314D)"));
    QCOMPARE(package.summary(), QStringLiteral("INFO package target=0x4343314D version=0.8.1 code=0x00000801 size=1000"));
}

void tst_ccprotocol::packageSmallLoadSetsThePath()
{
    const QString path = dataPath(QStringLiteral("package_small.pkg"));
    CcPackage package;
    QString error;
    QVERIFY2(CcPackage::load(path, &package, &error), qPrintable(error));
    QCOMPARE(package.filePath, path);
    QCOMPARE(package.size, 1000u);
    QCOMPARE(package.header + package.image, m_small);
    CcPackage fromBytes;
    QVERIFY(CcPackage::parse(m_small, &fromBytes));
    QVERIFY(fromBytes.filePath.isEmpty());                      // only load() sets it
    QVERIFY(!CcPackage().isValid());
}

void tst_ccprotocol::packagePadded()
{
    const QJsonObject expected = m_vectors.value(QStringLiteral("package_padded")).toObject();
    QString error;
    const QByteArray built = CcPackage::build(QByteArray("abcde"), 0x4343314D, 109, &error);
    QVERIFY2(!built.isEmpty(), qPrintable(error));
    QCOMPARE(built.toHex(), hex(expected.value(QStringLiteral("bytes"))).toHex());
    CcPackage package;
    QVERIFY2(CcPackage::parse(built, &package, &error), qPrintable(error));
    QCOMPARE(package.image.toHex(), hex(expected.value(QStringLiteral("image"))).toHex());
    QCOMPARE(package.size, static_cast<quint32>(expected.value(QStringLiteral("size")).toInt()));
    QCOMPARE(package.version, static_cast<quint32>(expected.value(QStringLiteral("version")).toInt()));
}

void tst_ccprotocol::packageInvalidVectors_data()
{
    QTest::addColumn<QByteArray>("whole");
    QTest::addColumn<QString>("reason");
    int row = 0;
    for (const QJsonValue& entry : m_vectors.value(QStringLiteral("package_invalid")).toArray()) {
        const QJsonObject invalid = entry.toObject();
        QTest::addRow("vector %d (%s)", row++, qPrintable(invalid.value(QStringLiteral("reason")).toString()))
            << hex(invalid.value(QStringLiteral("whole"))) << invalid.value(QStringLiteral("reason")).toString();
    }
}

void tst_ccprotocol::packageInvalidVectors()
{
    QFETCH(QByteArray, whole);
    QFETCH(QString, reason);
    CcPackage package;
    package.target = 99;
    QString error;
    QVERIFY(!CcPackage::parse(whole, &package, &error));
    QVERIFY2(error.contains(reason), qPrintable(error));
    QVERIFY(!package.isValid());                                // *package is written only on success
    QCOMPARE(package.target, 99u);
}

void tst_ccprotocol::packageReleaseVector()
{
    const QJsonObject expected = m_vectors.value(QStringLiteral("package_release")).toObject();
    CcPackage package;
    QString error;
    QVERIFY2(CcPackage::load(dataPath(expected.value(QStringLiteral("file")).toString()), &package, &error),
             qPrintable(error));
    QCOMPARE(package.header.size() + package.image.size(), expected.value(QStringLiteral("bytes")).toInt());
    QCOMPARE(package.header.size() + package.image.size(), 78072);
    QCOMPARE(package.size, 78040u);
    QCOMPARE(int(package.size), expected.value(QStringLiteral("size")).toInt());
    QCOMPARE(package.versionText(), QStringLiteral("0.8.1"));
    QCOMPARE(package.versionText(), expected.value(QStringLiteral("version_text")).toString());
    QCOMPARE(package.imageCrc, static_cast<quint32>(expected.value(QStringLiteral("image_crc")).toDouble()));
    QCOMPARE(package.target, 0x4343314Du);
    QCOMPARE(package.summary(), QStringLiteral("INFO package target=0x4343314D version=0.8.1 code=0x00000801 size=78040"));
}

void tst_ccprotocol::packageBuildReproducesTheSmallPackage()
{
    CcPackage package;
    QVERIFY(CcPackage::parse(m_small, &package));
    QString error;
    const QByteArray rebuilt = CcPackage::build(package.image, package.target, package.version, &error);
    QVERIFY2(!rebuilt.isEmpty(), qPrintable(error));
    QCOMPARE(rebuilt.toHex(), m_small.toHex());                 // bit for bit what the Python builder wrote
}

void tst_ccprotocol::roundTripPreservesMetadataAndPadsTheImage()
{
    // test_fw_package.py test_round_trip_preserves_metadata_and_pads_image
    const QByteArray bytes = CcPackage::build(QByteArray("abcde"), kTarget, 109);
    CcPackage parsed;
    QString error;
    QVERIFY2(CcPackage::parse(bytes, &parsed, &error), qPrintable(error));
    QCOMPARE(parsed.target, kTarget);
    QCOMPARE(parsed.address, CcPackage::kAppAddress);
    QCOMPARE(parsed.version, 109u);
    QCOMPARE(parsed.image, QByteArray("abcde\xff\xff\xff", 8));
    QCOMPARE(parsed.size, 8u);
    QCOMPARE(bytes.size(), 32 + 8);
}

void tst_ccprotocol::packageFormatsPackedVersions()
{
    CcPackage package;
    QVERIFY(CcPackage::parse(CcPackage::build(QByteArray(4, 'x'), kTarget, (2u << 16) | (17u << 8) | 255u), &package));
    QCOMPARE(package.versionText(), QStringLiteral("2.17.255"));
    QVERIFY(package.summary().contains(QStringLiteral("version=2.17.255 code=0x000211FF")));
}

void tst_ccprotocol::packageRejectsReservedVersionByte()
{
    QString error;
    QVERIFY(CcPackage::build(QByteArray("test"), kTarget, 0x01000000, &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("most-significant byte")), qPrintable(error));
    // parse() too, even with a header CRC that matches the doctored version
    const QByteArray good = CcPackage::build(QByteArray("test"), kTarget, 7);
    CcPackage package;
    QVERIFY(!CcPackage::parse(patchHeader(good, kOffVersion, 4, 0x01000007), &package, &error));
    QVERIFY2(error.contains(QStringLiteral("most-significant byte")), qPrintable(error));
}

void tst_ccprotocol::packageRejectsShortInput()
{
    CcPackage package;
    QString error;
    QVERIFY(!CcPackage::parse(QByteArray(), &package, &error));
    QCOMPARE(error, QStringLiteral("package is shorter than its header"));
    QVERIFY(!CcPackage::parse(m_small.left(31), &package, &error));
    QCOMPARE(error, QStringLiteral("package is shorter than its header"));
}

void tst_ccprotocol::packageRejectsCorruptedHeaderCrc()
{
    QByteArray bytes = CcPackage::build(QByteArray("\x00\x01\x02\x03", 4), kTarget, 7);
    bytes[31] = static_cast<char>(bytes.at(31) ^ 0x01);         // the last byte of the header
    CcPackage package;
    QString error;
    QVERIFY(!CcPackage::parse(bytes, &package, &error));
    QVERIFY2(error.contains(QStringLiteral("header CRC32")), qPrintable(error));
}

void tst_ccprotocol::packageRejectsCorruptedImageCrc()
{
    QByteArray bytes = CcPackage::build(QByteArray("\x00\x01\x02\x03", 4), kTarget, 7);
    bytes[bytes.size() - 1] = static_cast<char>(bytes.at(bytes.size() - 1) ^ 0x01);
    CcPackage package;
    QString error;
    QVERIFY(!CcPackage::parse(bytes, &package, &error));
    QVERIFY2(error.contains(QStringLiteral("image CRC32")), qPrintable(error));
}

void tst_ccprotocol::packageRejectsInvalidHeaderFieldsWithValidCrc_data()
{
    QTest::addColumn<int>("offset");
    QTest::addColumn<int>("width");
    QTest::addColumn<quint32>("value");
    QTest::addColumn<QString>("reason");
    // test_fw_package.py test_rejects_invalid_header_fields_even_with_valid_header_crc
    QTest::newRow("magic 0") << kOffMagic << 4 << 0u << QStringLiteral("unsupported package header");
    QTest::newRow("format 2") << kOffFormat << 2 << 2u << QStringLiteral("unsupported package header");
    QTest::newRow("header size 0") << kOffHeaderSize << 2 << 0u << QStringLiteral("unsupported package header");
    QTest::newRow("address 0") << kOffAddress << 4 << 0u << QStringLiteral("invalid application address");
    QTest::newRow("address 0x8004") << kOffAddress << 4 << 0x8004u << QStringLiteral("invalid application address");
    QTest::newRow("size 0") << kOffSize << 4 << 0u << QStringLiteral("invalid application address");
    QTest::newRow("size max+1") << kOffSize << 4 << (CcPackage::kMaxImageSize + 1)
                                << QStringLiteral("invalid application address");
}

void tst_ccprotocol::packageRejectsInvalidHeaderFieldsWithValidCrc()
{
    QFETCH(int, offset);
    QFETCH(int, width);
    QFETCH(quint32, value);
    QFETCH(QString, reason);
    const QByteArray good = CcPackage::build(QByteArray("\x00\x01\x02\x03", 4), kTarget, 7);
    CcPackage package;
    QString error;
    QVERIFY(!CcPackage::parse(patchHeader(good, offset, width, value), &package, &error));
    QVERIFY2(error.contains(reason), qPrintable(error));
}

void tst_ccprotocol::packageChecksRunInThePythonOrder()
{
    const QByteArray good = CcPackage::build(QByteArray(8, 'x'), kTarget, 7);
    CcPackage package;
    QString error;
    // magic is wrong AND the header CRC no longer matches: the header check speaks first
    QByteArray bad = good;
    bad[0] = 0;
    QVERIFY(!CcPackage::parse(bad, &package, &error));
    QCOMPARE(error, QStringLiteral("unsupported package header"));
    // header fields invalid AND the image is the wrong length: the field check speaks first
    QVERIFY(!CcPackage::parse(patchHeader(good, kOffAddress, 4, 0).chopped(1), &package, &error));
    QCOMPARE(error, QStringLiteral("invalid application address or size"));
    // length wrong AND the image CRC wrong: the length speaks first
    QByteArray both = good.chopped(1);
    both[both.size() - 1] = static_cast<char>(both.at(both.size() - 1) ^ 0x55);
    QVERIFY(!CcPackage::parse(both, &package, &error));
    QCOMPARE(error, QStringLiteral("package length does not match header image size"));
}

void tst_ccprotocol::packageRejectsLengthMismatches()
{
    const QByteArray good = CcPackage::build(QByteArray("\x00\x01\x02\x03", 4), kTarget, 7);
    CcPackage package;
    QString error;
    QVERIFY(!CcPackage::parse(good.chopped(1), &package, &error));        // one byte short
    QVERIFY2(error.contains(QStringLiteral("package length")), qPrintable(error));
    QVERIFY(!CcPackage::parse(good + QByteArray(1, '\xFF'), &package, &error));   // appended bytes
    QVERIFY2(error.contains(QStringLiteral("package length")), qPrintable(error));
}

void tst_ccprotocol::buildRejectsEmptyAndOversizedImages()
{
    QString error;
    QVERIFY(CcPackage::build(QByteArray(), kTarget, 1, &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("outside the App slot")), qPrintable(error));
    error.clear();
    QVERIFY(CcPackage::build(QByteArray(CcPackage::kMaxImageSize + 1, 'x'), kTarget, 1, &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("outside the App slot")), qPrintable(error));
    error.clear();
    // 0x16FFD bytes pad to 0x17000 and fit; 0x17001 pads to 0x17004 and does not
    QVERIFY(!CcPackage::build(QByteArray(CcPackage::kMaxImageSize - 3, 'x'), kTarget, 1, &error).isEmpty());
    QVERIFY(CcPackage::build(QByteArray(CcPackage::kMaxImageSize + 1, 'x'), kTarget, 1, nullptr).isEmpty());
}

void tst_ccprotocol::buildAcceptsMinimumAndMaximumAlignedImages()
{
    CcPackage package;
    QString error;
    QVERIFY2(CcPackage::parse(CcPackage::build(QByteArray(4, '\xFF'), kTarget, 7), &package, &error), qPrintable(error));
    QCOMPARE(package.size, 4u);
    QVERIFY2(CcPackage::parse(CcPackage::build(QByteArray(int(CcPackage::kMaxImageSize), '\xFF'), kTarget, 8),
                              &package, &error), qPrintable(error));
    QCOMPARE(package.size, CcPackage::kMaxImageSize);
    QCOMPARE(package.image.size(), int(CcPackage::kMaxImageSize));
}

void tst_ccprotocol::loadReportsAnUnreadableFile()
{
    CcPackage package;
    QString error;
    QVERIFY(!CcPackage::load(dataPath(QStringLiteral("no-such-file.pkg")), &package, &error));
    QVERIFY2(error.contains(QStringLiteral("no-such-file.pkg")), qPrintable(error));
    QVERIFY(!package.isValid());
    QVERIFY(!CcPackage::load(dataPath(QStringLiteral("vectors.json")), &package, &error));   // not a package
    QVERIFY2(error.contains(QStringLiteral("unsupported package header")), qPrintable(error));
}

void tst_ccprotocol::parseWithoutOutputObject()
{
    QVERIFY(CcPackage::parse(m_small, nullptr, nullptr));
    QVERIFY(!CcPackage::parse(QByteArray(10, 'x'), nullptr, nullptr));
}

QTEST_GUILESS_MAIN(tst_ccprotocol)
#include "tst_ccprotocol.moc"
