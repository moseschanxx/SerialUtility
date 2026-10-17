#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

/**
 * What the tool knows about one board, keyed by its ADB serial (v0.8.0; docs/DESIGN.md 4.17): the
 * bench inventory an agent reads before it flashes ("which image does board X run?") and the owner
 * reads after ("what happened to each board?"). Filled by InventoryRecorder from what the
 * operations report; nothing in here is measured by the inventory itself. Every field keeps its
 * last known value: a record is only ever extended, a field is never cleared by a later operation
 * that did not learn it. No secret is ever stored (no password, no host key, no path outside the
 * image's file name).
 *
 * JSON (one object per board under "boards", the key and "serial" equal; times ISO 8601 with
 * offset; missing = never learned):
 *   {"serial":"0225ce3c8d8d6e44","profile":"node","port":"6&38635FDB&0&3",
 *    "layout":"sfc_nor:64K(env),...","gadgetSerialString":"no",
 *    "lastImage":{"file":"update.img","chipTag":"110E","md5":"9e107d9d372bb6826bd81d3542a419d6",
 *                 "gitDescribe":"v1.3.0-4-ge0e0d41","partitions":["rootfs"],
 *                 "written":"2026-10-16T22:10:31+08:00"},
 *    "mcu":{"version":"0.8.1","role":"rx","read":"2026-10-16T22:11:02+08:00"},
 *    "lastOperation":{"name":"flash","result":"done","message":"Upgrade firmware ok; board is back on ADB",
 *                     "time":"2026-10-16T22:10:31+08:00"},
 *    "firstSeen":"2026-10-14T12:53:10+08:00","lastSeen":"2026-10-16T22:11:02+08:00"}
 */
struct InventoryRecord
{
    enum class Tri { Unknown, Yes, No };

    /// The image last written to the board by a write that ended ok (a flash step that ended ok, a
    /// fleet board Done or NotBack) and WHICH of its rows that write put on the board - never a
    /// failed or cancelled write. `partitions` is the truth an agent needs: a daily `--partitions
    /// rootfs` (Node) / `boot` (G2) puts one row of the image on a board that still runs the old
    /// build in the others, and `--expect-image` must not call that "runs the image" (FirmwareCli.h).
    /// A later ok write of the SAME md5 merges its rows into `partitions` (order of first
    /// appearance, no duplicates) and refreshes `written`; a write of a DIFFERENT md5 replaces the
    /// whole Image (its rows only). The rows are the write's own: the rows the single-board flow
    /// wrote completely ((fix4, m4) SocFlasher::writtenPartitions(), not the selection), every
    /// row of the image for a UF (MASKROM) write, FleetBoard::written for a fleet board.
    struct Image
    {
        QString file;           ///< the file name only ("update.img", "phaselock_fix2_update.img")
        QString chipTag;        ///< the RKFW header's chip tag ("110E" / "1106"); "" when unreadable
        QString md5;            ///< lower-case hex MD5 of the whole image file (Inventory::md5Hex())
        QString gitDescribe;    ///< the cache note's git describe when the image is the cache's; "" otherwise
        QStringList partitions; ///< (JSON "partitions") the rows of this image written to the board, merged over ok writes of the same md5
        QDateTime written;      ///< when the (last) write ended
        bool isValid() const { return !md5.isEmpty(); }
        bool operator==(const Image& other) const;
    };

    /// The last operation that touched the board, whatever its result. A dry run touches nothing:
    /// a fleet board Ready (FleetBoard::Result::Ready) is never recorded here (InventoryRecorder.h).
    struct Operation
    {
        QString name;           ///< FirmwareJob's kOp* name ("flash", "mcu", "set-role", "info", "pull-flash", "fleet-flash")
        QString result;         ///< "done" / "failed" / "cancelled" (a fleet board: FleetBoard::resultText() - "done" / "not back" / "failed" / "skipped"; never "ready" / "pending")
        QString message;        ///< the operation's result message (a fleet board: its detail)
        QDateTime time;
        bool operator==(const Operation& other) const;
    };

    QString serial;             ///< the ADB serial: the key
    QString profile;            ///< BoardProfile::id ("node" / "stacked") from a DETECTION whose evidence is the board's (its Loader PID, product string or flash layout) - never the --profile / Board-profile-box override, which is the operator's claim; "" = never detected
    QString port;               ///< UsbDeviceInfo::portKey() learned by a fleet run; "" = never
    QString layout;             ///< the mtdparts read from the board (normalized); "" = never
    Tri gadgetSerialString = Tri::Unknown;   ///< does the ADB gadget carry a USB serial string (its instance id = the serial) or a bus address (the Node, open question 5)
    Image lastImage;
    QString mcuVersion;         ///< the CC1310 version last read ("0.8.1"); "" = never
    QString mcuRole;            ///< "rx" / "tx" / "unknown"; "" = never
    QDateTime mcuRead;          ///< when the MCU was last read
    Operation lastOperation;
    QDateTime firstSeen;        ///< the first record
    QDateTime lastSeen;         ///< the last status that showed the board on ADB, or its last operation

    bool isValid() const { return !serial.isEmpty(); }
    QJsonObject toJson() const;
    /// A record from its object; `serial` wins over the object's "serial" (the key is the truth).
    /// Unknown fields are ignored, missing ones stay at their defaults, a time that does not parse
    /// is an invalid QDateTime.
    static InventoryRecord fromJson(const QString& serial, const QJsonObject& object);
    static QString triText(Tri value);           ///< "unknown" / "yes" / "no"
    static Tri triFromText(const QString& text); ///< the reverse; anything else Unknown
    bool operator==(const InventoryRecord& other) const;
};

/**
 * The inventory file: `<AppSettings::dataDirectory()>/inventory.json`, shared by the Firmware tab
 * and every su-fw on this PC (never between PCs, never over the network):
 *   {"version":1,"updated":"2026-10-16T22:11:02+08:00","boards":{"<serial>":{...}, ...}}
 *
 * Writers never clobber each other: update() takes a QLockFile next to the file
 * (`inventory.json.lock`, waited for up to kLockWaitMs; a lock left by a dead process is stale and
 * taken over, as FirmwareJob's firmware.lock), re-reads the file under the lock, applies the change
 * to that fresh copy and writes the whole file through QSaveFile (a temporary next to it, renamed
 * into place: a reader never sees half a file; a crash leaves the old file). Two su-fw processes
 * ending at once therefore both land, in some order, with nothing lost.
 *
 * A corrupt file (not JSON, no object, "boards" not an object) is never silently overwritten:
 * load() reports it (false, *error = "<path>: <reason>") and records() is empty; the first update()
 * after that moves it aside to `inventory.json.corrupt-<yyyyMMdd-HHmmss>` (movedAsideFile()) and
 * starts a fresh file holding the one record - the owner logs the move. A file with a "version"
 * above kVersion is still read (the known fields of every record); update() rewrites every record
 * through toJson(), so fields a newer tool added are dropped - accepted in 0.8.0 and said here. A
 * missing file is an empty inventory (load() true).
 *
 * Pure helpers for the CLI and the tests: tableLines() (the `su-fw inventory` table, header
 * "serial  profile  port  image  tag  md5  written  mcu  last operation  result  time  last seen",
 * widths from the content, two blanks between columns, "-" for an unknown cell, md5 shortened to
 * its first 8 hex digits, times as "yyyy-MM-dd HH:mm"), json() (the `su-fw inventory --json`
 * done fields: "inventory": {"file": <native path>, "boards": [toJson() each, serial order]}),
 * md5Hex() (the whole file, streamed; "" with *error for a file that cannot be read).
 */
class Inventory
{
public:
    static constexpr const char* kFileName = "inventory.json";
    static constexpr int kVersion = 1;
    static constexpr int kLockWaitMs = 3000;

    explicit Inventory(const QString& filePath = QString());   ///< "" = defaultFilePath(); nothing is read until load() / update()
    ~Inventory();

    static QString defaultFilePath();                          ///< AppSettings::dataDirectory() + "/inventory.json"
    QString filePath() const;

    /// Re-read the file: true with the records (a missing file: true, none); false and *error for
    /// a corrupt or unreadable file (records() empty then).
    bool load(QString* error = nullptr);
    QList<InventoryRecord> records() const;                    ///< as of the last load() / update(), serial order
    std::optional<InventoryRecord> record(const QString& serial) const;

    using Change = std::function<void(InventoryRecord& record)>;
    /// Lock, re-read, apply `change` to the record of `serial` (a new one gets serial and firstSeen
    /// = now; every call sets lastSeen = now unless the change set it), write atomically, refresh
    /// records(). False with *error when the lock could not be taken in time, the folder cannot be
    /// created, or the file cannot be written; a corrupt file is moved aside first (movedAsideFile()).
    bool update(const QString& serial, const Change& change, QString* error = nullptr);
    QString movedAsideFile() const;                            ///< the corrupt file's new name after the last update(); "" = none

    static QStringList tableLines(const QList<InventoryRecord>& records);
    static QJsonObject json(const QList<InventoryRecord>& records, const QString& filePath);
    static QString md5Hex(const QString& filePath, QString* error = nullptr);
    static QString isoTime(const QDateTime& time);             ///< Qt::ISODate with the local offset; "" for an invalid time
    static QDateTime timeFromIso(const QString& text);         ///< the reverse; invalid when it does not parse

private:
    struct Private;
    std::unique_ptr<Private> d;
};
