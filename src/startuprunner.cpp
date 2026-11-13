/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "startuprunner.h"

#include "nbdservice.h"

#include <QDebug>
#include <QSet>
#include <QTimer>

StartupRunner::StartupRunner(NbdService *service, QObject *parent)
    : QObject(parent)
    , m_service(service)
{
    connect(m_service, &NbdService::operationFinished, this,
            &StartupRunner::handleOperationFinished);
}

bool StartupRunner::hadFailures() const
{
    return m_failed;
}

int StartupRunner::connectedCount() const
{
    return m_connectedCount;
}

int StartupRunner::mountedCount() const
{
    return m_mountedCount;
}

int StartupRunner::unlockedCount() const
{
    return m_unlockedCount;
}

QString StartupRunner::lastError() const
{
    return m_lastError;
}

void StartupRunner::start()
{
    if (m_running) {
        return;
    }
    m_running = true;

    // Each pass starts from the saved profiles and fresh results, so the
    // service can be triggered repeatedly.
    m_failed = false;
    m_lastError.clear();
    m_connectedCount = 0;
    m_mountedCount = 0;
    m_unlockedCount = 0;
    m_connectQueue.clear();
    m_unlockQueue.clear();
    m_mountQueue.clear();
    m_service->reloadState();

    const QList<NbdProfile> profiles = m_service->profiles();
    if (profiles.isEmpty()) {
        qInfo() << QStringLiteral("Startup service: no saved profiles.");
        finish();
        return;
    }

    if (!m_service->nbdModuleLoaded()) {
        m_phase = Phase::LoadingModule;
        qInfo() << QStringLiteral("Startup service: loading the nbd kernel module.");
        m_service->loadNbdModule();
        return;
    }

    beginConnections();
}

void StartupRunner::beginConnections()
{
    m_phase = Phase::Connecting;

    QString error;
    const QList<NbdDevice> devices = m_service->scanDevices(&error);
    QSet<QString> connectedDevices;
    for (const NbdDevice &device : devices) {
        if (device.connected) {
            connectedDevices.insert(device.path);
        }
    }

    for (const NbdProfile &profile : m_service->profiles()) {
        if (!profile.autoConnect || !profile.isValid()) {
            continue;
        }
        if (connectedDevices.contains(profile.options.device)) {
            continue;
        }
        m_connectQueue.enqueue(profile.options);
    }

    if (m_connectQueue.isEmpty()) {
        beginMounts();
        return;
    }

    qInfo() << QStringLiteral("Startup service: reconnecting") << m_connectQueue.size()
            << QStringLiteral("device(s).");
    runNextConnect();
}

void StartupRunner::beginMounts()
{
    buildUnlockQueue();
    if (!m_unlockQueue.isEmpty()) {
        m_phase = Phase::Unlocking;
        runNextUnlock();
        return;
    }

    m_phase = Phase::Mounting;
    buildMountQueue();
    if (m_mountQueue.isEmpty()) {
        finish();
        return;
    }
    runNextMount();
}

void StartupRunner::buildUnlockQueue()
{
    QString error;
    const QList<NbdDevice> devices = m_service->scanDevices(&error);
    for (const NbdDevice &device : devices) {
        if (!device.connected) {
            continue;
        }
        const auto profile = m_service->profileForDevice(device.path);
        if (!profile || profile->rememberedVolumes.isEmpty()) {
            continue;
        }
        for (const NbdVolume &volume : device.volumes) {
            if (!volume.isLockedLuks()
                || !profile->rememberedVolumes.contains(volume.path)) {
                continue;
            }
            const QString passphrase =
                m_service->volumePassphrase(device.path, volume.path);
            if (passphrase.isEmpty()) {
                qWarning() << QStringLiteral("Startup service: no stored passphrase for")
                           << volume.path;
                continue;
            }
            m_unlockQueue.enqueue({volume.path, passphrase, device.readOnly});
        }
    }

    if (!m_unlockQueue.isEmpty()) {
        qInfo() << QStringLiteral("Startup service: unlocking") << m_unlockQueue.size()
                << QStringLiteral("volume(s).");
    }
}

void StartupRunner::buildMountQueue()
{
    QString error;
    const QList<NbdDevice> devices = m_service->scanDevices(&error);
    for (const NbdDevice &device : devices) {
        if (!device.connected) {
            continue;
        }
        const auto profile = m_service->profileForDevice(device.path);
        if (!profile || !profile->autoMount) {
            continue;
        }

        for (const NbdVolume &volume : device.volumes) {
            const auto mountPoint = profile->mountPoints.constFind(volume.path);
            if (mountPoint == profile->mountPoints.cend() || mountPoint.value().isEmpty()
                || volume.isMounted() || volume.isLockedLuks()) {
                continue;
            }
            m_mountQueue.enqueue({volume.mountPath(), volume.mountFileSystem(),
                                  device.readOnly, mountPoint.value()});
        }
    }

    if (!m_mountQueue.isEmpty()) {
        qInfo() << QStringLiteral("Startup service: mounting") << m_mountQueue.size()
                << QStringLiteral("volume(s).");
    }
}

void StartupRunner::runNextConnect()
{
    if (m_connectQueue.isEmpty() || m_service->isBusy()) {
        return;
    }

    const ConnectOptions options = m_connectQueue.dequeue();
    qInfo() << QStringLiteral("Startup service: connecting") << options.device
            << QStringLiteral("to") << options.host << options.port;
    m_service->connectDevice(options);
}

void StartupRunner::runNextUnlock()
{
    if (m_unlockQueue.isEmpty() || m_service->isBusy()) {
        return;
    }

    UnlockRequest request = m_unlockQueue.dequeue();
    qInfo() << QStringLiteral("Startup service: unlocking") << request.path;
    m_service->unlockVolume(request.path, request.passphrase, request.readOnly);
    request.passphrase.fill(QChar(0));
}

void StartupRunner::runNextMount()
{
    if (m_mountQueue.isEmpty() || m_service->isBusy()) {
        return;
    }

    const MountRequest request = m_mountQueue.dequeue();
    qInfo() << QStringLiteral("Startup service: mounting") << request.path
            << QStringLiteral("at") << request.mountPoint;
    m_service->mountVolume(request.path, request.fileSystem, request.readOnly,
                           request.mountPoint);
}

void StartupRunner::handleOperationFinished(bool success, const QString &message)
{
    if (!m_running || m_phase == Phase::Idle) {
        return;
    }

    if (success) {
        qInfo() << QStringLiteral("Startup service:") << message;
    } else {
        m_failed = true;
        m_lastError = message;
        qWarning() << QStringLiteral("Startup service failed:") << message;
    }

    switch (m_phase) {
    case Phase::LoadingModule:
        // A missing module is not fatal for the scan; the backend may load it.
        beginConnections();
        return;
    case Phase::Connecting:
        if (success) {
            ++m_connectedCount;
        }
        if (!m_connectQueue.isEmpty()) {
            runNextConnect();
            return;
        }
        // Let sysfs settle so the freshly connected volumes are visible.
        QTimer::singleShot(500, this, [this]() {
            if (m_running && m_phase == Phase::Idle) {
                beginMounts();
            }
        });
        m_phase = Phase::Idle;
        return;
    case Phase::Unlocking:
        if (success) {
            ++m_unlockedCount;
        }
        if (!m_unlockQueue.isEmpty()) {
            runNextUnlock();
            return;
        }
        // Give the mapper device a moment before scanning for mounts.
        m_phase = Phase::Idle;
        QTimer::singleShot(300, this, [this]() {
            if (!m_running || m_phase != Phase::Idle) {
                return;
            }
            m_phase = Phase::Mounting;
            buildMountQueue();
            if (m_mountQueue.isEmpty()) {
                finish();
                return;
            }
            runNextMount();
        });
        return;
    case Phase::Mounting:
        if (success) {
            ++m_mountedCount;
        }
        if (!m_mountQueue.isEmpty()) {
            runNextMount();
            return;
        }
        finish();
        return;
    case Phase::Idle:
        return;
    }
}

void StartupRunner::finish()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_phase = Phase::Idle;

    qInfo() << QStringLiteral("Startup service finished:") << m_connectedCount
            << QStringLiteral("connected,") << m_unlockedCount
            << QStringLiteral("unlocked,") << m_mountedCount << QStringLiteral("mounted.")
            << (m_failed ? QStringLiteral("Some steps failed.")
                         : QStringLiteral("All steps succeeded."));
    Q_EMIT finished();
}
