// 层：数据
// Candidate PY3 adapter. Reuses PY1's one process-wide permit, pump and cleanup.
#include "pythonsandbox_linux.h"
#include "pythonsandbox_linux_image.h"
#include "services/pythonprocess_linux_internal.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QLibrary>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace paleo::python {
namespace {
using process_detail::linux_native::Fd;
std::shared_ptr<Fd> strongFilter() {
  QLibrary lib("/usr/lib/libseccomp.so.2.6.0");
  using Init=void *(*)(quint32);using Resolve=int (*)(const char *);
  using Add=int (*)(void *,quint32,int,unsigned,...);using Export=int (*)(void *,int);using Release=void (*)(void *);
  const auto init=reinterpret_cast<Init>(lib.resolve("seccomp_init"));
  const auto resolve=reinterpret_cast<Resolve>(lib.resolve("seccomp_syscall_resolve_name"));
  const auto add=reinterpret_cast<Add>(lib.resolve("seccomp_rule_add"));
  const auto exportBpf=reinterpret_cast<Export>(lib.resolve("seccomp_export_bpf"));
  const auto release=reinterpret_cast<Release>(lib.resolve("seccomp_release"));
  if(!init||!resolve||!add||!exportBpf||!release)return {};
  void *ctx=init(0x7fff0000U);if(!ctx)return {};
  auto fd=std::make_shared<Fd>(::memfd_create("paleo-python-seccomp",MFD_CLOEXEC|MFD_ALLOW_SEALING));
  bool valid=fd->value>=0;
  const char *blocked[]={"socket","socketpair","connect","bind","listen","accept","accept4","fork","vfork","clone",
    "unshare","setns","ptrace","process_vm_readv","process_vm_writev","mount","umount2","pivot_root","keyctl",
    "add_key","request_key","bpf","perf_event_open","userfaultfd","io_uring_setup","io_uring_register","io_uring_enter",
    "open_by_handle_at","name_to_handle_at","pidfd_getfd","open_tree","move_mount","fsopen","fsmount","fspick","mount_setattr"};
  for(const auto name:blocked) {const int nr=resolve(name);if(nr>=0&&add(ctx,0x50000U|EPERM,nr,0))valid=false;}
  const auto clone3=resolve("clone3");if(clone3>=0&&add(ctx,0x50000U|ENOSYS,clone3,0))valid=false;
  if(valid)valid=exportBpf(ctx,fd->value)==0&&::lseek(fd->value,0,SEEK_SET)==0&&
    ::fcntl(fd->value,F_ADD_SEALS,F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE)==0;
  release(ctx);return valid?fd:std::shared_ptr<Fd>();
}
QString strongPrepare(const SandboxRequest &request,process_detail::Launch &native,
                      process_detail::linux_native::Prepared &prepared,const SealedRuntimeImage &image,
                      const SealedRuntimeImage *input) {
  using namespace process_detail::linux_native;
  const auto runtime=image.runtime->root;
  const auto program=image.runtime->interpreter;
  if(runtime.isEmpty()||!program.startsWith(runtime+"/"))
    return "python_sandbox_paths_invalid";
  const auto executable=QDir(runtime).relativeFilePath(program);
  bool executablePresent=false;for(const auto &file:image.files)if(file.path==executable&&file.executable)executablePresent=true;
  if(!executablePresent)return "python_manifest_interpreter_missing";
  if(!input)return "python_sandbox_input_identity_required";
  const auto inputBytes=input?input->bytes:0;
  if(image.bytes+inputBytes>512*1024*1024||image.bytes+inputBytes>request.policy.memoryBytes-64*1024*1024)
    return "python_snapshot_memory_budget";
  if(image.files.size()+(input?input->files.size():0)>4000)return "python_snapshot_fd_budget";
  if(const auto code=prepareEnvelope(native,prepared);!code.isEmpty())return code;
  const auto helpers=verifiedHelpers();if(helpers.isEmpty())return "python_linux_helpers_untrusted";
  prepared.filter=strongFilter();if(!prepared.filter)return "python_linux_seccomp_unavailable";
  prepared.fileBytes=0;
  prepared.arguments += {"/usr/bin/bwrap","--unshare-all","--unshare-user","--unshare-cgroup","--disable-userns","--die-with-parent",
    "--new-session","--cap-drop","ALL","--clearenv","--setenv","LANG","C.UTF-8","--setenv","OPENBLAS_NUM_THREADS","1",
    "--setenv","OMP_NUM_THREADS","1","--dir","/usr/lib","--symlink","usr/lib","/lib",
    "--symlink","usr/lib","/lib64","--proc","/proc","--remount-ro","/proc","--dev","/dev","--remount-ro","/dev",
    "--dir","/work","--dir","/tmp","--dir","/runtime","--dir","/input"};
  const auto libraries=helpers.value("runtimeLibraries").toObject();
  for(auto it=libraries.begin();it!=libraries.end();++it)prepared.arguments += {"--ro-bind",it.value().toString(),it.key()};
  const auto mountImage=[&](const SealedRuntimeImage &source,const QString &prefix) {
   for(const auto &file:source.files) {
    if(file.fd<16||file.fd>=4096)return "python_snapshot_fd_budget";
    if(file.executable)prepared.arguments += {"--perms","0555"};
    prepared.arguments += {"--ro-bind-data",QString::number(file.fd),prefix+file.path};
    prepared.imageFds.append(file.fd);prepared.fileBytes=qMax(prepared.fileBytes,file.size);
   }
   return "";
  };
  if(const auto code=mountImage(image,"/runtime/");*code)return code;
  if(input)if(const auto code=mountImage(*input,"/input/");*code)return code;
  if(prepared.arguments.size()>8750)return "python_snapshot_argument_budget";
  prepared.arguments += {"--seccomp","3","--block-fd","4","--remount-ro","/","--chdir","/work","--",
    "/runtime/"+executable,"-I","-u","-B","-X","utf8","/input/code.py"};
  qint64 argumentBytes=0;
  for(const auto &arg:prepared.arguments)argumentBytes+=arg.toUtf8().size()+1+qint64(sizeof(char*));
  if(argumentBytes>1024*1024)return "python_snapshot_argument_budget";
  return {};
}
}
SandboxResult runLinuxSandbox(const SandboxRequest &request) {
  SandboxResult result;
  result.childMemoryLimitBytes=request.policy.memoryBytes;
  if(!request.policy.allowGenerated) {result.code="python_generated_execution_denied";return result;}
  if(const auto code=request.policy.validate();!code.isEmpty()) {result.code=code;return result;}
  process_detail::Launch native;native.policy=request.policy;native.cancelled=request.cancelled;
  native.permit=request.permit?request.permit:process_detail::acquireProcessPermit(request.cancelled);
  if(!native.permit) {result.code="cancelled";return result;}
  if(!request.runtime||!request.input) {result.code="python_sandbox_identity_required";return result;}
  constexpr qint64 metadataReservation=32*1024*1024;
  const qint64 fixedReservation=metadataReservation+3*(request.policy.outputBytes+request.policy.logBytes);
  const auto hostCapacity=qMin<qint64>(512*1024*1024,request.policy.memoryBytes)-fixedReservation;
  if(hostCapacity<0||request.runtime->treeSize>hostCapacity) {result.code="python_snapshot_memory_budget";return result;}
  const auto image=snapshotLinuxRuntime(request.runtime,hostCapacity,request.cancelled,result.code);
  if(!image)return result;
  std::shared_ptr<SealedRuntimeImage> input;
  if(request.input) {
    const auto capacity=qMin<qint64>(512*1024*1024,request.policy.memoryBytes-64*1024*1024)-image->bytes;
    input=snapshotLinuxInputs(*request.input,request.policy,capacity,hostCapacity-image->bytes,request.cancelled,result.code);
    if(!input)return result;
  }
  result.hostSnapshotReservedBytes=fixedReservation+image->bytes+input->bytes+input->bufferedBytes;
  native.inputChunks=input->stdinChunks;
  process_detail::linux_native::Prepared prepared;
  result.code=strongPrepare(request,native,prepared,*image,input.get());if(!result.code.isEmpty())return result;
  const auto work=process_detail::linux_native::runPreparedChild(native,prepared);
  result.code=work.code;result.output=work.output;result.error=work.error;
  result.outputLog=work.outputLog;
  result.cpuMicros=work.cpuMicros;result.peakMemoryBytes=work.peakMemoryBytes;
  result.exitCode=work.exitCode;result.memoryPeakComplete=work.memoryPeakComplete;return result;
}
QString linuxSandboxReadiness() {
  using namespace process_detail::linux_native;
  process_detail::Launch native;native.policy.allowTrusted=true;
  native.policy.memoryBytes=64*1024*1024;native.policy.cpuSeconds=5;native.policy.wallSeconds=5;
  native.permit=process_detail::acquireProcessPermit();
  Prepared prepared;if(const auto code=prepareEnvelope(native,prepared);!code.isEmpty())return code;
  prepared.filter=strongFilter();if(!prepared.filter)return "python_linux_seccomp_unavailable";
  prepared.fileBytes=0;
  prepared.arguments += {"/usr/bin/bwrap","--unshare-all","--unshare-user","--unshare-cgroup","--disable-userns",
    "--die-with-parent","--new-session","--cap-drop","ALL","--clearenv","--ro-bind","/usr","/usr",
    "--symlink","usr/lib","/lib","--symlink","usr/lib","/lib64","--seccomp","3","--block-fd","4",
    "--remount-ro","/","--","/usr/bin/bwrap","--version"};
  const auto result=runPreparedChild(native,prepared);
  return result.ok()?QStringLiteral("ok"):result.code;
}
}
