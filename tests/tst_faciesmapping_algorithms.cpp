// 层：测试壳（被测对象：paleo_algorithms faciesmapping 四个纯计算核——
// 优势相统计/候选相界提取/证据合成/QA 检测器；goal/facies-automapping）
#include <QtTest/QtTest>

#include "algorithms/faciesmapping/candidateboundaries.h"
#include "algorithms/faciesmapping/dominantfacies.h"
#include "algorithms/faciesmapping/evidencesynthesis.h"
#include "algorithms/faciesmapping/faciesqa.h"

#include <algorithm>
#include <cmath>

using namespace paleo::faciesmapping;
using paleo::singlefactor::ContourLevelLines;
using paleo::singlefactor::Point2;
using paleo::singlefactor::Polygon;

namespace
{

Polygon rect( double x0, double y0, double x1, double y1 )
{
  Polygon poly;
  poly.exterior.points = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
  return poly;
}

// 不闭合的矩形（首尾不重合）——QA UnclosedRing 的构造缺陷。
Polygon openRect( double x0, double y0, double x1, double y1 )
{
  Polygon poly;
  poly.exterior.points = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
  return poly;
}

FaciesInterval interval( double top, double bottom, int code, double confidence = 1.0 )
{
  FaciesInterval i;
  i.top = top;
  i.bottom = bottom;
  i.faciesCode = code;
  i.confidence = confidence;
  return i;
}

const DominantFaciesRow *rowFor( const DominantFaciesResult &result, const char *wellId )
{
  for ( const DominantFaciesRow &row : result.rows )
    if ( row.wellId == wellId )
      return &row;
  return nullptr;
}

const FaciesFrequency *freqFor( const DominantFaciesRow &row, int code )
{
  for ( const FaciesFrequency &freq : row.frequencies )
    if ( freq.faciesCode == code )
      return &freq;
  return nullptr;
}

const FaciesQaIssue *firstIssue( const FaciesQaResult &result, FaciesQaIssueType type )
{
  for ( const FaciesQaIssue &issue : result.issues )
    if ( issue.type == type )
      return &issue;
  return nullptr;
}

} // namespace

class TestFaciesMappingAlgorithms : public QObject
{
  Q_OBJECT

  private slots:
    // ---- 阶段1：优势相统计 ------------------------------------------------
    void dominantFrequenciesAndCoverage();
    void dominantOverlapTopmostPolicyAndTie();
    void dominantConfidenceWeighting();
    void dominantGatesAndEmptyEvidence();
    void dominantRejectsEmptyInput();

    // ---- 阶段2：候选相界提取 ----------------------------------------------
    void candidateRegionsSplitByContourAndZone();
    void candidateRegionsZoneConflictEmittedHonestly();
    void candidateRegionsResidualGapPreserved();
    void candidateRegionsMinAreaDropsFragments();
    void candidateRegionsOpenContourWithoutDomain();

    // ---- 阶段3：证据合成 ---------------------------------------------------
    void synthesisWeightedVote();
    void synthesisThresholdBlocksAssignment();
    void synthesisConstraintInheritance();
    void synthesisRejectsEmptyRegions();

    // ---- 阶段4：QA 检测器 --------------------------------------------------
    void qaDetectsUnclosedOverlapIsland();
    void qaConstraintCrossingLocated();
    void qaWellCoverageRadius();
    void qaReproducibleDiagnostics();

    // ---- 方向 39：按相界类型（boundaryKind）核查 ----------------------------
    void qaPinchoutOpenEndExemptAndTipDangling();
    void qaTransitionBandMissingForFaciesChange();
    void qaConformableCutFaciesDetected();
};

// ---- 阶段1 ------------------------------------------------------------------

void TestFaciesMappingAlgorithms::dominantFrequenciesAndCoverage()
{
  WellFaciesColumn well;
  well.wellId = "W1";
  well.x = 1;
  well.y = 2;
  well.top = 1000;
  well.bottom = 1100;
  well.intervals = { interval( 1000, 1030, 1 ), interval( 1030, 1060, 2 ),
                     interval( 1070, 1090, 1 ) }; // 1060-1070 缺失段

  DominantFaciesResult result = computeDominantFacies( { well } );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  QCOMPARE( result.rows.size(), size_t( 1 ) );

  const DominantFaciesRow &row = result.rows.front();
  QCOMPARE( row.wellId, std::string( "W1" ) );
  QCOMPARE( row.dominantCode, 1 );
  // 逐格断言：分类厚度 80/100，覆盖率 0.8。
  QCOMPARE( row.classifiedThickness, 80.0 );
  QCOMPARE( row.coverage, 0.8 );
  QCOMPARE( row.horizonThickness, 100.0 );
  QVERIFY( !row.tie );
  // 相 1：厚 50，占比 0.625；相 2：厚 30，占比 0.375。
  const FaciesFrequency *f1 = freqFor( row, 1 );
  const FaciesFrequency *f2 = freqFor( row, 2 );
  QVERIFY( f1 && f2 );
  QCOMPARE( f1->thickness, 50.0 );
  QCOMPARE( f1->fraction, 0.625 );
  QCOMPARE( f2->thickness, 30.0 );
  QCOMPARE( f2->fraction, 0.375 );
  // 频率按厚度降序。
  QCOMPARE( row.frequencies.front().faciesCode, 1 );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "overlap_policy" ) ).toString(),
            QStringLiteral( "topmost_wins_no_double_count" ) );
}

void TestFaciesMappingAlgorithms::dominantOverlapTopmostPolicyAndTie()
{
  WellFaciesColumn well;
  well.wellId = "W1";
  well.top = 1000;
  well.bottom = 1040;
  // 乱序 + 重叠：c1 [1000,1020)、c2 [1010,1040)——最上优先：c1 得 20、c2 得 20。
  well.intervals = { interval( 1010, 1040, 2 ), interval( 1000, 1020, 1 ) };

  DominantFaciesResult result = computeDominantFacies( { well } );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  const DominantFaciesRow &row = result.rows.front();
  QCOMPARE( row.classifiedThickness, 40.0 ); // 不双算
  QCOMPARE( row.coverage, 1.0 );
  const FaciesFrequency *f1 = freqFor( row, 1 );
  const FaciesFrequency *f2 = freqFor( row, 2 );
  QVERIFY( f1 && f2 );
  QCOMPARE( f1->thickness, 20.0 );
  QCOMPARE( f2->thickness, 20.0 );
  // 得分并列 → tie 如实标记，优势相取小 code。
  QVERIFY( row.tie );
  QCOMPARE( row.dominantCode, 1 );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "tie_rows" ) ).toInt(), 1 );
}

void TestFaciesMappingAlgorithms::dominantConfidenceWeighting()
{
  WellFaciesColumn well;
  well.wellId = "W1";
  well.top = 1000;
  well.bottom = 1070;
  // c1 厚 40 但置信 0.5（得分 20）；c2 厚 30 置信 1.0（得分 30）——
  // 厚度占比 c1 更大，优势裁决按得分归 c2（诚实语义：弱证据不冒充优势）。
  well.intervals = { interval( 1000, 1040, 1, 0.5 ), interval( 1040, 1070, 2, 1.0 ) };

  DominantFaciesResult result = computeDominantFacies( { well } );
  const DominantFaciesRow &row = result.rows.front();
  QCOMPARE( row.dominantCode, 2 );
  QCOMPARE( row.dominance, 0.6 ); // 30 / (20+30)
  const FaciesFrequency *f1 = freqFor( row, 1 );
  QCOMPARE( f1->fraction, 40.0 / 70.0 ); // 频率口径仍是真实厚度占比
}

void TestFaciesMappingAlgorithms::dominantGatesAndEmptyEvidence()
{
  WellFaciesColumn partial;
  partial.wellId = "WP";
  partial.top = 1000;
  partial.bottom = 1100;
  partial.intervals = { interval( 1000, 1080, 3 ) }; // 覆盖率 0.8

  WellFaciesColumn missing;
  missing.wellId = "WM";
  missing.top = 2000;
  missing.bottom = 2100;
  missing.intervals = { interval( 2000, 2050, -1 ) }; // 未定相：不进频率

  DominantFaciesOptions options;
  options.minCoverage = 0.9;
  DominantFaciesResult result = computeDominantFacies( { partial, missing }, options );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  QCOMPARE( result.rows.size(), size_t( 2 ) );

  const DominantFaciesRow *rowP = rowFor( result, "WP" );
  QVERIFY( rowP );
  QCOMPARE( rowP->coverage, 0.8 );
  QCOMPARE( rowP->dominantCode, -1 ); // 门槛拦截：不赋相，统计照记
  QVERIFY( !rowP->frequencies.empty() );
  const DominantFaciesRow *rowM = rowFor( result, "WM" );
  QVERIFY( rowM );
  QCOMPARE( rowM->coverage, 0.0 ); // 未定相段只推进 cursor，不算覆盖
  QCOMPARE( rowM->dominantCode, -1 );
  QVERIFY( rowM->frequencies.empty() );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "gated_rows" ) ).toInt(), 1 );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "no_code_rows" ) ).toInt(), 1 );
}

void TestFaciesMappingAlgorithms::dominantRejectsEmptyInput()
{
  DominantFaciesResult result = computeDominantFacies( {} );
  QCOMPARE( result.status, paleo::singlefactor::Status::InvalidInput );
  QVERIFY( result.rows.empty() );
}

// ---- 阶段2 ------------------------------------------------------------------

void TestFaciesMappingAlgorithms::candidateRegionsSplitByContourAndZone()
{
  CandidateBoundaryRequest request;
  request.domain = { rect( 0, 0, 10, 10 ) };
  ContourLevelLines level;
  level.level = 5.0;
  level.lines = { { { { 5, 0 }, { 5, 10 } } } };
  request.contours = { level };
  FaciesZone west;
  west.id = "zone_west";
  west.faciesCode = 10;
  west.polygons = { rect( 0, 0, 5, 10 ) };
  FaciesZone east;
  east.id = "zone_east";
  east.faciesCode = 20;
  east.polygons = { rect( 5, 0, 10, 10 ) };
  request.zones = { west, east };

  const CandidateBoundaryResult result = extractCandidateRegions( request );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  QCOMPARE( result.regions.size(), size_t( 2 ) );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "face_count" ) ).toInt(), 2 );

  double area10 = 0;
  double area20 = 0;
  for ( const CandidateRegion &region : result.regions )
  {
    // 证据标记完整：两侧都以 level:5#0 为界。
    QCOMPARE( region.contourEvidence.size(), size_t( 1 ) );
    QCOMPARE( region.contourEvidence.front(), std::string( "level:5#0" ) );
    QVERIFY( region.touchesDomainEdge );
    QCOMPARE( region.geometry.exterior.points.size(), size_t( 5 ) ); // 闭合四边形
    if ( region.faciesCode == 10 )
    {
      area10 = region.area;
      QCOMPARE( region.constraintEvidence.front(), std::string( "zone_west" ) );
    }
    else
    {
      area20 = region.area;
      QCOMPARE( region.faciesCode, 20 );
      QCOMPARE( region.constraintEvidence.front(), std::string( "zone_east" ) );
    }
  }
  // 面积断言：中缝等值线各分 50。
  QCOMPARE( area10, 50.0 );
  QCOMPARE( area20, 50.0 );
}

void TestFaciesMappingAlgorithms::candidateRegionsZoneConflictEmittedHonestly()
{
  CandidateBoundaryRequest request;
  request.domain = { rect( 0, 0, 10, 10 ) };
  FaciesZone a;
  a.id = "za";
  a.faciesCode = 1;
  a.polygons = { rect( 0, 0, 6, 10 ) };
  FaciesZone b;
  b.id = "zb";
  b.faciesCode = 2;
  b.polygons = { rect( 4, 0, 10, 10 ) };
  request.zones = { a, b };

  const CandidateBoundaryResult result = extractCandidateRegions( request );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  QCOMPARE( result.regions.size(), size_t( 2 ) ); // 重叠双方各出一块，不静默合并
  QCOMPARE( result.diagnostics.value( QStringLiteral( "zone_conflict_faces" ) ).toInt(), 1 );
  double area1 = 0;
  double area2 = 0;
  for ( const CandidateRegion &region : result.regions )
    ( region.faciesCode == 1 ? area1 : area2 ) = region.area;
  QCOMPARE( area1, 60.0 );
  QCOMPARE( area2, 60.0 ); // 60+60 > 100：重叠 20 交 QA 检测器裁决
}

void TestFaciesMappingAlgorithms::candidateRegionsResidualGapPreserved()
{
  CandidateBoundaryRequest request;
  request.domain = { rect( 0, 0, 10, 10 ) };
  FaciesZone a;
  a.id = "za";
  a.faciesCode = 1;
  a.polygons = { rect( 0, 0, 4, 10 ) };
  FaciesZone b;
  b.id = "zb";
  b.faciesCode = 2;
  b.polygons = { rect( 6, 0, 10, 10 ) };
  request.zones = { a, b };

  const CandidateBoundaryResult result = extractCandidateRegions( request );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  QCOMPARE( result.regions.size(), size_t( 3 ) );
  double unassignedArea = 0;
  int assigned = 0;
  for ( const CandidateRegion &region : result.regions )
  {
    if ( region.faciesCode < 0 )
      unassignedArea += region.area;
    else
      assigned++;
  }
  QCOMPARE( assigned, 2 );
  // 中缝 (4,0)-(6,10) 空隙如实保留为未定相单元：40 + 40 + 20 = 100。
  QCOMPARE( unassignedArea, 20.0 );
  double total = 0;
  for ( const CandidateRegion &region : result.regions )
    total += region.area;
  QCOMPARE( total, 100.0 ); // 面积守恒
}

void TestFaciesMappingAlgorithms::candidateRegionsMinAreaDropsFragments()
{
  CandidateBoundaryRequest request;
  request.domain = { rect( 0, 0, 10, 10 ) };
  ContourLevelLines level;
  level.level = 1.0;
  level.lines = { { { { 9, 0 }, { 9, 10 } } } }; // 右侧切出 1 宽的窄条
  request.contours = { level };
  request.minArea = 5.0; // 窄条面积 10 保留；再切 0.5 宽碎片应丢弃

  ContourLevelLines second;
  second.level = 2.0;
  second.lines = { { { { 9.5, 0 }, { 9.5, 10 } } } }; // 0.5×10=5 → 面积 == 阈值不丢
  request.contours = { level, second };

  const CandidateBoundaryResult result = extractCandidateRegions( request );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "dropped_small_faces" ) ).toInt(), 0 );
  // 三条带：9.5 右侧 5、9-9.5 之间 5、0-9 之间 90。
  QCOMPARE( result.regions.size(), size_t( 3 ) );
  double total = 0;
  for ( const CandidateRegion &region : result.regions )
    total += region.area;
  QCOMPARE( total, 100.0 );

  // 阈值抬到 6：两条 5 的窄条都丢弃。
  request.minArea = 6.0;
  const CandidateBoundaryResult dropped = extractCandidateRegions( request );
  QCOMPARE( dropped.diagnostics.value( QStringLiteral( "dropped_small_faces" ) ).toInt(), 2 );
  QCOMPARE( dropped.regions.size(), size_t( 1 ) );
  QCOMPARE( dropped.regions.front().area, 90.0 );
}

void TestFaciesMappingAlgorithms::candidateRegionsOpenContourWithoutDomain()
{
  // 无域 + 开放折线：无法闭合 → 如实零面（不伪造闭合）。
  CandidateBoundaryRequest request;
  ContourLevelLines level;
  level.level = 3.0;
  level.lines = { { { { 0, 0 }, { 10, 0 } } } };
  request.contours = { level };

  const CandidateBoundaryResult open = extractCandidateRegions( request );
  QCOMPARE( open.status, paleo::singlefactor::Status::Ok );
  QVERIFY( open.regions.empty() );
  QVERIFY( !open.message.empty() );

  // 无域 + 闭合等值线圈：圈内面成立。
  ContourLevelLines ring;
  ring.level = 4.0;
  ring.lines = { { { { 0, 0 }, { 6, 0 }, { 6, 6 }, { 0, 6 }, { 0, 0 } } } };
  request.contours = { ring };
  const CandidateBoundaryResult closed = extractCandidateRegions( request );
  QCOMPARE( closed.status, paleo::singlefactor::Status::Ok );
  QCOMPARE( closed.regions.size(), size_t( 1 ) );
  QCOMPARE( closed.regions.front().area, 36.0 );
  QCOMPARE( closed.regions.front().faciesCode, -1 ); // 无约束 → 未定相
}

// ---- 阶段3 ------------------------------------------------------------------

void TestFaciesMappingAlgorithms::synthesisWeightedVote()
{
  std::vector<CandidateRegion> regions;
  CandidateRegion r0;
  r0.regionId = "r0";
  r0.geometry = rect( 0, 0, 10, 10 );
  CandidateRegion r1;
  r1.regionId = "r1";
  r1.geometry = rect( 10, 0, 20, 10 );
  regions = { r0, r1 };

  EvidenceSource wells;
  wells.id = "well_facies";
  wells.kind = "well";
  wells.weight = 1.0;
  wells.samples = { { 5, 5, 1, 1.0, 1.0 }, { 15, 5, 2, 1.0, 1.0 } };
  EvidenceSource factor;
  factor.id = "factor:sand";
  factor.kind = "factor";
  factor.weight = 3.0;
  factor.samples = { { 15, 5, 3, 1.0, 1.0 } };

  const SynthesisResult result = synthesizeFaciesRegions( regions, { wells, factor } );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  QCOMPARE( result.regions.size(), size_t( 2 ) );
  // r0：单票 → 满份额赋相。
  QCOMPARE( result.regions[0].assignedCode, 1 );
  QCOMPARE( result.regions[0].confidence, 1.0 );
  QCOMPARE( result.regions[0].voteCount, 1 );
  // r1：井票 c2=1、因素票 c3=3 → c3 份额 0.75 胜出（权重改变结论）。
  QCOMPARE( result.regions[1].assignedCode, 3 );
  QCOMPARE( result.regions[1].confidence, 0.75 );
  QCOMPARE( result.regions[1].voteCount, 2 );
  QVERIFY( !result.regions[1].constraintInherited );
  QCOMPARE( result.regions[1].contributingSources.size(), size_t( 2 ) );

  // 因素权重降到 0.2：c3 得分 0.2 < c2 的 1.0 → 翻转为井相（份额 1/1.2）。
  factor.weight = 0.2;
  const SynthesisResult flipped = synthesizeFaciesRegions( regions, { wells, factor } );
  QCOMPARE( flipped.regions[1].assignedCode, 2 );
  QCOMPARE( flipped.regions[1].confidence, 1.0 / 1.2 );
}

void TestFaciesMappingAlgorithms::synthesisThresholdBlocksAssignment()
{
  CandidateRegion r0;
  r0.regionId = "r0";
  r0.geometry = rect( 0, 0, 10, 10 );
  EvidenceSource source;
  source.id = "well_facies";
  source.weight = 1.0;
  source.samples = { { 3, 3, 1, 1.0, 1.0 }, { 7, 7, 2, 1.0, 1.0 } };

  SynthesisOptions options;
  options.assignThreshold = 0.9; // 50/50 平分 → 份额 0.5 不过阈值
  const SynthesisResult result = synthesizeFaciesRegions( { r0 }, { source }, options );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  QCOMPARE( result.regions.front().assignedCode, -1 ); // 证据对峙：不赋相
  QCOMPARE( result.regions.front().voteCount, 2 );
  QVERIFY( !result.regions.front().constraintInherited );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "unassigned" ) ).toInt(), 1 );
}

void TestFaciesMappingAlgorithms::synthesisConstraintInheritance()
{
  CandidateRegion r0;
  r0.regionId = "r0";
  r0.faciesCode = 7; // 约束相代码
  r0.geometry = rect( 0, 0, 10, 10 );
  EvidenceSource farAway;
  farAway.id = "well_facies";
  farAway.samples = { { 100, 100, 1, 1.0, 1.0 } }; // 区外票

  const SynthesisResult result = synthesizeFaciesRegions( { r0 }, { farAway } );
  QCOMPARE( result.regions.front().assignedCode, 7 ); // 无点证据 → 继承约束
  QVERIFY( result.regions.front().constraintInherited );
  QCOMPARE( result.regions.front().confidence, 0.0 ); // 如实：置信归零
  QCOMPARE( result.diagnostics.value( QStringLiteral( "constraint_inherited" ) ).toInt(), 1 );
}

void TestFaciesMappingAlgorithms::synthesisRejectsEmptyRegions()
{
  const SynthesisResult result = synthesizeFaciesRegions( {}, {} );
  QCOMPARE( result.status, paleo::singlefactor::Status::InvalidInput );
}

// ---- 阶段4 ------------------------------------------------------------------

void TestFaciesMappingAlgorithms::qaDetectsUnclosedOverlapIsland()
{
  // 人工构造三类缺陷：u1 不闭合、u1×u2 重叠、u3 孤岛小面。
  FaciesMapUnit u1;
  u1.regionId = "u1";
  u1.geometry = openRect( 0, 0, 5, 5 ); // 首尾缺口
  FaciesMapUnit u2;
  u2.regionId = "u2";
  u2.geometry = rect( 4, 0, 9, 5 ); // 与 u1 重叠 1×5
  FaciesMapUnit u3;
  u3.regionId = "u3";
  u3.geometry = rect( 20, 20, 20.5, 20.5 ); // 面积 0.25

  FaciesQaOptions options;
  options.minIslandArea = 1.0;
  FaciesQaResult result = runFaciesQa( { u1, u2, u3 }, {}, {}, options );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );

  // 全部检出。
  const FaciesQaIssue *unclosed = firstIssue( result, FaciesQaIssueType::UnclosedRing );
  const FaciesQaIssue *overlap = firstIssue( result, FaciesQaIssueType::Overlap );
  const FaciesQaIssue *island = firstIssue( result, FaciesQaIssueType::SmallIsland );
  QVERIFY( unclosed );
  QVERIFY( overlap );
  QVERIFY( island );

  // 逐条可定位：单元 id + 度量 + 位置。
  QCOMPARE( unclosed->regionIds.front(), std::string( "u1" ) );
  QCOMPARE( unclosed->metric, 5.0 ); // 缺口 = (0,5)-(5,5) 距离？首=(0,0) 尾=(0,5) → 5
  QCOMPARE( overlap->regionIds.size(), size_t( 2 ) );
  QVERIFY( overlap->regionIds[0] == "u1" || overlap->regionIds[1] == "u1" );
  QVERIFY( overlap->regionIds[0] == "u2" || overlap->regionIds[1] == "u2" );
  QCOMPARE( overlap->metric, 5.0 ); // 重叠面积 1×5
  QVERIFY( overlap->location.x >= 4 && overlap->location.x <= 5 );
  QVERIFY( overlap->location.y >= 0 && overlap->location.y <= 5 );
  QCOMPARE( island->regionIds.front(), std::string( "u3" ) );
  QCOMPARE( island->metric, 0.25 );
  QVERIFY( island->location.x >= 20 && island->location.x <= 20.5 );

  // 诊断计数对账。
  QCOMPARE( result.diagnostics.value( QStringLiteral( "issue_unclosed_ring" ) ).toInt(), 1 );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "issue_overlap" ) ).toInt(), 1 );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "issue_small_island" ) ).toInt(), 1 );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "island_detector_on" ) ).toBool(), true );
}

void TestFaciesMappingAlgorithms::qaConstraintCrossingLocated()
{
  FaciesMapUnit unit;
  unit.regionId = "u1";
  unit.geometry = rect( 0, 0, 10, 10 );
  QaConstraintLine fault;
  fault.id = "fault_1";
  fault.points = { { 5, -5 }, { 5, 15 } }; // 竖直穿越
  QaConstraintLine outside;
  outside.id = "fault_2";
  outside.points = { { 20, 0 }, { 20, 10 } }; // 区外平行
  QaConstraintLine tangent;
  tangent.id = "fault_3";
  tangent.points = { { 0, 0 }, { 0, 10 } }; // 沿边共线——不是穿越

  const FaciesQaResult result = runFaciesQa( { unit }, {}, { fault, outside, tangent } );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  const FaciesQaIssue *conflict =
      firstIssue( result, FaciesQaIssueType::ConstraintConflict );
  QVERIFY( conflict );
  QCOMPARE( conflict->regionIds.front(), std::string( "u1" ) );
  QCOMPARE( conflict->relatedIds.front(), std::string( "fault_1" ) );
  QCOMPARE( conflict->metric, 2.0 ); // 进出各一：两个交叉点
  QVERIFY( std::abs( conflict->location.x - 5.0 ) < 1e-6 );
  QVERIFY( conflict->location.y >= -5 && conflict->location.y <= 15 );
  // 只有一条冲突：区外/共线不误报。
  int conflicts = 0;
  for ( const FaciesQaIssue &issue : result.issues )
    if ( issue.type == FaciesQaIssueType::ConstraintConflict )
      conflicts++;
  QCOMPARE( conflicts, 1 );
}

void TestFaciesMappingAlgorithms::qaWellCoverageRadius()
{
  FaciesMapUnit covered;
  covered.regionId = "uc";
  covered.geometry = rect( 0, 0, 5, 5 );
  FaciesMapUnit far;
  far.regionId = "uf";
  far.geometry = rect( 10, 10, 15, 15 );
  const QaWellPoint well{ 2, 2 };

  // 半径 0：uf 缺覆盖（最近距离 = 井到 (10,10)-(15,15) 的角距离）。
  FaciesQaResult result = runFaciesQa( { covered, far }, { well }, {} );
  const FaciesQaIssue *missing = firstIssue( result, FaciesQaIssueType::NoWellCoverage );
  QVERIFY( missing );
  QCOMPARE( missing->regionIds.front(), std::string( "uf" ) );
  QCOMPARE( missing->metric, std::hypot( 8.0, 8.0 ) );
  int coverageIssues = 0;
  for ( const FaciesQaIssue &issue : result.issues )
    if ( issue.type == FaciesQaIssueType::NoWellCoverage )
      coverageIssues++;
  QCOMPARE( coverageIssues, 1 ); // uc 井在面内，不报

  // 缓冲半径 20：uf 在半径内 → 覆盖，无报。
  FaciesQaOptions options;
  options.wellCoverageRadius = 20.0;
  FaciesQaResult buffered = runFaciesQa( { covered, far }, { well }, {}, options );
  QVERIFY( !firstIssue( buffered, FaciesQaIssueType::NoWellCoverage ) );

  // 全工区无井：所有面报缺覆盖，metric = -1（如实，不编造距离）。
  FaciesQaResult noWells = runFaciesQa( { covered, far }, {}, {} );
  const FaciesQaIssue *missingAll =
      firstIssue( noWells, FaciesQaIssueType::NoWellCoverage );
  QVERIFY( missingAll );
  QCOMPARE( missingAll->metric, -1.0 );
}

void TestFaciesMappingAlgorithms::qaReproducibleDiagnostics()
{
  FaciesMapUnit a;
  a.regionId = "ua";
  a.geometry = rect( 0, 0, 5, 5 );
  FaciesMapUnit b;
  b.regionId = "ub";
  b.geometry = rect( 4, 0, 9, 5 );
  FaciesQaOptions options;
  options.minIslandArea = 1.0;

  const FaciesQaResult first = runFaciesQa( { a, b }, {}, {}, options );
  const FaciesQaResult second = runFaciesQa( { a, b }, {}, {}, options );
  QCOMPARE( first.issues.size(), second.issues.size() );
  QCOMPARE( first.diagnostics, second.diagnostics ); // 同输入同输出：报告可复现
  QCOMPARE( first.diagnostics.value( QStringLiteral( "unit_count" ) ).toInt(), 2 );
  QCOMPARE( first.diagnostics.value( QStringLiteral( "well_count" ) ).toInt(), 0 );
}

// ---- 方向 39：按相界类型（boundaryKind）核查 ---------------------------------

void TestFaciesMappingAlgorithms::qaPinchoutOpenEndExemptAndTipDangling()
{
  // 尖灭开放端：UnclosedRing 豁免（合法形态）；未分类开放环仍照报（回归护栏）。
  FaciesMapUnit pinch;
  pinch.regionId = "pinch";
  pinch.geometry = openRect( 0, 0, 5, 5 );
  pinch.boundaryKind = "pinchout";
  FaciesMapUnit plain;
  plain.regionId = "plain";
  plain.geometry = openRect( 10, 0, 15, 5 ); // 同样开放但未分类

  FaciesQaResult result = runFaciesQa( { pinch, plain }, {}, {} );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  const auto unclosedFor = []( const FaciesQaResult &r, const char *region ) {
    int count = 0;
    for ( const FaciesQaIssue &issue : r.issues )
      if ( issue.type == FaciesQaIssueType::UnclosedRing &&
           !issue.regionIds.empty() && issue.regionIds.front() == region )
        ++count;
    return count;
  };
  QCOMPARE( unclosedFor( result, "pinch" ), 0 ); // 尖灭开放端：豁免
  QCOMPARE( unclosedFor( result, "plain" ), 1 ); // 未分类开放环：照报（回归护栏）
  QVERIFY( !firstIssue( result, FaciesQaIssueType::PinchoutTipDangling ) ); // 容差 0 → 检测关

  // 端点落位（容差 0.5）：尖灭端距最近可落位边界 5 → 悬空报；metric 实测距离。
  FaciesMapUnit neighbor;
  neighbor.regionId = "nb";
  neighbor.geometry = rect( 5, 0, 10, 5 );
  FaciesQaOptions withTolerance;
  withTolerance.pinchoutTipTolerance = 0.5;
  result = runFaciesQa( { pinch, neighbor }, {}, {}, withTolerance );
  const FaciesQaIssue *dangling = firstIssue( result, FaciesQaIssueType::PinchoutTipDangling );
  QVERIFY( dangling );
  QCOMPARE( dangling->regionIds.front(), std::string( "pinch" ) );
  QVERIFY( std::abs( dangling->metric - 5.0 ) < 1e-6 ); // 端 (0,0)/(0,5) 距 x=5 边
  QVERIFY( std::abs( dangling->location.y - 2.5 ) < 1e-6 ); // 缺口中点

  // 端点贴住约束线（x=0 竖线）→ 落位成立不报。
  QaConstraintLine landing;
  landing.id = "edge";
  landing.points = { { 0, -1 }, { 0, 6 } };
  result = runFaciesQa( { pinch }, {}, { landing }, withTolerance );
  QVERIFY( !firstIssue( result, FaciesQaIssueType::PinchoutTipDangling ) );

  // 场内无可落位边界（独一单元 + 无约束）→ 如实报，metric = -1。
  result = runFaciesQa( { pinch }, {}, {}, withTolerance );
  dangling = firstIssue( result, FaciesQaIssueType::PinchoutTipDangling );
  QVERIFY( dangling );
  QCOMPARE( dangling->metric, -1.0 );

  // 诊断开关落账。
  QCOMPARE( result.diagnostics.value( QStringLiteral( "pinchout_tip_detector_on" ) ).toBool(),
            true );
}

void TestFaciesMappingAlgorithms::qaTransitionBandMissingForFaciesChange()
{
  FaciesMapUnit noBand;
  noBand.regionId = "t1";
  noBand.geometry = rect( 0, 0, 5, 5 );
  noBand.boundaryKind = "facies_change";
  FaciesMapUnit withBand = noBand;
  withBand.regionId = "t2";
  withBand.geometry = rect( 10, 0, 15, 5 );
  withBand.transitionWidth = 250.0;
  FaciesMapUnit unclassified = noBand;
  unclassified.regionId = "t3";
  unclassified.geometry = rect( 20, 0, 25, 5 );
  unclassified.boundaryKind.clear(); // 未分类不参与按类型核查

  const FaciesQaResult result = runFaciesQa( { noBand, withBand, unclassified }, {}, {} );
  QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
  const FaciesQaIssue *missing =
      firstIssue( result, FaciesQaIssueType::TransitionBandMissing );
  QVERIFY( missing );
  QCOMPARE( missing->regionIds.front(), std::string( "t1" ) );
  QCOMPARE( result.diagnostics.value( QStringLiteral( "issue_transition_band_missing" ) ).toInt(),
            1 );
}

void TestFaciesMappingAlgorithms::qaConformableCutFaciesDetected()
{
  FaciesMapUnit a;
  a.regionId = "a";
  a.faciesCode = 1;
  a.geometry = rect( 0, 0, 5, 5 );
  a.boundaryKind = "conformable";
  FaciesMapUnit b;
  b.regionId = "b";
  b.faciesCode = 2;
  b.geometry = rect( 5, 0, 10, 5 ); // 共享边 x=5（长 5）

  const FaciesQaResult cut = runFaciesQa( { a, b }, {}, {} );
  const FaciesQaIssue *issue = firstIssue( cut, FaciesQaIssueType::ConformableCutFacies );
  QVERIFY( issue );
  QCOMPARE( issue->regionIds.size(), size_t( 2 ) );
  QVERIFY( issue->regionIds[0] == "a" || issue->regionIds[1] == "a" );
  QVERIFY( issue->regionIds[0] == "b" || issue->regionIds[1] == "b" );
  QVERIFY( std::abs( issue->metric - 5.0 ) < 1e-6 ); // 共享边长
  QVERIFY( issue->location.x >= 5 - 1e-6 && issue->location.x <= 5 + 1e-6 );

  // 两侧同相 → 不报；侧相未知 → 不报（诚实中性）；两侧不接触 → 不报。
  FaciesMapUnit same = b;
  same.faciesCode = 1;
  QVERIFY( !firstIssue( runFaciesQa( { a, same }, {}, {} ),
                        FaciesQaIssueType::ConformableCutFacies ) );
  FaciesMapUnit unknown = b;
  unknown.faciesCode = -1;
  QVERIFY( !firstIssue( runFaciesQa( { a, unknown }, {}, {} ),
                        FaciesQaIssueType::ConformableCutFacies ) );
  FaciesMapUnit away = b;
  away.geometry = rect( 20, 0, 25, 5 );
  QVERIFY( !firstIssue( runFaciesQa( { a, away }, {}, {} ),
                        FaciesQaIssueType::ConformableCutFacies ) );
}

QTEST_MAIN( TestFaciesMappingAlgorithms )
#include "tst_faciesmapping_algorithms.moc"
