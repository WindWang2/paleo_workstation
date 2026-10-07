// 层：组装根
// paleo_mkproject — 无头建 demo 工程工具（鄂尔多斯竞赛数据）。
//
// 用法（就地工程模式，默认工程束直接生成在数据根里——PROJECT_FILE_DESIGN
// 的「从工区文件夹新建」契约：<目录名>.qgz + project.paleo 落在数据夹内，
// GUI「打开工程/从工区文件夹新建」指到该目录即识别）：
//   paleo_mkproject --data <paleo_data 根>                 # 就地：<根>/<目录名>.qgz
//   paleo_mkproject --out <目录> --data <paleo_data 根>    # 另建工程目录
//   [--name 名字]（缺省 = 工程目录名）
//
// 全程走生产代码路径（产物与 GUI「新建工程 + 导入」等价）：
//   1. QgisProjectService::createProject（.qgz + project.paleo 双件）；
//   2. 井位/分层 xlsx 经 paleo::io::readWorkbook 转规范井文本
//   （井名 X Y KB TD / 井名 层名 MD），原 xlsx 另按自然类型入 catalog 作
//   reference 资产（provenance 可见）；
//   3. DataImportService 显式清单导入（井口建井 → LAS×21（A3 双份）→
//   分层×2（分层数据 + 20kou_tops.dat，头驱动列序）→ SEG-Y 外链冻结几何）；
//   4. project.paleo 写入 georeference 节（局部网格→WGS84 相似变换，
//   7 口井 LAS 头泄露经纬度最小二乘拟合）+ sourceArea 溯源；
//   5. 重开工程自校验（openProject(.paleo)）并打印 catalog 摘要：
//   井数、每井各角色成员数、A3 双测井、双分层、地震几何与 WGS84 包围盒。
#include "../catalog/datacatalog.h"
#include "../io/dataimportservice.h"
#include "../io/outsourceworkbook.h"
#include "../metadata/paleoprojectfile.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgisruntime.h"

#include <qgsapplication.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QDirIterator>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <optional>

namespace
{
int fails = 0;
void note(bool ok, const QString &what)
{
  std::printf("  %s %s\n", ok ? "OK  " : "FAIL", qPrintable(what));
  if (!ok)
    ++fails;
}

QString requireFile(const QString &path)
{
  if (QFile::exists(path))
    return path;
  std::printf("缺少输入文件: %s\n", qPrintable(path));
  ::exit(2);
}

// 表头子串定位列（-1 = 无）。
int headerCol(const QStringList &headers, const QStringList &keys)
{
  for (int i = 0; i < headers.size(); ++i)
    for (const QString &k : keys)
      if (headers.at(i).contains(k, Qt::CaseInsensitive))
        return i;
  return -1;
}

// 井位坐标.xlsx（井号/井口横坐标X/井口纵坐标Y/补心海拔/完钻井深）→
// 规范井口文本（井名 X Y KB TD）。多井单文件契约原样保留（20 行）。
bool convertWellHeadXlsx(const QString &xlsxPath, const QString &outPath,
                         QString *error)
{
  const paleo::io::WorkbookReadResult wb = paleo::io::readWorkbook(xlsxPath);
  if (!wb.ok || wb.sheets.isEmpty())
  {
    *error = QStringLiteral("读工作簿失败: %1").arg(wb.error);
    return false;
  }
  const paleo::io::WorkbookSheet &sh = wb.sheets.first();
  const int cName = headerCol(sh.headers, {QStringLiteral("井号"), QStringLiteral("井名")});
  const int cX = headerCol(sh.headers, {QStringLiteral("横坐标"), QStringLiteral("X")});
  const int cY = headerCol(sh.headers, {QStringLiteral("纵坐标"), QStringLiteral("Y")});
  const int cKb = headerCol(sh.headers, {QStringLiteral("补心")});
  const int cTd = headerCol(sh.headers, {QStringLiteral("完钻"), QStringLiteral("井深")});
  if (cName < 0 || cX < 0 || cY < 0 || cKb < 0 || cTd < 0)
  {
    *error = QStringLiteral("井位表列不全: %1").arg(sh.headers.join('/'));
    return false;
  }
  QFile f(outPath);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
  {
    *error = QStringLiteral("写转换件失败: %1").arg(outPath);
    return false;
  }
  QTextStream ts(&f);
  int rows = 0;
  for (const QStringList &r : sh.rows)
  {
    if (r.size() <= std::max({cName, cX, cY, cKb, cTd}))
      continue;
    double x = 0, y = 0, kb = 0, td = 0;
    if (!paleo::io::parseNumericCell(r.at(cX), &x) ||
        !paleo::io::parseNumericCell(r.at(cY), &y) ||
        !paleo::io::parseNumericCell(r.at(cKb), &kb) ||
        !paleo::io::parseNumericCell(r.at(cTd), &td))
      continue;
    ts << r.at(cName).trimmed() << ' ' << x << ' ' << y << ' ' << kb << ' ' << td
       << '\n';
    ++rows;
  }
  f.close();
  if (rows == 0)
  {
    *error = QStringLiteral("井位表无有效行");
    return false;
  }
  return true;
}

// 分层数据.xlsx（per-井 sheet：序号/层位/顶界垂深/底界垂深）→
// 规范分层文本（井名 层名 MD，多井单文件）。
bool convertStratXlsx(const QString &xlsxPath, const QString &outPath,
                      QString *error)
{
  const paleo::io::WorkbookReadResult wb = paleo::io::readWorkbook(xlsxPath);
  if (!wb.ok)
  {
    *error = QStringLiteral("读工作簿失败: %1").arg(wb.error);
    return false;
  }
  QFile f(outPath);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
  {
    *error = QStringLiteral("写转换件失败: %1").arg(outPath);
    return false;
  }
  QTextStream ts(&f);
  int rows = 0, wells = 0;
  for (const paleo::io::WorkbookSheet &sh : wb.sheets)
  {
    const QString well = sh.name.trimmed();
    if (well.isEmpty())
      continue;
    const int cTop = headerCol(sh.headers, {QStringLiteral("层位"), QStringLiteral("层名")});
    const int cMd = headerCol(sh.headers, {QStringLiteral("顶界")});
    if (cTop < 0 || cMd < 0)
      continue; // 非分层 sheet（说明页等）——跳过不硬猜
    bool wrote = false;
    for (const QStringList &r : sh.rows)
    {
      if (r.size() <= std::max(cTop, cMd))
        continue;
      double md = 0;
      if (!paleo::io::parseNumericCell(r.at(cMd), &md))
        continue;
      const QString top = r.at(cTop).trimmed();
      if (top.isEmpty())
        continue;
      ts << well << ' ' << top << ' ' << md << '\n';
      ++rows;
      wrote = true;
    }
    if (wrote)
      ++wells;
  }
  f.close();
  if (rows == 0)
  {
    *error = QStringLiteral("分层表无有效行");
    return false;
  }
  std::printf("  分层转换: %d 口井 %d 行\n", wells, rows);
  return true;
}

// demo 工区地理配准（2026-10-06 拟合；控制点 = 7 口井 LAS 头泄露经纬度）。
PaleoGeoreference ordosGeoreference()
{
  PaleoGeoreference g;
  g.kind = QStringLiteral("similarity2d");
  g.targetCrs = QStringLiteral("EPSG:4326");
  g.anchorLonDeg = 108.05;
  g.anchorLatDeg = 36.10;
  g.metersPerDegLon = 90049.687955;
  g.metersPerDegLat = 110960.830261;
  g.a = 1.000896238;
  g.b = 0.030275703;
  g.tE = -6182.244744;
  g.tN = -9871.264523;
  g.formula = QStringLiteral(
      "E = a*x - b*y + tE ; N = b*x + a*y + tN ; "
      "lon = lon0 + E/mPerLon ; lat = lat0 + N/mPerLat (meters)");
  g.provenance = QStringLiteral(
      "7 口井（A2/A4/A5/A6/A10/A12/A18）LAS 头 Longitude/Latitude 泄露值的最小"
      "二乘相似变换拟合（2026-10-06）；scale=1.001354，rotation=+1.733deg，"
      "残差 4.7-48.7m");
  struct Cp { const char *w; double x, y, lon, lat, r; };
  const Cp cps[] = {
      {"A2", 3720.83, 3899.60, 108.021518697795, 36.047302568879, 14.0},
      {"A4", 5627.18, 5359.59, 108.042150198982, 36.0609662485938, 7.6},
      {"A5", 3529.68, 15162.10, 108.015900907312, 36.1489128577844, 41.1},
      {"A6", 1189.14, 13567.65, 107.989473075707, 36.1336536752268, 48.7},
      {"A10", 10547.09, 11754.19, 108.094569386109, 36.1198504511991, 11.3},
      {"A12", 6533.51, 12189.22, 108.049894892643, 36.1226763903836, 10.8},
      {"A18", 8941.95, 6866.63, 108.078376693022, 36.0754294038299, 4.7}};
  for (const Cp &c : cps)
  {
    PaleoGeoreference::ControlPoint cp;
    cp.well = QString::fromLatin1(c.w);
    cp.x = c.x;
    cp.y = c.y;
    cp.lon = c.lon;
    cp.lat = c.lat;
    cp.residualM = c.r;
    g.controlPoints.append(cp);
  }
  g.maxResidualM = 48.7;
  return g;
}

// 导入一件并断言成功（AlreadyStored 也算过——重跑幂等）。
bool importOne(DataImportService &svc, const QString &path,
               const QString &forceType = QString())
{
  DataImportService::ImportOptions opts;
  opts.forceType = forceType;
  QString err;
  const DataImportService::ImportResult r =
      svc.importProjectFileEx(path, opts, &err);
  if (r.outcome == DataImportService::ImportOutcome::Failed)
  {
    note(false, QStringLiteral("导入 %1: %2").arg(QFileInfo(path).fileName(), err));
    return false;
  }
  const char *outcome =
      r.outcome == DataImportService::ImportOutcome::AlreadyStored ? "已在库"
                                                                   : "入库";
  std::printf("  导入 %-24s %s\n", QFileInfo(path).fileName().toUtf8().constData(),
              outcome);
  return true;
}
} // namespace

int main(int argc, char *argv[])
{
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(
      qEnvironmentVariable("QGIS_PREFIX_PATH", QgisRuntime::defaultPrefixPath()),
      true);
  app.initQgis();

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("paleo_workstation demo 工程无头构建（鄂尔多斯竞赛数据；"
                     "缺省就地模式：工程束生成在数据根里）"));
  parser.addOptions({
      {{"o", "out"},
       QStringLiteral("工程输出目录（缺省=数据根——就地工程，project.paleo "
                      "直接落在数据夹内）"),
       "dir"},
      {{"d", "data"}, QStringLiteral("paleo_data 数据根目录"), "dir"},
      {{"n", "name"}, QStringLiteral("工程名（缺省=工程目录名）"), "name"},
  });
  parser.process(app);
  const QString dataRoot = parser.value("data");
  if (dataRoot.isEmpty())
  {
    std::printf("用法: paleo_mkproject --data <paleo_data 根> [--out 目录] "
                "[--name 名字]\n");
    return 2;
  }
  // 就地工程：缺省 --out = 数据根（PROJECT_FILE_DESIGN「从工区文件夹新建」
  // 契约——<目录名>.qgz + project.paleo 落在数据夹内，GUI 指到目录即识别）。
  const QString outDir =
      parser.isSet("out") ? parser.value("out") : dataRoot;
  const QString name = parser.isSet("name")
                           ? parser.value("name")
                           : QFileInfo(outDir).fileName();
  const QDir data(dataRoot);
  if (!data.exists())
  {
    std::printf("数据根不存在: %s\n", qPrintable(dataRoot));
    return 2;
  }
  const QString projectName = name;
  if (QFile::exists(QDir(outDir).filePath(
          QString::fromLatin1(PaleoProjectFile::kFileName))))
  {
    std::printf("目标目录已有 project.paleo（%s）——请换目录或先删除，不覆盖"
                "既有工程。\n",
                qPrintable(outDir));
    return 2;
  }

  // 输入清单（第 9 届竞赛数据布局）。
  const QString wellCoordXlsx = requireFile(
      data.filePath(QStringLiteral("2.沉积相分析-第9届/2.1井位坐标/井位坐标.xlsx")));
  const QString stratXlsx = requireFile(
      data.filePath(QStringLiteral("2.沉积相分析-第9届/2.2分层数据/分层数据.xlsx")));
  const QString lasDir =
      data.filePath(QStringLiteral("2.沉积相分析-第9届/2.5测井资料"));
  const QString a3SeismicLas = requireFile(data.filePath(
      QStringLiteral("1.地震资料构造解释-第9届/03.测井曲线/A3_log.las")));
  const QString topsDat = requireFile(data.filePath(QStringLiteral(
      "1.地震资料构造解释-第9届/01.三维数据、工区加载相关参数及位置图/20kou_tops.dat")));
  const QString sgy = requireFile(data.filePath(QStringLiteral(
      "1.地震资料构造解释-第9届/01.三维数据、工区加载相关参数及位置图/200P_seismic.sgy")));

  std::printf("== 1/5 创建工程 %s ==\n", qPrintable(outDir));
  QgisProjectService projectSvc;
  const QString qgzPath = QDir(outDir).filePath(projectName + ".qgz");
  if (!projectSvc.createProject(qgzPath))
  {
    std::printf("createProject 失败: %s\n",
                qPrintable(projectSvc.lastErrors().join(';')));
    return 1;
  }
  note(true, QStringLiteral(".qgz + project.paleo 双件落盘"));

  std::printf("== 2/5 转换 xlsx 为规范井文本 ==\n");
  QTemporaryDir staging;
  const QString wellHeadDat =
      staging.filePath(QStringLiteral("井位坐标.dat"));
  const QString stratDat = staging.filePath(QStringLiteral("分层数据.dat"));
  QString convErr;
  if (!convertWellHeadXlsx(wellCoordXlsx, wellHeadDat, &convErr) ||
      !convertStratXlsx(stratXlsx, stratDat, &convErr))
  {
    note(false, convErr);
    return 1;
  }
  note(true, QStringLiteral("井位坐标.dat（20 井）+ 分层数据.dat（多井分层）"));

  std::printf("== 3/5 导入数据（显式清单） ==\n");
  // 与 AppContext 同款组装：store 持有 qgz/gpkg/sqlite 三路路径（导入的
  // produce-then-commit 走它），导入服务经 store 成链。
  PaleoProjectStore store;
  store.setProjectPaths(
      qgzPath,
      QFileInfo(qgzPath).absoluteDir().filePath(
          QFileInfo(qgzPath).completeBaseName() + QStringLiteral(".gpkg")),
      qgzPath + QStringLiteral(".project.sqlite"));
  DataImportService importer(&store);
  importer.setProjectDir(outDir);
  importer.setGeoreference(ordosGeoreference());
  int imported = 0;
  // 井口先走（两阶段导入的井建齐约定）；forceType 固定类型不赌目录规则。
  imported += importOne(importer, wellHeadDat, QStringLiteral("well_head"));
  imported += importOne(importer, stratDat, QStringLiteral("well_stratification"));
  imported += importOne(importer, topsDat, QStringLiteral("well_stratification"));
  for (int i = 1; i <= 20; ++i)
    imported += importOne(importer,
                          QDir(lasDir).filePath(QStringLiteral("A%1_log.las").arg(i)));
  imported += importOne(importer, a3SeismicLas); // A3 第二份 LAS（多文件井）
  imported += importOne(importer, sgy);          // 外链 + 几何冻结

  // 井数据全景（导入侧井附件通道：文件名/父目录识井 + 关键词角色 + 照片
  // 深度锚）——岩心照片→core（图片井道）、岩屑录井→cuttings（岩性道）、
  // 薄片/粒度→lab_analysis；参考井合成地震记录→井域 reference。
  int corePhotos = 0, cuttingsFiles = 0, labFiles = 0;
  {
    const struct
    {
      const char *rel;
      const char *what;
    } dirs[] = {
        {"2.沉积相分析-第9届/2.3岩心资料", "岩心照片"},
        {"2.沉积相分析-第9届/2.6薄片岩矿鉴定", "薄片"},
        {"2.沉积相分析-第9届/2.7粒度分析", "粒度"},
    };
    for (const auto &d : dirs)
    {
      const QString root = data.filePath(QString::fromUtf8(d.rel));
      QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext())
      {
        const QString p = it.next();
        if (it.fileInfo().size() == 0)
          continue; // 空占位件
        if (importOne(importer, p))
        {
          ++imported;
          if (p.contains(QString::fromUtf8("岩心")))
            ++corePhotos;
          else if (p.contains(QString::fromUtf8("粒度")) ||
                   p.contains(QString::fromUtf8("薄片")))
            ++labFiles;
        }
      }
    }
    const QString cutDir =
        data.filePath(QStringLiteral("2.沉积相分析-第9届/2.4岩屑录井数据"));
    QDirIterator cit(cutDir, QDir::Files);
    while (cit.hasNext())
    {
      const QString p = cit.next();
      if (cit.fileInfo().size() == 0)
        continue;
      if (importOne(importer, p))
      {
        ++imported;
        ++cuttingsFiles;
      }
    }
  }
  // 参考井合成地震记录（A12 井域 reference）+ 工区参数文档/位置图（零匹配
  // → 辅助参考）。
  {
    QDir synthDir(data.filePath(
        QStringLiteral("1.地震资料构造解释-第9届/05.参考井合成地震记录")));
    for (const QFileInfo &f : synthDir.entryInfoList(QDir::Files))
      imported += importOne(importer, f.absoluteFilePath());
    const QString loadDir = data.filePath(QStringLiteral(
        "1.地震资料构造解释-第9届/01.三维数据、工区加载相关参数及位置图"));
    for (const QFileInfo &f :
         QDir(loadDir).entryInfoList({QStringLiteral("*.docx"), QStringLiteral("*.pptx")},
                                     QDir::Files))
      imported += importOne(importer, f.absoluteFilePath());
    imported += importOne(importer, data.filePath(QStringLiteral(
        "2.沉积相分析-第9届/2.1井位坐标/20260722-沉积相平面作图范围-ok.JPG")));
  }
  // 辅助参考测井（真实惠州工区综合柱状图，外委工作簿口径）：外链入库不复制
  //（22×几十~几百 MB），零井匹配 → 辅助参考实体。
  int auxRefs = 0;
  {
    QDir auxDir(data.filePath(QStringLiteral("辅助-参考测井excel")));
    for (const QFileInfo &f : auxDir.entryInfoList({QStringLiteral("*.xml")}, QDir::Files))
    {
      DataImportService::ImportOptions opts;
      opts.forceType = QStringLiteral("outsource_workbook");
      opts.linkExternal = true;
      QString err;
      if (importer.importProjectFileEx(f.absoluteFilePath(), opts, &err).outcome !=
          DataImportService::ImportOutcome::Failed)
      {
        ++imported;
        ++auxRefs;
      }
      else
        note(false, QStringLiteral("导入 %1: %2").arg(f.fileName(), err));
    }
  }
  std::printf("  井数据全景: 岩心照片 %d、岩屑 %d、薄片/粒度 %d、辅助外链 %d\n",
              corePhotos, cuttingsFiles, labFiles, auxRefs);
  imported += importOne(importer, wellCoordXlsx); // 原 xlsx 作 reference（溯源）
  imported += importOne(importer, stratXlsx);
  if (fails)
    return 1;

  std::printf("== 4/5 写 georeference + sourceArea ==\n");
  {
    bool ok = false;
    QString rerr;
    PaleoProjectFile pf = readProjectFile(
        paleoProjectFilePath(outDir), &ok, &rerr);
    if (!ok)
    {
      note(false, QStringLiteral("回读 project.paleo: %1").arg(rerr));
      return 1;
    }
    pf.georeference = ordosGeoreference();
    pf.sourceAreaRoot = dataRoot;
    pf.sourceAreaImportedUtc =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    pf.sourceStats.insert(QStringLiteral("files"), imported);
    QString werr;
    if (!writeProjectFile(outDir, pf, &werr))
    {
      note(false, QStringLiteral("写 project.paleo: %1").arg(werr));
      return 1;
    }
  }
  note(true, QStringLiteral("georeference 节 + sourceArea 溯源写入"));

  // 井名泄露兜底：A18 的 LAS ~W 井名是原始名（zhuang181），resolveWell 落
  // 未决；文件名主名可前缀识井的未决 well_log 链接，按 GUI 拖拽同款
  // attachLink 面挂回正确井——不猜（零/双匹配保持未决）。
  {
    DataCatalog *cat2 = importer.catalog();
    const auto wellsAll = cat2->entities(QStringLiteral("well"));
    for (int i = 0; i < cat2->links().size(); ++i)
    {
      const EntityAssetLink l = cat2->links().at(i);
      if (l.role != QStringLiteral("well_log") || !l.unresolved)
        continue;
      const QVector<CatalogVersion> vers = cat2->versionsForAsset(l.assetId);
      if (vers.isEmpty())
        continue;
      const QString stem = QFileInfo(vers.front().fileName).completeBaseName();
      QStringList hit;
      for (const CatalogEntity &w : wellsAll)
        if (stem.startsWith(w.name, Qt::CaseInsensitive) &&
            (stem.size() == w.name.size() ||
             !stem.at(w.name.size()).isLetterOrNumber() ||
             stem.at(w.name.size()).toLatin1() == '_'))
          hit.append(w.id);
      if (hit.size() == 1)
      {
        QString aerr;
        if (!cat2->attachLink(i, hit.front(), &aerr))
          note(false, QStringLiteral("挂回未决测井链接: %1").arg(aerr));
        else
          std::printf("  井名泄露兜底: %s → %s\n",
                      qPrintable(vers.front().fileName), qPrintable(hit.front()));
      }
    }
  }

  std::printf("== 5/5 重开自校验 ==\n");
  DataCatalog *cat = importer.catalog();
  if (!cat || !cat->isOpen())
  {
    note(false, QStringLiteral("catalog 未打开"));
    return 1;
  }
  const auto wells = cat->entities(QStringLiteral("well"));
  note(wells.size() == 20,
       QStringLiteral("井实体 20 口（实际 %1）").arg(wells.size()));
  int dualLog = 0, dualTops = 0, headOk = 0, geoOk = 0;
  for (const CatalogEntity &w : wells)
  {
    int nLog = 0, nTops = 0, nHead = 0;
    for (const EntityAssetLink &l : cat->linksForEntity(w.id))
    {
      if (l.role == QStringLiteral("well_log"))
        ++nLog;
      else if (l.role == QStringLiteral("tops"))
        ++nTops;
      else if (l.role == QStringLiteral("well_head"))
        ++nHead;
    }
    if (nLog >= 2)
      ++dualLog;
    if (nTops >= 2)
      ++dualTops;
    if (nHead >= 1)
      ++headOk;
    if (w.coordinateStatus == QStringLiteral("ok") &&
        w.extra.contains(QStringLiteral("projectLon")))
      ++geoOk;
  }
  note(dualLog == 1, QStringLiteral("A3 多文件测井（实际 %1 口 ≥2 份）").arg(dualLog));
  // 井附件角色面：岩心（图片井道）/岩屑（岩性道）/实验分析/井域参考。
  int coreLinks = 0, cutLinks = 0, labLinks = 0, wellRefs = 0, unresolvedLinks = 0;
  for (const CatalogEntity &w : wells)
    for (const EntityAssetLink &l : cat->linksForEntity(w.id))
    {
      if (l.unresolved)
        ++unresolvedLinks;
      if (l.role == QStringLiteral("core"))
        ++coreLinks;
      else if (l.role == QStringLiteral("cuttings"))
        ++cutLinks;
      else if (l.role == QStringLiteral("lab_analysis"))
        ++labLinks;
      else if (l.role == QStringLiteral("reference"))
        ++wellRefs;
    }
  note(coreLinks == corePhotos,
       QStringLiteral("岩心照片挂井 core %1/%2（图片井道，含 depthMd 锚）")
           .arg(coreLinks)
           .arg(corePhotos));
  note(cutLinks == cuttingsFiles,
       QStringLiteral("岩屑录井挂井 cuttings %1/%2（岩性道；源数据缺 A8/A13/A16/A17 四井）")
           .arg(cutLinks)
           .arg(cuttingsFiles));
  note(labLinks == labFiles, QStringLiteral("薄片/粒度挂井 lab_analysis %1/%2").arg(labLinks).arg(labFiles));
  note(unresolvedLinks == 0,
       QStringLiteral("井附件未决链接 %1（应为 0）").arg(unresolvedLinks));
  note(dualTops == 20, QStringLiteral("每井双分层（实际 %1 口 ≥2 份）").arg(dualTops));
  note(headOk == 20, QStringLiteral("每井井口关联（实际 %1）").arg(headOk));
  note(geoOk == 20, QStringLiteral("配准应用（coordinateStatus=ok + 经纬度，实际 %1）").arg(geoOk));

  const auto surveys = cat->entities(QStringLiteral("seismic_survey"));
  if (!surveys.isEmpty())
  {
    const CatalogEntity &s = surveys.front();
    note(s.xlineMin == 4165 && s.xlineMax == 4805 &&
             s.inlineMin == 1315 && s.inlineMax == 1725,
         QStringLiteral("地震几何 inline 1315-1725 × xline 4165-4805"
                        "（实际 %1-%2 × %3-%4）")
             .arg(s.inlineMin)
             .arg(s.inlineMax)
             .arg(s.xlineMin)
             .arg(s.xlineMax));
    note(s.extra.contains(QStringLiteral("wgs84BboxWest")),
         QStringLiteral("survey WGS84 包围盒: W%1 E%2 S%3 N%4")
             .arg(s.extra.value(QStringLiteral("wgs84BboxWest")).toDouble(), 0, 'f', 4)
             .arg(s.extra.value(QStringLiteral("wgs84BboxEast")).toDouble(), 0, 'f', 4)
             .arg(s.extra.value(QStringLiteral("wgs84BboxSouth")).toDouble(), 0, 'f', 4)
             .arg(s.extra.value(QStringLiteral("wgs84BboxNorth")).toDouble(), 0, 'f', 4));
  }
  else
    note(false, QStringLiteral("地震 survey 实体缺失"));

  QgisProjectService reopen;
  const bool opened =
      reopen.openProject(paleoProjectFilePath(outDir));
  if (!reopen.lastErrors().isEmpty())
    std::printf("  重开 lastErrors: %s\n",
                qPrintable(reopen.lastErrors().join(';')));
  note(opened && reopen.georeference().has_value(),
       QStringLiteral("openProject(project.paleo) 重开 + georeference 解析"));

  if (fails)
    std::printf("== 失败 %d 项 ==\n", fails);
  else
    std::printf("== 全部通过：工程就绪 %s ==\n",
                qPrintable(paleoProjectFilePath(outDir)));
  return fails ? 1 : 0;
}
