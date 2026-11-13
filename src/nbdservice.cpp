/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nbdservice.h"

#include "helperclient.h"
#include "helperprotocol.h"
#include "secretstore.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <unistd.h>

namespace
{
QString readSysfs(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromLatin1(file.readAll()).trimmed();
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

QString mapperPathForVolume(const QString &path)
{
    const QString name = mapperNameForVolume(path);
    return name.isEmpty() ? QString() : QStringLiteral("/dev/mapper/%1").arg(name);
}

bool isSupportedVolumePath(const QString &path)
{
    return path.startsWith(QStringLiteral("/dev/nbd"))
        || path.startsWith(QStringLiteral("/dev/mapper/nbdisk-"));
}

void collectFileSystems(const QJsonArray &items, QHash<QString, QString> *result)
{
    for (const QJsonValue &item : items) {
        const QJsonObject object = item.toObject();
        const QString name = object.value(QStringLiteral("name")).toString();
        const QString fileSystem = object.value(QStringLiteral("fstype")).toString();
        if (!name.isEmpty() && !fileSystem.isEmpty()) {
            result->insert(name, fileSystem);
        }
        collectFileSystems(object.value(QStringLiteral("children")).toArray(), result);
    }
}

void appendMounts(QStringList *mountPoints, QString *fileSystem,
                  const QList<NbdMount> &mounts)
{
    for (const NbdMount &mount : mounts) {
        if (!mount.mountPoint.isEmpty() && !mountPoints->contains(mount.mountPoint)) {
            mountPoints->append(mount.mountPoint);
        }
        if (fileSystem->isEmpty() && !mount.fileSystem.isEmpty()) {
            *fileSystem = mount.fileSystem;
        }
    }
}

QString decodeMountField(QString value)
{
    value.replace(QStringLiteral("\\040"), QStringLiteral(" "));
    value.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
    value.replace(QStringLiteral("\\012"), QStringLiteral("\n"));
    value.replace(QStringLiteral("\\134"), QStringLiteral("\\"));
    return value;
}

QHash<QString, QList<NbdMount>> readMounts()
{
    QHash<QString, QList<NbdMount>> mounts;
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return mounts;
    }

    while (!file.atEnd()) {
        const QString line = QString::fromLocal8Bit(file.readLine()).trimmed();
        const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const int separator = fields.indexOf(QStringLiteral("-"));
        if (separator < 0 || fields.size() <= separator + 2 || fields.size() < 5) {
            continue;
        }

        const QString source = decodeMountField(fields.at(separator + 2));
        if (!source.startsWith(QStringLiteral("/dev/nbd"))
            && !source.startsWith(QStringLiteral("/dev/mapper/nbdisk-"))
            && !source.startsWith(QStringLiteral("/dev/dm-"))) {
            continue;
        }

        NbdMount mount;
        mount.path = source;
        mount.mountPoint = decodeMountField(fields.at(4));
        mount.fileSystem = fields.at(separator + 1);
        mounts[source].append(mount);
    }
    return mounts;
}

QString conciseOutput(const QString &output)
{
    QString text = output.trimmed();
    constexpr int maximumLength = 4000;
    if (text.size() > maximumLength) {
        text = text.left(maximumLength) + QStringLiteral("…");
    }
    return text;
}

QString backendEndpoint(const ConnectOptions &options)
{
    return QStringLiteral("%1:%2").arg(options.host).arg(options.port);
}

QJsonObject profileToJson(const NbdProfile &profile)
{
    const ConnectOptions &options = profile.options;
    QJsonObject value;
    value.insert(QStringLiteral("host"), options.host);
    value.insert(QStringLiteral("port"), options.port);
    value.insert(QStringLiteral("export"), options.exportName);
    value.insert(QStringLiteral("backend"), static_cast<int>(options.backend));
    value.insert(QStringLiteral("readOnly"), options.readOnly);
    value.insert(QStringLiteral("blockSize"), options.blockSize);
    value.insert(QStringLiteral("timeout"), options.timeoutSeconds);
    value.insert(QStringLiteral("persistent"), options.persistent);
    value.insert(QStringLiteral("tls"), options.tls);
    value.insert(QStringLiteral("certFile"), options.certFile);
    value.insert(QStringLiteral("keyFile"), options.keyFile);
    value.insert(QStringLiteral("caFile"), options.caFile);
    value.insert(QStringLiteral("tlsHostname"), options.tlsHostname);
    value.insert(QStringLiteral("autoConnect"), profile.autoConnect);
    value.insert(QStringLiteral("autoMount"), profile.autoMount);
    QJsonObject mountPoints;
    for (auto it = profile.mountPoints.constBegin(); it != profile.mountPoints.constEnd(); ++it) {
        mountPoints.insert(it.key(), it.value());
    }
    value.insert(QStringLiteral("mountPoints"), mountPoints);
    QJsonArray rememberedVolumes;
    for (const QString &volume : profile.rememberedVolumes) {
        rememberedVolumes.append(volume);
    }
    value.insert(QStringLiteral("rememberedVolumes"), rememberedVolumes);
    value.insert(QStringLiteral("lastConnected"),
                 profile.lastConnected.toString(Qt::ISODateWithMs));
    return value;
}

NbdProfile profileFromJson(const QString &device, const QJsonObject &value)
{
    NbdProfile profile;
    ConnectOptions &options = profile.options;
    options.device = device;
    options.host = value.value(QStringLiteral("host")).toString();
    options.port = static_cast<quint16>(
        qBound(1, value.value(QStringLiteral("port")).toInt(10809), 65535));
    options.exportName = value.value(QStringLiteral("export")).toString();
    const int backend = value.value(QStringLiteral("backend")).toInt();
    options.backend = backend == static_cast<int>(NbdBackend::NbdClient)
            || backend == static_cast<int>(NbdBackend::QemuNbd)
        ? static_cast<NbdBackend>(backend)
        : NbdBackend::Auto;
    options.readOnly = value.value(QStringLiteral("readOnly")).toBool();
    const int blockSize = value.value(QStringLiteral("blockSize")).toInt(512);
    options.blockSize = blockSize == 512 || blockSize == 1024 || blockSize == 2048
            || blockSize == 4096
        ? blockSize
        : 512;
    options.timeoutSeconds =
        qBound(0, value.value(QStringLiteral("timeout")).toInt(), 3600);
    options.persistent = value.value(QStringLiteral("persistent")).toBool();
    options.tls = value.value(QStringLiteral("tls")).toBool();
    options.certFile = value.value(QStringLiteral("certFile")).toString();
    options.keyFile = value.value(QStringLiteral("keyFile")).toString();
    options.caFile = value.value(QStringLiteral("caFile")).toString();
    options.tlsHostname = value.value(QStringLiteral("tlsHostname")).toString();
    profile.autoConnect = value.value(QStringLiteral("autoConnect")).toBool();
    profile.autoMount = value.value(QStringLiteral("autoMount")).toBool();
    const QJsonObject mountPoints = value.value(QStringLiteral("mountPoints")).toObject();
    for (auto it = mountPoints.constBegin(); it != mountPoints.constEnd(); ++it) {
        const QString mountPoint = it.value().toString().trimmed();
        if (!mountPoint.isEmpty()) {
            profile.mountPoints.insert(it.key(), mountPoint);
        }
    }
    const QJsonArray rememberedVolumes =
        value.value(QStringLiteral("rememberedVolumes")).toArray();
    for (const QJsonValue &volume : rememberedVolumes) {
        const QString path = volume.toString();
        if (!path.isEmpty()) {
            profile.rememberedVolumes.insert(path);
        }
    }
    profile.lastConnected = QDateTime::fromString(
        value.value(QStringLiteral("lastConnected")).toString(), Qt::ISODateWithMs);
    return profile;
}
}

NbdService::NbdService(QObject *parent)
    : QObject(parent)
    , m_nbdClientPath(QStandardPaths::findExecutable(QStringLiteral("nbd-client")))
    , m_qemuNbdPath(QStandardPaths::findExecutable(QStringLiteral("qemu-nbd")))
    , m_pkexecPath(QStandardPaths::findExecutable(QStringLiteral("pkexec")))
    , m_modprobePath(QStandardPaths::findExecutable(QStringLiteral("modprobe")))
    , m_udisksctlPath(QStandardPaths::findExecutable(QStringLiteral("udisksctl")))
    , m_mountPath(QStandardPaths::findExecutable(QStringLiteral("mount")))
    , m_mkdirPath(QStandardPaths::findExecutable(QStringLiteral("mkdir")))
    , m_cryptsetupPath(QStandardPaths::findExecutable(QStringLiteral("cryptsetup")))
    , m_lsblkPath(QStandardPaths::findExecutable(QStringLiteral("lsblk")))
{
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_finishedConnection =
        connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                &NbdService::commandFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            processFailedToStart();
        }
    });

    m_commandTimer = new QTimer(this);
    m_commandTimer->setSingleShot(true);
    connect(m_commandTimer, &QTimer::timeout, this, &NbdService::commandTimedOut);

    m_helperClient = new HelperClient(this);
    connect(m_helperClient, &HelperClient::finished, this,
            &NbdService::helperCommandFinished);

    m_lsblkProcess = new QProcess(this);
    connect(m_lsblkProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &NbdService::handleFileSystemsFinished);
    m_lsblkTimer = new QTimer(this);
    m_lsblkTimer->setSingleShot(true);
    m_lsblkTimer->setInterval(3000);
    connect(m_lsblkTimer, &QTimer::timeout, this, [this]() {
        if (m_lsblkProcess->state() != QProcess::NotRunning) {
            m_lsblkProcess->kill();
        }
    });

    loadState();
}

NbdService::~NbdService()
{
    if (m_lsblkProcess && m_lsblkProcess->state() != QProcess::NotRunning) {
        m_lsblkProcess->kill();
        m_lsblkProcess->waitForFinished(200);
    }
}

QList<NbdDevice> NbdService::scanDevices(QString *error)
{
    if (error) {
        error->clear();
    }

    QDir classDir(QStringLiteral("/sys/class/block"));
    const QStringList entries =
        classDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);

    QRegularExpression devicePattern(QStringLiteral("^nbd(\\d+)$"));
    QList<QPair<int, QString>> deviceNames;
    for (const QString &entry : entries) {
        const QRegularExpressionMatch match = devicePattern.match(entry);
        if (match.hasMatch() && QFileInfo(classDir.filePath(entry)).isDir()) {
            deviceNames.append({match.captured(1).toInt(), entry});
        }
    }
    std::sort(deviceNames.begin(), deviceNames.end(), [](const auto &left, const auto &right) {
        return left.first < right.first;
    });

    if (deviceNames.isEmpty()) {
        if (error) {
            *error = nbdModuleLoaded()
                ? i18n("The nbd kernel module is loaded, but no NBD devices were found.")
                : i18n("The nbd kernel module is not loaded. Load it to create NBD devices.");
        }
        return {};
    }

    refreshFileSystems();
    const QHash<QString, QList<NbdMount>> mounts = readMounts();
    const QHash<QString, QString> fileSystems = m_fileSystems;
    auto fillVolume = [&](NbdVolume &volume) {
        volume.fileSystem = fileSystems.value(volume.name);
        appendMounts(&volume.mountPoints, &volume.fileSystem, mounts.value(volume.path));
        volume.encrypted = volume.fileSystem == QStringLiteral("crypto_LUKS");
        if (!volume.encrypted) {
            return;
        }

        volume.mapperPath = mapperPathForVolume(volume.path);
        volume.unlocked =
            !volume.mapperPath.isEmpty() && QFileInfo::exists(volume.mapperPath);
        if (!volume.unlocked) {
            return;
        }

        appendMounts(&volume.mountPoints, &volume.fileSystem,
                     mounts.value(volume.mapperPath));
        const QString canonicalPath = QFileInfo(volume.mapperPath).canonicalFilePath();
        if (!canonicalPath.isEmpty() && canonicalPath != volume.mapperPath) {
            appendMounts(&volume.mountPoints, &volume.fileSystem,
                         mounts.value(canonicalPath));
        }
    };
    QList<NbdDevice> devices;
    QSet<QString> connectedNames;

    for (const auto &[index, name] : deviceNames) {
        const QString sysfsPath = classDir.filePath(name);
        NbdDevice device;
        device.index = index;
        device.name = name;
        device.path = QStringLiteral("/dev/") + name;
        const auto profile = m_profiles.constFind(device.path);
        device.hasProfile = profile != m_profiles.cend();
        device.autoConnect = device.hasProfile && profile.value().autoConnect;
        device.size = readSysfs(sysfsPath + QStringLiteral("/size")).toLongLong() * 512;
        device.readOnly = readSysfs(sysfsPath + QStringLiteral("/ro")) == QStringLiteral("1");

        const QString pid = readSysfs(sysfsPath + QStringLiteral("/pid"));
        device.pid = pid.toLongLong();
        // Disconnect resets the NBD capacity synchronously but the pid file can
        // linger briefly. Treat a zero-capacity device as disconnected so the
        // row does not remain "Connected" during or after a disconnect.
        device.connected = !pid.isEmpty() && device.pid > 0 && device.size > 0;

        if (device.connected) {
            connectedNames.insert(name);

            const auto saved = m_connections.constFind(name);
            if (saved != m_connections.cend()) {
                device.connection = saved.value();
            }

            if (m_pendingConnection && m_pendingConnection->device == device.path) {
                NbdConnectionInfo info;
                info.host = m_pendingConnection->host;
                info.port = m_pendingConnection->port;
                info.exportName = m_pendingConnection->exportName;
                info.backend = m_pendingConnection->backend;
                info.readOnly = m_pendingConnection->readOnly;
                info.tls = m_pendingConnection->tls;
                info.since = QDateTime::currentDateTime();
                m_connections.insert(name, info);
                device.connection = info;
                m_pendingConnection.reset();
                saveState();
            }

            QRegularExpression partitionPattern(
                QStringLiteral("^%1p(\\d+)$").arg(QRegularExpression::escape(name)));
            QList<QPair<int, QString>> partitionNames;
            for (const QString &entry : entries) {
                const QRegularExpressionMatch match = partitionPattern.match(entry);
                if (match.hasMatch() && QFileInfo(classDir.filePath(entry)).isDir()) {
                    partitionNames.append({match.captured(1).toInt(), entry});
                }
            }
            std::sort(partitionNames.begin(), partitionNames.end(),
                      [](const auto &left, const auto &right) {
                          return left.first < right.first;
                      });

            if (partitionNames.isEmpty()) {
                NbdVolume volume;
                volume.name = name;
                volume.path = device.path;
                volume.size = device.size;
                fillVolume(volume);
                device.volumes.append(volume);
            } else {
                for (const auto &[partitionIndex, partitionName] : partitionNames) {
                    Q_UNUSED(partitionIndex);
                    NbdVolume volume;
                    volume.name = partitionName;
                    volume.path = QStringLiteral("/dev/") + partitionName;
                    volume.size =
                        readSysfs(classDir.filePath(partitionName) + QStringLiteral("/size"))
                            .toLongLong()
                        * 512;
                    fillVolume(volume);
                    device.volumes.append(volume);
                }
            }
        }

        devices.append(device);
    }

    if (!m_busy) {
        bool stateChanged = false;
        for (auto it = m_connections.begin(); it != m_connections.end();) {
            if (!connectedNames.contains(it.key())) {
                it = m_connections.erase(it);
                stateChanged = true;
            } else {
                ++it;
            }
        }
        if (stateChanged) {
            saveState();
        }
    }

    return devices;
}

BackendAvailability NbdService::backends() const
{
    BackendAvailability availability;
    availability.nbdClient = !m_nbdClientPath.isEmpty();
    availability.qemuNbd = !m_qemuNbdPath.isEmpty();
    return availability;
}

NbdBackend NbdService::effectiveBackend(NbdBackend preferred) const
{
    switch (preferred) {
    case NbdBackend::NbdClient:
        return m_nbdClientPath.isEmpty() ? NbdBackend::Auto : NbdBackend::NbdClient;
    case NbdBackend::QemuNbd:
        return m_qemuNbdPath.isEmpty() ? NbdBackend::Auto : NbdBackend::QemuNbd;
    case NbdBackend::Auto:
        break;
    }

    if (!m_nbdClientPath.isEmpty()) {
        return NbdBackend::NbdClient;
    }
    if (!m_qemuNbdPath.isEmpty()) {
        return NbdBackend::QemuNbd;
    }
    return NbdBackend::Auto;
}

bool NbdService::isBusy() const
{
    return m_busy;
}

bool NbdService::isRoot() const
{
    return geteuid() == 0;
}

bool NbdService::nbdModuleLoaded() const
{
    return QFileInfo(QStringLiteral("/sys/module/nbd")).isDir();
}

void NbdService::reloadState()
{
    m_profiles.clear();
    m_connections.clear();
    m_pendingConnection.reset();
    loadState();
}

QList<NbdProfile> NbdService::profiles() const
{
    QList<NbdProfile> result = m_profiles.values();
    std::sort(result.begin(), result.end(), [](const NbdProfile &left, const NbdProfile &right) {
        return left.options.device < right.options.device;
    });
    return result;
}

std::optional<NbdProfile> NbdService::profileForDevice(const QString &device) const
{
    const auto it = m_profiles.constFind(device);
    if (it == m_profiles.cend()) {
        return std::nullopt;
    }
    return it.value();
}

void NbdService::saveProfile(const ConnectOptions &options, bool autoConnect)
{
    NbdProfile profile;
    profile.options = options;
    profile.autoConnect = autoConnect;
    if (!profile.isValid()) {
        return;
    }

    const auto existing = m_profiles.constFind(profile.options.device);
    if (existing != m_profiles.cend()) {
        profile.lastConnected = existing.value().lastConnected;
        profile.autoMount = existing.value().autoMount;
        profile.mountPoints = existing.value().mountPoints;
        profile.rememberedVolumes = existing.value().rememberedVolumes;
    }
    m_profiles.insert(profile.options.device, profile);
    saveState();
}

void NbdService::deleteProfile(const QString &device)
{
    const auto it = m_profiles.constFind(device);
    if (it == m_profiles.cend()) {
        return;
    }

    SecretStore::forgetDevice(device);
    m_profiles.erase(it);
    saveState();
    emit stateChanged();
}

void NbdService::setProfileAutoConnect(const QString &device, bool enabled)
{
    auto it = m_profiles.find(device);
    if (it == m_profiles.end() || it.value().autoConnect == enabled) {
        return;
    }
    it.value().autoConnect = enabled;
    saveState();
    emit stateChanged();
}

void NbdService::setProfileAutoMount(const QString &device, bool enabled)
{
    auto it = m_profiles.find(device);
    if (it == m_profiles.end() || it.value().autoMount == enabled) {
        return;
    }
    it.value().autoMount = enabled;
    saveState();
    emit stateChanged();
}

void NbdService::setProfileMountPoint(const QString &device, const QString &volumePath,
                                      const QString &mountPoint)
{
    auto it = m_profiles.find(device);
    if (it == m_profiles.end()) {
        return;
    }

    const QString cleaned = mountPoint.trimmed().isEmpty()
        ? QString()
        : QDir::cleanPath(mountPoint.trimmed());
    if (cleaned.isEmpty()) {
        it.value().mountPoints.remove(volumePath);
    } else {
        it.value().mountPoints.insert(volumePath, cleaned);
    }
    saveState();
}

void NbdService::setProfileRememberPassphrase(const QString &device,
                                              const QString &volumePath, bool remember)
{
    auto it = m_profiles.find(device);
    if (it == m_profiles.end()) {
        return;
    }

    if (remember) {
        if (it.value().rememberedVolumes.contains(volumePath)) {
            return;
        }
        it.value().rememberedVolumes.insert(volumePath);
    } else {
        if (!it.value().rememberedVolumes.contains(volumePath)) {
            return;
        }
        it.value().rememberedVolumes.remove(volumePath);
        SecretStore::forget(device, volumePath);
    }
    saveState();
    emit stateChanged();
}

bool NbdService::storeVolumePassphrase(const QString &device, const QString &volumePath,
                                       const QString &passphrase)
{
    auto it = m_profiles.find(device);
    if (it == m_profiles.end()) {
        return false;
    }
    if (!SecretStore::store(device, volumePath, passphrase)) {
        return false;
    }

    if (!it.value().rememberedVolumes.contains(volumePath)) {
        it.value().rememberedVolumes.insert(volumePath);
        saveState();
        emit stateChanged();
    }
    return true;
}

QString NbdService::volumePassphrase(const QString &device, const QString &volumePath) const
{
    const auto it = m_profiles.constFind(device);
    if (it == m_profiles.cend() || !it.value().rememberedVolumes.contains(volumePath)) {
        return {};
    }
    return SecretStore::passphrase(device, volumePath);
}

void NbdService::loadNbdModule()
{
    if (m_busy) {
        return;
    }
    if (nbdModuleLoaded()) {
        emit operationFinished(false, i18n("The nbd kernel module is already loaded."));
        return;
    }
    if (m_modprobePath.isEmpty() && !helperAvailable()) {
        emit operationFinished(false,
                               i18n("modprobe is not installed. Install kmod and try again."));
        return;
    }
    if (m_pkexecPath.isEmpty() && !isRoot() && !helperAvailable()) {
        emit operationFinished(
            false,
            i18n("Authorization is unavailable. Install pkexec or run NBDisk as root."));
        return;
    }

    auto operation = std::make_unique<Operation>();
    operation->description = i18n("Loading the nbd kernel module…");
    operation->completedMessage = i18n("The nbd kernel module was loaded.");

    const Command fallback = privilegedCommand(
        m_modprobePath, {QStringLiteral("nbd"), QStringLiteral("max_part=16")},
        operation->description);
    const Command command = helperAvailable()
        ? helperCommand(QStringLiteral("load_module"), QJsonObject(),
                        operation->description)
        : fallback;
    if (!command.isValid()) {
        emit operationFinished(false, i18n("Could not construct the modprobe command."));
        return;
    }
    operation->commands.enqueue(command);
    if (helperAvailable()) {
        operation->fallbackCommands.enqueue(fallback);
    }
    startOperation(std::move(operation));
}

void NbdService::connectDevice(const ConnectOptions &options)
{
    if (m_busy) {
        return;
    }

    const NbdBackend backend = effectiveBackend(options.backend);
    QString error;
    if (!NbdCommandBuilder::validate(options, backend, &error)) {
        emit operationFinished(false, error);
        return;
    }

    const QString program =
        backend == NbdBackend::QemuNbd ? m_qemuNbdPath : m_nbdClientPath;
    if ((program.isEmpty() && !helperAvailable())
        || (m_pkexecPath.isEmpty() && !isRoot() && !helperAvailable())) {
        emit operationFinished(false, i18n("Authorization is unavailable. Install pkexec or run NBDisk as root."));
        return;
    }

    auto operation = std::make_unique<Operation>();
    operation->description =
        i18n("Connecting %1 to %2…", options.device, backendEndpoint(options));
    operation->completedMessage =
        i18n("Connected %1 to %2.", options.device, backendEndpoint(options));

    const Command fallback = privilegedCommand(
        program, NbdCommandBuilder::connectArguments(options, backend), operation->description);
    Command command;
    if (helperAvailable()) {
        QJsonObject payload;
        payload.insert(QStringLiteral("device"), options.device);
        payload.insert(QStringLiteral("backend"), static_cast<int>(backend));
        payload.insert(QStringLiteral("host"), options.host);
        payload.insert(QStringLiteral("port"), options.port);
        payload.insert(QStringLiteral("export"), options.exportName);
        payload.insert(QStringLiteral("readOnly"), options.readOnly);
        payload.insert(QStringLiteral("blockSize"), options.blockSize);
        payload.insert(QStringLiteral("timeout"), options.timeoutSeconds);
        payload.insert(QStringLiteral("persistent"), options.persistent);
        payload.insert(QStringLiteral("tls"), options.tls);
        payload.insert(QStringLiteral("certFile"), options.certFile);
        payload.insert(QStringLiteral("keyFile"), options.keyFile);
        payload.insert(QStringLiteral("caFile"), options.caFile);
        payload.insert(QStringLiteral("tlsHostname"), options.tlsHostname);
        command = helperCommand(QStringLiteral("connect"), payload,
                                operation->description);
    } else {
        command = fallback;
    }
    if (!command.isValid()) {
        emit operationFinished(false, i18n("The selected NBD backend is not available."));
        return;
    }
    operation->commands.enqueue(command);
    if (helperAvailable()) {
        operation->fallbackCommands.enqueue(fallback);
    }

    ConnectOptions recorded = options;
    recorded.backend = backend;
    operation->onSuccess = [this, recorded]() {
        m_pendingConnection = std::make_unique<ConnectOptions>(recorded);
        if (auto it = m_profiles.find(recorded.device); it != m_profiles.end()) {
            it.value().lastConnected = QDateTime::currentDateTime();
            saveState();
        }
    };

    startOperation(std::move(operation));
}

void NbdService::disconnectDevice(const NbdDevice &device, const QStringList &volumesToUnmount)
{
    if (m_busy) {
        return;
    }
    if (!device.path.startsWith(QStringLiteral("/dev/nbd"))) {
        emit operationFinished(false, i18n("The selected device is not a valid NBD device."));
        return;
    }

    NbdBackend backend = device.connection.isValid()
        ? effectiveBackend(device.connection.backend)
        : effectiveBackend(NbdBackend::Auto);
    if (backend == NbdBackend::Auto) {
        backend = effectiveBackend(NbdBackend::Auto);
    }
    const QString program = backend == NbdBackend::QemuNbd ? m_qemuNbdPath : m_nbdClientPath;
    if ((program.isEmpty() && !helperAvailable())
        || (m_pkexecPath.isEmpty() && !isRoot() && !helperAvailable())) {
        emit operationFinished(false, i18n("No usable NBD backend is available."));
        return;
    }

    auto operation = std::make_unique<Operation>();
    operation->description = i18n("Disconnecting %1…", device.path);
    operation->completedMessage = i18n("Disconnected %1.", device.path);

    QStringList unmountPaths = volumesToUnmount;
    unmountPaths.removeDuplicates();
    for (const QString &path : unmountPaths) {
        if (m_udisksctlPath.isEmpty()) {
            emit operationFinished(false, i18n("udisksctl is required to unmount a volume."));
            return;
        }
        const QString mapperPath = mapperPathForVolume(path);
        const QString target = QFileInfo::exists(mapperPath) ? mapperPath : path;
        operation->commands.enqueue(userCommand(
            m_udisksctlPath, {QStringLiteral("unmount"), QStringLiteral("-b"), target},
            i18n("Unmounting %1…", target)));
    }

    for (const NbdVolume &volume : device.volumes) {
        const QString mapperPath = volume.mapperPath.isEmpty()
            ? mapperPathForVolume(volume.path)
            : volume.mapperPath;
        if (!volume.encrypted || mapperPath.isEmpty()
            || !QFileInfo::exists(mapperPath)) {
            continue;
        }
        if (m_cryptsetupPath.isEmpty()) {
            emit operationFinished(
                false, i18n("cryptsetup is required to lock an encrypted volume."));
            return;
        }
        const QString closeDescription = i18n("Locking %1…", volume.path);
        const Command closeFallback = privilegedCommand(
            m_cryptsetupPath, {QStringLiteral("close"), mapperNameForVolume(volume.path)},
            closeDescription);
        Command closeCommand = closeFallback;
        if (helperAvailable()) {
            QJsonObject payload;
            payload.insert(QStringLiteral("volume"), volume.path);
            closeCommand = helperCommand(QStringLiteral("lock"), payload,
                                         closeDescription);
        }
        if (!closeCommand.isValid()) {
            emit operationFinished(false, i18n("Could not construct the cryptsetup command."));
            return;
        }
        operation->commands.enqueue(closeCommand);
        if (helperAvailable()) {
            operation->fallbackCommands.enqueue(closeFallback);
        }
    }

    const Command disconnectFallback = privilegedCommand(
        program, NbdCommandBuilder::disconnectArguments(device.path, backend),
        operation->description);
    Command command = disconnectFallback;
    if (helperAvailable()) {
        QJsonObject payload;
        payload.insert(QStringLiteral("device"), device.path);
        payload.insert(QStringLiteral("backend"), static_cast<int>(backend));
        command = helperCommand(QStringLiteral("disconnect"), payload,
                                operation->description);
    }
    if (!command.isValid()) {
        emit operationFinished(false, i18n("The NBD backend is not available."));
        return;
    }
    operation->commands.enqueue(command);
    if (helperAvailable()) {
        operation->fallbackCommands.enqueue(disconnectFallback);
    }
    operation->onSuccess = [this, name = device.name]() {
        m_connections.remove(name);
        saveState();
    };

    startOperation(std::move(operation));
}

void NbdService::mountVolume(const QString &path, const QString &fileSystem,
                             bool readOnly, const QString &mountPoint)
{
    if (m_busy) {
        return;
    }
    if (!isSupportedVolumePath(path)) {
        emit operationFinished(false, i18n("Select a valid NBD volume to mount."));
        return;
    }

    const QString cleanedMountPoint = mountPoint.trimmed();
    if (cleanedMountPoint.isEmpty()) {
        if (m_udisksctlPath.isEmpty()) {
            emit operationFinished(false, i18n("udisksctl is not installed."));
            return;
        }

        auto operation = std::make_unique<Operation>();
        operation->description = i18n("Mounting %1…", path);
        operation->completedMessage = i18n("Mounted %1.", path);
        operation->useOutput = true;
        operation->commands.enqueue(userCommand(
            m_udisksctlPath, {QStringLiteral("mount"), QStringLiteral("-b"), path},
            operation->description));
        startOperation(std::move(operation));
        return;
    }

    if (!QDir::isAbsolutePath(cleanedMountPoint) || QDir::cleanPath(cleanedMountPoint) == QStringLiteral("/")) {
        emit operationFinished(false,
                               i18n("Choose an absolute mount point other than /."));
        return;
    }
    if (!helperAvailable()
        && (m_mountPath.isEmpty() || m_mkdirPath.isEmpty())) {
        emit operationFinished(false,
                               i18n("mount and mkdir are required for custom mount points."));
        return;
    }
    if (m_pkexecPath.isEmpty() && !isRoot() && !helperAvailable()) {
        emit operationFinished(
            false,
            i18n("Authorization is unavailable. Install pkexec or run NBDisk as root."));
        return;
    }

    auto operation = std::make_unique<Operation>();
    operation->description = i18n("Mounting %1 at %2…", path, cleanedMountPoint);
    operation->completedMessage = i18n("Mounted %1 at %2.", path, cleanedMountPoint);

    QStringList mountArguments;
    if (!fileSystem.isEmpty()) {
        mountArguments.append({QStringLiteral("-t"), fileSystem});
    }
    if (readOnly) {
        mountArguments.append({QStringLiteral("-o"), QStringLiteral("ro")});
    }
    mountArguments.append({path, cleanedMountPoint});

    if (helperAvailable()) {
        QJsonObject payload;
        payload.insert(QStringLiteral("source"), path);
        payload.insert(QStringLiteral("fileSystem"), fileSystem);
        payload.insert(QStringLiteral("target"), cleanedMountPoint);
        payload.insert(QStringLiteral("readOnly"), readOnly);
        operation->commands.enqueue(helperCommand(
            QStringLiteral("mount"), payload, operation->description));

        if (!QFileInfo(cleanedMountPoint).isDir()) {
            const Command mkdirFallback = privilegedCommand(
                m_mkdirPath, {QStringLiteral("-p"), cleanedMountPoint},
                i18n("Creating mount point %1…", cleanedMountPoint));
            if (mkdirFallback.isValid()) {
                operation->fallbackCommands.enqueue(mkdirFallback);
            }
        }
        const Command mountFallback = privilegedCommand(
            m_mountPath, mountArguments, operation->description);
        if (mountFallback.isValid()) {
            operation->fallbackCommands.enqueue(mountFallback);
        }
        startOperation(std::move(operation));
        return;
    }

    if (!QFileInfo(cleanedMountPoint).isDir()) {
        const Command mkdirCommand = privilegedCommand(
            m_mkdirPath, {QStringLiteral("-p"), cleanedMountPoint},
            i18n("Creating mount point %1…", cleanedMountPoint));
        if (mkdirCommand.program.isEmpty()) {
            emit operationFinished(false, i18n("Could not construct the mkdir command."));
            return;
        }
        operation->commands.enqueue(mkdirCommand);
    }

    const Command mountCommand = privilegedCommand(
        m_mountPath, mountArguments, operation->description);
    if (mountCommand.program.isEmpty()) {
        emit operationFinished(false, i18n("Could not construct the mount command."));
        return;
    }
    operation->commands.enqueue(mountCommand);
    startOperation(std::move(operation));
}

void NbdService::unmountVolume(const QString &path)
{
    if (m_busy) {
        return;
    }
    if (!isSupportedVolumePath(path)) {
        emit operationFinished(false, i18n("Select a valid NBD volume to unmount."));
        return;
    }
    if (m_udisksctlPath.isEmpty()) {
        emit operationFinished(false, i18n("udisksctl is not installed."));
        return;
    }

    const QString mapperPath = path.startsWith(QStringLiteral("/dev/nbd"))
        ? mapperPathForVolume(path)
        : QString();
    const QString target = !mapperPath.isEmpty() && QFileInfo::exists(mapperPath)
        ? mapperPath
        : path;

    auto operation = std::make_unique<Operation>();
    operation->description = i18n("Unmounting %1…", target);
    operation->completedMessage = i18n("Unmounted %1.", target);
    operation->useOutput = true;
    operation->commands.enqueue(userCommand(
        m_udisksctlPath, {QStringLiteral("unmount"), QStringLiteral("-b"), target},
        operation->description));
    startOperation(std::move(operation));
}

void NbdService::unlockVolume(const QString &path, const QString &passphrase, bool readOnly)
{
    if (m_busy) {
        return;
    }
    if (!path.startsWith(QStringLiteral("/dev/nbd"))) {
        emit operationFinished(false, i18n("Select a valid NBD volume to unlock."));
        return;
    }
    if (m_cryptsetupPath.isEmpty() && !helperAvailable()) {
        emit operationFinished(false, i18n("cryptsetup is not installed."));
        return;
    }

    const QString mapperName = mapperNameForVolume(path);
    const QString mapperPath = mapperPathForVolume(path);
    if (mapperName.isEmpty() || mapperPath.isEmpty()) {
        emit operationFinished(false, i18n("Could not determine the mapper name."));
        return;
    }
    if (QFileInfo::exists(mapperPath)) {
        emit operationFinished(false, i18n("%1 is already unlocked.", path));
        return;
    }

    const QString description = i18n("Unlocking %1…", path);
    QStringList arguments;
    if (readOnly) {
        arguments.append(QStringLiteral("--readonly"));
    }
    arguments.append({QStringLiteral("--key-file=-"), QStringLiteral("luksOpen"), path,
                      mapperName});
    Command fallback = privilegedCommand(m_cryptsetupPath, arguments, description);
    fallback.stdinData = passphrase.toUtf8();

    Command command = fallback;
    if (helperAvailable()) {
        QJsonObject payload;
        payload.insert(QStringLiteral("volume"), path);
        payload.insert(QStringLiteral("passphrase"), passphrase);
        payload.insert(QStringLiteral("readOnly"), readOnly);
        command = helperCommand(QStringLiteral("unlock"), payload, description);
    }
    if (!command.isValid()) {
        emit operationFinished(false, i18n("Authorization is unavailable. Install pkexec or run NBDisk as root."));
        return;
    }

    auto operation = std::make_unique<Operation>();
    operation->description = description;
    operation->completedMessage = i18n("Unlocked %1.", path);
    operation->commands.enqueue(command);
    if (helperAvailable()) {
        operation->fallbackCommands.enqueue(fallback);
    }
    startOperation(std::move(operation));
}

void NbdService::lockVolume(const QString &path)
{
    if (m_busy) {
        return;
    }
    if (!path.startsWith(QStringLiteral("/dev/nbd"))) {
        emit operationFinished(false, i18n("Select a valid NBD volume to lock."));
        return;
    }
    if (m_cryptsetupPath.isEmpty() && !helperAvailable()) {
        emit operationFinished(false, i18n("cryptsetup is not installed."));
        return;
    }

    const QString mapperPath = mapperPathForVolume(path);
    if (mapperPath.isEmpty() || !QFileInfo::exists(mapperPath)) {
        emit operationFinished(false, i18n("%1 is not unlocked.", path));
        return;
    }

    const QString description = i18n("Locking %1…", path);
    const Command fallback = privilegedCommand(
        m_cryptsetupPath, {QStringLiteral("close"), mapperNameForVolume(path)},
        description);
    Command command = fallback;
    if (helperAvailable()) {
        QJsonObject payload;
        payload.insert(QStringLiteral("volume"), path);
        command = helperCommand(QStringLiteral("lock"), payload, description);
    }
    if (!command.isValid()) {
        emit operationFinished(false, i18n("Could not construct the cryptsetup command."));
        return;
    }

    auto operation = std::make_unique<Operation>();
    operation->description = description;
    operation->completedMessage = i18n("Locked %1.", path);
    operation->commands.enqueue(command);
    if (helperAvailable()) {
        operation->fallbackCommands.enqueue(fallback);
    }
    startOperation(std::move(operation));
}

void NbdService::startOperation(std::unique_ptr<Operation> operation)
{
    if (!operation || operation->commands.isEmpty()) {
        emit operationFinished(false, i18n("The requested operation has no commands."));
        return;
    }

    m_operation = std::move(operation);
    setBusy(true);
    emit operationStarted(m_operation->description);
    startNextCommand();
}

void NbdService::startNextCommand()
{
    if (!m_operation) {
        return;
    }
    if (m_operation->commands.isEmpty()) {
        finishOperation(true, m_operation->completedMessage);
        return;
    }

    Command command = m_operation->commands.dequeue();
    if (!command.isValid()) {
        finishOperation(false, i18n("The required executable is not available."));
        return;
    }

    if (!command.helperOperation.isEmpty()) {
        m_helperActive = true;
        m_commandTimer->start(m_commandTimeoutMs);
        m_helperClient->send(command.helperOperation, command.helperArguments);
        return;
    }
    m_helperActive = false;

    // A command that timed out may still be shutting down. Never start the next
    // one on top of it, and detach its finishing signal so it cannot be
    // mistaken for this command's result.
    disconnect(m_finishedConnection);
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(500);
    }
    m_finishedConnection =
        connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                &NbdService::commandFinished);
    m_commandTimer->start(m_commandTimeoutMs);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    m_process->setProcessEnvironment(environment);
    m_process->setProgram(command.program);
    m_process->setArguments(command.arguments);
    m_process->start();

    if (!command.stdinData.isEmpty()) {
        m_process->write(command.stdinData);
        m_process->closeWriteChannel();
        command.stdinData.fill('\0');
    }
}

void NbdService::commandFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_commandTimer->stop();
    if (!m_operation) {
        return;
    }

    const QString output = QString::fromLocal8Bit(m_process->readAll());
    if (!output.trimmed().isEmpty()) {
        m_operation->output.append(output.trimmed());
    }

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        startNextCommand();
        return;
    }

    QString message = conciseOutput(output);
    if (message.isEmpty()) {
        message = i18n("The command failed with exit code %1.", exitCode);
    }
    if (exitStatus == QProcess::CrashExit) {
        message = i18n("The command terminated unexpectedly. %1", message);
    }
    if (exitCode == 126 || exitCode == 127) {
        message = i18n("Authorization was cancelled or permission was denied.");
    }
    finishOperation(false, message);
}

void NbdService::commandTimedOut()
{
    if (!m_operation) {
        return;
    }

    if (m_helperActive) {
        m_helperActive = false;
        m_helperClient->abort();
        finishOperation(false,
                        i18n("Timed out after %1 seconds: %2",
                             qMax(1, m_commandTimeoutMs / 1000),
                             m_operation->description));
        return;
    }

    disconnect(m_finishedConnection);
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(500);
    }
    finishOperation(false,
                    i18n("Timed out after %1 seconds: %2",
                         qMax(1, m_commandTimeoutMs / 1000), m_operation->description));
}

void NbdService::processFailedToStart()
{
    if (!m_operation) {
        return;
    }
    finishOperation(false, i18n("Could not start %1: %2",
                               QFileInfo(m_process->program()).fileName(),
                               m_process->errorString()));
}

void NbdService::finishOperation(bool success, const QString &message)
{
    m_commandTimer->stop();
    if (m_helperActive) {
        m_helperActive = false;
        m_helperClient->abort();
    }
    std::unique_ptr<Operation> operation = std::move(m_operation);
    setBusy(false);

    if (!operation) {
        return;
    }

    if (success) {
        if (operation->onSuccess) {
            operation->onSuccess();
        }
        emit stateChanged();
    }

    QString finalMessage = message;
    if (success && operation->useOutput && !operation->output.isEmpty()) {
        finalMessage = conciseOutput(operation->output.join(QLatin1Char('\n')));
    }
    emit operationFinished(success, finalMessage);
}

void NbdService::setBusy(bool busy)
{
    m_busy = busy;
}

void NbdService::setCommandTimeoutMs(int milliseconds)
{
    m_commandTimeoutMs = qMax(1, milliseconds);
}

void NbdService::refreshFileSystems()
{
    if (m_lsblkPath.isEmpty() || m_lsblkProcess->state() != QProcess::NotRunning) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_fileSystemsRequestedAt < 2000) {
        return;
    }
    m_fileSystemsRequestedAt = now;

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    m_lsblkProcess->setProcessEnvironment(environment);
    m_lsblkProcess->start(m_lsblkPath, {QStringLiteral("-J"), QStringLiteral("-o"),
                                        QStringLiteral("NAME,FSTYPE,TYPE")});
    m_lsblkTimer->start();
}

void NbdService::handleFileSystemsFinished()
{
    m_lsblkTimer->stop();
    if (m_lsblkProcess->exitStatus() != QProcess::NormalExit
        || m_lsblkProcess->exitCode() != 0) {
        return;
    }

    QHash<QString, QString> result;
    const QJsonDocument document =
        QJsonDocument::fromJson(m_lsblkProcess->readAllStandardOutput());
    if (document.isObject()) {
        collectFileSystems(document.object().value(QStringLiteral("blockdevices")).toArray(),
                           &result);
    }

    if (result != m_fileSystems) {
        m_fileSystems = result;
        // Show the new file-system types now instead of waiting for the next
        // refresh tick.
        emit stateChanged();
    }
}

NbdService::Command NbdService::privilegedCommand(const QString &program,
                                                  const QStringList &arguments,
                                                  const QString &description) const
{
    if (program.isEmpty()) {
        return {};
    }
    if (isRoot()) {
        return {program, arguments, description};
    }
    if (m_pkexecPath.isEmpty()) {
        return {};
    }

    QStringList privilegedArguments;
    privilegedArguments.reserve(arguments.size() + 1);
    privilegedArguments.append(program);
    privilegedArguments.append(arguments);
    return {m_pkexecPath, privilegedArguments, description};
}

NbdService::Command NbdService::helperCommand(const QString &operation,
                                              const QJsonObject &arguments,
                                              const QString &description) const
{
    Command command;
    command.description = description;
    command.helperOperation = operation;
    command.helperArguments = arguments;
    return command;
}

bool NbdService::helperAvailable() const
{
    return !isRoot() && HelperClient::isAvailable();
}

void NbdService::helperCommandFinished(bool success, int exitCode,
                                       const QString &message,
                                       const QString &output)
{
    m_commandTimer->stop();
    m_helperActive = false;
    if (!m_operation) {
        return;
    }

    if (!output.trimmed().isEmpty()) {
        m_operation->output.append(output.trimmed());
    }

    if (success) {
        // One helper command completed; drop its matching fallback.
        if (!m_operation->fallbackCommands.isEmpty()) {
            m_operation->fallbackCommands.dequeue();
        }
        startNextCommand();
        return;
    }

    // The helper is optional. When it is unreachable or the caller is not in
    // its group, retry the remaining steps through pkexec so authorization
    // still works.
    if (exitCode == HelperProtocol::UnavailableExitCode
        && !m_operation->fallbackCommands.isEmpty()) {
        QQueue<Command> fallbacks = m_operation->fallbackCommands;
        m_operation->fallbackCommands.clear();
        m_operation->commands.clear();
        while (!fallbacks.isEmpty()) {
            const Command fallback = fallbacks.dequeue();
            if (fallback.isValid()) {
                m_operation->commands.enqueue(fallback);
            }
        }
        if (!m_operation->commands.isEmpty()) {
            startNextCommand();
            return;
        }
    }

    finishOperation(false,
                    message.isEmpty()
                        ? i18n("The privileged helper could not complete the operation.")
                        : message);
}

NbdService::Command NbdService::userCommand(const QString &program,
                                            const QStringList &arguments,
                                            const QString &description) const
{
    if (program.isEmpty()) {
        return {};
    }
    return {program, arguments, description};
}

QString NbdService::stateFilePath() const
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return directory + QStringLiteral("/connections.json");
}

void NbdService::loadState()
{
    QFile file(stateFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return;
    }

    const QJsonObject root = document.object();
    const QJsonObject connections = root.value(QStringLiteral("connections")).toObject();
    for (auto it = connections.constBegin(); it != connections.constEnd(); ++it) {
        const QJsonObject value = it.value().toObject();
        NbdConnectionInfo info;
        info.host = value.value(QStringLiteral("host")).toString();
        info.port = static_cast<quint16>(
            qBound(1, value.value(QStringLiteral("port")).toInt(10809), 65535));
        info.exportName = value.value(QStringLiteral("export")).toString();
        info.readOnly = value.value(QStringLiteral("readOnly")).toBool();
        info.tls = value.value(QStringLiteral("tls")).toBool();
        info.backend = static_cast<NbdBackend>(value.value(QStringLiteral("backend")).toInt());
        if (info.backend != NbdBackend::NbdClient && info.backend != NbdBackend::QemuNbd) {
            info.backend = NbdBackend::Auto;
        }
        info.since = QDateTime::fromString(value.value(QStringLiteral("since")).toString(),
                                          Qt::ISODateWithMs);
        if (info.isValid()) {
            m_connections.insert(it.key(), info);
        }
    }

    const QJsonObject profiles = root.value(QStringLiteral("profiles")).toObject();
    for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it) {
        const NbdProfile profile = profileFromJson(it.key(), it.value().toObject());
        if (profile.isValid()) {
            m_profiles.insert(it.key(), profile);
        }
    }
}

void NbdService::saveState() const
{
    QJsonObject connections;
    for (auto it = m_connections.constBegin(); it != m_connections.constEnd(); ++it) {
        const NbdConnectionInfo &info = it.value();
        QJsonObject value;
        value.insert(QStringLiteral("host"), info.host);
        value.insert(QStringLiteral("port"), info.port);
        value.insert(QStringLiteral("export"), info.exportName);
        value.insert(QStringLiteral("backend"), static_cast<int>(info.backend));
        value.insert(QStringLiteral("readOnly"), info.readOnly);
        value.insert(QStringLiteral("tls"), info.tls);
        value.insert(QStringLiteral("since"), info.since.toString(Qt::ISODateWithMs));
        connections.insert(it.key(), value);
    }

    QJsonObject profiles;
    for (auto it = m_profiles.constBegin(); it != m_profiles.constEnd(); ++it) {
        profiles.insert(it.key(), profileToJson(it.value()));
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 4);
    root.insert(QStringLiteral("connections"), connections);
    root.insert(QStringLiteral("profiles"), profiles);

    const QFileInfo fileInfo(stateFilePath());
    if (!QDir().mkpath(fileInfo.absolutePath())) {
        return;
    }

    QSaveFile file(stateFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}
