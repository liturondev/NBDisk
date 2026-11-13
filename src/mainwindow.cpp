/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mainwindow.h"

#include "backgroundserviceclient.h"
#include "backgroundservicedialog.h"
#include "connectdialog.h"
#include "devicedetailswidget.h"
#include "nbdcommand.h"
#include "nbddevicemodel.h"
#include "unlockdialog.h"

#include <KAboutApplicationDialog>
#include <KAboutData>
#include <KActionCollection>
#include <KLocalizedString>
#include <KStandardAction>
#include <KToolBar>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSize>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : KXmlGuiWindow(parent)
    , m_service(new NbdService(this))
    , m_model(new NbdDeviceModel(this))
{
    setObjectName(QStringLiteral("MainWindow"));
    // main() owns this window on the stack; KXmlGui may otherwise request a
    // deferred delete when the window is closed.
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowTitle(i18n("NBDisk"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.liturondev.NBDisk"),
                                   QIcon(QStringLiteral(":/icons/io.github.liturondev.NBDisk.svg"))));

    setupUi();
    setupActions();

    setupGUI(QSize(1120, 700), Default, QStringLiteral(":/nbdiskui.rc"));
    setAutoSaveSettings();

    if (KToolBar *mainToolBar = toolBar(QStringLiteral("mainToolBar"))) {
        mainToolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        mainToolBar->setIconSize(QSize(22, 22));
    }

    m_countLabel = new QLabel(this);
    m_backendLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_backendLabel);
    statusBar()->addPermanentWidget(m_countLabel);

    connect(m_service, &NbdService::operationStarted, this,
            &MainWindow::handleOperationStarted);
    connect(m_service, &NbdService::operationFinished, this,
            &MainWindow::handleOperationFinished);
    connect(m_service, &NbdService::stateChanged, this, &MainWindow::refresh);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(4000);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::refresh);
    m_refreshTimer->start();

    QTimer::singleShot(0, this, [this]() {
        refresh();
        maybeAutoConnectProfiles();
    });
}

void MainWindow::setupUi()
{
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    m_message = new KMessageWidget(central);
    m_message->setCloseButtonVisible(true);
    m_message->setWordWrap(true);
    m_message->hide();
    layout->addWidget(m_message);

    m_view = new QTableView(central);
    m_view->setModel(m_model);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setAlternatingRowColors(true);
    m_view->setShowGrid(false);
    m_view->setWordWrap(false);
    m_view->setSortingEnabled(false);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setTextElideMode(Qt::ElideMiddle);
    m_view->verticalHeader()->hide();
    m_view->horizontalHeader()->setSectionResizeMode(NbdDeviceModel::DeviceColumn,
                                                     QHeaderView::ResizeToContents);
    m_view->horizontalHeader()->setSectionResizeMode(NbdDeviceModel::StatusColumn,
                                                     QHeaderView::ResizeToContents);
    m_view->horizontalHeader()->setSectionResizeMode(NbdDeviceModel::AutoConnectColumn,
                                                     QHeaderView::ResizeToContents);
    m_view->horizontalHeader()->setSectionResizeMode(NbdDeviceModel::SizeColumn,
                                                     QHeaderView::ResizeToContents);
    m_view->horizontalHeader()->setSectionResizeMode(NbdDeviceModel::ServerColumn,
                                                     QHeaderView::ResizeToContents);
    m_view->horizontalHeader()->setSectionResizeMode(NbdDeviceModel::MountPointColumn,
                                                     QHeaderView::Stretch);

    auto *splitter = new QSplitter(Qt::Horizontal, central);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(m_view);
    m_details = new DeviceDetailsWidget(splitter);
    splitter->addWidget(m_details);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({720, 380});
    layout->addWidget(splitter, 1);

    setCentralWidget(central);

    connect(m_view->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &MainWindow::selectionChanged);
    connect(m_view, &QTableView::customContextMenuRequested, this,
            &MainWindow::showContextMenu);
    connect(m_details, &DeviceDetailsWidget::autoConnectChanged, this,
            [this](bool enabled) {
                if (const auto device = selectedDevice()) {
                    m_service->setProfileAutoConnect(device->path, enabled);
                }
            });
    connect(m_details, &DeviceDetailsWidget::autoMountChanged, this,
            [this](bool enabled) {
                if (const auto device = selectedDevice()) {
                    if (enabled) {
                        for (const NbdVolume &volume : device->volumes) {
                            m_autoMountAttempted.remove(volume.path);
                        }
                    }
                    m_service->setProfileAutoMount(device->path, enabled);
                }
            });
    connect(m_details, &DeviceDetailsWidget::mountPointChanged, this,
            [this](const QString &volumePath, const QString &mountPoint) {
                if (const auto device = selectedDevice()) {
                    m_autoMountAttempted.remove(volumePath);
                    m_service->setProfileMountPoint(device->path, volumePath, mountPoint);
                }
            });
    connect(m_details, &DeviceDetailsWidget::rememberPassphraseChanged, this,
            [this](const QString &volumePath, bool remember) {
                if (const auto device = selectedDevice()) {
                    m_service->setProfileRememberPassphrase(device->path, volumePath,
                                                           remember);
                }
            });
    connect(m_details, &DeviceDetailsWidget::editProfileRequested, this,
            &MainWindow::editSelectedProfile);
    connect(m_details, &DeviceDetailsWidget::deleteProfileRequested, this,
            &MainWindow::deleteSelectedProfile);
    connect(m_details, &DeviceDetailsWidget::mountRequested, this,
            &MainWindow::mountSelected);
    connect(m_details, &DeviceDetailsWidget::unmountRequested, m_service,
            &NbdService::unmountVolume);
    connect(m_details, &DeviceDetailsWidget::unlockRequested, this,
            &MainWindow::unlockSelectedVolume);
    connect(m_details, &DeviceDetailsWidget::lockRequested, m_service,
            &NbdService::lockVolume);
    connect(m_details, &DeviceDetailsWidget::openRequested, this,
            [](const QString &mountPoint) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(mountPoint));
            });

    m_details->clear();
}

void MainWindow::setupActions()
{
    m_connectAction = new QAction(QIcon::fromTheme(QStringLiteral("network-connect")),
                                  i18n("&Connect…"), this);
    m_connectAction->setStatusTip(i18n("Connect a remote NBD export to a local device"));
    actionCollection()->addAction(QStringLiteral("connect"), m_connectAction);
    actionCollection()->setDefaultShortcut(m_connectAction,
                                           QKeySequence(QStringLiteral("Ctrl+N")));
    connect(m_connectAction, &QAction::triggered, this, &MainWindow::connectSelected);

    m_disconnectAction = new QAction(QIcon::fromTheme(QStringLiteral("network-disconnect")),
                                     i18n("&Disconnect"), this);
    m_disconnectAction->setStatusTip(i18n("Disconnect the selected NBD device"));
    actionCollection()->addAction(QStringLiteral("disconnect"), m_disconnectAction);
    actionCollection()->setDefaultShortcut(m_disconnectAction,
                                           QKeySequence(QStringLiteral("Ctrl+D")));
    connect(m_disconnectAction, &QAction::triggered, this, &MainWindow::disconnectSelected);

    m_loadModuleAction = new QAction(QIcon::fromTheme(QStringLiteral("list-add")),
                                     i18n("Load NBD &Module"), this);
    m_loadModuleAction->setStatusTip(
        i18n("Load the nbd kernel module with modprobe"));
    actionCollection()->addAction(QStringLiteral("load_nbd_module"), m_loadModuleAction);
    connect(m_loadModuleAction, &QAction::triggered, this, &MainWindow::loadNbdModule);

    m_refreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                  i18n("&Refresh"), this);
    m_refreshAction->setStatusTip(i18n("Rescan NBD devices and mounts"));
    actionCollection()->addAction(QStringLiteral("refresh"), m_refreshAction);
    actionCollection()->setDefaultShortcut(m_refreshAction, QKeySequence(Qt::Key_F5));
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::refresh);

    m_mountAction = new QAction(QIcon::fromTheme(QStringLiteral("media-mount")),
                                i18n("&Mount Volume"), this);
    m_mountAction->setStatusTip(i18n("Mount the selected volume"));
    actionCollection()->addAction(QStringLiteral("mount"), m_mountAction);
    actionCollection()->setDefaultShortcut(m_mountAction,
                                           QKeySequence(QStringLiteral("Ctrl+M")));
    connect(m_mountAction, &QAction::triggered, this, &MainWindow::mountSelected);

    m_unmountAction = new QAction(QIcon::fromTheme(QStringLiteral("media-eject")),
                                  i18n("&Unmount Volume"), this);
    m_unmountAction->setStatusTip(i18n("Unmount the selected volume"));
    actionCollection()->addAction(QStringLiteral("unmount"), m_unmountAction);
    actionCollection()->setDefaultShortcut(m_unmountAction,
                                           QKeySequence(QStringLiteral("Ctrl+U")));
    connect(m_unmountAction, &QAction::triggered, this, &MainWindow::unmountSelected);

    m_openAction = new QAction(QIcon::fromTheme(QStringLiteral("folder-open")),
                               i18n("&Open in File Manager"), this);
    m_openAction->setStatusTip(i18n("Open the selected volume's mount point"));
    actionCollection()->addAction(QStringLiteral("open_mount"), m_openAction);
    actionCollection()->setDefaultShortcut(m_openAction,
                                           QKeySequence(QStringLiteral("Ctrl+O")));
    connect(m_openAction, &QAction::triggered, this, &MainWindow::openSelected);

    m_autostartAction = new QAction(i18n("Start at &Login"), this);
    m_autostartAction->setCheckable(true);
    m_autostartAction->setChecked(QFileInfo::exists(autostartFilePath()));
    m_autostartAction->setStatusTip(
        i18n("Reconnect saved network disks in the background when you log in"));
    actionCollection()->addAction(QStringLiteral("start_at_login"), m_autostartAction);
    connect(m_autostartAction, &QAction::toggled, this, &MainWindow::setAutostartEnabled);

    m_serviceAction = new QAction(i18n("Background &Service…"), this);
    m_serviceAction->setStatusTip(
        i18n("Check or trigger the login background service"));
    actionCollection()->addAction(QStringLiteral("background_service"), m_serviceAction);
    connect(m_serviceAction, &QAction::triggered, this,
            &MainWindow::showBackgroundService);

    m_editProfileAction = new QAction(QIcon::fromTheme(QStringLiteral("document-edit")),
                                      i18n("&Edit Profile…"), this);
    m_editProfileAction->setStatusTip(
        i18n("Change the saved connection details for the selected device"));
    actionCollection()->addAction(QStringLiteral("edit_profile"), m_editProfileAction);
    connect(m_editProfileAction, &QAction::triggered, this,
            &MainWindow::editSelectedProfile);

    m_deleteProfileAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                        i18n("&Delete Profile"), this);
    m_deleteProfileAction->setStatusTip(
        i18n("Remove the saved profile and remembered passphrases"));
    actionCollection()->addAction(QStringLiteral("delete_profile"),
                                  m_deleteProfileAction);
    connect(m_deleteProfileAction, &QAction::triggered, this,
            &MainWindow::deleteSelectedProfile);

    KStandardAction::quit(qApp, &QCoreApplication::quit, actionCollection());
    KStandardAction::aboutApp(this, &MainWindow::showAbout, actionCollection());
}

void MainWindow::refresh()
{
    if (m_busy) {
        return;
    }
    // Scanning behind a modal dialog only stalls it and can change the device
    // list while the user is still answering. Its own completion triggers the
    // next refresh.
    if (QApplication::activeModalWidget()) {
        return;
    }

    m_refreshing = true;
    const QString previousPath = selectedDevicePath();
    QString error;
    const QList<NbdDevice> devices = m_service->scanDevices(&error);
    m_model->setDevices(devices);

    if (!previousPath.isEmpty()) {
        selectDevicePath(previousPath);
    }
    if (m_model->rowCount() > 0 && m_view->currentIndex().row() < 0) {
        m_view->selectRow(0);
    }

    updateDetails();
    updateActions();
    updateStatusBar();

    if (!error.isEmpty()) {
        const bool environmentChanged = error != m_environmentError;
        m_environmentError = error;
        if (environmentChanged && m_message->messageType() != KMessageWidget::Error) {
            showEnvironmentMessage(error);
        }
    } else if (!m_environmentError.isEmpty()) {
        const bool showingEnvironmentError =
            m_message->messageType() == KMessageWidget::Warning
            && m_message->text() == m_environmentError;
        m_environmentError.clear();
        if (showingEnvironmentError) {
            m_message->animatedHide();
        }
    }

    maybeAutoConnectProfiles();
    maybeAutoMountVolumes();
    m_refreshing = false;
}

void MainWindow::selectionChanged()
{
    updateDetails();
    updateActions();
}

void MainWindow::connectSelected()
{
    if (m_busy) {
        return;
    }
    if (!m_service->backends().any()) {
        showMessage(KMessageWidget::Error,
                    i18n("Install nbd-client or qemu-nbd to connect network disks."));
        return;
    }

    QList<NbdDevice> disconnectedDevices;
    for (const NbdDevice &device : m_model->devices()) {
        if (!device.connected) {
            disconnectedDevices.append(device);
        }
    }
    if (disconnectedDevices.isEmpty()) {
        showMessage(KMessageWidget::Warning, i18n("All NBD devices are already connected."));
        return;
    }

    // Fall back to the first free device so its saved profile still prefills
    // the wizard when the current row is connected or nothing is selected.
    QString initialDevice;
    if (const auto device = selectedDevice(); device && !device->connected) {
        initialDevice = device->path;
    } else {
        initialDevice = disconnectedDevices.constFirst().path;
    }

    const auto profile = initialDevice.isEmpty()
        ? std::optional<NbdProfile>()
        : m_service->profileForDevice(initialDevice);
    ConnectDialog dialog(disconnectedDevices, m_service->backends(), initialDevice, this,
                         profile.value_or(NbdProfile()));
    if (dialog.exec() == QDialog::Accepted) {
        const ConnectOptions options = dialog.connectOptions();
        m_service->saveProfile(options, dialog.autoConnect());
        m_service->connectDevice(options);
    }
}

void MainWindow::disconnectSelected()
{
    const auto device = selectedDevice();
    if (!device || !device->connected || m_busy) {
        return;
    }

    QStringList mountedVolumes;
    for (const NbdVolume &volume : device->volumes) {
        if (volume.isMounted()) {
            mountedVolumes.append(volume.path);
        }
    }

    if (!mountedVolumes.isEmpty()) {
        QMessageBox box(this);
        box.setWindowTitle(i18n("Unmount Volumes?"));
        box.setIcon(QMessageBox::Warning);
        box.setText(i18n("The selected device has mounted volumes."));
        box.setInformativeText(
            i18n("The volumes must be unmounted before the device can be disconnected."));
        auto *unmountButton =
            box.addButton(i18n("Unmount and Disconnect"), QMessageBox::AcceptRole);
        auto *cancelButton = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancelButton);
        box.exec();
        if (box.clickedButton() != unmountButton) {
            return;
        }
    }

    for (const NbdVolume &volume : device->volumes) {
        m_autoMountAttempted.remove(volume.path);
    }
    m_disconnectPending = true;
    m_service->disconnectDevice(*device, mountedVolumes);
}

void MainWindow::mountSelected()
{
    const auto device = selectedDevice();
    const QString path = m_details->currentVolumePath();
    if (!device || path.isEmpty()) {
        return;
    }

    const NbdVolume *volume = nullptr;
    for (const NbdVolume &candidate : device->volumes) {
        if (candidate.path == path) {
            volume = &candidate;
            break;
        }
    }
    if (!volume || volume->isLockedLuks()) {
        return;
    }

    QString mountPoint;
    if (const auto profile = m_service->profileForDevice(device->path)) {
        mountPoint = profile->mountPoints.value(path);
    }
    m_service->mountVolume(volume->mountPath(), volume->mountFileSystem(),
                           device->readOnly, mountPoint);
}

void MainWindow::unlockSelectedVolume(const QString &path)
{
    const auto device = selectedDevice();
    if (m_busy || path.isEmpty() || !device) {
        return;
    }

    const auto profile = m_service->profileForDevice(device->path);
    const bool remembered =
        profile && profile->rememberedVolumes.contains(path);
    UnlockDialog dialog(path, remembered, profile.has_value(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    if (!dialog.remember()) {
        m_service->setProfileRememberPassphrase(device->path, path, false);
    }

    m_pendingUnlock = {device->path, path, dialog.passphrase(), dialog.remember()};
    m_hasPendingUnlock = true;
    m_service->unlockVolume(path, dialog.passphrase(), device->readOnly);
}

void MainWindow::editSelectedProfile()
{
    const auto device = selectedDevice();
    if (!device || m_busy) {
        return;
    }
    const auto profile = m_service->profileForDevice(device->path);
    if (!profile) {
        return;
    }

    ConnectDialog dialog(m_model->devices(), m_service->backends(), device->path, this,
                         *profile, true);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    ConnectOptions options = dialog.connectOptions();
    options.device = device->path;
    m_service->saveProfile(options, dialog.autoConnect());
    showMessage(KMessageWidget::Positive, i18n("Saved the profile for %1.", device->path));
    refresh();
}

void MainWindow::deleteSelectedProfile()
{
    const auto device = selectedDevice();
    if (!device || m_busy || !device->hasProfile) {
        return;
    }

    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, i18n("Delete Profile"),
        i18n("Delete the saved profile for %1?\n\nThe device stays connected, but "
             "it will no longer be reconnected automatically, and any remembered "
             "LUKS passphrases for it are removed from the wallet.",
             device->path),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    m_service->deleteProfile(device->path);
    showMessage(KMessageWidget::Positive, i18n("Deleted the profile for %1.", device->path));
    refresh();
}

void MainWindow::unmountSelected()
{
    const QString path = m_details->currentVolumePath();
    if (!path.isEmpty()) {
        m_service->unmountVolume(path);
    }
}

void MainWindow::openSelected()
{
    const QString mountPoint = m_details->currentMountPoint();
    if (!mountPoint.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(mountPoint));
    }
}

void MainWindow::loadNbdModule()
{
    if (!m_busy) {
        m_service->loadNbdModule();
    }
}

void MainWindow::setAutostartEnabled(bool enabled)
{
    const QString path = autostartFilePath();
    if (!enabled) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            QSignalBlocker blocker(m_autostartAction);
            m_autostartAction->setChecked(true);
            showMessage(KMessageWidget::Error,
                        i18n("Could not remove the autostart entry: %1", path));
        }
        return;
    }

    const QFileInfo fileInfo(path);
    if (!QDir().mkpath(fileInfo.absolutePath())) {
        QSignalBlocker blocker(m_autostartAction);
        m_autostartAction->setChecked(false);
        showMessage(KMessageWidget::Error,
                    i18n("Could not create the autostart directory: %1",
                         fileInfo.absolutePath()));
        return;
    }

    QString executable = BackgroundServiceClient::daemonExecutablePath();
    if (executable.isEmpty()) {
        QSignalBlocker blocker(m_autostartAction);
        m_autostartAction->setChecked(false);
        showMessage(KMessageWidget::Error,
                    i18n("The background service (nbdisk-daemon) is not installed."));
        return;
    }
    executable.replace(QLatin1Char('"'), QStringLiteral("\\\""));

    const QString entry = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=NBDisk Background Service\n"
        "Comment=Reconnect and mount saved network disks at login\n"
        "Exec=\"%1\"\n"
        "Icon=io.github.liturondev.NBDisk\n"
        "Terminal=false\n"
        "NoDisplay=true\n"
        "X-GNOME-Autostart-enabled=true\n")
                              .arg(executable);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)
        || file.write(entry.toUtf8()) < 0 || !file.commit()) {
        QSignalBlocker blocker(m_autostartAction);
        m_autostartAction->setChecked(false);
        showMessage(KMessageWidget::Error,
                    i18n("Could not write the autostart entry: %1", path));
    }
}

void MainWindow::showContextMenu(const QPoint &position)
{
    const QModelIndex index = m_view->indexAt(position);
    if (index.isValid()) {
        m_view->selectRow(index.row());
    }

    QMenu menu(this);
    menu.addAction(m_connectAction);
    menu.addAction(m_disconnectAction);
    menu.addSeparator();
    menu.addAction(m_editProfileAction);
    menu.addAction(m_deleteProfileAction);
    menu.addSeparator();
    menu.addAction(m_mountAction);
    menu.addAction(m_unmountAction);
    menu.addAction(m_openAction);
    menu.addSeparator();
    menu.addAction(m_loadModuleAction);
    menu.addAction(m_refreshAction);
    menu.exec(m_view->viewport()->mapToGlobal(position));
}

void MainWindow::handleOperationStarted(const QString &description)
{
    m_busy = true;
    m_details->setBusy(true);
    showMessage(KMessageWidget::Information, description);
    statusBar()->showMessage(description);
    updateActions();
}

void MainWindow::handleOperationFinished(bool success, const QString &message)
{
    bool walletStoreFailed = false;
    if (m_hasPendingUnlock) {
        if (success && m_pendingUnlock.remember) {
            walletStoreFailed = !m_service->storeVolumePassphrase(
                m_pendingUnlock.device, m_pendingUnlock.path,
                m_pendingUnlock.passphrase);
        }
        m_pendingUnlock.passphrase.fill(QChar(0));
        m_pendingUnlock = {};
        m_hasPendingUnlock = false;
    }

    const bool wasDisconnect = m_disconnectPending;
    m_disconnectPending = false;
    m_busy = false;
    m_details->setBusy(false);
    showMessage(success ? KMessageWidget::Positive : KMessageWidget::Error, message);
    if (walletStoreFailed) {
        showMessage(KMessageWidget::Warning,
                    i18n("The volume was unlocked, but the passphrase could not be "
                         "saved in the KDE Wallet. Check that a wallet is available, "
                         "then unlock again."));
    }
    if (!success && !m_service->nbdModuleLoaded()) {
        m_message->addAction(m_loadModuleAction);
    }
    statusBar()->showMessage(success ? i18n("Operation completed") : i18n("Operation failed"),
                             5000);
    const bool continueAutoConnect = m_autoConnectActive;
    const bool continueAutoMount = m_autoMountActive;
    refresh();
    if (success && wasDisconnect) {
        // Give sysfs a moment to settle after the disconnect command.
        QTimer::singleShot(500, this, &MainWindow::refresh);
    }
    if (continueAutoConnect) {
        QTimer::singleShot(0, this, &MainWindow::connectNextAutoProfile);
    } else if (continueAutoMount) {
        if (m_autoUnlockActive || !m_autoUnlockQueue.isEmpty()) {
            QTimer::singleShot(0, this, &MainWindow::unlockNextAutoVolume);
        } else {
            QTimer::singleShot(0, this, &MainWindow::mountNextAutoVolume);
        }
    }
}

void MainWindow::showBackgroundService()
{
    BackgroundServiceDialog dialog(this);
    dialog.exec();
}

void MainWindow::showAbout()
{
    KAboutApplicationDialog dialog(KAboutData::applicationData(), this);
    dialog.exec();
}

void MainWindow::updateActions()
{
    const auto device = selectedDevice();
    const bool backendAvailable = m_service->backends().any();
    const bool moduleLoaded = m_service->nbdModuleLoaded();

    m_loadModuleAction->setVisible(!moduleLoaded);
    m_loadModuleAction->setEnabled(!m_busy && !moduleLoaded);

    bool hasDisconnectedDevice = false;
    for (const NbdDevice &candidate : m_model->devices()) {
        if (!candidate.connected) {
            hasDisconnectedDevice = true;
            break;
        }
    }

    m_connectAction->setEnabled(!m_busy && backendAvailable && hasDisconnectedDevice);
    m_disconnectAction->setEnabled(!m_busy && backendAvailable && device && device->connected);
    m_editProfileAction->setEnabled(!m_busy && device && device->hasProfile);
    m_deleteProfileAction->setEnabled(!m_busy && device && device->hasProfile);
    m_refreshAction->setEnabled(!m_busy);

    const QString volumePath = m_details->currentVolumePath();
    bool selectedVolumeMounted = false;
    bool selectedVolumeLocked = false;
    if (device && !volumePath.isEmpty()) {
        for (const NbdVolume &volume : device->volumes) {
            if (volume.path == volumePath) {
                selectedVolumeMounted = volume.isMounted();
                selectedVolumeLocked = volume.isLockedLuks();
                break;
            }
        }
    }

    m_mountAction->setEnabled(!m_busy && !volumePath.isEmpty() && !selectedVolumeMounted
                              && !selectedVolumeLocked);
    m_unmountAction->setEnabled(!m_busy && selectedVolumeMounted);
    m_openAction->setEnabled(!m_busy && !m_details->currentMountPoint().isEmpty());

    if (!backendAvailable) {
        m_connectAction->setToolTip(
            i18n("Install nbd-client or qemu-nbd to connect network disks."));
    } else {
        m_connectAction->setToolTip(QString());
    }
}

void MainWindow::updateStatusBar()
{
    int connected = 0;
    for (const NbdDevice &device : m_model->devices()) {
        if (device.connected) {
            ++connected;
        }
    }

    if (m_model->devices().isEmpty()) {
        m_countLabel->setText(i18n("No NBD devices"));
    } else {
        m_countLabel->setText(
            i18n("%1 of %2 NBD devices connected", connected, m_model->devices().size()));
    }

    const NbdBackend backend = currentBackend();
    if (backend == NbdBackend::Auto) {
        m_backendLabel->setText(i18n("No NBD backend"));
    } else {
        m_backendLabel->setText(
            i18n("Backend: %1", NbdCommandBuilder::backendName(backend)));
    }
}

void MainWindow::updateDetails()
{
    if (m_refreshing && m_details->isEditingMountPoint()) {
        return;
    }

    const auto device = selectedDevice();
    if (device) {
        m_details->setDevice(*device);
        const auto profile = m_service->profileForDevice(device->path);
        m_details->setProfile(profile.value_or(NbdProfile()), profile.has_value());
    } else {
        m_details->clear();
    }
}

void MainWindow::maybeAutoConnectProfiles()
{
    if (m_autoConnectStarted || m_busy || !m_service->nbdModuleLoaded()) {
        return;
    }

    const QList<NbdProfile> profiles = m_service->profiles();
    if (profiles.isEmpty()) {
        m_autoConnectStarted = true;
        return;
    }

    for (const NbdProfile &profile : profiles) {
        if (!profile.autoConnect || !profile.isValid()) {
            continue;
        }
        const NbdDevice *device = m_model->deviceAt(m_model->rowForPath(profile.options.device));
        if (device && !device->connected) {
            m_autoConnectQueue.enqueue(profile.options);
        }
    }

    m_autoConnectStarted = true;
    if (m_autoConnectQueue.isEmpty()) {
        return;
    }

    m_autoConnectActive = true;
    showMessage(KMessageWidget::Information,
                i18n("Auto-connecting %1 saved profile(s)…", m_autoConnectQueue.size()));
    QTimer::singleShot(0, this, &MainWindow::connectNextAutoProfile);
}

void MainWindow::connectNextAutoProfile()
{
    if (!m_autoConnectActive) {
        return;
    }
    if (m_autoConnectQueue.isEmpty()) {
        m_autoConnectActive = false;
        updateActions();
        maybeAutoMountVolumes();
        return;
    }
    if (m_busy) {
        return;
    }
    m_service->connectDevice(m_autoConnectQueue.dequeue());
}

void MainWindow::maybeAutoMountVolumes()
{
    if (m_autoMountActive || m_autoConnectActive || m_busy
        || !m_service->nbdModuleLoaded()) {
        return;
    }

    for (const NbdDevice &device : m_model->devices()) {
        if (!device.connected || !device.hasProfile) {
            continue;
        }
        const auto profile = m_service->profileForDevice(device.path);
        if (!profile) {
            continue;
        }

        for (const NbdVolume &volume : device.volumes) {
            if (volume.isMounted() || m_autoMountAttempted.contains(volume.path)) {
                continue;
            }

            // Unlock remembered LUKS volumes before the mount pass, even when
            // automount is off: unlocking is part of auto-connect.
            if (volume.isLockedLuks()) {
                if (!profile->rememberedVolumes.contains(volume.path)) {
                    continue;
                }
                const QString passphrase =
                    m_service->volumePassphrase(device.path, volume.path);
                if (passphrase.isEmpty()) {
                    continue;
                }
                m_autoMountAttempted.insert(volume.path);
                m_autoUnlockQueue.enqueue({volume.path, passphrase, device.readOnly});
                continue;
            }

            if (!profile->autoMount) {
                continue;
            }
            const auto mountPoint = profile->mountPoints.constFind(volume.path);
            if (mountPoint == profile->mountPoints.cend() || mountPoint.value().isEmpty()) {
                continue;
            }

            m_autoMountAttempted.insert(volume.path);
            m_autoMountQueue.enqueue({volume.mountPath(), volume.mountFileSystem(),
                                      device.readOnly, mountPoint.value()});
        }
    }

    if (!m_autoUnlockQueue.isEmpty()) {
        m_autoMountActive = true;
        m_autoUnlockActive = true;
        showMessage(KMessageWidget::Information,
                    i18n("Unlocking %1 remembered volume(s)…",
                         m_autoUnlockQueue.size()));
        QTimer::singleShot(0, this, &MainWindow::unlockNextAutoVolume);
        return;
    }

    if (m_autoMountQueue.isEmpty()) {
        return;
    }

    m_autoMountActive = true;
    showMessage(KMessageWidget::Information,
                i18n("Automounting %1 volume(s)…", m_autoMountQueue.size()));
    QTimer::singleShot(0, this, &MainWindow::mountNextAutoVolume);
}

void MainWindow::unlockNextAutoVolume()
{
    if (m_autoUnlockQueue.isEmpty()) {
        // Rebuild the mount plan now that the unlock operations have finished.
        m_autoUnlockActive = false;
        m_autoMountActive = false;
        maybeAutoMountVolumes();
        return;
    }
    if (m_busy) {
        return;
    }

    UnlockRequest request = m_autoUnlockQueue.dequeue();
    m_service->unlockVolume(request.path, request.passphrase, request.readOnly);
    request.passphrase.fill(QChar(0));
}

void MainWindow::mountNextAutoVolume()
{
    if (!m_autoMountActive) {
        return;
    }
    if (m_autoMountQueue.isEmpty()) {
        m_autoMountActive = false;
        updateActions();
        return;
    }
    if (m_busy) {
        return;
    }

    const MountRequest request = m_autoMountQueue.dequeue();
    m_service->mountVolume(request.path, request.fileSystem, request.readOnly,
                           request.mountPoint);
}

void MainWindow::showMessage(KMessageWidget::MessageType type, const QString &text)
{
    m_message->clearActions();
    m_message->setMessageType(type);
    m_message->setText(text);
    m_message->setCloseButtonVisible(true);
    m_message->animatedShow();
}

void MainWindow::showEnvironmentMessage(const QString &text)
{
    showMessage(KMessageWidget::Warning, text);
    if (!m_service->nbdModuleLoaded()) {
        m_message->addAction(m_loadModuleAction);
    }
}

std::optional<NbdDevice> MainWindow::selectedDevice() const
{
    int row = m_view->currentIndex().row();
    if (row < 0 && m_view->selectionModel()) {
        const QModelIndexList rows = m_view->selectionModel()->selectedRows();
        if (!rows.isEmpty()) {
            row = rows.first().row();
        }
    }
    if (const NbdDevice *device = m_model->deviceAt(row)) {
        return *device;
    }
    return std::nullopt;
}

QString MainWindow::selectedDevicePath() const
{
    const auto device = selectedDevice();
    return device ? device->path : QString();
}

void MainWindow::selectDevicePath(const QString &path)
{
    const int row = m_model->rowForPath(path);
    if (row >= 0) {
        m_view->selectRow(row);
        m_view->setCurrentIndex(m_model->index(row, NbdDeviceModel::DeviceColumn));
    }
}

QString MainWindow::autostartFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/autostart/io.github.liturondev.NBDisk.desktop");
}

NbdBackend MainWindow::currentBackend() const
{
    return m_service->effectiveBackend(NbdBackend::Auto);
}
