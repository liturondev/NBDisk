/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unlockdialog.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

UnlockDialog::UnlockDialog(const QString &volumePath, bool rememberByDefault,
                           bool allowRemember, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18n("Unlock LUKS Volume"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto *label = new QLabel(i18n("Enter the passphrase for %1:", volumePath), this);
    label->setWordWrap(true);
    layout->addWidget(label);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_passphraseEdit = new QLineEdit(this);
    m_passphraseEdit->setEchoMode(QLineEdit::Password);
    m_passphraseEdit->setClearButtonEnabled(true);
    form->addRow(i18n("Passphrase:"), m_passphraseEdit);

    m_rememberCheck = new QCheckBox(
        i18n("Remember this passphrase for automatic unlock"), this);
    m_rememberCheck->setChecked(allowRemember && rememberByDefault);
    m_rememberCheck->setEnabled(allowRemember);
    m_rememberCheck->setToolTip(
        allowRemember
            ? i18n("The passphrase is stored in the KDE Wallet and used when this device "
                   "is reconnected automatically.")
            : i18n("Save a profile for this device first to remember its passphrase."));
    form->addRow(QString(), m_rememberCheck);
    layout->addLayout(form);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18n("Unlock"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_passphraseEdit, &QLineEdit::textChanged, this, [this, buttons]() {
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled(!m_passphraseEdit->text().isEmpty());
    });
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);

    m_passphraseEdit->setFocus();
}

QString UnlockDialog::passphrase() const
{
    return m_passphraseEdit->text();
}

bool UnlockDialog::remember() const
{
    return m_rememberCheck->isChecked();
}
