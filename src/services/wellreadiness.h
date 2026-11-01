// 层：数据
#pragma once
#include <QHash>
#include <QSet>
#include <QString>
#include <string>
class DataCatalog;
namespace paleo::data { class DataService; }

namespace paleo {

// 井就绪度（可分析判定）：曲线 + 可用时深双全才进下游分析的选井面
//（连井剖面 / 地震剖面 / 井预测 / 地层对比 / 交会图；数据管理与几何类
// 面板不过滤——HY 验收 2026-11-01 口径）。「有时深表但口径未确认」不算
// 就绪：与消费端确认门（tdTableFor 元数据门）一致，宁缺勿假。
struct WellReadiness {
  bool hasCurves = false; // 有可用测井曲线（模型井 extra 标志 / 链接判定）
  bool tdUsable = false;  // 时深可用：模型侧已确认口径，或 legacy 链接表可插值
  bool analysisReady() const { return hasCurves && tdUsable; }
};

// 时深口径已确认（单位/单双程/基准）且有 ≥2 有限样点的模型井集合——
// 就绪表与模型原生链（地层对比）共用的确认门。
QSet<qlonglong> usableTimeDepthWells(data::DataService &service,
                                     std::string *error = nullptr);

// 全工区井就绪表（entityId → 就绪度）。stamp 缓存（data.sqlite(+wal)
// 指纹）：时深确认 / 导入写库后自动失效重查，无需信号（areawellprojection
// 同款惯例）。无工区模型时模型侧恒 false，legacy 链接侧照判。
QHash<QString, WellReadiness> analysisReadiness(DataCatalog *catalog,
                                                const QString &projectDir);

} // namespace paleo
