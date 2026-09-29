// 层：视图
#include "wellcompositepanel.h"
#include "curveconfigdialog.h"
#include "wellpositionlegendwidget.h"
#include "../paleotheme.h"
#include "../../services/previewdoc.h" // 数据门面（W1：XML 解析入口不直触）
#include <QHBoxLayout>
#include <QLineEdit>
#include <QVBoxLayout>
#include <algorithm>

namespace WellComposite
{

WellCompositePanel::WellCompositePanel(QWidget *parent)
  : QWidget(parent)
{
  setupUi();
}

void WellCompositePanel::setupUi()
{
  auto *rootLay = new QVBoxLayout(this);
  rootLay->setContentsMargins(0, 0, 0, 0);
  rootLay->setSpacing(4);

  // 置顶工具栏
  auto *topBar = new QWidget(this);
  topBar->setObjectName(QStringLiteral("wellCompositeTopBar"));
  PaleoTheme::applyThemedStyleSheet(topBar, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
               "#wellCompositeTopBar { background: %1; border-bottom: 1px solid %2; }")
        .arg(t.surface.name(), t.border.name());
  });
  auto *topLay = new QHBoxLayout(topBar);
  topLay->setContentsMargins(8, 6, 8, 6);
  topLay->setSpacing(8);

  // 井名 Badge（样式由 setWellName 统一收口，此处仅占位）
  m_lblWellName = new QLabel(topBar);
  m_lblWellName->setObjectName(QStringLiteral("lblWellName"));
  topLay->addWidget(m_lblWellName);

  // 比例尺选择（可编辑，且随着放大/缩小联动动态更新）
  auto *lblScaleTitle = new QLabel(tr("比例尺:"), topBar);
  PaleoTheme::applyThemedStyleSheet(lblScaleTitle, [] {
    return PaleoTheme::mutedCaptionStyleSheet() + QStringLiteral(" font-size: 8pt;");
  });
  topLay->addWidget(lblScaleTitle);

  m_scaleCombo = new QComboBox(topBar);
  m_scaleCombo->setObjectName(QStringLiteral("scaleCombo"));
  m_scaleCombo->setAccessibleName(tr("比例尺"));
  m_scaleCombo->setEditable(true);
  m_scaleCombo->addItems({QStringLiteral("1:200"), QStringLiteral("1:500"),
                          QStringLiteral("1:1000"), QStringLiteral("1:2000"), tr("自适应")});
  m_scaleCombo->setCurrentText(QStringLiteral("1:500"));
  PaleoTheme::applyThemedStyleSheet(m_scaleCombo, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
               "QComboBox { border: 1px solid %1; border-radius: 4px; padding: 2px 6px; font-size: 8pt; background: %2; }"
               "QComboBox:hover { border-color: %3; }")
        .arg(t.border.name(), t.surface.name(), t.primary.name());
  });
  topLay->addWidget(m_scaleCombo);

  // 缩放控制组
  auto *lblZoomTitle = new QLabel(tr("深度缩放:"), topBar);
  PaleoTheme::applyThemedStyleSheet(lblZoomTitle, [] {
    return PaleoTheme::mutedCaptionStyleSheet() + QStringLiteral(" font-size: 8pt; margin-left: 6px;");
  });
  topLay->addWidget(lblZoomTitle);

  const auto themedBtnStyle = [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
               "QToolButton { background: %1; border: 1px solid %2; border-radius: 4px; "
               "padding: 2px 7px; font-size: 8pt; color: %3; }"
               "QToolButton:hover { background: %4; border-color: %5; }"
               "QToolButton:pressed { background: %2; }")
        .arg(t.surface.name(), t.border.name(), t.text.name(),
             t.surfaceAlt.name(), t.textDisabled.name());
  };

  m_btnZoomOut = new QToolButton(topBar);
  m_btnZoomOut->setObjectName(QStringLiteral("btnCompZoomOut"));
  m_btnZoomOut->setText(tr("缩小"));
  m_btnZoomOut->setToolTip(tr("缩小深度 (Ctrl+滚轮下)"));
  PaleoTheme::applyThemedStyleSheet(m_btnZoomOut, themedBtnStyle);
  topLay->addWidget(m_btnZoomOut);

  m_lblZoom = new QLabel(QStringLiteral("100% (1:500)"), topBar);
  m_lblZoom->setObjectName(QStringLiteral("lblCompZoomFactor"));
  PaleoTheme::applyThemedStyleSheet(m_lblZoom, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral("QLabel { color: %1; font-size: 8pt; min-width: 65px; }")
        .arg(t.text.name());
  });
  m_lblZoom->setAlignment(Qt::AlignCenter);
  topLay->addWidget(m_lblZoom);

  m_btnZoomIn = new QToolButton(topBar);
  m_btnZoomIn->setObjectName(QStringLiteral("btnCompZoomIn"));
  m_btnZoomIn->setText(tr("放大"));
  m_btnZoomIn->setToolTip(tr("放大深度 (Ctrl+滚轮上)"));
  PaleoTheme::applyThemedStyleSheet(m_btnZoomIn, themedBtnStyle);
  topLay->addWidget(m_btnZoomIn);

  m_btnResetZoom = new QToolButton(topBar);
  m_btnResetZoom->setObjectName(QStringLiteral("btnCompResetZoom"));
  m_btnResetZoom->setText(tr("全井适应"));
  m_btnResetZoom->setToolTip(tr("双击道内任意位置或点击此键恢复全井段"));
  PaleoTheme::applyThemedStyleSheet(m_btnResetZoom, themedBtnStyle);
  topLay->addWidget(m_btnResetZoom);

  // 测井道配置与排列管理按钮
  m_btnConfigCurves = new QToolButton(topBar);
  m_btnConfigCurves->setObjectName(QStringLiteral("btnConfigCurves"));
  m_btnConfigCurves->setText(tr("测井道配置"));
  m_btnConfigCurves->setToolTip(tr("打开测井道配置与排列管理：支持调整所有井道顺序、合并与解散测井曲线道"));
  PaleoTheme::applyThemedStyleSheet(m_btnConfigCurves, themedBtnStyle);
  topLay->addWidget(m_btnConfigCurves);

  topLay->addStretch(1);

  // 状态信息显示（悬停深度等）
  m_lblStatus = new QLabel(tr("就绪 | 支持按住拖拽漫游，Ctrl+滚轮缩放"), topBar);
  m_lblStatus->setObjectName(QStringLiteral("lblStatus"));
  PaleoTheme::applyThemedStyleSheet(m_lblStatus, [] {
    return PaleoTheme::mutedCaptionStyleSheet() + QStringLiteral(" font-size: 8pt;");
  });
  topLay->addWidget(m_lblStatus);

  rootLay->addWidget(topBar);

  // 中央综合柱状图画布
  m_canvas = new WellCompositeCanvas(this);
  m_canvas->setObjectName(QStringLiteral("wellCompositeCanvas"));
  rootLay->addWidget(m_canvas, 1);

  // 事件与信号绑定
  connect(m_btnZoomIn, &QToolButton::clicked, m_canvas, &WellCompositeCanvas::zoomIn);
  connect(m_btnZoomOut, &QToolButton::clicked, m_canvas, &WellCompositeCanvas::zoomOut);
  connect(m_btnResetZoom, &QToolButton::clicked, m_canvas, &WellCompositeCanvas::resetZoom);
  connect(m_btnConfigCurves, &QToolButton::clicked, this, &WellCompositePanel::openCurveConfigDialog);

  connect(m_canvas, &WellCompositeCanvas::zoomChanged, this, [this](double z) {
    m_lblZoom->setText(QStringLiteral("%1% (%2)").arg(qRound(z * 100)).arg(m_canvas->scaleRatio()));
  });

  connect(m_scaleCombo, &QComboBox::currentTextChanged, this, [this](const QString &scaleText) {
    m_canvas->setScaleRatio(scaleText);
  });

  if (m_scaleCombo->lineEdit())
  {
    connect(m_scaleCombo->lineEdit(), &QLineEdit::editingFinished, this, [this]() {
      m_canvas->setScaleRatio(m_scaleCombo->currentText());
    });
  }

  connect(m_canvas, &WellCompositeCanvas::scaleRatioChanged, this, [this](const QString &ratio) {
    if (m_scaleCombo->currentText() != ratio)
    {
      m_scaleCombo->blockSignals(true);
      m_scaleCombo->setEditText(ratio);
      m_scaleCombo->blockSignals(false);
    }
    m_lblZoom->setText(QStringLiteral("%1% (%2)")
                           .arg(qRound(m_canvas->zoomFactor() * 100))
                           .arg(ratio));
  });

  connect(m_canvas, &WellCompositeCanvas::depthHovered, this, [this](double d) {
    if (d > 0.0)
    {
      m_lblStatus->setText(tr("当前测深: %1 m | 井深跨度: %2 - %3 m")
                               .arg(QString::number(d, 'f', 1))
                               .arg(QString::number(m_canvas->minDepth(), 'f', 1))
                               .arg(QString::number(m_canvas->maxDepth(), 'f', 1)));
    }
    else
    {
      m_lblStatus->setText(tr("井深跨度: %1 - %2 m | Ctrl+滚轮缩放 / 拖拽漫游")
                               .arg(QString::number(m_canvas->minDepth(), 'f', 1))
                               .arg(QString::number(m_canvas->maxDepth(), 'f', 1)));
    }
  });

  // 底部位置显示与比例尺图例综合控制栏
  m_legendWidget = new WellPositionLegendWidget(this);
  m_legendWidget->setObjectName(QStringLiteral("wellPositionLegendWidget"));
  rootLay->addWidget(m_legendWidget);

  connect(m_canvas, &WellCompositeCanvas::viewportChanged,
          m_legendWidget, &WellPositionLegendWidget::updateViewport);
  connect(m_canvas, &WellCompositeCanvas::depthHovered,
          m_legendWidget, &WellPositionLegendWidget::updateHoverDepth);
  connect(m_legendWidget, &WellPositionLegendWidget::requestScrollDepth,
          m_canvas, &WellCompositeCanvas::setScrollDepth);

  auto syncScaleToLegend = [this]() {
    if (m_legendWidget && m_canvas)
      m_legendWidget->updateScale(m_canvas->pxPerMeter(), m_canvas->scaleRatio());
  };
  connect(m_canvas, &WellCompositeCanvas::scaleRatioChanged, this, syncScaleToLegend);
  connect(m_canvas, &WellCompositeCanvas::zoomChanged, this, [syncScaleToLegend](double) {
    syncScaleToLegend();
  });

  syncScaleToLegend();
  m_legendWidget->updateViewport(m_canvas->visibleTopDepth(),
                                 m_canvas->visibleBottomDepth(),
                                 m_canvas->visibleDepthSpan());

  // 井名徽章初始态（样式与文字收口在 setWellName）
  setWellName(QString(), false);
}

void WellCompositePanel::openCurveConfigDialog()
{
  CurveConfigDialog dlg(m_canvas, this);
  dlg.exec();
}

void WellCompositePanel::setWellName(const QString &name, bool reference)
{
  m_wellName = name;
  m_referenceWell = reference;

  // 测区井徽章 = 中性徽章（surface-alt-raised 底 + text 字）；
  // 参考井徽章 = warning 胶囊语义（辅助资料、待区分）。
  const auto neutralBadgeStyle = [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
               "background: %1; color: %2; font-weight: bold; border-radius: 4px; padding: 2px 8px; font-size: 9pt;")
        .arg(t.surfaceAltRaised.name(), t.text.name());
  };
  const auto referenceBadgeStyle = [] {
    return PaleoTheme::capsuleStyleSheet(PaleoTheme::CapsuleKind::Warning) +
           QStringLiteral(" font-weight: bold; padding: 2px 8px; font-size: 9pt;");
  };

  if (name.isEmpty())
  {
    m_lblWellName->setText(tr("井号: —"));
    PaleoTheme::applyThemedStyleSheet(m_lblWellName, neutralBadgeStyle);
    m_lblWellName->setToolTip(QString());
  }
  else if (reference)
  {
    m_lblWellName->setText(tr("参考井: %1").arg(name));
    PaleoTheme::applyThemedStyleSheet(m_lblWellName, referenceBadgeStyle);
    m_lblWellName->setToolTip(tr("辅助资料中的参考井，不属于本测区井序列"));
  }
  else
  {
    m_lblWellName->setText(tr("井号: %1").arg(name));
    PaleoTheme::applyThemedStyleSheet(m_lblWellName, neutralBadgeStyle);
    m_lblWellName->setToolTip(QString());
  }
}

bool WellCompositePanel::loadComprehensiveXml(const QString &xmlPath)
{
  ComprehensiveWellData data;
  QString err;
  if (!PreviewDocService::wellCompositeAt(xmlPath, &data, &err))
    return false;

  m_data = data;
  setWellName(data.wellName, true); // 综合柱状图 XML 只出现在辅助资料里，井名是参考井
  setupTracksFromData(data);
  if (m_legendWidget)
    m_legendWidget->setWellData(data);
  emit wellLoaded(data.wellName);
  return true;
}

bool WellCompositePanel::loadLasCurves(const QString &wellName, const QVector<CurveData> &curves,
                                       const QVector<FormationInterval> &formations)
{
  m_canvas->clearTracks();
  setWellName(wellName);

  if (curves.isEmpty() && formations.isEmpty())
    return false;

  // 计算深度跨度
  double minD = 1e9, maxD = -1e9;
  for (const auto &c : curves)
  {
    if (!c.depths.isEmpty())
    {
      if (c.depths.first() < minD) minD = c.depths.first();
      if (c.depths.last() > maxD) maxD = c.depths.last();
    }
  }
  for (const auto &f : formations)
  {
    if (f.topDepth < minD) minD = f.topDepth;
    if (f.bottomDepth > maxD) maxD = f.bottomDepth;
  }
  if (minD >= maxD)
  {
    minD = 0.0;
    maxD = 1000.0;
  }

  m_canvas->setDepthRange(minD, maxD);

  // 1. 地层系统组组合道 (系 | 统 | 组) —— 仅当分层名能映射出系/统时才展示，
  //    否则不摆一个大量留空的组合道（地层单位道已覆盖真实分层）。
  if (!formations.isEmpty())
  {
    auto stratTrack = std::make_shared<StratigraphyCompoundTrack>(QStringLiteral("地层"), 145.0);
    stratTrack->autoDeriveStratigraphy(formations, minD, maxD);
    const bool anySystem = std::any_of(stratTrack->intervals().begin(), stratTrack->intervals().end(),
                                       [](const StratigraphyInterval &si) { return !si.system.isEmpty(); });
    if (anySystem)
      m_canvas->addTrack(stratTrack);
  }

  // 2. 深度标尺道 (DepthScaleTrack)
  auto scaleTrack = std::make_shared<DepthScaleTrack>(68.0);
  scaleTrack->setScaleRatio(m_scaleCombo->currentText());
  m_canvas->addTrack(scaleTrack);

  // 3. 地层道 (FormationTrack) —— 若有分层数据
  if (!formations.isEmpty())
  {
    auto formTrack = std::make_shared<FormationTrack>(QStringLiteral("地层"), 80.0);
    formTrack->setIntervals(formations);
    m_canvas->addTrack(formTrack);
  }

  // 3. 曲线道 (CurveTrack) —— 严格按照 1-4 根曲线分道合并显示
  const auto isLitho = [](const QString &name) {
    const QString n = name.toUpper();
    return n.startsWith(QStringLiteral("GR")) || n.startsWith(QStringLiteral("CAL")) ||
           n.startsWith(QStringLiteral("SP")) || n.startsWith(QStringLiteral("BS")) ||
           n.startsWith(QStringLiteral("AZIM"));
  };
  const auto isPorosity = [](const QString &name) {
    const QString n = name.toUpper();
    return n.startsWith(QStringLiteral("AC")) || n.startsWith(QStringLiteral("DEN")) ||
           n.startsWith(QStringLiteral("CNL")) || n.startsWith(QStringLiteral("POR")) ||
           n.startsWith(QStringLiteral("CPOR")) || n.startsWith(QStringLiteral("PHIF"));
  };
  const auto isResistivity = [](const QString &name) {
    const QString n = name.toUpper();
    return n.startsWith(QStringLiteral("RT")) || n.startsWith(QStringLiteral("RXO")) ||
           n.startsWith(QStringLiteral("RD")) || n.startsWith(QStringLiteral("RS")) ||
           n.startsWith(QStringLiteral("ILD")) || n.startsWith(QStringLiteral("ILM")) ||
           n.startsWith(QStringLiteral("AT"));
  };

  QVector<CurveData> lithoCurves;
  QVector<CurveData> poroCurves;
  QVector<CurveData> resCurves;
  QVector<CurveData> otherCurves;

  for (const auto &c : curves)
  {
    if (isLitho(c.name)) lithoCurves.append(c);
    else if (isPorosity(c.name)) poroCurves.append(c);
    else if (isResistivity(c.name)) resCurves.append(c);
    else otherCurves.append(c);
  }

  const auto addTrackGroup = [this](const QString &baseTitle, const QVector<CurveData> &group) {
    for (int i = 0; i < group.size(); i += 4)
    {
      QString title = baseTitle;
      if (group.size() > 4)
        title += QStringLiteral(" (%1)").arg(i / 4 + 1);
      auto track = std::make_shared<CurveTrack>(title, 180.0);
      for (int j = 0; j < 4 && (i + j) < group.size(); ++j)
        track->addCurve(group.at(i + j));
      m_canvas->addTrack(track);
    }
  };

  if (!lithoCurves.isEmpty())
    addTrackGroup(QStringLiteral("岩性测井"), lithoCurves);
  if (!poroCurves.isEmpty())
    addTrackGroup(QStringLiteral("三孔隙测井"), poroCurves);
  if (!resCurves.isEmpty())
    addTrackGroup(QStringLiteral("电阻率测井"), resCurves);
  if (!otherCurves.isEmpty())
    addTrackGroup(QStringLiteral("辅助曲线"), otherCurves);

  if (lithoCurves.isEmpty() && poroCurves.isEmpty() && resCurves.isEmpty() && otherCurves.isEmpty())
  {
    for (int i = 0; i < curves.size(); i += 4)
    {
      auto track = std::make_shared<CurveTrack>(
          i == 0 ? QStringLiteral("常规测井") : QStringLiteral("辅助曲线"), 180.0);
      for (int j = 0; j < 4 && (i + j) < curves.size(); ++j)
        track->addCurve(curves.at(i + j));
      m_canvas->addTrack(track);
    }
  }

  // 沉积相道：无真实相数据时不展示（不臆造相序）。
  m_data.wellName = wellName;
  m_data.minDepth = minD;
  m_data.maxDepth = maxD;
  m_data.continuousCurves = curves;
  m_data.formationIntervals = formations;
  if (m_legendWidget)
    m_legendWidget->setWellData(m_data);

  m_canvas->setScaleRatio(m_scaleCombo->currentText());
  return true;
}

void WellCompositePanel::setupTracksFromData(const ComprehensiveWellData &data)
{
  m_canvas->clearTracks();
  m_canvas->setDepthRange(data.minDepth, data.maxDepth);

  // 1. 地层系统组组合道 (系 | 统 | 组) —— 有真实地层系统数据，或分层名能映射出
  //    系/统时才展示。
  if (!data.stratigraphyIntervals.isEmpty() || !data.formationIntervals.isEmpty())
  {
    auto stratTrack = std::make_shared<StratigraphyCompoundTrack>(QStringLiteral("地层"), 145.0);
    if (!data.stratigraphyIntervals.isEmpty())
    {
      stratTrack->setIntervals(data.stratigraphyIntervals);
      m_canvas->addTrack(stratTrack);
    }
    else
    {
      stratTrack->autoDeriveStratigraphy(data.formationIntervals, data.minDepth, data.maxDepth);
      const bool anySystem = std::any_of(stratTrack->intervals().begin(), stratTrack->intervals().end(),
                                         [](const StratigraphyInterval &si) { return !si.system.isEmpty(); });
      if (anySystem)
        m_canvas->addTrack(stratTrack);
    }
  }

  // 2. 地层单位道
  if (!data.formationIntervals.isEmpty())
  {
    auto formTrack = std::make_shared<FormationTrack>(QStringLiteral("地层单位"), 75.0);
    formTrack->setIntervals(data.formationIntervals);
    m_canvas->addTrack(formTrack);
  }

  // 3. 砂层组道（细分层道）
  if (!data.sandIntervals.isEmpty())
  {
    auto sandTrack = std::make_shared<FormationTrack>(QStringLiteral("砂层组"), 60.0);
    sandTrack->setIntervals(data.sandIntervals);
    m_canvas->addTrack(sandTrack);
  }

  // 4. 沉积旋回与符号道
  if (!data.symbolItems.isEmpty())
  {
    auto symTrack = std::make_shared<SymbolTrack>(QStringLiteral("沉积旋回"), 50.0);
    symTrack->setItems(data.symbolItems);
    m_canvas->addTrack(symTrack);
  }

  // 5. 岩性道（标准地质岩性花纹填充）
  if (!data.lithologyIntervals.isEmpty())
  {
    auto lithoTrack = std::make_shared<LithologyTrack>(QStringLiteral("岩性分析"), 80.0);
    lithoTrack->setIntervals(data.lithologyIntervals);
    m_canvas->addTrack(lithoTrack);
  }

  // 6. 深度标尺道（居中基准）
  auto scaleTrack = std::make_shared<DepthScaleTrack>(64.0);
  scaleTrack->setScaleRatio(m_scaleCombo->currentText());
  m_canvas->addTrack(scaleTrack);

  // 7. 取芯道（筒号与收获率对比）
  if (!data.coreBarrels.isEmpty())
  {
    auto coreTrack = std::make_shared<CoreTrack>(QStringLiteral("取心数据"), 65.0);
    coreTrack->setBarrels(data.coreBarrels);
    m_canvas->addTrack(coreTrack);
  }

  // 8. 曲线道（连续物理曲线：支持 1-4 根曲线合并显示）
  if (!data.continuousCurves.isEmpty())
  {
    for (int i = 0; i < data.continuousCurves.size(); i += 4)
    {
      // 根据道内曲线的测井物理属性智能化命名
      bool hasGR = false, hasNeutronDensity = false, hasGas = false, hasRes = false, hasInterp = false;
      for (int j = 0; j < 4 && (i + j) < data.continuousCurves.size(); ++j)
      {
        const QString &cn = data.continuousCurves.at(i + j).name.toUpper();
        if (cn.contains(QStringLiteral("GR")) || cn.contains(QStringLiteral("CALI")) || cn.contains(QStringLiteral("SP"))) hasGR = true;
        if (cn.contains(QStringLiteral("CNCF")) || cn.contains(QStringLiteral("ZDEN")) || cn.contains(QStringLiteral("AC")) || cn.contains(QStringLiteral("PE"))) hasNeutronDensity = true;
        if (cn.contains(QStringLiteral("RPC")) || cn.contains(QStringLiteral("RAC")) || cn.contains(QStringLiteral("RT")) || cn.contains(QStringLiteral("RXO"))) hasRes = true;
        if ((cn.startsWith(QLatin1Char('C')) && cn.length() <= 3) || cn.contains(QStringLiteral("TG")) || cn.contains(QStringLiteral("CO2"))) hasGas = true;
        if (cn.contains(QStringLiteral("PIGN")) || cn.contains(QStringLiteral("KINT")) || cn.contains(QStringLiteral("SUWI"))) hasInterp = true;
      }

      QString title;
      if (hasInterp)
        title = QStringLiteral("储层物性解释");
      else if (hasGas)
        title = QStringLiteral("气测录井烃类");
      else if (hasGR && hasRes)
        title = QStringLiteral("常规/电阻率");
      else if (hasNeutronDensity)
        title = QStringLiteral("三孔隙度/密度");
      else if (hasRes)
        title = QStringLiteral("电阻率测井");
      else
        title = QStringLiteral("测井道 %1").arg(i / 4 + 1);

      auto curveTrack = std::make_shared<CurveTrack>(title, 180.0);
      for (int j = 0; j < 4 && (i + j) < data.continuousCurves.size(); ++j)
      {
        curveTrack->addCurve(data.continuousCurves.at(i + j));
      }
      m_canvas->addTrack(curveTrack);
    }
  }

  // 9. 离散曲线道（实测散点/化验分析：支持 1-4 根曲线合并展示）
  if (!data.discreteCurves.isEmpty())
  {
    for (int i = 0; i < data.discreteCurves.size(); i += 4)
    {
      const QString title = (i == 0) ? QStringLiteral("实测物性分析") : QStringLiteral("地化/生烃潜量");
      auto discTrack = std::make_shared<CurveTrack>(title, 160.0);
      for (int j = 0; j < 4 && (i + j) < data.discreteCurves.size(); ++j)
      {
        discTrack->addCurve(data.discreteCurves.at(i + j));
      }
      m_canvas->addTrack(discTrack);
    }
  }

  // 10. 文本道（取样与试油结论）
  if (!data.textIntervals.isEmpty())
  {
    auto textTrack = std::make_shared<TextTrack>(QStringLiteral("解释结论/取样"), 120.0);
    textTrack->setIntervals(data.textIntervals);
    m_canvas->addTrack(textTrack);
  }

  // 11. 沉积相组合道 (相 | 亚 | 微，带地质纹理填充，规范放置在最右侧/最后)
  //     仅当文档真实提供了相区间时展示，不自动臆造。
  if (!data.faciesIntervals.isEmpty())
  {
    auto faciesTrack = std::make_shared<FaciesCompoundTrack>(QStringLiteral("沉积相"), 180.0);
    faciesTrack->setIntervals(data.faciesIntervals);
    m_canvas->addTrack(faciesTrack);
  }

  m_canvas->setScaleRatio(m_scaleCombo->currentText());
}

} // namespace WellComposite
