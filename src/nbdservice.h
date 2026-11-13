/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nbdcommand.h"
#include "nbddevice.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class HelperClient;
class QTimer;

struct BackendAvailability {
    bool nbdClient = false;
    bool qemuNbd = false;

    bool any() const
    {
        return nbdClient || qemuNbd;
    }
};

class NbdService : public QObject
{
    Q_OBJECT

public:
    explicit NbdService(QObject *parent = nullptr);
    ~NbdService() override;

    QList<NbdDevice> scanDevices(QString *error = nullptr);
    BackendAvailability backends() const;
    NbdBackend effectiveBackend(NbdBackend preferred) const;
    bool isBusy() const;
    bool isRoot() const;
    bool nbdModuleLoaded() const;
    QList<NbdProfile> profiles() const;
    std::optional<NbdProfile> profileForDevice(const QString &device) const;
    // Re-reads the profile and connection store from disk. Used by the
    // background service before each triggered pass so it sees current data.
    void reloadState();

    void saveProfile(const ConnectOptions &options, bool autoConnect);
    // Removes the saved profile and any remembered LUKS passphrases for it.
    void deleteProfile(const QString &device);
    void setProfileAutoConnect(const QString &device, bool enabled);
    void setProfileAutoMount(const QString &device, bool enabled);
    void setProfileMountPoint(const QString &device, const QString &volumePath,
                              const QString &mountPoint);
    // Marks a volume for auto-unlock. Disabling it also removes the stored
    // passphrase from the wallet.
    void setProfileRememberPassphrase(const QString &device, const QString &volumePath,
                                      bool remember);
    // Stores a passphrase in the wallet and marks the volume for auto-unlock.
    // Returns false when no wallet is available.
    bool storeVolumePassphrase(const QString &device, const QString &volumePath,
                               const QString &passphrase);
    // Returns the remembered passphrase for a volume, or an empty string.
    QString volumePassphrase(const QString &device, const QString &volumePath) const;
    void loadNbdModule();
    void connectDevice(const ConnectOptions &options);
    void setCommandTimeoutMs(int milliseconds);
    void disconnectDevice(const NbdDevice &device, const QStringList &volumesToUnmount = QStringList());
    void mountVolume(const QString &path, const QString &fileSystem = QString(),
                     bool readOnly = false, const QString &mountPoint = QString());
    void unmountVolume(const QString &path);
    void unlockVolume(const QString &path, const QString &passphrase, bool readOnly = false);
    void lockVolume(const QString &path);

Q_SIGNALS:
    void operationStarted(const QString &description);
    void operationFinished(bool success, const QString &message);
    void stateChanged();

private:
    struct Command {
        QString program;
        QStringList arguments;
        QString description;
        QByteArray stdinData;

        // Set for operations delegated to the privileged helper.
        QString helperOperation;
        QJsonObject helperArguments;

        bool isValid() const
        {
            return !program.isEmpty() || !helperOperation.isEmpty();
        }
    };

    struct Operation {
        QString description;
        QString completedMessage;
        QQueue<Command> commands;
        // Equivalent pkexec commands, used when the helper is missing or
        // denies the request.
        QQueue<Command> fallbackCommands;
        QStringList output;
        bool useOutput = false;
        std::function<void()> onSuccess;
    };

    void startOperation(std::unique_ptr<Operation> operation);
    void startNextCommand();
    void commandFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void commandTimedOut();
    void processFailedToStart();
    void finishOperation(bool success, const QString &message);
    void setBusy(bool busy);
    void refreshFileSystems();
    void handleFileSystemsFinished();

    Command privilegedCommand(const QString &program, const QStringList &arguments,
                              const QString &description) const;
    Command helperCommand(const QString &operation, const QJsonObject &arguments,
                          const QString &description) const;
    Command userCommand(const QString &program, const QStringList &arguments,
                        const QString &description) const;
    bool helperAvailable() const;
    void helperCommandFinished(bool success, int exitCode, const QString &message,
                               const QString &output);
    BackendAvailability detectBackends() const;
    QString stateFilePath() const;
    void loadState();
    void saveState() const;

    QString m_nbdClientPath;
    QString m_qemuNbdPath;
    QString m_pkexecPath;
    QString m_modprobePath;
    QString m_udisksctlPath;
    QString m_mountPath;
    QString m_mkdirPath;
    QString m_cryptsetupPath;
    QString m_lsblkPath;
    bool m_busy = false;

    QProcess *m_process = nullptr;
    HelperClient *m_helperClient = nullptr;
    bool m_helperActive = false;
    QMetaObject::Connection m_finishedConnection;
    QTimer *m_commandTimer = nullptr;
    int m_commandTimeoutMs = 120000;
    std::unique_ptr<Operation> m_operation;

    QProcess *m_lsblkProcess = nullptr;
    QTimer *m_lsblkTimer = nullptr;
    QHash<QString, QString> m_fileSystems;
    qint64 m_fileSystemsRequestedAt = 0;

    QHash<QString, NbdConnectionInfo> m_connections;
    QHash<QString, NbdProfile> m_profiles;
    std::unique_ptr<ConnectOptions> m_pendingConnection;
};
