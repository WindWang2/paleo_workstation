// 层：视图
#pragma once
#include <QCoreApplication>
#include <QVariantMap>
#include <functional>

class MappingWorkbench;
class QWidget;
class SeismicClusterInterpretation {
  Q_DECLARE_TR_FUNCTIONS(SeismicClusterInterpretation)
public:
  using Highlight = std::function<void(const QList<qint64> &)>;
  static QVariantMap chooseManual(MappingWorkbench *workbench, const QString &id,
                                  const Highlight &highlight, QWidget *parent, QString *error);
  static bool previewWells(MappingWorkbench *workbench, const QString &id, QWidget *parent, QString *error);
};
