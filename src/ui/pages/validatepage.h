// 层：视图
#pragma once

#include <QPointer>
#include <QWidget>
#include <QString>
#include <QVariantMap>

class ValidationWorkflow;



// ④验证 — run button + issues table + locate intent.
class ValidatePage : public QWidget
{
  Q_OBJECT
  public:
    ValidatePage(ValidationWorkflow *wf, QWidget *parent = nullptr);
    void populate();                              // run validate(), fill table
    // 残差表渲染（populate 复用）：行 map 契约 well_name/status/residual_ms/
    // reason/threshold_ms…。独立成静态面供测试直灌（workflow 无注入点时
    // validate() 会清空 residualRows 属性）。
    static void fillResidualTable(class QTableWidget *table, const QVariantList &rows);
  signals:
    // wave/mapping-pipeline：payload 携带三视图联动所需的机器字段（来自
    // ValidationIssue::wellId + details）：wellId、horizon、inline、time_ms
    // 等；非残差问题 payload 为空表。layerId 仍用于地图缩放。
    void locateRequested(const QString &layerId, const QString &wktLocation,
                         const QVariantMap &payload);
    // 「在数据页看这条剖面」（预览壳重排）：payload 同 locateRequested——
    // inline 是目标测线号、time_ms 是目标时间。shell 负责换页+打开剖面标签。
    void seismicSectionRequested(const QVariantMap &payload);
};
