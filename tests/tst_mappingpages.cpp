#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVariantMap>

#include "../src/services/singlefactordef.h"
#include "../src/ui/pages/pagepanels.h"

// m2/mapping-pages — 三个编图页（预测/单因素/智能编图）的贯通测试。
// 约定与 tst_panels 相同：面板只发意图信号，测试用 nullptr/null 服务栈 +
// QSignalSpy + objectName 子件查找（plain QApplication，无 QgisRuntime）。
// 文件内三个测试类共用一个 main（多类 qExec）；每页一个类，各任务段
// 只改自己的类：
//   PredictPageTests   —— 预测编图页（任务 A）
//   FactorPageTests    —— 单因素图页（任务 B；ConstraintPage 重定位）
//   ComposePageTests   —— 智能编图页（任务 C）

class PredictPageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
};

void PredictPageTests::constructsWithNullServices()
{
  PredictPage page(nullptr, nullptr);
  QVERIFY(page.findChild<QComboBox *>(QStringLiteral("horizonCombo")));
  QVERIFY(page.findChild<QComboBox *>(QStringLiteral("algoCombo")));
}

// ---- 任务 B（单因素图页）：词表/双区/信号/互斥/折叠区（平面栈）。------
// 真实栈（生成链 declare/样式落盘/等值线）在 tst_factorworkflow.cpp。
class FactorPageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
    void registryVocabularyComplete();
    void dualZoneWidgetsAndLegacyNames();
    void generateFactorPayload();
    void factorCheckIsExclusive();
    void visibilitySignalOnCheckTransitions();
    void contourRowGatingAndSignal();
    void thicknessSamplesInsideCollapsibleSection();
    void typedDrawEntries();
};

void FactorPageTests::constructsWithNullServices()
{
  ConstraintPage page(nullptr);
  QVERIFY(page.findChild<QTableWidget *>(QStringLiteral("thicknessTable")));
}

void FactorPageTests::registryVocabularyComplete()
{
  const QVector<SingleFactorDefinition> defs = SingleFactorRegistry::builtins();
  QCOMPARE(defs.size(), 7);
  // §10 固定顺序 + 稳定 id。
  const QStringList expectedIds = { QStringLiteral( "sandthick" ),
                                    QStringLiteral( "sandratio" ),
                                    QStringLiteral( "strathick" ),
                                    QStringLiteral( "poro" ),
                                    QStringLiteral( "perm" ),
                                    QStringLiteral( "welldist" ),
                                    QStringLiteral( "confidence" ) };
  for ( int i = 0; i < defs.size(); ++i )
  {
    QCOMPARE( defs.at( i ).factorId, expectedIds.at( i ) );
    QVERIFY2( !defs.at( i ).title.isEmpty(), "title must be set" );
    bool hasHan = false; // 标题须含汉字（中显示名）
    for ( const QChar &c : defs.at( i ).title )
    {
      if ( c.unicode() >= 0x4E00 && c.unicode() <= 0x9FA5 )
      {
        hasHan = true;
        break;
      }
    }
    QVERIFY2( hasHan,
              qPrintable( QStringLiteral( "title should be Chinese: %1" ).arg( defs.at( i ).title ) ) );
    QVERIFY2( !defs.at( i ).inputAssetType.isEmpty(), "inputAssetType must be set" );
    QVERIFY2( !defs.at( i ).algorithm.isEmpty(), "algorithm label must be set" );
    QVERIFY2( !defs.at( i ).processingAlgId.isEmpty(), "processingAlgId must be set" );
    QCOMPARE( defs.at( i ).styleRef, QStringLiteral( "factor_%1" ).arg( defs.at( i ).factorId ) );
    QVERIFY( defs.at( i ).defaultParams.contains( QStringLiteral( "field" ) ) );
    QVERIFY( defs.at( i ).defaultParams.contains( QStringLiteral( "cellSize" ) ) );
  }
  QCOMPARE( defs.at( 0 ).title, QStringLiteral( "砂体厚度" ) );
  QCOMPARE( defs.at( 6 ).title, QStringLiteral( "预测置信度" ) );

  // byId 命中/未命中、titleFor 回环。
  bool ok = true;
  const SingleFactorDefinition hit = SingleFactorRegistry::byId( QStringLiteral( "poro" ), &ok );
  QVERIFY( ok );
  QCOMPARE( hit.title, QStringLiteral( "孔隙度" ) );
  SingleFactorRegistry::byId( QStringLiteral( "nope" ), &ok );
  QVERIFY( !ok );
  QCOMPARE( SingleFactorRegistry::titleFor( QStringLiteral( "welldist" ) ),
            QStringLiteral( "距井距离" ) );
  QCOMPARE( SingleFactorRegistry::titleFor( QStringLiteral( "unknown_x" ) ),
            QStringLiteral( "unknown_x" ) );
}

void FactorPageTests::dualZoneWidgetsAndLegacyNames()
{
  ConstraintPage page( nullptr );
  // 双区新件。
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  QVERIFY( factors != nullptr );
  QCOMPARE( factors->rowCount(), 7 ); // 词表行
  QCOMPARE( factors->columnCount(), 4 ); // 勾选/名称/输入/状态
  QCOMPARE( factors->item( 0, 1 )->text(), QStringLiteral( "砂体厚度" ) );
  QCOMPARE( factors->item( 0, 2 )->text(), QStringLiteral( "wells" ) );
  QCOMPARE( factors->item( 0, 3 )->text(), QStringLiteral( "未生成" ) );
  auto *field = page.findChild<QLineEdit *>(QStringLiteral("factorFieldEdit"));
  auto *cell = page.findChild<QDoubleSpinBox *>(QStringLiteral("factorCellSizeSpin"));
  auto *generate = page.findChild<QPushButton *>(QStringLiteral("generateFactorButton"));
  auto *interval = page.findChild<QDoubleSpinBox *>(QStringLiteral("contourIntervalSpin"));
  auto *contour = page.findChild<QPushButton *>(QStringLiteral("contourButton"));
  QVERIFY( field && cell && generate && interval && contour );
  QCOMPARE( field->text(), QStringLiteral( "z" ) );
  QCOMPARE( cell->value(), 1.0 );
  QCOMPARE( interval->value(), 20.0 );
  // 禁用态带 reason tooltip（DESIGN.md）。
  QVERIFY( !generate->isEnabled() );
  QVERIFY2( !generate->toolTip().isEmpty(), "disabled generate needs a reason tooltip" );
  QVERIFY( !contour->isEnabled() );
  QVERIFY2( !contour->toolTip().isEmpty(), "disabled contour needs a reason tooltip" );

  // 既有 objectName 全保留（契约：不删不改）。
  for ( const char *name : { "horizonCombo", "constraintList", "faciesCodeSpin",
                             "shapeCombo", "drawButton", "idwField", "idwCellSize",
                             "runIdwButton", "statusLabel", "thicknessCaption",
                             "thicknessTable", "thicknessHint" } )
  {
    QVERIFY2( page.findChild<QWidget *>( QString::fromLatin1( name ) ) != nullptr,
              qPrintable( QStringLiteral( "missing legacy objectName %1" ).arg( name ) ) );
  }
}

void FactorPageTests::generateFactorPayload()
{
  ConstraintPage page( nullptr );
  auto *horizons = page.findChild<QComboBox *>(QStringLiteral("horizonCombo"));
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  auto *field = page.findChild<QLineEdit *>(QStringLiteral("factorFieldEdit"));
  auto *cell = page.findChild<QDoubleSpinBox *>(QStringLiteral("factorCellSizeSpin"));
  horizons->addItem( QStringLiteral( "T1" ) );
  horizons->setCurrentIndex( 0 );
  field->setText( QStringLiteral( "sand_thick" ) );
  cell->setValue( 2.5 );

  // 未勾选 → 生成按钮禁用，点击无信号。
  QSignalSpy spy( &page, &ConstraintPage::generateFactorRequested );
  auto *generate = page.findChild<QPushButton *>(QStringLiteral("generateFactorButton"));
  QVERIFY( !generate->isEnabled() );
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  QVERIFY( generate->isEnabled() );

  generate->click();
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "sandthick" ) );
  QCOMPARE( spy.at( 0 ).at( 1 ).toString(), QStringLiteral( "T1" ) );
  const QVariantMap params = spy.at( 0 ).at( 2 ).toMap();
  QCOMPARE( params.value( QStringLiteral( "field" ) ).toString(), QStringLiteral( "sand_thick" ) );
  QCOMPARE( params.value( QStringLiteral( "cellSize" ) ).toDouble(), 2.5 );
}

void FactorPageTests::factorCheckIsExclusive()
{
  ConstraintPage page( nullptr );
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  factors->item( 0, 0 )->setCheckState( Qt::Checked ); // 砂体厚度
  QCOMPARE( factors->item( 0, 0 )->checkState(), Qt::Checked );
  factors->item( 2, 0 )->setCheckState( Qt::Checked ); // 地层厚度
  QCOMPARE( factors->item( 2, 0 )->checkState(), Qt::Checked );
  QCOMPARE( factors->item( 0, 0 )->checkState(), Qt::Unchecked ); // 互斥：A 自动取消
  int checked = 0;
  for ( int r = 0; r < factors->rowCount(); ++r )
    if ( factors->item( r, 0 )->checkState() == Qt::Checked )
      ++checked;
  QCOMPARE( checked, 1 );
}

void FactorPageTests::visibilitySignalOnCheckTransitions()
{
  ConstraintPage page( nullptr );
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  QSignalSpy spy( &page, &ConstraintPage::factorVisibilityRequested );

  // 未生成因素的勾选不带上图意图（无 layerId 可指）。
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  QCOMPARE( spy.count(), 0 );

  page.noteFactorLayer( QStringLiteral( "sandthick" ), QStringLiteral( "factor.T1.sandthick" ) );
  // 勾选行刚好生成 → 直接上图。
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "factor.T1.sandthick" ) );
  QCOMPARE( spy.at( 0 ).at( 1 ).toBool(), true );

  page.noteFactorLayer( QStringLiteral( "strathick" ), QStringLiteral( "factor.T1.strathick" ) );
  factors->item( 2, 0 )->setCheckState( Qt::Checked ); // 换看地层厚度
  QCOMPARE( spy.count(), 3 ); // 旧层 false + 新层 true
  QCOMPARE( spy.at( 1 ).at( 0 ).toString(), QStringLiteral( "factor.T1.sandthick" ) );
  QCOMPARE( spy.at( 1 ).at( 1 ).toBool(), false );
  QCOMPARE( spy.at( 2 ).at( 0 ).toString(), QStringLiteral( "factor.T1.strathick" ) );
  QCOMPARE( spy.at( 2 ).at( 1 ).toBool(), true );

  // 手动取消勾选 → 该层下图。
  factors->item( 2, 0 )->setCheckState( Qt::Unchecked );
  QCOMPARE( spy.count(), 4 );
  QCOMPARE( spy.at( 3 ).at( 0 ).toString(), QStringLiteral( "factor.T1.strathick" ) );
  QCOMPARE( spy.at( 3 ).at( 1 ).toBool(), false );
}

void FactorPageTests::contourRowGatingAndSignal()
{
  ConstraintPage page( nullptr );
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  auto *interval = page.findChild<QDoubleSpinBox *>(QStringLiteral("contourIntervalSpin"));
  auto *contour = page.findChild<QPushButton *>(QStringLiteral("contourButton"));
  QVERIFY( !contour->isEnabled() );

  // 勾选但未生成 → 仍禁用，tooltip 说明原因。
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  QVERIFY( !contour->isEnabled() );
  QVERIFY2( contour->toolTip().contains( QStringLiteral( "生成" ) ),
            "reason tooltip should say why (not generated yet)" );

  page.noteFactorLayer( QStringLiteral( "sandthick" ), QStringLiteral( "factor.T1.sandthick" ) );
  QVERIFY( contour->isEnabled() );
  interval->setValue( 15.0 );
  QSignalSpy spy( &page, &ConstraintPage::contourRequested );
  contour->click();
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "factor.T1.sandthick" ) );
  QCOMPARE( spy.at( 0 ).at( 1 ).toDouble(), 15.0 );

  // 状态列回执：noteFactorLayer 后写「已生成·<layerId>」。
  QCOMPARE( factors->item( 0, 3 )->text(), QStringLiteral( "已生成·factor.T1.sandthick" ) );
}

void FactorPageTests::thicknessSamplesInsideCollapsibleSection()
{
  ConstraintPage page( nullptr );
  // 折叠区在（默认展开），三个 objectName 仍可 findChild 命中（递归无关）。
  auto *section = page.findChild<QWidget *>(QStringLiteral("thicknessSection"));
  auto *table = page.findChild<QTableWidget *>(QStringLiteral("thicknessTable"));
  auto *caption = page.findChild<QLabel *>(QStringLiteral("thicknessCaption"));
  auto *hint = page.findChild<QLabel *>(QStringLiteral("thicknessHint"));
  QVERIFY( section && table && caption && hint );
  QVERIFY( table->isVisibleTo( section ) ); // 默认展开（不依赖整页 show）
  // 厚度表确实在折叠区容器内（挪进了 CollapsibleSection）。
  QVERIFY( table->parentWidget() != &page );
  QCOMPARE( table->columnCount(), 4 );
  QCOMPARE( caption->text().contains( QStringLiteral( "厚度样本" ) ), true );

  // 折叠后仍能 findChild（objectName 不因折叠失效），且表隐藏。
  auto *toggle = section->findChild<QToolButton *>();
  QVERIFY( toggle != nullptr );
  toggle->setChecked( false );
  QVERIFY( !table->isVisibleTo( section ) );
  QVERIFY( page.findChild<QTableWidget *>(QStringLiteral("thicknessTable")) != nullptr );
}

void FactorPageTests::typedDrawEntries()
{
  ConstraintPage page( nullptr );
  auto *horizons = page.findChild<QComboBox *>(QStringLiteral("horizonCombo"));
  auto *spin = page.findChild<QSpinBox *>(QStringLiteral("faciesCodeSpin"));
  horizons->addItem( QStringLiteral( "D61" ) );
  horizons->setCurrentIndex( 0 );
  spin->setValue( 7 );

  QSignalSpy spy( &page, &ConstraintPage::drawTypedConstraintRequested );
  auto *provenance = page.findChild<QPushButton *>(QStringLiteral("provenanceButton"));
  auto *distribution = page.findChild<QPushButton *>(QStringLiteral("distributionButton"));
  auto *controlPoint = page.findChild<QPushButton *>(QStringLiteral("controlPointButton"));
  QVERIFY( provenance && distribution && controlPoint );

  provenance->click();
  distribution->click();
  controlPoint->click();
  QCOMPARE( spy.count(), 3 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "D61" ) );
  QCOMPARE( spy.at( 0 ).at( 1 ).toString(), QStringLiteral( "line" ) );
  QCOMPARE( spy.at( 0 ).at( 2 ).toString(), QStringLiteral( "provenance_line" ) );
  QCOMPARE( spy.at( 0 ).at( 3 ).toInt(), 7 );
  QCOMPARE( spy.at( 1 ).at( 2 ).toString(), QStringLiteral( "distribution_line" ) );
  QCOMPARE( spy.at( 2 ).at( 1 ).toString(), QStringLiteral( "point" ) );
  QCOMPARE( spy.at( 2 ).at( 2 ).toString(), QStringLiteral( "control_point" ) );

  // 旧绘制链共存（drawButton → drawConstraintRequested，tst_panels 详测，此处冒烟）。
  QSignalSpy legacy( &page, &ConstraintPage::drawConstraintRequested );
  page.findChild<QPushButton *>(QStringLiteral("drawButton"))->click();
  QCOMPARE( legacy.count(), 1 );
}

class ComposePageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
};

void ComposePageTests::constructsWithNullServices()
{
  ComposePage page(nullptr, nullptr);
  QVERIFY(page.findChild<QListWidget *>(QStringLiteral("factorList")));
}

// 多测试类单可执行体：QTEST_MAIN 只支持单类，这里手动跑三个类。
int main(int argc, char *argv[])
{
  QApplication app(argc, argv);
  int status = 0;
  {
    PredictPageTests t;
    status |= QTest::qExec(&t, argc, argv);
  }
  {
    FactorPageTests t;
    status |= QTest::qExec(&t, argc, argv);
  }
  {
    ComposePageTests t;
    status |= QTest::qExec(&t, argc, argv);
  }
  return status;
}

#include "tst_mappingpages.moc"
