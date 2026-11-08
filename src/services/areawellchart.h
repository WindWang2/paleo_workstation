// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <functional>
#include "domain/wellsection.h"
#include "documentrelationresolver.h"
namespace paleo::data { class DataService; }
namespace WellComposite { struct ComprehensiveWellData; }

struct AreaWellCurveWindow {
  double topMd=qQNaN(), bottomMd=qQNaN(); // MD metres. NaN leaves that end open.
  qlonglong maxSamples=2000000;
  qlonglong maxReadBytes=64LL*1024*1024;
};
struct AreaWellChartQuery {
  AreaWellCurveWindow curves;
  QStringList curveNames;
  std::optional<paleo::document::InterpretationContext> context;
  std::optional<paleo::document::RelationResolveRequest> intervalTrack;
};
bool readAreaWellChart(paleo::data::DataService &service, qlonglong well,
                      WellComposite::ComprehensiveWellData &out, QString *error,
                      const std::function<bool()> &cancelled = {},
                      const AreaWellChartQuery *query=nullptr);
// Interval-only read: no log samples, photos or source-file reparsing.
bool readAreaWellLithology(const QString &project, qlonglong well,
                          QVector<wellsection::LithoSegment> &out, QString *error);
QStringList areaWellCurveNames(const QString &project,const QVector<qlonglong> &wells);
// Log-only native read; each group keeps its declared MD unit and sampling axis.
bool readAreaWellSectionCurves(const QString &project,qlonglong well,const QStringList &requested,
    QVector<wellsection::Curve> &out,QString *error,const std::function<bool()> &cancelled={},
    const AreaWellCurveWindow &window={});
