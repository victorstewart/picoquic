/* TEST ONLY. Predictable crypto randomness for byte-for-byte poison comparisons.
 * Never compile PERSISTENT_DETERMINISTIC into a production application. */
#include <openssl/rand.h>
static uint64_t bench_crypto_state = UINT64_C(0x137bce942685ad0f);
static int bench_random_bytes(unsigned char* out, int length)
{
    for (int i = 0; i < length; i++) {
        bench_crypto_state ^= bench_crypto_state << 13;
        bench_crypto_state ^= bench_crypto_state >> 7;
        bench_crypto_state ^= bench_crypto_state << 17;
        out[i] = (unsigned char)bench_crypto_state;
    }
    return 1;
}
static int bench_random_seed(const void* bytes, int length)
{ (void)bytes; (void)length; return 1; }
static int bench_random_add(const void* bytes, int length, double entropy)
{ (void)bytes; (void)length; (void)entropy; return 1; }
static int bench_random_status(void) { return 1; }
static void bench_deterministic_random_init(void)
{
    static RAND_METHOD method = { bench_random_seed, bench_random_bytes, NULL,
        bench_random_add, bench_random_bytes, bench_random_status };
    if (RAND_set_rand_method(&method) != 1) abort();
    fputs("TEST ONLY: deterministic crypto randomness enabled\n", stderr);
}
