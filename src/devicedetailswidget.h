/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nbdcommand.h"
#include "nbddevice.h"

#include <QWidget>

class QCheckBox;
class QFrame;
class QGroupBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QVBoxLayout;

class DeviceDetailsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DeviceDetailsWidget(QWidget *parent = nullptr);

    void setDevice(const NbdDevice &device);
    void setProfile(const NbdProfile &profile, bool hasProfile);
    void setBusy(bool busy);
    void clear();
    QString currentVolumePath() const;
    QString currentMountPoint() const;
    bool isEditingMountPoint() const;

Q_SIGNALS:
    void mountRequested(const QString &path);
    void unmountRequested(const QString &path);
    void openRequested(const QString &path);
    void autoConnectChanged(bool enabled);
    void autoMountChanged(bool enabled);
    void mountPointChanged(const QString &volumePath, const QString &mountPoint);
    void rememberPassphraseChanged(const QString &volumePath, bool remember);
    void editProfileRequested();
    void deleteProfileRequested();
    void unlockRequested(const QString &volumePath);
    void lockRequested(const QString &volumePath);

private:
    void populate();
    void updateButtons();
    void updateSelectionDetails();
    void updateMountPointColumn();
    void updateRememberColumn();
    void updateProfileSummary();
    void clearCapacityBars();

    NbdDevice m_device;
    NbdProfile m_profile;
    bool m_hasDevice = false;
    bool m_hasProfile = false;
    bool m_busy = false;

    QFrame *m_diskCard = nullptr;
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QLabel *m_statusBadge = nullptr;
    QLabel *m_capacityLabel = nullptr;
    QWidget *m_capacityBars = nullptr;
    QVBoxLayout *m_capacityBarsLayout = nullptr;
    QLabel *m_connectionLine = nullptr;
    QWidget *m_startupWidget = nullptr;
    QGroupBox *m_profileGroup = nullptr;
    QLabel *m_profileSummary = nullptr;
    QPushButton *m_editProfileButton = nullptr;
    QPushButton *m_deleteProfileButton = nullptr;
    QGroupBox *m_volumeGroup = nullptr;
    QCheckBox *m_autoConnectCheck = nullptr;
    QCheckBox *m_autoMountCheck = nullptr;
    QTableWidget *m_volumeTable = nullptr;
    QPushButton *m_mountButton = nullptr;
    QPushButton *m_unmountButton = nullptr;
    QPushButton *m_browseMountPointButton = nullptr;
    QPushButton *m_openButton = nullptr;
    QPushButton *m_luksButton = nullptr;
};
