// 层：数据
#pragma once
// facies-ux 5：预测 / 派生图件的证据链（纯数据，随版本 extra["evidence"] 落库）。
//
// 结构（schema = "paleo_evidence_v1"）：
//   inputs[]    —— 每个父版本：version_id / asset_id / name / asset_type /
//                  kind / version_number / sha256 / stage / mock / role
//   model       —— method / model_names / request_id / algorithm_id
//   parameters  —— 去掉登记簿记字段后的生成参数；parameter_hash 为其规范
//                  JSON（键排序、紧凑）的 SHA-256，参数相同 ⇔ 指纹相同
//   facies_schema_hash —— 相分类表指纹（分类变更可追溯）
//   support     —— 支撑证据：wells（参与/跳过井）、seismic_attributes
//                  （地震输入资产与层位窗/属性参数）、source_layer
//   software    —— 应用 / Qt / QGIS 版本
// 只追加、不改写既有 extra 键——旧读者不受影响。
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace paleo::evidence
{
struct InputRef
{
  QString versionId;
  QString assetId;
  QString name;
  QString assetType;
  QString kind;
  int versionNumber = 0;
  QString sha256;
  QString stage;
  bool mock = false;
  QString role = QStringLiteral("DERIVED_FROM");
  QString horizon;
  bool missing = false;
};

// 规范化参数（去簿记字段）与其指纹。
QVariantMap parameters( const QVariantMap &extra );
QString fingerprint( const QVariant &value );

QVariantMap build( const QVector<InputRef> &inputs, const QVariantMap &extra,
                   const QVariantMap &software );

// 面向界面的一行摘要（输入数 / 方法 / 参数指纹前 12 位）。
QString summary( const QVariantMap &evidence );
// Diagnose a stored chain without rewriting it. Unknown/malformed schema is
// distinct from a genuinely absent early-project record.
QVariantList validate(const QVariantMap &evidence);
} // namespace paleo::evidence
