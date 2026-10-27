// 层：视图
#include "provisionalregistrationdialog.h"

#include "../paleotheme.h"
#include "services/previewdoc.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>

bool PaleoProvisionalRegistration::prompt(QWidget *parent, PreviewDocService *document,
                                            const QString &path, QVariantMap *parameters,
                                            QString *error)
{
  if (error) error->clear();
  double sourceBounds[4];
  QString boundsError;
  if (!document || !document->geoJsonBounds(path, sourceBounds, &boundsError))
  {
    if (error) *error = QObject::tr("读不出坐标范围：%1").arg(boundsError);
    return false;
  }
  QDialog dialog(parent);
  dialog.setObjectName(QStringLiteral("provisionalRegistrationDialog"));
  dialog.setWindowTitle(QObject::tr("临时配准（手工仿射）"));
  auto *form = new QFormLayout(&dialog);
  auto *source = new QLabel(
      QObject::tr("源坐标范围：X %1–%2 · Y %3–%4")
          .arg(QString::number(sourceBounds[0], 'f', 2), QString::number(sourceBounds[2], 'f', 2),
               QString::number(sourceBounds[1], 'f', 2), QString::number(sourceBounds[3], 'f', 2)),
      &dialog);
  source->setWordWrap(true);
  form->addRow(source);
  auto *hint = new QLabel(
      QObject::tr("目标：工程局部测网（约 X 0–12800 · Y 0–16400，单位米）"), &dialog);
  hint->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(hint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  form->addRow(hint);
  auto *tx = new QDoubleSpinBox(&dialog);
  auto *ty = new QDoubleSpinBox(&dialog);
  auto *sx = new QDoubleSpinBox(&dialog);
  auto *sy = new QDoubleSpinBox(&dialog);
  auto *rotation = new QDoubleSpinBox(&dialog);
  for (auto *spin : {tx, ty})
  {
    spin->setRange(-1e9, 1e9);
    spin->setDecimals(2);
    spin->setSingleStep(1000.0);
  }
  for (auto *spin : {sx, sy})
  {
    spin->setRange(1e-6, 1e6);
    spin->setDecimals(6);
    spin->setValue(1.0);
  }
  rotation->setRange(-360.0, 360.0);
  rotation->setDecimals(2);
  const QList<QPair<QDoubleSpinBox *, QString>> fields{
      {tx, QStringLiteral("affineTranslateX")}, {ty, QStringLiteral("affineTranslateY")},
      {sx, QStringLiteral("affineScaleX")}, {sy, QStringLiteral("affineScaleY")},
      {rotation, QStringLiteral("affineRotation")}};
  for (const auto &field : fields)
  {
    field.first->setObjectName(field.second);
    field.first->setFont(PaleoTheme::monoFont());
  }
  tx->setSuffix(QObject::tr(" m"));
  ty->setSuffix(QObject::tr(" m"));
  rotation->setSuffix(QObject::tr(" °"));
  form->addRow(new QLabel(QObject::tr("坐标平移（目标工程局部米制网格）"), &dialog));
  form->addRow(QObject::tr("东向偏移 X"), tx);
  form->addRow(QObject::tr("北向偏移 Y"), ty);
  form->addRow(new QLabel(QObject::tr("仿射缩放与旋转"), &dialog));
  form->addRow(QObject::tr("缩放 X"), sx);
  form->addRow(QObject::tr("缩放 Y"), sy);
  form->addRow(QObject::tr("旋转"), rotation);
  auto *destination = new QLabel(&dialog);
  destination->setObjectName(QStringLiteral("affineDstBounds"));
  destination->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(destination, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  form->addRow(destination);
  const auto refresh = [document, sourceBounds, tx, ty, sx, sy, rotation, destination] {
    const QVariantMap values{{QStringLiteral("tx"), tx->value()}, {QStringLiteral("ty"), ty->value()},
                             {QStringLiteral("sx"), sx->value()}, {QStringLiteral("sy"), sy->value()},
                             {QStringLiteral("rotDeg"), rotation->value()}};
    double lo[2], hi[2];
    document->affinePreviewBounds(sourceBounds, values, lo, hi);
    destination->setText(QObject::tr("变换后范围：X %1–%2 · Y %3–%4")
        .arg(QString::number(lo[0], 'f', 1), QString::number(hi[0], 'f', 1),
             QString::number(lo[1], 'f', 1), QString::number(hi[1], 'f', 1)));
  };
  for (auto *spin : {tx, ty, sx, sy, rotation})
    QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), destination, refresh);
  refresh();
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("登记为临时配准"));
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return false;
  if (parameters)
    *parameters = {{QStringLiteral("tx"), tx->value()}, {QStringLiteral("ty"), ty->value()},
                   {QStringLiteral("sx"), sx->value()}, {QStringLiteral("sy"), sy->value()},
                   {QStringLiteral("rotDeg"), rotation->value()}};
  return true;
}
