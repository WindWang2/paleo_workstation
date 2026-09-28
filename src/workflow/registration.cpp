// 层：功能
#include "registration.h"

#include "../catalog/datacatalog.h"
#include "../io/dataimportservice.h"
#include "../io/geojsonaffine.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <qgsfillsymbol.h>
#include <qgslinesymbol.h>
#include <qgsmaplayer.h>
#include <qgsmarkersymbol.h>
#include <qgssinglesymbolrenderer.h>
#include <qgsvectorlayer.h>

RegistrationWorkflow::RegistrationWorkflow(DataImportService *svc,
                                           QgisLayerService *layerSvc,
                                           QObject *parent)
    : QObject(parent), m_svc(svc), m_layerSvc(layerSvc)
{
}

void RegistrationWorkflow::applyProvisionalRegistration(
    const QString &assetId, const QVariantMap &params)
{
  const auto fail = [this](const QString &why) {
    emit registrationFailed(tr("临时配准失败：%1").arg(why));
  };

  DataCatalog *cat = m_svc ? m_svc->catalog() : nullptr;
  if (!cat || !m_layerSvc)
  {
    fail(tr("目录或图层服务未就绪"));
    return;
  }
  const CatalogVersion src = cat->currentVersion(assetId);
  if (src.id.isEmpty())
  {
    fail(tr("资产没有可用版本"));
    return;
  }
  const QString srcAbs = m_svc->absolutePathForVersion(src);
  if (srcAbs.isEmpty() || !QFile::exists(srcAbs))
  {
    fail(tr("找不到源文件 %1").arg(srcAbs));
    return;
  }

  // 工程目录：catalogPath() = <projectDir>/artifacts/metadata/catalog.json。
  const QDir projectDir = QFileInfo(cat->catalogPath())
                              .absoluteDir()
                              .absoluteFilePath(QStringLiteral("../.."));
  const QString verId = cat->nextVersionId();
  const QString relDir =
      QStringLiteral("artifacts/derived/%1/%2").arg(assetId, verId);
  const QString outDir = projectDir.absoluteFilePath(relDir);
  if (!QDir().mkpath(outDir))
  {
    fail(tr("派生目录创建失败"));
    return;
  }
  const QString outName = QFileInfo(src.fileName).completeBaseName() +
                          QStringLiteral(".provisional.geojson");
  const QString outAbs = QDir(outDir).filePath(outName);

  GeoAffineParams p;
  p.tx = params.value(QStringLiteral("tx")).toDouble();
  p.ty = params.value(QStringLiteral("ty")).toDouble();
  p.sx = params.value(QStringLiteral("sx"), 1.0).toDouble();
  p.sy = params.value(QStringLiteral("sy"), 1.0).toDouble();
  p.rotDeg = params.value(QStringLiteral("rotDeg")).toDouble();

  QString terr;
  int featureCount = 0;
  double bounds[4] = {0, 0, 0, 0};
  if (!geoAffineTransformFile(srcAbs, outAbs, p, &terr, &featureCount, bounds))
  {
    fail(terr);
    return;
  }
  QFile::setPermissions(outAbs, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                    QFileDevice::ReadGroup | QFileDevice::ReadOther);

  CatalogVersion d;
  d.id = verId;
  d.assetId = assetId;
  d.stage = QStringLiteral("DERIVED");
  d.versionNumber = src.versionNumber + 1;
  d.managed = true;
  d.path = relDir + QLatin1Char('/') + outName;
  d.sourceUri = srcAbs;
  d.sha256 = DataCatalog::sha256FileHex(outAbs, &terr);
  d.fileName = outName;
  d.parentVersionIds = QStringList{src.id};
  d.extra.insert(QStringLiteral("provisional"), true);
  d.extra.insert(QStringLiteral("affine"), geoAffineToMap(p));
  QString verr;
  if (!cat->addVersion(d, &verr))
  {
    fail(verr);
    return;
  }

  // 「临时配准 · 名」矢量图层：告警橙描边虚线——与正式图层视觉隔离。
  LayerDeclaration decl;
  decl.layerId = QStringLiteral("provisional.%1").arg(assetId);
  decl.type = QStringLiteral("vector");
  decl.source = outAbs;
  decl.group = QStringLiteral("00_Data");
  decl.title =
      tr("临时配准 · %1").arg(cat->assetById(assetId).displayName);
  QString derr;
  if (!m_layerSvc->declare(decl, &derr))
  {
    fail(derr);
    return;
  }
  QgsMapLayer *ml = m_layerSvc->instantiate(decl.layerId, &derr);
  auto *vl = qobject_cast<QgsVectorLayer *>(ml);
  if (!vl)
  {
    fail(derr.isEmpty() ? tr("图层实例化失败") : derr);
    return;
  }
  QVariantMap symProps;
  symProps.insert(QStringLiteral("color"),
                  QStringLiteral("242,153,0,60")); // warning 25%
  symProps.insert(QStringLiteral("outline_color"), QStringLiteral("#F29900"));
  symProps.insert(QStringLiteral("outline_width"), QStringLiteral("0.8"));
  symProps.insert(QStringLiteral("outline_style"), QStringLiteral("dash"));
  QgsSymbol *sym = nullptr;
  switch (vl->geometryType())
  {
  case Qgis::GeometryType::Line:
    symProps.remove(QStringLiteral("color"));
    symProps.insert(QStringLiteral("line_color"), QStringLiteral("#F29900"));
    symProps.insert(QStringLiteral("line_width"), QStringLiteral("0.8"));
    symProps.insert(QStringLiteral("line_style"), QStringLiteral("dash"));
    sym = QgsLineSymbol::createSimple(symProps).release();
    break;
  case Qgis::GeometryType::Point:
    symProps.insert(QStringLiteral("name"), QStringLiteral("triangle"));
    symProps.insert(QStringLiteral("size"), QStringLiteral("4"));
    sym = QgsMarkerSymbol::createSimple(symProps).release();
    break;
  default:
    sym = QgsFillSymbol::createSimple(symProps).release();
    break;
  }
  if (sym)
    vl->setRenderer(new QgsSingleSymbolRenderer(sym));

  ++m_provisionalLayers;
  emit provisionalLayerCountChanged(m_provisionalLayers);
  emit provisionalRegistered(decl.layerId, decl.title, featureCount);
}
