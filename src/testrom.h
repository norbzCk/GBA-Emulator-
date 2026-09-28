#ifndef GBA_TESTROM_H
#define GBA_TESTROM_H

#include <stdint.h>

/* Size of the synthetic boot ROM built by test_rom_build(). */
#define TEST_ROM_SIZE 512

/* Assemble a small ARM program into a GamePak ROM image.  The program sets up
 * a paletted bitmap, fills it with a known pattern, waits for VBlank with the
 * BIOS VBlankIntrWait service and then writes a marker word, so a test can
 * check that the whole machine came up. */
void test_rom_build(uint8_t *rom, uint32_t size);

/* The 32-bit color the renderer produces for palette entry `value`. */
uint32_t test_rom_expected_color(uint32_t value);

#endif
