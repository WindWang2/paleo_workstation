#!/usr/bin/env python3
# vendor/basemap/fetch-tiles.py — 离线底图瓦片抓取 + MBTiles 打包。
#
# 从 Esri 公开瓦片服务（免密钥）按范围/层级抓 XYZ 瓦片到本地暂存树
#（vendor/basemap/tiles/<svc>/<z>/<x>/<y>.jpg，可断点续传：已存在即跳过），
# 然后合并打包为 QGIS/GDAL 可直接离线读取的 MBTiles（单文件、TMS y 翻转）。
#
# 服务与范围计划（改 JOBS 即可重跑，新增区域/层级只补抓增量）：
#   · esri_topo       World_Topo_Map（地形+道路+地名）  中国 z2-8 + 鄂尔多斯 demo 区 z9-11
#   · esri_hillshade  World_Hillshade（纯地形晕渲）      中国 z2-7
#
# 用法：
#   python3 vendor/basemap/fetch-tiles.py --dry-run   # 只打印瓦片数
#   python3 vendor/basemap/fetch-tiles.py             # 抓取 + 打包
#
# 瓦片格式实测全部为 JPEG（magic ffd8），metadata format=jpeg。
# 版权：Esri 服务条款要求署名随图显示（attribution 已写入 MBTiles metadata，
# 加载方需把该字符串渲染到画布角标——见 README）。
import concurrent.futures as cf
import math
import os
import sqlite3
import sys
import time
import urllib.request

BASE = os.path.dirname(os.path.abspath(__file__))
STAGE = os.path.join(BASE, "tiles")
UA = "paleo-workstation-basemap-fetch/1.0"
WORKERS = 8
TIMEOUT = 30
RETRIES = 3

SERVICES = {
    "esri_topo": {
        # 注意 Esri 瓦片 URL 是 {z}/{y}/{x} 顺序
        "url": "https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}",
        "mbtiles": "basemap_topo.mbtiles",
        "title": "Esri World Topo (offline)",
        "attribution": "Sources: Esri, HERE, Garmin, USGS, NGA, EPA, USDA, NPS",
    },
    "esri_hillshade": {
        "url": "https://server.arcgisonline.com/ArcGIS/rest/services/Elevation/World_Hillshade/MapServer/tile/{z}/{y}/{x}",
        "mbtiles": "basemap_hillshade.mbtiles",
        "title": "Esri World Hillshade (offline)",
        "attribution": "Esri, USGS, NGA, Earthstar Geographics",
    },
}

# (service, (west, south, east, north), zmin, zmax, 说明)
JOBS = [
    ("esri_topo", (73.0, 15.0, 135.5, 54.5), 2, 8, "中国 z2-8"),
    ("esri_topo", (104.0, 32.0, 112.0, 40.0), 9, 11, "鄂尔多斯 demo 区 z9-11"),
    ("esri_hillshade", (73.0, 15.0, 135.5, 54.5), 2, 7, "中国 z2-7"),
]


def tile_range(bbox, z):
    """经纬度范围 → 该层级的 (x0,x1,y0,y1)（y 从北向南递增，XYZ 约定）。"""
    n = 1 << z
    w, s, e, nn = bbox
    x0 = max(0, min(n - 1, math.floor((w + 180.0) / 360.0 * n)))
    x1 = max(0, min(n - 1, math.floor((e + 180.0) / 360.0 * n)))
    y0 = max(0, min(n - 1, math.floor((1 - math.asinh(math.tan(math.radians(nn))) / math.pi) / 2 * n)))
    y1 = max(0, min(n - 1, math.floor((1 - math.asinh(math.tan(math.radians(s))) / math.pi) / 2 * n)))
    return x0, x1, y0, y1


def fetch_one(url, path):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return "skip"
    for attempt in range(RETRIES):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
                data = r.read()
            if not data or data[:2] != b"\xff\xd8":
                raise IOError(f"not jpeg ({len(data)}B)")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".part"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, path)
            return "ok"
        except Exception as ex:
            if attempt == RETRIES - 1:
                print(f"FAIL {url}: {ex}", file=sys.stderr)
                return "fail"
            time.sleep(1.5 * (attempt + 1))


def build_tasks():
    tasks = []  # (job序号, url, path)
    for ji, (svc, bbox, zmin, zmax, _desc) in enumerate(JOBS):
        tpl = SERVICES[svc]["url"]
        for z in range(zmin, zmax + 1):
            x0, x1, y0, y1 = tile_range(bbox, z)
            for x in range(x0, x1 + 1):
                for y in range(y0, y1 + 1):
                    url = tpl.format(z=z, x=x, y=y)
                    path = os.path.join(STAGE, svc, str(z), str(x), f"{y}.jpg")
                    tasks.append((ji, url, path))
    return tasks


def pack(service):
    """把暂存树打包为 MBTiles（tile_row 取 TMS 翻转 = 2^z-1-y）。"""
    stage = os.path.join(STAGE, service)
    out = os.path.join(BASE, SERVICES[service]["mbtiles"])
    boxes = [b for s, b, *_ in JOBS if s == service]
    bounds = [min(b[0] for b in boxes), min(b[1] for b in boxes),
              max(b[2] for b in boxes), max(b[3] for b in boxes)]
    zooms = sorted(int(d) for d in os.listdir(stage) if d.isdigit()) if os.path.isdir(stage) else []
    if os.path.exists(out):
        os.remove(out)
    db = sqlite3.connect(out)
    cur = db.cursor()
    cur.executescript(
        "CREATE TABLE metadata (name TEXT, value TEXT);"
        "CREATE TABLE tiles (zoom_level INTEGER, tile_column INTEGER, "
        "tile_row INTEGER, tile_data BLOB, PRIMARY KEY "
        "(zoom_level, tile_column, tile_row));")
    meta = {
        "name": SERVICES[service]["title"],
        "format": "jpeg",
        "type": "baselayer",
        "bounds": ",".join(f"{v:.4f}" for v in bounds),
        "minzoom": str(zooms[0]) if zooms else "0",
        "maxzoom": str(zooms[-1]) if zooms else "0",
        "attribution": SERVICES[service]["attribution"],
        "description": "Offline tile cache fetched by vendor/basemap/fetch-tiles.py",
    }
    cur.executemany("INSERT INTO metadata VALUES (?,?)", meta.items())
    n = 0
    for z in zooms:
        flip = (1 << z) - 1
        for xstr in os.listdir(os.path.join(stage, str(z))):
            for fname in os.listdir(os.path.join(stage, str(z), xstr)):
                if not fname.endswith(".jpg"):
                    continue
                y = int(fname[:-4])
                with open(os.path.join(stage, str(z), xstr, fname), "rb") as f:
                    cur.execute("INSERT OR REPLACE INTO tiles VALUES (?,?,?,?)",
                                (int(z), int(xstr), flip - y, sqlite3.Binary(f.read())))
                n += 1
    db.commit()
    db.close()
    return n, out


def main():
    dry = "--dry-run" in sys.argv
    tasks = build_tasks()
    for ji, (svc, bbox, zmin, zmax, desc) in enumerate(JOBS):
        cnt = sum(1 for j, *_ in tasks if j == ji)
        print(f"  {svc:16} {desc:28} -> {cnt} tiles")
    print(f"total: {len(tasks)} tiles")
    if dry:
        return
    t0 = time.time()
    done = fail = skip = 0
    with cf.ThreadPoolExecutor(max_workers=WORKERS) as ex:
        for r in ex.map(lambda t: fetch_one(*t[1:]), tasks):
            done += 1
            if r == "fail":
                fail += 1
            elif r == "skip":
                skip += 1
            if done % 200 == 0:
                print(f"  {done}/{len(tasks)} ({fail} failed, {skip} skipped) {time.time()-t0:.0f}s", flush=True)
    print(f"fetch done: {len(tasks)-fail}/{len(tasks)} ok ({fail} failed) in {time.time()-t0:.0f}s")
    for svc in SERVICES:
        n, out = pack(svc)
        print(f"packed {svc}: {n} tiles -> {out} ({os.path.getsize(out)/1e6:.1f} MB)")
    sys.exit(1 if fail > len(tasks) // 100 else 0)


if __name__ == "__main__":
    main()
