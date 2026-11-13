/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nbddevice.h"
#include "nbdservice.h"

#include <KXmlGuiWindow>
#include <KMessageWidget>

#include <QPoint>
#include <QQueue>
#include <QSet>
#include <QString>

#include <optional>

class QAction;
class QLabel;
class QTableView;
class QTimer;
class DeviceDetailsWidget;
class NbdDeviceModel;

class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private Q_SLOTS:
    void refresh();
    void selectionChanged();
    void connectSelected();
    void disconnectSelected();
    void editSelectedProfile();
    void deleteSelectedProfile();
    void mountSelected();
    void unmountSelected();
    void openSelected();
    void unlockSelectedVolume(const QString &path);
    void loadNbdModule();
    void showContextMenu(const QPoint &position);
    void handleOperationStarted(const QString &description);
    void handleOperationFinished(bool success, const QString &message);
    void setAutostartEnabled(bool enabled);
    void showBackgroundService();
    void showAbout();

private:
    void setupUi();
    void setupActions();
    void updateActions();
    void updateStatusBar();
    void updateDetails();
    void maybeAutoConnectProfiles();
    void connectNextAutoProfile();
    void maybeAutoMountVolumes();
    void mountNextAutoVolume();
    void unlockNextAutoVolume();
    QString autostartFilePath() const;
    void showMessage(KMessageWidget::MessageType type, const QString &text);
    void showEnvironmentMessage(const QString &text);
    std::optional<NbdDevice> selectedDevice() const;
    QString selectedDevicePath() const;
    void selectDevicePath(const QString &path);
    void setOperationMessage(const QString &text);
    NbdBackend currentBackend() const;

    NbdService *m_service = nullptr;
    NbdDeviceModel *m_model = nullptr;
    QTableView *m_view = nullptr;
    DeviceDetailsWidget *m_details = nullptr;
    KMessageWidget *m_message = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QLabel *m_countLabel = nullptr;
    QLabel *m_backendLabel = nullptr;

    QAction *m_connectAction = nullptr;
    QAction *m_disconnectAction = nullptr;
    QAction *m_loadModuleAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_mountAction = nullptr;
    QAction *m_unmountAction = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_autostartAction = nullptr;
    QAction *m_serviceAction = nullptr;
    QAction *m_editProfileAction = nullptr;
    QAction *m_deleteProfileAction = nullptr;

    struct MountRequest {
        QString path;
        QString fileSystem;
        bool readOnly = false;
        QString mountPoint;
    };

    QString m_environmentError;
    bool m_busy = false;
    bool m_refreshing = false;
    bool m_disconnectPending = false;
    struct UnlockRequest {
        QString path;
        QString passphrase;
        bool readOnly = false;
    };

    struct PendingUnlock {
        QString device;
        QString path;
        QString passphrase;
        bool remember = false;
    };

    QQueue<ConnectOptions> m_autoConnectQueue;
    QQueue<MountRequest> m_autoMountQueue;
    QQueue<UnlockRequest> m_autoUnlockQueue;
    QSet<QString> m_autoMountAttempted;
    bool m_autoMountActive = false;
    bool m_autoUnlockActive = false;
    bool m_autoConnectStarted = false;
    bool m_autoConnectActive = false;

    PendingUnlock m_pendingUnlock;
    bool m_hasPendingUnlock = false;
};
