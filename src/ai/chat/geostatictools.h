// 层：功能
// ai/chat/geostatictools.h — I1 地统计原子的聊天工具描述与嵌套 schema。
//
// 通用能力信封（contract_version/client_request_id/binding/…/parameters/
// inputs）与既有 capabilityTool 同形；本文件额外提供 parameters/inputs 的
// 嵌套 JSON Schema（变差参数、网格、样本引用、分析单元）——模型可见的
// 输入契约与宿主解码（workflow/geostatatomcontract）同源。
#pragma once

#include "domaintools.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace paleo::geostat_atom
{

// paleo.geostat_kriging | paleo.geostat_sgs。
bool isGeostatAtomTool( const QString &name );

// 两只工具的 AiToolSpec（domaintools 的 builtinAiToolSpecs 追加）。
QVector<AiToolSpec> geostatAtomToolSpecs();

// 嵌套 schema（AiToolSpec::toJsonSchema 按名委派）。
QJsonObject geostatAtomToolSchema( const QString &name );

} // namespace paleo::geostat_atom
