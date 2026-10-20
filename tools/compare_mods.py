import subprocess, os

cdb = r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
deps = r'C:\Users\wangj.KEVIN\paleo-qgis-deps'
qgis = r'C:\Users\wangj.KEVIN\paleo-qgis-prefix'
qtpdf = r'C:\deps\Qt\6.11.2\msvc2022_64'

env = os.environ.copy()
env['PATH'] = f"{os.path.join(deps, 'Library', 'bin')};{os.path.join(qgis, 'bin')};{os.path.join(qtpdf, 'bin')};{deps};{env['PATH']}"
env['QT_PLUGIN_PATH'] = os.path.join(deps, 'Library', 'plugins')

for name, target in [('assistant', os.path.join(deps, 'Library', 'bin', 'assistant.exe')),
                     ('paleo', r'c:\Users\wangj.KEVIN\projects\paleo_workstation\build\paleo.exe')]:
    cmd = [cdb, '-c', 'lm; q', target]
    res = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    mods = [line for line in res.stdout.splitlines() if line.startswith('0000')]
    print(f'=== {name} ({len(mods)} modules) ===')
    with open(f'{name}_mods.txt', 'w') as f:
        f.write('\n'.join(mods))
    print(f'Saved {name}_mods.txt')
