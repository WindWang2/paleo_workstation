// 层：视图
#include "projectstatisticspage.h"
#include "../paleotheme.h"
#include "../../catalog/datacatalog.h"
#include "../../workflow/projectareasummary.h"
#include <QHeaderView>
#include <QLabel>
#include <QTimer>
#include <QPointer>
#include <QTreeWidget>
#include <QVBoxLayout>
ProjectStatisticsPage::ProjectStatisticsPage(DataCatalog *catalog, QWidget *parent) : QWidget(parent) {
  setObjectName("projectStatisticsPage");
  auto *layout = new QVBoxLayout(this); layout->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  auto *summary = new QLabel(this); summary->setObjectName("projectStatisticsSummary"); summary->setWordWrap(true); summary->setTextFormat(Qt::PlainText); layout->addWidget(summary);
  auto *table = new QTreeWidget(this); table->setObjectName("projectProgressTable"); table->setRootIsDecorated(false);
  table->setHeaderLabels({tr("层位"), tr("现有古地理图"), tr("测井预测"), tr("地震预测"), tr("单因素图"), tr("编图成果"), tr("图件输出")});
  table->header()->setSectionResizeMode(QHeaderView::ResizeToContents); layout->addWidget(table, 1);
  auto *note = new QLabel(tr("进度按已登记成果标记“已有／未登记”，不代表质量验收或解释完成度。面积采用已登记测区角点的并集（局部米网格）；井位包络面积仅为分布范围估算。"), this);
  note->setWordWrap(true); note->setTextFormat(Qt::PlainText); layout->addWidget(note);
  const QPointer<DataCatalog> safeCatalog(catalog);
  const auto refresh = [safeCatalog, summary, table] {
    const auto data = ProjectAreaSummary::snapshot(safeCatalog);
    const auto area = data.value("survey_area_km2", -1).toDouble();
    summary->setText(tr("井：%1 口（有坐标 %2 口）　地震测区：%3 个\n资料：%4 项／%5 个版本　现有古地理图：%6 项　输出版本：%7 个\n测区覆盖面积：%8　井位包络面积：%9 km²（估算）")
        .arg(data.value("wells").toInt()).arg(data.value("positioned_wells").toInt()).arg(data.value("seismic_surveys").toInt())
        .arg(data.value("assets").toInt()).arg(data.value("versions").toInt()).arg(data.value("paleomaps").toInt()).arg(data.value("outputs").toInt())
        .arg(area < 0 ? tr("未登记有效边界") : tr("%1 km²").arg(area, 0, 'f', 3)).arg(data.value("well_envelope_km2").toDouble(), 0, 'f', 3));
    table->clear();
    for (const auto &v : data.value("horizons").toList()) { const auto row = v.toMap(); QStringList cells{row.value("horizon").toString(), QString::number(row.value("existing_maps").toInt())};
      for (const auto &key : {"well", "seismic", "factor", "map", "output"}) cells << (row.value(key).toBool() ? tr("已有") : tr("未登记"));
      new QTreeWidgetItem(table, cells);
    }
  };
  auto *timer = new QTimer(this); timer->setSingleShot(true); timer->setInterval(100);
  connect(timer, &QTimer::timeout, this, refresh);
  if (catalog) connect(catalog, &DataCatalog::changed, timer, qOverload<>(&QTimer::start));
  refresh();
}
