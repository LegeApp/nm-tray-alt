#ifndef TROUBLESHOOTDIALOG_H
#define TROUBLESHOOTDIALOG_H

#include <QDialog>
#include <QList>
#include <QString>

class QLabel;
class QPushButton;
class QTreeWidget;
class QVBoxLayout;
class NmModel;

struct DiagResult
{
    enum class Verdict
    {
        Pending,
        Running,
        Pass,
        Fail,
        Skipped
    };

    QString title;
    QString detail;
    Verdict verdict = Verdict::Pending;
};

struct WifiCandidate
{
    QString ssid;
    QString connectionPath;
    QString devicePath;
    QString apPath;
    int strength = 0;
    bool passwordOnDisk = false;
};

class NetDiagnostics : public QObject
{
    Q_OBJECT

public:
    enum Stage
    {
        StNmState,
        StLink,
        StIpConfig,
        StRoute,
        StDns,
        StHttpPortal,
        StNmVerdict,
        StAlternatives,
        StageCount
    };

    explicit NetDiagnostics(NmModel *model, QObject *parent = nullptr);

    const QList<DiagResult> &results() const { return mResults; }
    const QList<WifiCandidate> &candidates() const { return mCandidates; }
    QString verdictText() const { return mVerdict; }
    QString suggestedActionId() const { return mAction; }

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void stageUpdated(int stage);
    void finished();

private:
    void set(Stage stage, DiagResult::Verdict verdict, const QString &detail = {});
    void finish();

    NmModel *mModel;
    QList<DiagResult> mResults;
    QList<WifiCandidate> mCandidates;
    QString mVerdict;
    QString mAction;
};

class TroubleshootDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TroubleshootDialog(NmModel *model, QWidget *parent = nullptr);

private Q_SLOTS:
    void paintStage(int stage);
    void paintVerdict();

private:
    NetDiagnostics *mDiag;
    NmModel *mModel;
    QTreeWidget *mList;
    QLabel *mVerdict;
    QPushButton *mActionBtn;
    QVBoxLayout *mAltBox;
};

#endif
