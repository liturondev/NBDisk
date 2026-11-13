/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nbddevice.h"

#include <QAbstractTableModel>

class NbdDeviceModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        DeviceColumn = 0,
        StatusColumn,
        AutoConnectColumn,
        SizeColumn,
        ServerColumn,
        MountPointColumn,
        ColumnCount,
    };

    explicit NbdDeviceModel(QObject *parent = nullptr);

    void setDevices(const QList<NbdDevice> &devices);
    const QList<NbdDevice> &devices() const;
    const NbdDevice *deviceAt(int row) const;
    int rowForPath(const QString &path) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    QList<NbdDevice> m_devices;
};
