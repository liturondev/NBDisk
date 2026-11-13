/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

// Names shared by the background service and its GUI client.
namespace ServiceBus
{
inline QString serviceName()
{
    return QStringLiteral("io.github.liturondev.NBDisk.Service");
}

inline QString objectPath()
{
    return QStringLiteral("/io/github/liturondev/NBDisk/Service");
}

inline QString interfaceName()
{
    return QStringLiteral("io.github.liturondev.NBDisk.Service");
}
}
