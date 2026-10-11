// 层：视图
#include "faciesconsistencydialog.h"
#include "../paleotheme.h"
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

FaciesConsistencyDialog::FaciesConsistencyDialog(QWidget *parent) : QDialog(parent) {
  setObjectName("faciesConsistencyDialog");
  setWindowTitle(tr("地震相 × 测井相一致性报告"));
  setAttribute(Qt::WA_DeleteOnClose);
  auto *layout = new QVBoxLayout(this);
  const auto &t = PaleoTheme::tokens();
  layout->setContentsMargins(t.spacingMd, t.spacingMd, t.spacingMd, t.spacingMd);
  layout->setSpacing(t.spacingSm);
  m_summary = new QLabel(this);
  m_summary->setObjectName("consistencySummary");
  m_summary->setWordWrap(true);
  m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(m_summary);
  auto *tabs = new QTabWidget(this);
  m_wells = new QTableWidget(0, 5, tabs);
  m_wells->setObjectName("consistencyWells");
  m_wells->setHorizontalHeaderLabels(
      {tr("井"), tr("测井相"), tr("地震相"), tr("一致性"), tr("说明")});
  m_wells->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_wells->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_wells->setSelectionMode(QAbstractItemView::SingleSelection);
  m_wells->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  m_wells->setAccessibleName(tr("逐井一致性"));
  tabs->addTab(m_wells, tr("逐井"));
  m_matrix = new QTableWidget(0, 0, tabs);
  m_matrix->setObjectName("consistencyMatrix");
  m_matrix->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_matrix->setAccessibleName(tr("混淆矩阵：行为测井相，列为地震相"));
  tabs->addTab(m_matrix, tr("混淆矩阵（行：测井相 / 列：地震相）"));
  layout->addWidget(tabs, 1);
  m_path = new QLabel(this);
  m_path->setObjectName("consistencyReportPath");
  m_path->setWordWrap(true);
  m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
  PaleoTheme::applyThemedStyleSheet(m_path, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  layout->addWidget(m_path);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  layout->addWidget(buttons);
  connect(m_wells, &QTableWidget::cellDoubleClicked, this, [this](int row) {
    const auto rows = m_report.value("rows").toList();
    if (row < 0 || row >= rows.size())
      return;
    const auto r = rows.at(row).toMap();
    emit wellActivated(r.value("well_id").toString(), r.value("x").toDouble(),
                       r.value("y").toDouble());
  });
  resize(720, 520);
}

void FaciesConsistencyDialog::setReport(const QVariantMap &report) {
  m_report = report;
  const int compared = report.value("compared").toInt();
  const int agreed = report.value("agreed").toInt();
  const int uncompared = report.value("uncompared").toInt();
  m_summary->setText(
      tr("层位 %1\n测井相：%2\n地震相：%3\n参与比对 %4 井，一致 %5 井，一致率 %6；"
         "Cohen κ = %7；未比对 %8 井（未预测或井位在地震相范围外）。")
          .arg(report.value("horizon").toString(), report.value("well_title").toString(),
               report.value("seismic_title").toString())
          .arg(compared)
          .arg(agreed)
          .arg(compared > 0 ? QStringLiteral("%1%").arg(report.value("agreement").toDouble() * 100.0, 0, 'f', 1)
                            : tr("—"))
          .arg(compared > 0 ? QString::number(report.value("kappa").toDouble(), 'f', 3) : tr("—"))
          .arg(uncompared));
  const auto rows = report.value("rows").toList();
  m_wells->setRowCount(rows.size());
  for (int i = 0; i < rows.size(); ++i) {
    const auto r = rows.at(i).toMap();
    const auto text = [](const QVariant &v) {
      return v.isNull() || v.toString().isEmpty() ? QObject::tr("—") : v.toString();
    };
    m_wells->setItem(i, 0, new QTableWidgetItem(text(r.value("well_name"))));
    m_wells->setItem(i, 1, new QTableWidgetItem(text(r.value("well_facies"))));
    m_wells->setItem(i, 2, new QTableWidgetItem(text(r.value("seismic_facies"))));
    // 语义状态永远配文字（DESIGN.md：不单独用色）。
    m_wells->setItem(i, 3, new QTableWidgetItem(
                               !r.value("compared").toBool() ? tr("未比对")
                               : r.value("agree").toBool()   ? tr("一致")
                                                             : tr("不一致")));
    m_wells->setItem(i, 4, new QTableWidgetItem(r.value("reason").toString()));
  }
  const auto codes = report.value("codes").toList();
  const auto matrix = report.value("confusion").toList();
  m_matrix->clear();
  m_matrix->setRowCount(codes.size());
  m_matrix->setColumnCount(codes.size());
  QStringList labels;
  for (const auto &c : codes) {
    const auto m = c.toMap();
    labels << (m.value("name").toString().isEmpty() ? m.value("code").toString()
                                                    : m.value("name").toString());
  }
  m_matrix->setHorizontalHeaderLabels(labels);
  m_matrix->setVerticalHeaderLabels(labels);
  for (int i = 0; i < matrix.size() && i < codes.size(); ++i) {
    const auto line = matrix.at(i).toList();
    for (int j = 0; j < line.size() && j < codes.size(); ++j) {
      auto *item = new QTableWidgetItem(QString::number(line.at(j).toInt()));
      item->setTextAlignment(Qt::AlignCenter);
      if (i == j) {
        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);
        item->setToolTip(tr("对角线 = 一致"));
      }
      m_matrix->setItem(i, j, item);
    }
  }
  m_path->setText(report.value("report_path").toString().isEmpty()
                      ? QString()
                      : tr("报告已留档：%1").arg(report.value("report_path").toString()));
}
