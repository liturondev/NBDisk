/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "helperserver.h"
#include "helperprotocol.h"

#include <QCoreApplication>
#include <QDebug>

#include <cstdio>
#include <unistd.h>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nbdisk-helper"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("io.github.liturondev.NBDisk"));

    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.contains(QStringLiteral("--help"))
        || arguments.contains(QStringLiteral("-h"))) {
        std::printf("Usage: nbdisk-helper\n"
                    "\n"
                    "Serves validated privileged disk operations to NBDisk on\n"
                    "%s. Normally started by systemd as root.\n",
                    qPrintable(HelperProtocol::socketPath()));
        return 0;
    }

    if (geteuid() != 0) {
        qWarning() << QStringLiteral(
            "nbdisk-helper is not running as root; operations will fail.");
    }

    HelperServer server;
    QString error;
    if (!server.listen(&error)) {
        qCritical() << QStringLiteral("Could not start the NBDisk helper:")
                    << error;
        return 1;
    }

    qInfo() << QStringLiteral("NBDisk helper listening on")
            << HelperProtocol::socketPath();
    return application.exec();
}
