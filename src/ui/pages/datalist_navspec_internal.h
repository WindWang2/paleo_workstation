// 层：视图
#pragma once
#include "datalist.h"
struct DataListPanel::NavNodeSpec
{
  QString id;      // Qt::UserRole（资产/集合 id；分类节点空）
  QString eid;     // Qt::UserRole + 1（井/实体 id）
  QString type;    // Qt::UserRole + 2（节点类型标记）
  QString sub;     // Qt::UserRole + 3（子标记：well_attachments/ungrouped/inline/…）
  QVariant role4;  // Qt::UserRole + 4（realization 成员 idx / 统计 token；无效 = 清除）
  QString text0;
  QString text1;
  QString tooltip0;
  QString icon;    // qgis 主题图标名（PaleoIcons::qgisTheme 输入）
  bool expanded = false;   // 构建默认展开（仅新建项生效；存量项不碰用户态）
  bool monoDetail = false; // text1 走 mono 字体（坐标列，DESIGN.md）
  bool warning = false;    // text0 走警色（未决关联）
  bool enabled = true;     // false = 禁选（realization 缺席成员）
  QVector<NavNodeSpec> children;
};
