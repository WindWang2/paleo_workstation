// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

using namespace paleo::datapreview_detail;

#include "../paleotheme.h"
#include "../paleoicons.h"
#include "../../catalog/datacatalog.h"
#include "../../domain/faciescatalog.h"
#include "../../domain/singlefactorrequest.h"
#include "previewhistogramwidget.h"
#include "previewidentifypanel.h"
#include "previewmappage.h"
#include "previewmapstates.h"
#include "previewprofilepanel.h"
#include "previewtocpanel.h"
#include "../../qgis/facieshierarchyrenderer.h"
#include "../../qgis/factorstylewriter.h"
#include "../../qgis/mappingartifactwriter.h"
#include "../../qgis/previewrasteranalysis.h"
#include "../../qgis/qgisstyleservice.h"

#include <qgsmapcanvas.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>
#include <qgsfields.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgsrastershader.h>
#include <qgscolorrampshader.h>

#include <QAction>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

QWidget *DataPreviewTabs::buildMapProductContent(
    DataCatalog *cat, const CatalogAsset &asset, const CatalogVersion &v,
    const QString &abs, const QString &assetId, QWidget *host, QVBoxLayout *lay)
{
  const QString layerType = v.extra.value(QStringLiteral("layer_type")).toString();
  bool isRaster = false;
  if (layerType == QLatin1String("raster"))
    isRaster = true;
  else if (layerType == QLatin1String("vector"))
    isRaster = false;
  else if (asset.format == QLatin1String("tif") || asset.format == QLatin1String("tiff") ||
           abs.endsWith(QLatin1String(".tif"), Qt::CaseInsensitive) ||
           abs.endsWith(QLatin1String(".tiff"), Qt::CaseInsensitive))
    isRaster = true;
  else if (asset.format == QLatin1String("gpkg") || asset.format == QLatin1String("geojson") ||
           abs.endsWith(QLatin1String(".gpkg"), Qt::CaseInsensitive) ||
           abs.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive))
    isRaster = false;
  else if (asset.type == QLatin1String("seismic_prediction") ||
           asset.type == QLatin1String("composed_facies") ||
           asset.type == QLatin1String("single_factor_raster") ||
           asset.type == QLatin1String("single_factor_cartographic_work") ||
           asset.type == QLatin1String("facies_fusion_raster"))
    isRaster = true;
  else
    isRaster = false;

  const QString title = v.extra.value(QStringLiteral("title"), asset.displayName).toString();

  if (isRaster)
  {
    auto raster = std::make_unique<QgsRasterLayer>(abs, title, QStringLiteral("gdal"));
    if (!raster->isValid() || raster->extent().isEmpty())
    {
      lay->addWidget(PreviewMapStates::buildErrorPage(
                         tr("无法读取成果栅格图件"), abs, host, tr("重试"),
                         [this, assetId] { rebuildAssetTab(assetId); }),
                     1);
      return host;
    }

    addRasterPyramidHint(m_doc, raster.get(), assetId, host, lay);

    auto *page = new PreviewMapPage(host);
    page->setObjectName(QStringLiteral("mapProductPreviewPage"));
    page->setAssetKey(assetId);
    page->setRenderCacheIdentity(assetId, v.id);
    page->mapCanvas()->canvas()->setObjectName(QStringLiteral("mapProductCanvas"));
    page->decorations()->setObjectName(QStringLiteral("mapProductDecorManager"));

    const QString styledKind = v.extra.value(QStringLiteral("kind"), asset.type).toString();
    const QString styledSource = v.extra.value(QStringLiteral("value_source")).toString();
    const bool analysisRaster =
        paleo::singlefactor::isAnalysisFactorRaster(styledKind, styledSource);
    const bool cartographic =
        paleo::singlefactor::rejectsQuantitativeUse(styledKind, styledSource);
    const QString factorId = v.extra.value(QStringLiteral("factor_id")).toString();

    if ((analysisRaster || cartographic || !factorId.isEmpty()) && !factorId.isEmpty())
    {
      FactorStyleWriter::applyTo(raster.get(), factorId);
    }
    else
    {
      QVariantList faciesList = v.extra.value(QStringLiteral("facies")).toList();
      if (faciesList.isEmpty())
      {
        const QString horizon = v.extra.value(QStringLiteral("horizon")).toString();
        if (!horizon.isEmpty() && cat)
        {
          for (const auto &a : cat->assets())
          {
            if (a.type == QLatin1String("facies_schema") &&
                a.displayName == QStringLiteral("facies-schema-%1").arg(horizon))
            {
              const auto sv = cat->currentVersion(a.id);
              faciesList = sv.extra.value(QStringLiteral("facies")).toList();
              break;
            }
          }
        }
      }
      if (faciesList.isEmpty() &&
          (styledKind.contains(QLatin1String("prediction")) ||
           styledKind.contains(QLatin1String("facies")) ||
           asset.type.contains(QLatin1String("prediction")) ||
           asset.type.contains(QLatin1String("facies"))))
      {
        faciesList = FaciesCatalog::defaults();
      }

      if (!faciesList.isEmpty())
      {
        MappingArtifactWriter::applyFaciesStyle(raster.get(), faciesList);
      }
      else
      {
        const auto sum = PreviewRasterAnalysis::summarize(raster.get());
        if (sum.valid)
        {
          PreviewRasterAnalysis::applyPseudoColorRenderer(
              raster.get(), 1, sum.min, sum.max,
              *PreviewRasterAnalysis::rampPreset(QStringLiteral("depthBlues")), false,
              PreviewRasterAnalysis::Classification::Continuous);
        }
      }
    }

    QgsRasterLayer *rawRaster = raster.release();
    rawRaster->setParent(host);
    page->addMapLayer(rawRaster, title, abs);

    // 统计面板 (D5.5)
    const auto sum = PreviewRasterAnalysis::summarize(rawRaster);
    {
      auto *statsPage = new QWidget(page);
      auto *statsLay = new QFormLayout(statsPage);
      const auto addStat = [&statsLay, statsPage](const QString &k, const QString &val,
                                                  const QString &objectName) {
        auto *l = new QLabel(val, statsPage);
        l->setObjectName(objectName);
        l->setFont(monoFont());
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        statsLay->addRow(k, l);
      };
      addStat(tr("最小值"), QString::number(sum.min, 'f', 2), QStringLiteral("mapProductStatMin"));
      addStat(tr("最大值"), QString::number(sum.max, 'f', 2), QStringLiteral("mapProductStatMax"));
      addStat(tr("均值"), QString::number(sum.mean, 'f', 2), QStringLiteral("mapProductStatMean"));
      addStat(tr("标准差"), QString::number(sum.stdDev, 'f', 2), QStringLiteral("mapProductStatStd"));
      addStat(tr("有效像元占比"), QStringLiteral("%1%").arg(sum.validRatio() * 100.0, 0, 'f', 1),
              QStringLiteral("mapProductStatValid"));
      page->addAnalysisTab(tr("统计"), statsPage);
    }

    // 直方图面板 (D2.3/D5.8)
    {
      auto *histPage = new QWidget(page);
      auto *histLay = new QVBoxLayout(histPage);
      histLay->setContentsMargins(PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs,
                                  PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs);
      auto *hist = new PreviewHistogramWidget(false, histPage);
      hist->setObjectName(QStringLiteral("mapProductHistogram"));
      const auto refreshHist = [hist, rawRaster](int bins) {
        hist->setHistogram(PreviewRasterAnalysis::histogram(rawRaster, bins));
        if (auto *r = dynamic_cast<QgsSingleBandPseudoColorRenderer *>(rawRaster->renderer()))
          if (auto *fn = r->shader()->rasterShaderFunction())
            hist->setStretchMarks(fn->minimumValue(), fn->maximumValue());
      };
      refreshHist(64);
      QObject::connect(hist, &PreviewHistogramWidget::binsChanged, histPage, refreshHist);
      QObject::connect(page->tocPanel(), &PreviewTocPanel::rasterStyleChanged, histPage,
                       [refreshHist, hist] { refreshHist(hist->bins()); });
      histLay->addWidget(hist, 1);
      page->addAnalysisTab(tr("直方图"), histPage);
    }

    // 剖面功能 (D5.1)
    QObject::connect(page, &PreviewMapPage::profileLineDrawn, host,
                     [page, rawRaster](const QgsPointXY &p1, const QgsPointXY &p2, int total) {
                       const auto samples = PreviewRasterAnalysis::sampleProfile(rawRaster, p1, p2, 200);
                       QVector<PreviewProfilePanel::Sample> pts;
                       pts.reserve(samples.size());
                       for (const auto &sm : samples)
                         pts.append({sm.distance, sm.value, sm.valid});
                       page->profilePanel()->addProfile(tr("剖面 %1").arg(total), pts, p1, p2);
                     });
    page->setProfileEnabled(true);

    lay->addWidget(page, 1);

    QTimer::singleShot(0, host, [page]() {
      page->mapCanvas()->zoomToFullExtent();
      page->primeRenderCache();
      if (!page->mapCanvas()->overlayVisible())
        page->showLowResSnapshot();
    });

    return host;
  }
  else
  {
    QString layerSource = abs;
    const QString sourceSuffix = v.extra.value(QStringLiteral("source_suffix")).toString();
    if (!sourceSuffix.isEmpty() && !layerSource.contains(QLatin1Char('|')))
      layerSource += sourceSuffix;

    auto vlayer = std::make_unique<QgsVectorLayer>(
        layerSource, title, QStringLiteral("ogr"));
    if (!vlayer->isValid() && !sourceSuffix.isEmpty())
    {
      vlayer = std::make_unique<QgsVectorLayer>(abs, title, QStringLiteral("ogr"));
    }
    if (!vlayer->isValid() && abs.endsWith(QLatin1String(".gpkg"), Qt::CaseInsensitive))
    {
      for (const QString &sub : {QStringLiteral("|layername=facies_polygons"),
                                 QStringLiteral("|layername=features"),
                                 QStringLiteral("|layername=contours")})
      {
        vlayer = std::make_unique<QgsVectorLayer>(abs + sub, title, QStringLiteral("ogr"));
        if (vlayer->isValid())
          break;
      }
    }

    if (!vlayer->isValid())
    {
      lay->addWidget(PreviewMapStates::buildErrorPage(
                         tr("无法读取成果矢量图件"), abs, host, tr("重试"),
                         [this, assetId] { rebuildAssetTab(assetId); }),
                     1);
      return host;
    }

    auto *page = new PreviewMapPage(host);
    page->setObjectName(QStringLiteral("mapProductPreviewPage"));
    page->setAssetKey(assetId);
    page->setRenderCacheIdentity(assetId, v.id);
    page->mapCanvas()->canvas()->setObjectName(QStringLiteral("mapProductCanvas"));
    page->decorations()->setObjectName(QStringLiteral("mapProductDecorManager"));

    const QString styledKind = v.extra.value(QStringLiteral("kind"), asset.type).toString();
    if (styledKind == QLatin1String("contour_lines") ||
        styledKind == QLatin1String("single_factor_cartographic_contour") ||
        asset.type == QLatin1String("contour_lines"))
    {
      QgisStyleService::applyContourLayerStyle(vlayer.get());
    }
    else
    {
      QVariantList faciesList = v.extra.value(QStringLiteral("facies")).toList();
      if (faciesList.isEmpty())
      {
        const QString horizon = v.extra.value(QStringLiteral("horizon")).toString();
        if (!horizon.isEmpty() && cat)
        {
          for (const auto &a : cat->assets())
          {
            if (a.type == QLatin1String("facies_schema") &&
                a.displayName == QStringLiteral("facies-schema-%1").arg(horizon))
            {
              const auto sv = cat->currentVersion(a.id);
              faciesList = sv.extra.value(QStringLiteral("facies")).toList();
              break;
            }
          }
        }
      }
      if (faciesList.isEmpty() && vlayer->fields().indexOf(QStringLiteral("facies_code")) >= 0)
      {
        faciesList = FaciesCatalog::defaults();
      }

      if (vlayer->fields().indexOf(QStringLiteral("facies_code")) >= 0)
      {
        FaciesHierarchyRenderer::apply(vlayer.get(), faciesList, QString());
        MappingArtifactWriter::applyFaciesLabels(vlayer.get(), 3);
      }
      else
      {
        QString labelField;
        for (const QString &cand : {QStringLiteral("相"), QStringLiteral("微相"), QStringLiteral("亚相"),
                                    QStringLiteral("facies"), QStringLiteral("name"), QStringLiteral("NAME"),
                                    QStringLiteral("id"), QStringLiteral("fid")})
        {
          if (vlayer->fields().indexOf(cand) >= 0)
          {
            labelField = cand;
            break;
          }
        }
        if (labelField.isEmpty() && !vlayer->fields().isEmpty())
          labelField = vlayer->fields().at(0).name();

        if (!labelField.isEmpty())
        {
          applyFaciesRendererToLayer(vlayer.get(), labelField);
        }
      }
    }

    QgsVectorLayer *rawVector = vlayer.release();
    rawVector->setParent(host);
    page->addMapLayer(rawVector, title, abs);

    // 属性表按钮
    auto *tableAction = new QAction(
        PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")),
        tr("属性表"), page);
    tableAction->setObjectName(QStringLiteral("previewAction_openAttrTable"));
    tableAction->setToolTip(tr("打开要素属性表"));
    QObject::connect(tableAction, &QAction::triggered, page, [page, rawVector] {
      auto *dlg = new PreviewAttributeTableDialog(rawVector, page);
      dlg->show();
    });
    page->addToolBarAction(tableAction);

    // 标注开关按钮
    auto *labelAction = new QAction(
        PaleoIcons::qgisTheme(QStringLiteral("mActionLabeling.svg")),
        tr("标注"), page);
    labelAction->setObjectName(QStringLiteral("previewAction_toggleLabels"));
    labelAction->setCheckable(true);
    labelAction->setChecked(rawVector->labelsEnabled());
    labelAction->setToolTip(tr("显示/隐藏要素标注"));
    QObject::connect(labelAction, &QAction::toggled, page, [rawVector, page](bool on) {
      rawVector->setLabelsEnabled(on);
      page->mapCanvas()->canvas()->refresh();
    });
    page->addToolBarAction(labelAction);

    lay->addWidget(page, 1);

    QTimer::singleShot(0, host, [page]() {
      page->mapCanvas()->zoomToFullExtent();
      page->primeRenderCache();
      if (!page->mapCanvas()->overlayVisible())
        page->showLowResSnapshot();
    });

    return host;
  }
}
