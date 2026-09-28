// 层：数据
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

// io/ — LAS 2.x 测井曲线的纯类型门面（从 lasparser.h 拆出；解析入口仍在
// lasparser.h，视图只消费这里的值类型 + services/previewdoc.h 门面）。

// ~C 段一条曲线定义 + ~A 数据行（NULL token → NaN）。curves[0] 是 DEPT
// 索引道。
struct LasCurve
{
  QString name;             // mnemonic, e.g. "DEPT"
  QString unit;             // e.g. "M"
  QString descr;            // description text after ':'
  QVector<double> values;   // one entry per ~A data row; NULL tokens -> NaN
};

// LAS 解析结果（PreviewDocService::lasAt 的返回型）。
struct LasDoc
{
  bool ok = false;
  QString error;
  QStringList curveNames;   // ~C 曲线名序（含 DEPT）
  QList<LasCurve> curves;
};
