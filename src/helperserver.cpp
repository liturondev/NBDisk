/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "helperserver.h"

#include "helperprotocol.h"
#include "nbdcommand.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr int kMaximumRequestBytes = 128 * 1024;
constexpr int kCommandTimeoutMs = 120 * 1000;

struct ExecResult {
    bool success = false;
    int exitCode = 1;
    QString output;
};

bool hasControlCharacters(const QString &value)
{
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7f) {
            return true;
        }
    }
    return false;
}

bool isValidDevice(const QString &device)
{
    static const QRegularExpression pattern(QStringLiteral("^/dev/nbd\\d+$"));
    return device.size() <= 32 && pattern.match(device).hasMatch();
}

bool isValidVolume(const QString &volume)
{
    static const QRegularExpression pattern(
        QStringLiteral("^/dev/nbd\\d+(p\\d+)?$"));
    return volume.size() <= 64 && pattern.match(volume).hasMatch();
}

bool isValidMountSource(const QString &source)
{
    static const QRegularExpression mapperPattern(
        QStringLiteral("^/dev/mapper/nbdisk-[A-Za-z0-9_.-]+$"));
    return isValidVolume(source)
        || (source.size() <= 128 && mapperPattern.match(source).hasMatch());
}

bool isValidFileSystem(const QString &fileSystem)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9._+-]*$"));
    return fileSystem.size() <= 64 && pattern.match(fileSystem).hasMatch();
}

QString mapperNameForVolume(const QString &path)
{
    QString name = QFileInfo(path).fileName();
    if (name.isEmpty()) {
        return {};
    }
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                 QStringLiteral("_"));
    return QStringLiteral("nbdisk-%1").arg(name);
}

bool underAllowedRoot(const QString &path)
{
    static const QStringList roots = {
        QStringLiteral("/mnt"),
        QStringLiteral("/media"),
        QStringLiteral("/run/media"),
    };
    for (const QString &root : roots) {
        if (path == root || path.startsWith(root + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

QString homeForUser(uid_t uid)
{
    if (struct passwd *user = getpwuid(uid)) {
        const QString home = QString::fromLocal8Bit(user->pw_dir);
        if (QDir::isAbsolutePath(home) && home != QStringLiteral("/")) {
            return QDir::cleanPath(home);
        }
    }
    return {};
}

bool underPath(const QString &path, const QString &root)
{
    return root != QStringLiteral("/")
        && (path == root || path.startsWith(root + QLatin1Char('/')));
}

// Mount points are limited to the standard share locations and the calling
// user's own home. Resolve existing ancestors so a symlink cannot redirect the
// mount somewhere else.
bool isAllowedMountTarget(const QString &target, uid_t uid)
{
    if (!QDir::isAbsolutePath(target)) {
        return false;
    }
    static const QStringList shareRoots = {
        QStringLiteral("/mnt"),
        QStringLiteral("/media"),
        QStringLiteral("/run/media"),
    };
    const QString cleaned = QDir::cleanPath(target);
    if (cleaned != target || shareRoots.contains(cleaned)) {
        return false;
    }

    const QString home = homeForUser(uid);
    const bool inShare = underAllowedRoot(cleaned);
    const bool inHome = !home.isEmpty() && cleaned != home
        && underPath(cleaned, home);
    if (!inShare && !inHome) {
        return false;
    }

    QString existing = cleaned;
    bool checkingTarget = true;
    while (!existing.isEmpty()) {
        const QFileInfo info(existing);
        if (info.exists()) {
            const QString canonical = info.canonicalFilePath();
            if (canonical.isEmpty()) {
                return false;
            }
            if (inShare) {
                // Never mount over a share root itself.
                return !(checkingTarget && shareRoots.contains(canonical))
                    && underAllowedRoot(canonical);
            }
            return !(checkingTarget && canonical == home)
                && underPath(canonical, home);
        }
        const QString parent = info.absolutePath();
        if (parent == existing || parent == QStringLiteral("/")) {
            return false;
        }
        existing = parent;
        checkingTarget = false;
    }
    return false;
}

ExecResult runProgram(const QString &program, const QStringList &arguments,
                      const QByteArray &stdinData = QByteArray())
{
    ExecResult result;
    if (program.isEmpty()) {
        result.exitCode = HelperProtocol::UnavailableExitCode;
        result.output = QStringLiteral("The required program is not installed.");
        return result;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.start(program, arguments);
    if (!process.waitForStarted(10000)) {
        result.exitCode = HelperProtocol::UnavailableExitCode;
        result.output = process.errorString();
        return result;
    }

    if (!stdinData.isEmpty()) {
        process.write(stdinData);
        process.closeWriteChannel();
    }

    if (!process.waitForFinished(kCommandTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        result.exitCode = 124;
        result.output = QStringLiteral("The command timed out.");
        return result;
    }

    result.output = QString::fromLocal8Bit(process.readAll()).trimmed();
    result.success = process.exitStatus() == QProcess::NormalExit
        && process.exitCode() == 0;
    result.exitCode = process.exitCode();
    return result;
}

QJsonObject response(bool success, int exitCode, const QString &message,
                     const QString &output)
{
    QJsonObject value;
    value.insert(QStringLiteral("success"), success);
    value.insert(QStringLiteral("exitCode"), exitCode);
    value.insert(QStringLiteral("message"), message);
    value.insert(QStringLiteral("output"), output);
    return value;
}

QJsonObject rejected(const QString &message)
{
    return response(false, HelperProtocol::UnavailableExitCode, message, QString());
}

QString findProgram(const QString &name)
{
    return QStandardPaths::findExecutable(name);
}

bool userInGroup(uid_t uid, gid_t gid, const QString &groupName)
{
    if (uid == 0) {
        return true;
    }

    struct group *group = getgrnam(groupName.toLocal8Bit().constData());
    if (!group) {
        return false;
    }
    if (gid == group->gr_gid) {
        return true;
    }

    struct passwd *user = getpwuid(uid);
    if (!user) {
        return false;
    }
    for (char **member = group->gr_mem; member && *member; ++member) {
        if (qstrcmp(*member, user->pw_name) == 0) {
            return true;
        }
    }
    return false;
}

QString helperGroupName()
{
    const QByteArray override = qgetenv("NBDISK_HELPER_GROUP");
    return override.isEmpty() ? QStringLiteral("nbdisk")
                              : QString::fromLocal8Bit(override);
}
}

HelperServer::HelperServer(QObject *parent)
    : QObject(parent)
{
}

bool HelperServer::listen(QString *error)
{
    const QString path = HelperProtocol::socketPath();
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        if (error) {
            *error = QStringLiteral("Could not create %1.").arg(directory);
        }
        return false;
    }

    gid_t socketGroup = getgid();
    struct group *group = getgrnam(helperGroupName().toLocal8Bit().constData());
    if (group) {
        socketGroup = group->gr_gid;
    } else {
        qWarning("Helper group '%s' does not exist; only root can connect.",
                 qPrintable(helperGroupName()));
    }

    if (geteuid() == 0) {
        if (chown(directory.toLocal8Bit().constData(), 0, socketGroup) != 0
            || chmod(directory.toLocal8Bit().constData(), 0750) != 0) {
            if (error) {
                *error = QStringLiteral("Could not secure %1.").arg(directory);
            }
            return false;
        }
    }

    QLocalServer::removeServer(path);
    m_server = new QLocalServer(this);
    m_server->setMaxPendingConnections(8);
    if (!m_server->listen(path)) {
        if (error) {
            *error = m_server->errorString();
        }
        return false;
    }

    if (geteuid() == 0) {
        chown(path.toLocal8Bit().constData(), 0, socketGroup);
        chmod(path.toLocal8Bit().constData(), 0660);
    }

    connect(m_server, &QLocalServer::newConnection, this,
            &HelperServer::handleNewConnection);
    return true;
}

void HelperServer::handleNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        handleConnection(socket);
        socket->deleteLater();
    }
}

void HelperServer::handleConnection(QLocalSocket *socket)
{
    socket->setParent(this);

    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (getsockopt(socket->socketDescriptor(), SOL_SOCKET, SO_PEERCRED,
                   &credentials, &length) != 0) {
        const QJsonObject reply =
            rejected(QStringLiteral("Could not identify the caller."));
        socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
        return;
    }

    if (geteuid() == 0
        && !userInGroup(credentials.uid, credentials.gid, helperGroupName())) {
        qWarning("Denied helper request from uid %u.", credentials.uid);
        const QJsonObject reply = rejected(
            QStringLiteral("The user is not allowed to use the NBDisk helper. "
                           "Add it to the '%1' group.")
                .arg(helperGroupName()));
        socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
        return;
    }

    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();
    while (!buffer.contains('\n') && timer.elapsed() < 5000) {
        if (!socket->waitForReadyRead(5000 - static_cast<int>(timer.elapsed()))) {
            break;
        }
        buffer.append(socket->readAll());
        if (buffer.size() > kMaximumRequestBytes) {
            break;
        }
    }

    QJsonObject reply;
    const int newline = buffer.indexOf('\n');
    if (newline < 0 || buffer.size() > kMaximumRequestBytes) {
        reply = rejected(QStringLiteral("Invalid helper request."));
    } else {
        const QJsonDocument document = QJsonDocument::fromJson(buffer.left(newline));
        if (!document.isObject()
            || document.object().value(QStringLiteral("version")).toInt()
                != HelperProtocol::ProtocolVersion) {
            reply = rejected(QStringLiteral("Unsupported helper protocol."));
        } else {
            reply = handleRequest(document.object(), credentials.uid,
                                  credentials.gid);
        }
    }

    socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
    socket->flush();
    socket->waitForBytesWritten(2000);
    socket->disconnectFromServer();
}

QJsonObject HelperServer::handleRequest(const QJsonObject &request, uid_t callerUid,
                                        gid_t)
{
    const QString operation = request.value(QStringLiteral("operation")).toString();
    const QJsonObject args = request.value(QStringLiteral("arguments")).toObject();

    if (operation == QStringLiteral("load_module")) {
        const ExecResult result = runProgram(
            findProgram(QStringLiteral("modprobe")),
            {QStringLiteral("nbd"), QStringLiteral("max_part=16")});
        return response(result.success, result.exitCode,
                        result.success ? QStringLiteral("The nbd module was loaded.")
                                       : result.output,
                        result.output);
    }

    if (operation == QStringLiteral("connect")
        || operation == QStringLiteral("disconnect")) {
        const int backendValue = args.value(QStringLiteral("backend")).toInt();
        if (backendValue != static_cast<int>(NbdBackend::NbdClient)
            && backendValue != static_cast<int>(NbdBackend::QemuNbd)) {
            return rejected(QStringLiteral("Unknown NBD backend."));
        }
        const auto backend = static_cast<NbdBackend>(backendValue);
        const QString device = args.value(QStringLiteral("device")).toString();
        if (!isValidDevice(device)) {
            return rejected(QStringLiteral("Invalid NBD device."));
        }

        const QString program = findProgram(
            backend == NbdBackend::QemuNbd ? QStringLiteral("qemu-nbd")
                                           : QStringLiteral("nbd-client"));
        if (program.isEmpty()) {
            return response(false, 127, QStringLiteral("The NBD backend is not installed."),
                            QString());
        }

        if (operation == QStringLiteral("disconnect")) {
            const ExecResult result = runProgram(
                program, NbdCommandBuilder::disconnectArguments(device, backend));
            return response(result.success, result.exitCode,
                            result.success
                                ? QStringLiteral("Disconnected %1.").arg(device)
                                : result.output,
                            result.output);
        }

        ConnectOptions options;
        options.device = device;
        options.host = args.value(QStringLiteral("host")).toString();
        options.port = static_cast<quint16>(
            qBound(0, args.value(QStringLiteral("port")).toInt(), 65535));
        options.exportName = args.value(QStringLiteral("export")).toString();
        options.readOnly = args.value(QStringLiteral("readOnly")).toBool();
        options.blockSize = args.value(QStringLiteral("blockSize")).toInt(512);
        options.timeoutSeconds = args.value(QStringLiteral("timeout")).toInt();
        options.persistent = args.value(QStringLiteral("persistent")).toBool();
        options.tls = args.value(QStringLiteral("tls")).toBool();
        options.certFile = args.value(QStringLiteral("certFile")).toString();
        options.keyFile = args.value(QStringLiteral("keyFile")).toString();
        options.caFile = args.value(QStringLiteral("caFile")).toString();
        options.tlsHostname = args.value(QStringLiteral("tlsHostname")).toString();

        if (options.host.size() > 255 || options.exportName.size() > 1024
            || hasControlCharacters(options.host)
            || hasControlCharacters(options.exportName)
            || hasControlCharacters(options.tlsHostname)) {
            return rejected(QStringLiteral("Invalid connection options."));
        }
        const QStringList tlsFiles = {options.certFile, options.keyFile,
                                      options.caFile};
        for (const QString &file : tlsFiles) {
            if (!file.isEmpty()
                && (!QDir::isAbsolutePath(file) || file.size() > 4096
                    || hasControlCharacters(file))) {
                return rejected(QStringLiteral("Invalid TLS file path."));
            }
        }

        QString validationError;
        if (!NbdCommandBuilder::validate(options, backend, &validationError)) {
            return rejected(validationError);
        }

        const ExecResult result = runProgram(
            program, NbdCommandBuilder::connectArguments(options, backend));
        return response(result.success, result.exitCode,
                        result.success
                            ? QStringLiteral("Connected %1.").arg(device)
                            : result.output,
                        result.output);
    }

    if (operation == QStringLiteral("unlock")) {
        const QString volume = args.value(QStringLiteral("volume")).toString();
        if (!isValidVolume(volume)) {
            return rejected(QStringLiteral("Invalid LUKS volume."));
        }
        const QString passphrase =
            args.value(QStringLiteral("passphrase")).toString();
        if (passphrase.isEmpty() || passphrase.size() > 64 * 1024) {
            return rejected(QStringLiteral("Invalid passphrase."));
        }

        const QString mapperName = mapperNameForVolume(volume);
        if (mapperName.isEmpty()) {
            return rejected(QStringLiteral("Could not determine the mapper name."));
        }
        QStringList arguments;
        if (args.value(QStringLiteral("readOnly")).toBool()) {
            arguments.append(QStringLiteral("--readonly"));
        }
        arguments.append({QStringLiteral("--key-file=-"),
                          QStringLiteral("luksOpen"), volume, mapperName});

        QByteArray secret = passphrase.toUtf8();
        const ExecResult result = runProgram(
            findProgram(QStringLiteral("cryptsetup")), arguments, secret);
        secret.fill('\0');
        return response(result.success, result.exitCode,
                        result.success
                            ? QStringLiteral("Unlocked %1.").arg(volume)
                            : result.output,
                        result.output);
    }

    if (operation == QStringLiteral("lock")) {
        const QString volume = args.value(QStringLiteral("volume")).toString();
        if (!isValidVolume(volume)) {
            return rejected(QStringLiteral("Invalid LUKS volume."));
        }
        const QString mapperName = mapperNameForVolume(volume);
        if (mapperName.isEmpty()) {
            return rejected(QStringLiteral("Could not determine the mapper name."));
        }
        const ExecResult result = runProgram(
            findProgram(QStringLiteral("cryptsetup")),
            {QStringLiteral("close"), mapperName});
        return response(result.success, result.exitCode,
                        result.success
                            ? QStringLiteral("Locked %1.").arg(volume)
                            : result.output,
                        result.output);
    }

    if (operation == QStringLiteral("mount")) {
        const QString source = args.value(QStringLiteral("source")).toString();
        const QString target = args.value(QStringLiteral("target")).toString();
        const QString fileSystem =
            args.value(QStringLiteral("fileSystem")).toString();
        if (!isValidMountSource(source)) {
            return rejected(QStringLiteral("Invalid mount source."));
        }
        if (!isAllowedMountTarget(target, callerUid)) {
            return rejected(QStringLiteral(
                "Mount points managed by the helper must be under /mnt, /media, "
                "/run/media, or your home directory."));
        }
        if (!isValidFileSystem(fileSystem)) {
            return rejected(QStringLiteral("Invalid file system type."));
        }

        if (!QFileInfo(target).isDir()) {
            const ExecResult mkdirResult = runProgram(
                findProgram(QStringLiteral("mkdir")),
                {QStringLiteral("-p"), target});
            if (!mkdirResult.success) {
                return response(false, mkdirResult.exitCode,
                                mkdirResult.output, mkdirResult.output);
            }
        }

        // -n keeps mount from touching /etc/mtab, which the hardened service
        // unit mounts read-only.
        QStringList arguments = {QStringLiteral("-n")};
        if (!fileSystem.isEmpty()) {
            arguments.append({QStringLiteral("-t"), fileSystem});
        }
        if (args.value(QStringLiteral("readOnly")).toBool()) {
            arguments.append({QStringLiteral("-o"), QStringLiteral("ro")});
        }
        arguments.append({source, target});

        const ExecResult result = runProgram(
            findProgram(QStringLiteral("mount")), arguments);
        return response(result.success, result.exitCode,
                        result.success
                            ? QStringLiteral("Mounted %1 at %2.").arg(source, target)
                            : result.output,
                        result.output);
    }

    return rejected(QStringLiteral("Unknown helper operation."));
}
