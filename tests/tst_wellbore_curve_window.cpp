// 层：测试壳
#include <QtTest>
#include <QJsonDocument>
#include "fixtures/documentsourcefixture.h"
#include "services/projectquerycapability.h"
#include "services/pythonanalysis_internal.h"
#include "services/pythonanalysisselection.h"
#include "workflow/assetassociation.h"
#include "workflow/wellboresourceconfirmation.h"

using namespace document_fixture;
using namespace paleo;
class WellboreCurveWindowTest : public QObject {
    Q_OBJECT
private slots:
    void relinkingLasToAnotherWellCannotConfirmItsBore() {
        Fixture f;
        QVERIFY2(f.init(), qPrintable(f.error));
        for (const QString &name : QStringList{"A", "B"}) {
            const auto las = QStringLiteral("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. %1\n"
                                            "~Parameter\nDREF. KB : depth reference\n"
                                            "~Curve\nMD.M : measured depth\nGR.API : gamma ray\n"
                                            "~Ascii\n100 %2\n101 %3\n")
                                 .arg(name).arg(name == "A" ? 10 : 110).arg(name == "A" ? 20 : 120);
            const auto path = f.temp.filePath(name + ".las");
            QVERIFY(write(path, las.toUtf8()));
            QVERIFY2(f.importModel(path), qPrintable(f.error));
        }
        const auto wellA = f.catalog.wellsMatchingName("A").value(0);
        const auto wellB = f.catalog.wellsMatchingName("B").value(0);
        QVERIFY(!wellA.isEmpty() && !wellB.isEmpty() && wellA != wellB);
        const auto modelWellA = f.catalog.storedEntityById(wellA).extra.value("areaModelWellId").toLongLong();
        const auto modelWellB = f.catalog.storedEntityById(wellB).extra.value("areaModelWellId").toLongLong();
        QVERIFY(modelWellA > 0 && modelWellB > 0 && modelWellA != modelWellB);
        const auto modelDirectory = area_model::directory(f.project).toStdString();
        data::DataService model;
        std::string why;
        QVERIFY2(model.openProject(modelDirectory, why), why.c_str());
        std::vector<data::DataEntry> curves;
        QVERIFY2(model.listData("curves", modelWellA, curves, why), why.c_str());
        QCOMPARE(curves.size(), size_t(1));
        const auto curve = curves.front();
        data::DepthInfo depth;
        depth.mdReference = "KB";
        QVERIFY2(model.saveLogSetDepthInfo(curve.parentId, depth, why), why.c_str());
        model.closeProject();

        QVERIFY2(f.bind(), qPrintable(f.error));
        SourceRecord source;
        for (const auto &record : f.session.sources())
            if (record.kind == "well_log" && QFileInfo(record.originalPath).fileName() == "A.las") source = record;
        QVERIFY(source.ref.valid());
        QCOMPARE(source.ref.sourceId, curve.sourceId);
        const auto moveLink = [&](const QString &from, const QString &to) {
            const auto links = f.catalog.links();
            for (int i = 0; i < links.size(); ++i) {
                if (links[i].assetId == source.assetId && links[i].entityId == from)
                    return assetassociation::apply(&f.catalog, {source.assetId, to, links[i].role, f.catalog.mutationSeq(), i, false}, &f.error);
            }
            f.error = "Expected the LAS to be linked to " + from;
            return false;
        };
        const auto confirm = [&](const QString &well) {
            return f.session.confirmWellboreSource({source.ref, source.versionId, well, "side", "侧钻",
                                                    f.session.epoch(), f.session.revision()});
        };
        QVERIFY2(moveLink(wellA, wellB), qPrintable(f.error));
        const auto moved = f.catalog.linksForAsset(source.assetId);
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved.front().entityId, wellB);
        const auto rejected = confirm(wellB);
        QVERIFY(!rejected.ok);
        QCOMPARE(rejected.code, QString("wellbore_source_well_mismatch"));

        QVERIFY2(model.openProjectReadOnly(modelDirectory, why), why.c_str());
        std::optional<data::WellboreRef> binding;
        QVERIFY2(model.findWellbore(data::SourceWellboreKey{source.ref.sourceId, "file"}, binding, why), why.c_str());
        QVERIFY(!binding);
        data::WellborePage bores;
        QVERIFY2(model.listWellbores(modelWellB, {}, 10, bores, why), why.c_str());
        QCOMPARE(bores.items.size(), size_t(1));
        QCOMPARE(QString::fromStdString(bores.items.front().ref.key), QString("main"));
        model.closeProject();

        QVERIFY2(moveLink(wellB, wellA), qPrintable(f.error));
        const auto accepted = confirm(wellA);
        QVERIFY2(accepted.ok, qPrintable(accepted.error));
        query::ProjectQueryCapability query;
        query.bind(&f.catalog, {f.session.documentId(), 1, 1});
        query.setSourceResolver([&](const SourceRef &ref) { return f.session.source(ref); });
        const auto wellRef = query.query({{"topic", "well_details"}, {"well_id", wellA}}).value("well_ref").toObject();
        const auto page = query.query({{"topic", "well_curves"}, {"well_ref", wellRef}, {"wellbore_key", "side"}});
        QVERIFY2(page.value("ok").toBool(), qPrintable(QJsonDocument(page).toJson()));
        const auto rows = page.value("curves").toArray();
        QCOMPARE(rows.size(), 1);
        const auto input = rows.first().toObject().value("input_ref").toObject();
        QCOMPARE(input.value("wellbore_key").toString(), QString("side"));
        QCOMPARE(input.value("source_version_id").toString(), source.versionId);
        data::CurveWindowQuery window;
        window.curveId = input.value("curve_id").toString().toLongLong();
        QCOMPARE(window.curveId, curve.id);
        window.sampleCount = 2;
        window.processed = false;
        data::CurveWindowResult samples;
        QVERIFY2(model.openProjectReadOnly(modelDirectory, why), why.c_str());
        QVERIFY2(model.readCurveWindow(window, samples, why), why.c_str());
        QCOMPARE(samples.curve.values.size(), size_t(2));
        QCOMPARE(samples.curve.values[0], 10.);
        QCOMPARE(samples.curve.values[1], 20.);
    }
    void sameNamedCurvesReadOnlyTheirConfirmedLas() {
        Fixture f;
        QVERIFY2(f.init(), qPrintable(f.error));
        const QStringList names{"main", "side", "pending"};
        for (int i = 0; i < names.size(); ++i) {
            const auto path = f.temp.filePath(names[i] + ".las");
            const auto bytes = QStringLiteral("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. A\n"
                                              "~Parameter\nDREF. KB : measured depth referenced to Kelly bushing\n"
                                              "~Curve\nMD.M : measured depth\nGR.API : gamma ray\n"
                                              "~Ascii\n100 %1\n101 %2\n").arg(i*100+10).arg(i*100+20).toUtf8();
            QVERIFY(write(path, bytes));
            QVERIFY2(f.importModel(path), qPrintable(f.error));
        }
        std::string why;
        data::DataService model;
        QVERIFY(model.openProject(area_model::directory(f.project).toStdString(), why));
        const auto well = f.catalog.wellsMatchingName("A").value(0);
        const auto wellId = f.catalog.storedEntityById(well).extra.value("areaModelWellId").toLongLong();
        std::vector<data::DataEntry> curves;
        QVERIFY(model.listData("curves", wellId, curves, why));
        QSet<qlonglong> logs;
        for (const auto &curve : curves) logs.insert(curve.parentId);
        QCOMPARE(logs.size(), 3);
        // The LAS reader preserves MD.M. Confirm only the datum declared by the source's DREF parameter.
        data::DepthInfo confirmed;
        confirmed.mdReference = "KB";
        for (const auto id : logs) QVERIFY2(model.saveLogSetDepthInfo(id, confirmed, why), why.c_str());
        QVERIFY2(f.bind(), qPrintable(f.error));
        QHash<QString, SourceRecord> sources;
        for (const auto &source : f.session.sources()) sources.insert(QFileInfo(source.originalPath).baseName(), source);
        for (const auto &key : QStringList{"main","side"}) {
            const auto source = sources.value(key);
            const auto result = f.session.confirmWellboreSource({source.ref, source.versionId, well, key, key, f.session.epoch(), f.session.revision()});
            QVERIFY2(result.ok, qPrintable(result.error));
        }
        query::ProjectQueryCapability query;
        query.bind(&f.catalog, {f.session.documentId(), 1, 1});
        query.setSourceResolver([&](const SourceRef &ref){return f.session.source(ref);});
        const auto wellRef = query.query({{"topic","well_details"},{"well_id",well}}).value("well_ref").toObject();
        for (int i = 0; i < 2; ++i) {
            const auto page = query.query({{"topic","well_curves"},{"well_ref",wellRef},{"wellbore_key",names[i]}});
            QVERIFY2(page.value("ok").toBool(), qPrintable(QJsonDocument(page).toJson()));
            QVERIFY(page.value("unconfirmedCurveCount").toInt() > 0);
            QJsonObject curveInput;
            for (const auto &value : page.value("curves").toArray()) {
                const auto row = value.toObject();
                if (row.value("name") != "GR") continue;
                QVERIFY(curveInput.isEmpty());
                curveInput = row.value("input_ref").toObject();
            }
            QCOMPARE(curveInput.value("wellbore_key").toString(), names[i]);
            QCOMPARE(curveInput.value("depth_type").toString(), QString("MD"));
            QCOMPARE(curveInput.value("depth_unit").toString(), QString("M"));
            QCOMPARE(curveInput.value("depth_reference").toString(), QString("KB"));
            const QJsonObject inputs{{"well_ref",wellRef},{"curves",QJsonArray{curveInput}},{"first_sample","0"},{"sample_count",2},
                {"processed",false},{"alignment","exact_shared_axis_v1"},{"sampling",QJsonObject{{"method","all_selected_window_v1"},{"seed",0}}}};
            python::AnalysisSelectionRequest selected;
            QVERIFY2(python::analysisSelectionFromJson(inputs, python::AnalysisKind::WellQuality, selected, &f.error), qPrintable(f.error));
            python::AnalysisReadPlan plan;
            plan.curves = selected.curves;
            auto &selection = plan.curves[0];
            selection.source = f.session.source(selection.source.ref);
            plan.documentId = f.session.documentId();
            QVERIFY(model.readModelChangeHead(plan.expectedHead, why));
            QVector<data::CurveWindowResult> windows;
            python::AnalysisReadMetrics usage;
            QCOMPARE(python::analysis_detail::readColumns(model, plan, windows, usage, {}), QString());
            QCOMPARE(windows.size(), 1);
            QCOMPARE(windows[0].curve.values.size(), size_t(2));
            QCOMPARE(windows[0].curve.values[0], double(i*100+10));
            QCOMPARE(windows[0].curve.values[1], double(i*100+20));
            selection.wellboreKey = names[1-i];
            usage = {};
            QCOMPARE(python::analysis_detail::readColumns(model, plan, windows, usage, {}), QString("wellbore_source_mismatch"));
            QCOMPARE(usage.payloadBytes, 0);
        }
    }
};
QTEST_GUILESS_MAIN(WellboreCurveWindowTest)
#include "tst_wellbore_curve_window.moc"
