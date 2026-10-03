#!/usr/bin/env bash
# CI：安装 QGIS 4.2（qgis.org resolute 源）+ Qt6 开发包，QGIS 版本钉死（#138）。
#
# 版本单一来源 = vendor/deb-closure.lock（ET1 deb 闭包锁，lock-debs.py 生成）：
# 从其中 qgis-providers 条目解析出 epoch:version（如 1:4.2.3+44resolute），
# 写 apt preferences 把 qgis*/libqgis*/python3-qgis* 钉到该版本。
# 升级 QGIS = 重跑 vendor/lock-debs.py 更新锁文件（有意变更），CI 自动跟随。
#
# qgis.org 的 Packages 索引通常只保留最新版：若钉的版本已从索引下架，默认
# ::warning 并回退到索引当前版本（CI 不因上游发版整体变红，日志留痕）；
# QGIS_PIN_STRICT=1 时直接失败。
#
# 注意：superbuild 路（vendor/superbuild，带 labelsWithLayer 补丁）声明的是
# 4.2.2；apt/deb 闭包是 4.2.3 原生包，没有该补丁——tst_labelzorder 的
# labelsStackWithLayer* 在 apt 路上 SKIP，见 #138。
#
# 用法：tools/ci_apt_qgis.sh [额外 apt 包...]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
lock="$here/vendor/deb-closure.lock"

ver="$(grep -oE 'qgis-providers_[0-9]+%3a[^_ ]+_amd64\.deb' "$lock" | head -1 |
       sed -E 's/^qgis-providers_([0-9]+)%3a([^_]+)_amd64\.deb$/\1:\2/')"
if [ -z "$ver" ]; then
  echo "::error::无法从 $lock 解析 QGIS 版本" >&2
  exit 1
fi
echo "QGIS apt pin (from deb-closure.lock): $ver"

sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg
sudo install -d -m 755 /etc/apt/keyrings
sudo curl -fsSL https://download.qgis.org/downloads/qgis-archive-keyring.gpg \
  -o /etc/apt/keyrings/qgis-archive-keyring.gpg
printf 'Types: deb\nURIs: https://qgis.org/debian\nSuites: resolute\nArchitectures: amd64\nComponents: main\nSigned-By: /etc/apt/keyrings/qgis-archive-keyring.gpg\n' |
  sudo tee /etc/apt/sources.list.d/qgis.sources >/dev/null
sudo apt-get update

avail="$(apt-cache madison qgis-providers | awk -F'|' '{gsub(/ /,"",$2); print $2}' | paste -sd' ' -)"
if tr ' ' '\n' <<<"$avail" | grep -xF -- "$ver" >/dev/null; then
  printf 'Package: qgis qgis-* libqgis-* python3-qgis*\nPin: version %s\nPin-Priority: 1001\n' "$ver" |
    sudo tee /etc/apt/preferences.d/paleo-qgis-pin >/dev/null
else
  msg="钉的 QGIS $ver 不在 qgis.org 索引中（可用：${avail:-无}）——请用 vendor/lock-debs.py 更新 deb-closure.lock"
  if [ "${QGIS_PIN_STRICT:-0}" = "1" ]; then
    echo "::error::$msg" >&2
    exit 1
  fi
  echo "::warning::$msg；本次回退到索引当前版本"
fi

sudo apt-get install -y \
  qgis qgis-providers libqgis-dev \
  qt6-base-dev qt6-svg-dev qt6-serialport-dev qt6-positioning-dev qt6-tools-dev \
  qt6-webengine-dev qt6-pdf-dev libgdal-dev "$@"

echo "== installed QGIS packages =="
dpkg-query -W -f='${Package} ${Version}\n' 'qgis' 'qgis-providers' 'libqgis-dev' 'libqgis-core*' 2>/dev/null || true
