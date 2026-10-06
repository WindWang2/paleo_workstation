// 层：数据
#pragma once
#include <QString>

// services/scriptprotocol.h — 脚本 stdout JSON 行协议解析（方向68 契约，可选遵守）。
// 约定：脚本向 stdout 写单行 JSON，带 "paleo":"1" 标记且 type ∈
// {progress, result, error} 的对象才按协议解释；其余行一律按纯文本呈现。
// 解析永不失败——任何输入都有 Text 兜底（不遵守协议的脚本照常运行）。
// 契约全文与示例脚本见 tools/reference/scripts/README.md。
struct ScriptMessage
{
  enum class Type { Text, Progress, Result, Error };
  Type type = Type::Text;
  QString raw;      // 原始行（协议行也保留原文，便于回看）
  QString message;  // 三型通用说明（缺省空串）
  int percent = -1; // progress：解析原值，展示侧自行钳制 0..100
  QString path;     // result：产出文件路径（相对脚本工作目录或绝对）
  QString kind;     // result：类型提示（geojson/csv/...，仅供展示与导入预判）
  int code = 0;     // error：脚本自定义错误码（缺省 0）
};

ScriptMessage parseScriptLine(const QString &line);
