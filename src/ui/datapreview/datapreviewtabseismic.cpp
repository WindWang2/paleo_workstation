// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

// 共享辅助（caption8/qssHex/SectionPanel/CurvePanel 等）来自内部头——
// 与 datapreviewtabs.cpp 用同一 using 引入，语义一致。
using namespace paleo::datapreview_detail;
#include "../paleoviewport.h"
#include "../paleotheme.h" // DESIGN.md token 出口（颜色/字阶/活体样式共用）
#include "../paleoicons.h" // 角落最大化/还原自绘图标
#include "../../catalog/datacatalog.h"
#include "../../domain/seismic/nicestep.h"
#include "../../domain/wellrecords.h"     // WellTopRecord/TimeDepthTable（domain 纯数据）
#include "../../domain/sectiontrace.h"    // SegyTrace/SegySectionGrid（domain 纯数据）
#include "../../io/lasdoc.h"              // LasCurve（白名单：数据模型）
#include "../../services/fspathutils.h"   // #291 QString↔filesystem::path 走 UTF-16（MSVC 窄构造按 ANSI 解码）
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
#include <QSignalBlocker>
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
#include "../notifications/paleonotify.h"
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

// 方向20 轮4：buildContent 的「seismic」分支按资产类型析出到本文件。
// 共享辅助（caption8/qssHex/SectionPanel 等）已由 datapreviewtabs_internal.h 提供。
// 参数全是 buildContent 已算好的量，本函数不重算、不改语义。

namespace {
QWidget *makeSeismic2DTab(QWidget *parent, SectionPanel *panel, QWidget *bar, QLabel *tieCaption)
{
  auto *page = new QWidget(parent);
  page->setObjectName(QStringLiteral("seismic2DContainer"));
  auto *layout = new QVBoxLayout(page);
  const auto &theme = PaleoTheme::tokens();
  layout->setContentsMargins(theme.spacingSm, theme.spacingSm, theme.spacingSm, theme.spacingSm);
  layout->setSpacing(theme.spacingXs);
  layout->addWidget(caption8(QObject::tr("选择一条测线解码"), page));
  layout->addWidget(new PaleoToolRow(bar, page));
  layout->addWidget(tieCaption);
  layout->addWidget(panel, 1);
  return page;
}

// 测线号范围：按当前模式取 survey 冻结几何；两轴都缺（无 survey 元数据，
// 导入时未冻结）时放开到 0..1000000。初始建页与模式切换处理器同走这一份
// ——回退曾只写在建页处，切模式后范围塌缩成 [0,0] 把输入锁死。
void applyLineSpinRange(QSpinBox *no, const CatalogEntity &survey, bool isInline)
{
  const double lo = isInline ? survey.inlineMin : survey.xlineMin;
  const double hi = isInline ? survey.inlineMax : survey.xlineMax;
  if (lo == 0.0 && hi == 0.0)
  {
    no->setRange(0, 1000000);
    return;
  }
  no->setRange(static_cast<int>(lo), static_cast<int>(qMax(hi, lo)));
}

// 转码进度条/取消按钮接线（.sf3c 与 .sf3p 共用同一组控件）。绑定前摘掉上一
// 任务的连接——否则一次取消会同时 requestCancel 两个在途任务，先完成的
// 任务还会把另一任务的进度条与取消入口一并隐藏。可见性按在途任务计数管理
// （active 归零才隐藏）；cancel 只作用于最近一次绑定的任务（bound）。
void bindTranscodeTaskUi(QProgressBar *bar, QToolButton *cancel, PaleoTask *task,
                         const std::shared_ptr<int> &active,
                         const std::shared_ptr<QPointer<PaleoTask>> &bound)
{
  if (!task)
    return;
  if (PaleoTask *prev = bound->data())
  {
    QObject::disconnect(prev, nullptr, bar, nullptr);
    QObject::disconnect(cancel, &QToolButton::clicked, prev, &PaleoTask::requestCancel);
  }
  *bound = task;
  ++(*active);
  bar->setVisible(true);
  bar->setValue(0);
  cancel->setVisible(true);
  const QPointer<PaleoTask> taskGuard(task);
  QObject::connect(task, &PaleoTask::changed, bar, [taskGuard, bar]() {
    if (taskGuard)
      bar->setValue(taskGuard->percent());
  });
  QObject::connect(cancel, &QToolButton::clicked, task, &PaleoTask::requestCancel);
  QObject::connect(task, &PaleoTask::finished, bar, [bar, cancel, active]() {
    if (--(*active) <= 0)
    {
      bar->setVisible(false);
      cancel->setVisible(false);
    }
  });
}
}

QWidget *DataPreviewTabs::buildSeismicContent(
    DataCatalog *cat, const CatalogAsset &asset, const CatalogVersion &v,
    const QString &abs, const QString &assetId,
    const QVector<EntityAssetLink> &links, QWidget *host, QVBoxLayout *lay)
{
  // survey 几何（导入时冻结）驱动测线选择；只解码选中的一条（§7）。
  QString surveyId;
  for (const EntityAssetLink &l : links)
    if (l.role == QLatin1String("seismic_volume"))
      surveyId = l.entityId;
  const CatalogEntity survey =
      surveyId.isEmpty() ? CatalogEntity() : cat->entityById(surveyId);

  // ---- 标定井：catalog 序第一口有目标层位分层的井 + 主 time_depth 表插值
  // + 初始测线内插——派生量全在数据门面一次算好（§3/阶段 B 口径不变）。----
  const PreviewDocService::TieMarker tie = m_doc->seismicTieMarker(assetId);
  const bool haveTieTop = tie.haveTop;
  const QString tieWellName = tie.wellName;
  const int initialInline = tie.initialInline;

  auto *bar = new QWidget(host);
  auto *barLay = new QHBoxLayout(bar);
  barLay->setContentsMargins(0, 0, 0, 0);
  auto *mode = new QComboBox(bar);
  mode->setObjectName(QStringLiteral("lineMode"));
  mode->setAccessibleName(tr("测线"));
  mode->addItem(tr("纵测线"), QStringLiteral("inline"));
  mode->addItem(tr("横测线"), QStringLiteral("crossline"));
  auto *no = new QSpinBox(bar);
  no->setObjectName(QStringLiteral("lineSpin"));
  no->setAccessibleName(tr("测线号"));
  applyLineSpinRange(no, survey, /*isInline=*/true); // 初始页 = 纵测线
  if (initialInline >= 0)
    no->setValue(initialInline);
  auto *panel = new SectionPanel(host);
  auto *tieCaption = caption8(QString(), host);
  tieCaption->setObjectName(QStringLiteral("tieLabel"));
  if (haveTieTop)
  {
    // 标定写「A1 D61」和时间，或「无时深表」「超出时深表」「时深表无序」之一。
    if (tie.ok)
      tieCaption->setText(
          tr("%1 %2 · %3 ms").arg(tieWellName, tie.horizon).arg(tie.timeMs, 0, 'f', 1));
    else
      tieCaption->setText(
          tr("%1 %2 · %3").arg(tieWellName, tie.horizon, tie.statusText));
  }
  const auto decode = [this, assetId, abs, v, mode, no, panel, tieCaption]() {
    panel->clearImage(); // 换测线先清掉上一张剖面（§4）
    const QString idxTip = tr("正在建立道索引");
    mode->setEnabled(false);
    no->setEnabled(false);
    mode->setToolTip(idxTip);
    no->setToolTip(idxTip);
    const bool isInline = mode->currentData().toString() == QLatin1String("inline");
    const int lineNo = no->value();

    // D1/T23：读者缓存/世代号/协作取消/SHA 复验/下游标过时全在门面——
    // 这里只挂起控件组（结果信号回来按 assetId 找回控件贴图）。
    SectionPending pend;
    pend.panel = panel;
    pend.mode = mode;
    pend.spin = no;
    pend.tieCaption = tieCaption;
    pend.tieText = tieCaption->text();
    const PreviewDocService::TieMarker tieNow = m_doc->seismicTieMarker(assetId);
    pend.hasTie = tieNow.haveTop && tieNow.ok;
    pend.tieMs = tieNow.timeMs;
    m_pendingSection[assetId] = pend;
    m_doc->requestSection(assetId, v.id, abs, v.managed, v.sha256,
                          isInline, lineNo);
  };

  connect(mode, &QComboBox::currentIndexChanged, host, [mode, no, survey, decode]() {
    applyLineSpinRange(no, survey, mode->currentData().toString() == QLatin1String("inline"));
    decode();
  });
  connect(no, &QSpinBox::valueChanged, host, decode);
  decode();
  barLay->addWidget(mode);
  barLay->addWidget(no);
  barLay->addWidget(panel->createColorMapControl(bar));
  if (initialInline >= 0)
    barLay->addWidget(caption8(tr("%1 所在测线").arg(tieWellName), bar)); // §4 旁注
  barLay->addStretch(1);
  auto *modeTabs = new QTabWidget(host);
  modeTabs->setObjectName(QStringLiteral("seismicSubTabs"));
  // 普通页签选中 = 深字 + 加粗（同预览主标签栏范式），不用 primary 蓝字。
  PaleoTheme::applyThemedStyleSheet(modeTabs, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: 1px solid %1; background: %2; }"
        "QTabBar::tab { background: %3; color: %4; padding: {spacing.xs}px {spacing.md}px; border: 1px solid %1; border-bottom: none; }"
        "QTabBar::tab:selected { background: %2; color: %5; font-weight: 600; }"))
        .arg(qssHex(t.border), qssHex(t.surface), qssHex(t.surfaceAlt),
             qssHex(t.textMuted), qssHex(t.text));
  });

  // 1. 二维测线 (2D)
  auto *w2d = makeSeismic2DTab(modeTabs, panel, bar, tieCaption);
  modeTabs->addTab(w2d, tr("二维测线 (2D)"));

  // 2. 三维立体 (3D)
  auto *panel3d = new seismic::Seismic3DViewPanel(modeTabs);
  panel3d->setProperty("fullPrecisionAfterIndex",true);
  panel3d->setObjectName(QStringLiteral("seismic3DPanel"));
  modeTabs->addTab(panel3d, tr("三维立体 (3D)"));

  // D7.3 解释层位面上图：伴生解释会话 <sgy>.seispicks.json 有拾取 →
  // 按层位名分组 IDW 网格化 → 3D 面片（显隐走面板「解释」菜单/信号）。
  {
    seismic::SeismicInterpretationSession session;
    if (seismic::SeismicTaskService::loadSession(abs, session, nullptr) &&
        !session.picks.isEmpty()) {
      QHash<QString, QList<seismic::SeismicPick>> byHorizon;
      for (const seismic::SeismicPick &p : session.picks)
        byHorizon[p.horizonName].append(p);
      QStringList names;
      std::vector<seismic::SeismicHorizonGrid> grids;
      for (auto it = byHorizon.constBegin(); it != byHorizon.constEnd(); ++it) {
        names << (it.key().isEmpty() ? tr("未命名层位") : it.key());
        grids.push_back(seismic::SeismicTaskService::gridPicks(it.value()));
      }
      if (!grids.empty())
        panel3d->setHorizons(names, grids);
    }
  }

  // D3.2：三维切片拖动/剖面条联动 2D——只拨同页 2D 测线控件（控件自己的
  // decode 链换测线）。不开新标签、不切回 2D 子页签。
  connect(panel3d, &seismic::Seismic3DViewPanel::inlineChanged,
          panel3d, [mode, no](int inlineNo) {
    const int want = mode->findData(QStringLiteral("inline"));
    if (want >= 0 && mode->currentIndex() != want)
      mode->setCurrentIndex(want);
    if (no->value() != inlineNo)
      no->setValue(inlineNo);
  });
  connect(panel3d, &seismic::Seismic3DViewPanel::crosslineChanged,
          panel3d, [mode, no](int xlineNo) {
    const int want = mode->findData(QStringLiteral("crossline"));
    if (want >= 0 && mode->currentIndex() != want)
      mode->setCurrentIndex(want);
    if (no->value() != xlineNo)
      no->setValue(xlineNo);
  });

  // ---- 引擎通道状态（先于转码区声明）：两段式体加载 + 显式 .sf3p 通道 ----
  auto sharedVol = std::make_shared<std::shared_ptr<seismic::SgyVolume>>();
  auto sharedPaged = std::make_shared<QString>();
  *sharedPaged = QFile::exists(abs + QStringLiteral(".sf3p"))
                     ? abs + QStringLiteral(".sf3p")
                     : QString(); // Auto 永不自动升级 .sf3p：存在即显式启用（引擎语义）

  // 3. 水平时间切片剖面 (Time Slice)
  auto *wTime = new QWidget(modeTabs);
  wTime->setObjectName(QStringLiteral("seismicTimeSliceContainer"));
  auto *layTime = new QVBoxLayout(wTime);
  layTime->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  layTime->setSpacing(PaleoTheme::tokens().spacingXs);

  auto *timeBar = new QWidget(wTime);
  auto *timeBarLay = new QHBoxLayout(timeBar);
  timeBarLay->setContentsMargins(0, 0, 0, 0);
  timeBarLay->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *lblTimeTitle = caption8(tr("水平时间切片 (TWT)"), timeBar);
  timeBarLay->addWidget(lblTimeTitle);

  auto *lblTimeIndex = new QLabel(tr("时间采样:"), timeBar);
  {
    QFont f = lblTimeIndex->font();
    f.setPointSize(PaleoTheme::tokens().labelPt);
    lblTimeIndex->setFont(f);
  }
  PaleoTheme::applyThemedStyleSheet(lblTimeIndex,
                                    [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  timeBarLay->addWidget(lblTimeIndex);

  auto *sliderTime = new QSlider(Qt::Horizontal, timeBar);
  sliderTime->setObjectName(QStringLiteral("timeSliceSlider"));
  sliderTime->setFixedWidth(160);
  timeBarLay->addWidget(sliderTime);

  QFont mono8 = PaleoTheme::monoFont();
  mono8.setPointSize(PaleoTheme::tokens().labelPt);

  auto *spinTime = new QSpinBox(timeBar);
  spinTime->setObjectName(QStringLiteral("timeSliceSpin"));
  spinTime->setFont(mono8);
  spinTime->setFixedWidth(64);
  timeBarLay->addWidget(spinTime);

  // 时间读数：mono 数字面 + 正文色（数值读数不是装饰蓝的三许可用途）。
  auto *lblTimeMs = new QLabel(QStringLiteral("0.0 ms"), timeBar);
  lblTimeMs->setObjectName(QStringLiteral("timeSliceMsLabel"));
  lblTimeMs->setFont(mono8);
  PaleoTheme::applyThemedStyleSheet(lblTimeMs, [] {
    return QStringLiteral("color: %1;").arg(qssHex(PaleoTheme::tokens().text));
  });
  lblTimeMs->setFixedWidth(90);
  timeBarLay->addWidget(lblTimeMs);

  // 时间片/转码区按钮统一走一份活体样式（原 8.5pt + 浅色字面量收口）。
  PaleoTheme::applyThemedStyleSheet(timeBar, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: 1px solid %1;"
        " border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; color: %2; }"
        "QToolButton:hover { background: %3; border-color: %4; }"
        "QToolButton:disabled { color: %4; }"))
        .arg(qssHex(t.border), qssHex(t.text), qssHex(t.surfaceAltRaised),
             qssHex(t.textDisabled));
  });

  auto *btnFitTime = new QToolButton(timeBar);
  btnFitTime->setText(tr("适应窗口"));
  timeBarLay->addWidget(btnFitTime);

  // ---- 转码区（主线6）：.sf3c（Auto 自动升级）与 .sf3p（显式 paged/LOD 通道）
  // 双通道并存；进度条非 spinner（有总量即百分比）、可取消、完成后热切换
  // 后端并把状态写在 backendLabel 上。取消后两条通道都可续跑。 ----
  auto *btnTranscode = new QToolButton(timeBar);
  btnTranscode->setText(tr("转码工作区 (.sf3c)"));
  btnTranscode->setToolTip(tr("将 SEG-Y 转码为 .sf3c 分片工作区（可续跑）；转码后切片与任意剖面走随机访问后端"));
  btnTranscode->setEnabled(!abs.isEmpty() && QFile::exists(abs)
                           && !QFile::exists(abs + QStringLiteral(".sf3c.meta")));
  // D1.2/D1.6：断点探测——半成品给「继续转码」入口，旧版/损坏给「重建」提示
  if (auto *probeSvc = (m_doc ? m_doc->seismicTaskService() : nullptr))
  {
    const seismic::SeismicWorkspaceProbe wp = probeSvc->probeWorkspace(abs);
    if (wp.exists && wp.readable && !wp.complete)
    {
      btnTranscode->setEnabled(true);
      btnTranscode->setText(tr("继续转码 (.sf3c)"));
      btnTranscode->setToolTip(tr("检测到未完成的 .sf3c 工作区（%1）。点击继续，已写分片自动跳过")
                                   .arg(wp.stateText()));
    }
    else if (wp.exists && !wp.readable)
    {
      btnTranscode->setEnabled(true);
      btnTranscode->setText(tr("重建工作区 (.sf3c)"));
      btnTranscode->setToolTip(tr("现有 .sf3c 工作区不可读（%1，格式版本 v%2）。再次转码将自动重建")
                                   .arg(wp.error.isEmpty() ? tr("格式不支持") : wp.error)
                                   .arg(wp.formatVersion));
    }
  }
  timeBarLay->addWidget(btnTranscode);

  auto *btnPagedTranscode = new QToolButton(timeBar);
  btnPagedTranscode->setObjectName(QStringLiteral("btnPagedTranscode"));
  btnPagedTranscode->setText(tr("转码分页工作区 (.sf3p)"));
  btnPagedTranscode->setToolTip(tr("转码为 .sf3p 分页工作区并构建 L1/L2 金字塔（可续跑）；"
                                   "提供瓦片渐进时间片与拖动粗/静止细的渐进 LOD。Auto 后端不自动启用，需显式选择"));
  const QString pagedPathForButtons = abs + QStringLiteral(".sf3p");
  btnPagedTranscode->setEnabled(!abs.isEmpty() && QFile::exists(abs)
                                && !QFile::exists(pagedPathForButtons));
  if (auto *probeSvc = (m_doc ? m_doc->seismicTaskService() : nullptr))
  {
    const seismic::SeismicWorkspaceProbe pp = probeSvc->probePagedWorkspace(pagedPathForButtons);
    if (pp.exists && !pp.complete)
    {
      btnPagedTranscode->setEnabled(true);
      btnPagedTranscode->setText(tr("继续转码 (.sf3p)"));
      btnPagedTranscode->setToolTip(tr("检测到未完成的 .sf3p 分页工作区（%1）。点击继续，已完成页自动跳过")
                                        .arg(pp.stateText()));
    }
  }
  timeBarLay->addWidget(btnPagedTranscode);

  auto *transcodeProgress = new QProgressBar(timeBar);
  transcodeProgress->setObjectName(QStringLiteral("transcodeProgress"));
  transcodeProgress->setFixedWidth(140);
  transcodeProgress->setRange(0, 100);
  transcodeProgress->setVisible(false);
  timeBarLay->addWidget(transcodeProgress);

  auto *btnCancelTranscode = new QToolButton(timeBar);
  btnCancelTranscode->setObjectName(QStringLiteral("btnCancelTranscode"));
  btnCancelTranscode->setText(tr("取消"));
  btnCancelTranscode->setVisible(false);
  timeBarLay->addWidget(btnCancelTranscode);

  auto *backendLabel = new QLabel(timeBar);
  backendLabel->setObjectName(QStringLiteral("seismicBackendLabel"));
  backendLabel->setFont(mono8);
  PaleoTheme::applyThemedStyleSheet(backendLabel,
                                    [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  timeBarLay->addWidget(backendLabel);

  // 后端状态探测（转码完成后的「热切换」提示；Auto 只认 .sf3c 伴生）
  const auto refreshBackendStatus = [this, abs, backendLabel](const QString &suffix = QString()) {
    auto *svc = (m_doc ? m_doc->seismicTaskService() : nullptr);
    if (!svc)
      return;
    const QPointer<QLabel> labelGuard(backendLabel);
    svc->startBackendProbe(abs, [labelGuard, suffix](bool ok, const seismic::SeismicBackendStatus &s, const QString &) {
      if (!labelGuard)
        return;
      if (!ok)
      {
        labelGuard->setText(QObject::tr("后端：探测失败"));
        return;
      }
      QString name;
      if (s.backendName == QLatin1String("workspace"))
        name = QObject::tr(".sf3c 工作区");
      else if (s.backendName == QLatin1String("paged-workspace"))
        name = QObject::tr(".sf3p 分页工作区");
      else
        name = QObject::tr("直读 SEG-Y");
      QString text = QObject::tr("后端：%1%2").arg(
          name, s.fellBackToDirect ? QObject::tr("（回退直读）") : QString());
      if (s.backendName == QLatin1String("paged-workspace") && !s.quality.isEmpty())
        text += QStringLiteral(" · %1").arg(s.quality);
      if (!suffix.isEmpty())
        text += suffix;
      labelGuard->setText(text);
    });
  };

  // 转码任务的进度条/取消接线（两通道共用同一组控件；实现见匿名命名空间的
  // bindTranscodeTaskUi——绑定前摘旧连接 + 在途任务计数管可见性）。
  const auto activeTasks = std::make_shared<int>(0);
  const auto boundTask = std::make_shared<QPointer<PaleoTask>>();
  const auto bindTranscodeTask = [transcodeProgress, btnCancelTranscode, activeTasks,
                                  boundTask](PaleoTask *task) {
    bindTranscodeTaskUi(transcodeProgress, btnCancelTranscode, task, activeTasks, boundTask);
  };

  connect(btnTranscode, &QToolButton::clicked, host, [this, abs, btnTranscode, bindTranscodeTask, refreshBackendStatus]() {
    auto *svc = (m_doc ? m_doc->seismicTaskService() : nullptr);
    if (!svc)
      return;
    // D1.2：点击时重新探测——续跑/新建/重建三种话术
    const seismic::SeismicWorkspaceProbe wp = svc->probeWorkspace(abs);
    QString questionText;
    if (wp.exists && wp.readable && !wp.complete)
      questionText = tr("检测到未完成的 .sf3c 工作区（%1）。\n继续转码（已写分片自动跳过）？")
                         .arg(wp.stateText());
    else if (wp.exists && !wp.readable)
      questionText = tr("现有 .sf3c 工作区不可读（%1）。\n重新转码将自动重建，继续？")
                         .arg(wp.error.isEmpty() ? tr("格式版本不支持") : wp.error);
    else
      questionText = tr("将 %1 转码为 .sf3c 分片工作区（体积与源文件同量级）。\n"
                        "过程可取消并续跑；完成后切片与任意剖面走随机访问后端。")
                         .arg(QFileInfo(abs).fileName());
    if (!PaleoNotify::ask(btnTranscode, tr("转码地震工作区"), questionText))
      return;
    btnTranscode->setEnabled(false);
    btnTranscode->setText(tr("转码中…（可取消）"));
    const QPointer<QToolButton> guard(btnTranscode);
    PaleoTask *task = svc->startWorkspaceTranscodeDetailed(
        abs, QString(),
        [guard, refreshBackendStatus](bool ok, const QString &, const QString &err) {
      if (!guard)
        return;
      if (ok)
      {
        guard->setText(QObject::tr("工作区已就绪"));
        guard->setEnabled(false);
        refreshBackendStatus(QObject::tr("（已热切换）"));
      }
      else
      {
        // D1.2：取消/失败后若留有半成品，入口变「继续转码」
        guard->setText(QObject::tr("继续转码 (.sf3c)"));
        guard->setEnabled(true);
        if (!err.isEmpty())
          PaleoNotify::warning(guard, QObject::tr("转码未完成"), err);
      }
    },
        [guard](const seismic::SeismicTranscodeReport &report) {
      if (!guard)
        return;
      // D1.4：质量报告挂按钮 tooltip（道数/覆盖率/丢弃率/值域）
      guard->setToolTip(report.summaryLine());
      if (report.ok)
        PaleoNotify::information(guard, QObject::tr("转码完成"), report.summaryLine());
      else if (report.damagedTraces > 0)
        PaleoNotify::warning(guard, QObject::tr("转码包含坏道"),
                             QObject::tr("损坏源道 %1 条已跳过（NaN 填充），如 %2…")
                                 .arg(report.damagedTraces)
                                 .arg(report.damagedSample.isEmpty() ? QString() : report.damagedSample.first()));
    });
    bindTranscodeTask(task);
  });

  connect(btnPagedTranscode, &QToolButton::clicked, host,
          [this, abs, pagedPathForButtons, btnPagedTranscode, bindTranscodeTask, refreshBackendStatus,
           sharedPaged, panel3d, sharedVol]() {
    auto *svc = (m_doc ? m_doc->seismicTaskService() : nullptr);
    if (!svc)
      return;
    // D1.2：点击时重新探测（.partial 半成品 → 续跑话术）
    const seismic::SeismicWorkspaceProbe pp = svc->probePagedWorkspace(pagedPathForButtons);
    QString pagedQuestion;
    if (pp.exists && !pp.complete)
      pagedQuestion = tr("检测到未完成的 .sf3p 分页工作区（%1）。\n继续转码（已完成页自动跳过）？")
                          .arg(pp.stateText());
    else
      pagedQuestion = tr("将 %1 转码为 .sf3p 分页工作区并按体量自适应构建金字塔\n"
                         "（含瓦片渐进时间片与渐进 LOD；体积与源文件同量级）。\n"
                         "过程可取消并续跑。")
                          .arg(QFileInfo(abs).fileName());
    if (!PaleoNotify::ask(btnPagedTranscode, tr("转码分页工作区"), pagedQuestion))
      return;
    btnPagedTranscode->setEnabled(false);
    btnPagedTranscode->setText(tr("转码中…（可取消）"));
    const QPointer<QToolButton> guard(btnPagedTranscode);
    PaleoTask *task = svc->startPagedTranscodeDetailed(
        abs, pagedPathForButtons, /*buildLod=*/true,
        [guard, refreshBackendStatus, sharedPaged, panel3d, sharedVol,
         pagedPathForButtons](bool ok, const QString &, const QString &err) {
      if (!guard)
        return;
      if (ok)
      {
        guard->setText(QObject::tr("分页工作区已就绪"));
        guard->setEnabled(false);
        // 热切换：启用显式 .sf3p 通道（瓦片时间片 + 3D 渐进 LOD）
        *sharedPaged = pagedPathForButtons;
        if (*sharedVol != nullptr)
          panel3d->setPagedWorkspace(*sharedPaged);
        refreshBackendStatus(QObject::tr("（已热切换）"));
      }
      else
      {
        guard->setText(QObject::tr("继续转码 (.sf3p)"));
        guard->setEnabled(true);
        if (!err.isEmpty())
          PaleoNotify::warning(guard, QObject::tr("分页转码未完成"), err);
      }
    },
        [guard](const seismic::SeismicTranscodeReport &report) {
      if (!guard)
        return;
      guard->setToolTip(report.summaryLine());
      if (report.ok)
        PaleoNotify::information(guard, QObject::tr("分页转码完成"), report.summaryLine());
    });
    bindTranscodeTask(task);
  });
  refreshBackendStatus();

  timeBarLay->addStretch(1);
  layTime->addWidget(new PaleoToolRow(timeBar, wTime));

  auto *timeCanvas = new seismic::SeismicSectionCanvas(wTime);
  timeCanvas->setObjectName(QStringLiteral("timeSliceCanvas"));
  timeCanvas->setColorMap(seismic::SectionColorMapType::RedWhiteBlue);
  timeCanvas->setGain(1.2f);
  timeCanvas->setContrast(1.3f);
  layTime->addWidget(timeCanvas, 1);

  connect(btnFitTime, &QToolButton::clicked, timeCanvas, &seismic::SeismicSectionCanvas::fitToWindow);

  modeTabs->addTab(wTime, tr("水平时间切片 (Time Slice)"));

  // ---- 两段式秒开（主线1）：QuickOpen 秒级预览 → 后台体加载换装 ----
  auto *quickInfo = new QLabel(host);
  quickInfo->setObjectName(QStringLiteral("seismicQuickInfo"));
  quickInfo->setFont(mono8);
  PaleoTheme::applyThemedStyleSheet(quickInfo,
                                    [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  quickInfo->setVisible(false);

  const QPointer<QLabel> quickInfoGuard(quickInfo);
  const QPointer<seismic::Seismic3DViewPanel> panel3dGuard(panel3d);

  // 体就绪后的统一换装（两段共用；只装一次）
  const auto finishVolumeSetup = [panel3dGuard](const std::shared_ptr<seismic::SgyVolume> &vol) {
    if (!panel3dGuard || !vol)
      return;
    if (panel3dGuard->viewport())
    {
      panel3dGuard->viewport()->setPresetView(seismic::SeismicCameraController::PresetView::Isometric);
      panel3dGuard->viewport()->fitToBounds();
    }


  };

  const auto installVolume = [sharedVol, sharedPaged, panel3dGuard, finishVolumeSetup](
                                 const std::shared_ptr<seismic::SgyVolume> &vol) {
    // #280（UIV-02）：标签页可能已被关闭、3D 面板已销毁——守卫先于一切
    // 解引用，且在写 sharedVol 之前（没地方装的结果不占位）。
    if (!panel3dGuard || !vol || (*sharedVol && !(*sharedVol)->IsRuleBased()))
      return;
    const bool promoted=*sharedVol && panel3dGuard->promoteIndexedVolume(vol);
    *sharedVol = vol;
    if(promoted)return;
    // paged 通道先于 volume：初始三槽切片请求即带 LOD 映射（从最粗层起步）
    if (!sharedPaged->isEmpty())
      panel3dGuard->setPagedWorkspace(*sharedPaged);
    panel3dGuard->setVolume(vol);
    finishVolumeSetup(vol);
  };

  const auto ensureVolumeLoaded = [this, abs, sharedVol, panel3dGuard, quickInfoGuard, installVolume, host]() {
    if (*sharedVol != nullptr || abs.isEmpty() || !QFile::exists(abs))
      return;
    auto *svc = (m_doc ? m_doc->seismicTaskService() : nullptr);
    if (panel3dGuard && svc)
      panel3dGuard->setTaskService(svc);

    if (!svc)
    {
      // 无任务服务（小夹具测试环境）：保留同步加载路径
      auto vol = std::make_shared<seismic::SgyVolume>();
      std::string volErr;
      if (vol->Load(paleo::toFsPath(abs), volErr))
        installVolume(vol);
      return;
    }

    // 第一段：QuickOpen 秒级首屏（网格/角点/中央测线真振幅缩略）
    svc->startQuickOpen(abs, 128, [quickInfoGuard, installVolume](bool ok, const seismic::SeismicQuickPreview &p) {
      if (!quickInfoGuard)
        return;
      if (!ok)
      {
        quickInfoGuard->setText(QObject::tr("秒开失败：%1").arg(p.error));
        quickInfoGuard->setVisible(true);
        return;
      }
      QString text;
      if (p.ruleVerified)
      {
        text = QObject::tr("秒开 %1 ms · IL %2–%3 · XL %4–%5 · %6 道 × %7 样点 · 预览 IL %8")
                   .arg(p.totalMs, 0, 'f', 0)
                   .arg(p.inlineMin).arg(p.inlineMax)
                   .arg(p.xlineMin).arg(p.xlineMax)
                   .arg(p.traceCount).arg(p.sampleCount)
                   .arg(p.previewInline);
      }
      else
      {
        text = QObject::tr("秒开 %1 ms · %2").arg(p.totalMs, 0, 'f', 0).arg(p.summary);
      }
      if(p.ruleVerified)text+=QObject::tr(" · 3D 粗预览，后台建立完整索引");
      quickInfoGuard->setText(text);
      quickInfoGuard->setVisible(true);

      // 快速规则快照只用于 3D 浏览；普通 2D 剖面一律由 Data_service 获取。
      if(p.ruleVerified&&p.volume)installVolume(p.volume);
    });

    // 第二段：后台体加载（.sgyidx 命中时秒级），完成换装 3D/时间片面板。
    // 回调连在长寿的 SeismicTaskService 上，不随页签断开——ok 分支的
    // QPointer 守卫与 installVolume 首行守卫是防「关标签后迟到回调对已
    // 销毁 3D 面板调 setVolume」的双保险（#280）。
    if (PaleoTask *volTask = svc->startVolumeLoad(
            abs, [quickInfoGuard, panel3dGuard, installVolume](
                     bool ok, std::shared_ptr<seismic::SgyVolume> vol, const QString &err) {
              if (ok)
              {
                if (panel3dGuard)installVolume(vol);
                if(quickInfoGuard){quickInfoGuard->setText(QObject::tr("索引已就绪 · 3D 原始分辨率交互"));quickInfoGuard->setVisible(true);}
                return;
              }
              if (quickInfoGuard && !err.isEmpty())
              {
                quickInfoGuard->setText(QObject::tr("体加载失败：%1").arg(err));
                quickInfoGuard->setVisible(true);
              }
            });
        volTask)
    {
      // #280：关标签 → 页销毁即取消在途体加载（大体积不白跑）；任务终态
      // 回收（deleteLater）由 QPointer 兜底。
      const QPointer<PaleoTask> volTaskGuard(volTask);
      QObject::connect(host, &QObject::destroyed, svc, [volTaskGuard] {
        if (volTaskGuard)
          volTaskGuard->requestCancel();
      });
    }
  };
  // 工区打开即启动两段式（不等页签切换）；页签切换回调只做幂等兜底与聚焦
  ensureVolumeLoaded();

  // 2D 时间片独立查询 Data_service，不等待、不读取 3D 的索引或分页工作区。
  auto *sliceDebounce=new QTimer(host);sliceDebounce->setSingleShot(true);sliceDebounce->setInterval(120);
  auto pendingIdx=std::make_shared<int>(-1);
  auto nativeInfo=std::make_shared<seismic::SeismicNativeInfo>();
  auto nativeRead=std::make_shared<QPointer<PaleoTask>>();
  auto nativeGeneration=std::make_shared<quint64>(0);
  const QPointer<seismic::SeismicSectionCanvas> nativeCanvas(timeCanvas);
  const auto requestTimeSlice=[this,abs,nativeInfo,nativeRead,nativeCanvas,lblTimeMs,pendingIdx,nativeGeneration](int index){
    if(!nativeCanvas||index<0||index>=nativeInfo->sampleCount)return;
    const auto generation=++(*nativeGeneration);
    if(*nativeRead)(*nativeRead)->requestCancel();
    const double ms=nativeInfo->firstTimeMs+index*nativeInfo->sampleIntervalMs;
    lblTimeMs->setText(QStringLiteral("%1 ms").arg(ms,0,'f',1));
    auto* service=m_doc?m_doc->seismicTaskService():nullptr;
    if(!service){nativeCanvas->setNoDataReason(tr("后台地震服务未连接"));return;}
    *nativeRead=service->startDataSlice(abs,seismic::SgySliceType::Time,index,
      [nativeCanvas,nativeInfo,pendingIdx,index,ms,nativeGeneration,generation](bool ok,std::shared_ptr<const seismic::SgySliceImage> image,const QString& error){
        if(!nativeCanvas||*pendingIdx!=index||*nativeGeneration!=generation)return;
        if(!ok||!image){nativeCanvas->clearData();nativeCanvas->setNoDataReason(QObject::tr("时间切片获取失败\n%1").arg(error));return;}
        nativeCanvas->setTimeSliceData(*image,ms,nativeInfo->inlineMin,nativeInfo->inlineMax,nativeInfo->xlineMin,nativeInfo->xlineMax);
      });
  };
  connect(host,&QObject::destroyed,this,[nativeRead]{if(*nativeRead)(*nativeRead)->requestCancel();});
  connect(sliceDebounce,&QTimer::timeout,host,[pendingIdx,requestTimeSlice]{if(*pendingIdx>=0)requestTimeSlice(*pendingIdx);});
  const auto updateTimeSlice = [pendingIdx, sliceDebounce](int sampleIndex) {
    *pendingIdx = sampleIndex;
    sliceDebounce->start();
  };

  connect(sliderTime, &QSlider::valueChanged, host, [spinTime, updateTimeSlice](int val) {
    spinTime->blockSignals(true);
    spinTime->setValue(val);
    spinTime->blockSignals(false);
    updateTimeSlice(val);
  });

  connect(spinTime, QOverload<int>::of(&QSpinBox::valueChanged), host, [sliderTime, updateTimeSlice](int val) {
    sliderTime->blockSignals(true);
    sliderTime->setValue(val);
    sliderTime->blockSignals(false);
    updateTimeSlice(val);
  });

  if(auto* service=m_doc?m_doc->seismicTaskService():nullptr) {
    const QPointer<QSlider> slider(sliderTime);const QPointer<QSpinBox> spin(spinTime);
    const QPointer<PaleoTask> infoTask=service->startDataInfo(abs,[nativeCanvas,nativeInfo,slider,spin,updateTimeSlice](bool ok,const seismic::SeismicNativeInfo& info,const QString& error){
      if(!nativeCanvas||!slider||!spin)return;
      if(!ok){nativeCanvas->setNoDataReason(QObject::tr("地震元数据查询失败\n%1").arg(error));return;}
      *nativeInfo=info;QSignalBlocker a(slider),b(spin);slider->setRange(0,qMax(0,info.sampleCount-1));spin->setRange(0,qMax(0,info.sampleCount-1));
      const int mid=qMax(0,info.sampleCount-1)/2;slider->setValue(mid);spin->setValue(mid);updateTimeSlice(mid);
    });
    connect(host,&QObject::destroyed,service,[infoTask]{if(infoTask)infoTask->requestCancel();});
  }
  connect(modeTabs, &QTabWidget::currentChanged, host, [ensureVolumeLoaded, panel3d, timeCanvas, updateTimeSlice, sliderTime](int idx) {
    if (idx == 1)
    {
      ensureVolumeLoaded();
      if (panel3d->viewport())
      {
        panel3d->viewport()->fitToBounds();
        panel3d->viewport()->update();
      }
    }
    else if (idx == 2)
    {
      ensureVolumeLoaded();
      updateTimeSlice(sliderTime->value());
      QTimer::singleShot(20, timeCanvas, [timeCanvas]() {
        timeCanvas->fitToWindow();
      });
    }
  });

  lay->addWidget(quickInfo);
  lay->addWidget(modeTabs, 1);
  return host;
}
