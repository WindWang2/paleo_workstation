// 层：测试壳
// I2（unit.onnx_predict v1）单元级回归：冻结契约、输入物化、特征/形状合同、
// 钉权重推理与产物。真实 SeismicUnitInput（合成 SEG-Y + 方案/层位）+ 受控
// ORT 夹具模型；不经 QGIS/宿主——owner 段的 catalog/适配器在 agent 级测试
// （tst_unit_onnx_agent）覆盖。无 ORT 构建同样注册：断言结构化不可用，
// 不 QSKIP。
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gdal.h>
#include <ogr_spatialref.h>
#include <cmath>
#include <cstring>

#include "ai/onnxfixture.h"
#include "ai/onnxpredictionservice.h"
#include "io/areamodel.h"
#include "services/algorithmspec.h"
#include "services/capabilityspec.h"
#include "services/frozenrasterinput.h"
#include "services/seismicunitinput.h"
#include "services/stratigraphicsubintervals.h"
#include "services/unitonnxcapabilityspec.h"
#include "workflow/unitonnxcontract.h"
#include "workflow/unitonnxoperation.h"
#include "seismichorizonfixture.h"

#include <paleo_data/service.h>

using namespace paleo;
using namespace paleo::strat;

namespace {
constexpr auto kLayerWindowRmsFeature =
    R"("features":[{"kind":"layer_window_rms","unit":"amplitude","domain":"TWT","preprocessing":"raw_float32_v1"}])";

#if PALEO_HAVE_ORT
// 与 tst_analysisunitcontext / tst_p4_onnx_prediction 同源：合成 SEG-Y 体 +
// 层位 A/B + 方案（含 seismicHorizonResourceId 接线），单元取数走生产路径。
struct UnitFixture
{
  QTemporaryDir dir;
  paleo::data::DataService ds;
  paleo::data::StratigraphicScheme scheme;
  QString project, seismicSource;

  bool init()
  {
    if (!dir.isValid())
      return false;
    project = dir.filePath(QStringLiteral("project"));
    std::string detail;
    if (!ds.createProject(paleo::area_model::directory(project).toStdString(), QStringLiteral("i2").toStdString(),
                          detail))
      return false;
    seismicSource = dir.filePath(QStringLiteral("volume.sgy"));
    if (!SeismicHorizonFixture::writeFile(seismicSource, SeismicHorizonFixture::buildVolume()))
      return false;
    paleo::data::ImportOptions opts;
    opts.copySources = false;
    paleo::data::ImportReport report;
    if (!ds.importFile(seismicSource.toStdString(), opts, report, detail))
      return false;
    const QString input = dir.filePath(QStringLiteral("picks.txt"));
    if (!SeismicHorizonFixture::writeFile(input, QByteArrayLiteral("synthetic")))
      return false;
    paleo::data::ParsedFile parsed;
    parsed.kind = "test";
    parsed.status = "parsed";
    for (int k = 0; k < 2; ++k)
    {
      paleo::data::SeismicHorizon h;
      h.horizonName = k ? "B" : "A";
      h.timeUnit = "ms";
      h.timeType = "TWT";
      for (int il = 100; il < 104; ++il)
        for (int xl = 2000; xl < 2004; ++xl)
          h.points.push_back({il, xl, double((xl - 2000) * 10), double((il - 100) * 20),
                              il == 101 && xl == 2001 ? NAN : (80.0 + k * 80.0)});
      parsed.horizons.push_back(h);
    }
    long long id = 0;
    if (!ds.importPreparedFile(input.toStdString(), input.toStdString(), input.toStdString(),
                               QCryptographicHash::hash("synthetic", QCryptographicHash::Sha256).toHex().toStdString(),
                               parsed, id, detail))
      return false;
    WellTopTable tops;
    tops[17] = {{QStringLiteral("A"), 1000}, {QStringLiteral("B"), 1080}};
    auto draft = draftScheme(QStringLiteral("i2"), QStringLiteral("test"),
                             {QStringLiteral("A"), QStringLiteral("B")}, tops);
    std::vector<paleo::data::DataEntry> horizons;
    if (!ds.listData("horizons", 0, horizons, detail))
      return false;
    for (auto &boundary : draft.boundaries)
      for (const auto &entry : horizons)
        if (entry.name == boundary.name)
          boundary.seismicHorizonResourceId = entry.id;
    std::vector<paleo::data::FrameworkIssue> issues;
    return ds.commitStratigraphicScheme(draft, {}, scheme, issues, detail);
  }

  UnitSelection unit(int index = 1) const
  {
    return makeSelection(scheme, selectorOptions(scheme).first().intervalId, index);
  }

  // models/manifest.json：钉 SHA 的 seg 类夹具模型。manifest 形态按注册表
  // 解析口径（modelregistry.cpp parseEntry）：特征声明与 featureContractVersion
  // 位于 input 内；outputs.semantics/classes 是声明值，ORT 实测形状另行核对。
  struct ManifestOptions
  {
    QString name = QStringLiteral("seg3");
    QString features = QString::fromUtf8(kLayerWindowRmsFeature);
    QString semantics = QStringLiteral("logits");
    QString shape = QStringLiteral("[1,1,null,null]");
    bool pin = true;
    int fixedSide = -1;      // >0：夹具模型的输入为固定 fixedSide×fixedSide
    int declaredClasses = -1; // <0：与夹具模型通道数一致
  };
  bool installModel(PaleoOnnxService &onnx, const QVector<float> &gains, const QVector<float> &biases)
  {
    return installModel(onnx, gains, biases, ManifestOptions{});
  }
  bool installModel(PaleoOnnxService &onnx, const QVector<float> &gains, const QVector<float> &biases,
                    const ManifestOptions &options)
  {
    const QString modelDir = dir.filePath(QStringLiteral("models"));
    if (!QDir().mkpath(modelDir))
      return false;
    const QString path = modelDir + QLatin1Char('/') + options.name + QStringLiteral(".onnx");
    const bool written = options.fixedSide > 0
                             ? OnnxFixtureWriter::writeSeg(path, gains, biases, 8, nullptr, options.fixedSide,
                                                           options.fixedSide)
                             : OnnxFixtureWriter::writeSeg(path, gains, biases);
    if (!written)
      return false;
    QFile manifestFile(modelDir + QStringLiteral("/manifest.json"));
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return false;
    manifestFile.write(QStringLiteral(R"({"models":[{"name":"%1","file":"%1.onnx",)"
                                      R"("version":"1.0.0","task":"segmentation","dataType":"amplitude",)"
                                      R"("input":{"name":"x","dtype":"float32","shape":%2,"featureContractVersion":1,%3},)"
                                      R"("outputs":{"semantics":"%4","classes":%5},)"
                                      R"("sha256":"%6"}]})")
                           .arg(options.name, options.shape, options.features, options.semantics)
                           .arg(options.declaredClasses < 0 ? gains.size() : options.declaredClasses)
                           .arg(options.pin ? PaleoOnnxService::sha256OfFile(path) : QString())
                           .toUtf8());
    manifestFile.close();
    onnx.setModelRoot(modelDir);
    onnx.setModelRegistry(ModelRegistry::scan(modelDir));
    return true;
  }

  // owner 段（capture）冻结的受信输入值：地震体目录版本身份 + 文件有界指纹。
  UnitOnnxPrepareSource source(const UnitSelection &selection, const QString &model = QStringLiteral("seg3")) const
  {
    UnitOnnxPrepareSource out;
    out.projectDir = project;
    out.model = model;
    out.seismicVersionId = QStringLiteral("ver-seismic");
    out.seismicSha256 = QString::fromLatin1(QByteArray(64, '1'));
    out.seismicPath = seismicSource;
    const QFileInfo file(seismicSource);
    out.seismicSize = file.size();
    out.seismicMtimeMs = file.lastModified().toMSecsSinceEpoch();
    out.unit = selection;
    return out;
  }
};

QByteArray sampleBytes(const QVector<float> &samples)
{
  QByteArray bytes(samples.size() * qsizetype(sizeof(float)), Qt::Uninitialized);
  std::memcpy(bytes.data(), samples.constData(), qsizetype(bytes.size()));
  return bytes;
}
#endif
} // namespace

class TestUnitOnnxOperation : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase() { GDALAllRegister(); }

  void specAndCapabilityRegistryAreRegistered()
  {
    const auto spec = AlgorithmSpecRegistry::specAtVersion(QStringLiteral("onnx:unit_rms_predict"), 1);
    QVERIFY(spec.complete);
    QCOMPARE(spec.engine, QStringLiteral("onnx"));
    QCOMPARE(spec.outputs.size(), 2);
    QCOMPARE(spec.outputs[0].id, QStringLiteral("OUTPUT"));
    QVERIFY(spec.outputs[0].required);
    QCOMPARE(spec.outputs[1].id, QStringLiteral("DIAGNOSTICS"));
    QVERIFY(!spec.outputs[1].required);
    QVERIFY(spec.outputs[1].constraints.value("diagnostic").toBool());
    QVERIFY(spec.params.isEmpty()); // v1 无可调参数：网格/特征/输出合同全在冻结计划
    QCOMPARE(spec.inputs.size(), 1);
    QCOMPARE(spec.inputs[0].constraints.value("maxWindowValues").toInt(), 1048576);
    QVERIFY(AlgorithmSpecRegistry::specAtVersion(QStringLiteral("onnx:unit_rms_predict"), 2).id.isEmpty());
    const auto capability = CapabilityRegistry::spec(QStringLiteral("unit.onnx_predict"), 1);
    QCOMPARE(capability.exposure, CapabilityExposure::Callable);
    QCOMPARE(capability.algorithmId, QStringLiteral("onnx:unit_rms_predict"));
    QVERIFY(CapabilityRegistry::spec(QStringLiteral("unit.onnx_predict"), 2).capabilityId.isEmpty());
    QString code;
    QCOMPARE(CapabilityRegistry::resolveAlias(QStringLiteral("paleo.unit_onnx_predict"), &code),
             QStringLiteral("unit.onnx_predict"));
    QVERIFY(CapabilityRegistry::aliasCollisions().isEmpty());
    QVERIFY(code.isEmpty());
    QCOMPARE(paleo::unitOnnxInputFeatureContract(),
             (QJsonObject{{"version", 1}, {"kind", "layer_window_rms"}, {"unit", "amplitude"},
                          {"domain", "TWT"}, {"preprocessing", "raw_float32_v1"}}));
    QJsonObject normalized;
    QVERIFY(normalizeRasterFeatureContract(paleo::unitOnnxInputFeatureContract(), normalized));
    QCOMPARE(normalized, paleo::unitOnnxInputFeatureContract());
  }

#if PALEO_HAVE_ORT
  void prepareFreezesUnitRmsAgainstRealService()
  {
    UnitFixture fixture;
    QVERIFY(fixture.init());
    PaleoOnnxService onnx;
    QVERIFY(fixture.installModel(onnx, {1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 0.1f}));
    const auto selection = fixture.unit(1);
    FrozenUnitOnnxPlan plan;
    QString error;
    QVERIFY2(prepareUnitOnnxPlan(&onnx, fixture.source(selection), plan, {}, &error), qPrintable(error));
    QCOMPARE(plan.rows, 64);
    QCOMPARE(plan.columns, 64);
    QCOMPARE(plan.classCount, 3);
    QCOMPARE(plan.outputSemantics, QStringLiteral("logits"));
    QCOMPARE(plan.modelSha256,
             PaleoOnnxService::sha256OfFile(fixture.dir.filePath(QStringLiteral("models/seg3.onnx"))));
    QVERIFY(plan.validCells > 0);
    QVERIFY(plan.validCells <= 64 * 64);
    const auto frozen = fromMap(plan.unit.toVariantMap());
    QVERIFY(!frozen.revision.isEmpty());
    QCOMPARE(frozen.subIntervalId, selection.subIntervalId);
    QCOMPARE(frozen.schemeId, selection.schemeId);
    QCOMPARE(plan.unitKey, analysisLayerKey(frozen));
    QCOMPARE(plan.samples.size(), 64 * 64 * 4);
    QCOMPARE(plan.seismicVersionId, QStringLiteral("ver-seismic"));
    // 对拍：冻结样本 = 直接调用生产 SeismicUnitInput 的同单元输出（字节级）。
    const auto direct = prepareSeismicUnitInput(fixture.project, frozen, fixture.seismicSource, {});
    QVERIFY(direct.error.isEmpty());
    QCOMPARE(direct.validCells, plan.validCells);
    QCOMPARE(direct.extent, plan.extent);
    QCOMPARE(direct.crs, plan.crs);
    QCOMPARE(plan.samples, sampleBytes(direct.samples));
    // 序列化回读等价（含 schemaHash 自校验）。
    FrozenUnitOnnxPlan restored;
    QVERIFY2(FrozenUnitOnnxPlan::fromJson(plan.toJson(), restored, &error), qPrintable(error));
    QCOMPARE(restored.samples, plan.samples);
    QCOMPARE(restored.featureSchemaHash, plan.featureSchemaHash);
    QCOMPARE(restored.unit, plan.unit);
    QVERIFY(restored.samplesSha256().size() == 64);
  }

  void siblingUnitsFreezeDistinctIdentitiesFromTheSameSource()
  {
    UnitFixture fixture;
    QVERIFY(fixture.init());
    PaleoOnnxService onnx;
    QVERIFY(fixture.installModel(onnx, {1.0f, -1.0f}, {0.0f, 0.0f}));
    FrozenUnitOnnxPlan first, second;
    QString error;
    QVERIFY2(prepareUnitOnnxPlan(&onnx, fixture.source(fixture.unit(1)), first, {}, &error), qPrintable(error));
    QVERIFY2(prepareUnitOnnxPlan(&onnx, fixture.source(fixture.unit(2)), second, {}, &error), qPrintable(error));
    // 同一地震来源与模型：父引用、模型钉值、网格、CRS 一致；单元身份与数值不同。
    QCOMPARE(second.seismicVersionId, first.seismicVersionId);
    QCOMPARE(second.modelSha256, first.modelSha256);
    QCOMPARE(second.seismicPath, first.seismicPath);
    QCOMPARE(second.extent, first.extent);
    QCOMPARE(second.crs, first.crs);
    QCOMPARE(fromMap(second.unit.toVariantMap()).revision, fromMap(first.unit.toVariantMap()).revision);
    QCOMPARE(fromMap(second.unit.toVariantMap()).subIndex, 2);
    QVERIFY(second.unitKey != first.unitKey);
    QVERIFY(fromMap(second.unit.toVariantMap()).subIntervalId != fromMap(first.unit.toVariantMap()).subIntervalId);
    QVERIFY(second.featureSchemaHash != first.featureSchemaHash);
    // S2 的 RMS/有效 mask 同样与真实服务逐字节一致（S1 的对拍见上一用例）。
    const auto direct = prepareSeismicUnitInput(fixture.project, fromMap(second.unit.toVariantMap()),
                                                fixture.seismicSource, {});
    QVERIFY(direct.error.isEmpty());
    QCOMPARE(second.samples, sampleBytes(direct.samples));
    QCOMPARE(second.validCells, direct.validCells);
    // 冻结计划各自可独立回读，身份不串。
    FrozenUnitOnnxPlan restored;
    QVERIFY(FrozenUnitOnnxPlan::fromJson(first.toJson(), restored, &error));
    QCOMPARE(restored.unitKey, first.unitKey);
    QVERIFY(FrozenUnitOnnxPlan::fromJson(second.toJson(), restored, &error));
    QCOMPARE(restored.unitKey, second.unitKey);
  }

  void invalidUnitsAndSourcesAreRejectedBeforeInference()
  {
    UnitFixture fixture;
    QVERIFY(fixture.init());
    PaleoOnnxService onnx;
    QVERIFY(fixture.installModel(onnx, {1.0f, -1.0f}, {0.0f, 0.0f}));
    FrozenUnitOnnxPlan plan;
    QString error;
    // 伪造 S 子段：freezeAnalysisUnit 拒绝，机器码不含路径/中文句子。
    auto forged = fixture.unit(1);
    forged.subIntervalId = QStringLiteral("S9");
    forged.subIndex = 9;
    QVERIFY(!prepareUnitOnnxPlan(&onnx, fixture.source(forged), plan, {}, &error));
    QCOMPARE(error, QStringLiteral("unit_selection_invalid"));
    QVERIFY(plan.samples.isEmpty());
    // aggregate：本链只接受具体 S1–S4。
    auto aggregate = fixture.unit(1);
    aggregate.aggregate = true;
    aggregate.subIntervalId.clear();
    aggregate.subIndex = 0;
    QVERIFY(!prepareUnitOnnxPlan(&onnx, fixture.source(aggregate), plan, {}, &error));
    QCOMPARE(error, QStringLiteral("unit_selection_invalid"));
    // 未知方案：跨工程来源同属身份不符。
    auto foreign = fixture.unit(1);
    foreign.schemeId = QStringLiteral("other-scheme");
    QVERIFY(!prepareUnitOnnxPlan(&onnx, fixture.source(foreign), plan, {}, &error));
    QCOMPARE(error, QStringLiteral("unit_selection_invalid"));
    // 来源不在本工区数据服务：换一个不存在的文件。
    auto source = fixture.source(fixture.unit(1));
    source.seismicPath = fixture.dir.filePath(QStringLiteral("missing.sgy"));
    QVERIFY(!prepareUnitOnnxPlan(&onnx, source, plan, {}, &error));
    QCOMPARE(error, QStringLiteral("seismic_source_unavailable"));
    // 前两次取消检查是 prepare 入口闸。第三次起落在地震读取（检查或层位窗口，
    // 句子是「地震读取已取消」/「地震层位窗口读取已取消」），必须是 cancelled。
    int reads = 0;
    const auto cancelDuringRead = [&reads] { return ++reads > 2; };
    source = fixture.source(fixture.unit(1));
    QVERIFY(!prepareUnitOnnxPlan(&onnx, source, plan, cancelDuringRead, &error));
    QCOMPARE(error, QStringLiteral("cancelled"));
    QVERIFY(plan.samples.isEmpty());
    // 未登记模型。
    QVERIFY(!prepareUnitOnnxPlan(&onnx, fixture.source(fixture.unit(1), QStringLiteral("ghost")), plan, {}, &error));
    QCOMPARE(error, QStringLiteral("model_unavailable"));
  }

  void registryAndContractFailuresAreHonest()
  {
    UnitFixture fixture;
    QVERIFY(fixture.init());
    PaleoOnnxService onnx;
    QString error;
    FrozenUnitOnnxPlan plan;
    const auto prepare = [&](const UnitFixture::ManifestOptions &options, const char *where) {
      const bool installed = fixture.installModel(onnx, {1.0f, -1.0f}, {0.0f, 0.0f}, options);
      QVERIFY2(installed, where);
      const auto source = fixture.source(fixture.unit(1));
      const bool ok = prepareUnitOnnxPlan(&onnx, source, plan, {}, &error);
      QVERIFY2(!ok, where);
      // 失败不留下半份计划，错误码不含本地路径。
      QVERIFY2(plan.samples.isEmpty() && plan.modelSha256.isEmpty(), where);
      QVERIFY2(!error.contains(fixture.dir.path()), qPrintable(error));
    };
    // 未钉哈希：扫描 Ok 但无身份，拒绝。
    UnitFixture::ManifestOptions unpinned;
    unpinned.pin = false;
    prepare(unpinned, "unpinned");
    QCOMPARE(error, QStringLiteral("model_unavailable"));
    // 特征声明不匹配（scalar/dimensionless ≠ layer_window_rms/amplitude）。
    UnitFixture::ManifestOptions wrongFeature;
    wrongFeature.features = QStringLiteral(
        R"("features":[{"kind":"scalar","unit":"dimensionless","domain":"none","preprocessing":"raw_float32_v1"}])");
    prepare(wrongFeature, "wrongFeature");
    QCOMPARE(error, QStringLiteral("unit_feature_contract_mismatch"));
    // 缺特征声明 / 多特征：不能从模型名或 dataType 猜 RMS 语义。
    UnitFixture::ManifestOptions noFeatures;
    noFeatures.features = QStringLiteral(R"("features":[])");
    prepare(noFeatures, "noFeatures");
    QCOMPARE(error, QStringLiteral("model_feature_contract_unavailable"));
    UnitFixture::ManifestOptions twoFeatures;
    // "features":[{…},{…}]：把单特征声明原样重复一份。
    const auto single = QString::fromUtf8(kLayerWindowRmsFeature);
    twoFeatures.features = single.chopped(1) + QLatin1Char(',') + single.mid(12);
    prepare(twoFeatures, "twoFeatures");
    QCOMPARE(error, QStringLiteral("model_feature_contract_unavailable"));
    // 真实固定 2×2 输入的模型与 64×64 网格不符（manifest 形状与实测一致）。
    UnitFixture::ManifestOptions fixed;
    fixed.fixedSide = 2;
    fixed.shape = QStringLiteral("[1,1,2,2]");
    prepare(fixed, "fixed");
    QCOMPARE(error, QStringLiteral("unit_model_signature_incompatible"));
    // 声明类数与实测输出通道不符。
    UnitFixture::ManifestOptions classes;
    classes.declaredClasses = 3;
    prepare(classes, "classes");
    QCOMPARE(error, QStringLiteral("unit_model_output_classes_mismatch"));
    // 未知输出语义（manifest 可写，但本原子只开放 logits/probabilities 分类）。
    UnitFixture::ManifestOptions semantics;
    semantics.semantics = QStringLiteral("class_index");
    prepare(semantics, "semantics");
    QCOMPARE(error, QStringLiteral("unit_model_output_semantics_unsupported"));
    // 上面每次拒绝后计划都被清空（无半份冻结结果）。
    QVERIFY(plan.samples.isEmpty());
    QVERIFY(plan.modelSha256.isEmpty());
    // 正常模型在同一夹具上恢复可用：上面的拒绝都是合同判定而非环境损坏。
    QVERIFY(fixture.installModel(onnx, {1.0f, -1.0f}, {0.0f, 0.0f}));
    QVERIFY2(prepareUnitOnnxPlan(&onnx, fixture.source(fixture.unit(1)), plan, {}, &error), qPrintable(error));
    QCOMPARE(plan.classCount, 2);
  }

  void computeProducesVerifiedClassRaster()
  {
    UnitFixture fixture;
    QVERIFY(fixture.init());
    PaleoOnnxService onnx;
    const QVector<float> gains{2.0f, -1.0f, 0.5f};
    const QVector<float> biases{0.0f, 0.0f, 0.1f};
    QVERIFY(fixture.installModel(onnx, gains, biases));
    FrozenUnitOnnxPlan plan;
    QString error;
    QVERIFY2(prepareUnitOnnxPlan(&onnx, fixture.source(fixture.unit(1)), plan, {}, &error), qPrintable(error));
    const QString outputs = fixture.dir.filePath(QStringLiteral("outputs"));
    QJsonObject summary;
    AlgorithmRunResult diagnostics;
    const auto result = computeUnitOnnxPlan(&onnx, plan, outputs, diagnostics, summary, {});
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.artifacts.size(), 1);
    QCOMPARE(result.artifacts[0].portId, QStringLiteral("OUTPUT"));
    QVERIFY(diagnostics.ok); // 宿主只保留 ok 的私有诊断
    QCOMPARE(diagnostics.artifacts.size(), 1);
    QCOMPARE(diagnostics.artifacts[0].portId, QStringLiteral("DIAGNOSTICS"));
    QCOMPARE(summary.value("rows").toInt(), 64);
    QCOMPARE(summary.value("classes").toInt(), 3);
    QCOMPARE(summary.value("validCells").toInt(), plan.validCells);
    QCOMPARE(summary.value("sourceVersionId").toString(), QStringLiteral("ver-seismic"));
    QCOMPARE(summary.value("modelSha256").toString(), plan.modelSha256);
    // 逐像元对拍：期望 argmax 来自冻结样本 × 夹具模型增益；缺覆盖格 → nodata。
    QVector<float> samples(64 * 64);
    std::memcpy(samples.data(), plan.samples.constData(), plan.samples.size());
    auto ds = GDALOpen(result.artifacts[0].absolutePath.toUtf8().constData(), GA_ReadOnly);
    QVERIFY2(ds, qPrintable(result.artifacts[0].absolutePath));
    QCOMPARE(GDALGetRasterXSize(ds), 64);
    QCOMPARE(GDALGetRasterYSize(ds), 64);
    QCOMPARE(GDALGetRasterCount(ds), 1);
    QCOMPARE(GDALGetRasterDataType(GDALGetRasterBand(ds, 1)), GDT_Byte);
    int hasNodata = 0;
    QCOMPARE(GDALGetRasterNoDataValue(GDALGetRasterBand(ds, 1), &hasNodata), 255.0);
    QVERIFY(hasNodata);
    const auto provenance =
        QJsonDocument::fromJson(GDALGetMetadataItem(ds, "PALEO_PROVENANCE", nullptr)).object();
    QCOMPARE(provenance.value("sourceVersionId").toString(), QStringLiteral("ver-seismic"));
    QCOMPARE(provenance.value("modelSha256Pinned").toString(), plan.modelSha256);
    QCOMPARE(provenance.value("modelSha256Actual").toString(), plan.modelSha256);
    QCOMPARE(provenance.value("classEncoding").toString(), QStringLiteral("declared_channel_index"));
    QCOMPARE(provenance.value("inputAttribute").toString(), QStringLiteral("layer_window_rms"));
    QCOMPARE(provenance.value("unitKey").toString(), plan.unitKey);
    double gt[6];
    QCOMPARE(GDALGetGeoTransform(ds, gt), CE_None);
    QCOMPARE(gt[0], plan.extent.left());
    QCOMPARE(gt[1], plan.extent.width() / 64.0);
    QCOMPARE(gt[3], plan.extent.bottom());
    QCOMPARE(gt[5], -plan.extent.height() / 64.0);
    const auto projection = QString::fromUtf8(GDALGetProjectionRef(ds));
    QVERIFY(!projection.isEmpty());
    {
      OGRSpatialReference expected;
      QVERIFY(expected.importFromWkt(plan.crs.toUtf8().constData()) == OGRERR_NONE);
      OGRSpatialReference actual;
      QVERIFY(actual.importFromWkt(projection.toUtf8().constData()) == OGRERR_NONE);
      QVERIFY(actual.IsSame(&expected));
    }
    quint8 pixels[64 * 64];
    QCOMPARE(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, 0, 0, 64, 64, pixels, 64, 64, GDT_Byte, 0, 0), CE_None);
    GDALClose(ds);
    int matched = 0;
    for (int i = 0; i < 64 * 64; ++i)
    {
      if (!std::isfinite(samples[i]))
      {
        QCOMPARE(pixels[i], quint8(255));
        continue;
      }
      int best = 0;
      float bestValue = gains[0] * samples[i] + biases[0];
      for (int c = 1; c < 3; ++c)
      {
        const float value = gains[c] * samples[i] + biases[c];
        if (value > bestValue)
        {
          bestValue = value;
          best = c;
        }
      }
      QCOMPARE(pixels[i], quint8(best));
      ++matched;
    }
    QCOMPARE(matched, plan.validCells);
    QFile diagnosticsFile(diagnostics.artifacts[0].absolutePath);
    QVERIFY(diagnosticsFile.open(QIODevice::ReadOnly));
    const auto diagnosticsJson = QJsonDocument::fromJson(diagnosticsFile.readAll()).object();
    QCOMPARE(diagnosticsJson.value("provenance").toObject().value("unitKey").toString(), plan.unitKey);
    QCOMPARE(diagnosticsJson.value("samplesSha256").toString(), plan.samplesSha256());
    QCOMPARE(diagnosticsJson.value("classHistogram").toObject(), summary.value("classHistogram").toObject());
    QVERIFY(diagnosticsJson.value("note").toString().contains(QStringLiteral("channel indexes")));
    QVERIFY(!diagnosticsJson.contains(QStringLiteral("seismicPath")));
  }

  void probabilitiesContractIsNotClippedOrRenormalized()
  {
    UnitFixture fixture;
    QVERIFY(fixture.init());
    PaleoOnnxService onnx;
    // 夹具模型输出是无界 logits；manifest 声明成 probabilities → 实测向量
    // 不在 [0,1] 概率域，严格拒绝，不裁剪、不归一。
    UnitFixture::ManifestOptions probabilities;
    probabilities.semantics = QStringLiteral("probabilities");
    QVERIFY(fixture.installModel(onnx, {2.0f, -1.0f}, {0.0f, 0.0f}, probabilities));
    FrozenUnitOnnxPlan plan;
    QString error;
    QVERIFY2(prepareUnitOnnxPlan(&onnx, fixture.source(fixture.unit(1)), plan, {}, &error), qPrintable(error));
    QCOMPARE(plan.outputSemantics, QStringLiteral("probabilities"));
    QJsonObject summary;
    AlgorithmRunResult diagnostics;
    const auto result = computeUnitOnnxPlan(&onnx, plan, fixture.dir.filePath(QStringLiteral("prob")), diagnostics,
                                            summary, {});
    QVERIFY(!result.ok);
    QVERIFY2(result.error == QStringLiteral("model_probability_out_of_contract") ||
                 result.error == QStringLiteral("model_probability_sum_invalid"),
             qPrintable(result.error));
    QVERIFY(result.artifacts.isEmpty());
    QVERIFY(diagnostics.artifacts.isEmpty());
    // 失败不外发半份产物。
    QVERIFY(QDir(fixture.dir.filePath(QStringLiteral("prob"))).entryList(QDir::Files).isEmpty());
  }

  void modelReplacementCancellationAndTampering()
  {
    UnitFixture fixture;
    QVERIFY(fixture.init());
    PaleoOnnxService onnx;
    QVERIFY(fixture.installModel(onnx, {2.0f, -2.0f}, {0.0f, 0.0f}));
    FrozenUnitOnnxPlan plan;
    QString error;
    QVERIFY2(prepareUnitOnnxPlan(&onnx, fixture.source(fixture.unit(1)), plan, {}, &error), qPrintable(error));
    // 取消：入口即查，失败不留半份计划（用独立出参，不覆盖上面已冻结的计划）。
    FrozenUnitOnnxPlan discarded;
    QVERIFY(!prepareUnitOnnxPlan(&onnx, fixture.source(fixture.unit(2)), discarded, [] { return true; }, &error));
    QCOMPARE(error, QStringLiteral("cancelled"));
    QVERIFY(discarded.samples.isEmpty());
    QVERIFY(discarded.modelSha256.isEmpty());
    QJsonObject summary;
    AlgorithmRunResult diagnostics;
    const auto cancelledDir = fixture.dir.filePath(QStringLiteral("cancelled"));
    auto result = computeUnitOnnxPlan(&onnx, plan, cancelledDir, diagnostics, summary, [] { return true; });
    QVERIFY(!result.ok);
    QCOMPARE(result.error, QStringLiteral("cancelled"));
    QVERIFY(result.artifacts.isEmpty());
    QVERIFY(diagnostics.artifacts.isEmpty());
    QVERIFY(QDir(cancelledDir).entryList(QDir::Files).isEmpty());
    // prepare 后替换模型文件并重钉 manifest：compute 按冻结钉值拒绝，绝不
    // 静默切到新权重。
    QVERIFY(OnnxFixtureWriter::writeSeg(fixture.dir.filePath(QStringLiteral("models/seg3.onnx")), {7.0f, -7.0f},
                                        {0.0f, 0.0f}));
    QFile manifest(fixture.dir.filePath(QStringLiteral("models/manifest.json")));
    QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
    manifest.write(QStringLiteral(R"({"models":[{"name":"seg3","file":"seg3.onnx",)"
                                  R"("version":"2.0.0","task":"segmentation","dataType":"amplitude",)"
                                  R"("input":{"name":"x","dtype":"float32","shape":[1,1,null,null],"featureContractVersion":1,%1},)"
                                  R"("outputs":{"semantics":"logits","classes":2},)"
                                  R"("sha256":"%2"}]})")
                       .arg(QString::fromUtf8(kLayerWindowRmsFeature),
                            PaleoOnnxService::sha256OfFile(fixture.dir.filePath(QStringLiteral("models/seg3.onnx"))))
                       .toUtf8());
    manifest.close();
    onnx.setModelRegistry(ModelRegistry::scan(fixture.dir.filePath(QStringLiteral("models"))));
    result = computeUnitOnnxPlan(&onnx, plan, fixture.dir.filePath(QStringLiteral("replaced")), diagnostics, summary, {});
    QVERIFY(!result.ok);
    QCOMPARE(result.error, QStringLiteral("model_changed_since_prepare"));
    QVERIFY(QDir(fixture.dir.filePath(QStringLiteral("replaced"))).entryList(QDir::Files).isEmpty());
    // 篡改冻结样本 / 换成别的单元身份 / 改网格：schemaHash 自校验整体拒绝。
    const auto json = plan.toJson();
    FrozenUnitOnnxPlan forged;
    auto tampered = json;
    QByteArray bytes(plan.samples);
    bytes[0] = bytes[0] == 0 ? 1 : 0;
    tampered.insert(QStringLiteral("samples"), QLatin1String(bytes.toBase64()));
    QVERIFY(!FrozenUnitOnnxPlan::fromJson(tampered, forged, &error));
    QCOMPARE(error, QStringLiteral("invalid_frozen_unit_plan"));
    tampered = json;
    auto unit = tampered.value("unit").toObject();
    unit.insert(QStringLiteral("aggregate"), true);
    unit.insert(QStringLiteral("subIndex"), 0);
    tampered.insert(QStringLiteral("unit"), unit);
    QVERIFY(!FrozenUnitOnnxPlan::fromJson(tampered, forged, &error));
    tampered = json;
    tampered.insert(QStringLiteral("rows"), 128);
    QVERIFY(!FrozenUnitOnnxPlan::fromJson(tampered, forged, &error));
    tampered = json;
    tampered.insert(QStringLiteral("schema"), QStringLiteral("paleo.unit-onnx-plan.v2"));
    QVERIFY(!FrozenUnitOnnxPlan::fromJson(tampered, forged, &error));
    tampered = json;
    auto model = tampered.value("modelSha256").toString();
    model[0] = model[0] == QLatin1Char('0') ? QLatin1Char('1') : QLatin1Char('0');
    tampered.insert(QStringLiteral("modelSha256"), model);
    QVERIFY(!FrozenUnitOnnxPlan::fromJson(tampered, forged, &error));
    QVERIFY(forged.samples.isEmpty());
  }
#endif

  void seismicWindowCancelIsNotSourceUnavailable()
  {
    QCOMPARE(unitOnnxInputFailureCode(QStringLiteral("已取消")), QStringLiteral("cancelled"));
    QCOMPARE(unitOnnxInputFailureCode(QStringLiteral("地震读取已取消")), QStringLiteral("cancelled"));
    QCOMPARE(unitOnnxInputFailureCode(QStringLiteral("地震层位窗口读取已取消")),
             QStringLiteral("cancelled"));
    QCOMPARE(unitOnnxInputFailureCode(QStringLiteral("所选地震来源不在当前工区数据服务中")),
             QStringLiteral("seismic_source_unavailable"));
    QCOMPARE(unitOnnxInputFailureCode(QStringLiteral("工区中没有已导入的地震体")),
             QStringLiteral("seismic_source_unavailable"));
    QCOMPARE(unitOnnxInputFailureCode(QStringLiteral("当前层位单元内没有有效地震样点")),
             QStringLiteral("unit_window_no_valid_samples"));
  }

#if !PALEO_HAVE_ORT
  void withoutOrtTheEntryPointsAreStructuredUnavailable()
  {
    UnitOnnxPrepareSource source;
    FrozenUnitOnnxPlan plan;
    QString error;
    QCOMPARE(prepareUnitOnnxPlan(nullptr, source, plan, {}, &error), false);
    QCOMPARE(error, QStringLiteral("ort_unavailable"));
    QJsonObject summary;
    AlgorithmRunResult diagnostics;
    const auto result = computeUnitOnnxPlan(nullptr, plan, QStringLiteral("/tmp"), diagnostics, summary, {});
    QVERIFY(!result.ok);
    QCOMPARE(result.error, QStringLiteral("ort_unavailable"));
    QVERIFY(result.artifacts.isEmpty());
  }
#endif
};

QTEST_GUILESS_MAIN(TestUnitOnnxOperation)
#include "tst_unit_onnx_operation.moc"
