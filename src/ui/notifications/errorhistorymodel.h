// 层：视图
#pragma once

#include <QAbstractTableModel>
#include <QDateTime>
#include <QSortFilterProxyModel>
#include <QVector>
#include <optional>

#include "services/errorhub.h"

namespace paleo::ui {

/// 错误历史表格模型（视图层）
/// 严格遵守 500 条 FIFO 环形缓冲区同步协议与 Qt Model/View 变更时序
class ErrorHistoryModel : public QAbstractTableModel
{
  Q_OBJECT

public:
  enum Column {
    ColTimestamp = 0, ///< 发生时间 (yyyy-MM-dd hh:mm:ss)
    ColLevel,         ///< 严重级别 (提示/警告/错误/致命)
    ColDomain,        ///< 来源领域 (General, Project, IO, ...)
    ColTitle,         ///< 概要标题 (ErrorEntry::message)
    ColMessage,       ///< 详细信息 (ErrorEntry::details 或 message)
    ColCount,         ///< 60s 窗口内聚合计数
    ColumnCount
  };
  Q_ENUM(Column)

  enum CustomRole {
    RawEntryRole = Qt::UserRole + 1, ///< 返回完整 ErrorEntry 结构体
    LevelRole,                       ///< 返回 paleo::services::ErrorLevel 枚举值
    DomainRole,                      ///< 返回 QString 领域名
    TimestampRole,                   ///< 返回 QDateTime 时间戳
    CountRole,                       ///< 返回 int 聚合次数
    IdRole                           ///< 返回 qint64 全局自增 ID
  };
  Q_ENUM(CustomRole)

  explicit ErrorHistoryModel(paleo::services::ErrorHub *hub = nullptr, QObject *parent = nullptr);
  ~ErrorHistoryModel() override = default;

  // QAbstractItemModel 契约
  int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  int columnCount(const QModelIndex &parent = QModelIndex()) const override;
  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  // 便捷实体访问
  const paleo::services::ErrorEntry &entryAt(int row) const;
  std::optional<paleo::services::ErrorEntry> entryById(qint64 id) const;
  QVector<paleo::services::ErrorEntry> allEntries() const { return m_entries; }

  // 绑定与重同步
  void setHub(paleo::services::ErrorHub *hub);
  paleo::services::ErrorHub *hub() const { return m_hub; }
  void reload();

public slots:
  void onErrorRaised(const paleo::services::ErrorEntry &entry);
  void onErrorAggregated(const paleo::services::ErrorEntry &entry);
  void onHistoryCleared();

private:
  paleo::services::ErrorHub *m_hub = nullptr;
  QVector<paleo::services::ErrorEntry> m_entries;
};

/// 错误历史筛选与排序代理模型（视图层）
/// 提供按领域、按严重级别（精确/最低等级）及模糊全文过滤
class ErrorHistoryFilterProxyModel : public QSortFilterProxyModel
{
  Q_OBJECT

public:
  enum class LevelFilterMode {
    All = 0,          ///< 全部级别
    InfoOnly,         ///< 仅信息
    WarningOnly,      ///< 仅警告
    ErrorOnly,        ///< 仅一般错误
    CriticalOnly,     ///< 仅致命错误
    WarningAndAbove,  ///< 警告及以上 (>= Warning)
    ErrorAndAbove     ///< 错误及以上 (>= Error)
  };
  Q_ENUM(LevelFilterMode)

  explicit ErrorHistoryFilterProxyModel(QObject *parent = nullptr);
  ~ErrorHistoryFilterProxyModel() override = default;

  // 过滤属性设置
  void setDomainFilter(const QString &domain);
  QString domainFilter() const { return m_domainFilter; }

  void setLevelFilterMode(LevelFilterMode mode);
  LevelFilterMode levelFilterMode() const { return m_levelMode; }
  void setLevelFilter(std::optional<paleo::services::ErrorLevel> level);

  void setSearchFilter(const QString &text);
  QString searchFilter() const { return m_searchText; }

protected:
  bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
  bool lessThan(const QModelIndex &source_left, const QModelIndex &source_right) const override;

private:
  QString m_domainFilter;
  LevelFilterMode m_levelMode = LevelFilterMode::All;
  QString m_searchText;
};

} // namespace paleo::ui
