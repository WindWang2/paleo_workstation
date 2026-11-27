// 层：视图
#include "mappingworkbenchpage.h"
#include "../../workflow/mappingworkbench.h"
#include "../paleotheme.h"
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

void MappingWorkbenchPage::buildClusterInputs(QWidget *body, QFormLayout *form) {
  m_clusterCount = new QSpinBox(body);
  m_clusterCount->setObjectName(QStringLiteral("seismicClusterCount"));
  m_clusterCount->setRange(1, 64);
  m_clusterCount->setValue(4);
  form->addRow(tr("聚类数 K"), m_clusterCount);
  connect(m_kind, &QComboBox::currentIndexChanged, this, [this, form] {
    form->setRowVisible(m_clusterCount, m_kind->currentData() == "seismic_mock");
  });
  m_horizonSourceRow = new QWidget(body);
  auto *horizonSource = new QHBoxLayout(m_horizonSourceRow);
  horizonSource->setContentsMargins(0, 0, 0, 0);
  horizonSource->setSpacing(PaleoTheme::tokens().spacingSm);
  horizonSource->addWidget(new QLabel(tr("层位文件"), m_horizonSourceRow));
  m_horizonFile = new QLineEdit(m_horizonSourceRow);
  m_horizonFile->setObjectName("workbenchHorizonFile");
  m_horizonFile->setAccessibleName(tr("当前层位原始文件"));
  m_horizonFile->setPlaceholderText(tr("自动使用已导入层位，或直接选择 DAT"));
  m_horizonFile->setToolTip(tr("按文件中的 Inline、Crossline、时间（ms）直接提取地震窗；无需预先生成时间栅格。"));
  horizonSource->addWidget(m_horizonFile, 1);
  m_chooseHorizonFile = new QPushButton(tr("选择…"), m_horizonSourceRow);
  m_chooseHorizonFile->setObjectName("workbenchChooseHorizonFile");
  m_chooseHorizonFile->setAccessibleName(tr("选择当前层位原始文件"));
  horizonSource->addWidget(m_chooseHorizonFile);
  form->addRow(m_horizonSourceRow);
  connect(m_chooseHorizonFile, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getOpenFileName(this,
        tr("选择 %1 的层位时间文件").arg(m_horizon), m_horizonFile->text(),
        tr("SMI 层位文件 (*.dat *.txt);;所有文件 (*)"));
    if (!path.isEmpty()) m_horizonFile->setText(path);
  });
  connect(m_workbench, &MappingWorkbench::catalogBound, m_horizonFile, &QLineEdit::clear);
}

void MappingWorkbenchPage::refreshClusterInputs(bool horizon, bool busy, const QString &readiness) {
  if (!m_horizonSourceRow) return;
  m_clusterCount->setEnabled(!busy);
  m_horizonSourceRow->setVisible(m_kind->currentData() == "seismic_mock"&&!m_workbench->analysisUnit().isValid());
  m_horizonFile->setEnabled(horizon && !busy);
  m_chooseHorizonFile->setEnabled(horizon && !busy);
  m_chooseHorizonFile->setToolTip(horizon && !busy
      ? tr("选择 %1 的原始层位时间文件，直接从地震体提取").arg(m_horizon)
      : readiness);
}
