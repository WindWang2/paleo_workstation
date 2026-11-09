#pragma once
#include "catalog/datacatalog.h"
#include "catalog/areasourceprojection.h"
#include "catalog/areawellprojection.h"
#include "io/areamodel.h"
#include "metadata/paleoprojectstore.h"
#include "services/paleotaskservice.h"
#include "workflow/documentsession.h"
#include <paleo_data/service.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstring>
#include <algorithm>

namespace document_fixture {
using namespace paleo::document;
inline bool write(const QString &path,const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size();
}
inline QByteArray smallSegy() {
    QByteArray bytes(3600,'\0');for(int i=0;i<3200;++i)bytes[i]=' ';
    qToBigEndian<quint16>(2000,bytes.data()+3216);qToBigEndian<quint16>(8,bytes.data()+3220);
    qToBigEndian<quint16>(5,bytes.data()+3224);qToBigEndian<quint16>(0x100,bytes.data()+3500);
    qToBigEndian<quint16>(1,bytes.data()+3502);
    for(int i=0;i<4;++i) {
        QByteArray trace(272,'\0');qToBigEndian<quint16>(1,trace.data()+70);
        qToBigEndian<quint16>(8,trace.data()+114);qToBigEndian<quint16>(2000,trace.data()+116);
        qToBigEndian<quint32>(100+i/2,trace.data()+188);qToBigEndian<quint32>(200+i%2,trace.data()+192);
        qToBigEndian<quint32>(500000+i%2*50,trace.data()+180);qToBigEndian<quint32>(4000000+i/2*50,trace.data()+184);
        for(int sample=0;sample<8;++sample) {const float v=float(i*10+sample);quint32 bits;std::memcpy(&bits,&v,4);qToBigEndian(bits,trace.data()+240+sample*4);}
        bytes.append(trace);
    }
    return bytes;
}
struct Fixture {
    QTemporaryDir temp;
    QString project=temp.filePath("project"),meta=project+"/project.sqlite",error;
    DataCatalog catalog;
    PaleoProjectStore store;
    PaleoTaskService tasks{&store};
    DocumentSession session{&store,&tasks};
    bool init() {
        QDir().mkpath(project);store.setProjectPaths(project+"/project.qgz",project+"/project.gpkg",meta);
        return catalog.open(project,&error);
    }
    bool bind(bool readOnly=false) {return session.bind(&catalog,meta,"fixture-document",readOnly,&error);}
    QString external(const QString &name,const QByteArray &bytes) {
        const auto path=temp.filePath(name);if(!write(path,bytes)) return {};
        CatalogAsset asset;asset.id=catalog.nextAssetId();asset.type="document";asset.displayName=name;
        if(!catalog.addAsset(asset,&error))return {};
        CatalogVersion version;version.id=catalog.nextVersionId();version.assetId=asset.id;version.stage="RAW";
        version.managed=false;version.path=path;version.sourceUri=path;version.fileName=QFileInfo(path).fileName();
        version.sha256=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
        return catalog.addVersion(version,&error)?version.id:QString();
    }
    bool importModel(const QString &path,bool comprehensiveWellMetreDepth=false) {
        paleo::data::DataService service;std::string why;const auto root=paleo::area_model::directory(project).toStdString();
        const bool exists=QFileInfo::exists(QString::fromStdString(root)+"/data.sqlite");
        if(!(exists?service.openProject(root,why):service.createProject(root,"fixture",why))) {error=QString::fromUtf8(why);return false;}
        paleo::data::ImportOptions options;options.copySources=false;
        options.parsing.comprehensiveWellMetreDepth=comprehensiveWellMetreDepth;
        paleo::data::ImportReport report;
        if(!service.importFile(path.toUtf8().toStdString(),options,report,why)) {error=QString::fromUtf8(why);return false;}
        service.closeProject();
        return registerAreaWells(catalog,&error) && registerAreaSources(catalog,&error);
    }
    bool preparedWellSource(const QString &name,const QStringList &wellNames,
                            const QStringList &relations={"checks"},const QStringList &tracks={},bool withSurvey=false) {
        const auto path=temp.filePath(name);if(!write(path,name.toUtf8()))return false;
        paleo::data::DataService service;std::string why;const auto root=paleo::area_model::directory(project).toStdString();
        const bool exists=QFileInfo::exists(QString::fromStdString(root)+"/data.sqlite");
        if(!(exists?service.openProject(root,why):service.createProject(root,"fixture",why))) {error=QString::fromUtf8(why);return false;}
        paleo::data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";
        for(const auto &wellName:wellNames) {
            paleo::data::WellDataset well;well.wellInfo.wellName=wellName.toStdString();
            if(withSurvey) {
                well.defaultDepthInfo.depthType="MD";well.defaultDepthInfo.depthUnit="m";well.defaultDepthInfo.mdReference="KB";
                well.wellbore.trajectories.push_back({"vertical test survey","grid north",{{0,0,0},{250,0,0}}});
            }
            for(int i=0;i<relations.size();++i) {
                paleo::data::TimeDepthTable table;table.relationName=relations[i].toStdString();table.depthInfo.depthType="MD";table.depthInfo.depthUnit="m";
                table.timeUnit="ms";table.timeType="TWT";table.timeReference="fixture";table.depthTimePairs={{100,double(200+i*20)},{200,double(300+i*20)}};
                well.timeDepthRelations.push_back(table);
            }
            for(int i=0;i<tracks.size();++i) {
                paleo::data::IntervalTrack track;track.trackName=tracks[i].toStdString();track.trackType="stratigraphy";
                track.depthInfo.depthType="MD";track.depthInfo.depthUnit="m";
                paleo::data::GeologicalInterval interval;interval.intervalName="same-unit-name";
                interval.topDepth=100+i*10;interval.bottomDepth=200+i*10;track.intervals.push_back(interval);
                well.wellbore.intervalTracks.push_back(track);
            }
            parsed.wells.push_back(well);
        }
        const auto sha=QCryptographicHash::hash(name.toUtf8(),QCryptographicHash::Sha256).toHex().toStdString();long long id=0;
        if(!service.importPreparedFile(path.toStdString(),path.toStdString(),path.toStdString(),sha,parsed,id,why)) {error=QString::fromUtf8(why);return false;}
        service.closeProject();return registerAreaWells(catalog,&error) && registerAreaSources(catalog,&error);
    }
};
}
