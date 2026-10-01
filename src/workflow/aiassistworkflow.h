// 层：功能
#pragma once
#include <QHash>
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

    // 异步批量分类（PaleoTaskService 池 + 协作取消）：推理跑在任务线程，
    // 栅格落盘/登记/声明收尾在主线程（QgisLayerService 非线程安全）。
    // 取消 → 任务 Cancelled，不写任何产品（诚实无部分产品）。返回空指针
    // + *error 当任务服务未绑定或参数即刻可判非法。
    PaleoTask *startClassification( const QString &horizon, const QString &model,
                                    int gridRows, int gridCols, int tileRows, int tileCols,
                                    int halo, const GridFetch &fetch,
                                    double lowConfidenceThreshold, QString *error = nullptr );

    // ---- 层位追踪建议（范围 4）----
    // 生成建议（引擎纯计算；不写任何持久状态）。out 同步返回 + 缓存待裁决。
    bool suggestTracking( const QString &horizon, const QString &model,
                          const QVector<TrackingSeed> &seeds, int windowSamples, int radius,
                          const TraceWindowFetcher &fetch,
                          QVector<TrackingSuggestion> *suggestions, QString *error = nullptr );
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
    QHash<QString, QVector<SuggestionState>> m_suggestions; // key = horizon
};
