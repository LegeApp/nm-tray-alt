#include "wifi_password_dialog.h"
#include "backend/nm_actions.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QDialogButtonBox>

WifiPasswordDialog::WifiPasswordDialog(const QString &ssid, bool secure, QWidget *parent)
    : QDialog(parent)
    , mSsid(ssid)
    , mSecure(secure)
{
    setWindowTitle(tr("Connect to Wi-Fi Network"));
    setModal(true);
    setSizeGripEnabled(false);
    setMinimumWidth(380);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(16, 16, 16, 16);

    mTitleLabel = new QLabel();
    mTitleLabel->setWordWrap(true);
    mTitleLabel->setStyleSheet("font-weight: bold; font-size: 12pt;");
    layout->addWidget(mTitleLabel);

    mSsidLabel = new QLabel();
    mSsidLabel->setWordWrap(true);
    mSsidLabel->setStyleSheet("color: #666; margin: 8px 0;");
    layout->addWidget(mSsidLabel);

    mInfoLabel = new QLabel();
    mInfoLabel->setWordWrap(true);
    mInfoLabel->setStyleSheet("color: #555; background-color: #eef5ff; padding: 8px; border: 1px solid #9dc2f0; border-radius: 4px;");
    mInfoLabel->hide();
    layout->addWidget(mInfoLabel);

    if (mSecure) {
        auto *passwordLabel = new QLabel(tr("Password:"));
        layout->addWidget(passwordLabel);

        mPasswordEdit = new QLineEdit();
        mPasswordEdit->setEchoMode(QLineEdit::Password);
        mPasswordEdit->setPlaceholderText(tr("Enter network password"));
        connect(mPasswordEdit, &QLineEdit::textChanged, this, [this]() { onPasswordChanged(); });
        connect(mPasswordEdit, &QLineEdit::returnPressed, this, [this]() {
            if (mConnectButton != nullptr && mConnectButton->isEnabled()) {
                accept();
            }
        });
        layout->addWidget(mPasswordEdit);

        mValidationLabel = new QLabel(tr("Use 8–63 characters, or exactly 64 hex digits."));
        mValidationLabel->setWordWrap(true);
        mValidationLabel->setStyleSheet("color: #666;");
        layout->addWidget(mValidationLabel);
    } else {
        mPasswordEdit = nullptr;
        mValidationLabel = nullptr;
    }

    if (!mSecure) {
        auto *warningLabel = new QLabel(tr("This is an open network. Nearby devices may be able to inspect unencrypted traffic."));
        warningLabel->setWordWrap(true);
        warningLabel->setStyleSheet("color: #d35400; background-color: #fef9e7; padding: 8px; border: 1px solid #f39c12; border-radius: 4px;");
        layout->addWidget(warningLabel);
    }

    mConnectAnywayButton = nullptr;

    mButtonBox = new QDialogButtonBox(QDialogButtonBox::Cancel);
    mConnectButton = mButtonBox->addButton(tr("Connect"), QDialogButtonBox::AcceptRole);
    mConnectButton->setEnabled(false);
    connect(mButtonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(mButtonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(mButtonBox);

    if (mSecure) {
        mTitleLabel->setText(tr("Connect to secure Wi-Fi network"));
        mSsidLabel->setText(tr("Network: %1").arg(mSsid));
    } else {
        mTitleLabel->setText(tr("Connect to open Wi-Fi network"));
        mSsidLabel->setText(tr("Network: %1").arg(mSsid));
        mConnectButton->setEnabled(true);
    }

    if (mPasswordEdit != nullptr) {
        mPasswordEdit->setFocus(Qt::OtherFocusReason);
    }

    resize(400, sizeHint().height());
}

WifiPasswordDialog::~WifiPasswordDialog() = default;

QString WifiPasswordDialog::password() const
{
    return mPasswordEdit ? mPasswordEdit->text() : QString();
}

void WifiPasswordDialog::setInfoText(const QString &text)
{
    mInfoLabel->setText(text);
    mInfoLabel->setVisible(!text.isEmpty());
}

bool WifiPasswordDialog::connectAnyway() const
{
    return false;
}

void WifiPasswordDialog::onPasswordChanged()
{
    const bool valid = mPasswordEdit && nm::isWpaPskValid(mPasswordEdit->text());
    mConnectButton->setEnabled(valid);
    if (mValidationLabel != nullptr) {
        mValidationLabel->setStyleSheet(valid ? QStringLiteral("color: #2e7d32;") : QStringLiteral("color: #b00020;"));
    }
}

void WifiPasswordDialog::onConnectAnyway()
{
    accept();
}
