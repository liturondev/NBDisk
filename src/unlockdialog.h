/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;

// Asks for a LUKS passphrase and whether it should be remembered in the
// wallet for auto-unlock.
class UnlockDialog : public QDialog
{
    Q_OBJECT

public:
    UnlockDialog(const QString &volumePath, bool rememberByDefault,
                 bool allowRemember = true, QWidget *parent = nullptr);

    QString passphrase() const;
    bool remember() const;

private:
    QLineEdit *m_passphraseEdit = nullptr;
    QCheckBox *m_rememberCheck = nullptr;
};
