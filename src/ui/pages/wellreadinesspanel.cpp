// 层：界面
#include "wellreadinesspanel.h"
#include "../../workflow/mappingworkbench.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

WellReadinessPanel::WellReadinessPanel(MappingWorkbench *workbench, QListWidget *inputs,
                                       Selection selection, QWidget *parent)
    : QWidget(parent), m_workbench(workbench), m_selection(std::move(selection)) {
  setObjectName(QStringLiteral("wellReadinessPanel"));
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  auto *row = new QHBoxLayout;
  m_check = new QPushButton(tr("检查可预测井"), this);
  m_check->setObjectName(QStringLiteral("wellReadinessCheck"));
  m_check->setToolTip(tr("按与正式运行相同的规则逐井检查（仅查询模型清单，不提交预测）"));
  row->addWidget(m_check);
  m_summary = new QLabel(this);
  m_summary->setObjectName(QStringLiteral("wellReadinessSummary"));
  m_summary->setWordWrap(true);
  row->addWidget(m_summary, 1);
  m_toggle = new QPushButton(tr("查看原因"), this);
  m_toggle->setObjectName(QStringLiteral("wellReadinessToggle"));
  m_toggle->setCheckable(true);
  m_toggle->hide();
  row->addWidget(m_toggle);
  lay->addLayout(row);
  m_reasons = new QListWidget(this);
  m_reasons->setObjectName(QStringLiteral("wellReadinessReasons"));
  m_reasons->setAccessibleName(tr("逐井预测准备情况"));
  m_reasons->setMaximumHeight(120);
  m_reasons->hide();
  lay->addWidget(m_reasons);
  connect(m_toggle, &QPushButton::toggled, m_reasons, &QWidget::setVisible);
  connect(m_check, &QPushButton::clicked, this, [this] {
    const auto [horizon, ids] = m_selection();
    QString error;
    if (!m_workbench->checkWellReadiness(horizon, ids, &error))
      return showResult({}, error);
    m_checking = true;
    m_check->setEnabled(false);
    m_summary->setText(tr("正在检查 %1 口井…").arg(ids.size()));
  });
  connect(m_workbench, &MappingWorkbench::wellReadinessReady, this,
          [this](const QString &, const QVariantList &wells, const QString &error) {
    m_checking = false;
    showResult(wells, error);
  });
  // 输入或勾选变化：旧结论作废（不自动发起检查——检查会查询服务模型清单）。
  const auto invalidate = [this] {
    if (m_checking) return;
    m_summary->clear(); m_reasons->clear(); m_reasons->hide();
    m_toggle->setChecked(false); m_toggle->hide();
    // 下一轮再读勾选：页面析构时清空输入列表会触发这里，届时选择回调里的
    // 控件可能已先析构；延后到事件循环，面板随页面一起销毁即自然作废。
    QTimer::singleShot(0, this, &WellReadinessPanel::refresh);
  };
  connect(inputs->model(), &QAbstractItemModel::dataChanged, this, invalidate); // 全选按钮会屏蔽 itemChanged
  connect(inputs->model(), &QAbstractItemModel::modelReset, this, invalidate);
  connect(inputs->model(), &QAbstractItemModel::rowsInserted, this, invalidate);
  connect(inputs->model(), &QAbstractItemModel::rowsRemoved, this, invalidate);
  refresh();
}

void WellReadinessPanel::refresh() {
  const auto ids = m_selection().second;
  setVisible(!ids.isEmpty() || !m_summary->text().isEmpty());
  if (m_checking) return;
  m_check->setEnabled(!ids.isEmpty() && !m_workbench->busy());
}

void WellReadinessPanel::showResult(const QVariantList &wells, const QString &error) {
  m_reasons->clear();
  int ready = 0;
  for (const auto &v : wells) {
    const auto well = v.toMap();
    const bool ok = well.value("ready").toBool();
    ready += ok;
    const QString name = well.value("name").toString().isEmpty() ? well.value("id").toString()
                                                                 : well.value("name").toString();
    const auto *item = new QListWidgetItem(
        ok ? tr("✓ %1：可预测（%2）").arg(name, well.value("groups").toStringList().join(QStringLiteral("、")))
           : tr("✗ %1：%2").arg(name, well.value("reason").toString()), m_reasons);
    Q_UNUSED(item);
  }
  if (!error.isEmpty())
    m_summary->setText(tr("检查未完成：%1").arg(error));
  else
    m_summary->setText(tr("%1 口井 / %2 口可预测 / %3 口不可预测")
                           .arg(wells.size()).arg(ready).arg(wells.size() - ready));
  m_toggle->setVisible(!wells.isEmpty());
  if (wells.isEmpty()) m_toggle->setChecked(false);
  m_reasons->setVisible(m_toggle->isChecked() && !wells.isEmpty());
  m_check->setEnabled(true);
  refresh();
}
