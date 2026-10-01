// 层：视图
#pragma once

#include <QWidget>

#include "seismic3dtf.h"

namespace seismic {

// D7.1 TF 编辑器（自绘数据控件，同剖面画布护城河组件模式）：
// 下半=色带停靠点渐变（数据符号，不走 UI token），上半=不透明度折线。
// 交互：拖停靠点（横=pos 纵=alpha，邻居区间内）；双击空区插入；右键停靠
// 点删除（保留 ≥2）。变更实时发 transferFunctionChanged（面板即时重传 LUT）。
class Seismic3DTfEditorWidget : public QWidget
{
    Q_OBJECT
public:
    explicit Seismic3DTfEditorWidget(QWidget *parent = nullptr);

    void setTransferFunction(const Seismic3DTransferFunction &tf);
    [[nodiscard]] const Seismic3DTransferFunction &transferFunction() const { return tf_; }

    [[nodiscard]] QSize minimumSizeHint() const override { return {300, 150}; }

signals:
    void transferFunctionChanged(const seismic::Seismic3DTransferFunction &tf);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    [[nodiscard]] QRect gradientRect() const;
    [[nodiscard]] QRect curveRect() const;
    [[nodiscard]] int hitStop(const QPoint &pos) const;
    [[nodiscard]] QPointF stopToPoint(int index) const; // 曲线坐标系
    [[nodiscard]] QColor interpolatedColor(float pos) const;
    void emitChanged();

    Seismic3DTransferFunction tf_;
    int dragStop_ = -1;
    int hoverStop_ = -1;
};

} // namespace seismic
