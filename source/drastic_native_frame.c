/* Native display handoff for DraStic r2.6.0.4a build 109.
 * See docs/deko3d.md for the verified producer/consumer ABI and disassembly. */
#include "drastic_native_frame.h"
#include "pthr.h"
#include <string.h>

enum {
  STATE = 0x03f2d1f8,
  WRITE_BANK = 0x958,
  ALLOCATION = 0x950,
  COLOR_BITS = 0x95c,
  HIGH_RES = 0x968,
  DIRTY = 0x988,
  MUTEX = 0x98c,
  CONDITION = 0x9b4,
};

static uint8_t *g_state;
static struct { void *object; DrasticNativeFrameWait wait; } g_leases[2];
static struct { unsigned bits, resolution[2]; bool valid; } g_banks[2];
static unsigned g_pending_dirty;
static unsigned g_locked_bank;

static uint32_t read32(const void *address) {
  uint32_t value; memcpy(&value, address, sizeof(value)); return value;
}
static void *read_pointer(const void *address) {
  void *value; memcpy(&value, address, sizeof(value)); return value;
}
static pthread_mutex_t_bionic *frame_mutex(void) {
  return (pthread_mutex_t_bionic *)(g_state + MUTEX);
}

static void native_flip(void) {
  pthread_mutex_lock_soloader(frame_mutex());
  const unsigned completed = read32(g_state + WRITE_BANK) & 1u;
  const unsigned next = completed ^ 1u;
  if (g_leases[next].wait) {
    g_leases[next].wait(g_leases[next].object);
    g_leases[next].wait = NULL;
  }
  /* Resolution is producer state, updated before the screen is rendered and
   * outside this mutex by the original core. Publish a snapshot with its bank
   * at the flip, so next frame's resolution cannot describe this frame. */
  g_banks[completed].bits = read32(g_state + COLOR_BITS);
  for (unsigned screen = 0; screen < 2; screen++)
    g_banks[completed].resolution[screen] = read32(g_state + HIGH_RES + screen * 4);
  g_banks[completed].valid = true;
  g_pending_dirty |= g_state[DIRTY] & 3;
  g_state[DIRTY] = 0;
  memcpy(g_state + WRITE_BANK, &next, sizeof(next));
  pthread_cond_signal_soloader((pthread_cond_t_bionic *)(g_state + CONDITION));
  pthread_mutex_unlock_soloader(frame_mutex());
}

bool drastic_native_frame_install(so_module *module) {
  /* No heuristic signatures: reject a different core before writing anything.
   * The function at 1cb14 toggles the write bank under this exact mutex and
   * signals the screen condition. Its replacement preserves those operations. */
  static const uint32_t flip[] = {
    0xa9be4ff4,0xa9017bfd,0x910043fd,0xb001f893,0x912d4273,
    0x9100d274,0xaa1403e0,0x97ffe728,0xb9400268,0x2a2803e8,
    0x12000108,0xb805c668,0xaa1303e0,0x97ffe69e,0xa9417bfd,
    0xaa1403e0,0xa8c24ff4,0x17ffe60a,
  };
  static const uint32_t read_bank[] = {
    0xb001f888,0x9107e108,0xb9895909,0x52a0018a,0xf8697908,
    0x12000009,0x9baa2120,0xd65f03c0,
  };
  static const uint32_t render[] = {
    0xd101c3ff,0xf9000bfb,0xa90267fa,0xa9035ff8,0xa90457f6,
    0xa9054ff4,0xa9067bfd,0x910183fd,0xb001f888,0xf945a508,
  };
  if (!module || !module->load_base || !module->load_virtbase ||
      module->load_size < STATE + 0x9e8) return false;
  const uint8_t *code = module->load_base;
  if (memcmp(code + 0x1cb14, flip, sizeof(flip)) ||
      memcmp(code + 0x1cd18, read_bank, sizeof(read_bank)) ||
      memcmp(code + 0x1ceac, render, sizeof(render)) ||
      read32(code + 0x1cc30) != 0x52800201u || /* mov w1,#16 */
      read32(code + 0x1cc34) != 0x52a00602u || /* mov w2,#0x300000 */
      read32(code + 0x1cc44) != 0x3d825e61u ||
      read32(code + 0x1cc48) != 0x97ffe4eau || /* posix_memalign */
      read32(code + 0x1cb9c) != 0x97ffe689u || /* premature dirty notification */
      read32(code + 0x3d2a8) != 0x97ff7e2du ||
      read32(code + 0x3d2b8) != 0x97ff7e29u ||
      read32(code + 0x3d2bc) != 0x97ff7e16u ||
      read32(code + 0x1cf18) != 0xf8687b77u ||
      read32(code + 0x1cfe4) != 0x392e02dfu) return false;

  const uintptr_t target = (uintptr_t)native_flip;
  const uint32_t hook[] = {0x58000050u,0xd61f0200u,
                          (uint32_t)target,(uint32_t)(target >> 32)};
  const uint32_t page_alignment = 0x52820001u; /* mov w1,#4096 */
  const uint32_t nop = 0xd503201fu;
  memcpy((uint8_t *)module->load_base + 0x1cb14, hook, sizeof(hook));
  memcpy((uint8_t *)module->load_base + 0x1cc30, &page_alignment, 4);
  /* Both mark-dirty callers immediately proceed to the bank flip. Notify at
   * publication instead: waking here can make the consumer present the old
   * bank and block the producer's flip while that redundant frame is built. */
  memcpy((uint8_t *)module->load_base + 0x1cb9c, &nop, 4);
  g_state = (uint8_t *)module->load_virtbase + STATE;
  memset(g_leases, 0, sizeof(g_leases));
  memset(g_banks, 0, sizeof(g_banks));
  g_pending_dirty = 0;
  return true;
}

bool drastic_native_frame_lock(DrasticNativeFrame *frame) {
  if (!g_state || !frame) return false;
  pthread_mutex_lock_soloader(frame_mutex());
  memset(frame, 0, sizeof(*frame));
  frame->allocation = read_pointer(g_state + ALLOCATION);
  const unsigned writer = read32(g_state + WRITE_BANK);
  const unsigned bank_index = (writer ^ 1u) & 1u;
  const unsigned bits = g_banks[bank_index].valid ? g_banks[bank_index].bits
                                               : read32(g_state + COLOR_BITS);
  if (!frame->allocation || ((uintptr_t)frame->allocation & 4095) ||
      writer > 1 || (bits != 16 && bits != 32)) {
    pthread_mutex_unlock_soloader(frame_mutex());
    return false;
  }
  frame->bank = writer ^ 1u;
  const uint8_t *bank = read_pointer(g_state + frame->bank * sizeof(void *));
  if (bank != (const uint8_t *)frame->allocation +
                  frame->bank * DRASTIC_NATIVE_BANK_BYTES) {
    pthread_mutex_unlock_soloader(frame_mutex());
    return false;
  }
  frame->dirty = g_pending_dirty;
  frame->bytes_per_pixel = bits / 8;
  for (unsigned screen = 0; screen < 2; screen++) {
    const unsigned resolution = g_banks[bank_index].valid
        ? g_banks[bank_index].resolution[screen]
        : read32(g_state + HIGH_RES + screen * 4);
    if (resolution > 1) {
      pthread_mutex_unlock_soloader(frame_mutex());
      return false;
    }
    frame->width[screen] = 256u << resolution;
    frame->height[screen] = 192u << resolution;
    frame->pixels[screen] = bank + screen * DRASTIC_NATIVE_SCREEN_BYTES;
  }
  g_locked_bank = frame->bank;
  return true;
}

void drastic_native_frame_unlock(void *lease, DrasticNativeFrameWait wait,
                                 unsigned uploaded) {
  if (lease && wait) {
    g_leases[g_locked_bank].object = lease;
    g_leases[g_locked_bank].wait = wait;
  }
  g_pending_dirty &= ~uploaded;
  pthread_mutex_unlock_soloader(frame_mutex());
}

void drastic_native_frame_detach(void) {
  if (!g_state) return;
  pthread_mutex_lock_soloader(frame_mutex());
  memset(g_leases, 0, sizeof(g_leases));
  pthread_mutex_unlock_soloader(frame_mutex());
}
