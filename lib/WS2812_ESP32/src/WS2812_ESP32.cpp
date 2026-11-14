// Copyright 2026 by Frobenius Norm LLC 2026-11-02 12:44:27
//
// WS2812/WS2812B strip driver -- implementation.  See WS2812_ESP32.h for the call surface
// this class owes its two callers, and for the channel-order contract it shares with
// scm/core/WS2812.scm.  [B1047]

#if LL_WS2812

#include "WS2812_ESP32.h"

/* WS2812B BIT WAVEFORM, from the part's datasheet.  Every bit is one RMT symbol: a high
   pulse then a low pulse, and the RATIO is what carries the value.

       bit 0:  T0H 0.40 us high, T0L 0.85 us low      (+/- 150 ns on each)
       bit 1:  T1H 0.80 us high, T1L 0.45 us low
       latch:  low for >= 50 us

   The channel is clocked at 10 MHz, so one tick is 100 ns and the durations below are in
   ticks.  T0L is sent as 800 ns rather than the nominal 850: that is inside the datasheet
   tolerance, makes every bit exactly 1.2 us, and is what has been driving the strips on
   this hardware all along -- do not "correct" it without a scope.

   NO LATCH SYMBOL IS EMITTED, deliberately.  The line idles low when the transfer ends and
   the next show() is always more than 50 us behind this one (the Scheme layer reaches it
   through the behaviour queue), so the gap satisfies the reset by construction.  A caller
   that hammered show() back to back in C++ would need one. */
#define WS2812_T0H 4
#define WS2812_T0L 8
#define WS2812_T1H 8
#define WS2812_T1L 4

/* How much channel RAM to reserve.  The enumerators RMT_MEM_NUM_BLOCKS_n are literally n,
   but only exist up to the SoC's limit, so the cap has to be guarded the same way the
   core's header guards the enum. */
#if SOC_RMT_TX_CANDIDATES_PER_GROUP > 4
#define WS2812_MAX_BLOCKS 8
#elif SOC_RMT_TX_CANDIDATES_PER_GROUP > 2
#define WS2812_MAX_BLOCKS 4
#else
#define WS2812_MAX_BLOCKS 2
#endif

WS2812::WS2812(uint16_t n /*= 8*/, uint8_t pin_gpio /*= 2*/, uint8_t chn /*= 0*/,
               LED_TYPE t /*= TYPE_GRB*/)
{
  ledCounts = n;
  pin       = pin_gpio;
  rmt_chn   = chn;
  br        = 255;
  started   = false;
  setLedType(t);
}

void WS2812::setLedType(LED_TYPE t)
{
  rOffset = (t >> 4) & 0x03;
  gOffset = (t >> 2) & 0x03;
  bOffset = t & 0x03;
}

void WS2812::setBrightness(uint8_t brightness)
{
  br = brightness;                      //!< a uint8_t is already 0-255; nothing to clamp
}

bool WS2812::begin()
{
  started = false;
  if (ledCounts == 0 || ledCounts > LL_WS2812_MAX_LEDS) return false;

  /* The parameter is named for a channel because that is what the Scheme API calls it, but
     core 3.x allocates the channel itself from the PIN and takes a RESERVATION SIZE here.
     The vendored version mapped channel n to n+1 memory blocks via a 45-line switch; this
     is that same mapping, clamped to what the SoC has.  Channel 0 -- the only value this
     tree ever passes (scm/core/WS2812.scm sets ledchan 0) -- means one block, which is
     also what the core's own neopixelWrite() reserves. */
  int blocks = rmt_chn + 1;
  if (blocks > WS2812_MAX_BLOCKS) blocks = WS2812_MAX_BLOCKS;

  if (!rmtInit(pin, RMT_TX_MODE, (rmt_reserve_memsize_t) blocks, 10000000)) return false;

  for (int i = 0; i < ledCounts; i++) writeBits(i, 0, 0, 0);
  started = true;
  return true;
}

//! Lay 24 symbols down for one LED, most significant bit of the first wire byte first.
void WS2812::writeBits(int index, uint8_t b0, uint8_t b1, uint8_t b2)
{
  uint32_t    bits = ((uint32_t) b0 << 16) | ((uint32_t) b1 << 8) | b2;
  rmt_data_t *sym  = &led_data[index * bits_per_led];

  for (int bit = 0; bit < bits_per_led; bit++) {
    bool one = (bits >> (bits_per_led - 1 - bit)) & 1;
    sym[bit].level0    = 1;
    sym[bit].duration0 = one ? WS2812_T1H : WS2812_T0H;
    sym[bit].level1    = 0;
    sym[bit].duration1 = one ? WS2812_T1L : WS2812_T0L;
  }
}

esp_err_t WS2812::setLedColorData(int index, uint32_t rgb)
{
  return setLedColorData(index, (uint8_t) (rgb >> 16), (uint8_t) (rgb >> 8), (uint8_t) rgb);
}

esp_err_t WS2812::setLedColorData(int index, uint8_t r, uint8_t g, uint8_t b)
{
  /* THE BOUND IS THE BUFFER, NOT ledCounts, AND THAT IS DELIBERATE.  Checking against the
     strip length would be tighter and would also change behaviour: scm/core/WS2812.scm's
     setAll walks 0..Nleds INCLUSIVE, so it writes one LED past the end on every call and
     relies on that write being harmless.  It is harmless -- show() clocks out only
     ledCounts LEDs -- so this check exists to stop the write that is NOT: an index past
     the array itself, which corrupts the heap and faults nowhere near here.  [B1049] */
  if (index < 0 || index >= LL_WS2812_MAX_LEDS) return ESP_ERR_INVALID_ARG;

  uint8_t wire[3];
  wire[rOffset] = (uint8_t) (r * br / 255);
  wire[gOffset] = (uint8_t) (g * br / 255);
  wire[bOffset] = (uint8_t) (b * br / 255);

  writeBits(index, wire[0], wire[1], wire[2]);
  return ESP_OK;
}

esp_err_t WS2812::show()
{
  if (!started) return ESP_FAIL;
  /* RETURNS ESP_OK (0) ON SUCCESS.  rmtWrite() answers a bool, and the vendored version
     returned it straight out of an esp_err_t function, so a SUCCESSFUL show() used to
     answer 1 -- which is ESP_FAIL's magnitude, and the opposite of what the published
     manual says at src/ll_xmop3_WS2812.cpp ("returns 0 on success").  Nothing in scm/ ever
     read the value, so this now matches the documentation instead of contradicting it. */
  if (!rmtWrite(pin, led_data, ledCounts * bits_per_led, RMT_WAIT_FOR_EVER)) return ESP_FAIL;
  return ESP_OK;
}

uint32_t WS2812::Wheel(byte pos)
{
  /* Three-segment linear hue ramp: red falls while green rises, then green falls while
     blue rises, then blue falls while red rises.  The modulus is 255, not 256, so the
     cycle has 255 steps and positions 0 and 255 are the same colour -- that is the
     long-standing behaviour of this call and the Scheme layer is written against it. */
  uint32_t p = pos % 0xff;
  uint32_t r = 0, g = 0, b = 0;

  if (p < 85) {
    r = 255 - p * 3;
    g = p * 3;
  }
  else if (p < 170) {
    p -= 85;
    g = 255 - p * 3;
    b = p * 3;
  }
  else {
    p -= 170;
    b = 255 - p * 3;
    r = p * 3;
  }
  return (r << 16) | (g << 8) | b;
}

#endif
