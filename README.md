# DC Meter with INA219 and ESP-IDF

This project implements a DC meter using up to three INA219 sensors on an ESP32 (or compatible) platform, with UART communication and configurable measurement channels. The project is built using ESP-IDF and FreeRTOS.

## Features

- Measures voltage, current, and power on up to 3 channels using INA219 sensors.
- Configurable measurement channels (enable/disable voltage, current, power per channel).
- UART communication for data output and command input.
- Supports both raw binary and ASCII print modes for measurement data.
- Periodic measurement with configurable timer period.
- FreeRTOS-based multitasking for measurement and communication.
- Easily configurable I2C pins and addresses via `menuconfig`.

## Hardware Requirements

- ESP32 or compatible board.
- Up to three INA219 sensors.
- Pull-up resistors for I2C lines.
- Proper wiring for I2C (see below).

## Wiring

Connect `SCL` and `SDA` pins to the following GPIOs (configurable in `menuconfig`):

| Name                              | Description               | Defaults (see Kconfig) |
|------------------------------------|---------------------------|------------------------|
| `CONFIG_EXAMPLE_I2C_MASTER_SCL`    | GPIO number for `SCL`     | 19 for ESP32           |
| `CONFIG_EXAMPLE_I2C_MASTER_SDA`    | GPIO number for `SDA`     | 18 for ESP32           |

INA219 I2C addresses can be set via hardware pins and configured in `menuconfig`.

## Usage

1. Configure the project using `idf.py menuconfig`:
    - Set I2C pins, INA219 addresses, shunt resistor value, and LCD options if needed.
2. Build and flash the firmware:
    ```
    idf.py build
    idf.py flash monitor
    ```
3. Use a serial terminal to interact with the device via UART.

### UART Commands

- **Start/Stop Measurement:** Send command to start or stop periodic measurement.
- **Set Timer Period:** Change the measurement interval.
- **Set Print Mode:** Switch between raw binary and ASCII output.
- **Configure Channels:** Enable/disable voltage, current, power measurement per channel.
- **Status:** Query current configuration and status.

See `CommM.c` for command protocol details.

### UART Protocol Details

The device communicates over UART using a simple binary protocol for both commands and data. Below are the details for sending commands and interpreting received data.

#### Data Output

- **Raw Binary Mode:**  
  Each message starts with two header bytes (`0xAA`, `0xBB`), followed by a length byte, and then the measurement data.  
  The data fields and their order depend on the enabled configuration for each channel (voltage, current, power).  
  - Header: `0xAA 0xBB`
  - Length: Number of data bytes following the header (excluding header and length byte)
  - Data: Floats (4 bytes each) for enabled measurements, in channel order (ch1, ch2, ch3)
- **ASCII Mode:**  
  Each measurement is printed as a comma-separated line of floats:  
  ```
  V1, I1, P1, V2, I2, P2, V3, I3, P3
  ```
  Only enabled measurements are printed.

#### Command Input

Commands are sent as binary messages. The first byte is the command type, followed by command-specific data.

| Command Name         | Code (hex) | Payload Format                  | Description                                 |
|----------------------|------------|---------------------------------|---------------------------------------------|
| Start/Stop           | 0x0A       | `[0x0A, 0x01]` or `[0x0A, 0x00]`| Start (`0x01`) or stop (`0x00`) measurement |
| Set Timer Period     | 0x0B       | `[0x0B, <u8>, <u8>, <u8>, <u8>]`| Set period in microseconds (little-endian)  |
| Set Print Mode       | 0x0C       | `[0x0C, 0x00]` or `[0x0C, 0x01]`| Raw (`0x00`) or ASCII (`0x01`) output       |
| Channel Config       | 0x0D       | `[0x0D, <ch1>, <ch2>, <ch3>]`   | Enable/disable V/I/P per channel (see below)|
| Status               | 0x0E       | `[0x0E]`                        | Query current status/config                 |

**Channel Config Byte Format:**  
Each channel config byte enables/disables measurements:
- Bit 0: Voltage (1 = enabled)
- Bit 1: Current (1 = enabled)
- Bit 2: Power   (1 = enabled)

Example:  
To enable voltage and current for channel 1, only current for channel 2, and all for channel 3:  
`[0x0D, 0x03, 0x02, 0x07]`  
- ch1: 0x03 = 0b011 (voltage+current)
- ch2: 0x02 = 0b010 (current)
- ch3: 0x07 = 0b111 (all enabled)

#### Example Command Sequences

- **Start measurement:**  
  `0x0A 0x01`
- **Stop measurement:**  
  `0x0A 0x00`
- **Set timer period to 100ms (100,000us):**  
  `0x0B 0xA0 0x86 0x01 0x00`  (0x000186A0 = 100,000)
- **Set print mode to ASCII:**  
  `0x0C 0x01`
- **Enable voltage+current on all channels:**  
  `0x0D 0x03 0x03 0x03`
- **Query status:**  
  `0x0E`

## Notes

- The default shunt resistor is 100 milliohm; adjust as needed in `menuconfig`.
- Breadboards are not recommended for high current measurements (>1A).
- For float output in `printf`, ensure `CONFIG_NEWLIB_LIBRARY_LEVEL_NORMAL` is enabled on ESP8266.
- LCD support is included but not enabled by default.

## File Structure

- `main.c` - Application entry point.
- `CurrentDrv.c/h` - INA219 driver and measurement logic.
- `CommM.c/h` - UART communication and command handling.
- `Kconfig.projbuild` - Project configuration options.
- `idf_component.yml` - Component dependencies.

## License

See repository for license details.
