// 层：QGIS 封装
#pragma once

#include <QObject>
#include <QPointer>
#include <cmath>

class QComboBox;
class QLabel;
class QgsExtentWidget;

struct PaleoMapExtent
{
  double xMin = 0.0;
  double yMin = 0.0;
  double xMax = 0.0;
  double yMax = 0.0;
  bool valid() const
  {
    return std::isfinite(xMin) && std::isfinite(yMin) && std::isfinite(xMax) &&
           std::isfinite(yMax) && xMax > xMin && yMax > yMin;
  }
};

class QWidget;

class PaleoMapExtentWidget : public QObject
{
  Q_OBJECT
public:
  explicit PaleoMapExtentWidget(QWidget *parent = nullptr);
  ~PaleoMapExtentWidget() override;

  PaleoMapExtent area() const;
  void setArea(const PaleoMapExtent &area);
  void setCanvas(QWidget *canvas);
  void setTargetCrsDefinition(const QString &definition);
  QgsExtentWidget *nativeWidget() const { return m_native; }
  QWidget *widget() const;

signals:
  void extentChanged();
  void canvasVisibilityRequested(bool visible);

private:
  void refreshLayers();
  QPointer<QWidget> m_widget;
  QgsExtentWidget *m_native = nullptr;
  QComboBox *m_layers = nullptr;
  QLabel *m_status = nullptr;
  // #75：画布归外部持有，销毁后裸指针悬垂——activated 回调里对悬垂指针做
  // qobject_cast 是对已删对象的虚调用。QPointer 在画布销毁时自动清零，各处的
  // `if (!canvas)` 守卫即可拦截。
  QPointer<QWidget> m_canvas;
};
