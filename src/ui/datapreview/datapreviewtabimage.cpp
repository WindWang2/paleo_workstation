// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

// 共享辅助来自内部头——与 datapreviewtabs.cpp 用同一 using 引入。
using namespace paleo::datapreview_detail;
#include "../paleoviewport.h"
#include "../paleotheme.h" // DESIGN.md token 出口（颜色/字阶/活体样式共用）
#include "../paleoicons.h" // 角落最大化/还原自绘图标
#include "../../catalog/datacatalog.h"
#include "../../domain/seismic/nicestep.h"
#include "../../domain/wellrecords.h"     // WellTopRecord/TimeDepthTable（domain 纯数据）
#include "../../domain/sectiontrace.h"    // SegyTrace/SegySectionGrid（domain 纯数据）
#include "../../io/lasdoc.h"              // LasCurve（白名单：数据模型）
#include "../../services/previewdoc.h"    // 唯一数据门面——解析/解码/SHA/PDF 编排全经它（W1）
#include "../../services/welllogset.h"    // 井曲线并集（综合柱状图；只读 ~C 头）
#include "../../services/paleotaskservice.h" // PaleoTask 进度/取消（地震转码区）
#include "../seismic3d/seismic3dviewpanel.h"
#include "../seismicsection/seismicsectioncanvas.h"
#include "../wellcomposite/wellcompositepanel.h"
#include "../decorations/paleodecorations.h"
#include "previewhistogramwidget.h"
#include "previewmappage.h"
#include "previewmapstates.h"
#include "previewprofilepanel.h"
#include "previewtocpanel.h"
#include "../../qgis/factorcontour.h"
#include "../../qgis/previewrasteranalysis.h"
#include <qgsmapcanvas.h>
#include <qgslayertreemapcanvasbridge.h>
#include <qgsmaptoolpan.h>
#include <qgsproject.h>
#include <qgsrasterbandstats.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsrastershader.h>
#include <qgscolorrampshader.h>
#include <qgscolorrampimpl.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgsrubberband.h>
#include <qgsexpression.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>
#include <qgsfields.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbol.h>
#include <qgsfillsymbol.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <qgslinesymbol.h>
#include <qgspallabeling.h>
#include <qgsvectorlayerlabeling.h>
#include <qgstextbuffersettings.h>
#include <QButtonGroup>
#include <QTimer>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFileInfo>
#include <QHeaderView>
#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QMouseEvent>
#include <QPdfDocument>
#include <QPdfView>
#include <QPixmap>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <limits>
#include "datapreviewtabs_internal.h"

// 方向20 轮4：buildContent 的「image_reference」分支按资产类型析出。
// 共享辅助来自内部头，与主文件同一 using。
// 段内的 page 是本段新建的 PreviewMapPage，与 buildContent 那个被
// Q_UNUSED 丢弃的 page 形参同名但无关，故本函数签名不收它。

QWidget *DataPreviewTabs::buildImageReferenceContent(
    DataCatalog *cat, const CatalogAsset &asset, const CatalogVersion &v,
    const QString &abs, const QString &assetId, QWidget *host, QVBoxLayout *lay)
{
  // D2.7：有 world file/配准边车 → 栅格上图；未配准 → 图片查看器 + 引导。
  // 托管副本身旁没有边车、源目录有 → 成对搬进临时目录再上图（GDAL 只认
  // 数据文件旁的边车）。
  const auto georefPair =
      PreviewMapStates::stageGeorefPairIfNeeded(abs, v.sourceUri, host);
  const QString worldFile = georefPair.second;
  if (!worldFile.isEmpty())
  {
    auto raster = std::make_unique<QgsRasterLayer>(georefPair.first, asset.displayName,
                                                   QStringLiteral("gdal"));
    if (raster->isValid() && !raster->extent().isEmpty())
    {
      auto *page = new PreviewMapPage(host);
      page->setObjectName(QStringLiteral("imagePreviewPage"));
      page->setAssetKey(assetId);
      page->setRenderCacheIdentity(assetId, v.id);
      page->mapCanvas()->canvas()->setObjectName(QStringLiteral("imageMapCanvas"));
      page->decorations()->setObjectName(QStringLiteral("imageDecorManager"));
      page->setProfileEnabled(false); // 影像非连续值面——剖面采样无意义
      QgsRasterLayer *rasterRaw = raster.release();
      rasterRaw->setParent(host);
      page->addMapLayer(rasterRaw, asset.displayName, abs);
      // D2.11 大图（>50MB 无金字塔）提示：降级仍可用。
      const QString bigHint = PreviewRasterAnalysis::bigRasterHint(rasterRaw);
      if (!bigHint.isEmpty())
      {
        lay->addWidget(PreviewMapStates::buildBigRasterHintBar(bigHint, host));
        // B3：消费侧预热（同层位栅格页口径——.ovr 完成后重载层刷新）。
        if (m_doc)
        {
          QPointer<QgsRasterLayer> rasterGuard(rasterRaw);
          connect(m_doc, &PreviewDocService::rasterPyramidFinished, host,
                  [rasterGuard, assetId](const QString &doneId, bool ok) {
                    if (doneId != assetId || !ok || !rasterGuard)
                      return;
                    rasterGuard->reload();
                    rasterGuard->triggerRepaint();
                  });
          m_doc->ensureRasterPyramidVersion(assetId);
        }
      }
      lay->addWidget(page, 1);
      lay->addWidget(caption8(tr("已按配准边车 %1 上图（RGB 影像原色）")
                                  .arg(QFileInfo(worldFile).fileName()),
                              host));
      QTimer::singleShot(0, host, [page, rasterRaw]() {
        page->mapCanvas()->zoomToLayer(rasterRaw);
        page->primeRenderCache();
        if (!page->mapCanvas()->overlayVisible())
          page->showLowResSnapshot();
      });
      return host;
    }
  }
  // 未配准 → 图片查看器（原行为）+ 「去配准」引导入口（D2.7）。
  auto *scroll = new QScrollArea(host);
  scroll->setWidgetResizable(true);
  auto *imgLabel = new QLabel(scroll);
  QPixmap pm(abs);
  if (pm.isNull())
  {
    lay->addWidget(failureState(assetId, tr("无法解析图片"), host), 1);
    return host;
  }
  imgLabel->setPixmap(pm.scaledToWidth(560, Qt::SmoothTransformation)); // 按面板宽缩放
  scroll->setWidget(imgLabel);
  lay->addWidget(scroll, 1);
  auto *regGuideBtn = new QPushButton(tr("去配准…"), host);
  regGuideBtn->setObjectName(QStringLiteral("goRegisterGuideBtn"));
  regGuideBtn->setToolTip(tr("把这张平面相图配准到工程测网"));
  connect(regGuideBtn, &QPushButton::clicked, host, [this, host]() {
    QMessageBox::information(
        host, tr("去配准"),
        tr("配准两条路：\n"
           "· 在图片旁放同名 world file（.wld/.pgw/.jgw，六参数文本）——"
           "重新打开预览即按栅格上图；\n"
           "· 或把相图界线转为 GeoJSON，用 D11「临时配准（手工仿射）」"
           "登记为 DERIVED 版本。"));
  });
  lay->addWidget(regGuideBtn, 0, Qt::AlignLeft);
  lay->addWidget(warnLabel(tr("未配准，不加入地图"), host)); // §4
  return host;
}
