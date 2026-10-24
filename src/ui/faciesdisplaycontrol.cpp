// 层：视图
#include "faciesdisplaycontrol.h"
#include "paleotheme.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

FaciesDisplayControl::FaciesDisplayControl(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(PaleoTheme::tokens().spacingXs);
  m_mode = new QComboBox(this);
  m_mode->setObjectName("ribbonFaciesDisplayMode");
  m_mode->setAccessibleName(tr("相图显示层级"));
  m_mode->addItem(tr("自动（随比例尺）"), "auto");
  m_mode->addItem(tr("固定相"), "facies");
  m_mode->addItem(tr("固定亚相"), "sub_facies");
  m_mode->addItem(tr("固定微相"), "micro_facies");
  m_mode->setEnabled(false);
  layout->addWidget(m_mode);
  m_state = new QLabel(tr("请显示相图件"), this);
  m_state->setObjectName("ribbonFaciesDisplayState");
  m_state->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  layout->addWidget(m_state);
  auto *settings = new QToolButton(this);
  settings->setObjectName("ribbonFaciesScaleSettings");
  settings->setText(tr("自动切换设置…"));
  settings->setToolTip(tr("设置相、亚相、微相自动切换的比例尺，保存后立即应用并在下次启动恢复"));
  settings->setWhatsThis(settings->toolTip());
  settings->setAccessibleName(settings->text());
  settings->setFocusPolicy(Qt::StrongFocus);
  layout->addWidget(settings);
  connect(settings, &QToolButton::clicked, this, &FaciesDisplayControl::editThresholds);
  connect(m_mode, &QComboBox::activated, this, [this] {
    if (!m_layerId.isEmpty()) emit displayModeRequested(m_layerId, m_mode->currentData().toString());
  });
}
void FaciesDisplayControl::setState(const QString &id, const QString &title, const QString &mode,
    const QString &level, const FaciesHierarchy::ScaleThresholds &thresholds, int pinnedCount) {
  m_layerId = id; m_thresholds = thresholds;
  const QSignalBlocker blocked(m_mode);
  m_mode->setCurrentIndex(qMax(0, m_mode->findData(mode)));
  m_mode->setEnabled(!id.isEmpty());
  m_mode->setToolTip(id.isEmpty() ? tr("请显示并选择相图件；测井点保持最终预测相显示")
      : tr("作用于：%1；固定显示不随缩放切换，单独锁定的相面仍保留其锁定").arg(title));
  // 有相类固定时为混合显示：只附加提示，下拉选项与切换行为不变。
  const bool mixed = !id.isEmpty() && pinnedCount > 0;
  if (mixed)
    m_mode->setToolTip(m_mode->toolTip() + QLatin1Char('\n') +
                       tr("另有 %1 个相类固定显示层级，不随此处切换（在「相类别与标注」中管理）").arg(pinnedCount));
  m_mode->setWhatsThis(m_mode->toolTip());
  m_state->setText(id.isEmpty() ? tr("请显示相图件")
      : mixed ? tr("当前：%1（%2 个相类固定）").arg(FaciesHierarchy::title(level)).arg(pinnedCount)
              : tr("当前：%1").arg(FaciesHierarchy::title(level)));
  m_state->setProperty("paleo.pinnedCount", mixed ? pinnedCount : 0);
  m_state->setToolTip(title);
}
QString FaciesDisplayControl::scaleHint(const QString &level, const FaciesHierarchy::ScaleThresholds &thresholds) {
  return tr("当前显示：%1；自动阈值为 1:%2 与 1:%3，可在 ribbon 的「相图显示」中设置")
      .arg(FaciesHierarchy::title(level)).arg(thresholds.faciesToSubfacies, 0, 'f', 0)
      .arg(thresholds.subfaciesToMicrofacies, 0, 'f', 0);
}
void FaciesDisplayControl::editThresholds() {
  QDialog dialog(this);
  dialog.setObjectName("faciesScaleSettingsDialog");
  dialog.setWindowTitle(tr("自动切换比例尺设置"));
  auto *layout = new QVBoxLayout(&dialog);
  auto *form = new QFormLayout;
  const auto spin = [&dialog](const char *name, double value) {
    auto *box = new QSpinBox(&dialog); box->setObjectName(QLatin1String(name));
    box->setRange(1, 1000000000); box->setPrefix(QStringLiteral("1:"));
    box->setGroupSeparatorShown(true); box->setSingleStep(1000); box->setValue(int(qBound(1., value, 1000000000.)));
    return box;
  };
  auto *facies = spin("faciesScaleThreshold", m_thresholds.faciesToSubfacies);
  auto *micro = spin("microfaciesScaleThreshold", m_thresholds.subfaciesToMicrofacies);
  facies->setAccessibleName(tr("相与亚相的切换比例尺"));
  micro->setAccessibleName(tr("亚相与微相的切换比例尺"));
  form->addRow(tr("相／亚相界限"), facies);
  form->addRow(tr("亚相／微相界限"), micro);
  layout->addLayout(form);
  auto *hint = new QLabel(&dialog); hint->setWordWrap(true); layout->addWidget(hint);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
      QDialogButtonBox::RestoreDefaults, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("保存"));
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
  buttons->button(QDialogButtonBox::RestoreDefaults)->setText(tr("恢复默认"));
  layout->addWidget(buttons);
  const auto validate = [=] {
    const bool valid = facies->value() > micro->value();
    buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
    hint->setText(valid ? tr("缩小到超过 %1 显示相；大于 %2 且不超过 %1 显示亚相；放大到 %2 及以内显示微相。固定显示不受影响。")
        .arg(facies->text(), micro->text()) : tr("相／亚相界限的分母必须大于亚相／微相界限"));
  };
  connect(facies, &QSpinBox::valueChanged, &dialog, validate);
  connect(micro, &QSpinBox::valueChanged, &dialog, validate);
  connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [=] {
    const FaciesHierarchy::ScaleThresholds defaults;
    facies->setValue(int(defaults.faciesToSubfacies)); micro->setValue(int(defaults.subfaciesToMicrofacies));
  });
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  validate();
  if (dialog.exec() == QDialog::Accepted) emit thresholdsRequested(facies->value(), micro->value());
}
