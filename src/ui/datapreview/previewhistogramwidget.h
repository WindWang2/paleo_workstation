// 层：视图
#pragma once

#include <QWidget>

#include "../../qgis/previewrasteranalysis.h"

class QCheckBox;
class QSpinBox;
class QTimer;

// ui/datapreview/previewhistogramwidget — 预览直方图小图（P2 D2.3/D5.8）。
// 嵌入属性页的小图形态与对话框的大图形态同一控件：bars + 当前拉伸界
//（lo/hi 着色区）+ 分箱可调 + 对数纵轴 + 导出 PNG。数据由分支从
// PreviewRasterAnalysis::histogram() 取；重分箱经 binsChanged 信号回到
// 分支再拉一次直方图（分箱口径在数据侧）。
class PreviewHistogramWidget : public QWidget
{
    Q_OBJECT
  public:
    // compact=true：隐藏分箱/对数控件（属性页小图）；false 显示全控件。
    explicit PreviewHistogramWidget( bool compact, QWidget *parent = nullptr );

    void setHistogram( const PreviewRasterAnalysis::Histogram &histogram );
    // 当前拉伸界（shaded 区间外的 bars 变灰——视觉直答「2%–98% 截掉了什么」）。
    void setStretchMarks( double lo, double hi );
    void setTitle( const QString &title );

    int bins() const;
    bool logScale() const;
    const PreviewRasterAnalysis::Histogram &histogram() const { return m_histogram; }

  signals:
    void binsChanged( int bins );
    void logScaleChanged( bool on );
    void exportRequested(); // 分支自己落盘（写路径不进视图）

  protected:
    void paintEvent( QPaintEvent *event ) override;
    void mouseMoveEvent( QMouseEvent *event ) override;
    void leaveEvent( QEvent *event ) override;

  private:
    QRect plotRect() const;
    double barValue( int binIndex ) const;      // 计数（log 时 log10+1）
    double barMax() const;
    QString hoverText( int binIndex ) const;

    PreviewRasterAnalysis::Histogram m_histogram;
    bool m_hasStretch = false;
    double m_stretchLo = 0.0;
    double m_stretchHi = 1.0;
    QString m_title;
    bool m_compact = false;
    int m_hoverBin = -1;
    QSpinBox *m_binsSpin = nullptr;
    QCheckBox *m_logCheck = nullptr;
    bool m_log = false;
};
