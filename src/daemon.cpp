/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "backgroundservice.h"

#include <KLocalizedString>

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QLockFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <cstdio>

namespace
{
// Bounds a single login-time pass when the process exits afterwards.
constexpr int kOnceTimeoutMs = 10 * 60 * 1000;
// A dead daemon's lock file is reclaimed after this long.
constexpr int kLockStaleMs = 2 * 60 * 1000;

void printUsage()
{
    std::printf("Usage: nbdisk-daemon [--once]\n"
                "\n"
                "Reconnects saved NBD profiles and mounts their configured volumes.\n"
                "By default it also stays on the session bus and serves\n"
                "io.github.liturondev.NBDisk.Service requests from the NBDisk window.\n"
                "--once runs a single pass, then exits with 0 on success or 1 on\n"
                "failure.\n");
}
}

int main(int argc, char *argv[])
{
    // The service has no terminal, so Qt would otherwise hand messages to the
    // system logger and drop them when nothing is collecting. Keep them on
    // stderr, which works the same when started from a shell or a session.
    qputenv("QT_FORCE_STDERR_LOGGING", QByteArrayLiteral("1"));

    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nbdisk"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("io.github.liturondev.NBDisk"));
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("nbdisk"));

    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.contains(QStringLiteral("--help"))
        || arguments.contains(QStringLiteral("-h"))) {
        printUsage();
        return 0;
    }
    const bool runOnce = arguments.contains(QStringLiteral("--once"));

    const QString stateDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(stateDirectory);

    // One service per session: a resident daemon keeps this lock, and a second
    // login attempt exits quietly.
    QLockFile lock(stateDirectory + QStringLiteral("/startup.lock"));
    lock.setStaleLockTime(kLockStaleMs);
    if (!lock.tryLock(0)) {
        qInfo() << QStringLiteral("Background service is already running.");
        return 0;
    }

    BackgroundService service;
    const bool onBus = service.registerOnBus();
    if (!onBus) {
        qWarning() << QStringLiteral(
            "The session bus is unavailable; running one pass and exiting.");
    }

    if (runOnce || !onBus) {
        QObject::connect(&service, &BackgroundService::Finished, &application,
                         [&application](bool success, int, int, const QString &) {
                             application.exit(success ? 0 : 1);
                         });
        QTimer::singleShot(kOnceTimeoutMs, &application, [&application]() {
            qWarning() << QStringLiteral("Startup pass did not finish in time.");
            application.exit(1);
        });
    }

    // Runs at login (and answers RunStartup on the bus when resident).
    QTimer::singleShot(0, &service, &BackgroundService::RunStartup);
    return application.exec();
}
