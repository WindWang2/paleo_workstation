// 层：视图
#pragma once
#include <QList>
#include <QString>

class QWidget;

// ui/help/whatsthiscatalog — 「这是什么？」上下文帮助的中央清单（方向 63）。
//
// 每条 = (所属面板类, 控件 objectName, 一句话说明[, 关联快捷键 id])。说明是
// 中文源串，经 "PaleoWhatsThis" 上下文翻译；带快捷键 id 的条目在说明后
// 追加「（快捷键：…）」，键序取自中央注册表，不会与实际绑定漂移。
//
// 不在各面板里散写 setWhatsThis：objectName 是面板的既有测试契约，按名
// 回填不改面板结构；同名控件（如各页都有 runButton）靠「所属面板类」区分。
// 懒建面板（综合柱状图）在自身构造末尾调一次 applyWhatsThis(this)。
namespace paleo::help
{

struct WhatsThisEntry
{
  const char *scopeClass; // 所属面板的 Q_OBJECT 类名（含命名空间），控件须在其子树内
  const char *objectName; // 控件 objectName
  const char *text;       // 中文源串（QT_TRANSLATE_NOOP "PaleoWhatsThis"）
  const char *shortcutId; // 可空：关联快捷键登记 id
};

QList<WhatsThisEntry> whatsThisEntries();

// 条目的用户可见文本（已翻译，含快捷键后缀）。
QString whatsThisText(const WhatsThisEntry &entry);

// 在 root 子树（含 root 自身）里按清单回填 whatsThis；已有 whatsThis 的控件
// 不覆盖。返回本次新设的控件数。可重复调用（幂等）。
int applyWhatsThis(QWidget *root);

} // namespace paleo::help
