// 层：视图
#include "pythonenvironmentdialog.h"
#include "../paleotheme.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <utility>

namespace {
QLineEdit *field(QWidget *parent, const char *name) {
  auto *value = new QLineEdit(parent);
  value->setObjectName(QString::fromLatin1(name));
  return value;
}
QString stateText(const QString &state) {
  if (state == "installed") return QObject::tr("已安装");
  if (state == "verified") return QObject::tr("校验通过");
  if (state == "ready") return QObject::tr("可执行");
  if (state == "missing") return QObject::tr("未安装");
  if (state == "unconfigured") return QObject::tr("尚未选择环境");
  if (state == "disabled") return QObject::tr("未启用");
  if (state == "unavailable") return QObject::tr("当前不可执行");
  if (state == "error") return QObject::tr("检查未通过");
  return QObject::tr("尚未检查");
}
void addDetails(QTreeWidgetItem *parent, const QJsonObject &object) {
  for (auto entry = object.begin(); entry != object.end(); ++entry) {
    auto *item = new QTreeWidgetItem(parent, {entry.key()});
    if (entry.value().isObject()) addDetails(item, entry.value().toObject());
    else if (entry.value().isBool()) item->setText(1, entry.value().toBool() ? QObject::tr("是") : QObject::tr("否"));
    else item->setText(1, entry.value().toVariant().toString());
  }
}
}

PythonEnvironmentDialog::PythonEnvironmentDialog(QWidget *parent) : QDialog(parent) {
  setObjectName(QStringLiteral("pythonEnvironmentDialog"));
  setWindowTitle(tr("Python 计算环境"));
  setFont(PaleoTheme::bodyFont());
  const auto &tokens = PaleoTheme::tokens();
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(tokens.spacingMd, tokens.spacingMd, tokens.spacingMd, tokens.spacingMd);
  root->setSpacing(tokens.spacingSm);
  auto *scroll = new QScrollArea(this); scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *contents = new QWidget(scroll); auto *body = new QVBoxLayout(contents);
  body->setContentsMargins(0, 0, 0, 0); body->setSpacing(tokens.spacingSm);
  buildStatus(body); buildInstall(body); buildConfiguration(body);
  m_details = new QTreeWidget(contents);
  m_details->setObjectName(QStringLiteral("pythonEnvironmentDiagnostics"));
  m_details->setHeaderLabels({tr("诊断项"), tr("实际状态")});
  m_details->setRootIsDecorated(true); m_details->setAlternatingRowColors(false);
  body->addWidget(m_details);
  scroll->setWidget(contents); root->addWidget(scroll, 1);
  buildActions(root);
  m_message = new QLabel(this); m_message->setObjectName(QStringLiteral("pythonEnvironmentMessage"));
  m_message->setWordWrap(true); root->addWidget(m_message);
  m_progress = new QProgressBar(this); m_progress->setRange(0, 0); m_progress->setVisible(false);
  root->addWidget(m_progress);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &PythonEnvironmentDialog::reject);
  root->addWidget(buttons);
  if (const auto *screen = QGuiApplication::primaryScreen()) {
    const auto available = screen->availableGeometry().size() * 0.85;
    resize(sizeHint().boundedTo(available));
  }
}
void PythonEnvironmentDialog::buildStatus(QVBoxLayout *root) {
  auto *status = new QFormLayout;
  status->setSpacing(PaleoTheme::tokens().spacingSm);
  m_installed = new QLabel(tr("尚未检查"), this); m_installed->setObjectName(QStringLiteral("pythonInstalledStatus"));
  m_integrity = new QLabel(tr("尚未检查"), this); m_integrity->setObjectName(QStringLiteral("pythonIntegrityStatus"));
  m_templatesIntegrity = new QLabel(tr("尚未检查"), this);
  m_templatesIntegrity->setObjectName(QStringLiteral("pythonTemplatesIntegrityStatus"));
  m_trustedStatus = new QLabel(tr("未启用"), this); m_trustedStatus->setObjectName(QStringLiteral("pythonTrustedStatus"));
  m_generatedStatus = new QLabel(tr("未启用"), this); m_generatedStatus->setObjectName(QStringLiteral("pythonGeneratedStatus"));
  m_source = new QLabel(this); m_source->setWordWrap(true);
  status->addRow(tr("安装"), m_installed); status->addRow(tr("运行时完整性"), m_integrity);
  status->addRow(tr("可信模板完整性"), m_templatesIntegrity);
  status->addRow(tr("可信分析"), m_trustedStatus); status->addRow(tr("受控生成计算"), m_generatedStatus);
  status->addRow(tr("配置来源"), m_source); root->addLayout(status);
}
void PythonEnvironmentDialog::buildInstall(QVBoxLayout *root) {
  m_installForm = new QGroupBox(tr("安装离线环境"), this);
  auto *layout = new QFormLayout(m_installForm); layout->setSpacing(PaleoTheme::tokens().spacingSm);
  m_package = field(m_installForm, "pythonDeliveryPackage");
  auto *pickPackage = new QPushButton(tr("选择包…"), m_installForm);
  auto *packageRow = new QHBoxLayout; packageRow->addWidget(m_package, 1); packageRow->addWidget(pickPackage);
  connect(pickPackage, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getOpenFileName(this, tr("选择离线环境包"));
    if (!path.isEmpty()) m_package->setText(path);
  });
  m_packageAnchor = field(m_installForm, "pythonDeliveryAnchor");
  m_packageAnchor->setToolTip(tr("使用独立交付记录中的完整 SHA-256 校验码。"));
  m_store = field(m_installForm, "pythonDeliveryStore");
  m_store->setToolTip(tr("使用专用安装目录；运行时和可信模板分别安装。"));
  auto *pickStore = new QPushButton(tr("选择目录…"), m_installForm);
  auto *storeRow = new QHBoxLayout; storeRow->addWidget(m_store, 1); storeRow->addWidget(pickStore);
  connect(pickStore, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("选择专用安装目录"));
    if (!path.isEmpty()) m_store->setText(path);
  });
  m_install = new QPushButton(tr("安装"), m_installForm); m_install->setObjectName(QStringLiteral("pythonDeliveryInstall"));
  connect(m_install, &QPushButton::clicked, this, [this] {
    if (!m_busy) emit installRequested(m_package->text(), m_packageAnchor->text(), m_store->text());
  });
  layout->addRow(tr("离线包"), packageRow); layout->addRow(tr("交付校验码"), m_packageAnchor);
  layout->addRow(tr("专用安装目录"), storeRow); layout->addRow(QString(), m_install);
  auto *note = new QLabel(tr("安装不会启用计算。请选择受信任的运行时包；可信分析还需模板包。"), m_installForm);
  note->setWordWrap(true); layout->addRow(note); root->addWidget(m_installForm);
}
void PythonEnvironmentDialog::buildConfiguration(QVBoxLayout *root) {
  m_configurationForm = new QGroupBox(tr("启用计算"), this);
  auto *layout = new QFormLayout(m_configurationForm); layout->setSpacing(PaleoTheme::tokens().spacingSm);
  m_runtimeRoot = field(m_configurationForm, "pythonRuntimeRoot");
  m_runtimeAnchor = field(m_configurationForm, "pythonRuntimeManifestAnchor");
  m_scriptsRoot = field(m_configurationForm, "pythonScriptsRoot");
  m_scriptsAnchor = field(m_configurationForm, "pythonScriptsManifestAnchor");
  layout->addRow(tr("运行时目录"), m_runtimeRoot); layout->addRow(tr("运行时清单校验码"), m_runtimeAnchor);
  layout->addRow(tr("可信模板目录"), m_scriptsRoot); layout->addRow(tr("模板清单校验码"), m_scriptsAnchor);
  m_trusted = new QCheckBox(tr("允许已登记的可信分析"), m_configurationForm);
  m_trusted->setObjectName(QStringLiteral("pythonAllowTrusted"));
  m_generated = new QCheckBox(tr("允许受控生成计算"), m_configurationForm);
  m_generated->setObjectName(QStringLiteral("pythonAllowGenerated"));
  layout->addRow(m_trusted); layout->addRow(m_generated);
  m_activate = new QPushButton(tr("验证并应用"), m_configurationForm);
  m_activate->setObjectName(QStringLiteral("pythonEnvironmentActivate"));
  connect(m_activate, &QPushButton::clicked, this, [this] { if (!m_busy) emit activationRequested(form()); });
  layout->addRow(QString(), m_activate);
  root->addWidget(m_configurationForm);
}
void PythonEnvironmentDialog::buildActions(QVBoxLayout *root) {
  auto *actions = new QHBoxLayout;
  m_reload = new QPushButton(tr("重新载入配置"), this);
  m_reload->setObjectName(QStringLiteral("pythonEnvironmentReload"));
  m_reload->setToolTip(tr("丢弃表单草稿并载入最新配置；其他窗口修改配置后请先重新载入。"));
  connect(m_reload, &QPushButton::clicked, this, &PythonEnvironmentDialog::reloadRequested);
  m_probe = new QPushButton(tr("检查可执行状态"), this); m_probe->setObjectName(QStringLiteral("pythonEnvironmentProbe"));
  m_disable = new QPushButton(tr("停用计算"), this);
  m_disable->setObjectName(QStringLiteral("pythonEnvironmentDisable"));
  m_disable->setToolTip(tr("立即停用 Python 执行，并保存停用配置。"));
  m_export = new QPushButton(tr("导出诊断…"), this);
  connect(m_probe, &QPushButton::clicked, this, &PythonEnvironmentDialog::probeRequested);
  connect(m_disable, &QPushButton::clicked, this, &PythonEnvironmentDialog::disableRequested);
  connect(m_export, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getSaveFileName(this, tr("导出环境诊断"), QString(), tr("JSON 文件 (*.json)"));
    if (!path.isEmpty()) emit exportRequested(path);
  });
  actions->addWidget(m_reload); actions->addWidget(m_probe); actions->addWidget(m_disable); actions->addWidget(m_export); root->addLayout(actions);
  auto *history = new QHBoxLayout;
  m_history = new QComboBox(this); m_history->setObjectName(QStringLiteral("pythonEnvironmentHistory"));
  m_rollback = new QPushButton(tr("回退到已核验版本"), this);
  m_revoke = new QPushButton(tr("撤销所选版本"), this);
  m_revoke->setObjectName(QStringLiteral("pythonEnvironmentRevoke"));
  connect(m_revoke, &QPushButton::clicked, this, [this] {
    if (!m_busy && m_history->currentIndex() >= 0) emit revokeRequested(m_history->currentData().toString());
  });
  connect(m_rollback, &QPushButton::clicked, this, [this] {
    if (!m_busy && m_history->currentIndex() >= 0) emit rollbackRequested(m_history->currentData().toString());
  });
  history->addWidget(m_history, 1); history->addWidget(m_rollback); history->addWidget(m_revoke); root->addLayout(history);
}
QJsonObject PythonEnvironmentDialog::form() const {
  return {{"runtimeRoot", m_runtimeRoot->text()}, {"runtimeManifestSha256", m_runtimeAnchor->text()},
      {"scriptsRoot", m_scriptsRoot->text()}, {"scriptsManifestSha256", m_scriptsAnchor->text()},
      {"allowTrusted", m_trusted->isChecked()}, {"allowGenerated", m_generated->isChecked()}};
}
void PythonEnvironmentDialog::setConfiguration(const QJsonObject &configuration, const QJsonArray &history) {
  m_runtimeRoot->setText(configuration.value("runtimeRoot").toString());
  m_runtimeAnchor->setText(configuration.value("runtimeManifestSha256").toString());
  m_scriptsRoot->setText(configuration.value("scriptsRoot").toString());
  m_scriptsAnchor->setText(configuration.value("scriptsManifestSha256").toString());
  m_trusted->setChecked(configuration.value("allowTrusted").toBool());
  m_generated->setChecked(configuration.value("allowGenerated").toBool());
  m_history->clear();
  for (const auto &value : history) {
    const auto identity = value.toObject();
    m_history->addItem(identity.value("label").toString(), identity.value("id").toString());
  }
  m_rollback->setEnabled(!m_busy && m_history->count() > 0);
  m_revoke->setEnabled(!m_busy && m_history->count() > 0);
}
void PythonEnvironmentDialog::setInstalledCandidate(const QJsonObject &installed) {
  const auto kind = installed.value("kind").toString();
  const auto path = installed.value("root").toString();
  const auto anchor = installed.value("manifestSha256").toString();
  if (kind == "runtime") { m_runtimeRoot->setText(path); m_runtimeAnchor->setText(anchor); }
  else if (kind == "scripts") { m_scriptsRoot->setText(path); m_scriptsAnchor->setText(anchor); }
  else return;
  showMessage(tr("离线包已安装并填入表单；验证并应用后才会使用所选环境。"));
}
void PythonEnvironmentDialog::setDiagnostics(const QJsonObject &status) {
  m_installed->setText(stateText(status.value("installed").toString()));
  m_integrity->setText(stateText(status.value("integrity").toString()));
  m_templatesIntegrity->setText(stateText(status.value("templatesIntegrity").toString()));
  m_trustedStatus->setText(stateText(status.value("trusted").toString()));
  m_generatedStatus->setText(stateText(status.value("generated").toString()));
  m_source->setText(status.value("sourceLabel").toString());
  m_details->clear();
  const auto details = status.value("details").toObject();
  const std::pair<const char *, QString> sections[] = {{"status", tr("宿主配置与完整性")},
      {"requestedPolicy", tr("已申请的资源上限")}, {"trusted", tr("可信分析实测")}, {"generated", tr("受控生成实测")}};
  for (const auto &section : sections) {
    const auto values = details.value(QLatin1String(section.first)).toObject();
    if (values.isEmpty()) continue;
    auto *item = new QTreeWidgetItem(m_details, {section.second}); addDetails(item, values);
    item->setExpanded(true);
  }
  showMessage(status.value("message").toString());
}
void PythonEnvironmentDialog::showMessage(const QString &message) { m_message->setText(message); }
void PythonEnvironmentDialog::setBusy(bool busy) {
  m_busy = busy; m_progress->setVisible(busy);
  m_installForm->setEnabled(!busy); m_configurationForm->setEnabled(!busy);
  for (auto *button : {m_probe, m_export, m_reload}) button->setEnabled(!busy);
  m_history->setEnabled(!busy); m_rollback->setEnabled(!busy && m_history->count() > 0);
  m_revoke->setEnabled(!busy && m_history->count() > 0);
  if (!busy && m_closeWhenIdle) QDialog::reject();
}
void PythonEnvironmentDialog::requestClose() {
  if (!m_busy) { QDialog::reject(); return; }
  if (!m_closeWhenIdle) { m_closeWhenIdle = true; emit cancelRequested(); }
  showMessage(tr("正在结束当前操作，完成后关闭。"));
}
void PythonEnvironmentDialog::reject() { requestClose(); }
void PythonEnvironmentDialog::closeEvent(QCloseEvent *event) {
  if (m_busy) { event->ignore(); requestClose(); return; }
  QDialog::closeEvent(event);
}
