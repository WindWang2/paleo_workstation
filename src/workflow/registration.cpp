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
#include <QStringList>

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

  // BIZ-07（方向58）：参数闸前置到建目录/读源之前——非数值字符串（toDouble
  // 静默回 0）、NaN/Inf、零缩放一律拒绝，原因走 registrationFailed 错误通道，
  // 不留空派生目录、不产出 [null,null] 或塌缩几何的 GeoJSON。
  GeoAffineParams p;
  QStringList unparsable;
  const auto num = [&params, &unparsable](const char *key, double fallback) {
    const QString k = QLatin1String(key);
    if (!params.contains(k))
      return fallback;
    bool ok = false;
    const double v = params.value(k).toDouble(&ok);
    if (!ok)
      unparsable << k;
    return v;
  };
  p.tx = num("tx", 0.0);
  p.ty = num("ty", 0.0);
  p.sx = num("sx", 1.0);
  p.sy = num("sy", 1.0);
  p.rotDeg = num("rotDeg", 0.0);
  if (!unparsable.isEmpty())
  {
    fail(tr("仿射参数不是数值：%1").arg(unparsable.join(QStringLiteral(", "))));
    return;
  }
  QString paramWhy;
  if (!geoAffineParamsValid(p, &paramWhy))
  {
    fail(paramWhy);
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

  // 派生 GeoJSON 已写出即置只读；其后任一环节失败都必须把这份「无目录
  // 记录」的受管字节收掉——否则每次重试再落一份，artifacts/derived 下
  // 孤儿累积（#588 B.9；对照 mapexport.cpp:147-153 显式 remove）。
  // outAbs 刚被 setPermissions 为只读，Windows 删只读文件会失败，先恢复
  // 可写再删；目录是本次 mkpath 新建的，收掉空的版本目录与其上的资产
  // 目录（只收空的——同名资产此前的版本目录不动）。
  const auto discardDerived = [&outAbs, &outDir]() {
    if (QFile::exists(outAbs))
    {
      QFile::setPermissions(outAbs, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                       QFileDevice::ReadUser);
      QFile::remove(outAbs);
    }
    const QString assetDir = QFileInfo(outDir).absolutePath();
    if (QDir(outDir).isEmpty())
      QDir().rmdir(outDir);
    if (QDir(assetDir).isEmpty())
      QDir().rmdir(assetDir);
  };

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
    discardDerived();
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
    discardDerived();
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
