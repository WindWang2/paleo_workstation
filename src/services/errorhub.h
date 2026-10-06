// 层：数据
#pragma once

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>
#include <deque>
#include <optional>
#include <unordered_map>

namespace paleo::services {

/// 错误级别
enum class ErrorLevel {
  Info = 0,     ///< 提示信息，对应状态栏或轻量提示
  Warning = 1,  ///< 警告，对应非模态通知卡片
  Error = 2,    ///< 一般业务错误，对应非模态通知卡片（可展开详情）
  Critical = 3  ///< 致命/阻塞错误，受 60s 去重保护的模态弹窗
};

QString errorLevelToString(ErrorLevel level);
ErrorLevel stringToErrorLevel(const QString &str, ErrorLevel fallback = ErrorLevel::Error);

/// 标准领域词表
namespace ErrorDomain {
inline const QString General    = QStringLiteral("General");
inline const QString Project    = QStringLiteral("Project");
inline const QString Catalog    = QStringLiteral("Catalog");
inline const QString IO         = QStringLiteral("IO");
inline const QString Seismic    = QStringLiteral("Seismic");
inline const QString Well       = QStringLiteral("Well");
inline const QString Gridding   = QStringLiteral("Gridding");
inline const QString Surface    = QStringLiteral("Surface");
inline const QString Crossplot  = QStringLiteral("Crossplot");
inline const QString AI         = QStringLiteral("AI");
inline const QString Layout     = QStringLiteral("Layout");
inline const QString System     = QStringLiteral("System");
} // namespace ErrorDomain

/// 错误条目实体
struct ErrorEntry {
  qint64 id = 0;                        ///< 全局自增序号
  ErrorLevel level = ErrorLevel::Error; ///< 级别
  QString domain = ErrorDomain::General;///< 来源域
  QDateTime timestamp;                  ///< 最近一次发生时间
  QDateTime firstSeen;                  ///< 聚合窗口内首次发生时间
  QString deduplicationKey;             ///< 60s 去重匹配键
  int aggregationCount = 1;             ///< 聚合计数（>=1）
  QString message;                      ///< 概要标题/文案
  QString details;                      ///< 详情信息（堆栈/路径/上下文）
  bool isModal = false;                 ///< 是否建议模态呈现（Critical 或致命决策）

  static QString makeDefaultKey(ErrorLevel level, const QString &domain, const QString &message);
  bool operator==(const ErrorEntry &other) const;
};

/// 历史查询过滤器
struct ErrorQueryFilter {
  std::optional<ErrorLevel> level;      ///< 精确级别匹配
  std::optional<ErrorLevel> minLevel;   ///< 最低级别匹配
  QString domain;                       ///< 来源域过滤（空为全部）
  QString searchText;                   ///< 文本模糊过滤（匹配 message 或 details）
  QDateTime since;                      ///< 起始时间
  int limit = 0;                        ///< 返回条数上限（0 为不限）
};

/// ErrorHub: 集中式错误汇聚与历史管理服务（数据层）
class ErrorHub : public QObject {
  Q_OBJECT

public:
  static constexpr int kDefaultMaxHistory = 500;
  static constexpr qint64 kDefaultDedupWindowSecs = 60;

  explicit ErrorHub(QObject *parent = nullptr);
  ~ErrorHub() override;

  /// 全局单例/宿主访问点
  static ErrorHub *instance();
  static void setInstance(ErrorHub *customInstance);

  // ---- 核心上报接口 ----
  void report(const ErrorEntry &entry);

  // 便捷上报实例方法
  void reportInfo(const QString &domain, const QString &message,
                  const QString &details = QString(), const QString &dedupKey = QString());
  void reportWarning(const QString &domain, const QString &message,
                     const QString &details = QString(), const QString &dedupKey = QString());
  void reportError(const QString &domain, const QString &message,
                   const QString &details = QString(), const QString &dedupKey = QString());
  void reportCritical(const QString &domain, const QString &message,
                      const QString &details = QString(), const QString &dedupKey = QString());

  // 静态便捷快捷调用
  static void postInfo(const QString &domain, const QString &message,
                       const QString &details = QString(), const QString &dedupKey = QString());
  static void postWarning(const QString &domain, const QString &message,
                          const QString &details = QString(), const QString &dedupKey = QString());
  static void postError(const QString &domain, const QString &message,
                        const QString &details = QString(), const QString &dedupKey = QString());
  static void postCritical(const QString &domain, const QString &message,
                           const QString &details = QString(), const QString &dedupKey = QString());

  /// 业务辅助：若 ok 为 false，自动包装上报并返回 false；若 ok 为 true 直接返回 true
  static bool checkOrReport(bool ok, ErrorLevel level, const QString &domain,
                            const QString &message, const QString &details = QString(),
                            const QString &dedupKey = QString());

  // ---- 查询接口 ----
  QVector<ErrorEntry> history() const;
  QVector<ErrorEntry> query(const ErrorQueryFilter &filter) const;
  QVector<ErrorEntry> queryByDomain(const QString &domain) const;
  QVector<ErrorEntry> queryByLevel(ErrorLevel level) const;
  QVector<ErrorEntry> queryByMinLevel(ErrorLevel minLevel) const;
  std::optional<ErrorEntry> entryById(qint64 id) const;
  int count() const;
  int countByLevel(ErrorLevel level) const;

  // ---- 容量与管理配置 ----
  void clear();
  int maxCapacity() const;
  void setMaxCapacity(int capacity);
  qint64 dedupWindowSecs() const;
  void setDedupWindowSecs(qint64 secs);

signals:
  /// 当新错误事件发生（未被 60s 去重聚合）时触发，UI 通知管理器据此弹出通知卡或弹窗
  void errorRaised(const paleo::services::ErrorEntry &entry);

  /// 当错误命中 60s 去重窗口时触发，更新聚合计数与时间戳，通知卡据此更新角标
  void errorAggregated(const paleo::services::ErrorEntry &entry);

  /// 错误历史被清空
  void historyCleared();

  /// 错误历史发生任何变更（新增、聚合、逐出或清空）
  void historyChanged();

private:
  mutable QMutex m_mutex;
  std::deque<ErrorEntry> m_history;                     ///< 环形队列（上限 500）
  std::unordered_map<std::string, qint64> m_keyToId;   ///< 去重键 -> entry.id 映射
  int m_maxCapacity = kDefaultMaxHistory;
  qint64 m_dedupWindowSecs = kDefaultDedupWindowSecs;
  qint64 m_nextId = 0;
};

} // namespace paleo::services

Q_DECLARE_METATYPE(paleo::services::ErrorEntry)
Q_DECLARE_METATYPE(paleo::services::ErrorLevel)
