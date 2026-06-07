# STM32 Handheld Gaming Console (Based on CHIPnGo)

First and foremost, sincere thanks to Kurt J. D. and the original CHIPnGo project for creating an open-source handheld gaming console platform that made this work possible.

Original Project:
https://github.com/kurtjd/CHIPnGo

This repository contains my customized version of CHIPnGo, developed to support different hardware configurations and improve functionality for my specific implementation.

## Key Modifications

- Added support for a 6-pin SPI OLED display.
- Modified cartridge handling and generation logic in `cartridge.py`.
- Adapted firmware and hardware interfaces for custom components.
- Improved compatibility with alternative display hardware.
- Refined game loading and cartridge workflows.
- Performed various bug fixes, optimizations, and hardware-specific adjustments.

## Project Overview

This project is an STM32-based handheld gaming console built around the CHIP-8 ecosystem. It combines embedded systems programming, hardware interfacing, display control, storage management, and real-time input handling on STM32 microcontrollers.

Through this project, I gained hands-on experience with:
- STM32 embedded development
- SPI communication and display drivers
- Firmware customization
- Python tooling for asset and cartridge management
- Hardware-software integration
- Debugging and optimization of embedded systems

While the foundation originates from the CHIPnGo project, this repository documents my modifications, experiments, and enhancements made during the process of adapting the console to my own hardware and requirements.
