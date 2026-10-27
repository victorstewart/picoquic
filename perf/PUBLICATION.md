# Published performance commits

Publication date: 2026-10-27 (America/New_York).

The fork is `https://github.com/victorstewart/picoquic`. Its `master` was
fast-forwarded normally from `1d416567cc999aad379ad3d9c3b2b7dca66cb1ea` to
upstream `1d81a730ebbc0f4598f8127a7f43260a71a9ac7d` before creating
`codex/perf-wins-2026-10-27` from that exact fork revision. No force push was used.

The six experiment commits were reproduced with expanded messages. Each
published revision below has **exactly the same full Git tree** as its original
revision; only commit metadata/parent IDs changed. Both tree-ID equality and
`git diff --exit-code ORIGINAL PUBLISHED` were checked for every row. Therefore
existing build, benchmark and sanitizer evidence applies without rerunning it.
This publication record and report links are subsequent documentation changes.

| Original experiment revision | Published revision | Purpose |
| --- | --- | --- |
| `f08260017c1a137a263084a9a2e1590351efde18` | [4a4c05f426578b62ad603336733d30e6bcb43ac6](https://github.com/victorstewart/picoquic/commit/4a4c05f426578b62ad603336733d30e6bcb43ac6) | perf: record native Mac ordered-index A/B experiments |
| `7714e4c0767e06a46da5adbdb09177f5b2fce431` | [ca4eada264c1fe64f5d90d3ec07b1d27d8228cee](https://github.com/victorstewart/picoquic/commit/ca4eada264c1fe64f5d90d3ec07b1d27d8228cee) | perf: cache the minimum future connection wake time |
| `bc3793cbf481f13e6f424cfa70679f7b225c4df7` | [a99fef3ceda63002ce733ee3fadb4513da0fa1ec](https://github.com/victorstewart/picoquic/commit/a99fef3ceda63002ce733ee3fadb4513da0fa1ec) | perf: use an indexed heap for future connection wake times |
| `58ed1e6a33ce96e302c8fcecd3e2953b1e9105d4` | [2ec46fa9736d22c15ea7798a514069df7b913576](https://github.com/victorstewart/picoquic/commit/2ec46fa9736d22c15ea7798a514069df7b913576) | perf: record persistent workloads and cross-usage regression evidence |
| `a9d95cc524edd6d5b69576dc6796ae87e60bc8dc` | [137834078e30a901e8b0024da92f2e299467660f](https://github.com/victorstewart/picoquic/commit/137834078e30a901e8b0024da92f2e299467660f) | perf: append queued stream and TLS data through a tail pointer |
| `b00cc8d4ed54b2b0fa61764094c21b001952d7f2` | [837c170bfdd11db964d2bf5da6b31e722fa47718](https://github.com/victorstewart/picoquic/commit/837c170bfdd11db964d2bf5da6b31e722fa47718) | perf: initialize packet metadata without clearing payload storage |

The cached-minimum commit is a measured alternative preserved separately. The
following indexed-heap commit supersedes it. The final production changes are
the indexed heap, queue-tail pointer and metadata-only packet initialization.
Compact-layout screening results and the rejected token/in-place-heap patches
remain evidence only; they are not enabled in production.

Each optimization message includes its mechanism, exact baseline and named
workloads, environment, sample count, median costs, paired reductions and 95%
intervals, protocol controls, correctness tests, memory cost and limitations.
Later combined-build checks are labeled as such rather than attributed to an
isolated candidate. Invalid and inconclusive evidence remains explicit.

Historical reports, configurations and binary manifests retain original
experiment IDs. When reproducing from this published branch, substitute the
corresponding published revision above. The original `codex/ordered-index-perf`
branch remains intact locally. Upstream baseline IDs are unchanged. Reports
describe the completed measurement phases; this publication followed them.

Final source review confirmed no production, test or benchmark-source difference
from the tested original tip `b00cc8d4ed54b2b0fa61764094c21b001952d7f2`.
The finite regression matrix found no repeatable slowdown in its named usages;
it does not prove zero slowdown for every possible workload or platform.

See [original ordered-index results](RESULTS.md) and
[persistent/queue/pool results](followup/RESULTS.md) for raw-evidence links.
