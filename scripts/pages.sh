#!/bin/sh
# The GitHub Pages site, put on the gh-pages branch:
#
#   index.html   web/landing.html: links to the two below
#   app/         the app in the browser (make web's build-web/)
#   demo/        the demo (scripts/demo.sh's build-demo/)
#
#   scripts/pages.sh [demo dir]
#
# Builds the site in build-pages/ and makes it the one commit of gh-pages
# (no parent: each run replaces the last, so old maps and builds don't
# pile up in the history). Pushing is left to you:
#   git push -f origin gh-pages
# The app and the demo are in folders of their own so the app's service
# worker (registered for app/) never handles the demo's requests.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"
demo=${1:-build-demo}
[ -f build-web/prism.wasm ] || { echo "build the app first: make web" >&2; exit 1; }
[ -f "$demo/map/map.json" ] || { echo "no demo in $demo: scripts/demo.sh first" >&2; exit 1; }

repo=$(git remote get-url origin 2>/dev/null | sed 's/\.git$//; s|^git@github.com:|https://github.com/|')
out=build-pages
rm -rf "$out"
mkdir -p "$out"
cp -r build-web "$out/app"
cp -r "$demo" "$out/demo"
sed "s|@REPO@|$repo|" web/landing.html > "$out/index.html"
: > "$out/.nojekyll"

# the commit: build-pages as its tree, staged in an index of its own (the
# working tree's index is left alone)
src=$(git rev-parse --short HEAD)
dirty=$(git status --porcelain | grep -q . && echo " + uncommitted changes" || true)
index=$(mktemp)
trap 'rm -f "$index"' EXIT
rm -f "$index"
tree=$(cd "$out" && export GIT_DIR="$here/.git" GIT_WORK_TREE=. GIT_INDEX_FILE="$index" &&
       git add -A -f . && git write-tree)
commit=$(echo "Pages site from $src$dirty" | git commit-tree "$tree")
git update-ref refs/heads/gh-pages "$commit"
echo "gh-pages: $(git log --oneline -1 gh-pages) ($(du -sh "$out" | cut -f1))"
echo "push it with: git push -f origin gh-pages"
