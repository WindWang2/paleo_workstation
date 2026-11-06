#!/usr/bin/env python3
"""试点纯核不得依赖 UI、Catalog、Workflow 或 QGIS。"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = [
    ROOT / "src/algorithms/depthbatch.h",
    ROOT / "src/algorithms/depthbatch.cpp",
    ROOT / "src/algorithms/seisattrgroup.h",
    ROOT / "src/algorithms/seisattrgroup.cpp",
]
BANNED = (
    "QtWidgets",
    "QWidget",
    "Qgs",
    "ui/",
    "catalog/",
    "workflow/",
    "DataCatalog",
    "qgis/",
)


def main() -> int:
    failed = False
    for path in FILES:
        if not path.is_file():
            print(f"missing {path}")
            failed = True
            continue
        text = path.read_text(encoding="utf-8")
        head = "\n".join(text.splitlines()[:3])
        if "层：数据" not in head:
            print(f"{path}: missing data-layer marker")
            failed = True
        for token in BANNED:
            if token in text:
                print(f"{path}: banned token {token}")
                failed = True
    cmake = (ROOT / "cmake/paleo-modules-data.cmake").read_text(encoding="utf-8")
    start = cmake.find("add_library(paleo_algorithms STATIC")
    end = cmake.find("target_link_libraries(paleo_algorithms", start)
    link = cmake[end:cmake.find("\n", end)]
    for token in ("paleo_ui", "paleo_store", "paleo_workflow"):
        if token in link:
            print(f"paleo_algorithms links {token}")
            failed = True
    if failed:
        return 1
    print("atomic kernel boundary: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
