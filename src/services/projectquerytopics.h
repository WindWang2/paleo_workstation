// 层：数据
// services/projectquerytopics.h — paleo.query_project 主题字段契约单源（G3）。
//
// 模型可见的 query_project 公开 schema、聊天分派校验（domaintools）与
// 服务/运行器侧（projectquerycapability、aichattoolrunner_query/_models）
// 共用这一份主题表：每个主题允许哪些字段、哪些必填、分页预算域。
// 之前三处各持一份口径（runner allowed 清单、schema 描述文本、service
// 运行期检查），字段集与预算域互相漂移——本表是唯一权威，运行期数值
// 检查继续留在各自执行面（cursor/HMAC/缓存算法不在此层）。
//
// 边界：typed 主题（numeric_table_projection / seismic_trace_descriptor）
// 的 source_ref/table_id/volume_id 深度校验属于既有专业解析器
// （pythonnumerictablequery / pythonseismictracequery），不在此重复；
// 本表也不做 catalog 读取——纯参数形状判断。
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace paleo::query {

// 单个主题的字段契约。声明、校验和执行共用 page_size/max_bytes 的允许域与缺省值。
struct QueryTopicRule {
  QString topic;
  QStringList required;   // 该主题必填（缺 → missing_required_field）
  QStringList allowed;    // 该主题允许的全部字段（含 topic/分页三件）
  int pageSizeMin = 1, pageSizeMax = 256, pageSizeDefault = 50;
  int maxBytesMin = 2048, maxBytesMax = 65536, maxBytesDefault = 8192;
};

// query_project 的非 typed 主题表（顺序稳定）。typed 主题不登记——
// 其字段校验由专业解析器负责（见上）。
const QList<QueryTopicRule> &queryTopicRules();

// 按名取规则；未登记的主题返回 false（typed/未知主题走各自路径）。
bool queryTopicRule(const QString &topic, QueryTopicRule &out);

// 校验一帧 query_project 实参（非 typed 主题）：
//  · 未知字段（不在 allowed）→ unsupported_topic_argument（field 给出键名）
//  · 缺必填 → missing_required_field
//  · cursor/page_size 出现在不分页主题 → unsupported_topic_argument
//  · page_size/max_bytes 表示/越界 → query_budget_invalid（严格 JSON double，
//    与运行期 integer() 同口径；不接受数字文本）
// typed 主题返回空串（由专业解析器负责）；topic 缺失也不在此判
// （枚举校验在 schema/分派层）。通过返回空串。
QString validateQueryTopicArguments(const QJsonObject &args, QString *field = nullptr);

} // namespace paleo::query
