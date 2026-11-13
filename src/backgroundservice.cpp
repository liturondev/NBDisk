/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "backgroundservice.h"

#include "nbdservice.h"
#include "servicedbus.h"
#include "startuprunner.h"

#include <QDateTime>
#include <QDBusConnection>

BackgroundService::BackgroundService(QObject *parent)
    : QObject(parent)
    , m_service(new NbdService(this))
    , m_runner(new StartupRunner(m_service, this))
{
    connect(m_runner, &StartupRunner::finished, this,
            &BackgroundService::handleRunnerFinished);
}

bool BackgroundService::registerOnBus()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return false;
    }
    if (!bus.registerService(ServiceBus::serviceName())) {
        return false;
    }

    return bus.registerObject(ServiceBus::objectPath(), this,
                              QDBusConnection::ExportAllSlots
                                  | QDBusConnection::ExportAllSignals);
}

bool BackgroundService::RunStartup()
{
    if (m_running) {
        return false;
    }

    m_running = true;
    m_lastMessage.clear();
    Q_EMIT Started();
    m_runner->start();
    return true;
}

bool BackgroundService::IsRunning() const
{
    return m_running;
}

QVariantMap BackgroundService::Status() const
{
    QVariantMap status;
    status.insert(QStringLiteral("running"), m_running);
    status.insert(QStringLiteral("hasRun"), m_hasRun);
    status.insert(QStringLiteral("lastSuccess"), m_lastSuccess);
    status.insert(QStringLiteral("lastConnected"), m_lastConnected);
    status.insert(QStringLiteral("lastMounted"), m_lastMounted);
    status.insert(QStringLiteral("lastMessage"), m_lastMessage);
    status.insert(QStringLiteral("lastRun"), m_lastRun);
    return status;
}

void BackgroundService::handleRunnerFinished()
{
    m_running = false;
    m_hasRun = true;
    m_lastSuccess = !m_runner->hadFailures();
    m_lastConnected = m_runner->connectedCount();
    m_lastMounted = m_runner->mountedCount();
    m_lastMessage = m_runner->lastError();
    m_lastRun = QDateTime::currentDateTime().toString(Qt::ISODate);

    Q_EMIT Finished(m_lastSuccess, m_lastConnected, m_lastMounted, m_lastMessage);
}
