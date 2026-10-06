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
// - 去重键缺省 = level|source|title|text（文本完全相同才算同一错误）。
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
                bool severe = false);

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
