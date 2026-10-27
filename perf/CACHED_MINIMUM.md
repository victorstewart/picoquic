# Cached-minimum commit proof

This commit applies `patches/cached-min-v2.patch` to the pinned upstream source.
It caches the first future connection, updating the cache on insertion and before
minimum removal. The existing future splay index and ready FIFO remain.

In 12 native Mac paired runs with separately allocated connections, query-heavy
scheduling at 10,000 connections used **48.14% less CPU** (95% bootstrap interval
47.87–48.44%). Random update/query improved 6.43% (5.74–6.69%). Ready-only was
inconclusive (+1.43%, -0.15–1.53%); protocol controls showed no material regression
or proven throughput gain. The context costs one additional pointer.

Proof: `results/cached-min-v2-separate.jsonl`, its summary/config, and protocol
cases in `results/cached-min-v2.jsonl`. Creation and the new real-connection
`wake_queue` test passed (`results/verification/cache-v2-checks.log`). Source and
binary identities are in `results/identities.json`.

This is the smaller measured alternative. A subsequent indexed-heap commit
supersedes it after direct A/B testing; their percentages are not cumulative.
