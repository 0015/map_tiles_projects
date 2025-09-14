# Local Map Viewer Demo - Independent ESP32 Example Projects

This project demonstrates how to use the **0015__map_tiles** component([ESP Registry](https://components.espressif.com/components/0015/map_tiles)) across multiple independent ESP-IDF projects. It showcases different use cases of interactive map display on ESP32 devices with LVGL 9.x, providing complete examples of GPS-based map navigation with various interaction patterns.

## Project Structure

This contains ESP-IDF projects that share a common map component:

```
map_tiles_projects/
├── shared_components/
│   └── simple_map/                         # Shared SimpleMap component
│       ├── simple_map.hpp
│       ├── simple_map.cpp
│       └── CMakeLists.txt
├── waveshare_esp32_s3_touch_amoled_1_75/   # Demo for waveshare_esp32_s3_touch_amoled_1_75
│   ├── main/
│   │   ├── main.cpp
│   │   └── CMakeLists.txt
│   └── CMakeLists.txt
├── waveshare_esp32_p4_wifi6_touch_lcd_xc/  # Demo for waveshare_esp32_p4_wifi6_touch_lcd_xc
│   ├── main/
│   │   ├── main.cpp
│   │   └── CMakeLists.txt
│   └── CMakeLists.txt
├── components/                             # Device-specific components
└── CMakeLists.txt                          # Shared components only
```

## Features

- **Interactive Map Display**: Touch-scrollable map with smooth tile loading
- **Real-time GPS Coordinates**: Automatically updates latitude/longitude as you scroll
- **Zoom Control**: Adjustable zoom levels (X-19) with dynamic tile reloading
- **GPS Input Panel**: Manual coordinate entry with on-screen keyboard
- **Smart Tile Loading**: Preserves old tiles during updates for smooth transitions
- **Multiple Tile Types**: Support for different map styles (street, satellite, etc.)
- **Touch-optimized UI**: Designed for touchscreen interaction

## Hardware Requirements

- ESP32-S3 with at least 8MB PSRAM (recommended)
- Display with LVGL 9.x support
- Touch panel support
- SD card for map tile storage
- Minimum 4MB flash memory

## Dependencies

- **ESP-IDF 5.0+**: ESP32 development framework
- **LVGL 9.3+**: Graphics library
- **0015__map_tiles**: Map tiles component (included in managed_components)
- **SD card support**: For tile storage (FAT filesystem)

## Map Tiles Format

The map tiles should be stored on SD card in the following format:
- **Format**: RGB565 binary files
- **Size**: 256x256 pixels per tile
- **Structure**: `/sdcard/tiles1/zoom/x/y.bin`
- **Zoom levels**: 10-19 (configurable)

Example directory structure:
```
/sdcard/
├── tiles1/
│   ├── 15/
│   │   ├── 10485/
│   │   │   ├── 12733.bin
│   │   │   ├── 12734.bin
│   │   │   └── ...
│   │   └── ...
│   ├── 16/
│   └── ...
```

## Usage

### Basic Operation

1. **Power on**: The map initializes and displays a default location
2. **Scroll**: Touch and drag to pan around the map
3. **View coordinates**: Current GPS coordinates are shown in the input panel
4. **Zoom**: Use the slider to select zoom level, then press "Update Map"
5. **Go to location**: Enter coordinates manually and press "Update Map"

### Map Controls

- **Touch scrolling**: Drag to move around the map
- **GPS coordinates**: Real-time updates as you scroll
- **Zoom slider**: Select zoom level (10-19)
- **Update button**: Apply coordinate/zoom changes
- **Keyboard**: Appears when editing coordinates

### Key Features

#### Real-time GPS Updates
```cpp
// GPS coordinates update automatically as you scroll
void SimpleMap::update_current_gps_from_map_center();
```

#### Smooth Tile Loading
- Old tiles remain visible during updates
- Loading indicators show progress
- Automatic tile caching and management

#### Touch-optimized Interface
- Debounced scroll events for smooth performance
- Responsive coordinate updates
- User-friendly input validation

## Code Structure

### Shared Components

- **`SimpleMap` class** (`shared_components/simple_map/`): Main map interface and logic
- **`map_tiles` component**: Low-level tile management (managed component)
- **Input panel**: GPS coordinate entry and zoom control
- **Event handlers**: Touch, scroll, and button events

### Main Task Stack Size

This project requires a larger main task stack size than the ESP-IDF default to function correctly. The application loads and displays map tiles directly from an SD card while updating the LVGL graphical interface.

The call stack for these operations—including file I/O (VFS/FATFS), image decoding, and LVGL object updates—can be quite deep. The default main task stack is insufficient for these nested function calls and will likely result in a stack overflow and a system crash.

To ensure stability, you **must** increase the main task stack size to a minimum of **10,240 bytes (10 KB)**. If you plan to enable long filenames or complex decoders, or if you add more features, we recommend a safer size of 12–16 KB.

#### How to set it

- **menuconfig**
  - `idf.py menuconfig` → **Component config → ESP System Settings → Main task stack size**
  - Set to **10240** (or higher).

- **sdkconfig**
  ```ini
  CONFIG_ESP_MAIN_TASK_STACK_SIZE=10240
  ```

### Key Functions

```cpp
// Initialize the map system
bool SimpleMap::init(lv_obj_t* parent_screen);

// Display map at specific location
void SimpleMap::show_location(double lat, double lon, int zoom);

// Handle user interactions
void SimpleMap::map_scroll_event_cb(lv_event_t *e);
void SimpleMap::update_button_event_cb(lv_event_t *e);

// GPS coordinate management
void SimpleMap::update_current_gps_from_map_center();
void SimpleMap::get_current_location(double* lat, double* lon);
```

### Configuration

The map system is configured in `SimpleMap::init()`:

```cpp
map_tiles_config_t config = {
    .base_path = "/sdcard",           // SD card mount point
    .tile_folders = {"tiles1"},       // Tile directory names
    .tile_type_count = 1,             // Number of tile types
    .grid_cols = 5,                   // Tile grid width
    .grid_rows = 5,                   // Tile grid height
    .default_zoom = 18,               // Initial zoom level
    .use_spiram = true,               // Use PSRAM for tiles
    .default_tile_type = 0            // Default tile type
};
```

## Performance Optimization

### Memory Management
- Uses PSRAM for tile storage when available
- Efficient tile caching and cleanup
- Minimal memory fragmentation

### Rendering Optimization
- Preserves old tiles during updates
- Debounced scroll events (50ms for GPS updates)
- Optimized LVGL object management

### Touch Response
- Smooth scrolling with momentum disabled
- Real-time coordinate feedback
- Responsive zoom controls

## API Reference

### Public Methods

```cpp
class SimpleMap {
public:
    // Core functionality
    static bool init(lv_obj_t* parent_screen);
    static void show_location(double lat, double lon, int zoom = 18);
    static void update_location(double lat, double lon);
    static void cleanup();
    
    // Tile management
    static bool set_tile_type(int tile_type);
    static int get_tile_type();
    static int get_tile_type_count();
    
    // Coordinate access
    static void get_current_location(double* lat, double* lon);
    static int get_current_zoom();
    
    // Map positioning
    static void center_map_on_gps();
};
```

### Configuration Options

- **Grid size**: 5x5 to 9x9 tiles (5x5 recommended)
- **Zoom range**: 10-19 (standard map zoom levels)
- **Tile types**: Up to 8 different tile sets
- **Memory**: PSRAM or regular RAM for tile storage

## Troubleshooting

### Common Issues

1. **Map not displaying**:
   - Check SD card mount and tile directory structure
   - Verify tile format (RGB565, 256x256 pixels)
   - Ensure sufficient PSRAM/memory

2. **Slow performance**:
   - Enable PSRAM in menuconfig
   - Reduce grid size if memory limited
   - Check SD card speed class

3. **Touch not working**:
   - Verify touch panel configuration
   - Check LVGL input device setup
   - Ensure proper touch calibration

4. **Coordinates incorrect**:
   - Verify tile coordinate system matches your map data
   - Check zoom level calculations
   - Ensure proper GPS coordinate conversion

### Debug Output

Enable debug output to troubleshoot issues:
```cpp
// Key debug messages
"SimpleMap: Initialized with grid size %dx%d"
"SimpleMap: Loading %dx%d grid at position (%d,%d)"
"SimpleMap: Updated GPS from map center: %.6f, %.6f"
"SimpleMap: Zoom changed from %d to %d"
```

## Example Integration

### Device A Implementation (Interactive)

```cpp
#include "simple_map.hpp"

extern "C" void app_main(void) {
    // Standard ESP32 initialization...
    
    // Initialize interactive map
    if (!SimpleMap::init(lv_screen_active())) {
        ESP_LOGE(TAG, "Failed to initialize map");
        return;
    }

    // Show initial location with full controls
    SimpleMap::show_location(37.77490, -122.41942, 16);
    SimpleMap::center_map_on_gps();
}
```

### Device B Implementation (GPS Tracking)

```cpp
#include "simple_map.hpp"

// Predefined locations for demo
static const struct {
    double lat, lon;
    const char* name;
} demo_locations[] = {
    {40.7128, -74.0060, "New York City"},
    {51.5074, -0.1278, "London"},
    {35.6762, 139.6503, "Tokyo"}
};

void location_cycle_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 second intervals
        
        // Cycle through locations
        current_location_index = (current_location_index + 1) % 3;
        SimpleMap::show_location(
            demo_locations[current_location_index].lat,
            demo_locations[current_location_index].lon,
            15
        );
    }
}

extern "C" void app_main(void) {
    // Standard ESP32 initialization...
    
    // Initialize map for tracking mode
    SimpleMap::init(lv_screen_active());
    SimpleMap::show_location(demo_locations[0].lat, demo_locations[0].lon, 15);
    
    // Start location cycling task
    xTaskCreate(location_cycle_task, "location_cycle", 4096, NULL, 5, NULL);
}
```

### Shared Component Usage

Both devices use the same SimpleMap API:

```cpp
// Change tile type (e.g., satellite view)
SimpleMap::set_tile_type(1);

// Get current position
double lat, lon;
SimpleMap::get_current_location(&lat, &lon);

// Update to new location
SimpleMap::update_location(40.7128, -74.0060);
```

## Creating Custom Device Projects

### Step 1: Create New Project Directory
```bash
mkdir device_custom_project
cd device_custom_project
```

### Step 2: Create CMakeLists.txt
```cmake
# Device Custom - Your Demo
cmake_minimum_required(VERSION 3.5)

# Add shared components from parent directory
set(EXTRA_COMPONENT_DIRS "../shared_components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(simple_map_device_custom)
```

### Step 3: Create main/ directory and files
```bash
mkdir main
```

**main/CMakeLists.txt:**
```cmake
idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES simple_map)
```

**main/main.cpp:**
```cpp
#include "simple_map.hpp"

extern "C" void app_main(void) {
    // Your device-specific initialization
    
    // Initialize shared map component
    if (!SimpleMap::init(lv_screen_active())) {
        return;
    }
    
    // Your custom map configuration
    SimpleMap::show_location(your_lat, your_lon, your_zoom);
}
```

### Step 4: Build and Test
```bash
idf.py build flash monitor
```

## Contributing

This is a multi-device example project demonstrating the 0015__map_tiles component. For improvements or bug fixes:
- **Shared SimpleMap component**: Contribute to `shared_components/simple_map/`
- **Device-specific features**: Create new device configurations
- **Core map functionality**: Contribute to the main 0015__map_tiles component repository

## Acknowledgments

- **LVGL Team**: For the excellent graphics library
- **Espressif**: For the ESP-IDF framework
- **Map tile providers**: For map data (ensure proper attribution)

## Support

For questions and support:
1. Check the troubleshooting section above
2. Review the 0015__map_tiles component documentation
3. Post issues with full debug output and hardware configuration


## License

This project is provided as an example for the 0015__map_tiles component. Check individual component licenses for specific terms.