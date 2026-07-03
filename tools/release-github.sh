#!/usr/bin/env bash
set -euo pipefail

# Publishes a single-commit, history-free snapshot of the local `main` tree to the
# GitHub release remote. Development history stays only in the Bitbucket repo (origin).
#
# Usage:            tools/release-github.sh <version>        e.g. tools/release-github.sh 0.1.0
# Remote override:  RELEASE_REMOTE=<name> (default: github)
# Branch override:  RELEASE_BRANCH=<name> (default: main)
#
# Every release rewrites the remote branch with a fresh parentless commit, so the GitHub
# repository always contains exactly one commit per its main branch plus release tags.

version="${1:?usage: tools/release-github.sh <version>   e.g. 0.1.0}"
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

tree="$(git rev-parse "main^{tree}")"
commit="$(git commit-tree "$tree" -m "gnomecast ${version}")"

git tag --force "$tag" "$commit" >/dev/null
git push --force "$remote" "$commit:refs/heads/$branch" "refs/tags/$tag"
echo "release-github: pushed history-free snapshot $commit as $remote/$branch ($tag)"
