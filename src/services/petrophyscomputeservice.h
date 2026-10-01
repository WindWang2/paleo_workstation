// 层：数据
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <limits>

#include "algorithms/petrophys.h" // CurveStats / DepthInterval（QC 结果型）

// services/ — PetroPhysTaskService: 测井岩石物理批处理任务面（seismic
// startAttributeSlice 同构：PaleoTaskService::start 池化 worker + 协作取消 +
// 逐井进度；公式核在 algorithms/petrophys.h，服务只编排不画像素不解析数学）。
//
// 数据流：井集（catalog 解析的 LAS 路径）→ LasCache 载入 → 逐井公式/表达式
// 计算（行对齐：同文档曲线共享 ~A 行）→ QC 统计+越界区间 → LAS 产物写出
// （经 PaleoProjectStore::enqueueWrite 单写者队列）→ catalog DERIVED 登记
// （任务终态后在服务线程做——catalog 非线程安全，worker 不碰）。
//
// 取消语义：井间检查点（单井计算毫秒级，粒度足够）；已完成的井产物保留并
// 照常登记（部分成果如实交付，batch 标记 cancelled）。
// 进度语义：reportBytes(井序 ×1000) 单调 + reportStage(parse/compute)——
// 行数在解析前不可知，以井为进度单位。

struct LasCurve;
class DataCatalog;
class PaleoProjectStore;
class PaleoTask;
class PaleoTaskService;

namespace paleo::petrophys
{

class PetroPhysTaskService : public QObject
{
public:
  enum class Formula
  {
    VshGrLinear,
    VshGrLarionovYoung,
    VshGrLarionovOld,
    VshGrClavier,
    PhiDensity,
    PhiNeutron,
    PhiSonicWyllie,
    SwArchie,
    Expression
  };

  // 参数显式原则：地质常数无隐式默认，未设（NaN）→ validateRequest 报错。
  // UI 预填的文献值是显式参数的可见形态（出处见 petrophys.h 注释）。
  struct FormulaParams
  {
    // Vsh GR 基线：auto=true 用该井 GR 非空极值（产物 extra 记实取值）；
    // false 用 grMin/grMax（grMax>grMin 必须成立）。
    bool grAutoBaseline = true;
    double grMin = std::numeric_limits<double>::quiet_NaN();
    double grMax = std::numeric_limits<double>::quiet_NaN();

    double rhoMa = std::numeric_limits<double>::quiet_NaN();    // φD 骨架密度 g/cm³
    double rhoFluid = std::numeric_limits<double>::quiet_NaN(); // φD 流体密度 g/cm³
    bool neutronInPercent = false;                              // NPHI % → v/v

    double dtMa = std::numeric_limits<double>::quiet_NaN();     // Wyllie µs/m 或 µs/ft
    double dtFluid = std::numeric_limits<double>::quiet_NaN();
    double cpFactor = 1.0; // Cp 压实校正（1 = 不校正，显式语义非默认臆造）

    // Archie：a/m/n/Rw 显式；孔隙度来源 = swPorosityMnemonic（同名曲线）
    // 或留空时密度内联（φD 先算再入 Archie，两式出处同 petrophys.h）。
    double archieA = std::numeric_limits<double>::quiet_NaN();
    double archieM = std::numeric_limits<double>::quiet_NaN();
    double archieN = std::numeric_limits<double>::quiet_NaN();
    double rw = std::numeric_limits<double>::quiet_NaN();
    QString swPorosityMnemonic;
  };

  struct WellRef
  {
    QString wellId;          // catalog 井实体 id（well-<name>）
    QString lasPath;         // 源 LAS 路径
    QString sourceVersionId; // 源 RAW 版本 id（DERIVED parent；空 = 无 parent）
  };

  struct BatchRequest
  {
    QVector<WellRef> wells;
    Formula formula = Formula::VshGrLinear;
    QString expression;      // Formula::Expression 必填
    FormulaParams params;
    QString outputMnemonic;  // 结果曲线名（如 VSH/PHID/SW）
    QString outputUnit;      // 如 "v/v"（可空）
    QString outputDescr;     // ~C 描述（可空）
    bool qcBandEnabled = true;
    double qcLo = 0.0, qcHi = 1.0; // 越界区间计数门（如 Vsh/φ/Sw [0,1]）
    bool writeProduct = true;      // 落盘 + 登记；false = 纯计算（测试/预览）
  };

  struct WellResult
  {
    QString wellId;
    bool ok = false;
    QString error;
    QVector<double> depths;  // 源 DEPT 回显
    QVector<double> values;  // 结果（NaN = 缺失）
    CurveStats stats;
    QVector<DepthInterval> anomalies;
    QString productPath;     // LAS 产物（writeProduct）
    QString assetId;         // catalog 登记 id
    double grBaselineMin = std::numeric_limits<double>::quiet_NaN();
    double grBaselineMax = std::numeric_limits<double>::quiet_NaN();
    double parseMs = 0.0, computeMs = 0.0;
  };

  struct BatchResult
  {
    bool ok = false;         // 全部井成功 = true；取消/全失败 = false
    QString error;           // "cancelled" / 首个致命错误
    QVector<WellResult> wells;
    int succeeded = 0, failed = 0;
    double totalMs = 0.0;
  };

  explicit PetroPhysTaskService(PaleoTaskService *taskService,
                                PaleoProjectStore *store,
                                QObject *parent = nullptr);

  // 请求校验（同步，空串 = 通过）；拒绝时 onFinished(false) 同步回调并返回
  // nullptr（seismic 同惯例）。
  static QString validateRequest(const BatchRequest &req);

  // 启动批处理。onFinished 在服务线程回调（含取消——部分井成果在
  // result.wells 里如实交付）。catalog 可空（writeProduct=true 且 catalog
  // 空 → 只落盘不登记，测试用）；outputDir 为产物目录（不存在则建）。
  PaleoTask *startBatch(const BatchRequest &req, DataCatalog *catalog,
                        const QString &outputDir,
                        std::function<void(bool ok, const BatchResult &)> onFinished);

  // 井集 → 源 LAS 解析（主线程调用）：well 实体 → role="well_log" 链接
  // （isPrimary 优先）→ currentVersion → resolvedVersionPath。解析不到的井
  // 记入 missing（id + 原因），不抛错。
  static QVector<WellRef> resolveWellLas(DataCatalog *catalog,
                                         const QString &projectDir,
                                         const QStringList &wellIds,
                                         QStringList *missing);

  // 单井产物登记（startBatch 终态已做；公开供重放/测试）。返回资产 id，
  // 失败回空串 + error。id 幂等：同名资产已存在则复用（版本仍新增）。
  static QString registerComputedCurveAsset(DataCatalog *catalog,
                                            const WellRef &well,
                                            const BatchRequest &req,
                                            const WellResult &r, QString *error);

  static QString formulaKey(Formula f);   // 资产 id 短名（vsh_lin 等）
  static QString formulaName(Formula f);  // 显示名（中文 UI 用）

private:
  static bool computeWell(const BatchRequest &req, const QVector<LasCurve> &curves,
                          WellResult *out, QString *error);

  PaleoTaskService *m_taskSvc;
  PaleoProjectStore *m_store;
};

} // namespace paleo::petrophys
