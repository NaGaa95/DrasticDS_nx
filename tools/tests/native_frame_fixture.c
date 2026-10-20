/* Runs the actual handoff implementation as AArch64 code under Unicorn.
 * Only Android pthread calls are replaced with deterministic lock assertions. */
#include <stddef.h>
#include <stdint.h>
#include "../../source/drastic_native_frame.c"
#include "../../source/drastic_dksh.h"

_Alignas(4096) uint8_t module_image[0x3f2e000];
_Alignas(4096) static uint8_t frame_pool[DRASTIC_NATIVE_POOL_BYTES];
static int locked, error_line, signals, waits;
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
#define CALLBACK_CHECK(condition) do { if (!(condition)) error_line = __LINE__; } while (0)

void *memcpy(void *dst, const void *src, size_t n) {
  for (size_t i = 0; i < n; i++) ((uint8_t *)dst)[i] = ((const uint8_t *)src)[i];
  return dst;
}
void *memset(void *dst, int c, size_t n) {
  for (size_t i = 0; i < n; i++) ((uint8_t *)dst)[i] = (uint8_t)c;
  return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int d = ((const uint8_t *)a)[i] - ((const uint8_t *)b)[i];
    if (d) return d;
  }
  return 0;
}
int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *m) {
  CALLBACK_CHECK(!locked && m == frame_mutex()); locked = 1; return 0;
}
int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *m) {
  CALLBACK_CHECK(locked && m == frame_mutex()); locked = 0; return 0;
}
int pthread_cond_signal_soloader(pthread_cond_t_bionic *c) {
  CALLBACK_CHECK(locked && (uint8_t *)c == g_state + CONDITION); signals++; return 0;
}
static void store32(unsigned offset, unsigned value) { memcpy(g_state + offset, &value, 4); }
static void test_wait(void *object) {
  /* The callback must finish before the producer changes its write bank. */
  const unsigned leased_bank = (uintptr_t)object - 1;
  CALLBACK_CHECK(locked && read32(g_state + WRITE_BANK) != leased_bank);
  waits++;
}

int bridge_test(void) {
  so_module module = {0};
  module.load_base = module.load_virtbase = module_image;
  module.load_size = sizeof(module_image);
  CHECK(!drastic_native_frame_install(NULL));
  /* Invalid profiles must not partially patch the input. */
  uint32_t original;
  memcpy(&original, module_image + 0x1cfe4, 4);
  module_image[0x1cfe4] ^= 1;
  CHECK(!drastic_native_frame_install(&module));
  CHECK(read32(module_image + 0x1cc30) == 0x52800201);
  CHECK(read32(module_image + 0x1cb14) == 0xa9be4ff4);
  CHECK(read32(module_image + 0x1cb9c) == 0x97ffe689);
  memcpy(module_image + 0x1cfe4, &original, 4);
  CHECK(drastic_native_frame_install(&module));
  CHECK(read32(module_image + 0x1cc30) == 0x52820001);
  CHECK(read32(module_image + 0x1cc34) == 0x52a00602);
  CHECK(read32(module_image + 0x1cb14) == 0x58000050);
  CHECK(read32(module_image + 0x1cb9c) == 0xd503201f);
  CHECK(read_pointer(module_image + 0x1cb1c) == (void *)native_flip);
  memset(g_state, 0, 0x9e8);
  void *allocation = frame_pool;
  memcpy(g_state + ALLOCATION, &allocation, 8);
  memcpy(g_state, &allocation, 8);
  allocation = frame_pool + DRASTIC_NATIVE_BANK_BYTES;
  memcpy(g_state + 8, &allocation, 8);
  store32(COLOR_BITS, 32);
  DrasticNativeFrame frame;
  CHECK(drastic_native_frame_lock(&frame));
  CHECK(frame.bank == 1 && frame.dirty == 0 && frame.bytes_per_pixel == 4);
  drastic_native_frame_unlock(NULL, NULL, 0);
  for (unsigned i = 0; i < 4096; i++) {
    const unsigned writer = i & 1, top = (i >> 1) & 1, bottom = (i >> 2) & 1;
    const unsigned bits = (i & 8) ? 16 : 32;
    store32(COLOR_BITS, bits);
    store32(HIGH_RES, top); store32(HIGH_RES + 4, bottom);
    g_state[DIRTY] = 3;
    native_flip();
    CHECK(!locked && read32(g_state + WRITE_BANK) == (writer ^ 1));
    /* Simulate producer preparation of the next frame before consumption. */
    store32(HIGH_RES, top ^ 1); store32(HIGH_RES + 4, bottom ^ 1);
    store32(COLOR_BITS, bits == 16 ? 32 : 16);
    g_state[DIRTY] = 1;
    CHECK(drastic_native_frame_lock(&frame));
    CHECK(frame.bank == writer && frame.dirty == 3);
    CHECK(frame.bytes_per_pixel == bits / 8);
    CHECK(frame.width[0] == (256u << top) && frame.height[0] == (192u << top));
    CHECK(frame.width[1] == (256u << bottom) && frame.height[1] == (192u << bottom));
    CHECK(frame.pixels[0] == frame_pool + writer * DRASTIC_NATIVE_BANK_BYTES);
    CHECK(frame.pixels[1] == (const uint8_t *)frame.pixels[0] + DRASTIC_NATIVE_SCREEN_BYTES);
    CHECK((size_t)frame.width[0] * frame.height[0] * frame.bytes_per_pixel <= DRASTIC_NATIVE_SCREEN_BYTES);
    drastic_native_frame_unlock((void *)(uintptr_t)(writer + 1), test_wait, 1);
    CHECK(g_state[DIRTY] == 1); /* Never clear unpublished producer changes. */
    CHECK(drastic_native_frame_lock(&frame));
    CHECK(frame.dirty == 2); /* Hidden/unuploaded screen stays dirty. */
    drastic_native_frame_unlock(NULL, NULL, 2);
    CHECK(!locked && !error_line);
  }
  CHECK(waits == 4095 && signals == 4096);
  const unsigned saved_writer = read32(g_state + WRITE_BANK);
  store32(WRITE_BANK, 2);
  CHECK(!drastic_native_frame_lock(&frame) && !locked);
  store32(WRITE_BANK, saved_writer);
  const unsigned read_bank_index = saved_writer ^ 1;
  const unsigned saved_bits = g_banks[read_bank_index].bits;
  g_banks[read_bank_index].bits = 24;
  CHECK(!drastic_native_frame_lock(&frame) && !locked);
  g_banks[read_bank_index].bits = saved_bits;
  g_banks[read_bank_index].resolution[0] = 2;
  CHECK(!drastic_native_frame_lock(&frame) && !locked);
  g_banks[read_bank_index].resolution[0] = 0;
  drastic_native_frame_detach();
  CHECK(!locked && !g_leases[0].wait && !g_leases[1].wait);
  native_flip();
  CHECK(waits == 4095 && !error_line);
  return 0;
}

int dksh_check(const void *bytes, size_t size, unsigned stage) {
  return drastic_dksh_valid(bytes, size, stage);
}
