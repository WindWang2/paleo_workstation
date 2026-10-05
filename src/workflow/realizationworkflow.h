// 层：功能
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

class DataCatalog;
class QgisLayerService;

namespace paleo::ensemble
{
struct StatsRequest;
}

// workflow/realizationworkflow —— realization 集合的登记/派生编排（方向 47）。
// 契约见 src/catalog/realizationset.h 头注；本类只做编排不碰数值核
//（统计走 algorithms/ensemblestats，集合查询走 catalog/realizationset）。
//
// 线程/事务纪律与 ConstraintWorkflow 同：publish/derive 在 catalog 所属线程
// 同步跑；成员逐个 commit——中途失败留下的不完整集合由缺号检测如实呈现
// （catalog 成员版本是不可变行，无半成品回滚——契约允许的退化形态）。
// 内存纪律：统计派生逐成员惰性读盘（均值/标准差一次一成员；分位数按行带
// 集齐切片），不全体驻留。
class RealizationWorkflow : public QObject
{
  Q_OBJECT
  public:
    explicit RealizationWorkflow( QObject *parent = nullptr );

    // catalog/projectDir 供登记；layers 供图层声明（可空——空则只登记不声明，
    // 集合仍是完整 catalog 产物，图层可后补）。
    void bind( DataCatalog *catalog, const QString &projectDir,
               QgisLayerService *layers );
    bool isBound() const { return m_catalog != nullptr && !m_projectDir.isEmpty(); }

    // 计算段已写好的成员栅格（Float32 GeoTIFF；nodata=-9999 口径与
    // constraintfactorjobs 的 writeFloatRaster 一致）。
    struct MemberInput
    {
      QString tempPath;
      quint64 seed = 0; // 集合基种子（SGS 共享随机流——成员间靠 index 区分）
    };

    // 发布集合：新建 realization_set 资产（displayName = title + 运行戳——
    // 每次生成都得新集合，重跑不混入旧集合），成员拷入受管路径并登记为
    // DERIVED 版本（extra 带契约键；parentVersionIds = 输入版本）。
    // 绑定图层服务时每成员顺带声明图层（realset.<setId>.m<index>，
    // 组 "04_SingleFactor/Realizations"）。
    // 空成员表 → 拒绝；中途失败 → 已提交成员不回滚（缺号检测接管诚实面），
    // 返回空串 + error 点名失败成员。
    QString publishSet( const QString &title, const QString &horizon,
                        const QVector<MemberInput> &members,
                        const QStringList &inputParentVersionIds,
                        const QVariantMap &sharedExtra, QString *error = nullptr );

    // 派生统计面：在场成员 ≥2 才做（单成员/零成员 → 如实拒绝，无不确定性可产）。
    // 成员文件读不到 → error 点名缺席成员；成员网格不一致 → 拒绝（不写假面）。
    // 每统计口径落独立 realization_stat 资产 DERIVED 版本（parents = 全部
    // 在场成员版本），并声明图层 realset.<setId>.stat.<token>。
    // 已存在的口径不重复派生（重派生需调用方先清理——幂等面）。
    bool deriveStatistics( const QString &setId, const paleo::ensemble::StatsRequest &want,
                           QString *error = nullptr );

    // 全口径便捷面：均值+总体标准差+P10+P90（壳层/UI 意图不带算法类型——
    // StatsRequest 属 algorithms 层，视图侧不直持）。
    bool deriveAllStatistics( const QString &setId, QString *error = nullptr );

    // 两集合均值差（A−B）：缺均值面的集合先补派生（派生本身记档）。
    // 网格不一致 → 拒绝。产物 = realization_diff 资产版本（parents =
    // 两侧均值版本）+ 图层 realsetdiff.<A>.<B>。成功发 realizationDiffReady。
    bool differenceOfMeans( const QString &setIdA, const QString &setIdB,
                            QString *error = nullptr );

    // ---- 图层 id 词表（UI/测试同源）----
    static QString memberLayerId( const QString &setId, int index );
    static QString statLayerId( const QString &setId, const QString &token );
    static QString diffLayerId( const QString &setIdA, const QString &setIdB );

    // 动画帧序：成员图层 id 按 index 升序（缺号位自然跳过）。
    QStringList memberFrameOrder( const QString &setId ) const;

    // 成员 index → 受管绝对路径（缺 → 空串）。
    QString memberRasterPath( const QString &setId, int index ) const;

    // 集合最近一次派生的统计面图层 id 表（按 token 词表序）。
    QStringList statLayerIds( const QString &setId ) const;

  signals:
    void realizationSetPublished( const QString &setId, int memberCount );
    void realizationStatsDerived( const QString &setId, const QStringList &tokens );
    void realizationDiffReady( const QString &setIdA, const QString &setIdB,
                               const QString &layerId );

  private:
    QPointer<DataCatalog> m_catalog;
    QPointer<QgisLayerService> m_layers;
    QString m_projectDir;
};
