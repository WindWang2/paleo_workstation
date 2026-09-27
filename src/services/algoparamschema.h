// 层：数据
// services/algoparamschema.h — m2/mapping-pages(A)：算法参数 schema 注册表。
// paleo:* 算法「标量参数」的单一事实源：页面（PredictPage）据此动态建参数
// 表单，测试据此校验词表。字段语义逐一取自算法注册表真值——
// src/algorithms/paleoalgorithms.{h,cpp} 与 faciespolygonize.cpp 的
// initAlgorithm（key/类型/默认值/范围），不虚构算法不存在的参数。
// 图层型参数（INPUT/INPUTS/INPUT_TOP/INPUT_BASE/CONSTRAINTS/OUTPUT）由调用
// 上下文（清单声明/工作流编排）解析，不属于标量表单，故不在此列。
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
    // 算法 id 是否已登记 schema（onnx:* 恒 false——三控件语义在页内）。
    static bool knows(const QString &algorithmId);
    // 该算法的标量参数字段；未登记算法返回空表（paleo:paleo_facies_fusion
    // 登记为「零标量字段」——它只有图层型参数）。
    static QVector<AlgorithmParamField> fieldsFor(const QString &algorithmId);
    // 已登记算法 id 列表（测试/诊断用）。
    static QStringList registeredAlgorithms();
};
