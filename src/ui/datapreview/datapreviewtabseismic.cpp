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
  no->setRange(static_cast<int>(survey.inlineMin),
               static_cast<int>(qMax(survey.inlineMax, survey.inlineMin)));
  if (survey.inlineMin == 0 && survey.inlineMax == 0) // 无 survey 元数据时放开范围
    no->setRange(0, 1000000);
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
    const bool isInline = mode->currentData().toString() == QLatin1String("inline");
    no->setRange(isInline ? static_cast<int>(survey.inlineMin) : static_cast<int>(survey.xlineMin),
                 isInline ? static_cast<int>(qMax(survey.inlineMax, survey.inlineMin))
                          : static_cast<int>(qMax(survey.xlineMax, survey.xlineMin)));
    decode();
  });
  connect(no, &QSpinBox::valueChanged, host, decode);
  decode();
  barLay->addWidget(mode);
  barLay->addWidget(no);
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
  auto *w2d = new QWidget(modeTabs);
  w2d->setObjectName(QStringLiteral("seismic2DContainer"));
  auto *lay2d = new QVBoxLayout(w2d);
  lay2d->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  lay2d->setSpacing(PaleoTheme::tokens().spacingXs);
  lay2d->addWidget(caption8(tr("选择一条测线解码"), w2d));
  lay2d->addWidget(bar);
  lay2d->addWidget(tieCaption);
  lay2d->addWidget(panel, 1);
  modeTabs->addTab(w2d, tr("二维测线 (2D)"));

  // 2. 三维立体 (3D)
  auto *panel3d = new seismic::Seismic3DViewPanel(modeTabs);
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

  // 转码任务的进度条/取消接线（两通道共用）
  const auto bindTranscodeTask = [transcodeProgress, btnCancelTranscode](PaleoTask *task) {
    if (!task)
      return;
    transcodeProgress->setVisible(true);
    transcodeProgress->setValue(0);
    btnCancelTranscode->setVisible(true);
    const QPointer<PaleoTask> taskGuard(task);
    QObject::connect(task, &PaleoTask::changed, transcodeProgress, [taskGuard, transcodeProgress]() {
      if (taskGuard)
        transcodeProgress->setValue(taskGuard->percent());
    });
    QObject::connect(btnCancelTranscode, &QToolButton::clicked, task, &PaleoTask::requestCancel);
    QObject::connect(task, &PaleoTask::finished, transcodeProgress, [transcodeProgress, btnCancelTranscode]() {
      transcodeProgress->setVisible(false);
      btnCancelTranscode->setVisible(false);
    });
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
  const QPointer<QWidget> sectionPanelGuard(panel);
  const QPointer<seismic::Seismic3DViewPanel> panel3dGuard(panel3d);

  // 体就绪后的统一换装（两段共用；只装一次）
  const auto finishVolumeSetup = [panel3dGuard, sliderTime, spinTime](const std::shared_ptr<seismic::SgyVolume> &vol) {
    if (!panel3dGuard || !vol)
      return;
    if (panel3dGuard->viewport())
    {
      panel3dGuard->viewport()->setPresetView(seismic::SeismicCameraController::PresetView::Isometric);
      panel3dGuard->viewport()->fitToBounds();
    }

    sliderTime->blockSignals(true);
    spinTime->blockSignals(true);
    sliderTime->setRange(0, vol->SampleMax());
    spinTime->setRange(0, vol->SampleMax());
    const int mid = vol->SampleMax() / 2;
    sliderTime->setValue(mid);
    spinTime->setValue(mid);
    sliderTime->blockSignals(false);
    spinTime->blockSignals(false);
  };

  const auto installVolume = [sharedVol, sharedPaged, panel3dGuard, finishVolumeSetup](
                                 const std::shared_ptr<seismic::SgyVolume> &vol) {
    if (*sharedVol != nullptr || !vol)
      return;
    *sharedVol = vol;
    // paged 通道先于 volume：初始三槽切片请求即带 LOD 映射（从最粗层起步）
    if (!sharedPaged->isEmpty())
      panel3dGuard->setPagedWorkspace(*sharedPaged);
    panel3dGuard->setVolume(vol);
    finishVolumeSetup(vol);
  };

  const auto ensureVolumeLoaded = [this, abs, sharedVol, panel3dGuard, quickInfoGuard, sectionPanelGuard, installVolume]() {
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
      if (vol->Load(abs.toStdString(), volErr))
        installVolume(vol);
      return;
    }

    // 第一段：QuickOpen 秒级首屏（网格/角点/中央测线真振幅缩略）
    svc->startQuickOpen(abs, 128, [quickInfoGuard, sectionPanelGuard](bool ok, const seismic::SeismicQuickPreview &p) {
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
      quickInfoGuard->setText(text);
      quickInfoGuard->setVisible(true);

      // 秒级缩略：真振幅中央测线先上 2D 剖面（测线解码结果到达后自然替换）
      if (p.ruleVerified && p.preview && p.preview->width > 0 && sectionPanelGuard)
      {
        auto *sectionPanel = static_cast<SectionPanel *>(sectionPanelGuard.data());
        if (!sectionPanel->hasImage())
        {
          QVector<SegyTrace> traces;
          traces.reserve(p.preview->width);
          for (int col = 0; col < p.preview->width; ++col)
          {
            SegyTrace t;
            t.lineNo = p.previewInline;
            t.xlineNo = p.xlineMin + col;
            t.cdp = t.xlineNo;
            t.sampleIntervalUs = static_cast<float>(p.sampleIntervalUs);
            t.samples.reserve(p.preview->height);
            for (int s = 0; s < p.preview->height; ++s) // row 0 = 最深采样
              t.samples.push_back(p.preview->values[
                  static_cast<std::size_t>(p.preview->height - 1 - s) * p.preview->width + col]);
            traces.push_back(t);
          }
          sectionPanel->setTraces(traces, static_cast<float>(p.sampleIntervalUs), 0.0);
        }
      }
    });

    // 第二段：后台体加载（.sgyidx 命中时秒级），完成换装 3D/时间片面板
    svc->startVolumeLoad(abs, [quickInfoGuard, installVolume](
                                  bool ok, std::shared_ptr<seismic::SgyVolume> vol, const QString &err) {
      if (ok)
      {
        installVolume(vol);
        return;
      }
      if (quickInfoGuard && !err.isEmpty())
      {
        quickInfoGuard->setText(QObject::tr("体加载失败：%1").arg(err));
        quickInfoGuard->setVisible(true);
      }
    });
  };
  // 工区打开即启动两段式（不等页签切换）；页签切换回调只做幂等兜底与聚焦
  ensureVolumeLoaded();

  // 时间片走任务服务：sdk::Dataset 在已转码时命中工作区随机访问后端；
  // 未转码走 Direct（同一份 SgyVolume 实现）。防抖 120ms，仅贴最新请求。
  // 取舍（主线2）：显式 .sf3p 存在时走瓦片渐进（引擎焦点优先，冷缓存
  // 首见先出中心瓦片再补边角）；直读/热缓存一次性整图更省——两路并存。
  auto *sliceDebounce = new QTimer(host);
  sliceDebounce->setSingleShot(true);
  sliceDebounce->setInterval(120);
  const auto pendingIdx = std::make_shared<int>(-1);

  // 瓦片信号路由（每服务接一次）：只贴最新一次瓦片请求的目标画布，
  // 采样号世代不符（陈旧请求/其他资产标签）直接丢弃。
  if (auto *tileSvc = (m_doc ? m_doc->seismicTaskService() : nullptr);
      tileSvc && m_tiledSignalService != tileSvc)
  {
    m_tiledSignalService = tileSvc;
    connect(tileSvc, &seismic::SeismicTaskService::timeSliceTileReady, this,
            [this](const seismic::SeismicTimeTile &tile) {
              if (!m_tiledCanvas || tile.sampleIndex != m_tiledSample || !tile.image)
                return;
              auto *canvas = qobject_cast<seismic::SeismicSectionCanvas *>(m_tiledCanvas.data());
              if (canvas)
                canvas->appendTimeSliceTile(*tile.image, tile.x, tile.y);
            });
  }

  const auto requestTimeSlice = [this, sharedVol, sharedPaged, timeCanvas, lblTimeMs, pendingIdx](int sampleIndex) {
    if (!*sharedVol || !(*sharedVol)->IsLoaded())
      return;
    const auto &vol = *sharedVol;
    const double ms = sampleIndex * (vol->SampleIntervalUs() / 1000.0);
    lblTimeMs->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));

    auto *svc = (m_doc ? m_doc->seismicTaskService() : nullptr);
    const QPointer<seismic::SeismicSectionCanvas> canvasGuard(timeCanvas);
    if (svc && !sharedPaged->isEmpty())
    {
      // paged 通道：瓦片渐进（焦点=网格中心；tileSize 64 与引擎页几何匹配）。
      // 焦点取轴上真值（InlineValues/XlineValues 中位）——step≠1 的轴上
      // InlineMin()+count/2 不保证存在，引擎会退回默认中心。
      const auto &inlVals = vol->InlineValues();
      const auto &xlVals = vol->XlineValues();
      const int inlCount = qMax(1, static_cast<int>(inlVals.size()));
      const int xlCount = qMax(1, static_cast<int>(xlVals.size()));
      const int focusInl = inlVals[static_cast<std::size_t>(inlCount / 2)];
      const int focusXl = xlVals[static_cast<std::size_t>(xlCount / 2)];
      timeCanvas->beginTimeSliceTiled(xlCount, inlCount, ms,
                                      vol->InlineMin(), vol->InlineMax(),
                                      vol->XlineMin(), vol->XlineMax());
      m_tiledCanvas = timeCanvas;
      m_tiledSample = sampleIndex;
      // A3（wave/deepen-perf）：失败如实显示原因态（不再留整幅 NaN 灰无解释）；
      // 被新请求顶替的取消回调经世代过滤（*pendingIdx 已是最新采样号）丢弃。
      svc->startTimeSliceTiled(
          *sharedPaged, sampleIndex, 64, focusInl, focusXl,
          [canvasGuard, pendingIdx, sampleIndex](bool ok,
                                                 std::shared_ptr<const seismic::SgySliceImage> img,
                                                 const QString &error) {
            if (!canvasGuard || *pendingIdx != sampleIndex)
              return; // 陈旧请求（已被顶替/换采样）——静默丢弃
            if (!ok || !img) {
              canvasGuard->clearData();
              canvasGuard->setNoDataReason(
                  QObject::tr("时间切片获取失败（分页通道）\n%1").arg(error));
              return;
            }
            canvasGuard->finishTimeSliceTiled(*img);
          });
      return;
    }
    if (svc)
    {
      // A3：直读/工作区通道同一空态语义（失败原因上屏，不留旧图冒充新采样）
      svc->startSliceExtraction(
          vol, seismic::SgySliceType::Time, sampleIndex,
          [canvasGuard, pendingIdx, sampleIndex, ms, vol](
              bool ok, std::shared_ptr<const seismic::SgySliceImage> img, const QString &error) {
            if (!canvasGuard || *pendingIdx != sampleIndex)
              return;
            if (!ok || !img) {
              canvasGuard->clearData();
              canvasGuard->setNoDataReason(
                  QObject::tr("时间切片获取失败\n%1").arg(error));
              return;
            }
            canvasGuard->setTimeSliceData(*img, ms, vol->InlineMin(), vol->InlineMax(),
                                          vol->XlineMin(), vol->XlineMax());
          });
      return;
    }
    seismic::SgySliceImage img;
    std::string err;
    if (vol->ExtractSlice(seismic::SgySliceType::Time, sampleIndex, img, err))
    {
      timeCanvas->setTimeSliceData(img, ms, vol->InlineMin(), vol->InlineMax(), vol->XlineMin(), vol->XlineMax());
    }
  };
  connect(sliceDebounce, &QTimer::timeout, host, [pendingIdx, requestTimeSlice]() {
    if (*pendingIdx >= 0)
      requestTimeSlice(*pendingIdx);
  });
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
