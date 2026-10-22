// 层：视图
#include "mappingworkbenchpage.h"
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>

void MappingWorkbenchPage::renderEvidenceAttribution(const QVariantMap &snapshot) {
  // G03/G04 计算归因：状态摘要 + 逐输入决策分布 + 上游回执 + 井空间关联。
  if (m_attributionSummary && m_attributionDetail) {
    const auto attribution = snapshot.value("attribution").toMap();
    QStringList summary;
    if (attribution.isEmpty()) {
      summary << (snapshot.value("details_deferred").toBool() ? tr("展开更多详情后加载计算归因。") : tr("非面状成果或未选中图斑：无计算归因"));
    } else {
      const auto status = attribution.value("status").toString();
      if (status == QLatin1String("ok")) {
        const auto inputs = attribution.value("inputs").toList();
        QStringList parts;
        for (const auto &value : inputs.mid(0, 8)) {
          const auto input = value.toMap();
          parts << tr("优先级 %1（%2%）")
                      .arg(input.value("priority").toString())
                      .arg(qRound(input.value("fraction").toDouble() * 100));
        }
        summary << tr("形成依据：保存时冻结的输入按优先级重放——%1")
                       .arg(parts.isEmpty() ? tr("全部像元无有效输入") : parts.join(tr("、")));
        const auto fidelity = attribution.value("fidelity").toString();
        if (fidelity == QLatin1String("exact")) {
          summary << tr("重放与成果网格逐像元同口径");
        } else if (fidelity == QLatin1String("approximate")) {
          QStringList reasons;
          for (const auto &r : attribution.value("fidelity_reasons").toList())
            reasons << r.toString();
          summary << tr("近似重放：%1").arg(reasons.join(tr("；")));
        }
        if (attribution.contains("consistent_fraction"))
          summary << tr("重放分布与图斑相编码一致率 %1%（平滑聚合可能吸收少数像元）")
                      .arg(qRound(attribution.value("consistent_fraction").toDouble() * 100));
      } else if (status == QLatin1String("manual")) {
        summary << tr("人工图斑：无自动分类过程，依据以人工解释记录为准");
      } else {
        summary << tr("归因不可确定：%1").arg(attribution.value("message").toString());
      }
      if (!attribution.value("draft_note").toString().isEmpty())
        summary << attribution.value("draft_note").toString();
    }
    m_attributionSummary->setText(summary.join("\n"));
    QStringList detail;
    const auto inputs = attribution.value("inputs").toList();
    if (!inputs.isEmpty())
      detail << tr("【实际参与输入（重放决策）】");
    for (const auto &value : inputs.mid(0, 32)) {
      const auto input = value.toMap();
      QStringList line;
      line << tr("· %1（优先级 %2，版本 %3…）")
                  .arg(input.value("title", input.value("layer_id")).toString())
                  .arg(input.value("priority").toString())
                  .arg(input.value("version_id").toString().left(8));
      line << tr("决策 %1，覆盖 %2 个采样像元（%3%）")
                  .arg(input.value("decision").toString())
                  .arg(input.value("cells").toLongLong())
                  .arg(qRound(input.value("fraction").toDouble() * 100));
      const auto alignment = input.value("alignment").toString();
      if (!alignment.isEmpty()) {
        line << (alignment == QLatin1String("grid_defining")
                     ? tr("网格同口径")
                     : alignment == QLatin1String("aligned")
                           ? tr("网格对齐")
                           : alignment == QLatin1String("resampled")
                                 ? tr("重采样近似")
                                 : alignment == QLatin1String("sampled_fallback")
                                       ? tr("逐点取样")
                                       : alignment == QLatin1String("categorical_nearest")
                                             ? tr("相点最近邻")
                                             : alignment);
      }
      if (input.contains("value_min"))
        line << tr("值域 %1–%2")
                    .arg(input.value("value_min").toDouble(), 0, 'f', 1)
                    .arg(input.value("value_max").toDouble(), 0, 'f', 1);
      detail << line.join(tr("，"));
      const auto receipts = input.value("upstream_receipts").toMap();
      if (!receipts.isEmpty()) {
        QStringList receiptParts;
        if (receipts.contains("constraints_consumed"))
          receiptParts << tr("消费约束 %1 条")
                              .arg(receipts.value("constraints_consumed").toList().size());
        if (receipts.contains("constraints_unused"))
          receiptParts << tr("未用约束 %1 条")
                              .arg(receipts.value("constraints_unused").toList().size());
        if (receipts.contains("engine_samples_used"))
          receiptParts << tr("参与井样 %1")
                              .arg(receipts.value("engine_samples_used").toList().size());
        detail << tr("  上游单因素回执：%1（约束被该因素消费 ≠ 决定了本图斑相分类）")
                      .arg(receiptParts.join(tr("，")));
      }
    }
    const auto unresolved = attribution.value("unresolved").toList();
    if (!unresolved.isEmpty()) {
      detail << tr("【未确定】");
      for (const auto &value : unresolved.mid(0, 16)) {
        const auto row = value.toMap();
        detail << tr("· %1：%2").arg(row.value("layer_id").toString(),
                                    row.value("reason").toString());
      }
    }
    const auto wells = snapshot.value("wells_in_feature").toList();
    if (!wells.isEmpty()) {
      detail << tr("【井口位于图斑内的井（空间事实，不自动表示相支持）】");
      for (const auto &value : wells.mid(0, 50)) {
        const auto well = value.toMap();
        QStringList line{tr("· %1（%2, %3）")
                             .arg(well.value("well_name").toString())
                             .arg(well.value("x").toDouble(), 0, 'f', 0)
                             .arg(well.value("y").toDouble(), 0, 'f', 0)};
        if (well.contains("formation_facies_code"))
          line << tr("本层位解释相编码 %1").arg(well.value("formation_facies_code").toInt());
        detail << line.join(tr("，"));
      }
    }
    if (snapshot.contains("wells_in_feature_truncated"))
      detail << tr("（井列表已按 500 上限截断）");
    // G06 证据冲突分析结果：分级 + 类型 + 定位 + 建议（不合成置信度）。
    const auto conflicts = snapshot.value("conflicts").toList();
    if (!conflicts.isEmpty()) {
      detail << tr("【多源证据冲突分析（%1 条）】").arg(conflicts.size());
      for (const auto &value : conflicts.mid(0, 30)) {
        const auto finding = value.toMap();
        detail << tr("· [%1] %2（%3）\n  %4\n  建议：%5")
                      .arg(finding.value("severity").toString(),
                           finding.value("message").toString(),
                           finding.value("conflict_type").toString(),
                           finding.value("basis").toString(),
                           finding.value("suggestion").toString());
      }
    }
    const auto seismic = snapshot.value("seismic_sources");
    if (!seismic.toList().isEmpty())
      detail << tr("【地震证据来源】%1 条（版本事实；详见生成来源）")
                  .arg(seismic.toList().size());
    m_attributionDetail->setPlainText(
        detail.isEmpty() ? tr("暂无计算归因信息") : detail.join("\n"));
    renderEvidenceAttributionTargets(snapshot);
  }
}

void MappingWorkbenchPage::renderEvidenceAttributionTargets(const QVariantMap &snapshot) {
  const auto wells = snapshot.value("wells_in_feature").toList();
  const auto conflicts = snapshot.value("conflicts").toList();
    if (m_attributionTargets) {
      m_attributionTargets->clear();
      // 行数据按 EvidenceInteraction::navigate 的入参口径组织：井走实体坐标，
      // 单因素冲突走来源版本。
      if (snapshot.contains("wells_in_feature_unavailable")) {
        auto *item = new QListWidgetItem(
            snapshot.value("wells_in_feature_unavailable").toString(),
            m_attributionTargets);
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
      }
      for (const auto &value : wells.mid(0, 200)) {
        const auto well = value.toMap();
        auto *item = new QListWidgetItem(
            tr("井 %1（%2, %3）")
                .arg(well.value("well_name", well.value("entity_id")).toString())
                .arg(well.value("x").toDouble(), 0, 'f', 0)
                .arg(well.value("y").toDouble(), 0, 'f', 0),
            m_attributionTargets);
        item->setData(Qt::UserRole,
                      QVariantMap{{"entity_id", well.value("entity_id")},
                                  {"has_coordinates", true},
                                  {"x", well.value("x")},
                                  {"y", well.value("y")},
                                  {"horizon", snapshot.value("horizon")}});
        item->setToolTip(tr("双击定位井口"));
      }
      for (const auto &value : conflicts.mid(0, 200)) {
        const auto finding = value.toMap();
        const auto evidence = finding.value("evidence").toMap();
        const auto locator = evidence.value("locator").toMap();
        QVariantMap row;
        if (!locator.value("entity_id").toString().isEmpty())
          row = {{"entity_id", locator.value("entity_id")},
                 {"has_coordinates", locator.value("has_coordinates", true)},
                 {"x", locator.value("x")}, {"y", locator.value("y")},
                 {"horizon", snapshot.value("horizon")}};
        else if (!locator.value("version_id").toString().isEmpty())
          row = {{"source_version", locator.value("version_id")},
                 {"horizon", snapshot.value("horizon")}};
        if (row.isEmpty())
          continue;
        auto *item = new QListWidgetItem(
            tr("[%1] %2 · %3")
                .arg(finding.value("severity").toString(),
                     finding.value("conflict_type").toString(),
                     evidence.value("label").toString()),
            m_attributionTargets);
        item->setData(Qt::UserRole, row);
        item->setToolTip(tr("双击定位冲突来源：%1")
                             .arg(finding.value("message").toString()));
      }
    }
}
