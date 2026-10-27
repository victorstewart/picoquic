/* Native ordered-index mechanism screen. Not production Picoquic code. */
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include "picosplay.h"

#define LEAF_CAP 32
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)

typedef struct item {
    uint64_t deadline, ordinal;
    uint32_t id;
    picosplay_node_t link; /* The current production-shaped intrusive link. */
} item;

typedef struct key { uint64_t deadline, ordinal; } key;
typedef struct leaf {
    uint64_t deadline[LEAF_CAP];
    uint64_t ordinal[LEAF_CAP];
    item *value[LEAF_CAP];
    unsigned count;
} leaf;
typedef struct compact {
    leaf **leaves;
    key *maximum;
    size_t count, capacity, entries, heap_bytes;
} compact;
typedef struct splay { picosplay_tree_t tree; } splay;
typedef struct ackinline { item *small[8]; unsigned count; splay overflow; } ackinline;
typedef enum { V_SPLAY, V_COMPACT, V_ACKINLINE } variant;
typedef struct ix_t { variant kind; splay s; compact c; ackinline a; } ix_t;

static uint64_t prng(uint64_t *state)
{
    uint64_t z;
    *state += UINT64_C(0x9e3779b97f4a7c15);
    z = *state;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static key item_key(const item *x) { return (key){ x->deadline, x->ordinal }; }
static int key_cmp(key a, key b)
{
    if (a.deadline != b.deadline) return a.deadline < b.deadline ? -1 : 1;
    if (a.ordinal != b.ordinal) return a.ordinal < b.ordinal ? -1 : 1;
    return 0;
}
static int item_cmp(const item *a, const item *b) { return key_cmp(item_key(a), item_key(b)); }

/* Intrusive current splay baseline: no wrapper allocations or lookup-on-delete. */
static int64_t splay_cmp(void *a, void *b) { return item_cmp(a, b); }
static picosplay_node_t *splay_create(void *value) { return &((item *)value)->link; }
static void splay_delete_node(void *tree, picosplay_node_t *node) { (void)tree; (void)node; }
static void *splay_value(picosplay_node_t *node)
{
    return (void *)((char *)node - offsetof(item, link));
}
static void splay_init(splay *s)
{
    picosplay_init_tree(&s->tree, splay_cmp, splay_create, splay_delete_node, splay_value);
}
static void splay_put(splay *s, item *x) { CHECK(picosplay_insert(&s->tree, x) != NULL); }
static void splay_del(splay *s, item *x) { picosplay_delete_hint(&s->tree, &x->link); }
static item *splay_get(splay *s, item *x)
{
    picosplay_node_t *n = picosplay_find(&s->tree, x);
    return n == NULL ? NULL : splay_value(n);
}
static item *splay_pred(splay *s, item *x)
{
    picosplay_node_t *n = picosplay_find_previous(&s->tree, x); /* <= */
    return n == NULL ? NULL : splay_value(n);
}
static item *splay_first(splay *s)
{
    picosplay_node_t *n = picosplay_first(&s->tree);
    return n == NULL ? NULL : splay_value(n);
}
static void splay_clear(splay *s) { picosplay_empty_tree(&s->tree); }

static void *xrealloc(void *p, size_t n)
{
    void *q;
    CHECK(n != 0 && n <= SIZE_MAX / 2);
    q = realloc(p, n);
    CHECK(q != NULL);
    return q;
}
static void *xcalloc(size_t n, size_t size)
{
    void *p;
    CHECK(size == 0 || n <= SIZE_MAX / size);
    p = calloc(n, size);
    CHECK(p != NULL);
    return p;
}
static void compact_reserve(compact *c, size_t want)
{
    size_t cap = c->capacity == 0 ? 8 : c->capacity;
    while (cap < want) { CHECK(cap <= SIZE_MAX / 2); cap *= 2; }
    if (cap != c->capacity) {
        CHECK(cap <= SIZE_MAX / sizeof(*c->leaves));
        CHECK(cap <= SIZE_MAX / sizeof(*c->maximum));
        c->leaves = xrealloc(c->leaves, cap * sizeof(*c->leaves));
        c->maximum = xrealloc(c->maximum, cap * sizeof(*c->maximum));
        c->capacity = cap;
    }
}
static void compact_refresh(compact *c, size_t page)
{
    leaf *l = c->leaves[page];
    CHECK(l->count != 0);
    c->maximum[page] = (key){ l->deadline[l->count - 1], l->ordinal[l->count - 1] };
}
static size_t lower_max(const compact *c, key target)
{
    size_t lo = 0, hi = c->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (key_cmp(c->maximum[mid], target) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}
static unsigned lower_leaf(const leaf *l, key target)
{
    unsigned lo = 0, hi = l->count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        key at = { l->deadline[mid], l->ordinal[mid] };
        if (key_cmp(at, target) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}
static unsigned upper_leaf(const leaf *l, key target)
{
    unsigned lo = 0, hi = l->count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        key at = { l->deadline[mid], l->ordinal[mid] };
        if (key_cmp(at, target) <= 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}
static void compact_init(compact *c) { memset(c, 0, sizeof(*c)); }
static size_t compact_page(const compact *c, key target)
{
    size_t page;
    CHECK(c->count != 0);
    page = lower_max(c, target);
    return page == c->count ? c->count - 1 : page;
}
static void compact_insert_page(compact *c, size_t page, leaf *l)
{
    compact_reserve(c, c->count + 1);
    memmove(c->leaves + page + 1, c->leaves + page, (c->count - page) * sizeof(*c->leaves));
    memmove(c->maximum + page + 1, c->maximum + page, (c->count - page) * sizeof(*c->maximum));
    c->leaves[page] = l;
    c->count++;
    c->heap_bytes += sizeof(*l);
}
static void compact_put(compact *c, item *x)
{
    key target = item_key(x);
    size_t page;
    leaf *l;
    unsigned at;
    if (c->count == 0) {
        l = xcalloc(1, sizeof(*l));
        compact_insert_page(c, 0, l);
        c->maximum[0] = target;
    }
    page = compact_page(c, target);
    l = c->leaves[page];
    at = lower_leaf(l, target);
    CHECK(at == l->count || key_cmp((key){l->deadline[at], l->ordinal[at]}, target) != 0);
    if (l->count == LEAF_CAP) {
        leaf *right = xcalloc(1, sizeof(*right));
        memcpy(right->deadline, l->deadline + LEAF_CAP / 2, (LEAF_CAP / 2) * sizeof(uint64_t));
        memcpy(right->ordinal, l->ordinal + LEAF_CAP / 2, (LEAF_CAP / 2) * sizeof(uint64_t));
        memcpy(right->value, l->value + LEAF_CAP / 2, (LEAF_CAP / 2) * sizeof(item *));
        right->count = LEAF_CAP / 2;
        l->count = LEAF_CAP / 2;
        compact_insert_page(c, page + 1, right);
        compact_refresh(c, page);
        compact_refresh(c, page + 1);
        if (key_cmp(target, (key){right->deadline[0], right->ordinal[0]}) >= 0) { page++; l = right; }
        at = lower_leaf(l, target);
    }
    memmove(l->deadline + at + 1, l->deadline + at, (l->count - at) * sizeof(uint64_t));
    memmove(l->ordinal + at + 1, l->ordinal + at, (l->count - at) * sizeof(uint64_t));
    memmove(l->value + at + 1, l->value + at, (l->count - at) * sizeof(item *));
    l->deadline[at] = target.deadline;
    l->ordinal[at] = target.ordinal;
    l->value[at] = x;
    l->count++;
    c->entries++;
    compact_refresh(c, page);
}
static item *compact_get(compact *c, item *x)
{
    leaf *l; unsigned at;
    if (c->count == 0) return NULL;
    l = c->leaves[compact_page(c, item_key(x))];
    at = lower_leaf(l, item_key(x));
    return at < l->count && key_cmp((key){l->deadline[at],l->ordinal[at]},item_key(x)) == 0 ? l->value[at] : NULL;
}
static item *compact_pred(compact *c, item *x)
{
    size_t page; leaf *l; unsigned at;
    if (c->count == 0) return NULL;
    page = compact_page(c, item_key(x)); l = c->leaves[page]; at = upper_leaf(l, item_key(x));
    if (at != 0) return l->value[at - 1];
    while (page != 0) { l = c->leaves[--page]; if (l->count != 0) return l->value[l->count - 1]; }
    return NULL;
}
static item *compact_first(compact *c)
{
    return c->count == 0 || c->entries == 0 ? NULL : c->leaves[0]->value[0];
}
static void compact_del(compact *c, item *x)
{
    size_t page; leaf *l; unsigned at;
    CHECK(c->count != 0);
    page = compact_page(c, item_key(x)); l = c->leaves[page]; at = lower_leaf(l, item_key(x));
    CHECK(at < l->count && l->value[at] == x);
    memmove(l->deadline + at, l->deadline + at + 1, (l->count - at - 1) * sizeof(uint64_t));
    memmove(l->ordinal + at, l->ordinal + at + 1, (l->count - at - 1) * sizeof(uint64_t));
    memmove(l->value + at, l->value + at + 1, (l->count - at - 1) * sizeof(item *));
    l->count--; c->entries--;
    if (l->count == 0 && c->count > 1) {
        free(l); c->heap_bytes -= sizeof(*l);
        memmove(c->leaves + page, c->leaves + page + 1, (c->count - page - 1) * sizeof(*c->leaves));
        memmove(c->maximum + page, c->maximum + page + 1, (c->count - page - 1) * sizeof(*c->maximum));
        c->count--;
    } else if (l->count != 0) compact_refresh(c, page);
}
static void compact_clear(compact *c)
{
    size_t i;
    for (i = 0; i < c->count; i++) free(c->leaves[i]);
    free(c->leaves); free(c->maximum); compact_init(c);
}

static void ack_init(ackinline *a) { memset(a, 0, sizeof(*a)); splay_init(&a->overflow); }
static void ack_put(ackinline *a, item *x)
{
    unsigned i;
    if (a->count == 8) { splay_put(&a->overflow, x); return; }
    i = a->count;
    while (i != 0 && item_cmp(a->small[i - 1], x) > 0) { a->small[i] = a->small[i - 1]; i--; }
    a->small[i] = x; a->count++;
}
static item *ack_get(ackinline *a, item *x)
{
    unsigned i;
    for (i = 0; i < a->count; i++) if (item_cmp(a->small[i], x) == 0) return a->small[i];
    return splay_get(&a->overflow, x);
}
static item *ack_pred(ackinline *a, item *x)
{
    item *best = NULL, *overflow = splay_pred(&a->overflow, x); unsigned i;
    for (i = 0; i < a->count && item_cmp(a->small[i], x) <= 0; i++) best = a->small[i];
    return overflow != NULL && (best == NULL || item_cmp(overflow, best) > 0) ? overflow : best;
}
static item *ack_first(ackinline *a)
{
    item *overflow = splay_first(&a->overflow);
    if (a->count == 0) return overflow;
    return overflow == NULL || item_cmp(a->small[0], overflow) < 0 ? a->small[0] : overflow;
}
static void ack_del(ackinline *a, item *x)
{
    unsigned i;
    for (i = 0; i < a->count; i++) if (item_cmp(a->small[i], x) == 0) {
        memmove(a->small + i, a->small + i + 1, (a->count - i - 1) * sizeof(item *)); a->count--; return;
    }
    splay_del(&a->overflow, x);
}
static void ack_clear(ackinline *a) { splay_clear(&a->overflow); a->count = 0; }

static void index_init(ix_t *ix, variant kind)
{
    memset(ix, 0, sizeof(*ix)); ix->kind = kind;
    if (kind == V_SPLAY) splay_init(&ix->s); else if (kind == V_COMPACT) compact_init(&ix->c); else ack_init(&ix->a);
}
static void index_put(ix_t *ix, item *x)
{
    if (ix->kind == V_SPLAY) splay_put(&ix->s,x);
    else if (ix->kind == V_COMPACT) compact_put(&ix->c,x);
    else ack_put(&ix->a,x);
}
static void index_del(ix_t *ix, item *x)
{
    if (ix->kind == V_SPLAY) splay_del(&ix->s,x);
    else if (ix->kind == V_COMPACT) compact_del(&ix->c,x);
    else ack_del(&ix->a,x);
}
static item *index_get(ix_t *ix, item *x)
{
    if (ix->kind == V_SPLAY) return splay_get(&ix->s,x);
    if (ix->kind == V_COMPACT) return compact_get(&ix->c,x);
    return ack_get(&ix->a,x);
}
static item *index_pred(ix_t *ix, item *x)
{
    if (ix->kind == V_SPLAY) return splay_pred(&ix->s,x);
    if (ix->kind == V_COMPACT) return compact_pred(&ix->c,x);
    return ack_pred(&ix->a,x);
}
static item *index_first(ix_t *ix)
{
    if (ix->kind == V_SPLAY) return splay_first(&ix->s);
    if (ix->kind == V_COMPACT) return compact_first(&ix->c);
    return ack_first(&ix->a);
}
static void index_clear(ix_t *ix)
{
    if (ix->kind == V_SPLAY) splay_clear(&ix->s);
    else if (ix->kind == V_COMPACT) compact_clear(&ix->c);
    else ack_clear(&ix->a);
}
static size_t index_heap_bytes(ix_t *ix)
{
    return ix->kind == V_COMPACT ? ix->c.heap_bytes + ix->c.capacity * (sizeof(leaf *) + sizeof(key)) : 0;
}

static int item_ptr_cmp(const void *a, const void *b) { return item_cmp(*(item * const *)a, *(item * const *)b); }
static void verify_reference(ix_t *ix, item *items, size_t n)
{
    item **sorted = xcalloc(n, sizeof(*sorted)); size_t i;
    for (i = 0; i < n; i++) { sorted[i] = &items[i]; CHECK(index_get(ix, sorted[i]) == sorted[i]); }
    qsort(sorted, n, sizeof(*sorted), item_ptr_cmp);
    CHECK(index_first(ix) == sorted[0]);
    for (i = 0; i < n; i++) {
        item q = *sorted[i]; size_t lo = 0, hi = n;
        CHECK(index_pred(ix, sorted[i]) == sorted[i]);
        q.deadline = q.deadline == UINT64_MAX ? UINT64_MAX : q.deadline + 1;
        while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (item_cmp(sorted[mid], &q) <= 0) lo = mid + 1; else hi = mid; }
        CHECK(index_pred(ix, &q) == (lo == 0 ? NULL : sorted[lo - 1]));
    }
    free(sorted);
}

static const char *base_workload(const char *w)
{
    if (strncmp(w,"stream",6)==0) return "stream";
    if (strncmp(w,"reassembly",10)==0) return "reassembly";
    if (strncmp(w,"ack",3)==0) return "ack";
    if (strncmp(w,"lost",4)==0) return "lost";
    if (strncmp(w,"token",5)==0) return "token";
    if (strncmp(w,"timers",6)==0) return "timers";
    return "";
}
static size_t workload_size(const char *w)
{
    if (!strcmp(w,"stream-small2") || !strcmp(w,"stream-hot2")) return 2;
    if (!strcmp(w,"stream-large4096")) return 4096;
    if (!strcmp(w,"reassembly-4")) return 4; if (!strcmp(w,"reassembly-1024")) return 1024;
    if (!strcmp(w,"ack-1")) return 1; if (!strcmp(w,"ack-8")) return 8; if (!strcmp(w,"ack-1024")) return 1024;
    if (!strcmp(w,"lost-8")) return 8; if (!strcmp(w,"lost-1024")) return 1024;
    if (!strcmp(w,"token-10000") || !strcmp(w,"timers-10000")) return 10000;
    return 0;
}
static uint64_t exercise(ix_t *ix, item *items, size_t n, size_t iterations, uint64_t seed, const char *work, int verify)
{
    const char *base = base_workload(work); uint64_t sum = 0; size_t z;
    for (z = 0; z < iterations; z++) {
        uint64_t r = prng(&seed); item *x = (!strcmp(work,"stream-hot2") && r % 100 < 95) ? &items[0] : &items[r % n];
        unsigned op = (unsigned)(r % 100);
        if (!strcmp(base,"stream")) {
            if (op < 90) { item *y = index_get(ix,x); CHECK(y != NULL); sum += y->id; }
            else { index_del(ix,x); x->deadline ^= prng(&seed) & 255; index_put(ix,x); }
        } else if (!strcmp(base,"lost")) {
            x = index_first(ix); CHECK(x != NULL); index_del(ix,x); x->deadline ^= prng(&seed) & 1023; index_put(ix,x); sum += x->id;
        } else if (!strcmp(base,"token")) {
            if (op < 45) { item *y = index_get(ix,x); CHECK(y != NULL); sum += y->id; }
            else { index_del(ix,x); x->deadline += (prng(&seed) & 4095) + 1; index_put(ix,x); }
        } else if (!strcmp(base,"timers")) {
            if (op < 70) { item *y = index_first(ix); CHECK(y != NULL); sum += y->id; }
            else { index_del(ix,x); x->deadline ^= prng(&seed) & 1023; index_put(ix,x); }
        } else if (op < 55) {
            item q = *x; item *y; q.deadline += prng(&seed) & 63; y = index_pred(ix,&q); sum += y == NULL ? 0 : y->id;
        } else if (op < 80) { item *y = index_first(ix); CHECK(y != NULL); sum += y->id; }
        else { index_del(ix,x); x->deadline ^= prng(&seed) & 1023; index_put(ix,x); }
        if (verify && z % 97 == 0) verify_reference(ix,items,n);
    }
    return sum;
}
static void make_items(item *items, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        items[i].deadline = i % 17 == 0 ? UINT64_MAX - i : (i % 17) * 4096 + i / 17;
        items[i].ordinal = UINT64_MAX - i; items[i].id = (uint32_t)i;
        memset(&items[i].link, 0, sizeof(items[i].link));
    }
}
static uint64_t checked_run(variant kind, const char *work, size_t n)
{
    ix_t ix; item *items = xcalloc(n,sizeof(*items)); uint64_t sum; size_t i;
    make_items(items,n); index_init(&ix,kind); for(i=0;i<n;i++) index_put(&ix,&items[i]);
    CHECK(index_get(&ix,&items[0]) == &items[0]);
    { item missing = items[0]; missing.ordinal ^= UINT64_C(0x8000000000000000); CHECK(index_get(&ix,&missing) == NULL); }
    sum = exercise(&ix,items,n,2000,UINT64_C(0x1234),work,1);
    for (i = 0; i < n; i++) index_del(&ix, &items[i]);
    CHECK(index_first(&ix) == NULL);
    index_put(&ix, &items[0]); CHECK(index_first(&ix) == &items[0]);
    index_clear(&ix); CHECK(index_first(&ix) == NULL);
    index_clear(&ix); CHECK(index_first(&ix) == NULL); free(items); return sum;
}
static int selfcheck(void)
{
    const char *works[] = {"stream-small2","stream-hot2","stream-large4096","reassembly-4","reassembly-1024","ack-1","ack-8","ack-1024","lost-8","lost-1024","token-10000","timers-10000"};
    size_t w;
    for (w=0; w<sizeof(works)/sizeof(works[0]); w++) {
        size_t n=workload_size(works[w]); uint64_t s=checked_run(V_SPLAY,works[w],n), c=checked_run(V_COMPACT,works[w],n), a=checked_run(V_ACKINLINE,works[w],n);
        CHECK(s == c); CHECK(s == a);
    }
    return 0;
}
static uint64_t wall_ns(void) { struct timespec t; CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0); return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec; }
static uint64_t cpu_ns(void) { struct rusage r; CHECK(getrusage(RUSAGE_SELF,&r)==0); return ((uint64_t)r.ru_utime.tv_sec+(uint64_t)r.ru_stime.tv_sec)*UINT64_C(1000000000)+((uint64_t)r.ru_utime.tv_usec+(uint64_t)r.ru_stime.tv_usec)*1000; }
static void usage(const char *name)
{
    fprintf(stderr,"usage: %s --selfcheck | WORKLOAD VARIANT ITERATIONS SEED\n",name);
    fprintf(stderr,"workloads: stream-small2 stream-hot2 stream-large4096 reassembly-4 reassembly-1024 ack-1 ack-8 ack-1024 lost-8 lost-1024 token-10000 timers-10000\n");
}
int main(int argc, char **argv)
{
    size_t n, iterations, i; variant kind; ix_t ix; item *items; uint64_t seed, wall0, cpu0, sum, wall, cpu;
    if (argc == 2 && !strcmp(argv[1],"--selfcheck")) return selfcheck();
    if (argc != 5) { usage(argv[0]); return 2; }
    n = workload_size(argv[1]);
    if (!strcmp(argv[2],"splay")) kind=V_SPLAY; else if (!strcmp(argv[2],"compact")) kind=V_COMPACT; else if (!strcmp(argv[2],"ack-inline")) kind=V_ACKINLINE; else { usage(argv[0]); return 2; }
    if (n == 0) { usage(argv[0]); return 2; }
    iterations=strtoull(argv[3],NULL,0); seed=strtoull(argv[4],NULL,0); items=xcalloc(n,sizeof(*items)); make_items(items,n); index_init(&ix,kind);
    for(i=0;i<n;i++) { items[i].deadline = (i % 17) * 4096 + i / 17; items[i].ordinal = i; index_put(&ix,&items[i]); } wall0=wall_ns(); cpu0=cpu_ns(); sum=exercise(&ix,items,n,iterations,seed,argv[1],0); wall=wall_ns()-wall0; cpu=cpu_ns()-cpu0;
    printf("workload=%s variant=%s entries=%zu iterations=%zu checksum=%" PRIu64 " wall_ns=%" PRIu64 " cpu_ns=%" PRIu64 " wall_ns_op=%.2f cpu_ns_op=%.2f embedded_link_bytes_entry=%zu heap_index_bytes=%zu\n",argv[1],argv[2],n,iterations,sum,wall,cpu,iterations?(double)wall/iterations:0,iterations?(double)cpu/iterations:0,sizeof(picosplay_node_t),index_heap_bytes(&ix));
    index_clear(&ix); free(items); return 0;
}
