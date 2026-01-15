# Building and Flashing STM32F103VB Firmware

This document explains how to compile and flash the firmware using command-line tools.

## Prerequisites

Make sure you have the following tools installed:
- `arm-none-eabi-gcc` - ARM GCC toolchain
- `make` - Build system
- `st-flash` or `openocd` - For flashing (see below)

## Compiling

Simply run:
```bash
make
```

This will create the following files in the `build/` directory:
- `GBVBT6.elf` - ELF executable (for debugging)
- `GBVBT6.hex` - Intel HEX format
- `GBVBT6.bin` - Binary format (for flashing)

To clean the build directory:
```bash
make clean
```

## Flashing the Firmware

### Option 1: Using st-flash (ST-Link)

If you have an ST-Link programmer connected via USB:

1. **Flash the binary:**
   ```bash
   st-flash write build/GBVBT6.bin 0x8000000
   ```

2. **Verify the flash:**
   ```bash
   st-flash read build/GBVBT6_readback.bin 0x8000000 <size>
   ```

3. **Reset the device:**
   ```bash
   st-flash reset
   ```

**Note:** You may need to add yourself to the `plugdev` group to access the ST-Link:
```bash
sudo usermod -a -G plugdev $USER
# Log out and back in for the change to take effect
```

### Option 2: Using OpenOCD

If you're using OpenOCD with an ST-Link:

1. **Flash and reset:**
   ```bash
   openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "program build/GBVBT6.elf verify reset exit"
   ```

   Or if you prefer using the binary:
   ```bash
   openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "program build/GBVBT6.bin 0x08000000 verify reset exit"
   ```

### Option 3: Using STM32CubeProgrammer

If you have STM32CubeProgrammer installed:

```bash
STM32_Programmer_CLI -c port=SWD -w build/GBVBT6.bin 0x08000000 -v -rst
```

## Troubleshooting

### Build Issues

- **Missing source files:** Make sure all source files listed in the Makefile exist
- **Linker errors:** Check that all required HAL modules are enabled in `Core/Inc/stm32f1xx_hal_conf.h`

### Flashing Issues

- **Permission denied:** Make sure you're in the `plugdev` group (see Option 1 above)
- **Device not found:** 
  - Check USB connection
  - Try `lsusb` to see if the ST-Link is detected
  - For st-flash: `st-info --probe`
  - For OpenOCD: Check if the interface file matches your programmer

- **Flash verification failed:**
  - The device might be locked. Try unlocking:
    ```bash
    st-flash erase
    ```
  - Or with OpenOCD:
    ```bash
    openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "init; halt; stm32f1x unlock 0; reset halt; stm32f1x mass_erase 0; shutdown"
    ```

## Project Structure

- `Core/` - Main application code
- `Drivers/` - STM32 HAL and CMSIS drivers
- `USB_DEVICE/` - USB device configuration
- `Middlewares/` - USB device library
- `build/` - Build output directory (generated)
- `Makefile` - Build configuration
- `STM32F103VB_FLASH.ld` - Linker script

## Memory Layout (STM32F103VB)

- **Flash:** 128 KB (0x08000000 - 0x0801FFFF)
- **RAM:** 20 KB (0x20000000 - 0x20004FFF)
