// 层：数据
#pragma once
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

// domain/ — 文件夹导入的瞬态行类型（从 io/dataimportservice.h 解嵌套）。
// 瞬态 DTO 不进 types.h（其契约要求 QVariantMap 往返）；确认表/工作流/服务
// 三方共用这一份行定义。

// 确认表预览行：与 importFolder 同一枚举/分类口径，只列行不导入。
// skipped=true 的行是软链逃逸/非普通文件（预览里灰显、不可改类型）。
// decision 是 plan 期决策（"skip"=重复→跳过 等），确认表逐行显示。
//
// displayType/typeEditable/typeVocab 在 previewFolder 里由分类器谓词算好
// 回填——确认表纯渲染，不回查 io 谓词：
//   displayType  行默认显示类型（固定辅助→reference；「参考资料」目录内
//                井类/未判→reference；其余=分类器原类型，空→unknown）。
//   typeEditable 类型下拉是否可改（skipped 行/固定辅助锁死）。
//   typeVocab    该行的类型词表（分类器词表 + 词表外类型兜底 + reference）。
struct FolderPreviewRow
{
  QString path;
  QString classifiedType;
  QString decision;          // accept | skip | as_new_version（plan 期）
  bool skipped = false;
  QString skipReason;
  QString displayType;       // 空 = 由调用方按 classifiedType 显示
  bool typeEditable = true;
  QStringList typeVocab;
};

// 导入行结果（两阶段导入 / 单行重导 / plan 执行共用口径）。
struct FolderRowResult
{
  QString path;            // 源路径（所选目录内）
  QString classifiedType;  // 分类器类型（well_head/well_log/tops/...）
  QString entityName;      // 已解析实体名；多个主关联用 ", " 连接；未决/失败为空
  enum class Outcome { Imported, Unresolved, Failed, Skipped };
  Outcome outcome = Outcome::Skipped;
  QString message;         // 失败原因 / 未决备注 / dedup「字节已在库」文案
};
