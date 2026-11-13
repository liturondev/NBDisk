/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "devicedetailswidget.h"

#include "nbdcommand.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QStorageInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace
{
QString formatSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("—");
    }
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString formatEndpoint(const QString &host, quint16 port)
{
    if (host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('['))) {
        return QStringLiteral("[%1]:%2").arg(host).arg(port);
    }
    return QStringLiteral("%1:%2").arg(host).arg(port);
}

QString formatEndpoint(const NbdConnectionInfo &connection)
{
    return formatEndpoint(connection.host, connection.port);
}

QIcon luksIcon(bool unlocked)
{
    return unlocked
        ? QIcon::fromTheme(QStringLiteral("system-unlock"),
                           QIcon::fromTheme(QStringLiteral("object-unlocked")))
        : QIcon::fromTheme(QStringLiteral("system-lock-screen"),
                           QIcon::fromTheme(QStringLiteral("object-locked")));
}

QString statusBadgeStyle(bool connected)
{
    if (connected) {
        return QStringLiteral("QLabel { background-color: palette(highlight);"
                              " color: palette(highlighted-text); border-radius: 9px;"
                              " padding: 2px 9px; }");
    }
    return QStringLiteral("QLabel { background-color: palette(mid); color: palette(text);"
                          " border-radius: 9px; padding: 2px 9px; }");
}

QString capacityBarStyle()
{
    return QStringLiteral("QProgressBar { border: none; background-color: palette(mid);"
                          " border-radius: 4px; }"
                          "QProgressBar::chunk { background-color: palette(highlight);"
                          " border-radius: 4px; }");
}

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

constexpr int MountPointColumn = 2;
constexpr int RememberColumn = 3;
}

DeviceDetailsWidget::DeviceDetailsWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    m_diskCard = new QFrame(this);
    m_diskCard->setObjectName(QStringLiteral("diskCard"));
    m_diskCard->setStyleSheet(
        QStringLiteral("QFrame#diskCard { background-color: palette(alternate-base);"
                       " border: 1px solid palette(mid); border-radius: 12px; }"));
    auto *cardLayout = new QVBoxLayout(m_diskCard);
    cardLayout->setContentsMargins(16, 16, 16, 16);
    cardLayout->setSpacing(9);

    auto *topLayout = new QHBoxLayout;
    topLayout->setSpacing(14);

    m_icon = new QLabel(m_diskCard);
    m_icon->setPixmap(QIcon::fromTheme(QStringLiteral("drive-harddisk")).pixmap(48, 48));
    m_icon->setFixedSize(52, 52);
    m_icon->setAlignment(Qt::AlignCenter);
    topLayout->addWidget(m_icon);

    auto *titleLayout = new QVBoxLayout;
    titleLayout->setSpacing(3);
    m_title = new QLabel(m_diskCard);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.25);
    m_title->setFont(titleFont);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_title->setWordWrap(true);
    m_subtitle = new QLabel(m_diskCard);
    m_subtitle->setWordWrap(true);
    titleLayout->addWidget(m_title);
    titleLayout->addWidget(m_subtitle);
    topLayout->addLayout(titleLayout, 1);

    m_statusBadge = new QLabel(m_diskCard);
    m_statusBadge->setAlignment(Qt::AlignCenter);
    topLayout->addWidget(m_statusBadge, 0, Qt::AlignTop);
    cardLayout->addLayout(topLayout);

    m_capacityLabel = new QLabel(m_diskCard);
    m_capacityLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_capacityLabel->setWordWrap(true);
    cardLayout->addWidget(m_capacityLabel);

    m_capacityBars = new QWidget(m_diskCard);
    m_capacityBarsLayout = new QVBoxLayout(m_capacityBars);
    m_capacityBarsLayout->setContentsMargins(0, 0, 0, 0);
    m_capacityBarsLayout->setSpacing(8);
    cardLayout->addWidget(m_capacityBars);

    m_connectionLine = new QLabel(m_diskCard);
    m_connectionLine->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_connectionLine->setWordWrap(true);
    QPalette connectionPalette = m_connectionLine->palette();
    connectionPalette.setColor(QPalette::WindowText,
                               connectionPalette.color(QPalette::PlaceholderText));
    m_connectionLine->setPalette(connectionPalette);
    cardLayout->addWidget(m_connectionLine);

    layout->addWidget(m_diskCard);

    m_startupWidget = new QWidget(this);
    auto *startupLayout = new QVBoxLayout(m_startupWidget);
    startupLayout->setContentsMargins(4, 0, 4, 0);
    startupLayout->setSpacing(4);
    m_autoConnectCheck = new QCheckBox(i18n("Auto-connect on startup"), m_startupWidget);
    m_autoConnectCheck->setToolTip(
        i18n("Reconnect this device from its saved profile when NBDisk starts."));
    m_autoMountCheck = new QCheckBox(i18n("Automount configured volumes"), m_startupWidget);
    m_autoMountCheck->setToolTip(
        i18n("Mount each volume that has a configured mount point after the device connects."));
    startupLayout->addWidget(m_autoConnectCheck);
    startupLayout->addWidget(m_autoMountCheck);
    layout->addWidget(m_startupWidget);

    m_profileGroup = new QGroupBox(i18n("Saved profile"), this);
    auto *profileLayout = new QVBoxLayout(m_profileGroup);
    profileLayout->setSpacing(8);
    m_profileSummary = new QLabel(m_profileGroup);
    m_profileSummary->setWordWrap(true);
    m_profileSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    profileLayout->addWidget(m_profileSummary);
    auto *profileButtons = new QHBoxLayout;
    profileButtons->setContentsMargins(0, 0, 0, 0);
    m_editProfileButton = new QPushButton(
        QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Edit Profile…"),
        m_profileGroup);
    m_editProfileButton->setToolTip(
        i18n("Change the saved server, export, backend, and connection options."));
    m_deleteProfileButton = new QPushButton(
        QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Delete Profile"),
        m_profileGroup);
    m_deleteProfileButton->setToolTip(
        i18n("Remove the saved profile and any remembered LUKS passphrases."));
    profileButtons->addWidget(m_editProfileButton);
    profileButtons->addWidget(m_deleteProfileButton);
    profileButtons->addStretch();
    profileLayout->addLayout(profileButtons);
    m_profileGroup->hide();
    layout->addWidget(m_profileGroup);

    m_volumeGroup = new QGroupBox(i18n("Volumes"), this);
    auto *volumeLayout = new QVBoxLayout(m_volumeGroup);

    m_volumeTable = new QTableWidget(m_volumeGroup);
    m_volumeTable->setColumnCount(4);
    m_volumeTable->setHorizontalHeaderLabels(
        {i18n("Volume"), i18n("Size"), i18n("Mount point"),
         i18n("Remember passphrase")});
    m_volumeTable->verticalHeader()->setVisible(false);
    m_volumeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_volumeTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_volumeTable->setEditTriggers(QAbstractItemView::DoubleClicked
                                   | QAbstractItemView::SelectedClicked
                                   | QAbstractItemView::EditKeyPressed);
    m_volumeTable->setAlternatingRowColors(true);
    m_volumeTable->setShowGrid(false);
    m_volumeTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_volumeTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_volumeTable->horizontalHeader()->setSectionResizeMode(MountPointColumn,
                                                            QHeaderView::Stretch);
    m_volumeTable->horizontalHeader()->setSectionResizeMode(RememberColumn,
                                                            QHeaderView::ResizeToContents);
    m_volumeTable->setMinimumHeight(150);
    volumeLayout->addWidget(m_volumeTable);

    auto *buttonLayout = new QHBoxLayout;
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    m_mountButton = new QPushButton(QIcon::fromTheme(QStringLiteral("media-mount")),
                                    i18n("Mount"), m_volumeGroup);
    m_unmountButton = new QPushButton(QIcon::fromTheme(QStringLiteral("media-eject")),
                                      i18n("Unmount"), m_volumeGroup);
    m_browseMountPointButton = new QPushButton(
        QIcon::fromTheme(QStringLiteral("folder-open")), i18n("Mount point…"),
        m_volumeGroup);
    m_openButton = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-open")),
                                   i18n("Open"), m_volumeGroup);
    m_luksButton = new QPushButton(i18n("Unlock"), m_volumeGroup);
    m_luksButton->setIcon(luksIcon(false));
    m_luksButton->setIconSize(QSize(16, 16));
    m_luksButton->setVisible(false);
    buttonLayout->addWidget(m_mountButton);
    buttonLayout->addWidget(m_unmountButton);
    buttonLayout->addWidget(m_luksButton);
    buttonLayout->addWidget(m_browseMountPointButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_openButton);
    volumeLayout->addLayout(buttonLayout);
    layout->addWidget(m_volumeGroup);

    layout->addStretch(1);

    connect(m_autoConnectCheck, &QCheckBox::toggled, this,
            &DeviceDetailsWidget::autoConnectChanged);
    connect(m_autoMountCheck, &QCheckBox::toggled, this,
            &DeviceDetailsWidget::autoMountChanged);
    connect(m_volumeTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) {
                if (!m_hasProfile) {
                    return;
                }
                const int row = item->row();
                if (row < 0 || row >= m_device.volumes.size()) {
                    return;
                }

                const NbdVolume &volume = m_device.volumes.at(row);
                if (item->column() == RememberColumn) {
                    if (!volume.encrypted) {
                        return;
                    }
                    Q_EMIT rememberPassphraseChanged(
                        volume.path, item->checkState() == Qt::Checked);
                    return;
                }
                if (item->column() != MountPointColumn) {
                    return;
                }
                const QString text = item->text().trimmed();
                if (text.isEmpty()) {
                    m_profile.mountPoints.remove(volume.path);
                    Q_EMIT mountPointChanged(volume.path, QString());
                    return;
                }

                const QString mountPoint = QDir::cleanPath(text);
                if (!QDir::isAbsolutePath(mountPoint)
                    || mountPoint == QStringLiteral("/")) {
                    QSignalBlocker blocker(m_volumeTable);
                    item->setText(m_profile.mountPoints.value(volume.path));
                    item->setToolTip(i18n("Choose an absolute path other than /."));
                    QMessageBox::warning(
                        this, i18n("Invalid Mount Point"),
                        i18n("The mount point must be an absolute path other than /."));
                    return;
                }

                if (m_profile.mountPoints.value(volume.path) != mountPoint) {
                    m_profile.mountPoints.insert(volume.path, mountPoint);
                    Q_EMIT mountPointChanged(volume.path, mountPoint);
                }
            });
    connect(m_volumeTable, &QTableWidget::itemSelectionChanged, this,
            &DeviceDetailsWidget::updateButtons);
    connect(m_volumeTable, &QTableWidget::itemSelectionChanged, this,
            &DeviceDetailsWidget::updateSelectionDetails);
    connect(m_mountButton, &QPushButton::clicked, this, [this]() {
        const QString path = currentVolumePath();
        if (!path.isEmpty()) {
            Q_EMIT mountRequested(path);
        }
    });
    connect(m_unmountButton, &QPushButton::clicked, this, [this]() {
        const QString path = currentVolumePath();
        if (!path.isEmpty()) {
            Q_EMIT unmountRequested(path);
        }
    });
    connect(m_luksButton, &QPushButton::clicked, this, [this]() {
        const int row = m_volumeTable->currentRow();
        if (row < 0 || row >= m_device.volumes.size()) {
            return;
        }
        const NbdVolume &volume = m_device.volumes.at(row);
        if (!volume.encrypted) {
            return;
        }
        if (volume.unlocked) {
            Q_EMIT lockRequested(volume.path);
        } else {
            Q_EMIT unlockRequested(volume.path);
        }
    });
    connect(m_browseMountPointButton, &QPushButton::clicked, this, [this]() {
        const int row = m_volumeTable->currentRow();
        if (!m_hasProfile || row < 0 || row >= m_device.volumes.size()) {
            return;
        }
        QTableWidgetItem *item = m_volumeTable->item(row, MountPointColumn);
        if (!item) {
            return;
        }
        const QString directory = QFileDialog::getExistingDirectory(
            this, i18n("Select Mount Point"),
            item->text().isEmpty() ? QStringLiteral("/mnt") : item->text(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!directory.isEmpty()) {
            item->setText(directory);
        }
    });
    connect(m_openButton, &QPushButton::clicked, this, [this]() {
        const QString path = currentMountPoint();
        if (!path.isEmpty()) {
            Q_EMIT openRequested(path);
        }
    });
    connect(m_editProfileButton, &QPushButton::clicked, this,
            &DeviceDetailsWidget::editProfileRequested);
    connect(m_deleteProfileButton, &QPushButton::clicked, this,
            &DeviceDetailsWidget::deleteProfileRequested);

    clear();
}

void DeviceDetailsWidget::setDevice(const NbdDevice &device)
{
    const QString previousVolume = currentVolumePath();
    m_device = device;
    m_hasDevice = true;
    populate();

    int rowToSelect = 0;
    if (!previousVolume.isEmpty()) {
        for (int row = 0; row < m_device.volumes.size(); ++row) {
            if (m_device.volumes.at(row).path == previousVolume) {
                rowToSelect = row;
                break;
            }
        }
    }
    if (!m_device.volumes.isEmpty()) {
        m_volumeTable->selectRow(rowToSelect);
    }
    updateButtons();
    updateSelectionDetails();
}

void DeviceDetailsWidget::setProfile(const NbdProfile &profile, bool hasProfile)
{
    QSignalBlocker blocker(m_autoConnectCheck);
    QSignalBlocker autoMountBlocker(m_autoMountCheck);

    m_hasProfile = hasProfile && profile.isValid();
    m_profile = m_hasProfile ? profile : NbdProfile();

    m_profileGroup->setVisible(m_hasProfile);
    if (!m_hasProfile) {
        m_autoConnectCheck->setChecked(false);
        m_autoConnectCheck->setEnabled(false);
        m_autoMountCheck->setChecked(false);
        m_autoMountCheck->setEnabled(false);
        updateProfileSummary();
        updateMountPointColumn();
        updateRememberColumn();
        updateButtons();
        return;
    }

    m_autoConnectCheck->setEnabled(true);
    m_autoConnectCheck->setChecked(profile.autoConnect);
    m_autoMountCheck->setEnabled(true);
    m_autoMountCheck->setChecked(profile.autoMount);
    updateProfileSummary();
    updateMountPointColumn();
    updateRememberColumn();
    updateButtons();
}

void DeviceDetailsWidget::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    updateButtons();
}

void DeviceDetailsWidget::clear()
{
    m_hasDevice = false;
    m_device = NbdDevice();
    m_title->setText(i18n("No device selected"));
    m_subtitle->setText(i18n("Select an NBD device to view its capacity and volumes."));
    m_statusBadge->setText(i18n("No device"));
    m_statusBadge->setStyleSheet(statusBadgeStyle(false));
    m_capacityLabel->setText(i18n("Capacity unavailable"));
    m_capacityLabel->setToolTip(QString());
    m_capacityLabel->setVisible(true);
    clearCapacityBars();
    m_capacityBars->setVisible(false);
    m_connectionLine->clear();
    m_connectionLine->setVisible(false);
    m_startupWidget->setVisible(false);
    m_profileGroup->setVisible(false);
    m_volumeGroup->setVisible(false);

    QSignalBlocker blocker(m_autoConnectCheck);
    QSignalBlocker autoMountBlocker(m_autoMountCheck);
    m_autoConnectCheck->setChecked(false);
    m_autoConnectCheck->setEnabled(false);
    m_autoMountCheck->setChecked(false);
    m_autoMountCheck->setEnabled(false);
    m_hasProfile = false;
    m_profile = NbdProfile();
    updateProfileSummary();
    updateMountPointColumn();
}

void DeviceDetailsWidget::populate()
{
    QSignalBlocker tableBlocker(m_volumeTable);

    const QString diskTitle =
        m_device.connected && !m_device.connection.exportName.isEmpty()
        ? m_device.connection.exportName
        : i18n("NBD Disk");
    m_title->setText(diskTitle);

    QString status;
    if (!m_device.connected) {
        status = i18n("Not connected");
    } else if (m_device.isExternallyConnected()) {
        status = i18n("Connected outside NBDisk");
    } else {
        status = m_device.readOnly ? i18n("Connected · read-only") : i18n("Connected");
    }
    m_subtitle->setText(i18n("%1 · %2", m_device.path, status));

    if (!m_device.connected) {
        m_statusBadge->setText(i18n("Offline"));
    } else if (m_device.isExternallyConnected()) {
        m_statusBadge->setText(i18n("External"));
    } else if (m_device.readOnly) {
        m_statusBadge->setText(i18n("Read-only"));
    } else {
        m_statusBadge->setText(i18n("Connected"));
    }
    m_statusBadge->setStyleSheet(statusBadgeStyle(m_device.connected));

    clearCapacityBars();
    bool hasUsageBar = false;
    if (m_device.connected) {
        for (const NbdVolume &volume : m_device.volumes) {
            if (volume.mountPoints.isEmpty()) {
                continue;
            }

            const QStorageInfo storage(volume.mountPoints.first());
            if (!storage.isValid() || !storage.isReady()) {
                continue;
            }

            const qint64 total = storage.bytesTotal();
            if (total <= 0) {
                continue;
            }
            const qint64 available = qBound<qint64>(0, storage.bytesAvailable(), total);
            const qint64 used = total - available;
            const int usedPercent = static_cast<int>((used * 100) / total);

            auto *row = new QWidget(m_capacityBars);
            auto *rowLayout = new QVBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(3);

            auto *label = new QLabel(row);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setWordWrap(true);
            label->setText(
                i18n("%1 · %2 used of %3", volume.name,
                     QLocale().formattedDataSize(used, 1,
                                                 QLocale::DataSizeTraditionalFormat),
                     QLocale().formattedDataSize(total, 1,
                                                 QLocale::DataSizeTraditionalFormat)));
            label->setToolTip(
                i18n("%1 free",
                     QLocale().formattedDataSize(available, 1,
                                                 QLocale::DataSizeTraditionalFormat)));

            auto *bar = new QProgressBar(row);
            bar->setRange(0, 100);
            bar->setValue(usedPercent);
            bar->setTextVisible(false);
            bar->setFixedHeight(8);
            bar->setStyleSheet(capacityBarStyle());
            bar->setAccessibleName(i18n("Storage usage for %1", volume.name));

            rowLayout->addWidget(label);
            rowLayout->addWidget(bar);
            m_capacityBarsLayout->addWidget(row);
            hasUsageBar = true;
        }
    }

    m_capacityBars->setVisible(hasUsageBar);
    m_capacityLabel->setVisible(!hasUsageBar);
    if (hasUsageBar) {
        m_capacityLabel->clear();
        m_capacityLabel->setToolTip(QString());
    } else if (m_device.connected && m_device.size > 0) {
        m_capacityLabel->setText(i18n("%1 total capacity", formatSize(m_device.size)));
        m_capacityLabel->setToolTip(QString());
    } else if (m_device.connected) {
        m_capacityLabel->setText(i18n("Capacity unavailable"));
        m_capacityLabel->setToolTip(QString());
    } else {
        m_capacityLabel->setText(i18n("Not connected"));
        m_capacityLabel->setToolTip(QString());
    }

    QStringList connectionDetails;
    if (m_device.connected) {
        if (m_device.connection.isValid()) {
            connectionDetails.append(formatEndpoint(m_device.connection));
            if (!m_device.connection.exportName.isEmpty()) {
                connectionDetails.append(m_device.connection.exportName);
            }
            connectionDetails.append(
                NbdCommandBuilder::backendName(m_device.connection.backend));
        } else {
            connectionDetails.append(i18n("Connected outside NBDisk"));
        }
    }
    m_connectionLine->setText(connectionDetails.join(QStringLiteral(" · ")));
    m_connectionLine->setVisible(!connectionDetails.isEmpty());
    m_startupWidget->setVisible(true);

    const bool showVolumes = m_device.connected && !m_device.volumes.isEmpty();
    m_volumeGroup->setVisible(showVolumes);
    m_volumeTable->setRowCount(0);
    if (showVolumes) {
        m_volumeTable->setRowCount(static_cast<int>(m_device.volumes.size()));
        for (int row = 0; row < m_device.volumes.size(); ++row) {
            const NbdVolume &volume = m_device.volumes.at(row);
            auto *nameItem = readOnlyItem(volume.name);
            if (volume.encrypted) {
                nameItem->setIcon(luksIcon(volume.unlocked));
                nameItem->setToolTip(
                    volume.unlocked ? i18n("LUKS encrypted volume (unlocked)")
                                    : i18n("LUKS encrypted volume (locked)"));
            }
            m_volumeTable->setItem(row, 0, nameItem);
            auto *sizeItem = readOnlyItem(formatSize(volume.size));
            sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_volumeTable->setItem(row, 1, sizeItem);

            auto *mountPointItem = readOnlyItem(
                m_hasProfile ? m_profile.mountPoints.value(volume.path) : QString());
            mountPointItem->setToolTip(
                i18n("Optional. Leave empty to use the default udisks mount point."));
            m_volumeTable->setItem(row, MountPointColumn, mountPointItem);

            auto *rememberItem = readOnlyItem(QString());
            rememberItem->setToolTip(
                i18n("Store this volume's LUKS passphrase in the wallet so it can be "
                     "unlocked automatically."));
            m_volumeTable->setItem(row, RememberColumn, rememberItem);
        }
    }

    updateProfileSummary();
    updateMountPointColumn();
    updateRememberColumn();
}


QString DeviceDetailsWidget::currentVolumePath() const
{
    const int row = m_volumeTable->currentRow();
    if (!m_hasDevice || row < 0 || row >= m_device.volumes.size()) {
        return {};
    }
    return m_device.volumes.at(row).path;
}

bool DeviceDetailsWidget::isEditingMountPoint() const
{
    return m_volumeTable->findChild<QLineEdit *>() != nullptr;
}

QString DeviceDetailsWidget::currentMountPoint() const
{
    const int row = m_volumeTable->currentRow();
    if (!m_hasDevice || row < 0 || row >= m_device.volumes.size()) {
        return {};
    }
    const NbdVolume &volume = m_device.volumes.at(row);
    return volume.mountPoints.isEmpty() ? QString() : volume.mountPoints.first();
}

void DeviceDetailsWidget::updateButtons()
{
    const int row = m_volumeTable->currentRow();
    const bool hasVolume =
        m_hasDevice && row >= 0 && row < m_device.volumes.size() && m_device.connected;
    const NbdVolume *volume = hasVolume ? &m_device.volumes.at(row) : nullptr;
    const bool mounted = volume && volume->isMounted();
    const bool encrypted = volume && volume->encrypted;
    const bool locked = encrypted && !volume->unlocked;
    const bool unlocked = encrypted && volume->unlocked;

    // While an operation is running the service ignores new requests, so make
    // that visible instead of accepting a click that does nothing.
    const bool actionsEnabled = !m_busy;
    m_mountButton->setEnabled(actionsEnabled && hasVolume && !mounted && !locked);
    m_unmountButton->setEnabled(actionsEnabled && mounted);
    m_browseMountPointButton->setEnabled(actionsEnabled && hasVolume && m_hasProfile);
    m_openButton->setEnabled(actionsEnabled && mounted);
    m_editProfileButton->setEnabled(actionsEnabled && m_hasProfile);
    m_deleteProfileButton->setEnabled(actionsEnabled && m_hasProfile);

    m_luksButton->setVisible(hasVolume && encrypted);
    if (!hasVolume || !encrypted) {
        return;
    }

    m_luksButton->setEnabled(actionsEnabled && (locked || (unlocked && !mounted)));
    m_luksButton->setIcon(luksIcon(unlocked));
    m_luksButton->setText(locked ? i18n("Unlock") : i18n("Lock"));
    if (locked) {
        m_luksButton->setToolTip(i18n("Unlock this LUKS volume…"));
        m_luksButton->setAccessibleName(i18n("Unlock encrypted volume"));
    } else if (mounted) {
        m_luksButton->setToolTip(i18n("Unmount the volume before locking it."));
        m_luksButton->setAccessibleName(i18n("Lock encrypted volume"));
    } else {
        m_luksButton->setToolTip(i18n("Lock this LUKS volume"));
        m_luksButton->setAccessibleName(i18n("Lock encrypted volume"));
    }
}

void DeviceDetailsWidget::updateMountPointColumn()
{
    QSignalBlocker blocker(m_volumeTable);
    for (int row = 0; row < m_device.volumes.size(); ++row) {
        QTableWidgetItem *item = m_volumeTable->item(row, MountPointColumn);
        if (!item) {
            continue;
        }
        const NbdVolume &volume = m_device.volumes.at(row);
        Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
        if (m_hasProfile) {
            flags |= Qt::ItemIsEditable;
        }
        item->setFlags(flags);
        item->setText(m_hasProfile ? m_profile.mountPoints.value(volume.path) : QString());
        item->setToolTip(
            m_hasProfile
                ? i18n("Optional. Leave empty to use the default udisks mount point.")
                : i18n("Connect this device once to save a mount point."));
    }
}

void DeviceDetailsWidget::updateRememberColumn()
{
    QSignalBlocker blocker(m_volumeTable);
    for (int row = 0; row < m_device.volumes.size(); ++row) {
        QTableWidgetItem *item = m_volumeTable->item(row, RememberColumn);
        if (!item) {
            continue;
        }
        const NbdVolume &volume = m_device.volumes.at(row);
        const bool available = m_hasProfile && volume.encrypted;
        Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
        if (available) {
            flags |= Qt::ItemIsUserCheckable;
        }
        item->setFlags(flags);
        item->setCheckState(available && m_profile.rememberedVolumes.contains(volume.path)
                                ? Qt::Checked
                                : Qt::Unchecked);
        if (!volume.encrypted) {
            item->setText(QString());
            item->setToolTip(i18n("Only LUKS encrypted volumes can remember a passphrase."));
        } else if (!m_hasProfile) {
            item->setText(QString());
            item->setToolTip(i18n("Connect this device once to save a profile first."));
        } else {
            item->setText(QString());
            item->setToolTip(
                i18n("Store this volume's LUKS passphrase in the wallet so it can be "
                     "unlocked automatically."));
        }
    }
}

void DeviceDetailsWidget::updateProfileSummary()
{
    if (!m_hasProfile || !m_profile.isValid()) {
        m_profileSummary->clear();
        return;
    }

    const ConnectOptions &options = m_profile.options;
    QString endpoint = options.host.contains(QLatin1Char(':'))
            && !options.host.startsWith(QLatin1Char('['))
        ? QStringLiteral("[%1]:%2").arg(options.host).arg(options.port)
        : QStringLiteral("%1:%2").arg(options.host).arg(options.port);

    QStringList details;
    details.append(i18n("Server: %1", endpoint));
    details.append(i18n("Export: %1",
                        options.exportName.isEmpty() ? i18n("default") : options.exportName));
    details.append(i18n("Backend: %1", NbdCommandBuilder::backendName(options.backend)));

    QStringList flags;
    if (options.readOnly) {
        flags.append(i18n("read-only"));
    }
    if (options.tls) {
        flags.append(i18n("TLS"));
    }
    if (options.persistent) {
        flags.append(i18n("persistent reconnect"));
    }
    if (options.timeoutSeconds > 0) {
        flags.append(i18n("timeout %1 s", options.timeoutSeconds));
    }
    if (options.blockSize != 512) {
        flags.append(i18n("block size %1", options.blockSize));
    }
    if (!flags.isEmpty()) {
        details.append(flags.join(QStringLiteral(" · ")));
    }

    QStringList startup;
    startup.append(m_profile.autoConnect ? i18n("auto-connect") : i18n("manual"));
    if (m_profile.autoMount) {
        startup.append(i18n("automount"));
    }
    if (!m_profile.rememberedVolumes.isEmpty()) {
        startup.append(i18n("%1 remembered passphrase(s)",
                            m_profile.rememberedVolumes.size()));
    }
    details.append(startup.join(QStringLiteral(" · ")));

    if (m_profile.lastConnected.isValid()) {
        details.append(i18n("Last connected: %1",
                            QLocale().toString(m_profile.lastConnected,
                                               QLocale::ShortFormat)));
    }

    m_profileSummary->setText(details.join(QLatin1Char('\n')));
}

void DeviceDetailsWidget::clearCapacityBars()
{
    while (QLayoutItem *item = m_capacityBarsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void DeviceDetailsWidget::updateSelectionDetails()
{
    const int row = m_volumeTable->currentRow();
    if (!m_hasDevice || row < 0 || row >= m_device.volumes.size()) {
        return;
    }
    const NbdVolume &volume = m_device.volumes.at(row);
    QString details = i18n("%1 — %2", volume.path, formatSize(volume.size));
    if (volume.encrypted) {
        details += i18n(" · LUKS encrypted (%1)",
                        volume.unlocked ? i18n("unlocked") : i18n("locked"));
    }
    m_volumeTable->setToolTip(details);
}
