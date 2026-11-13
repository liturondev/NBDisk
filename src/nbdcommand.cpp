/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nbdcommand.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

QString NbdCommandBuilder::backendName(NbdBackend backend)
{
    switch (backend) {
    case NbdBackend::NbdClient:
        return QStringLiteral("nbd-client");
    case NbdBackend::QemuNbd:
        return QStringLiteral("qemu-nbd");
    case NbdBackend::Auto:
        break;
    }
    return QStringLiteral("automatic");
}

QStringList NbdCommandBuilder::connectArguments(const ConnectOptions &options, NbdBackend backend)
{
    Q_ASSERT(backend != NbdBackend::Auto);

    if (backend == NbdBackend::QemuNbd) {
        QStringList args;
        args << QStringLiteral("--connect=%1").arg(options.device);
        if (options.readOnly) {
            args << QStringLiteral("--read-only");
        }
        args << nbdUrl(options);
        return args;
    }

    QStringList args;
    if (!options.exportName.isEmpty()) {
        args << QStringLiteral("-N") << options.exportName;
    }
    args << options.host << QString::number(options.port) << options.device;

    if (options.blockSize > 0) {
        args << QStringLiteral("-b") << QString::number(options.blockSize);
    }
    if (options.timeoutSeconds > 0) {
        args << QStringLiteral("-t") << QString::number(options.timeoutSeconds);
    }
    if (options.readOnly) {
        args << QStringLiteral("-R");
    }
    if (options.persistent) {
        args << QStringLiteral("-p");
    }
    if (options.tls) {
        args << QStringLiteral("-x");
        if (!options.certFile.isEmpty()) {
            args << QStringLiteral("-F") << options.certFile;
        }
        if (!options.keyFile.isEmpty()) {
            args << QStringLiteral("-K") << options.keyFile;
        }
        if (!options.caFile.isEmpty()) {
            args << QStringLiteral("-A") << options.caFile;
        }
        const QString hostname = options.tlsHostname.isEmpty() ? options.host : options.tlsHostname;
        if (!hostname.isEmpty()) {
            args << QStringLiteral("-H") << hostname;
        }
    }
    return args;
}

QStringList NbdCommandBuilder::disconnectArguments(const QString &device, NbdBackend backend)
{
    Q_ASSERT(backend != NbdBackend::Auto);

    if (backend == NbdBackend::QemuNbd) {
        return {QStringLiteral("--disconnect"), device};
    }
    return {QStringLiteral("-d"), device};
}

QString NbdCommandBuilder::nbdUrl(const ConnectOptions &options)
{
    QUrl url;
    url.setScheme(QStringLiteral("nbd"));
    url.setHost(options.host);
    url.setPort(options.port);

    QString path = options.exportName;
    while (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    url.setPath(QStringLiteral("/") + path);
    return url.toString(QUrl::FullyEncoded);
}

bool NbdCommandBuilder::supportsAdvancedOptions(NbdBackend backend)
{
    return backend == NbdBackend::NbdClient;
}

bool NbdCommandBuilder::validate(const ConnectOptions &options, NbdBackend backend, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (backend == NbdBackend::Auto) {
        return fail(QStringLiteral("No NBD backend is available."));
    }
    if (options.host.trimmed().isEmpty()) {
        return fail(QStringLiteral("Enter a server host name or address."));
    }
    if (options.host != options.host.trimmed() || options.host.startsWith(QLatin1Char('-'))
        || options.host.contains(QRegularExpression(QStringLiteral("\\s")))) {
        return fail(QStringLiteral("The server host name contains invalid characters."));
    }
    if (options.port == 0) {
        return fail(QStringLiteral("The server port must be between 1 and 65535."));
    }
    if (!isNbdDevicePath(options.device)) {
        return fail(QStringLiteral("Select a valid NBD device."));
    }

    if (backend == NbdBackend::QemuNbd) {
        if (options.persistent || options.tls || options.timeoutSeconds > 0
            || options.blockSize != 512) {
            return fail(QStringLiteral("qemu-nbd does not support these advanced options."));
        }
    }

    if (options.tls) {
        const QStringList files = {options.certFile, options.keyFile, options.caFile};
        for (const QString &file : files) {
            if (!file.isEmpty() && !QFileInfo(file).isReadable()) {
                return fail(QStringLiteral("TLS file is not readable: %1").arg(file));
            }
        }
        if (options.certFile.isEmpty() != options.keyFile.isEmpty()) {
            return fail(QStringLiteral("Choose both a client certificate and its private key."));
        }
        if (backend == NbdBackend::QemuNbd) {
            return fail(QStringLiteral("TLS is supported through nbd-client only."));
        }
    }

    if (options.blockSize != 512 && options.blockSize != 1024 && options.blockSize != 2048
        && options.blockSize != 4096) {
        return fail(QStringLiteral("The block size must be 512, 1024, 2048, or 4096 bytes."));
    }

    return true;
}
