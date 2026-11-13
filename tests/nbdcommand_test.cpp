/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nbdcommand.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class NbdCommandTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void nbdClientArguments();
    void qemuNbdArguments();
    void disconnectArguments();
    void nbdUrlEncoding();
    void validation();
    void tlsValidation();
    void devicePaths();
    void backendNames();
};

void NbdCommandTest::nbdClientArguments()
{
    ConnectOptions options;
    options.device = QStringLiteral("/dev/nbd0");
    options.host = QStringLiteral("disk.example.org");
    options.port = 10809;
    options.exportName = QStringLiteral("backup");
    options.readOnly = true;
    options.blockSize = 4096;
    options.timeoutSeconds = 30;
    options.persistent = true;
    options.tls = true;
    options.certFile = QStringLiteral("/tmp/client.pem");
    options.keyFile = QStringLiteral("/tmp/client.key");
    options.caFile = QStringLiteral("/tmp/ca.pem");
    options.tlsHostname = QStringLiteral("disk.example.org");

    const QStringList expected = {
        QStringLiteral("-N"),
        QStringLiteral("backup"),
        QStringLiteral("disk.example.org"),
        QStringLiteral("10809"),
        QStringLiteral("/dev/nbd0"),
        QStringLiteral("-b"),
        QStringLiteral("4096"),
        QStringLiteral("-t"),
        QStringLiteral("30"),
        QStringLiteral("-R"),
        QStringLiteral("-p"),
        QStringLiteral("-x"),
        QStringLiteral("-F"),
        QStringLiteral("/tmp/client.pem"),
        QStringLiteral("-K"),
        QStringLiteral("/tmp/client.key"),
        QStringLiteral("-A"),
        QStringLiteral("/tmp/ca.pem"),
        QStringLiteral("-H"),
        QStringLiteral("disk.example.org"),
    };

    QCOMPARE(NbdCommandBuilder::connectArguments(options, NbdBackend::NbdClient), expected);
}

void NbdCommandTest::qemuNbdArguments()
{
    ConnectOptions options;
    options.device = QStringLiteral("/dev/nbd2");
    options.host = QStringLiteral("192.0.2.10");
    options.port = 10810;
    options.exportName = QStringLiteral("share/one");
    options.readOnly = true;

    const QStringList expected = {
        QStringLiteral("--connect=/dev/nbd2"),
        QStringLiteral("--read-only"),
        QStringLiteral("nbd://192.0.2.10:10810/share/one"),
    };

    QCOMPARE(NbdCommandBuilder::connectArguments(options, NbdBackend::QemuNbd), expected);
}

void NbdCommandTest::disconnectArguments()
{
    QCOMPARE(NbdCommandBuilder::disconnectArguments(QStringLiteral("/dev/nbd1"),
                                                    NbdBackend::NbdClient),
             QStringList({QStringLiteral("-d"), QStringLiteral("/dev/nbd1")}));
    QCOMPARE(NbdCommandBuilder::disconnectArguments(QStringLiteral("/dev/nbd1"),
                                                    NbdBackend::QemuNbd),
             QStringList({QStringLiteral("--disconnect"), QStringLiteral("/dev/nbd1")}));
}

void NbdCommandTest::nbdUrlEncoding()
{
    ConnectOptions options;
    options.host = QStringLiteral("2001:db8::1");
    options.port = 10809;
    options.exportName = QStringLiteral("a b");
    QCOMPARE(NbdCommandBuilder::nbdUrl(options),
             QStringLiteral("nbd://[2001:db8::1]:10809/a%20b"));

    options.exportName.clear();
    QCOMPARE(NbdCommandBuilder::nbdUrl(options),
             QStringLiteral("nbd://[2001:db8::1]:10809/"));
}

void NbdCommandTest::validation()
{
    ConnectOptions options;
    options.device = QStringLiteral("/dev/nbd0");
    options.host = QStringLiteral("server.example.org");

    QString error;
    QVERIFY(NbdCommandBuilder::validate(options, NbdBackend::QemuNbd, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    options.timeoutSeconds = 15;
    QVERIFY(!NbdCommandBuilder::validate(options, NbdBackend::QemuNbd, &error));
    QVERIFY(!error.isEmpty());

    options.timeoutSeconds = 0;
    options.host = QStringLiteral("-oProxyCommand=bad");
    QVERIFY(!NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error));

    options.host = QStringLiteral("server.example.org");
    options.blockSize = 1234;
    QVERIFY(!NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error));

    options.blockSize = 512;
    QVERIFY(NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error));
    QVERIFY(!NbdCommandBuilder::validate(options, NbdBackend::Auto, &error));

    // Only a whole, numbered NBD device may be connected.
    options.device = QStringLiteral("/dev/nbd0p1");
    QVERIFY(!NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error));
    options.device = QStringLiteral("/dev/nbdX");
    QVERIFY(!NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error));
    options.device = QStringLiteral("/dev/nbd0");
    QVERIFY(NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error));

    // Internal whitespace cannot be handed to the backend.
    options.host = QStringLiteral("server example.org");
    QVERIFY(!NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error));
    options.host = QStringLiteral("server.example.org");
    QVERIFY(NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error));
}

void NbdCommandTest::tlsValidation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString certPath = dir.filePath(QStringLiteral("client.pem"));
    const QString keyPath = dir.filePath(QStringLiteral("client.key"));
    for (const QString &path : {certPath, keyPath}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("test"), 4);
    }

    ConnectOptions options;
    options.device = QStringLiteral("/dev/nbd0");
    options.host = QStringLiteral("disk.example.org");
    options.tls = true;

    QString error;
    QVERIFY2(NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error),
             qPrintable(error));

    // A certificate is useless without its private key.
    options.certFile = certPath;
    QVERIFY(!NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error));
    QVERIFY2(error.contains(QStringLiteral("certificate")), qPrintable(error));

    options.keyFile = keyPath;
    QVERIFY2(NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error),
             qPrintable(error));

    // Missing files must be reported before nbd-client runs.
    options.keyFile = dir.filePath(QStringLiteral("missing.key"));
    QVERIFY(!NbdCommandBuilder::validate(options, NbdBackend::NbdClient, &error));
    QVERIFY2(error.contains(QStringLiteral("not readable")), qPrintable(error));

    // qemu-nbd has no TLS support at all.
    options.keyFile = keyPath;
    QVERIFY(!NbdCommandBuilder::validate(options, NbdBackend::QemuNbd, &error));
    QVERIFY2(error.contains(QStringLiteral("qemu-nbd")), qPrintable(error));
}

void NbdCommandTest::devicePaths()
{
    QVERIFY(isNbdDevicePath(QStringLiteral("/dev/nbd0")));
    QVERIFY(isNbdDevicePath(QStringLiteral("/dev/nbd127")));
    QVERIFY(!isNbdDevicePath(QStringLiteral("/dev/nbd")));
    QVERIFY(!isNbdDevicePath(QStringLiteral("/dev/nbd0p1")));
    QVERIFY(!isNbdDevicePath(QStringLiteral("/dev/nbd0 ")));
    QVERIFY(!isNbdDevicePath(QStringLiteral("/dev/nbdX")));
    QVERIFY(!isNbdDevicePath(QStringLiteral("/dev/sda")));
    QVERIFY(!isNbdDevicePath(QString()));

    // Profiles use the same rule, so a stale or hand-edited key is ignored.
    NbdProfile profile;
    profile.options.host = QStringLiteral("server.example.org");
    profile.options.device = QStringLiteral("/dev/nbd0");
    QVERIFY(profile.isValid());

    profile.options.device = QStringLiteral("/dev/nbd0p1");
    QVERIFY(!profile.isValid());

    profile.options.device = QStringLiteral("/dev/nbd0");
    profile.options.host.clear();
    QVERIFY(!profile.isValid());
}

void NbdCommandTest::backendNames()
{
    QCOMPARE(NbdCommandBuilder::backendName(NbdBackend::NbdClient),
             QStringLiteral("nbd-client"));
    QCOMPARE(NbdCommandBuilder::backendName(NbdBackend::QemuNbd),
             QStringLiteral("qemu-nbd"));
    QCOMPARE(NbdCommandBuilder::backendName(NbdBackend::Auto),
             QStringLiteral("automatic"));
    QVERIFY(NbdCommandBuilder::supportsAdvancedOptions(NbdBackend::NbdClient));
    QVERIFY(!NbdCommandBuilder::supportsAdvancedOptions(NbdBackend::QemuNbd));
}

QTEST_GUILESS_MAIN(NbdCommandTest)

#include "nbdcommand_test.moc"
