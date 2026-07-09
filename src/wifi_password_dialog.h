#ifndef WIFI_PASSWORD_DIALOG_H
#define WIFI_PASSWORD_DIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QObject>

class QLabel;
class QPushButton;
class QDialogButtonBox;

class WifiPasswordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit WifiPasswordDialog(const QString &ssid, bool secure, QWidget *parent = nullptr);
    ~WifiPasswordDialog();

    QString password() const;
    bool connectAnyway() const;
    void setInfoText(const QString &text);

private:
    void onPasswordChanged();
    void onConnectAnyway();

private:
    QString mSsid;
    bool mSecure;
    
    QLabel *mTitleLabel;
    QLabel *mSsidLabel;
    QLabel *mInfoLabel;
    QLabel *mValidationLabel;
    QLineEdit *mPasswordEdit;
    QPushButton *mConnectAnywayButton;
    QDialogButtonBox *mButtonBox;
    QPushButton *mConnectButton;
};

#endif // WIFI_PASSWORD_DIALOG_H
