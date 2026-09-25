#pragma once
#include <QObject>
#include <QString>
#include <QVector>

#include "../domain/types.h"

class ConstraintWorkflow;
class CompositionWorkflow;
class QgisLayerService;
class ProjectDataFacade;
struct ValidationIssue;

// workflow/mappingworkflow — 阶段C「只编 D61」的链路编排（PROJECT_AREA_PLAN §5C）。
// 结构面 = 层位 DERIVED 时间栅格；单因素 = 井上 D61→D62 的 TVD 厚度，
// 在测网网格上跑现有约束 IDW（像元 = 测网像元，凸包裁剪沿用约束图层），
// 再以厚度栅格为输入走 paleo:paleo_facies_polygonize 得到可编辑相多边形。
// 相序规则融合不在本阶段（plan §6）。
//
// UI 接线（ComposePage 的「厚度→IDW→转相面」按钮）只调 runThicknessChain；
// 状态文案经 chainDone/chainFailed 信号落到页面的 statusLabel。

// 一口井的厚度控制点（局部米坐标）。
struct ThicknessPoint
{
  QString wellId;
  QString wellName;
  double x = 0.0;
  double y = 0.0;
  double thickness = 0.0; // TVD(基面) − TVD(层位)，米
};

class MappingWorkflow : public QObject
{
  Q_OBJECT
  public:
    MappingWorkflow( ConstraintWorkflow *constraints, CompositionWorkflow *compose,
                     QgisLayerService *layers, QObject *parent = nullptr );

    void setProjectData( ProjectDataFacade *projectData );

    // 逐井取 tops：层位与基面的 TVD 都在 → 厚度点；缺基面分层的井跳过并
    // 计入 *skipped（不造假厚度）。缺项目数据/层位 → 空表 + error。
    QVector<ThicknessPoint> computeThickness( const QString &horizon, const QString &baseHorizon,
                                              int *skipped, QString *error = nullptr );

    // 完整链：厚度点 → 声明 wells.thickness.<h> 点层 → runConstraintIDW
    // （CELL_SIZE = 测网像元，来自 horizonRasterDecl）→ deriveFaciesPolygons
    // （声明 facies.<h>）。成功发 chainDone(horizon, "facies.<h>")。
    bool runThicknessChain( const QString &horizon, QString *error = nullptr );

  signals:
    void chainDone( const QString &horizon, const QString &faciesLayerId );
    void chainFailed( const QString &horizon, const QString &error );

  private:
    ConstraintWorkflow *m_constraints = nullptr;
    CompositionWorkflow *m_compose = nullptr;
    QgisLayerService *m_layers = nullptr;
    ProjectDataFacade *m_projectData = nullptr;
};

// 阶段C验证 — 井上时间残差（PROJECT_AREA_PLAN §5C）。
// 对每口有 <horizon> 分层的井：用其 TD 表把分层 TVD 插成 TIME(ms)（分段
// 线性，井段外按端点段斜率外推），与层位 DERIVED 时间栅格在井位处的采样
// 值（最近像元）求差；|差| > thresholdMs 记一条 TIME_RESIDUAL 问题，携带
// wellId、井位坐标、目标测线（inline）、目标时间。无 TD 表的井记
// NO_TD_TABLE「无时深表」，不造假时间；井位无栅格采样或无分层的井不记
// 残差（与缺分层跳过同一纪律）。无栅格/无门面 → 空表。
QList<ValidationIssue> computeTimeResiduals( const ProjectDataFacade *projectData,
                                             const QString &horizon, double thresholdMs );
