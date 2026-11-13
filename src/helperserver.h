/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <sys/types.h>

class QLocalServer;
class QLocalSocket;

// Root-side helper. It listens on a Unix socket and accepts a small, fixed
// set of high-level operations. Every path and option is validated here before
// a program is started; arbitrary programs or shell command lines are never
// accepted.
class HelperServer : public QObject
{
    Q_OBJECT

public:
    explicit HelperServer(QObject *parent = nullptr);

    bool listen(QString *error);

private:
    void handleNewConnection();
    void handleConnection(QLocalSocket *socket);

    QJsonObject handleRequest(const QJsonObject &request, uid_t peerUid,
                              gid_t peerGid);

    QLocalServer *m_server = nullptr;
};
