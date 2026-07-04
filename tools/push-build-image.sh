#!/usr/bin/env bash
# Build and push the CI build-environment image consumed by bitbucket-pipelines.yml.
#
# The image is public and carries only public toolchains (see Dockerfile); no
# credentials are stored in the repo or the image — run `docker login` in your own
# session before pushing. After pushing, bump the pinned tag in bitbucket-pipelines.yml.
#
# Usage: tools/push-build-image.sh [tag]   (tag defaults to today's YYYYMMDD)
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="${GNOMECAST_BUILD_IMAGE:-cubicattache/gnomecast-webos-build}"
tag="${1:-$(date +%Y%m%d)}"

docker build -t "${image}:${tag}" -t "${image}:latest" "${repo_root}"
docker push "${image}:${tag}"
docker push "${image}:latest"

echo "push-build-image: pushed ${image}:${tag} and ${image}:latest"
echo "push-build-image: now pin image: ${image}:${tag} in bitbucket-pipelines.yml"
