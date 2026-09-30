// 层：视图
#pragma once
#include <QLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>

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
