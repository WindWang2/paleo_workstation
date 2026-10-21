// 层：数据
// services/algoparamschema.h — m2/mapping-pages(A)：算法参数 schema 注册表。
// paleo:* 算法「标量参数」表单面：algo-atom P3 起，对 **uiReady allowlist**
// 内算法以 AlgorithmSpec.params 为单一事实源（fieldsFor 派生）；本头只描述
// 字段形状。标签/InList 选项等 UI 富化在 .cpp。
// Spec.complete ≠ UI 可展示：未列入 allowlist 的 Processing Spec（params 仍
// 可能是 paramKey 默认 Double 占位）knows()=false → PredictPage 空表单。
// 图层型参数（INPUT/…/OUTPUT）由调用上下文解析，不在此列。
// onnx:* 不走本注册表：其参数语义由页内固定三控件退化承载（见 PredictPage）。
#pragma once
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

// InList 选项：label 为用户可见词，value 为传给算法的值（页面用 userData）。
struct AlgorithmParamOption
{
  QString label;
  QVariant value;
};

struct AlgorithmParamField
{
  enum Type
  {
    Int,       // QSpinBox → int
    Double,    // QDoubleSpinBox → double
    String,    // QLineEdit → QString
    FloatList, // QLineEdit 逗号分隔 → QVariantList(float)
    InList     // QComboBox 词表 → userData
  };

  QString key;   // Processing 参数名（如 "PASSES"），params map 的键
  QString label; // 用户可见标签（tr 由实现侧承载）
  Type type = Double;
  QVariant defaultValue;
  bool required = false; // String 非空必填（收集侧拒收并写状态；主线7）
  bool hasMin = false;
  double minValue = 0.0;
  bool hasMax = false;
  double maxValue = 0.0;
  int decimals = 2;               // Double 显示小数位
  QString suffix;                 // 单位后缀（如 "°"），可空
  QVector<AlgorithmParamOption> options; // InList 词表（其余类型为空）
};

class AlgorithmParamSchema
{
  public:
    // 算法 id 是否在 UI allowlist（onnx:* 与未分型 Spec 恒 false）。
    static bool knows(const QString &algorithmId);
    // 该算法的标量参数字段；未登记算法返回空表（paleo:paleo_facies_fusion
    // 登记为「零标量字段」——它只有图层型参数）。
    static QVector<AlgorithmParamField> fieldsFor(const QString &algorithmId);
    // UI allowlist 算法 id 列表（测试/诊断用；≠全部 Spec）。
    static QStringList registeredAlgorithms();
};
