/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "backgroundserviceclient.h"

#include <QDialog>
#include <QVariantMap>

class QLabel;
class QPushButton;
class QTimer;

class BackgroundServiceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BackgroundServiceDialog(QWidget *parent = nullptr);

private Q_SLOTS:
    void refreshStatus();
    void runStartup();
    void pollService();
    void handleStarted();
    void handleFinished(bool success, int connected, int mounted, const QString &message);

private:
    void updateUi();

    BackgroundServiceClient m_client;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_runButton = nullptr;
    QTimer *m_pollTimer = nullptr;
    QVariantMap m_status;
    bool m_available = false;
    bool m_running = false;
    bool m_starting = false;
    int m_pollAttempts = 0;
};
