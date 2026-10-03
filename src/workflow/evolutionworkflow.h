// 层：功能
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "../algorithms/evolution/types.h"

class QgisLayerService;
class DataCatalog;

// workflow/evolutionworkflow — 沉积体系多期演化分析（方向35）。
// 编排面：从图层清单解析两期 facies.<层位> 相多边形 → io 读取器取 DTO 与
// 格网口径戳 → algorithms/evolution 内核对比（不同源直接拒算）→
// 迁移矢量图层（evolution.vectors.<层位>，05_PaleoMap）+ 指标表 CSV 资产。
// 指标口径随 EvolutionResult::methodNote 透传，不做井控密度加权。
class EvolutionWorkflow : public QObject
{
  Q_OBJECT
  public:
    explicit EvolutionWorkflow( QgisLayerService *layers, QObject *parent = nullptr );

    // 派生产物登记通道（同 CompositionWorkflow 的 T26 纪律）。
    void setCatalog( DataCatalog *catalog, const QString &projectDir );

    // 相邻期对比：earlier（老/深）→ later（新/浅）。两个层位都须已有
    // facies.<层位> 声明；格网/坐标域不同源 → 拒算（error 带内核原因）。
    // params 词表：boundary_sample_spacing（<=0 自动=像元尺寸）、
    // max_boundary_samples（默认 4000）。
    bool analyzePair( const QString &earlierHorizon, const QString &laterHorizon,
                      const QVariantMap &params = QVariantMap(), QString *error = nullptr );

    // 全层位序：mappingHorizons()（浅→深）逐一取相邻对，对内 earlier=更深者。
    // 单对失败不中断其他对（failures 收集「earlyH→lateH: 原因」）。
    // 返回成功对数；layers 未绑定 → false。
    int analyzeSequence( QStringList *failures = nullptr, QString *error = nullptr );

    // 最近一次成功 analyzePair 的内核指标（报告/表格消费；未跑过为 InvalidInput）。
    paleo::evolution::EvolutionResult lastMetrics() const;
    // 本次会话累计的指标行（列名见 metricsCsvHeader()；序 = 分析先后）。
    static QStringList metricsCsvHeader();
    QVariantList metricsRows() const { return m_metricsRows; }

    QgisLayerService *layerService() const;
    DataCatalog *catalog() const;
    QString projectDir() const { return m_projectDir; }

  signals:
    void pairAnalyzed( const QString &earlierHorizon, const QString &laterHorizon,
                       const QString &vectorLayerId );
    void analysisFailed( const QString &earlierHorizon, const QString &laterHorizon,
                         const QString &error );
    void sequenceDone( int analyzedPairs );

  private:
    // 指标行 → CSV 文本（含表头；列序与 metricsCsvHeader() 一致）。
    QString buildMetricsCsv() const;

    QPointer<QgisLayerService> m_layers;
    QPointer<DataCatalog> m_catalog;
    QString m_projectDir;
    paleo::evolution::EvolutionResult m_last;
    QVariantList m_metricsRows;
};
