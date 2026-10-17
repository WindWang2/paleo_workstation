// 层：视图
#include "mappingworkbenchpage.h"
#include "pageshared.h"
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

// 与综合计算输入分开的参考入口：只发命令，面板选择不会改输入勾选。
void MappingWorkbenchPage::buildReferenceEntry(QWidget *body, QVBoxLayout *layout)
{
  layout->addWidget(paleo::pagesinternal::caption(tr("绘制参考证据"), body));
  auto *hint = new QLabel(tr("绘制相面时，可并排查看地震相、测井相、单因素图和参考相图。"
                            "在同一参考面板中添加视图，用各视图上方的下拉框选择证据。"), body);
  hint->setWordWrap(true);
  layout->addWidget(hint);
  for (const auto &entry : {qMakePair(QStringLiteral("referencePanel"), tr("打开参考证据面板")),
                            qMakePair(QStringLiteral("references"), tr("在参考面板中显示勾选输入"))}) {
    auto *button = new QPushButton(entry.second, body);
    button->setObjectName(QStringLiteral("workbench.") + entry.first);
    button->setAccessibleName(entry.second);
    button->setToolTip(entry.first == QLatin1String("referencePanel")
        ? tr("打开统一参考面板，在面板内增加视图并选择参考证据") : entry.second);
    layout->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, name = entry.first] { issue(name); });
  }
}
