/* Map display and GPS marker management */

#pragma once

#include "lvgl.h"
#include "map_tiles.h"

class UserMap {
public:
    // Initialize the simple map system
    static bool init(lv_obj_t* parent_screen);
    
    // Display map at specific coordinates
    static void show_location(double latitude, double longitude, int zoom_level = 18);
    
    // Update location (for moving GPS)
    static void update_location(double latitude, double longitude);
    
    // Toggle between map and satellite view
    static void set_satellite_view(bool satellite);
    
    // Set specific tile type (0 = tiles1, 1 = tiles2, etc.)
    static bool set_tile_type(int tile_type);
    
    // Get current tile type
    static int get_tile_type();
    
    // Get available tile type count
    static int get_tile_type_count();
    
    // Get tile type folder name
    static const char* get_tile_type_folder(int tile_type);
    
    // Center map on current GPS coordinates with pixel precision
    static void center_map_on_gps();
    
    // Change zoom level and reload map tiles
    static void change_zoom_level(int new_zoom);
    
    // Get current GPS coordinates (latitude, longitude) at center of screen
    static void get_current_location(double* latitude, double* longitude);
    
    // Get current zoom level
    static int get_current_zoom();
    
    // Get GPS coordinates (for status panel)
    static void get_gps_coordinates(double* latitude, double* longitude);
    
    // Update battery voltage display
    static void update_battery_voltage(float voltage);
    
    // Get map center coordinates (calculated from current scroll position)
    static void get_map_center_coordinates(double* latitude, double* longitude);
    
    // GPS marker functions
    static void create_gps_marker();
    static void update_gps_marker_position();  // Uses global gps_lat/gps_lon
    
    // GPS direction arrows (shows direction to GPS when off-screen) - Adapted for rectangular display
    static void create_gps_direction_arrows();
    static void update_gps_direction_arrows();
    static bool is_marker_visible();
    
    // Notification border (status indicator) - Adapted for rectangular display
    static void create_notification_border();
    static void set_notification_state(int state);  // 0=gray, 1=green, 2=yellow, 3=red
    
    // Remote GPS markers (from other devices)
    static void update_remote_marker(uint16_t device_id, uint8_t device_type, const char* device_name, double latitude, double longitude);
    static void remove_remote_marker(uint16_t device_id);
    static void update_all_remote_markers();
    
    // Distance calculation (Haversine formula)
    static double calculate_distance(double lat1, double lon1, double lat2, double lon2);
    
    // Remote marker structure (public for alarm monitoring)
    struct RemoteMarker {
        bool active;
        uint16_t device_id;
        uint8_t device_type;       // Device type (0=mom, 1=kid, etc.)
        char device_name[16];      // Device name
        double latitude;
        double longitude;
        uint32_t last_update_ms;
        lv_obj_t* marker_obj;      // Container
        lv_obj_t* marker_icon;     // Icon
        lv_obj_t* marker_label;    // Label with coordinates and time
        // Direction arrows for this remote marker
        lv_obj_t* arrow_left;
        lv_obj_t* arrow_right;
        lv_obj_t* arrow_top;
        lv_obj_t* arrow_bottom;
    };
    
    static const int MAX_REMOTE_MARKERS = 10;
    static RemoteMarker remote_markers[MAX_REMOTE_MARKERS];
    static int remote_marker_count;
    
    // Cleanup
    static void cleanup();

private:
    static lv_obj_t* map_container;
    static lv_obj_t* map_group;
    static lv_obj_t** tile_widgets;
    static int grid_cols, grid_rows, tile_count;  // Cache grid dimensions
    static lv_obj_t* zoom_label;
    static lv_obj_t* zoom_plus_button;
    static lv_obj_t* zoom_minus_button;
    static lv_obj_t* settings_button;
    static lv_obj_t* loading_popup;
    static lv_obj_t* gps_marker;  // GPS marker container
    static lv_obj_t* gps_marker_icon;  // GPS marker icon
    static lv_obj_t* gps_marker_label;  // GPS marker label (coordinates)
    
    // Direction arrows for rectangular display (replaces circular arcs)
    static lv_obj_t* gps_arrow_left;
    static lv_obj_t* gps_arrow_right;
    static lv_obj_t* gps_arrow_top;
    static lv_obj_t* gps_arrow_bottom;
    
    // Notification border (replaces circular arc)
    static lv_obj_t* notification_border;
    
    static lv_obj_t* lat_label;  // Latitude display label
    static lv_obj_t* lon_label;  // Longitude display label
    static lv_obj_t* battery_voltage_label;  // Battery voltage display label
    
    static lv_obj_t* distance_label;  // Distance label in feet (large font)
    static lv_obj_t* km_distance_label;  // Distance label in km (smaller font)
    static lv_obj_t* parent_scr;  // Parent screen for marker
    static double current_lat;
    static double current_lon;
    static int current_zoom;
    static bool initialized;
    static bool is_loading;  // Flag to prevent multiple simultaneous loads
    static uint32_t last_scroll_time;  // Timestamp of last scroll event
    static uint32_t last_gps_update_time;  // Timestamp of last GPS update
    static map_tiles_handle_t map_handle;
    
    // Helper functions for remote markers
    static RemoteMarker* find_remote_marker(uint16_t device_id);
    static RemoteMarker* get_or_create_remote_marker(uint16_t device_id);
    static void create_remote_marker_objects(RemoteMarker* marker);
    static void update_remote_marker_position(RemoteMarker* marker);
    static bool is_remote_marker_visible(RemoteMarker* marker);
    static void update_remote_direction_arrows(RemoteMarker* marker);
    static void create_distance_label();
    static void update_distance_label();
    
    // Helper functions
    static void load_map_tiles();
    static void create_tile_widgets();
    static void map_scroll_event_cb(lv_event_t *e);
    static void create_zoom_panel(lv_obj_t* parent_screen);
    static void zoom_plus_event_cb(lv_event_t *e);
    static void zoom_minus_event_cb(lv_event_t *e);
    static void settings_button_event_cb(lv_event_t *e);
    static void update_zoom_label();
    static void show_loading_popup();
    static void hide_loading_popup();
    static void update_current_gps_from_map_center();
};
