// 层：视图
#include "errorhistorydock.h"
#include "errorhistorymodel.h"
#include "notificationmanager.h"
#include "../paleoicons.h"
#include "../paleotheme.h"
#include "services/errorhub.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace paleo::ui {

using namespace paleo::services;

ErrorHistoryDock::ErrorHistoryDock(QWidget *parent, ErrorHub *hub)
  : QDockWidget(parent)
  , m_hub(hub ? hub : ErrorHub::instance())
{
  setObjectName(QStringLiteral("errorHistoryDock"));
  setWindowTitle(tr("错误历史"));
  setAccessibleName(tr("错误历史面板"));

  QAction *toggleAct = toggleViewAction();
  toggleAct->setObjectName(QStringLiteral("actionViewErrorHistory"));
  toggleAct->setText(tr("错误历史"));
  toggleAct->setToolTip(tr("显示/隐藏错误与警告历史面板"));
  toggleAct->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionHistory.svg")));

  setupUi();
  setupConnections();
  updateSummaryLabel();
}

ErrorHistoryDock::~ErrorHistoryDock() = default;

void ErrorHistoryDock::setupUi()
{
  auto *centralWidget = new QWidget(this);
  centralWidget->setObjectName(QStringLiteral("errorHistoryDockContent"));
  centralWidget->setProperty("paleo.scrollHost", true);

  const auto &t = PaleoTheme::tokens();

  auto *rootLayout = new QVBoxLayout(centralWidget);
  rootLayout->setContentsMargins(t.spacingSm, t.spacingSm, t.spacingSm, t.spacingSm);
  rootLayout->setSpacing(t.spacingXs);

  // 1. 顶部筛选与操作工具条
  auto *toolbarLayout = new QHBoxLayout();
  toolbarLayout->setContentsMargins(0, 0, 0, 0);
  toolbarLayout->setSpacing(t.spacingXs);

  // 领域筛选
  auto *domainLabel = new QLabel(tr("来源域:"), centralWidget);
  domainLabel->setFont(PaleoTheme::bodyFont());
  toolbarLayout->addWidget(domainLabel);

  m_domainCombo = new QComboBox(centralWidget);
  m_domainCombo->setObjectName(QStringLiteral("domainFilterCombo"));
  m_domainCombo->setAccessibleName(tr("错误来源域筛选"));
  m_domainCombo->addItem(tr("全部领域"));
  m_domainCombo->addItem(ErrorDomain::General);
  m_domainCombo->addItem(ErrorDomain::Project);
  m_domainCombo->addItem(ErrorDomain::Catalog);
  m_domainCombo->addItem(ErrorDomain::IO);
  m_domainCombo->addItem(ErrorDomain::Seismic);
  m_domainCombo->addItem(ErrorDomain::Well);
  m_domainCombo->addItem(ErrorDomain::Gridding);
  m_domainCombo->addItem(ErrorDomain::Surface);
  m_domainCombo->addItem(ErrorDomain::Crossplot);
  m_domainCombo->addItem(ErrorDomain::AI);
  m_domainCombo->addItem(ErrorDomain::Layout);
  m_domainCombo->addItem(ErrorDomain::System);
  toolbarLayout->addWidget(m_domainCombo);

  // 级别筛选
  auto *levelLabel = new QLabel(tr("级别:"), centralWidget);
  levelLabel->setFont(PaleoTheme::bodyFont());
  toolbarLayout->addWidget(levelLabel);

  m_levelCombo = new QComboBox(centralWidget);
  m_levelCombo->setObjectName(QStringLiteral("levelFilterCombo"));
  m_levelCombo->setAccessibleName(tr("错误级别筛选"));
  m_levelCombo->addItem(tr("全部级别"), static_cast<int>(ErrorHistoryFilterProxyModel::LevelFilterMode::All));
  m_levelCombo->addItem(tr("警告及以上"), static_cast<int>(ErrorHistoryFilterProxyModel::LevelFilterMode::WarningAndAbove));
  m_levelCombo->addItem(tr("错误及以上"), static_cast<int>(ErrorHistoryFilterProxyModel::LevelFilterMode::ErrorAndAbove));
  m_levelCombo->addItem(tr("仅信息"), static_cast<int>(ErrorHistoryFilterProxyModel::LevelFilterMode::InfoOnly));
  m_levelCombo->addItem(tr("仅警告"), static_cast<int>(ErrorHistoryFilterProxyModel::LevelFilterMode::WarningOnly));
  m_levelCombo->addItem(tr("仅错误"), static_cast<int>(ErrorHistoryFilterProxyModel::LevelFilterMode::ErrorOnly));
  m_levelCombo->addItem(tr("仅致命"), static_cast<int>(ErrorHistoryFilterProxyModel::LevelFilterMode::CriticalOnly));
  toolbarLayout->addWidget(m_levelCombo);

  // 模糊搜索框
  m_searchEdit = new QLineEdit(centralWidget);
  m_searchEdit->setObjectName(QStringLiteral("searchFilterEdit"));
  m_searchEdit->setAccessibleName(tr("错误信息模糊搜索"));
  m_searchEdit->setPlaceholderText(tr("搜索错误信息/详情..."));
  m_searchEdit->setClearButtonEnabled(true);
  toolbarLayout->addWidget(m_searchEdit, 1);

  // 动作按钮组
  m_copySelectedBtn = new QToolButton(centralWidget);
  m_copySelectedBtn->setObjectName(QStringLiteral("copySelectedButton"));
  m_copySelectedBtn->setText(tr("复制选中"));
  m_copySelectedBtn->setToolTip(tr("复制选中条目的完整信息至剪贴板"));
  m_copySelectedBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionEditCopy.svg")));
  m_copySelectedBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_copySelectedBtn->setEnabled(false);
  toolbarLayout->addWidget(m_copySelectedBtn);

  m_copyAllBtn = new QToolButton(centralWidget);
  m_copyAllBtn->setObjectName(QStringLiteral("copyAllButton"));
  m_copyAllBtn->setText(tr("复制全部"));
  m_copyAllBtn->setToolTip(tr("复制当前显示的全部错误信息至剪贴板"));
  m_copyAllBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionEditCopy.svg")));
  m_copyAllBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toolbarLayout->addWidget(m_copyAllBtn);

  m_clearBtn = new QToolButton(centralWidget);
  m_clearBtn->setObjectName(QStringLiteral("clearHistoryButton"));
  m_clearBtn->setText(tr("清空历史"));
  m_clearBtn->setToolTip(tr("清空错误历史记录"));
  m_clearBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionTrash.svg")));
  m_clearBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toolbarLayout->addWidget(m_clearBtn);

  rootLayout->addLayout(toolbarLayout);

  // 2. 表格模型与视图
  m_model = new ErrorHistoryModel(m_hub, this);
  m_proxyModel = new ErrorHistoryFilterProxyModel(this);
  m_proxyModel->setSourceModel(m_model);

  m_tableView = new QTableView(centralWidget);
  m_tableView->setObjectName(QStringLiteral("errorHistoryTableView"));
  m_tableView->setAccessibleName(tr("错误历史表格"));
  m_tableView->setModel(m_proxyModel);
  m_tableView->setAlternatingRowColors(true);
  m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_tableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_tableView->setSortingEnabled(true);
  m_tableView->setShowGrid(false);
  m_tableView->setWordWrap(false);
  m_tableView->setContextMenuPolicy(Qt::CustomContextMenu);

  auto *hHeader = m_tableView->horizontalHeader();
  hHeader->setStretchLastSection(true);
  hHeader->setHighlightSections(false);
  hHeader->setSectionResizeMode(ErrorHistoryModel::ColTimestamp, QHeaderView::ResizeToContents);
  hHeader->setSectionResizeMode(ErrorHistoryModel::ColLevel, QHeaderView::ResizeToContents);
  hHeader->setSectionResizeMode(ErrorHistoryModel::ColDomain, QHeaderView::ResizeToContents);
  hHeader->setSectionResizeMode(ErrorHistoryModel::ColTitle, QHeaderView::Interactive);
  hHeader->setSectionResizeMode(ErrorHistoryModel::ColMessage, QHeaderView::Stretch);
  hHeader->setSectionResizeMode(ErrorHistoryModel::ColCount, QHeaderView::ResizeToContents);
  m_tableView->setColumnWidth(ErrorHistoryModel::ColTitle, 200);

  auto *vHeader = m_tableView->verticalHeader();
  vHeader->setVisible(false);
  vHeader->setDefaultSectionSize(PaleoTheme::tableRowHeight(PaleoTheme::currentDensity()));

  PaleoTheme::applyDensityToViewTree(m_tableView);

  rootLayout->addWidget(m_tableView, 1);

  // 3. 底部状态汇总栏
  m_statusLabel = new QLabel(centralWidget);
  m_statusLabel->setObjectName(QStringLiteral("errorHistoryStatusLabel"));
  m_statusLabel->setFont(PaleoTheme::bodyFont());
  PaleoTheme::applyThemedStyleSheet(m_statusLabel, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  rootLayout->addWidget(m_statusLabel);

  setWidget(centralWidget);

  // 动态主题联动绑定
  PaleoTheme::applyThemedStyleSheet(m_copySelectedBtn, [] {
    return PaleoTheme::toolButtonStyleSheet();
  });
  PaleoTheme::applyThemedStyleSheet(m_copyAllBtn, [] {
    return PaleoTheme::toolButtonStyleSheet();
  });
  PaleoTheme::applyThemedStyleSheet(m_clearBtn, [] {
    return PaleoTheme::toolButtonStyleSheet();
  });
}

void ErrorHistoryDock::setupConnections()
{
  connect(m_domainCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &ErrorHistoryDock::onDomainFilterChanged);
  connect(m_levelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &ErrorHistoryDock::onLevelFilterChanged);
  connect(m_searchEdit, &QLineEdit::textChanged,
          this, &ErrorHistoryDock::onSearchTextChanged);

  connect(m_copySelectedBtn, &QToolButton::clicked, this, &ErrorHistoryDock::copySelectedToClipboard);
  connect(m_copyAllBtn, &QToolButton::clicked, this, &ErrorHistoryDock::copyAllToClipboard);
  connect(m_clearBtn, &QToolButton::clicked, this, &ErrorHistoryDock::clearHistory);

  if (auto *selModel = m_tableView->selectionModel()) {
    connect(selModel, &QItemSelectionModel::selectionChanged,
            this, &ErrorHistoryDock::onTableSelectionChanged);
  }

  connect(m_proxyModel, &QAbstractItemModel::rowsInserted, this, &ErrorHistoryDock::updateSummaryLabel);
  connect(m_proxyModel, &QAbstractItemModel::rowsRemoved, this, &ErrorHistoryDock::updateSummaryLabel);
  connect(m_proxyModel, &QAbstractItemModel::modelReset, this, &ErrorHistoryDock::updateSummaryLabel);
  connect(m_proxyModel, &QAbstractItemModel::layoutChanged, this, &ErrorHistoryDock::updateSummaryLabel);

  // 右键菜单
  connect(m_tableView, &QTableView::customContextMenuRequested, this, [this](const QPoint &pos) {
    QMenu menu(this);
    QAction *actCopySel = menu.addAction(PaleoIcons::qgisTheme(QStringLiteral("mActionEditCopy.svg")), tr("复制选中条目"));
    actCopySel->setEnabled(m_copySelectedBtn->isEnabled());
    connect(actCopySel, &QAction::triggered, this, &ErrorHistoryDock::copySelectedToClipboard);

    QAction *actCopyAll = menu.addAction(PaleoIcons::qgisTheme(QStringLiteral("mActionEditCopy.svg")), tr("复制全部条目"));
    actCopyAll->setEnabled(m_proxyModel->rowCount() > 0);
    connect(actCopyAll, &QAction::triggered, this, &ErrorHistoryDock::copyAllToClipboard);

    menu.addSeparator();
    QAction *actClear = menu.addAction(PaleoIcons::qgisTheme(QStringLiteral("mActionTrash.svg")), tr("清空历史记录"));
    actClear->setEnabled(m_model->rowCount() > 0);
    connect(actClear, &QAction::triggered, this, &ErrorHistoryDock::clearHistory);

    menu.exec(m_tableView->viewport()->mapToGlobal(pos));
  });
}

void ErrorHistoryDock::onDomainFilterChanged(int index)
{
  if (index <= 0) {
    m_proxyModel->setDomainFilter(QString());
  } else {
    m_proxyModel->setDomainFilter(m_domainCombo->itemText(index));
  }
}

void ErrorHistoryDock::onLevelFilterChanged(int index)
{
  if (index < 0) {
    return;
  }
  auto mode = static_cast<ErrorHistoryFilterProxyModel::LevelFilterMode>(m_levelCombo->itemData(index).toInt());
  m_proxyModel->setLevelFilterMode(mode);
}

void ErrorHistoryDock::onSearchTextChanged(const QString &text)
{
  m_proxyModel->setSearchFilter(text);
}

void ErrorHistoryDock::onTableSelectionChanged()
{
  bool hasSel = m_tableView->selectionModel() && m_tableView->selectionModel()->hasSelection();
  m_copySelectedBtn->setEnabled(hasSel);
}

void ErrorHistoryDock::updateSummaryLabel()
{
  int total = m_model->rowCount();
  int visible = m_proxyModel->rowCount();
  if (total == visible) {
    m_statusLabel->setText(tr("共 %1 条记录").arg(total));
  } else {
    m_statusLabel->setText(tr("显示 %1 / 共 %2 条记录").arg(visible).arg(total));
  }
}

void ErrorHistoryDock::setDomainFilter(const QString &domain)
{
  if (domain.isEmpty() || domain == tr("全部领域") || domain == QStringLiteral("All") || domain == QStringLiteral("全部")) {
    m_domainCombo->setCurrentIndex(0);
    m_proxyModel->setDomainFilter(QString());
    return;
  }
  int idx = m_domainCombo->findText(domain, Qt::MatchFixedString);
  if (idx >= 0) {
    m_domainCombo->setCurrentIndex(idx);
  } else {
    m_proxyModel->setDomainFilter(domain);
  }
}

void ErrorHistoryDock::setLevelFilter(ErrorLevel level)
{
  ErrorHistoryFilterProxyModel::LevelFilterMode mode = ErrorHistoryFilterProxyModel::LevelFilterMode::All;
  switch (level) {
    case ErrorLevel::Info:
      mode = ErrorHistoryFilterProxyModel::LevelFilterMode::InfoOnly;
      break;
    case ErrorLevel::Warning:
      mode = ErrorHistoryFilterProxyModel::LevelFilterMode::WarningOnly;
      break;
    case ErrorLevel::Error:
      mode = ErrorHistoryFilterProxyModel::LevelFilterMode::ErrorOnly;
      break;
    case ErrorLevel::Critical:
      mode = ErrorHistoryFilterProxyModel::LevelFilterMode::CriticalOnly;
      break;
  }
  int idx = m_levelCombo->findData(static_cast<int>(mode));
  if (idx >= 0) {
    m_levelCombo->setCurrentIndex(idx);
  } else {
    m_proxyModel->setLevelFilterMode(mode);
  }
}

void ErrorHistoryDock::setLevelFilter(const QString &level)
{
  QString s = level.trimmed();
  if (s.compare(QStringLiteral("Error"), Qt::CaseInsensitive) == 0 || s == tr("错误") || s == tr("仅错误")) {
    setLevelFilter(ErrorLevel::Error);
  } else if (s.compare(QStringLiteral("Warning"), Qt::CaseInsensitive) == 0 || s == tr("警告") || s == tr("仅警告")) {
    setLevelFilter(ErrorLevel::Warning);
  } else if (s.compare(QStringLiteral("Info"), Qt::CaseInsensitive) == 0 || s == tr("信息") || s == tr("仅信息")) {
    setLevelFilter(ErrorLevel::Info);
  } else if (s.compare(QStringLiteral("Critical"), Qt::CaseInsensitive) == 0 || s == tr("致命") || s == tr("仅致命")) {
    setLevelFilter(ErrorLevel::Critical);
  } else {
    m_levelCombo->setCurrentIndex(0);
    m_proxyModel->setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode::All);
  }
}

void ErrorHistoryDock::setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode mode)
{
  int idx = m_levelCombo->findData(static_cast<int>(mode));
  if (idx >= 0) {
    m_levelCombo->setCurrentIndex(idx);
  } else {
    m_proxyModel->setLevelFilterMode(mode);
  }
}

void ErrorHistoryDock::setSearchText(const QString &text)
{
  m_searchEdit->setText(text);
}

QString ErrorHistoryDock::formatEntryForClipboard(const ErrorEntry &e) const
{
  QString levelStr = errorLevelToString(e.level);
  QString timeStr = e.timestamp.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
  QString line = QStringLiteral("[%1] [%2] [%3] (x%4): %5")
                   .arg(timeStr, levelStr, e.domain, QString::number(e.aggregationCount), e.message);
  if (!e.details.isEmpty()) {
    line += QStringLiteral("\n  详情: ") + e.details;
  }
  return line;
}

void ErrorHistoryDock::copySelectedToClipboard()
{
  auto *selModel = m_tableView->selectionModel();
  if (!selModel) {
    return;
  }

  QModelIndexList selectedRows = selModel->selectedRows();
  if (selectedRows.isEmpty()) {
    QModelIndex cur = m_tableView->currentIndex();
    if (cur.isValid()) {
      selectedRows.append(m_proxyModel->index(cur.row(), 0));
    }
  }

  if (selectedRows.isEmpty()) {
    return;
  }

  QStringList lines;
  lines.reserve(selectedRows.size());
  for (const auto &proxyIdx : selectedRows) {
    QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
    if (srcIdx.isValid()) {
      lines.append(formatEntryForClipboard(m_model->entryAt(srcIdx.row())));
    }
  }

  QApplication::clipboard()->setText(lines.join(QStringLiteral("\n---\n")));
}

void ErrorHistoryDock::copyAllToClipboard()
{
  int rows = m_proxyModel->rowCount();
  if (rows == 0) {
    return;
  }

  QStringList lines;
  lines.reserve(rows);
  for (int i = 0; i < rows; ++i) {
    QModelIndex proxyIdx = m_proxyModel->index(i, 0);
    QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
    if (srcIdx.isValid()) {
      lines.append(formatEntryForClipboard(m_model->entryAt(srcIdx.row())));
    }
  }

  QApplication::clipboard()->setText(lines.join(QStringLiteral("\n---\n")));
}

void ErrorHistoryDock::clearHistory()
{
  if (m_model->rowCount() == 0) {
    return;
  }

  // 严格遵循三级联动与 0 QMessageBox 规范，支持无头环境自动应答
  if (!NotificationManager::confirmDestructive(this, tr("清空历史"),
                                               tr("确定清空全部错误历史记录？该操作不可撤销。"))) {
    return;
  }

  if (m_hub) {
    m_hub->clear();
  } else if (auto *hub = ErrorHub::instance()) {
    hub->clear();
  }
}

} // namespace paleo::ui
