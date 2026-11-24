// 层：测试壳
// I2（unit.onnx_predict v1）Agent 级回归：真实工具解码 → per-capability 宿主
// 适配器 → 持久 operation 链。覆盖工具实参严格解码、prepare/compute-only/
// publish 三段、幂等重放与意图冲突、继承输入不可重述、未知版本/绑定模式、
// 非地震体来源、准备后换模型、算后移除模型/推理服务再发布、受理前中断/
// 取消不登记操作、全新宿主经持久 journal 恢复未运行的 compute、readiness
// 分相语义与机器可处理错误。装配走生产 bindChatToolRunner（app/aiwiring.cpp），
// 与产品同源；单元/地震/模型夹具与 tst_p4_onnx_prediction 同口径。
#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "app/aiwiring.h"
#include "app/capabilityhostwiring.h"
#include "ai/chat/domaintools.h"
#include "ai/onnxfixture.h"
#include "ai/onnxpredictionservice.h"
#include "domain/stratigraphicunit.h"
#include "linkage/selectioncontext.h"
#include "metadata/projectlock.h"
#include "qgis/qgisprojectservice.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisruntime.h"
#include "services/seismicunitinput.h"
#include "services/stratigraphicsubintervals.h"
#include "services/unitonnxcapabilityspec.h"
#include "workflow/aichatcontroller.h"
#include "workflow/capabilityhost.h"
#include "workflow/capabilitysourcepublication.h"
#include "workflow/unitonnxservice.h"
#include "io/areamodel.h"
#include "seismichorizonfixture.h"
#include <paleo_data/service.h>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QScopeGuard>

using namespace paleo;
using namespace paleo::strat;

namespace {
constexpr auto kTool = "paleo.unit_onnx_predict";
constexpr auto kPrepare = "prepare";
constexpr auto kCompute = "compute-only";
constexpr auto kPublish = "publish";
// 注册表 manifest 的 input.features：与本原子的 layer_window_rms 输入合同一致。
constexpr auto kRmsFeature =
    R"([{"kind":"layer_window_rms","unit":"amplitude","domain":"TWT","preprocessing":"raw_float32_v1"}])";
// 形状相同但语义不同的单特征（tile 夹具口径）——不能靠形状碰巧相同通过。
constexpr auto kScalarFeature =
    R"([{"kind":"scalar","unit":"dimensionless","domain":"none","preprocessing":"raw_float32_v1"}])";

QString text(const QJsonObject &value)
{
  return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

void pump(int milliseconds)
{
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < milliseconds)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// operation_results 的端口按 diagnostic 标记分组（排序后的 portId 列表）。
QStringList portIds(const QJsonObject &results, bool diagnostic)
{
  QStringList out;
  for (const auto &value : results.value("ports").toArray())
  {
    const auto port = value.toObject();
    if (port.value("diagnostic").toBool() == diagnostic)
      out.append(port.value("portId").toString());
  }
  out.sort();
  return out;
}

struct Fixture
{
  document_fixture::Fixture data;
  ProjectDirLock lock{data.project};
  LayerManifest manifest{data.meta};
  QgisProjectService project;
  QgisLayerService layers{&project, &manifest};
  SelectionContext selection;
  paleo::capability::CapabilityHost host;
  PaleoOnnxService onnx;
  AiAssistWorkflow assist{&layers};
  AiChatController chat;
  paleo::data::DataService ds;
  paleo::data::StratigraphicScheme scheme;
  QString seismicSource, seismicVersionId, error;

  bool initAreaModel()
  {
    std::string detail;
    const QString root = paleo::area_model::directory(data.project);
    const bool exists = QFileInfo::exists(root + QStringLiteral("/data.sqlite"));
    if (exists ? !ds.openProject(root.toStdString(), detail)
               : !ds.createProject(root.toStdString(), QStringLiteral("i2").toStdString(), detail))
      return false;
    seismicSource = data.temp.filePath(QStringLiteral("volume.sgy"));
    if (!SeismicHorizonFixture::writeFile(seismicSource, SeismicHorizonFixture::buildVolume()))
      return false;
    paleo::data::ImportOptions opts;
    opts.copySources = false;
    paleo::data::ImportReport report;
    if (!ds.importFile(seismicSource.toStdString(), opts, report, detail))
      return false;
    const QString input = data.temp.filePath(QStringLiteral("picks.txt"));
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

  UnitSelection unit(int subIndex = 1) const
  {
    return makeSelection(scheme, selectorOptions(scheme).first().intervalId, subIndex);
  }

  // 把地震体登记为 catalog 的 RAW 未受管版本（受管血缘锚点，P4 同口径）。
  bool registerSeismicVersion()
  {
    CatalogAsset asset;
    asset.id = data.catalog.nextAssetId();
    asset.type = QStringLiteral("seismic_volume");
    asset.displayName = QStringLiteral("合成地震体");
    if (!data.catalog.addAsset(asset, &error))
      return false;
    QFile file(seismicSource);
    if (!file.open(QIODevice::ReadOnly))
      return false;
    CatalogVersion version;
    version.id = data.catalog.nextVersionId();
    version.assetId = asset.id;
    version.stage = QStringLiteral("RAW");
    version.managed = false;
    version.path = seismicSource;
    version.sourceUri = seismicSource;
    version.fileName = QFileInfo(seismicSource).fileName();
    version.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
    if (!data.catalog.addVersion(version, &error))
      return false;
    seismicVersionId = version.id;
    return true;
  }

  QString modelDir() const { return data.temp.filePath(QStringLiteral("models")); }
  QString modelPath() const { return QDir(modelDir()).filePath(QStringLiteral("seg3.onnx")); }

  // models/manifest.json：单条 seg3 模型。特征声明位于 input 内（注册表解析
  // 口径 modelregistry.cpp parseEntry）；outputs 是声明值，ORT 实测另行核对。
  bool writeManifest(const char *featuresJson, const QString &sha,
                     const QString &semantics = QStringLiteral("logits"))
  {
    QFile manifestFile(QDir(modelDir()).filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return false;
    const QByteArray bytes = QStringLiteral(R"({"models":[{"name":"seg3","file":"seg3.onnx",)"
                                            R"("version":"1.0.0","task":"segmentation","dataType":"amplitude",)"
                                            R"("input":{"name":"x","dtype":"float32","shape":[1,1,null,null],)"
                                            R"("featureContractVersion":1,"features":%1},)"
                                            R"("outputs":{"semantics":"%2","classes":3},)"
                                            R"("sha256":"%3"}]})")
                                 .arg(QString::fromUtf8(featuresJson), semantics, sha)
                                 .toUtf8();
    const bool written = manifestFile.write(bytes) == bytes.size();
    manifestFile.close();
    onnx.setModelRegistry(ModelRegistry::scan(modelDir()));
    return written;
  }

  // 写夹具模型并重钉 manifest（默认 3 类、层段 RMS 特征合同）。夹具字节确定：
  // 同参数重写得到同一 SHA。
  bool installModel(const QVector<float> &gains = {2.0f, -1.0f, 0.5f},
                    const QVector<float> &biases = {0.0f, 0.0f, 0.1f})
  {
    if (!QDir().mkpath(modelDir()))
      return false;
    if (!OnnxFixtureWriter::writeSeg(modelPath(), gains, biases))
      return false;
    return writeManifest(kRmsFeature, PaleoOnnxService::sha256OfFile(modelPath()));
  }

  bool init()
  {
    if (!data.init() || !initAreaModel() || !registerSeismicVersion())
      return false;
    onnx.setModelRoot(modelDir());
    if (!installModel())
      return false;
    if (!data.bind() || !lock.tryLock(&error) || !manifest.open(&error))
      return false;
    selection.setActiveHorizon(QStringLiteral("T1"));
    configureCapabilityCallers(&host);
    host.setTaskService(&data.tasks);
    host.setBindingProvider([this] {
      return captureCapabilityBinding(&data.session, &data.catalog, &lock, &selection, chat.toolRunner());
    });
    data.session.setHostContextProvider([this] { return host.context(); });
    assist.setOnnxService(&onnx);
    assist.setCatalog(&data.catalog, data.project);
    assist.setTaskService(&data.tasks);
    chat.toolRunner()->setTaskService(&data.tasks);
    rebind(true);
    return true;
  }

  void rebind(bool model)
  {
    assist.setOnnxService(model ? &onnx : nullptr);
    bindChatToolRunner(&chat, &assist, &layers, &data.catalog, data.project, data.session.documentId(), &host,
                       &data.session);
  }

  // 当前聊天会话对应的 Agent callerScope（宿主由可信会话派生，模型不可指定）。
  QString scope() { return host.callerScope(paleo::host::Caller::Agent, chat.session().id); }

  QJsonObject call(const QString &name, const QJsonObject &args)
  {
    QSignalSpy done(chat.toolRunner(), &AiChatToolRunner::toolFinished);
    chat.toolRunner()->run({QUuid::createUuid().toString(), name,
                            QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))});
    QElapsedTimer timer;
    timer.start();
    while (done.isEmpty() && timer.elapsed() < 15000)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (done.isEmpty())
      return {{"ok", false}, {"code", "test_timeout"}};
    auto out = QJsonDocument::fromJson(done[0][2].toString().toUtf8()).object();
    out.insert("toolOk", done[0][1].toBool());
    return out;
  }

  QJsonObject unitInputs(int subIndex = 1)
  {
    return QJsonObject{{"model", "seg3"}, {"seismic_version_id", seismicVersionId},
                       {"unit_selection", QJsonObject::fromVariantMap(toMap(unit(subIndex)))}};
  }

  QJsonObject phase(const QString &key, const QString &phaseName, const QJsonObject &inputs,
                    const QJsonObject &extra = {})
  {
    QJsonObject args{{"client_request_id", key}, {"publish_intent", phaseName}, {"inputs", inputs}};
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
      args.insert(it.key(), it.value());
    return call(kTool, args);
  }

  QJsonObject prepare(const QString &key, int subIndex = 1, const QJsonObject &extra = {})
  {
    return phase(key, kPrepare, unitInputs(subIndex), extra);
  }

  QJsonObject inherit(const QString &key, const QString &phaseName, const QString &source)
  {
    return phase(key, phaseName, QJsonObject{{"source_operation_id", source}});
  }

  QJsonObject get(const QString &id) { return call("paleo.operation_get", {{"operation_id", id}}); }
};
} // namespace

class TestUnitOnnxAgent : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  // 工具表面的形状闸（无需工程）：模型给出的 JSON 是不可信数据。
  void toolArgumentsAreStrictlyDecoded()
  {
    const AiToolSpec spec = aiToolSpec(QString::fromUtf8(kTool));
    QVERIFY(!spec.name.isEmpty());
    QVERIFY(isAiCapabilityTool(spec.name));
    QVERIFY(!spec.requiresLocalOrt); // ORT 门按阶段在 readiness 委托里给
    QVERIFY(!spec.legacyAdapter);
    QCOMPARE(spec.exposure, CapabilityExposure::Callable);
    // Pi bridge 与 builtin 共用同一份工具快照：单工具 schema 预算 16 KiB。
    QVERIFY(QJsonDocument(spec.toJsonSchema()).toJson(QJsonDocument::Compact).size() < 16 * 1024);
    const QJsonObject unit{{"schemeId", "s"}, {"intervalId", "i"}, {"subIntervalId", "S1"}, {"subIndex", 1},
                           {"aggregate", false}};
    const QJsonObject goodInputs{{"model", "seg3"}, {"seismic_version_id", "v1"}, {"unit_selection", unit}};
    const auto prepare = [&](QJsonValue inputs, QJsonObject extra = {}) {
      QJsonObject args{{"client_request_id", "k"}, {"publish_intent", kPrepare}, {"inputs", inputs}};
      for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        args.insert(it.key(), it.value());
      return args;
    };
    const auto withUnit = [&](QJsonObject patch, bool dropSub = false) {
      auto patched = unit;
      for (auto it = patch.constBegin(); it != patch.constEnd(); ++it)
        patched.insert(it.key(), it.value());
      if (dropSub)
        patched.remove(QStringLiteral("subIntervalId"));
      return QJsonObject{{"model", "seg3"}, {"seismic_version_id", "v1"}, {"unit_selection", patched}};
    };
    const auto accepted = [&](const QJsonObject &args) {
      const QString loose = spec.validateParameters(args);
      QVERIFY2(loose.isEmpty(), qPrintable(loose));
      const QString strict = spec.validateExternalArguments(args);
      QVERIFY2(strict.isEmpty(), qPrintable(strict));
      QCOMPARE(dispatchAiTool(spec.name, args).status, AiToolDispatchStatus::Routed);
    };
    const auto rejected = [&](const QJsonObject &args) {
      QVERIFY2(!spec.validateParameters(args).isEmpty(), qPrintable(text(args)));
      QCOMPARE(dispatchAiTool(spec.name, args).status, AiToolDispatchStatus::Disabled);
    };
    accepted(prepare(goodInputs));
    accepted(prepare(goodInputs, QJsonObject{{"contract_version", 1}, {"binding_mode", "pinned-value"}}));
    // 显示字段合法但不参与身份（宿主按方案派生/核验）。
    accepted(prepare(withUnit({{"displayName", "T70–T71/S2"}, {"topBoundary", "A"}, {"bottomBoundary", "B"}})));
    // 自授权/路径/脚本/任意 handler：都不是本工具的参数。
    for (const auto &key : {QStringLiteral("approved"), QStringLiteral("actor"), QStringLiteral("caller_scope"),
                            QStringLiteral("parameters"), QStringLiteral("output_path"), QStringLiteral("model_path"),
                            QStringLiteral("source_path"), QStringLiteral("script"), QStringLiteral("handler")})
      rejected(prepare(goodInputs, QJsonObject{{key, 1}}));
    rejected(prepare(QJsonObject{{"model", "seg3"}}));  // 缺地震来源与单元
    rejected(prepare(QJsonObject{}));                   // 缺全部输入
    rejected(prepare(QJsonArray{}));                    // inputs 须为 object
    rejected(prepare(QJsonObject{{"model", "seg3"}, {"seismic_version_id", "v1"}, {"unit_selection", unit},
                                {"extra", 1}}));       // 夹带多余输入
    rejected(prepare(QJsonObject{{"model", "../seg3"}, {"seismic_version_id", "v1"}, {"unit_selection", unit}}));
    rejected(prepare(QJsonObject{{"model", "a/b"}, {"seismic_version_id", "v1"}, {"unit_selection", unit}}));
    rejected(prepare(QJsonObject{{"model", 3}, {"seismic_version_id", "v1"}, {"unit_selection", unit}}));
    rejected(prepare(QJsonObject{{"model", "seg3"}, {"seismic_version_id", 7}, {"unit_selection", unit}}));
    rejected(prepare(QJsonObject{{"model", "seg3"}, {"seismic_version_id", ""}, {"unit_selection", unit}}));
    rejected(prepare(QJsonObject{{"model", "seg3"}, {"seismic_version_id", "v1"}, {"unit_selection", "T70"}}));
    // 单元形状：必须是方案内具体 S1–S4 子段；声称已核验/给图层键都不构成证据。
    rejected(prepare(withUnit({{"aggregate", true}})));
    rejected(prepare(withUnit({}, true)));  // 整个 Interval（无子段）
    rejected(prepare(withUnit({{"subIndex", 0}})));
    rejected(prepare(withUnit({{"subIndex", 5}})));
    rejected(prepare(withUnit({{"subIndex", 2.5}})));
    rejected(prepare(withUnit({{"subIndex", "2"}})));  // 文本整数只属宿主内部宽松档
    rejected(prepare(withUnit({{"verified", true}})));
    rejected(prepare(withUnit({{"analysisLayerKey", "T70"}})));
    rejected(prepare(withUnit({{"subIntervalId", ""}})));
    // 阶段/请求号/版本/绑定模式。
    rejected(QJsonObject{{"client_request_id", "k"}, {"publish_intent", "execute"}, {"inputs", goodInputs}});
    rejected(QJsonObject{{"client_request_id", "k"}, {"inputs", goodInputs}});
    rejected(QJsonObject{{"publish_intent", kPrepare}, {"inputs", goodInputs}});
    rejected(prepare(goodInputs, QJsonObject{{"contract_version", 2}}));
    rejected(prepare(goodInputs, QJsonObject{{"contract_version", "1"}}));
    rejected(prepare(goodInputs, QJsonObject{{"binding_mode", "follow-selection"}}));
    rejected(prepare(goodInputs, QJsonObject{{"binding_mode", "follow-object-head"}}));
    // 继承阶段只接受 source_operation_id；输入不可重述。
    const QJsonObject source{{"source_operation_id", "op-1"}};
    accepted(QJsonObject{{"client_request_id", "k"}, {"publish_intent", kCompute}, {"inputs", source}});
    accepted(QJsonObject{{"client_request_id", "k"}, {"publish_intent", kPublish}, {"inputs", source}});
    rejected(QJsonObject{{"client_request_id", "k"}, {"publish_intent", kCompute},
                         {"inputs", QJsonObject{{"source_operation_id", "op-1"}, {"model", "seg3"}}}});
    rejected(QJsonObject{{"client_request_id", "k"}, {"publish_intent", kPublish},
                         {"inputs", QJsonObject{{"source_operation_id", ""}}}});
    rejected(QJsonObject{{"client_request_id", "k"}, {"publish_intent", kPublish}, {"inputs", QJsonObject{}}});
  }

  void toolIsAdvertisedAndRoutedThroughTheProductionBinding()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    QStringList advertised;
    for (const auto &tool : f.chat.tools())
      advertised.append(tool.name);
    QCOMPARE(advertised.count(QString::fromUtf8(kTool)), 1);  // 不重复登记
    QString code;
    QCOMPARE(CapabilityRegistry::resolveAlias(QString::fromUtf8(kTool), &code),
             QString::fromUtf8(paleo::kUnitOnnxCapabilityId));
    QVERIFY(CapabilityRegistry::aliasCollisions().isEmpty());
    // 生产装配已把 per-capability 适配器装进宿主：prepare 能受理即证明。
    const auto prepared = f.prepare("adv1");
    QVERIFY2(prepared.value("toolOk").toBool(), qPrintable(text(prepared)));
    QCOMPARE(prepared.value("execution").toString(), QStringLiteral("prepared"));
    const auto facts = f.get(prepared.value("operationId").toString());
    QVERIFY(facts.value("toolOk").toBool());
    QCOMPARE(facts.value("execution").toString(), QStringLiteral("prepared"));
    QCOMPARE(facts.value("computeCount").toInt(), 0);
    QCOMPARE(facts.value("commitState").toString(), QStringLiteral("not_requested"));
  }

  void prepareComputePublishWithIdempotentReplay()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    const auto versionsBefore = f.data.catalog.versions().size();
    const auto assetsBefore = f.data.catalog.assets().size();
    const auto mutationBefore = f.data.catalog.mutationSeq();
    // prepare（显式 v1 + pinned-value）：持久 operationId；未动 catalog、未声明图层。
    const auto prepared = f.prepare("p1", 1, QJsonObject{{"contract_version", 1}, {"binding_mode", "pinned-value"}});
    QVERIFY2(prepared.value("toolOk").toBool(), qPrintable(text(prepared)));
    const QString prepareId = prepared.value("operationId").toString();
    QVERIFY(!prepareId.isEmpty());
    QCOMPARE(prepared.value("execution").toString(), QStringLiteral("prepared"));
    QCOMPARE(prepared.value("commitState").toString(), QStringLiteral("not_requested"));
    QCOMPARE(f.data.catalog.versions().size(), versionsBefore);
    QCOMPARE(f.data.catalog.assets().size(), assetsBefore);
    QCOMPARE(f.data.catalog.mutationSeq(), mutationBefore);
    QVERIFY(f.layers.declared().isEmpty());
    // 同键同参重放：返回原 prepare 操作，不建第二操作。
    const auto replay = f.prepare("p1", 1);
    QVERIFY(replay.value("toolOk").toBool());
    QCOMPARE(replay.value("operationId").toString(), prepareId);
    QCOMPARE(f.get(prepareId).value("computeCount").toInt(), 0);
    // 同键异图（换成 S2）→ 冲突，且不产生第二个 prepare。
    const auto conflict = f.prepare("p1", 2);
    QVERIFY(!conflict.value("toolOk").toBool());
    QCOMPARE(conflict.value("code").toString(), QStringLiteral("idempotency_conflict"));
    QVERIFY(conflict.value("operationId").toString().isEmpty());
    // compute-only：只给 source_operation_id；仍不登记正式成果。
    const auto computed = f.inherit("c1", kCompute, prepareId);
    QVERIFY2(computed.value("toolOk").toBool(), qPrintable(text(computed)));
    const QString computeId = computed.value("operationId").toString();
    QVERIFY(computeId != prepareId);
    QCOMPARE(computed.value("execution").toString(), QStringLiteral("succeeded"));
    QCOMPARE(computed.value("commitState").toString(), QStringLiteral("not_requested"));
    QCOMPARE(computed.value("code").toString(), QStringLiteral("computed"));
    QCOMPARE(computed.value("rows").toInt(), 64);
    QCOMPARE(computed.value("columns").toInt(), 64);
    QCOMPARE(computed.value("classes").toInt(), 3);
    QVERIFY(computed.value("validCells").toInt() > 0);
    QVERIFY(!computed.value("unitKey").toString().isEmpty());
    QCOMPARE(computed.value("sourceVersionId").toString(), f.seismicVersionId);
    QVERIFY(!text(computed).contains(f.data.temp.path()));
    QCOMPARE(f.get(computeId).value("computeCount").toInt(), 1);
    QCOMPARE(f.data.catalog.versions().size(), versionsBefore);
    QVERIFY(f.layers.declared().isEmpty());
    // 结果端口：1 个正式 OUTPUT + 1 个私有诊断；诊断不含来源路径。
    const auto ports = f.call("paleo.operation_results", {{"operation_id", computeId}});
    QCOMPARE(portIds(ports, false), QStringList{QStringLiteral("OUTPUT")});
    QCOMPARE(portIds(ports, true), QStringList{QStringLiteral("DIAGNOSTICS")});
    const auto diagnosticsPage = f.call("paleo.operation_read",
                                        {{"operation_id", computeId}, {"port_id", "DIAGNOSTICS"},
                                         {"max_bytes", 8192}});
    QVERIFY2(diagnosticsPage.value("toolOk").toBool(), qPrintable(text(diagnosticsPage)));
    const auto diagnosticsBytes = QByteArray::fromBase64(diagnosticsPage.value("data").toString().toLatin1());
    const auto diagnostics = QJsonDocument::fromJson(diagnosticsBytes).object();
    QCOMPARE(diagnostics.value("classHistogram").toObject(), computed.value("classHistogram").toObject());
    QCOMPARE(diagnostics.value("provenance").toObject().value("unitKey").toString(),
             computed.value("unitKey").toString());
    QVERIFY(!diagnosticsBytes.contains(f.seismicSource.toUtf8()));
    // 既有请求键不能换意图复用。
    const auto phaseConflict = f.inherit("p1", kCompute, prepareId);
    QVERIFY(!phaseConflict.value("toolOk").toBool());
    QCOMPARE(phaseConflict.value("code").toString(), QStringLiteral("idempotency_conflict"));
    // publish：提交已算结果，不重复推理；无展示意图 → 不声明图层。
    const auto published = f.inherit("pub1", kPublish, computeId);
    QVERIFY2(published.value("toolOk").toBool(), qPrintable(text(published)));
    const QString publishId = published.value("operationId").toString();
    QVERIFY(publishId != computeId);
    const auto facts = f.get(publishId);
    QCOMPARE(facts.value("commitState").toString(), QStringLiteral("committed"));
    QCOMPARE(facts.value("computeCount").toInt(), 0);
    QCOMPARE(facts.value("presentation").toString(), QStringLiteral("not_requested"));
    QVERIFY(!facts.value("commitReceiptRef").toString().isEmpty());
    QCOMPARE(f.data.catalog.versions().size(), versionsBefore + 1);
    QVERIFY(f.layers.declared().isEmpty());
    const auto publishedPorts = f.call("paleo.operation_results", {{"operation_id", publishId}});
    QCOMPARE(portIds(publishedPorts, false), QStringList{QStringLiteral("OUTPUT")});
    QVERIFY(portIds(publishedPorts, true).isEmpty());  // 诊断不随发布成为正式成员
    // 同一 publish 请求重放：原提交事实，数值计数与版本数都不增加。
    const auto publishReplay = f.inherit("pub1", kPublish, computeId);
    QVERIFY(publishReplay.value("toolOk").toBool());
    QCOMPARE(publishReplay.value("operationId").toString(), publishId);
    QCOMPARE(publishReplay.value("computeCount").toInt(), 0);
    QCOMPARE(f.data.catalog.versions().size(), versionsBefore + 1);
    // 受控结果引用可读（不暴露路径）。
    const auto bytes = f.call("paleo.operation_read",
                              {{"operation_id", publishId}, {"port_id", "OUTPUT"}, {"max_bytes", 16}});
    QVERIFY2(bytes.value("toolOk").toBool(), qPrintable(text(bytes)));
    QVERIFY(!bytes.value("data").toString().isEmpty());
    QVERIFY(!bytes.contains(QStringLiteral("path")));
    // 换会话后原操作不可见（作用域隔离）。
    f.chat.newSession();
    QVERIFY(!f.get(publishId).value("toolOk").toBool());
  }

  void inheritedInputsAreImmutableAndVersionsAreGated()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    const auto prepared = f.prepare("g1");
    QVERIFY2(prepared.value("toolOk").toBool(), qPrintable(text(prepared)));
    const QString prepareId = prepared.value("operationId").toString();
    // compute-only 夹带模型/来源/单元 → 工具表面拒绝（输入不可重述）。
    const QJsonObject unitInputs = f.unitInputs();
    for (const auto &key : {QStringLiteral("model"), QStringLiteral("seismic_version_id"),
                            QStringLiteral("unit_selection")})
    {
      auto mixed = QJsonObject{{"source_operation_id", prepareId}};
      mixed.insert(key, unitInputs.value(key));
      const auto smuggled = f.phase("g2", kCompute, mixed);
      QVERIFY(!smuggled.value("toolOk").toBool());
      QVERIFY(smuggled.value("operationId").toString().isEmpty());
    }
    // publish 夹带模型/参数：同语义。
    {
      auto mixed = QJsonObject{{"source_operation_id", prepareId}, {"model", "seg3"}};
      QVERIFY(!f.phase("g3", kPublish, mixed).value("toolOk").toBool());
      const auto withParameters = f.call(kTool, {{"client_request_id", QStringLiteral("g4")},
                                                 {"publish_intent", kPublish},
                                                 {"parameters", QJsonObject{{"x", 1}}},
                                                 {"inputs", QJsonObject{{"source_operation_id", prepareId}}}});
      QVERIFY(!withParameters.value("toolOk").toBool());
      QVERIFY(withParameters.value("error").toString().contains(QStringLiteral("parameters")));
    }
    // 未知版本 / 未支持的绑定模式：明确拒绝，不回落到最新版或当前选择。
    const auto wrongVersion = f.phase("g5", kPrepare, unitInputs, QJsonObject{{"contract_version", 2}});
    QVERIFY(!wrongVersion.value("toolOk").toBool());
    QVERIFY(wrongVersion.value("error").toString().contains(QStringLiteral("unsupported_version")));
    QVERIFY(wrongVersion.value("operationId").toString().isEmpty());
    for (const auto &mode : {QStringLiteral("follow-selection"), QStringLiteral("follow-object-head")})
    {
      const auto follow = f.phase("g6", kPrepare, unitInputs, QJsonObject{{"binding_mode", mode}});
      QVERIFY(!follow.value("toolOk").toBool());
      QVERIFY(follow.value("operationId").toString().isEmpty());
    }
    // 未知地震版本 / 未登记模型：owner 段早拒，机器码。
    {
      auto inputs = unitInputs;
      inputs.insert(QStringLiteral("seismic_version_id"), QStringLiteral("ver-ghost"));
      const auto ghost = f.phase("g7", kPrepare, inputs);
      QVERIFY(!ghost.value("toolOk").toBool());
      QCOMPARE(ghost.value("code").toString(), QStringLiteral("seismic_version_missing"));
      inputs = unitInputs;
      inputs.insert(QStringLiteral("model"), QStringLiteral("ghost"));
      const auto ghostModel = f.phase("g8", kPrepare, inputs);
      QVERIFY(!ghostModel.value("toolOk").toBool());
      QCOMPARE(ghostModel.value("code").toString(), QStringLiteral("model_unavailable"));
    }
  }

  void nonSeismicCatalogVersionIsRejectedBeforeMaterialization()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    // 登记一个非地震体资产/版本（内容无关），拿它的版本 id 冒充地震来源。
    const QString other = f.data.external(QStringLiteral("other.bin"), QByteArrayLiteral("not seismic"));
    QVERIFY(!other.isEmpty());
    auto inputs = f.unitInputs();
    inputs.insert(QStringLiteral("seismic_version_id"), other);
    const auto rejected = f.phase("n1", kPrepare, inputs);
    QVERIFY(!rejected.value("toolOk").toBool());
    QCOMPARE(rejected.value("code").toString(), QStringLiteral("seismic_version_type_invalid"));
    QVERIFY(rejected.value("operationId").toString().isEmpty());
  }

  void readinessIsPhaseScoped()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    const auto runner = f.chat.toolRunner();
    const QString tool(kTool);
    // 推理服务解绑：只有需要新加载/数值运行的阶段受阻。
    f.rebind(false);
    QCOMPARE(runner->readiness(tool, {{"publish_intent", kPrepare}}, false).code,
             QStringLiteral("ort_provider_unavailable"));
    QCOMPARE(runner->readiness(tool, {{"publish_intent", kCompute}}, false).code,
             QStringLiteral("ort_provider_unavailable"));
    // publish 与列表广播不受 ORT/模型安装状态牵连。
    QVERIFY(runner->readiness(tool, {{"publish_intent", kPublish}}, false).ready);
    QVERIFY(runner->readiness(tool, {}, true).ready);
    // 解绑时 prepare 在执行前被拒，不产生 operation。
    const auto rejected = f.prepare("rd1");
    QVERIFY(!rejected.value("toolOk").toBool());
    QCOMPARE(rejected.value("code").toString(), QStringLiteral("ort_provider_unavailable"));
    QVERIFY(rejected.value("operationId").toString().isEmpty());
    f.rebind(true);
    QVERIFY(runner->readiness(tool, {{"publish_intent", kPrepare}}, false).ready);
    QVERIFY(runner->readiness(tool, {{"publish_intent", kCompute}}, false).ready);
    // 已受理的请求键：按 Read 重放，不再要求推理依赖。
    const auto accepted = f.prepare("rd2");
    QVERIFY2(accepted.value("toolOk").toBool(), qPrintable(text(accepted)));
    f.rebind(false);
    QVERIFY(runner->readiness(tool, {{"client_request_id", QStringLiteral("rd2")},
                                      {"publish_intent", kPrepare}}, false).ready);
    const auto replay = f.prepare("rd2");
    QVERIFY2(replay.value("toolOk").toBool(), qPrintable(text(replay)));
    QCOMPARE(replay.value("operationId").toString(), accepted.value("operationId").toString());
  }

  void computedResultsPublishWithoutModelOrRuntime()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    const auto prepared = f.prepare("r1-prepare");
    QVERIFY2(prepared.value("toolOk").toBool(), qPrintable(text(prepared)));
    const QString prepareId = prepared.value("operationId").toString();
    const auto computed = f.inherit("r1-compute", kCompute, prepareId);
    QVERIFY2(computed.value("toolOk").toBool(), qPrintable(text(computed)));
    const QString computeId = computed.value("operationId").toString();
    const auto versionsBefore = f.data.catalog.versions().size();
    // 计算后清空模型注册表并解绑推理服务：发布已认证输出不需要新推理。
    ModelRegistryScan empty;
    empty.manifestFound = true;
    f.onnx.setModelRegistry(empty);
    f.rebind(false);
    const auto published = f.inherit("r1-publish", kPublish, computeId);
    QVERIFY2(published.value("toolOk").toBool(), qPrintable(text(published)));
    const auto facts = f.get(published.value("operationId").toString());
    QCOMPARE(facts.value("commitState").toString(), QStringLiteral("committed"));
    QCOMPARE(facts.value("computeCount").toInt(), 0);
    QCOMPARE(f.data.catalog.versions().size(), versionsBefore + 1);
    QVERIFY(f.inherit("r1-publish", kPublish, computeId).value("toolOk").toBool());
    // 但同一状态下新的 compute 仍被 ORT 门挡住，不会偷偷放行。
    const auto recomputed = f.inherit("r2-compute", kCompute, prepareId);
    QVERIFY(!recomputed.value("toolOk").toBool());
    QCOMPARE(recomputed.value("code").toString(), QStringLiteral("ort_provider_unavailable"));
    QVERIFY(recomputed.value("operationId").toString().isEmpty());
    QCOMPARE(f.data.catalog.versions().size(), versionsBefore + 1);
  }

  // 受理前被中断/取消：执行器口径「取消 = 作废」（在途调用不再回报），这里
  // 直接查持久日志——晚到的物化结果不得受理，也不得登记 operation。
  void interruptionBeforeAcceptanceRegistersNothing()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    auto *runner = f.chat.toolRunner();
    const auto issue = [&](const QString &key, const QString &phaseName, const QJsonObject &inputs) {
      const QJsonObject args{{"client_request_id", key}, {"publish_intent", phaseName}, {"inputs", inputs}};
      runner->run({QUuid::createUuid().toString(), QString::fromUtf8(kTool),
                   QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))});
    };
    // 宿主中断（非用户显式取消）在途的 prepare。
    issue("i1", kPrepare, f.unitInputs());
    runner->interrupt();
    pump(1500);
    QCOMPARE(f.host.recovery()->lookup(f.scope(), "i1").code, QStringLiteral("not_found"));
    // 中断未留受理记录：同键 prepare 是全新受理。若旧物化尚未收尾，宿主会
    // 短暂回 operation_preparing——等它收尾后再断言。
    QJsonObject again;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
      again = f.prepare("i1");
      if (again.value("code").toString() != QLatin1String("operation_preparing"))
        break;
      pump(100);
    }
    QVERIFY2(again.value("toolOk").toBool(), qPrintable(text(again)));
    QCOMPARE(again.value("execution").toString(), QStringLiteral("prepared"));
    const QString prepareId = again.value("operationId").toString();
    // 用户显式取消在途的 compute（受理前）：同样不登记 operation。
    issue("i2", kCompute, QJsonObject{{"source_operation_id", prepareId}});
    runner->cancel();
    pump(1500);
    QCOMPARE(f.host.recovery()->lookup(f.scope(), "i2").code, QStringLiteral("not_found"));
    const auto facts = f.get(prepareId);
    QCOMPARE(facts.value("execution").toString(), QStringLiteral("prepared"));
    QCOMPARE(facts.value("computeCount").toInt(), 0);
    // 取消不污染后续调用：同源可正常计算。
    const auto computed = f.inherit("i3", kCompute, prepareId);
    QVERIFY2(computed.value("toolOk").toBool(), qPrintable(text(computed)));
  }

  void invalidUnitsAndFeatureContractsFailBeforeAnyOperation()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    const auto versionsBefore = f.data.catalog.versions().size();
    const auto withUnit = [&](QJsonObject patch, bool dropSub = false) {
      auto inputs = f.unitInputs();
      auto patched = inputs.value("unit_selection").toObject();
      for (auto it = patch.constBegin(); it != patch.constEnd(); ++it)
        patched.insert(it.key(), it.value());
      if (dropSub)
        patched.remove(QStringLiteral("subIntervalId"));
      inputs.insert(QStringLiteral("unit_selection"), patched);
      return inputs;
    };
    // aggregate / 整个 Interval：工具形状闸即拒。
    QVERIFY(!f.phase("bad1", kPrepare, withUnit({{"aggregate", true}, {"subIndex", 0}}, true))
                .value("toolOk")
                .toBool());
    QVERIFY(!f.phase("bad2", kPrepare, withUnit({{"subIndex", 0}}, true)).value("toolOk").toBool());
    // subIndex 越界：工具表面即拒（error 文本，无 code、无 operation）。
    const auto shapeRejected = f.phase("bad3", kPrepare, withUnit({{"subIndex", 9}}));
    QVERIFY(!shapeRejected.value("toolOk").toBool());
    QVERIFY(shapeRejected.value("error").toString().contains(QStringLiteral("unit_selection_invalid")));
    QVERIFY(shapeRejected.value("operationId").toString().isEmpty());
    // 形状合法但方案里不存在的子段 id：物化段 freezeAnalysisUnit 拒绝——没有
    // operation 产物，错误是机器码。
    const auto forged = f.phase(
        "bad4", kPrepare, withUnit({{"subIntervalId", f.unit().subIntervalId + QStringLiteral("-forged")}}));
    QVERIFY(!forged.value("toolOk").toBool());
    QCOMPARE(forged.value("code").toString(), QStringLiteral("unit_selection_invalid"));
    QVERIFY(forged.value("operationId").toString().isEmpty());
    // 未知方案：同属身份不符。
    const auto foreign = f.phase("bad5", kPrepare, withUnit({{"schemeId", QStringLiteral("other-scheme")}}));
    QVERIFY(!foreign.value("toolOk").toBool());
    QCOMPARE(foreign.value("code").toString(), QStringLiteral("unit_selection_invalid"));
    // 失败的请求键不留受理记录：同键可重新发起合法 prepare。
    const auto retried = f.prepare("bad4");
    QVERIFY2(retried.value("toolOk").toBool(), qPrintable(text(retried)));
    QCOMPARE(f.data.catalog.versions().size(), versionsBefore);
    // 形状相同但特征语义不匹配的模型：prepare 在合同核对处拒绝，无 fallback、
    // 不自动改写模型声明。
    const QString modelSha = PaleoOnnxService::sha256OfFile(f.modelPath());
    QVERIFY(f.writeManifest(kScalarFeature, modelSha));
    const auto mismatch = f.prepare("bad6");
    QVERIFY(!mismatch.value("toolOk").toBool());
    QCOMPARE(mismatch.value("code").toString(), QStringLiteral("unit_feature_contract_mismatch"));
    QVERIFY(mismatch.value("operationId").toString().isEmpty());
    // 缺特征声明：不从模型名/dataType 猜 RMS 语义。
    QVERIFY(f.writeManifest("[]", modelSha));
    const auto undeclared = f.prepare("bad7");
    QVERIFY(!undeclared.value("toolOk").toBool());
    QCOMPARE(undeclared.value("code").toString(), QStringLiteral("model_feature_contract_unavailable"));
    // 声明成别的输出语义：v1 只开放 logits/probabilities 分类。
    QVERIFY(f.writeManifest(kRmsFeature, modelSha, QStringLiteral("class_index")));
    const auto semantics = f.prepare("bad8");
    QVERIFY(!semantics.value("toolOk").toBool());
    QCOMPARE(semantics.value("code").toString(), QStringLiteral("unit_model_output_semantics_unsupported"));
    // 未钉哈希的清单：列表可见不等于可运行。
    QVERIFY(f.writeManifest(kRmsFeature, QString()));
    const auto unpinned = f.prepare("bad9");
    QVERIFY(!unpinned.value("toolOk").toBool());
    QCOMPARE(unpinned.value("code").toString(), QStringLiteral("model_unavailable"));
    QCOMPARE(f.data.catalog.versions().size(), versionsBefore);
    // 错误载荷不含本地路径。
    for (const auto &payload : {shapeRejected, forged, foreign, mismatch, undeclared, semantics, unpinned})
      QVERIFY2(!text(payload).contains(f.data.temp.path()), qPrintable(text(payload)));
  }

  void modelSwapAfterPrepareIsRejectedAtCompute()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    const auto prepared = f.prepare("s1");
    QVERIFY2(prepared.value("toolOk").toBool(), qPrintable(text(prepared)));
    const QString prepareId = prepared.value("operationId").toString();
    // prepare 后替换模型文件并重钉 manifest（同名不同权重）。
    QVERIFY(OnnxFixtureWriter::writeSeg(f.modelPath(), {7.0f, -7.0f, 3.0f}, {0.0f, 0.0f, 0.0f}));
    QVERIFY(f.writeManifest(kRmsFeature, PaleoOnnxService::sha256OfFile(f.modelPath())));
    QCOMPARE(f.onnx.registryEntryFor(QStringLiteral("seg3")).status, ModelRegistryEntry::Status::Ok);
    const auto computed = f.inherit("s2", kCompute, prepareId);
    QVERIFY(!computed.value("toolOk").toBool());
    QCOMPARE(computed.value("code").toString(), QStringLiteral("model_changed_since_prepare"));
    QCOMPARE(computed.value("execution").toString(), QStringLiteral("failed"));
    // 失败的 compute 不产出可发布端口。
    const QString failedId = computed.value("operationId").toString();
    QVERIFY(!failedId.isEmpty());
    QCOMPARE(f.get(failedId).value("computeCount").toInt(), 1);
    const auto published = f.inherit("s3", kPublish, failedId);
    QVERIFY(!published.value("toolOk").toBool());
    QVERIFY(published.value("commitState").toString() != QStringLiteral("committed"));
    // 换回原权重（夹具字节确定）后：原冻结 prepare 仍按原钉值重新计算。
    QVERIFY(OnnxFixtureWriter::writeSeg(f.modelPath(), {2.0f, -1.0f, 0.5f}, {0.0f, 0.0f, 0.1f}));
    QVERIFY(f.writeManifest(kRmsFeature, PaleoOnnxService::sha256OfFile(f.modelPath())));
    const auto recovered = f.inherit("s4", kCompute, prepareId);
    QVERIFY2(recovered.value("toolOk").toBool(), qPrintable(text(recovered)));
  }

  // 合法 S1 与 S2、同一地震来源和模型：各自冻结单元身份；有效 mask 与真实
  // SeismicUnitInput 一致（validCells 对拍），父引用只含实际来源。
  void siblingUnitsMatchTheRealServiceThroughTheChain()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    QJsonObject results[2];
    const UnitSelection units[2] = {f.unit(1), f.unit(2)};
    for (int index = 0; index < 2; ++index)
    {
      const auto suffix = QString::number(index + 1);
      const auto prepared = f.prepare("u" + suffix + "-prepare", index + 1);
      QVERIFY2(prepared.value("toolOk").toBool(), qPrintable(text(prepared)));
      const auto computed = f.inherit("u" + suffix + "-compute", kCompute,
                                      prepared.value("operationId").toString());
      QVERIFY2(computed.value("toolOk").toBool(), qPrintable(text(computed)));
      results[index] = computed;
      const auto direct = prepareSeismicUnitInput(f.data.project, units[index], f.seismicSource, {});
      QVERIFY2(direct.error.isEmpty(), qPrintable(direct.error));
      QCOMPARE(computed.value("validCells").toInt(), direct.validCells);
      QCOMPARE(computed.value("unitKey").toString(), units[index].subIntervalId);
      QCOMPARE(computed.value("sourceVersionId").toString(), f.seismicVersionId);
      QCOMPARE(computed.value("modelSha256").toString(), PaleoOnnxService::sha256OfFile(f.modelPath()));
    }
    QVERIFY(results[0].value("unitKey").toString() != results[1].value("unitKey").toString());
    QCOMPARE(results[0].value("modelSha256").toString(), results[1].value("modelSha256").toString());
    QCOMPARE(results[0].value("sourceVersionId").toString(), results[1].value("sourceVersionId").toString());
  }

  // 「重启」：compute 已受理（execution=queued）但尚未运行时进程失去运行期
  // 钩子；全新宿主经生产重绑后由 paleo.operation_resume 恢复——只能走持久
  // journal + 适配器的 validateFrozen / restoreInputValidator / restoreWorker。
  void freshHostRestoresQueuedComputeFromTheDurableJournal()
  {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    const auto resetProvider = qScopeGuard([&] { f.data.session.setHostContextProvider({}); });
    const auto prepared = f.prepare("x1");
    QVERIFY2(prepared.value("toolOk").toBool(), qPrintable(text(prepared)));
    const QString prepareId = prepared.value("operationId").toString();
    // 基准：进程内按原宿主正常算一次。
    const auto baseline = f.inherit("x2", kCompute, prepareId);
    QVERIFY2(baseline.value("toolOk").toBool(), qPrintable(text(baseline)));
    // 按宿主受理路径冻结并受理一个 compute（不启动）：worker 钩子只存在于
    // 旧宿主内存。
    paleo::capability::ExecuteRequest request;
    request.capabilityId = QString::fromUtf8(paleo::kUnitOnnxCapabilityId);
    request.contractVersion = paleo::kUnitOnnxContractVersion;
    request.documentId = f.data.session.documentId();
    request.callerScope = f.scope();
    request.clientRequestId = QStringLiteral("x-restore");
    request.bindingMode = QStringLiteral("pinned-value");
    request.publishIntent = kCompute;
    request.inputs.insert(QStringLiteral("source_operation_id"), prepareId);
    const auto intent = paleo::capability::capabilityIntent(request);
    QString code;
    const auto preparation = paleo::unitOnnxExecutionAdapter(&f.onnx, QPointer<DataCatalog>(&f.data.catalog))
                                 .capture(request, intent, &code);
    QVERIFY2(static_cast<bool>(preparation.worker), qPrintable(code));
    const auto operation = preparation.worker({}, &code);
    QVERIFY2(!operation.capabilityId.isEmpty(), qPrintable(code));
    const auto accepted = f.host.recovery()->accept(operation);
    QVERIFY2(accepted.ok, qPrintable(accepted.code));
    const QString queued = accepted.record.operationId;
    QCOMPARE(f.get(queued).value("execution").toString(), QStringLiteral("queued"));
    QCOMPARE(f.get(queued).value("computeCount").toInt(), 0);
    // 全新宿主 + 生产重绑（运行期钩子全失）。
    paleo::capability::CapabilityHost restarted;
    configureCapabilityCallers(&restarted);
    restarted.setTaskService(&f.data.tasks);
    restarted.setBindingProvider([&] {
      return captureCapabilityBinding(&f.data.session, &f.data.catalog, &f.lock, &f.selection, f.chat.toolRunner());
    });
    f.data.session.setHostContextProvider([&] { return restarted.context(); });
    f.assist.setOnnxService(&f.onnx);
    bindChatToolRunner(&f.chat, &f.assist, &f.layers, &f.data.catalog, f.data.project, f.data.session.documentId(),
                       &restarted, &f.data.session);
    const auto resumed = f.call("paleo.operation_resume", {{"operation_id", queued}});
    QVERIFY2(resumed.value("toolOk").toBool(), qPrintable(text(resumed)));
    const auto facts = f.get(queued);
    QCOMPARE(facts.value("execution").toString(), QStringLiteral("succeeded"));
    QCOMPARE(facts.value("computeCount").toInt(), 1);
    // 与基准同源：同一冻结 64×64 层段 RMS → 确定性一致，不依赖活动模型/重取数。
    QCOMPARE(resumed.value("unitKey").toString(), baseline.value("unitKey").toString());
    QCOMPARE(resumed.value("validCells").toInt(), baseline.value("validCells").toInt());
    QCOMPARE(resumed.value("classHistogram").toObject(), baseline.value("classHistogram").toObject());
    QCOMPARE(resumed.value("modelSha256").toString(), baseline.value("modelSha256").toString());
    QCOMPARE(resumed.value("sourceVersionId").toString(), f.seismicVersionId);
    // 算后移除模型：在新宿主上发布仍只需已认证输出。
    ModelRegistryScan empty;
    empty.manifestFound = true;
    f.onnx.setModelRegistry(empty);
    const auto published = f.call(kTool, {{"client_request_id", QStringLiteral("x-publish")},
                                          {"publish_intent", kPublish},
                                          {"inputs", QJsonObject{{"source_operation_id", queued}}}});
    QVERIFY2(published.value("toolOk").toBool(), qPrintable(text(published)));
    QCOMPARE(f.get(published.value("operationId").toString()).value("commitState").toString(),
             QStringLiteral("committed"));
  }
};

int main(int argc, char **argv)
{
  if (!QgisRuntime::initialize())
    return 2;
  TestUnitOnnxAgent test;
  const int result = QTest::qExec(&test, argc, argv);
  QgisRuntime::shutdown();
  return result;
}
#include "tst_unit_onnx_agent.moc"
