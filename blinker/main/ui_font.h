// Shared Badgemon UI fonts: UNSCII 16 default, UNSCII 8 for compact stats.
// Requires CONFIG_LV_FONT_UNSCII_8/16=y (see sdkconfig.defaults).
#pragma once

#include "sdkconfig.h"
#include "lvgl.h"

// LVGL only declares lv_font_unscii_* when the Kconfig enables them,
// so fall back to Montserrat when UNSCII is off (e.g. stale sdkconfig).
#ifdef CONFIG_LV_FONT_UNSCII_16
#define BADGE_FONT (&lv_font_unscii_16)
#else
#define BADGE_FONT (&lv_font_montserrat_14)
#endif

#ifdef CONFIG_LV_FONT_UNSCII_8
#define BADGE_FONT_SMALL (&lv_font_unscii_8)
#else
#define BADGE_FONT_SMALL (&lv_font_montserrat_14)
#endif

// badge_font(): BADGE_FONT plus a Montserrat 14 fallback chain, so
// LV_SYMBOL_* glyphs (e.g. LV_SYMBOL_UP on the shift key) still render
// even though UNSCII only covers ASCII. Safe to call more than once.
static inline const lv_font_t *badge_font(void) {
  static lv_font_t f;
  static bool ready = false;
  if (!ready) {
    f = *BADGE_FONT;
#ifdef CONFIG_LV_FONT_MONTSERRAT_14
    if (f.fallback == NULL) f.fallback = &lv_font_montserrat_14;
#endif
    ready = true;
  }
  return &f;
}
