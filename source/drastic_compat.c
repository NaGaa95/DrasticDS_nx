/* Compatibility fixes for the supported DraStic ARM64 core. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "drastic_compat.h"

/* r2.6.0.4a build 109 regressed one byte-write path for AUXSPICNT.  It stores
 * bit 7 even though that bit is the read-only SPI busy flag.  Dementium II
 * writes the register and then polls the flag, so the incorrect stored value
 * leaves the game in an infinite loop at boot.
 *
 * The older core masks bit 7 before storing.  Reuse a nearby two-instruction
 * dispatch stub for that operation, and redirect the stub's original DIV
 * register entries to an equivalent existing handler.  The latter also masks
 * reserved bit 7, preserving the original DIV side effect and valid values. */
#define WRITE8_DISPATCH_BASE       0x0010a6e8u
#define WRITE8_AUXSPICNT_LOW       0x01a0u
#define WRITE8_DIVCNT_LOW          0x0280u
#define WRITE8_DIV_RESULT_FIRST    0x0290u
#define WRITE8_DIV_RESULT_LAST     0x029fu

#define AUX_MASK_STUB_OFFSET       0x000235b0u
#define DIV_MASK_STUB_OFFSET       0x00023850u

#define DISPATCH_COMMON_STORE      0x019bu
#define DISPATCH_AUX_MASK_STUB     0x002fu
#define DISPATCH_DIV_MASK_STUB     0x00d7u

#define ARM64_STRB_WZR_X26_88      0x3901635fu
#define ARM64_AND_W20_7F           0x12001a94u
#define ARM64_B_COMMON_FROM_AUX    0x1400016bu
#define ARM64_B_COMMON_FROM_DIV    0x140000c2u

#define CAPPED_FAST_FORWARD_CHECK 0x0001ba24u
#define FIXED_SKIP_PHASE_CHECK    0x0001b958u

static const uint32_t capped_fast_forward_code[] = {
  0x5280003au, 0x528000d9u, 0x3940a6eau, 0x35fff2eau, /* mov #1, mov #6, ldrb, cbnz */
};
static const uint32_t fixed_skip_phase_code[] = {
  0x6b2a013fu, 0x1a8807e8u, 0x3900b2ebu, /* cmp, csinc eq, strb */
};

/* Fixed frame skipping (fast-forward) never resyncs its timing target, so a
 * fast-forward that falls behind leaves a deficit. Automatic frame skipping
 * then skips frames to catch up until its skip counter equals the configured
 * limit plus one, but it shares that counter with fixed skipping, which leaves
 * it at 1 or 2. The counter runs past the limit and the picture freezes until
 * the core catches up or the byte wraps. Treat a count at or past the limit as
 * reached, which resyncs at once. */
#define AUTO_SKIP_LIMIT_CHECK     0x0001b9d4u

static const uint32_t auto_skip_limit_code[] = {
  0x3940bee8u, 0x11000729u, 0x11000508u, /* ldrb w8, add w9, add w8 */
  0x6b28013fu, 0x3900bee8u, 0x54000641u, /* cmp uxtb, strb, b.ne skip */
};

/* At scanline 214 the core starts the 3D render for the next frame. With
 * threaded 3D the worker renders it while emulation continues, and the image is
 * published at the end of the following frame, so the render started at frame
 * k-2 is the one frame k shows. The core skips that render when engine A
 * skipped the frame that just ended, and a skipped render only copies the last
 * image. During fast-forward the render a drawn frame needs is therefore always
 * a skipped one, and the frame shows an older image from the other frame
 * parity: in games that alternate their 3D scene between the screens every
 * frame, the two views swap for as long as fast-forward is on.
 *
 * With threaded 3D, render every frame; only the core's own forced skip (flag
 * bit 3) still skips. The bic clears the skip when the threaded flag is set.
 * Without threaded 3D the image shows one frame after its render and the
 * original rule is kept. x0 still holds the system pointer x19 here, so the
 * call no longer needs its mov. */
#define THREADED_3D_SKIP_CHECK 0x0002c98cu

static const uint32_t threaded_3d_skip_code[] = {
  0x5293be08u, 0x72a06da8u, 0xaa1303e0u, /* mov w8, movk w8, mov x0, x19 */
  0x8b080274u, 0x97ffbb83u, 0x39400369u, /* add x20, bl 0x1b7a8, ldrb w9 */
  0x39400288u, 0xb9400f4au, 0x528dd80bu, /* ldrb w8, [x20], ldr w10, mov w11 */
  0x72a06dabu, 0x721d013fu, 0x8b0b0260u, /* movk w11, tst w9, add x0 */
  0x1a9f0501u,                           /* csinc w1, w8, wzr, eq */
};
static const uint32_t threaded_3d_skip_patch[] = {
  0x5293be08u, /* mov   w8, #0x9df0 */
  0x72a06da8u, /* movk  w8, #0x36d, lsl #16 */
  0x8b080274u, /* add   x20, x19, x8              engine A skip flag */
  0x97ffbb84u, /* bl    0x1b7a8 */
  0x39400369u, /* ldrb  w9, [x27] */
  0x39400288u, /* ldrb  w8, [x20] */
  0xb9400f4au, /* ldr   w10, [x26, #12]           threaded 3D */
  0x0a2a0108u, /* bic   w8, w8, w10 */
  0x528dd80bu, /* mov   w11, #0x6ec0 */
  0x72a06dabu, /* movk  w11, #0x36d, lsl #16 */
  0x721d013fu, /* tst   w9, #0x8 */
  0x8b0b0260u, /* add   x0, x19, x11 */
  0x1a9f0501u, /* csinc w1, w8, wzr, eq */
};

static int range_valid(const so_module *mod, size_t offset, size_t size) {
  return mod && mod->load_base && offset <= mod->load_size &&
         size <= mod->load_size - offset;
}

static uint32_t read_u32(const void *base, size_t offset) {
  uint32_t value;
  memcpy(&value, (const char *)base + offset, sizeof(value));
  return value;
}

static uint16_t read_u16(const void *base, size_t offset) {
  uint16_t value;
  memcpy(&value, (const char *)base + offset, sizeof(value));
  return value;
}

static void write_u32(void *base, size_t offset, uint32_t value) {
  memcpy((char *)base + offset, &value, sizeof(value));
}

static void write_u16(void *base, size_t offset, uint16_t value) {
  memcpy((char *)base + offset, &value, sizeof(value));
}

static size_t dispatch_offset(unsigned io_offset) {
  return WRITE8_DISPATCH_BASE + io_offset * sizeof(uint16_t);
}

int drastic_compat_install(so_module *mod) {
  const size_t table_end = dispatch_offset(WRITE8_DIV_RESULT_LAST) +
                           sizeof(uint16_t);
  if (!range_valid(mod, AUX_MASK_STUB_OFFSET, 2 * sizeof(uint32_t)) ||
      !range_valid(mod, DIV_MASK_STUB_OFFSET, 3 * sizeof(uint32_t)) ||
      !range_valid(mod, WRITE8_DISPATCH_BASE,
                   table_end - WRITE8_DISPATCH_BASE) ||
      !range_valid(mod, CAPPED_FAST_FORWARD_CHECK, sizeof(capped_fast_forward_code)) ||
      !range_valid(mod, FIXED_SKIP_PHASE_CHECK, sizeof(fixed_skip_phase_code)) ||
      !range_valid(mod, AUTO_SKIP_LIMIT_CHECK, sizeof(auto_skip_limit_code)) ||
      !range_valid(mod, THREADED_3D_SKIP_CHECK, sizeof(threaded_3d_skip_code)))
    return 0;

  /* Validate the complete code and table layout before changing any byte. */
  if (read_u32(mod->load_base, AUX_MASK_STUB_OFFSET) !=
          ARM64_STRB_WZR_X26_88 ||
      read_u32(mod->load_base, AUX_MASK_STUB_OFFSET + 4) !=
          ARM64_B_COMMON_FROM_AUX ||
      read_u32(mod->load_base, DIV_MASK_STUB_OFFSET) != ARM64_AND_W20_7F ||
      read_u32(mod->load_base, DIV_MASK_STUB_OFFSET + 4) !=
          ARM64_STRB_WZR_X26_88 ||
      read_u32(mod->load_base, DIV_MASK_STUB_OFFSET + 8) !=
          ARM64_B_COMMON_FROM_DIV ||
      read_u16(mod->load_base, dispatch_offset(WRITE8_AUXSPICNT_LOW)) !=
          DISPATCH_COMMON_STORE ||
      read_u16(mod->load_base, dispatch_offset(WRITE8_DIVCNT_LOW)) !=
          DISPATCH_AUX_MASK_STUB ||
      read_u16(mod->load_base, dispatch_offset(WRITE8_DIVCNT_LOW + 1)) !=
          DISPATCH_DIV_MASK_STUB ||
      memcmp((const char *)mod->load_base + CAPPED_FAST_FORWARD_CHECK,
             capped_fast_forward_code, sizeof(capped_fast_forward_code)) != 0 ||
      memcmp((const char *)mod->load_base + FIXED_SKIP_PHASE_CHECK,
             fixed_skip_phase_code, sizeof(fixed_skip_phase_code)) != 0 ||
      memcmp((const char *)mod->load_base + AUTO_SKIP_LIMIT_CHECK,
             auto_skip_limit_code, sizeof(auto_skip_limit_code)) != 0 ||
      memcmp((const char *)mod->load_base + THREADED_3D_SKIP_CHECK,
             threaded_3d_skip_code, sizeof(threaded_3d_skip_code)) != 0)
    return 0;

  for (unsigned offset = WRITE8_DIV_RESULT_FIRST;
       offset <= WRITE8_DIV_RESULT_LAST; offset++) {
    if (read_u16(mod->load_base, dispatch_offset(offset)) !=
        DISPATCH_AUX_MASK_STUB)
      return 0;
  }

  write_u32(mod->load_base, AUX_MASK_STUB_OFFSET, ARM64_AND_W20_7F);
  write_u16(mod->load_base, dispatch_offset(WRITE8_AUXSPICNT_LOW),
            DISPATCH_AUX_MASK_STUB);
  write_u16(mod->load_base, dispatch_offset(WRITE8_DIVCNT_LOW),
            DISPATCH_DIV_MASK_STUB);
  for (unsigned offset = WRITE8_DIV_RESULT_FIRST;
       offset <= WRITE8_DIV_RESULT_LAST; offset++) {
    write_u16(mod->load_base, dispatch_offset(offset),
              DISPATCH_DIV_MASK_STUB);
  }
  /* Use the configured count already in w25 for capped fast-forward. */
  write_u32(mod->load_base, CAPPED_FAST_FORWARD_CHECK + 4,
            0xd503201fu); /* nop */
  /* A smaller skip count can leave the old phase past the new limit. */
  write_u32(mod->load_base, FIXED_SKIP_PHASE_CHECK + 4,
            0x1a8897e8u); /* csinc w8, wzr, w8, ls */
  write_u32(mod->load_base, AUTO_SKIP_LIMIT_CHECK + 20,
            0x54000648u); /* b.hi skip */
  memcpy((char *)mod->load_base + THREADED_3D_SKIP_CHECK,
         threaded_3d_skip_patch, sizeof(threaded_3d_skip_patch));
  return 1;
}
