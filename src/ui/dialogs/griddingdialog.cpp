// 层：视图
#include "griddingdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include "ui/notifications/notificationmanager.h"
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace
{

// 与 io/horizonbinner + PaleoAlgoGuards 同口径的像元预算（审计 #33）。
constexpr double kMaxCellCount = 100'000'000;

// 规模反馈文案；超预算 → 非空 error。
QString dimsFeedback(double minX, double maxX, double minY, double maxY, double cellSize,
                     bool *overBudget)
{
  *overBudget = false;
  if (!(cellSize > 0.0) || maxX <= minX || maxY <= minY)
    return QObject::tr("网格尺寸无效");
  const double cols = std::ceil((maxX - minX) / cellSize);
  const double rows = std::ceil((maxY - minY) / cellSize);
  const double cells = cols * rows;
  if (cells > kMaxCellCount)
  {
    *overBudget = true;
    return QObject::tr("输出网格 %1×%2（%3 像元）超出 %4 像元预算——增大网格尺寸或缩小范围")
        .arg(qint64(cols))
        .arg(qint64(rows))
        .arg(cells, 0, 'g', 12)
        .arg(kMaxCellCount);
  }
  return QObject::tr("输出网格 %1×%2（%3 像元）")
      .arg(qint64(cols))
      .arg(qint64(rows))
      .arg(cells, 0, 'g', 12);
}

} // namespace

namespace PaleoGriddingDialog
{

bool prompt(QWidget *parent, const RequestContext &ctx, Request *out)
{
  QDialog dlg(parent);
  dlg.setObjectName(QStringLiteral("griddingDialog"));
  dlg.setWindowTitle(QObject::tr("网格化 %1（最小曲率）").arg(ctx.horizonName));
  dlg.setModal(true);

  auto *cellSpin = new QDoubleSpinBox(&dlg);
  cellSpin->setObjectName(QStringLiteral("griddingCellSizeSpin"));
  cellSpin->setDecimals(1);
  cellSpin->setRange(1.0, 100000.0);
  cellSpin->setValue(ctx.hasHeaderCell && ctx.headerCellSize > 0 ? ctx.headerCellSize : 25.0);
  cellSpin->setSuffix(QObject::tr(" m"));
  cellSpin->setToolTip(QObject::tr("输出栅格像元尺寸（平面单位）"));

  auto *tensionSpin = new QDoubleSpinBox(&dlg);
  tensionSpin->setObjectName(QStringLiteral("griddingTensionSpin"));
  tensionSpin->setDecimals(2);
  tensionSpin->setRange(0.0, 0.99);
  tensionSpin->setSingleStep(0.05);
  tensionSpin->setValue(0.25);
  tensionSpin->setToolTip(
      QObject::tr("连续曲率样条张力：0=最小曲率（平面/二次面精确），大值抑制过冲"));

  auto *sweepsSpin = new QSpinBox(&dlg);
  sweepsSpin->setObjectName(QStringLiteral("griddingMaxSweepsSpin"));
  sweepsSpin->setRange(50, 20000);
  sweepsSpin->setSingleStep(50);
  sweepsSpin->setValue(500);
  sweepsSpin->setToolTip(QObject::tr("多级级联每级迭代上限（累计进元数据）"));

  auto *barrierCheck = new QCheckBox(QObject::tr("约束库 break_line 作硬屏障"), &dlg);
  barrierCheck->setObjectName(QStringLiteral("griddingBarrierCheck"));
  barrierCheck->setEnabled(ctx.hasConstraints);
  barrierCheck->setChecked(ctx.hasConstraints);
  barrierCheck->setToolTip(
      QObject::tr("断层/约束线两侧各自插值（无约束线时无效果）"));

  auto *cvCheck = new QCheckBox(QObject::tr("留一法交叉验证（QC）"), &dlg);
  cvCheck->setObjectName(QStringLiteral("griddingCvCheck"));
  cvCheck->setChecked(false);
  auto *cvPointsSpin = new QSpinBox(&dlg);
  cvPointsSpin->setObjectName(QStringLiteral("griddingCvPointsSpin"));
  cvPointsSpin->setRange(4, 64);
  cvPointsSpin->setValue(16);
  cvPointsSpin->setEnabled(false);
  QObject::connect(cvCheck, &QCheckBox::toggled, cvPointsSpin, &QSpinBox::setEnabled);

  auto *dimsLabel = new QLabel(&dlg);
  dimsLabel->setObjectName(QStringLiteral("griddingDimsLabel"));
  dimsLabel->setWordWrap(true);
  // 超预算 → 确认禁用（语义状态走禁用态，不引装饰色——DESIGN.md）。
  const auto updateDims = [&]()
  {
    bool over = false;
    const QString text = dimsFeedback(ctx.minX, ctx.maxX, ctx.minY, ctx.maxY,
                                      cellSpin->value(), &over);
    dimsLabel->setText(text);
    for (QPushButton *b : dlg.findChildren<QPushButton *>())
      if (b->objectName() == QLatin1String("griddingOkButton"))
        b->setEnabled(!over);
  };
  QObject::connect(cellSpin, &QDoubleSpinBox::valueChanged, &dlg, updateDims);

  auto *form = new QFormLayout;
  form->addRow(QObject::tr("网格尺寸"), cellSpin);
  form->addRow(QObject::tr("张力"), tensionSpin);
  form->addRow(QObject::tr("迭代上限"), sweepsSpin);
  form->addRow(QString(), barrierCheck);
  form->addRow(QString(), cvCheck);
  form->addRow(QObject::tr("CV 折数"), cvPointsSpin);
  form->addRow(QObject::tr("输出规模"), dimsLabel);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                       Qt::Horizontal, &dlg);
  buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("griddingOkButton"));
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

  auto *lay = new QVBoxLayout(&dlg);
  lay->addLayout(form);
  lay->addWidget(buttons);
  dlg.setLayout(lay);
  updateDims();

  if (dlg.exec() != QDialog::Accepted)
    return false;
  if (out)
  {
    out->cellSize = cellSpin->value();
    out->tension = tensionSpin->value();
    out->maxSweeps = sweepsSpin->value();
    out->useBarriers = barrierCheck->isEnabled() && barrierCheck->isChecked();
    out->runCrossValidation = cvCheck->isChecked();
    out->cvPoints = cvPointsSpin->value();
  }
  return true;
}

QString safeAssetLabel(const QString &displayName)
{
  QString out;
  out.reserve(displayName.size());
  for (const QChar &c : displayName)
  {
    const bool safe = (c.isLetterOrNumber() && c.toLatin1() != 0) || c == QLatin1Char('_') ||
                      c == QLatin1Char('.') || c == QLatin1Char('-');
    out.append(safe ? c : QLatin1Char('_'));
  }
  if (out.isEmpty())
    out = QStringLiteral("surface");
  return out;
}

bool promptIsopach(QWidget *parent, const QVector<QPair<QString, QString>> &candidates,
                   IsopachSelection *out)
{
  if (candidates.size() < 2)
  {
    paleo::ui::NotificationManager::showInfo(parent, QObject::tr("面运算"),
                             QObject::tr("需要至少两个已声明的栅格图层（顶/底结构面）。"));
    return false;
  }
  QDialog dlg(parent);
  dlg.setObjectName(QStringLiteral("isopachDialog"));
  dlg.setWindowTitle(QObject::tr("等厚 / 体积（面运算）"));
  dlg.setModal(true);

  auto *topCombo = new QComboBox(&dlg);
  topCombo->setObjectName(QStringLiteral("isopachTopCombo"));
  auto *baseCombo = new QComboBox(&dlg);
  baseCombo->setObjectName(QStringLiteral("isopachBaseCombo"));
  for (const auto &c : candidates)
  {
    topCombo->addItem(c.first);
    baseCombo->addItem(c.first);
  }
  topCombo->setCurrentIndex(0);
  baseCombo->setCurrentIndex(1);
  baseCombo->setToolTip(QObject::tr("底面（较老层位）；厚度 = 顶 − 底，带符号"));

  auto *writeCheck = new QCheckBox(QObject::tr("写出受管等厚栅格并上图"), &dlg);
  writeCheck->setObjectName(QStringLiteral("isopachWriteCheck"));
  writeCheck->setChecked(false);

  auto *form = new QFormLayout;
  form->addRow(QObject::tr("顶面（上层位）"), topCombo);
  form->addRow(QObject::tr("底面（下层位）"), baseCombo);
  form->addRow(QString(), writeCheck);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                       Qt::Horizontal, &dlg);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  auto *lay = new QVBoxLayout(&dlg);
  lay->addLayout(form);
  lay->addWidget(buttons);
  dlg.setLayout(lay);

  if (dlg.exec() != QDialog::Accepted)
    return false;
  if (topCombo->currentIndex() == baseCombo->currentIndex())
  {
    paleo::ui::NotificationManager::showWarning(parent, QObject::tr("面运算"), QObject::tr("顶/底不能是同一图层。"));
    return false;
  }
  if (out)
  {
    out->topIndex = topCombo->currentIndex();
    out->baseIndex = baseCombo->currentIndex();
    out->writeManaged = writeCheck->isChecked();
  }
  return true;
}

QString showVolumeReport(QWidget *parent, const QString &title,
                         const QVector<QPair<QString, QString>> &metrics, const QString &csv)
{
  QDialog dlg(parent);
  dlg.setObjectName(QStringLiteral("volumeReportDialog"));
  dlg.setWindowTitle(title);
  dlg.setModal(true);
  dlg.resize(420, 360);

  auto *text = new QPlainTextEdit(&dlg);
  text->setObjectName(QStringLiteral("volumeReportText"));
  text->setReadOnly(true);
  QString body;
  for (const auto &m : metrics)
    body += QStringLiteral("%1：%2\n").arg(m.first, m.second);
  text->setPlainText(body);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, &dlg);
  auto *exportBtn = buttons->addButton(QObject::tr("导出 CSV…"), QDialogButtonBox::ActionRole);
  exportBtn->setObjectName(QStringLiteral("volumeReportExportButton"));
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::close);
  QObject::connect(exportBtn, &QPushButton::clicked, &dlg, [&]()
  {
    const QString path = QFileDialog::getSaveFileName(
        parent, QObject::tr("导出体积报告"), QString(), QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty())
      return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text) ||
        f.write(csv.toUtf8()) < 0)
    {
      paleo::ui::NotificationManager::showWarning(parent, QObject::tr("导出失败"),
                           QObject::tr("无法写入文件：%1").arg(path));
      return;
    }
    f.close();
    paleo::ui::NotificationManager::showInfo(parent, QObject::tr("导出完成"), path);
  });

  auto *lay = new QVBoxLayout(&dlg);
  lay->addWidget(text);
  lay->addWidget(buttons);
  dlg.setLayout(lay);
  dlg.exec();
  return QString();
}

} // namespace PaleoGriddingDialog
