// 层：数据
#pragma once
#include <QDateTime>
#include <QHash>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

// 方向64：统一错误通道（服务层，无 QtWidgets）。
//
// 契约（单点事实，DESIGN.md「错误呈现」节与此一致）：
// - 条目 = 级别（Info/Warning/Error）+ 来源域 + 标题 + 正文 + 首次/末次时间戳
//   + 去重键 + 聚合计数 + severe 标记（仅 Error 有意义：请求模态呈现）。
// - 去重窗口：同一去重键在「首次出现起 60s」内再次 raise 只聚合（count+1、
//   lastMs 刷新），不新增历史行，errorRaised 的 firstInWindow=false；窗口
//   以首次出现计（固定窗口，不随重复滑动）——持续风暴下每 60s 至多一次
//   firstInWindow=true，呈现层据此「同键 60s 单弹」。
// - 去重键缺省 = level|source|title|text（文本完全相同才算同一错误；severe
//   不入键——同文本的 severe 与非 severe Error 视为同一错误，severe 只升不降）。
// - historyOnly：调用方已自行呈现（如保留清单内的模态），本次只记历史；
//   随 errorRaised 快照带出，呈现层见此标记不再呈现。
// - 环形历史上限 kCapacity=500：满后逐出最旧条目（其去重索引同步作废）。
// - 线程：raise/查询/清空均加锁，任意线程可调；errorRaised 在 raise 的
//   调用线程发出，跨线程接收者经 AutoConnection 排队到其所在线程。
// - 全局实例：组装根（AppContext）构造并 installGlobal；视图层经
//   ErrorHub::global() 取用。未安装时 global()==nullptr，调用方自行降级。
class ErrorHub : public QObject
{
    Q_OBJECT
public:
    enum class Level { Info = 0, Warning = 1, Error = 2 };
    Q_ENUM(Level)

    struct Entry
    {
        quint64 id = 0;          // 单调递增序号（clear 不回卷）
        Level level = Level::Info;
        QString source;          // 来源域，如 "welltops" / "mapping"
        QString title;
        QString text;
        QString dedupKey;
        qint64 firstMs = 0;      // epoch ms
        qint64 lastMs = 0;
        int count = 0;           // 聚合计数（≥1）
        bool severe = false;     // Error 级请求模态
        bool historyOnly = false;// 本次只入账历史、不请求呈现（调用方已自行呈现）
    };

    struct Filter
    {
        int levelMask = 0x7;     // bit(Level)
        QString source;          // 空 = 不过滤；否则精确匹配
        QString contains;        // 空 = 不过滤；title/text/source 子串（大小写不敏感）
    };

    static constexpr int kCapacity = 500;
    static constexpr qint64 kDedupWindowMs = 60 * 1000;

    explicit ErrorHub(QObject *parent = nullptr);
    ~ErrorHub() override;

    // 返回条目（聚合后状态）。text 为空时仍记录（不吞），去重键照常计算。
    Entry raise(Level level, const QString &source, const QString &title,
                const QString &text, const QString &dedupKey = QString(),
                bool severe = false, bool historyOnly = false);

    QVector<Entry> entries() const;                 // 旧 → 新
    QVector<Entry> entries(const Filter &f) const;  // 旧 → 新
    int size() const;
    quint64 totalRaised() const;                    // 含聚合的 raise 总次数
    void clear();

    static QString defaultKey(Level level, const QString &source,
                              const QString &title, const QString &text);
    static QString levelName(Level level);          // 稳定英文名（日志/复制用）

    // 测试注入：时间源（epoch ms）。传空恢复系统时钟。
    void setClockForTest(std::function<qint64()> clock);

    static void installGlobal(ErrorHub *hub);
    static ErrorHub *global();

signals:
    void errorRaised(const ErrorHub::Entry &entry, bool firstInWindow);
    void historyCleared();

private:
    qint64 nowMs() const;
    Entry *findLive(quint64 id);

    mutable QMutex m_mutex;
    QVector<Entry> m_ring;               // 预分配 kCapacity
    int m_size = 0;
    quint64 m_nextId = 1;
    quint64 m_total = 0;
    QHash<QString, quint64> m_keyIndex;  // 去重键 → 条目 id
    std::function<qint64()> m_clock;
};

Q_DECLARE_METATYPE(ErrorHub::Entry)

// ============================================================================
// 方向54（#251）并行实现：paleo::services 命名空间的 ErrorHub。
// 与上方全局 ErrorHub 并存——notificationmanager/notificationcard/
// errorhistorymodel/errorhistorydock 一脉走此接口；合并期保留两套。
// ============================================================================

#include <deque>
#include <optional>
#include <unordered_map>

namespace paleo::services {

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
