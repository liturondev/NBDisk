/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "backgroundservicedialog.h"

#include "servicedbus.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QDBusConnection>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

BackgroundServiceDialog::BackgroundServiceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18n("Background Service"));
    setModal(true);
    setMinimumWidth(460);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_statusLabel);

    auto *buttonLayout = new QHBoxLayout;
    m_runButton = new QPushButton(i18n("Run Startup Now"), this);
    m_runButton->setDefault(true);
    buttonLayout->addWidget(m_runButton);
    buttonLayout->addStretch(1);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttonLayout->addWidget(buttonBox);
    layout->addLayout(buttonLayout);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(300);
    connect(m_pollTimer, &QTimer::timeout, this, &BackgroundServiceDialog::pollService);

    connect(m_runButton, &QPushButton::clicked, this,
            &BackgroundServiceDialog::runStartup);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::accept);

    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(ServiceBus::serviceName(), ServiceBus::objectPath(),
                ServiceBus::interfaceName(), QStringLiteral("Started"), this,
                SLOT(handleStarted()));
    bus.connect(ServiceBus::serviceName(), ServiceBus::objectPath(),
                ServiceBus::interfaceName(), QStringLiteral("Finished"), this,
                SLOT(handleFinished(bool, int, int, QString)));

    refreshStatus();
}

void BackgroundServiceDialog::refreshStatus()
{
    m_available = m_client.isAvailable();
    m_status = m_available ? m_client.status() : QVariantMap();
    m_running = m_available && m_status.value(QStringLiteral("running")).toBool();
    if (m_available) {
        m_starting = false;
    }
    updateUi();
}

void BackgroundServiceDialog::updateUi()
{
    if (!m_available) {
        m_statusLabel->setText(
            i18n("The background service is not running.\n\n"
                 "It reconnects saved disks and mounts their configured volumes at "
                 "login. Start it now, or enable Settings > Start at Login."));
        m_runButton->setText(i18n("Start Service"));
        m_runButton->setEnabled(
            !m_starting && !BackgroundServiceClient::daemonExecutablePath().isEmpty());
        return;
    }

    if (m_running) {
        m_statusLabel->setText(
            i18n("The background service is reconnecting saved disks…"));
        m_runButton->setText(i18n("Running…"));
        m_runButton->setEnabled(false);
        return;
    }

    QString text = i18n("The background service is running.");
    if (!m_status.value(QStringLiteral("hasRun")).toBool()) {
        text += QLatin1Char('\n') + i18n("It has not run yet.");
    } else {
        const int connected = m_status.value(QStringLiteral("lastConnected")).toInt();
        const int mounted = m_status.value(QStringLiteral("lastMounted")).toInt();
        text += QLatin1Char('\n')
            + i18n("Last run: %1 connected, %2 mounted.", connected, mounted);

        const QDateTime when = QDateTime::fromString(
            m_status.value(QStringLiteral("lastRun")).toString(), Qt::ISODate);
        if (when.isValid()) {
            text += QLatin1Char(' ')
                + i18n("(%1)", QLocale().toString(when, QLocale::ShortFormat));
        }

        if (!m_status.value(QStringLiteral("lastSuccess")).toBool()) {
            const QString message =
                m_status.value(QStringLiteral("lastMessage")).toString();
            text += QLatin1Char('\n')
                + (message.isEmpty() ? i18n("Some steps failed; see the service log.")
                                     : i18n("Some steps failed: %1", message));
        }
    }

    m_statusLabel->setText(text);
    m_runButton->setText(i18n("Run Startup Now"));
    m_runButton->setEnabled(true);
}

void BackgroundServiceDialog::runStartup()
{
    if (!m_available) {
        if (!m_client.startService()) {
            m_statusLabel->setText(
                i18n("Could not start nbdisk-daemon. Install it or enable "
                     "Settings > Start at Login."));
            return;
        }
        m_starting = true;
        m_pollAttempts = 0;
        m_statusLabel->setText(i18n("Starting the background service…"));
        m_runButton->setEnabled(false);
        m_pollTimer->start();
        return;
    }

    if (m_client.runStartup()) {
        m_running = true;
        updateUi();
        return;
    }

    // The service is already busy; just show its current state.
    refreshStatus();
}

void BackgroundServiceDialog::pollService()
{
    ++m_pollAttempts;
    if (m_client.isAvailable()) {
        m_pollTimer->stop();
        m_starting = false;
        refreshStatus();
        return;
    }

    if (m_pollAttempts >= 20) {
        m_pollTimer->stop();
        m_starting = false;
        m_statusLabel->setText(i18n("The background service did not start."));
        m_runButton->setEnabled(true);
    }
}

void BackgroundServiceDialog::handleStarted()
{
    m_running = true;
    updateUi();
}

void BackgroundServiceDialog::handleFinished(bool success, int connected, int mounted,
                                             const QString &message)
{
    Q_UNUSED(success);
    Q_UNUSED(connected);
    Q_UNUSED(mounted);
    Q_UNUSED(message);
    refreshStatus();
}
