// 层：QGIS 封装
#pragma once
// 可折叠分组框的封装（QgsCollapsibleGroupBox）：视图层只拿到 QGroupBox*，
// 不直接 include qgs 头（docs/PALEO_QGIS_PLAN.md §2，check_qgis_spread）。
#include <QString>

class QGroupBox;
class QWidget;

namespace paleo::collapsible {
// 新建可折叠分组。rememberKey 非空时：折叠态按该键存入 QSettings
//（ui/collapsedGroups/<rememberKey>），下次新建时恢复，无记录则用 collapsed；
// rememberKey 为空则每次按 collapsed 初始化、不记忆。
QGroupBox *create(const QString &title, QWidget *parent, const QString &objectName,
                  bool collapsed, const QString &rememberKey = {});
// 设置/读取折叠态（非本封装创建的分组：设置无操作、读取返回 false）。
void setCollapsed(QGroupBox *group, bool collapsed);
bool isCollapsed(const QGroupBox *group);
// 记忆键对应的 QSettings 键（测试/迁移用）。
QString settingsKey(const QString &rememberKey);
} // namespace paleo::collapsible
