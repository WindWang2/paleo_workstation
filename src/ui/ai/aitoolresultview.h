// 层：视图
#pragma once

#include <QString>
#include <QWidget>

class QJsonObject;
class QTableWidget;

// ui/ai — AI 工具结果的结构化呈现（方向 93）。
//
// 分派口径（纯函数 aiToolResultViewKind，单测面）：
//   paleo.query_project topic=wells/horizons/assets → Table（紧凑表格）
//   paleo.query_project topic=summary               → KeyValue（计数键值对）
//   paleo.query_project topic=well_details          → KeyValue（井头）+ 关联表
//   paleo.asset_lineage 且节点数 ≤ 小图上限          → LineageGraph（血缘小图）
//   其余（tile/horizon/facies/未知工具/失败/解析失败/超限）→ Fallback（JSON 折叠块）
//
// 只读纪律（DESIGN.md「AI 结果卡片」）：零写路径——组件不碰 catalog/stores；
// 唯一外发是导航与复制意图（用户显式动作），由宿主接线消费
// （先例：DataPage::assetActivated 通道）。血缘小图复用数据页渲染核
// DerivationGraph（视图层内部复用），悬停版本信息即其节点 tooltip。
enum class AiToolResultViewKind { Fallback, Table, KeyValue, LineageGraph };

// 血缘小图节点上限：闭包快照在 dock 卡内以可读为先，超过降级 Fallback
// （注记如实），全功能交互走「在血缘页打开」。
constexpr int kAiLineageMiniNodeCap = 40;

// 结果形态 → 视图类型（payload = 已解析的工具出参 JSON）。
AiToolResultViewKind aiToolResultViewKind(const QString &tool, bool ok,
                                          const QJsonObject &payload);

class AiToolResultView : public QWidget {
  Q_OBJECT
public:
  // resultJson = 工具执行器原始出参（完整未截断；解析失败按 Fallback）。
  AiToolResultView(const QString &tool, bool ok, const QString &resultJson,
                   QWidget *parent = nullptr);
  AiToolResultViewKind kind() const { return m_kind; }

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
  QTableWidget *makeCompactTable(int columns, const QStringList &headers);

  AiToolResultViewKind m_kind = AiToolResultViewKind::Fallback;
  class DerivationGraph *m_fitPendingGraph = nullptr; // 首秀待适配的小图
};
