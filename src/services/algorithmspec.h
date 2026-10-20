// 层：数据
// services/algorithmspec.h — 算法原子契约（P0）。
// 完整 I/O 规格：inputs/outputs/params/engine。标量字段复用
// AlgorithmParamField（见 algoparamschema.h）。无 QtWidgets、无 Qgs*。
// 设计：docs/designs/algorithm-atom-contract.md
// P0 只钉静态表与对齐测试；不改 QgisProcessingService / workflow 运行时。
#pragma once

#include "algoparamschema.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <functional>

// 端口：图层型 / 文件型 / 隐藏约定（如 LOCAL_GRID_WKT）。
struct AlgoPort
{
  QString id;     // Processing 参数名或约定键（INPUT / OUTPUT / LOCAL_GRID_WKT）
  QString kind;   // raster | vector | table | scalar | file | string
  QString role;   // 可选：对齐 RoleRegistry / catalog role 的提示
  QString geometryHint; // point | line | polygon | ""（非矢量可空）
  bool required = true;
  bool hidden = false; // true：编排注入，通常不在 initAlgorithm 登记
  QVariantMap constraints;
};

// 执行上下文形状（P1+ 执行面使用；P0 仅声明）。
struct AlgorithmRunContext
{
  QString projectDir;
  QString localGridWkt;
  std::function<bool()> cancelled;
  std::function<void( double, QString )> progress;
};

// 单份产出工件（publishArtifacts 消费）。
struct AlgorithmOutputArtifact
{
  QString portId;
  QString absolutePath;
  QString suggestedAssetType;
  QString suggestedDisplayName;
  QVariantMap extra;
};

struct AlgorithmRunResult
{
  bool ok = false;
  QString error;
  QVector<AlgorithmOutputArtifact> artifacts;
  QVariantMap diagnostics;
};

struct AlgorithmSpec
{
  QString id;          // "paleo:paleo_constraint_idw"
  QString displayName;
  QString engine;      // processing | native_cpp | onnx
  QVector<AlgoPort> inputs;
  QVector<AlgoPort> outputs;
  QVector<AlgorithmParamField> params; // 复用 schema 字段类型
  bool complete = false;               // false = TODO/桩，仅占位 id

  // 应 ⊆ initAlgorithm(addParameter) 参数名的键：
  // 非 hidden 输入 + params.key + 目的地型输出（raster/vector/file/table）。
  // 不含 hidden（LOCAL_GRID_WKT），不含诊断型 string 输出（addOutput）。
  QStringList processingParameterNames() const;

  // hidden 输入约定（如 LOCAL_GRID_WKT），不要求出现在 initAlgorithm。
  QStringList hiddenParameterNames() const;

  QStringList paramKeys() const;
};

class AlgorithmSpecRegistry
{
  public:
    static bool knows( const QString &algorithmId );
    static AlgorithmSpec spec( const QString &algorithmId ); // 未知 → id 空、complete false
    static QStringList knownIds();
    // complete==true 的 id（12 Processing + P4 native 入口）
    static QStringList completeIds();
};
