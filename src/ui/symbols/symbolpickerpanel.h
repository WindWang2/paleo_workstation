// 层：视图
// 符号选择器面板（方向 31）：人工覆盖默认样式时的花纹/线型浏览面板。
// 视图只发信号：patternPicked 携带语义 id，由宿主接 QgisStyleService 入口
// （applyLithologyPatternStyle/applyFaultLineLayerStyle 等）应用覆盖。
// 缩略与图层渲染同管线（GeoPatterns 构造符号 → bigSymbolPreviewImage，
// 同图例 builder）——选择器所见 = 图面所得。
#pragma once
#include <QWidget>

class QListWidget;
class QLineEdit;

class SymbolPickerPanel : public QWidget
{
  Q_OBJECT
public:
  // 语义族：岩性花纹 / 相花纹 / 线型规范（词表单点 GeoPatterns）。
  enum class Family
  {
    Lithology,
    Facies,
    LineStyle
  };
  Q_ENUM(Family)

  explicit SymbolPickerPanel(Family family, QWidget *parent = nullptr);

  // 语义过滤：按词面/语义 id 包含匹配（大小写不敏感；空 = 全量）。
  void setFilter(const QString &text);
  QString filter() const;

  // 当前选中语义 id（无选中 → 空）。
  QString selectedPatternId() const;

signals:
  void patternPicked(const QString &patternId);

private:
  void rebuild();

  Family m_family;
  QLineEdit *m_filter = nullptr;
  QListWidget *m_grid = nullptr;
};
