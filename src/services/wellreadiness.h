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
//（连井剖面 / 地震剖面 / 地层对比 / 交会图；数据管理与几何类
// 面板不过滤——HY 验收 2026-11-01 口径）。「有时深表但口径未确认」不算
// 就绪：与消费端确认门（tdTableFor 元数据门）一致，宁缺勿假。
struct WellReadiness {
  bool hasCurves = false; // 有可用测井曲线（模型井 extra 标志 / 链接判定）
  bool tdUsable = false;  // 时深可用：模型侧已确认口径，或 legacy 链接表可插值
  bool hasTimeDepth = false; // 存在时深资料，与通过确认/校验分开
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

// A-W3：就绪判定的快照/计算拆分——catalog 只读部分（extra 标志、链接、
// 版本路径）在 catalog 所属线程一次取齐；模型侧只读会话与 legacy 时深
// 文件读取可用同一份输入在工作线程复算。computeReadiness 不碰活 catalog，
// 口径与 analysisReadiness 逐井一致（快照=纯 catalog 读，计算=纯文件/模型）。
struct WellReadinessInput {
  bool hasCurves = false;       // wellSupportsAnalysis(catalog, well, true) 快照
  qlonglong areaModelWellId = 0;
  QString legacyTimeDepthPath;  // 主 time_depth 链接的已决版本路径（无则空）
};
QHash<QString, WellReadinessInput> snapshotReadinessInputs(DataCatalog *catalog);
QHash<QString, WellReadiness> computeReadiness(
    const QHash<QString, WellReadinessInput> &inputs, const QString &projectDir);

} // namespace paleo
