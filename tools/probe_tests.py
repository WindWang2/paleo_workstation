import glob, os, subprocess

deps = r'C:\Users\wangj.KEVIN\paleo-qgis-deps'
qgis = r'C:\Users\wangj.KEVIN\paleo-qgis-prefix'
qtpdf = r'C:\deps\Qt\6.11.2\msvc2022_64'

env = os.environ.copy()
env['PATH'] = f"{os.path.join(deps, 'Library', 'bin')};{os.path.join(qgis, 'bin')};{os.path.join(qtpdf, 'bin')};{deps};{env['PATH']}"
env['QT_PLUGIN_PATH'] = os.path.join(deps, 'Library', 'plugins')
env['QT_QPA_PLATFORM'] = 'offscreen'

exes = glob.glob(r'build\tst_*.exe')[:30]
for e in exes:
    name = os.path.basename(e)
    try:
        res = subprocess.run([e, '-help'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        print(f'{name}: {res.returncode}')
    except Exception as ex:
        print(f'{name}: EXCEPTION {ex}')
