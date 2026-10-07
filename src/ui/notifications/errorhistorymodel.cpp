// 层：视图
#include "errorhistorymodel.h"
#include "../paleotheme.h"

#include <QFont>

namespace paleo::ui {

using namespace paleo::services;

static const ErrorEntry s_emptyEntry{};

ErrorHistoryModel::ErrorHistoryModel(paleo::services::ErrorHub *hub, QObject *parent)
  : QAbstractTableModel(parent)
{
  setHub(hub ? hub : paleo::services::ErrorHub::instance());
}

void ErrorHistoryModel::setHub(paleo::services::ErrorHub *hub)
{
  if (m_hub == hub) {
    return;
  }

  if (m_hub) {
    disconnect(m_hub, nullptr, this, nullptr);
  }

  m_hub = hub;

  if (m_hub) {
    connect(m_hub, &paleo::services::ErrorHub::errorRaised, this, &ErrorHistoryModel::onErrorRaised, Qt::QueuedConnection);
    connect(m_hub, &paleo::services::ErrorHub::errorAggregated, this, &ErrorHistoryModel::onErrorAggregated, Qt::QueuedConnection);
    connect(m_hub, &paleo::services::ErrorHub::historyCleared, this, &ErrorHistoryModel::onHistoryCleared, Qt::QueuedConnection);
  }

  reload();
}

void ErrorHistoryModel::reload()
{
  beginResetModel();
  if (m_hub) {
    m_entries = m_hub->history();
  } else {
    m_entries.clear();
  }
  endResetModel();
}

int ErrorHistoryModel::rowCount(const QModelIndex &parent) const
{
  if (parent.isValid()) {
    return 0;
  }
  return static_cast<int>(m_entries.size());
}

int ErrorHistoryModel::columnCount(const QModelIndex &parent) const
{
  if (parent.isValid()) {
    return 0;
  }
  return ColumnCount;
}

QVariant ErrorHistoryModel::data(const QModelIndex &index, int role) const
{
  if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
    return QVariant();
  }

  const auto &entry = m_entries.at(index.row());

  if (role == RawEntryRole) {
    return QVariant::fromValue(entry);
  }
  if (role == LevelRole) {
    return QVariant::fromValue(entry.level);
  }
  if (role == DomainRole) {
    return entry.domain;
  }
  if (role == TimestampRole) {
    return entry.timestamp;
  }
  if (role == CountRole) {
    return entry.aggregationCount;
  }
  if (role == IdRole) {
    return entry.id;
  }

  if (role == Qt::DisplayRole) {
    switch (index.column()) {
      case ColTimestamp:
        return entry.timestamp.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
      case ColLevel: {
        switch (entry.level) {
          case ErrorLevel::Info:     return tr("信息");
          case ErrorLevel::Warning:  return tr("警告");
          case ErrorLevel::Error:    return tr("错误");
          case ErrorLevel::Critical: return tr("致命");
        }
        return tr("错误");
      }
      case ColDomain:
        return entry.domain;
      case ColTitle:
        return entry.message;
      case ColMessage:
        return entry.details.isEmpty() ? entry.message : entry.details;
      case ColCount:
        return entry.aggregationCount;
      default:
        break;
    }
  }

  if (role == Qt::ToolTipRole) {
    QString tip = tr("【序号 %1】领域: %2 | 级别: %3 | 时间: %4\n概要: %5")
                    .arg(QString::number(entry.id), entry.domain, errorLevelToString(entry.level),
                         entry.timestamp.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")), entry.message);
    if (!entry.details.isEmpty()) {
      tip += tr("\n详情:\n%1").arg(entry.details);
    }
    if (entry.aggregationCount > 1) {
      tip += tr("\n(60s 内聚合发生 %1 次，首次时间: %2)")
               .arg(QString::number(entry.aggregationCount), entry.firstSeen.toString(QStringLiteral("hh:mm:ss")));
    }
    return tip;
  }

  if (role == Qt::TextAlignmentRole) {
    switch (index.column()) {
      case ColTimestamp:
      case ColLevel:
      case ColDomain:
      case ColCount:
        return static_cast<int>(Qt::AlignCenter);
      case ColTitle:
      case ColMessage:
      default:
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }
  }

  if (role == Qt::FontRole) {
    if (index.column() == ColTimestamp || index.column() == ColCount) {
      return PaleoTheme::monoFont();
    }
    return PaleoTheme::bodyFont();
  }

  return QVariant();
}

QVariant ErrorHistoryModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
    switch (section) {
      case ColTimestamp: return tr("时间");
      case ColLevel:     return tr("级别");
      case ColDomain:    return tr("来源域");
      case ColTitle:     return tr("概要");
      case ColMessage:   return tr("详细信息");
      case ColCount:     return tr("计数");
      default: break;
    }
  }
  return QAbstractTableModel::headerData(section, orientation, role);
}

const ErrorEntry &ErrorHistoryModel::entryAt(int row) const
{
  if (row >= 0 && row < m_entries.size()) {
    return m_entries.at(row);
  }
  return s_emptyEntry;
}

std::optional<ErrorEntry> ErrorHistoryModel::entryById(qint64 id) const
{
  for (const auto &e : m_entries) {
    if (e.id == id) {
      return e;
    }
  }
  return std::nullopt;
}

void ErrorHistoryModel::onErrorRaised(const ErrorEntry &entry)
{
  int maxCap = m_hub ? m_hub->maxCapacity() : paleo::services::ErrorHub::kDefaultMaxHistory;

  // 严格 FIFO 逐出协议：当缓冲已满上限时，首先逐出第 0 行
  while (static_cast<int>(m_entries.size()) >= maxCap && !m_entries.isEmpty()) {
    beginRemoveRows(QModelIndex(), 0, 0);
    m_entries.removeFirst();
    endRemoveRows();
  }

  int newRow = static_cast<int>(m_entries.size());
  beginInsertRows(QModelIndex(), newRow, newRow);
  m_entries.append(entry);
  endInsertRows();
}

void ErrorHistoryModel::onErrorAggregated(const ErrorEntry &entry)
{
  // 倒序查找（聚合通常发生在最近几条）
  for (int i = static_cast<int>(m_entries.size()) - 1; i >= 0; --i) {
    if (m_entries[i].id == entry.id) {
      m_entries[i] = entry;
      QModelIndex left = index(i, 0);
      QModelIndex right = index(i, ColumnCount - 1);
      emit dataChanged(left, right, {Qt::DisplayRole, Qt::ToolTipRole, RawEntryRole, CountRole, TimestampRole});
      return;
    }
  }

  // 若极端情况下未找到，以新条目处理
  onErrorRaised(entry);
}

void ErrorHistoryModel::onHistoryCleared()
{
  beginResetModel();
  m_entries.clear();
  endResetModel();
}

// ----------------------------------------------------------------------------
// ErrorHistoryFilterProxyModel Implementation
// ----------------------------------------------------------------------------

ErrorHistoryFilterProxyModel::ErrorHistoryFilterProxyModel(QObject *parent)
  : QSortFilterProxyModel(parent)
{
  setDynamicSortFilter(true);
}

void ErrorHistoryFilterProxyModel::setDomainFilter(const QString &domain)
{
  if (m_domainFilter != domain) {
    m_domainFilter = domain;
    invalidateFilter();
  }
}

void ErrorHistoryFilterProxyModel::setLevelFilterMode(LevelFilterMode mode)
{
  if (m_levelMode != mode) {
    m_levelMode = mode;
    invalidateFilter();
  }
}

void ErrorHistoryFilterProxyModel::setLevelFilter(std::optional<ErrorLevel> level)
{
  if (!level.has_value()) {
    setLevelFilterMode(LevelFilterMode::All);
  } else {
    switch (level.value()) {
      case ErrorLevel::Info:
        setLevelFilterMode(LevelFilterMode::InfoOnly);
        break;
      case ErrorLevel::Warning:
        setLevelFilterMode(LevelFilterMode::WarningOnly);
        break;
      case ErrorLevel::Error:
        setLevelFilterMode(LevelFilterMode::ErrorOnly);
        break;
      case ErrorLevel::Critical:
        setLevelFilterMode(LevelFilterMode::CriticalOnly);
        break;
    }
  }
}

void ErrorHistoryFilterProxyModel::setSearchFilter(const QString &text)
{
  if (m_searchText != text) {
    m_searchText = text.trimmed();
    invalidateFilter();
  }
}

bool ErrorHistoryFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
  if (!sourceModel()) {
    return false;
  }

  // 1. 来源域过滤
  if (!m_domainFilter.isEmpty() && m_domainFilter != tr("全部领域") &&
      m_domainFilter != QStringLiteral("All") && m_domainFilter != QStringLiteral("全部")) {
    QString domain = sourceModel()->data(sourceModel()->index(sourceRow, ErrorHistoryModel::ColDomain, sourceParent),
                                         ErrorHistoryModel::DomainRole).toString();
    if (domain.compare(m_domainFilter, Qt::CaseInsensitive) != 0) {
      return false;
    }
  }

  // 2. 级别过滤
  if (m_levelMode != LevelFilterMode::All) {
    auto level = sourceModel()->data(sourceModel()->index(sourceRow, ErrorHistoryModel::ColLevel, sourceParent),
                                     ErrorHistoryModel::LevelRole).value<ErrorLevel>();
    switch (m_levelMode) {
      case LevelFilterMode::InfoOnly:
        if (level != ErrorLevel::Info) return false;
        break;
      case LevelFilterMode::WarningOnly:
        if (level != ErrorLevel::Warning) return false;
        break;
      case LevelFilterMode::ErrorOnly:
        if (level != ErrorLevel::Error) return false;
        break;
      case LevelFilterMode::CriticalOnly:
        if (level != ErrorLevel::Critical) return false;
        break;
      case LevelFilterMode::WarningAndAbove:
        if (static_cast<int>(level) < static_cast<int>(ErrorLevel::Warning)) return false;
        break;
      case LevelFilterMode::ErrorAndAbove:
        if (static_cast<int>(level) < static_cast<int>(ErrorLevel::Error)) return false;
        break;
      default:
        break;
    }
  }

  // 3. 模糊全文搜索
  if (!m_searchText.isEmpty()) {
    QString title = sourceModel()->data(sourceModel()->index(sourceRow, ErrorHistoryModel::ColTitle, sourceParent)).toString();
    QString msg = sourceModel()->data(sourceModel()->index(sourceRow, ErrorHistoryModel::ColMessage, sourceParent)).toString();
    QString domain = sourceModel()->data(sourceModel()->index(sourceRow, ErrorHistoryModel::ColDomain, sourceParent)).toString();
    if (!title.contains(m_searchText, Qt::CaseInsensitive) &&
        !msg.contains(m_searchText, Qt::CaseInsensitive) &&
        !domain.contains(m_searchText, Qt::CaseInsensitive)) {
      return false;
    }
  }

  return true;
}

bool ErrorHistoryFilterProxyModel::lessThan(const QModelIndex &source_left, const QModelIndex &source_right) const
{
  if (!sourceModel()) {
    return false;
  }

  int col = source_left.column();
  switch (col) {
    case ErrorHistoryModel::ColTimestamp: {
      QDateTime t1 = sourceModel()->data(source_left, ErrorHistoryModel::TimestampRole).toDateTime();
      QDateTime t2 = sourceModel()->data(source_right, ErrorHistoryModel::TimestampRole).toDateTime();
      return t1 < t2;
    }
    case ErrorHistoryModel::ColLevel: {
      auto l1 = sourceModel()->data(source_left, ErrorHistoryModel::LevelRole).value<ErrorLevel>();
      auto l2 = sourceModel()->data(source_right, ErrorHistoryModel::LevelRole).value<ErrorLevel>();
      return static_cast<int>(l1) < static_cast<int>(l2);
    }
    case ErrorHistoryModel::ColCount: {
      int c1 = sourceModel()->data(source_left, ErrorHistoryModel::CountRole).toInt();
      int c2 = sourceModel()->data(source_right, ErrorHistoryModel::CountRole).toInt();
      return c1 < c2;
    }
    default: {
      QString s1 = sourceModel()->data(source_left).toString();
      QString s2 = sourceModel()->data(source_right).toString();
      return QString::localeAwareCompare(s1, s2) < 0;
    }
  }
}

} // namespace paleo::ui
