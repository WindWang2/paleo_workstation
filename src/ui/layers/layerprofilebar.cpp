// 层：视图
#include "layerprofilebar.h"

#include "qgis/qgislayerprofile.h"

#include <QAction>
#include <QComboBox>
#include <QDialog>
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
  // DESIGN.md：surface #FFFFFF 底；正文 9pt（pointSize 跟随系统缩放）。
  setStyleSheet(QStringLiteral("LayerProfileBar { background: #FFFFFF; }"));
  QFont base = font();
  base.setPointSizeF(9.0);
  setFont(base);

  m_combo = new QComboBox(this);
  m_combo->setObjectName(QStringLiteral("layerThemeCombo"));
  m_combo->setToolTip(tr("可见性主题（页面档案与保存的主题）"));
  m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_combo->setMinimumContentsLength(10);
  // 用户交互路径才走 applyTheme：activated 仅由弹出选择/键盘触发，
  // 程序化 setCurrentIndex 不发；程序化改选另叠 QSignalBlocker 兜底。
  connect(m_combo, &QComboBox::activated, this, &LayerProfileBar::onComboActivated);

  auto *saveButton = mkTextButton(
      this, QStringLiteral("layerSaveThemeButton"), tr("保存当前可见性为主题…"),
      [this] { saveCurrentAsThemeWithDialog(); });
  auto *manageButton = mkTextButton(
      this, QStringLiteral("layerManageThemesButton"), tr("管理主题…"),
      [this] { showManageDialog(); });

  // 当前页档案指示：8pt 次级（DESIGN.md label 字阶 + text-muted #5D6E80）。
  m_pageLabel = new QLabel(this);
  m_pageLabel->setObjectName(QStringLiteral("layerPageProfileLabel"));
  m_pageLabel->setStyleSheet(QStringLiteral("color: #5D6E80;"));
  QFont labelFont = m_pageLabel->font();
  labelFont.setPointSizeF(8.0);
  m_pageLabel->setFont(labelFont);

  // 紧凑工具条：水平布局，间距 xs=4；水平内边距 sm=8、垂直 xs（压高度）。
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(8, 4, 8, 4);
  layout->setSpacing(4);
  layout->addWidget(m_combo, 1);
  layout->addWidget(saveButton);
  layout->addWidget(manageButton);
  layout->addSpacing(4);
  layout->addWidget(m_pageLabel);

  if (m_service)
    connect(m_service, &QgisLayerProfileService::mapThemesChanged, this,
            &LayerProfileBar::refreshThemeCombo);

  refreshThemeCombo();
  updatePageLabel();
}

void LayerProfileBar::setCurrentPage(const QString &pageId)
{
  m_currentPage = pageId;
  updatePageLabel();
  if (m_service && !pageId.isEmpty())
  {
    const QString themeName = QgisLayerProfileService::pageThemeName(pageId);
    if (m_service->themes().contains(themeName) && m_combo)
    {
      const QSignalBlocker blocker(m_combo); // 程序化选中：不发激活路径
      const int idx = m_combo->findData(themeName);
      if (idx >= 0)
        m_combo->setCurrentIndex(idx);
    }
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
  m_service->applyTheme(name); // 失败静默（主题刚被并发移除等边缘态）
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
    m_service->applyTheme(item->data(Qt::UserRole).toString());
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

  // V1 裁决可视面：不做重命名（QgsMapThemeCollection 无 rename API 且
  // QgisLayerProfileService public 面无记录复制），小字注明。
  auto *renameNote = new QLabel(tr("重命名暂未支持"), dialog);
  renameNote->setObjectName(QStringLiteral("layerManageRenameNote"));
  renameNote->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
  QFont noteFont = renameNote->font();
  noteFont.setPointSizeF(8.0); // DESIGN.md label 8pt
  renameNote->setFont(noteFont);

  auto *buttonRow = new QHBoxLayout;
  buttonRow->setSpacing(4); // xs
  buttonRow->addWidget(applyButton);
  buttonRow->addWidget(removeButton);
  buttonRow->addSpacing(4);
  buttonRow->addWidget(renameNote, 1);
  vlay->addLayout(buttonRow);
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
