// 层：视图
#pragma once
#include <QString>
#include <QWidget>

// ui/dialogs/depthanchordialog — 井附件锚深编辑对话框（方向 79）：预览 +
// 深度输入 + 显式清锚。纯输入面——校验经 workflow/wellattachmentops 的
// parseDepthInput（结构化 reason → reasonText 出文案），落库归调用方。
// 图片预览按 LOD 单张口径全载（imagelod::loadFull）；透明图垫棋盘底
//（纸面图件中性灰，不随暗色翻转）。
namespace PaleoDepthAnchorDialog
{

struct Context
{
  QString wellName;      // 标题语境（井名）
  QString fileName;      // 附件文件名
  bool hasAnchor = false;
  double currentDepth = 0.0;
  QString anchorSource;  // filename | manual | 空
  QString imagePath;     // 预览图（空 = 无预览位）
};

struct Result
{
  bool accepted = false; // 对话框确认退出
  bool clear = false;    // 「清除锚定」路径（accepted 为真时有效）
  double depth = 0.0;    // 新锚深（clear=false 时有效，恒 > 0）
};

// 模态执行；返回 accepted 与意图，不落库。
bool prompt(QWidget *parent, const Context &ctx, Result *out);

} // namespace PaleoDepthAnchorDialog
