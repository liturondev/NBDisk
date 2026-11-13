/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "backgroundserviceclient.h"

#include "servicedbus.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

bool BackgroundServiceClient::isAvailable() const
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return false;
    }
    QDBusConnectionInterface *interface = bus.interface();
    return interface && interface->isServiceRegistered(ServiceBus::serviceName()).value();
}

QVariantMap BackgroundServiceClient::status() const
{
    QDBusInterface interface(ServiceBus::serviceName(), ServiceBus::objectPath(),
                             ServiceBus::interfaceName(), QDBusConnection::sessionBus());
    if (!interface.isValid()) {
        return {};
    }
    const QDBusReply<QVariantMap> reply = interface.call(QStringLiteral("Status"));
    return reply.isValid() ? reply.value() : QVariantMap();
}

bool BackgroundServiceClient::runStartup() const
{
    QDBusInterface interface(ServiceBus::serviceName(), ServiceBus::objectPath(),
                             ServiceBus::interfaceName(), QDBusConnection::sessionBus());
    if (!interface.isValid()) {
        return false;
    }
    const QDBusReply<bool> reply = interface.call(QStringLiteral("RunStartup"));
    return reply.isValid() && reply.value();
}

bool BackgroundServiceClient::startService() const
{
    const QString executable = daemonExecutablePath();
    if (executable.isEmpty()) {
        return false;
    }
    return QProcess::startDetached(executable, {});
}

QString BackgroundServiceClient::daemonExecutablePath()
{
    const QString installed =
        QStandardPaths::findExecutable(QStringLiteral("nbdisk-daemon"));
    if (!installed.isEmpty()) {
        return installed;
    }

    // Running from a build tree: the daemon is built next to the GUI binary.
    const QFileInfo sibling(
        QCoreApplication::applicationDirPath() + QStringLiteral("/nbdisk-daemon"));
    if (sibling.isFile() && sibling.isExecutable()) {
        return sibling.absoluteFilePath();
    }
    return QString();
}
