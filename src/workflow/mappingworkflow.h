#pragma once
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QList>

#include <cmath>

#include "../domain/types.h"

class ConstraintWorkflow;
class CompositionWorkflow;
class QgisLayerService;
class ProjectDataFacade;
struct ValidationIssue;

// workflow/mappingworkflow — 阶段C「只编 D61」的链路编排（PROJECT_AREA_PLAN §5C
// autoplan 口径）。
//
// 等厚栅格（米）= (D62 时间栅格 − D61 时间栅格)/2000 × 像元处层间速度 Vint。
// Vint 由每口井的时深表插值得到：Vint = (TVD_D62 − TVD_D61)/(dt_ms/2000)，
// dt_ms = D62 时间 − D61 时间。Vint 用 power-2 IDW（与
// paleo:paleo_constraint_idw 同权重）插到 D61 既有网格的像元中心——不外扩、
// 不传约束线；落在贡献井分层点凸包之外的像元为 nodata(-9999)。
// 两张层位栅格尺寸/geotransform/nodata 不一致 → 不写等厚。贡献井 <3 或凸包
// 无面积 → 不写栅格；层位栅格缺失时才退回「井点厚度」IDW。
// 本链绝不调用 deriveFaciesPolygons——厚度栅格不是相编码。

// 一口井的厚度控制点（局部米坐标）——井点厚度退回路径与测试用。
struct ThicknessPoint
{
  QString wellId;
  QString wellName;
  double x = 0.0;
  double y = 0.0;
  double thickness = 0.0; // TVD(基面) − TVD(层位)，米
};

// 一口井的 D61→D62 厚度样本（约束页面板逐井渲染：井名/D61 TVD/D62 TVD/
// 层间速度或原因）。contributing=false 时 reason 写明原因。
struct ThicknessSample
{
  QString wellId;
  QString wellName;
  double x = qQNaN();          // 分层点坐标（top X/Y；缺省退井口）
  double y = qQNaN();
  double tvdTop = qQNaN();     // 层位 TVD（D61），米
  double tvdBase = qQNaN();    // 基面 TVD（D62），米
  double thickness = qQNaN();  // tvdBase − tvdTop，米（退回 IDW 的值）
  double dtMs = qQNaN();       // D62−D61 双程时间差，毫秒（TD 表插值）
  double vint = qQNaN();       // 层间速度 m/s = thickness/(dtMs/2000)
  bool contributing = false;   // vint 有效且 dtMs>0、TVD 差>0、坐标可定位
  QString reason;              // 不贡献时的原因文案（已 tr）
};

class MappingWorkflow : public QObject
{
  Q_OBJECT
  public:
    MappingWorkflow( ConstraintWorkflow *constraints, CompositionWorkflow *compose,
                     QgisLayerService *layers, QObject *parent = nullptr );

    void setProjectData( ProjectDataFacade *projectData );

    // 逐井厚度样本：层位与基面的 TVD + TD 插值时间 → Vint（或原因）。
    // 每口井都有一行；缺分层/缺 TVD/TD 失败/dt≤0/TVD 差≤0 都写明 reason。
    QVector<ThicknessSample> computeThicknessSamples( const QString &horizon,
                                                      const QString &baseHorizon,
                                                      QString *error = nullptr );

    // 井点厚度（TVD 差，米）：退回路径与测试用；缺任一分层/TVD 的井跳过并计入
    // *skipped（不造假厚度）。缺项目数据/层位 → 空表 + error。
    QVector<ThicknessPoint> computeThickness( const QString &horizon, const QString &baseHorizon,
                                              int *skipped, QString *error = nullptr );

    // 完整链（autoplan §5C）：等厚 = 双程时差栅格 × IDW²(Vint)，写在 D61 网格上，
    // 凸包外 -9999。层位栅格缺失 → 退回井点厚度 IDW「井点厚度（米，无层位栅格）」。
    // 成功声明「D61–D62 等厚（米）」并发 chainDone(horizon, layerId)。
    bool runThicknessChain( const QString &horizon, QString *error = nullptr );

    // 最近一次厚度评估的行表/文案（约束页面板渲染源；同时镜像到
    // ConstraintWorkflow 的 paleo.thickness.* 动态属性——面板只持有它）。
    QVariantList thicknessSampleRows() const;
    QString thicknessSampleMessage() const;

  signals:
    void chainDone( const QString &horizon, const QString &layerId );
    void chainFailed( const QString &horizon, const QString &error );

  private:
    void publishThicknessSamples( const QVector<ThicknessSample> &samples,
                                  const QString &message );
    // wells.thickness.<h> 井点图层：厚度样本的井位/井名（PDF 标注与图面定位用）。
    // 声明失败不失败链路——厚度栅格本身已经产出；只记警告。
    void declareThicknessWellsLayer( const QString &horizon,
                                     const QVector<ThicknessSample> &samples );

    ConstraintWorkflow *m_constraints = nullptr;
    CompositionWorkflow *m_compose = nullptr;
    QgisLayerService *m_layers = nullptr;
    ProjectDataFacade *m_projectData = nullptr;
};

// 阶段C验证 — 井上 D61 时间残差（PROJECT_AREA_PLAN §5C autoplan 口径）。
// 每口井一行（不只超限井）：井上时间由 TD 表插值（TimeDepthTool，TVD 优先、
// MD 兜底），栅格值取包含井位的像元（左闭右开；x==12793→最后一列，y==0→最后
// 一行）。残差 = 井时间 − 栅格时间，保留符号；|r|>thresholdMs → Exceeds。
struct TimeResidualRow
{
  enum class Status
  {
    Pass,        // 数值残差且 |r|<=阈值
    Exceeds,     // 数值残差且 |r|>阈值
    Warn,        // 井位不在测网内 / 井位落在空道（警告行，不算数值残差）
    NotComputed, // 无分层 / TD 原因（无时深表/超出时深表/时深表无序）
  };
  QString wellId;
  QString wellName;
  Status status = Status::NotComputed;
  double x = qQNaN();          // 采样点（分层 X/Y → 井口兜底）
  double y = qQNaN();
  double timeMs = qQNaN();     // 井上 D61 时间（TD 插值）
  double rasterMs = qQNaN();   // D61 栅格在井位处的时间
  double residualMs = qQNaN(); // 有符号残差（仅数值行）
  int inlineNo = -1;           // 目标测线（三视图联动），-1 未知
  QString reason;              // 非数值行的原因文案（已 tr）
};

// 逐井残差行；无门面/无栅格/栅格打不开 → 空表（本检查不适用）。
QList<TimeResidualRow> computeTimeResiduals( const ProjectDataFacade *projectData,
                                             const QString &horizon, double thresholdMs );
