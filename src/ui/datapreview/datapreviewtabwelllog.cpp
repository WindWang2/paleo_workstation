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
  if (!m_doc->lasHeaderAt(abs, &header, &perr))
  {
    lay->addWidget(failureState(assetId, perr, host), 1);
    return host;
  }
  const QStringList names = header.curveNames;
  auto *singlePage = new QWidget(host);
  auto *singleLay = new QVBoxLayout(singlePage);
  singleLay->setContentsMargins(0, 0, 0, 0);
  singleLay->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *panel = new CurvePanel(singlePage);
  panel->setObjectName(QStringLiteral("curvePanel"));
  panel->setEmptyText(tr("这条曲线没有有效样点")); // §4：整条 -99999 → 不绘制

  // 1. 顶部控制栏（主选曲线 + 预设 + 缩放控制）
  auto *topBar = new QWidget(singlePage);
  auto *topLay = new QHBoxLayout(topBar);
  topLay->setContentsMargins(0, 0, 0, 0);
  topLay->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *combo = new QComboBox(topBar);
  combo->setObjectName(QStringLiteral("curveCombo"));
  combo->setAccessibleName(tr("曲线"));
  for (int i = 1; i < names.size(); ++i) // curves[0] 是深度道
    combo->addItem(names.at(i), i); // userData = curves 下标（禁用项不受序号偏移影响）

  // §4：约定的 GR/AC/DEN 缺了就给禁用项，tooltip 写「这条曲线不在文件里」。
  static const QStringList kExpected{QStringLiteral("GR"), QStringLiteral("AC"),
                                     QStringLiteral("DEN")};
  for (const QString &cn : kExpected)
    if (combo->findText(cn) < 0)
    {
      const int j = combo->count();
      combo->addItem(cn, -1);
      combo->setItemData(j, tr("这条曲线不在文件里"), Qt::ToolTipRole);
      auto *model = qobject_cast<QStandardItemModel *>(combo->model());
      if (model && model->item(j))
        model->item(j)->setEnabled(false);
    }

  const int def = combo->findText(QStringLiteral("GR"));
  if (def >= 0 && combo->itemData(def).toInt() > 0) // 禁用项不当作默认曲线
    combo->setCurrentIndex(def);

  // 深度缩放按钮组
  auto *btnZoomOut = new QToolButton(topBar);
  btnZoomOut->setText(QStringLiteral("−"));
  btnZoomOut->setToolTip(tr("缩小深度 (Ctrl+滚轮向下)"));
  btnZoomOut->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; min-width: 24px; min-height: 22px; }"));

  auto *lblZoom = new QLabel(QStringLiteral("100%"), topBar);
  lblZoom->setFont(monoFont());
  PaleoTheme::applyThemedStyleSheet(lblZoom, [] {
    return QStringLiteral("color: %1; min-width: 44px;")
        .arg(qssHex(PaleoTheme::tokens().textMuted));
  });
  lblZoom->setAlignment(Qt::AlignCenter);

  auto *btnZoomIn = new QToolButton(topBar);
  btnZoomIn->setText(QStringLiteral("+"));
  btnZoomIn->setToolTip(tr("放大深度 (Ctrl+滚轮向上)"));
  btnZoomIn->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; min-width: 24px; min-height: 22px; }"));

  auto *btnZoomReset = new QToolButton(topBar);
  btnZoomReset->setText(tr("1:1 适应"));
  btnZoomReset->setToolTip(tr("重置为全井深 (双击图道重置)"));
  btnZoomReset->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 {spacing.sm}px; }")));

  // 曲线快速预设按钮
  auto *btnSelectDefault = new QToolButton(topBar);
  btnSelectDefault->setText(tr("常规(GR/AC/DEN)"));
  btnSelectDefault->setToolTip(tr("显示三孔隙/常规测井曲线"));
  btnSelectDefault->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 {spacing.sm}px; }")));

  auto *btnSelectAll = new QToolButton(topBar);
  btnSelectAll->setText(tr("全选"));
  btnSelectAll->setToolTip(tr("同时显示所有曲线"));
  btnSelectAll->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 {spacing.sm}px; }")));

  auto *btnClear = new QToolButton(topBar);
  btnClear->setText(tr("仅主选"));
  btnClear->setToolTip(tr("仅显示当前下拉框选中的单根曲线"));
  btnClear->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 {spacing.sm}px; }")));

  topLay->addWidget(caption8(tr("主选曲线:"), topBar));
  topLay->addWidget(combo);
  topLay->addSpacing(PaleoTheme::tokens().spacingSm);
  topLay->addWidget(btnSelectDefault);
  topLay->addWidget(btnSelectAll);
  topLay->addWidget(btnClear);
  topLay->addStretch(1);
  topLay->addWidget(caption8(tr("深度缩放:"), topBar));
  topLay->addWidget(btnZoomOut);
  topLay->addWidget(lblZoom);
  topLay->addWidget(btnZoomIn);
  topLay->addWidget(btnZoomReset);

  // 初始显示曲线集合（默认优先显示 GR/AC/DEN 常规三孔隙）
  QSet<QString> defaultShown;
  if (combo->findText(QStringLiteral("GR")) >= 0 && combo->itemData(combo->findText(QStringLiteral("GR"))).toInt() > 0)
    defaultShown.insert(QStringLiteral("GR"));
  if (combo->findText(QStringLiteral("AC")) >= 0 && combo->itemData(combo->findText(QStringLiteral("AC"))).toInt() > 0)
    defaultShown.insert(QStringLiteral("AC"));
  if (combo->findText(QStringLiteral("DEN")) >= 0 && combo->itemData(combo->findText(QStringLiteral("DEN"))).toInt() > 0)
    defaultShown.insert(QStringLiteral("DEN"));
  if (defaultShown.isEmpty() && names.size() > 1)
    defaultShown.insert(names.at(1));

  // 曲线数据本体（values/unit）两段式第二段到达后经 fill 回调补装——
  // 此处只建骨架（chips/下拉/缩放/呈现切换），不碰数据行。

  // 2. 曲线多选 Chips 栏（横向滚动条，支持单击自由切换各曲线可见性）
  auto *chipScroll = new QScrollArea(singlePage);
  chipScroll->setWidgetResizable(true);
  chipScroll->setFixedHeight(32);
  chipScroll->setFrameShape(QFrame::NoFrame);
  chipScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  chipScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  chipScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

  auto *chipContainer = new QWidget(chipScroll);
  chipContainer->setStyleSheet(QStringLiteral("background: transparent;"));
  auto *chipLay = new QHBoxLayout(chipContainer);
  chipLay->setContentsMargins(0, 0, 0, 0);
  chipLay->setSpacing(PaleoTheme::tokens().spacingSm);
  chipLay->addWidget(caption8(tr("多曲线叠合:"), chipContainer));

  // 曲线 chip：描边保留曲线数据色；文字和底/边/悬停走 chrome
  // token。切换时重算当前主题样式，活体注册保证运行中换主题跟随。
  const auto chipStyle = [](const QColor &col, bool on) {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    if (on)
      return PaleoTheme::metricStyleSheet(QStringLiteral(
          "QToolButton { background: %1; border: 1.5px solid %2; border-radius: {rounded.md}px; "
          "color: %4; font-weight: bold; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; }"
          "QToolButton:hover { background: %3; }"))
          .arg(qssHex(t.surface), col.name(), qssHex(t.surfaceAltRaised), qssHex(t.text));
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QToolButton { background: %1; border: 1px solid %2; border-radius: {rounded.md}px; "
        "color: %3; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; }"
        "QToolButton:hover { background: %4; border-color: %5; }"))
        .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.textMuted),
             qssHex(t.surfaceAltRaised), qssHex(t.textDisabled));
  };

  auto chipMap = std::make_shared<QHash<QString, QToolButton *>>();
  for (int i = 1; i < names.size(); ++i)
  {
    const QString &cname = names.at(i);
    const QColor col = pickCurveColor(cname, i - 1);
    auto *chip = new QToolButton(chipContainer);
    chip->setText(cname);
    chip->setCheckable(true);
    const bool isChecked = defaultShown.contains(cname);
    chip->setChecked(isChecked);
    chip->setToolTip(cname); // 单位两段式第二段（lasReady）随数据补写

    PaleoTheme::applyThemedStyleSheet(chip, [chipStyle, chip, col] {
      return chipStyle(col, chip->isChecked());
    });

    connect(chip, &QToolButton::toggled, host, [panel, chip, cname, chipStyle, col](bool on) {
      panel->setCurveVisible(cname, on);
      chip->setStyleSheet(chipStyle(col, on));
    });

    (*chipMap)[cname] = chip;
    chipLay->addWidget(chip);
  }
  chipLay->addStretch(1);
  chipScroll->setWidget(chipContainer);

  // 缩放接线
  connect(btnZoomIn, &QToolButton::clicked, host, [panel]() { panel->zoomIn(); });
  connect(btnZoomOut, &QToolButton::clicked, host, [panel]() { panel->zoomOut(); });
  connect(btnZoomReset, &QToolButton::clicked, host, [panel]() { panel->resetZoom(); });
  panel->onZoomChanged = [lblZoom](double z) {
    lblZoom->setText(QStringLiteral("%1%").arg(qRound(z * 100)));
  };

  // 主选下拉框变更时，自动确保该曲线被勾选显示
  connect(combo, &QComboBox::currentIndexChanged, host, [panel, combo, names, chipMap]() {
    const int ci = combo->currentData().toInt();
    if (ci <= 0 || ci >= names.size())
      return;
    const QString &selName = names.at(ci);
    if (chipMap->contains(selName))
    {
      auto *btn = chipMap->value(selName);
      if (!btn->isChecked())
        btn->setChecked(true);
    }
  });

  // 预设按钮事件
  connect(btnSelectAll, &QToolButton::clicked, host, [chipMap]() {
    for (auto *b : *chipMap)
      if (!b->isChecked()) b->setChecked(true);
  });

  connect(btnSelectDefault, &QToolButton::clicked, host, [chipMap, defaultShown]() {
    for (auto it = chipMap->begin(); it != chipMap->end(); ++it)
    {
      const bool on = defaultShown.contains(it.key());
      if (it.value()->isChecked() != on)
        it.value()->setChecked(on);
    }
  });

  connect(btnClear, &QToolButton::clicked, host, [combo, names, chipMap]() {
    const int ci = combo->currentData().toInt();
    const QString activeName = (ci > 0 && ci < names.size()) ? names.at(ci) : QString();
    for (auto it = chipMap->begin(); it != chipMap->end(); ++it)
    {
      const bool on = (it.key() == activeName);
      if (it.value()->isChecked() != on)
        it.value()->setChecked(on);
    }
  });

  singleLay->addWidget(new PaleoToolRow(topBar, singlePage));
  singleLay->addWidget(chipScroll);
  singleLay->addWidget(panel, 1);

  // ResFormStar 多井道综合柱状图总装（骨架即建；曲线数据两段式第二段补装）
  auto *compPanel = new WellComposite::WellCompositePanel(host);
  compPanel->setObjectName(QStringLiteral("wellCompositePanel"));

  // 查询该井是否有关联分层数据 (DC.dat)
  QVector<WellComposite::FormationInterval> formationIntervals;
  if (!linkedWell.isEmpty())
  {
    static const QVector<QColor> kFormColors = {
        QColor(QStringLiteral("#FFE082")), QColor(QStringLiteral("#FFF59D")),
        QColor(QStringLiteral("#C8E6C9")), QColor(QStringLiteral("#A5D6A7")),
        QColor(QStringLiteral("#80CBC4")), QColor(QStringLiteral("#80DEEA")),
        QColor(QStringLiteral("#90CAF9")), QColor(QStringLiteral("#B39DDB"))};

    const auto wLinks = cat->linksForEntity(linkedWell);
    for (const auto &lk : wLinks)
    {
      if (lk.role == QLatin1String("tops"))
      {
        CatalogAsset topsAsset = cat->assetById(lk.assetId);
        CatalogVersion topsVer = cat->currentVersion(lk.assetId);
        QString topsPath = m_doc->absolutePathForVersion(topsVer);
        {
          QVector<WellTopRecord> tops;
          if (!topsPath.isEmpty() && m_doc->wellTopsAt(topsPath, &tops))
          {
            const QString normWell = DataCatalog::normalizeWellName(wells.isEmpty() ? QString() : wells.front().second);
            QVector<WellTopRecord> wellTops;
            for (const auto &tr : tops)
            {
              if (normWell.isEmpty() || DataCatalog::normalizeWellName(tr.wellName) == normWell)
                wellTops.append(tr);
            }
            std::sort(wellTops.begin(), wellTops.end(), [](const WellTopRecord &a, const WellTopRecord &b) {
              return a.md < b.md;
            });
            for (int ti = 0; ti < wellTops.size(); ++ti)
            {
              WellComposite::FormationInterval fi;
              fi.name = wellTops.at(ti).topName;
              fi.topDepth = static_cast<float>(wellTops.at(ti).md);
              fi.bottomDepth = static_cast<float>((ti + 1 < wellTops.size()) ? wellTops.at(ti + 1).md : (wellTops.at(ti).md + 50.0));
              fi.color = kFormColors.at(ti % kFormColors.size());
              formationIntervals.append(fi);
            }
          }
        }
        break;
      }
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

  // ---- F1 两段式第二段挂起：数据到达后一次装齐两个消费方 ----
  // 单道检视仍只用当前文件。无已决 well_log 时综合图与旧路径一致；
  // 有已决链接时综合图按 wellCurveIndex，每条曲线用自己文件的深度列。
  const QPointer<CurvePanel> panelFill(panel);
  const QPointer<WellComposite::WellCompositePanel> compFill(compPanel);
  const std::function<void(const QList<LasCurve> &, const QHash<QString, LasDoc> &)> fillCurves =
      [panelFill, compFill, chipMapFill = chipMap, hintFill, names, defaultShown, coreImages,
       wellTitle, formationIntervals, wellCurves, compositeFromWell, abs](
          const QList<LasCurve> &curves, const QHash<QString, LasDoc> &siblings) {
        if (hintFill)
          hintFill->hide(); // 数据到齐，占位提示退场
        if (curves.size() != names.size())
          return; // 头/整份契约：lasHeaderAt 与 lasAt 曲线名逐项一致——不符不装
        if (panelFill)
        {
          for (int i = 1; i < names.size(); ++i)
          {
            const QString &cname = names.at(i);
            panelFill->addCurve(cname, curves.at(i).unit, curves.at(i).values,
                                curves.at(0).values, pickCurveColor(cname, i - 1),
                                defaultShown.contains(cname));
            if (auto *chip = chipMapFill->value(cname))
              chip->setToolTip(QStringLiteral("%1 (%2)").arg(cname, curves.at(i).unit));
          }
        }
        if (compFill)
        {
          QVector<WellComposite::CurveData> compCurves;
          if (!compositeFromWell)
          {
            const auto &depList = curves.at(0).values;
            QVector<float> depVec;
            depVec.reserve(depList.size());
            for (double d : depList)
              depVec.append(static_cast<float>(d));

            for (int i = 1; i < names.size(); ++i)
            {
              const auto &src = curves.at(i);
              WellComposite::CurveData cd;
              cd.name = names.at(i);
              cd.unit = src.unit;
              cd.color = pickCurveColor(cd.name, i - 1);
              cd.depths = depVec;
              cd.values.reserve(src.values.size());
              float valMin = 1e9f, valMax = -1e9f;
              for (double v : src.values)
              {
                if (v <= -999.0 || v >= 99999.0)
                {
                  cd.values.append(-9999.0f);
                  continue;
                }
                float fv = static_cast<float>(v);
                cd.values.append(fv);
                if (fv < valMin) valMin = fv;
                if (fv > valMax) valMax = fv;
              }
              if (valMin < valMax)
              {
                cd.minScale = valMin;
                cd.maxScale = valMax;
              }
              else
              {
                cd.minScale = 0.0f;
                cd.maxScale = 100.0f;
              }
              compCurves.append(cd);
            }
          }
          else
          {
            int colorIndex = 0;
            for (const WellCurveRef &ref : wellCurves)
            {
              const QList<LasCurve> *body = lasBodyFor(ref.path, abs, curves, siblings);
              if (!body || body->isEmpty() || ref.column <= 0 || ref.column >= body->size())
                continue; // 兄弟文件解析失败：跳过，不让整页失败
              const LasCurve &src = body->at(ref.column);
              compCurves.append(compositeCurve(ref.mnemonic, src.unit, body->at(0).values,
                                               src.values, colorIndex));
              ++colorIndex;
            }
          }
          compFill->loadLasCurves(wellTitle, compCurves, formationIntervals);
          if (compFill && !coreImages.isEmpty())
            compFill->setCoreImages(coreImages);
        }
      };

  // 视图模式切换条与堆叠容器：选中 = chip 语义（primary 描边 + 浮起面底，
  // 同 ribbonStyleSheet checked 范式）；样式挂切换条一份，:checked 自动生效。
  auto *viewSwitchBar = new QWidget(host);
  auto *switchLay = new QHBoxLayout(viewSwitchBar);
  switchLay->setContentsMargins(0, 0, 0, 0);
  switchLay->setSpacing(PaleoTheme::tokens().spacingSm);
  styleViewSwitchBar(viewSwitchBar);

  auto *btnResForm = new QToolButton(viewSwitchBar);
  btnResForm->setObjectName(QStringLiteral("btnResFormView"));
  btnResForm->setText(tr("ResFormStar 综合多井道柱状图 (推荐)"));
  btnResForm->setCheckable(true);
  btnResForm->setChecked(true);

  auto *btnSingle = new QToolButton(viewSwitchBar);
  btnSingle->setObjectName(QStringLiteral("btnSingleView"));
  btnSingle->setText(tr("单道叠合检视"));
  btnSingle->setCheckable(true);
  btnSingle->setChecked(false);

  auto *viewStack = new QStackedWidget(host);
  viewStack->setObjectName(QStringLiteral("logViewStack"));
  viewStack->addWidget(compPanel);   // 0: ResForm 多井道柱状图（默认）
  viewStack->addWidget(singlePage);  // 1: 单道快速检视

  connect(btnResForm, &QToolButton::clicked, host, [btnResForm, btnSingle, viewStack] {
    btnResForm->setChecked(true);
    btnSingle->setChecked(false);
    viewStack->setCurrentIndex(0);
  });

  connect(btnSingle, &QToolButton::clicked, host, [btnResForm, btnSingle, viewStack] {
    btnSingle->setChecked(true);
    btnResForm->setChecked(false);
    viewStack->setCurrentIndex(1);
  });

  switchLay->addWidget(caption8(tr("呈现模式:"), viewSwitchBar));
  switchLay->addWidget(btnResForm);
  switchLay->addWidget(btnSingle);
  switchLay->addStretch(1);

  lay->addWidget(new PaleoToolRow(viewSwitchBar, host));
  lay->addWidget(lasPendingHint);
  lay->addWidget(viewStack, 1);

  // F1 二段挂起注册 + 数据请求（页根 = viewStack：整份解析失败时栈内容
  // 换成「读取失败」面，重试=重建标签再走一遍两段式）。无任务服务时
  // requestLas 同步执行、返回前信号已发——测试环境行为与旧路径一致。
  m_pendingLas.insert(assetId, {viewStack, fillCurves});
  m_doc->requestLas(assetId, abs, siblingPaths);
  return host;
}
