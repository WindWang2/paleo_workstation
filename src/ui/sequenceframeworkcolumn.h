// 层：视图
#pragma once
#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "../domain/sequenceframework.h"

// ui/ — 格架柱状视图（方向 28 目标 5）。
// 单元层级色带柱状图：按格架树顺序自上而下堆叠，二级（体系域）缩进并画在
// 所属层序色带内，带高按 thickness（未填则同级等分）。只做渲染——排序、
// 覆盖区间、层级一律来自 domain/sequenceframework，视图不复算。
//
// 联动：currentUnitChanged 供剖面/平面高亮当前单元；setCurrentUnit 由外部
// （树选择或平面高亮）驱动。颜色走 QColor::fromHslF 生成的数据符号色，
// chrome 色（边框/文字/高亮）一律取 PaleoTheme token——不在本文件写色值。
class SequenceFrameworkColumnView : public QWidget
{
  Q_OBJECT
  public:
    explicit SequenceFrameworkColumnView( QWidget *parent = nullptr );

    void setFramework( const SequenceFramework::Framework &fw, const QStringList &horizons );
    const SequenceFramework::Framework &framework() const { return m_fw; }

    // 当前单元（高亮）。空串 = 无。
    QString currentUnit() const { return m_current; }
    void setCurrentUnit( const QString &unitId );

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    // 命中测试（供测试与键盘导航定位）：返回该点所在单元 id，空 = 空白。
    QString unitAt( const QPoint &pos ) const;

  signals:
    void currentUnitChanged( const QString &unitId );

  protected:
    void paintEvent( QPaintEvent *event ) override;
    void mousePressEvent( QMouseEvent *event ) override;
    void resizeEvent( QResizeEvent *event ) override;

  private:
    struct Band
    {
      QString unitId;
      QRect rect;
    };
    void relayout();
    QColor unitColor( const QString &unitId ) const;

    SequenceFramework::Framework m_fw;
    QStringList m_horizons;
    QString m_current;
    QVector<Band> m_bands;
};
