// 层：QGIS 封装
#pragma once
#include <QWidget>

class QgsPrintLayout;
class QgsLayoutView;

// qgis/layoutpreview — QgsLayoutView 的薄壳小部件（#531）。
//
// 地质图导出对话框的预览区只需要「显示某个版面 + 缩到全页」两个动作；直接
// new QgsLayoutView 会把外部 qgslayoutview.h（及其 QtWidgets 闭包）扩散进
// 视图层，违反 docs/PALEO_QGIS_PLAN.md §2「QGIS 迁移成本只许落在 src/qgis」。
// 实现放在 layoutexport.cpp（本批不新增 .cpp）。
class PaleoLayoutPreview : public QWidget {
public:
  explicit PaleoLayoutPreview(QWidget *parent = nullptr);
  ~PaleoLayoutPreview() override;
  // QGIS 4.2：setCurrentLayout 不接受空布局——调用方先把替换版面建好再切换，
  // 且应在旧场景仍存活时绑定替换（见 geologicalexportdialog::rebuild 注释）。
  void setCurrentLayout(QgsPrintLayout *layout);
  void zoomFull();
private:
  QgsLayoutView *m_view = nullptr;
};
