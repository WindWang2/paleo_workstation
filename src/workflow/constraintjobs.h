// 层：功能
#pragma once
// constraintjobs — #558 扇出根二拆头：ConstraintWorkflow 的三段式 job 族
//（LocalDirection / Geostat / AnalysisContour / InterpretiveContour /
// ConstraintJob）自 workflows.h 外提。动机：InterpretiveContourJob 持
// std::vector<paleo::singlefactor::ConstraintLine>，是 workflows.h 直连
// algorithms/singlefactor/types.h 的唯一消费点（28-TU 出闸边）——job 完整
// 类型只在本头展开，workflows.h 回到前置声明面，出闸边随迁到本头的直接
// 用户（构造/检查 job 的 TU；只传指针/引用的 API 面 include workflows.h
// 即可）。本头 include workflows.h 后按嵌套类型类外定义（类内留有前置
// 声明，见彼处注释）；成员别名/枚举无法类外定义——ConstraintJobKind 用
// 类内不透明枚举声明 + 此处定义保名，variant 词表收进 ConstraintJob。

#include "workflows.h"

#include "../algorithms/singlefactor/types.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <functional>
#include <variant>

enum class ConstraintWorkflow::ConstraintJobKind : int
{
  LocalDirection,
  AnalysisContour,
  InterpretiveContour
};

struct ConstraintWorkflow::LocalDirectionJob
{
  DerivedStaging staging;
  DerivedFileFingerprint fingerprint;
  QString stagedSupportSha, stagedQcSha;
  bool prepared = false;
  bool ok = false;
  quint64 generation = 0;
  QString error;
  QString horizon;
  QString factorId;
  QString field;
  double cellSize = 1.0;
  QString engineId = QStringLiteral("paleo:paleo_local_direction_idw");
  QString wellUri;
  QString constraintUri;
  bool hasConstraints = false;
  QStringList parentPaths;
  QVariantMap params;
  QString outputPath;
  QString supportPath;
  QString qcPath;
};

struct ConstraintWorkflow::GeostatJob
{
  DerivedStaging staging;
  DerivedFileFingerprint fingerprint;
  QString stagedSupportSha, stagedQcSha;
  bool prepared = false;
  bool ok = false;
  bool degraded = false; // 样本不足 → 计算段降级 IDW（method_actual="idw"）
  quint64 generation = 0;
  QString error;
  QString method; // "kriging" | "sgs"
  QString horizon;
  QString factorId;
  QString field;
  double cellSize = 1.0;
  QString wellUri;
  QStringList parentPaths;
  QVariantMap params;
  QString outputPath;
  QString supportPath; // 克里金：估计方差场；SGS：实现间标准差场；降级：空
  // 方向 47：SGS 成员集合——计算段把各 realization 写成员栅格（同一临时
  // 目录），发布段由 RealizationWorkflow 收编为 realization_set 成员版本。
  QStringList memberPaths; // 按 realizationIndex 升序
  quint64 memberSeed = 0;  // 集合基种子（SGS 共享随机流）
  QString qcPath;
  // P1-A：地统方法不读约束线——准备段按统一计划逐条记「未使用」与原因，
  // 计算段写进 QC，发布段写进血缘 extra（降级 IDW 也不带约束）。
  QVariantList constraintsUnused;
  QString constraintCapability;
};

struct ConstraintWorkflow::AnalysisContourJob
{
  DerivedFileFingerprint inputFingerprint;
  bool prepared = false;
  bool ok = false;
  bool fixedLevels = false;
  bool structural = false;
  quint64 generation = 0;
  QString error;
  QString horizon;
  QString factorLayerId;
  QString factorId;
  QString rasterPath;
  QString structuralPath;
  QString analysisSha;
  QStringList parentPaths;
  double interval = 0.0;
  QVector<double> levels;
  QVector<double> resolvedLevels;
  QString outputPath;
  QString contourMethod;
};

struct ConstraintWorkflow::InterpretiveContourJob
{
  DerivedFileFingerprint inputFingerprint;
  bool prepared = false;
  bool ok = false;
  bool strict = true;
  bool hasConstraints = false;
  bool frozenConstraints = false;
  quint64 generation = 0;
  int unresolved = 0;
  QString error;
  QString horizon;
  QString factorLayerId;
  QString factorId;
  QString rasterPath;
  QString constraintUri;
  QString analysisSha;
  QStringList parentPaths;
  QVector<double> levels;
  // singlefactor::ConstraintLine 向量是本族唯一的 algorithms 消费点
  //（#558 拆头的根因所在——见文件头注释）。
  std::vector<paleo::singlefactor::ConstraintLine> constraintLines;
  std::vector<std::string> ignoredConstraints;
  QString workPath;
  QString qcPath;
  QString contourPath;
};

struct ConstraintWorkflow::ConstraintJob
{
  // 旧 workflows.h 内嵌词表 ConstraintJobVariant 的同义内嵌面（成员别名
  // 无法类外定义，收进 job 本体；payload 字段语义不变）。
  using Variant = std::variant<LocalDirectionJob, AnalysisContourJob,
                               InterpretiveContourJob>;
  ConstraintJobKind kind = ConstraintJobKind::LocalDirection;
  Variant payload;
  // 任务标题（现状各调用点的标题文案原样搬过来，避免改 i18n 面）。
  QString title;
  // 进度阶段词表：现状各调用点自带的 prepare/geometry/encode 映射在此
  // 声明式表达，由框架统一节流后经 reportStage 上任务面板。
  std::function<QString(double)> stageOf;
};
