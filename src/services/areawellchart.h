// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <functional>
#include "domain/wellsection.h"
namespace paleo::data { class DataService; }
namespace WellComposite { struct ComprehensiveWellData; }
bool readAreaWellChart(paleo::data::DataService &service, qlonglong well,
                      WellComposite::ComprehensiveWellData &out, QString *error,
                      const std::function<bool()> &cancelled = {});
// Interval-only read: no log samples, photos or source-file reparsing.
bool readAreaWellLithology(const QString &project, qlonglong well,
                          QVector<wellsection::LithoSegment> &out, QString *error);
QStringList areaWellCurveNames(const QString &project,const QVector<qlonglong> &wells);
// Log-only native read; each group keeps its declared MD unit and sampling axis.
bool readAreaWellSectionCurves(const QString &project,qlonglong well,const QStringList &requested,
    QVector<wellsection::Curve> &out,QString *error,const std::function<bool()> &cancelled={});
