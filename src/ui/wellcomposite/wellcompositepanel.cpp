#include "wellcompositepanel.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

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
  topBar->setStyleSheet(QStringLiteral(
      "#wellCompositeTopBar { background: #FFFFFF; border-bottom: 1px solid #DFE5EC; }"));
  auto *topLay = new QHBoxLayout(topBar);
  topLay->setContentsMargins(8, 6, 8, 6);
  topLay->setSpacing(8);

  // 井名 Badge
  m_lblWellName = new QLabel(QStringLiteral("井号: —"), topBar);
  m_lblWellName->setObjectName(QStringLiteral("lblWellName"));
  m_lblWellName->setStyleSheet(QStringLiteral(
      "QLabel { background: #E8F0FE; color: #1B73D0; font-weight: bold; border-radius: 4px; padding: 2px 8px; font-size: 9pt; }"));
  topLay->addWidget(m_lblWellName);

  // 比例尺选择
  auto *lblScaleTitle = new QLabel(tr("比例尺:"), topBar);
  lblScaleTitle->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8pt;"));
  topLay->addWidget(lblScaleTitle);

  m_scaleCombo = new QComboBox(topBar);
  m_scaleCombo->setObjectName(QStringLiteral("scaleCombo"));
  m_scaleCombo->addItems({QStringLiteral("1:200"), QStringLiteral("1:500"),
                          QStringLiteral("1:1000"), QStringLiteral("1:2000"), tr("自适应")});
  m_scaleCombo->setCurrentText(QStringLiteral("1:500"));
  m_scaleCombo->setStyleSheet(QStringLiteral(
      "QComboBox { border: 1px solid #DFE5EC; border-radius: 4px; padding: 2px 6px; font-size: 8pt; background: #FFFFFF; }"
      "QComboBox:hover { border-color: #1B73D0; }"));
  topLay->addWidget(m_scaleCombo);

  // 缩放控制组
  auto *lblZoomTitle = new QLabel(tr("深度缩放:"), topBar);
  lblZoomTitle->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8pt; margin-left: 6px;"));
  topLay->addWidget(lblZoomTitle);

  const QString btnStyle = QStringLiteral(
      "QToolButton { background: #FFFFFF; border: 1px solid #DFE5EC; border-radius: 4px; "
      "padding: 2px 7px; font-size: 8pt; color: #24303E; }"
      "QToolButton:hover { background: #EDF1F5; border-color: #9AA7B4; }"
      "QToolButton:pressed { background: #DFE5EC; }");

  m_btnZoomOut = new QToolButton(topBar);
  m_btnZoomOut->setText(QStringLiteral("−"));
  m_btnZoomOut->setToolTip(tr("缩小深度 (Ctrl+滚轮下)"));
  m_btnZoomOut->setStyleSheet(btnStyle);
  topLay->addWidget(m_btnZoomOut);

  m_lblZoom = new QLabel(QStringLiteral("100%"), topBar);
  m_lblZoom->setObjectName(QStringLiteral("lblZoomFactor"));
  m_lblZoom->setStyleSheet(QStringLiteral(
      "QLabel { color: #24303E; font-size: 8pt; min-width: 40px; }"));
  m_lblZoom->setAlignment(Qt::AlignCenter);
  topLay->addWidget(m_lblZoom);

  m_btnZoomIn = new QToolButton(topBar);
  m_btnZoomIn->setText(QStringLiteral("+"));
  m_btnZoomIn->setToolTip(tr("放大深度 (Ctrl+滚轮上)"));
  m_btnZoomIn->setStyleSheet(btnStyle);
  topLay->addWidget(m_btnZoomIn);

  m_btnResetZoom = new QToolButton(topBar);
  m_btnResetZoom->setText(tr("1:1 适应"));
  m_btnResetZoom->setToolTip(tr("双击道内任意位置或点击此键恢复全井段"));
  m_btnResetZoom->setStyleSheet(btnStyle);
  topLay->addWidget(m_btnResetZoom);

  topLay->addStretch(1);

  // 状态信息显示（悬停深度等）
  m_lblStatus = new QLabel(tr("就绪 | 支持按住拖拽漫游与滚轮缩放"), topBar);
  m_lblStatus->setObjectName(QStringLiteral("lblStatus"));
  m_lblStatus->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8pt;"));
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

  connect(m_canvas, &WellCompositeCanvas::zoomChanged, this, [this](double z) {
    m_lblZoom->setText(QStringLiteral("%1%").arg(qRound(z * 100)));
  });

  connect(m_scaleCombo, &QComboBox::currentTextChanged, this, [this](const QString &scaleText) {
    m_canvas->setScaleRatio(scaleText);
  });

  connect(m_canvas, &WellCompositeCanvas::scaleRatioChanged, this, [this](const QString &ratio) {
    if (m_scaleCombo->currentText() != ratio)
    {
      m_scaleCombo->blockSignals(true);
      m_scaleCombo->setCurrentText(ratio);
      m_scaleCombo->blockSignals(false);
    }
  });

  connect(m_canvas, &WellCompositeCanvas::depthHovered, this, [this](double d) {
    if (d > 0.0)
    {
      m_lblStatus->setText(QStringLiteral("当前测深: %1 m | 井深跨度: %2 - %3 m")
                               .arg(QString::number(d, 'f', 1))
                               .arg(QString::number(m_canvas->minDepth(), 'f', 1))
                               .arg(QString::number(m_canvas->maxDepth(), 'f', 1)));
    }
    else
    {
      m_lblStatus->setText(QStringLiteral("井深跨度: %1 - %2 m | 滚轮缩放 / 拖拽漫游")
                               .arg(QString::number(m_canvas->minDepth(), 'f', 1))
                               .arg(QString::number(m_canvas->maxDepth(), 'f', 1)));
    }
  });
}

void WellCompositePanel::setWellName(const QString &name)
{
  m_wellName = name;
  m_lblWellName->setText(name.isEmpty() ? QStringLiteral("井号: —") : QStringLiteral("井号: %1").arg(name));
}

bool WellCompositePanel::loadComprehensiveXml(const QString &xmlPath)
{
  ComprehensiveWellData data;
  QString err;
  if (!parseComprehensiveWellXml(xmlPath, data, &err))
    return false;

  m_data = data;
  setWellName(data.wellName);
  setupTracksFromData(data);
  emit wellLoaded(data.wellName);
  return true;
}

bool WellCompositePanel::loadLasCurves(const QString &wellName, const QVector<CurveData> &curves)
{
  m_canvas->clearTracks();
  setWellName(wellName);

  if (curves.isEmpty())
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

  m_canvas->setDepthRange(minD, maxD);

  // 1. 标尺道
  auto scaleTrack = std::make_shared<DepthScaleTrack>(64.0);
  scaleTrack->setScaleRatio(m_scaleCombo->currentText());
  m_canvas->addTrack(scaleTrack);

  // 2. 曲线道（严格按最多4根曲线合并显示）
  for (int i = 0; i < curves.size(); i += 4)
  {
    auto curveTrack = std::make_shared<CurveTrack>(
        i == 0 ? QStringLiteral("常规测井") : QStringLiteral("辅助曲线"), 180.0);

    for (int j = 0; j < 4 && (i + j) < curves.size(); ++j)
    {
      curveTrack->addCurve(curves.at(i + j));
    }
    m_canvas->addTrack(curveTrack);
  }

  m_canvas->setScaleRatio(m_scaleCombo->currentText());
  return true;
}

void WellCompositePanel::setupTracksFromData(const ComprehensiveWellData &data)
{
  m_canvas->clearTracks();
  m_canvas->setDepthRange(data.minDepth, data.maxDepth);

  // 1. 地层单位道
  if (!data.formationIntervals.isEmpty())
  {
    auto formTrack = std::make_shared<FormationTrack>(QStringLiteral("地层单位"), 75.0);
    formTrack->setIntervals(data.formationIntervals);
    m_canvas->addTrack(formTrack);
  }

  // 2. 砂层组道（细分层道）
  if (!data.sandIntervals.isEmpty())
  {
    auto sandTrack = std::make_shared<FormationTrack>(QStringLiteral("砂层组"), 60.0);
    sandTrack->setIntervals(data.sandIntervals);
    m_canvas->addTrack(sandTrack);
  }

  // 3. 沉积旋回与符号道
  if (!data.symbolItems.isEmpty())
  {
    auto symTrack = std::make_shared<SymbolTrack>(QStringLiteral("沉积旋回"), 50.0);
    symTrack->setItems(data.symbolItems);
    m_canvas->addTrack(symTrack);
  }

  // 4. 岩性道（标准地质岩性花纹填充）
  if (!data.lithologyIntervals.isEmpty())
  {
    auto lithoTrack = std::make_shared<LithologyTrack>(QStringLiteral("岩性分析"), 80.0);
    lithoTrack->setIntervals(data.lithologyIntervals);
    m_canvas->addTrack(lithoTrack);
  }

  // 5. 深度标尺道（居中基准）
  auto scaleTrack = std::make_shared<DepthScaleTrack>(64.0);
  scaleTrack->setScaleRatio(m_scaleCombo->currentText());
  m_canvas->addTrack(scaleTrack);

  // 6. 取芯道（筒号与收获率对比）
  if (!data.coreBarrels.isEmpty())
  {
    auto coreTrack = std::make_shared<CoreTrack>(QStringLiteral("取心数据"), 65.0);
    coreTrack->setBarrels(data.coreBarrels);
    m_canvas->addTrack(coreTrack);
  }

  // 7. 曲线道（连续物理曲线：支持 1-4 根曲线合并显示）
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

  // 8. 离散曲线道（实测散点/化验分析：支持 1-4 根曲线合并展示）
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

  // 9. 文本道（取样与试油结论）
  if (!data.textIntervals.isEmpty())
  {
    auto textTrack = std::make_shared<TextTrack>(QStringLiteral("解释结论/取样"), 120.0);
    textTrack->setIntervals(data.textIntervals);
    m_canvas->addTrack(textTrack);
  }

  m_canvas->setScaleRatio(m_scaleCombo->currentText());
}

} // namespace WellComposite
