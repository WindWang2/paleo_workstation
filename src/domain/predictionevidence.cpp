// 层：数据
#include "predictionevidence.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSet>

namespace paleo::evidence
{
namespace
{
QString tr( const char *text )
{
  return QCoreApplication::translate( "PredictionEvidence", text );
}
// 登记簿记字段：与「怎么算出来的」无关，进指纹会让同参数结果指纹漂移。
const QSet<QString> &bookkeeping()
{
  static const QSet<QString> keys{
    QStringLiteral( "mapping_product" ), QStringLiteral( "layer_id" ),
    QStringLiteral( "title" ), QStringLiteral( "group" ),
    QStringLiteral( "created_at" ), QStringLiteral( "layer_type" ),
    QStringLiteral( "source_suffix" ), QStringLiteral( "evidence" ),
    QStringLiteral( "facies" ), QStringLiteral( "request_id" ),
    QStringLiteral( "manifest_layer_id" ), QStringLiteral( "display_mode" ),
    QStringLiteral( "label_mode" ), QStringLiteral("interpretation_reference_versions"),
    QStringLiteral("feature_summary"), QStringLiteral("quality_findings"),
    QStringLiteral("operation_id"), QStringLiteral("actor_id"),
    QStringLiteral("interpretation_events"), QStringLiteral("feature_identity"),
    QStringLiteral( "model" ), QStringLiteral( "model_names" ),
    QStringLiteral( "model_sha256" ), QStringLiteral( "model_hash" ),
    QStringLiteral( "execution_engine" ) };
  return keys;
}
QVariant pick( const QVariantMap &extra, std::initializer_list<const char *> keys )
{
  QVariantMap out;
  for ( const char *k : keys )
  {
    const QString key = QString::fromLatin1( k );
    if ( extra.contains( key ) && !extra.value( key ).isNull() )
      out.insert( key, extra.value( key ) );
  }
  return out.isEmpty() ? QVariant() : QVariant( out );
}
} // namespace

QVariantMap parameters( const QVariantMap &extra )
{
  QVariantMap out;
  for ( auto it = extra.cbegin(); it != extra.cend(); ++it )
    if ( !bookkeeping().contains( it.key() ) )
      out.insert( it.key(), it.value() );
  return out;
}

QString fingerprint( const QVariant &value )
{
  // QJsonDocument 的对象键天然排序 → 规范 JSON。
  const QByteArray json = QJsonDocument::fromVariant(
                              value.typeId() == QMetaType::QVariantMap || value.typeId() == QMetaType::QVariantList
                                  ? value
                                  : QVariant( QVariantList{ value } ) )
                              .toJson( QJsonDocument::Compact );
  return QString::fromLatin1( QCryptographicHash::hash( json, QCryptographicHash::Sha256 ).toHex() );
}

// 预测专项 P0：成果来源三分。kind ∈ model_prediction / mock /
// manual_revision / derived；based_on_mock 沿输入传染（Mock 派生、Mock 底图
// 的人工修订都不得被当作真实模型结果）；validation 只有 extra 真带独立验证
// 指标才为 validated；confidence_semantics 恒为 model_score（置信度是模型
// 输出分数，不是准确率）。
static QVariantMap provenanceOf( const QVector<InputRef> &inputs, const QVariantMap &extra )
{
  int mockInputs = 0;
  for ( const InputRef &in : inputs )
    if ( in.mock )
      ++mockInputs;
  const QString method = extra.value( QStringLiteral( "method" ) ).toString();
  const QString kindKey = extra.value( QStringLiteral( "kind" ) ).toString();
  const bool basedOnMock = extra.value( QStringLiteral( "mock" ) ).toBool() || mockInputs > 0 ||
                           method.startsWith( QLatin1String( "mock" ) );
  const bool revised = method == QLatin1String( "manual-edit-copy" ) ||
                       kindKey == QLatin1String( "edited_facies" ) ||
                       !extra.value( QStringLiteral( "spatial_edit_events" ) ).toList().isEmpty();
  const auto nonEmpty = [&extra]( const char *k ) {
    const QVariant v = extra.value( QLatin1String( k ) );
    return v.isValid() && !v.isNull() && !v.toString().isEmpty() ? true
           : v.typeId() == QMetaType::QStringList ? !v.toStringList().isEmpty()
           : v.typeId() == QMetaType::QVariantList ? !v.toList().isEmpty() : false;
  };
  const bool model = nonEmpty( "model_names" ) || nonEmpty( "model" ) || nonEmpty( "execution_engine" ) ||
                     nonEmpty( "prediction_model" ) || method.startsWith( QLatin1String( "remote" ) ) ||
                     method.startsWith( QLatin1String( "onnx" ) );
  const QString kind = revised ? QStringLiteral( "manual_revision" )
                       : basedOnMock ? QStringLiteral( "mock" )
                       : model ? QStringLiteral( "model_prediction" )
                               : QStringLiteral( "derived" );
  const QVariantMap metrics = extra.value( QStringLiteral( "validation_metrics" ) ).toMap();
  return { { QStringLiteral( "kind" ), kind },
           { QStringLiteral( "based_on_mock" ), basedOnMock },
           { QStringLiteral( "mock_input_count" ), mockInputs },
           { QStringLiteral( "validation" ), metrics.isEmpty() ? QStringLiteral( "unvalidated" )
                                                               : QStringLiteral( "validated" ) },
           { QStringLiteral( "confidence_semantics" ), QStringLiteral( "model_score" ) } };
}

QVariantMap build( const QVector<InputRef> &inputs, const QVariantMap &extra,
                   const QVariantMap &software )
{
  QVariantList inputList;
  QVariantList seismic;
  QVariantList records;
  for ( const InputRef &in : inputs )
  {
    QVariantMap m{ { QStringLiteral( "version_id" ), in.versionId },
                   { QStringLiteral( "asset_id" ), in.assetId },
                   { QStringLiteral( "name" ), in.name },
                   { QStringLiteral( "asset_type" ), in.assetType },
                   { QStringLiteral( "kind" ), in.kind },
                   { QStringLiteral( "version_number" ), in.versionNumber },
                   { QStringLiteral( "sha256" ), in.sha256 },
                   { QStringLiteral( "stage" ), in.stage },
                   { QStringLiteral( "mock" ), in.mock },
                   { QStringLiteral("role"), in.role },
                   { QStringLiteral("horizon"), in.horizon },
                   { QStringLiteral("missing"), in.missing } };
    inputList << m;
    auto record = m;
    const QString stableId = QStringLiteral("source:") + fingerprint(QVariantList{in.assetId, in.versionId});
    record.insert("id", stableId);
    record.insert("evidence_id", stableId);
    record.insert("source_version", in.versionId);
    record.insert("source_asset", in.assetId);
    record.insert("relation", in.role);
    record.insert("origin", "computed_input");
    QString type = QStringLiteral("reference");
    if (in.assetType.contains("well") || in.assetType == "core" || in.assetType == "interpretation") type = "well";
    else if (in.assetType.contains("seismic") || in.kind.contains("seismic")) type = "seismic";
    else if (in.kind.contains("factor") || in.assetType.contains("factor")) type = "single_factor";
    else if (in.kind.contains("constraint") || in.assetType.contains("constraint")) type = "constraint";
    record.insert("type", type);
    records << record;
    if ( in.assetType.contains( QLatin1String( "seismic" ) ) || in.kind.contains( QLatin1String( "seismic" ) ) ||
         in.assetType == QLatin1String( "horizon" ) )
      seismic << m;
  }
  const QVariantMap params = parameters( extra );
  QVariantMap model{ { QStringLiteral( "method" ), extra.value( QStringLiteral( "method" ), extra.value( QStringLiteral( "kind" ) ) ) } };
  for ( const char *k : { "model_names", "prediction_model", "algorithm_id", "request_id", "hierarchy_model",
                          "model_sha256", "model_hash", "execution_engine" } )
    if ( extra.contains( QLatin1String( k ) ) && !extra.value( QLatin1String( k ) ).isNull() )
      model.insert( QLatin1String( k ), extra.value( QLatin1String( k ) ) );
  if ( !model.contains( QStringLiteral( "model_names" ) ) && extra.contains( QStringLiteral( "model" ) ) )
  {
    const QVariant mval = extra.value( QStringLiteral( "model" ) );
    if ( mval.typeId() == QMetaType::QStringList )
      model.insert( QStringLiteral( "model_names" ), mval );
    else if ( !mval.toString().isEmpty() )
      model.insert( QStringLiteral( "model_names" ), QStringList{ mval.toString() } );
  }
  model.insert( QStringLiteral( "mock" ), extra.value( QStringLiteral( "mock" ), false ).toBool() );

  QVariantMap support;
  QVariantMap wells;
  if ( extra.contains( QStringLiteral( "wells" ) ) )
    wells.insert( QStringLiteral( "requested" ), extra.value( QStringLiteral( "wells" ) ) );
  for ( const char *k : { "supporting_wells", "skipped_wells", "predicted_well_count", "skipped_well_count" } )
    if ( extra.contains( QLatin1String( k ) ) )
      wells.insert( QLatin1String( k ), extra.value( QLatin1String( k ) ) );
  if ( !wells.isEmpty() )
    support.insert( QStringLiteral( "wells" ), wells );
  const QVariant window = pick( extra, { "window_half_ms", "horizon_source_format", "horizon_source_uri",
                                          "seismic_attributes", "valid_cells" } );
  if ( !seismic.isEmpty() || window.isValid() )
  {
    QVariantMap s{ { QStringLiteral( "inputs" ), seismic } };
    if ( window.isValid() )
      s.insert( QStringLiteral( "parameters" ), window );
    support.insert( QStringLiteral( "seismic_attributes" ), s );
  }
  if ( extra.contains( QStringLiteral( "source_layer" ) ) )
    support.insert( QStringLiteral( "source_layer" ), extra.value( QStringLiteral( "source_layer" ) ) );

  QVariantMap out{ { QStringLiteral( "schema" ), QStringLiteral( "paleo_evidence_v1" ) },
                   { QStringLiteral( "inputs" ), inputList },
                   { QStringLiteral( "model" ), model },
                   { QStringLiteral( "parameters" ), params },
                   { QStringLiteral( "parameter_hash" ), fingerprint( params ) },
                   { QStringLiteral( "support" ), support },
                   { QStringLiteral( "software" ), software } };
  out.insert("extension_version", 1);
  out.insert( QStringLiteral( "provenance" ), provenanceOf( inputs, extra ) );
  out.insert("records", records);
  if ( extra.contains( QStringLiteral( "facies" ) ) )
    out.insert( QStringLiteral( "facies_schema_hash" ), fingerprint( extra.value( QStringLiteral( "facies" ) ) ) );
  return out;
}

QString summary( const QVariantMap &evidence )
{
  if ( evidence.isEmpty() )
    return tr( "无证据链记录（早期版本）" );
  const auto findings = validate(evidence);
  for (const auto &finding : findings)
    if (finding.toMap().value("severity").toString() == "Blocking")
      return tr("证据链无法验证：%1").arg(finding.toMap().value("message").toString());
  const QVariantMap model = evidence.value( QStringLiteral( "model" ) ).toMap();
  const QVariantMap wells = evidence.value( QStringLiteral( "support" ) ).toMap().value( QStringLiteral( "wells" ) ).toMap();
  // 来源身份打头：真实模型 / 模拟 / 人工修订一眼可辨；旧证据（无 provenance
  // 块）不猜测来源，只按 model.mock 如实标模拟。
  const QVariantMap prov = evidence.value( QStringLiteral( "provenance" ) ).toMap();
  const QString pk = prov.value( QStringLiteral( "kind" ) ).toString();
  const bool basedOnMock = prov.value( QStringLiteral( "based_on_mock" ), model.value( QStringLiteral( "mock" ) ) ).toBool();
  QString head;
  if ( pk == QLatin1String( "manual_revision" ) )
    head = basedOnMock ? tr( "人工修订（底图为模拟结果）" ) : tr( "人工修订" );
  else if ( pk == QLatin1String( "mock" ) || ( pk.isEmpty() && basedOnMock ) )
    head = tr( "模拟/Mock 结果 · 不代表真实模型预测" );
  else if ( pk == QLatin1String( "model_prediction" ) )
    head = tr( "模型预测" );
  // derived（单因素等非预测派生）不加来源头，摘要与此前一致。
  if ( pk == QLatin1String( "model_prediction" ) || pk == QLatin1String( "manual_revision" ) )
    head += prov.value( QStringLiteral( "validation" ) ).toString() == QLatin1String( "validated" )
                ? tr( " · 有独立验证记录" )
                : tr( " · 尚无独立验证" );
  QString text = head.isEmpty() ? QString() : head + QStringLiteral( " · " );
  text += tr( "输入 %1 个版本 · 方法 %2 · 参数指纹 %3" )
                     .arg( evidence.value( QStringLiteral( "inputs" ) ).toList().size() )
                     .arg( model.value( QStringLiteral( "method" ) ).toString(),
                           evidence.value( QStringLiteral( "parameter_hash" ) ).toString().left( 12 ) );
  QString models = model.value( QStringLiteral( "model_names" ) ).toStringList().join( QStringLiteral( "、" ) );
  if ( models.isEmpty() && model.contains( QStringLiteral( "model" ) ) )
    models = model.value( QStringLiteral( "model" ) ).toString();
  if ( !models.isEmpty() )
    text += tr( " · 模型 %1" ).arg( models );
  if ( wells.contains( QStringLiteral( "supporting_wells" ) ) )
    text += tr( " · 支撑井 %1 口" ).arg( wells.value( QStringLiteral( "supporting_wells" ) ).toList().size() );
  return text;
}
QVariantList validate(const QVariantMap &evidence) {
  QVariantList findings;
  // 缺证据必须显式告警，不能和"证据完整且健康"同形。既有四个调用点只消费
  // "Blocking"，故这里降为 Warning 不改变任何默认发布/登记行为；但从此
  // 「无记录」与「记录齐全」在 API 层可区分（此前两者都返回空列表）。
  if (evidence.isEmpty()) {
    findings << QVariantMap{{"severity", QStringLiteral("Warning")},
                            {"code", QStringLiteral("no_evidence")},
                            {"message", tr("该版本没有证据链记录（早期版本），来源无法验证")},
                            {"source_version", QString()}};
    return findings;
  }
  auto add = [&](const QString &severity, const QString &code, const QString &message, const QString &version = {}) {
    findings << QVariantMap{{"severity", severity}, {"code", code}, {"message", message}, {"source_version", version}};
  };
  if (evidence.value("schema").toString() != "paleo_evidence_v1") {
    add("Blocking", "unknown_schema", tr("未知或损坏的证据 schema")); return findings;
  }
  if (evidence.value("inputs").typeId() != QMetaType::QVariantList || evidence.value("parameters").typeId() != QMetaType::QVariantMap ||
      evidence.value("model").typeId() != QMetaType::QVariantMap) {
    add("Blocking", "invalid_structure", tr("来源、参数或模型记录格式无效")); return findings;
  }
  QSet<QString> ids;
  for (const auto &input : evidence.value("inputs").toList()) {
    const auto row = input.toMap(); const auto version = row.value("version_id").toString();
    if (input.typeId() != QMetaType::QVariantMap || version.isEmpty() || ids.contains(version))
      add("Blocking", "invalid_source_identity", tr("来源版本身份缺失或重复"), version);
    ids.insert(version);
    // 文案须与真实行为一致：Blocking 会让登记事务整体 abort、发布被拒，
    // 引用根本不会生成——不是"保留原引用供追溯"。
    if (row.value("missing").toBool()) add("Blocking", "missing_source", tr("来源版本缺失，无法登记或发布该成果（引用未生成）"), version);
  }
  if (evidence.value("parameter_hash").toString() != fingerprint(evidence.value("parameters")))
    add("Blocking", "parameter_fingerprint", tr("参数与保存的指纹不一致"));
  return findings;
}
} // namespace paleo::evidence
