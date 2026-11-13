/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QVariantMap>

// Talks to the nbdisk-daemon D-Bus service on the session bus.
class BackgroundServiceClient
{
public:
    bool isAvailable() const;
    QVariantMap status() const;
    bool runStartup() const;
    bool startService() const;

    static QString daemonExecutablePath();
};
