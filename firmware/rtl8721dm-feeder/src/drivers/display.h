#pragma once
#include <Arduino.h>

// ===========================================================================
//  Dot-matrix display — two WS1625 (TM1629-family) chips, pseudo-I2C on a
//  shared clock + two data lines:  SCL=PA_13, SDA_LEFT=PA_14, SDA_RIGHT=PA_15
//  (PA_12 = connector "PWM", not a data line). 7 rows x 28 cols; x0,x1 are the
//  red alert columns. Origin (0,0) = bottom-left, x left->right, y bottom->top.
//
//  The (x,y)->(line,grid,bit) mapping (MAP_L table + line-R mirror + x27 split)
//  is ported VERBATIM from the pinmap explorer's verified xy2cell() — it was
//  solved empirically and confirmed dot-by-dot (Docs/DISPLAY_PROTOCOL.md). Do
//  not "simplify" it; there is no closed-form formula.
//
//  Used as an ALERT SURFACE: mostly dark, showing short messages on fault.
// ===========================================================================
namespace Display {
    void begin();
    void update();                  // advances the marquee if scrolling
    void on(uint8_t bri);           // brightness 0..7, display ON
    void off();
    void clear();                   // blank the panel (stays powered)
    void setPixel(int x, int y);    // light one (x,y) via the verified map
    void flush();                   // push framebuffer to both chips
    void showText(const char* s);   // static text from the left (fits ~4 chars)
    void scroll(const char* s);     // non-blocking marquee, loops until replaced/cleared
}
void displayInit();
