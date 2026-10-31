// 层：QGIS 封装
#pragma once

#include <QString>

class QgsProject;

// #745：工程级 UI 状态的稳定作用域键（工程文件绝对路径的 SHA1；无工程 → 空串）。
//
// 视图层需要按工程隔离 QSettings 时用它，不要为此 include <qgsproject.h>：
// qgs-audit 把「非 src/qgis 目录直连 qgs* 头」按新增直连判红。
QString paleoProjectScopeKey(QgsProject *project);
