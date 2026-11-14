// 层：测试壳
#include "wellfaciesagentfixture.h"
#include "services/areawellmetadata.h"
class WellFaciesInputPlanTest:public QObject {
  Q_OBJECT
private slots:
  void metadataProducerBudgets_data() {
    QTest::addColumn<QString>("kind");QTest::addColumn<int>("count");
    QTest::newRow("curve-limit-probe")<<"curves"<<4096;
    QTest::newRow("track-limit-probe")<<"intervals"<<128;
  }
  void metadataProducerBudgets() {
    QFETCH(QString,kind);QFETCH(int,count);Fixture f;QVERIFY2(f.init(),f.error.c_str());
    data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";data::WellDataset well;
    well.wellInfo.wellName="SYNTH-1";well.defaultDepthInfo={"MD","m","KB"};
    data::LogSet logs;logs.logSetName="Additional";logs.depthInfo=well.defaultDepthInfo;logs.samplingAxis.depthValues={100.,101.,102.};
    for(int i=0;i<count;++i) {
      if(kind=="curves"){data::LogCurve curve;curve.curveName="Other-"+std::to_string(i);curve.curveUnit="API";
        curve.sampleValues={1.,2.,3.};logs.logCurves.push_back(curve);}
      else{data::IntervalTrack track;track.trackName="Other-"+std::to_string(i);track.trackType="岩性";
        track.depthInfo=well.defaultDepthInfo;well.wellbore.intervalTracks.push_back(track);}
    }
    if(kind=="curves")well.wellbore.logSets.push_back(logs);parsed.wells.push_back(well);
    const QByteArray bytes("expanded synthetic fixture");const auto path=f.temp.filePath("expanded");QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(bytes),bytes.size());file.close();long long source=0;
    const auto sha=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex().toStdString();
    QVERIFY2(f.service.importPreparedFile(path.toStdString(),path.toStdString(),path.toStdString(),sha,parsed,source,f.error),f.error.c_str());
    const auto input=wf::prepareInput(f.job);QCOMPARE(input.code,"input_metadata_budget_exceeded");QVERIFY(input.requestBodies.isEmpty());
    const auto inspected=wf::inspectInput({f.job.projectDir,f.job.request.well,f.job.request.context});
    QVERIFY(!inspected.value("ok").toBool());QCOMPARE(inspected.value("code").toString(),"input_metadata_budget_exceeded");
    if(kind=="curves") {
      int checks=0;std::vector<data::DataEntry> entries;std::string error;
      QVERIFY(!readBoundedAreaWellMetadata(f.service,"curves",f.job.request.well.modelWellId,4096,entries,error,[&]{return ++checks==3;}));
      QCOMPARE(checks,3);QVERIFY(entries.empty());QCOMPARE(error,std::string("cancelled"));
    }
  }
  void metadataDiscoveryProvidesExactReferencesAndFixedUnit() {
    Fixture f;QVERIFY2(f.init(),f.error.c_str());wf::InputInspectionJob job;
    job.projectDir=f.job.projectDir;job.well=f.job.request.well;job.context=f.job.request.context;
    const auto metadata=wf::inspectInput(job);QVERIFY2(metadata.value("ok").toBool(),qPrintable(metadata.value("code").toString()));
    QCOMPARE(metadata.value("coverage").toObject().value("scope").toString(),"active_curve_and_interval_metadata");
    QVERIFY(metadata.value("coverage").toObject().value("complete").toBool());QVERIFY(!metadata.value("hasMore").toBool());
    const auto curve=metadata.value("curves").toArray().first().toObject();QCOMPARE(curve.value("name").toString(),"GR");
    QVERIFY(curve.value("id").isString());QCOMPARE(curve.value("unit").toString(),"API");QCOMPARE(curve.value("mdReference").toString(),"KB");
    QVERIFY(metadata.value("contextAvailable").toBool());QCOMPARE(metadata.value("occurrences").toArray().size(),2);
    QCOMPARE(metadata.value("unit_scope").toObject(),wf::unitScopeJson(f.job.request.scope));
    job.context.reset();const auto unselected=wf::inspectInput(job);QVERIFY(unselected.value("ok").toBool());
    QVERIFY(!unselected.value("contextAvailable").toBool());QCOMPARE(unselected.value("contextCode").toString(),"unit_scope_unavailable");
  }
  void sparseReferenceDoesNotBridgeAnotherCurveHole() {
    WellComposite::ComprehensiveWellData data;WellComposite::CurveData sparse,dense;
    sparse.name="GR";sparse.unit="API";dense.name="DT";dense.unit="US/M";
    for(int depth=100;depth<=150;depth+=10){sparse.depths.append(depth);sparse.values.append(20);}
    for(int depth=100;depth<=150;++depth)if(depth<109||depth>111){dense.depths.append(depth);dense.values.append(30);}
    data.continuousCurves={sparse,dense};WellComposite::FormationInterval unit;
    unit.topDepth=100;unit.bottomDepth=151;unit.name="unit";unit.unitType=QStringLiteral("地震层位");data.formationIntervals={unit};
    WellComposite::LithologyInterval lithology;lithology.topDepth=100;lithology.bottomDepth=151;lithology.lithoName=QStringLiteral("砂岩");
    data.lithologyIntervals={lithology};WellFaciesModel model;model.id="model";model.window=2;model.curves={"GR","DT"};
    QVector<WellFaciesInput> segments;QStringList skipped;
    const auto first=prepareWellFaciesInput(data,model,&segments,&skipped,false,WellFaciesSourcePolicy::DeclaredSdkValues);
    QVERIFY2(first.ready(),qPrintable(first.reason));
    QVERIFY(!skipped.isEmpty());for(const auto &segment:segments)for(const auto &row:segment.rows)
      QVERIFY(row.toObject().value(QStringLiteral("深度")).toDouble()!=110.);
  }
  void actualSourcesAndSeparateOccurrences() {
    Fixture f;QVERIFY2(f.init(),f.error.c_str());
    const auto out=wf::prepareInput(f.job);QVERIFY2(out.ok,qPrintable(out.code+": "+out.error));
    QCOMPARE(out.occurrences.size(),2);QCOMPARE(out.segments.size(),2);QCOMPARE(out.curves.size(),1);
    QVERIFY(out.curves.front().curveId>0);QVERIFY(out.curves.front().sourceId>0);QVERIFY(out.curves.front().logSetId>0);
    QCOMPARE(out.curves.front().sourceFingerprint,QString::fromLatin1(QCryptographicHash::hash("synthetic",QCryptographicHash::Sha256).toHex()));
    QSet<int>depths;
    for(int i=0;i<out.segments.size();++i){const auto &segment=out.segments[i];QCOMPARE(segment.rows.size(),10);
      QCOMPARE(segment.modelLabels,f.job.model.labels);QVERIFY(!segment.facies.isEmpty());
      const auto body=QJsonDocument::fromJson(out.requestBodies[i]).object();QCOMPARE(body.value("modelVersionId").toString(),"model");
      for(const auto &row:segment.rows){const auto d=row.toObject().value(QStringLiteral("深度")).toDouble();
        QVERIFY(d<110||d>=200);depths.insert(int(d));}}
    QCOMPARE(depths.size(),20);QCOMPARE(out.requestHashes.size(),2);QVERIFY(out.requestHashes[0]!=out.requestHashes[1]);
    QCOMPARE(out.inputHash.size(),64);QCOMPARE(out.featureSchemaHash.size(),64);
    QCOMPARE(out.sourceManifest.value("occurrences").toArray().size(),2);
  }
  void holesProduceIndependentRequests() {
    Fixture f;QVERIFY2(f.init("MD","m",true),f.error.c_str());
    const auto out=wf::prepareInput(f.job);QVERIFY2(out.ok,qPrintable(out.error));QCOMPARE(out.segments.size(),3);
    for(const auto &segment:out.segments){double previous=qQNaN();for(const auto &value:segment.rows){
      const double d=value.toObject().value(QStringLiteral("深度")).toDouble();QVERIFY(d!=104.);
      if(std::isfinite(previous))QVERIFY(d-previous<1.01);previous=d;}}
    QVERIFY(!out.skipped.isEmpty());
  }
  void exactCurveResolvesAmbiguity() {
    Fixture f;QVERIFY2(f.init("MD","m",false,true),f.error.c_str());
    QCOMPARE(wf::prepareInput(f.job).code,"ambiguous_curve_source");
    wf::InputInspectionJob query{f.job.projectDir,f.job.request.well,f.job.request.context};
    const auto metadata=wf::inspectInput(query);QVERIFY(metadata.value("ok").toBool());
    const auto curves=metadata.value("curves").toArray();QCOMPARE(curves.size(),2);
    const auto id=curves.last().toObject().value("id").toString().toLongLong();f.job.request.curveIds.insert("GR",id);
    const auto out=wf::prepareInput(f.job);QVERIFY2(out.ok,qPrintable(out.error));QCOMPARE(out.curves.front().curveId,id);
  }
  void distinctModelFeaturesCanSelectDifferentAliasChannels() {
    Fixture f;QVERIFY2(f.init("MD","m",false,true),f.error.c_str());std::vector<data::DataEntry>curves;
    QVERIFY(f.service.listData("curves",f.job.request.well.modelWellId,curves,f.error));QCOMPARE(curves.size(),size_t(2));
    f.job.model.curves={"GR","SGR"};f.job.model.curveUnits={{"GR","API"},{"SGR","API"}};
    f.job.request.curveIds={{"GR",curves[0].id},{"SGR",curves[1].id}};
    QVERIFY(f.service.updateCurveValue(curves[1].id,0,77.,f.error));
    const auto out=wf::prepareInput(f.job);QVERIFY2(out.ok,qPrintable(out.error));QCOMPARE(out.curves.size(),2);
    bool observed=false;
    for(const auto &segment:out.segments)for(const auto &value:segment.rows){const auto row=value.toObject();
      if(row.value(QStringLiteral("深度")).toDouble()==100.){QCOMPARE(row.value("GR").toDouble(),20.);
        QCOMPARE(row.value("SGR").toDouble(),77.);observed=true;}}
    QVERIFY(observed);
  }
  void depthConventions_data() {
    QTest::addColumn<QString>("type");QTest::addColumn<QString>("unit");QTest::addColumn<bool>("success");
    QTest::newRow("feet")<<"MD"<<"ft"<<true;
    QTest::newRow("tvd")<<"TVD"<<"m"<<false;
    QTest::newRow("tvdss")<<"TVDSS"<<"m"<<false;
    QTest::newRow("unknown-unit")<<"MD"<<""<<false;
  }
  void depthConventions() {
    QFETCH(QString,type);QFETCH(QString,unit);QFETCH(bool,success);
    Fixture f;QVERIFY2(f.init(type,unit),f.error.c_str());const auto out=wf::prepareInput(f.job);
    QCOMPARE(out.ok,success);if(success){QCOMPARE(out.segments.size(),2);
      QCOMPARE(out.curves.front().sourceDepthUnit,unit);QCOMPARE(out.curves.front().sourceDepthType,"MD");
      QVERIFY(std::abs(out.occurrences.front().bottomMd-out.occurrences.front().topMd-10.)<1e-8);}
    else{QVERIFY(!out.code.isEmpty());QVERIFY(out.requestBodies.isEmpty());}
  }
  void unknownTrainingUnitsRejectBeforeDataAccess() {
    Fixture f;f.job.model={"model","M","v1",{}, {"GR"},{QStringLiteral("河口坝")},2};
    f.job.request.modelId="model";f.job.request.modelVersion="v1";
    QCOMPARE(wf::prepareInput(f.job).code,"model_feature_units_unavailable");
  }
  void featureUnitPrefixesAreNotCaseFolded() {
    Fixture f;QVERIFY2(f.init("MD","m",false,false,"mV"),f.error.c_str());f.job.model.curveUnits["GR"]="MV";
    QCOMPARE(wf::prepareInput(f.job).code,"model_feature_unit_mismatch");
  }
  void declaredManualValueIsNotReclassifiedByLegacySentinels() {
    Fixture f;QVERIFY2(f.init(),f.error.c_str());std::vector<data::DataEntry> curves;
    QVERIFY(f.service.listData("curves",f.job.request.well.modelWellId,curves,f.error));
    QVERIFY(f.service.updateCurveValue(curves.front().id,0,-999.25,f.error));
    data::CurveWindowQuery query;query.curveId=curves.front().id;query.sampleCount=1;data::CurveWindowResult values;
    QVERIFY(f.service.readCurveWindow(query,values,f.error));QCOMPARE(values.curve.values.front(),-999.25);
    QVERIFY(values.curve.quality.front()&4);
    const auto out=wf::prepareInput(f.job);QVERIFY2(out.ok,qPrintable(out.error));bool observed=false;
    for(const auto &segment:out.segments)for(const auto &value:segment.rows){const auto row=value.toObject();
      if(row.value(QStringLiteral("深度")).toDouble()==100.){QCOMPARE(row.value("GR").toDouble(),-999.25);observed=true;}}
    QVERIFY(observed);
  }
  void unknownMdOriginIsNotAnAgreement() {
    Fixture f;QVERIFY2(f.init("MD","m",false,false,"API","unknown"),f.error.c_str());
    QCOMPARE(wf::prepareInput(f.job).code,"input_md_reference_unknown");
    const auto metadata=wf::inspectInput({f.job.projectDir,f.job.request.well,f.job.request.context});
    QVERIFY(metadata.value("ok").toBool());QVERIFY(!metadata.value("inputFrameReady").toBool());
  }
  void completeModelClassContractIsResolvedBeforeCurveInput() {
    Fixture f;QVERIFY2(f.init(),f.error.c_str());f.job.model.labels={QStringLiteral("未登记的本地分类")};
    f.job.model.curveUnits["GR"]="also different";
    QCOMPARE(wf::prepareInput(f.job).code,"model_class_schema_unavailable");
  }
  void valueHashAndQueryWatermarkHaveDifferentRoles() {
    Fixture f;QVERIFY2(f.init(),f.error.c_str());const auto first=wf::prepareInput(f.job);QVERIFY2(first.ok,qPrintable(first.error));
    data::WellInfo info;QVERIFY(f.service.readWellInfo(f.job.request.well.modelWellId,info,f.error));
    info.remark="unrelated metadata";QVERIFY(f.service.saveWellInfo(f.job.request.well.modelWellId,info,f.error));
    const auto second=wf::prepareInput(f.job);QVERIFY2(second.ok,qPrintable(second.error));
    QCOMPARE(first.requestHashes,second.requestHashes); // A global watermark is not a curve value revision.
    QCOMPARE(first.inputHash,second.inputHash);
    f.job.request.well.modelGeneration="other generation";QCOMPARE(wf::prepareInput(f.job).code,"stale_well_ref");
  }
  void cancellationDiscardsEverySegment() {
    Fixture f;QVERIFY2(f.init(),f.error.c_str());const auto out=wf::prepareInput(f.job,[]{return true;});
    QCOMPARE(out.code,"cancelled");QVERIFY(!out.ok);QVERIFY(out.requestBodies.isEmpty());
  }
  void persistedFrozenInputSurvivesLiveValuesAndRejectsTampering() {
    Fixture f;QVERIFY2(f.init(),f.error.c_str());const auto input=wf::prepareInput(f.job);QVERIFY2(input.ok,qPrintable(input.error));
    QString code;const auto ref=wf::storeInput(f.job.projectDir,input,code);QVERIFY2(!ref.isEmpty(),qPrintable(code));
    const auto loaded=wf::loadInput(f.job.projectDir,ref);QVERIFY2(loaded.ok,qPrintable(loaded.error));
    QCOMPARE(loaded.requestBodies,input.requestBodies);QCOMPARE(loaded.requestHashes,input.requestHashes);
    QCOMPARE(loaded.model.version,input.model.version);QCOMPARE(loaded.segments[0].modelLabels,input.segments[0].modelLabels);
    QCOMPARE(QJsonArray::fromVariantList(loaded.segments[0].facies),QJsonArray::fromVariantList(input.segments[0].facies));
    QCOMPARE(loaded.occurrences.size(),input.occurrences.size());QCOMPARE(loaded.skipped,input.skipped);
    const QByteArray result("{\"contract\":\"synthetic-result\",\"confidence\":0.9}");
    const auto hash=QString::fromLatin1(QCryptographicHash::hash(result,QCryptographicHash::Sha256).toHex());
    QVERIFY2(wf::storeResultCache(f.job.projectDir,ref,0,result,code),qPrintable(code));
    QCOMPARE(wf::loadResultCache(f.job.projectDir,ref,0,hash,code),result);
    QFile cached(QDir(f.job.projectDir).filePath(ref.value("directory").toString()+"/result-0-"+hash+".json"));
    QVERIFY(cached.open(QIODevice::WriteOnly));cached.write("{}");cached.close();
    QVERIFY(wf::loadResultCache(f.job.projectDir,ref,0,hash,code).isEmpty());QCOMPARE(code,"external_result_cache_mismatch");
    QFile body(QDir(f.job.projectDir).filePath(ref.value("directory").toString()+"/body-0.json"));
    QVERIFY(body.open(QIODevice::WriteOnly));body.write("{}");body.close();
    QCOMPARE(wf::loadInput(f.job.projectDir,ref).code,"stored_input_hash_mismatch");
    auto escaped=ref;escaped.insert("directory","../outside");QCOMPARE(wf::loadInput(f.job.projectDir,escaped).code,"invalid_stored_input_ref");
  }
  void typedRequestHasNoNameOrPathFallback() {
    Fixture f;QVERIFY2(f.init(),f.error.c_str());
    QJsonObject json{{"contract_version",2},{"client_request_id",f.job.request.clientRequestId},
      {"endpoint_id",f.job.request.endpointId},
      {"model",f.job.request.modelId},{"model_version",f.job.request.modelVersion},
      {"well_ref",paleo::document::wellRefJson(f.job.request.well)},
      {"context",paleo::document::interpretationContextJson(f.job.request.context)},
      {"unit_scope",wf::unitScopeJson(f.job.request.scope)}};
    wf::Request parsed;QString code;QVERIFY2(wf::requestFromJson(json,parsed,&code),qPrintable(code));
    json.insert("well_name","other well");QVERIFY(!wf::requestFromJson(json,parsed,&code));QCOMPARE(code,"unknown_well_request_field");
    json.remove("well_name");auto scope=json.value("unit_scope").toObject();scope.insert("aggregate","true");
    json.insert("unit_scope",scope);QVERIFY(!wf::requestFromJson(json,parsed,&code));
  }
  void finalSubIntervalIncludesItsBottomOnly() {
    Fixture f;QVERIFY2(f.init(),f.error.c_str());data::StratigraphicScheme scheme,saved;
    QVERIFY(f.service.readStratigraphicScheme(f.job.request.scope.unit.schemeId.toStdString(),{},scheme,f.error));
    const auto previous=scheme.revision.value;
    for(auto &occ:scheme.wellOccurrences)occ.bottom=occ.top+8.;
    std::vector<data::FrameworkIssue>issues;QVERIFY2(f.service.commitStratigraphicScheme(scheme,previous,saved,issues,f.error),f.error.c_str());
    auto &unit=f.job.request.scope.unit;unit.aggregate=false;unit.revision=QString::fromStdString(saved.revision.value);
    f.job.request.context.aggregate=false;f.job.request.context.schemeRevision=unit.revision;
    for(int index:{1,4}) {
      const auto it=std::find_if(saved.subIntervals.begin(),saved.subIntervals.end(),[index](const auto &sub){return sub.index==index;});
      QVERIFY(it!=saved.subIntervals.end());unit.subIndex=index;unit.subIntervalId=QString::fromStdString(it->id.value);
      f.job.request.context.subIntervalId=unit.subIntervalId;
      const auto out=wf::prepareInput(f.job);QVERIFY2(out.ok,qPrintable(out.error));QCOMPARE(out.segments.size(),2);
      for(const auto &input:out.segments)QCOMPARE(input.rows.size(),index==4?3:2);
    }
  }
};
QTEST_GUILESS_MAIN(WellFaciesInputPlanTest)
#include "tst_wellfaciesinputplan.moc"
