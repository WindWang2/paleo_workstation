// 层：测试壳
#pragma once
#include "documentsourcefixture.h"
#include "pythonnumerictablefixture.h"
#include "app/pythonnumerictablewiring.h"
#include "metadata/projectlock.h"
#include <memory>
namespace numeric_table_host_fixture {
using namespace paleo;
struct Fixture:document_fixture::Fixture {
  numeric_table_fixture::Model table;
  std::unique_ptr<ProjectDirLock> lock;
  std::unique_ptr<capability::CapabilityHost> host;
  host::CallContext context;
  ~Fixture(){host.reset();tasks.shutdown(5000,true);}
  bool open() {
    if(!init())return false;lock=std::make_unique<ProjectDirLock>(project);if(!lock->tryLock(&error))return false;
    if(!table.open(project,"fixture-document")){error=table.error;return false;}
    if(!registerAreaSources(catalog,&error)||!bind())return false;
    table.plan.source=session.source(table.plan.selection.source);
    if(table.plan.source.versionId.isEmpty()){error="formal table source mapping missing";return false;}
    context.documentId=session.documentId();context.epoch=1;context.open=true;context.persistentIdentity=true;
    context.ownsWrites=true;context.allowedEffects=host::Read|host::PrivateStage|host::LocalExecute|host::CatalogPublish|host::Presentation;
    resetHost();return true;
  }
  void resetHost() {
    host=std::make_unique<capability::CapabilityHost>(capability::NativeAdapterFactory{},nullptr);host->setTaskService(&tasks);
    host->setCallerScope(host::Caller::Agent,table.plan.callerScope);
    host->setBindingProvider([this] {
      capability::HostBinding out;out.catalog=&catalog;out.projectDir=project;out.operationDir=project+"/.paleo/capability-ops";
      out.context=context;out.writeClaim=lock->claim();return out;
    });
  }
  capability::ExecuteRequest request() const {
    capability::ExecuteRequest out;out.capabilityId=QLatin1String(python::NumericTableCapability);out.contractVersion=1;
    out.documentId=session.documentId();out.callerScope=table.plan.callerScope;out.clientRequestId="numeric-table-request";
    out.projectDir=project;out.bindingMode="pinned-value";out.inputs=python::numericTableSelectionJson(table.plan.selection).toVariantMap();return out;
  }
};
}
