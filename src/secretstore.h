/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

// Stores LUKS passphrases in the KDE Wallet so auto-connect can unlock
// encrypted volumes without prompting. Entries are scoped to a whole NBD
// device and a single volume inside it.
namespace SecretStore
{
bool isAvailable();

// Returns an empty string when no passphrase is stored or the wallet is
// unavailable.
QString passphrase(const QString &device, const QString &volume);

bool store(const QString &device, const QString &volume, const QString &passphrase);

bool forget(const QString &device, const QString &volume);

// Removes every passphrase stored for the device, for example when its
// profile is deleted.
void forgetDevice(const QString &device);
}
