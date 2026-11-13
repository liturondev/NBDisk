/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nbdservice.h"
#include "startuprunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

class StartupRunnerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanup();

    void finishesWithoutProfiles();
    void connectsProfilesWithAutoConnect();
    void reportsFailedConnections();

private:
    QString stateFilePath() const;
    QString freeDevicePath() const;
    void writeProfiles(const QString &device, bool autoConnect);
    void writeFakeCommands(const QString &binDir, const QString &logPath,
                           int nbdClientExitCode) const;
    bool makeExecutable(const QString &path) const;
};

void StartupRunnerTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    // Use the same application name as the daemon so the profile store matches.
    QCoreApplication::setApplicationName(QStringLiteral("nbdisk"));
}

void StartupRunnerTest::cleanup()
{
    QFile::remove(stateFilePath());
}

QString StartupRunnerTest::stateFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/connections.json");
}

QString StartupRunnerTest::freeDevicePath() const
{
    const QDir classDir(QStringLiteral("/sys/class/block"));
    const QStringList entries = classDir.entryList(
        {QStringLiteral("nbd*")}, QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        const QString sysfsPath = classDir.filePath(entry);
        if (!QFileInfo(sysfsPath).isDir()) {
            continue;
        }
        if (QFileInfo::exists(sysfsPath + QStringLiteral("/pid"))) {
            continue;
        }
        QFile sizeFile(sysfsPath + QStringLiteral("/size"));
        if (!sizeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        if (sizeFile.readAll().trimmed().toLongLong() != 0) {
            continue;
        }
        return QStringLiteral("/dev/") + entry;
    }
    return QString();
}

bool StartupRunnerTest::makeExecutable(const QString &path) const
{
    return QFile::setPermissions(path,
                                 QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

void StartupRunnerTest::writeFakeCommands(const QString &binDir, const QString &logPath,
                                          int nbdClientExitCode) const
{
    // Log the command line, then actually run it so its exit status propagates.
    QFile pkexec(binDir + QStringLiteral("/pkexec"));
    if (pkexec.open(QIODevice::WriteOnly | QIODevice::Text)) {
        pkexec.write("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"");
        pkexec.write(logPath.toUtf8());
        pkexec.write("\"\nexec \"$@\"\n");
    }
    makeExecutable(pkexec.fileName());

    // The runner may try to load the module before connecting.
    QFile modprobe(binDir + QStringLiteral("/modprobe"));
    if (modprobe.open(QIODevice::WriteOnly | QIODevice::Text)) {
        modprobe.write("#!/bin/sh\nexit 0\n");
    }
    makeExecutable(modprobe.fileName());

    QFile nbdClient(binDir + QStringLiteral("/nbd-client"));
    if (nbdClient.open(QIODevice::WriteOnly | QIODevice::Text)) {
        nbdClient.write("#!/bin/sh\nexit ");
        nbdClient.write(QByteArray::number(nbdClientExitCode));
        nbdClient.write("\n");
    }
    makeExecutable(nbdClient.fileName());
}

void StartupRunnerTest::writeProfiles(const QString &device, bool autoConnect)
{
    QJsonObject profile;
    profile.insert(QStringLiteral("host"), QStringLiteral("example.org"));
    profile.insert(QStringLiteral("port"), 10809);
    profile.insert(QStringLiteral("export"), QStringLiteral("disk-1.img"));
    profile.insert(QStringLiteral("backend"), static_cast<int>(NbdBackend::NbdClient));
    profile.insert(QStringLiteral("autoConnect"), autoConnect);
    profile.insert(QStringLiteral("autoMount"), false);

    QJsonObject profiles;
    profiles.insert(device, profile);

    QJsonObject root;
    root.insert(QStringLiteral("version"), 3);
    root.insert(QStringLiteral("connections"), QJsonObject());
    root.insert(QStringLiteral("profiles"), profiles);

    const QString path = stateFilePath();
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void StartupRunnerTest::finishesWithoutProfiles()
{
    QFile::remove(stateFilePath());

    NbdService service;
    StartupRunner runner(&service);
    QSignalSpy finished(&runner, &StartupRunner::finished);

    runner.start();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QVERIFY(!runner.hadFailures());
    QCOMPARE(runner.connectedCount(), 0);
    QCOMPARE(runner.mountedCount(), 0);
}

void StartupRunnerTest::connectsProfilesWithAutoConnect()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString binDir = tempDir.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(binDir));
    const QString logPath = tempDir.filePath(QStringLiteral("pkexec.log"));
    writeFakeCommands(binDir, logPath, 0);

    const QString device = freeDevicePath();
    if (device.isEmpty()) {
        QSKIP("No disconnected NBD device is available to test with.");
    }
    writeProfiles(device, true);

    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", binDir.toUtf8() + ':' + oldPath);

    NbdService service;
    StartupRunner runner(&service);
    QSignalSpy finished(&runner, &StartupRunner::finished);

    runner.start();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);

    QVERIFY2(!runner.hadFailures(), "the fake connect should succeed");
    QCOMPARE(runner.connectedCount(), 1);
    QCOMPARE(runner.mountedCount(), 0);

    if (!service.isRoot()) {
        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString command = QString::fromUtf8(log.readAll());
        QVERIFY2(command.contains(QStringLiteral("nbd-client")), qPrintable(command));
        QVERIFY2(command.contains(device), qPrintable(command));
        QVERIFY2(command.contains(QStringLiteral("example.org")), qPrintable(command));
    }

    // Triggering a second pass must report fresh results, not accumulated ones.
    const int runs = finished.count();
    runner.start();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), runs + 1, 10000);
    QVERIFY(!runner.hadFailures());
    QCOMPARE(runner.connectedCount(), 1);
    QCOMPARE(runner.mountedCount(), 0);

    qputenv("PATH", oldPath);
}

void StartupRunnerTest::reportsFailedConnections()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString binDir = tempDir.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(binDir));
    const QString logPath = tempDir.filePath(QStringLiteral("pkexec.log"));
    writeFakeCommands(binDir, logPath, 1);

    const QString device = freeDevicePath();
    if (device.isEmpty()) {
        QSKIP("No disconnected NBD device is available to test with.");
    }
    writeProfiles(device, true);

    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", binDir.toUtf8() + ':' + oldPath);

    NbdService service;
    StartupRunner runner(&service);
    QSignalSpy finished(&runner, &StartupRunner::finished);

    runner.start();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
    QVERIFY(runner.hadFailures());
    QCOMPARE(runner.connectedCount(), 0);

    qputenv("PATH", oldPath);
}

QTEST_GUILESS_MAIN(StartupRunnerTest)

#include "startuprunner_test.moc"
