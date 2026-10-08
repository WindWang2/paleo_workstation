// 测试进程级 Windows 注册表沙箱（方向 81 QSettings 决策落地，见
// .goal-loop-ledger-env-debt2.md「QSettings 三选一」）。
//
// 问题：add_paleo_test 的 XDG/HOME 沙箱在 Linux 隔离了 QSettings（NativeFormat
// = XDG 下的 ini），Windows 上 NativeFormat 走注册表 HKCU\Software\paleo\paleo，
// 不受任何环境变量控制——测试读写的是开发者真实的应用配置：
//   · tst_ui/tst_panels/tst_wellsection_ui/tst_stratigraphicweb 开局
//     QSettings("paleo","paleo").clear()——每跑一次就清空本机真实配置；
//   · 本机真实配置/策略残留让 7 个 UI 测试只在本机红、CI 干净 runner 绿。
// 对策（产品零改动、注册表语义保留）：测试可执行文件静态初始化期（早于
// main/QCoreApplication/任何 QSettings）用 RegLoadAppKey 挂一个私有 app hive
// （文件在 ctest-home/<test>/ 树内，进程私有、无需特权、他进程不可见），再以
// RegOverridePredefKey 把本进程的 HKEY_CURRENT_USER 重映射到它。Qt 的
// QWinSettingsPrivate 用 HKEY_CURRENT_USER 句柄打开 Software\<org>\<app>，
// 于是读写全部落进沙箱 hive；子进程不继承重映射（各自真实 HKCU）。
//   · hive 路径：环境变量 PALEO_TEST_REGISTRY_HIVE（paleo_test_sandbox 为每个
//     ctest 项注入 <build>/ctest-home/<test>/registry.hiv）；未设置（直跑）时
//     落 %TEMP%\paleo-test-registry\<exe>.hiv；设为 native/off 则不启用。
//   · 每次运行先删 hive 文件——从空白开始，与 Linux 沙箱「干净状态」前提一致。
//   · 种子：把真实 HKCU 的 Explorer\User Shell Folders / Shell Folders、每用户
//     字体注册、Control Panel\International / Desktop 拷进沙箱——known folder
//     （QStandardPaths 的 AppData 等）、字体与区域解析结果不变。
//   · 直跑时 app hive 挂载/重映射失败才退到真实 HKCU 下的专用键
//     Software\paleo-test-sandbox\<exe>（运行前后各整树删除；仍不碰 Software\paleo）。
//   · 生效标志：环境变量 PALEO_TEST_REGISTRY_SANDBOX_ACTIVE=<hive 路径|hkcu-key:...>
//     ——tst_test_sandbox 据此与 HKEY_USERS\<SID>（真实用户 hive）双向断言。
//   · 同一测试拉起的自身子进程（如 tst_metastore 的死 pid 夹具）继承同一 hive
//     路径：删除因占用失败、按共享方式挂同一 hive——不影响父进程。
// 非 Windows 平台本文件为空 TU。
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace
{

std::wstring envVar( const wchar_t *name )
{
  const DWORD n = ::GetEnvironmentVariableW( name, nullptr, 0 );
  if ( n == 0 )
    return {};
  std::wstring v( n, L'\0' );
  const DWORD m = ::GetEnvironmentVariableW( name, &v[0], n );
  v.resize( m );
  return v;
}

std::wstring exeBaseName()
{
  wchar_t exe[MAX_PATH];
  const DWORD e = ::GetModuleFileNameW( nullptr, exe, MAX_PATH );
  if ( e == 0 || e >= MAX_PATH )
    return L"unknown";
  std::wstring base( exe, e );
  const size_t slash = base.find_last_of( L"\\/" );
  if ( slash != std::wstring::npos )
    base = base.substr( slash + 1 );
  const size_t dot = base.find_last_of( L'.' );
  if ( dot != std::wstring::npos )
    base = base.substr( 0, dot );
  return base;
}

std::wstring defaultHivePath()
{
  wchar_t tmp[MAX_PATH + 1];
  const DWORD n = ::GetTempPathW( MAX_PATH + 1, tmp );
  if ( n == 0 || n > MAX_PATH )
    return {};
  std::wstring dir = std::wstring( tmp, n ) + L"paleo-test-registry";
  ::CreateDirectoryW( dir.c_str(), nullptr );
  return dir + L"\\" + exeBaseName() + L".hiv";
}

void deleteHiveFiles( const std::wstring &hive )
{
  for ( const wchar_t *suffix : { L"", L".LOG", L".LOG1", L".LOG2" } )
    ::DeleteFileW( ( hive + suffix ).c_str() );
}

void seedKey( HKEY sandboxRoot, const wchar_t *subKey )
{
  HKEY src = nullptr;
  if ( ::RegOpenKeyExW( HKEY_CURRENT_USER, subKey, 0, KEY_READ, &src ) != ERROR_SUCCESS )
    return;
  HKEY dst = nullptr;
  if ( ::RegCreateKeyExW( sandboxRoot, subKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &dst,
                          nullptr ) == ERROR_SUCCESS )
  {
    ::RegCopyTreeW( src, nullptr, dst );
    ::RegCloseKey( dst );
  }
  ::RegCloseKey( src );
}

// 种子 + 重映射；任一步失败返回 false（调用方关句柄、走下一档）。
bool activate( HKEY root )
{
  // 只读种子：进程内可能经 HKCU 读取的「机器外观」类键（known folder 路径、
  // 每用户字体注册、区域格式、桌面 DPI/字体平滑），保持与真实用户一致——
  // 沙箱只隔离应用配置，不改变字体/区域/路径解析。
  for ( const wchar_t *subKey : {
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
          L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
          L"Control Panel\\International",
          L"Control Panel\\Desktop",
        } )
    seedKey( root, subKey );
  const LSTATUS rc = ::RegOverridePredefKey( HKEY_CURRENT_USER, root );
  if ( rc != ERROR_SUCCESS )
    std::fwprintf( stderr, L"[paleo-test-sandbox] RegOverridePredefKey failed: %ld\n", long( rc ) );
  return rc == ERROR_SUCCESS;
}

struct RegistrySandbox
{
  HKEY root = nullptr;
  std::wstring fallbackKey; // 第二档生效时的 HKCU 子键（退出时删除）

  RegistrySandbox()
  {
    std::wstring hive = envVar( L"PALEO_TEST_REGISTRY_HIVE" );
    if ( hive == L"native" || hive == L"off" )
      return;
    ::SetEnvironmentVariableW( L"PALEO_TEST_REGISTRY_SANDBOX_ACTIVE", nullptr );
    const bool requiredHive = !hive.empty(); // ctest 显式路径必须第一档成功
    if ( hive.empty() )
      hive = defaultHivePath();
    for ( wchar_t &c : hive )
      if ( c == L'/' )
        c = L'\\';

    // 第一档：树内私有 app hive。
    if ( !hive.empty() )
    {
      deleteHiveFiles( hive );
      const LSTATUS rc = ::RegLoadAppKeyW( hive.c_str(), &root, KEY_ALL_ACCESS, 0, 0 );
      if ( rc == ERROR_SUCCESS && activate( root ) )
      {
        ::SetEnvironmentVariableW( L"PALEO_TEST_REGISTRY_SANDBOX_ACTIVE", hive.c_str() );
        return;
      }
      if ( rc != ERROR_SUCCESS )
        std::fwprintf( stderr, L"[paleo-test-sandbox] RegLoadAppKey(%ls) failed: %ld\n",
                       hive.c_str(), long( rc ) );
      if ( rc == ERROR_SUCCESS )
        ::RegCloseKey( root );
      root = nullptr;
    }
    if ( requiredHive )
    {
      std::fwprintf( stderr, L"[paleo-test-sandbox] required app hive unavailable — stopping before QSettings\n" );
      ::ExitProcess( ERROR_ACCESS_DENIED );
    }

    // 第二档：真实 HKCU 下的专用键 Software\paleo-test-sandbox\<exe>——每次运行
    // 先整树删除再建（不能用 REG_OPTION_VOLATILE：Qt 以非 volatile 方式建子键，
    // volatile 父键下会 ERROR_CHILD_MUST_BE_VOLATILE）。仍不碰 Software\paleo。
    const std::wstring key = L"Software\\paleo-test-sandbox\\" + exeBaseName();
    ::RegDeleteTreeW( HKEY_CURRENT_USER, key.c_str() );
    if ( ::RegCreateKeyExW( HKEY_CURRENT_USER, key.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                            KEY_ALL_ACCESS, nullptr, &root, nullptr ) == ERROR_SUCCESS &&
         activate( root ) )
    {
      fallbackKey = key;
      const std::wstring active = L"hkcu-key:HKCU\\" + key;
      ::SetEnvironmentVariableW( L"PALEO_TEST_REGISTRY_SANDBOX_ACTIVE", active.c_str() );
      return;
    }
    if ( root )
      ::RegCloseKey( root );
    root = nullptr;
    std::fwprintf( stderr, L"[paleo-test-sandbox] no registry sandbox — stopping before QSettings\n" );
    ::ExitProcess( ERROR_ACCESS_DENIED );
  }

  ~RegistrySandbox()
  {
    if ( !root )
      return;
    ::RegOverridePredefKey( HKEY_CURRENT_USER, nullptr );
    ::RegCloseKey( root );
    if ( !fallbackKey.empty() )
      ::RegDeleteTreeW( HKEY_CURRENT_USER, fallbackKey.c_str() );
  }
};

// 静态初始化：先于 main()。同一可执行文件内其他静态对象不碰 QSettings。
RegistrySandbox g_paleoTestRegistrySandbox;

} // namespace
#endif // _WIN32
