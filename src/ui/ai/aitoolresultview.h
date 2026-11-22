// 层：视图
#pragma once

#include "../../workflow/aiconversationpresentation.h"
#include <QString>
#include <QWidget>

class QJsonObject;
class QTableWidget;

// ui/ai — AI 工具结果的结构化呈现（方向 93；G5 扩展 envelope/降级/operation）。
//
// 分派口径（facts = workflow 投影层归一后的呈现事实）：
//   facts.operationFacts                        → Operation（三轴状态卡）
//   envelope/数据完整且 data 为 object：
//     paleo.query_project topic=wells/horizons/assets → Table（紧凑表格）
//     paleo.query_project topic=summary               → KeyValue（计数键值对）
//     paleo.query_project topic=well_details          → KeyValue（井头）+ 关联表
//     paleo.asset_lineage 且节点数 ≤ 小图上限          → LineageGraph（血缘小图）
//   其余（tile/horizon/facies/未知工具/失败/解析失败/超限/transport 不完整/
//   数组或标量根）→ Fallback（JSON 折叠块或不可解析证据的降级呈现）
//
// 只读纪律（DESIGN.md「AI 结果卡片」）：零写路径——组件不碰 catalog/stores；
// 唯一外发是导航与复制意图（用户显式动作），由宿主接线消费
// （先例：DataPage::assetActivated 通道）。血缘小图复用数据页渲染核
// DerivationGraph（视图层内部复用），悬停版本信息即其节点 tooltip。
// G5 红线：transport 不完整 ≠ 业务失败；查询 ok=true ≠ 业务完成；坏 JSON
// 绝不呈现为「成功的空结果」；未知状态/字段原样保留并标注未知。
enum class AiToolResultViewKind { Fallback, Table, KeyValue, LineageGraph, Operation };

// 血缘小图节点上限：闭包快照在 dock 卡内以可读为先，超过降级 Fallback
// （注记如实），全功能交互走「在血缘页打开」。
constexpr int kAiLineageMiniNodeCap = 40;

// 结果形态 → 视图类型（payload = 已解析的工具出参 JSON；legacy 口径保留，
// 供既有调用/测试使用；内部经 projectToolResult 归一）。
AiToolResultViewKind aiToolResultViewKind(const QString &tool, bool ok,
                                          const QJsonObject &payload);

class AiToolResultView : public QWidget {
  Q_OBJECT
public:
  // resultJson = 工具执行器原始出参（完整未截断；解析失败按 Fallback）。
  AiToolResultView(const QString &tool, bool ok, const QString &resultJson,
                   QWidget *parent = nullptr);
  // G5 主入口：归一化呈现事实（实时 raw 与历史 envelope 同一 renderer）。
  AiToolResultView(const paleo::chat::ToolResultFacts &facts, QWidget *parent = nullptr);
  AiToolResultViewKind kind() const { return m_kind; }
  // 结果身份里的 operationId（无则空）——卡片层据此提供「详情/控制」入口。
  QString operationId() const { return m_operationId; }

signals:
  // 导航意图（定位不是数据修改）：资产行 → 数据页选中；井实体行 → 数据页
  // 实体定位；血缘小图「在血缘页打开」/节点点击 → 数据页版本上下文。
  void assetNavigateRequested(const QString &assetId);
  void entityNavigateRequested(const QString &entityId);
  void lineageNavigateRequested(const QString &assetId, const QString &versionId);

protected:
  // 血缘小图首秀适配：结果常在 dock 隐藏时到达，零延迟 fitInView 会按
  // 未布局的 viewport 算缩放——首次 Show/Expose 再补一次（此后不碰，
  // 尊重渲染核「保留用户缩放」口径）。
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void buildFallback(const QJsonObject &payload);
  void buildTable(const QJsonObject &payload, const QString &topic);
  void buildKeyValue(const QJsonObject &payload);
  void buildKeyValueWithLinks(const QJsonObject &payload);
  void buildLineageGraph(const QJsonObject &payload);
  void buildOperation(const paleo::chat::ToolResultFacts &facts);
  void buildDegraded(const paleo::chat::ToolResultFacts &facts); // 不完整/坏 JSON/标量
  QTableWidget *makeCompactTable(int columns, const QStringList &headers);

  AiToolResultViewKind m_kind = AiToolResultViewKind::Fallback;
  QString m_operationId;
  class DerivationGraph *m_fitPendingGraph = nullptr; // 首秀待适配的小图
};
