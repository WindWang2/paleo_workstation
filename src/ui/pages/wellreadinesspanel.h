#pragma once
// 层：视图
// 预测专项 P0：测井预测的批量逐井准备检查面板（编图台 predict 模式）。
// 只呈现 MappingWorkbench::checkWellReadiness 的结论，不含任何判定规则。
#include <QStringList>
#include <QWidget>
#include <functional>
#include <utility>

class MappingWorkbench;
class QLabel;
class QListWidget;
class QPushButton;

class WellReadinessPanel : public QWidget {
  Q_OBJECT
public:
  // selection：当前层位 + 所勾选井 id（非测井输入时返回空列表即隐藏）。
  using Selection = std::function<std::pair<QString, QStringList>()>;
  WellReadinessPanel(MappingWorkbench *workbench, QListWidget *inputs, Selection selection,
                     QWidget *parent = nullptr);
  void refresh(); // 输入/勾选变化：作废旧结论，更新按钮可用性

private:
  void showResult(const QVariantList &wells, const QString &error);
  MappingWorkbench *m_workbench;
  Selection m_selection;
  QPushButton *m_check = nullptr, *m_toggle = nullptr;
  QLabel *m_summary = nullptr;
  QListWidget *m_reasons = nullptr;
  bool m_checking = false;
};
