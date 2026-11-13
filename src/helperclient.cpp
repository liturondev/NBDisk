/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "helperclient.h"

#include "helperprotocol.h"

#include <KLocalizedString>

#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcessEnvironment>

namespace
{
constexpr int kMaximumResponseBytes = 128 * 1024;
constexpr int kHelperExitCodeUnavailable = HelperProtocol::UnavailableExitCode;
}

HelperClient::HelperClient(QObject *parent)
    : QObject(parent)
{
}

QString HelperClient::socketPath()
{
    return HelperProtocol::socketPath();
}

bool HelperClient::isAvailable()
{
    return QFileInfo::exists(socketPath());
}

void HelperClient::send(const QString &operation, const QJsonObject &arguments)
{
    abort();

    m_active = true;
    m_buffer.clear();
    m_pendingOperation = operation;
    m_pendingArguments = arguments;
    m_socket = new QLocalSocket(this);

    connect(m_socket, &QLocalSocket::connected, this,
            &HelperClient::handleConnected);
    connect(m_socket, &QLocalSocket::readyRead, this,
            &HelperClient::handleReadyRead);
    connect(m_socket, &QLocalSocket::errorOccurred, this,
            &HelperClient::handleError);

    m_socket->connectToServer(socketPath());
}

void HelperClient::abort()
{
    if (!m_socket) {
        return;
    }
    m_socket->disconnect(this);
    m_socket->abort();
    m_socket->deleteLater();
    m_socket = nullptr;
    m_active = false;
    m_buffer.clear();
}

void HelperClient::handleConnected()
{
    if (!m_socket) {
        return;
    }

    QJsonObject request;
    request.insert(QStringLiteral("version"), HelperProtocol::ProtocolVersion);
    request.insert(QStringLiteral("operation"), m_pendingOperation);
    request.insert(QStringLiteral("arguments"), m_pendingArguments);
    m_socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    m_socket->write("\n");
    m_socket->flush();
}

void HelperClient::handleReadyRead()
{
    if (!m_socket) {
        return;
    }

    m_buffer.append(m_socket->readAll());
    if (m_buffer.size() > kMaximumResponseBytes) {
        emitResult(false, kHelperExitCodeUnavailable,
                   i18n("The privileged helper returned an oversized response."),
                   QString());
        return;
    }

    const int newline = m_buffer.indexOf('\n');
    if (newline < 0) {
        return;
    }

    const QJsonDocument document =
        QJsonDocument::fromJson(m_buffer.left(newline));
    if (!document.isObject()) {
        emitResult(false, kHelperExitCodeUnavailable,
                   i18n("The privileged helper returned an invalid response."),
                   QString());
        return;
    }

    const QJsonObject response = document.object();
    emitResult(response.value(QStringLiteral("success")).toBool(),
               response.value(QStringLiteral("exitCode")).toInt(),
               response.value(QStringLiteral("message")).toString(),
               response.value(QStringLiteral("output")).toString());
}

void HelperClient::handleError()
{
    if (!m_active) {
        return;
    }
    emitResult(false, kHelperExitCodeUnavailable,
               m_socket ? m_socket->errorString()
                        : i18n("The privileged helper is not available."),
               QString());
}

void HelperClient::emitResult(bool success, int exitCode, const QString &message,
                              const QString &output)
{
    if (!m_active) {
        return;
    }
    m_active = false;
    abort();
    Q_EMIT finished(success, exitCode, message, output);
}
