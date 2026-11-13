/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QVariantMap>

class NbdService;
class StartupRunner;

// Owns the daemon's profile store and exposes the startup pass over D-Bus.
// One instance lives for the whole daemon process, so repeated runs are
// possible and always reload the saved profiles.
class BackgroundService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.liturondev.NBDisk.Service")

public:
    explicit BackgroundService(QObject *parent = nullptr);

    bool registerOnBus();

public Q_SLOTS:
    bool RunStartup();
    bool IsRunning() const;
    QVariantMap Status() const;

Q_SIGNALS:
    void Started();
    void Finished(bool success, int connected, int mounted, const QString &message);

private:
    void handleRunnerFinished();

    NbdService *m_service = nullptr;
    StartupRunner *m_runner = nullptr;
    bool m_running = false;
    bool m_hasRun = false;
    bool m_lastSuccess = false;
    int m_lastConnected = 0;
    int m_lastMounted = 0;
    QString m_lastMessage;
    QString m_lastRun;
};
