// 层：QGIS 封装
#include "collapsiblegroup.h"
#include <QSettings>
#include <qgscollapsiblegroupbox.h>

namespace paleo::collapsible {

QString settingsKey(const QString &rememberKey) {
  return QStringLiteral("ui/collapsedGroups/") + rememberKey;
}

QGroupBox *create(const QString &title, QWidget *parent, const QString &objectName,
                  bool collapsed, const QString &rememberKey) {
  auto *group = new QgsCollapsibleGroupBox(title, parent);
  group->setObjectName(objectName);
  // 不用 QGIS 自带的 saveCollapsedState：其键随顶层窗口 objectName 变化，且只在
  // show/hide 时读写；这里显式按 rememberKey 记忆，切换即落盘。
  group->setSaveCollapsedState(false);
  if (rememberKey.isEmpty()) {
    group->setCollapsed(collapsed);
    return group;
  }
  const auto key = settingsKey(rememberKey);
  group->setCollapsed(QSettings().value(key, collapsed).toBool());
  QObject::connect(group, &QgsCollapsibleGroupBoxBasic::collapsedStateChanged, group,
                   [key](bool state) { QSettings().setValue(key, state); });
  return group;
}

void setCollapsed(QGroupBox *group, bool collapsed) {
  if (auto *box = qobject_cast<QgsCollapsibleGroupBoxBasic *>(group)) box->setCollapsed(collapsed);
}

bool isCollapsed(const QGroupBox *group) {
  const auto *box = qobject_cast<const QgsCollapsibleGroupBoxBasic *>(group);
  return box && box->isCollapsed();
}

} // namespace paleo::collapsible
