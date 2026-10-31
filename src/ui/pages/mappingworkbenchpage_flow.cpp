// 层：视图
#include "../windowsizing_internal.h"
#include "../../workflow/mappingworkbench.h"
#include "../paleotheme.h"
#include "mappingworkbenchpage.h"
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

void MappingWorkbenchPage::buildComposeAdvanced(QWidget *body, QVBoxLayout *root) {
  auto *group = new QGroupBox(body);
  group->setObjectName("mappingComposeAdvanced");
  auto *outer = new QVBoxLayout(group);
  auto *toggle = new QToolButton(group);
  toggle->setObjectName("mappingComposeToggle");
  toggle->setText(tr("可选 · 多证据综合计算与重算"));
  toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toggle->setArrowType(Qt::RightArrow);
  toggle->setCheckable(true);
  outer->addWidget(toggle);
  auto *content = new QWidget(group);
  outer->addWidget(content);
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  connect(toggle, &QToolButton::toggled, content, [content, toggle](bool expanded) {
    content->setVisible(expanded);
    toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
  });
  auto *hint = new QLabel(tr("需要计算初始相图时，勾选本层位输入；按列表顺序采用有效相值。"
                             "已有基础相图可直接建立工作副本，进行人工编制。"),
                          group);
  hint->setWordWrap(true);
  layout->addWidget(hint);
  m_inputs = new QListWidget(group);
  m_inputs->setObjectName("workbenchInputs");
  m_inputs->setMinimumHeight(paleo::ui_detail::lineRows(m_inputs, 4)); // R6 #836：行高口径，不钉死
  m_inputs->setAccessibleName(tr("编图输入，按空格勾选"));
  layout->addWidget(m_inputs, 1);
  auto *order = new QHBoxLayout;
  for (bool up : {true, false}) {
    auto *b = new QPushButton(up ? tr("上移优先级") : tr("下移优先级"), group);
    b->setObjectName(up ? "workbenchMoveUp" : "workbenchMoveDown");
    order->addWidget(b);
    connect(b, &QPushButton::clicked, this, [this, up] {
      const int i = m_inputs->currentRow(), j = i + (up ? -1 : 1);
      if (i < 0 || j < 0 || j >= m_inputs->count())
        return;
      auto *item = m_inputs->takeItem(i);
      m_inputs->insertItem(j, item);
      m_inputs->setCurrentItem(item);
    });
  }
  layout->addLayout(order);
  m_thresholds = new QLineEdit(group);
  m_thresholds->setAccessibleName(tr("单因素分相阈值"));
  m_thresholds->setPlaceholderText(tr("例如 10, 25（3 类相）"));
  auto *form = new QFormLayout;
  form->addRow(tr("单因素分相阈值"), m_thresholds);
  layout->addLayout(form);
  auto *note = new QLabel(tr("不同量纲的单因素请分别分相；相栅格和相点不使用阈值。"), group);
  note->setWordWrap(true);
  layout->addWidget(note);
  for (const auto &entry : {qMakePair(QString("compose"), tr("生成综合相图与相面")),
                            qMakePair(QString("references"), tr("在参考面板中显示勾选输入")),
                            qMakePair(QString("impactAnalysis"), tr("分析所选输入的版本变化影响")),
                            qMakePair(QString("recompute"), tr("按最近影响报告受控重算")),
                            qMakePair(QString("compareRecompute"), tr("对比重算成果与旧版本"))}) {
    auto *button = new QPushButton(entry.second, group);
    button->setObjectName("workbench." + entry.first);
    layout->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, action = entry.first] { issue(action); });
  }
  content->setVisible(false);
  root->addWidget(group);
}

void MappingWorkbenchPage::buildDeliveryStep(QWidget *body, QVBoxLayout *layout) {
  auto *group = new QGroupBox(tr("4 · 检查、保存与输出"), body);
  auto *flow = new QVBoxLayout(group);
  auto *hint = new QLabel(tr("相、亚相、微相共用一套相面分区。先检查并保存新版本，再导出。"
                             "显示层级可按图层固定，也可锁定选中相面；导出图例沿用实际显示。"),
                          group);
  hint->setWordWrap(true);
  flow->addWidget(hint);
  for (const auto &entry : {qMakePair(QString("checkTopology"), tr("检查三级相面拓扑")),
                            qMakePair(QString("save"), tr("保存编辑及图件新版本")),
                            qMakePair(QString("exportVector"), tr("导出矢量与证据…")),
                            qMakePair(QString("exportMap"), tr("输出地图（PDF / PNG / SVG）…"))}) {
    auto *button = new QPushButton(entry.second, group);
    button->setObjectName("workbench." + entry.first);
    flow->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, action = entry.first] { issue(action); });
    if (entry.first == "save")
      continue; // 复用页面既有保存会话门控。
    const auto refresh = [this, button, action = entry.first] {
      const auto id = selectedLayer();
      const auto version = m_workbench->versionForLayer(id);
      const bool topology = action == "checkTopology";
      const bool allowed =
          !id.isEmpty() && (topology ? m_workbench->isFaciesPolygon(id)
                                     : !version.id.isEmpty() && !m_workbench->hasUnpublishedEdits(id));
      button->setEnabled(allowed);
      button->setToolTip(allowed
                             ? (action == "exportVector" ? tr("GPKG / SHP / GeoJSON，包含三级分类及解释证据")
                                                         : button->text())
                         : topology ? tr("请选择三级相面图件")
                                    : tr("请选择图件并保存当前编辑及新版本"));
    };
    connect(m_results, &QTreeWidget::currentItemChanged, button, refresh);
    connect(m_workbench, &MappingWorkbench::changed, button, refresh);
    connect(m_workbench, &MappingWorkbench::displayChanged, button, refresh);
    connect(m_workbench, &MappingWorkbench::faciesEdited, button, refresh);
    refresh();
  }
  layout->addWidget(group);
}
