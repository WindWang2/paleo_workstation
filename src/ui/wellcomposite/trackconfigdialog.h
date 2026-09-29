// 层：视图
#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QTableWidget;

#include <QList>

#include "trackregistry.h"
#include "domain/wellcompositemodel.h"

// ui/wellcomposite/trackconfigdialog — D1.6/D1.7 道配置对话框
//
// 单道全量配置：
//   道类型（注册表可建类型）/ 标题 / 宽度 / 打印导出开关（D1.6）
//   曲线族道：曲线集选择进同一道 + 各曲线独立量程（线性/对数切换）/ 单位 / 色，
//   重叠网格开关与密度（D1.7）；预置配色方案一键应用（颜色集，D1.6）。
// 对话框只产出新 TrackSpec（纯渲染收集输入）；应用到画布由调用方（面板）执行。

namespace WellComposite
{

class TrackConfigDialog : public QDialog
{
  Q_OBJECT

public:
  // spec：当前道规格；curvePool：可选曲线全集（曲线族道编辑用）
  TrackConfigDialog(const TrackSpec &spec, const QVector<CurveData> &curvePool,
                    QWidget *parent = nullptr);

  // 应用后的新规格（类型未变时 params 原样带回；曲线族道带新曲线集/覆盖/网格）
  TrackSpec resultSpec() const { return m_result; }

  // 测试钩子：直接改表格选择（列：0 选择 1 名称 2 min 3 max 4 log 5 单位 6 色）
  void setCurveRowChecked(int row, bool checked);

  static QColor paletteColor(int paletteIdx, int curveIdx);
  // 预置配色方案名（颜色集）
  static QStringList paletteNames();

  void accept() override;

private slots:
  void applyPalette(int palette);

private:
  void buildUi();
  void populateCurveTable();
  void gatherResult();

  TrackSpec m_initial;
  TrackSpec m_result;
  QVector<CurveData> m_curvePool;

  QComboBox *m_typeCombo = nullptr;
  QLineEdit *m_titleEdit = nullptr;
  QSpinBox *m_widthSpin = nullptr;
  QCheckBox *m_printCheck = nullptr;
  QComboBox *m_paletteCombo = nullptr;
  QTableWidget *m_curveTable = nullptr;
  QCheckBox *m_gridCheck = nullptr;
  QComboBox *m_gridDensityCombo = nullptr;
  QWidget *m_curveGroup = nullptr;
  bool m_curveFamily = false;
};

} // namespace WellComposite
