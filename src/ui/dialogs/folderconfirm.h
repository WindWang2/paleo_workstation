// 层：视图
#pragma once

#include <functional>

#include <QMap>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include "../../domain/importrows.h" // FolderPreviewRow / FolderRowResult（domain 纯数据）

class QComboBox;
class QDialog;
class QTableWidget;

// ui/dialogs/folderconfirm — 「导入工区文件夹」确认对话框的视图件（W2：从
// 主窗下沉成独立对话框单元）。本命名空间只做渲染与输入收集：
//   · populateFolderConfirmTable —— 建行 + 类型下拉（锁定/跳过行如实灰显）；
//   · collectFolderTypeOverrides —— 收集用户改过的类型（override）；
//   · writeFolderRowResult —— 行结果写回 + 「重试」按钮回挂；
//   · folderImportSummaryText —— 汇总文案；
//   · buildFolderConfirmDialog —— 整体搭建。全部导入动作经 Hooks 回调
//     出壳（壳把回调接到 FolderImportWorkflow/预览/数据页上）——本文件
//     不知道任何 io/workflow 类型。
namespace PaleoFolderConfirm
{

// 壳注入的视图出口。全部可空——空的出口对应行为静默跳过（测试场景可只给
// importAll 断言用）。
struct Hooks
{
  std::function<void()> cancelImport;
  // 行重试：对话框已按当前下拉算好 forceType；实现走 FolderImportWorkflow。
  std::function<FolderRowResult(const QString &path, const QString &forceType)>
      importRow;
  std::function<FolderRowResult(const QString &path, const QString &forceType,
                                const QString &wellTarget,
                                const QString &expectedHash)> importRowWithWell;
  // 整批导入：done(rows, importErr) 总在 GUI 线程回调。
  std::function<void(const QMap<QString, QString> &overrides,
                     std::function<void(const QVector<FolderRowResult> &,
                                        const QString &, bool cancelled)> done)>
      importAll;
  // （T2 跳过策略，additive）带「仍导入」改判的整批导入：forceImportPaths
  // 是用户对「重复→跳过」行点了「仍导入」的源路径集合。给了这个出口，
  // 确认表才会挂「仍导入」按钮；只给 importAll 的旧壳看不到该能力。
  std::function<void(const QMap<QString, QString> &overrides,
                     const QStringList &forceImportPaths,
                     std::function<void(const QVector<FolderRowResult> &,
                                        const QString &, bool cancelled)> done)>
      importAllWithForced;
  std::function<void(const QMap<QString, QString> &overrides,
                     const QStringList &forceImportPaths,
                     const QMap<QString, QString> &wellTargets,
                     const QMap<QString, QString> &expectedHashes,
                     const QStringList &skippedPaths,
                     const QStringList &previewedPaths,
                     std::function<void(const QVector<FolderRowResult> &,
                                        const QString &, bool cancelled)> done)>
      importAllWithWells;
  // 确认完成后按行源文件名反查刚入库的井口资产（空 → 不开预览）。
  std::function<QString(const QString &rowPath)> importedWellHead;
  // 「查看未决」→ 数据页切未决过滤。
  std::function<void()> showUnresolved;
  // 井口入库 → 开预览标签。
  std::function<void(const QString &assetId)> previewAsset;
  // 导入完成 → 回写工程 sourceArea（目录匹配由实现侧自决）。
  std::function<void(const QVariantMap &stats)> stampSourceArea;
};

// 类型 id → 中文标签（词表外裸显 id）。
QString folderTypeLabel(const QString &type);
// 行默认显示类型：HZ28-6-1 固定辅助 → 「参考」；「参考资料」目录内井类/
// 未判内容默认「参考」（可改）；其余行显示分类器原类型。
QString folderRowDisplayType(const QString &path, const QString &classifiedType);
// T22/单文件确认共用的 CRS 契约句。
QString engineeringCrsSentence();
// 建确认表行（锁定行禁用下拉 + tooltip、跳过行灰显）；combosOut 收每行下拉。
void populateFolderConfirmTable(QTableWidget *table, const QString &rootDir,
                                const QVector<FolderPreviewRow> &rows,
                                QVector<QComboBox *> *combosOut);
// 覆盖收集：只看启用行；保留不同于检测类型或默认类型的合法显式选择。
QMap<QString, QString>
collectFolderTypeOverrides(const QTableWidget *table,
                           const QVector<FolderPreviewRow> &rows,
                           const QVector<QComboBox *> &combos);
// 行结果写回（实体列 + 结果列）；Failed 且给了 onRetry → 结果列挂「重试」按钮。
void writeFolderRowResult(QTableWidget *table, int row, const FolderRowResult &res,
                          const std::function<void(int)> &onRetry);
// 汇总文案：「入库 n，未决 n，失败 n（，跳过 n）」——D3 保留第四计数。
QString folderImportSummaryText(const QVector<FolderRowResult> &rows);
// 预览期估算文案（T2 大小估算）：「将导入 n 项 · 约 12.4 MB（重复跳过 n，
// 枚举跳过 n）」——大小未知的行计入「大小未知」；无行回空串。
QString folderEstimateText(const QVector<FolderPreviewRow> &rows,
                           const QStringList &skippedPaths = {},
                           const QStringList &forceImportPaths = {});
// 确认对话框整体搭建（类型表 + CRS 说明句 + 确认/取消/查看未决 + 行重试
// 接线）。hooks 为空出口对应行为跳过；dlg.exec() 由调用方负责。
void buildFolderConfirmDialog(QDialog *dlg, const QString &dir,
                              const QVector<FolderPreviewRow> &preview,
                              const Hooks &hooks);
// 方向 D1：「复制进工程」勾选状态（folderImportCopyCheck，确认按钮区上方，
// 默认不勾选）。确认动作在对话框内经 hooks 发起，壳在 importAllWithWells
// 出口里读它调 FolderImportWorkflow::setLinkExternal(!copyIntoProject)——
// 不勾选 = 只建索引（外链），勾选 = 旧受管复制语义。dlg 未经
// buildFolderConfirmDialog 搭建（或勾选框已销毁）→ false（保持只建索引默认）。
bool folderImportCopyIntoProject(const QDialog *dlg);

// 「显示名」列（folderTable 末列，可编辑）的覆盖收集：源路径 → 编辑后名称。
// 只收录「trim 后非空且不同于原文件 basename」的行——未动的行不发 override
//（io 侧空白值同样回退 basename）。显示名只改数据页/确认表展示名，物理
// 文件名恒不动。壳在 importAllWithWells 出口里读它（同
// folderImportCopyIntoProject 模式）透传 FolderImportWorkflow::importFolder/
// importFiles 的 nameOverrides 尾参；未勾选跳过的行即使编辑了也不会导入
//（io 侧 skippedPaths 先行跳过，map 里的多余条目无害）。
QMap<QString, QString> folderImportNameOverrides(const QDialog *dlg);

} // namespace PaleoFolderConfirm
