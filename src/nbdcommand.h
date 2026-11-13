/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nbddevice.h"

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

// A whole NBD device such as /dev/nbd0 may be connected. Partitions
// (/dev/nbd0p1) are volumes and are validated separately.
inline bool isNbdDevicePath(const QString &path)
{
    static const QRegularExpression pattern(QStringLiteral("^/dev/nbd\\d+$"));
    return pattern.match(path).hasMatch();
}

struct ConnectOptions {
    QString device;
    QString host;
    quint16 port = 10809;
    QString exportName;
    bool readOnly = false;
    int blockSize = 512;
    int timeoutSeconds = 0;
    bool persistent = false;
    bool tls = false;
    QString certFile;
    QString keyFile;
    QString caFile;
    QString tlsHostname;
    NbdBackend backend = NbdBackend::Auto;
};

struct NbdProfile {
    ConnectOptions options;
    bool autoConnect = false;
    bool autoMount = false;
    QHash<QString, QString> mountPoints;
    // Volume paths whose LUKS passphrase is kept in the wallet so the
    // startup service can unlock them before automounting.
    QSet<QString> rememberedVolumes;
    QDateTime lastConnected;

    bool isValid() const
    {
        return isNbdDevicePath(options.device) && !options.host.isEmpty();
    }
};

class NbdCommandBuilder
{
public:
    static QString backendName(NbdBackend backend);
    static QStringList connectArguments(const ConnectOptions &options, NbdBackend backend);
    static QStringList disconnectArguments(const QString &device, NbdBackend backend);
    static QString nbdUrl(const ConnectOptions &options);
    static bool supportsAdvancedOptions(NbdBackend backend);
    static bool validate(const ConnectOptions &options, NbdBackend backend, QString *error);
};
