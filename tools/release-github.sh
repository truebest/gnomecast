#!/usr/bin/env bash
set -euo pipefail

# Publishes the local `main` tree to the GitHub release remote as ONE new release commit
# on top of the previous release commit. Development history stays only in the Bitbucket
# repo (origin); the GitHub history is an append-only chain of release snapshots, one
# commit per version. GitHub history is NEVER rewritten: pushes are fast-forward only and
# existing tags are never moved.
#
# Usage:            tools/release-github.sh <version>        e.g. tools/release-github.sh 0.1.2
# Remote override:  RELEASE_REMOTE=<name> (default: github)
# Branch override:  RELEASE_BRANCH=<name> (default: main)

version="${1:?usage: tools/release-github.sh <version>   e.g. 0.1.2}"
remote="${RELEASE_REMOTE:-github}"
branch="${RELEASE_BRANCH:-main}"
tag="v${version}"

fail() {
  echo "release-github: $*" >&2
  exit 2
}

git diff --quiet && git diff --cached --quiet || fail "working tree is dirty; commit or stash first"
git rev-parse --verify main >/dev/null 2>&1 || fail "local main branch not found"
git remote get-url "$remote" >/dev/null 2>&1 || fail "remote '$remote' is not configured"
git rev-parse --verify --quiet "refs/tags/$tag" >/dev/null && fail "tag $tag already exists locally; bump the version"

tree="$(git rev-parse "main^{tree}")"

parent_args=()
if git fetch --quiet "$remote" "$branch" 2>/dev/null; then
  parent="$(git rev-parse FETCH_HEAD)"
  parent_args=(-p "$parent")
  [ "$(git rev-parse "$parent^{tree}")" = "$tree" ] && fail "tree is identical to the latest published release; nothing to release"
fi

commit="$(git commit-tree "$tree" "${parent_args[@]}" -m "gnomecast ${version}")"

git tag "$tag" "$commit" >/dev/null
git push "$remote" "$commit:refs/heads/$branch" "refs/tags/$tag"
echo "release-github: published release commit $commit as $remote/$branch ($tag)"
