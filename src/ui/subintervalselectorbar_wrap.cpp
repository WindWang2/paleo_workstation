// 层：视图
// 层段分析条的折行：容器 = 主段（界限 + 方案/修订）+ 次段（父单元 + S1–S4 + 综合 + 状态）。
// 宽度不够时把容器方向切成上下两行，宽窗仍单行（拆出以守 #502 文件规模闸）。
#include "subintervalselectorbar.h"

#include "paleotheme.h"

#include <QBoxLayout>
#include <QHBoxLayout>
#include <QResizeEvent>

QWidget *SubIntervalSelectorBar::buildRowContainers( QHBoxLayout **secondaryLayout )
{
  const PaleoTheme::ThemeTokens &tk = PaleoTheme::tokens();
  m_rowContainer = new QWidget( this );
  m_rowContainer->setObjectName( QStringLiteral( "subIntervalSelectorRows" ) );
  m_rowLayout = new QBoxLayout( QBoxLayout::LeftToRight, m_rowContainer );
  m_rowLayout->setContentsMargins( 0, 0, 0, 0 );
  m_rowLayout->setSpacing( tk.spacingXs );
  m_primaryRow = new QWidget( m_rowContainer );
  m_primaryRow->setObjectName( QStringLiteral( "subIntervalSelectorRow" ) );
  m_primaryRow->setSizePolicy( QSizePolicy::Maximum, QSizePolicy::Preferred );
  auto *lay = new QHBoxLayout( m_primaryRow );
  lay->setContentsMargins( 0, 0, 0, 0 );
  lay->setSpacing( tk.spacingXs );
  m_secondaryRow = new QWidget( m_rowContainer );
  m_secondaryRow->setObjectName( QStringLiteral( "subIntervalSelectorSecondaryRow" ) );
  auto *lay2 = new QHBoxLayout( m_secondaryRow );
  lay2->setContentsMargins( 0, 0, 0, 0 );
  lay2->setSpacing( tk.spacingXs );
  m_rowLayout->addWidget( m_primaryRow );
  m_rowLayout->addWidget( m_secondaryRow, 1 );
  *secondaryLayout = lay2;
  return m_primaryRow;
}

void SubIntervalSelectorBar::resizeEvent( QResizeEvent *event )
{
  QWidget::resizeEvent( event );
  updateWrap();
}

void SubIntervalSelectorBar::updateWrap()
{
  QWidget *primary = m_primaryRow;
  if ( !primary || !m_secondaryRow || !m_splitSeparator )
    return;
  // 单行所需最小宽 = 主段 sizeHint + 间距 + 次段最小宽（状态文字 Ignored，不计入）。
  const QMargins m = layout()->contentsMargins();
  const int available = width() - m.left() - m.right();
  const int needed = primary->sizeHint().width() + m_rowLayout->spacing() + m_secondaryRow->minimumSizeHint().width();
  const bool wrap = available < needed;
  if ( wrap == isWrapped() )
    return;
  m_rowLayout->setDirection( wrap ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight );
  m_splitSeparator->setVisible( !wrap );
  updateGeometry();
}

bool SubIntervalSelectorBar::isWrapped() const
{
  return m_rowLayout && m_rowLayout->direction() == QBoxLayout::TopToBottom;
}
