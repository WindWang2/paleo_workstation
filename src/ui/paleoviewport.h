// 层：视图
#pragma once
#include <QLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QStyle>

// A dock has one scroll viewport. Only the visible panel may determine its
// overflow; hidden long forms must not stretch a compact validation/data page.
class PaleoPanelHost : public QWidget
{
public:
  explicit PaleoPanelHost(QWidget *parent = nullptr) : QWidget(parent)
  {
    auto *stack = new QStackedLayout(this);
    stack->setSizeConstraint(QLayout::SetNoConstraint);
    connect(stack, &QStackedLayout::currentChanged, this, [this] { updateGeometry(); });
  }
  QSize minimumSizeHint() const override
  {
    auto *stack = qobject_cast<QStackedLayout *>(layout());
    auto *page = stack ? stack->currentWidget() : nullptr;
    return page ? page->minimumSizeHint().expandedTo(page->minimumSize()) : QSize(0, 0);
  }
  QSize sizeHint() const override
  {
    auto *stack = qobject_cast<QStackedLayout *>(layout());
    auto *page = stack ? stack->currentWidget() : nullptr;
    return page ? page->sizeHint().expandedTo(minimumSizeHint()) : QSize(0, 0);
  }
  bool hasHeightForWidth() const override
  {
    auto *stack = qobject_cast<QStackedLayout *>(layout());
    auto *page = stack ? stack->currentWidget() : nullptr;
    return page && page->hasHeightForWidth();
  }
  int heightForWidth(int width) const override
  {
    auto *stack = qobject_cast<QStackedLayout *>(layout());
    auto *page = stack ? stack->currentWidget() : nullptr;
    return page ? page->heightForWidth(width) : -1;
  }
};

// A canvas uses the remaining space. Hidden pages and long tool rows must not
// propagate their minimum widths to QMainWindow's dock splitter constraints.
class PaleoViewportStack : public QStackedWidget
{
public:
  explicit PaleoViewportStack(QWidget *parent = nullptr) : QStackedWidget(parent)
  {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout()->setSizeConstraint(QLayout::SetNoConstraint);
    setMinimumSize(0, 0);
  }
  QSize minimumSizeHint() const override { return {0, 0}; }
};

// Only the command row scrolls; the canvas below continues to fill its viewport.
class PaleoToolRow : public QScrollArea
{
public:
  explicit PaleoToolRow(QWidget *content, QWidget *parent = nullptr) : QScrollArea(parent)
  {
    setObjectName(QStringLiteral("scrollableToolRow"));
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setWidget(content);
    connect(horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this] { updateGeometry(); });
  }
  QSize sizeHint() const override
  {
    const int h = widget() ? widget()->sizeHint().height() : 0;
    return {320, h + (horizontalScrollBar()->maximum() > 0
                      ? style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0)};
  }
  QSize minimumSizeHint() const override { return {0, sizeHint().height()}; }
};
