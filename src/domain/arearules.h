// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

// domain/ — 工程级参数 seam（wave4/area-parametrization，TODOS「第二工区参数化接缝」）。
// 本工区（project_area）的钉死值统一从这里读；默认值 = master 当前行为，
// 逐字节一致。四类参数：
//   1. 层序界面名单 —— dataimportservice.cpp isKnownSequenceBoundary（plan §1：
//      C3/C6/D53/D61/D62/D63/D71/D72；名单外层位产未决实体，不进编图 chip）。
//   2. 分类器目录规则 —— projectclassifier.cpp 的中文目录名→类型映射
//      （.dat 段规则表 / 「参考资料」目录 / HZ28-6-1 固定辅助）。
//   3. SEG-Y 道号索引约定 —— segyreader.cpp 的道头偏移（inline 字 188 恒定 →
//      道号序：inline = field-record base + 道号/N，crossline = CDP 字，plan §2/§7）。
//   4. ONNX 期望网格 —— D61 411×641（workflows.cpp 网格门同值）。
//
// 读取顺序：工程目录 project_area.json → 内置默认。缺文件/缺键 = 默认；
// 坏 JSON（语法错/类型错/未知键/越界值）如实报错拒用，不静默回退。
// 全局规则（扩展名→类型表、身份顺序、受管复制语义）不在此处——它们不是
// 每测区参数，分界见 docs/AREA_PARAMETERS.md。
namespace AreaRules
{

// .dat 路径段规则：路径段（小写化）精确命中 exactSegments、或包含
// segmentKeywords 中任一关键字、或文件名（小写化）包含 filenameKeywords
// 中任一关键字 → type。规则按表序先中先得；全不中 → tabular。
struct DatPathRule
{
  QStringList exactSegments;
  QStringList segmentKeywords;
  QStringList filenameKeywords;
  QString type; // projectClassifierTypes() 词表内
};

struct ClassifierRules
{
  QVector<DatPathRule> datPathRules; // 默认：时深(td)→层位→井分层→井位/wellhead
  QStringList referenceDirNames;     // 目录段名 → 默认「参考」角色（isDefaultReferencePath）
  QString fixedAuxiliaryNameStem;    // 基名含此串 → 固定辅助（isFixedAuxiliaryPath）；空 = 无
};

// SEG-Y 道号索引约定（0 基道头偏移）。inline 字在探测段恒定 → 道号序索引：
// inline = 首道 field-record 字（= 冻结 inlineMin，本工区 1315）+ 道号/N；
// crossline = 该道 CDP 字。inline 字可变 → 标准位直读。偏移上限 236（i32
// 字完整落在 240 字节道头内）。
struct SegyIndexing
{
  int inlineWordOffset = 188;
  int crosslineWordOffset = 192;
  int fieldRecordOffset = 8;
  int cdpXlineOffset = 20;
};

struct OnnxGrid
{
  int rows = 411; // D61 工区网格行（inline）
  int cols = 641; // 列（crossline）
};

struct Rules
{
  // 有序层序界面集合（浅→深）。双职：isKnownSequenceBoundary 的成员词表
  // （大写比较）+ mappingHorizons()/编图 chip/厚度基面推导的有序集合——
  // 顺序即地层序，project_area.json 里按浅→深写。
  QStringList sequenceBoundaries;
  // 本工程的标定层位（时间残差验证 / ONNX 结果落栅格 / 时深井 tie 的目标
  // 层位）——必须是 sequenceBoundaries 的成员，否则配置拒用。
  QString targetHorizon;
  ClassifierRules classifier;
  SegyIndexing segy;
  OnnxGrid onnxGrid;
};

// 内置默认 = 本工区钉死值（唯一权威；消费方一律经 active()）。
Rules defaults();

// 工程配置文件约定路径：<projectDir>/project_area.json。
QString configFilePath(const QString &projectDir);

// 装载工程规则。缺文件 → out=defaults() 返回 true（读取顺序第一档）；
// 坏 JSON → 返回 false、error 写明原因（拒用，不静默回退）。
bool loadFromProjectDir(const QString &projectDir, Rules *out, QString *error);

// 进程级 active 规则（分类器/导入/SEG-Y 读取消费）。setProjectDir 绑定工程
// 目录并装载：成功 → active=装载结果；坏 JSON → active 保持 defaults 且
// lastError() 写明原因（拒用）。空串 = 解绑回默认（等价 reset()）。
// 工程打开时机由宿主接线（见 docs/AREA_PARAMETERS.md「接线点」）。
void setProjectDir(const QString &projectDir);
void reset();
Rules active();               // 线程安全快照（值拷贝；导入/索引在任务池上读）
QString activeProjectDir();   // 当前绑定目录；空 = 未绑定（用内置默认）
QString lastError();          // 最近一次装载失败原因；空 = 无失败/未发生
} // namespace AreaRules
