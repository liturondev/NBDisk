/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

enum class NbdBackend {
    Auto,
    NbdClient,
    QemuNbd,
};

struct NbdMount {
    QString path;
    QString mountPoint;
    QString fileSystem;
};

struct NbdVolume {
    QString name;
    QString path;
    qint64 size = 0;
    QString fileSystem;
    QStringList mountPoints;
    bool encrypted = false;
    bool unlocked = false;
    QString mapperPath;

    bool isMounted() const
    {
        return !mountPoints.isEmpty();
    }

    bool isLockedLuks() const
    {
        return encrypted && !unlocked;
    }

    QString mountPath() const
    {
        return unlocked && !mapperPath.isEmpty() ? mapperPath : path;
    }

    QString mountFileSystem() const
    {
        return encrypted ? QString() : fileSystem;
    }
};

struct NbdConnectionInfo {
    QString host;
    quint16 port = 10809;
    QString exportName;
    NbdBackend backend = NbdBackend::Auto;
    bool readOnly = false;
    bool tls = false;
    QDateTime since;

    bool isValid() const
    {
        return !host.isEmpty();
    }
};

struct NbdDevice {
    int index = -1;
    QString name;
    QString path;
    bool connected = false;
    bool readOnly = false;
    qint64 size = 0;
    qint64 pid = 0;
    bool hasProfile = false;
    bool autoConnect = false;
    QList<NbdVolume> volumes;
    NbdConnectionInfo connection;

    bool isExternallyConnected() const
    {
        return connected && !connection.isValid();
    }

    bool hasMountedVolumes() const
    {
        for (const NbdVolume &volume : volumes) {
            if (volume.isMounted()) {
                return true;
            }
        }
        return false;
    }
};
