// 层：视图
#include "mappingworkbenchpage.h"
#include "pageshared.h"
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

// 与综合计算输入分开的参考入口：只发命令，面板选择不会改输入勾选。
void MappingWorkbenchPage::buildReferenceEntry(QWidget *body, QVBoxLayout *layout)
{
  auto *hint = new QLabel(tr("选择地震相、测井相、单因素图或参考相图与主图对照。可添加多个参考视图。"), body);
  hint->setWordWrap(true);
  hint->hide(); // 说明移入参考入口 tooltip，避免任务侧栏重复长文。
  for (const auto &entry : {qMakePair(QStringLiteral("referencePanel"), tr("打开参考证据")),
                            qMakePair(QStringLiteral("references"), tr("对照勾选的输入"))}) {
    if (m_mode == "compose" && entry.first == "references") continue;
    auto *button = new QPushButton(entry.second, body);
    button->setObjectName(QStringLiteral("workbench.") + entry.first);
    button->setAccessibleName(entry.second);
    button->setToolTip(entry.first == QLatin1String("referencePanel")
        ? hint->text() : entry.second);
    layout->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, name = entry.first] { issue(name); });
  }
}
