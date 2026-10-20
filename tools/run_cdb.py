import subprocess, os

cdb = r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
deps = r'C:\Users\wangj.KEVIN\paleo-qgis-deps'
qgis = r'C:\Users\wangj.KEVIN\paleo-qgis-prefix'
qtpdf = r'C:\deps\Qt\6.11.2\msvc2022_64'

env = os.environ.copy()
env['PATH'] = f"{os.path.join(deps, 'Library', 'bin')};{os.path.join(qgis, 'bin')};{os.path.join(qtpdf, 'bin')};{deps};{env['PATH']}"
env['QT_PLUGIN_PATH'] = os.path.join(deps, 'Library', 'plugins')

exe = r'c:\Users\wangj.KEVIN\projects\paleo_workstation\build\paleo.exe'
# Set breakpoints at dllmain_crt_dispatch, DllMain, and their returns
script = '''
bp gdi32full!dllmain_crt_dispatch ".echo [dllmain_crt_dispatch enter]; g"
bp gdi32full!dllmain_crt_dispatch+0x30 ".echo [dllmain_crt_dispatch ret eax=]; r eax; g"
bp gdi32full!DllMain ".echo [DllMain enter]; g"
bp gdi32full!DllMain+0x20 ".echo [DllMain near ret]; g"
bp gdi32full!GdiDllInitialize ".echo [GdiDllInitialize]; g"
g
'''
cmd = [cdb, '-c', script + '; q', exe]
res = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
print(res.stdout)
