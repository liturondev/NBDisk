/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mainwindow.h"

#include <KAboutData>
#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("nbdisk"));
    QApplication::setApplicationDisplayName(i18n("NBDisk"));
    QApplication::setOrganizationDomain(QStringLiteral("io.github.liturondev.NBDisk"));
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("nbdisk"));

    KAboutData aboutData(QStringLiteral("nbdisk"),
                         i18n("NBDisk"),
                         QStringLiteral(NBDISK_VERSION),
                         i18n("Connect and manage remote NBD disks"),
                         KAboutLicense::GPL_V3,
                         i18n("© 2026 liturondev"),
                         i18n("A Qt/KDE client for network block devices."),
                         QString(),
                         QString());
    aboutData.setDesktopFileName(QStringLiteral("io.github.liturondev.NBDisk"));
    aboutData.setHomepage(QStringLiteral("https://github.com/liturondev/NBDisk"));
    aboutData.setBugAddress(QByteArrayLiteral("https://github.com/liturondev/NBDisk/issues"));
    aboutData.setOtherText(i18n("Documentation: %1",
                                QStringLiteral("https://github.com/liturondev/NBDisk#readme")));
    aboutData.addAuthor(i18n("liturondev"),
                        QStringLiteral("339808872+liturondev@users.noreply.github.com"),
                        QStringLiteral("https://github.com/liturondev"),
                        i18n("Maintainer"));
    KAboutData::setApplicationData(aboutData);

    QApplication::setWindowIcon(QIcon::fromTheme(
        QStringLiteral("io.github.liturondev.NBDisk"),
        QIcon(QStringLiteral(":/icons/io.github.liturondev.NBDisk.svg"))));

    QCommandLineParser parser;
    parser.setApplicationDescription(i18n("Connect and manage remote NBD disks"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(application);

    MainWindow window;
    window.show();

    return application.exec();
}
