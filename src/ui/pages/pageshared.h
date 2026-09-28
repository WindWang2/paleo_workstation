// 层：视图
#pragma once

#include <QLabel>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>
#include "../paleotheme.h"

// ui/pages/pageshared — 各页面板共用的视图助手（W5b：自 pagepanels.cpp
// 的匿名命名空间收敛成内联头）。只含纯 Qt 布局/查找助手，不含业务。
namespace paleo::pagesinternal
{

// 服务绑定走动态 QObject 属性（pagepanels.h 的公共形态不声明数据成员；
// 与 workflows.cpp 同一惯例），子控件按 objectName 查找。
inline constexpr char kLayersProp[] = "paleo.page.layers"; // QObject* (QgisLayerService)
inline constexpr char kWfProp[] = "paleo.page.wf";         // QObject* (page workflow)

// 五页序表：ribbon 页签序 = 阅读序 = 右栏栈序。壳（paleomainwindow.cpp）
// 与接线 TU（paleomainwindow_attach.cpp）共用，W4 后两处 TU 都要查序。
inline const QStringList kPageIds = {
  QStringLiteral("data"),       // 数据管理
  QStringLiteral("predict"),    // 预测编图
  QStringLiteral("constraint"), // 单因素图
  QStringLiteral("compose"),    // 智能编图
  QStringLiteral("validate"),   // 验证
};

// DESIGN.md `label` token: 8pt muted captions mark panel groups.
inline QLabel *caption(const QString &text, QWidget *parent)
{
  auto *l = new QLabel(text, parent);
  QFont f = l->font();
  f.setPointSize(8);
  l->setFont(f);
  // text-muted——PaleoTheme 现取（每次 caption() 调用现算，天然随主题）。
  l->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  return l;
}

inline QVBoxLayout *panelLayout(QWidget *page)
{
  auto *lay = new QVBoxLayout(page);
  lay->setContentsMargins(8, 8, 8, 8); // spacing.sm
  lay->setSpacing(8);
  return lay;
}

template <typename T>
T *child(QObject *root, const char *name)
{
  return root->findChild<T *>(QLatin1String(name));
}

} // namespace paleo::pagesinternal
