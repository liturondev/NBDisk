/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nbdcommand.h"
#include "nbdservice.h"

#include <QWizard>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class KMessageWidget;

class ConnectDialog : public QWizard
{
    Q_OBJECT

public:
    ConnectDialog(const QList<NbdDevice> &devices, const BackendAvailability &backends,
                  const QString &selectedDevice, QWidget *parent = nullptr,
                  const NbdProfile &profile = NbdProfile(),
                  bool editingProfile = false);

    ConnectOptions connectOptions() const;
    bool autoConnect() const;

protected:
    bool validateCurrentPage() override;
    void accept() override;

private:
    class Page;

    enum PageId {
        ServerPage,
        DevicePage,
        ConnectionPage,
    };

    void createServerPage();
    void createDevicePage(const QList<NbdDevice> &devices, const QString &selectedDevice);
    void createConnectionPage();
    void loadSettings();
    void applyProfile(const NbdProfile &profile);
    void saveSettings() const;
    void updateBackendState();
    void updateServerStep();
    QString serverErrorText() const;
    void browseCertificate(QLineEdit *target, const QString &title);
    NbdBackend selectedBackend() const;
    NbdBackend effectiveBackend() const;

    BackendAvailability m_backends;
    bool m_editingProfile = false;
    Page *m_serverPage = nullptr;
    Page *m_devicePage = nullptr;
    Page *m_connectionPage = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_exportEdit = nullptr;
    QComboBox *m_deviceCombo = nullptr;
    QComboBox *m_backendCombo = nullptr;
    QCheckBox *m_readOnlyCheck = nullptr;
    QCheckBox *m_autoConnectCheck = nullptr;
    QComboBox *m_blockSizeCombo = nullptr;
    QSpinBox *m_timeoutSpin = nullptr;
    QCheckBox *m_persistentCheck = nullptr;
    QCheckBox *m_tlsCheck = nullptr;
    QWidget *m_tlsGroup = nullptr;
    QLineEdit *m_certEdit = nullptr;
    QLineEdit *m_keyEdit = nullptr;
    QLineEdit *m_caEdit = nullptr;
    QLineEdit *m_tlsHostnameEdit = nullptr;
    KMessageWidget *m_serverError = nullptr;
    KMessageWidget *m_backendNote = nullptr;
    KMessageWidget *m_connectionError = nullptr;
};
