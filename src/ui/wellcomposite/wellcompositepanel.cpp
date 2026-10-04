// 层：视图
#include "wellcompositepanel.h"
#include "../../workflow/wellfaciesworkflow.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSignalBlocker>
#include "../paleoviewport.h"
#include "curveconfigdialog.h"
#include "wellpositionlegendwidget.h"
#include "../paleotheme.h"
#include "../../services/previewdoc.h" // 数据门面（W1：XML 解析入口不直触）
#include "../../services/paleotaskservice.h" // F2：两段式 XML 任务池路径
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPrinter>
#include <QPrintDialog>
#include <QPushButton>
#include <QLineEdit>
#include <QShortcut>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <utility>

#include "curveconfigdialog.h"
#include "editsession.h"
#include "exportengine.h"
#include "hiddentrackbar.h"
#include "intervaleditor.h"
#include "intervalstatistics.h"
#include "derivedsink.h"
#include "trackconfigdialog.h"
#include "trackops.h"
#include "stratassignment.h"
#include "trackregistry.h"
#include "wellpositionlegendwidget.h"

namespace WellComposite
{
namespace { WellCompositePanel::FaciesWorkflowFactory s_faciesFactory; }
void WellCompositePanel::setFaciesWorkflowFactory(FaciesWorkflowFactory factory) {
  s_faciesFactory = std::move(factory);
}

WellCompositePanel::WellCompositePanel(QWidget *parent)
  : QWidget(parent)
{
  setupUi();

  // D2.7 Ctrl+G 跳深度
  auto *shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+G")), this);
  connect(shortcut, &QShortcut::activated, this, &WellCompositePanel::openGotoDepthDialog);

  // D1：登记进活跃面板表并挂接默认 sink（壳 attachWorkflows 先于预览页建立；
  // sink 迟装时由 setDefault 补挂）。
  WellCompositeDerivedSink::registerPanel(this);
  if (s_faciesFactory) bindFaciesWorkflow(s_faciesFactory(this));
}

WellCompositePanel::~WellCompositePanel()
{
  saveSessionState();
  // 子控件由 ~QWidget 在本类析构完成之后才删除；期间画布刷新刻度、组合框失焦
  // （editingFinished → setScaleRatio → scaleRatioChanged）等仍会发信号，进入捕获
  // this 的 lambda 访问已析构成员（UBSan：member access … not WellCompositePanel）。
  // 先断开所有子对象 → this 的连接；this 作为 context 的自动断开要到 ~QObject 才发生。
  const auto kids = findChildren<QObject *>();
  for (QObject *child : kids)
    QObject::disconnect(child, nullptr, this, nullptr);
}

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
      tr("把当前编辑保存为派生版本：壳登记 catalog（父版本=源井数据 RAW），"
         "审计历史写入派生 XML 的「编辑审计」工作表"));
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
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "background: %1; color: %2; font-weight: bold; border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.body}pt;"))
        .arg(t.surfaceAltRaised.name(), t.text.name());
  };
  const auto referenceBadgeStyle = [] {
    return PaleoTheme::capsuleStyleSheet(PaleoTheme::CapsuleKind::Warning) +
           PaleoTheme::metricStyleSheet(QStringLiteral(" font-weight: bold; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.body}pt;"));
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

void WellCompositePanel::setProjectName(const QString &project)
{
  m_projectName = project.trimmed().isEmpty() ? QStringLiteral("default") : project.trimmed();
}

void WellCompositePanel::setSourceDataPath(const QString &path)
{
  m_sourceDataPath = path;
  if (path.isEmpty())
  {
    m_store.reset();
    return;
  }
  m_store = std::make_unique<WellCompositeStore>(path);
  m_store->load();
  const qint64 mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
  if (m_editSession)
    m_editSession->setSourceMtime(mtime);
}

// ----------------------------------------------------------------------------
// 数据装配
// ----------------------------------------------------------------------------
bool WellCompositePanel::loadComprehensiveXml(const QString &xmlPath)
{
  ++m_xmlLoadSeq; // 同步换源同样使在途异步结果作废
  ComprehensiveWellData data;
  QString err;
  if (!PreviewDocService::wellCompositeAt(xmlPath, &data, &err))
    return false; // 同步失败语义保持：返回值即终态，不发信号（调用方直取）
  applyComprehensiveData(data, xmlPath);
  return true;
}

void WellCompositePanel::applyComprehensiveData(const ComprehensiveWellData &data,
                                                const QString &xmlPath, bool reference)
{
  m_data = data;
  setWellName(data.wellName, reference);
  setupTracksFromData(data);
  if (m_legendWidget)
    m_legendWidget->setWellData(data);
  clearDepthTables(); // D1：换源复位井斜/时深（wellLoaded 后 sink 按新源重喂）

  // D3.x 编辑会话：工作副本 + 源 mtime 跟踪（D3.10）
  m_editSession = std::make_unique<EditSession>(data, this);
  connect(m_editSession.get(), &EditSession::documentChanged, this, [this]() {
    syncSessionToTracks();
  });
  setSourceDataPath(xmlPath);

  // D2.x sidecar + D1.8 会话记忆
  loadSidecar();
  restoreSessionState();
  emit wellLoaded(data.wellName);
  emit faciesDataChanged(m_data);
  emit comprehensiveXmlLoaded(true);
}

bool WellCompositePanel::loadWellData(const ComprehensiveWellData &data, const QString &sourcePath, bool reference)
{
  ++m_xmlLoadSeq;
  if (data.isEmpty()) return false;
  applyComprehensiveData(data, sourcePath, reference);
  return true;
}

bool WellCompositePanel::loadComprehensiveXmlAsync(const QString &xmlPath,
                                                   PaleoTaskService *svc)
{
  // 快速失败留在同步侧（与旧路径一致：文件不存在 false，不拉任务）。
  if (xmlPath.isEmpty() || !QFile::exists(xmlPath))
    return false;
  if (!svc)
    return loadComprehensiveXml(xmlPath); // 无任务服务：同步旧路径（测试）

  const int seq = ++m_xmlLoadSeq;
  // ComprehensiveWellData 值语义（QVector 底）——池线程产出、GUI 线程装配，
  // shared_ptr 交接（与 WellCorrelationPanel::submitLasLoad 同一纪律）。
  auto out = std::make_shared<ComprehensiveWellData>();
  auto *task = svc->start(
      tr("解析综合柱状图 %1").arg(QFileInfo(xmlPath).fileName()),
      [xmlPath, out](PaleoTask *) -> QString {
        QString err;
        if (!PreviewDocService::wellCompositeAt(xmlPath, out.get(), &err))
          return err.isEmpty() ? QObject::tr("无法解析综合柱状图 XML") : err;
        return QString();
      },
      QString(), /*quiet=*/true); // 交互内嵌取数——不拉起任务中心
  connect(task, &PaleoTask::finished, this,
          [this, seq, out, xmlPath, task]() {
            if (seq != m_xmlLoadSeq)
              return; // 陈旧结果丢弃：换源/重入已接管
            if (task->state() == PaleoTask::State::Succeeded)
              applyComprehensiveData(*out, xmlPath);
            else
              emit comprehensiveXmlLoaded(false);
          });
  return true;
}

bool WellCompositePanel::loadLasCurves(const QString &wellName, const QVector<CurveData> &curves,
                                       const QVector<FormationInterval> &formations)
{
  ++m_xmlLoadSeq;
  m_data = {};
  m_editSession.reset();
  clearFaciesPrediction();
  m_canvas->clearTracks();
  setWellName(wellName);
  clearDepthTables(); // D1：换井复位（LAS 路径无井斜/时深表，保持禁用态）

  if (curves.isEmpty() && formations.isEmpty())
  {
    emit faciesDataChanged(m_data);
    return false;
  }

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
    // intervals() 按值返回：先绑定到局部再取迭代器（两次调用 = 两个不同临时对象）。
    const auto derived = stratTrack->intervals();
    const bool anySystem = std::any_of(derived.cbegin(), derived.cend(),
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

  // 4. 曲线道 —— 按助记名语义分道（H3：经 TrackSpec/注册表装配，配置对话框可改）
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

  QVector<CurveData> lithoCurves;
  QVector<CurveData> poroCurves;
  QVector<CurveData> resCurves;
  QVector<CurveData> otherCurves;

  for (const auto &c : curves)
  {
    if (isLitho(c.name)) lithoCurves.append(c);
    else if (isPorosity(c.name)) poroCurves.append(c);
    else if (c.name.toUpper().startsWith(QStringLiteral("RT"))) resCurves.append(c);
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

  // LAS 路径无 sidecar（曲线为主）；编辑会话仍可建（层位编辑）
  m_editSession = std::make_unique<EditSession>(m_data, this);
  connect(m_editSession.get(), &EditSession::documentChanged, this, [this]() {
    syncSessionToTracks();
  });

  const QList<TrackSpec> mem = WellCompositeStore::loadSessionTracks(m_projectName, m_wellName);
  restoreSessionState();
  m_canvas->setScaleRatio(m_scaleCombo->currentText());
  emit faciesDataChanged(m_data);
  return true;
}

void WellCompositePanel::setupTracksFromData(const ComprehensiveWellData &data)
{
  m_canvas->clearTracks();
  m_canvas->setDepthRange(data.minDepth, data.maxDepth);

  // D2.x 标志层线（渲染/吸附/读数/gap 数据面）
  m_canvas->setMarkerLines(data.standardHorizons);

  // 1. 地层系统组组合道：文档真实地层系统数据，或分层名能映射出系/统时展示。
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
      const auto derived = stratTrack->intervals(); // 按值返回，绑定局部后再迭代
      const bool anySystem = std::any_of(derived.cbegin(), derived.cend(),
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

  // 7. 取芯道
  if (!data.coreBarrels.isEmpty())
  {
    auto coreTrack = std::make_shared<CoreTrack>(QStringLiteral("取心数据"), 65.0);
    coreTrack->setBarrels(data.coreBarrels);
    m_canvas->addTrack(coreTrack);
  }

  // 8. 曲线道（连续物理曲线：4 根合并）
  if (!data.continuousCurves.isEmpty())
  {
    for (int i = 0; i < data.continuousCurves.size(); i += 4)
    {
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
        curveTrack->addCurve(data.continuousCurves.at(i + j));
      m_canvas->addTrack(curveTrack);
    }
  }

  // 9. 离散曲线道
  if (!data.discreteCurves.isEmpty())
  {
    for (int i = 0; i < data.discreteCurves.size(); i += 4)
    {
      const QString title = (i == 0) ? QStringLiteral("实测物性分析") : QStringLiteral("地化/生烃潜量");
      auto discTrack = std::make_shared<CurveTrack>(title, 160.0);
      for (int j = 0; j < 4 && (i + j) < data.discreteCurves.size(); ++j)
        discTrack->addCurve(data.discreteCurves.at(i + j));
      m_canvas->addTrack(discTrack);
    }
  }

  // 10. 文本道
  if (!data.textIntervals.isEmpty())
  {
    auto textTrack = std::make_shared<TextTrack>(QStringLiteral("解释结论/取样"), 120.0);
    textTrack->setIntervals(data.textIntervals);
    m_canvas->addTrack(textTrack);
  }

  // 11. 沉积相组合道（规范放置在最右侧）
  if (!data.faciesIntervals.isEmpty())
  {
    auto faciesTrack = std::make_shared<FaciesCompoundTrack>(QStringLiteral("沉积相"), 180.0);
    faciesTrack->setIntervals(data.faciesIntervals);
    m_canvas->addTrack(faciesTrack);
  }

  m_canvas->setScaleRatio(m_scaleCombo->currentText());
}

// 编辑会话数据 → 画布道重同步（undo/redo/编辑后统一走这里）
void WellCompositePanel::syncSessionToTracks()
{
  if (!m_editSession)
    return;
  const auto &doc = m_editSession->document();
  m_canvas->setMarkerLines(doc.standardHorizons);

  for (const auto &t : m_canvas->tracks())
  {
    if (!t)
      continue;
    if (t->type() == TrackType::Formation)
    {
      auto ft = std::static_pointer_cast<FormationTrack>(t);
      if (!doc.formationIntervals.isEmpty())
        ft->setIntervals(doc.formationIntervals);
    }
    else if (t->type() == TrackType::Lithology)
    {
      auto lt = std::static_pointer_cast<LithologyTrack>(t);
      lt->setIntervals(doc.lithologyIntervals);
    }
    else if (t->type() == TrackType::FaciesCompound)
    {
      auto fc = std::static_pointer_cast<FaciesCompoundTrack>(t);
      fc->setIntervals(doc.faciesIntervals);
    }
    else if (t->type() == TrackType::StratigraphyCompound)
    {
      auto sc = std::static_pointer_cast<StratigraphyCompoundTrack>(t);
      if (!doc.stratigraphyIntervals.isEmpty())
        sc->setIntervals(doc.stratigraphyIntervals);
    }
  }
  m_canvas->updateAll();
  emit faciesDataChanged(doc);
}

void WellCompositePanel::bindFaciesWorkflow(WellFaciesWorkflow *workflow) {
  if (!workflow || m_faciesWorkflow) return;
  m_faciesWorkflow = workflow;
  connect(this, &WellCompositePanel::faciesDataChanged, workflow, &WellFaciesWorkflow::setData);
  connect(this, &WellCompositePanel::faciesPredictionRequested, workflow, &WellFaciesWorkflow::run);
  connect(this, &WellCompositePanel::faciesCancelRequested, workflow, &WellFaciesWorkflow::cancel);
  connect(this, &WellCompositePanel::faciesModelsRequested, workflow, &WellFaciesWorkflow::refreshModels);
  connect(this, &WellCompositePanel::faciesModelSelected, workflow, &WellFaciesWorkflow::selectModel);
  connect(this, &WellCompositePanel::faciesConfigurationRequested, workflow, [workflow](const QString &url, const QString &key, bool allowInsecureHttp) {
    WellFaciesConfig config; config.baseUrl = QUrl(url); config.apiKey = key.toUtf8(); config.allowInsecureHttp = allowInsecureHttp; workflow->configure(config);
  });
  connect(workflow, &WellFaciesWorkflow::modelsChanged, this, [this](const QVariantList &models) {
    QSignalBlocker block(m_faciesModel); m_faciesModel->clear();
    for (const auto &v : models) {
      const auto m = v.toMap(); m_faciesModel->addItem(m.value("name").toString(), m.value("id"));
      m_faciesModel->setItemData(m_faciesModel->count()-1, m.value("requirements"), Qt::ToolTipRole);
      if (m.value("selected").toBool()) m_faciesModel->setCurrentIndex(m_faciesModel->count()-1);
    }
  });
  connect(workflow, &WellFaciesWorkflow::availabilityChanged, this, [this](bool ready, const QString &reason) {
    m_btnPredictFacies->setEnabled(ready);
    const QString requirements = m_faciesModel->currentData(Qt::ToolTipRole).toString();
    m_faciesModel->setToolTip(requirements);
    m_btnPredictFacies->setToolTip(ready ? tr("提交当前井的对应井段预测相：%1").arg(requirements) : reason);
    m_faciesStatus->setText(ready ? tr("可预测：%1").arg(requirements) : reason);
  });
  connect(workflow, &WellFaciesWorkflow::busyChanged, this, [this](bool busy) {
    m_btnCancelFacies->setEnabled(busy);
    m_btnCancelFacies->setToolTip(busy ? tr("停止本地等待，服务端已受理任务继续执行") : tr("没有正在等待的预测"));
    m_faciesModel->setEnabled(!busy); m_btnFaciesService->setEnabled(!busy); m_btnRefreshFacies->setEnabled(!busy);
  });
  connect(workflow, &WellFaciesWorkflow::statusChanged, m_faciesStatus, &QLabel::setText);
  connect(workflow, &WellFaciesWorkflow::resultReady, this, &WellCompositePanel::showFaciesPrediction);
  connect(workflow, &WellFaciesWorkflow::resultCleared, this, &WellCompositePanel::clearFaciesPrediction);
  workflow->setData(m_data);
}
void WellCompositePanel::clearFaciesPrediction() {
  for (int i=m_canvas->trackCount()-1; i>=0; --i) {
    const auto track = m_canvas->tracks().at(i);
    if (track == m_predictionTrack || track == m_confidenceTrack) m_canvas->removeTrack(i);
  }
  m_predictionTrack.reset(); m_confidenceTrack.reset(); m_btnShowFacies->setEnabled(false);
  m_btnShowFacies->setToolTip(tr("先运行测井相预测"));
}
void WellCompositePanel::showFaciesPrediction(const WellFaciesResult &result) {
  clearFaciesPrediction();
  auto labels = std::make_shared<TextTrack>(tr("预测相"), 120.0);
  labels->setIntervals(result.intervals);
  labels->setKeepTextVisible(true);
  auto confidence = std::make_shared<CurveTrack>(tr("预测置信度"), 140.0);
  confidence->addCurve(result.confidence);
  m_predictionTrack = labels; m_confidenceTrack = confidence;
  labels->setVisible(m_btnShowFacies->isChecked()); confidence->setVisible(m_btnShowFacies->isChecked());
  m_canvas->addTrack(labels); m_canvas->addTrack(confidence);
  m_btnShowFacies->setEnabled(true);
  m_btnShowFacies->setToolTip(tr("显示或隐藏预测相和置信度；模型 %1 %2；任务 %3")
                            .arg(result.modelName, result.modelVersion, result.jobId));
  m_canvas->updateAll();
}

// ----------------------------------------------------------------------------
// D1.5/D1.6/D1.10 道操作槽
// ----------------------------------------------------------------------------
void WellCompositePanel::onTrackCsvRequested(int trackIndex)
{
  const QString csv = m_canvas->trackCsvAt(trackIndex);
  if (csv.isEmpty())
    return;

  const QString suggested = QStringLiteral("%1_%2.csv").arg(m_wellName.isEmpty() ? QStringLiteral("well") : m_wellName,
                                                            m_canvas->tracks().at(trackIndex)->title());
  QString path = QFileDialog::getSaveFileName(this, tr("导出该道 CSV"), suggested,
                                              QStringLiteral("CSV (*.csv)"));
  if (path.isEmpty())
    return;
  if (!path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive))
    path += QStringLiteral(".csv");

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件: %1").arg(path));
    return;
  }
  f.write("\xEF\xBB\xBF"); // UTF-8 BOM（Excel 中文兼容）
  f.write(csv.toUtf8());
  m_lblStatus->setText(tr("已导出: %1").arg(path));
}

void WellCompositePanel::onTrackConfigRequested(int trackIndex)
{
  if (trackIndex < 0 || trackIndex >= m_canvas->trackCount())
    return;

  const auto track = m_canvas->tracks().at(trackIndex); // 拷贝 shared_ptr：tracks() 按值返回，后续跨 exec()
  TrackSpec initial = TrackRegistry::instance().captureSpec(track);
  if (initial.typeId.isEmpty())
    initial.typeId = TrackRegistry::typeIdForEnum(track->type());

  TrackConfigDialog dlg(initial, TrackOps::combinedCurvePool(m_data.continuousCurves, m_data.discreteCurves), this);
  if (dlg.exec() != QDialog::Accepted)
    return;

  const TrackSpec spec = dlg.resultSpec();
  track->setTitle(spec.title);
  track->setWidth(spec.width);
  track->setVisible(spec.visible);
  track->setPrintIncluded(spec.printIncluded);

  if (auto ct = std::dynamic_pointer_cast<CurveTrack>(track))
  {
    ct->setShowGrid(spec.showGrid());
    ct->setGridDensity(spec.gridDensity());
    if (!spec.curveNames().isEmpty())
      TrackOps::injectCurvesFromWellData(*ct, spec, m_data);
  }
  m_canvas->updateAll();
  onTrackOrderOrWidthChanged();
}

void WellCompositePanel::onTrackDuplicateRequested(int trackIndex)
{
  if (trackIndex < 0 || trackIndex >= m_canvas->trackCount())
    return;
  auto dup = TrackOps::duplicateTrack(m_canvas->tracks().at(trackIndex));
  if (!dup)
    return;
  dup->setTitle(dup->title() + tr(" 副本"));
  m_canvas->insertTrack(trackIndex + 1, dup);
}

void WellCompositePanel::onTrackVisibilityChanged(int trackIndex)
{
  // D1.10 隐藏道管理条刷新
  m_hiddenBar->setTracks(currentSpecs());
  onTrackOrderOrWidthChanged();
}

void WellCompositePanel::onTrackOrderOrWidthChanged()
{
  // D1.8 会话记忆（道序/宽度/显隐/打印开关）
  WellCompositeStore::saveSessionTracks(m_projectName, m_wellName, currentSpecs());

  // D1.4 宽度集合独立记忆（标题键）
  QVariantMap widths;
  for (const auto &t : m_canvas->tracks())
    if (t)
      widths.insert(t->title(), t->width());
  WellCompositeStore::saveWidthSet(m_projectName, m_wellName, widths);
}

QList<TrackSpec> WellCompositePanel::currentSpecs() const
{
  QList<TrackSpec> specs;
  for (const auto &t : m_canvas->tracks())
  {
    TrackSpec spec = TrackRegistry::instance().captureSpec(t);
    if (spec.typeId.isEmpty() && t)
      spec.typeId = TrackRegistry::typeIdForEnum(t->type());
    specs << spec;
  }
  return specs;
}

void WellCompositePanel::applySpecsIncrementally(const QList<TrackSpec> &specs)
{
  // D1.1 道增删改不重建面板：按 spec 列表增量调整既有道（顺序/宽度/可见性/
  // 打印开关按「类型+标题」匹配）；匹配不上的忽略（数据驱动的道可能改名）。
  if (specs.isEmpty())
    return;

  QList<std::shared_ptr<WellTrack>> reordered;
  for (const auto &spec : specs)
  {
    for (const auto &t : m_canvas->tracks())
    {
      if (!t)
        continue;
      const QString typeId = TrackRegistry::typeIdForEnum(t->type());
      if (typeId == spec.typeId && t->title() == spec.title)
      {
        t->setWidth(spec.width);
        t->setVisible(spec.visible);
        t->setPrintIncluded(spec.printIncluded);
        if (auto ct = std::dynamic_pointer_cast<CurveTrack>(t))
        {
          ct->setShowGrid(spec.showGrid());
          ct->setGridDensity(spec.gridDensity());
        }
        reordered << t;
        break;
      }
    }
  }
  // 记忆外的道（新建/数据新加）保持尾部
  for (const auto &t : m_canvas->tracks())
    if (t && !reordered.contains(t))
      reordered << t;

  if (reordered.size() == m_canvas->trackCount())
    m_canvas->setTracks(reordered);
  m_canvas->syncScrollBars();
  m_canvas->updateAll();
}

void WellCompositePanel::saveSessionState() const
{
  if (m_wellName.isEmpty())
    return;
  WellCompositeStore::saveSessionTracks(m_projectName, m_wellName, currentSpecs());
}

void WellCompositePanel::restoreSessionState()
{
  if (m_wellName.isEmpty())
    return;
  const QList<TrackSpec> remembered = WellCompositeStore::loadSessionTracks(m_projectName, m_wellName);
  applySpecsIncrementally(remembered);
  m_hiddenBar->setTracks(currentSpecs());
}

// ----------------------------------------------------------------------------
// D2.x 深度交互槽
// ----------------------------------------------------------------------------
void WellCompositePanel::onIntervalSelected(double top, double bottom)
{
  // D2.2 区间统计对话框
  const IntervalStatsReport rep = computeIntervalStats(top, bottom, m_data);
  if (!rep.isValid())
    return;

  QDialog dlg(this);
  dlg.setObjectName(QStringLiteral("wellCompositeIntervalStatsDialog"));
  dlg.setWindowTitle(tr("区间统计 [%1~%2m]").arg(QString::number(top, 'f', 1),
                                                QString::number(bottom, 'f', 1)));
  dlg.setMinimumSize(QSize(520, 380));
  auto *lay = new QVBoxLayout(&dlg);
  auto *edit = new QPlainTextEdit(&dlg);
  edit->setReadOnly(true);
  edit->setFont(PaleoTheme::monoFont());
  edit->setPlainText(rep.toTsv());
  lay->addWidget(edit, 1);

  auto *btnRow = new QWidget(&dlg);
  auto *btnLay = new QHBoxLayout(btnRow);
  auto *btnCopy = new QPushButton(tr("复制 TSV"), btnRow);
  auto *btnClose = new QPushButton(tr("关闭"), btnRow);
  btnLay->addStretch(1);
  btnLay->addWidget(btnCopy);
  btnLay->addWidget(btnClose);
  lay->addWidget(btnRow);
  connect(btnCopy, &QPushButton::clicked, this, [edit]() {
    QApplication::clipboard()->setText(edit->toPlainText());
  });
  connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);
  dlg.exec();
}

void WellCompositePanel::onPinCreateRequested(double depth)
{
  bool ok = false;
  const QString text = QInputDialog::getText(this, tr("添加深度标注"),
                                             tr("深度 %1 m 的标注文字:").arg(QString::number(depth, 'f', 1)),
                                             QLineEdit::Normal, QString(), &ok);
  if (!ok)
    return;

  QList<DepthPin> pins = m_canvas->pins();
  DepthTools::addPin(&pins, depth, text);
  m_canvas->setPins(pins);
  if (m_store)
  {
    m_store->setPins(pins);
    m_store->save();
  }
}

void WellCompositePanel::onPinEditRequested(int pinIndex)
{
  QList<DepthPin> pins = m_canvas->pins();
  if (pinIndex < 0 || pinIndex >= pins.size())
    return;

  bool ok = false;
  const QString text = QInputDialog::getText(this, tr("编辑深度标注"),
                                             tr("深度 %1 m 的标注文字:").arg(QString::number(pins.at(pinIndex).depth, 'f', 1)),
                                             QLineEdit::Normal, pins.at(pinIndex).text, &ok);
  if (!ok)
    return;

  DepthTools::updatePinText(&pins, pinIndex, text);
  m_canvas->setPins(pins);
  if (m_store)
  {
    m_store->setPins(pins);
    m_store->save();
  }
}

void WellCompositePanel::clearPins()
{
  m_canvas->setPins({});
  if (m_store)
  {
    m_store->setPins({});
    m_store->save();
  }
}

void WellCompositePanel::addPinAt(double depth, const QString &text)
{
  QList<DepthPin> pins = m_canvas->pins();
  DepthTools::addPin(&pins, depth, text);
  m_canvas->setPins(pins);
  if (m_store)
  {
    m_store->setPins(pins);
    m_store->save();
  }
}

void WellCompositePanel::openGotoDepthDialog()
{
  GotoDepthDialog dlg(m_canvas->minDepth(), m_canvas->maxDepth(),
                      m_canvas->visibleTopDepth(), m_depthFeet, this);
  if (dlg.exec() == QDialog::Accepted)
    m_canvas->setScrollDepth(dlg.selectedDepth());
}

void WellCompositePanel::onBookmarkMenuAboutToShow()
{
  QMenu *menu = m_btnBookmarks->menu();
  menu->clear();

  QAction *actAdd = menu->addAction(tr("在视口顶部添加书签…"));
  connect(actAdd, &QAction::triggered, this, [this]() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("添加书签"),
                                               tr("书签名（深度 %1 m）:")
                                                   .arg(QString::number(m_canvas->visibleTopDepth(), 'f', 1)),
                                               QLineEdit::Normal, QString(), &ok);
    if (ok)
      addBookmark(name, m_canvas->visibleTopDepth());
  });

  if (!m_bookmarks.isEmpty())
  {
    menu->addSeparator();
    for (const auto &bm : std::as_const(m_bookmarks))
    {
      QAction *act = menu->addAction(QStringLiteral("%1 @ %2 m").arg(bm.name, QString::number(bm.depth, 'f', 1)));
      connect(act, &QAction::triggered, this, [this, name = bm.name]() { jumpToBookmark(name); });
    }
    menu->addSeparator();
    QAction *actClear = menu->addAction(tr("清除全部书签"));
    connect(actClear, &QAction::triggered, this, [this]() {
      setBookmarks({});
    });
  }
}

bool WellCompositePanel::addBookmark(const QString &name, double depth)
{
  if (DepthTools::addBookmark(&m_bookmarks, name, depth) < 0)
    return false;
  if (m_store)
  {
    m_store->setBookmarks(m_bookmarks);
    m_store->save();
  }
  return true;
}

void WellCompositePanel::setBookmarks(const QList<DepthBookmark> &bms)
{
  m_bookmarks = bms;
  if (m_store)
  {
    m_store->setBookmarks(m_bookmarks);
    m_store->save();
  }
}

bool WellCompositePanel::jumpToBookmark(const QString &name)
{
  for (const auto &bm : m_bookmarks)
    if (bm.name == name)
    {
      m_canvas->setScrollDepth(bm.depth);
      return true;
    }
  return false;
}

void WellCompositePanel::setDepthUnitFeet(bool feet)
{
  if (m_depthFeet == feet)
    return;
  m_depthFeet = feet;
  m_canvas->setDepthUnitLabel(feet ? QStringLiteral("ft") : QString());
  m_lblReadout->setText(QStringLiteral("— ") + (feet ? QStringLiteral("ft") : QStringLiteral("m")));
}

void WellCompositePanel::setGapThresholdMeters(double meters)
{
  m_gapThresholdM = meters;
  m_canvas->setGapThresholdMeters(meters);
}

// ----------------------------------------------------------------------------
// D1 深度装配链（wave/deepen-perf）
// ----------------------------------------------------------------------------
void WellCompositePanel::applyDepthTables(const QVector<DeviationStation> &stations,
                                          const QVector<QPair<double, double>> &tvdTwtPairs,
                                          double kbElevation)
{
  if (!stations.isEmpty())
    m_depthTransform.setDeviationSurvey(stations);
  if (!tvdTwtPairs.isEmpty())
    m_depthTransform.setTimeDepthTable(tvdTwtPairs);
  if (!std::isnan(kbElevation))
    m_depthTransform.setKbElevation(kbElevation);
  refreshTwtLabels();
  // 刷新读数条（悬停深度沿用当前值；无悬停时下次移动自然带出）
  m_lblStatus->setText(
      tr("深度装配：井斜表%1、时深表%2（TVD/TWT 副刻度已%3）")
          .arg(hasDeviationSurvey() ? tr("已加载") : tr("缺失"))
          .arg(hasTimeDepthTable() ? tr("已加载") : tr("缺失"))
          .arg(hasTimeDepthTable() ? tr("启用") : tr("禁用")));
}

void WellCompositePanel::clearDepthTables()
{
  m_depthTransform = DepthTransform{};
  refreshTwtLabels();
}

void WellCompositePanel::refreshTwtLabels()
{
  QVector<QPair<double, QString>> labels;
  if (m_depthTransform.hasTimeDepthTable())
  {
    // 副刻度步长：深度跨度/12 取整到 1-2-5 序列（与标尺主刻度密度同量级）
    const double span = qMax(1.0, m_canvas->maxDepth() - m_canvas->minDepth());
    double step = span / 12.0;
    double mag = std::pow(10.0, std::floor(std::log10(step)));
    double norm = step / mag;
    step = (norm <= 1.0 ? 1.0 : norm <= 2.0 ? 2.0 : norm <= 5.0 ? 5.0 : 10.0) * mag;
    for (double md = std::ceil(m_canvas->minDepth() / step) * step;
         md <= m_canvas->maxDepth(); md += step)
    {
      const double twt = twtAtDepth(md);
      if (!std::isnan(twt) && twt > 0.0)
        labels.append({md, QString::number(qRound(twt))});
    }
  }
  m_canvas->setTwtLabels(labels);
}

QString WellCompositePanel::depthReadoutSuffix(double md) const
{
  if (!hasDeviationSurvey() && !hasTimeDepthTable())
    return QString();
  QString suffix;
  if (hasDeviationSurvey())
    suffix += QStringLiteral(" | TVD %1").arg(QString::number(mdToTvd(md), 'f', 1));
  if (hasTimeDepthTable())
  {
    const double twt = twtAtDepth(md);
    if (!std::isnan(twt))
      suffix += QStringLiteral(" | %1 ms").arg(QString::number(twt, 'f', 0));
  }
  return suffix;
}

// ----------------------------------------------------------------------------
// D3.x 编辑模式
// ----------------------------------------------------------------------------
void WellCompositePanel::setEditMode(bool on)
{
  if (m_editSession && m_editSession->isReadOnly() && on)
  {
    // D3.15 只读降级：拒绝进入并给出原因
    m_btnEdit->blockSignals(true);
    m_btnEdit->setChecked(false);
    m_btnEdit->blockSignals(false);
    QMessageBox::information(this, tr("不可编辑"),
                             m_editSession->readOnlyReason().isEmpty()
                                 ? tr("当前资产为只读（RAW 或未授权路径）。")
                                 : m_editSession->readOnlyReason());
    return;
  }
  m_canvas->setEditMode(on);
}

bool WellCompositePanel::editMode() const
{
  return m_canvas->editMode();
}

bool WellCompositePanel::saveDerived()
{
  if (!m_editSession || !m_editSession->isDirty())
    return false;

  // D3.3 派生文档 + manifest 风格摘要（落盘由壳接 derivedDocumentReady；测试
  // 直接断言信号携带的文档内容与审计摘要）
  const ComprehensiveWellData derived = m_editSession->buildDerivedDocument();
  const QString summary = m_editSession->buildManifestStyleSummary();
  // D1（wave/deepen-perf）：审计逐条随行——壳侧登记写「编辑审计」工作表
  emit derivedDocumentReady(derived, summary, m_editSession->auditLines());

  // D3.11 审计摘要同步 sidecar（auditLog 在各编辑操作时已逐条累积）
  if (m_store)
    m_store->save();
  m_editSession->markSaved();
  return true;
}

void WellCompositePanel::setHighContrast(bool on)
{
  m_highContrast = on;
  m_canvas->setHighContrast(on);
}

void WellCompositePanel::onMarkerMoved(const QString &name, double newDepth)
{
  if (!m_editSession)
    return;

  // D3.10 源冲突检测：编辑时源文件被动过 → 警告（重载/分叉由用户选）
  if (!m_sourceDataPath.isEmpty())
  {
    const qint64 cur = QFileInfo(m_sourceDataPath).lastModified().toMSecsSinceEpoch();
    if (m_editSession->sourceChanged(cur))
      m_editSession->checkSourceConflict(cur); // 内部发 sourceConflictDetected
  }

  m_editSession->moveMarker(name, newDepth); // undo 栈 + 审计；documentChanged → 重同步
}

// ----------------------------------------------------------------------------
// sidecar
// ----------------------------------------------------------------------------
void WellCompositePanel::loadSidecar()
{
  if (!m_store)
    return;

  m_canvas->setPins(m_store->pins());
  m_bookmarks = m_store->bookmarks();

  // D3.12 曲线量程/单位覆盖层应用（源 LAS 不动）
  const QVariantMap ov = m_store->curveOverrides();
  if (!ov.isEmpty())
  {
    for (const auto &t : m_canvas->tracks())
    {
      if (auto ct = std::dynamic_pointer_cast<CurveTrack>(t))
      {
        QVector<CurveData> curves = ct->curves();
        for (auto &c : curves)
        {
          const QVariantMap o = ov.value(c.name).toMap();
          if (!o.isEmpty())
            c = applyCurveOverride(c, o);
        }
        ct->setCurves(curves);
      }
    }
  }

  // D3.6 地层指派应用（stratigraphyIntervals 补齐未识别层名的系/统）
  const auto assigns = m_store->stratAssignments();
  if (!assigns.isEmpty() && m_editSession)
  {
    for (const auto &t : m_canvas->tracks())
    {
      if (auto sc = std::dynamic_pointer_cast<StratigraphyCompoundTrack>(t))
        sc->setIntervals(StratAssign::applyAssignments(sc->intervals(), assigns));
    }
  }
  m_canvas->updateAll();
}

void WellCompositePanel::saveSidecar() const
{
  if (!m_store)
    return;
  m_store->setPins(m_canvas->pins());
  m_store->setBookmarks(m_bookmarks);
  m_store->save();
}

void WellCompositePanel::applyStratAssignments(QVector<FormationInterval> *formations) const
{
  // 遗留接口兼容（指派直接作用于 stratigraphyIntervals 展示层，不污染源数据）
  Q_UNUSED(formations);
}

// ----------------------------------------------------------------------------
// 导出（D4.x；引擎实现见 exportengine）
// ----------------------------------------------------------------------------
void WellCompositePanel::exportCurrent(ExportEngine::Format format)
{
  QString filter;
  QString ext;
  switch (format)
  {
  case ExportEngine::Format::Png: filter = tr("PNG 图像 (*.png)"); ext = QStringLiteral("png"); break;
  case ExportEngine::Format::Svg: filter = tr("SVG 矢量 (*.svg)"); ext = QStringLiteral("svg"); break;
  default: filter = tr("PDF 文档 (*.pdf)"); ext = QStringLiteral("pdf"); break;
  }

  const QString suggested = QStringLiteral("%1_综合柱状图.%2").arg(m_wellName.isEmpty() ? QStringLiteral("well") : m_wellName, ext);
  QString path = QFileDialog::getSaveFileName(this, tr("导出综合柱状图"), suggested, filter);
  if (path.isEmpty())
    return;

  ExportEngine::Options opt;
  opt.scaleRatio = m_canvas->scaleRatio();
  opt.topDepth = m_canvas->minDepth();
  opt.bottomDepth = m_canvas->maxDepth();
  opt.includeHeader = true;
  opt.includeLegend = true;
  opt.dpi = 300;
  opt.wellName = m_wellName;
  opt.projectName = m_projectName;

  const QString err = ExportEngine::exportCanvas(*m_canvas, m_data, format, path, opt);
  if (!err.isEmpty())
    QMessageBox::warning(this, tr("导出失败"), err);
  else
    m_lblStatus->setText(tr("已导出: %1").arg(path));
}

void WellCompositePanel::printCurrent()
{
  // D3（wave/deepen-perf）原生打印接线：有系统打印机 → QPrintDialog + QPrinter
  //（与 PDF 导出同一分页渲染管线 exportToPagedDevice；取消 = 用户意图静默返回）。
  // 无打印环境（offscreen/无打印服务）→ 降级为 PDF 导出（QPdfWriter 即打印
  // 数据流，wave/wellcomposite D4.8 决策语义保持）。
  if (ExportEngine::nativePrintAvailable())
  {
    QPrinter printer(QPrinter::HighResolution);
    printer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait,
                                      QMarginsF(12, 14, 12, 14), QPageLayout::Millimeter));
    printer.setDocName(QStringLiteral("%1_综合柱状图").arg(
        m_wellName.isEmpty() ? QStringLiteral("well") : m_wellName));
    QPrintDialog dlg(&printer, this);
    dlg.setWindowTitle(tr("打印综合柱状图"));
    if (dlg.exec() != QDialog::Accepted)
      return;

    ExportEngine::Options opt;
    opt.scaleRatio = m_canvas->scaleRatio();
    opt.topDepth = m_canvas->minDepth();
    opt.bottomDepth = m_canvas->maxDepth();
    opt.includeHeader = true;
    opt.includeLegend = true;
    opt.wellName = m_wellName;
    opt.projectName = m_projectName;
    const QString err = ExportEngine::exportToPagedDevice(*m_canvas, m_data, printer, opt);
    if (!err.isEmpty())
      QMessageBox::warning(this, tr("打印失败"), err);
    else
      m_lblStatus->setText(tr("已发送到打印机: %1").arg(printer.printerName()));
    return;
  }
  m_lblStatus->setText(tr("未检测到系统打印机——打印降级为导出 PDF"));
  exportCurrent(ExportEngine::Format::Pdf);
}

void WellCompositePanel::manageExportPresets()
{
  if (!m_store)
  {
    QMessageBox::information(this, tr("导出预设"), tr("加载井数据后可用（预设按源数据 sidecar 保存）。"));
    return;
  }

  QStringList rows;
  const auto presets = m_store->exportPresets();
  for (const auto &p : presets)
    rows << QStringLiteral("%1 [%2 %3dpi]").arg(p.name, p.format, QString::number(p.dpi));
  QMessageBox::information(this, tr("导出预设"),
                           rows.isEmpty() ? tr("暂无预设。导出一次后可经 sidecar 保存。")
                                          : rows.join(QLatin1Char('\n')));
}

} // namespace WellComposite
