// Copyright 2026 by Frobenius Norm LLC 2026-11-02 12:44:27
//
// WS2812 / WS2812B ("NeoPixel") strip driver for the ESP32 family, written against the
// Arduino-ESP32 core 3.x public RMT API (cores/esp32/esp32-hal-rmt.h) and the WS2812B
// datasheet bit timings.  The RMT peripheral does all the work: we hand it one 32-bit
// symbol per data bit and it clocks the waveform out without CPU involvement.
//
// Original Frobenius Norm LLC work.  The waveform comes from the WS2812B datasheet and the
// API from the Arduino core's own header; it is not derived from any third-party library.
// The bit timings and the channel-order encoding below necessarily agree with any other
// correct driver for this part, because they are facts about the hardware.  [B1047]
//
// THE CALL SURFACE IS FIXED BY TWO FILES, SO IT IS NOT A MATTER OF TASTE:
//   src/ll_xmop3_WS2812.cpp -- the only C++ caller.  It uses the 4-arg constructor,
//        begin(), setBrightness(), both setLedColorData() overloads, show() and Wheel().
//        Nothing else is defined here, deliberately: add a method when a mop3 needs one,
//        not before, so the surface stays something a reader can check against the caller.
//   scm/core/WS2812.scm -- passes the LED type to (WS2812.begin ...) as a PLAIN INTEGER
//        and keeps its own copy of the six constants, with #x12 (GRB) as the default.
//        The six values below are therefore a cross-file contract: change one and the
//        Scheme layer sends a channel order that no longer means what it says.

#ifndef WS2812_ESP32_h
#define WS2812_ESP32_h

#if LL_WS2812

#include <Arduino.h>
#include "esp32-hal.h"
#include "esp_arduino_version.h"

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "WS2812_ESP32 requires Arduino-ESP32 core 3.x: rmtInit() takes a frequency and \
rmtWrite() is pin-addressed.  Core 2.x used rmt_obj_t*/rmtSetTick() and is not supported."
#endif

/*! Channel order, encoded as three 2-bit BYTE OFFSETS packed into one byte:
    bits 5:4 = where red goes, bits 3:2 = green, bits 1:0 = blue.  So TYPE_GRB = 0x12 =
    0b01_00_10 means "red at offset 1, green at offset 0, blue at offset 2", i.e. the
    wire order is G,R,B -- which is what an ordinary WS2812B expects.
    These six values are duplicated in scm/core/WS2812.scm; keep them identical. */
enum LED_TYPE
{                     // R  G  B
  TYPE_RGB = 0x06,    // 00 01 10
  TYPE_RBG = 0x09,    // 00 10 01
  TYPE_GRB = 0x12,    // 01 00 10
  TYPE_GBR = 0x21,    // 10 00 01
  TYPE_BRG = 0x18,    // 01 10 00
  TYPE_BGR = 0x24     // 10 01 00
};

/*! Strip capacity in LEDs.  The symbol buffer is an INSTANCE MEMBER, so it is allocated
    with the object and costs LL_WS2812_MAX_LEDS * 24 * sizeof(rmt_data_t) bytes -- 24,576 B
    at the default 256, for a strip this tree configures as 12 LEDs (864 B of it used).
    `src/ll_xmop3_WS2812.cpp` creates the driver with `new`, and 24 KB is far above the
    1 KB internal-DRAM preference set by heap_caps_malloc_extmem_enable(), so on a PSRAM
    part the buffer lives in PSRAM and the RMT driver reads it from there.  That is the
    existing arrangement and it works; it is recorded here because it is not what a reader
    assumes of a peripheral buffer.  Override with -DLL_WS2812_MAX_LEDS=<n> for a longer
    strip, or a smaller one to get the space back. */
#ifndef LL_WS2812_MAX_LEDS
#define LL_WS2812_MAX_LEDS 256
#endif

class WS2812
{
protected:

  static const int bits_per_led = 24;                                 //!< GRB, 8 bits each
  static const int max_symbols  = LL_WS2812_MAX_LEDS * bits_per_led;

  uint16_t ledCounts;           //!< LEDs in the strip, as begin() accepted it
  uint8_t  pin;                 //!< data GPIO
  uint8_t  br;                  //!< brightness scale, 0-255, applied on setLedColorData
  uint8_t  rmt_chn;             //!< caller's channel index; selects the reserved memsize

  uint8_t  rOffset;             //!< byte position of red on the wire, from LED_TYPE
  uint8_t  gOffset;             //!< ... green
  uint8_t  bOffset;             //!< ... blue

  bool     started;             //!< begin() succeeded; show() is a no-op until it has

  rmt_data_t led_data[max_symbols];   //!< one symbol per data bit, in wire order

  void setLedType(LED_TYPE t);        //!< decode LED_TYPE into the three offsets
  void writeBits(int index, uint8_t b0, uint8_t b1, uint8_t b2);

public:

  WS2812(uint16_t n = 8, uint8_t pin_gpio = 2, uint8_t chn = 0, LED_TYPE t = TYPE_GRB);

  //! Claim the RMT channel and fill the buffer with "all bits zero".  False if the pin
  //! could not be initialised, or if n exceeds LL_WS2812_MAX_LEDS.
  bool begin();

  void setBrightness(uint8_t brightness);

  //! Both overloads scale by brightness, reorder to the wire order, and write 24 symbols.
  //! They return ESP_ERR_INVALID_ARG for an index outside the buffer and touch nothing.
  //! The index reaches here straight from Scheme, so it is validated rather than trusted:
  //! an unchecked index is a write past a heap object, which faults nowhere near the call.
  //! [B1049]
  esp_err_t setLedColorData(int index, uint32_t rgb);
  esp_err_t setLedColorData(int index, uint8_t r, uint8_t g, uint8_t b);

  //! Clock the buffer out to the strip.  Blocks until the transfer completes; returns
  //! ESP_OK, or ESP_FAIL if the RMT write failed or begin() never succeeded.
  esp_err_t show();

  //! Position 0-254 to a packed 0xRRGGBB on a three-segment linear hue ramp.
  uint32_t Wheel(byte pos);
};

#endif

#endif
