#include "ssh/SecretStore.h"

#include "app/Logging.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QSettings>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <wincrypt.h>
#else
#include <QRandomGenerator>
#endif

// Secrets live in QSettings under the group "secrets". The caller's free-form key ("ssh/<id>/
// password") is encoded base64url (no padding) so that neither "/" (a QSettings group
// separator) nor any other character has a meaning to QSettings or to the INI/registry backend.
//
// Windows: the value is a DPAPI blob (CryptProtectData, current user, application entropy,
// no UI), stored base64. Any other Windows user - and the same user on another machine -
// cannot decrypt it.
//
// Other platforms: the value is the UTF-8 secret XORed with a 32-byte per-installation key
// (QRandomGenerator::system(), created once and stored base64 at "secrets/_key"). This is
// obfuscation only: anybody who can read the settings file can also read the key.

namespace {

const char kGroup[] = "secrets";
const char kKeyEntry[] = "_key";

QString settingsKey(const QString& key)
{
    const QByteArray encoded = key.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QLatin1String(kGroup) + QLatin1Char('/') + QString::fromLatin1(encoded);
}

#if defined(Q_OS_WIN)

const char kEntropy[] = "BuildAI-SerialUtility-SecretStore-v1";

DATA_BLOB entropyBlob()
{
    DATA_BLOB blob;
    blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    blob.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);
    return blob;
}

std::optional<QByteArray> protect(const QByteArray& plain)
{
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"BuildAI Serial Utility", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        const DWORD lastError = GetLastError();
        qCWarning(lcSsh) << "CryptProtectData failed:" << lastError;
        return std::nullopt;
    }
    QByteArray blob(reinterpret_cast<const char*>(out.pbData), static_cast<qsizetype>(out.cbData));
    LocalFree(out.pbData);
    return blob;
}

std::optional<QByteArray> unprotect(const QByteArray& blob)
{
    if (blob.isEmpty()) {
        return std::nullopt;
    }
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(blob.constData()));
    in.cbData = static_cast<DWORD>(blob.size());
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        const DWORD lastError = GetLastError();
        qCWarning(lcSsh) << "CryptUnprotectData failed:" << lastError;
        return std::nullopt;
    }
    QByteArray plain(reinterpret_cast<const char*>(out.pbData), static_cast<qsizetype>(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

#else

constexpr int kObfuscationKeyBytes = 32;

QByteArray obfuscationKey(QSettings& settings)
{
    const QString entry = QLatin1String(kGroup) + QLatin1Char('/') + QLatin1String(kKeyEntry);
    QByteArray key = QByteArray::fromBase64(settings.value(entry).toString().toLatin1());
    if (key.size() == kObfuscationKeyBytes) {
        return key;
    }
    key.resize(kObfuscationKeyBytes);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(key.data()),
                                          kObfuscationKeyBytes / static_cast<int>(sizeof(quint32)));
    settings.setValue(entry, QString::fromLatin1(key.toBase64()));
    settings.sync();
    return key;
}

QByteArray xorWithKey(const QByteArray& data, const QByteArray& key)
{
    QByteArray out = data;
    for (qsizetype i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(out.at(i) ^ key.at(i % key.size()));
    }
    return out;
}

#endif

} // namespace

namespace SecretStore {

bool isSecure()
{
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

bool store(const QString& key, const QString& secret)
{
    if (key.isEmpty()) {
        return false;
    }
    if (secret.isEmpty()) {
        return remove(key);
    }
    QSettings settings;
    const QByteArray plain = secret.toUtf8();
#if defined(Q_OS_WIN)
    const std::optional<QByteArray> blob = protect(plain);
    if (!blob) {
        return false;
    }
    settings.setValue(settingsKey(key), QString::fromLatin1(blob->toBase64()));
#else
    const QByteArray key32 = obfuscationKey(settings);
    settings.setValue(settingsKey(key), QString::fromLatin1(xorWithKey(plain, key32).toBase64()));
#endif
    settings.sync();
    qCDebug(lcSsh) << "secret stored for" << key;
    return settings.status() == QSettings::NoError;
}

std::optional<QString> load(const QString& key)
{
    if (key.isEmpty()) {
        return std::nullopt;
    }
    QSettings settings;
    const QString entry = settingsKey(key);
    if (!settings.contains(entry)) {
        return std::nullopt;
    }
    const QByteArray stored = QByteArray::fromBase64(settings.value(entry).toString().toLatin1());
    if (stored.isEmpty()) {
        return std::nullopt;
    }
#if defined(Q_OS_WIN)
    const std::optional<QByteArray> plain = unprotect(stored);
    if (!plain) {
        return std::nullopt;
    }
    return QString::fromUtf8(*plain);
#else
    const QByteArray key32 = obfuscationKey(settings);
    return QString::fromUtf8(xorWithKey(stored, key32));
#endif
}

bool remove(const QString& key)
{
    if (key.isEmpty()) {
        return false;
    }
    QSettings settings;
    const QString entry = settingsKey(key);
    const bool existed = settings.contains(entry);
    settings.remove(entry);
    settings.sync();
    if (existed) {
        qCDebug(lcSsh) << "secret removed for" << key;
    }
    return existed;
}

bool contains(const QString& key)
{
    if (key.isEmpty()) {
        return false;
    }
    QSettings settings;
    return settings.contains(settingsKey(key));
}

QString storageDescription()
{
#if defined(Q_OS_WIN)
    return QCoreApplication::translate("SecretStore", "Stored with Windows Data Protection (DPAPI) for the current user");
#else
    return QCoreApplication::translate("SecretStore", "Obfuscated in the settings file - not encrypted");
#endif
}

} // namespace SecretStore
