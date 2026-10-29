// 层：数据
#include "receiptsemantics.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QStringList>

namespace paleo::receipts
{
namespace
{
QString tr( const char *text )
{
  return QCoreApplication::translate( "ReceiptSemantics", text );
}

void addFinding( QVariantMap &out, const QString &severity, const QString &code,
                 const QString &message )
{
  QVariantList findings = out.value( QStringLiteral( "quality_findings" ) ).toList();
  findings << QVariantMap{ { QStringLiteral( "severity" ), severity },
                           { QStringLiteral( "code" ), code },
                           { QStringLiteral( "message" ), message } };
  out.insert( QStringLiteral( "quality_findings" ), findings );
}

// 行身份：按候选键顺序取第一个非空字符串值（不同算法的行用不同主键）。
QString rowIdentity( const QVariantMap &row, std::initializer_list<const char *> keys )
{
  for ( const char *k : keys )
  {
    const QString value = row.value( QString::fromLatin1( k ) ).toString();
    if ( !value.isEmpty() )
      return value;
  }
  return QString();
}

QString sampleIdentity( const QVariantMap &row )
{
  return rowIdentity( row, { "well_id", "source_record_id", "id", "name" } );
}

QString wellIdentity( const QVariantMap &row )
{
  return rowIdentity( row, { "well_id", "entity_id", "id", "well_name", "name" } );
}

void copyKeys( const QVariantMap &from, QVariantMap &to, std::initializer_list<const char *> keys )
{
  for ( const char *k : keys )
  {
    const QString key = QString::fromLatin1( k );
    if ( from.contains( key ) && !from.value( key ).isNull() )
      to.insert( key, from.value( key ) );
  }
}

// ---- single_factor_raster：约束（constraints_*）+ 引擎样点（engine_samples_*）
//
// consumed 只认 constraints_consumed（引擎回执，运行后事实）；样点维度按
// participating 进 accepted/skipped。无任何回执键的旧版本 → legacy。
QVariantMap normalizeSingleFactor( const QVariantMap &extra, QVariantMap out )
{
  out.insert( QStringLiteral( "known" ), true );
  const bool hasConstraints = extra.contains( QStringLiteral( "constraints_consumed" ) ) ||
                              extra.contains( QStringLiteral( "constraints_unused" ) );
  const bool hasSamples = extra.contains( QStringLiteral( "engine_samples_used" ) ) ||
                          extra.contains( QStringLiteral( "engine_samples_skipped" ) );
  if ( !hasConstraints && !hasSamples )
  {
    addFinding( out, QStringLiteral( "Info" ), QStringLiteral( "legacy_no_receipts" ),
                tr( "该版本无计算回执（早期引擎未产出），参与情况不可追溯" ) );
    return out; // legacy 保持 true
  }
  out.insert( QStringLiteral( "legacy" ), false );

  QVariantList consumed, accepted, skipped;
  for ( const QVariant &entry : extra.value( QStringLiteral( "constraints_consumed" ) ).toList() )
  {
    const QVariantMap row = entry.toMap();
    QVariantMap item{ { QStringLiteral( "id" ), row.value( QStringLiteral( "id" ) ) },
                      { QStringLiteral( "role" ), row.value( QStringLiteral( "role" ) ) },
                      { QStringLiteral( "dimension" ), QStringLiteral( "constraints" ) } };
    if ( item.value( QStringLiteral( "id" ) ).toString().isEmpty() )
      continue;
    consumed << item;
  }
  for ( const QVariant &entry : extra.value( QStringLiteral( "constraints_unused" ) ).toList() )
  {
    const QVariantMap row = entry.toMap();
    if ( row.value( QStringLiteral( "id" ) ).toString().isEmpty() )
      continue;
    skipped << QVariantMap{ { QStringLiteral( "id" ), row.value( QStringLiteral( "id" ) ) },
                            { QStringLiteral( "reason" ), row.value( QStringLiteral( "reason" ) ) },
                            { QStringLiteral( "dimension" ), QStringLiteral( "constraints" ) } };
  }
  for ( const QVariant &entry : extra.value( QStringLiteral( "engine_samples_used" ) ).toList() )
  {
    const QVariantMap row = entry.toMap();
    // 宽容：写作端已按 participating 拆分，这里再过滤一次防外部数据。
    if ( !row.value( QStringLiteral( "participating" ), true ).toBool() )
      continue;
    QVariantMap item{ { QStringLiteral( "id" ), sampleIdentity( row ) },
                      { QStringLiteral( "dimension" ), QStringLiteral( "samples" ) } };
    if ( row.contains( QStringLiteral( "name" ) ) )
      item.insert( QStringLiteral( "title" ), row.value( QStringLiteral( "name" ) ) );
    if ( item.value( QStringLiteral( "id" ) ).toString().isEmpty() )
      continue;
    accepted << item;
  }
  for ( const QVariant &entry : extra.value( QStringLiteral( "engine_samples_skipped" ) ).toList() )
  {
    const QVariantMap row = entry.toMap();
    const QString id = sampleIdentity( row );
    if ( id.isEmpty() )
      continue;
    skipped << QVariantMap{ { QStringLiteral( "id" ), id },
                            { QStringLiteral( "reason" ), row.value( QStringLiteral( "reason" ) ) },
                            { QStringLiteral( "dimension" ), QStringLiteral( "samples" ) } };
  }
  out.insert( QStringLiteral( "accepted_inputs" ), accepted );
  out.insert( QStringLiteral( "consumed_inputs" ), consumed );
  out.insert( QStringLiteral( "skipped_inputs" ), skipped );

  QVariantMap effective;
  copyKeys( extra, effective, { "algorithm_id", "engine_id", "method_actual", "cell_size", "field",
                                "factor_id", "horizon", "variogram", "fallback_reason", "crs_mode",
                                "extent_source", "value_source" } );
  out.insert( QStringLiteral( "effective_parameters" ), effective );
  // 克里金回落：请求的 engine_id 没有执行，algorithm_id/method_actual 才是
  // 实际引擎——请求值如实记为被忽略参数（原因见 fallback_reason）。
  QVariantMap ignored;
  if ( !extra.value( QStringLiteral( "fallback_reason" ) ).toString().isEmpty() &&
       extra.contains( QStringLiteral( "engine_id" ) ) &&
       extra.value( QStringLiteral( "engine_id" ) ) != extra.value( QStringLiteral( "algorithm_id" ) ) )
    ignored.insert( QStringLiteral( "engine_id" ), extra.value( QStringLiteral( "engine_id" ) ) );
  out.insert( QStringLiteral( "ignored_parameters" ), ignored );

  QVariantMap resultVersion;
  copyKeys( extra, resultVersion,
            { "finite_cells", "nodata_cells", "extrapolated_cells", "barrier_cells",
              "kriging_cells", "idw_fallback_cells" } );
  out.insert( QStringLiteral( "result_version" ), resultVersion );
  return out;
}

// ---- composed_facies：ordered_inputs（计划）× compose_receipts（事实）
QVariantMap normalizeComposed( const QVariantMap &extra, QVariantMap out )
{
  out.insert( QStringLiteral( "known" ), true );
  const QVariantMap receipts = extra.value( QStringLiteral( "compose_receipts" ) ).toMap();
  if ( receipts.isEmpty() )
  {
    addFinding( out, QStringLiteral( "Info" ), QStringLiteral( "legacy_no_receipts" ),
                tr( "该版本无融合回执（早期综合编图未记录 won_cells），逐输入参与不可追溯" ) );
    return out;
  }
  out.insert( QStringLiteral( "legacy" ), false );

  const QVariantList ordered = extra.value( QStringLiteral( "ordered_inputs" ) ).toList();
  QVariantList declared;
  QSet<QString> receiptVersions;
  for ( const QVariant &entry : ordered )
  {
    const QVariantMap row = entry.toMap();
    declared << QVariantMap{ { QStringLiteral( "layer_id" ), row.value( QStringLiteral( "layer_id" ) ) },
                             { QStringLiteral( "version_id" ), row.value( QStringLiteral( "version_id" ) ) },
                             { QStringLiteral( "priority" ), row.value( QStringLiteral( "priority" ) ) } };
  }
  out.insert( QStringLiteral( "declared_inputs" ), declared );

  QVariantList accepted, consumed, skipped;
  const QVariantList receiptInputs = receipts.value( QStringLiteral( "inputs" ) ).toList();
  for ( const QVariant &entry : receiptInputs )
  {
    const QVariantMap row = entry.toMap();
    const QString versionId = row.value( QStringLiteral( "version_id" ) ).toString();
    if ( !versionId.isEmpty() )
      receiptVersions.insert( versionId );
    // dimension=inputs：这些是图层/版本行，不是约束维度——不标维度就会被
    // compareWithPlan 的约束口径当成约束参与事实，与「约束使用计划」对比
    // 必然产生成片伪差异（finding-36）。
    QVariantMap base{ { QStringLiteral( "layer_id" ), row.value( QStringLiteral( "layer_id" ) ) },
                      { QStringLiteral( "version_id" ), row.value( QStringLiteral( "version_id" ) ) },
                      { QStringLiteral( "priority" ), row.value( QStringLiteral( "priority" ) ) },
                      { QStringLiteral( "dimension" ), QStringLiteral( "inputs" ) } };
    const qlonglong wonCells = row.value( QStringLiteral( "won_cells" ) ).toLongLong();
    accepted << base;
    if ( wonCells > 0 )
    {
      base.insert( QStringLiteral( "consumed_count" ), wonCells );
      base.insert( QStringLiteral( "participation_scope" ), QStringLiteral( "won_cells" ) );
      consumed << base;
    }
    else
    {
      base.insert( QStringLiteral( "reason" ),
                   tr( "全网格被更高优先级输入覆盖（won_cells=0）" ) );
      skipped << base;
    }
  }
  // 计划里有、回执里没有的输入：如实进 skipped 并记 finding，不静默消失。
  int uncovered = 0;
  for ( const QVariant &entry : declared )
  {
    const QString versionId = entry.toMap().value( QStringLiteral( "version_id" ) ).toString();
    if ( versionId.isEmpty() || receiptVersions.contains( versionId ) )
      continue;
    ++uncovered;
    skipped << QVariantMap{ { QStringLiteral( "version_id" ), versionId },
                            { QStringLiteral( "layer_id" ), entry.toMap().value( QStringLiteral( "layer_id" ) ) },
                            { QStringLiteral( "reason" ), tr( "运行回执未覆盖该输入" ) },
                            { QStringLiteral( "dimension" ), QStringLiteral( "inputs" ) } };
  }
  if ( uncovered > 0 )
    addFinding( out, QStringLiteral( "Warning" ), QStringLiteral( "receipt_inputs_incomplete" ),
                tr( "ordered_inputs 共 %1 项，运行回执未覆盖 %2 项" ).arg( declared.size() ).arg( uncovered ) );
  out.insert( QStringLiteral( "accepted_inputs" ), accepted );
  out.insert( QStringLiteral( "consumed_inputs" ), consumed );
  out.insert( QStringLiteral( "skipped_inputs" ), skipped );

  QVariantMap resultVersion;
  copyKeys( receipts, resultVersion, { "decided_cells", "total_cells" } );
  out.insert( QStringLiteral( "result_version" ), resultVersion );

  QVariantMap effective = extra.value( QStringLiteral( "algorithm_parameters" ) ).toMap();
  effective.remove( QStringLiteral( "ordered_inputs" ) ); // 输入清单不是参数，已单列 declared
  if ( extra.contains( QStringLiteral( "thresholds" ) ) )
    effective.insert( QStringLiteral( "thresholds" ), extra.value( QStringLiteral( "thresholds" ) ) );
  out.insert( QStringLiteral( "effective_parameters" ), effective );
  return out;
}

// ---- facies_polygons：polygonize_provenance（JSON 字符串）+ 血缘父版本
QVariantMap normalizePolygons( const QVariantMap &extra, QVariantMap out )
{
  out.insert( QStringLiteral( "known" ), true );
  const QString provenanceJson = extra.value( QStringLiteral( "polygonize_provenance" ) ).toString();
  QVariantMap provenance;
  if ( !provenanceJson.isEmpty() )
  {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson( provenanceJson.toUtf8(), &parseError );
    if ( parseError.error != QJsonParseError::NoError || !doc.isObject() )
      addFinding( out, QStringLiteral( "Warning" ), QStringLiteral( "provenance_unparsable" ),
                  tr( "polygonize_provenance 不是合法 JSON：%1" ).arg( parseError.errorString() ) );
    else
      provenance = doc.object().toVariantMap();
  }
  if ( provenance.isEmpty() )
  {
    if ( provenanceJson.isEmpty() )
      addFinding( out, QStringLiteral( "Info" ), QStringLiteral( "legacy_no_receipts" ),
                  tr( "该版本无转面口径戳（早期算法未产出 PROVENANCE），平滑/聚合参数不可追溯" ) );
    return out; // legacy 保持 true
  }
  out.insert( QStringLiteral( "legacy" ), false );

  QVariantList accepted, consumed;
  const QString sourceLayer = extra.value( QStringLiteral( "source_layer" ) ).toString();
  for ( const QString &parent : extra.value( QStringLiteral( "computational_parent_versions" ) ).toStringList() )
  {
    QVariantMap row;
    if ( !sourceLayer.isEmpty() )
      row.insert( QStringLiteral( "layer_id" ), sourceLayer );
    row.insert( QStringLiteral( "version_id" ), parent );
    // dimension=parents：血缘父版本行，不是约束维度（finding-36）。
    row.insert( QStringLiteral( "dimension" ), QStringLiteral( "parents" ) );
    accepted << row;
    // 转面读取整个父相栅格（grid 戳即其口径），无部分参与维度。
    QVariantMap used = row;
    used.insert( QStringLiteral( "participation_scope" ), QStringLiteral( "entire_parent_grid" ) );
    consumed << used;
  }
  out.insert( QStringLiteral( "accepted_inputs" ), accepted );
  out.insert( QStringLiteral( "consumed_inputs" ), consumed );
  out.insert( QStringLiteral( "skipped_inputs" ), QVariantList() ); // 无「跳过输入」维度

  QVariantMap effective;
  if ( extra.contains( QStringLiteral( "method" ) ) )
    effective.insert( QStringLiteral( "method" ), extra.value( QStringLiteral( "method" ) ) );
  copyKeys( provenance, effective, { "smooth_passes", "min_cells", "min_cells_effective", "min_fraction",
                                     "min_area", "simplify", "snap_tolerance", "angle_tolerance_deg" } );
  out.insert( QStringLiteral( "effective_parameters" ), effective );

  QVariantMap resultVersion;
  copyKeys( provenance, resultVersion, { "faces", "arcs", "smoothed_cells", "slivers_merged",
                                         "conflated_arcs", "steps", "grid" } );
  out.insert( QStringLiteral( "result_version" ), resultVersion );
  return out;
}

// ---- 带 evidence 的（wells_prediction / seismic_prediction 等）
QVariantMap normalizePrediction( const QVariantMap &evidence, QVariantMap out )
{
  out.insert( QStringLiteral( "known" ), true );
  if ( evidence.isEmpty() )
  {
    addFinding( out, QStringLiteral( "Info" ), QStringLiteral( "legacy_no_receipts" ),
                tr( "该版本无证据链记录（早期版本），输入与井参与不可追溯" ) );
    return out;
  }
  out.insert( QStringLiteral( "legacy" ), false );
  if ( evidence.value( QStringLiteral( "schema" ) ).toString() != QStringLiteral( "paleo_evidence_v1" ) )
    addFinding( out, QStringLiteral( "Warning" ), QStringLiteral( "unknown_evidence_schema" ),
                tr( "证据链 schema 未知（%1），按宽容口径解析" )
                    .arg( evidence.value( QStringLiteral( "schema" ) ).toString() ) );

  QVariantList inputs;
  for ( const QVariant &entry : evidence.value( QStringLiteral( "inputs" ) ).toList() )
  {
    const QVariantMap row = entry.toMap();
    inputs << QVariantMap{ { QStringLiteral( "version_id" ), row.value( QStringLiteral( "version_id" ) ) },
                           { QStringLiteral( "title" ), row.value( QStringLiteral( "name" ) ) },
                           { QStringLiteral( "kind" ), row.value( QStringLiteral( "kind" ) ) },
                           { QStringLiteral( "role" ), row.value( QStringLiteral( "role" ) ) },
                           { QStringLiteral( "missing" ), row.value( QStringLiteral( "missing" ), false ) } };
  }
  out.insert( QStringLiteral( "declared_inputs" ), inputs );
  out.insert( QStringLiteral( "accepted_inputs" ), inputs );

  const QVariantMap wells = evidence.value( QStringLiteral( "support" ) ).toMap()
                                .value( QStringLiteral( "wells" ) ).toMap();
  QVariantList consumed, skipped;
  for ( const QVariant &entry : wells.value( QStringLiteral( "supporting_wells" ) ).toList() )
  {
    const QVariantMap row = entry.toMap();
    const QString id = wellIdentity( row );
    if ( id.isEmpty() )
      continue;
    QVariantMap item{ { QStringLiteral( "id" ), id },
                      { QStringLiteral( "dimension" ), QStringLiteral( "wells" ) } };
    if ( row.contains( QStringLiteral( "well_name" ) ) )
      item.insert( QStringLiteral( "title" ), row.value( QStringLiteral( "well_name" ) ) );
    consumed << item;
  }
  for ( const QVariant &entry : wells.value( QStringLiteral( "skipped_wells" ) ).toList() )
  {
    const QVariantMap row = entry.toMap();
    const QString id = wellIdentity( row );
    if ( id.isEmpty() )
      continue;
    skipped << QVariantMap{ { QStringLiteral( "id" ), id },
                            { QStringLiteral( "reason" ), row.value( QStringLiteral( "reason" ) ) },
                            { QStringLiteral( "dimension" ), QStringLiteral( "wells" ) } };
  }
  out.insert( QStringLiteral( "consumed_inputs" ), consumed );
  out.insert( QStringLiteral( "skipped_inputs" ), skipped );

  out.insert( QStringLiteral( "effective_parameters" ),
              evidence.value( QStringLiteral( "parameters" ) ).toMap() );
  QVariantMap resultVersion;
  copyKeys( wells, resultVersion, { "requested", "predicted_well_count", "skipped_well_count" } );
  out.insert( QStringLiteral( "result_version" ), resultVersion );
  return out;
}
} // namespace

QVariantMap normalize( const QVariantMap &versionExtra )
{
  const QString kind = versionExtra.value( QStringLiteral( "kind" ) ).toString();
  QVariantMap out{ { QStringLiteral( "known" ), false },
                   { QStringLiteral( "kind" ), kind },
                   { QStringLiteral( "declared_inputs" ), QVariantList() },
                   { QStringLiteral( "accepted_inputs" ), QVariantList() },
                   { QStringLiteral( "consumed_inputs" ), QVariantList() },
                   { QStringLiteral( "skipped_inputs" ), QVariantList() },
                   { QStringLiteral( "effective_parameters" ), QVariantMap() },
                   { QStringLiteral( "ignored_parameters" ), QVariantMap() },
                   { QStringLiteral( "result_version" ), QVariantMap() },
                   { QStringLiteral( "quality_findings" ), QVariantList() },
                   { QStringLiteral( "legacy" ), true } };
  if ( kind == QStringLiteral( "single_factor_raster" ) )
    return normalizeSingleFactor( versionExtra, out );
  if ( kind == QStringLiteral( "composed_facies" ) )
    return normalizeComposed( versionExtra, out );
  if ( kind == QStringLiteral( "facies_polygons" ) )
    return normalizePolygons( versionExtra, out );
  const QVariant evidence = versionExtra.value( QStringLiteral( "evidence" ) );
  if ( evidence.typeId() == QMetaType::QVariantMap )
    return normalizePrediction( evidence.toMap(), out );
  addFinding( out, QStringLiteral( "Info" ), QStringLiteral( "unknown_kind" ),
              tr( "未知成果类型「%1」：无已知回执家族可归一化" )
                  .arg( kind.isEmpty() ? tr( "（空）" ) : kind ) );
  return out;
}

QVariantMap compareWithPlan( const QVariantMap &normalizedReceipts, const QVariantMap &plan )
{
  QVariantList differences;
  const auto notComparable = [&]( const QString &note ) {
    return QVariantMap{ { QStringLiteral( "comparable" ), false },
                        { QStringLiteral( "consistent" ), false },
                        { QStringLiteral( "differences" ), differences },
                        { QStringLiteral( "note" ), note } };
  };
  if ( normalizedReceipts.value( QStringLiteral( "legacy" ) ).toBool() )
    return notComparable( tr( "尚未运行，不能把计划当实际结果（该版本无运行回执）" ) );

  // 实际事实：约束维度的 consumed / skipped（样点/井/输入版本维度不参与约束
  // 计划对比）。维度标签由各 normalize 家族写入；未标注的行一律不当作约束
  // 参与事实——把无 dimension 行默认成约束，会让 composed_facies 的图层/版本
  // 行与 facies_polygons 的父版本行混进约束对比，成片伪差异（finding-36）。
  QSet<QString> consumedIds;
  QHash<QString, QString> skipReasons;
  const auto isConstraintDimension = []( const QVariantMap &row ) {
    return row.value( QStringLiteral( "dimension" ) ).toString() == QStringLiteral( "constraints" );
  };
  for ( const QVariant &entry : normalizedReceipts.value( QStringLiteral( "consumed_inputs" ) ).toList() )
  {
    const QVariantMap row = entry.toMap();
    if ( !isConstraintDimension( row ) )
      continue;
    // 空 id（version_id 缺失的行）不构成事实：否则会产出 id 为空的伪差异行。
    const QString id = rowIdentity( row, { "id", "version_id" } );
    if ( id.isEmpty() )
      continue;
    consumedIds.insert( id );
  }
  for ( const QVariant &entry : normalizedReceipts.value( QStringLiteral( "skipped_inputs" ) ).toList() )
  {
    const QVariantMap row = entry.toMap();
    if ( !isConstraintDimension( row ) )
      continue;
    const QString id = rowIdentity( row, { "id", "version_id" } );
    if ( !id.isEmpty() )
      skipReasons.insert( id, row.value( QStringLiteral( "reason" ) ).toString() );
  }
  // 无 consumed 不等于未运行（克里金等方法约束零消费但 skipped 有记录）；
  // 只有约束维度整条回执都没有时才不可比。
  if ( consumedIds.isEmpty() && skipReasons.isEmpty() )
    return notComparable( tr( "尚未运行，不能把计划当实际结果（回执未记录任何约束参与）" ) );

  // 计划快照：宽容解析 decisions 列表或 summary 形状（consumed/unused/available/blocking）。
  struct PlannedRow
  {
    QString id;
    QString planned;
  };
  QList<PlannedRow> plannedRows;
  QSet<QString> plannedIds;
  const auto appendPlanned = [&]( const QString &id, const QString &planned ) {
    if ( id.isEmpty() || plannedIds.contains( id ) )
      return;
    plannedIds.insert( id );
    plannedRows << PlannedRow{ id, planned };
  };
  const QVariant decisions = plan.value( QStringLiteral( "decisions" ) );
  if ( decisions.typeId() == QMetaType::QVariantList )
  {
    for ( const QVariant &entry : decisions.toList() )
    {
      const QVariantMap row = entry.toMap();
      const QString id = rowIdentity( row, { "id", "feature_id" } );
      QString planned = row.value( QStringLiteral( "planned" ) ).toString();
      if ( planned.isEmpty() )
        planned = row.value( QStringLiteral( "blocking" ) ).toBool()
                      ? QStringLiteral( "blocking" )
                      : ( row.value( QStringLiteral( "consumed" ) ).toBool()
                              ? QStringLiteral( "consumed" )
                              : QStringLiteral( "available" ) );
      appendPlanned( id, planned );
    }
  }
  else
  {
    // summary 快照：consumed 是 id 字符串列表，unused 是 {id,reason} 行；
    // available/blocking 也可能是字符串列表——统一按「字符串或 map」宽容取 id。
    const auto idList = [&plan]( const char *key ) {
      QStringList ids;
      const QVariant value = plan.value( QString::fromLatin1( key ) );
      if ( value.typeId() == QMetaType::QStringList )
        return value.toStringList();
      for ( const QVariant &entry : value.toList() )
      {
        if ( entry.typeId() == QMetaType::QVariantMap )
          ids << rowIdentity( entry.toMap(), { "id", "feature_id" } );
        else
          ids << entry.toString();
      }
      return ids;
    };
    for ( const QString &id : idList( "consumed" ) )
      appendPlanned( id, QStringLiteral( "consumed" ) );
    for ( const QString &id : idList( "blocking" ) )
      appendPlanned( id, QStringLiteral( "blocking" ) );
    for ( const QString &id : idList( "available" ) )
      appendPlanned( id, QStringLiteral( "available" ) );
    for ( const QVariant &entry : plan.value( QStringLiteral( "unused" ) ).toList() )
    {
      if ( entry.typeId() == QMetaType::QVariantMap )
        appendPlanned( rowIdentity( entry.toMap(), { "id", "feature_id" } ), QStringLiteral( "available" ) );
      else
        appendPlanned( entry.toString(), QStringLiteral( "available" ) );
    }
  }
  if ( plannedRows.isEmpty() )
    return notComparable( tr( "计划快照为空或无法解析，无从比较" ) );

  for ( const PlannedRow &row : plannedRows )
  {
    const QString actual = consumedIds.contains( row.id )
                               ? QStringLiteral( "consumed" )
                               : ( skipReasons.contains( row.id ) ? QStringLiteral( "unused" )
                                                                  : QStringLiteral( "missing" ) );
    QString reason;
    if ( row.planned == QStringLiteral( "consumed" ) )
    {
      if ( actual == QStringLiteral( "unused" ) )
        reason = tr( "计划消费，实际未消费：%1" ).arg( skipReasons.value( row.id ) );
      else if ( actual == QStringLiteral( "missing" ) )
        reason = tr( "计划消费，但运行回执未登记该约束" );
    }
    else if ( row.planned == QStringLiteral( "blocking" ) )
    {
      if ( actual == QStringLiteral( "consumed" ) )
        reason = tr( "计划判定为阻断（应整次拒绝），实际却被消费" );
      // blocking + 未消费：方向一致，不算差异。
    }
    else // available / 其他：计划不消费
    {
      if ( actual == QStringLiteral( "consumed" ) )
        reason = tr( "计划不消费，实际被消费" );
      else if ( actual == QStringLiteral( "missing" ) )
        reason = tr( "计划登记为可用，但运行回执未登记该约束" );
    }
    if ( !reason.isEmpty() )
      differences << QVariantMap{ { QStringLiteral( "id" ), row.id },
                                  { QStringLiteral( "planned" ), row.planned },
                                  { QStringLiteral( "actual" ), actual },
                                  { QStringLiteral( "reason" ), reason } };
  }
  for ( const QString &id : consumedIds )
    if ( !plannedIds.contains( id ) )
      differences << QVariantMap{ { QStringLiteral( "id" ), id },
                                  { QStringLiteral( "planned" ), QStringLiteral( "(not_planned)" ) },
                                  { QStringLiteral( "actual" ), QStringLiteral( "consumed" ) },
                                  { QStringLiteral( "reason" ),
                                    tr( "实际被消费，但计划快照未登记" ) } };
  const QString note = differences.isEmpty()
                           ? tr( "计划与运行回执一致（%1 条计划）" ).arg( plannedRows.size() )
                           : tr( "计划与运行回执存在 %1 处差异" ).arg( differences.size() );
  return QVariantMap{ { QStringLiteral( "comparable" ), true },
                       { QStringLiteral( "consistent" ), differences.isEmpty() },
                       { QStringLiteral( "differences" ), differences },
                       { QStringLiteral( "note" ), note } };
}
} // namespace paleo::receipts
