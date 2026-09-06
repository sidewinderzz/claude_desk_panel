#pragma once

// Claude's palette, in RGB565 for the panel. Warm near-black ground, bone text,
// and the clay/orange accent doing the work.

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BG RGB565(0x1F, 0x1E, 0x1D)      // page ground
#define C_SURFACE RGB565(0x26, 0x26, 0x24) // header bar, cards
#define C_LINE RGB565(0x3A, 0x3A, 0x37)    // hairline borders, gauge track
#define C_BONE RGB565(0xF0, 0xEE, 0xE6)    // primary text
#define C_MUTED RGB565(0x8A, 0x87, 0x80)   // secondary text

#define C_ACCENT RGB565(0xD9, 0x77, 0x57)  // Claude clay — the default gauge colour
#define C_WARN RGB565(0xC2, 0x70, 0x3F)    // 70%+
#define C_HOT RGB565(0xA6, 0x3D, 0x2F)     // 90%+ and limit reached
#define C_LAVENDER RGB565(0xB4, 0x9F, 0xD8)
#define C_SKY RGB565(0x6A, 0x9B, 0xCC)
#define C_GOOD RGB565(0x7D, 0x9A, 0x6B)    // healthy status dot

// Severity ramp shared by the gauge and the bars.
static inline uint16_t severityColor(float pct)
{
  if (pct >= 90.0f)
    return C_HOT;
  if (pct >= 70.0f)
    return C_WARN;
  return C_ACCENT;
}
