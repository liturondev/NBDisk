/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nbddevicemodel.h"

#include <KLocalizedString>

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QLocale>
#include <QPalette>

namespace
{
QString formatSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("—");
    }
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString formatEndpoint(const NbdConnectionInfo &connection)
{
    if (connection.host.contains(QLatin1Char(':'))
        && !connection.host.startsWith(QLatin1Char('['))) {
        return QStringLiteral("[%1]:%2").arg(connection.host).arg(connection.port);
    }
    return QStringLiteral("%1:%2").arg(connection.host).arg(connection.port);
}

QStringList uniqueFileSystems(const NbdDevice &device)
{
    QStringList result;
    for (const NbdVolume &volume : device.volumes) {
        if (!volume.fileSystem.isEmpty() && !result.contains(volume.fileSystem)) {
            result.append(volume.fileSystem);
        }
    }
    return result;
}

QStringList mountPoints(const NbdDevice &device)
{
    QStringList result;
    for (const NbdVolume &volume : device.volumes) {
        for (const QString &mountPoint : volume.mountPoints) {
            if (!result.contains(mountPoint)) {
                result.append(mountPoint);
            }
        }
    }
    return result;
}
}

NbdDeviceModel::NbdDeviceModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void NbdDeviceModel::setDevices(const QList<NbdDevice> &devices)
{
    beginResetModel();
    m_devices = devices;
    endResetModel();
}

const QList<NbdDevice> &NbdDeviceModel::devices() const
{
    return m_devices;
}

const NbdDevice *NbdDeviceModel::deviceAt(int row) const
{
    if (row < 0 || row >= m_devices.size()) {
        return nullptr;
    }
    return &m_devices.at(row);
}

int NbdDeviceModel::rowForPath(const QString &path) const
{
    for (int row = 0; row < m_devices.size(); ++row) {
        if (m_devices.at(row).path == path) {
            return row;
        }
    }
    return -1;
}

int NbdDeviceModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_devices.size());
}

int NbdDeviceModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant NbdDeviceModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_devices.size()) {
        return {};
    }

    const NbdDevice &device = m_devices.at(index.row());
    const QStringList fileSystems = uniqueFileSystems(device);
    const QStringList mounts = mountPoints(device);

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case DeviceColumn:
            return device.path;
        case StatusColumn:
            if (!device.connected) {
                return i18n("Not connected");
            }
            return device.readOnly ? i18n("Connected · read-only") : i18n("Connected");
        case AutoConnectColumn:
            if (!device.hasProfile) {
                return QStringLiteral("—");
            }
            return device.autoConnect ? i18n("Auto-connect") : i18n("Manual");
        case SizeColumn:
            return formatSize(device.size);
        case ServerColumn:
            if (!device.connected) {
                return QStringLiteral("—");
            }
            if (device.isExternallyConnected()) {
                return i18n("External connection");
            }
            return formatEndpoint(device.connection);
        case MountPointColumn:
            return mounts.isEmpty() ? QStringLiteral("—") : mounts.join(QStringLiteral(", "));
        default:
            break;
        }
    }

    if (role == Qt::DecorationRole) {
        if (index.column() == DeviceColumn) {
            return QIcon::fromTheme(QStringLiteral("drive-harddisk"));
        }
        if (index.column() == StatusColumn) {
            return QIcon::fromTheme(device.connected ? QStringLiteral("network-connect")
                                                     : QStringLiteral("network-disconnect"));
        }
        if (index.column() == AutoConnectColumn && device.autoConnect) {
            return QIcon::fromTheme(QStringLiteral("dialog-ok"));
        }
    }

    if (role == Qt::FontRole && index.column() == DeviceColumn) {
        QFont font = QApplication::font();
        font.setBold(true);
        return font;
    }

    if (role == Qt::ForegroundRole && !device.connected) {
        return QApplication::palette().brush(QPalette::Disabled, QPalette::Text);
    }

    if (role == Qt::TextAlignmentRole && index.column() == SizeColumn) {
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }

    if (role == Qt::ToolTipRole) {
        QString tooltip = QStringLiteral("<b>%1</b><br>").arg(device.path.toHtmlEscaped());
        if (!device.connected) {
            tooltip += i18n("Not connected");
            return tooltip;
        }
        tooltip += i18n("Size: %1<br>", formatSize(device.size));
        if (device.isExternallyConnected()) {
            tooltip += i18n("Connected outside NBDisk.");
        } else {
            tooltip += i18n("Server: %1<br>", formatEndpoint(device.connection).toHtmlEscaped());
            if (!device.connection.exportName.isEmpty()) {
                tooltip += i18n("Export: %1<br>", device.connection.exportName.toHtmlEscaped());
            }
        }
        if (device.hasProfile) {
            tooltip += device.autoConnect ? i18n("Auto-connect profile enabled<br>")
                                          : i18n("Connection profile saved<br>");
        }
        if (!fileSystems.isEmpty()) {
            tooltip += i18n("File system: %1<br>", fileSystems.join(QStringLiteral(", ")).toHtmlEscaped());
        }
        if (!mounts.isEmpty()) {
            tooltip += i18n("Mounted at: %1", mounts.join(QStringLiteral(", ")).toHtmlEscaped());
        }
        return tooltip;
    }

    if (role == Qt::UserRole) {
        return device.path;
    }

    return {};
}

QVariant NbdDeviceModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole) {
        return {};
    }

    if (orientation == Qt::Vertical) {
        return section + 1;
    }

    switch (section) {
    case DeviceColumn:
        return i18n("Device");
    case StatusColumn:
        return i18n("Status");
    case AutoConnectColumn:
        return i18n("Startup");
    case SizeColumn:
        return i18n("Size");
    case ServerColumn:
        return i18n("Server");
    case MountPointColumn:
        return i18n("Mounted at");
    default:
        break;
    }
    return {};
}
