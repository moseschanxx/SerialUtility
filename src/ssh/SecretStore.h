#pragma once

#include <QString>
#include <optional>

/**
 * Small per-user secret storage for saved SSH passwords and key passphrases.
 *
 *  - Windows: DPAPI (CryptProtectData with the current user's credentials and an
 *             application-specific entropy) -> base64 in QSettings group "secrets".
 *             Only the same Windows user on the same machine can decrypt. isSecure() == true.
 *  - Other platforms: the value is obfuscated (XOR with a per-installation random key stored
 *             in QSettings) -> base64 in QSettings. This only prevents casual reading of the
 *             settings file; isSecure() == false and the UI says so next to "Save password".
 *
 * Keys are free-form strings; SshConnection uses "ssh/<profileId>/password" and
 * "ssh/<profileId>/passphrase". All functions are synchronous and GUI-thread only.
 */
namespace SecretStore {

bool isSecure();
bool store(const QString& key, const QString& secret);          ///< empty secret removes the entry
std::optional<QString> load(const QString& key);                ///< nullopt when absent or undecodable
bool remove(const QString& key);
bool contains(const QString& key);
/// Translatable one-line description for the UI ("Stored with Windows DPAPI" / "Obfuscated only").
QString storageDescription();

} // namespace SecretStore
