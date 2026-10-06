// 层：数据
#pragma once
#include <QList>
#include "../domain/wellnumeric.h"
#include <QString>

#include "lasdoc.h"

// io/ — LAS 2.0 写出器（本仓首个 LAS 出向路径；解析器 lasparser.h 契约
// 只读不动）。写 DEPT + 计算结果类派生曲线：curves[0] 必须是深度道，
// NaN → NULL token（读回经 LasParser 同一 NaN 语义闭环）。
//
// 落盘纪律：本函数只生成文本文件；产物写入工程目录必须经
// PaleoProjectStore::enqueueWrite 单写者队列（调用方=服务层负责），
// 临时目录/测试夹具直写不受限。

struct LasWriteOptions
{
  QString wellName;              // ~W WELL（可空）
  double nullToken = paleo::wellnumeric::kLasDefaultNull;    // LAS 2.0 惯例 NULL（读侧 lasparser 同默认）
  int valuePrecision = 6;        // ~A 数值小数位
};

namespace LasWriter
{
// curves 非空、首列为深度道、各列等长——违反返回 false + error。
// STEP 写名义步长 (末深−首深)/(n−1)（~A 行本身是权威采样，LAS STEP 仅
// 为头部提示；不规则深度以行为准）。
bool writeLasFile(const QString &path, const QList<LasCurve> &curves,
                  const LasWriteOptions &opt, QString *error);
}
