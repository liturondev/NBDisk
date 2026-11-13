/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "secretstore.h"

#include <kwallet.h>

#include <QEventLoop>
#include <QLatin1Char>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace
{
const QString &folderName()
{
    static const QString folder = QStringLiteral("NBDisk");
    return folder;
}

QString entryKey(const QString &device, const QString &volume)
{
    return device + QLatin1Char('|') + volume;
}

std::unique_ptr<KWallet::Wallet> openWallet()
{
    // Avoid blocking on a wallet prompt when running without a session bus,
    // such as in the headless test environment.
    if (!KWallet::Wallet::isEnabled()
        || qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS")) {
        return {};
    }

    std::unique_ptr<KWallet::Wallet> wallet(
        KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), 0,
                                    KWallet::Wallet::Asynchronous));
    if (!wallet) {
        return {};
    }
    if (!wallet->isOpen()) {
        // Never let a closed wallet hold up connecting at login for long.
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.setInterval(15000);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(wallet.get(), &KWallet::Wallet::walletOpened, &loop,
                         [&loop](bool) { loop.quit(); });
        timeout.start();
        loop.exec();
    }
    if (!wallet->isOpen()) {
        return {};
    }
    if (!wallet->hasFolder(folderName())) {
        wallet->createFolder(folderName());
    }
    if (!wallet->setFolder(folderName())) {
        return {};
    }
    return wallet;
}
}

bool SecretStore::isAvailable()
{
    return KWallet::Wallet::isEnabled();
}

QString SecretStore::passphrase(const QString &device, const QString &volume)
{
    const auto wallet = openWallet();
    if (!wallet) {
        return {};
    }

    QString value;
    if (wallet->readPassword(entryKey(device, volume), value) != 0) {
        return {};
    }
    return value;
}

bool SecretStore::store(const QString &device, const QString &volume,
                        const QString &passphrase)
{
    const auto wallet = openWallet();
    if (!wallet) {
        return false;
    }
    return wallet->writePassword(entryKey(device, volume), passphrase) == 0;
}

bool SecretStore::forget(const QString &device, const QString &volume)
{
    const auto wallet = openWallet();
    if (!wallet) {
        return false;
    }
    return wallet->removeEntry(entryKey(device, volume)) == 0;
}

void SecretStore::forgetDevice(const QString &device)
{
    const auto wallet = openWallet();
    if (!wallet) {
        return;
    }

    const QString prefix = device + QLatin1Char('|');
    const QStringList entries = wallet->entryList();
    for (const QString &entry : entries) {
        if (entry.startsWith(prefix)) {
            wallet->removeEntry(entry);
        }
    }
}
