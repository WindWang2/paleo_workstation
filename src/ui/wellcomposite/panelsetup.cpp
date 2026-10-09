// 层：视图
// 综合图面板·UI 搭建（工具条/深度读数条/隐藏道管理条/位置图例）——自 wellcompositepanel.cpp 拆出（方向 66，行为零变更）
#include "wellcompositepanel.h"
#include "../../workflow/wellfaciesworkflow.h"
#include "../paleoviewport.h"
#include "../paleotheme.h"
#include "curveconfigdialog.h"
#include "depthtools.h"
#include "exportengine.h"
#include "hiddentrackbar.h"
#include "wellpositionlegendwidget.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QVBoxLayout>

namespace WellComposite
{
void WellCompositePanel::setupUi()
{
  auto *rootLay = new QVBoxLayout(this);
  rootLay->setContentsMargins(0, 0, 0, 0);
  rootLay->setSpacing(PaleoTheme::tokens().spacingXs);

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
  topLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  topLay->setSpacing(PaleoTheme::tokens().spacingSm);

  // 井名 Badge（样式由 setWellName 统一收口，此处仅占位）
  m_lblWellName = new QLabel(topBar);
  m_lblWellName->setObjectName(QStringLiteral("lblWellName"));
  topLay->addWidget(m_lblWellName);

  // 比例尺选择（可编辑，且随着放大/缩小联动动态更新）
  auto *lblScaleTitle = new QLabel(tr("比例尺:"), topBar);
  PaleoTheme::applyThemedStyleSheet(lblScaleTitle, [] {
    return PaleoTheme::mutedCaptionStyleSheet() + PaleoTheme::metricStyleSheet(QStringLiteral(" font-size: {typography.label}pt;"));
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
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QComboBox { border: 1px solid %1; border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; background: %2; }"
               "QComboBox:hover { border-color: %3; }"))
        .arg(t.border.name(), t.surface.name(), t.primary.name());
  });
  topLay->addWidget(m_scaleCombo);

  // 缩放控制组
  auto *lblZoomTitle = new QLabel(tr("深度缩放:"), topBar);
  PaleoTheme::applyThemedStyleSheet(lblZoomTitle, [] {
    return PaleoTheme::mutedCaptionStyleSheet() + PaleoTheme::metricStyleSheet(QStringLiteral(" font-size: {typography.label}pt; margin-left: {spacing.sm}px;"));
  });
  topLay->addWidget(lblZoomTitle);

  // goal/ui-experience-polish：旧 btnStyle 字面量（含 :checked 蓝染底）收敛进
  // 同一活体出口；checked 档换 token 范式（surfaceAltRaised 底 + primary
  // 描边/字，同 ribbonStyleSheet）——主题化收口递延债清偿。
  const auto themedBtnStyle = [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QToolButton { background: %1; border: 1px solid %2; border-radius: {rounded.sm}px; "
               "padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; color: %3; }"
               "QToolButton:hover { background: %4; border-color: %5; }"
               "QToolButton:pressed { background: %2; }"
               "QToolButton:disabled { color: %5; }"
               "QToolButton#btnPredictFacies:enabled { color: %7; }"
               "QToolButton:checked { background: %6; border-color: %7; color: %7; }"))
        .arg(t.surface.name(), t.border.name(), t.text.name(),
             t.surfaceAlt.name(), t.textDisabled.name(),
             t.surfaceAltRaised.name(), t.primaryText.name());
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
    return PaleoTheme::metricStyleSheet(QStringLiteral("QLabel { color: %1; font-size: {typography.label}pt; min-width: 65px; }"))
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

  // ---- 深度交互组（D2.x）----
  m_btnGoto = new QToolButton(topBar);
  m_btnGoto->setObjectName(QStringLiteral("btnCompGoto"));
  m_btnGoto->setText(tr("跳深度"));
  m_btnGoto->setToolTip(tr("跳转到指定深度 (Ctrl+G)"));
  PaleoTheme::applyThemedStyleSheet(m_btnGoto, themedBtnStyle);
  topLay->addWidget(m_btnGoto);

  m_btnBookmarks = new QToolButton(topBar);
  m_btnBookmarks->setObjectName(QStringLiteral("btnCompBookmarks"));
  m_btnBookmarks->setText(tr("书签"));
  m_btnBookmarks->setToolTip(tr("深度书签：添加/跳转/删除"));
  m_btnBookmarks->setPopupMode(QToolButton::InstantPopup);
  PaleoTheme::applyThemedStyleSheet(m_btnBookmarks, themedBtnStyle);
  auto *bmMenu = new QMenu(m_btnBookmarks);
  m_btnBookmarks->setMenu(bmMenu);
  connect(bmMenu, &QMenu::aboutToShow, this, &WellCompositePanel::onBookmarkMenuAboutToShow);
  topLay->addWidget(m_btnBookmarks);

  m_btnSnap = new QToolButton(topBar);
  m_btnSnap->setObjectName(QStringLiteral("btnCompSnap"));
  m_btnSnap->setText(tr("吸附"));
  m_btnSnap->setToolTip(tr("深度标尺吸附整刻度与标志层线（D2.1）"));
  m_btnSnap->setCheckable(true);
  m_btnSnap->setChecked(false);
  PaleoTheme::applyThemedStyleSheet(m_btnSnap, themedBtnStyle);
  topLay->addWidget(m_btnSnap);

  // 测井道配置与排列管理按钮
  m_btnConfigCurves = new QToolButton(topBar);
  m_btnConfigCurves->setObjectName(QStringLiteral("btnConfigCurves"));
  m_btnConfigCurves->setText(tr("测井道配置"));
  m_btnConfigCurves->setToolTip(tr("打开测井道配置与排列管理：支持调整所有井道顺序、合并与解散测井曲线道"));
  PaleoTheme::applyThemedStyleSheet(m_btnConfigCurves, themedBtnStyle);
  topLay->addWidget(m_btnConfigCurves);

  m_faciesModel = new QComboBox(topBar);
  m_faciesModel->setObjectName(QStringLiteral("faciesPredictionModel"));
  m_faciesModel->setAccessibleName(tr("测井相预测模型"));
  m_faciesModel->setMinimumContentsLength(12);
  m_faciesModel->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  topLay->addWidget(m_faciesModel);
  const auto faciesButton = [&](const QString &name, const QString &text) {
    auto *button = new QToolButton(topBar); button->setObjectName(name);
    button->setText(text); button->setAccessibleName(text);
    PaleoTheme::applyThemedStyleSheet(button, themedBtnStyle); topLay->addWidget(button); return button;
  };
  m_btnPredictFacies = faciesButton(QStringLiteral("btnPredictFacies"), tr("预测相"));
  m_btnPredictFacies->setEnabled(false);
  m_btnPredictFacies->setToolTip(tr("测井相预测服务尚未连接"));
  m_btnCancelFacies = faciesButton(QStringLiteral("btnCancelFacies"), tr("停止等待"));
  m_btnCancelFacies->setEnabled(false); m_btnCancelFacies->setToolTip(tr("没有正在等待的预测"));
  m_btnShowFacies = faciesButton(QStringLiteral("btnShowPredictedFacies"), tr("显示预测相"));
  m_btnShowFacies->setCheckable(true); m_btnShowFacies->setChecked(true); m_btnShowFacies->setEnabled(false);
  m_btnShowFacies->setToolTip(tr("先运行测井相预测"));
  m_btnRefreshFacies = faciesButton(QStringLiteral("btnRefreshFaciesModels"), tr("刷新模型"));
  m_btnRefreshFacies->setToolTip(tr("获取网络服务当前可调用的模型及输入要求"));
  m_btnFaciesService = faciesButton(QStringLiteral("btnFaciesService"), tr("预测服务"));
  m_btnFaciesService->setToolTip(tr("设置测井相预测服务地址与 API 密钥"));
  connect(m_btnPredictFacies, &QToolButton::clicked, this, &WellCompositePanel::faciesPredictionRequested);
  connect(m_btnCancelFacies, &QToolButton::clicked, this, &WellCompositePanel::faciesCancelRequested);
  connect(m_btnRefreshFacies, &QToolButton::clicked, this, &WellCompositePanel::faciesModelsRequested);
  connect(m_faciesModel, &QComboBox::currentIndexChanged, this, [this] {
    emit faciesModelSelected(m_faciesModel->currentData().toString());
  });
  connect(m_btnShowFacies, &QToolButton::toggled, this, [this](bool visible) {
    if (m_predictionTrack) m_predictionTrack->setVisible(visible);
    if (m_confidenceTrack) m_confidenceTrack->setVisible(visible);
    m_canvas->updateAll();
  });
  connect(m_btnFaciesService, &QToolButton::clicked, this, [this] {
    if (!m_faciesWorkflow) return;
    const auto config = m_faciesWorkflow->config();
    QDialog dialog(this); dialog.setWindowTitle(tr("测井相预测服务"));
    auto *form = new QFormLayout(&dialog); form->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd); form->setSpacing(PaleoTheme::tokens().spacingSm);
    QLineEdit url(config.baseUrl.toString()), key(QString::fromUtf8(config.apiKey));
    url.setPlaceholderText(tr("https://服务地址"));
    key.setEchoMode(QLineEdit::Password);
    // #133：只接受 https；内网 http 服务须显式勾选并接受明文风险。
    QCheckBox insecure(tr("允许不加密的 HTTP（API 密钥与井数据将明文传输）"));
    insecure.setChecked(config.allowInsecureHttp);
    form->addRow(tr("服务地址"), &url); form->addRow(tr("API 密钥"), &key);
    form->addRow(QString(), &insecure);
    QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) emit faciesConfigurationRequested(url.text().trimmed(), key.text().trimmed(), insecure.isChecked());
  });

  // ---- D4.x 导出菜单 ----
  m_btnExport = new QToolButton(topBar);
  m_btnExport->setObjectName(QStringLiteral("btnCompExport"));
  m_btnExport->setText(tr("导出"));
  m_btnExport->setToolTip(tr("导出 PDF/PNG/SVG、打印、导出预设管理"));
  m_btnExport->setPopupMode(QToolButton::InstantPopup);
  PaleoTheme::applyThemedStyleSheet(m_btnExport, themedBtnStyle);
  auto *exportMenu = new QMenu(m_btnExport);
  QAction *actPdf = exportMenu->addAction(tr("导出 PDF…"));
  QAction *actPng = exportMenu->addAction(tr("导出 PNG (300dpi)…"));
  QAction *actSvg = exportMenu->addAction(tr("导出 SVG…"));
  exportMenu->addSeparator();
  QAction *actPrint = exportMenu->addAction(tr("打印…"));
  QAction *actPreset = exportMenu->addAction(tr("导出预设…"));
  connect(actPdf, &QAction::triggered, this, [this]() { exportCurrent(ExportEngine::Format::Pdf); });
  connect(actPng, &QAction::triggered, this, [this]() { exportCurrent(ExportEngine::Format::Png); });
  connect(actSvg, &QAction::triggered, this, [this]() { exportCurrent(ExportEngine::Format::Svg); });
  connect(actPrint, &QAction::triggered, this, [this]() { printCurrent(); });
  connect(actPreset, &QAction::triggered, this, [this]() { manageExportPresets(); });
  m_btnExport->setMenu(exportMenu);
  topLay->addWidget(m_btnExport);

  // ---- D3.1/D3.14 编辑模式开关 ----
  m_btnEdit = new QToolButton(topBar);
  m_btnEdit->setObjectName(QStringLiteral("btnCompEdit"));
  m_btnEdit->setText(tr("TOPs 编辑"));
  m_btnEdit->setToolTip(tr("进入标志层/区间编辑模式：标志层线可拖拽改顶深，区间可编辑"));
  m_btnEdit->setCheckable(true);
  PaleoTheme::applyThemedStyleSheet(m_btnEdit, themedBtnStyle);
  topLay->addWidget(m_btnEdit);

  // ---- D1 保存派生版本：意图信号 → 壳 derivedsink 落 catalog DERIVED ----
  m_btnSaveDerived = new QToolButton(topBar);
  m_btnSaveDerived->setObjectName(QStringLiteral("btnCompSaveDerived"));
  m_btnSaveDerived->setText(tr("保存派生"));
  m_btnSaveDerived->setToolTip(
      tr("将当前编辑保存为派生版本。父版本为源井数据的原始版本，"
         "审计记录写入派生文件的「编辑审计」工作表"));
  PaleoTheme::applyThemedStyleSheet(m_btnSaveDerived, themedBtnStyle);
  connect(m_btnSaveDerived, &QToolButton::clicked, this, [this]() {
    if (!saveDerived())
      m_lblStatus->setText(tr("无可保存的编辑（先在 TOPs 编辑模式修改数据）"));
  });
  topLay->addWidget(m_btnSaveDerived);

  topLay->addStretch(1);

  // D2.11 当前深度读数条：大字号 mono 深度 + 最近标志层名
  m_lblReadout = new QLabel(QStringLiteral("— m"), topBar);
  m_lblReadout->setObjectName(QStringLiteral("lblCompReadout"));
  PaleoTheme::applyThemedStyleSheet(m_lblReadout, [] {
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QLabel { font-family: '{font.mono}'; font-size: {typography.title}pt; color: %1;"
        " font-weight: 500; padding: 0 {spacing.sm}px; }"))
        .arg(PaleoTheme::tokens().text.name());
  });
  topLay->addWidget(m_lblReadout);

  for (const auto &entry : {qMakePair(QStringLiteral("btnWellAttributes"), tr("岩性与相属性")),
                            qMakePair(QStringLiteral("btnWellFactors"), tr("井点因子属性"))}) {
    auto *button = new QToolButton(topBar);
    button->setObjectName(entry.first); button->setText(entry.second);
    button->setAccessibleName(entry.second); button->setEnabled(false);
    button->setToolTip(tr("请先打开工程并绑定当前井"));
    PaleoTheme::applyThemedStyleSheet(button, themedBtnStyle);
    topLay->addWidget(button);
  }

  // 状态信息显示（悬停深度等）
  m_lblStatus = new QLabel(tr("就绪 | 支持按住拖拽漫游，Ctrl+滚轮缩放"), topBar);
  m_lblStatus->setObjectName(QStringLiteral("lblStatus"));
  PaleoTheme::applyThemedStyleSheet(m_lblStatus, [] {
    return PaleoTheme::mutedCaptionStyleSheet() + PaleoTheme::metricStyleSheet(QStringLiteral(" font-size: {typography.label}pt;"));
  });
  topLay->addWidget(m_lblStatus);

  rootLay->addWidget(new PaleoToolRow(topBar, this));
  m_faciesStatus = new QLabel(this);
  m_faciesStatus->setObjectName(QStringLiteral("lblFaciesPredictionStatus"));
  m_faciesStatus->setWordWrap(true);
  rootLay->addWidget(m_faciesStatus);

  // 中央综合柱状图画布
  m_canvas = new WellCompositeCanvas(this);
  m_canvas->setObjectName(QStringLiteral("wellCompositeCanvas"));
  rootLay->addWidget(m_canvas, 1);

  // D1.10 隐藏道管理条
  m_hiddenBar = new HiddenTrackBar(this);
  rootLay->addWidget(m_hiddenBar);

  // 底部位置显示与比例尺图例综合控制栏
  m_legendWidget = new WellPositionLegendWidget(this);
  m_legendWidget->setObjectName(QStringLiteral("wellPositionLegendWidget"));
  rootLay->addWidget(new PaleoToolRow(m_legendWidget, this));

  // ---- 事件与信号绑定 ----
  connect(m_btnZoomIn, &QToolButton::clicked, m_canvas, &WellCompositeCanvas::zoomIn);
  connect(m_btnZoomOut, &QToolButton::clicked, m_canvas, &WellCompositeCanvas::zoomOut);
  connect(m_btnResetZoom, &QToolButton::clicked, m_canvas, &WellCompositeCanvas::resetZoom);
  connect(m_btnConfigCurves, &QToolButton::clicked, this, &WellCompositePanel::openCurveConfigDialog);
  connect(m_btnGoto, &QToolButton::clicked, this, &WellCompositePanel::openGotoDepthDialog);
  connect(m_btnSnap, &QToolButton::toggled, m_canvas, &WellCompositeCanvas::setSnapEnabled);
  connect(m_btnEdit, &QToolButton::toggled, this, &WellCompositePanel::setEditMode);

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
    // D2.11 读数条：深度 + 最近标志层名；D1 追加 TVD/TWT（壳喂表后自动）
    m_lblReadout->setText(DepthTools::readoutText(qMax(0.0, d), m_canvas->markerLines()) +
                          depthReadoutSuffix(qMax(0.0, d)));

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

  // ---- 道操作意图接线（D1.5/D1.6/D1.10）----
  connect(m_canvas, &WellCompositeCanvas::trackCsvRequested, this, &WellCompositePanel::onTrackCsvRequested);
  connect(m_canvas, &WellCompositeCanvas::trackConfigRequested, this, &WellCompositePanel::onTrackConfigRequested);
  connect(m_canvas, &WellCompositeCanvas::trackDuplicationRequested, this, &WellCompositePanel::onTrackDuplicateRequested);
  connect(m_canvas, &WellCompositeCanvas::trackVisibilityChanged, this, &WellCompositePanel::onTrackVisibilityChanged);
  connect(m_canvas, &WellCompositeCanvas::trackOrderChanged, this, &WellCompositePanel::onTrackOrderOrWidthChanged);
  connect(m_canvas, &WellCompositeCanvas::trackWidthChanged, this, &WellCompositePanel::onTrackOrderOrWidthChanged);
  connect(m_canvas, &WellCompositeCanvas::gotoDepthRequested, this, &WellCompositePanel::openGotoDepthDialog);
  connect(m_hiddenBar, &HiddenTrackBar::trackRestoreRequested, this, [this](const QString &title) {
    for (int i = 0; i < m_canvas->trackCount(); ++i)
    {
      if (m_canvas->tracks().at(i)->title() == title && !m_canvas->tracks().at(i)->isVisible())
      {
        m_canvas->tracks().at(i)->setVisible(true);
        m_canvas->syncScrollBars();
        m_canvas->updateAll();
        emit m_canvas->trackVisibilityChanged(i);
        return;
      }
    }
  });

  // ---- 深度交互接线（D2.2/D2.3）----
  connect(m_canvas, &WellCompositeCanvas::intervalSelected, this, &WellCompositePanel::onIntervalSelected);
  connect(m_canvas, &WellCompositeCanvas::pinCreateRequested, this, &WellCompositePanel::onPinCreateRequested);
  connect(m_canvas, &WellCompositeCanvas::pinEditRequested, this, &WellCompositePanel::onPinEditRequested);

  // ---- 编辑接线（D3.1）----
  connect(m_canvas, &WellCompositeCanvas::markerMoved, this, &WellCompositePanel::onMarkerMoved);

  connect(m_canvas, &WellCompositeCanvas::viewportChanged,
          m_legendWidget, &WellPositionLegendWidget::updateViewport);
  connect(m_canvas, &WellCompositeCanvas::depthHovered,
          m_legendWidget, &WellPositionLegendWidget::updateHoverDepth);
  connect(m_legendWidget, &WellPositionLegendWidget::requestScrollDepth, this,
          [this](double depth) { m_canvas->setScrollDepth(depth); });

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

} // namespace WellComposite
