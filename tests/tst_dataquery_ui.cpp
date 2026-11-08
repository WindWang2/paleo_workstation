// 层：测试壳
// tst_dataquery_ui — P2 端到端：真实临时工程（320 资产跨页）+ DataListPanel，
// 经实际提交面（setFilterFromStateString/列约束/搜索框/chip/预设/视图切换）
// 验证：逻辑结果五视图一致、跨页候选、漏斗约束下的可操作选区、坏分享串
// 不清空、查询零工程写入（catalog revision + 工程文件哈希 + GUI 线程 stat
// 走查，interposer 同 tst_assetpaging 先例）。
#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableView>
#include <QTreeView>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QApplication>
#include <atomic>
#include <functional>
#include <thread>
#ifdef Q_OS_LINUX
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstring>
#endif

#include "../src/io/dataimportservice.h"
#include "../src/services/previewdoc.h"
#include "../src/ui/pages/datalist.h"
#include "../src/ui/paleotheme.h"

#ifdef Q_OS_LINUX
namespace {
std::atomic<bool> traceStats{false};
std::atomic<int> uiArtifactStats{0};
std::thread::id guiThread;
void countStat(const char *path) {
  if (traceStats && path && std::this_thread::get_id() == guiThread &&
      (std::strstr(path, "/artifacts/") || std::strstr(path, "/raw/") ||
       std::strstr(path, "/derived/")))
    ++uiArtifactStats;
}
} // namespace
extern "C" int statx(int fd, const char *path, int flags, unsigned mask, struct statx *out) noexcept {
  static auto original = reinterpret_cast<int(*)(int, const char *, int, unsigned, struct statx *)>(dlsym(RTLD_NEXT, "statx"));
  countStat(path); return original(fd, path, flags, mask, out);
}
extern "C" int stat(const char *path, struct stat *out) noexcept {
  static auto original = reinterpret_cast<int(*)(const char *, struct stat *)>(dlsym(RTLD_NEXT, "stat"));
  countStat(path); return original(path, out);
}
extern "C" int lstat(const char *path, struct stat *out) noexcept {
  static auto original = reinterpret_cast<int(*)(const char *, struct stat *)>(dlsym(RTLD_NEXT, "lstat"));
  countStat(path); return original(path, out);
}
#endif

namespace {

// ---- 确定性夹具规格（320 资产，跨页：>200 触发逻辑分页）-----------------------
constexpr int kAssetCount = 320;
const char *kTypes[4] = {"well_log", "tops", "seismic", "horizon"};

QString assetId(int i) { return QStringLiteral("ast-%1").arg(i, 4, 10, QLatin1Char('0')); }
QString assetName(int i)
{
  if (i == kAssetCount - 2 || i == kAssetCount - 1)
    return QStringLiteral("重复井.las"); // 同名不同 ID 对
  return QStringLiteral("W%1-data.%2").arg(i).arg(kTypes[i % 4]);
}
QString catalogType(int i)
{
  if (i == 301)
    return QStringLiteral("tops");
  return QLatin1String(kTypes[i % 4]);
}
// 有效类型口径（含 ast-0301 的用户改写 tops→horizon；与夹具 sidecar 一致）。
QString effectiveType(int i)
{
  if (i == 301)
    return QStringLiteral("custom-type-x"); // 仅存在于覆盖值的类型
  return QLatin1String(kTypes[i % 4]);
}
QString wellId(int i) { return QStringLiteral("well-%1").arg(i % 10); }
bool linked(int i) { return i % 3 != 0; }
bool warned(int i) { return i % 50 == 5; }
QStringList tagsOf(int i)
{
  QStringList t;
  if (i % 7 == 0) t << QStringLiteral("标签-A");
  if (i % 11 == 0) t << QStringLiteral("标签-B");
  return t;
}

// 测试侧独立 oracle 枚举（与被测 matcher 无关，按同一夹具规格枚举）。
QSet<QString> oracleVisible(const std::function<bool(int)> &pred)
{
  QSet<QString> out;
  for (int i = 0; i < kAssetCount; ++i)
    if (pred(i))
      out.insert(assetId(i));
  return out;
}

void buildProject(DataImportService &svc)
{
  DataCatalog *cat = svc.catalog();
  for (int w = 0; w < 10; ++w)
  {
    CatalogEntity e;
    e.id = wellId(w);
    e.entityType = QStringLiteral("well");
    e.name = QStringLiteral("well-%1").arg(w);
    QVERIFY2(cat->addEntity(e), "add well");
  }
  for (int i = 0; i < kAssetCount; ++i)
  {
    CatalogAsset a;
    a.id = assetId(i);
    a.type = catalogType(i);
    a.displayName = assetName(i);
    QVERIFY2(cat->addAsset(a), "add asset");
    if (linked(i))
    {
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.entityId = wellId(i);
      l.assetId = a.id;
      l.role = QStringLiteral("well_log");
      QVERIFY2(cat->addLink(l), "add link");
    }
    if (warned(i))
    {
      EntityAssetLink u;
      u.entityType = QStringLiteral("well");
      u.assetId = a.id;
      u.role = QStringLiteral("well_log");
      u.unresolved = true;
      u.note = QStringLiteral("未匹配井名");
      QVERIFY2(cat->addLink(u), "add unresolved link");
    }
  }
  // sidecar：标签 + 类型改写（面板 refresh 时经 loadStoresForCatalog 读取）。
  const QString pd = svc.projectDir();
  QDir().mkpath(pd + QStringLiteral("/.paleo"));
  QFile tags(pd + QStringLiteral("/.paleo/asset_tags.json"));
  QVERIFY2(tags.open(QIODevice::WriteOnly), "open tags sidecar");
  {
    QJsonObject root;
    QJsonObject tagMap;
    for (int i = 0; i < kAssetCount; ++i)
    {
      const QStringList t = tagsOf(i);
      if (t.isEmpty())
        continue;
      QJsonArray arr;
      for (const QString &x : t)
        arr.append(x);
      tagMap.insert(assetId(i), arr);
    }
    root.insert(QStringLiteral("tags"), tagMap);
    tags.write(QJsonDocument(root).toJson());
  }
  QFile ov(pd + QStringLiteral("/.paleo/asset_overrides.json"));
  QVERIFY2(ov.open(QIODevice::WriteOnly), "open override sidecar");
  {
    QJsonObject root;
    QJsonObject types;
    types.insert(assetId(301), QStringLiteral("custom-type-x"));
    root.insert(QStringLiteral("types"), types);
    ov.write(QJsonDocument(root).toJson());
  }
}

QByteArray dirHash(const QString &path)
{
  QCryptographicHash h(QCryptographicHash::Sha256);
  QDir dir(path);
  const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                         QDir::Name);
  for (const QFileInfo &fi : entries)
  {
    h.addData(fi.filePath().toUtf8());
    if (fi.isDir())
      h.addData(dirHash(fi.absoluteFilePath()));
    else if (QFile f(fi.absoluteFilePath()); f.open(QIODevice::ReadOnly))
      h.addData(f.readAll());
  }
  return h.result();
}

int visibleCountFromLabel(DataListPanel *panel)
{
  auto *label = panel->findChild<QLabel *>(QStringLiteral("assetCountLabel"));
  QRegularExpression re(QStringLiteral("(\\d+) / 共 (\\d+)"));
  const auto m = re.match(label->text());
  if (m.hasMatch())
    return m.captured(1).toInt();
  QRegularExpression total(QStringLiteral("共 (\\d+)"));
  const auto m2 = total.match(label->text());
  return m2.hasMatch() ? m2.captured(1).toInt() : -1;
}

} // namespace

class TestDataQueryUi : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    PaleoTheme::ensureApplicationFonts();
    qApp->setFont(PaleoTheme::bodyFont());
    PaleoTheme::applyLightTheme();
#ifdef Q_OS_LINUX
    guiThread = std::this_thread::get_id();
#endif
  }

  void init()
  {
#ifdef Q_OS_LINUX
    uiArtifactStats = 0;
#endif
  }

  // 独立夹具：每用例自建工程（svc/doc 以本测试对象为父，随用例销毁）。
  DataListPanel *makePanel(QTemporaryDir *dir, DataImportService **svcOut)
  {
    if (!dir->isValid())
      return nullptr; // QVERIFY 不能出现在返回指针的函数体里
    auto *svc = new DataImportService(nullptr, this);
    svc->setProjectDir(dir->path());
    buildProject(*svc);
    auto *doc = new PreviewDocService(svc, this);
    *svcOut = svc;
    auto *panel = new DataListPanel();
    panel->setDocService(doc);
    panel->resize(640, 740);
    panel->setViewMode(1);
    panel->show();
    // setDocService 只换指针不刷新（正常由壳接 catalogChanged）；直连面板的
    // 夹具按公开槽位显式刷一次。
    panel->refreshAssetTable();
    QCoreApplication::processEvents();
    return panel;
  }

  // ---- W1/W5：仅否定条件 + 全选口径 = 完整逻辑范围 ------------------------
  void negativeOnlySurvivesRefreshAndMirror()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    QCOMPARE(int(svc->catalog()->assets().size()), kAssetCount);
    // 仅否定条件：非类型 seismic。旧搜索框/类型框均为空。
    paleo::dataops::query::DataQuery q;
    q.group.add({paleo::dataops::FilterDim::Type, QStringLiteral("seismic"), true});
    panel->applyQuery(q);
    auto *search = panel->findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
    QVERIFY(search->text().isEmpty()); // 镜像 = 首个正向条件（无）→ 空，且不删条件
    // 刷新（refreshAssetTable → applyListFilter → 镜像）不得删除否定条件。
    panel->refreshAssetTable();
    QCoreApplication::processEvents();
    QCOMPARE(panel->filterGroup().conditions.size(), 1);
    QVERIFY(panel->filterGroup().conditions.at(0).negate);
    // 全选口径 = 完整逻辑范围（含当前页外）：oracle 手推非 seismic。
    const QSet<QString> expected = oracleVisible([](int i) {
      return effectiveType(i) != QStringLiteral("seismic");
    });
    QCOMPARE(int(expected.size()), 240); // 320 - 80 个 i%4==2
    panel->selectAllVisibleAssets();
    QCOMPARE(panel->currentAssetSelection(), expected);
    // 反选以同一范围为全集。
    panel->invertAssetSelection();
    QCOMPARE(panel->currentAssetSelection(), QSet<QString>{});
    panel->invertAssetSelection();
    QCOMPARE(panel->currentAssetSelection(), expected);
    delete panel;
  }

  // ---- W3/W5：列约束进入查询；隐藏选区不达批操作；清除后恢复 -------------
  void funnelConstraintBoundsSelectionAndRestores()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    panel->selectAssetInViews(assetId(4)); // i=4：well_log、已链接
    QTRY_VERIFY(panel->currentAssetSelection().contains(assetId(4)));
    // 漏斗提交（生产菜单的同一提交面）：类型只留 tops。
    panel->setColumnConstraint(paleo::dataops::query::ColumnKey::Type,
                               {QStringLiteral("tops")});
    // 约束进查询 + chip 可见可清除。
    QVERIFY(panel->currentQuery().column(paleo::dataops::query::ColumnKey::Type));
    auto chips = panel->findChildren<QPushButton *>(QStringLiteral("columnChip"));
    QCOMPARE(chips.size(), 1);
    // 被漏斗隐藏的选中资产不得再进批操作输入。
    QVERIFY(!panel->currentAssetSelection().contains(assetId(4)));
    QVERIFY(panel->currentAssetSelection().isEmpty());
    // 逻辑数量 = oracle（tops 79 个：80 - ast-0301 被改写为 custom-type-x）。
    panel->selectAllVisibleAssets();
    const QSet<QString> expected = oracleVisible([](int i) {
      return effectiveType(i) == QStringLiteral("tops");
    });
    QCOMPARE(panel->currentAssetSelection(), expected);
    QCOMPARE(int(expected.size()), 79);
    // 清除（chip 点击）→ 记忆选区恢复（既定契约：过滤解除后身份恢复）：
    // 先把被隐藏身份重新选中（selectAll 已替换选区），再清约束。
    panel->selectAssetInViews(assetId(4));
    QVERIFY(!panel->currentAssetSelection().contains(assetId(4))); // 仍被隐藏
    chips = panel->findChildren<QPushButton *>(QStringLiteral("columnChip"));
    QCOMPARE(chips.size(), 1);
    chips.at(0)->click();
    QTRY_VERIFY(panel->currentAssetSelection().contains(assetId(4)));
    delete panel;
  }

  // ---- W7：候选值跨页（值只在其他页出现也列出）----------------------------
  void funnelCandidatesSpanLogicalSet()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    // 320 资产 → 分页模式（>200）。
    QVERIFY(panel->findChild<QWidget *>(QStringLiteral("assetPager"))->isVisible());
    // 名字约束到单资产 → 当前页只有 1 行可见。
    panel->setColumnConstraint(paleo::dataops::query::ColumnKey::Name,
                               {assetName(0)});
    QCOMPARE(visibleCountFromLabel(panel), 1);
    // 类型候选 = 完整逻辑资产集（应用其他条件、排除本列约束）：四种类型都在，
    // 且含零命中的已选值（ghost-type）。
    const QStringList types =
        panel->columnCandidatesFor(paleo::dataops::query::ColumnKey::Type);
    for (const char *t : {"well_log", "tops", "seismic", "horizon"})
      QVERIFY2(types.contains(QLatin1String(t)), t);
    panel->setColumnConstraint(paleo::dataops::query::ColumnKey::Type,
                               {QStringLiteral("ghost-type")});
    QCOMPARE(visibleCountFromLabel(panel), 0);
    QVERIFY(panel->columnCandidatesFor(paleo::dataops::query::ColumnKey::Type)
                .contains(QStringLiteral("ghost-type"))); // 零命中已选值仍可见
    // 空态出现且清除入口可用。
    auto *empty = panel->findChild<QWidget *>(QStringLiteral("filterEmptyState"));
    QVERIFY(!empty->isHidden());
    empty->findChild<QPushButton *>(QStringLiteral("filterEmptyClearButton"))->click();
    QCOMPARE(visibleCountFromLabel(panel), kAssetCount);
    delete panel;
  }

  // ---- W5：换页/排序/刷新/五视图切换 → 查询不丢、数量一致、选区身份稳定 ----
  void viewsPagingSortKeepQueryAndSelection()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    paleo::dataops::query::DataQuery q;
    q.group.add({paleo::dataops::FilterDim::Type, QStringLiteral("seismic"),
                 false});
    panel->applyQuery(q);
    const QString before = panel->filterStateString();
    const QSet<QString> expected = oracleVisible([](int i) {
      return effectiveType(i) == QStringLiteral("seismic");
    });
    panel->selectAssetInViews(assetId(2)); // seismic
    // 翻页。
    for (int p = 0; p < 3; ++p)
    {
      panel->findChild<QPushButton *>(QStringLiteral("assetNextPage"))->click();
      QCoreApplication::processEvents();
    }
    panel->findChild<QPushButton *>(QStringLiteral("assetPreviousPage"))->click();
    // 排序（列头点击：类型列）+ 显示顺序按排序契约检查（非降序）。
    auto *table = panel->findChild<QTableWidget *>(QStringLiteral("assetTable"));
    QTest::mouseClick(table->horizontalHeader(), Qt::LeftButton);
    QString prev;
    for (int r = 0; r < table->rowCount(); ++r)
    {
      const QString t = table->item(r, 1)->text();
      QVERIFY2(prev.isNull() || QString::localeAwareCompare(prev, t) <= 0,
               qPrintable(QStringLiteral("行序破坏排序契约: %1 > %2").arg(prev, t)));
      prev = t;
    }
    // 刷新 + 五视图切换。
    panel->refreshAssetTable();
    for (int mode = 0; mode <= 4; ++mode)
    {
      panel->setViewMode(mode);
      QCoreApplication::processEvents();
      QCOMPARE(panel->filterStateString(), before);
      panel->selectAllVisibleAssets();
      QCOMPARE(panel->currentAssetSelection(), expected);
      QVERIFY(panel->currentAssetSelection().contains(assetId(2)));
      // 逻辑数量稳定（虚拟表未 fetch 行也在范围内）。
      QCOMPARE(visibleCountFromLabel(panel), int(expected.size()));
    }
    delete panel;
  }

  // ---- W5：虚拟表未 fetch 行 + 实体节点不冒充资产 ---------------------------
  void unfetchedRowsAndEntityNodes()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    panel->setViewMode(3); // 虚拟表（fetchMore 自管，默认一批 256 < 320）
    auto *virt = panel->findChild<QTableView *>(QStringLiteral("assetVirtualTable"));
    QVERIFY(virt);
    QVERIFY(virt->model()->rowCount() < kAssetCount); // 确有未 fetch 行
    panel->selectAllVisibleAssets();
    QSet<QString> all;
    for (int i = 0; i < kAssetCount; ++i)
      all.insert(assetId(i));
    QCOMPARE(panel->currentAssetSelection(), all); // 未 fetch 行同属逻辑全集
    // 树视图实体节点：井节点选中 → 实体面非空、资产面为空。
    panel->setViewMode(0);
    auto *tree = panel->findChild<QTreeWidget *>(QStringLiteral("dataTree"));
    QVERIFY(tree);
    bool clickedWell = false;
    for (QTreeWidgetItemIterator it(tree); *it && !clickedWell; ++it)
    {
      if (!(*it)->data(0, Qt::UserRole).toString().isEmpty())
        continue; // 资产节点跳过
      const QString well = (*it)->data(0, Qt::UserRole + 1).toString();
      if (well.isEmpty())
        continue;
      tree->setCurrentItem(*it);
      clickedWell = true;
    }
    QVERIFY(clickedWell);
    QVERIFY(!panel->currentEntitySelection().isEmpty());
    QVERIFY(panel->currentAssetSelection().isEmpty()); // 井 id 不冒充资产
    delete panel;
  }

  // ---- W4/W5：坏分享串不清空；恢复交互正常 ----------------------------------
  void badShareStringKeepsQuerySelectionAndInteraction()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    paleo::dataops::query::DataQuery q;
    q.group.add({paleo::dataops::FilterDim::Type, QStringLiteral("tops"), false});
    q.setColumnValues(paleo::dataops::query::ColumnKey::Entity,
                      {QStringLiteral("well-0")});
    panel->applyQuery(q);
    const QSet<QString> expected = oracleVisible([](int i) {
      return effectiveType(i) == QStringLiteral("tops") && linked(i) &&
             i % 10 == 0;
    });
    panel->selectAllVisibleAssets();
    QCOMPARE(panel->currentAssetSelection(), expected);
    // 坏串（未知字段）：诊断返回、当前查询与选区保持。
    const auto bad =
        panel->tryApplyQueryStateString(QStringLiteral("paleo://dataops-filter?v=2&bogus=1"));
    QVERIFY(!bad.valid);
    QVERIFY(!bad.diagnostic.isEmpty());
    QCOMPARE(panel->currentQuery().group.conditions.size(), 1);
    QVERIFY(panel->currentQuery().column(paleo::dataops::query::ColumnKey::Entity));
    QCOMPARE(panel->currentAssetSelection(), expected);
    // 失败后继续正常交互：清空再从旧搜索框改一个合法条件。
    panel->applyQuery({});
    panel->findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"))
        ->setText(assetName(311).section(QLatin1Char('.'), 0, 0));
    panel->selectAllVisibleAssets(); // 全选以新逻辑范围为全集
    const QSet<QString> after = oracleVisible([](int i) {
      return assetName(i).startsWith(QStringLiteral("W311-data"));
    });
    QCOMPARE(panel->currentAssetSelection(), after);
    // 合法旧格式（v1）照常整组替换。
    panel->setFilterFromStateString(
        QStringLiteral("paleo://dataops-filter?op=and&q=重复井"));
    const QSet<QString> dup = oracleVisible([](int i) {
      return assetName(i) == QStringLiteral("重复井.las");
    });
    QCOMPARE(int(dup.size()), 2);
    panel->selectAllVisibleAssets();
    QCOMPARE(panel->currentAssetSelection(), dup); // 同名不同 ID 都在
    // v2 全量串（含列约束+标签）恢复。
    paleo::dataops::query::DataQuery full;
    full.group.add({paleo::dataops::FilterDim::Tag, QStringLiteral("标签-B"), false});
    full.setColumnValues(paleo::dataops::query::ColumnKey::Type,
                         {QStringLiteral("horizon")});
    panel->applyQuery(full);
    const auto round = panel->tryApplyQueryStateString(panel->filterStateString());
    QVERIFY(round.valid);
    QCOMPARE(round.query.columns.size(), 1);
    const QSet<QString> tagHorizon = oracleVisible([](int i) {
      return tagsOf(i).contains(QStringLiteral("标签-B")) &&
             effectiveType(i) == QStringLiteral("horizon");
    });
    panel->selectAllVisibleAssets();
    QCOMPARE(panel->currentAssetSelection(), tagHorizon);
    delete panel;
  }

  // ---- W5：定位被过滤资产 → 如实说明、不暗中放宽 ---------------------------
  void locatingHiddenAssetExplainsAndKeepsQuery()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    panel->setColumnConstraint(paleo::dataops::query::ColumnKey::Type,
                               {QStringLiteral("tops")});
    QSignalSpy status(panel, &DataListPanel::statusMessage);
    panel->selectAssetInViews(assetId(0)); // well_log → 被隐藏
    QVERIFY(!status.isEmpty());
    bool explained = false;
    for (const auto &args : status)
      if (args.at(0).toString().contains(QStringLiteral("被当前过滤条件隐藏")))
        explained = true;
    QVERIFY(explained);
    // 查询保持、隐藏身份不进批操作。
    QVERIFY(panel->currentQuery().column(paleo::dataops::query::ColumnKey::Type));
    QVERIFY(panel->currentAssetSelection().isEmpty());
    // 清除过滤 → 身份恢复（记忆选区契约）。
    panel->clearColumnConstraint(paleo::dataops::query::ColumnKey::Type);
    QTRY_VERIFY(panel->currentAssetSelection().contains(assetId(0)));
    delete panel;
  }

  // ---- W7：查询/筛选/切视图零工程写入 ---------------------------------------
  void queryOperationsWriteNothing()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    const QString projectDir = svc->projectDir();
    const int revBefore = svc->catalog()->catalogRevision();
    const QByteArray hashBefore = dirHash(projectDir);
#ifdef Q_OS_LINUX
    traceStats = true;
#endif
    // 查询操作序列：条件、列约束、标签、排序、翻页、五视图、全选/反选、
    // 分享串往返。
    paleo::dataops::query::DataQuery q;
    q.group.add({paleo::dataops::FilterDim::Type, QStringLiteral("tops"), false});
    q.setColumnValues(paleo::dataops::query::ColumnKey::Tag,
                      {QStringLiteral("标签-A")});
    panel->applyQuery(q);
    panel->selectAllVisibleAssets();
    panel->invertAssetSelection();
    panel->setColumnConstraint(paleo::dataops::query::ColumnKey::Entity,
                               {QStringLiteral("well-1")});
    panel->clearColumnConstraint(paleo::dataops::query::ColumnKey::Entity);
    auto *table = panel->findChild<QTableWidget *>(QStringLiteral("assetTable"));
    QTest::mouseClick(table->horizontalHeader(), Qt::LeftButton);
    panel->setViewMode(3);
    panel->setViewMode(0);
    panel->setViewMode(1);
    panel->tryApplyQueryStateString(panel->filterStateString());
    panel->refreshAssetTable();
    QCoreApplication::processEvents();
#ifdef Q_OS_LINUX
    traceStats = false;
    QCOMPARE(int(uiArtifactStats), 0); // GUI 线程不逐资产碰盘
#endif
    QCOMPARE(svc->catalog()->catalogRevision(), revBefore);
    QCOMPARE(dirHash(projectDir), hashBefore);
    delete panel;
  }

  // ---- W6：窄窗 + 计数一致（数量/错误/清除入口在场）------------------------
  void narrowWindowStatusEntriesPresent()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    panel->resize(240, 420);
    QCoreApplication::processEvents();
    panel->setColumnConstraint(paleo::dataops::query::ColumnKey::Type, {});
    // 显式空集合 → 零命中 + 空态说明含列约束描述 + 清除入口在场。
    QCOMPARE(visibleCountFromLabel(panel), 0);
    auto *emptyText =
        panel->findChild<QLabel *>(QStringLiteral("filterEmptyText"));
    QVERIFY(emptyText->text().contains(QStringLiteral("列·类型")));
    QVERIFY(panel->findChild<QPushButton *>(QStringLiteral("filterEmptyClearButton")));
    QVERIFY(panel->findChild<QLabel *>(QStringLiteral("assetCountLabel")));
    QVERIFY(panel->findChild<QLabel *>(QStringLiteral("selectionBadge")));
    // 清除后数量恢复。
    panel->clearColumnConstraint(paleo::dataops::query::ColumnKey::Type);
    QCOMPARE(visibleCountFromLabel(panel), kAssetCount);
    delete panel;
  }

  // ---- W6：快捷按钮/词表与查询一致 ------------------------------------------
  void quickButtonsAndVocabConsistent()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    auto *quick = panel->findChild<QWidget *>(QStringLiteral("pendingQuickBar"));
    auto *warnedBtn = quick->findChild<QPushButton *>(QStringLiteral("quickWarned"));
    warnedBtn->click();
    QVERIFY(warnedBtn->isChecked());
    // 有警告正向条件在查询里；计数 = oracle（i%50==5 → 7 个：5..305）。
    QVERIFY(panel->currentQuery().hasPositive(paleo::dataops::FilterDim::Warned));
    panel->selectAllVisibleAssets();
    const QSet<QString> warnedSet = oracleVisible([](int i) { return warned(i); });
    QCOMPARE(panel->currentAssetSelection(), warnedSet);
    QCOMPARE(int(warnedSet.size()), 7);
    // 分享串替换后勾选态随查询（不残留）。
    panel->setFilterFromStateString(
        QStringLiteral("paleo://dataops-filter?op=and&type=horizon"));
    QVERIFY(!warnedBtn->isChecked());
    // 类型词表含仅存在于覆盖值的类型（ast-0301 → custom-type-x）——旧类型
    // 下拉仍可选、过滤走 effectiveType 口径。
    auto *tf = panel->findChild<QComboBox *>(QStringLiteral("assetTypeFilter"));
    QVERIFY(tf->findData(QStringLiteral("custom-type-x")) >= 0);
    // 再开有警告：只补正向；关掉只删正向。
    warnedBtn->click();
    QVERIFY(panel->currentQuery().hasPositive(paleo::dataops::FilterDim::Warned));
    warnedBtn->click();
    QVERIFY(!panel->currentQuery().hasPositive(paleo::dataops::FilterDim::Warned));
    delete panel;
  }

  // ---- W4/W7：预设经面板往返（列约束+标签完整含义）--------------------------
  void presetRoundTripThroughPanel()
  {
    QTemporaryDir dir;
    DataImportService *svc = nullptr;
    DataListPanel *panel = makePanel(&dir, &svc);
    QVERIFY(panel);
    paleo::dataops::query::DataQuery q;
    q.group.add({paleo::dataops::FilterDim::Type, QStringLiteral("horizon"), false});
    q.setColumnValues(paleo::dataops::query::ColumnKey::Entity,
                      {QStringLiteral("well-3")});
    panel->applyQuery(q);
    QVERIFY(paleo::dataops::query::saveQueryPreset(QStringLiteral("p2-e2e"), q));
    // 换个查询，再从预设回来：列约束/标签完整恢复，数量一致。
    panel->applyQuery({});
    QCOMPARE(visibleCountFromLabel(panel), kAssetCount);
    const auto loaded =
        paleo::dataops::query::loadQueryPreset(QStringLiteral("p2-e2e"));
    QVERIFY(loaded.valid);
    panel->applyQuery(loaded.query);
    const QSet<QString> expected = oracleVisible([](int i) {
      return effectiveType(i) == QStringLiteral("horizon") && linked(i) &&
             i % 10 == 3;
    });
    panel->selectAllVisibleAssets();
    QCOMPARE(panel->currentAssetSelection(), expected);
    QVERIFY(paleo::dataops::deleteFilterPreset(QStringLiteral("p2-e2e")));
    delete panel;
  }
};

QTEST_MAIN(TestDataQueryUi)
#include "tst_dataquery_ui.moc"
