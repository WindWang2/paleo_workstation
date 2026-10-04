// 层：功能
#pragma once
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

class DataCatalog;

// workflow/importledger — 文件夹导入批次台账（方向 30：导入完成报告持久化）。
// FolderImportWorkflow 每完成一批（同步或任务池路径）把行级结局写进工程
// sidecar <projectDir>/.paleo/import_ledger.json：目录、起止时间、四计数
// （入库/未决/失败/跳过）与逐行（路径/类型/实体/结局/原因）。窗口保留最近
// kMaxBatches 批——台账是审计面不是无限日志。数据页「导入台账」查看器读
// 同一文件；写入失败不回滚导入（辅助面），qWarning 如实留痕。
//
// sidecar 路径从 catalogPath() 推导（<projectDir>/artifacts/metadata/
// catalog.json 上两级）——与 ui/pages/dataops/dataopsmodel.h 的 sidecar 约定
// 同形；功能层不 include 视图层，推导在本文件复刻。
namespace paleo::imports
{

// 行结局：与 domain/importrows.h FolderRowResult::Outcome 同口径的字符串形
// （"imported" | "unresolved" | "failed" | "skipped"）——台账跨会话存 JSON，
// 不携带枚举二进制语义。
struct LedgerRow
{
  QString path;
  QString type;
  QString entity;
  QString outcome;
  QString message;
};

struct LedgerBatch
{
  QString id;      // "batch-<ms>"
  QString dir;
  QDateTime startedAt;
  QDateTime finishedAt;
  int imported = 0;
  int unresolved = 0;
  int failed = 0;
  int skipped = 0;
  QString error;  // 整批失败原因（rows 为空时也如实入账）；成功为空
  QVector<LedgerRow> rows;

  QVariantMap toVariant() const;
  static LedgerBatch fromVariant(const QVariantMap &m);
};

class ImportLedger
{
  public:
    static constexpr int kMaxBatches = 50;

    // catalog 未开（或路径推不出来）→ 空转：record 不同步失败也不抛——
    // 台账不挡导入主路径。load 幂等，可反复调。
    void load(DataCatalog *cat);
    // 追加一批 + 裁剪窗口 + 原子落盘（QSaveFile temp+rename）。
    bool record(const LedgerBatch &batch, QString *error = nullptr);

    // 时间升序（append 序）。查看器自行倒序展示。
    QVector<LedgerBatch> batches() const { return m_batches; }
    int count() const { return static_cast<int>(m_batches.size()); }

    QString filePath() const { return m_path; }

  private:
    QString m_path; // sidecar 绝对路径；空 = 未激活（catalog 未开）
    QVector<LedgerBatch> m_batches;
};

} // namespace paleo::imports
