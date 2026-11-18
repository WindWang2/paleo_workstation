#!/usr/bin/env bash
# Seed the source install prefix with the verified Debian Qt/development base.
set -euo pipefail

prefix=$1
base=$2
lock=$3
receipt=.paleo-superbuild-base.sha256
wanted=$(tr -d '\r' < "$lock" | sha256sum | cut -d' ' -f1)
if [ "$(cat "$base/.paleo-deb-closure.sha256" 2>/dev/null)" != "$wanted" ]; then
  echo "Missing or stale Debian 13 base. Run bash vendor/fetch-deps.sh --superbuild-base." >&2
  exit 1
fi

mkdir -p "$(dirname "$prefix")"
exec 9>"$base/.paleo-prefix-prepare.lock"
flock 9
if [ -e "$prefix" ] || [ -L "$prefix" ]; then
  if [ "$(cat "$prefix/$receipt" 2>/dev/null)" = "$wanted" ]; then
    exit 0
  fi
  echo "Source install prefix $prefix belongs to a different base. Use a fresh SUPERBUILD_PREFIX." >&2
  exit 1
fi

staging=$(mktemp -d "$prefix.new.XXXXXX")
trap 'rm -rf -- "$staging"' EXIT
cp -a --reflink=auto "$base/usr/." "$staging/"
mkdir -p "$staging/etc"
cp -a --reflink=auto "$base/etc/fonts" "$staging/etc/fonts"
sed -i 's#../../usr/share/fonts#../../share/fonts#' "$staging/etc/fonts/fonts.conf"

# Debian pc files use /usr. Make paths relative to the retained metadata so
# the source projects can consume Debian and source-built packages together.
while IFS= read -r -d '' file; do
  relative=$(realpath --relative-to="$(dirname "$file")" "$staging")
  origin='${pcfiledir}'/"$relative"
  sed -i -E "s#=/usr(/|$)#=$origin\\1#g; s#(-I|-L)/usr/#\\1$origin/#g" "$file"
done < <(find "$staging/lib" "$staging/share" -type f -name '*.pc' -print0)

# Debian's Expat export uses an absolute /lib path for the imported library.
# Preserve the exported target and point it at the matching copied archive.
find "$staging/lib/x86_64-linux-gnu/cmake" -path '*/expat-*/expat-noconfig.cmake' \
  -exec sed -i 's#"/lib/x86_64-linux-gnu/libexpat\.so#"${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libexpat.so#g' {} +

printf '%s\n' "$wanted" > "$staging/$receipt"
chmod "$(umask -S)" "$staging"
mv -- "$staging" "$prefix"
