// 层：功能
#pragma once
#include <QString>
#include <functional>

// ai/onnxruntimeprobe — ORT C API 准入探测（P4-W1，TODOS「ONNX 动态库
// API 不匹配诊断」）。
//
// 在构造任何依赖 OrtApi 指针的 Ort:: 包装对象（Env/SessionOptions/
// Session/Value…）之前核验 C API 可用性。TODOS 记录的失效模式：Windows
// 构建目录缺应用本地 DLL 时进程实际加载了 System32 的旧 ONNX Runtime，
// `OrtGetApiBase()->GetApi(ORT_API_VERSION)` 返回 nullptr 后包装对象构造
// 发生访问异常——C++ catch 无法处理空指针解引用，必须在包装对象之前
// 用 C API 判定并拒绝。
//
// 状态分类（进程已正常启动且服务可探测的范围内）：
//   · Ok               —— C API 取得，所需 API 版本可用；
//   · NotBuilt         —— 本构建未编入 ORT（PALEO_HAVE_ORT=0，编译期事实，
//                          探测实现不会被链接，此值仅供无 ORT 构建的调用方
//                          语义参考）；
//   · ApiUnavailable   —— OrtGetApiBase() 本身不可用（可观测的运行库缺失/
//                          加载不可用，仅限进程已启动后的运行期状态）；
//   · ApiVersionMismatch —— 运行库可用但 GetApi(所需版本) 返回空（旧运行库）。
//
// 部署边界（如实记录，不为诊断用例改造 loader）：ORT 以动态库链接进本
// 进程，运行库缺失/无法满足导入表时操作系统在 main 之前终止启动——该
// 状况不可被任何进程内 guard 捕获，只能由部署/打包自检（paleo-dev、
// selfcheck、便携包校验）在进程外发现。本探测只承诺：进程已启动后，
// 上述两类 API 不匹配在任何 Ort 包装对象构造之前得到确定结论。
//
// 错误报告包含：所需 API 版本、实际运行库版本（GetVersionString，读不到
// 记「无法读取」）、实测加载模块位置（Linux /proc/self/maps；Windows
// GetModuleHandle+GetModuleFileName——不是磁盘候选路径）、受控前缀信息
// （PaleoOnnxService::vendorRuntimeDir()）。不降级所需 API、不注入系统
// 目录、不替换系统 DLL、不新增依赖来源。
//
// 头文件保持 ORT-free（同 onnxpredictionservice.h 约定）：消费方只引
// Qt 头；ORT C 头仅在 .cpp 内。
namespace paleo::onnx
{

struct OnnxRuntimeProbeResult
{
  enum class Status
  {
    Ok,
    NotBuilt,
    ApiUnavailable,
    ApiVersionMismatch,
  };
  Status status = Status::NotBuilt;
  int requiredApiVersion = 0;   // 编译头所需的 ORT_API_VERSION
  QString runtimeVersion;       // 运行库自报版本；空 = 无法读取
  QString loadedModulePath;     // 实测已加载模块路径；空 = 未知/未加载
  QString controlledPrefix;     // vendor/onnxruntime 受控前缀（信息性）
  QString detail;               // 人类可读诊断（含上述全部事实）

  bool ok() const { return status == Status::Ok; }
  // 稳定公开码（错误通道/UI 分类用，见 onnxPublicLoadCode 同族约定）。
  QString publicCode() const;
  // 汇总文案：失败时逐项列事实；成功时给一行运行库身份。
  QString summaryText() const;
};

class OnnxRuntimeProbe
{
public:
  // 线程安全：首次并发探测一致（纯 C API 调用 + 只读 /proc 解析，无共享
  // 可变状态；结果不缓存——运行库身份在进程生命周期内不变，调用方可在
  // 自己的临界区内缓存）。
  static OnnxRuntimeProbeResult probe();

  // 测试注入点（确定性负向回归专用）：置空恢复真实探测。生产代码不得
  // 调用；注入函数在 probe() 最前求值，保证结论先于任何 Ort 包装对象。
  static void setTestOverride( const std::function<OnnxRuntimeProbeResult()> &overrideFn );

  // 实测已加载模块路径（Linux: /proc/self/maps 中 libonnxruntime 首个映射；
  // Windows: GetModuleHandleW 查询失败则空）。只报告事实，不猜测。
  static QString loadedRuntimeModulePath();
};

} // namespace paleo::onnx
