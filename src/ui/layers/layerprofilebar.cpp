// 层：视图
#include "layerprofilebar.h"

#include "../paleotheme.h"

#include "qgis/qgislayerprofile.h"

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
  // offscreen（测试/CI）无窗口系统：模态对话框一律跳过——硬纪律
  //（layertreepanel 同款；QInputDialog/QMessageBox/exec 都会卡死事件循环）。
  bool noDialogs()
  {
    return QGuiApplication::platformName() == QLatin1String("offscreen");
  }

  // pageId → 中文页名（页面档案指示与 "page:" 主题显示共用）。
  // 未知 id 原样返回；空 id 返回空。
  QString pageDisplayName(const QString &pageId)
  {
    if (pageId == QLatin1String("data"))
      return QObject::tr("数据管理");
    if (pageId == QLatin1String("predict"))
      return QObject::tr("预测编图");
    if (pageId == QLatin1String("constraint"))
      return QObject::tr("单因素图");
    if (pageId == QLatin1String("compose"))
      return QObject::tr("智能编图");
    if (pageId == QLatin1String("validate"))
      return QObject::tr("验证");
    return pageId;
  }

  // 主题显示名："page:<pageId>" → 「页面·<中文页名>」；用户命名主题原样。
  QString themeDisplayName(const QString &themeName)
  {
    if (themeName.startsWith(QStringLiteral("page:")))
      return QObject::tr("页面·%1").arg(pageDisplayName(themeName.mid(5)));
    return themeName;
  }

  // DESIGN.md：文字型紧凑工具条按钮（QToolButton 文本态 + autoRaise，
  // hover 即 surface-alt 质感；同 layertreepanel 的 defaultActions 用法）。
  QToolButton *mkTextButton(QWidget *parent, const QString &objectName,
                            const QString &text, const std::function<void()> &onTriggered)
  {
    auto *button = new QToolButton(parent);
    button->setObjectName(objectName);
    auto *action = new QAction(text, button);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setAutoRaise(true);
    button->setToolTip(text);
    QObject::connect(action, &QAction::triggered, parent, onTriggered);
    return button;
  }
} // namespace

LayerProfileBar::LayerProfileBar(QgisLayerProfileService *service, QWidget *parent)
    : QWidget(parent), m_service(service)
{
  // DESIGN.md：surface 底（token 活体注册，随主题重算）；正文 9pt
  //（pointSize 跟随系统缩放）。
  PaleoTheme::applyThemedStyleSheet(this, [] {
    return QStringLiteral("LayerProfileBar { background: %1; }")
        .arg(PaleoTheme::tokens().surface.name().toUpper());
  });
  QFont base = font();
  base.setPointSizeF(9.0);
  setFont(base);

  m_combo = new QComboBox(this);
  m_combo->setObjectName(QStringLiteral("layerThemeCombo"));
  m_combo->setToolTip(tr("可见性主题（页面档案与保存的主题）"));
  m_combo->setPlaceholderText(tr("未应用主题"));
  m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_combo->setMinimumContentsLength(10);
  // 用户交互路径才走 applyTheme：activated 仅由弹出选择/键盘触发，
  // 程序化 setCurrentIndex 不发；程序化改选另叠 QSignalBlocker 兜底。
  connect(m_combo, &QComboBox::activated, this, &LayerProfileBar::onComboActivated);

  auto *saveButton = mkTextButton(
      this, QStringLiteral("layerSaveThemeButton"), tr("保存主题…"),
      [this] { saveCurrentAsThemeWithDialog(); });
  auto *manageButton = mkTextButton(
      this, QStringLiteral("layerManageThemesButton"), tr("管理主题…"),
      [this] { showManageDialog(); });

  saveButton->setToolTip(tr("保存当前图层可见性为主题"));

  // 当前页档案指示：8pt 次级（DESIGN.md label 字阶 + text-muted 活体）。
  m_pageLabel = new QLabel(this);
  m_pageLabel->setObjectName(QStringLiteral("layerPageProfileLabel"));
  PaleoTheme::applyThemedStyleSheet(
      m_pageLabel, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  QFont labelFont = m_pageLabel->font();
  labelFont.setPointSizeF(8.0);
  m_pageLabel->setFont(labelFont);

  // Two compact rows keep the layer dock from imposing a wide minimum on the map.
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 4, 8, 4);
  layout->setSpacing(4);
  auto *themeRow = new QHBoxLayout;
  themeRow->setSpacing(4);
  themeRow->addWidget(m_combo, 1);
  themeRow->addWidget(m_pageLabel);
  auto *actionsRow = new QHBoxLayout;
  actionsRow->setSpacing(4);
  actionsRow->addWidget(saveButton);
  actionsRow->addWidget(manageButton);
  actionsRow->addStretch(1);
  layout->addLayout(themeRow);
  layout->addLayout(actionsRow);

  if (m_service)
    connect(m_service, &QgisLayerProfileService::mapThemesChanged, this,
            &LayerProfileBar::refreshThemeCombo);

  refreshThemeCombo();
  updatePageLabel();
}

void LayerProfileBar::setCurrentPage(const QString &pageId)
{
  m_currentPage = pageId;
  refreshThemeCombo(); // project reopen may have replaced its theme collection
  updatePageLabel();
  if (m_combo)
  {
    const QSignalBlocker blocker(m_combo);
    // A missing profile must not leave the previous page's theme displayed.
    m_combo->setCurrentIndex(m_combo->findData(QgisLayerProfileService::pageThemeName(pageId)));
  }
}

QString LayerProfileBar::currentPage() const { return m_currentPage; }

QComboBox *LayerProfileBar::themeCombo() const { return m_combo; }

// ---- 下拉刷新 ----

void LayerProfileBar::refreshThemeCombo()
{
  if (!m_combo)
    return;
  const QString current = m_combo->currentData().toString();
  const QSignalBlocker blocker(m_combo); // 程序化重建不发选中信号
  m_combo->clear();
  if (m_service)
  {
    const QStringList themes = m_service->themes();
    for (const QString &name : themes)
      m_combo->addItem(themeDisplayName(name), name); // UserRole 存真实主题名
  }
  // 保住当前选中（若仍在列表）；不在则清空选中。
  m_combo->setCurrentIndex(current.isEmpty() ? -1 : m_combo->findData(current));
}

void LayerProfileBar::updatePageLabel()
{
  if (!m_pageLabel)
    return;
  m_pageLabel->setText(m_currentPage.isEmpty()
                           ? QString()
                           : tr("页面档案：%1").arg(pageDisplayName(m_currentPage)));
}

// ---- 用户激活路径 ----

void LayerProfileBar::onComboActivated(int index)
{
  if (!m_service || !m_combo || index < 0)
    return;
  const QString name = m_combo->itemData(index).toString();
  if (name.isEmpty())
    return;
  // 失败要可见（主题刚被并发移除等边缘态）——不静默吞掉。
  if (!m_service->applyTheme(name))
    emit statusMessage(tr("应用主题「%1」失败（主题可能已被移除）")
                           .arg(themeDisplayName(name)));
  emit themeSelected(name);
}

// ---- 保存当前可见性为主题 ----

void LayerProfileBar::saveCurrentAsThemeWithDialog()
{
  if (noDialogs() || !m_service)
    return; // offscreen：no-op，不弹不死
  bool ok = false;
  const QString name = QInputDialog::getText(this, tr("保存主题"), tr("主题名称："),
                                             QLineEdit::Normal, QString(), &ok);
  if (!ok)
    return; // 取消
  if (name.trimmed().isEmpty())
  {
    QMessageBox::warning(this, tr("保存主题"), tr("主题名称不能为空"));
    return; // 空名拒绝
  }
  if (!m_service->captureCurrentAsTheme(name.trimmed()))
    QMessageBox::warning(this, tr("保存主题"),
                         tr("保存主题失败（工程或图层树未就绪）"));
}

// ---- 管理主题对话框 ----

QDialog *LayerProfileBar::buildManageDialog()
{
  auto *dialog = new QDialog(this); // bar 自持（Qt 父子所有权，随 bar 析构）
  dialog->setObjectName(QStringLiteral("layerManageThemesDialog"));
  dialog->setWindowTitle(tr("管理主题"));

  auto *vlay = new QVBoxLayout(dialog);
  vlay->setContentsMargins(8, 8, 8, 8); // DESIGN.md sm=8
  vlay->setSpacing(8);

  auto *list = new QListWidget(dialog);
  list->setObjectName(QStringLiteral("layerManageThemeList"));
  if (m_service)
  {
    const QStringList themes = m_service->themes(); // 全部主题（含 page:*）
    for (const QString &name : themes)
    {
      auto *item = new QListWidgetItem(themeDisplayName(name), list);
      item->setData(Qt::UserRole, name);
    }
  }
  vlay->addWidget(list, 1);

  auto *applyButton = new QPushButton(tr("应用"), dialog);
  applyButton->setObjectName(QStringLiteral("layerManageApplyThemeButton"));
  connect(applyButton, &QPushButton::clicked, this, [this, list, dialog]() {
    const QListWidgetItem *item = list->currentItem();
    if (!item || !m_service)
      return;
    if (!m_service->applyTheme(item->data(Qt::UserRole).toString()))
    {
      emit statusMessage(tr("应用主题「%1」失败（主题可能已被移除）")
                             .arg(themeDisplayName(item->data(Qt::UserRole).toString())));
      return; // 失败不关窗——用户看得见、可改选
    }
    dialog->accept(); // 应用即关窗
  });

  // 删除：offscreen 免确认直接删（硬纪律）；page:* 主题允许删
  //（applyPageProfile 会按档案表重建）。
  auto *removeButton = new QPushButton(tr("删除主题"), dialog);
  removeButton->setObjectName(QStringLiteral("layerManageDeleteThemeButton"));
  connect(removeButton, &QPushButton::clicked, this, [this, list]() {
    QListWidgetItem *item = list->currentItem();
    if (!item || !m_service)
      return;
    const QString name = item->data(Qt::UserRole).toString();
    if (!noDialogs())
    {
      const QMessageBox::StandardButton answer = QMessageBox::question(
          this, tr("删除主题"), tr("确定删除主题「%1」？").arg(themeDisplayName(name)),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
      if (answer != QMessageBox::Yes)
        return;
    }
    if (m_service->removeMapTheme(name))
      delete list->takeItem(list->row(item)); // 列表即时收敛（下拉走 mapThemesChanged）
  });

  // 主线5：重命名落地——「先建新名再删旧名」的记录复制（QgsMapThemeCollection
  // 无原生 rename API）；page:* 是页面档案约定名，不改（档案按 pageId 重建）。
  auto *renameButton = new QPushButton(tr("重命名"), dialog);
  renameButton->setObjectName(QStringLiteral("layerManageRenameThemeButton"));
  renameButton->setToolTip(tr("重命名所选主题；page:* 页面档案名不可改"));
  connect(renameButton, &QPushButton::clicked, this, [this, list]() {
    QListWidgetItem *item = list->currentItem();
    if (!item || !m_service)
      return;
    const QString name = item->data(Qt::UserRole).toString();
    if (name.startsWith(QStringLiteral("page:")))
      return; // 系统约定名
    if (noDialogs())
      return; // offscreen：无输入通道（服务面语义由测试直证）
    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, tr("重命名主题"), tr("主题新名字"), QLineEdit::Normal, name, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == name)
      return;
    if (m_service->renameTheme(name, newName.trimmed()))
      delete list->takeItem(list->row(item)); // 下拉经 mapThemesChanged 收敛
    else
      QMessageBox::warning(this, tr("重命名主题"),
                           tr("重命名失败：新名字可能已被占用"));
  });

  auto *renameNote = new QLabel(tr("page:* 页面档案名不可改"), dialog);
  renameNote->setObjectName(QStringLiteral("layerManageRenameNote"));
  PaleoTheme::applyThemedStyleSheet(
      renameNote, [] { return PaleoTheme::mutedCaptionStyleSheet(); }); // text-muted 活体
  QFont noteFont = renameNote->font();
  noteFont.setPointSizeF(8.0); // DESIGN.md label 8pt
  renameNote->setFont(noteFont);

  auto *buttonRow = new QHBoxLayout;
  buttonRow->setSpacing(4); // xs
  buttonRow->addWidget(applyButton);
  buttonRow->addWidget(removeButton);
  buttonRow->addWidget(renameButton);
  buttonRow->addSpacing(4);
  buttonRow->addWidget(renameNote, 1);
  vlay->addLayout(buttonRow);

  // 对话框必须有显式关闭出口（只留标题栏 ✕ 不够）。
  auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  buttonBox->setObjectName(QStringLiteral("layerManageCloseBox"));
  connect(buttonBox, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  vlay->addWidget(buttonBox);
  return dialog;
}

void LayerProfileBar::showManageDialog()
{
  QDialog *dialog = buildManageDialog();
  if (noDialogs())
  {
    dialog->deleteLater(); // offscreen：不 exec，直接返回（测试不死等）
    return;
  }
  dialog->exec();
  dialog->deleteLater();
}
