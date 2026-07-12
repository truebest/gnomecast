# Documentation

The root [README](../README.md) is the product and architecture overview. The documents
in this directory have deliberately separate responsibilities:

- [Native webOS runbook](native-runbook.md) — current runtime controls, configuration,
  local checks, package/deploy procedures, device acceptance, logging, and triage.
- [Build environment](build-environment.md) — reproducible container and manual host
  setup. The repository `Dockerfile` and CI image remain the source of truth for tool
  versions.

Pinned dependency revisions and licenses live in
[third-party provenance](../third_party/PROVENANCE.md). The maintained IronRDP fork delta
is recorded separately in
[IronRDP provenance](../third_party/IronRDP/PROVENANCE.md).

Historical implementation plans and status snapshots do not belong in this directory.
Durable decisions should be folded into the relevant current-state document; unfinished
work should be kept only when it includes concrete invariants or acceptance criteria.
