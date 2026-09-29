// 层：视图
#pragma once

#include <QImage>
#include <QWidget>

namespace seismic {

// D3.9 OpenGL 回退：GL 不可用时以 2D 切片拼接替代 3D 视口（不崩、不空白）。
// 三向切片并排 CPU 渲染 + 说明条；切片槽位数据由面板在回填时喂入。
class Seismic3DFallbackWidget : public QWidget {
    Q_OBJECT

public:
    explicit Seismic3DFallbackWidget(QWidget *parent = nullptr);

    // 槽位切片（IL/XL/T）；数据到达即重绘
    void setSlice(int slot, const QImage &image, const QString &label);
    void clearSlices();

    QSize minimumSizeHint() const override { return QSize(400, 260); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::array<QImage, 3> slices_;
    std::array<QString, 3> labels_;
};

} // namespace seismic
