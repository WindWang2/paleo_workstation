// 层：视图
#pragma once

#include <QDockWidget>

class QTableView;
class QComboBox;
class QLineEdit;
class QToolButton;
class QLabel;

namespace paleo::services {
class ErrorHub;
struct ErrorEntry;
enum class ErrorLevel;
} // namespace paleo::services

#include "errorhistorymodel.h"

namespace paleo::ui {

/// 错误历史停靠面板（视图层）
/// 严格遵守 DESIGN.md 规范：PaleoTheme 样式与令牌集成，无硬编码颜色与无编排动效
class ErrorHistoryDock : public QDockWidget
{
  Q_OBJECT

public:
  explicit ErrorHistoryDock(QWidget *parent = nullptr, paleo::services::ErrorHub *hub = nullptr);
  ~ErrorHistoryDock() override;

  ErrorHistoryModel *model() const { return m_model; }
  ErrorHistoryFilterProxyModel *proxyModel() const { return m_proxyModel; }
  QTableView *tableView() const { return m_tableView; }

  // 筛选设置
  void setDomainFilter(const QString &domain);
  void setLevelFilter(paleo::services::ErrorLevel level);
  void setLevelFilter(const QString &level);
  void setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode mode);
  void setSearchText(const QString &text);

public slots:
  void copySelectedToClipboard();
  void copyAllToClipboard();
  void clearHistory();

  void copySelected() { copySelectedToClipboard(); }
  void copyAll() { copyAllToClipboard(); }

private slots:
  void onDomainFilterChanged(int index);
  void onLevelFilterChanged(int index);
  void onSearchTextChanged(const QString &text);
  void onTableSelectionChanged();
  void updateSummaryLabel();

private:
  void setupUi();
  void setupConnections();
  QString formatEntryForClipboard(const paleo::services::ErrorEntry &entry) const;

  paleo::services::ErrorHub *m_hub = nullptr;
  ErrorHistoryModel *m_model = nullptr;
  ErrorHistoryFilterProxyModel *m_proxyModel = nullptr;

  QComboBox *m_domainCombo = nullptr;
  QComboBox *m_levelCombo = nullptr;
  QLineEdit *m_searchEdit = nullptr;
  QToolButton *m_copySelectedBtn = nullptr;
  QToolButton *m_copyAllBtn = nullptr;
  QToolButton *m_clearBtn = nullptr;
  QTableView *m_tableView = nullptr;
  QLabel *m_statusLabel = nullptr;
};

} // namespace paleo::ui
