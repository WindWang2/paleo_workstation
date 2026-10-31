// 层：视图
// 地震相掩膜：Slice vs Interval 语义条 + 切片投票→层段综合发布入口（自 PredictPage 拆出控规模）。
#include "predictpage.h"
#include "../windowsizing_internal.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../paleoicons.h"

#include <QCheckBox>
#include <QIcon>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;

void PredictPage::setupSeismicMaskChrome(QVBoxLayout *lay)
{
  auto *box = new QWidget(this);
  box->setObjectName(QStringLiteral("seismicMaskChrome"));
  auto *v = new QVBoxLayout(box);
  v->setContentsMargins(0, PaleoTheme::tokens().spacingSm, 0, 0);
  v->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *scopeCap = caption(tr("地震相范围"), box);
  scopeCap->setObjectName(QStringLiteral("seismicScopeCaption"));
  v->addWidget(scopeCap);

  auto *scope = new QComboBox(box);
  scope->setObjectName(QStringLiteral("seismicScopeCombo"));
  scope->setAccessibleName(tr("地震相范围"));
  scope->addItem(PaleoIcons::qgisTheme(QStringLiteral("mIconRasterLayer.svg")),
                 tr("时间切片（Slice）"), QStringLiteral("slice"));
  scope->addItem(PaleoIcons::qgisTheme(QStringLiteral("processingAlgorithm.svg")),
                 tr("层段综合（Interval）"), QStringLiteral("interval"));
  scope->setToolTip(tr("Slice=单一切片预测，不得冒充整段；Interval=子层段掩膜内可验证聚合（weighted_mode）。"));
  v->addWidget(scope);

  auto *covCap = caption(tr("层段综合最低覆盖率"), box);
  covCap->setObjectName(QStringLiteral("seismicMinCoverageCaption"));
  v->addWidget(covCap);
  auto *cov = new QDoubleSpinBox(box);
  cov->setObjectName(QStringLiteral("seismicMinCoverageSpin"));
  cov->setAccessibleName(tr("层段综合最低覆盖率"));
  cov->setRange(0.1, 1.0);
  cov->setSingleStep(0.05);
  cov->setDecimals(2);
  cov->setValue(0.50);
  cov->setToolTip(tr("Interval 专用：有效权重 / 总权重低于此值则拒绝发布层段综合。"));
  v->addWidget(cov);

  auto *votesCap = caption(tr("切片投票（每行：相码,权重）"), box);
  votesCap->setObjectName(QStringLiteral("seismicVotesCaption"));
  v->addWidget(votesCap);
  auto *votes = new QPlainTextEdit(box);
  votes->setObjectName(QStringLiteral("seismicVotesEdit"));
  votes->setAccessibleName(tr("切片投票列表"));
  votes->setPlaceholderText(tr("例如：\n3,10\n3,5\n2,4"));
  votes->setMaximumHeight(paleo::ui_detail::lineRows(votes, 5)); // R6 #845：行高口径
  votes->setToolTip(tr("掩膜内各时间切片的相分类码与有效样点权重；用于切片→层段聚合发布。"));
  v->addWidget(votes);

  auto *also = new QCheckBox(tr("同时登记各切片（Slice）绑定"), box);
  also->setObjectName(QStringLiteral("seismicAlsoBindSlices"));
  also->setToolTip(tr("除 Interval 综合外，为每个有效投票再写一条 Slice 绑定（语义分列）。"));
  v->addWidget(also);

  auto *pub = new QPushButton(tr("发布层段综合到 Data Service"), box);
  pub->setObjectName(QStringLiteral("btnPublishSeismicAggregate"));
  pub->setAccessibleName(tr("发布层段综合到 Data Service"));
  pub->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionFileSave.svg")));
  pub->setToolTip(tr("按当前画布分析层段（UnitSelection）将切片投票聚合并写入 DS；"
                     "需选择 Interval 范围。Slice 范围请用运行预测登记切片成果。"));
  v->addWidget(pub);
  connect(pub, &QPushButton::clicked, this, [this] {
    QVariantMap params;
    collectSeismicMaskParams(&params);
    if (params.value(QStringLiteral("seismicScope")).toString() != QLatin1String("interval"))
    {
      if (auto *status = child<QLabel>(this, "statusLabel"))
        status->setText(tr("层段综合发布需要将范围设为「层段综合（Interval）」"));
      return;
    }
    emit seismicMaskPublishRequested(params);
  });
  connect(scope, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int) { syncSeismicMaskChrome(); });

  lay->addWidget(box);
  syncSeismicMaskChrome();
}

void PredictPage::syncSeismicMaskChrome()
{
  auto *box = child<QWidget>(this, "seismicMaskChrome");
  auto *types = child<QComboBox>(this, "predictTypeCombo");
  auto *scope = child<QComboBox>(this, "seismicScopeCombo");
  if (!box || !types)
    return;
  const bool seismic = types->currentData().toString() == QLatin1String("seismic_facies");
  box->setVisible(seismic);
  if (!seismic || !scope)
    return;
  const bool interval = scope->currentData().toString() == QLatin1String("interval");
  if (auto *cov = child<QDoubleSpinBox>(this, "seismicMinCoverageSpin"))
    cov->setEnabled(interval);
  if (auto *cap = child<QLabel>(this, "seismicMinCoverageCaption"))
    cap->setEnabled(interval);
  if (auto *votes = child<QPlainTextEdit>(this, "seismicVotesEdit"))
    votes->setEnabled(interval);
  if (auto *also = child<QCheckBox>(this, "seismicAlsoBindSlices"))
    also->setEnabled(interval);
  if (auto *pub = child<QPushButton>(this, "btnPublishSeismicAggregate"))
    pub->setEnabled(interval);
}

void PredictPage::collectSeismicMaskParams(QVariantMap *params)
{
  if (!params)
    return;
  if (auto *types = child<QComboBox>(this, "predictTypeCombo"))
  {
    if (types->currentData().toString() != QLatin1String("seismic_facies"))
      return;
  }
  if (auto *scope = child<QComboBox>(this, "seismicScopeCombo"))
    params->insert(QStringLiteral("seismicScope"), scope->currentData());
  if (auto *cov = child<QDoubleSpinBox>(this, "seismicMinCoverageSpin"))
    params->insert(QStringLiteral("seismicMinCoverage"), cov->value());
  if (auto *also = child<QCheckBox>(this, "seismicAlsoBindSlices"))
    params->insert(QStringLiteral("seismicAlsoBindSlices"), also->isChecked());
  if (auto *votes = child<QPlainTextEdit>(this, "seismicVotesEdit"))
  {
    QVariantList list;
    const QStringList lines = votes->toPlainText().split(QChar('\n'), Qt::SkipEmptyParts);
    for (QString line : lines)
    {
      line = line.trimmed();
      if (line.isEmpty() || line.startsWith(QChar('#')))
        continue;
      const QStringList parts = line.split(QChar(','), Qt::SkipEmptyParts);
      if (parts.size() < 1)
        continue;
      QVariantMap vote;
      vote.insert(QStringLiteral("classCode"), parts.at(0).trimmed().toInt());
      vote.insert(QStringLiteral("weight"), parts.size() >= 2 ? parts.at(1).trimmed().toDouble() : 1.0);
      list.append(vote);
    }
    params->insert(QStringLiteral("seismicSliceVotes"), list);
  }
}
