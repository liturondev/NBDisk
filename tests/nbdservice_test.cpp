/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nbdservice.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

class NbdServiceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void customMountPointUsesPrivilegedMount();
    void unlockUsesLuksOpenAndPassphraseOnStdin();
    void commandTimeoutReleasesBusyState();
    void scanDevicesDoesNotWaitForLsblk();
    void rememberedVolumesRoundTrip();
    void deleteProfileRemovesSavedProfile();
};

void NbdServiceTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void NbdServiceTest::cleanup()
{
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + QStringLiteral("/connections.json"));
}

void NbdServiceTest::customMountPointUsesPrivilegedMount()
{
    if (geteuid() == 0) {
        QSKIP("Privileged commands run directly without pkexec as root.");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString binDir = tempDir.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(binDir));

    const QString logPath = tempDir.filePath(QStringLiteral("pkexec.log"));
    const QString pkexecPath = binDir + QStringLiteral("/pkexec");
    QFile pkexec(pkexecPath);
    QVERIFY(pkexec.open(QIODevice::WriteOnly | QIODevice::Text));
    pkexec.write("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"");
    pkexec.write(logPath.toUtf8());
    pkexec.write("\"\nexit 0\n");
    pkexec.close();
    QVERIFY(QFile::setPermissions(pkexecPath,
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", binDir.toUtf8() + ':' + oldPath);

    NbdService service;
    QSignalSpy finished(&service, &NbdService::operationFinished);

    const QString mountPoint = tempDir.filePath(QStringLiteral("mnt/volume"));
    service.mountVolume(QStringLiteral("/dev/nbd1"), QStringLiteral("ext4"), true,
                        mountPoint);

    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QVERIFY(finished.at(0).at(0).toBool());

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString commands = QString::fromUtf8(log.readAll());
    QVERIFY2(commands.contains(QStandardPaths::findExecutable(QStringLiteral("mkdir"))),
             qPrintable(commands));
    QVERIFY2(commands.contains(QStringLiteral("-p")), qPrintable(commands));
    QVERIFY2(commands.contains(mountPoint), qPrintable(commands));
    QVERIFY2(commands.contains(QStandardPaths::findExecutable(QStringLiteral("mount"))),
             qPrintable(commands));
    QVERIFY2(commands.contains(QStringLiteral("-t ext4")), qPrintable(commands));
    QVERIFY2(commands.contains(QStringLiteral("-o ro")), qPrintable(commands));
    QVERIFY2(commands.contains(QStringLiteral("/dev/nbd1")), qPrintable(commands));

    qputenv("PATH", oldPath);
}

void NbdServiceTest::unlockUsesLuksOpenAndPassphraseOnStdin()
{
    if (geteuid() == 0) {
        QSKIP("Privileged commands run directly without pkexec as root.");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString binDir = tempDir.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(binDir));

    const QString logPath = tempDir.filePath(QStringLiteral("pkexec.log"));
    const QString stdinPath = tempDir.filePath(QStringLiteral("pkexec.stdin"));
    const QString pkexecPath = binDir + QStringLiteral("/pkexec");
    QFile pkexec(pkexecPath);
    QVERIFY(pkexec.open(QIODevice::WriteOnly | QIODevice::Text));
    pkexec.write("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"");
    pkexec.write(logPath.toUtf8());
    pkexec.write("\"\ncase \"$*\" in\n  *cryptsetup*) cat > \"");
    pkexec.write(stdinPath.toUtf8());
    pkexec.write("\" ;;\nesac\nexit 0\n");
    pkexec.close();
    QVERIFY(QFile::setPermissions(pkexecPath,
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QString cryptsetupPath = binDir + QStringLiteral("/cryptsetup");
    QFile cryptsetup(cryptsetupPath);
    QVERIFY(cryptsetup.open(QIODevice::WriteOnly | QIODevice::Text));
    cryptsetup.write("#!/bin/sh\nexit 0\n");
    cryptsetup.close();
    QVERIFY(QFile::setPermissions(cryptsetupPath,
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", binDir.toUtf8() + ':' + oldPath);

    NbdService service;
    QSignalSpy finished(&service, &NbdService::operationFinished);
    const QByteArray passphrase("correct horse battery staple");
    service.unlockVolume(QStringLiteral("/dev/nbd9"), QString::fromUtf8(passphrase), true);

    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QVERIFY(finished.at(0).at(0).toBool());

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString command = QString::fromUtf8(log.readAll());
    QVERIFY2(command.contains(cryptsetupPath), qPrintable(command));
    QVERIFY2(command.contains(QStringLiteral("--readonly")), qPrintable(command));
    QVERIFY2(command.contains(QStringLiteral("--key-file=-")), qPrintable(command));
    QVERIFY2(command.contains(QStringLiteral("luksOpen")), qPrintable(command));
    QVERIFY2(command.contains(QStringLiteral("/dev/nbd9")), qPrintable(command));
    QVERIFY2(command.contains(QStringLiteral("nbdisk-nbd9")), qPrintable(command));

    QFile stdinFile(stdinPath);
    QVERIFY(stdinFile.open(QIODevice::ReadOnly));
    QCOMPARE(stdinFile.readAll(), passphrase);

    qputenv("PATH", oldPath);
}

void NbdServiceTest::commandTimeoutReleasesBusyState()
{
    if (geteuid() == 0) {
        QSKIP("Privileged commands run directly without pkexec as root.");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString binDir = tempDir.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(binDir));

    const QString pkexecPath = binDir + QStringLiteral("/pkexec");
    QFile pkexec(pkexecPath);
    QVERIFY(pkexec.open(QIODevice::WriteOnly | QIODevice::Text));
    pkexec.write("#!/bin/sh\nsleep 30\n");
    pkexec.close();
    QVERIFY(QFile::setPermissions(pkexecPath,
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", binDir.toUtf8() + ':' + oldPath);

    NbdService service;
    service.setCommandTimeoutMs(300);
    QSignalSpy finished(&service, &NbdService::operationFinished);

    const QString mountPoint = tempDir.filePath(QStringLiteral("mnt/volume"));
    service.mountVolume(QStringLiteral("/dev/nbd1"), QStringLiteral("ext4"), false,
                        mountPoint);

    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QVERIFY(!finished.at(0).at(0).toBool());
    QVERIFY2(finished.at(0).at(1).toString().contains(QStringLiteral("Timed out")),
             qPrintable(finished.at(0).at(1).toString()));
    QVERIFY(!service.isBusy());

    // The timeout must release the service, not wedge it behind the dead process.
    service.mountVolume(QStringLiteral("/dev/nbd1"), QStringLiteral("ext4"), false,
                        mountPoint);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
    QVERIFY(!finished.at(1).at(0).toBool());
    QVERIFY(!service.isBusy());

    qputenv("PATH", oldPath);
}

void NbdServiceTest::scanDevicesDoesNotWaitForLsblk()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString binDir = tempDir.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(binDir));

    const QString lsblkPath = binDir + QStringLiteral("/lsblk");
    QFile lsblk(lsblkPath);
    QVERIFY(lsblk.open(QIODevice::WriteOnly | QIODevice::Text));
    lsblk.write("#!/bin/sh\nsleep 30\n");
    lsblk.close();
    QVERIFY(QFile::setPermissions(lsblkPath,
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", binDir.toUtf8() + ':' + oldPath);

    NbdService service;
    QString error;

    QElapsedTimer timer;
    timer.start();
    service.scanDevices(&error);
    QVERIFY2(timer.elapsed() < 1000,
             qPrintable(QStringLiteral("first scan took %1 ms").arg(timer.elapsed())));

    // A probe still running must not serialize the next scan either.
    QTest::qWait(20);
    timer.restart();
    service.scanDevices(&error);
    QVERIFY2(timer.elapsed() < 500,
             qPrintable(QStringLiteral("repeat scan took %1 ms").arg(timer.elapsed())));

    qputenv("PATH", oldPath);
}

void NbdServiceTest::rememberedVolumesRoundTrip()
{
    NbdService service;
    ConnectOptions options;
    options.device = QStringLiteral("/dev/nbd0");
    options.host = QStringLiteral("example.org");
    options.exportName = QStringLiteral("disk.img");
    service.saveProfile(options, true);
    service.setProfileRememberPassphrase(QStringLiteral("/dev/nbd0"),
                                         QStringLiteral("/dev/nbd0p1"), true);

    // The marker must survive a reload without the passphrase being stored.
    service.reloadState();
    const auto profile = service.profileForDevice(QStringLiteral("/dev/nbd0"));
    QVERIFY(profile.has_value());
    QVERIFY(profile->autoConnect);
    QVERIFY(profile->rememberedVolumes.contains(QStringLiteral("/dev/nbd0p1")));
    QVERIFY(service.volumePassphrase(QStringLiteral("/dev/nbd0"),
                                     QStringLiteral("/dev/nbd0p1"))
                .isEmpty());

    service.setProfileRememberPassphrase(QStringLiteral("/dev/nbd0"),
                                         QStringLiteral("/dev/nbd0p1"), false);
    service.reloadState();
    const auto cleared = service.profileForDevice(QStringLiteral("/dev/nbd0"));
    QVERIFY(cleared.has_value());
    QVERIFY(cleared->rememberedVolumes.isEmpty());
}

void NbdServiceTest::deleteProfileRemovesSavedProfile()
{
    NbdService service;
    ConnectOptions options;
    options.device = QStringLiteral("/dev/nbd0");
    options.host = QStringLiteral("example.org");
    service.saveProfile(options, false);
    QVERIFY(service.profileForDevice(QStringLiteral("/dev/nbd0")).has_value());

    service.deleteProfile(QStringLiteral("/dev/nbd0"));
    QVERIFY(!service.profileForDevice(QStringLiteral("/dev/nbd0")).has_value());

    service.reloadState();
    QVERIFY(!service.profileForDevice(QStringLiteral("/dev/nbd0")).has_value());
}

QTEST_GUILESS_MAIN(NbdServiceTest)

#include "nbdservice_test.moc"
