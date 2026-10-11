// 层：QGIS 封装
#pragma once
#include <QRectF>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>
class QColor;
class QgsMapLayer;
class QgsSymbol;
class QgsVectorLayer;
namespace MappingArtifactWriter {
void restoreRasterCrs(QgsMapLayer *layer);
bool composeRaster(const QString &path, const QList<QgsMapLayer *> &layers,
                   const QVariantList &facies, const QList<double> &thresholds,
                   const QSet<QString> &continuous, QString *error);
bool raster(const QString &path, const QVector<int> &cells, int cols, int rows,
            const QRectF &extent, QString *error);
bool points(const QString &path, const QVariantList &points, QString *error);
bool vectorSnapshot(QgsVectorLayer *layer, const QString &path, QString *error);
QVariantList constraintGeometries(const QString &path, QString *error);
bool syncFaciesAttributes(QgsVectorLayer *layer, const QVariantList &facies,
                          QString *error);
void applyFaciesLabels(QgsVectorLayer *layer,
                       int mode); // 0 hidden, 1 identifiers, 2 facies, 3 both
// 用户未显式选择时的标注口径：面状相只标相名（不带要素序号），
// 测井相点标井名 + 相名。
int defaultFaciesLabelMode(const QgsVectorLayer *layer);
// 相面边缘描边：填充色同系加深一档。SimpleFill 直接补描边，SVG 纹理填充
// 追加只画轮廓的符号层。applyFaciesStyle / 分级重设相色后都要再调一次
// （QgsSymbol::setColor 会把描边层也刷成相色）。
void applyFaciesEdge(QgsSymbol *symbol, const QColor &fillColor);
void applyFaciesStyle(QgsMapLayer *layer, const QVariantList &facies);
} // namespace MappingArtifactWriter
