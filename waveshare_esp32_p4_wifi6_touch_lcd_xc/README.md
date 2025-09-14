# Simple Map Test - ESP32 Map Display Example

This project demonstrates how to use the **0015__map_tiles** component to create an interactive map display on ESP32 devices with LVGL 9.x. It provides a complete example of GPS-based map navigation with touch scrolling, zoom controls, and real-time coordinate updates.

## Features

- **Interactive Map Display**: Touch-scrollable map with smooth tile loading
- **Real-time GPS Coordinates**: Automatically updates latitude/longitude as you scroll
- **Zoom Control**: Adjustable zoom levels (10-19) with dynamic tile reloading
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

## Installation

1. **Clone or download this project**
2. **Set up ESP-IDF environment**:
   ```bash
   # Install ESP-IDF 5.0 or later
   # Set IDF_PATH environment variable
   ```

3. **Configure the project**:
   ```bash
   cd simple_map_test
   idf.py menuconfig
   ```
   - Configure PSRAM settings
   - Set display resolution and interface
   - Configure SD card pins

4. **Build and flash**:
   ```bash
   idf.py build
   idf.py flash monitor
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

### Main Components

- **`SimpleMap` class**: Main map interface and logic
- **`map_tiles` component**: Low-level tile management
- **Input panel**: GPS coordinate entry and zoom control
- **Event handlers**: Touch, scroll, and button events

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

### Basic Usage

```cpp
#include "simple_map.hpp"

void app_main() {
    // Initialize LVGL and display
    lv_init();
    
    // Create main screen
    lv_obj_t* screen = lv_screen_active();
    
    // Initialize map
    if (SimpleMap::init(screen)) {
        // Show initial location (San Francisco)
        SimpleMap::show_location(37.7749, -122.4194, 15);
    }
    
    // LVGL task loop
    while (1) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

### Advanced Features

```cpp
// Change tile type (e.g., satellite view)
SimpleMap::set_tile_type(1);

// Get current position
double lat, lon;
SimpleMap::get_current_location(&lat, &lon);
printf("Current position: %.6f, %.6f\n", lat, lon);

// Update to new location
SimpleMap::update_location(40.7128, -74.0060);  // New York
```

## License

This project is provided as an example for the 0015__map_tiles component. Check individual component licenses for specific terms.

## Contributing

This is an example project demonstrating the 0015__map_tiles component. For improvements or bug fixes, please contribute to the main component repository.

## Acknowledgments

- **LVGL Team**: For the excellent graphics library
- **Espressif**: For the ESP-IDF framework
- **Map tile providers**: For map data (ensure proper attribution)

## Support

For questions and support:
1. Check the troubleshooting section above
2. Review the 0015__map_tiles component documentation
3. Post issues with full debug output and hardware configuration