// 层：视图
#include "impactoutcome.h"

#include <QCoreApplication>
#include <QStringList>
#include <QVariantList>

#include <algorithm>

namespace paleo::workbench {
namespace {
QString tr(const char *text) {
  return QCoreApplication::translate("PaleoMainWindow", text);
}
} // namespace

ImpactOutcome describeImpactOutcome(const QVariantMap &report,
                                    const QString &error) {
  ImpactOutcome out;
  out.title = tr("影响分析");
  if (report.isEmpty()) {
    out.message = error.isEmpty() ? tr("影响分析未完成。") : error;
    return out;
  }
  out.ok = true;
  qint64 changedCells = 0;
  for (const auto &value : report.value("determined").toList())
    changedCells += value.toMap().value("changed_cells").toLongLong();
  const auto reportPath = report.value("report_path").toString();
  const auto direct = report.value("direct_products").toList().size();
  const auto potential = report.value("potential_products").toList().size();
  out.message =
      reportPath.isEmpty()
          ? tr("影响分析完成：直接依赖 %1 项，潜在影响 %2 项，可确定"
               "变化 %3 像元（报告未能保存到工程目录）")
                .arg(direct)
                .arg(potential)
                .arg(changedCells)
          : tr("影响分析完成：直接依赖 %1 项，潜在影响 %2 项，可确定变化 "
               "%3 像元（详见 %4）")
                .arg(direct)
                .arg(potential)
                .arg(changedCells)
                .arg(reportPath);
  return out;
}

ImpactOutcome describeCompareOutcome(const QVariantMap &report,
                                     const QString &error) {
  ImpactOutcome out;
  out.title = tr("重算对比");
  if (report.isEmpty()) {
    out.message = error.isEmpty() ? tr("重算对比未完成。") : error;
    return out;
  }
  out.ok = true;
  auto matrix = report.value("code_matrix").toList();
  std::stable_sort(matrix.begin(), matrix.end(),
                   [](const QVariant &a, const QVariant &b) {
                     return a.toMap().value("cells").toLongLong() >
                            b.toMap().value("cells").toLongLong();
                   });
  QStringList top;
  for (const auto &value : matrix.mid(0, 3))
    top << tr("%1：%2 像元")
               .arg(value.toMap().value("transition").toString())
               .arg(value.toMap().value("cells").toLongLong());
  out.message = tr("重算对比：变化 %1/%2 像元；主要转移 %3（详见 %4）")
                    .arg(report.value("cells_changed").toLongLong())
                    .arg(report.value("total_cells").toLongLong())
                    .arg(top.isEmpty() ? tr("无") : top.join("；"))
                    .arg(report.value("report_path").toString());
  return out;
}

} // namespace paleo::workbench
