// 层：测试壳
// 交会图井曲线清单与数据页「设为主文件」。offscreen，不进 QGIS。
#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include "catalog/datacatalog.h"
#include "io/dataimportservice.h"
#include "services/crossplotsources.h"
#include "services/previewdoc.h"
#include "ui/pages/datapage.h"

namespace
{

QString writeLas(const QString &path, const QStringList &mnems)
{
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return QString();
  QTextStream out(&file);
  out << "~Version Information\n";
  out << " VERS.                  2.0:   CWLS log ASCII Standard -VERSION 2.0\n";
  out << " WRAP.                   NO:   One line per depth step\n";
  out << "~Well Information Block\n";
  out << " STRT.M        1000.0000:\n";
  out << " STOP.M        1002.0000:\n";
  out << " STEP.M           1.0000:\n";
  out << " NULL.        -999.2500:\n";
  out << "~Curve Information Block\n";
  for (const QString &mnem : mnems)
    out << " " << mnem << ".                  :   " << mnem << "\n";
  out << "~A\n";
  out << "1000.00 1.00\n";
  out << "1001.00 2.00\n";
  out << "1002.00 3.00\n";
  file.close();
  return path;
}

bool addExternal(DataCatalog &cat, const QString &assetId, const QString &versionId,
                 const QString &type, const QString &path, const QString &sha,
                 QString *err)
{
  CatalogAsset asset;
  asset.id = assetId;
  asset.type = type;
  asset.format = QFileInfo(path).suffix().toLower();
  asset.displayName = QFileInfo(path).fileName();
  if (!cat.addAsset(asset, err))
    return false;
  CatalogVersion version;
  version.id = versionId;
  version.assetId = assetId;
  version.stage = QStringLiteral("RAW");
  version.versionNumber = 1;
  version.managed = false;
  version.path = path;
  version.sha256 = sha;
  version.fileName = asset.displayName;
  return cat.addVersion(version, err);
}

bool addLink(DataCatalog &cat, const QString &wellId, const QString &assetId,
             const QString &role, int ordinal, bool primary, bool unresolved,
             QString *err)
{
  EntityAssetLink link;
  link.entityType = QStringLiteral("well");
  link.entityId = wellId;
  link.assetId = assetId;
  link.role = role;
  link.ordinal = ordinal;
  link.isPrimary = primary;
  link.unresolved = unresolved;
  return cat.addLink(link, err);
}

const paleo::crossplot::SourceSpec *findCurve(const QVector<paleo::crossplot::SourceSpec> &specs,
                                              const QString &curve)
{
  for (const paleo::crossplot::SourceSpec &spec : specs)
    if (spec.curve == curve)
      return &spec;
  return nullptr;
}

bool resolvedWellLogPrimary(const DataCatalog &cat, const QString &assetId)
{
  for (const EntityAssetLink &link : cat.linksForAsset(assetId))
    if (link.role == QLatin1String("well_log") && !link.unresolved)
      return link.isPrimary;
  return false;
}

QStringList wellLogTexts(QTreeWidgetItem *well, int column)
{
  QStringList texts;
  if (!well)
    return texts;
  for (int i = 0; i < well->childCount(); ++i)
  {
    QTreeWidgetItem *child = well->child(i);
    if (child->data(0, Qt::UserRole + 2).toString() == QLatin1String("well_log"))
      texts << child->text(column);
  }
  return texts;
}

} // namespace

class TestWellLogUi : public QObject
{
  Q_OBJECT

private slots:
  void inventoryDisambiguatesDuplicateGr();
  void wellLogPrimaryButtonPromotesLink();
};

void TestWellLogUi::inventoryDisambiguatesDuplicateGr()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString pathPrimary = writeLas(dir.filePath(QStringLiteral("primary.las")),
                                       {QStringLiteral("DEPT"), QStringLiteral("GR")});
  const QString pathSecondary = writeLas(dir.filePath(QStringLiteral("secondary.las")),
                                         {QStringLiteral("DEPT"), QStringLiteral("GR")});
  QVERIFY(!pathPrimary.isEmpty());
  QVERIFY(!pathSecondary.isEmpty());

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  CatalogEntity well;
  well.id = QStringLiteral("well-1");
  well.entityType = QStringLiteral("well");
  well.name = QStringLiteral("W1");
  well.hasSurface = true;
  well.surfaceX = 10.0;
  well.surfaceY = 20.0;
  QVERIFY2(cat.addEntity(well, &err), qPrintable(err));
  QVERIFY2(addExternal(cat, QStringLiteral("ast-primary"), QStringLiteral("ver-primary"),
                       QStringLiteral("well_log"), pathPrimary, QStringLiteral("sha-primary"),
                       &err),
           qPrintable(err));
  QVERIFY2(addExternal(cat, QStringLiteral("ast-secondary"), QStringLiteral("ver-secondary"),
                       QStringLiteral("well_log"), pathSecondary, QStringLiteral("sha-secondary"),
                       &err),
           qPrintable(err));
  QVERIFY2(addExternal(cat, QStringLiteral("ast-pending"), QStringLiteral("ver-pending"),
                       QStringLiteral("well_log"),
                       dir.filePath(QStringLiteral("pending.las")), QStringLiteral("sha-pending"),
                       &err),
           qPrintable(err));
  QVERIFY2(addExternal(cat, QStringLiteral("ast-tops"), QStringLiteral("ver-tops"),
                       QStringLiteral("tops"), dir.filePath(QStringLiteral("tops.csv")),
                       QStringLiteral("sha-tops"), &err),
           qPrintable(err));
  // 先主后次：次文件 isPrimary=false，addLink 不会把主文件降下去。
  QVERIFY2(addLink(cat, well.id, QStringLiteral("ast-primary"), QStringLiteral("well_log"),
                   0, true, false, &err),
           qPrintable(err));
  QVERIFY2(addLink(cat, well.id, QStringLiteral("ast-secondary"), QStringLiteral("well_log"),
                   1, false, false, &err),
           qPrintable(err));
  QVERIFY2(addLink(cat, well.id, QStringLiteral("ast-pending"), QStringLiteral("well_log"),
                   5, false, true, &err),
           qPrintable(err));
  QVERIFY2(addLink(cat, well.id, QStringLiteral("ast-tops"), QStringLiteral("tops"),
                   0, false, false, &err),
           qPrintable(err));

  const QString secondaryMnemonic =
      QStringLiteral("GR@") + QFileInfo(pathSecondary).completeBaseName();
  const QVector<paleo::crossplot::SourceSpec> specs =
      paleo::crossplot::CrossplotSources::inventory(&cat, dir.path(), {});
  QCOMPARE(specs.size(), 2);

  const paleo::crossplot::SourceSpec *primary = findCurve(specs, QStringLiteral("GR"));
  const paleo::crossplot::SourceSpec *secondary = findCurve(specs, secondaryMnemonic);
  QVERIFY(primary);
  QVERIFY(secondary);
  QVERIFY(primary->versionId != secondary->versionId);
  QCOMPARE(primary->versionId, QStringLiteral("ver-primary"));
  QCOMPARE(secondary->versionId, QStringLiteral("ver-secondary"));
  QCOMPARE(primary->choice.id,
           QStringLiteral("well-1|ver-primary|GR"));
  QCOMPARE(secondary->choice.id,
           QStringLiteral("well-1|ver-secondary|") + secondaryMnemonic);
  QCOMPARE(primary->choice.title,
           QStringLiteral("W1 · GR · primary.las"));
  QCOMPARE(secondary->choice.title,
           QStringLiteral("W1 · %1 · secondary.las").arg(secondaryMnemonic));
  QCOMPARE(primary->choice.kind, QStringLiteral("well"));
  QCOMPARE(secondary->choice.kind, QStringLiteral("well"));
  QCOMPARE(primary->path, pathPrimary);
  QCOMPARE(secondary->path, pathSecondary);
  QCOMPARE(primary->sha256, QStringLiteral("sha-primary"));
  QCOMPARE(secondary->sha256, QStringLiteral("sha-secondary"));
  QVERIFY(!primary->managed);
  QVERIFY(!secondary->managed);
  QCOMPARE(primary->well.wellId, QStringLiteral("well-1"));
  QCOMPARE(secondary->well.wellId, QStringLiteral("well-1"));
  QVERIFY(primary->well.hasXY);
  QVERIFY(secondary->well.hasXY);
  QCOMPARE(primary->well.x, 10.0);
  QCOMPARE(primary->well.y, 20.0);
  QCOMPARE(secondary->well.x, 10.0);
  QCOMPARE(secondary->well.y, 20.0);
}

void TestWellLogUi::wellLogPrimaryButtonPromotesLink()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString pathPrimary = writeLas(dir.filePath(QStringLiteral("primary.las")),
                                       {QStringLiteral("DEPT"), QStringLiteral("GR")});
  const QString pathSecondary = writeLas(dir.filePath(QStringLiteral("secondary.las")),
                                         {QStringLiteral("DEPT"), QStringLiteral("GR")});
  QVERIFY(!pathPrimary.isEmpty());
  QVERIFY(!pathSecondary.isEmpty());

  DataImportService svc(nullptr, nullptr);
  svc.setProjectDir(dir.path());
  DataCatalog *cat = svc.catalog();
  QVERIFY(cat && cat->isOpen());
  QString err;
  CatalogEntity well;
  well.id = QStringLiteral("well-1");
  well.entityType = QStringLiteral("well");
  well.name = QStringLiteral("W1");
  well.hasSurface = true;
  well.surfaceX = 10.0;
  well.surfaceY = 20.0;
  QVERIFY2(cat->addEntity(well, &err), qPrintable(err));
  QVERIFY2(addExternal(*cat, QStringLiteral("ast-primary"), QStringLiteral("ver-primary"),
                       QStringLiteral("well_log"), pathPrimary, QStringLiteral("sha-primary"),
                       &err),
           qPrintable(err));
  QVERIFY2(addExternal(*cat, QStringLiteral("ast-secondary"), QStringLiteral("ver-secondary"),
                       QStringLiteral("well_log"), pathSecondary, QStringLiteral("sha-secondary"),
                       &err),
           qPrintable(err));
  QVERIFY2(addExternal(*cat, QStringLiteral("ast-pending"), QStringLiteral("ver-pending"),
                       QStringLiteral("well_log"),
                       dir.filePath(QStringLiteral("pending.las")), QStringLiteral("sha-pending"),
                       &err),
           qPrintable(err));
  QVERIFY2(addExternal(*cat, QStringLiteral("ast-tops"), QStringLiteral("ver-tops"),
                       QStringLiteral("tops"), dir.filePath(QStringLiteral("tops.csv")),
                       QStringLiteral("sha-tops"), &err),
           qPrintable(err));
  QVERIFY2(addLink(*cat, well.id, QStringLiteral("ast-primary"), QStringLiteral("well_log"),
                   0, true, false, &err),
           qPrintable(err));
  QVERIFY2(addLink(*cat, well.id, QStringLiteral("ast-secondary"), QStringLiteral("well_log"),
                   1, false, false, &err),
           qPrintable(err));
  QVERIFY2(addLink(*cat, well.id, QStringLiteral("ast-pending"), QStringLiteral("well_log"),
                   5, false, true, &err),
           qPrintable(err));
  QVERIFY2(addLink(*cat, well.id, QStringLiteral("ast-tops"), QStringLiteral("tops"),
                   0, false, false, &err),
           qPrintable(err));

  DataPage page;
  auto *doc = new PreviewDocService(&svc, &page);
  page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(doc));
  // 表还是空的时候先定实体上下文。表建好后再选会一次选中多条已决资产，
  // 实体面板改画批量概要，角色表被清掉。
  page.selectAssetsForEntities({well.id});
  page.refreshAssetTable();

  auto *tree = page.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
  QVERIFY(tree);
  QTreeWidgetItem *wellItem = nullptr;
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
  {
    if ((*it)->data(0, Qt::UserRole + 2).toString() == QLatin1String("well") &&
        (*it)->data(0, Qt::UserRole + 1).toString() == well.id)
      wellItem = *it;
  }
  QVERIFY(wellItem);
  const QStringList treeNames = wellLogTexts(wellItem, 0);
  const QStringList treeKinds = wellLogTexts(wellItem, 1);
  QCOMPARE(treeNames.size(), 3);
  QVERIFY(treeNames.at(0).contains(QStringLiteral("primary.las")));
  QVERIFY(treeNames.at(0).contains(QStringLiteral("主文件")));
  QVERIFY(treeNames.at(1).contains(QStringLiteral("secondary.las")));
  QVERIFY(treeNames.at(1).contains(QStringLiteral("成员")));
  QVERIFY(treeNames.at(2).contains(QStringLiteral("pending.las")));
  QVERIFY(!treeNames.at(2).contains(QStringLiteral("主文件")));
  QCOMPARE(treeKinds.at(2), QStringLiteral("未决关联"));

  auto *roleTable = page.findChild<QTableWidget *>(QStringLiteral("entityRoleTable"));
  QVERIFY(roleTable);
  QList<int> logRows;
  for (int row = 0; row < roleTable->rowCount(); ++row)
  {
    QTableWidgetItem *roleItem = roleTable->item(row, 0);
    if (roleItem && roleItem->text() == QStringLiteral("测井曲线"))
      logRows << row;
  }
  QCOMPARE(logRows.size(), 3);
  QVERIFY(roleTable->item(logRows.at(0), 1)->text().contains(QStringLiteral("primary.las")));
  QVERIFY(roleTable->item(logRows.at(0), 1)->text().contains(QStringLiteral("主文件")));
  QVERIFY(roleTable->item(logRows.at(1), 1)->text().contains(QStringLiteral("secondary.las")));
  QVERIFY(roleTable->item(logRows.at(1), 1)->text().contains(QStringLiteral("成员")));
  QVERIFY(roleTable->item(logRows.at(2), 1)->text().contains(QStringLiteral("pending.las")));
  QVERIFY(!roleTable->item(logRows.at(2), 1)->text().contains(QStringLiteral("主文件")));
  QCOMPARE(roleTable->item(logRows.at(2), 3)->text(), QStringLiteral("未决关联"));
  QVERIFY(roleTable->cellWidget(logRows.at(0), 2) == nullptr);
  QVERIFY(roleTable->cellWidget(logRows.at(2), 2) == nullptr);

  const QList<QPushButton *> roleButtons =
      roleTable->findChildren<QPushButton *>(QStringLiteral("setWellLogPrimaryButton"));
  QCOMPARE(roleButtons.size(), 1);
  QPushButton *panelButton = roleButtons.front();
  QCOMPARE(panelButton, roleTable->cellWidget(logRows.at(1), 2));
  QCOMPARE(panelButton->text(), QStringLiteral("设为主文件"));

  auto *assetTable = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
  QVERIFY(assetTable);
  int wellLogButtons = 0;
  int versionButtons = 0;
  for (int row = 0; row < assetTable->rowCount(); ++row)
  {
    QTableWidgetItem *nameItem = assetTable->item(row, 0);
    QWidget *cell = assetTable->cellWidget(row, 2);
    if (!nameItem)
      continue;
    QPushButton *wellButton = cell ? cell->findChild<QPushButton *>(
                                         QStringLiteral("setWellLogPrimaryButton"))
                                   : nullptr;
    QPushButton *versionButton = cell ? cell->findChild<QPushButton *>(
                                            QStringLiteral("setPrimaryButton"))
                                      : nullptr;
    if (nameItem->text() == QStringLiteral("secondary.las"))
    {
      QVERIFY(wellButton);
      QCOMPARE(wellButton->text(), QStringLiteral("设为主文件"));
      QVERIFY(versionButton == nullptr);
      ++wellLogButtons;
    }
    else if (nameItem->text() == QStringLiteral("primary.las") ||
             nameItem->text() == QStringLiteral("pending.las"))
    {
      QVERIFY(wellButton == nullptr);
      QVERIFY(versionButton == nullptr);
    }
    else if (nameItem->text() == QStringLiteral("tops.csv"))
    {
      QVERIFY(versionButton);
      QCOMPARE(versionButton->text(), QStringLiteral("设为主版本"));
      QVERIFY(wellButton == nullptr);
      ++versionButtons;
    }
  }
  QCOMPARE(wellLogButtons, 1);
  QCOMPARE(versionButtons, 1);

  panelButton->click();
  QVERIFY(resolvedWellLogPrimary(*cat, QStringLiteral("ast-secondary")));
  QVERIFY(!resolvedWellLogPrimary(*cat, QStringLiteral("ast-primary")));
  for (const EntityAssetLink &link : cat->linksForAsset(QStringLiteral("ast-pending")))
    if (link.role == QLatin1String("well_log"))
      QVERIFY(link.unresolved);

  QCoreApplication::processEvents();
  logRows.clear();
  for (int row = 0; row < roleTable->rowCount(); ++row)
  {
    QTableWidgetItem *roleItem = roleTable->item(row, 0);
    if (roleItem && roleItem->text() == QStringLiteral("测井曲线"))
      logRows << row;
  }
  QCOMPARE(logRows.size(), 3);
  QVERIFY(roleTable->item(logRows.at(0), 1)->text().contains(QStringLiteral("primary.las")));
  QVERIFY(roleTable->item(logRows.at(0), 1)->text().contains(QStringLiteral("成员")));
  QVERIFY(roleTable->item(logRows.at(1), 1)->text().contains(QStringLiteral("secondary.las")));
  QVERIFY(roleTable->item(logRows.at(1), 1)->text().contains(QStringLiteral("主文件")));
  QCOMPARE(roleTable->cellWidget(logRows.at(1), 2), nullptr);
  auto *moved = qobject_cast<QPushButton *>(roleTable->cellWidget(logRows.at(0), 2));
  QVERIFY(moved);
  QCOMPARE(moved->objectName(), QStringLiteral("setWellLogPrimaryButton"));
}

int main(int argc, char *argv[])
{
  if (qgetenv(QByteArrayLiteral("QT_QPA_PLATFORM")).isEmpty())
    qputenv(QByteArrayLiteral("QT_QPA_PLATFORM"), QByteArrayLiteral("offscreen"));
  QApplication app(argc, argv);
  TestWellLogUi test;
  return QTest::qExec(&test, argc, argv);
}

#include "tst_welllog_ui.moc"
