// 层：QGIS 封装
// qgis/commandlocatorentry.h — Ctrl+K 命令过滤器的一行数据（无 QGIS 依赖，供主窗头文件前置使用）。
#pragma once

#include <QString>
#include <QStringList>

struct CommandLocatorEntry
{
  QString commandId;
  QString title;    // 当前界面语言的标题（显示用）
  QStringList aliases; // 也参与匹配：中文源串 + 英文标题（任意界面语言都能按中/英文搜到）
  QString tooltip;
  QString group;   // ribbonGroup 或阶段名
  bool enabled = false;
  QString reason;  // 禁用原因（enabled 时为空）
};
