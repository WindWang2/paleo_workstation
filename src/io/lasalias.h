// 层：数据
#pragma once
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

// io/ — LAS 曲线助记符/单位别名归一（wave/io-perf-cache D1.7）。
// 工区 LAS 的道名五花八门（DT/AC/DT4P、GR/NGR/SGR、RHOB/DEN/ZDEN……），
// 消费方（关联曲线选择、预测输入道）需要问「这口井有没有声波/伽马」而不是
// 「有没有恰好叫 DT 的道」。API_ALIAS_MAP = 内置别名表；查表结果按输入串
// 记忆化（并发安全），重复归一零哈希开销。

struct LasAliasStats
{
    qint64 lookups = 0;
    qint64 memoHits = 0; // 命中记忆化表（未重走内置表）
    qint64 normalized = 0; // 输入 != 输出（发生了归一）
};

class LasAliasMap
{
  public:
    static LasAliasMap &shared();

    // 助记符 → 规范名（DEPT/DT/GR/RHOB/NPHI/SP/CALI/RT/RS/RXO/…）。
    // 未知名原样返回（大小写归一 + 去尾部数字修饰后仍未知 → 原文大写）。
    QString canonicalCurve(const QString &raw);
    // 单位 → 规范单位（m/ft/us/ms/g/api/ohmm/v/pct/gcm3/…）。
    QString canonicalUnit(const QString &raw);

    // 查询不计数（诊断/测试）：是否是已知别名。
    bool knowsCurve(const QString &raw) const;
    bool knowsUnit(const QString &raw) const;

    // 规范曲线族（canonicalCurve 的再归类：Depth/Sonic/Gamma/Density/
    // Neutron/SP/Caliper/Resistivity/Other）——预测输入道选择的口径。
    QString family(const QString &raw);

    LasAliasStats stats() const;
    void clearMemo(); // 测试钩子

  private:
    LasAliasMap();

    void buildTables();
    QString lookupNormalized(const QString &raw, const QHash<QString, QString> &table,
                             const QHash<QString, QString> &familyTable, bool isCurve);

    mutable QMutex m_mutex;
    QHash<QString, QString> m_curveTable;  // 大写归一后的别名 -> 规范名
    QHash<QString, QString> m_unitTable;   // 小写归一后的单位 -> 规范单位
    QHash<QString, QString> m_familyTable; // 规范名 -> 族
    QHash<QString, QString> m_curveMemo;   // 原输入串 -> 结果（记忆化）
    QHash<QString, QString> m_unitMemo;
    LasAliasStats m_stats;
};
