// 层：功能
#include "faciesmappingworkflow.h"

#include "derivedassets.h"
#include "workflows_internal.h"

#include "../catalog/datacatalog.h"
#include "../io/constraintstore.h"
#include "../qgis/qgislayerservice.h"

#include <qgscoordinatereferencesystem.h>
#include <qgsfeature.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace
{

QString joinEvidence( const std::vector<std::string> &items )
{
  QStringList parts;
  parts.reserve( static_cast<int>( items.size() ) );
  for ( const std::string &item : items )
    parts << QString::fromStdString( item );
  return parts.join( QLatin1Char( ';' ) );
}

// 草稿相图 GPKG：memory 图层 + QgsVectorFileWriter（writeStructuralSamples
// 同通道）。字段契约与 QA 报告/属性表对齐。
bool writeDraftGpkg( const QString &path, const QString &crsWkt,
                     const std::vector<paleo::faciesmapping::CandidateRegion> &regions,
                     const std::vector<paleo::faciesmapping::RegionSynthesis> &synthesis,
                     QString *error )
{
  using paleo::faciesmapping::CandidateRegion;
  using paleo::faciesmapping::RegionSynthesis;

  QgsVectorLayer layer( QStringLiteral( "Polygon" ), QStringLiteral( "facies_draft" ),
                        QStringLiteral( "memory" ) );
  if ( !layer.isValid() )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "草稿相图内存图层无法创建" ) );
    return false;
  }
  if ( !crsWkt.isEmpty() )
  {
    const QgsCoordinateReferenceSystem crs = QgsCoordinateReferenceSystem::fromWkt( crsWkt );
    if ( crs.isValid() )
      layer.setCrs( crs );
  }
  layer.startEditing();
  const QList<QPair<QString, QMetaType::Type>> fields = {
    { QStringLiteral( "region_id" ), QMetaType::Type::QString },
    { QStringLiteral( "facies_code" ), QMetaType::Type::Int },
    { QStringLiteral( "confidence" ), QMetaType::Type::Double },
    { QStringLiteral( "constraint_inherited" ), QMetaType::Type::Int },
    { QStringLiteral( "area_m2" ), QMetaType::Type::Double },
    { QStringLiteral( "contour_evidence" ), QMetaType::Type::QString },
    { QStringLiteral( "constraint_evidence" ), QMetaType::Type::QString },
    { QStringLiteral( "sources" ), QMetaType::Type::QString },
    { QStringLiteral( "vote_count" ), QMetaType::Type::Int },
  };
  for ( const auto &[name, type] : fields )
  {
    if ( !layer.addAttribute( QgsField( name, type ) ) )
    {
      paleo::workflow_detail::setError( error, QObject::tr( "草稿相图字段创建失败：%1" ).arg( name ) );
      return false;
    }
  }
  layer.updateFields();

  for ( std::size_t i = 0; i < regions.size(); ++i )
  {
    const CandidateRegion &region = regions[i];
    const RegionSynthesis *row =
        i < synthesis.size() ? &synthesis[i] : nullptr;
    QgsFeature feature( layer.fields() );
    QgsPolygonXY polygon;
    QVector<QgsPointXY> shell;
    for ( const paleo::singlefactor::Point2 &point : region.geometry.exterior.points )
      shell << QgsPointXY( point.x, point.y );
    polygon << shell;
    for ( const paleo::singlefactor::Ring &hole : region.geometry.holes )
    {
      QVector<QgsPointXY> ring;
      for ( const paleo::singlefactor::Point2 &point : hole.points )
        ring << QgsPointXY( point.x, point.y );
      polygon << ring;
    }
    feature.setGeometry( QgsGeometry::fromPolygonXY( polygon ) );
    feature.setAttribute( QStringLiteral( "region_id" ),
                          QString::fromStdString( region.regionId ) );
    feature.setAttribute( QStringLiteral( "facies_code" ),
                          row ? row->assignedCode : region.faciesCode );
    feature.setAttribute( QStringLiteral( "confidence" ),
                          row ? row->confidence : 0.0 );
    feature.setAttribute( QStringLiteral( "constraint_inherited" ),
                          row && row->constraintInherited ? 1 : 0 );
    feature.setAttribute( QStringLiteral( "area_m2" ), region.area );
    feature.setAttribute( QStringLiteral( "contour_evidence" ),
                          joinEvidence( region.contourEvidence ) );
    feature.setAttribute( QStringLiteral( "constraint_evidence" ),
                          joinEvidence( region.constraintEvidence ) );
    feature.setAttribute( QStringLiteral( "sources" ),
                          row ? joinEvidence( row->contributingSources ) : QString() );
    feature.setAttribute( QStringLiteral( "vote_count" ), row ? row->voteCount : 0 );
    if ( !layer.addFeature( feature ) )
    {
      paleo::workflow_detail::setError( error, QObject::tr( "草稿相图要素写入失败：%1" )
                                              .arg( QString::fromStdString( region.regionId ) ) );
      return false;
    }
  }
  if ( !layer.commitChanges() )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "草稿相图内存图层提交失败" ) );
    return false;
  }

  QgsVectorFileWriter::SaveVectorOptions options;
  options.driverName = QStringLiteral( "GPKG" );
  options.layerName = QStringLiteral( "facies_draft" );
  options.fileEncoding = QStringLiteral( "UTF-8" );
  options.actionOnExistingFile = QgsVectorFileWriter::CreateOrOverwriteFile;
  return QgsVectorFileWriter::writeAsVectorFormatV3(
             &layer, path, QgsCoordinateTransformContext(), options, error ) ==
         QgsVectorFileWriter::NoError;
}

QJsonObject issueToJson( const paleo::faciesmapping::FaciesQaIssue &issue )
{
  QJsonObject json;
  json.insert( QStringLiteral( "type" ),
               QString::fromLatin1( paleo::faciesmapping::faciesQaIssueName( issue.type ) ) );
  QJsonArray regionIds;
  for ( const std::string &id : issue.regionIds )
    regionIds.append( QString::fromStdString( id ) );
  json.insert( QStringLiteral( "region_ids" ), regionIds );
  QJsonArray relatedIds;
  for ( const std::string &id : issue.relatedIds )
    relatedIds.append( QString::fromStdString( id ) );
  json.insert( QStringLiteral( "related_ids" ), relatedIds );
  json.insert( QStringLiteral( "metric" ), issue.metric );
  QJsonObject location;
  location.insert( QStringLiteral( "x" ), issue.location.x );
  location.insert( QStringLiteral( "y" ), issue.location.y );
  json.insert( QStringLiteral( "location" ), location );
  return json;
}

} // namespace

FaciesMappingWorkflow::FaciesMappingWorkflow( DataCatalog *catalog, const QString &projectDir,
                                              QObject *parent )
  : QObject( parent ), m_catalog( catalog ), m_projectDir( projectDir )
{
}

void FaciesMappingWorkflow::rebind( DataCatalog *catalog, const QString &projectDir )
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

void FaciesMappingWorkflow::attachLayerService( QgisLayerService *layers )
{
  m_layers = layers;
}

void FaciesMappingWorkflow::setConstraintStore( ConstraintStore *store )
{
  m_constraints = store;
}

namespace
{

// WKT 多边形 → 算法层 Polygon 环（QgsGeometry 承担解析；无效 WKT → false）。
bool wktToPolygons( const QString &wkt, std::vector<paleo::singlefactor::Polygon> *out )
{
  const QgsGeometry geometry = QgsGeometry::fromWkt( wkt );
  if ( geometry.isNull() || geometry.type() != Qgis::GeometryType::Polygon )
    return false;
  const QVector<QgsPolygonXY> parts = geometry.isMultipart()
                                        ? geometry.asMultiPolygon()
                                        : QVector<QgsPolygonXY>{ geometry.asPolygon() };
  for ( const QgsPolygonXY &part : parts )
  {
    if ( part.isEmpty() || part.first().size() < 3 )
      continue;
    paleo::singlefactor::Polygon polygon;
    for ( const QgsPointXY &point : part.first() )
      polygon.exterior.points.push_back( { point.x(), point.y() } );
    for ( int ring = 1; ring < part.size(); ++ring )
    {
      paleo::singlefactor::Ring hole;
      for ( const QgsPointXY &point : part.at( ring ) )
        hole.points.push_back( { point.x(), point.y() } );
      polygon.holes.push_back( std::move( hole ) );
    }
    out->push_back( std::move( polygon ) );
  }
  return !out->empty();
}

bool wktToPoints( const QString &wkt, std::vector<paleo::singlefactor::Point2> *out )
{
  const QgsGeometry geometry = QgsGeometry::fromWkt( wkt );
  if ( geometry.isNull() || geometry.type() != Qgis::GeometryType::Line )
    return false;
  const QgsPolylineXY line = geometry.asPolyline();
  if ( line.size() < 2 )
    return false;
  for ( const QgsPointXY &point : line )
    out->push_back( { point.x(), point.y() } );
  return true;
}

} // namespace

bool FaciesMappingWorkflow::assembleConstraintInputs( DraftFaciesRequest *request,
                                                      QString *error ) const
{
  if ( !request )
  {
    paleo::workflow_detail::setError( error, tr( "装配请求为空" ) );
    return false;
  }
  if ( request->horizon.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "装配约束输入需要层位" ) );
    return false;
  }
  if ( !m_constraints )
  {
    paleo::workflow_detail::setError(
      error, tr( "约束库未绑定——无法从工程装配相区/硬约束线" ) );
    return false;
  }

  const QVector<QVariantMap> rows = m_constraints->load( request->horizon );
  int zoneCount = 0;
  int lineCount = 0;
  for ( const QVariantMap &row : rows )
  {
    const QString type = row.value( QStringLiteral( "type" ) ).toString();
    const QString wkt = row.value( QStringLiteral( "wkt" ) ).toString();
    const QString id = row.value( QStringLiteral( "id" ) ).toString();
    if ( wkt.isEmpty() )
      continue;
    if ( type == QStringLiteral( "polygon" ) )
    {
      const int faciesCode = row.value( QStringLiteral( "facies_code" ) ).toInt();
      if ( faciesCode < 0 )
        continue; // 未定相多边形不构成相区证据
      ZoneInput zone;
      zone.id = id;
      zone.faciesCode = faciesCode;
      if ( !wktToPolygons( wkt, &zone.polygons ) )
        continue; // 坏 WKT 行跳过——装配是聚合口径，单行坏不整体失败
      request->zones.push_back( std::move( zone ) );
      zoneCount++;
    }
    else if ( type == QStringLiteral( "break_line" ) )
    {
      HardLineInput line;
      line.id = id;
      if ( !wktToPoints( wkt, &line.points ) )
        continue;
      request->hardLines.push_back( std::move( line ) );
      lineCount++;
    }
  }

  // 编图域缺省 = 相区并集（约束驱动的诚实范围；有显式域时不覆盖）。
  if ( request->domain.empty() )
  {
    for ( const ZoneInput &zone : request->zones )
      for ( const paleo::singlefactor::Polygon &polygon : zone.polygons )
        request->domain.push_back( polygon );
  }

  if ( zoneCount == 0 && lineCount == 0 )
  {
    paleo::workflow_detail::setError(
      error, tr( "层位 %1 的约束库里没有可用的相区/硬约束线" ).arg( request->horizon ) );
    return false;
  }
  return true;
}

FaciesMappingWorkflow::DraftFaciesComputed FaciesMappingWorkflow::runCompute(
  const DraftFaciesRequest &request,
  const std::function<bool( double, const QString & )> &progress )
{
  using namespace paleo::faciesmapping;
  using paleo::singlefactor::Status;

  auto fail = [progress]( const QString &message ) {
    DraftFaciesComputed computed;
    computed.ok = false;
    computed.error = message;
    if ( progress )
      progress( 1.0, message );
    return computed;
  };
  auto report = [&progress]( double fraction, const QString &stage ) {
    return !progress || progress( fraction, stage );
  };

  if ( request.horizon.isEmpty() )
    return fail( tr( "草稿相图需要层位" ) );
  if ( request.domain.empty() && request.contours.empty() )
    return fail( tr( "草稿相图需要编图域或等值线输入" ) );

  // 取消语义走 JobRunner 框架的 CancelFn（同 PropertyModelWorkflow 现状）；
  // 算法核对空 Control 安全，这里不再伪造进度回调做取消通道。
  paleo::singlefactor::Control control;

  // ---- 阶段1：优势相统计（井相柱 → 逐井优势相/覆盖率） -------------------
  DominantFaciesResult dominant = computeDominantFacies( request.wellColumns,
                                                         request.dominantOptions,
                                                         &control );
  if ( dominant.status != Status::Ok )
    return fail( tr( "优势相统计失败：%1" )
                     .arg( QString::fromStdString( dominant.message ) ) );
  if ( !report( 0.2, tr( "优势相统计完成" ) ) )
    return fail( tr( "已取消" ) );

  // ---- 阶段2：候选相界提取（等值线 × 约束区 → 候选单元） -----------------
  CandidateBoundaryRequest boundaryRequest;
  boundaryRequest.domain = request.domain;
  boundaryRequest.contours = request.contours;
  boundaryRequest.minArea = request.minRegionArea;
  for ( const ZoneInput &zone : request.zones )
  {
    FaciesZone entry;
    entry.id = zone.id.toStdString();
    entry.faciesCode = zone.faciesCode;
    entry.polygons = zone.polygons;
    boundaryRequest.zones.push_back( std::move( entry ) );
  }
  CandidateBoundaryResult boundaries = extractCandidateRegions( boundaryRequest, &control );
  if ( boundaries.status != Status::Ok )
    return fail( tr( "候选相界提取失败：%1" )
                     .arg( QString::fromStdString( boundaries.message ) ) );
  if ( boundaries.regions.empty() )
    return fail( tr( "候选相界提取未产生任何闭合单元（检查等值线闭合与编图域）" ) );
  if ( !report( 0.45, tr( "候选相界提取完成" ) ) )
    return fail( tr( "已取消" ) );

  // ---- 阶段3：证据合成（优势相点票 + 附加源加权） -------------------------
  std::vector<EvidenceSource> sources;
  EvidenceSource wellSource;
  wellSource.id = "well_facies";
  wellSource.kind = "well";
  wellSource.weight = request.wellWeight;
  for ( const DominantFaciesRow &row : dominant.rows )
  {
    if ( row.dominantCode < 0 )
      continue; // 门槛拦截/证据不足的井不投票（诚实：缺证据不冒充）
    EvidenceSample sample;
    sample.x = row.x;
    sample.y = row.y;
    sample.faciesCode = row.dominantCode;
    sample.confidence = std::clamp( row.coverage, 0.0, 1.0 );
    sample.weightBoost = std::max( row.dominance, 0.0 );
    wellSource.samples.push_back( sample );
  }
  int wellVoters = static_cast<int>( wellSource.samples.size() );
  sources.push_back( std::move( wellSource ) );
  for ( const SampleSourceInput &input : request.extraSources )
  {
    EvidenceSource source;
    source.id = input.id.toStdString();
    source.kind = input.kind.toStdString();
    // factor/prediction 权重由请求给定；kind 不可识别时仍按显式权重。
    source.weight = input.kind == QStringLiteral( "prediction" ) ? request.predictionWeight
                                                                 : request.factorWeight;
    for ( const SamplePoint &point : input.points )
    {
      EvidenceSample sample;
      sample.x = point.x;
      sample.y = point.y;
      sample.faciesCode = point.faciesCode;
      sample.confidence = point.confidence;
      source.samples.push_back( sample );
    }
    sources.push_back( std::move( source ) );
  }
  SynthesisOptions synthesisOptions;
  synthesisOptions.assignThreshold = request.assignThreshold;
  SynthesisResult synthesis = synthesizeFaciesRegions( boundaries.regions, sources,
                                                       synthesisOptions, &control );
  if ( synthesis.status != Status::Ok )
    return fail( tr( "证据合成失败：%1" )
                     .arg( QString::fromStdString( synthesis.message ) ) );
  if ( !report( 0.7, tr( "证据合成完成" ) ) )
    return fail( tr( "已取消" ) );

  // ---- 阶段4：编图一致性 QA ----------------------------------------------
  std::vector<FaciesMapUnit> units;
  units.reserve( boundaries.regions.size() );
  for ( const CandidateRegion &region : boundaries.regions )
  {
    FaciesMapUnit unit;
    unit.regionId = region.regionId;
    unit.faciesCode = region.faciesCode;
    unit.geometry = region.geometry;
    unit.area = region.area;
    units.push_back( std::move( unit ) );
  }
  std::vector<QaWellPoint> wells;
  wells.reserve( request.wellColumns.size() );
  for ( const WellFaciesColumn &column : request.wellColumns )
    wells.push_back( QaWellPoint{ column.x, column.y } );
  std::vector<QaConstraintLine> hardLines;
  for ( const HardLineInput &line : request.hardLines )
  {
    QaConstraintLine entry;
    entry.id = line.id.toStdString();
    entry.points = line.points;
    hardLines.push_back( std::move( entry ) );
  }
  FaciesQaOptions qaOptions;
  qaOptions.minIslandArea = request.minIslandArea;
  qaOptions.wellCoverageRadius = request.wellCoverageRadius;
  FaciesQaResult qa = runFaciesQa( units, wells, hardLines, qaOptions, &control );
  if ( qa.status != Status::Ok )
    return fail( tr( "QA 检测失败：%1" ).arg( QString::fromStdString( qa.message ) ) );
  if ( !report( 0.9, tr( "QA 检测完成" ) ) )
    return fail( tr( "已取消" ) );

  // ---- extra（权重/阈值 = job params；随版本落库可复现） ------------------
  DraftFaciesComputed computed;
  computed.ok = true;
  computed.regions = std::move( boundaries.regions );
  computed.synthesis = std::move( synthesis.regions );
  computed.qaIssues = std::move( qa.issues );
  computed.qaDiagnostics = qa.diagnostics;
  computed.dominantDiagnostics = dominant.diagnostics;
  computed.boundaryDiagnostics = boundaries.diagnostics;
  computed.dominantRows = std::move( dominant.rows );
  computed.wellVoters = wellVoters;

  QVariantMap params;
  params.insert( QStringLiteral( "horizon" ), request.horizon );
  params.insert( QStringLiteral( "well_weight" ), request.wellWeight );
  params.insert( QStringLiteral( "factor_weight" ), request.factorWeight );
  params.insert( QStringLiteral( "prediction_weight" ), request.predictionWeight );
  params.insert( QStringLiteral( "assign_threshold" ), request.assignThreshold );
  params.insert( QStringLiteral( "min_region_area" ), request.minRegionArea );
  params.insert( QStringLiteral( "min_island_area" ), request.minIslandArea );
  params.insert( QStringLiteral( "well_coverage_radius" ), request.wellCoverageRadius );
  params.insert( QStringLiteral( "dominant_min_coverage" ),
                 request.dominantOptions.minCoverage );
  params.insert( QStringLiteral( "dominant_min_dominance" ),
                 request.dominantOptions.minDominance );
  params.insert( QStringLiteral( "well_columns" ),
                 static_cast<int>( request.wellColumns.size() ) );
  params.insert( QStringLiteral( "contour_levels" ),
                 static_cast<int>( request.contours.size() ) );
  params.insert( QStringLiteral( "zones" ), static_cast<int>( request.zones.size() ) );
  params.insert( QStringLiteral( "hard_lines" ),
                 static_cast<int>( request.hardLines.size() ) );

  QCryptographicHash hash( QCryptographicHash::Sha256 );
  hash.addData( QJsonDocument( QJsonObject::fromVariantMap( params ) ).toJson(
    QJsonDocument::Compact ) );
  computed.extra = params;
  computed.extra.insert( QStringLiteral( "algorithm_id" ),
                         QStringLiteral( "paleo:facies_draft_mapping" ) );
  computed.extra.insert( QStringLiteral( "semantic_profile" ),
                         QStringLiteral( "facies_draft_mapping_v1" ) );
  computed.extra.insert( QStringLiteral( "param_hash" ),
                         QString::fromLatin1( hash.result().toHex() ) );
  computed.extra.insert( QStringLiteral( "region_count" ),
                         static_cast<int>( computed.regions.size() ) );
  computed.extra.insert( QStringLiteral( "qa_issue_count" ),
                         static_cast<int>( computed.qaIssues.size() ) );
  computed.extra.insert( QStringLiteral( "well_voters" ), wellVoters );
  return computed;
}

bool FaciesMappingWorkflow::commitComputed( const DraftFaciesRequest &request,
                                            DraftFaciesComputed *computed )
{
  auto fail = [this, computed]( const QString &message ) {
    computed->error = message;
    emit draftFailed( message );
    return false;
  };
  if ( !computed || !computed->ok )
    return fail( computed ? computed->error : tr( "无计算结果" ) );
  if ( m_layers.isNull() )
    return fail( tr( "草稿相图工作流未绑定图层服务" ) );

  DerivedAssetRegistrar registrar( m_catalog, m_projectDir );
  if ( !registrar.isBound() )
    return fail( tr( "工作流未绑定数据目录（catalog）——草稿相图无法登记" ) );

  const QString h = request.horizon;

  // ---- 草稿相图 GPKG：DERIVED 资产 + 版本 --------------------------------
  QString regErr;
  const DerivedStaging draftStaging = registrar.stage(
    QStringLiteral( "facies_draft_map" ), tr( "%1 草稿相图" ).arg( h ),
    QStringLiteral( "FACIES_DRAFT_%1.gpkg" ).arg( h ), &regErr );
  if ( !draftStaging.isValid() )
    return fail( regErr );
  QString gpkgErr;
  if ( !writeDraftGpkg( draftStaging.absolutePath, request.crsWkt, computed->regions,
                        computed->synthesis, &gpkgErr ) )
    return fail( gpkgErr );
  const QStringList parents = computed->parentPaths.isEmpty()
                                ? QStringList()
                                : registrar.parentVersionIdsFor( computed->parentPaths );
  QVariantMap draftExtra = computed->extra;
  draftExtra.insert( QStringLiteral( "asset_role" ), QStringLiteral( "draft" ) );
  QString commitErr;
  if ( !registrar.commit( draftStaging, parents,
                          QStringLiteral( "paleo:facies_draft_mapping" ), draftExtra,
                          &commitErr ) )
    return fail( commitErr );

  // ---- QA 报告 JSON：DERIVED 资产 + 版本 ---------------------------------
  const DerivedStaging reportStaging = registrar.stage(
    QStringLiteral( "facies_qa_report" ), tr( "%1 编图 QA 报告" ).arg( h ),
    QStringLiteral( "FACIES_QA_%1.json" ).arg( h ), &regErr );
  if ( !reportStaging.isValid() )
    return fail( regErr );
  QJsonObject report;
  report.insert( QStringLiteral( "horizon" ), h );
  report.insert( QStringLiteral( "semantic_profile" ),
                 QStringLiteral( "facies_draft_mapping_v1" ) );
  report.insert( QStringLiteral( "params" ),
                 QJsonObject::fromVariantMap( computed->extra ) );
  report.insert( QStringLiteral( "dominant_diagnostics" ),
                 QJsonObject::fromVariantMap( computed->dominantDiagnostics ) );
  report.insert( QStringLiteral( "boundary_diagnostics" ),
                 QJsonObject::fromVariantMap( computed->boundaryDiagnostics ) );
  report.insert( QStringLiteral( "qa_diagnostics" ),
                 QJsonObject::fromVariantMap( computed->qaDiagnostics ) );
  report.insert( QStringLiteral( "dominant_rows" ),
                 static_cast<int>( computed->dominantRows.size() ) );
  QJsonArray issues;
  for ( const paleo::faciesmapping::FaciesQaIssue &issue : computed->qaIssues )
    issues.append( issueToJson( issue ) );
  report.insert( QStringLiteral( "issues" ), issues );
  QFile reportFile( reportStaging.absolutePath );
  if ( !reportFile.open( QIODevice::WriteOnly ) )
    return fail( tr( "QA 报告写入失败：%1" ).arg( reportStaging.absolutePath ) );
  if ( reportFile.write( QJsonDocument( report ).toJson( QJsonDocument::Indented ) ) < 0 )
  {
    reportFile.close();
    return fail( tr( "QA 报告写入失败：%1" ).arg( reportStaging.absolutePath ) );
  }
  reportFile.close();
  QVariantMap reportExtra;
  reportExtra.insert( QStringLiteral( "asset_role" ), QStringLiteral( "qa_report" ) );
  reportExtra.insert( QStringLiteral( "issue_count" ),
                      static_cast<int>( computed->qaIssues.size() ) );
  reportExtra.insert( QStringLiteral( "param_hash" ),
                      computed->extra.value( QStringLiteral( "param_hash" ) ) );
  reportExtra.insert( QStringLiteral( "draft_asset_id" ), draftStaging.assetId );
  reportExtra.insert( QStringLiteral( "draft_version_id" ), draftStaging.versionId );
  if ( !registrar.commit( reportStaging, { draftStaging.versionId },
                          QStringLiteral( "paleo:facies_draft_mapping" ), reportExtra,
                          &commitErr ) )
    return fail( commitErr );

  // ---- declare：正常图层管线（不产游离图层） -----------------------------
  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "facies_draft.%1" ).arg( h );
  decl.horizon = h;
  decl.type = QStringLiteral( "vector" );
  decl.source = QStringLiteral( "%1|layername=facies_draft" ).arg( draftStaging.absolutePath );
  decl.group = QStringLiteral( "05_PaleoMap" );
  QString declareErr;
  if ( !m_layers->declare( decl, &declareErr ) )
    return fail( declareErr.isEmpty() ? tr( "草稿相图图层声明失败：%1" ).arg( decl.layerId )
                                      : declareErr );
  paleo::workflow_detail::stampLayerAssetLink( m_layers, decl.layerId,
                                               draftStaging.assetId );

  emit draftReady( h, decl.layerId );
  emit qaReportReady( reportStaging.absolutePath,
                      static_cast<int>( computed->qaIssues.size() ) );
  return true;
}

bool FaciesMappingWorkflow::run( const DraftFaciesRequest &request, QString *error )
{
  DraftFaciesComputed computed = runCompute( request );
  if ( !computed.ok )
  {
    paleo::workflow_detail::setError( error, computed.error );
    emit draftFailed( computed.error );
    return false;
  }
  if ( !commitComputed( request, &computed ) )
  {
    paleo::workflow_detail::setError( error, computed.error );
    return false;
  }
  return true;
}

PaleoTask *FaciesMappingWorkflow::startJob( paleo::jobs::JobRunner<DraftFaciesJob> &runner,
                                            const DraftFaciesRequest &request,
                                            QObject *progressSink,
                                            std::shared_ptr<DraftFaciesJob> *started )
{
  using paleo::jobs::JobRunner;

  auto job = std::make_shared<DraftFaciesJob>();
  job->request = request;

  JobRunner<DraftFaciesJob>::Callbacks cb;
  // prepare：owner 线程——输入快照已由调用方在 owner 线程抓成纯数据。
  cb.prepare = []( DraftFaciesJob &, QString * ) { return true; };
  // compute：worker 线程纯计算（四阶段算法核，不碰 catalog/UI）。
  auto *sink = progressSink;
  cb.compute = [this, sink]( DraftFaciesJob &j, const paleo::jobs::CancelFn &,
                           const paleo::jobs::ProgressFn & ) {
    j.computed = runCompute( j.request, [sink]( double fraction, const QString &stage ) {
      if ( sink )
      {
        const int pct = fraction <= 0.0
                          ? 0
                          : ( fraction >= 1.0 ? 100
                                              : static_cast<int>( fraction * 100.0 + 0.5 ) );
        QMetaObject::invokeMethod( sink, "updateProgress", Qt::QueuedConnection,
                                  Q_ARG( int, pct ), Q_ARG( QString, stage ) );
      }
      return true; // 取消判定交给框架的 CancelFn
    } );
    return j.computed.ok;
  };
  // commit：owner 线程登记（#106 owner-thread 写守卫面）。
  cb.commit = [this]( DraftFaciesJob &j, QString * ) {
    return commitComputed( j.request, &j.computed );
  };
  // 取消/陈旧不进 commit（发布是临界区）；staging 由 registrar 托管。

  PaleoTask *task = runner.start( QStringLiteral( "草稿相图" ), job, cb, QString(),
                                 /*quiet=*/true );
  if ( started )
    *started = task ? job : nullptr;
  return task;
}
