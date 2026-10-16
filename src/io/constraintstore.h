// 层：数据
#pragma once
#include <QString>
#include <QVector>
#include <QVariantMap>
#include <functional>
#include "../metadata/paleoprojectstore.h"

class ConstraintStore {
public:
  struct AuditContext {
    QString actorId; // explicitly local:<operator>, no multi-user authentication
    QString reason;
    QString operationId;
    QString action; // edit/undo/redo/cancel; empty uses the actual create/update/delete
  };
  using WriteFn = std::function<PaleoProjectStore::WriteResult()>;
  using EnqueueFn = std::function<PaleoProjectStore::WriteResult(const WriteFn &)>;

  explicit ConstraintStore(const QString &gpkgPath, EnqueueFn enqueue = nullptr);
  explicit ConstraintStore(const QString &gpkgPath, PaleoProjectStore *store);

  QString gpkgPath() const { return m_gpkgPath; }
  bool setAuditContext(const AuditContext &context, QString *error = nullptr);
  void setReadOnly(bool readOnly) { m_readOnly = readOnly; }

  bool append(const QString &horizon, const QString &id, const QString &wkt,
              const QString &type, int faciesCode, QString *error = nullptr, double weight = 1.0);
  // 加法式参数。旧 append 不写这两列的值。schemaVersion 与 paramsJson 一起保存。
  bool appendExtended(const QString &horizon, const QString &id, const QString &wkt,
                      const QString &type, int code, const QString &parameters,
                      int schema, QString *error = nullptr, double weight = 1.0);
  // 文件不存在时失败，不创建数据库。失败回滚，不改已有要素。
  bool updateParameters(const QString &id, const QString &parameters, int schema,
                        const QString &semanticType, QString *error = nullptr);
  // P3：仅在缺失时建 constraints 表/字段/审计表（与首次 append 同一 schema），
  // 不改动既有行——供绘制前自动开启编辑会话（空工程首次绘制也进撤销栈）。
  bool ensureSchema(QString *error = nullptr);
  QVector<QVariantMap> load(const QString &horizon = QString(), bool includeAudit = false) const;
  // Newest first, max 200 per page. Deleted stable IDs remain queryable.
  QVector<QVariantMap> history(const QString &id, int limit = 50, int offset = 0,
                               QString *error = nullptr) const;
  QVector<QVariantMap> historyForHorizon(const QString &horizon, int limit = 200,
                                       int offset = 0, QString *error = nullptr) const;
  // 将该层位的追加审计复制进另一个 GPKG，供约束快照带走删除历史。
  // 不修改来源；没有审计时成功且不创建空表。
  bool copyAudit(const QString &destinationGpkg, const QString &horizon,
                 QString *error = nullptr, int *copied = nullptr) const;
  // 原子替换单一层位的完整编辑快照，保留 FID 与未编辑字段；拒绝跨层位
  // 及重复身份。经同一写队列；失败时整批回滚，不创建缺失资产。
  bool replaceHorizon(const QString &horizon, const QVector<QVariantMap> &rows,
                      QString *error = nullptr);
  bool remove(const QString &id, QString *error = nullptr);

  // P0-2（pr457-closure）：CRS 存储契约状态——只读检测，不迁移不写文件。
  struct CrsStorageStatus {
    bool fileExists = false;
    bool layerExists = false;  // constraints 表存在
    bool hasSrs = false;       // GPKG 里登记了 SRS（外部 QGIS/GDAL 可独立辨识）
    QString srsWkt;            // hasSrs 时的定义文本
    bool isStoreCrs = false;   // 与库约定 CRS（本地工程网格）同一
    bool legacyNoCrs = false;  // 有层但无 SRS：坐标仅按项目上下文推测解释
  };
  CrsStorageStatus inspectCrsStorage() const;

  // 显式可选迁移：把库约定 CRS 写进 gpkg_spatial_ref_sys /
  // gpkg_geometry_columns（只补元数据，不动几何/属性/FID）。迁移前生成字节级
  // 备份 <gpkg>.crs-backup-<时间戳>.bak；SQL 全程事务，失败回滚；提交后重新
  // 打开校验（SRS 生效 + fid/几何逐一相等），校验失败用备份原样还原。绝不自动
  // 调用——历史库的坐标含义必须由显式动作核定。已是目标状态时幂等成功。
  bool migrateLegacyCrs(QString *error = nullptr, QString *backupPathOut = nullptr);

  // 字节级还原：主文件 + 伴生 -wal/-shm 三件一起从备份恢复。只换主文件不删
  // WAL 脚手架的话，SQLite 下次打开会把遗留帧重放到「还原后的旧主文件」上
  // （帧校验链不校验与主库的同一性）→ 备份页+新页嵌合库。调用前须关闭本文件
  // 的全部连接（Windows 句柄未关时 remove 恒失败）。
  static bool restoreFileFromBackup(const QString &gpkgPath, const QString &backupPath,
                                    QString *error = nullptr);

private:
  bool appendInternal(const QString &horizon, const QString &id, const QString &wkt,
                      const QString &type, int code, const QString *parameters,
                      int schema, QString *error, double weight);
  // migrateLegacyCrs 的独占窗体实现（经 m_enqueue 单写者队列调用）。
  bool migrateLegacyCrsLocked(QString *error, QString *backupPathOut);

private:
  QString m_gpkgPath;
  EnqueueFn m_enqueue;
  AuditContext m_audit;
  bool m_readOnly = false;
};
