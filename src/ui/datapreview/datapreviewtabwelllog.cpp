// 层：视图
// token 例外：DESIGN 数据符号例外：关联分层区间填色，作为综合井图数据。（tools/ui-token-exceptions.json 精确计数）。
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"
#include "../paleoviewport.h"   // PaleoToolRow（工具条容器）

// 共享辅助来自内部头——与 datapreviewtabs.cpp 用同一 using 引入。
using namespace paleo::datapreview_detail;
#include "../paleotheme.h" // DESIGN.md token 出口（颜色/字阶/活体样式共用）
#include "../paleoicons.h" // 角落最大化/还原自绘图标
#include "../../catalog/datacatalog.h"
#include "../../domain/seismic/nicestep.h"
#include "../../domain/wellrecords.h"     // WellTopRecord/TimeDepthTable（domain 纯数据）
#include "../../domain/sectiontrace.h"    // SegyTrace/SegySectionGrid（domain 纯数据）
#include "../../io/lasdoc.h"              // LasCurve（白名单：数据模型）
#include "../../services/imagelod.h"      // 图片道缩略装载（方向 79 LOD）
#include "../../services/previewdoc.h"    // 唯一数据门面——解析/解码/SHA/PDF 编排全经它（W1）
#include "../../services/welllogset.h"    // 井曲线并集（综合柱状图；只读 ~C 头）
#include "../../workflow/wellsectionworkflow.h" // 岩性道：解释岩性 / 岩屑录井
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

// 方向20 轮4：buildContent 的「well_log」分支按资产类型析出到本文件。
// 共享辅助（caption8/qssHex/CurvePanel 等）来自内部头，与主文件同一 using。
// 参数是 buildContent 已算好的量；段内的 page 是本段新建的 PreviewMapPage，
// 与 buildContent 那个被 Q_UNUSED 丢弃的 page 形参同名但无关，故不收。

namespace {
QVector<WellComposite::FormationInterval> displayedFormations(
    QVector<WellComposite::FormationInterval> formations, const QVector<WellComposite::CurveData> &curves)
{
  double maximumDepth = -std::numeric_limits<double>::infinity();
  for (const auto &curve : curves)
    for (const auto depth : curve.depths) if (std::isfinite(depth)) maximumDepth = qMax(maximumDepth, double(depth));
  for (auto &formation : formations)
    if (!std::isfinite(formation.bottomDepth)) formation.bottomDepth = static_cast<float>(maximumDepth);
  formations.erase(std::remove_if(formations.begin(), formations.end(),
      [](const auto &formation) { return !std::isfinite(formation.bottomDepth) || formation.bottomDepth <= formation.topDepth; }), formations.end());
  return formations;
}
}

QWidget *DataPreviewTabs::buildWellLogContent(
    DataCatalog *cat, const CatalogAsset &asset, const CatalogVersion &v,
    const QString &abs, const QString &assetId,
    const QVector<QPair<QString, QString>> &wells, bool auxOnly,
    const QVector<EntityAssetLink> &links, QWidget *host, QVBoxLayout *lay)
{
  // 单井曲线：按已决链接过滤（LAS 本就是单井文件），标题带井名。
  QString linkedWell;
  if (!wells.isEmpty())
    linkedWell = wells.front().first;
  if (!linkedWell.isEmpty())
  {
    m_wellEntityOfAsset[assetId] = linkedWell;
    m_titleSuffixOfAsset[assetId] = wells.front().second;
  }
  // F1 两段式（goal/perf-systematize 簇2）：lasHeaderAt 只读 ~V/~W/~C 到
  // ~A 段头（代价与头部行数成正比、与数据行数无关）——曲线名秒出，整页
  // 控件骨架同步铺完；数据行 requestLas 池内解析（大文件冷解析曾 400ms+
  // 阻塞 UI），lasReady 到达后补数据与单位。无任务服务时 requestLas 同步
  // 执行、返回前信号已发——测试环境行为与旧路径一致。
  LasHeaderInfo header;
  QString perr;
  if (!m_doc->wellLogHeaderAt(abs, &header, &perr))
  {
    lay->addWidget(failureState(assetId, perr, host), 1);
    return host;
  }
  const QStringList names = header.curveNames;
  // ResFormStar 多井道综合柱状图总装（骨架即建；曲线数据两段式第二段补装）
  auto *compPanel = new WellComposite::WellCompositePanel(host);
  compPanel->setObjectName(QStringLiteral("wellCompositePanel"));

  // 查询该井是否有关联分层数据 (DC.dat)
  QVector<WellComposite::FormationInterval> formationIntervals;
  QStringList formationWarnings;
  if (!linkedWell.isEmpty())
  {
    static const QVector<QColor> kFormColors = {
        QColor(QStringLiteral("#FFE082")), QColor(QStringLiteral("#FFF59D")),
        QColor(QStringLiteral("#C8E6C9")), QColor(QStringLiteral("#A5D6A7")),
        QColor(QStringLiteral("#80CBC4")), QColor(QStringLiteral("#80DEEA")),
        QColor(QStringLiteral("#90CAF9")), QColor(QStringLiteral("#B39DDB"))};

    QVector<WellTopRecord> wellTops;
    QHash<QString, double> wellBases;
    m_doc->wellTopsForLog(linkedWell, &wellTops, &formationWarnings, &wellBases);
    for (int ti = 0; ti < wellTops.size(); ++ti)
    {
      WellComposite::FormationInterval fi;
      fi.name = wellTops.at(ti).topName;
      fi.topDepth = static_cast<float>(wellTops.at(ti).md);
      fi.bottomDepth = static_cast<float>(wellBases.value(fi.name, ti + 1 < wellTops.size() ? wellTops.at(ti + 1).md : qQNaN()));
      fi.color = kFormColors.at(ti % kFormColors.size());
      // 井道图上的分层就是这次预测的段井道。组名仍标成组，不能冒充段。
      fi.unitType = fi.name.trimmed().endsWith(QStringLiteral("组")) ? QStringLiteral("组") : QStringLiteral("段");
      formationIntervals.append(fi);
    }
  }

  // 图片道锚（岩心/薄片照片）：core/lab_analysis 角色井附件 + 导入侧
  // extra["depthMd"]（<井名>,<深度>m 文件名惯例，或后补编辑）。无锚照片
  // 不收——道内容不猜（未锚定清单归井附件管理面板）。装载走 imagelod
  // 缩略级（方向 79：解码期降采样 + EXIF 统一应用，不再全分辨率常驻）。
  QVector<WellComposite::ImageDepthItem> coreImages;
  if (!linkedWell.isEmpty())
  {
    for (const auto &lk : cat->linksForEntity(linkedWell))
    {
      if (lk.unresolved ||
          (lk.role != QLatin1String("core") && lk.role != QLatin1String("lab_analysis")))
        continue;
      const CatalogVersion ver = cat->currentVersion(lk.assetId);
      if (ver.id.isEmpty())
        continue;
      const QVariant depth = ver.extra.value(QLatin1String("depthMd"));
      if (!depth.isValid() || depth.toDouble() <= 0.0)
        continue;
      const QString imgPath = m_doc->absolutePathForVersion(ver);
      if (imgPath.isEmpty())
        continue;
      const paleo::imagelod::TrackImage ti = paleo::imagelod::loadThumbnail(imgPath);
      if (ti.isNull())
        continue;
      // 缩略位图直接入道（道宽 110px，缩略 256px ≥ 2× 道宽——绘制端再
      // 平滑缩到道宽）。透明图的棋盘底在道内绘制处理。
      WellComposite::ImageDepthItem it;
      it.topDepth = it.bottomDepth = static_cast<float>(depth.toDouble());
      it.imagePath = imgPath;
      it.caption = ver.fileName;
      it.pixmap = QPixmap::fromImage(ti.thumbnail);
      coreImages.append(it);
    }
  }

  // 岩性道：与连井同一来源。解释岩性优先，没有则用挂在这口井上的岩屑录井
  // （例如 A1岩屑录井数据）。没有段就不造空道。
  QVector<WellComposite::LithologyInterval> lithology;
  if (!linkedWell.isEmpty())
  {
    WellSectionWorkflow lithoSource(cat);
    const QVector<wellsection::LithoSegment> segments =
        lithoSource.lithologiesFor({linkedWell}).value(linkedWell);
    lithology.reserve(segments.size());
    for (const wellsection::LithoSegment &segment : segments)
    {
      WellComposite::LithologyInterval interval;
      interval.topDepth = static_cast<float>(segment.topMd);
      interval.bottomDepth = static_cast<float>(segment.baseMd);
      interval.lithoName = segment.litho;
      lithology.append(interval);
    }
  }

  const QString wellTitle = wells.isEmpty() ? asset.displayName : wells.front().second;

  // 两段式期间如实占位：数据行池内解析中（DESIGN.md 诚实状态；秒级内
  // 换装真实曲线，无骨架闪空）。
  auto *lasPendingHint = new QLabel(tr("正在后台解析数据行…"), host);
  lasPendingHint->setObjectName(QStringLiteral("lasPendingHint"));
  PaleoTheme::applyThemedStyleSheet(lasPendingHint, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  const QPointer<QLabel> hintFill(lasPendingHint);

  // 已决 well_log：综合图走井曲线并集。这里只读 ~C 头；兄弟文件数据体
  // 放进下面同一次 requestLas，不在 GUI 线程 parse。
  QString wellLogEntityId;
  for (const EntityAssetLink &l : links)
  {
    if (!l.unresolved && l.role == QLatin1String("well_log") && !l.entityId.isEmpty())
    {
      wellLogEntityId = l.entityId;
      break;
    }
  }
  const bool compositeFromWell = !wellLogEntityId.isEmpty();
  QVector<WellCurveRef> wellCurves;
  QStringList siblingPaths;
  if (compositeFromWell)
  {
    wellCurves = WellLogSet::wellCurveIndex(cat, catalogProjectDir(cat), wellLogEntityId);
    const QString currentAbs = QFileInfo(abs).absoluteFilePath();
    QSet<QString> seen;
    for (const WellCurveRef &ref : wellCurves)
    {
      const QString p = QFileInfo(ref.path).absoluteFilePath();
      if (p.isEmpty() || p == currentAbs || seen.contains(p))
        continue;
      seen.insert(p);
      siblingPaths.append(ref.path);
    }
  }

  // ---- F1 两段式第二段挂起：数据到达后装填综合图 ----
  // 无已决 well_log 时综合图用当前文件；有已决链接时按 wellCurveIndex，
  // 每条曲线用自己文件的深度列。
  const QPointer<WellComposite::WellCompositePanel> compFill(compPanel);
  const std::function<void(const QList<LasCurve> &, const QHash<QString, LasDoc> &)> fillCurves =
      [compFill, hintFill, names, coreImages,
       wellTitle, formationIntervals, formationWarnings, lithology, wellCurves, compositeFromWell, abs,
       currentBasis = header.indexBasis, currentFormat = asset.format](
          const QList<LasCurve> &curves, const QHash<QString, LasDoc> &siblings) {
        if (curves.size() != names.size())
        {
          // #413：头/整份曲线数契约不符——原实现先藏占位再静默 return，
          // 面板全空白无错误无入口；占位换成如实失败文案（姊妹失败路径
          // datapreviewtabs_well.cpp onLasFailed 同款语义）。
          if (hintFill)
            hintFill->setText(tr("曲线装载失败：头/整份曲线数不一致（头 %1 条，数据 %2 条）")
                                  .arg(names.size())
                                  .arg(curves.size()));
          return;
        }
        if (hintFill)
          hintFill->hide(); // 数据到齐，占位提示退场
        if (compFill)
        {
          QVector<WellComposite::CurveData> compCurves;
          QStringList axisWarnings = formationWarnings;
          QSet<QString> warnedPaths;
          if (!compositeFromWell)
          {
            WellCurveRef current;
            current.path = abs;
            current.indexBasis = currentBasis;
            current.format = currentFormat;
            QVector<double> md;
            QString error;
            if (WellLogSet::mdAxisInMetres(current, curves, &md, &error))
              for (int i = 1; i < names.size(); ++i)
                compCurves.append(compositeCurve(names[i], curves[i].unit, md, curves[i].values, i - 1));
            else axisWarnings << error;
          }
          else
          {
            int colorIndex = 0;
            for (const WellCurveRef &ref : wellCurves)
            {
              const QList<LasCurve> *body = lasBodyFor(ref.path, abs, curves, siblings);
              if (!body || body->isEmpty() || ref.column <= 0 || ref.column >= body->size())
                continue; // 兄弟文件解析失败：跳过，不让整页失败
              QVector<double> md;
              QString error;
              if (!WellLogSet::mdAxisInMetres(ref, *body, &md, &error))
              {
                if (!warnedPaths.contains(ref.path)) axisWarnings << error;
                warnedPaths.insert(ref.path);
                continue;
              }
              const LasCurve &src = body->at(ref.column);
              compCurves.append(compositeCurve(ref.mnemonic, src.unit, md, src.values, colorIndex));
              ++colorIndex;
            }
          }
          compFill->loadLasCurves(wellTitle, compCurves, displayedFormations(formationIntervals, compCurves));
          if (hintFill && !axisWarnings.isEmpty())
          {
            hintFill->setText(axisWarnings.join(QLatin1Char('\n')));
            hintFill->show();
          }
          if (!compFill)
            return;
          if (!lithology.isEmpty())
            compFill->setLithologyIntervals(lithology);
          if (!coreImages.isEmpty())
            compFill->setCoreImages(coreImages);
        }
      };

  // 综合图直接放在堆叠容器里：整份解析失败时容器内容换成「读取失败」面。
  auto *viewStack = new QStackedWidget(host);
  viewStack->setObjectName(QStringLiteral("logViewStack"));
  viewStack->addWidget(compPanel);
  lay->addWidget(lasPendingHint);
  lay->addWidget(viewStack, 1);

  // F1 二段挂起注册 + 数据请求（页根 = viewStack：整份解析失败时栈内容
  // 换成「读取失败」面，重试=重建标签再走一遍两段式）。无任务服务时
  // requestLas 同步执行、返回前信号已发——测试环境行为与旧路径一致。
  m_pendingLas.insert(assetId, {viewStack, fillCurves});
  m_doc->requestLas(assetId, abs, siblingPaths);
  return host;
}
