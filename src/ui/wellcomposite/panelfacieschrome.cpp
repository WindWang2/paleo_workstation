// 层：视图
// 测井相工具条 + 层段分析 UnitSelection 条（自 panelsetup::setupUi 拆出，控规模）。
#include "wellcompositepanel.h"

#include "../../linkage/selectioncontext.h"
#include "../../workflow/wellfaciesworkflow.h"
#include "../paleotheme.h"

#include <QBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <functional>

namespace WellComposite {
namespace {
SelectionContext *s_selectionProvider = nullptr;
}

void WellCompositePanel::setSelectionContextProvider(SelectionContext *selection)
{
  s_selectionProvider = selection;
}

void WellCompositePanel::setupFaciesChrome(QWidget *topBar, QBoxLayout *topLay,
                                          const std::function<QString()> &themedBtnStyle)
{
  // 层段分析：跟随画布 SubIntervalSelectorBar / SelectionContext::activeUnit。
  m_analysisUnitLabel = new QLabel(topBar);
  m_analysisUnitLabel->setObjectName(QStringLiteral("lblAnalysisUnit"));
  m_analysisUnitLabel->setAccessibleName(tr("当前分析层段"));
  m_analysisUnitLabel->setText(tr("分析层段：未选"));
  m_analysisUnitLabel->setToolTip(
      tr("测井相按子层段深度窗提交；请用画布上方父单元→S1–S4 选择条切换。"
         "此处只读显示，不改地图编辑层位。"));
  m_analysisUnitLabel->setWhatsThis(
      tr("<b>分析层段</b><p>测井相预测归属稳定 SubIntervalId。"
         "切换请用画布层段分析条；本标签只反映 SelectionContext::activeUnit。</p>"));
  topLay->addWidget(m_analysisUnitLabel);
  bindSelectionContext(s_selectionProvider);

  m_faciesModel = new QComboBox(topBar);
  m_faciesModel->setObjectName(QStringLiteral("faciesPredictionModel"));
  m_faciesModel->setAccessibleName(tr("测井相预测模型"));
  m_faciesModel->setMinimumContentsLength(12);
  m_faciesModel->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_faciesModel->setToolTip(tr("网络测井相模型版本；层段身份由上方分析层段决定，不在此下拉选择界面名。"));
  m_faciesModel->setWhatsThis(
      tr("<b>测井相模型</b><p>选择服务端模型版本。"
         "预测深度窗与成果身份跟随分析层段（UnitSelection），"
         "勿把本下拉当作层位选择。</p>"));
  topLay->addWidget(m_faciesModel);
  const auto faciesButton = [&](const QString &name, const QString &text) {
    auto *button = new QToolButton(topBar);
    button->setObjectName(name);
    button->setText(text);
    button->setAccessibleName(text);
    PaleoTheme::applyThemedStyleSheet(button, themedBtnStyle);
    topLay->addWidget(button);
    return button;
  };
  m_btnPredictFacies = faciesButton(QStringLiteral("btnPredictFacies"), tr("预测相"));
  m_btnPredictFacies->setToolTip(tr("测井相预测服务尚未连接"));
  m_btnPredictFacies->setWhatsThis(
      tr("<b>预测相</b><p>按当前分析层段深度窗提交测井相预测；"
         "成果带 SubIntervalId，可供编图证据拉取。</p>"));
  m_btnCancelFacies = faciesButton(QStringLiteral("btnCancelFacies"), tr("停止等待"));
  m_btnCancelFacies->setEnabled(false);
  m_btnCancelFacies->setToolTip(tr("没有正在等待的预测"));
  m_btnShowFacies = faciesButton(QStringLiteral("btnShowPredictedFacies"), tr("显示预测相"));
  m_btnShowFacies->setCheckable(true);
  m_btnShowFacies->setChecked(true);
  m_btnShowFacies->setEnabled(false);
  m_btnShowFacies->setToolTip(tr("先运行测井相预测"));
  m_btnRefreshFacies = faciesButton(QStringLiteral("btnRefreshFaciesModels"), tr("刷新模型"));
  m_btnRefreshFacies->setToolTip(tr("获取网络服务当前可调用的模型及输入要求"));
  m_btnFaciesService = faciesButton(QStringLiteral("btnFaciesService"), tr("预测服务"));
  m_btnFaciesService->setToolTip(tr("设置测井相预测服务地址与 API 密钥"));
  connect(m_btnPredictFacies, &QToolButton::clicked, this, [this] {
    if (!m_faciesWorkflow) {
      m_faciesStatus->setText(tr("测井相预测尚未接入"));
      return;
    }
    if (!m_faciesReady) {
      const QString reason = m_btnPredictFacies->toolTip();
      m_faciesStatus->setText(reason);
      if (reason.contains(QStringLiteral("API 密钥")) || reason.contains(QStringLiteral("服务地址"))
          || reason.contains(QStringLiteral("允许不加密")))
        m_btnFaciesService->click();
      return;
    }
    emit faciesPredictionRequested();
  });
  connect(m_btnCancelFacies, &QToolButton::clicked, this, &WellCompositePanel::faciesCancelRequested);
  connect(m_btnRefreshFacies, &QToolButton::clicked, this, &WellCompositePanel::faciesModelsRequested);
  connect(m_faciesModel, &QComboBox::currentIndexChanged, this, [this] {
    emit faciesModelSelected(m_faciesModel->currentData().toString());
  });
  connect(m_btnShowFacies, &QToolButton::toggled, this, [this](bool visible) {
    if (m_predictionTrack)
      m_predictionTrack->setVisible(visible);
    if (m_confidenceTrack)
      m_confidenceTrack->setVisible(visible);
    for (const auto &track : m_dominantTracks)
      track->setVisible(visible);
    m_canvas->syncScrollBars();
    m_canvas->updateAll();
  });
  connect(m_btnFaciesService, &QToolButton::clicked, this, [this] {
    if (!m_faciesWorkflow)
      return;
    const auto config = m_faciesWorkflow->config();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("测井相预测服务"));
    auto *form = new QFormLayout(&dialog);
    form->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd,
                             PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd);
    form->setSpacing(PaleoTheme::tokens().spacingSm);
    QLineEdit url(config.baseUrl.toString()), key(QString::fromUtf8(config.apiKey));
    url.setPlaceholderText(tr("https://服务地址"));
    key.setEchoMode(QLineEdit::Password);
    QCheckBox insecure(tr("允许不加密的 HTTP（API 密钥与井数据将明文传输）"));
    insecure.setChecked(config.allowInsecureHttp);
    form->addRow(tr("服务地址"), &url);
    form->addRow(tr("API 密钥"), &key);
    form->addRow(QString(), &insecure);
    QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
      emit faciesConfigurationRequested(url.text().trimmed(), key.text().trimmed(), insecure.isChecked());
  });
}

void WellCompositePanel::bindSelectionContext(SelectionContext *selection)
{
  if (!m_analysisUnitLabel)
    return;
  if (m_selectionContext) {
    disconnect(m_selectionContext, nullptr, this, nullptr);
    m_selectionContext = nullptr;
  }
  const auto apply = [this](const paleo::strat::UnitSelection &u) {
    if (!m_analysisUnitLabel)
      return;
    if (!u.isValid()) {
      m_analysisUnitLabel->setText(tr("分析层段：未选"));
      return;
    }
    m_analysisUnitLabel->setText(tr("分析层段：%1").arg(u.displayName));
  };
  m_selectionContext = selection;
  if (!selection) {
    apply({});
    return;
  }
  connect(selection, &SelectionContext::activeUnitChanged, this, apply);
  apply(selection->activeUnit());
}

} // namespace WellComposite
