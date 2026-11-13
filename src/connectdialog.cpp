/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "connectdialog.h"

#include <KLocalizedString>
#include <KMessageWidget>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWizardPage>

#include <functional>
#include <utility>

namespace
{
QString formatSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("—");
    }
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QWidget *fileRow(QLineEdit *edit, QPushButton *button, QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(edit, 1);
    layout->addWidget(button);
    return widget;
}

KMessageWidget *createMessageWidget(QWidget *parent)
{
    auto *widget = new KMessageWidget(parent);
    widget->setCloseButtonVisible(false);
    widget->setWordWrap(true);
    widget->hide();
    return widget;
}
}

// A wizard page whose Next/Finish button state is driven by the dialog.
class ConnectDialog::Page : public QWizardPage
{
public:
    explicit Page(QWidget *parent = nullptr)
        : QWizardPage(parent)
    {
    }

    void setCompletionCheck(std::function<bool()> check)
    {
        m_check = std::move(check);
    }

    bool isComplete() const override
    {
        return !m_check || m_check();
    }

    void recheck()
    {
        emit completeChanged();
    }

private:
    std::function<bool()> m_check;
};

ConnectDialog::ConnectDialog(const QList<NbdDevice> &devices,
                             const BackendAvailability &backends,
                             const QString &selectedDevice,
                             QWidget *parent,
                             const NbdProfile &profile,
                             bool editingProfile)
    : QWizard(parent)
    , m_backends(backends)
    , m_editingProfile(editingProfile)
{
    setWindowTitle(m_editingProfile ? i18n("Edit Network Disk Profile")
                                    : i18n("Connect Network Disk"));
    setModal(true);
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnStartPage);
    setButtonText(QWizard::NextButton, i18n("Continue"));
    setButtonText(QWizard::FinishButton,
                  m_editingProfile ? i18n("Save") : i18n("Connect"));
    setMinimumWidth(560);

    createServerPage();
    createDevicePage(devices, selectedDevice);
    createConnectionPage();

    // Do not carry a validation message over to another step.
    connect(this, &QWizard::currentIdChanged, this, [this]() {
        if (currentId() == ServerPage) {
            updateServerStep();
        } else {
            m_serverError->animatedHide();
        }
        m_connectionError->animatedHide();
    });

    loadSettings();
    applyProfile(profile);

    updateBackendState();
    updateServerStep();
}

void ConnectDialog::createServerPage()
{
    m_serverPage = new Page(this);
    m_serverPage->setTitle(i18n("Server"));
    m_serverPage->setSubTitle(i18n("Which remote NBD export should be connected?"));
    m_serverPage->setCompletionCheck([this]() { return serverErrorText().isEmpty(); });

    auto *layout = new QVBoxLayout(m_serverPage);
    layout->setSpacing(12);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_hostEdit = new QLineEdit(m_serverPage);
    m_hostEdit->setPlaceholderText(i18n("hostname or IP address"));
    m_portSpin = new QSpinBox(m_serverPage);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(10809);
    m_exportEdit = new QLineEdit(m_serverPage);
    m_exportEdit->setPlaceholderText(i18n("Default export"));
    m_exportEdit->setToolTip(
        i18n("Leave empty to ask the server for its default export."));

    form->addRow(i18n("Server:"), m_hostEdit);
    form->addRow(i18n("Port:"), m_portSpin);
    form->addRow(i18n("Export name:"), m_exportEdit);
    layout->addLayout(form);

    m_serverError = createMessageWidget(m_serverPage);
    m_serverError->setMessageType(KMessageWidget::Error);
    layout->addWidget(m_serverError);
    layout->addStretch(1);

    setPage(ServerPage, m_serverPage);

    connect(m_hostEdit, &QLineEdit::textChanged, this, &ConnectDialog::updateServerStep);
}

void ConnectDialog::createDevicePage(const QList<NbdDevice> &devices,
                                     const QString &selectedDevice)
{
    m_devicePage = new Page(this);
    m_devicePage->setTitle(i18n("Local Device"));
    m_devicePage->setSubTitle(
        m_editingProfile
            ? i18n("The saved profile stays bound to this local NBD device.")
            : i18n("Choose the local NBD device that the export should be attached to."));
    m_devicePage->setCompletionCheck([this]() { return m_deviceCombo->count() > 0; });

    auto *layout = new QVBoxLayout(m_devicePage);
    layout->setSpacing(12);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_deviceCombo = new QComboBox(m_devicePage);
    for (const NbdDevice &device : devices) {
        // While editing, the profile stays bound to its current device, even
        // when that device is connected; the profile is saved without
        // reconnecting.
        if (m_editingProfile && device.path != selectedDevice) {
            continue;
        }
        if (device.connected && !m_editingProfile) {
            continue;
        }
        m_deviceCombo->addItem(i18n("%1 — %2", device.path, formatSize(device.size)),
                               device.path);
    }
    m_deviceCombo->setEnabled(!m_editingProfile);
    if (!selectedDevice.isEmpty()) {
        const int index = m_deviceCombo->findData(selectedDevice);
        if (index >= 0) {
            m_deviceCombo->setCurrentIndex(index);
        }
    }
    form->addRow(i18n("Device:"), m_deviceCombo);
    layout->addLayout(form);

    if (m_deviceCombo->count() == 0) {
        auto *deviceNote = createMessageWidget(m_devicePage);
        deviceNote->setMessageType(KMessageWidget::Warning);
        deviceNote->setText(i18n("No disconnected NBD device is available. Disconnect one "
                                "first or load the nbd module."));
        deviceNote->show();
        layout->addWidget(deviceNote);
    }
    layout->addStretch(1);

    setPage(DevicePage, m_devicePage);

    connect(m_deviceCombo, &QComboBox::currentIndexChanged, this,
            [this]() { m_devicePage->recheck(); });
}

void ConnectDialog::createConnectionPage()
{
    m_connectionPage = new Page(this);
    m_connectionPage->setTitle(i18n("Connection"));
    m_connectionPage->setSubTitle(
        i18n("Choose how NBDisk talks to the server, plus any optional tuning."));
    m_connectionPage->setCompletionCheck(
        [this]() { return effectiveBackend() != NbdBackend::Auto; });

    auto *layout = new QVBoxLayout(m_connectionPage);
    layout->setSpacing(12);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_backendCombo = new QComboBox(m_connectionPage);

    const auto addBackend = [this](NbdBackend backend) {
        if (backend == NbdBackend::Auto) {
            const NbdBackend automatic = m_backends.nbdClient
                ? NbdBackend::NbdClient
                : (m_backends.qemuNbd ? NbdBackend::QemuNbd : NbdBackend::Auto);
            m_backendCombo->addItem(
                i18n("Automatic (%1)", NbdCommandBuilder::backendName(automatic)),
                static_cast<int>(NbdBackend::Auto));
        } else {
            m_backendCombo->addItem(NbdCommandBuilder::backendName(backend),
                                    static_cast<int>(backend));
        }
    };
    addBackend(NbdBackend::Auto);
    if (m_backends.nbdClient) {
        addBackend(NbdBackend::NbdClient);
    }
    if (m_backends.qemuNbd) {
        addBackend(NbdBackend::QemuNbd);
    }

    m_readOnlyCheck = new QCheckBox(i18n("Connect as read-only"), m_connectionPage);
    m_autoConnectCheck = new QCheckBox(
        i18n("Auto-connect this device when NBDisk starts"), m_connectionPage);
    m_autoConnectCheck->setToolTip(
        i18n("The connection details are saved as a profile for this NBD device."));

    form->addRow(i18n("Backend:"), m_backendCombo);
    form->addRow(QString(), m_readOnlyCheck);
    form->addRow(QString(), m_autoConnectCheck);
    layout->addLayout(form);

    m_backendNote = createMessageWidget(m_connectionPage);
    m_backendNote->setMessageType(KMessageWidget::Information);
    layout->addWidget(m_backendNote);

    auto *advancedGroup = new QGroupBox(i18n("Advanced (nbd-client)"), m_connectionPage);
    auto *advancedForm = new QFormLayout(advancedGroup);
    advancedForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_blockSizeCombo = new QComboBox(advancedGroup);
    const QList<int> blockSizes = {512, 1024, 2048, 4096};
    for (int size : blockSizes) {
        m_blockSizeCombo->addItem(i18n("%1 bytes", size), size);
    }
    m_timeoutSpin = new QSpinBox(advancedGroup);
    m_timeoutSpin->setRange(0, 3600);
    m_timeoutSpin->setSpecialValueText(i18n("No timeout"));
    m_timeoutSpin->setSuffix(i18n(" s"));
    m_persistentCheck =
        new QCheckBox(i18n("Reconnect automatically after a network interruption"),
                      advancedGroup);
    m_tlsCheck = new QCheckBox(i18n("Use TLS"), advancedGroup);

    advancedForm->addRow(i18n("Block size:"), m_blockSizeCombo);
    advancedForm->addRow(i18n("Request timeout:"), m_timeoutSpin);
    advancedForm->addRow(QString(), m_persistentCheck);
    advancedForm->addRow(QString(), m_tlsCheck);
    layout->addWidget(advancedGroup);

    m_tlsGroup = new QGroupBox(i18n("TLS certificates"), m_connectionPage);
    auto *tlsForm = new QFormLayout(m_tlsGroup);
    tlsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_certEdit = new QLineEdit(m_tlsGroup);
    m_certEdit->setPlaceholderText(i18n("Optional client certificate"));
    auto *certButton = new QPushButton(i18n("Browse…"), m_tlsGroup);
    m_keyEdit = new QLineEdit(m_tlsGroup);
    m_keyEdit->setPlaceholderText(i18n("Client private key"));
    auto *keyButton = new QPushButton(i18n("Browse…"), m_tlsGroup);
    m_caEdit = new QLineEdit(m_tlsGroup);
    m_caEdit->setPlaceholderText(i18n("Optional CA certificate"));
    auto *caButton = new QPushButton(i18n("Browse…"), m_tlsGroup);
    m_tlsHostnameEdit = new QLineEdit(m_tlsGroup);
    m_tlsHostnameEdit->setPlaceholderText(i18n("Defaults to the server host name"));

    tlsForm->addRow(i18n("Client certificate:"),
                    fileRow(m_certEdit, certButton, m_tlsGroup));
    tlsForm->addRow(i18n("Private key:"), fileRow(m_keyEdit, keyButton, m_tlsGroup));
    tlsForm->addRow(i18n("CA certificate:"), fileRow(m_caEdit, caButton, m_tlsGroup));
    tlsForm->addRow(i18n("TLS host name:"), m_tlsHostnameEdit);
    m_tlsGroup->hide();
    layout->addWidget(m_tlsGroup);

    m_connectionError = createMessageWidget(m_connectionPage);
    m_connectionError->setMessageType(KMessageWidget::Error);
    layout->addWidget(m_connectionError);
    layout->addStretch(1);

    setPage(ConnectionPage, m_connectionPage);

    connect(m_backendCombo, &QComboBox::currentIndexChanged, this,
            &ConnectDialog::updateBackendState);
    connect(m_tlsCheck, &QCheckBox::toggled, this, &ConnectDialog::updateBackendState);
    connect(certButton, &QPushButton::clicked, this,
            [this, cert = m_certEdit]() { browseCertificate(cert, i18n("Client Certificate")); });
    connect(keyButton, &QPushButton::clicked, this,
            [this, key = m_keyEdit]() { browseCertificate(key, i18n("Private Key")); });
    connect(caButton, &QPushButton::clicked, this,
            [this, ca = m_caEdit]() { browseCertificate(ca, i18n("CA Certificate")); });
}

void ConnectDialog::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("ConnectDialog"));
    m_hostEdit->setText(settings.value(QStringLiteral("host")).toString());
    m_portSpin->setValue(settings.value(QStringLiteral("port"), 10809).toInt());
    m_exportEdit->setText(settings.value(QStringLiteral("export")).toString());
    m_readOnlyCheck->setChecked(settings.value(QStringLiteral("readOnly"), false).toBool());
    const int backendValue = settings.value(QStringLiteral("backend"),
                                            static_cast<int>(NbdBackend::Auto))
                                 .toInt();
    const int backendIndex = m_backendCombo->findData(backendValue);
    if (backendIndex >= 0) {
        m_backendCombo->setCurrentIndex(backendIndex);
    }
    const int blockSize = settings.value(QStringLiteral("blockSize"), 512).toInt();
    const int blockIndex = m_blockSizeCombo->findData(blockSize);
    if (blockIndex >= 0) {
        m_blockSizeCombo->setCurrentIndex(blockIndex);
    }
    m_timeoutSpin->setValue(settings.value(QStringLiteral("timeout"), 0).toInt());
    m_persistentCheck->setChecked(
        settings.value(QStringLiteral("persistent"), false).toBool());
    settings.endGroup();
}

void ConnectDialog::applyProfile(const NbdProfile &profile)
{
    if (!profile.isValid()) {
        return;
    }

    m_hostEdit->setText(profile.options.host);
    m_portSpin->setValue(profile.options.port);
    m_exportEdit->setText(profile.options.exportName);
    m_readOnlyCheck->setChecked(profile.options.readOnly);
    m_autoConnectCheck->setChecked(profile.autoConnect);

    const int savedBackendIndex =
        m_backendCombo->findData(static_cast<int>(profile.options.backend));
    if (savedBackendIndex >= 0) {
        m_backendCombo->setCurrentIndex(savedBackendIndex);
    }
    const int savedBlockIndex = m_blockSizeCombo->findData(profile.options.blockSize);
    if (savedBlockIndex >= 0) {
        m_blockSizeCombo->setCurrentIndex(savedBlockIndex);
    }
    m_timeoutSpin->setValue(profile.options.timeoutSeconds);
    m_persistentCheck->setChecked(profile.options.persistent);
    m_tlsCheck->setChecked(profile.options.tls);
    m_certEdit->setText(profile.options.certFile);
    m_keyEdit->setText(profile.options.keyFile);
    m_caEdit->setText(profile.options.caFile);
    m_tlsHostnameEdit->setText(profile.options.tlsHostname);
}

void ConnectDialog::saveSettings() const
{
    const ConnectOptions current = connectOptions();

    QSettings settings;
    settings.beginGroup(QStringLiteral("ConnectDialog"));
    settings.setValue(QStringLiteral("host"), current.host);
    settings.setValue(QStringLiteral("port"), current.port);
    settings.setValue(QStringLiteral("export"), current.exportName);
    settings.setValue(QStringLiteral("backend"), static_cast<int>(current.backend));
    settings.setValue(QStringLiteral("readOnly"), current.readOnly);
    // Keep the nbd-client tuning even when another backend was used last, so
    // switching back to nbd-client does not silently reset it.
    settings.setValue(QStringLiteral("blockSize"), m_blockSizeCombo->currentData().toInt());
    settings.setValue(QStringLiteral("timeout"), m_timeoutSpin->value());
    settings.setValue(QStringLiteral("persistent"), m_persistentCheck->isChecked());
    settings.endGroup();
}

bool ConnectDialog::autoConnect() const
{
    return m_autoConnectCheck->isChecked();
}

ConnectOptions ConnectDialog::connectOptions() const
{
    ConnectOptions options;
    options.device = m_deviceCombo->currentData().toString();
    options.host = m_hostEdit->text().trimmed();
    options.port = static_cast<quint16>(m_portSpin->value());
    options.exportName = m_exportEdit->text().trimmed();
    options.readOnly = m_readOnlyCheck->isChecked();
    options.backend = selectedBackend();

    if (effectiveBackend() == NbdBackend::NbdClient) {
        options.blockSize = m_blockSizeCombo->currentData().toInt();
        options.timeoutSeconds = m_timeoutSpin->value();
        options.persistent = m_persistentCheck->isChecked();
        options.tls = m_tlsCheck->isChecked();
        if (options.tls) {
            options.certFile = m_certEdit->text().trimmed();
            options.keyFile = m_keyEdit->text().trimmed();
            options.caFile = m_caEdit->text().trimmed();
            options.tlsHostname = m_tlsHostnameEdit->text().trimmed();
        }
    }

    return options;
}

bool ConnectDialog::validateCurrentPage()
{
    switch (currentId()) {
    case ServerPage: {
        const QString error = serverErrorText();
        if (!error.isEmpty()) {
            m_serverError->setText(error);
            if (!m_serverError->isVisible()) {
                m_serverError->animatedShow();
            }
            m_hostEdit->setFocus();
            return false;
        }
        m_serverError->animatedHide();
        return true;
    }
    case DevicePage:
        return m_deviceCombo->count() > 0;
    case ConnectionPage:
        break;
    }

    QString error;
    if (!NbdCommandBuilder::validate(connectOptions(), effectiveBackend(), &error)) {
        m_connectionError->setText(error);
        if (!m_connectionError->isVisible()) {
            m_connectionError->animatedShow();
        }
        return false;
    }
    m_connectionError->animatedHide();
    return true;
}

void ConnectDialog::accept()
{
    if (!validateCurrentPage()) {
        return;
    }
    saveSettings();
    QWizard::accept();
}

void ConnectDialog::updateBackendState()
{
    const NbdBackend backend = effectiveBackend();
    const bool advanced = NbdCommandBuilder::supportsAdvancedOptions(backend);

    if (!advanced && m_tlsCheck->isChecked()) {
        m_tlsCheck->setChecked(false);
    }
    m_tlsGroup->setVisible(m_tlsCheck->isChecked());
    m_tlsCheck->setEnabled(advanced);
    m_persistentCheck->setEnabled(advanced);
    m_timeoutSpin->setEnabled(advanced);
    m_blockSizeCombo->setEnabled(advanced);

    if (backend == NbdBackend::Auto) {
        m_backendNote->setMessageType(KMessageWidget::Error);
        m_backendNote->setText(
            i18n("Install nbd-client or qemu-nbd to connect network disks."));
        m_backendNote->show();
    } else if (!advanced) {
        m_backendNote->setMessageType(KMessageWidget::Information);
        m_backendNote->setText(
            i18n("qemu-nbd uses its built-in NBD client. Block size, timeout, persistent "
                 "reconnect, and TLS are available with nbd-client only."));
        m_backendNote->show();
    } else {
        m_backendNote->hide();
    }

    if (isVisible()) {
        adjustSize();
    }
    m_connectionPage->recheck();
}

void ConnectDialog::updateServerStep()
{
    const QString error =
        m_hostEdit->text().trimmed().isEmpty() ? QString() : serverErrorText();
    if (error.isEmpty()) {
        m_serverError->animatedHide();
    } else {
        m_serverError->setText(error);
        if (!m_serverError->isVisible()) {
            m_serverError->animatedShow();
        }
    }
    m_serverPage->recheck();
}

QString ConnectDialog::serverErrorText() const
{
    const QString host = m_hostEdit->text().trimmed();
    if (host.isEmpty()) {
        return i18n("Enter the server host name or address.");
    }
    if (host.startsWith(QLatin1Char('-'))
        || host.contains(QRegularExpression(QStringLiteral("\\s")))) {
        return i18n("The server host name contains invalid characters.");
    }
    return QString();
}

void ConnectDialog::browseCertificate(QLineEdit *target, const QString &title)
{
    const QString file = QFileDialog::getOpenFileName(
        this, title, target->text(),
        i18n("PEM files (*.pem *.crt *.cer *.key);;All files (*)"));
    if (!file.isEmpty()) {
        target->setText(file);
    }
}

NbdBackend ConnectDialog::selectedBackend() const
{
    return static_cast<NbdBackend>(m_backendCombo->currentData().toInt());
}

NbdBackend ConnectDialog::effectiveBackend() const
{
    switch (selectedBackend()) {
    case NbdBackend::NbdClient:
        return m_backends.nbdClient ? NbdBackend::NbdClient : NbdBackend::Auto;
    case NbdBackend::QemuNbd:
        return m_backends.qemuNbd ? NbdBackend::QemuNbd : NbdBackend::Auto;
    case NbdBackend::Auto:
        break;
    }
    if (m_backends.nbdClient) {
        return NbdBackend::NbdClient;
    }
    if (m_backends.qemuNbd) {
        return NbdBackend::QemuNbd;
    }
    return NbdBackend::Auto;
}
