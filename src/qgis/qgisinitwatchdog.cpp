// 层：QGIS 封装
#include "qgisinitwatchdog.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
  constexpr int kMaxFrames = 48;
  constexpr int kMaxThreads = 32;
  constexpr auto kFirstDump = std::chrono::seconds( 30 );
  constexpr auto kDumpEvery = std::chrono::seconds( 60 );

  struct Module
  {
    std::uintptr_t base = 0;
    std::uintptr_t size = 0;
    std::string name;
  };

  std::string narrow( const wchar_t *text, int length )
  {
    if ( length <= 0 )
      return {};
    const int n = ::WideCharToMultiByte( CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr );
    std::string out( static_cast<size_t>( std::max( n, 0 ) ), '\0' );
    if ( n > 0 )
      ::WideCharToMultiByte( CP_UTF8, 0, text, length, &out[0], n, nullptr, nullptr );
    return out;
  }

  // psapi 对本进程读 PEB 的加载器链表，不取 loader lock——主线程正卡在
  // DLL 加载里时也能列出。
  std::vector<Module> snapshotModules()
  {
    const HANDLE self = ::GetCurrentProcess();
    std::vector<HMODULE> handles( 512 );
    DWORD needed = 0;
    for ( int attempt = 0; attempt < 2; ++attempt )
    {
      const DWORD bytes = static_cast<DWORD>( handles.size() * sizeof( HMODULE ) );
      if ( !::EnumProcessModules( self, handles.data(), bytes, &needed ) )
        return {};
      if ( needed <= bytes )
        break;
      handles.resize( needed / sizeof( HMODULE ) + 16 );
    }
    handles.resize( std::min<size_t>( handles.size(), needed / sizeof( HMODULE ) ) );
    std::vector<Module> out;
    out.reserve( handles.size() );
    for ( HMODULE h : handles )
    {
      MODULEINFO info {};
      wchar_t name[MAX_PATH];
      if ( !::GetModuleInformation( self, h, &info, sizeof( info ) ) )
        continue;
      const DWORD n = ::GetModuleBaseNameW( self, h, name, MAX_PATH );
      out.push_back( { reinterpret_cast<std::uintptr_t>( info.lpBaseOfDll ), info.SizeOfImage,
                       narrow( name, static_cast<int>( n ) ) } );
    }
    return out;
  }

  // 模块内不超过 rva 的最近导出名（不依赖 PDB / dbghelp，纯读已映射的 PE
  // 导出表）。返回 nullptr 表示无导出或读失败。
  const char *nearestExport( std::uintptr_t base, std::uintptr_t rva, std::uintptr_t *offset )
  {
#ifdef _MSC_VER
    __try
#endif
    {
      const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>( base );
      if ( dos->e_magic != IMAGE_DOS_SIGNATURE )
        return nullptr;
      const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>( base + dos->e_lfanew );
      if ( nt->Signature != IMAGE_NT_SIGNATURE )
        return nullptr;
      const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
      if ( dir.VirtualAddress == 0 || dir.Size == 0 )
        return nullptr;
      const auto *exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>( base + dir.VirtualAddress );
      const auto *names = reinterpret_cast<const DWORD *>( base + exp->AddressOfNames );
      const auto *ordinals = reinterpret_cast<const WORD *>( base + exp->AddressOfNameOrdinals );
      const auto *functions = reinterpret_cast<const DWORD *>( base + exp->AddressOfFunctions );
      DWORD best = 0;
      const char *bestName = nullptr;
      for ( DWORD i = 0; i < exp->NumberOfNames; ++i )
      {
        const WORD ordinal = ordinals[i];
        if ( ordinal >= exp->NumberOfFunctions )
          continue;
        const DWORD f = functions[ordinal];
        if ( f >= dir.VirtualAddress && f < dir.VirtualAddress + dir.Size )
          continue; // forwarder string, not code
        if ( f <= rva && f >= best )
        {
          best = f;
          bestName = reinterpret_cast<const char *>( base + names[i] );
        }
      }
      if ( bestName )
        *offset = rva - best;
      return bestName;
    }
#ifdef _MSC_VER
    __except ( EXCEPTION_EXECUTE_HANDLER )
    {
      return nullptr;
    }
#endif
  }

  // 挂起线程期间只做无分配、无锁的展开（RtlLookupFunctionEntry +
  // RtlVirtualUnwind），先收集 PC，恢复线程后再符号化——被挂起的线程
  // 可能正持有堆锁/loader lock。
  int captureFrames( HANDLE thread, DWORD64 *pcs, int capacity )
  {
    if ( ::SuspendThread( thread ) == static_cast<DWORD>( -1 ) )
      return -1;
    int n = 0;
    CONTEXT ctx;
    ::ZeroMemory( &ctx, sizeof( ctx ) );
    ctx.ContextFlags = CONTEXT_FULL;
#ifdef _MSC_VER
    __try
#endif
    {
      if ( ::GetThreadContext( thread, &ctx ) )
      {
#if defined( _M_X64 ) || defined( __x86_64__ )
        while ( n < capacity && ctx.Rip != 0 )
        {
          pcs[n++] = ctx.Rip;
          DWORD64 imageBase = 0;
          PRUNTIME_FUNCTION fn = ::RtlLookupFunctionEntry( ctx.Rip, &imageBase, nullptr );
          if ( fn )
          {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            ::RtlVirtualUnwind( UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData,
                                &establisher, nullptr );
          }
          else
          {
            if ( ctx.Rsp == 0 )
              break;
            ctx.Rip = *reinterpret_cast<const DWORD64 *>( ctx.Rsp );
            ctx.Rsp += 8;
          }
        }
#elif defined( _M_ARM64 )
        if ( capacity > 0 )
          pcs[n++] = ctx.Pc;
#endif
      }
    }
#ifdef _MSC_VER
    __except ( EXCEPTION_EXECUTE_HANDLER )
    {
    }
#endif
    ::ResumeThread( thread );
    return n;
  }

  std::string describePc( const std::vector<Module> &modules, DWORD64 pc )
  {
    char buf[64];
    for ( const Module &m : modules )
    {
      if ( pc < m.base || pc >= m.base + m.size )
        continue;
      const std::uintptr_t rva = static_cast<std::uintptr_t>( pc - m.base );
      std::uintptr_t offset = 0;
      if ( const char *name = nearestExport( m.base, rva, &offset ) )
      {
        std::snprintf( buf, sizeof( buf ), "+0x%llx", static_cast<unsigned long long>( offset ) );
        return m.name + "!" + name + buf;
      }
      std::snprintf( buf, sizeof( buf ), "+0x%llx", static_cast<unsigned long long>( rva ) );
      return m.name + buf;
    }
    std::snprintf( buf, sizeof( buf ), "0x%llx", static_cast<unsigned long long>( pc ) );
    return buf;
  }

  // GetThreadDescription 只在 Windows 10 1607+ 的 kernel32 里有：在主线程
  // 启动看门狗时按名解析一次（挂死后再走 GetProcAddress 可能撞上被占的
  // loader lock），缺失时不带线程名。
  using GetThreadDescriptionFn = HRESULT( WINAPI * )( HANDLE, PWSTR * );
  GetThreadDescriptionFn resolveGetThreadDescription()
  {
    const HMODULE kernel = ::GetModuleHandleW( L"kernel32.dll" );
    return kernel ? reinterpret_cast<GetThreadDescriptionFn>(
                        reinterpret_cast<void *>( ::GetProcAddress( kernel, "GetThreadDescription" ) ) )
                  : nullptr;
  }

  std::string threadDescription( GetThreadDescriptionFn getDescription, HANDLE thread )
  {
    PWSTR text = nullptr;
    if ( !getDescription || FAILED( getDescription( thread, &text ) ) || !text )
      return {};
    std::string out = narrow( text, static_cast<int>( ::wcslen( text ) ) );
    ::LocalFree( text );
    return out;
  }

  bool traceEnabled()
  {
    const char *v = std::getenv( "PALEO_QGIS_INIT_TRACE" );
    return v && *v;
  }
} // namespace

struct QgisInitWatchdog::Impl
{
  std::mutex mutex;
  std::condition_variable wake;
  bool stop = false;
  std::string step;
  std::chrono::steady_clock::time_point since;
  std::chrono::steady_clock::duration nextDump {};
  std::vector<Module> baseline;
  DWORD mainThreadId = ::GetCurrentThreadId();
  GetThreadDescriptionFn getDescription = resolveGetThreadDescription();
  std::thread worker;

  void run()
  {
    std::unique_lock<std::mutex> lock( mutex );
    while ( !stop )
    {
      wake.wait_for( lock, std::chrono::seconds( 1 ) );
      if ( stop || step.empty() )
        continue;
      const auto elapsed = std::chrono::steady_clock::now() - since;
      if ( elapsed < nextDump )
        continue;
      nextDump += kDumpEvery;
      const std::string current = step;
      const std::vector<Module> before = baseline;
      lock.unlock();
      dump( current, std::chrono::duration_cast<std::chrono::seconds>( elapsed ).count(), before );
      lock.lock();
    }
  }

  void dump( const std::string &current, long long seconds, const std::vector<Module> &before )
  {
    const std::vector<Module> modules = snapshotModules();
    std::string out = "[qgisruntime] watchdog: still in \"" + current + "\" after "
                      + std::to_string( seconds ) + " s\n";
    std::string loaded;
    int loadedCount = 0;
    for ( const Module &m : modules )
    {
      const bool known = std::any_of( before.begin(), before.end(),
                                      [&m]( const Module &b ) { return b.base == m.base && b.name == m.name; } );
      if ( known )
        continue;
      loaded += ' ' + m.name;
      ++loadedCount;
    }
    out += "[qgisruntime]   modules loaded during step (" + std::to_string( loadedCount ) + "):"
           + loaded + "\n";

    std::vector<DWORD> threads;
    const HANDLE snap = ::CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if ( snap != INVALID_HANDLE_VALUE )
    {
      THREADENTRY32 entry {};
      entry.dwSize = sizeof( entry );
      const DWORD pid = ::GetCurrentProcessId();
      const DWORD self = ::GetCurrentThreadId();
      for ( BOOL ok = ::Thread32First( snap, &entry ); ok; ok = ::Thread32Next( snap, &entry ) )
        if ( entry.th32OwnerProcessID == pid && entry.th32ThreadID != self )
          threads.push_back( entry.th32ThreadID );
      ::CloseHandle( snap );
    }
    std::stable_partition( threads.begin(), threads.end(),
                           [this]( DWORD id ) { return id == mainThreadId; } );
    out += "[qgisruntime]   threads: " + std::to_string( threads.size() ) + "\n";
    if ( threads.size() > static_cast<size_t>( kMaxThreads ) )
      threads.resize( kMaxThreads );

    DWORD64 pcs[kMaxFrames];
    for ( DWORD id : threads )
    {
      const HANDLE thread = ::OpenThread( THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                          FALSE, id );
      if ( !thread )
        continue;
      const std::string description = threadDescription( getDescription, thread );
      const int n = captureFrames( thread, pcs, kMaxFrames );
      ::CloseHandle( thread );
      out += "[qgisruntime]   thread " + std::to_string( id ) + ( id == mainThreadId ? " (main)" : "" );
      if ( !description.empty() )
        out += " \"" + description + "\"";
      out += n < 0 ? " <suspend failed>\n" : ":\n";
      for ( int i = 0; i < n; ++i )
        out += "[qgisruntime]     #" + std::to_string( i ) + ' ' + describePc( modules, pcs[i] ) + "\n";
    }
    std::fputs( out.c_str(), stderr );
    std::fflush( stderr );
  }
};

QgisInitWatchdog::QgisInitWatchdog()
{
  if ( !traceEnabled() )
    return;
  d = std::make_unique<Impl>();
  d->worker = std::thread( [impl = d.get()] { impl->run(); } );
}

QgisInitWatchdog::~QgisInitWatchdog()
{
  if ( !d )
    return;
  {
    std::lock_guard<std::mutex> lock( d->mutex );
    d->stop = true;
  }
  d->wake.notify_all();
  if ( d->worker.joinable() )
    d->worker.join();
}

void QgisInitWatchdog::enter( const char *step )
{
  if ( !d )
    return;
  std::vector<Module> modules = snapshotModules();
  std::lock_guard<std::mutex> lock( d->mutex );
  d->step = step ? step : "";
  d->since = std::chrono::steady_clock::now();
  d->nextDump = kFirstDump;
  d->baseline = std::move( modules );
}

void QgisInitWatchdog::leave()
{
  if ( !d )
    return;
  std::lock_guard<std::mutex> lock( d->mutex );
  d->step.clear();
}

#else // !_WIN32

struct QgisInitWatchdog::Impl
{
};

QgisInitWatchdog::QgisInitWatchdog() = default;
QgisInitWatchdog::~QgisInitWatchdog() = default;
void QgisInitWatchdog::enter( const char * ) {}
void QgisInitWatchdog::leave() {}

#endif
