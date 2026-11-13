/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QString>

// Shared between the privileged helper and its client.
namespace HelperProtocol
{
inline QString socketPath()
{
    const QByteArray override = qgetenv("NBDISK_HELPER_SOCKET");
    if (!override.isEmpty()) {
        return QString::fromLocal8Bit(override);
    }
    return QStringLiteral("/run/nbdisk/helper.sock");
}

// Exit code used when the helper could not be reached, denied the caller, or
// rejected the request before running a program.
constexpr int UnavailableExitCode = 126;

// The helper only runs programs from this list, and only through the
// high-level operations below. It never accepts arbitrary argv.
constexpr int ProtocolVersion = 1;
}
