#include <QtTest>

// DataCatalog 切到 catalog.sqlite 之前的占位。后续轮次换成 Oracle 2/3/4/6/7。
class TestDataCatalogSqlite : public QObject
{
  Q_OBJECT

private slots:
  void notSwitchedYet()
  {
    QSKIP("DataCatalog still uses catalog.json; sqlite switch is a later round");
  }
};

QTEST_MAIN(TestDataCatalogSqlite)
#include "tst_datacatalog_sqlite.moc"
