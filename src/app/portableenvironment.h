// 层：组装根
#pragma once
#include <QProcessEnvironment>
#include <QStringList>

struct PortableEnvironment {
  bool detected = false;
  QProcessEnvironment values;
  QStringList missing;
  QString anchorNote;   // 非 ASCII 安装路径且 ASCII 锚点不可用时的降级说明
};

PortableEnvironment inspectPortableEnvironment(const QString &directory,
                                              const QProcessEnvironment &inherited,
                                              const QString &anchor = QString());
// Runs before QApplication, QGIS, GDAL or worker threads are initialized.
PortableEnvironment initializePortableEnvironment();
