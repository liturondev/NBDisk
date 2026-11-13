/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nbdcommand.h"

#include <QObject>
#include <QQueue>

class NbdService;

// Runs the saved auto-connect and automount profiles without any user
// interface. This is what the login-time background service uses; the GUI
// keeps its own interactive flow.
class StartupRunner : public QObject
{
    Q_OBJECT

public:
    explicit StartupRunner(NbdService *service, QObject *parent = nullptr);

    void start();
    bool hadFailures() const;
    int connectedCount() const;
    int mountedCount() const;
    int unlockedCount() const;
    QString lastError() const;

Q_SIGNALS:
    void finished();

private:
    enum class Phase {
        Idle,
        LoadingModule,
        Connecting,
        Unlocking,
        Mounting,
    };

    struct MountRequest {
        QString path;
        QString fileSystem;
        bool readOnly = false;
        QString mountPoint;
    };

    struct UnlockRequest {
        QString path;
        QString passphrase;
        bool readOnly = false;
    };

    void beginConnections();
    void beginMounts();
    void buildUnlockQueue();
    void buildMountQueue();
    void runNextConnect();
    void runNextUnlock();
    void runNextMount();
    void handleOperationFinished(bool success, const QString &message);
    void finish();

    NbdService *m_service = nullptr;
    Phase m_phase = Phase::Idle;
    bool m_running = false;
    bool m_failed = false;
    int m_connectedCount = 0;
    int m_mountedCount = 0;
    int m_unlockedCount = 0;
    QString m_lastError;
    QQueue<ConnectOptions> m_connectQueue;
    QQueue<UnlockRequest> m_unlockQueue;
    QQueue<MountRequest> m_mountQueue;
};
