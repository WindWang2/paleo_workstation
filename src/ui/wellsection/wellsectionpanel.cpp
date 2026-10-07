// 层：视图
#include "wellsectionpanel.h"

#include "domain/mappinghorizons.h"
#include "linkage/selectioncontext.h"
#include "ui/paleoicons.h"
#include "ui/paleotheme.h"
#include "wellsectiondialogs.h"

#include <QActionGroup>
#include <QCursor>
#include <QFileDialog>
#include <QGraphicsScene>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSvgGenerator>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <QtNumeric>
#include <cmath>

namespace {
QSettings panelSettings() { return QSettings(QStringLiteral("paleo"),
                                             QStringLiteral("paleo")); }
} // namespace

WellSectionPanel::WellSectionPanel(SelectionContext *ctx, QWidget *parent)
    : QWidget(parent), m_ctx(ctx)
{
  // 用户设置恢复（读是自由的；写只在用户动作里发生）。
  {
    QSettings s = panelSettings();
    m_themeId = s.value(QStringLiteral("wellSection/theme"),
                        QStringLiteral("classic")).toString();
    m_highlightOn =
        s.value(QStringLiteral("wellSection/highlight"), true).toBool();
    m_seismicOn =
        s.value(QStringLiteral("wellSection/seismic"), false).toBool();
    m_faultsOn =
        s.value(QStringLiteral("wellSection/faults"), false).toBool();
    const int mode = s.value(QStringLiteral("wellSection/datumMode"),
                             int(wellsection::DatumMode::Depth)).toInt();
    m_datum.mode = mode == int(wellsection::DatumMode::Elevation)
                       ? wellsection::DatumMode::Elevation
                       : (mode == int(wellsection::DatumMode::Flatten)
                              ? wellsection::DatumMode::Flatten
                              : wellsection::DatumMode::Depth);
    if (m_datum.mode == wellsection::DatumMode::Flatten)
      m_datum.flattenTop = s.value(QStringLiteral("wellSection/datumTop"))
                               .toString();
    m_spacing = s.value(QStringLiteral("wellSection/spacing"),
                        int(wellsection::SpacingMode::Equal))
                      .toInt() == int(wellsection::SpacingMode::Proportional)
                    ? wellsection::SpacingMode::Proportional
                    : wellsection::SpacingMode::Equal;
    const QByteArray tj =
        s.value(QStringLiteral("wellSection/template")).toByteArray();
    if (!tj.isEmpty())
      m_tpl = wellsection::SectionTemplate::fromJson(
          QJsonDocument::fromJson(tj).object());
    else
      m_tpl = wellsection::SectionTemplate::defaults();
  }
  m_st.theme = wellsection::SectionTheme::byId(m_themeId);
  m_st.tpl = m_tpl;
  m_st.datum = m_datum;

  m_scene = new QGraphicsScene(this);
  m_scene->setBackgroundBrush(m_st.theme.paper);
  m_view = new wellsectionui::View(&m_st, m_scene, this);
  m_view->setObjectName(QStringLiteral("wellSectionView"));
  m_view->setAccessibleName(tr("连井剖面画布"));
  m_header = new wellsectionui::HeaderWidget(&m_st, this);
  m_header->setObjectName(QStringLiteral("wellSectionHeader"));

  // ---- 工具行（QWidget + HBox，不用 QToolBar::addWidget——销毁序雷区）----
  auto *bar = new QWidget(this);
  bar->setObjectName(QStringLiteral("wellSectionToolBar"));
  PaleoTheme::applyThemedStyleSheet(bar, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
               "#wellSectionToolBar { background: %1; border-bottom: 1px solid "
               "%2; }")
               .arg(t.surfaceAlt.name(), t.border.name()) +
           PaleoTheme::toolButtonStyleSheet();
  });
  auto *barLay = new QHBoxLayout(bar);
  barLay->setContentsMargins(PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs); // spacing.xs
  barLay->setSpacing(PaleoTheme::tokens().spacingXs);

  const auto mkBtn = [bar, barLay](const char *obj, const char *icon,
                                   const QString &tip) {
    auto *b = new QToolButton(bar);
    b->setObjectName(QLatin1String(obj));
    b->setIcon(PaleoIcons::qgisTheme(QLatin1String(icon)));
    b->setIconSize(PaleoIcons::toolbarSize());
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setAccessibleName(tip);
    barLay->addWidget(b);
    return b;
  };

  m_wellsBtn = mkBtn("wellSectionWellsButton", "mIconPointLayer.svg",
                     tr("选择连井的井与顺序"));
  m_fromSelBtn = mkBtn("wellSectionFromSelectionButton", "mActionSelect.svg",
                       tr("用地图/图层树选中的井生成剖面（按井位排序）"));
  connect(m_fromSelBtn, &QToolButton::clicked, this,
          [this] { generateFromSelection(); });
  m_tracksBtn = mkBtn("wellSectionTracksButton", "mActionFilterTableFields.svg",
                      tr("设置显示的井道与参与连井的分层"));
  m_themeBtn = mkBtn("wellSectionThemeButton", "propertyicons/symbology.svg",
                     tr("剖面显示主题与高亮"));
  m_flattenBtn = mkBtn("wellSectionFlattenButton", "mActionAlignTop.svg",
                       tr("基准面：井深 / 海拔 / 按分层拉平"));
  m_seismicBtn = mkBtn("wellSectionSeismicButton", "mIconRasterLayer.svg",
                       tr("井间叠加地震剖面（按时深关系自适应缩放）"));
  m_seismicBtn->setCheckable(true);
  m_faultBtn = mkBtn("wellSectionFaultButton", "mIconLineLayer.svg",
                     tr("断层投绘（断面与剖面井径求交）"));
  m_faultBtn->setCheckable(true);
  // 恢复的开/关态落到按钮上（信号用 clicked——setChecked 不触发）。
  if (m_seismicOn)
  {
    m_st.seismicOn = true;
    m_st.gapPx = qBound(m_st.minGap(), m_st.gapPx, m_st.maxGap());
  }
  m_fenceBtn = mkBtn("wellSectionFenceButton", "mActionAddMap.svg",
                    tr("栅状图（多剖面井网，交点井联动）"));
  connect(m_fenceBtn, &QToolButton::clicked, this,
          [this] { emit fenceRequested(); });
  m_fitBtn = mkBtn("wellSectionFitButton", "mActionZoomFullExtent.svg",
                   tr("适应窗口（Ctrl+滚轮纵向缩放，Ctrl+Shift+滚轮调整井间距）"));
  m_exportBtn = mkBtn("wellSectionExportButton", "mActionSaveMapAsImage.svg",
                      tr("导出剖面图（PNG/PDF）"));
  barLay->addStretch(1);
  m_status = new QLabel(bar);
  m_status->setObjectName(QStringLiteral("wellSectionStatus"));
  PaleoTheme::applyThemedStyleSheet(
      m_status, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  barLay->addWidget(m_status);

  // 主题菜单：三套预设单选 + 高亮开关。
  m_themeMenu = new QMenu(m_themeBtn);
  {
    auto *grp = new QActionGroup(m_themeMenu);
    grp->setExclusive(true);
    for (const wellsection::SectionTheme &t : wellsection::SectionTheme::presets())
    {
      QAction *a = m_themeMenu->addAction(t.name);
      a->setObjectName(QStringLiteral("wellSectionTheme_%1").arg(t.id));
      a->setCheckable(true);
      a->setData(t.id);
      grp->addAction(a);
      const QString id = t.id;
      connect(a, &QAction::triggered, this,
              [this, id] { applyThemeFromMenu(id); });
    }
    m_themeMenu->addSeparator();
    m_highlightAct = m_themeMenu->addAction(tr("高亮当前地层"));
    m_highlightAct->setObjectName(QStringLiteral("wellSectionHighlightAction"));
    m_highlightAct->setCheckable(true);
    // 用户点选才写设置（triggered 不响应程序化 setChecked）。
    connect(m_highlightAct, &QAction::triggered, this, [this](bool on) {
      setHighlightEnabled(on);
      panelSettings().setValue(QStringLiteral("wellSection/highlight"), on);
    });
  }
  m_themeBtn->setMenu(m_themeMenu);
  m_themeBtn->setPopupMode(QToolButton::InstantPopup);

  // 基准面菜单：井深 / 海拔（补心） / 按分层拉平（任一标志层）。
  // 模式切换只改视图偏移与轴标签，井深数据永不改写（拉平不变量）。
  m_flattenMenu = new QMenu(m_flattenBtn);
  connect(m_flattenMenu, &QMenu::aboutToShow, this, [this] {
    m_flattenMenu->clear();
    const auto addMode = [this](const QString &title,
                                wellsection::DatumMode mode) {
      QAction *a = m_flattenMenu->addAction(title);
      a->setCheckable(true);
      a->setChecked(m_datum.mode == mode);
      connect(a, &QAction::triggered, this, [this, mode] {
        wellsection::Datum d = m_datum;
        d.mode = mode;
        if (mode != wellsection::DatumMode::Flatten)
          d.flattenTop.clear();
        applyDatumFromMenu(d);
      });
      return a;
    };
    addMode(tr("井深（MD）"), wellsection::DatumMode::Depth);
    addMode(tr("海拔（补心基准）"), wellsection::DatumMode::Elevation);
    m_flattenMenu->addSeparator();
    for (const QString &name : wellsection::orderedTopNames(m_st.wells))
    {
      QAction *a = m_flattenMenu->addAction(tr("拉平于 %1").arg(name));
      a->setCheckable(true);
      a->setChecked(m_datum.mode == wellsection::DatumMode::Flatten &&
                    m_datum.flattenTop == name);
      connect(a, &QAction::triggered, this, [this, name] {
        applyDatumFromMenu(
            wellsection::Datum{wellsection::DatumMode::Flatten, name});
      });
    }
  });
  m_flattenBtn->setMenu(m_flattenMenu);
  m_flattenBtn->setPopupMode(QToolButton::InstantPopup);
  m_flattenBtn->setCheckable(true); // 非井深模式期间按钮呈按下态

  // 井距菜单：等距 / 按井口距离比例（缺坐标段用中位距离）。
  m_spacingBtn = mkBtn("wellSectionSpacingButton", "mActionDecorationGrid.svg",
                       tr("井距：等距 / 按井口距离比例"));
  m_spacingMenu = new QMenu(m_spacingBtn);
  {
    const auto addSpacing = [this](const QString &title,
                                   wellsection::SpacingMode mode) {
      QAction *a = m_spacingMenu->addAction(title);
      a->setCheckable(true);
      a->setChecked(m_spacing == mode);
      connect(a, &QAction::triggered, this, [this, mode] {
        setSpacingMode(mode);
        panelSettings().setValue(QStringLiteral("wellSection/spacing"),
                                 int(mode));
      });
    };
    addSpacing(tr("等距"), wellsection::SpacingMode::Equal);
    addSpacing(tr("按井口距离比例"), wellsection::SpacingMode::Proportional);
  }
  m_spacingBtn->setMenu(m_spacingMenu);
  m_spacingBtn->setPopupMode(QToolButton::InstantPopup);
  m_spacingBtn->setCheckable(true);

  connect(m_wellsBtn, &QToolButton::clicked, this,
          [this] { openWellsDialog(); });
  connect(m_tracksBtn, &QToolButton::clicked, this,
          [this] { openTracksDialog(); });
  // 用户点击才写设置并发起地震请求（clicked 不响应程序化 setChecked）。
  connect(m_seismicBtn, &QToolButton::clicked, this, [this](bool on) {
    m_seismicOn = on;
    m_st.seismicOn = on;
    m_st.gapPx = qBound(m_st.minGap(), m_st.gapPx, m_st.maxGap());
    panelSettings().setValue(QStringLiteral("wellSection/seismic"), on);
    syncGapToolTips();
    applyLayout(); // 地震开时井间距下限变大
    if (on && m_wells.size() >= 2)
      emit seismicRequested();
  });
  // 用户点击才写设置并发起投绘请求（clicked 不响应程序化 setChecked）。
  connect(m_faultBtn, &QToolButton::clicked, this, [this](bool on) {
    m_faultsOn = on;
    m_st.faultsOn = on;
    panelSettings().setValue(QStringLiteral("wellSection/faults"), on);
    syncToolbarState();
    if (on && m_wells.size() >= 2)
      emit faultsRequested();
    else if (!on)
      setFaultTraces({}, QString());
  });
  connect(m_fitBtn, &QToolButton::clicked, this, [this] { fitToView(); });
  connect(m_exportBtn, &QToolButton::clicked, this, [this] {
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出剖面图"), QString(),
        tr("PNG 图片 (*.png);;PDF 文档 (*.pdf);;SVG 矢量图 (*.svg);;"
           "层位井深表 CSV (*.csv)"));
    if (!path.isEmpty())
      exportTo(path);
  });

  // ---- 版头 + 视图 + 空态（同格叠加）----
  connect(m_view->horizontalScrollBar(), &QScrollBar::valueChanged, m_header,
          &wellsectionui::HeaderWidget::setScrollOffset);
  connect(m_view, &wellsectionui::View::depthZoomRequested, this,
          [this](double f, double anchorDepth, int vpY) {
            m_autofit = false;
            m_st.pxPerMeter = qBound(0.02, m_st.pxPerMeter * f, 40.0);
            ++m_st.curveVersion;
            applyLayout();
            const double newY =
                (anchorDepth - m_st.window.top) * m_st.pxPerMeter;
            m_view->verticalScrollBar()->setValue(int(newY - vpY));
          });
  connect(m_view, &wellsectionui::View::gapZoomRequested, this,
          [this](double f) {
            m_autofit = false;
            m_st.gapPx = qBound(m_st.minGap(), m_st.gapPx * f,
                                m_st.maxGap());
            applyLayout();
          });
  // resize 里重布局会把 LayoutRequest 递归进 posted 事件投递——refit
  // 挪到事件循环下一拍，且连发 resize 合并成一次（拉伸不抖、不堆重绘）。
  connect(m_view, &wellsectionui::View::viewportResized, this, [this] {
    if (m_refitPending || !m_autofit || m_st.wells.isEmpty())
      return;
    m_refitPending = true;
    QTimer::singleShot(0, this, [this] {
      m_refitPending = false;
      if (m_autofit && !m_st.wells.isEmpty())
        fitToView();
    });
  });
  connect(m_view, &wellsectionui::View::hoverChanged, this,
          [this](const QString &text) {
            m_hoverText = text;
            updateStatus();
          });
  const auto selectAt = [this](int i) {
    if (i < 0 || i >= m_wells.size())
      return;
    m_selectedId = m_wells[i].id;
    m_st.selected = i;
    m_header->update();
    m_view->viewport()->update();
    if (m_ctx)
      m_ctx->setSelection({m_selectedId}, QStringLiteral("wellsection"));
    emit wellClicked(m_selectedId);
  };
  connect(m_view, &wellsectionui::View::columnClicked, this, selectAt);
  connect(m_header, &wellsectionui::HeaderWidget::wellClicked, this, selectAt);
  connect(m_header, &wellsectionui::HeaderWidget::reorderRequested, this,
          [this](int from, int to) { moveWell(from, to); });
  connect(m_header, &wellsectionui::HeaderWidget::removeRequested, this,
          [this](int i) { removeWellAt(i); });

  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);
  lay->addWidget(bar);
  lay->addWidget(m_header);
  auto *cell = new QGridLayout;
  cell->setContentsMargins(0, 0, 0, 0);
  cell->setSpacing(0);
  cell->addWidget(m_view, 0, 0);

  m_empty = new QWidget(this);
  m_empty->setObjectName(QStringLiteral("wellSectionEmpty"));
  PaleoTheme::applyThemedStyleSheet(m_empty, [] {
    return QStringLiteral("#wellSectionEmpty { background: %1; }")
        .arg(PaleoTheme::tokens().surface.name());
  });
  auto *emptyLay = new QVBoxLayout(m_empty);
  auto *emptyLabel = new QLabel(tr("选择两口以上的井，按地层建立连井剖面"),
                                m_empty);
  emptyLabel->setObjectName(QStringLiteral("wellSectionEmptyLabel"));
  emptyLabel->setAlignment(Qt::AlignCenter);
  PaleoTheme::applyThemedStyleSheet(
      emptyLabel, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  auto *pick = new QPushButton(tr("选择井"), m_empty);
  pick->setObjectName(QStringLiteral("wellSectionEmptyPickButton"));
  connect(pick, &QPushButton::clicked, this, [this] { openWellsDialog(); });
  emptyLay->addStretch(1);
  emptyLay->addWidget(emptyLabel, 0, Qt::AlignHCenter);
  emptyLay->addSpacing(PaleoTheme::tokens().spacingSm);
  emptyLay->addWidget(pick, 0, Qt::AlignHCenter);
  emptyLay->addStretch(1);
  cell->addWidget(m_empty, 0, 0);
  lay->addLayout(cell, 1);

  m_header->setVisible(false);

  if (m_ctx)
  {
    connect(m_ctx, &SelectionContext::selectionChanged, this,
            [this](const QStringList &ids, const QString &origin) {
              if (origin != QLatin1String("wellsection"))
                updateSelection(ids, origin);
            });
    connect(m_ctx, &SelectionContext::activeHorizonChanged, this,
            [this](const QString &) {
              updateHighlight();
              ensureActiveIntervalVisible();
            });
  }
  syncToolbarState();
  updateStatus();
}

void WellSectionPanel::setWellChoices(const QVector<WellChoice> &choices)
{
  m_choices = choices;
}

void WellSectionPanel::setMnemonicChoices(const QStringList &mnemonics)
{
  m_mnemonicChoices = mnemonics;
}

void WellSectionPanel::setWellIds(const QStringList &ids)
{
  // 程序化恢复：发数据请求但不发 wellIdsChanged（持久化只记用户改动）。
  m_ids = ids;
  m_wells.clear();
  clearStrip();
  rebuildFiltered();
  rebuildItems();
  emit dataRequested(m_ids, m_tpl.mnemonics());
}

void WellSectionPanel::setSection(const QVector<wellsection::Well> &wells)
{
  m_wells = wells;
  m_ids.clear();
  for (const auto &w : m_wells)
    m_ids << w.id;
  clearStrip();
  rebuildFiltered();
  rebuildItems();
  m_empty->setVisible(m_wells.isEmpty() && !m_busy);
  m_header->setVisible(!m_wells.isEmpty());
  updateHighlight();
  updateStatus();
  if (m_autofit)
    fitToView();
  else
    applyLayout();
  syncToolbarState();
  if (m_seismicOn && m_wells.size() >= 2)
    emit seismicRequested();
  if (m_faultsOn && m_wells.size() >= 2)
    emit faultsRequested();
}

void WellSectionPanel::setSeismicStrip(const wellsection::SeismicStrip &strip)
{
  if (strip.gaps.size() != m_wells.size() - 1)
    return; // 与当前井集不符 → 丢弃
  m_st.strip = strip;
  ++m_st.stripVersion;
  syncGapToolTips();
  for (auto *g : m_gapItems)
    g->update();
  m_view->viewport()->update();
}

void WellSectionPanel::setSeismicAvailable(bool available,
                                           const QString &reason)
{
  m_seismicAvailable = available;
  m_seismicReason = reason;
  syncToolbarState();
}

void WellSectionPanel::selectWell(const QString &wellId)
{
  if (wellId.isEmpty() || !m_ids.contains(wellId))
    return;
  m_selectedId = wellId;
  m_st.selected = -1;
  for (int i = 0; i < m_st.wells.size(); ++i)
    if (m_st.wells[i].id == wellId)
      m_st.selected = i;
  m_header->update();
  m_view->viewport()->update();
}

void WellSectionPanel::setBusy(bool busy)
{
  m_busy = busy;
  m_empty->setVisible(m_wells.isEmpty() && !m_busy);
  updateStatus();
}

void WellSectionPanel::setWarnings(const QStringList &warnings)
{
  m_warnings = warnings;
  updateStatus();
}

void WellSectionPanel::setSectionTemplate(const wellsection::SectionTemplate &t)
{
  const bool mnemonicsChanged = t.mnemonics() != m_tpl.mnemonics();
  m_tpl = t;
  rebuildFiltered();
  applyLayout();
  m_header->update();
  if (mnemonicsChanged)
    emit dataRequested(m_ids, m_tpl.mnemonics());
}

void WellSectionPanel::setThemeId(const QString &id)
{
  m_themeId = wellsection::SectionTheme::byId(id).id;
  m_st.theme = wellsection::SectionTheme::byId(m_themeId);
  m_scene->setBackgroundBrush(m_st.theme.paper);
  applyLayout();
  m_header->update();
  syncToolbarState();
}

void WellSectionPanel::setHighlightEnabled(bool on)
{
  m_highlightOn = on;
  if (m_highlightAct)
  {
    const QSignalBlocker b(m_highlightAct); // 程序化同步不发 triggered
    m_highlightAct->setChecked(on);
  }
  updateHighlight();
  updateStatus();
}

void WellSectionPanel::setSeismicEnabled(bool on)
{
  if (m_seismicOn == on)
    return;
  m_seismicOn = on;
  m_st.seismicOn = on;
  if (m_seismicBtn)
  {
    const QSignalBlocker b(m_seismicBtn); // 程序化同步不发 clicked
    m_seismicBtn->setChecked(on);
  }
  m_st.gapPx = qBound(m_st.minGap(), m_st.gapPx, m_st.maxGap());
  syncGapToolTips();
  applyLayout();
  if (on && m_wells.size() >= 2)
    emit seismicRequested();
}

void WellSectionPanel::setFaultsEnabled(bool on)
{
  if (m_faultsOn == on)
    return;
  m_faultsOn = on;
  m_st.faultsOn = on;
  if (m_faultBtn)
  {
    const QSignalBlocker b(m_faultBtn); // 程序化同步不发 clicked
    m_faultBtn->setChecked(on);
  }
  syncToolbarState(); // 设置持久化只在按钮 clicked（用户路径）
  if (on && m_wells.size() >= 2)
    emit faultsRequested();
  else if (!on)
    setFaultTraces({}, QString());
}

void WellSectionPanel::setFaultTraces(
    const QVector<wellsection::FaultTrace> &traces, const QString &status)
{
  m_st.faultTraces = traces;
  m_faultStatus = status;
  if (m_faultItem)
    m_faultItem->update();
  syncToolbarState();
}

void WellSectionPanel::setFaultsAvailable(bool available, const QString &reason)
{
  m_faultsAvailable = available;
  m_faultsReason = reason;
  syncToolbarState();
}

void WellSectionPanel::setFlattenTop(const QString &top)
{
  setDatum(top.isEmpty()
               ? wellsection::Datum{wellsection::DatumMode::Depth, QString()}
               : wellsection::Datum{wellsection::DatumMode::Flatten, top});
}

void WellSectionPanel::setDatum(const wellsection::Datum &d)
{
  wellsection::Datum nd = d;
  if (nd.mode == wellsection::DatumMode::Flatten && nd.flattenTop.isEmpty())
    nd.mode = wellsection::DatumMode::Depth; // 空 flattenTop 视作井深
  if (nd == m_datum)
    return;
  m_datum = nd;
  rebuildFiltered();
  applyLayout();
  if (m_autofit)
    fitToView();
  syncToolbarState();
}

void WellSectionPanel::applyDatumFromMenu(const wellsection::Datum &d)
{
  setDatum(d);
  QSettings s = panelSettings();
  s.setValue(QStringLiteral("wellSection/datumMode"), int(m_datum.mode));
  s.setValue(QStringLiteral("wellSection/datumTop"), m_datum.flattenTop);
}

void WellSectionPanel::setSpacingMode(wellsection::SpacingMode mode)
{
  if (m_spacing == mode)
    return;
  m_spacing = mode;
  rebuildGapWidths();
  if (m_autofit)
    fitToView();
  else
    applyLayout();
  syncToolbarState();
}

void WellSectionPanel::setLinkOverrides(
    const QVector<wellsection::LinkOverride> &overrides)
{
  m_linkOverrides = overrides;
  m_st.linkOverrides = overrides;
  for (auto *g : m_gapItems)
    g->update();
}

// 井对 id 定位目标缝（菜单动作经 id 间接寻址——重建后仍指对缝）。
void WellSectionPanel::toggleLinkForPair(const QString &aId,
                                         const QString &bId,
                                         const QString &topName, bool connect)
{
  for (int i = 0; i + 1 < m_st.wells.size(); ++i)
    if ((m_st.wells[i].id == aId && m_st.wells[i + 1].id == bId) ||
        (m_st.wells[i].id == bId && m_st.wells[i + 1].id == aId))
    {
      toggleLink(i, topName, connect);
      return;
    }
}

void WellSectionPanel::toggleLink(int gap, const QString &topName,
                                  bool connect)
{
  if (gap < 0 || gap + 1 >= m_st.wells.size() || topName.isEmpty())
    return;
  const QString a = m_st.wells[gap].id;
  const QString b = m_st.wells[gap + 1].id;
  const wellsection::LinkOverride key =
      wellsection::makeLinkOverride(a, b, topName, connect);
  // upsert：同键替换（保留向量序稳定，便于 round-trip 对比）。
  bool replaced = false;
  for (wellsection::LinkOverride &o : m_linkOverrides)
    if (o.leftWellId == key.leftWellId && o.rightWellId == key.rightWellId &&
        o.topName == key.topName)
    {
      o.connected = key.connected;
      replaced = true;
      break;
    }
  if (!replaced)
    m_linkOverrides.push_back(key);
  m_st.linkOverrides = m_linkOverrides;
  for (auto *g : m_gapItems)
    g->update();
  emit linkOverridesChanged(m_linkOverrides);
}

void WellSectionPanel::rebuildGapWidths()
{
  m_st.gapWidths =
      m_spacing == wellsection::SpacingMode::Proportional
          ? wellsection::gapWidthsFor(m_st.wells, m_spacing,
                                      (m_st.wells.size() - 1) * m_st.gapPx,
                                      m_st.minGap(), m_st.maxGap())
          : QVector<double>();
}

QString WellSectionPanel::topsCsv() const
{
  return wellsection::topsTable(m_st.wells, m_datum).csv();
}

void WellSectionPanel::fitToView()
{
  m_autofit = true;
  const int vh = m_view->viewport()->height();
  const int vw = m_view->viewport()->width();
  const double span = m_st.window.base - m_st.window.top;
  const double ppm = (vh > 4 && span > 0) ? vh / span : m_st.pxPerMeter;
  if (ppm != m_st.pxPerMeter)
  {
    m_st.pxPerMeter = ppm;
    ++m_st.curveVersion; // 曲线几何变了才重建路径——只拉宽不动路径
  }
  const int n = m_st.wells.size();
  if (n >= 2 && vw > 0)
  {
    const double g =
        (vw - 2.0 * m_st.margin - n * m_st.columnWidth()) / (n - 1);
    m_st.gapPx = qBound(m_st.minGap(), g, m_st.maxGap());
  }
  applyLayout();
  m_view->verticalScrollBar()->setValue(0);
  m_view->horizontalScrollBar()->setValue(0);
}

QImage WellSectionPanel::renderImage(double scale) const
{
  const double w = m_st.sceneWidth();
  const double bodyH = m_st.sceneHeight();
  const double headerH = m_header ? m_header->headerHeight()
                                  : wellsectionui::HeaderWidget::kHeight;
  const double h = headerH + bodyH;
  QImage img(qMax(1, int(std::ceil(w * scale))),
             qMax(1, int(std::ceil(h * scale))),
             QImage::Format_ARGB32_Premultiplied);
  img.fill(m_st.theme.paper);
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing);
  p.scale(scale, scale);
  m_header->paintContents(&p, 0.0);
  m_scene->render(&p, QRectF(0, headerH, w, bodyH), QRectF(0, 0, w, bodyH));
  return img;
}

bool WellSectionPanel::exportTo(const QString &path) const
{
  if (path.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive))
  {
    // 层位井深表：MD 值不随基准面模式变（拉平不变量），模式只进表头标记。
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return false;
    const QByteArray bytes = topsCsv().toUtf8();
    return file.write(bytes) == bytes.size();
  }
  if (path.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive))
  {
    QSvgGenerator gen;
    gen.setFileName(path);
    const double w = m_st.sceneWidth();
    const double headerH = m_header ? m_header->headerHeight()
                                    : wellsectionui::HeaderWidget::kHeight;
    const double h = headerH + m_st.sceneHeight();
    gen.setSize(QSize(qMax(1, int(std::ceil(w))), qMax(1, int(std::ceil(h)))));
    gen.setViewBox(QRect(0, 0, int(w), int(h)));
    gen.setTitle(tr("连井剖面"));
    QPainter p(&gen);
    if (!p.isActive())
      return false;
    p.setRenderHint(QPainter::Antialiasing);
    m_header->paintContents(&p, 0.0);
    m_scene->render(&p, QRectF(0, headerH, w, m_st.sceneHeight()),
                    QRectF(0, 0, w, m_st.sceneHeight()));
    p.end();
    return true;
  }
  if (path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
  {
    const double w = m_st.sceneWidth();
    const double headerH = m_header ? m_header->headerHeight()
                                    : wellsectionui::HeaderWidget::kHeight;
    const double h = headerH + m_st.sceneHeight();
    QPdfWriter writer(path);
    writer.setResolution(96); // 1 图素 = 1/96 in → 页按图幅定制
    writer.setPageSize(QPageSize(QSizeF(w * 72.0 / 96.0, h * 72.0 / 96.0),
                                 QPageSize::Point));
    QPainter p(&writer);
    if (!p.isActive())
      return false;
    p.setRenderHint(QPainter::Antialiasing);
    m_header->paintContents(&p, 0.0);
    m_scene->render(&p, QRectF(0, headerH, w, m_st.sceneHeight()),
                    QRectF(0, 0, w, m_st.sceneHeight()));
    return true;
  }
  return renderImage(1.0).save(path);
}

// ---- 测试钩子 ----
int WellSectionPanel::linkCount() const
{
  int n = 0;
  for (int i = 0; i + 1 < m_st.wells.size(); ++i)
    for (const wellsection::Link &lk :
         wellsection::links(m_st.wells[i], m_st.wells[i + 1]))
      if (wellsection::linkConnected(m_linkOverrides, m_st.wells[i].id,
                                     m_st.wells[i + 1].id, lk.name))
        ++n;
  return n;
}

QString WellSectionPanel::highlightedFormation() const
{
  if (m_st.activeTop.isEmpty())
    return QString();
  for (const auto &w : m_st.wells)
    if (std::isfinite(w.topMd(m_st.activeTop)))
      return m_st.activeTop;
  return QString();
}

bool WellSectionPanel::isWellSelected(const QString &id) const
{
  return !id.isEmpty() && m_selectedId == id;
}

qreal WellSectionPanel::topLineY(const QString &wellId,
                                 const QString &top) const
{
  for (int i = 0; i < m_st.wells.size(); ++i)
    if (m_st.wells[i].id == wellId)
    {
      const double md = m_st.wells[i].topMd(top);
      return std::isfinite(md) ? m_st.yForMd(i, md) : qQNaN();
    }
  return qQNaN();
}

QString WellSectionPanel::gapReason(int gap) const
{
  return (gap >= 0 && gap < m_st.strip.gaps.size())
             ? m_st.strip.gaps[gap].reason
             : QString();
}

QString WellSectionPanel::statusText() const
{
  return m_status ? m_status->text() : QString();
}

// ---- 内部 ----
void WellSectionPanel::rebuildFiltered()
{
  m_st.wells = wellsection::filterTops(m_wells, m_tpl);
  m_st.offsets.clear();
  for (const auto &w : m_st.wells)
    m_st.offsets << wellsection::datumOffset(w, m_datum);
  m_st.window = wellsection::depthWindow(m_st.wells, m_datum);
  m_st.datum = m_datum;
  m_st.linkOverrides = m_linkOverrides;
  m_st.pathFractions = wellsection::wellPathFractions(m_st.wells);
  m_st.faultsOn = m_faultsOn;
  m_st.zoneOrder = wellsection::orderedTopNames(m_st.wells);
  m_st.tpl = m_tpl;
  m_st.seismicOn = m_seismicOn;
  m_st.selected = -1;
  for (int i = 0; i < m_st.wells.size(); ++i)
    if (m_st.wells[i].id == m_selectedId)
      m_st.selected = i;
  ++m_st.curveVersion; // 井集/偏移/窗口可能变 → 曲线路径重建
  ++m_st.imageVersion; // 图片道位图随井集/数据变更失效
  updateHighlight();
}

void WellSectionPanel::rebuildItems()
{
  // 井集变化才重建项；布局参数变化只走 relayout。
  for (auto *it : m_colItems)
    delete it;
  for (auto *it : m_gapItems)
    delete it;
  delete m_faultItem;
  m_faultItem = nullptr;
  m_colItems.clear();
  m_gapItems.clear();
  for (int i = 0; i < m_st.wells.size(); ++i)
  {
    auto *col = new wellsectionui::ColumnItem(&m_st, i);
    m_scene->addItem(col);
    m_colItems << col;
    if (i + 1 < m_st.wells.size())
    {
      auto *gap = new wellsectionui::GapItem(&m_st, i);
      gap->setZValue(-1); // 缝内容不压列框
      // 连线拾取回调（item 非 QObject）：hover 提示进状态行、右键菜单
      // 走面板的改接路径（信号 + 持久化钩子在面板侧）。
      gap->setLinkHoverCallback(
          [this](const QString &text) {
            m_hoverText = text;
            updateStatus();
          });
      gap->setLinkMenuCallback([this](int gapIndex, const QString &top,
                                      bool connected) {
        // 菜单动作捕获井对 id（非缝号）——非模态 popup 到触发之间若发生
        // 重建，按 id 重定位目标缝，不误改别缝。
        const QString aId = m_st.wells.value(gapIndex).id;
        const QString bId = m_st.wells.value(gapIndex + 1).id;
        auto *menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("wellSectionLinkMenu"));
        QAction *act = menu->addAction(
            connected ? tr("断开 %1 连线").arg(top)
                      : tr("重连 %1 连线").arg(top));
        connect(act, &QAction::triggered, this,
                [this, aId, bId, top, connected] {
                  toggleLinkForPair(aId, bId, top, !connected);
                });
        menu->setAttribute(Qt::WA_DeleteOnClose);
        // 非模态 popup（不嵌事件循环——exec 期间 rebuildItems 可能删除
        // 本 GapItem，回到已析构栈帧是 UB）。
        menu->popup(QCursor::pos());
      });
      m_scene->addItem(gap);
      m_gapItems << gap;
    }
  }
  if (!m_st.wells.isEmpty())
  {
    m_faultItem = new wellsectionui::FaultOverlayItem(&m_st);
    m_scene->addItem(m_faultItem);
  }
  applyLayout();
}

void WellSectionPanel::syncGapToolTips()
{
  // 缝 tooltip 随状态变（缝无效原因），paint 里不改 item 状态。
  for (int i = 0; i < m_gapItems.size(); ++i)
  {
    const wellsection::SeismicGap *g =
        i < m_st.strip.gaps.size() ? &m_st.strip.gaps[i] : nullptr;
    m_gapItems[i]->setToolTip(
        (m_st.seismicOn && g && !g->valid()) ? g->reason : QString());
  }
}

void WellSectionPanel::applyLayout()
{
  rebuildGapWidths(); // gapPx/井集/间距模式可能变 → 逐缝宽重算
  ++m_st.layoutVersion;
  for (auto *it : m_colItems)
    it->relayout();
  for (auto *it : m_gapItems)
    it->relayout();
  if (m_faultItem)
    m_faultItem->relayout();
  const QRectF r(0, 0, m_st.sceneWidth(), m_st.sceneHeight());
  if (m_scene->sceneRect() != r)
    m_scene->setSceneRect(r);
  syncGapToolTips();
  if (m_header)
    m_header->relayout(); // 道宽/模板可能改题注行数 → 动态版头高
}

void WellSectionPanel::updateStatus()
{
  if (!m_status)
    return;
  if (m_busy)
  {
    m_status->setText(tr("正在读取测井…"));
    m_status->setToolTip(QString());
  }
  else if (!m_hoverText.isEmpty())
    m_status->setText(m_hoverText);
  else if (!m_warnings.isEmpty())
  {
    m_status->setText(tr("%1 条提示").arg(m_warnings.size()));
    m_status->setToolTip(m_warnings.join(QLatin1Char('\n')));
  }
  else if (m_wells.size() == 1)
  {
    m_status->setText(tr("再选一口井即可连井"));
    m_status->setToolTip(QString());
  }
  else if (m_wells.size() >= 2)
  {
    QString s = tr("%1 口井").arg(m_wells.size());
    if (m_datum.mode == wellsection::DatumMode::Elevation)
      s += tr(" · 海拔基准");
    else if (m_datum.mode == wellsection::DatumMode::Flatten)
      s += tr(" · 拉平于 %1").arg(m_datum.flattenTop);
    if (!highlightedFormation().isEmpty())
    {
      s += tr(" · 高亮 %1").arg(m_st.activeTop);
      if (!m_st.baseTop.isEmpty())
        s += QStringLiteral("–%1").arg(m_st.baseTop);
    }
    m_status->setText(s);
    m_status->setToolTip(QString());
  }
  else
  {
    m_status->setText(QString());
    m_status->setToolTip(QString());
  }
}

void WellSectionPanel::updateSelection(const QStringList &ids,
                                       const QString &)
{
  QString pick;
  for (const QString &id : ids)
    if (m_ids.contains(id))
    {
      pick = id;
      break;
    }
  m_selectedId = pick;
  m_st.selected = -1;
  for (int i = 0; i < m_st.wells.size(); ++i)
    if (m_st.wells[i].id == pick)
      m_st.selected = i;
  m_header->update();
  m_view->viewport()->update();
}

void WellSectionPanel::updateHighlight()
{
  const QString active =
      (m_highlightOn && m_ctx) ? m_ctx->activeHorizon() : QString();
  m_st.activeTop = active;
  m_st.baseTop = active.isEmpty() ? QString() : baseHorizonFor(active);
  for (auto *it : m_colItems)
    it->update();
  for (auto *it : m_gapItems)
    it->update();
  m_view->viewport()->update();
  updateStatus(); // 状态行带「高亮 X–Y」
}

void WellSectionPanel::clearStrip()
{
  m_st.strip = wellsection::SeismicStrip();
  ++m_st.stripVersion;
}

void WellSectionPanel::moveWell(int from, int to)
{
  if (from < 0 || from >= m_wells.size() || to < 0 || to >= m_wells.size() ||
      from == to)
    return;
  m_wells.move(from, to);
  m_ids.move(from, to);
  clearStrip();
  rebuildFiltered();
  applyLayout(); // 井集数量未变：不重建项
  emit wellIdsChanged(m_ids);
  if (m_seismicOn && m_wells.size() >= 2)
    emit seismicRequested();
  // 井径变了：旧投绘的 along 映射已失真——先清后按需重求。
  if (m_faultsOn) {
    setFaultTraces({}, QString());
    if (m_wells.size() >= 2)
      emit faultsRequested();
  }
}

void WellSectionPanel::removeWellAt(int index)
{
  if (index < 0 || index >= m_wells.size())
    return;
  m_wells.removeAt(index);
  m_ids.removeAt(index);
  clearStrip();
  rebuildFiltered();
  rebuildItems();
  m_empty->setVisible(m_wells.isEmpty() && !m_busy);
  m_header->setVisible(!m_wells.isEmpty());
  updateStatus();
  emit wellIdsChanged(m_ids);
  if (m_seismicOn && m_wells.size() >= 2)
    emit seismicRequested();
  if (m_faultsOn) {
    setFaultTraces({}, QString());
    if (m_wells.size() >= 2)
      emit faultsRequested();
  }
}

void WellSectionPanel::openWellsDialog()
{
  WellSectionWellsDialog dlg(m_choices, m_ids,
                             m_ctx ? m_ctx->selectedIds() : QStringList(),
                             this);
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QStringList ids = dlg.selectedIds();
  if (ids == m_ids)
    return;
  m_ids = ids;
  m_wells.clear();
  clearStrip();
  rebuildFiltered();
  rebuildItems();
  emit wellIdsChanged(m_ids);
  emit dataRequested(m_ids, m_tpl.mnemonics());
}

QStringList WellSectionPanel::generateFromSelection()
{
  if (!m_ctx)
    return {};
  const QStringList selected = m_ctx->selectedIds();
  // 平面/树选中 ∩ 可选井（保选择序）。
  QStringList usable;
  for (const QString &id : selected) {
    for (const auto &c : m_choices)
      if (c.id == id) {
        usable << id;
        break;
      }
  }
  if (usable.size() < 2)
    return {}; // 一口井不成剖面（用户可手选）
  // 井位 PCA 序（缺坐标保选序排末）。
  QVector<wellsection::Well> positioned;
  for (const auto &c : m_choices)
    if (c.hasCoordinates) {
      wellsection::Well w;
      w.id = c.id;
      w.x = c.x;
      w.y = c.y;
      positioned << w;
    }
  const QStringList ordered =
      wellsection::orderWellsByPosition(usable, positioned);
  if (ordered == m_ids)
    return ordered;
  m_ids = ordered;
  m_wells.clear();
  clearStrip();
  rebuildFiltered();
  rebuildItems();
  emit wellIdsChanged(m_ids);
  emit dataRequested(m_ids, m_tpl.mnemonics());
  return ordered;
}

void WellSectionPanel::openTracksDialog()
{
  WellSectionTracksDialog dlg(m_tpl, m_mnemonicChoices, this);
  dlg.setTopNames(wellsection::orderedTopNames(m_wells));
  if (dlg.exec() != QDialog::Accepted)
    return;
  applyTemplateFromDialog(dlg.result());
}

void WellSectionPanel::applyThemeFromMenu(const QString &id)
{
  setThemeId(id);
  panelSettings().setValue(QStringLiteral("wellSection/theme"), m_themeId);
}

void WellSectionPanel::applyTemplateFromDialog(
    const wellsection::SectionTemplate &t)
{
  setSectionTemplate(t);
  panelSettings().setValue(
      QStringLiteral("wellSection/template"),
      QJsonDocument(m_tpl.toJson()).toJson(QJsonDocument::Compact));
}

void WellSectionPanel::syncToolbarState()
{
  if (m_seismicBtn)
  {
    m_seismicBtn->setEnabled(m_seismicAvailable);
    m_seismicBtn->setToolTip(
        m_seismicAvailable
            ? tr("井间叠加地震剖面（按时深关系自适应缩放）")
            : (m_seismicReason.isEmpty()
                   ? tr("先在「数据管理」导入带坐标的地震体")
                   : m_seismicReason));
    const QSignalBlocker b(m_seismicBtn); // 恢复态同步不触发用户路径
    m_seismicBtn->setChecked(m_seismicOn);
  }
  if (m_faultBtn)
  {
    m_faultBtn->setEnabled(m_faultsAvailable);
    m_faultBtn->setToolTip(
        !m_faultsAvailable
            ? (m_faultsReason.isEmpty() ? tr("工程内无断层解释") : m_faultsReason)
            : (m_faultStatus.isEmpty() || !m_faultsOn
                   ? tr("断层投绘（断面与剖面井径求交）")
                   : m_faultStatus));
    const QSignalBlocker b(m_faultBtn);
    m_faultBtn->setChecked(m_faultsOn);
  }
  if (m_flattenBtn)
    m_flattenBtn->setChecked(m_datum.mode != wellsection::DatumMode::Depth);
  if (m_spacingMenu)
    for (QAction *a : m_spacingMenu->actions())
      if (a->isCheckable())
        a->setChecked((a->text() == tr("等距")) ==
                      (m_spacing == wellsection::SpacingMode::Equal));
  if (m_spacingBtn)
    m_spacingBtn->setChecked(
        m_spacing == wellsection::SpacingMode::Proportional);
  if (m_themeMenu)
    for (QAction *a : m_themeMenu->actions())
      if (a->isCheckable() && !a->data().isNull())
        a->setChecked(a->data().toString() == m_themeId);
  if (m_highlightAct)
    m_highlightAct->setChecked(m_highlightOn);
}

void WellSectionPanel::ensureActiveIntervalVisible()
{
  if (m_st.activeTop.isEmpty())
    return;
  for (int i = 0; i < m_st.wells.size(); ++i)
  {
    const auto iv = wellsection::formationInterval(m_st.wells[i],
                                                   m_st.activeTop,
                                                   m_st.baseTop);
    if (!iv.valid())
      continue;
    const double y0 = m_st.yForMd(i, iv.topMd);
    const double y1 = iv.hasBase() ? m_st.yForMd(i, iv.baseMd) : y0;
    const QRectF vis =
        m_view->mapToScene(m_view->viewport()->rect()).boundingRect();
    if (y1 < vis.top() || y0 > vis.bottom())
    {
      const double yMid = (y0 + y1) * 0.5;
      m_view->verticalScrollBar()->setValue(
          int(yMid - m_view->viewport()->height() * 0.5));
    }
    return; // 只看第一口有该层的井
  }
}
