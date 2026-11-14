// Copyright 2026 by Frobenius Norm LLC 2026-11-02 12:44:27
//
// HD44780-over-PCF8574 character LCD -- implementation.  See LCD1602_I2C.h for the call
// surface this class owes its one caller, and for the datasheets it is written from.
// [B1047]

#if LL_LCD1602

#include "LCD1602_I2C.h"

/*! DDRAM address of column 0 of each line.  The HD44780 has two 40-byte halves, at 0x00
    and 0x40, and a four-line panel takes lines 3 and 4 from the tails of those halves at
    +0x14 -- 20 columns in, which is why the offsets suit a 20x4 and a 16x2 alike.  There
    is no fifth address to add: four entries is the hardware's limit, not a buffer size
    chosen here. */
static const uint8_t lcd_row_addr[] = { 0x00, 0x40, 0x14, 0x54 };

LCD1602_I2C::LCD1602_I2C(uint8_t lcd_addr, uint8_t lcd_cols, uint8_t lcd_rows)
{
  _addr            = lcd_addr;
  _cols            = lcd_cols ? lcd_cols : 16;
  _rows            = lcd_rows ? lcd_rows : 1;
  _displayfunction = LCD_4BITMODE | (_rows > 1 ? LCD_2LINE : LCD_1LINE) | LCD_5x8DOTS;
  _displaycontrol  = LCD_DISPLAYON;         //!< cursor and blink off, as before
  _backlightval    = 0;                     //!< dark until backlight(); the caller decides
}

void LCD1602_I2C::init()
{
  Wire.begin();

  /* The datasheet wants Vcc stable for more than 40 ms before the first instruction.
     THE SECOND AFTER THAT IS NOT FROM THE DATASHEET -- it is a conservative power-up wait,
     and it costs a second of boot on every env built with -DLL_LCD1602=1.  It is kept only
     because no panel has been available to retest a shorter one, and a wait shortened
     without a panel in front of you is a guess that ships.  50 ms is the figure to try.
     [B1047] */
  delay(50);
  expanderWrite(0);                         //!< park the expander: backlight off, EN low
  delay(1000);

  /* HD44780 Figure 24, the 4-bit initialisation.  The controller powers up expecting an
     8-bit bus and may be part-way through an instruction, so 0x3 goes out as a LONE
     NIBBLE three times to force it into a known 8-bit state, and 0x2 then switches the bus
     to 4 bits.  The waits are the datasheet minima (4.1 ms, 4.1 ms, 100 us) rounded up.
     send() cannot be used for these: it would follow each nibble with a second one that
     the controller is not yet listening for. */
  write4bits(0x03 << 4);
  delayMicroseconds(4500);
  write4bits(0x03 << 4);
  delayMicroseconds(4500);
  write4bits(0x03 << 4);
  delayMicroseconds(150);
  write4bits(0x02 << 4);

  command(LCD_FUNCTIONSET | _displayfunction);
  command(LCD_DISPLAYCONTROL | _displaycontrol);
  clear();
  command(LCD_ENTRYMODESET | LCD_ENTRYLEFT);
  home();
}

void LCD1602_I2C::clear()
{
  command(LCD_CLEARDISPLAY);
  delayMicroseconds(2000);                  //!< clear takes 1.52 ms and does not ack
}

void LCD1602_I2C::home()
{
  command(LCD_RETURNHOME);
  delayMicroseconds(2000);                  //!< likewise 1.52 ms
}

void LCD1602_I2C::noDisplay()
{
  _displaycontrol &= ~LCD_DISPLAYON;
  command(LCD_DISPLAYCONTROL | _displaycontrol);
}

void LCD1602_I2C::display()
{
  _displaycontrol |= LCD_DISPLAYON;
  command(LCD_DISPLAYCONTROL | _displaycontrol);
}

void LCD1602_I2C::noCursor()
{
  _displaycontrol &= ~LCD_CURSORON;
  command(LCD_DISPLAYCONTROL | _displaycontrol);
}

void LCD1602_I2C::cursor()
{
  _displaycontrol |= LCD_CURSORON;
  command(LCD_DISPLAYCONTROL | _displaycontrol);
}

void LCD1602_I2C::noBacklight()
{
  _backlightval = 0;
  expanderWrite(0);                         //!< push the bit; no instruction is involved
}

void LCD1602_I2C::backlight()
{
  _backlightval = LCD_PIN_BACKLIGHT;
  expanderWrite(0);
}

void LCD1602_I2C::setCursor(uint8_t col, uint8_t row)
{
  /* The bound is the SMALLER of the declared geometry and the row table, and the test is
     >= rather than >.  Both halves matter: `row > _rows` lets the first out-of-range row
     through, and clamping to the declared geometry alone still indexes a four-element
     table with whatever a caller declared -- so a panel declared with more than four rows
     would read past the table and send what it found there as a DDRAM address.  [B1050] */
  uint8_t maxrow = (_rows < max_rows) ? _rows : (uint8_t) max_rows;
  if (maxrow == 0) maxrow = 1;
  if (row >= maxrow) row = maxrow - 1;
  if (_cols && col >= _cols) col = _cols - 1;

  command(LCD_SETDDRAMADDR | (col + lcd_row_addr[row]));
}

size_t LCD1602_I2C::write(uint8_t value)
{
  send(value, LCD_PIN_RS);                  //!< RS high: this byte is character data
  return 1;
}

//! One byte to the expander.  The backlight bit rides every write, because the gate is one
//! of the expander's own pins -- drop it and the backlight blinks off on each character.
void LCD1602_I2C::expanderWrite(uint8_t data)
{
  Wire.beginTransmission(_addr);
  Wire.write((uint8_t) (data | _backlightval));
  Wire.endTransmission();
}

//! Latch whatever is already on the data lines: the HD44780 reads them on EN's falling
//! edge.  RW is never set -- it is tied low here and this driver never reads the panel.
void LCD1602_I2C::pulseEnable(uint8_t data)
{
  expanderWrite(data | LCD_PIN_EN);
  delayMicroseconds(1);                     //!< EN high for at least 450 ns
  expanderWrite(data & ~LCD_PIN_EN);
  delayMicroseconds(50);                    //!< at least 37 us for the instruction to run
}

void LCD1602_I2C::write4bits(uint8_t nibble)
{
  expanderWrite(nibble);
  pulseEnable(nibble);
}

//! A byte as two nibbles, high first.  The data lines are P4..P7, hence the shift.
void LCD1602_I2C::send(uint8_t value, uint8_t mode)
{
  write4bits((value & 0xf0) | mode);
  write4bits(((value << 4) & 0xf0) | mode);
}

void LCD1602_I2C::command(uint8_t value)
{
  send(value, 0);                           //!< RS low: this byte is an instruction
}

#endif
