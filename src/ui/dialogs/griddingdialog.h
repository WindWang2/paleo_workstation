// 层：视图
#pragma once
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

class QWidget;

// ui/dialogs/griddingdialog — 「网格化…」参数表（goal/gridding-surface-ops）。
// 本命名空间只做渲染与输入收集：参数表 + 实时网格规模反馈（超预算禁 OK，
// 审计 #33 口径的前置防御），不接任何 workflow/io 类型（壳负责接线）。
namespace PaleoGriddingDialog
{

// 壳侧已知的上下文（散点范围/建议像元/约束源有无）。
struct RequestContext
{
  QString horizonName;
  double minX = 0, maxX = 0, minY = 0, maxY = 0; // 散点范围（平面单位）
  bool hasHeaderCell = false;                    // 层位头 Grid_size 可推出像元
  double headerCellSize = 0;
  bool hasConstraints = false;                   // ConstraintStore 有 break_line
};

// 用户确认的参数（与 SurfaceGriddingWorkflow::Options 同形，纯数据镜像）。
struct Request
{
  double cellSize = 25.0;
  double tension = 0.25;
  int maxSweeps = 500;
  bool useBarriers = true;
  bool runCrossValidation = false;
  int cvPoints = 16;
};

// 模态弹参数表；返回 true = 用户确认（*out 填参数）。
bool prompt(QWidget *parent, const RequestContext &ctx, Request *out);

// ---- 面运算（等厚/体积）对话框 ---------------------------------------------
// 选择顶/底结构面（候选来自声明栅格清单，壳传入「显示名 → 源路径」对），
// 可选写出受管等厚栅格。
struct IsopachSelection
{
  int topIndex = -1;    // 候选表索引
  int baseIndex = -1;
  bool writeManaged = false;
};

// 候选 = (显示名, 源路径)。返回 true = 确认。
bool promptIsopach(QWidget *parent, const QVector<QPair<QString, QString>> &candidates,
                   IsopachSelection *out);

// 显示名 → 受管资产命名安全段（非 [A-Za-z0-9_.-] 折成下划线，防路径逃逸）。
QString safeAssetLabel(const QString &displayName);

// 体积报告展示 + 「导出 CSV」（QFileDialog 存盘由本视图完成，返回路径）。
// metrics = (标签, 值文本) 行；csv 为完整 CSV 文本。
QString showVolumeReport(QWidget *parent, const QString &title,
                         const QVector<QPair<QString, QString>> &metrics, const QString &csv);

} // namespace PaleoGriddingDialog
