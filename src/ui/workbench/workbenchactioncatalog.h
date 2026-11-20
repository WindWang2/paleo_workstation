// 层：视图
#pragma once

#include <QString>
#include <QVector>

// workbenchactioncatalog — 制图工作台动作的**单份**内部描述（C-W1，#983）。
// 此前 knownActions（字符串集合）、分派族（handleCommand 依次试探四段
// Skip 链）、准入条件（散落在各分支内的判空）和成功文案链是四张平行表，
// 新增动作漏改其一不会被发现。现在登记、分族、准入（必需能力位＋副作用
// 分类）和基础结果策略都从这张表派生：
//   - workbenchcommandregistry.h 的 knownActions() 由本表派生；
//   - MappingWorkbenchCommandController 按 spec.family 直达唯一处理入口，
//     不再「先试三族再兜底」；族内漏写分支 → 运行时落入「未被声明族受理」
//     的显式失败（与未知动作同样入账 ErrorHub）；
//   - C-W2 准入适配器按 spec.caps／spec.effect 判断依赖缺失与只读拒绝。
// 特殊结果（量化详情、后台提交、删除仅移除声明）仍由对应处理分支在运行
// 时决定；表的 Outcome 只是入口文案链的基线口径。
namespace paleo::workbenchcmd
{

// 处理入口（mappingworkbenchcommandcontroller.cpp 内的四个分派函数）。
enum class Family : unsigned char
{
    EditFlow,   // runMappingEditFlow：解释依据/拓扑检查/导出（分支自行收尾）
    Query,      // runWorkbenchQueryFamily：查询/预测/重算/取消
    Edit,       // runWorkbenchEditFamily：标注/层级/证据/副本/保存
    Lifecycle,  // runWorkbenchLifecycleFamily：新建/删除/导入/生成
};

// 副作用分类（准入用）：Read = 浏览/查询/导航/请求取消（只读工程放行）；
// Mutating = 修改工程数据或产出新成果/新图层（只读工程拒绝）。
// 注意 Query 族 ≠ 只读：predict/recompute/impactAnalysis 等会产生结果的操作
// 按实际副作用记 Mutating，不按族名或 CommandKind 名称推断。
enum class Effect : unsigned char
{
    Read,
    Mutating,
};

// 基础结果策略（入口文案链的派生口径）：
//   Standard  — 通用成功文案（分支可有专属覆盖）；
//   Queued    — 后台任务提交（任务中心可见/可取消）；
//   Detailed  — 分支产出量化详情文案（statusDetail）；
//   Removal   — 删除：按 declarationOnly 双文案；
//   FlowOwned — EditFlow 分支自行 notify 收尾，入口不再覆盖。
enum class Outcome : unsigned char
{
    Standard,
    Queued,
    Detailed,
    Removal,
    FlowOwned,
};

// 必需上下文能力位（MappingWorkbenchCommandController::Context 的依赖声明，
// C-W4）。基础依赖（workbench/page/dialogParent/statusBar）由入口守卫统一
// 检查，不进能力位；这里只列**该动作分支会直接使用**的可缺失项——缺失时
// 准入拒绝并给出具体原因，而不是空 std::function 调用（bad_function_call）
// 或静默成功。可选项（如 factorBusy/taskService/edit 对 copy/save 的增益
// 分支）不声明：缺了只影响增强路径。
enum Cap : unsigned
{
    kNone = 0,
    kCanvas = 1 << 0,         // canvasCtl（画布冻结/当前图层）
    kLayerService = 1 << 1,   // layerSvc（图层实例化/查询）
    kConstraintPage = 1 << 2, // constraint（约束绘制入口）
    kWellPanel = 1 << 3,      // wellPanel（删除后清理修订面板）
    kWellDock = 1 << 4,       // wellDock（删除后收起 dock）
    kImpact = 1 << 5,         // impact（受控重算读影响报告）
    kShownHistory = 1 << 6,   // shown（删除后清理已显示登记）
    kShow = 1 << 7,           // show 回调（上图）
    kCompare = 1 << 8,        // compare 回调（参考面板）
    kLegend = 1 << 9,         // legend 回调（图例刷新）
    kOpenWells = 1 << 10,     // openWells 回调（井道面板）
    kBeginEdit = 1 << 11,     // beginEdit 回调（编辑会话）
    kNavTable = 1 << 12,      // navigation.showAttributeTable
    kNavGraph = 1 << 13,      // navigation.openGraph
    kNavCatalog = 1 << 14,    // navigation.focusCatalogVersion
    kNavExportMap = 1 << 15,  // navigation.exportMap
};

struct ActionSpec
{
    const char *id;   // 稳定动作 ID（commandRequested 协议字符串）
    Family family;    // 唯一处理入口
    Effect effect;    // 副作用分类（准入）
    Outcome outcome;  // 基础结果策略（文案链）
    unsigned caps;    // 必需上下文能力位（准入）
};

// 全量动作目录（顺序无关；id 必须唯一——catalogIntegrity 校验）。
const QVector<ActionSpec> &catalog();

// 按动作 ID 查描述；未登记返回 nullptr（未知动作拒绝路径）。
const ActionSpec *find(const QString &action);

// 能力位缺失的人类可读原因（顿号串联具体依赖名）；caps 无缺失返回空串。
QString missingCapsReason(unsigned caps);

// 表完整性自检（供测试与装配期断言）：id 唯一、枚举值合法、无未知位。
// 返回问题描述列表；空列表 = 通过。
QStringList catalogIntegrity();

} // namespace paleo::workbenchcmd
