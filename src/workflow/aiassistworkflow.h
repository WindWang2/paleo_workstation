// 层：功能
#pragma once
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "../ai/horizonsuggest.h"
#include "../ai/tileinference.h"

class QgisLayerService;
class PaleoOnnxService;
class PaleoTaskService;
class PaleoTask;
class DataCatalog;

// workflow/ — AI 辅助解释编排（goal/ai-geological-assist 范围 2/3/4 落地点）。
// 语义红线：AI 结果永远是「建议」——追踪建议待解释员逐条接受，只有
// commitAccepted() 显式落盘；接受/否决本身不写任何解释数据（测试断言
// 提交前 catalog 零变更）。tile 分类产物是「派生栅格产品」（不是解释
// 写入），照 T26 纪律落 artifacts/derived + DERIVED 版本。
class AiAssistWorkflow : public QObject
{
  Q_OBJECT
  public:
    // fetch 语义见 ai/tileinference.h（读域含 halo；NaN=缺）。
    using GridFetch = std::function<bool( int, int, int, int, QVector<float> &, QString & )>;

    explicit AiAssistWorkflow( QgisLayerService *layers, QObject *parent = nullptr );

    void setOnnxService( PaleoOnnxService *onnx );
    // T26 登记通道（同 PredictionWorkflow::setCatalog）。
    void setCatalog( DataCatalog *catalog, const QString &projectDir );
    // 异步批量分类需要任务服务（startClassification 用；不绑仍可同步跑）。
    void setTaskService( PaleoTaskService *tasks );

    // ---- 相分类 tile 批量产品（范围 2+3）----
    // 产品三件套（全部 catalog DERIVED + 声明，group 03_Predict）：
    //   aifacies.<horizon>.<model>        Byte 相栅格（255=nodata）
    //   aifacies.<horizon>.<model>.masked Byte 低置信掩膜（conf<threshold→255）
    //   confidence.<horizon>.<model>      Float32 置信度伴生层（1−H/lnC；
    //                                       m2(A) 3b 留的真实置信度接入点）
    // lowConfidenceThreshold ∈ [0,1]：低于该值的相格在 masked 产品里掩成
    // nodata（低置信区像素被处理——断言面在测试）。
    bool classifyTiles( const QString &horizon, const QString &model,
                        int gridRows, int gridCols, int tileRows, int tileCols, int halo,
                        const GridFetch &fetch, double lowConfidenceThreshold,
                        QString *error = nullptr );
    QStringList lastProductLayerIds() const { return m_lastProductLayerIds; }
    // 方向61：上次成功产品的统计摘要（rows/cols/classes/类直方图/无数据数/
    // 有效均值置信/tilesDone/inferenceMs/layerIds）——聊天工具结果回灌用，
    // 从推理结果直接算，不回读栅格。失败/未跑过 = 空 object。
    QJsonObject lastProductStats() const { return m_lastStats; }

    // 异步批量分类（PaleoTaskService 池 + 协作取消）：推理跑在任务线程，
    // 栅格落盘/登记/声明收尾在主线程（QgisLayerService 非线程安全）。
    // 取消 → 任务 Cancelled，不写任何产品（诚实无部分产品）。返回空指针
    // + *error 当任务服务未绑定或参数即刻可判非法。
    PaleoTask *startClassification( const QString &horizon, const QString &model,
                                    int gridRows, int gridCols, int tileRows, int tileCols,
                                    int halo, const GridFetch &fetch,
                                    double lowConfidenceThreshold, QString *error = nullptr );

    // 方向61：面向聊天/批处理调用方的现成取数——按 horizon.<target> 声明读
    // 层位栅格窗口（Float32；nodata/脏值归一为 NaN）。每次取数重新解析声明
    // 并开关数据集：fetch 在任务池线程跑，GDAL 数据集不跨线程共享，按调用
    // 开关是线程安全口径（声明变化也自然生效）。声明缺失时如实 err。
    static GridFetch horizonGridFetch( QgisLayerService *layers );

    // ---- 层位追踪建议（范围 4）----
    // 生成建议（引擎纯计算；不写任何持久状态）。out 同步返回 + 缓存待裁决。
    bool suggestTracking( const QString &horizon, const QString &model,
                          const QVector<TrackingSeed> &seeds, int windowSamples, int radius,
                          const TraceWindowFetcher &fetch,
                          QVector<TrackingSuggestion> *suggestions, QString *error = nullptr );
    // 方向61：异步版建议（同 startClassification 范式：池线程跑纯引擎，主线程
    // 收尾入裁决队列——m_suggestions 与信号发射都不是线程安全面）。引擎无协作
    // 取消钩子：requestCancel 后任务仍算完，但终态判 Cancelled、建议不进队
    // （语义作废；聊天执行器另有世代号守卫丢弃迟到结果）。
    PaleoTask *startSuggestion( const QString &horizon, const QString &model,
                                const QVector<TrackingSeed> &seeds, int windowSamples,
                                int radius, const TraceWindowFetcher &fetch,
                                QString *error = nullptr );
    // 纯内存状态转移（不落盘、不动 catalog）。
    bool acceptSuggestion( const QString &horizon, int inlineNo, int xlineNo );
    bool vetoSuggestion( const QString &horizon, int inlineNo, int xlineNo );
    int pendingCount( const QString &horizon ) const;   // 未裁决（不含种子）
    int acceptedCount( const QString &horizon ) const;  // 已接受（含种子锚点）
    QVector<TrackingSuggestion> acceptedSuggestions( const QString &horizon ) const;

    // 显式提交已接受建议：JSON 点集（inline/xline/sample/score/confidence，
    // 测网道空间——无诚实地图几何就不声明图层）落 artifacts/derived +
    // DERIVED 版本。提交前 catalog 零变更（解释员裁决语义的测试面）。
    bool commitAccepted( const QString &horizon, QString *error = nullptr );

  signals:
    void tileClassificationDone( const QString &horizon, const QStringList &layerIds );
    void tileClassificationFailed( const QString &horizon, const QString &error );
    void suggestionsReady( const QString &horizon, int count );
    void acceptedCommitted( const QString &horizon, const QString &relativePath );

  private:
    struct SuggestionState
    {
      TrackingSuggestion suggestion;
      bool accepted = false;
    };
    bool writeTileProducts( const QString &horizon, const QString &model,
                            const TileInferenceResult &result,
                            double lowConfidenceThreshold, QString *error );

    QgisLayerService *m_layers = nullptr;
    PaleoOnnxService *m_onnx = nullptr;
    PaleoTaskService *m_tasks = nullptr;
    DataCatalog *m_catalog = nullptr;
    QString m_projectDir;
    QStringList m_lastProductLayerIds;
    QJsonObject m_lastStats;
    QHash<QString, QVector<SuggestionState>> m_suggestions; // key = horizon
};
