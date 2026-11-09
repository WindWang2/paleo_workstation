// 层：QGIS 封装
// qgis/commandlocatorentry.h — Ctrl+K 命令过滤器的一行数据（无 QGIS 依赖，供主窗头文件前置使用）。
#pragma once

#include <QString>

struct CommandLocatorEntry
{
  QString commandId;
  QString title;
  QString tooltip;
  QString group;   // ribbonGroup 或阶段名
  bool enabled = false;
  QString reason;  // 禁用原因（enabled 时为空）
};
