// 层：视图
#pragma once
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QVBoxLayout;

// A form and status projection only. Host services own verification, policy,
// installation, persistence and tasks; readiness never grants checkbox consent.
class PythonEnvironmentDialog : public QDialog {
  Q_OBJECT
public:
  explicit PythonEnvironmentDialog(QWidget *parent = nullptr);
  void setConfiguration(const QJsonObject &configuration, const QJsonArray &history = {});
  void setDiagnostics(const QJsonObject &status);
  void setBusy(bool busy);
  void showMessage(const QString &message);
signals:
  void installRequested(const QString &package, const QString &anchor, const QString &store);
  void activationRequested(const QJsonObject &configuration);
  void probeRequested();
  void disableRequested();
  void rollbackRequested(const QString &identityId);
  void exportRequested(const QString &path);
  void cancelRequested();
protected:
  void reject() override;
  void closeEvent(QCloseEvent *event) override;
private:
  void buildStatus(QVBoxLayout *root);
  void buildInstall(QVBoxLayout *root);
  void buildConfiguration(QVBoxLayout *root);
  void buildActions(QVBoxLayout *root);
  void requestClose();
  QJsonObject form() const;
  QLabel *m_installed = nullptr, *m_integrity = nullptr, *m_trustedStatus = nullptr, *m_generatedStatus = nullptr;
  QLabel *m_source = nullptr, *m_message = nullptr;
  QLineEdit *m_package = nullptr, *m_packageAnchor = nullptr, *m_store = nullptr;
  QLineEdit *m_runtimeRoot = nullptr, *m_runtimeAnchor = nullptr, *m_scriptsRoot = nullptr, *m_scriptsAnchor = nullptr;
  QCheckBox *m_trusted = nullptr, *m_generated = nullptr;
  QComboBox *m_history = nullptr;
  QPushButton *m_install = nullptr, *m_activate = nullptr, *m_probe = nullptr, *m_disable = nullptr;
  QPushButton *m_rollback = nullptr, *m_export = nullptr;
  QGroupBox *m_installForm = nullptr, *m_configurationForm = nullptr;
  QProgressBar *m_progress = nullptr;
  bool m_busy = false, m_closeWhenIdle = false;
};
