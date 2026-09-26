#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "../src/io/arearules.h"
#include "../src/io/projectclassifier.h"

// wave4/area-parametrization：工程级参数 seam 的行为等价性测试（TODOS
// 「第二工区参数化接缝」）。四类钉死值（层序界面名单 / 分类器目录规则 /
// SEG-Y 道号索引约定 / ONNX 期望网格）从 AreaRules 读取：
//   · 默认 = 本工区内置值 —— 与 master 行为逐字节一致（本测试钉住默认表，
//     tst_projectparsers / tst_import 的既有用例钉住消费方行为）；
//   · 工程目录 project_area.json 可覆盖 —— 自定义名单/规则生效；
//   · 坏 JSON 如实报错拒用 —— 不静默回退默认。
class TestAreaRules : public QObject
{
  Q_OBJECT

private slots:
  void defaultsPinCurrentAreaValues();
  void defaultClassifierBehaviorUnchanged();
  void missingConfigFallsBackToDefaults();
  void customConfigTakesEffect();
  void badJsonIsRefused();
  void resetRestoresDefaults();

private:
  static void writeConfig(const QString &dir, const QByteArray &json);
  static bool sameRules(const AreaRules::Rules &a, const AreaRules::Rules &b);
};

void TestAreaRules::writeConfig(const QString &dir, const QByteArray &json)
{
  QFile f(AreaRules::configFilePath(dir));
  QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  f.write(json);
  f.close();
}

bool TestAreaRules::sameRules(const AreaRules::Rules &a, const AreaRules::Rules &b)
{
  if (a.sequenceBoundaries != b.sequenceBoundaries)
    return false;
  if (a.classifier.datPathRules.size() != b.classifier.datPathRules.size())
    return false;
  for (int i = 0; i < a.classifier.datPathRules.size(); ++i)
  {
    const auto &ra = a.classifier.datPathRules.at(i);
    const auto &rb = b.classifier.datPathRules.at(i);
    if (ra.exactSegments != rb.exactSegments || ra.segmentKeywords != rb.segmentKeywords ||
        ra.filenameKeywords != rb.filenameKeywords || ra.type != rb.type)
      return false;
  }
  if (a.classifier.referenceDirNames != b.classifier.referenceDirNames)
    return false;
  if (a.classifier.fixedAuxiliaryNameStem != b.classifier.fixedAuxiliaryNameStem)
    return false;
  if (a.segy.inlineWordOffset != b.segy.inlineWordOffset ||
      a.segy.crosslineWordOffset != b.segy.crosslineWordOffset ||
      a.segy.fieldRecordOffset != b.segy.fieldRecordOffset ||
      a.segy.cdpXlineOffset != b.segy.cdpXlineOffset)
    return false;
  return a.onnxGrid.rows == b.onnxGrid.rows && a.onnxGrid.cols == b.onnxGrid.cols;
}

// 内置默认表 = 本工区（project_area）钉死值的逐项钉住。任何一项漂移都会
// 破坏「缺省行为 = 今天」的等价承诺。
void TestAreaRules::defaultsPinCurrentAreaValues()
{
  const AreaRules::Rules r = AreaRules::defaults();

  // 8 层序界面（dataimportservice.cpp isKnownSequenceBoundary 原名单，plan §1）
  QCOMPARE(r.sequenceBoundaries,
           QStringList({QStringLiteral("C3"), QStringLiteral("C6"), QStringLiteral("D53"),
                        QStringLiteral("D61"), QStringLiteral("D62"), QStringLiteral("D63"),
                        QStringLiteral("D71"), QStringLiteral("D72")}));

  // .dat 路径段规则（projectclassifier.cpp 原表，按优先级序）
  QCOMPARE(r.classifier.datPathRules.size(), 4);
  QCOMPARE(r.classifier.datPathRules.at(0).exactSegments, QStringList{QStringLiteral("td")});
  QCOMPARE(r.classifier.datPathRules.at(0).segmentKeywords,
           QStringList({QString::fromUtf8("时深")}));
  QCOMPARE(r.classifier.datPathRules.at(0).filenameKeywords, QStringList{});
  QCOMPARE(r.classifier.datPathRules.at(0).type, QStringLiteral("time_depth"));
  QCOMPARE(r.classifier.datPathRules.at(1).segmentKeywords,
           QStringList({QString::fromUtf8("层位")}));
  QCOMPARE(r.classifier.datPathRules.at(1).type, QStringLiteral("horizon"));
  QCOMPARE(r.classifier.datPathRules.at(2).segmentKeywords,
           QStringList({QString::fromUtf8("井分层")}));
  QCOMPARE(r.classifier.datPathRules.at(2).type, QStringLiteral("well_stratification"));
  QCOMPARE(r.classifier.datPathRules.at(3).segmentKeywords,
           QStringList({QString::fromUtf8("井位")}));
  QCOMPARE(r.classifier.datPathRules.at(3).filenameKeywords,
           QStringList({QStringLiteral("wellhead"), QStringLiteral("well_head")}));
  QCOMPARE(r.classifier.datPathRules.at(3).type, QStringLiteral("well_head"));

  QCOMPARE(r.classifier.referenceDirNames, QStringList({QString::fromUtf8("参考资料")}));
  QCOMPARE(r.classifier.fixedAuxiliaryNameStem, QStringLiteral("HZ28-6-1"));

  // SEG-Y 道号索引约定（segyreader.cpp 原常量；0 基道头偏移）
  QCOMPARE(r.segy.inlineWordOffset, 188);
  QCOMPARE(r.segy.crosslineWordOffset, 192);
  QCOMPARE(r.segy.fieldRecordOffset, 8);
  QCOMPARE(r.segy.cdpXlineOffset, 20);

  // ONNX 网格门 = D61 411×641（src/workflow/workflows.cpp kOnnxGridRows/Cols 同值；
  // 该门常量的 AreaRules 化接线归 workflow 属主，见 docs/AREA_PARAMETERS.md）
  QCOMPARE(r.onnxGrid.rows, 411);
  QCOMPARE(r.onnxGrid.cols, 641);
}

// 默认规则下分类器公共行为不变（与 tst_projectparsers::classifiesEachCategory
// 的 .dat 段一致；这里走 AreaRules::active() 消费路径再钉一遍）。
void TestAreaRules::defaultClassifierBehaviorUnchanged()
{
  const QString base = QStringLiteral("/data/project_area");
  QCOMPARE(classifyProjectPath(base + QString::fromUtf8("/井位/ExportWellHead.dat")).type,
           QStringLiteral("well_head"));
  QCOMPARE(classifyProjectPath(base + QString::fromUtf8("/井分层/DC.dat")).type,
           QStringLiteral("well_stratification"));
  QCOMPARE(classifyProjectPath(base + QString::fromUtf8("/时深/TD/A1.dat")).type,
           QStringLiteral("time_depth"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/x/td/A1.dat")).type,
           QStringLiteral("time_depth"));
  QCOMPARE(classifyProjectPath(base + QString::fromUtf8("/层位/D61.dat")).type,
           QStringLiteral("horizon"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/any/WellHead_2020.dat")).type,
           QStringLiteral("well_head"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/any/plain.dat")).type,
           QStringLiteral("tabular"));
  QVERIFY(isFixedAuxiliaryPath(QString::fromUtf8("/a/b/HZ28-6-1测井.xml")));
  QVERIFY(!isFixedAuxiliaryPath(QString::fromUtf8("/a/参考资料/other.xml")));
  QVERIFY(isDefaultReferencePath(QString::fromUtf8("/a/参考资料/doc.pdf")));
  QVERIFY(!isDefaultReferencePath(QString::fromUtf8("/a/普通/doc.pdf")));

  // 层序界面：默认名单大小写归一（isKnownSequenceBoundary 的公开消费面在
  // DataImportService 导入链（tst_import），这里钉名单成员语义。
  const AreaRules::Rules r = AreaRules::active();
  QVERIFY(r.sequenceBoundaries.contains(QStringLiteral("D61")));
  QVERIFY(!r.sequenceBoundaries.contains(QStringLiteral("X9")));
}

// 缺 project_area.json = 全默认（读取顺序契约的第一档）。
void TestAreaRules::missingConfigFallsBackToDefaults()
{
  QTemporaryDir dir;
  AreaRules::setProjectDir(dir.path());
  QCOMPARE(AreaRules::activeProjectDir(), dir.path());
  QVERIFY(AreaRules::lastError().isEmpty());
  QVERIFY(sameRules(AreaRules::active(), AreaRules::defaults()));
  AreaRules::reset();
}

// 自定义配置生效：名单/规则/偏移/网格全部可覆盖，且 .dat 规则表是整体替换
// （旧工区关键字不再命中——表语义是优先级序，不做合并）。
void TestAreaRules::customConfigTakesEffect()
{
  QTemporaryDir dir;
  writeConfig(dir.path(), QByteArrayLiteral(R"({
  "schema_version": 1,
  "sequence_boundaries": ["X1", "D61"],
  "classifier": {
    "dat_path_rules": [
      {"exact_segments": ["tdq"], "type": "time_depth"},
      {"segment_keywords": ["分层"], "type": "well_stratification"},
      {"segment_keywords": ["点位"], "filename_keywords": ["wh"], "type": "well_head"}
    ],
    "reference_dir_names": ["文献"],
    "fixed_auxiliary_name_stem": "OTHER-1"
  },
  "segy_indexing": {
    "inline_word_offset": 184,
    "crossline_word_offset": 188,
    "field_record_offset": 9,
    "cdp_xline_offset": 24
  },
  "onnx_grid": {"rows": 97, "cols": 129}
})"));
  AreaRules::Rules loaded;
  QString err;
  QVERIFY(AreaRules::loadFromProjectDir(dir.path(), &loaded, &err));
  QVERIFY(err.isEmpty());
  AreaRules::setProjectDir(dir.path());
  QVERIFY(AreaRules::lastError().isEmpty());

  const AreaRules::Rules r = AreaRules::active();
  QCOMPARE(r.sequenceBoundaries, QStringList({QStringLiteral("X1"), QStringLiteral("D61")}));
  QCOMPARE(r.onnxGrid.rows, 97);
  QCOMPARE(r.onnxGrid.cols, 129);
  QCOMPARE(r.segy.inlineWordOffset, 184);
  QCOMPARE(r.segy.cdpXlineOffset, 24);

  // 规则表整体替换：新关键字命中、旧关键字不再命中。
  QCOMPARE(classifyProjectPath(QStringLiteral("/p/分层/a.dat")).type,
           QStringLiteral("well_stratification"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/p/点位/b.dat")).type,
           QStringLiteral("well_head"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/p/wh_1.dat")).type,
           QStringLiteral("well_head"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/p/tdq/c.dat")).type,
           QStringLiteral("time_depth"));
  QCOMPARE(classifyProjectPath(QString::fromUtf8("/p/时深/a.dat")).type,
           QStringLiteral("tabular"));
  QCOMPARE(classifyProjectPath(QString::fromUtf8("/p/层位/a.dat")).type,
           QStringLiteral("tabular"));
  QVERIFY(isDefaultReferencePath(QStringLiteral("/a/文献/doc.pdf")));
  QVERIFY(!isDefaultReferencePath(QString::fromUtf8("/a/参考资料/doc.pdf")));
  QVERIFY(isFixedAuxiliaryPath(QStringLiteral("/a/OTHER-1.xml")));
  QVERIFY(!isFixedAuxiliaryPath(QString::fromUtf8("/a/HZ28-6-1测井.xml")));

  AreaRules::reset();
}

// 坏 JSON 拒用：语法错 / 类型错 / 未知键 / 越界偏移 / 非法 schema_version
// 都要报错，active 不被污染（不静默回退也不带病采纳）。
void TestAreaRules::badJsonIsRefused()
{
  struct Case
  {
    const char *name;
    QByteArray json;
  };
  const Case cases[] = {
      {"syntax", QByteArrayLiteral("{\"sequence_boundaries\": [")},
      {"wrong-type", QByteArrayLiteral(R"({"sequence_boundaries": "D61"})")},
      {"wrong-element-type", QByteArrayLiteral(R"({"sequence_boundaries": [1, 2]})")},
      {"unknown-top-key", QByteArrayLiteral(R"({"boundaries": ["D61"]})")},
      {"unknown-nested-key",
       QByteArrayLiteral(R"({"segy_indexing": {"inline_offset": 184}})")},
      {"offset-out-of-range",
       QByteArrayLiteral(R"({"segy_indexing": {"inline_word_offset": 300}})")},
      {"offset-negative",
       QByteArrayLiteral(R"({"segy_indexing": {"cdp_xline_offset": -4}})")},
      {"grid-zero-rows", QByteArrayLiteral(R"({"onnx_grid": {"rows": 0, "cols": 8}})")},
      {"schema-version", QByteArrayLiteral(R"({"schema_version": 2})")},
      {"rule-missing-type",
       QByteArrayLiteral(R"({"classifier": {"dat_path_rules": [{"segment_keywords": ["x"]}]}})")},
      {"rule-unknown-type",
       QByteArrayLiteral(R"({"classifier": {"dat_path_rules": [{"type": "nope"}]}})")},
  };
  for (const Case &c : cases)
  {
    QTemporaryDir dir;
    writeConfig(dir.path(), c.json);
    AreaRules::Rules loaded;
    QString err;
    QVERIFY2(!AreaRules::loadFromProjectDir(dir.path(), &loaded, &err), c.name);
    QVERIFY2(!err.isEmpty(), c.name);
    AreaRules::setProjectDir(dir.path());
    QVERIFY2(!AreaRules::lastError().isEmpty(), c.name);
    QVERIFY2(sameRules(AreaRules::active(), AreaRules::defaults()), c.name);
    AreaRules::reset();
  }
}

void TestAreaRules::resetRestoresDefaults()
{
  QTemporaryDir dir;
  writeConfig(dir.path(), QByteArrayLiteral(R"({"onnx_grid": {"rows": 8, "cols": 8}})"));
  AreaRules::setProjectDir(dir.path());
  QCOMPARE(AreaRules::active().onnxGrid.rows, 8);
  AreaRules::reset();
  QVERIFY(AreaRules::activeProjectDir().isEmpty());
  QVERIFY(sameRules(AreaRules::active(), AreaRules::defaults()));
}

QTEST_MAIN(TestAreaRules)
#include "tst_arearules.moc"
