# ESP32-S3 GPS Tracker with LoRa and **LVGL-Powered Local Mapping**

A robust, real-time GPS tracking system leveraging the power of LoRa for long-range communication and a **custom-built LVGL map library** for interactive local map visualization on the ESP32-S3.

## Key Features

* **Real-time GPS Tracking**: High-accuracy positioning (typically 20+ satellites) using the reliable LC76G GPS module.
* **LoRa Communication**: Achieves long-range wireless tracking using the RYLR998 module, ideal for off-grid scenarios.
* **Interactive Local Map Display (Powered by UserMap Library)**:
    * Features an **LVGL v9-based map engine**.
    * Supports dynamic rendering of **OpenStreetMap tiles** from an SD card.
    * Provides smooth **zoom controls (levels 10-19)** and **pan functionality** via the touch display.
* **Multi-device Tracking**: Displays the real-time location of multiple remote devices simultaneously on the map using distinct markers.
* **Proximity Alarm**: Triggers an audio alert when any tracked device exceeds a configurable distance threshold (default 15 feet).
* **Hardware Monitoring**: Real-time battery voltage and charging status display.
* **Touch Controls**: Full interactive control over map navigation and interface.

## Features

- **Real-time GPS Tracking**: LC76G GPS module with high-accuracy positioning (typically 20+ satellites)
- **LoRa Communication**: Long-range wireless communication (3-5km) using RYLR998 module
- **Interactive Map Display**: LVGL v9-based map with zoom controls (levels 10-19)
- **Proximity Alarm**: Audio alert when devices exceed configurable distance threshold (default 15 feet)
- **Multi-device Support**: Track multiple remote devices simultaneously
- **Battery Monitoring**: Real-time battery voltage display with charging status
- **Touch Controls**: Interactive map navigation with zoom and pan

## Hardware Requirements

- **MCU**: ESP32-S3 (Waveshare ESP32-S3-Touch-LCD-3.49)
- **Display**: 640x172 LCD with touch support
- **GPS Module**: LC76G (UART communication)
- **LoRa Module**: RYLR998 (UART communication)
- **Audio**: Speaker for alarm notifications
- **Power**: Battery with USB charging support

## System Architecture

### Workflow Overview

```
┌─────────────────┐
│   GPS Module    │ ──> Receives satellite signals
│    (LC76G)      │     - Position (Lat/Lon)
└────────┬────────┘     - Time, altitude, speed
         │              - Satellite count, HDOP
         ▼
┌─────────────────┐
│  GPS Processing │ ──> Parse NMEA data
│   (main.cpp)    │     - Extract coordinates
└────────┬────────┘     - Validate fix quality
         │
         ├──────────────────────┬─────────────────────┐
         ▼                      ▼                     ▼
┌─────────────────┐    ┌─────────────────┐   ┌──────────────┐
│  Map Display    │    │ LoRa Broadcast  │   │   Distance   │
│  (UserMap)      │    │   (RYLR998)     │   │ Calculation  │
└─────────────────┘    └─────────────────┘   └──────────────┘
         │                      │                     │
         │                      │                     ▼
         │                      │            ┌──────────────┐
         │                      │            │ Alarm System │
         │                      ▼            │  (Audio)     │
         │             ┌─────────────────┐   └──────────────┘
         │             │  LoRa Receive   │
         │             │  Remote GPS     │
         │             └────────┬────────┘
         │                      │
         └──────────────────────┘
                    │
                    ▼
         ┌─────────────────────┐
         │   Update Remote     │
         │   Device Markers    │
         └─────────────────────┘
```

### Key Components

1. **GPS Task**: Continuously reads GPS data, updates local position, triggers map refresh
2. **LoRa Broadcast Task**: Transmits local GPS coordinates to remote devices (every 10 seconds)
3. **LoRa Receive Handler**: Processes incoming GPS data from remote devices
4. **Map Update Task**: Renders map tiles and device markers using LVGL
5. **Distance Monitor**: Calculates real-time distance using Haversine formula
6. **Alarm Monitor**: Triggers audio alerts when distance threshold exceeded

## Configuration

### GPS Settings ([main/user_config.h](main/user_config.h))

```c
#define GPS_UART_NUM        UART_NUM_1
#define GPS_TX_PIN          43
#define GPS_RX_PIN          44
#define GPS_BAUD_RATE       115200
```

### LoRa Settings ([main/lora_rylr998.c](main/lora_rylr998.c))

```c
.spreading_factor = LORA_SF9,      // Range: SF5-SF9
.bandwidth = LORA_BW_125KHZ,       // 125 kHz bandwidth
.coding_rate = LORA_CR_4_5,        // 4/5 coding rate
.tx_power = 22,                     // 22 dBm (maximum)
.network_id = 3,                    // Network ID (0-255)
.password = "EEDCAA90",             // Network password
```

### Alarm Settings ([main/main.cpp](main/main.cpp))

```c
#define ALARM_DISTANCE_THRESHOLD_FEET 100.0  // Distance threshold
```

## Configuration Instructions

### Changing Device Type

Each device can be configured as either "Mom" or "Kid" to display different colored markers and icons on the map.

1. Open [main/device_config.c](main/device_config.c)
2. Locate the `get_device_type()` function (around line 32)
3. Change the return value:
   - For Mom device: `return DEVICE_TYPE_MOM;`
   - For Kid device: `return DEVICE_TYPE_KID;`

Example:
```c
DeviceType get_device_type(void) {
    return DEVICE_TYPE_KID;  // This device will show as blue kid marker
}
```

4. Save and flash the firmware to your device

**Device Type Characteristics:**
- **Kid** (Type 0): Blue marker, kid icon
- **Mom** (Type 1): Pink marker, mom icon

### Changing LoRa Address

Each device needs a unique LoRa address for identification in the network. The device name will be displayed on the map.

1. Open [main/device_config.c](main/device_config.c)
2. Locate the `get_lora_address()` function (around line 28)
3. Modify the return value to a unique address (1-65535)

Example:
```c
uint16_t get_lora_address(void) {
    return 1;  // Mom's device address
}
```

For a second device:
```c
uint16_t get_lora_address(void) {
    return 2;  // Kid's device address
}
```

4. Save and flash the firmware to your device

**Important Notes:**
- Each device must have a **unique LoRa address** (no duplicates on the network)
- The address is used to identify the device in LoRa communications
- The device name displayed on the map is automatically generated based on the device type configuration
- All devices must use the same **network ID** and **password** (configured in [main/lora_rylr998.c](main/lora_rylr998.c)) to communicate

### Changing LoRa Network Settings (Optional)

If you need to change the LoRa network ID or password:

1. Open [main/lora_rylr998.c](main/lora_rylr998.c)
2. Locate the `lora_init()` function
3. Modify these parameters:
   ```c
   .network_id = 3,        // Change to your network ID (0-255)
   .password = "EEDCAA90", // Change to your 8-character hex password
   ```
4. Apply the same changes to **all devices** in your network and flash them

## Usage

1. **Power On**: Device initializes GPS and LoRa modules
2. **GPS Lock**: Wait for GPS to acquire satellite fix (status shown on splash screen)
3. **Map Display**: Once GPS locked, map loads centered on current position
4. **Track Devices**: Remote devices appear as markers with names and device type icons
5. **Distance Monitoring**: Status panel shows distance to nearest remote device
6. **Alarm**: Audio alert triggers when devices exceed distance threshold

### Device Types

- **Kid**: Blue marker with kid icon
- **Mom**: Pink marker with mom icon

## Project Structure

```
ESP32-S3_Map_LoRa_GPS/
├── main/
│   ├── main.cpp              # Main application logic
│   ├── user_config.h         # Pin and feature configuration
│   ├── user_map.cpp/hpp      # Map rendering engine
│   ├── gps_lc76g.c/h         # GPS driver
│   ├── lora_rylr998.c/h      # LoRa driver
│   ├── device_config.c/h     # Device identification
│   ├── icon_kid.c            # Kid device icon data
│   └── icon_mom.c            # Mom device icon data
├── components/
│   ├── adc_bsp/              # ADC for battery monitoring
│   ├── audio_bsp/            # Audio playback driver
│   ├── button_bsp/           # Button input handling
│   ├── codec_board/          # Audio codec board support
│   ├── i2c_bsp/              # I2C driver
│   ├── lcd_bl_pwm_bsp/       # LCD backlight PWM control
│   └── sdcard_bsp/           # SD card driver (for map tiles)
└── managed_components/
    └── 0015__map_tiles/      # Map tile rendering library
```

## Technical Details

### LoRa Communication Protocol

Messages are formatted as CSV:
```
DeviceName,DeviceType,Latitude,Longitude
```

Example: `Mom,1,47.6175803,-122.1235667`

### Map Tiles

- Source: OpenStreetMap
- Format: PNG tiles (256×256 pixels)
- Storage: SD card
- Zoom levels: 10-19

## Troubleshooting

### GPS Not Locking
- Ensure antenna has clear sky view
- Check UART connections (TX/RX not swapped)
- Monitor NMEA output: should see `$GPGGA`, `$GPRMC` sentences

### LoRa Communication Failed
- Verify both devices use same network ID and password
- Check spreading factor compatibility (SF5-SF9)
- Ensure antennas are connected
- Monitor RSSI/SNR values for signal quality

### Map Not Loading
- Verify SD card is properly inserted
- Check map tiles exist in correct directory structure
- Ensure GPS has valid fix before map loads

## Performance

- **GPS Update Rate**: 1 Hz (1 second intervals)
- **LoRa Broadcast Rate**: 0.1 Hz (10 second intervals)
- **Map Refresh**: On GPS position change
- **Distance Calculation**: Real-time on GPS update
- **Alarm Check**: 1 Hz (1 second intervals)

## License

This project uses components from Espressif and LVGL libraries. Please refer to their respective licenses.