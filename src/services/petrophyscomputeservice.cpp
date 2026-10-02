// 层：数据
#include "petrophyscomputeservice.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QUuid>

#include "algorithms/curveexpr.h"
#include "catalog/datacatalog.h"
#include "io/lasalias.h"
#include "io/lascache.h"
#include "io/laswriter.h"
#include "metadata/paleoprojectstore.h"
#include "paleotaskservice.h"
#include "welllogset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace paleo::petrophys
{
namespace
{
double nowMs(const QElapsedTimer &t)
{
  return double(t.nsecsElapsed()) / 1.0e6;
}

QString sanitizeToken(QString s)
{
  QString out;
  out.reserve(s.size());
  for (const QChar c : s)
  {
    const char16_t u = c.unicode();
    const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                    || (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-';
    out.append(ok ? c : QLatin1Char('_'));
  }
  return out;
}

// 家族解析：先按 canonical 精确命中，再按 family 首个（DEN→RHOB、AC→DT
// 这类别名井场常见；family 兜底防个别表外助记符漏配）。深度道跳过。
const double *findCurve(const QList<LasCurve> &curves, const QString &canonical,
                        const QString &family)
{
  int fallback = -1;
  for (int i = 1; i < curves.size(); ++i)
  {
    const QString c = LasAliasMap::shared().canonicalCurve(curves.at(i).name);
    if (c == canonical)
      return curves.at(i).values.constData();
    if (fallback < 0 && LasAliasMap::shared().family(curves.at(i).name) == family)
      fallback = i;
  }
  return fallback >= 0 ? curves.at(fallback).values.constData() : nullptr;
}

// 显式 mnemonic 解析（Sw 的 φ 输入）：canonical 等价命中即可。
const double *findCurveByMnemonic(const QList<LasCurve> &curves, const QString &mnemonic)
{
  const QString want = LasAliasMap::shared().canonicalCurve(mnemonic);
  for (int i = 1; i < curves.size(); ++i)
  {
    if (LasAliasMap::shared().canonicalCurve(curves.at(i).name) == want)
      return curves.at(i).values.constData();
  }
  return nullptr;
}

QString sha256OfFile(const QString &path)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return QString();
  QCryptographicHash h(QCryptographicHash::Sha256);
  if (!h.addData(&f))
    return QString();
  return QString::fromLatin1(h.result().toHex());
}

QVariantMap statsToMap(const CurveStats &s)
{
  QVariantMap m;
  m.insert(QStringLiteral("n"), s.n);
  m.insert(QStringLiteral("valid"), s.valid);
  m.insert(QStringLiteral("nulls"), s.nulls);
  m.insert(QStringLiteral("nullRate"), s.nullRate);
  m.insert(QStringLiteral("min"), s.min);
  m.insert(QStringLiteral("max"), s.max);
  m.insert(QStringLiteral("mean"), s.mean);
  m.insert(QStringLiteral("stddev"), s.stddev);
  return m;
}

QVariantMap paramsToMap(const PetroPhysTaskService::BatchRequest &req)
{
  const PetroPhysTaskService::FormulaParams &p = req.params;
  QVariantMap m;
  const auto put = [&m](const char *k, double v) {
    if (!std::isnan(v))
      m.insert(QString::fromLatin1(k), v);
  };
  put("grMin", p.grMin);
  put("grMax", p.grMax);
  put("rhoMa", p.rhoMa);
  put("rhoFluid", p.rhoFluid);
  m.insert(QStringLiteral("grAutoBaseline"), p.grAutoBaseline);
  m.insert(QStringLiteral("neutronInPercent"), p.neutronInPercent);
  put("dtMa", p.dtMa);
  put("dtFluid", p.dtFluid);
  put("cpFactor", p.cpFactor);
  put("archieA", p.archieA);
  put("archieM", p.archieM);
  put("archieN", p.archieN);
  put("rw", p.rw);
  if (!p.swPorosityMnemonic.isEmpty())
    m.insert(QStringLiteral("swPorosityMnemonic"), p.swPorosityMnemonic);
  if (req.formula == PetroPhysTaskService::Formula::Expression)
    m.insert(QStringLiteral("expression"), req.expression);
  return m;
}

const double kNan = std::numeric_limits<double>::quiet_NaN();

QString projectDirOf(const DataCatalog *catalog)
{
  if (!catalog)
    return {};
  return QDir::cleanPath(catalog->catalogPath() + QStringLiteral("/../../.."));
}

QString mnemonicStem(const QString &mnemonic)
{
  int cut = mnemonic.size();
  const int at = mnemonic.indexOf(QLatin1Char('@'));
  const int hash = mnemonic.indexOf(QLatin1Char('#'));
  if (at >= 0)
    cut = std::min(cut, at);
  if (hash >= 0)
    cut = std::min(cut, hash);
  return mnemonic.left(cut);
}

bool sameMnemonic(const QString &a, const QString &b)
{
  return a.compare(b, Qt::CaseInsensitive) == 0;
}

// 表达式标识符（跳过函数名）。大小写保留，匹配曲线时再忽略大小写。
QStringList expressionIdentifiers(const QString &expression)
{
  QStringList ids;
  const auto isFn = [](const QString &id) {
    const QString k = id.toLower();
    return k == QLatin1String("where") || k == QLatin1String("min") || k == QLatin1String("max")
           || k == QLatin1String("abs") || k == QLatin1String("ln") || k == QLatin1String("log10")
           || k == QLatin1String("sqrt") || k == QLatin1String("exp") || k == QLatin1String("pow")
           || k == QLatin1String("clamp");
  };
  for (int i = 0; i < expression.size();)
  {
    const QChar c = expression.at(i);
    if (c.isLetter() || c == QLatin1Char('_'))
    {
      int j = i + 1;
      while (j < expression.size())
      {
        const QChar d = expression.at(j);
        if (!d.isLetterOrNumber() && d != QLatin1Char('_'))
          break;
        ++j;
      }
      const QString id = expression.mid(i, j - i);
      if (!isFn(id))
      {
        bool seen = false;
        for (const QString &prev : ids)
        {
          if (sameMnemonic(prev, id))
          {
            seen = true;
            break;
          }
        }
        if (!seen)
          ids.append(id);
      }
      i = j;
    }
    else
    {
      ++i;
    }
  }
  return ids;
}

QString driverMnemonic(PetroPhysTaskService::Formula formula)
{
  using F = PetroPhysTaskService::Formula;
  switch (formula)
  {
    case F::VshGrLinear:
    case F::VshGrLarionovYoung:
    case F::VshGrLarionovOld:
    case F::VshGrClavier:
      return QStringLiteral("GR");
    case F::PhiDensity:
      return QStringLiteral("RHOB");
    case F::PhiNeutron:
      return QStringLiteral("NPHI");
    case F::PhiSonicWyllie:
      return QStringLiteral("DT");
    case F::SwArchie:
      return QStringLiteral("RT");
    case F::Expression:
      break;
  }
  return {};
}

const WellCurveRef *findExactMnemonic(const QVector<WellCurveRef> &index, const QString &want)
{
  for (const WellCurveRef &ref : index)
  {
    if (sameMnemonic(ref.mnemonic, want))
      return &ref;
  }
  return nullptr;
}

// 精确名没有时，接受规范名等于驱动名的别名（NGR→GR）。不按家族兜底
// （RS 与 RT 同族，但 Sw 的驱动只认 RT）。
const WellCurveRef *findAliasMnemonic(const QVector<WellCurveRef> &index, const QString &want)
{
  const WellCurveRef *fallback = nullptr;
  for (const WellCurveRef &ref : index)
  {
    const QString stem = mnemonicStem(ref.mnemonic);
    const QString canon = LasAliasMap::shared().canonicalCurve(stem);
    if (!sameMnemonic(canon, want) && !sameMnemonic(stem, want))
      continue;
    if (ref.canonical)
      return &ref;
    if (!fallback)
      fallback = &ref;
  }
  return fallback;
}

// 表达式里恰好一条 mnemonic 命中才用它的文件；零条或多条 → 调用方改走主文件。
const WellCurveRef *uniqueExpressionRef(const QVector<WellCurveRef> &index,
                                        const QString &expression)
{
  const WellCurveRef *hit = nullptr;
  int distinct = 0;
  for (const QString &id : expressionIdentifiers(expression))
  {
    for (const WellCurveRef &ref : index)
    {
      if (!sameMnemonic(ref.mnemonic, id))
        continue;
      if (!hit || !sameMnemonic(hit->mnemonic, ref.mnemonic))
      {
        ++distinct;
        hit = &ref;
      }
      break;
    }
  }
  return distinct == 1 ? hit : nullptr;
}

const WellCurveRef *pickDriverRef(const QVector<WellCurveRef> &index,
                                  const PetroPhysTaskService::BatchRequest &req)
{
  if (req.formula == PetroPhysTaskService::Formula::Expression)
    return uniqueExpressionRef(index, req.expression);
  const QString want = driverMnemonic(req.formula);
  if (want.isEmpty())
    return nullptr;
  if (const WellCurveRef *exact = findExactMnemonic(index, want))
    return exact;
  return findAliasMnemonic(index, want);
}

struct CurvePiece
{
  QString mnemonic;
  QString path;
  QString versionId;
  int column = -1;
};

struct WellMergePlan
{
  bool active = false;
  QString driverPath;
  QString driverVersionId;
  QVector<CurvePiece> pieces;
};

WellMergePlan planWellMerge(const DataCatalog *catalog, const QString &projectDir,
                            const PetroPhysTaskService::WellRef &well,
                            const PetroPhysTaskService::BatchRequest &req)
{
  WellMergePlan plan;
  if (!catalog || well.wellId.isEmpty())
    return plan;
  const QVector<WellLogFile> files = WellLogSet::wellLogFiles(catalog, projectDir, well.wellId);
  const QVector<WellCurveRef> index =
      WellLogSet::wellCurveIndex(catalog, projectDir, well.wellId);
  if (files.isEmpty() && index.isEmpty())
    return plan;

  const WellCurveRef *picked = pickDriverRef(index, req);
  if (picked)
  {
    plan.driverPath = picked->path;
    plan.driverVersionId = picked->sourceVersionId;
  }
  else
  {
    const WellLogFile *primary = nullptr;
    const WellLogFile *first = nullptr;
    for (const WellLogFile &file : files)
    {
      if (!first)
        first = &file;
      if (file.isPrimary && !primary)
        primary = &file;
    }
    const WellLogFile *file = primary ? primary : first;
    if (file)
    {
      plan.driverPath = file->path;
      plan.driverVersionId = file->versionId;
    }
    else if (!index.isEmpty())
    {
      const WellCurveRef *fallback = &index.first();
      for (const WellCurveRef &ref : index)
      {
        if (ref.canonical)
        {
          fallback = &ref;
          break;
        }
      }
      plan.driverPath = fallback->path;
      plan.driverVersionId = fallback->sourceVersionId;
    }
  }
  if (plan.driverPath.isEmpty())
    return plan;
  plan.active = true;
  plan.pieces.reserve(index.size());
  for (const WellCurveRef &ref : index)
  {
    CurvePiece piece;
    piece.mnemonic = ref.mnemonic;
    piece.path = ref.path;
    piece.versionId = ref.sourceVersionId;
    piece.column = ref.column;
    plan.pieces.append(piece);
  }
  return plan;
}

bool depthUsable(const QVector<double> &depth)
{
  if (depth.isEmpty())
    return false;
  for (int i = 0; i < depth.size(); ++i)
  {
    if (!std::isfinite(depth.at(i)))
      return false;
    if (i > 0 && !(depth.at(i) > depth.at(i - 1)))
      return false;
  }
  return true;
}

// 源深度须已通过 depthUsable。网格点在源范围外为 NaN，不外推。
double sampleAt(const QVector<double> &depth, const QVector<double> &values, double t)
{
  const int n = depth.size();
  if (n <= 0 || !std::isfinite(t) || t < depth.first() || t > depth.last())
    return kNan;
  int lo = 0;
  int hi = n - 1;
  while (lo + 1 < hi)
  {
    const int mid = lo + (hi - lo) / 2;
    if (depth.at(mid) <= t)
      lo = mid;
    else
      hi = mid;
  }
  const auto at = [&](int i) -> double {
    if (i < 0 || i >= values.size())
      return kNan;
    return values.at(i);
  };
  if (hi == lo || depth.at(lo) == t)
    return at(lo);
  if (depth.at(hi) == t)
    return at(hi);
  const double v0 = at(lo);
  const double v1 = at(hi);
  if (!std::isfinite(v0) || !std::isfinite(v1))
    return kNan;
  const double span = depth.at(hi) - depth.at(lo);
  if (!(span > 0.0))
    return kNan;
  return v0 + (v1 - v0) * ((t - depth.at(lo)) / span);
}

QVector<double> fitToGrid(const QVector<double> &values, int n)
{
  if (values.size() == n)
    return values;
  QVector<double> fitted(n, kNan);
  const int m = std::min(n, static_cast<int>(values.size()));
  for (int i = 0; i < m; ++i)
    fitted[i] = values.at(i);
  return fitted;
}

bool onDriverFile(const CurvePiece &piece, const WellMergePlan &plan)
{
  if (!plan.driverVersionId.isEmpty() && piece.versionId == plan.driverVersionId)
    return true;
  return QDir::cleanPath(piece.path) == QDir::cleanPath(plan.driverPath);
}

bool loadWellCurves(const PetroPhysTaskService::WellRef &well, const WellMergePlan &plan,
                    QVector<LasCurve> *out, QString *error)
{
  if (!plan.active)
  {
    const LasDoc doc = LasCache::shared().load(well.lasPath);
    if (!doc.ok || doc.curves.isEmpty())
    {
      *error = doc.ok ? QStringLiteral("LAS 无曲线") : doc.error;
      return false;
    }
    *out = doc.curves;
    return true;
  }

  const LasDoc driver = LasCache::shared().load(plan.driverPath);
  if (!driver.ok || driver.curves.isEmpty())
  {
    *error = driver.ok ? QStringLiteral("LAS 无曲线") : driver.error;
    return false;
  }

  QVector<LasCurve> merged;
  merged.append(driver.curves.at(0));
  const int n = merged.first().values.size();
  const QVector<double> grid = merged.first().values;

  QVector<CurvePiece> driverPieces;
  QVector<CurvePiece> otherPieces;
  for (const CurvePiece &piece : plan.pieces)
  {
    if (onDriverFile(piece, plan))
      driverPieces.append(piece);
    else
      otherPieces.append(piece);
  }
  std::stable_sort(driverPieces.begin(), driverPieces.end(),
                   [](const CurvePiece &a, const CurvePiece &b) { return a.column < b.column; });

  for (const CurvePiece &piece : driverPieces)
  {
    LasCurve curve;
    curve.name = piece.mnemonic;
    if (piece.column >= 1 && piece.column < driver.curves.size())
    {
      const LasCurve &src = driver.curves.at(piece.column);
      curve.unit = src.unit;
      curve.descr = src.descr;
      curve.values = fitToGrid(src.values, n);
    }
    else
    {
      curve.values = QVector<double>(n, kNan);
    }
    merged.append(curve);
  }

  QStringList pathOrder;
  QHash<QString, QVector<CurvePiece>> byPath;
  for (const CurvePiece &piece : otherPieces)
  {
    if (!byPath.contains(piece.path))
      pathOrder.append(piece.path);
    byPath[piece.path].append(piece);
  }
  for (const QString &path : pathOrder)
  {
    QVector<CurvePiece> pieces = byPath.value(path);
    std::stable_sort(pieces.begin(), pieces.end(),
                     [](const CurvePiece &a, const CurvePiece &b) { return a.column < b.column; });
    const LasDoc doc = LasCache::shared().load(path);
    const bool usable = doc.ok && !doc.curves.isEmpty() && depthUsable(doc.curves.at(0).values);
    const QVector<double> srcDepth = usable ? doc.curves.at(0).values : QVector<double>();
    for (const CurvePiece &piece : pieces)
    {
      LasCurve curve;
      curve.name = piece.mnemonic;
      curve.values = QVector<double>(n, kNan);
      if (usable && piece.column >= 1 && piece.column < doc.curves.size())
      {
        const LasCurve &src = doc.curves.at(piece.column);
        curve.unit = src.unit;
        curve.descr = src.descr;
        for (int i = 0; i < n; ++i)
          curve.values[i] = sampleAt(srcDepth, src.values, grid.at(i));
      }
      merged.append(curve);
    }
  }

  *out = std::move(merged);
  return true;
}

} // namespace

PetroPhysTaskService::PetroPhysTaskService(PaleoTaskService *taskService,
                                           PaleoProjectStore *store,
                                           QObject *parent)
    : QObject(parent), m_taskSvc(taskService), m_store(store)
{
}

QString PetroPhysTaskService::formulaKey(Formula f)
{
  switch (f)
  {
    case Formula::VshGrLinear: return QStringLiteral("vsh_lin");
    case Formula::VshGrLarionovYoung: return QStringLiteral("vsh_lar_y");
    case Formula::VshGrLarionovOld: return QStringLiteral("vsh_lar_o");
    case Formula::VshGrClavier: return QStringLiteral("vsh_clav");
    case Formula::PhiDensity: return QStringLiteral("phi_den");
    case Formula::PhiNeutron: return QStringLiteral("phi_neu");
    case Formula::PhiSonicWyllie: return QStringLiteral("phi_son");
    case Formula::SwArchie: return QStringLiteral("sw_archie");
    case Formula::Expression: return QStringLiteral("expr");
  }
  return QStringLiteral("unknown");
}

QString PetroPhysTaskService::formulaName(Formula f)
{
  switch (f)
  {
    case Formula::VshGrLinear: return QStringLiteral("Vsh·GR 线性");
    case Formula::VshGrLarionovYoung: return QStringLiteral("Vsh·Larionov 年轻岩");
    case Formula::VshGrLarionovOld: return QStringLiteral("Vsh·Larionov 老岩");
    case Formula::VshGrClavier: return QStringLiteral("Vsh·Clavier");
    case Formula::PhiDensity: return QStringLiteral("孔隙度·密度");
    case Formula::PhiNeutron: return QStringLiteral("孔隙度·中子");
    case Formula::PhiSonicWyllie: return QStringLiteral("孔隙度·声波 Wyllie");
    case Formula::SwArchie: return QStringLiteral("Sw·Archie");
    case Formula::Expression: return QStringLiteral("表达式计算器");
  }
  return QStringLiteral("?");
}

QString PetroPhysTaskService::validateRequest(const BatchRequest &req)
{
  if (req.wells.isEmpty())
    return QStringLiteral("井集为空");
  if (req.outputMnemonic.trimmed().isEmpty())
    return QStringLiteral("输出曲线名不能为空");
  const QString m = req.outputMnemonic;
  for (const QChar c : m)
  {
    const char16_t u = c.unicode();
    const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                    || (u >= '0' && u <= '9') || u == '_';
    if (!ok)
      return QStringLiteral("输出曲线名 '%1' 含非法字符（仅字母/数字/下划线）").arg(m);
  }
  if (m.size() > 16)
    return QStringLiteral("输出曲线名过长（≤16 字符）");
  if (req.qcBandEnabled && !(req.qcLo < req.qcHi))
    return QStringLiteral("QC 门下限须小于上限");
  const auto finite = [](double v) { return std::isfinite(v); };
  const FormulaParams &p = req.params;
  switch (req.formula)
  {
    case Formula::VshGrLinear:
    case Formula::VshGrLarionovYoung:
    case Formula::VshGrLarionovOld:
    case Formula::VshGrClavier:
      if (!p.grAutoBaseline
          && (!finite(p.grMin) || !finite(p.grMax) || !(p.grMax > p.grMin)))
        return QStringLiteral("Vsh 基线非法：需 grMax > grMin 或勾选井内极值基线");
      break;
    case Formula::PhiDensity:
      if (!finite(p.rhoMa) || !finite(p.rhoFluid) || !(p.rhoMa > p.rhoFluid))
        return QStringLiteral("密度孔隙度参数非法：需 ρma > ρf（如砂岩 2.65 / 淡水 1.0 g/cm³）");
      break;
    case Formula::PhiNeutron:
      break;
    case Formula::PhiSonicWyllie:
      if (!finite(p.dtMa) || !finite(p.dtFluid) || p.dtMa == p.dtFluid
          || !(p.cpFactor > 0.0))
        return QStringLiteral("声波参数非法：需 Δtma ≠ Δtf 且 Cp > 0（如砂岩 182 / 淡水 620 µs/m）");
      break;
    case Formula::SwArchie:
    {
      if (!finite(p.archieA) || p.archieA <= 0 || !finite(p.archieM) || p.archieM <= 0
          || !finite(p.archieN) || p.archieN <= 0 || !finite(p.rw) || p.rw <= 0)
        return QStringLiteral("Archie 参数非法：需 a/m/n/Rw > 0（文献常用 a=1/m=2/n=2）");
      const bool inlinePhi = finite(p.rhoMa) && finite(p.rhoFluid) && p.rhoMa > p.rhoFluid;
      if (p.swPorosityMnemonic.trimmed().isEmpty() && !inlinePhi)
        return QStringLiteral("Archie 需要孔隙度来源：显式曲线名或 ρma/ρf 密度内联");
      break;
    }
    case Formula::Expression:
      if (req.expression.trimmed().isEmpty())
        return QStringLiteral("表达式为空");
      break;
  }
  return QString();
}

bool PetroPhysTaskService::computeWell(const BatchRequest &req,
                                       const QVector<LasCurve> &curves,
                                       WellResult *out, QString *error)
{
  // QVector<LasCurve> 与 QList<LasCurve> 在 Qt6 同型（QList=QVector）
  const QList<LasCurve> &cs = curves;
  const int n = cs.isEmpty() ? 0 : cs.first().values.size();
  if (n == 0 || cs.size() < 2)
  {
    *error = QStringLiteral("LAS 无数据曲线（仅 %1 道）").arg(cs.size());
    return false;
  }
  out->depths = cs.first().values;
  out->values.resize(n);
  double *dst = out->values.data();
  const FormulaParams &p = req.params;

  const auto grBaseline = [&](const double *gr) -> bool {
    if (!p.grAutoBaseline)
      return true; // 固定基线已在 validateRequest 校验
    double lo = 0.0, hi = 0.0;
    if (!grExtrema(gr, n, &lo, &hi))
    {
      *error = QStringLiteral("该井 GR 全缺失，无法取井内极值基线");
      return false;
    }
    out->grBaselineMin = lo;
    out->grBaselineMax = hi;
    return true;
  };

  switch (req.formula)
  {
    case Formula::VshGrLinear:
    case Formula::VshGrLarionovYoung:
    case Formula::VshGrLarionovOld:
    case Formula::VshGrClavier:
    {
      const double *gr = findCurve(cs, QStringLiteral("GR"), QStringLiteral("Gamma"));
      if (!gr)
      {
        *error = QStringLiteral("找不到 GR 曲线（Gamma 家族）");
        return false;
      }
      double lo = p.grMin, hi = p.grMax;
      if (!grBaseline(gr))
        return false;
      if (p.grAutoBaseline)
      {
        lo = out->grBaselineMin;
        hi = out->grBaselineMax;
      }
      switch (req.formula)
      {
        case Formula::VshGrLinear: vshGrLinear(gr, n, lo, hi, dst); break;
        case Formula::VshGrLarionovYoung: vshGrLarionovYoung(gr, n, lo, hi, dst); break;
        case Formula::VshGrLarionovOld: vshGrLarionovOld(gr, n, lo, hi, dst); break;
        default: vshGrClavier(gr, n, lo, hi, dst); break;
      }
      if (!(hi > lo))
      {
        // 井内极值退化为常数 GR（hi==lo）——IGR 分母为零，如实报错
        *error = QStringLiteral("GR 井内极值基线退化（min==max），请改用显式基线");
        return false;
      }
      break;
    }
    case Formula::PhiDensity:
    {
      const double *rhob = findCurve(cs, QStringLiteral("RHOB"), QStringLiteral("Density"));
      if (!rhob)
      {
        *error = QStringLiteral("找不到密度曲线（RHOB/DEN 家族）");
        return false;
      }
      phiDensity(rhob, n, p.rhoMa, p.rhoFluid, dst);
      break;
    }
    case Formula::PhiNeutron:
    {
      const double *nphi = findCurve(cs, QStringLiteral("NPHI"), QStringLiteral("Neutron"));
      if (!nphi)
      {
        *error = QStringLiteral("找不到中子曲线（NPHI 家族）");
        return false;
      }
      phiNeutron(nphi, n, p.neutronInPercent, dst);
      break;
    }
    case Formula::PhiSonicWyllie:
    {
      const double *dt = findCurve(cs, QStringLiteral("DT"), QStringLiteral("Sonic"));
      if (!dt)
      {
        *error = QStringLiteral("找不到声波曲线（DT/AC 家族）");
        return false;
      }
      phiSonicWyllie(dt, n, p.dtMa, p.dtFluid, p.cpFactor, dst);
      break;
    }
    case Formula::SwArchie:
    {
      const double *rt = findCurve(cs, QStringLiteral("RT"), QStringLiteral("Resistivity"));
      if (!rt)
      {
        *error = QStringLiteral("找不到电阻率曲线（RT 家族）");
        return false;
      }
      std::vector<double> phi;
      const double *phiPtr = nullptr;
      if (!p.swPorosityMnemonic.trimmed().isEmpty())
      {
        phiPtr = findCurveByMnemonic(cs, p.swPorosityMnemonic);
        if (!phiPtr)
        {
          *error = QStringLiteral("找不到孔隙度曲线 '%1'").arg(p.swPorosityMnemonic);
          return false;
        }
      }
      else
      {
        const double *rhob = findCurve(cs, QStringLiteral("RHOB"), QStringLiteral("Density"));
        if (!rhob)
        {
          *error = QStringLiteral("密度内联 φD 找不到 RHOB 曲线");
          return false;
        }
        phi.resize(size_t(n));
        phiDensity(rhob, n, p.rhoMa, p.rhoFluid, phi.data());
        phiPtr = phi.data();
      }
      swArchie(phiPtr, rt, n, p.archieA, p.archieM, p.archieN, p.rw, dst);
      break;
    }
    case Formula::Expression:
    {
      // 变量表 = 原始助记符 + canonical 名（DEN 与 RHOB 都可写）；重复名
      // 先到先得。含标识符外字符的助记符进不了词法，跳过（表达式里用不到）。
      std::vector<std::string> vars;
      std::vector<std::pair<QString, const double *>> binding;
      for (int i = 1; i < cs.size(); ++i)
      {
        const QString raw = cs.at(i).name;
        const QString canon = LasAliasMap::shared().canonicalCurve(raw);
        for (const QString &name : {raw, canon})
        {
          bool dup = false;
          for (const auto &v : vars)
          {
            if (v == name.toStdString())
            {
              dup = true;
              break;
            }
          }
          if (!dup)
          {
            vars.push_back(name.toStdString());
            binding.push_back({name, cs.at(i).values.constData()});
          }
        }
      }
      std::string compileErr;
      const curveexpr::CompiledExpr expr = curveexpr::CompiledExpr::compile(
          req.expression.toStdString(), vars, &compileErr);
      if (!expr.isValid())
      {
        *error = QStringLiteral("表达式错误：%1").arg(QString::fromStdString(compileErr));
        return false;
      }
      std::vector<std::pair<std::string, const double *>> evalVars;
      for (const auto &b : binding)
        evalVars.push_back({b.first.toStdString(), b.second});
      std::string evalErr;
      if (!expr.evaluate(n, evalVars, dst, &evalErr))
      {
        *error = QStringLiteral("求值失败：%1").arg(QString::fromStdString(evalErr));
        return false;
      }
      break;
    }
  }

  out->stats = curveStats(dst, n);
  if (req.qcBandEnabled)
  {
    // QVector<double> → anomalyIntervals 裸指针口径
    out->anomalies.clear();
    const std::vector<DepthInterval> iv = anomalyIntervals(
        out->depths.constData(), dst, n, req.qcLo, req.qcHi);
    for (const DepthInterval &d : iv)
      out->anomalies.append(d);
  }
  return true;
}

QVector<PetroPhysTaskService::WellRef> PetroPhysTaskService::resolveWellLas(
    DataCatalog *catalog, const QString &projectDir, const QStringList &wellIds,
    QStringList *missing)
{
  QVector<WellRef> out;
  for (const QString &id : wellIds)
  {
    WellRef ref;
    ref.wellId = id;
    const CatalogEntity ent = catalog->entityById(id);
    if (ent.id.isEmpty())
    {
      if (missing)
        missing->append(QStringLiteral("%1: 井实体不存在").arg(id));
      continue;
    }
    const QVector<EntityAssetLink> links = catalog->linksForEntity(id);
    int pick = -1;
    for (int i = 0; i < links.size(); ++i)
    {
      if (links.at(i).role != QStringLiteral("well_log"))
        continue;
      if (pick < 0 || (links.at(i).isPrimary && !links.at(pick).isPrimary))
        pick = i;
    }
    if (pick < 0)
    {
      if (missing)
        missing->append(QStringLiteral("%1: 无 well_log 链接").arg(id));
      continue;
    }
    const CatalogVersion v = catalog->currentVersion(links.at(pick).assetId);
    if (v.id.isEmpty())
    {
      if (missing)
        missing->append(QStringLiteral("%1: well_log 无版本").arg(id));
      continue;
    }
    ref.sourceVersionId = v.id;
    ref.lasPath = DataCatalog::resolvedVersionPath(projectDir, v);
    out.append(ref);
  }
  return out;
}

QString PetroPhysTaskService::registerComputedCurveAsset(DataCatalog *catalog,
                                                         const WellRef &well,
                                                         const BatchRequest &req,
                                                         const WellResult &r,
                                                         QString *error)
{
  const QString id = QStringLiteral("petrophys_%1_%2")
                         .arg(formulaKey(req.formula), sanitizeToken(well.wellId));
  const CatalogEntity ent = catalog->entityById(well.wellId);
  const QString wellName = ent.id.isEmpty() ? well.wellId : ent.name;

  CatalogAsset asset;
  asset.id = id;
  asset.type = QStringLiteral("well_log");
  asset.format = QStringLiteral("las");
  asset.displayName = QStringLiteral("%1 · %2（%3）")
                          .arg(req.outputMnemonic, wellName, formulaName(req.formula));
  if (catalog->assetById(id).id.isEmpty())
  {
    QString e;
    if (!catalog->addAsset(asset, &e))
    {
      if (error)
        *error = QStringLiteral("addAsset: %1").arg(e);
      return QString();
    }
  }

  CatalogVersion v;
  v.id = QStringLiteral("ver_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  v.assetId = id;
  v.stage = QStringLiteral("DERIVED");
  v.versionNumber = 1;
  v.managed = false; // 外链产物文件（seismic 派生资产同例），sha256 为凭
  v.path = QFileInfo(r.productPath).absoluteFilePath();
  v.sourceUri = well.lasPath;
  v.fileName = QFileInfo(r.productPath).fileName();
  v.sha256 = sha256OfFile(r.productPath);
  if (!well.sourceVersionId.isEmpty())
    v.parentVersionIds = QStringList{well.sourceVersionId};
  QVariantMap extra;
  extra.insert(QStringLiteral("origin"), QStringLiteral("petrophysics"));
  extra.insert(QStringLiteral("formula"), formulaKey(req.formula));
  extra.insert(QStringLiteral("mnemonic"), req.outputMnemonic);
  extra.insert(QStringLiteral("params"), paramsToMap(req));
  extra.insert(QStringLiteral("stats"), statsToMap(r.stats));
  extra.insert(QStringLiteral("anomalyCount"), r.anomalies.size());
  if (req.qcBandEnabled)
  {
    extra.insert(QStringLiteral("qcLo"), req.qcLo);
    extra.insert(QStringLiteral("qcHi"), req.qcHi);
  }
  if (!std::isnan(r.grBaselineMin))
  {
    extra.insert(QStringLiteral("grBaselineMin"), r.grBaselineMin);
    extra.insert(QStringLiteral("grBaselineMax"), r.grBaselineMax);
  }
  extra.insert(QStringLiteral("parseMs"), r.parseMs);
  extra.insert(QStringLiteral("computeMs"), r.computeMs);
  v.extra = extra;
  {
    QString e;
    if (!catalog->addVersion(v, &e))
    {
      if (error)
        *error = QStringLiteral("addVersion: %1").arg(e);
      return QString();
    }
  }

  EntityAssetLink link;
  link.entityType = QStringLiteral("well");
  link.assetId = id;
  link.role = QStringLiteral("well_log");
  link.isPrimary = false;
  link.ordinal = 0;
  if (ent.id.isEmpty())
  {
    // 井实体不在库（理论不该发生——resolveWellLas 已滤）：未决链接如实留痕
    link.unresolved = true;
    link.note = QStringLiteral("petrophysics: unknown well %1").arg(well.wellId);
  }
  else
  {
    link.entityId = well.wellId;
    link.note = QStringLiteral("petrophysics derived");
  }
  {
    QString e;
    if (!catalog->addLink(link, &e))
    {
      if (error)
        *error = QStringLiteral("addLink: %1").arg(e);
      return QString();
    }
  }
  return id;
}

PaleoTask *PetroPhysTaskService::startBatch(
    const BatchRequest &req, DataCatalog *catalog, const QString &outputDir,
    std::function<void(bool ok, const BatchResult &)> onFinished)
{
  const QString validation = validateRequest(req);
  if (!validation.isEmpty())
  {
    BatchResult r;
    r.error = validation;
    if (onFinished)
      onFinished(false, r);
    return nullptr;
  }
  if (!m_taskSvc)
  {
    BatchResult r;
    r.error = QStringLiteral("任务服务不可用");
    if (onFinished)
      onFinished(false, r);
    return nullptr;
  }

  const auto request = std::make_shared<BatchRequest>(req);
  const auto result = std::make_shared<BatchResult>();
  const auto plans = std::make_shared<QHash<QString, WellMergePlan>>();
  const QString outDir = outputDir;
  PaleoProjectStore *store = m_store;
  // 并集与驱动文件在服务线程定下来（worker 不碰 catalog）。无 catalog
  // 或索不到已决 LAS 时计划为空，worker 仍只解析 WellRef::lasPath。
  if (catalog)
  {
    const QString projectDir = projectDirOf(catalog);
    for (WellRef &well : request->wells)
    {
      const WellMergePlan plan = planWellMerge(catalog, projectDir, well, *request);
      if (!plan.active)
        continue;
      const QString oldPath = QDir::cleanPath(well.lasPath);
      const QString newPath = QDir::cleanPath(plan.driverPath);
      well.lasPath = plan.driverPath;
      if (!plan.driverVersionId.isEmpty()
          && (!well.sourceVersionId.isEmpty() || oldPath != newPath))
        well.sourceVersionId = plan.driverVersionId;
      plans->insert(well.wellId, plan);
    }
  }

  PaleoTask *task = m_taskSvc->start(
      QStringLiteral("测井计算：%1 × %2 井")
          .arg(formulaName(req.formula))
          .arg(req.wells.size()),
      [request, result, outDir, store, plans](PaleoTask *task) -> QString {
        QElapsedTimer totalClock;
        totalClock.start();
        const qint64 totalUnits = qint64(request->wells.size()) * 1000;
        task->reportStage(QStringLiteral("scan"));
        task->reportBytes(0, totalUnits);
        if (request->writeProduct)
          QDir().mkpath(outDir);

        for (int w = 0; w < request->wells.size(); ++w)
        {
          if (task->cancelRequested())
          {
            result->error = QStringLiteral("cancelled");
            result->totalMs = nowMs(totalClock);
            return QString(); // 已完成井的成果保留在 result->wells
          }
          const WellRef &well = request->wells.at(w);
          WellResult r;
          r.wellId = well.wellId;

          QElapsedTimer parseClock;
          parseClock.start();
          task->reportStage(QStringLiteral("parse"));
          QVector<LasCurve> curves;
          QString loadErr;
          if (!loadWellCurves(well, plans->value(well.wellId), &curves, &loadErr))
          {
            r.parseMs = nowMs(parseClock);
            r.error = loadErr;
            result->wells.append(r);
            task->reportBytes(qint64(w + 1) * 1000, totalUnits);
            task->reportDetail(QStringLiteral("%1：解析失败").arg(well.wellId));
            continue;
          }
          r.parseMs = nowMs(parseClock);

          task->reportStage(QStringLiteral("compute"));
          QElapsedTimer computeClock;
          computeClock.start();
          QString err;
          if (!computeWell(*request, curves, &r, &err))
          {
            r.error = err;
          }
          else
          {
            r.computeMs = nowMs(computeClock);
            if (request->writeProduct)
            {
              LasCurve depth = curves.first();
              LasCurve outCurve;
              outCurve.name = request->outputMnemonic;
              outCurve.unit = request->outputUnit;
              outCurve.descr = request->outputDescr.isEmpty()
                                   ? QStringLiteral("petrophysics %1")
                                         .arg(formulaKey(request->formula))
                                   : request->outputDescr;
              outCurve.values = r.values;
              LasWriteOptions wo;
              wo.wellName = well.wellId.startsWith(QStringLiteral("well-"))
                                ? well.wellId.mid(5)
                                : well.wellId;
              const QString path = QStringLiteral("%1/%2_%3_%4.las")
                                       .arg(outDir, sanitizeToken(well.wellId),
                                            formulaKey(request->formula),
                                            sanitizeToken(request->outputMnemonic));
              QString writeErr;
              bool writeOk = false;
              if (store)
              {
                // 产物写入走单写者队列（禁区：不旁路直写工程目录）
                const PaleoProjectStore::WriteResult wr = store->enqueueWrite([&]() {
                  QString e;
                  if (!LasWriter::writeLasFile(path, {depth, outCurve}, wo, &e))
                    return PaleoProjectStore::WriteResult{false, e};
                  return PaleoProjectStore::WriteResult{true, QString()};
                });
                writeOk = wr.ok;
                writeErr = wr.error;
              }
              else
              {
                writeOk = LasWriter::writeLasFile(path, {depth, outCurve}, wo, &writeErr);
              }
              if (writeOk)
                r.productPath = path;
              else
              {
                r.ok = false;
                r.error = QStringLiteral("产物写出失败：%1").arg(writeErr);
              }
            }
          }
          r.ok = r.error.isEmpty();
          result->wells.append(r);
          task->reportBytes(qint64(w + 1) * 1000, totalUnits);
          task->reportDetail(r.ok
                                 ? QStringLiteral("%1：完成（%2 样本，null %3%）")
                                       .arg(well.wellId)
                                       .arg(r.stats.n)
                                       .arg(r.stats.nullRate * 100.0, 0, 'f', 1)
                                 : QStringLiteral("%1：%2").arg(well.wellId, r.error));
        }
        result->totalMs = nowMs(totalClock);
        return QString(); // 单井失败记入 wells（分井处置），任务本身不算败
      },
      QString(), false);

  if (!task)
  {
    BatchResult r;
    r.error = QStringLiteral("任务提交失败");
    if (onFinished)
      onFinished(false, r);
    return nullptr;
  }

  connect(task, &PaleoTask::finished, this,
          [this, task, request, result, catalog, onFinished]() {
            // catalog 登记在服务线程做（catalog 非线程安全，worker 不碰；
            // 取消时已完成井也登记——部分成果如实交付）
            if (catalog && request->writeProduct)
            {
              for (WellResult &r : result->wells)
              {
                if (!r.ok || r.productPath.isEmpty())
                  continue;
                WellRef well;
                for (const WellRef &w : request->wells)
                {
                  if (w.wellId == r.wellId)
                  {
                    well = w;
                    break;
                  }
                }
                QString err;
                const QString asset =
                    registerComputedCurveAsset(catalog, well, *request, r, &err);
                if (asset.isEmpty())
                {
                  r.ok = false;
                  r.error = QStringLiteral("catalog 登记失败：%1").arg(err);
                }
                else
                {
                  r.assetId = asset;
                }
              }
            }
            const bool cancelled = task->cancelRequested();
            result->succeeded = 0;
            result->failed = 0;
            for (const WellResult &r : result->wells)
              r.ok ? ++result->succeeded : ++result->failed;
            result->ok = !cancelled && result->failed == 0 && result->succeeded > 0;
            if (result->error.isEmpty() && result->failed > 0 && !cancelled)
              result->error =
                  QStringLiteral("%1/%2 井失败").arg(result->failed).arg(result->wells.size());
            if (onFinished)
              onFinished(result->ok, *result);
          });
  return task;
}

} // namespace paleo::petrophys
