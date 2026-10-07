// 层：视图
#pragma once

#include <QByteArray>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "depthtransform.h"               // DeviationStation（视图侧类型）
#include "domain/wellcompositemodel.h"    // ComprehensiveWellData

class DataCatalog;

namespace WellComposite
{

class WellCompositePanel;

// ui/wellcomposite/derivedsink — D1 壳侧接线（wave/deepen-perf，递延项落地）
//
// 面板的 derivedDocumentReady(doc, 摘要, 审计) 意图信号经本 sink 落成
// catalog DERIVED 版本：
//   · 落位走 workflow/DerivedAssetRegistrar 受管路径
//     artifacts/derived/{asset_id}/{version_id}/{filename}，sha256 入库、
//     产物置只读、资产按 (type, displayName) find-or-create（重算进同一
//     资产的版本历史，不摊大资产表）；
//   · provenance：parentVersionIds = 源数据路径反查的版本
//     （registrar::parentVersionIdsFor——源井数据 RAW；解析不到 = 无父版本，
//     不伪造）；
//   · 序列化经注入的 SerializeFn（io/wellcompositexml 的写回函数）——
//     视图层 include io/* 被白名单挡死（词表只放行 io/lasdoc.h），函数在
//     装配期由组装根（src/app/main.cpp，唯一可同时 include io/ 与 ui/ 的
//     非视图目录）或测试注入；未注入时如实报错，绝不静默回落。
//
// 井斜/时深装配链（D6.1/D6.5 递延）：壳注入 io 解析函数后，面板装载综合
// 柱状图 XML 完成即自动解析「井斜数据」/「时深数据」工作表并喂
// DepthTransform（TVD/TWT 副刻度与读数条）。
//
// 默认实例：attachWorkflows 装一次、全局可见；面板构造即挂接（生产序 =
// 壳先装 sink、预览页后建面板；迟装时补挂已注册面板）。与 TrackRegistry
// 同一视图层单例先例。
class WellCompositeDerivedSink : public QObject
{
  Q_OBJECT

public:
  // io/wellcompositexml 写回与解析的类型适配面（视图侧类型；组装根/测试
  // 绑定时做字段对位——XmlDeviationStation/XmlTimeDepthPair 与此处类型
  // 字段同名同义）。
  using SerializeFn = std::function<QByteArray(const ComprehensiveWellData &doc,
                                               const QStringList &auditLines)>;
  using DeviationParseFn = std::function<bool(const QString &path,
                                              QVector<DeviationStation> *out,
                                              QString *error)>;
  using TimeDepthParseFn = std::function<bool(const QString &path,
                                              QVector<QPair<double, double>> *out,
                                              QString *error)>;

  explicit WellCompositeDerivedSink(QObject *parent = nullptr);

  // ---- 装配面（壳/测试）----
  void bind(DataCatalog *catalog, const QString &projectDir);
  bool isBound() const { return m_catalog != nullptr; }
  DataCatalog *catalog() const { return m_catalog; }
  QString projectDir() const { return m_projectDir; }

  // io 注入（未注入 → hasSerializer()/hasDepthTableParsers() 如实 false，
  // registerDerived/自动喂表按错误路径回报，不造数据）。
  void setSerializer(SerializeFn fn);
  void setDepthTableParsers(DeviationParseFn deviation, TimeDepthParseFn timeDepth);
  void setAlignmentProvider(std::function<void(WellCompositePanel *)> provider);
  void refreshAlignments();
  bool hasSerializer() const { return bool(m_serialize); }
  bool hasDepthTableParsers() const { return bool(m_parseDeviation) && bool(m_parseTimeDepth); }

  // ---- 默认实例 ----
  static void setDefault(WellCompositeDerivedSink *sink);
  static WellCompositeDerivedSink *defaultSink();
  // 面板构造期调用：登记进活跃面板表并挂接当前默认 sink。
  static void registerPanel(WellCompositePanel *panel);

  // 意图落成：序列化 → 受管落位 → DERIVED 登记。成功返回 catalog 版本 id
  // （managedPath 出参给调用方展示）；失败回空串 + error 原文。
  QString registerDerived(const ComprehensiveWellData &doc, const QStringList &auditLines,
                          const QString &sourcePath, QString *error = nullptr,
                          QString *managedPath = nullptr);

signals:
  void derivedRegistered(const QString &managedPath, const QString &versionId);
  void derivedFailed(const QString &reason);
  // 自动喂表结果（deviation/timeDepth = 是否解析到对应工作表）
  void depthTablesApplied(const QString &sourcePath, bool deviation, bool timeDepth);

private:
  friend class WellCompositeDerivedSinkStaticAccess; // 测试直达活跃面板表
  void watch(WellCompositePanel *panel);
  void feedDepthTables(WellCompositePanel *panel, const QString &sourcePath);

  DataCatalog *m_catalog = nullptr;
  QString m_projectDir;
  SerializeFn m_serialize;
  DeviationParseFn m_parseDeviation;
  TimeDepthParseFn m_parseTimeDepth;
  std::function<void(WellCompositePanel *)> m_alignmentProvider;

  static WellCompositeDerivedSink *s_default;
  static QList<QPointer<WellCompositePanel>> &livePanels();
};

} // namespace WellComposite
