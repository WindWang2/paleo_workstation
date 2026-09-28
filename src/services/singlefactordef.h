// 层：功能
#pragma once
#include <QString>
#include <QVariantMap>
#include <QVector>

// services/singlefactordef.h — 单因素定义注册表（数据层）。
// PALEO_QGIS_PLAN.md §10 词表：砂体厚度、砂地比、地层厚度、孔隙度、渗透率、
// 距井距离、预测置信度。页面（单因素图页）只消费这个词表渲染清单与参数行，
// 生成链在 ConstraintWorkflow / QgsProcessing 侧。
// 层：数据
struct SingleFactorDefinition
{
  QString factorId;         // 稳定 id：sandthick/sandratio/strathick/poro/perm/welldist/confidence
  QString title;            // 显示名（砂体厚度…）
  QString inputAssetType;   // 输入资产/图层类型（wells/well_log/predict…）
  QString algorithm;        // 算法标签（IDW→GDAL grid / 距离变换…）
  QString processingAlgId;  // processing 算法 id（如 gdal:gridinvdist）
  QString styleRef;         // styles/ 下色带样式名（QgsColorRamp 预设落盘 .qml）
  QVariantMap defaultParams; // 生成参数默认值（field/cellSize/…）
};

class SingleFactorRegistry
{
  public:
    // §10 内置词表（固定顺序）。
    static QVector<SingleFactorDefinition> builtins();
    // byId 未命中 → ok=false 并返回空定义。
    static SingleFactorDefinition byId(const QString &factorId, bool *ok = nullptr);
    // 稳定 id → 显示名（未知 id 原样返回）。
    static QString titleFor(const QString &factorId);
};
