#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "buttons.h"
#include "chip8.h"
#include "clock.h"
#include "delay.h"
#include "display.h"
#include "gpio.h"
#include "led.h"
#include "pwm.h"
#include "sd.h"
#include "sysclk.h"
#include "uart.h"

#define MAX_ROMS 25
#define SAVE_ADDR 0x0800FC00 
#define FLASH_KEYR (*(volatile uint32_t*)0x40022004)
#define FLASH_SR   (*(volatile uint32_t*)0x4002200C)
#define FLASH_CR   (*(volatile uint32_t*)0x40022010)
#define FLASH_AR   (*(volatile uint32_t*)0x40022014)       // Aligned with the 5x5 Grid Tool matrix parameters

// Emulator core assets
CHIP8 chip8;
uint8_t metadata[SD_BLOCK_SIZE] = {0};
char title[11];
uint32_t cpu_freq = CPU_FREQ_DEFAULT;
uint32_t timer_freq = TIMER_FREQ_DEFAULT;
uint32_t refresh_freq = REFRESH_FREQ_DEFAULT;
bool quirks[NUM_QUIRKS] = {0, 0, 0, 0, 0, 0, 0, 0};
uint16_t BTN_LEFT_MAP = 0x90;
uint16_t BTN_RIGHT_MAP = 0x240;
uint16_t BTN_UP_MAP = 0x20;
uint16_t BTN_DOWN_MAP = 0x100;
uint16_t BTN_A_MAP = 0x00;
uint16_t BTN_B_MAP = 0x40;
bool play_sound = false;
int rom_num = 0;

// Dynamic sector tracking variable for boot scanning operations
uint32_t discovered_sd_offset = 0; 

// Simple visual startup routine
void flash_unlock(void) {
    FLASH_KEYR = 0x45670123;
    FLASH_KEYR = 0xCDEF89AB;
}

void flash_lock(void) {
    FLASH_CR |= (1 << 7);
}

void flash_erase_page(uint32_t address) {

    FLASH_CR |= (1 << 1);

    FLASH_AR = address;

    FLASH_CR |= (1 << 6);

    while (FLASH_SR & 1);

    FLASH_CR &= ~(1 << 1);
}

void flash_write_word(uint32_t address, uint32_t data) {

    FLASH_CR |= (1 << 0);

    *(volatile uint16_t*)address = data & 0xFFFF;
    while (FLASH_SR & 1);

    *(volatile uint16_t*)(address + 2) = data >> 16;
    while (FLASH_SR & 1);

    FLASH_CR &= ~(1 << 0);
}

uint32_t flash_read_word(uint32_t address) {
    return *(volatile uint32_t*)address;
}
void show_splash(void) {
    display_print(37, 4, "CHIP N GO");

    for (int i = 0; i < 10; i++) {
        pwm_start();
        delay(100);
        pwm_stop();
        delay(100);
    }

    display_clear();
}

void btn_to_key(uint16_t btn_map, CHIP8K action) {
    for (int i = 0; i < 16; i++) {
        if (btn_map & 1)
            chip8.keypad[i] = action;
        btn_map >>= 1;
    }
}

void parse_quirks(uint8_t q) {
    for (int i = 0; i < 8; i++) {
        quirks[i] = q & 1;
        q >>= 1;
    }
}

// Spin up core emulation runtime architectures
bool init_emulator(void) {
    chip8_init(&chip8, cpu_freq, timer_freq, refresh_freq, PC_START_ADDR_DEFAULT,
               quirks, metadata, rom_num);
               
    chip8_load_font(&chip8);

    // Multi-slot configuration step: Game binaries follow metadata exactly 1 sector step later
    uint32_t game_code_sector = discovered_sd_offset + (rom_num * 8) + 1;
    
    sd_read_block(game_code_sector, chip8.RAM + PC_START_ADDR_DEFAULT);

    return true;
}

void load_rom(int rom_num) {
    // Multi-slot configuration step: Shift read target by sets of 8 sectors
    uint32_t rom_sector_addr = discovered_sd_offset + (rom_num * 8);
    sd_read_block(rom_sector_addr, metadata);
}

bool process_metadata(void) {
    // Verify our unique game cartridge verification key signature byte at Index 0
    if (metadata[0] == 0xC8) {
        
        // Clear buffer array, securely copy title up to 10 characters max
        memset(title, 0, sizeof(title));
        strncpy(title, (char *)(&metadata[1]), 10);
        title[10] = '\0'; 

        // Handle string corruption fallback protection loops
        if (title[0] < 32 || title[0] > 126) {
            sprintf(title, "GAME %02d", rom_num + 1);
        }

        // Set up operating frequencies parsed from the custom tool structure
        cpu_freq = (metadata[12] << 24) | (metadata[13] << 16) | (metadata[14] << 8) | metadata[15];
        timer_freq = metadata[16];
        refresh_freq = metadata[17];

        // Safe operational fallbacks if custom parameters turn up empty
        if (cpu_freq == 0) cpu_freq = 500;
        if (timer_freq == 0) timer_freq = 60;
        if (refresh_freq == 0) refresh_freq = 60;

        // Parse custom configurations straight from file parameters
        parse_quirks(metadata[18]);
        
        // Recover custom layout mappings safely from structure arrays
        BTN_LEFT_MAP  = (metadata[19] << 8) | metadata[20];
        BTN_RIGHT_MAP = (metadata[21] << 8) | metadata[22];
        BTN_UP_MAP    = (metadata[23] << 8) | metadata[24];
        BTN_DOWN_MAP  = (metadata[25] << 8) | metadata[26];
        BTN_A_MAP     = (metadata[27] << 8) | metadata[28];
        BTN_B_MAP     = (metadata[29] << 8) | metadata[30];

        return true; 
    }

    return false; 
}

bool seek_rom(int dir) {
    int attempts = 0;

    do {
        rom_num += dir;

        // Force rigid boundary wrapping constraints across our 25 grid indexes
        if (rom_num >= MAX_ROMS) {
            rom_num = 0;
        } else if (rom_num < 0) {
            rom_num = MAX_ROMS - 1;
        }

        load_rom(rom_num);

        if (process_metadata()) {
            return true; 
        }

        attempts++;
    } while (attempts < MAX_ROMS);

    return false; 
}

void select_rom(void) {
    display_clear();
    display_print(12, 4, "SCANNING CARTRIDGE...");

    bool found_valid_start = false;
    discovered_sd_offset = flash_read_word(SAVE_ADDR);

if (discovered_sd_offset != 0xFFFFFFFF) {

    sd_read_block(discovered_sd_offset, metadata);

    if (metadata[0] == 0xC8) {
        found_valid_start = true;
    }
}
if (!found_valid_start) {
    
    // EXTENDED: Sweep deeper across partitions to catch stashed clusters
    for (uint32_t sector = 0; sector < 20000; sector++) {
        sd_read_block(sector, metadata);
        if (metadata[0] == 0xC8) {
            discovered_sd_offset = sector; // Dynamic anchor lock achieved
            flash_unlock();
           flash_erase_page(SAVE_ADDR);
            flash_write_word(SAVE_ADDR, sector);
           flash_lock();
            found_valid_start = true;
            break;
        }
    }}

    // Hang system and display errors if the tracking signatures are missing completely
    if (!found_valid_start) {
        display_clear();
        display_print(16, 4, "CARTRIDGE READ ERR");
        while (1);
    }

    // Reset loop parameters cleanly to slot index 0
    rom_num = 0; 
    load_rom(rom_num);
    bool rom_exists = process_metadata();

    while (rom_exists) {
        display_clear();
        display_print(36 + ((10 - strlen(title)) * 2), 3, title);
        display_print(2, 4, "<                 >");
        display_print(19, 5, "PRESS A TO PLAY");

        int scan_dir = 0;
        while (!scan_dir) {
            if (btn_released(BTN_A)) {
                pwm_start();
                delay(500);
                pwm_stop();
                return; // Boot chosen game!
            } else if (btn_released(BTN_RIGHT)) {
                scan_dir = 1;
            } else if (btn_released(BTN_LEFT)) {
                scan_dir = -1;
            }
        }

        rom_exists = seek_rom(scan_dir);
    }

    display_clear();
    display_print(25, 4, "ROM NOT FOUND");
    while (1);
}

void draw_display(void) {
    display_draw(chip8.display);
}

void handle_sound(void) {
    if (!play_sound && chip8.beep) {
        pwm_start();
        play_sound = true;
    } else if (play_sound && !chip8.beep) {
        pwm_stop();
        play_sound = false;
    }
}

void handle_display(void) {
    if (chip8.display_updated) {
        draw_display();
    }
}

void handle_input(void) {
    if (btn_pressed(BTN_LEFT))   btn_to_key(BTN_LEFT_MAP, KEY_DOWN);
    if (btn_pressed(BTN_UP))     btn_to_key(BTN_UP_MAP, KEY_DOWN);
    if (btn_pressed(BTN_DOWN))   btn_to_key(BTN_DOWN_MAP, KEY_DOWN);
    if (btn_pressed(BTN_RIGHT))  btn_to_key(BTN_RIGHT_MAP, KEY_DOWN);
    if (btn_pressed(BTN_A))      btn_to_key(BTN_A_MAP, KEY_DOWN);
    if (btn_pressed(BTN_B))      btn_to_key(BTN_B_MAP, KEY_DOWN);

    if (btn_released(BTN_LEFT))  btn_to_key(BTN_LEFT_MAP, KEY_RELEASED);
    if (btn_released(BTN_UP))    btn_to_key(BTN_UP_MAP, KEY_RELEASED);
    if (btn_released(BTN_DOWN))  btn_to_key(BTN_DOWN_MAP, KEY_RELEASED);
    if (btn_released(BTN_RIGHT)) btn_to_key(BTN_RIGHT_MAP, KEY_RELEASED);
    if (btn_released(BTN_A))     btn_to_key(BTN_A_MAP, KEY_RELEASED);
    if (btn_released(BTN_B))     btn_to_key(BTN_B_MAP, KEY_RELEASED);
}

void echo_sd_read(uint32_t addr) {
    uint8_t data[SD_BLOCK_SIZE * 2] = {0};
    sd_read_blocks(addr, data, 2);

    for (int i = 0; i < SD_BLOCK_SIZE * 2; i++) {
        char msg[7] = {0};
        sprintf(msg, "0x%02X ", data[i]);

        if ((i + 1) % 16 == 0)
            msg[5] = '\n';

        uart_write_str(msg);
    }
    uart_write('\n');
}

void handle_sd() {
    if (!sd_inserted()) {
        display_print(2, 4, "INSERT GAME CARTRIDGE");
        while (!sd_inserted());
        display_clear();
        delay(100);
    }

    if (!sd_init()) {
        display_print(5, 3, "GAME CARTRIDGE ERROR");
        display_print(22, 4, "PLEASE RESTART");
        while (1);
    }
}

int main(void) {
    gpio_init(GPIOA);
    gpio_init(GPIOB);
    led_enable();

    pwm_init(880);

    display_init();
    show_splash();

    handle_sd();

    buttons_init();
    clock_start();

    // 1. Kick off menu select execution structures
    select_rom(); 

    // 2. Initialize the core virtual system with selected parameters
    init_emulator();
    
    // 3. Mount text arrays and run execution structures
    chip8_load_font(&chip8);

    while (1) {
        handle_input();
        chip8_cycle(&chip8);
        handle_sound();
        handle_display();

        if (chip8.exit)
            chip8_reset(&chip8);
    }

    return 0;
}