/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class QLocalSocket;

// Client side of the nbdisk-helper protocol. Each request opens a short
// lived connection to the privileged helper, sends one newline-delimited JSON
// operation, and waits for a single JSON response.
class HelperClient : public QObject
{
    Q_OBJECT

public:
    explicit HelperClient(QObject *parent = nullptr);

    // Path of the helper socket. The environment override is useful for a
    // locally started helper while developing.
    static QString socketPath();

    // True when the socket exists, so NbdService can prefer the helper over
    // interactive pkexec authorization.
    static bool isAvailable();

    void send(const QString &operation, const QJsonObject &arguments);
    void abort();

Q_SIGNALS:
    // exitCode 126 means the helper could not be reached or denied the
    // request, so callers may fall back to pkexec.
    void finished(bool success, int exitCode, const QString &message,
                  const QString &output);

private:
    void handleConnected();
    void handleReadyRead();
    void handleError();
    void emitResult(bool success, int exitCode, const QString &message,
                    const QString &output);

    QLocalSocket *m_socket = nullptr;
    QByteArray m_buffer;
    QString m_pendingOperation;
    QJsonObject m_pendingArguments;
    bool m_active = false;
};
