// Copyright 2026 by Frobenius Norm LLC 2026-11-02 12:44:27
//
// HD44780 character LCD behind a PCF8574 I2C expander -- the "1602 I2C backpack" module.
// Written against the Hitachi HD44780U datasheet (instruction set, and the 4-bit
// initialisation in Figure 24), the NXP PCF8574 datasheet, and the module's own wiring.
//
// Original Frobenius Norm LLC work, implemented from the datasheets named above; it is not
// derived from any third-party library.  The instruction opcodes below are the HD44780's
// own, the bit assignments are how the module is wired, and the delays are the datasheet's
// minima -- all facts about hardware, which is why they necessarily agree with any other
// correct driver for this part.  [B1047]
//
// THE CALL SURFACE IS FIXED BY src/ll_xmop3_LCD1602.cpp, the only caller: the 3-argument
// constructor, init(), clear(), home(), display()/noDisplay(), cursor()/noCursor(),
// backlight()/noBacklight(), setCursor() and Print::print() via write().  Nothing else is
// defined here, deliberately -- add a method when a mop3 needs one, not before, so the
// surface stays something a reader can check against the caller.

#ifndef LCD1602_I2C_h
#define LCD1602_I2C_h

#if LL_LCD1602

#include <inttypes.h>
#include <Arduino.h>
#include <Print.h>
#include <Wire.h>

/*! HD44780 instructions, from the datasheet's instruction table.  Each is a command byte
    whose low bits are the option flags OR'd in below. */
#define LCD_CLEARDISPLAY   0x01
#define LCD_RETURNHOME     0x02
#define LCD_ENTRYMODESET   0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_FUNCTIONSET    0x20
#define LCD_SETDDRAMADDR   0x80

/*! Entry-mode flags: advance the cursor after each character, do not shift the display. */
#define LCD_ENTRYLEFT      0x02

/*! Display-control flags, bits of the LCD_DISPLAYCONTROL instruction. */
#define LCD_DISPLAYON      0x04
#define LCD_CURSORON       0x02
#define LCD_BLINKON        0x01

/*! Function-set flags.  4-bit bus is 0x00; this module has no 8-bit path to the HD44780,
    because the expander only has four data lines wired to it. */
#define LCD_4BITMODE       0x00
#define LCD_2LINE          0x08
#define LCD_1LINE          0x00
#define LCD_5x8DOTS        0x00

/*! PCF8574 port bits, i.e. how the backpack is WIRED -- a property of the module, not of
    either chip.  P4..P7 carry the data nibble, which is why send() shifts by four. */
#define LCD_PIN_RS         0x01    //!< P0: register select, 0 = instruction, 1 = data
#define LCD_PIN_RW         0x02    //!< P1: read/write, tied low here -- we never read
#define LCD_PIN_EN         0x04    //!< P2: enable, latches the nibble on its falling edge
#define LCD_PIN_BACKLIGHT  0x08    //!< P3: backlight gate

class LCD1602_I2C : public Print
{
protected:

  //! How many lines this controller can address, and therefore how many entries the row
  //! table in the .cpp has.  Four is the HD44780's own limit -- two 40-byte halves, each
  //! split in two on a 4-line panel -- not a buffer size chosen here.  setCursor() clamps
  //! to it, because the caller's declared geometry is not checked against the hardware.
  //! [B1050]
  static const int max_rows = 4;

  uint8_t _addr;                //!< I2C address of the expander (0x27 or 0x3F typically)
  uint8_t _cols;               //!< columns, as the caller declared them
  uint8_t _rows;               //!< rows, as the caller declared them
  uint8_t _displayfunction;    //!< bus width, line count, font: the FUNCTIONSET operand
  uint8_t _displaycontrol;     //!< display/cursor/blink: the DISPLAYCONTROL operand
  uint8_t _backlightval;       //!< LCD_PIN_BACKLIGHT or 0; OR'd into every expander write

  void expanderWrite(uint8_t data);   //!< one byte to the PCF8574, backlight bit included
  void pulseEnable(uint8_t data);     //!< latch the nibble already on the data lines
  void write4bits(uint8_t nibble);    //!< present a nibble and latch it
  void send(uint8_t value, uint8_t mode);   //!< a byte as two nibbles, mode = RS bit
  void command(uint8_t value);        //!< send() with RS low

public:

  LCD1602_I2C(uint8_t lcd_addr, uint8_t lcd_cols, uint8_t lcd_rows);

  //! Bring up the I2C bus and run the HD44780 4-bit initialisation.  Leaves the display
  //! on, cursor off, blink off, cursor advancing, screen cleared and the cursor home.
  void init();

  void clear();
  void home();

  void noDisplay();
  void display();
  void noCursor();
  void cursor();
  void noBacklight();
  void backlight();

  //! Move the cursor.  Both arguments are CLAMPED to the geometry the constructor was
  //! given, so an out-of-range row addresses the last real line instead of reading past
  //! the row table.  A display declared with more rows than the controller can address is
  //! the case that makes this matter.  [B1050]
  void setCursor(uint8_t col, uint8_t row);

  //! Print's one required primitive: everything print() does arrives here.
  virtual size_t write(uint8_t value);
};

#endif

#endif
