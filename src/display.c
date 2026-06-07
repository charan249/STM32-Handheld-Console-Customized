#include "display.h"

#include <ctype.h>

#include "delay.h"
#include "gpio.h"

// CS: B12
// SCK: B13
// A0: A8
// SDA: B15
// RST: B14

#define SPI_CLK (1 << 14)
#define SPI2_START 0x40003800
#define SPI2_CR1 (*((volatile uint32_t *)(SPI2_START + 0x00)))
#define SPI2_CR2 (*((volatile uint32_t *)(SPI2_START + 0x04)))
#define SPI2_SR (*((volatile uint32_t *)(SPI2_START + 0x08)))
#define SPI2_DR (*((volatile uint32_t *)(SPI2_START + 0x0C)))

#define A0 (1 << 8)
#define RST (1 << 14)

#define SET_PAGE_ADDR 0xB0
#define SET_COL_ADDR_MSB 0x10
#define SET_COL_ADDR_LSB 0x0
#define DISPLAY_ON 0xAF
#define DISPLAY_OFF 0xAE

#define NUM_COLS 128
#define NUM_PAGES 8

#define CHAR_WIDTH 5
#define CHAR_HEIGHT 5

static const uint8_t NUM_FONT[][CHAR_WIDTH] = {
    {0x0E, 0x11, 0x11, 0x11, 0x0E},  // 0
    {0x00, 0x01, 0x1F, 0x00, 0x00},  // 1
    {0x12, 0x19, 0x15, 0x12, 0x00},  // 2
    {0x11, 0x15, 0x15, 0x15, 0x1F},  // 3
    {0x07, 0x04, 0x04, 0x04, 0x1F},  // 4
    {0x16, 0x15, 0x15, 0x15, 0x1D},  // 5
    {0x1F, 0x15, 0x15, 0x15, 0x1D},  // 6
    {0x11, 0x09, 0x05, 0x03, 0x00},  // 7
    {0x1B, 0x15, 0x15, 0x1B, 0x00},  // 8
    {0x17, 0x15, 0x15, 0x1F, 0x00}   // 9
};

static const uint8_t ALPHA_FONT[][CHAR_WIDTH] = {
    {0x1F, 0x05, 0x05, 0x05, 0x1F},  // A
    {0x1F, 0x15, 0x15, 0x15, 0x0E},  // B
    {0x1F, 0x11, 0x11, 0x11, 0x11},  // C
    {0x1F, 0x11, 0x11, 0x11, 0x0E},  // D
    {0x1F, 0x15, 0x15, 0x15, 0x11},  // E
    {0x1F, 0x05, 0x05, 0x05, 0x01},  // F
    {0x1F, 0x11, 0x15, 0x15, 0x1D},  // G
    {0x1F, 0x04, 0x04, 0x04, 0x1F},  // H
    {0x11, 0x11, 0x1F, 0x11, 0x11},  // I
    {0x19, 0x11, 0x1F, 0x01, 0x00},  // J
    {0x1F, 0x04, 0x0A, 0x11, 0x00},  // K
    {0x1F, 0x10, 0x10, 0x10, 0x00},  // L
    {0x1F, 0x02, 0x04, 0x02, 0x1F},  // M
    {0x1F, 0x02, 0x04, 0x08, 0x1F},  // N
    {0x1F, 0x11, 0x11, 0x11, 0x1F},  // O
    {0x1F, 0x05, 0x05, 0x05, 0x07},  // P
    {0x1F, 0x11, 0x11, 0x19, 0x1F},  // Q
    {0x1F, 0x05, 0x05, 0x0D, 0x17},  // R
    {0x17, 0x15, 0x15, 0x15, 0x1D},  // S
    {0x01, 0x01, 0x1F, 0x01, 0x01},  // T
    {0x1F, 0x10, 0x10, 0x10, 0x1F},  // U
    {0x03, 0x0C, 0x10, 0x0C, 0x03},  // V
    {0x1F, 0x10, 0x08, 0x10, 0x1F},  // W
    {0x11, 0x0A, 0x04, 0x0A, 0x11},  // X
    {0x01, 0x02, 0x1C, 0x02, 0x01},  // Y
    {0x11, 0x19, 0x15, 0x13, 0x11}   // Z
};

static const uint8_t NO_CHAR[][CHAR_WIDTH] = {{0}};

static const uint8_t LEFT_ARROW[] = {0x00, 0x00, 0x04, 0x0E, 0x1F};
static const uint8_t RIGHT_ARROW[] = {0x1F, 0x0E, 0x04, 0x00, 0x00};

static const uint8_t *_char_to_bits(char c) {
    switch (c) {
        case '<':
            return LEFT_ARROW;
        case '>':
            return RIGHT_ARROW;
    }

    c = toupper(c);
    if (c >= '0' && c <= '9') {
        return NUM_FONT[c - '0'];
    } else if (c >= 'A' && c <= 'Z') {
        return ALPHA_FONT[c - 'A'];
    } else {
        return NO_CHAR[0];
    }
}

static void _gpio_init(void) {
    // FORCE PORT B PERIPHERAL CLOCK ON (Critical for bit-banging ODR)
    *((volatile uint32_t *)0x40021018) |= (1 << 3); // Enables IOPBEN clock bit
    
    // 1. Clear configuration bits for the pins we are using
    GPIOA_CRH &= ~(0xF << 0);
    GPIOB_CRH &= ~(0xFFFF << 16);

    // 2. Set MODEy bits to 2MHz Output mode (0x2)
    GPIOA_CRH |= (0x2 << 0);   // PA8 (A0 / DC)
    GPIOB_CRH |= (0x2 << 16);  // PB12 (CS)     
    GPIOB_CRH |= (0x2 << 20);  // PB13 (SCK)    
    GPIOB_CRH |= (0x2 << 24);  // PB14 (RST)    
    GPIOB_CRH |= (0x2 << 28);  // PB15 (SDA)    

    // 3. Set CNFy bits 
    // Leaving PB13, PB14, and PB15 at 0x0 makes them standard General Purpose Outputs
    GPIOB_CRH |= (0x2 << 18);  // Only keep PB12 as an Alternate Function if needed
}

static void _spi_init(void) {
    // Intentionally left blank to bypass hardware SPI configuration
}

static void _display_write(uint8_t data) {
    // Software Bit-Banging loop (SPI Mode 0 compliant)
    for (int i = 0; i < 8; i++) {
        // 1. Set the Data pin (PB15) based on the highest bit (MSB)
        if (data & 0x80) {
            GPIOB_ODR |= (1 << 15);  // SDA HIGH
        } else {
            GPIOB_ODR &= ~(1 << 15); // SDA LOW
        }

        // 2. Pulse the Clock pin (PB13)
        GPIOB_ODR &= ~(1 << 13); // SCK LOW (Idle state for Mode 0)
        for (volatile int d = 0; d < 3; d++); // Brief setup delay
        
        GPIOB_ODR |= (1 << 13);  // SCK HIGH (OLED reads data bit here)
        for (volatile int d = 0; d < 3; d++); // Brief hold delay
        
        data <<= 1; // Shift to the next bit
    }
    // Return clock to idle low state
    GPIOB_ODR &= ~(1 << 13);
}

static void _display_putc(uint8_t x, uint8_t y, char c) {
    display_send_cmd(SET_COL_ADDR_MSB | (x >> 4));
    display_send_cmd(SET_COL_ADDR_LSB | (x & 0x0F));

    for (int i = 0; i < CHAR_WIDTH; i++) {
        display_send_data(_char_to_bits(c)[i]);
    }
}

void display_init(void) {
    _gpio_init();

    // Hardware Reset the display
    GPIOB_ODR &= ~RST;
    delay(50);             
    GPIOB_ODR |= RST;
    delay(50);

    _spi_init();

    // --- Send SSD1306 OLED Startup Commands ---
    display_send_cmd(0xAE); // Turn display off
    display_send_cmd(0xD5); // Set Display Clock Divide Ratio
    display_send_cmd(0x80); 
    display_send_cmd(0xA8); // Set Multiplex Ratio
    display_send_cmd(0x3F); // 1/64 duty cycle
    display_send_cmd(0xD3); // Set Display Offset
    display_send_cmd(0x00); // No offset
    display_send_cmd(0x40); // Set Start Line to 0
    display_send_cmd(0x8D); // Charge Pump Control
    display_send_cmd(0x14); // ENABLE Internal Charge Pump (Crucial!)
    display_send_cmd(0x20); // Set Memory Addressing Mode
    display_send_cmd(0x02); // Page Addressing Mode
    display_send_cmd(0xA1); // Segment Re-map (Flips horizontally)
    display_send_cmd(0xC8); // COM Output Scan Direction (Flips vertically)
    display_send_cmd(0xDA); // COM Pins Hardware Configuration
    display_send_cmd(0x12);
    display_send_cmd(0x81); // Set Contrast
    display_send_cmd(0xCF); 
    display_send_cmd(0xD9); // Set Pre-charge Period
    display_send_cmd(0xF1);
    display_send_cmd(0xDB); // Set VCOMH Deselect Level
    display_send_cmd(0x40);
    display_send_cmd(0xA4); // Entire Display On
    display_send_cmd(0xA6); // Set Normal Display Mode

    display_clear();        // Wipes black frame buffer memory
    display_send_cmd(0xAF); // Turn display ON

    // --- DIAGNOSTIC HARDWARE TEST LOCK ---
    // This fills screen pixels with 0xFF (solid color)
   // display_test(); 

    // Enable clock for GPIOC (PC13 status LED)
   // *((volatile uint32_t *)0x40021018) |= (1 << 4); 
    // Configure PC13 as standard 2MHz Output
   // *((volatile uint32_t *)0x40011004) &= ~(0xF << 20); 
   // *((volatile uint32_t *)0x40011004) |= (0x2 << 20);  

    // Trap execution here to blink PC13 indefinitely for validation
   // while (1) {
     //   *((volatile uint32_t *)0x4001100C) ^= (1 << 13); 
   // delay(500); 
    //}
}

void display_send_data(uint8_t data) {
    GPIOA_ODR |= A0;
    _display_write(data);
}

void display_send_cmd(uint8_t cmd) {
    GPIOA_ODR &= ~A0;
    _display_write(cmd);
}

void display_clear(void) {
    for (int y = 0; y < NUM_PAGES; y++) {
        display_send_cmd(SET_PAGE_ADDR | y);
        display_send_cmd(0x02); // Lower Column Address offset for SH1106 compatibility
        display_send_cmd(SET_COL_ADDR_MSB);

        for (int x = 0; x < NUM_COLS; x++) {
            display_send_data(0x00);
        }
    }
}

void display_draw(uint8_t buf[64][16]) {
    for (int y = 0; y < NUM_PAGES; y++) {
        display_send_cmd(SET_PAGE_ADDR | y);
        display_send_cmd(SET_COL_ADDR_MSB);
        display_send_cmd(SET_COL_ADDR_LSB);

        for (int x = 0; x < NUM_COLS; x++) {
            int row = NUM_PAGES * y;
            uint8_t data = 0;

            for (int i = 0; i < 8; i++) {
                uint8_t bit = (buf[row + i][x / 8]) & (1 << (7 - (x % 8)));

                if (bit) {
                    data |= (1 << i);
                }
            }

            display_send_data(data);
        }
    }
}

void display_print(uint8_t x, uint8_t y, const char *str) {
    display_send_cmd(SET_PAGE_ADDR | y);

    for (int i = 0; str[i] != 0; i++) {
        _display_putc(x + (i * (CHAR_WIDTH + 1)), y, str[i]);
    }
}

void display_test(void) {
    for (int y = 0; y < NUM_PAGES; y++) {
        display_send_cmd(SET_PAGE_ADDR | y);
        display_send_cmd(0x02); // Lower Column Address offset for SH1106 compatibility
        display_send_cmd(SET_COL_ADDR_MSB);

        for (int x = 0; x < NUM_COLS; x++) {
            display_send_data(0xFF);
        }
    }
}

void display_font_test(void) {
    display_print(35, 3, "0123456789");
    display_print(25, 4, "ABCDEFGHIJKLM");
    display_print(25, 5, "NOPQRSTUVWXYZ");
    delay(5000);
}