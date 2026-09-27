/* Map display and GPS marker implementation */

#include "user_map.hpp"
#include <stdio.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <cstring>

extern "C" {
#include "device_config.h"
}

// External image declarations
LV_IMAGE_DECLARE(icon_mom);
LV_IMAGE_DECLARE(icon_kid);

// Static member definitions
lv_obj_t* UserMap::map_container = nullptr;
lv_obj_t* UserMap::map_group = nullptr;
lv_obj_t** UserMap::tile_widgets = nullptr;
int UserMap::grid_cols = 0;
int UserMap::grid_rows = 0;
int UserMap::tile_count = 0;
lv_obj_t* UserMap::zoom_label = nullptr;
lv_obj_t* UserMap::zoom_plus_button = nullptr;
lv_obj_t* UserMap::zoom_minus_button = nullptr;
lv_obj_t* UserMap::settings_button = nullptr;
lv_obj_t* UserMap::loading_popup = nullptr;

// Static variable to store pending zoom level
static int pending_zoom = 15;
lv_obj_t* UserMap::gps_marker = nullptr;
lv_obj_t* UserMap::gps_marker_icon = nullptr;
lv_obj_t* UserMap::gps_marker_label = nullptr;

// Direction arrows for rectangular display
lv_obj_t* UserMap::gps_arrow_left = nullptr;
lv_obj_t* UserMap::gps_arrow_right = nullptr;
lv_obj_t* UserMap::gps_arrow_top = nullptr;
lv_obj_t* UserMap::gps_arrow_bottom = nullptr;

// Notification border
lv_obj_t* UserMap::notification_border = nullptr;

// Coordinate display labels
lv_obj_t* UserMap::lat_label = nullptr;
lv_obj_t* UserMap::lon_label = nullptr;
lv_obj_t* UserMap::battery_voltage_label = nullptr;

lv_obj_t* UserMap::distance_label = nullptr;
lv_obj_t* UserMap::km_distance_label = nullptr;
lv_obj_t* UserMap::parent_scr = nullptr;
double UserMap::current_lat = 0.0;
double UserMap::current_lon = 0.0;
int UserMap::current_zoom = 15;
bool UserMap::initialized = false;
bool UserMap::is_loading = false;
uint32_t UserMap::last_scroll_time = 0;
uint32_t UserMap::last_gps_update_time = 0;
map_tiles_handle_t UserMap::map_handle = nullptr;

// Remote marker storage
UserMap::RemoteMarker UserMap::remote_markers[MAX_REMOTE_MARKERS] = {};
int UserMap::remote_marker_count = 0;

// Cache the current tile grid position (like local_map_gps global variables)
static int cached_tile_x = 0;
static int cached_tile_y = 0;

// Separate variables for GPS position vs map center (like local_map_gps)
static double gps_lat = 0.0;  // Actual GPS latitude
static double gps_lon = 0.0;  // Actual GPS longitude

bool UserMap::init(lv_obj_t* parent_screen) {
    if (initialized) return true;
    
    // Store parent screen for GPS marker
    parent_scr = parent_screen;
    
    // Initialize the map tiles component
    map_tiles_config_t config = {
        .base_path = "/sdcard",
        .tile_folders = {"tiles1"}, 
        .tile_type_count = 1,
        .grid_cols = MAP_TILES_DEFAULT_GRID_COLS,
        .grid_rows = MAP_TILES_DEFAULT_GRID_ROWS,
        .default_zoom = 18,
        .use_spiram = true,
        .default_tile_type = 0  // Start with tiles1
    };
    
    map_handle = map_tiles_init(&config);
    if (!map_handle) {
        return false;
    }
    
    // Cache grid dimensions for performance
    map_tiles_get_grid_size(map_handle, &grid_cols, &grid_rows);
    tile_count = map_tiles_get_tile_count(map_handle);

    lv_display_t *disp = lv_display_get_default();    
    lv_coord_t _width = lv_display_get_horizontal_resolution(disp);
    lv_coord_t _height = lv_display_get_vertical_resolution(disp);
    
    // Create scrollable map container (like map_scroll in map_display.cpp)
    map_container = lv_obj_create(parent_screen);
    lv_obj_set_scrollbar_mode(map_container, LV_SCROLLBAR_MODE_OFF);  // Disable scrollbars first
    lv_obj_set_size(map_container, _width, _height);  // Match device resolution
    lv_obj_center(map_container);
    lv_obj_clear_flag(map_container, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(map_container, LV_DIR_ALL);
    lv_obj_add_flag(map_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(map_container, 0, 0);
    lv_obj_set_style_radius(map_container, 0, 0);
    lv_obj_set_style_bg_color(map_container, lv_color_black(), 0);
    lv_obj_add_event_cb(map_container, map_scroll_event_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(map_container, map_scroll_event_cb, LV_EVENT_SCROLL, NULL);
    
    // Create tile widgets
    create_tile_widgets();
    
    // Create zoom panel
    create_zoom_panel(parent_screen);
    
    // Create coordinate display labels in upper left corner
    lv_obj_t* coord_panel = lv_obj_create(parent_screen);
    lv_obj_set_scrollbar_mode(coord_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(coord_panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_pos(coord_panel, 5, 5); // Upper left corner
    lv_obj_set_style_bg_color(coord_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(coord_panel, 220, 0);
    lv_obj_set_style_border_width(coord_panel, 0, 0);
    lv_obj_set_style_radius(coord_panel, 4, 0);
    lv_obj_set_style_pad_all(coord_panel, 5, 0);
    lv_obj_clear_flag(coord_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(coord_panel, LV_FLEX_FLOW_ROW);  // Horizontal layout
    lv_obj_set_flex_align(coord_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(coord_panel, 10, 0);  // 10px spacing between items
    
    // Create battery voltage label
    battery_voltage_label = lv_label_create(coord_panel);
    lv_label_set_text(battery_voltage_label, LV_SYMBOL_BATTERY_FULL " --.-V");
    lv_obj_set_style_text_color(battery_voltage_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(battery_voltage_label, &lv_font_montserrat_14, 0);
    
    // Create latitude label
    lat_label = lv_label_create(coord_panel);
    lv_label_set_text(lat_label, LV_SYMBOL_GPS " Lat: 0.000000");
    lv_obj_set_style_text_color(lat_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(lat_label, &lv_font_montserrat_14, 0);
    
    // Create longitude label
    lon_label = lv_label_create(coord_panel);
    lv_label_set_text(lon_label, "Lon: 0.000000");
    lv_obj_set_style_text_color(lon_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(lon_label, &lv_font_montserrat_14, 0);
    
    lv_obj_move_foreground(coord_panel); // Bring to front
    
    // Create GPS direction arrows (optimized for 640x172 rectangular display)
    create_gps_direction_arrows();
    
    // Create notification border (optimized for rectangular display)
    create_notification_border();
    
    // Create distance label
    create_distance_label();
    
    initialized = true;
    return true;
}

void UserMap::create_tile_widgets() {
    if (grid_cols <= 0 || grid_rows <= 0 || tile_count <= 0) {
        return;
    }
    
    // Allocate tile_widgets array if not already done
    if (!tile_widgets) {
        tile_widgets = (lv_obj_t**)calloc(tile_count, sizeof(lv_obj_t*));
        if (!tile_widgets) {
            printf("Failed to allocate tile_widgets array\n");
            return;
        }
    }
    
    // Create map group with full tile dimensions (like map_display.cpp)
    if (!map_group) {
        map_group = lv_obj_create(map_container);
        lv_obj_set_size(map_group, MAP_TILES_TILE_SIZE * grid_cols, MAP_TILES_TILE_SIZE * grid_rows);
        lv_obj_set_style_pad_all(map_group, 0, 0);
        lv_obj_set_style_border_width(map_group, 0, 0);
        lv_obj_clear_flag(map_group, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(map_group, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_pos(map_group, 0, 0);
        
        // Create grid of image widgets for tiles
        for (int i = 0; i < tile_count; i++) {
            tile_widgets[i] = lv_image_create(map_group);
            
            int row = i / grid_cols;
            int col = i % grid_cols;
            
            // Position using full TILE_SIZE (like map_display.cpp)
            lv_obj_set_pos(tile_widgets[i], col * MAP_TILES_TILE_SIZE, row * MAP_TILES_TILE_SIZE);
            lv_obj_set_size(tile_widgets[i], MAP_TILES_TILE_SIZE, MAP_TILES_TILE_SIZE);
            
            // Set default background while tiles load
            lv_obj_set_style_bg_color(tile_widgets[i], lv_color_make(200, 200, 200), 0);
            lv_obj_set_style_bg_opa(tile_widgets[i], LV_OPA_COVER, 0);
        }
    }
}

void UserMap::show_location(double latitude, double longitude, int zoom_level) {
    if (!initialized || !map_handle) return;
    
    // Update map center position
    current_lat = latitude;
    current_lon = longitude;
    current_zoom = zoom_level;
    pending_zoom = zoom_level;  // Initialize pending zoom
    
    // Set GPS marker position
    gps_lat = latitude;
    gps_lon = longitude;
    
    // Set zoom level in the component
    map_tiles_set_zoom(map_handle, zoom_level);
    
    // Update zoom label
    update_zoom_label();
    
    // Set the tile center based on GPS coordinates
    map_tiles_set_center_from_gps(map_handle, latitude, longitude);
    
    // Cache the tile position IMMEDIATELY after setting center
    map_tiles_get_position(map_handle, &cached_tile_x, &cached_tile_y);
    
    // Create GPS marker if it doesn't exist
    if (!gps_marker) {
        create_gps_marker();
    }
    
    load_map_tiles();
    // Note: marker position will be updated after tiles load in load_map_tiles()
}

void UserMap::update_location(double latitude, double longitude) {
    if (!initialized || !map_handle) return;
    
    // Update GPS marker position
    gps_lat = latitude;
    gps_lon = longitude;
    
    if (!gps_marker) {
        create_gps_marker();
    }
    
    // Always just update marker position - don't recenter map automatically
    // This allows user to manually pan the map without it jumping back
    update_gps_marker_position();
    
    /* Disabled auto-recentering when GPS moves outside tiles
    if (!is_within_tiles) {
        // Reload tiles with GPS at center
        map_tiles_set_center_from_gps(map_handle, latitude, longitude);
        current_lat = latitude;
        current_lon = longitude;
        map_tiles_get_position(map_handle, &cached_tile_x, &cached_tile_y);
        load_map_tiles();
    } else {
        // Update marker position only
        update_gps_marker_position();
    }
    */
}

void UserMap::set_satellite_view(bool satellite) {
    if (!initialized || !map_handle) return;
    
    // Convert boolean to tile type (0 = regular, 1 = satellite)
    int tile_type = satellite ? 1 : 0;
    
    // Set tile type in the component
    if (map_tiles_set_tile_type(map_handle, tile_type)) {
        // Reload tiles with the new tile type
        load_map_tiles();
    }
}

bool UserMap::set_tile_type(int tile_type) {
    if (!initialized || !map_handle) return false;
    
    // Set tile type in the component
    if (map_tiles_set_tile_type(map_handle, tile_type)) {
        // Reload tiles with the new tile type
        load_map_tiles();
        return true;
    }
    return false;
}

int UserMap::get_tile_type() {
    if (!initialized || !map_handle) return -1;
    return map_tiles_get_tile_type(map_handle);
}

int UserMap::get_tile_type_count() {
    if (!initialized || !map_handle) return 0;
    return map_tiles_get_tile_type_count(map_handle);
}

const char* UserMap::get_tile_type_folder(int tile_type) {
    if (!initialized || !map_handle) return nullptr;
    return map_tiles_get_tile_type_folder(map_handle, tile_type);
}

void UserMap::load_map_tiles() {
    if (!initialized || !map_handle) return;
    
    // Check if already loading to prevent multiple simultaneous loads
    if (is_loading) {
        printf("UserMap: Already loading, skipping...\n");
        return;
    }
    
    is_loading = true;
    printf("UserMap: Starting tile loading...\n");
    
    // Show loading popup
    show_loading_popup();
    
    // Use a single-shot timer with longer delay to reduce CPU pressure
    lv_timer_create([](lv_timer_t *t) {
        lv_timer_del(t);  // Delete this timer immediately
        
        uint32_t start_time = esp_timer_get_time() / 1000;
        
        // Get current tile position
        int tx, ty;
        map_tiles_get_position(map_handle, &tx, &ty);
        
        printf("UserMap: Loading %dx%d grid at position (%d,%d) at zoom %d\n", 
               grid_cols, grid_rows, tx, ty, map_tiles_get_zoom(map_handle));
        
        // Disable automatic invalidation during bulk updates
        lv_obj_add_flag(map_group, LV_OBJ_FLAG_HIDDEN);
        
        // Load grid of tiles - minimize style changes
        for (int row = 0; row < grid_rows; row++) {
            for (int col = 0; col < grid_cols; col++) {
                int index = row * grid_cols + col;
                int tile_x = tx + col;
                int tile_y = ty + row;
                
                // Load tile from SD card
                bool loaded = map_tiles_load_tile(map_handle, index, tile_x, tile_y);
                
                if (loaded) {
                    // Get the tile image data and set it
                    lv_image_dsc_t* tile_img = map_tiles_get_image(map_handle, index);
                    if (tile_img && tile_img->data) {
                        lv_image_set_src(tile_widgets[index], (const void *)tile_img);
                        // Only clear background if we have valid image
                        lv_obj_set_style_bg_opa(tile_widgets[index], LV_OPA_TRANSP, 0);
                    }
                }
            }
        }
        
        // Re-enable visibility and trigger single invalidation
        lv_obj_clear_flag(map_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(map_group);  // Single invalidation instead of continuous ones
        
        uint32_t end_time = esp_timer_get_time() / 1000;
        printf("UserMap: Tile loading completed in %lu ms\n", end_time - start_time);
        
        // Update GPS marker position after tiles are reloaded
        if (gps_marker) {
            update_gps_marker_position();
        }
        
        // Hide loading popup
        hide_loading_popup();
        
        // Clear loading flag
        is_loading = false;
        
    }, 50, NULL);  // Increased delay to reduce CPU pressure
}

void UserMap::map_scroll_event_cb(lv_event_t *e)
{
    if (!initialized || !map_handle) return;
    
    lv_event_code_t event_code = lv_event_get_code(e);
    
    // Handle real-time GPS coordinate updates during scrolling
    if (event_code == LV_EVENT_SCROLL) {
        uint32_t current_time = esp_timer_get_time() / 1000;
        if (current_time - last_gps_update_time >= 50) {
            update_current_gps_from_map_center();
            last_gps_update_time = current_time;
        }
        // Update GPS direction arrows during scroll
        update_gps_direction_arrows();
        // Update all remote marker direction arrows
        update_all_remote_markers();
        return;
    }
    
    // Handle tile loading when scroll ends
    if (event_code == LV_EVENT_SCROLL_END) {
        // Don't process scroll events if already loading
        if (is_loading) {
            return;
        }
        
        // Add debouncing - ignore rapid scroll events
        uint32_t current_time = esp_timer_get_time() / 1000;
        if (current_time - last_scroll_time < 100) { // 100ms debounce
            return;
        }
        last_scroll_time = current_time;
        
        bool needUpdate = false;
        int tile_x, tile_y;
        map_tiles_get_position(map_handle, &tile_x, &tile_y);

        if (lv_obj_get_scroll_left(map_container) < -90)
        {
            printf("UserMap: at_left\n");
            tile_x -= 1;
            needUpdate = true;
        }
        else if (lv_obj_get_scroll_top(map_container) < -90)
        {
            printf("UserMap: at_top\n");
            tile_y -= 1;
            needUpdate = true;
        }
        else if (lv_obj_get_scroll_right(map_container) < -90)
        {
            printf("UserMap: at_right\n");
            tile_x += 1;
            needUpdate = true;
        }
        else if (lv_obj_get_scroll_bottom(map_container) < -90)
        {
            printf("UserMap: at_bottom\n");
            tile_y += 1;
            needUpdate = true;
        }

        if (needUpdate) {
            map_tiles_set_position(map_handle, tile_x, tile_y);
            map_tiles_get_position(map_handle, &cached_tile_x, &cached_tile_y);
            load_map_tiles();
        }
        
        update_current_gps_from_map_center();
        
        // Update all remote markers after scroll
        update_all_remote_markers();
    }
}

void UserMap::create_zoom_panel(lv_obj_t* parent_screen)
{
    // Create zoom panel at bottom left
    lv_obj_t* zoom_panel = lv_obj_create(parent_screen);
    lv_obj_set_scrollbar_mode(zoom_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(zoom_panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(zoom_panel, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_set_style_bg_color(zoom_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(zoom_panel, LV_OPA_50, 0);  // Increased transparency
    lv_obj_set_style_border_width(zoom_panel, 0, 0);
    lv_obj_set_style_radius(zoom_panel, 4, 0);
    lv_obj_set_style_pad_all(zoom_panel, 5, 0);
    lv_obj_clear_flag(zoom_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(zoom_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(zoom_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(zoom_panel, 5, 0);

    // Create zoom label
    zoom_label = lv_label_create(zoom_panel);
    lv_label_set_text_fmt(zoom_label, "Zoom: %d", current_zoom);
    lv_obj_set_style_text_color(zoom_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(zoom_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(zoom_label, &lv_font_montserrat_14, 0);

    // Create plus button (moved before minus)
    zoom_plus_button = lv_btn_create(zoom_panel);
    lv_obj_set_size(zoom_plus_button, 35, 30);
    lv_obj_set_style_bg_color(zoom_plus_button, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_add_event_cb(zoom_plus_button, zoom_plus_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* plus_label = lv_label_create(zoom_plus_button);
    lv_label_set_text(plus_label, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(plus_label, &lv_font_montserrat_24, 0);  
    lv_obj_center(plus_label);

    // Create minus button (moved after plus)
    zoom_minus_button = lv_btn_create(zoom_panel);
    lv_obj_set_size(zoom_minus_button, 35, 30);
    lv_obj_set_style_bg_color(zoom_minus_button, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_add_event_cb(zoom_minus_button, zoom_minus_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* minus_label = lv_label_create(zoom_minus_button);
    lv_label_set_text(minus_label, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(minus_label, &lv_font_montserrat_24, 0);
    lv_obj_center(minus_label);

    // Create Set button
    settings_button = lv_btn_create(zoom_panel);
    lv_obj_set_size(settings_button, 50, 30);
    lv_obj_set_style_bg_color(settings_button, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(settings_button, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(settings_button, settings_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* btn_label = lv_label_create(settings_button);
    lv_label_set_text(btn_label, "Set");
    lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
    lv_obj_center(btn_label);
}

void UserMap::zoom_plus_event_cb(lv_event_t *e)
{
    if (pending_zoom < 19) {
        pending_zoom++;
        if (zoom_label) {
            if (pending_zoom != current_zoom) {
                lv_label_set_text_fmt(zoom_label, "Zoom: %d*", pending_zoom);
            } else {
                lv_label_set_text_fmt(zoom_label, "Zoom: %d", pending_zoom);
            }
        }
        printf("UserMap: Zoom + clicked, pending zoom: %d\n", pending_zoom);
    }
}

void UserMap::zoom_minus_event_cb(lv_event_t *e)
{
    if (pending_zoom > 10) {
        pending_zoom--;
        if (zoom_label) {
            if (pending_zoom != current_zoom) {
                lv_label_set_text_fmt(zoom_label, "Zoom: %d*", pending_zoom);
            } else {
                lv_label_set_text_fmt(zoom_label, "Zoom: %d", pending_zoom);
            }
        }
        printf("UserMap: Zoom - clicked, pending zoom: %d\n", pending_zoom);
    }
}

void UserMap::settings_button_event_cb(lv_event_t *e)
{
    printf("UserMap: Set button clicked\n");
    
    // Apply pending zoom level
    if (pending_zoom != current_zoom) {
        printf("UserMap: Applying zoom level %d\n", pending_zoom);
        change_zoom_level(pending_zoom);
    }
}

void UserMap::update_zoom_label()
{
    if (zoom_label) {
        if (pending_zoom != current_zoom) {
            lv_label_set_text_fmt(zoom_label, "Zoom: %d*", current_zoom);
        } else {
            lv_label_set_text_fmt(zoom_label, "Zoom: %d", current_zoom);
        }
    }
}

void UserMap::get_gps_coordinates(double* latitude, double* longitude) {
    if (latitude) *latitude = current_lat;
    if (longitude) *longitude = current_lon;
}

void UserMap::update_battery_voltage(float voltage) {
    if (!battery_voltage_label) return;
    
    char voltage_str[20];
    snprintf(voltage_str, sizeof(voltage_str), LV_SYMBOL_BATTERY_FULL " %.2fV", voltage);
    lv_label_set_text(battery_voltage_label, voltage_str);
}

void UserMap::show_loading_popup()
{
    if (loading_popup) return;
   
    // Thin fullscreen overlay (light opacity)
    loading_popup = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(loading_popup);
    lv_obj_set_size(loading_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(loading_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(loading_popup, LV_OPA_40, 0); // lighter dim
    lv_obj_set_style_border_width(loading_popup, 0, 0);

    // Small pill card centered
    lv_obj_t* card = lv_obj_create(loading_popup);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 170, 42);
    lv_obj_center(card);

    // Simple, high-contrast look (no shadow/gradient)
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_pad_all(card, 8, 0);

    // Single static label
    lv_obj_t* label = lv_label_create(card);
    lv_label_set_text(label, "Loading map...");
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_center(label);

    lv_obj_move_foreground(loading_popup);
}

void UserMap::hide_loading_popup()
{
    if (loading_popup) {
        lv_obj_delete(loading_popup);
        loading_popup = nullptr;
    }
}

void UserMap::center_map_on_gps()
{
    if (!initialized || !map_handle || !map_group || !map_container) {
        printf("UserMap: Map not properly initialized for centering\n");
        return;
    }

    // Convert current GPS coordinates to tile coordinates
    double x, y;
    map_tiles_gps_to_tile_xy(map_handle, current_lat, current_lon, &x, &y);
    
    // Get current tile position (top-left of the 5x5 grid)
    int tile_x, tile_y;
    map_tiles_get_position(map_handle, &tile_x, &tile_y);
    
    // Calculate absolute pixel position of GPS coordinates
    int abs_px = (int)(x * MAP_TILES_TILE_SIZE);
    int abs_py = (int)(y * MAP_TILES_TILE_SIZE);
    
    // Calculate top-left pixel position of current tile grid
    int top_left_px_x = tile_x * MAP_TILES_TILE_SIZE;
    int top_left_px_y = tile_y * MAP_TILES_TILE_SIZE;
    
    // Calculate scroll position to center GPS coordinates in the map container
    int center_scroll_x = abs_px - top_left_px_x - lv_obj_get_width(map_container) / 2;
    int center_scroll_y = abs_py - top_left_px_y - lv_obj_get_height(map_container) / 2;
    
    // Apply the scroll to center the GPS coordinates
    lv_obj_scroll_to(map_container, center_scroll_x, center_scroll_y, LV_ANIM_OFF);
}

void UserMap::update_current_gps_from_map_center()
{
    if (!initialized || !map_handle || !map_container) {
        return;
    }
    
    // Get current scroll position
    lv_coord_t scroll_x = lv_obj_get_scroll_x(map_container);
    lv_coord_t scroll_y = lv_obj_get_scroll_y(map_container);
    
    // Get screen center offset
    lv_coord_t screen_center_x = lv_obj_get_width(map_container) / 2;
    lv_coord_t screen_center_y = lv_obj_get_height(map_container) / 2;
    
    // Calculate the center pixel position relative to the map
    int center_pixel_x = scroll_x + screen_center_x;
    int center_pixel_y = scroll_y + screen_center_y;
    
    // Get current tile position (top-left of the grid)
    int tile_x, tile_y;
    map_tiles_get_position(map_handle, &tile_x, &tile_y);
    
    // Calculate top-left pixel position of current tile grid
    int top_left_px_x = tile_x * MAP_TILES_TILE_SIZE;
    int top_left_px_y = tile_y * MAP_TILES_TILE_SIZE;
    
    // Calculate absolute pixel position
    int abs_pixel_x = top_left_px_x + center_pixel_x;
    int abs_pixel_y = top_left_px_y + center_pixel_y;
    
    // Convert absolute pixel position to tile coordinates
    double tile_coord_x = (double)abs_pixel_x / MAP_TILES_TILE_SIZE;
    double tile_coord_y = (double)abs_pixel_y / MAP_TILES_TILE_SIZE;
    
    // Convert tile coordinates to GPS coordinates
    double new_lat, new_lon;
    map_tiles_tile_xy_to_gps(map_handle, tile_coord_x, tile_coord_y, &new_lat, &new_lon);
    
    // Update current GPS coordinates
    current_lat = new_lat;
    current_lon = new_lon;
    
    // Update coordinate display labels
    if (lat_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " Lat: %.6f", current_lat);
        lv_label_set_text(lat_label, buf);
    }
    if (lon_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Lon: %.6f", current_lon);
        lv_label_set_text(lon_label, buf);
    }
    
    // GPS coordinates updated silently for better performance
}

void UserMap::change_zoom_level(int new_zoom) {
    if (!initialized || !map_handle) {
        return;
    }
    
    if (new_zoom == current_zoom) {
        return;
    }
    
    // Set zoom level in the component first
    map_tiles_set_zoom(map_handle, new_zoom);
    
    // Update current zoom after setting it in the component
    current_zoom = new_zoom;
    pending_zoom = new_zoom;  // Sync pending zoom with current
    
    // Update zoom label
    update_zoom_label();
    
    // Recalculate tile positions for the new zoom level centered on current GPS coordinates
    map_tiles_set_center_from_gps(map_handle, current_lat, current_lon);
    
    // Cache the tile position IMMEDIATELY after setting center
    map_tiles_get_position(map_handle, &cached_tile_x, &cached_tile_y);
    
    // Force reload all tiles for the new zoom level
    load_map_tiles();
    
    // Center the map view and update GPS marker after tiles are loaded
    lv_timer_create([](lv_timer_t *t) {
        lv_timer_del(t);
        center_map_on_gps();
        if (gps_marker) {
            update_gps_marker_position();
        }
        // Update all remote markers to new zoom level positions
        for (int i = 0; i < remote_marker_count; i++) {
            if (remote_markers[i].active && remote_markers[i].marker_obj) {
                update_remote_marker_position(&remote_markers[i]);
                update_remote_direction_arrows(&remote_markers[i]);
            }
        }
    }, 200, NULL);
}

void UserMap::get_current_location(double* latitude, double* longitude) {
    // Return actual GPS position, not map center
    if (latitude) *latitude = gps_lat;
    if (longitude) *longitude = gps_lon;
}

void UserMap::get_map_center_coordinates(double* latitude, double* longitude) {
    if (!initialized || !map_handle || !map_container) {
        if (latitude) *latitude = current_lat;
        if (longitude) *longitude = current_lon;
        return;
    }
    
    // Get current scroll position
    lv_coord_t scroll_x = lv_obj_get_scroll_x(map_container);
    lv_coord_t scroll_y = lv_obj_get_scroll_y(map_container);
    
    // Get screen center offset
    lv_coord_t screen_center_x = lv_obj_get_width(map_container) / 2;
    lv_coord_t screen_center_y = lv_obj_get_height(map_container) / 2;
    
    // Calculate the center pixel position relative to the map
    int center_pixel_x = scroll_x + screen_center_x;
    int center_pixel_y = scroll_y + screen_center_y;
    
    // Get current tile position (top-left of the grid)
    int tile_x, tile_y;
    map_tiles_get_position(map_handle, &tile_x, &tile_y);
    
    // Calculate top-left pixel position of current tile grid
    int top_left_px_x = tile_x * MAP_TILES_TILE_SIZE;
    int top_left_px_y = tile_y * MAP_TILES_TILE_SIZE;
    
    // Calculate absolute pixel position
    int abs_pixel_x = top_left_px_x + center_pixel_x;
    int abs_pixel_y = top_left_px_y + center_pixel_y;
    
    // Convert absolute pixel position to tile coordinates
    double tile_coord_x = (double)abs_pixel_x / MAP_TILES_TILE_SIZE;
    double tile_coord_y = (double)abs_pixel_y / MAP_TILES_TILE_SIZE;
    
    // Convert tile coordinates to GPS coordinates
    double new_lat, new_lon;
    map_tiles_tile_xy_to_gps(map_handle, tile_coord_x, tile_coord_y, &new_lat, &new_lon);
    
    if (latitude) *latitude = new_lat;
    if (longitude) *longitude = new_lon;
}

int UserMap::get_current_zoom() {
    return current_zoom;
}

void UserMap::create_gps_marker() {
    if (!map_group) {
        return;
    }
    
    if (gps_marker) {
        return;
    }
    
    // Create marker container (holds icon and label)
    gps_marker = lv_obj_create(map_group);
    lv_obj_set_size(gps_marker, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(gps_marker, 0, 0);
    lv_obj_set_style_border_width(gps_marker, 0, 0);
    lv_obj_set_style_bg_opa(gps_marker, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(gps_marker, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create marker icon using device-specific icon (64x64)
    gps_marker_icon = lv_image_create(gps_marker);
    const device_config_t* local_device_cfg = device_get_local_config();
    lv_image_set_src(gps_marker_icon, local_device_cfg->icon);
    lv_obj_align(gps_marker_icon, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Scale based on zoom level (zoom 18+ = 100%, zoom 10 = 50%)
    // Formula: scale = 50 + ((zoom - 10) / 8) * 50, clamped to 50-100%
    int zoom_clamped = current_zoom < 10 ? 10 : (current_zoom > 18 ? 18 : current_zoom);
    int scale_percent = 50 + ((zoom_clamped - 10) * 50) / 8;  // 50% to 100%
    lv_image_set_scale(gps_marker_icon, scale_percent * 256 / 100);  // LVGL uses 256 = 100%
    
    // Create GPS coordinate label with device name
    gps_marker_label = lv_label_create(gps_marker);
    char buf[80];
    snprintf(buf, sizeof(buf), "%s\n%.6f,%.6f", local_device_cfg->name, gps_lat, gps_lon);
    lv_label_set_text(gps_marker_label, buf);
    lv_obj_align_to(gps_marker_label, gps_marker_icon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);  // 6px spacing to the right of icon
    lv_obj_set_style_bg_color(gps_marker_label, lv_color_hex(local_device_cfg->color), 0);  // Use device color
    lv_obj_set_style_bg_opa(gps_marker_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(gps_marker_label, 4, 0);
    lv_obj_set_style_radius(gps_marker_label, 4, 0);
    lv_obj_set_style_text_color(gps_marker_label, lv_color_white(), 0);
    
    // Move to top layer so it's always visible above tiles
    lv_obj_move_to_index(gps_marker, -1);
}

void UserMap::update_gps_marker_position() {
    if (!gps_marker || !map_handle || !map_group) return;
    
    // Calculate scale based on current zoom level (zoom 10=20%, zoom 18=100%)
    int zoom_clamped = current_zoom < 10 ? 10 : (current_zoom > 18 ? 18 : current_zoom);
    int scale_percent = 20 + ((zoom_clamped - 10) * 80) / 8;  // 20% to 100%
    
    // Update marker scale
    if (gps_marker_icon) {
        lv_image_set_scale(gps_marker_icon, scale_percent * 256 / 100);
    }
    
    double tile_x, tile_y;
    map_tiles_gps_to_tile_xy(map_handle, gps_lat, gps_lon, &tile_x, &tile_y);

    double abs_px = tile_x * MAP_TILES_TILE_SIZE;
    double abs_py = tile_y * MAP_TILES_TILE_SIZE;

    int top_left_tile_px_x = cached_tile_x * MAP_TILES_TILE_SIZE;
    int top_left_tile_px_y = cached_tile_y * MAP_TILES_TILE_SIZE;

    int marker_px_x = (int)(abs_px - top_left_tile_px_x);
    int marker_px_y = (int)(abs_py - top_left_tile_px_y);

    // Calculate actual icon size and offset for positioning
    int icon_size = (64 * scale_percent) / 100;
    int offset = icon_size / 2;
    
    lv_obj_set_pos(gps_marker, marker_px_x - offset, marker_px_y - offset);
    
    // Update label text and position relative to icon
    if (gps_marker_label) {
        const device_config_t* local_device_cfg = device_get_local_config();
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\n%.8f,%.8f", local_device_cfg->name, gps_lat, gps_lon);
        lv_label_set_text(gps_marker_label, buf);
        // Reposition label to stay aligned with scaled icon
        lv_obj_align_to(gps_marker_label, gps_marker_icon, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
        printf("UserMap: GPS label updated to %.8f,%.8f\n", gps_lat, gps_lon);
    }
    
    // Update direction arrows after marker position changes
    update_gps_direction_arrows();
}

void UserMap::create_gps_direction_arrows() {
    if (gps_arrow_left) {
        return;
    }
    
    if (!parent_scr) {
        return;
    }
    
    // Get local device configuration for icon
    const device_config_t* local_device_cfg = device_get_local_config();
    lv_color_t arrow_color = lv_color_hex(local_device_cfg->color);
    
    // Create LEFT arrow indicator (image with icon)
    gps_arrow_left = lv_obj_create(parent_scr);
    lv_obj_set_size(gps_arrow_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(gps_arrow_left, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(gps_arrow_left, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(gps_arrow_left, LV_OPA_70, 0);
    lv_obj_set_style_border_width(gps_arrow_left, 2, 0);
    lv_obj_set_style_border_color(gps_arrow_left, arrow_color, 0);
    lv_obj_set_style_radius(gps_arrow_left, 8, 0);
    lv_obj_set_style_pad_all(gps_arrow_left, 8, 0);
    lv_obj_add_flag(gps_arrow_left, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* left_label = lv_label_create(gps_arrow_left);
    lv_label_set_text(left_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(left_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(left_label, arrow_color, 0);
    
    // Create RIGHT arrow indicator
    gps_arrow_right = lv_obj_create(parent_scr);
    lv_obj_set_size(gps_arrow_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(gps_arrow_right, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(gps_arrow_right, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(gps_arrow_right, LV_OPA_70, 0);
    lv_obj_set_style_border_width(gps_arrow_right, 2, 0);
    lv_obj_set_style_border_color(gps_arrow_right, arrow_color, 0);
    lv_obj_set_style_radius(gps_arrow_right, 8, 0);
    lv_obj_set_style_pad_all(gps_arrow_right, 8, 0);
    lv_obj_add_flag(gps_arrow_right, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* right_label = lv_label_create(gps_arrow_right);
    lv_label_set_text(right_label, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(right_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(right_label, arrow_color, 0);
    
    // Create TOP arrow indicator
    gps_arrow_top = lv_obj_create(parent_scr);
    lv_obj_set_size(gps_arrow_top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(gps_arrow_top, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_color(gps_arrow_top, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(gps_arrow_top, LV_OPA_70, 0);
    lv_obj_set_style_border_width(gps_arrow_top, 2, 0);
    lv_obj_set_style_border_color(gps_arrow_top, arrow_color, 0);
    lv_obj_set_style_radius(gps_arrow_top, 8, 0);
    lv_obj_set_style_pad_all(gps_arrow_top, 8, 0);
    lv_obj_add_flag(gps_arrow_top, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* top_label = lv_label_create(gps_arrow_top);
    lv_label_set_text(top_label, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(top_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(top_label, arrow_color, 0);
    
    // Create BOTTOM arrow indicator
    gps_arrow_bottom = lv_obj_create(parent_scr);
    lv_obj_set_size(gps_arrow_bottom, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(gps_arrow_bottom, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(gps_arrow_bottom, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(gps_arrow_bottom, LV_OPA_70, 0);
    lv_obj_set_style_border_width(gps_arrow_bottom, 2, 0);
    lv_obj_set_style_border_color(gps_arrow_bottom, arrow_color, 0);
    lv_obj_set_style_radius(gps_arrow_bottom, 8, 0);
    lv_obj_set_style_pad_all(gps_arrow_bottom, 8, 0);
    lv_obj_add_flag(gps_arrow_bottom, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* bottom_label = lv_label_create(gps_arrow_bottom);
    lv_label_set_text(bottom_label, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(bottom_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(bottom_label, arrow_color, 0);
}

void UserMap::update_gps_direction_arrows() {
    if (!gps_arrow_left || !gps_arrow_right || !gps_arrow_top || !gps_arrow_bottom) return;
    if (!map_container || !gps_marker) return;
    
    // Update arrow colors to match current device configuration
    const device_config_t* local_device_cfg = device_get_local_config();
    lv_color_t arrow_color = lv_color_hex(local_device_cfg->color);
    
    lv_obj_set_style_border_color(gps_arrow_left, arrow_color, 0);
    lv_obj_set_style_border_color(gps_arrow_right, arrow_color, 0);
    lv_obj_set_style_border_color(gps_arrow_top, arrow_color, 0);
    lv_obj_set_style_border_color(gps_arrow_bottom, arrow_color, 0);
    
    lv_obj_t* left_label = lv_obj_get_child(gps_arrow_left, 0);
    lv_obj_t* right_label = lv_obj_get_child(gps_arrow_right, 0);
    lv_obj_t* top_label = lv_obj_get_child(gps_arrow_top, 0);
    lv_obj_t* bottom_label = lv_obj_get_child(gps_arrow_bottom, 0);
    
    if (left_label) lv_obj_set_style_text_color(left_label, arrow_color, 0);
    if (right_label) lv_obj_set_style_text_color(right_label, arrow_color, 0);
    if (top_label) lv_obj_set_style_text_color(top_label, arrow_color, 0);
    if (bottom_label) lv_obj_set_style_text_color(bottom_label, arrow_color, 0);
    
    // Hide all arrows by default
    lv_obj_add_flag(gps_arrow_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(gps_arrow_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(gps_arrow_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(gps_arrow_bottom, LV_OBJ_FLAG_HIDDEN);
    
    // Check visibility first
    if (is_marker_visible()) {
        return;
    }
    
    // Get screen bounds
    lv_area_t scroll_area;
    lv_obj_get_coords(map_container, &scroll_area);
    
    // Get marker position in screen coordinates
    lv_coord_t marker_x = lv_obj_get_x(gps_marker);
    lv_coord_t marker_y = lv_obj_get_y(gps_marker);
    lv_coord_t group_x = lv_obj_get_x(map_group);
    lv_coord_t group_y = lv_obj_get_y(map_group);
    lv_coord_t scroll_x = lv_obj_get_scroll_x(map_container);
    lv_coord_t scroll_y = lv_obj_get_scroll_y(map_container);
    
    int marker_screen_x = group_x + marker_x - scroll_x + scroll_area.x1;
    int marker_screen_y = group_y + marker_y - scroll_y + scroll_area.y1;
    
    // Show appropriate arrows based on marker position
    // Horizontal arrows (primary for 640x172 display)
    if (marker_screen_x < scroll_area.x1) {
        lv_obj_clear_flag(gps_arrow_left, LV_OBJ_FLAG_HIDDEN);
    } else if (marker_screen_x > scroll_area.x2) {
        lv_obj_clear_flag(gps_arrow_right, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Vertical arrows
    if (marker_screen_y < scroll_area.y1) {
        lv_obj_clear_flag(gps_arrow_top, LV_OBJ_FLAG_HIDDEN);
    } else if (marker_screen_y > scroll_area.y2) {
        lv_obj_clear_flag(gps_arrow_bottom, LV_OBJ_FLAG_HIDDEN);
    }
}

bool UserMap::is_marker_visible() {
    if (!gps_marker_icon || !map_container) return false;
    
    lv_area_t marker_area;
    lv_area_t scroll_area;
    
    lv_obj_get_coords(gps_marker_icon, &marker_area);
    lv_obj_get_coords(map_container, &scroll_area);
    
    bool visible = 
        marker_area.x2 >= scroll_area.x1 &&
        marker_area.x1 <= scroll_area.x2 &&
        marker_area.y2 >= scroll_area.y1 &&
        marker_area.y1 <= scroll_area.y2;
    
    return visible;
}

void UserMap::create_notification_border() {
    if (notification_border) {
        return;
    }
    
    if (!parent_scr) {
        return;
    }
    
    // Get screen dimensions
    lv_display_t *disp = lv_display_get_default();
    lv_coord_t screen_width = lv_display_get_horizontal_resolution(disp);
    lv_coord_t screen_height = lv_display_get_vertical_resolution(disp);
    
    // Create border object on parent screen (covers full screen with border)
    notification_border = lv_obj_create(parent_scr);
    lv_obj_set_size(notification_border, screen_width, screen_height);
    lv_obj_center(notification_border);
    
    // Make background transparent
    lv_obj_set_style_bg_opa(notification_border, LV_OPA_TRANSP, 0);
    
    // Style: 4px border width, 50% transparent, gray color (default state)
    lv_obj_set_style_border_width(notification_border, 4, 0);
    lv_obj_set_style_border_color(notification_border, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_border_opa(notification_border, LV_OPA_50, 0);
    lv_obj_set_style_radius(notification_border, 0, 0);
    
    // Make it non-interactive so it doesn't interfere with map
    lv_obj_clear_flag(notification_border, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(notification_border, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(notification_border, LV_OBJ_FLAG_EVENT_BUBBLE);  // Let events pass through
    
    // Move to top layer
    lv_obj_move_to_index(notification_border, -1);
    
    printf("UserMap: Notification border created (gray state) - optimized for 640x172\n");
}

void UserMap::set_notification_state(int state) {
    if (!notification_border) return;
    
    switch (state) {
        case 0:  // Gray - normal
            lv_obj_set_style_border_color(notification_border, lv_palette_main(LV_PALETTE_GREY), 0);
            break;
        case 1:  // Green - good
            lv_obj_set_style_border_color(notification_border, lv_palette_main(LV_PALETTE_GREEN), 0);
            break;
        case 2:  // Yellow - warning
            lv_obj_set_style_border_color(notification_border, lv_palette_main(LV_PALETTE_YELLOW), 0);
            break;
        case 3:  // Red - error/alert
            lv_obj_set_style_border_color(notification_border, lv_palette_main(LV_PALETTE_RED), 0);
            break;
        default:
            break;
    }
}

void UserMap::cleanup() {
    if (map_container) {
        lv_obj_delete(map_container);
        map_container = nullptr;
    }
    
    // Zoom panel is part of parent screen, will be cleaned up automatically
    
    if (loading_popup) {
        lv_obj_delete(loading_popup);
        loading_popup = nullptr;
    }
    
    // Clean up GPS direction arrows
    if (gps_arrow_left) {
        lv_obj_delete(gps_arrow_left);
        gps_arrow_left = nullptr;
    }
    if (gps_arrow_right) {
        lv_obj_delete(gps_arrow_right);
        gps_arrow_right = nullptr;
    }
    if (gps_arrow_top) {
        lv_obj_delete(gps_arrow_top);
        gps_arrow_top = nullptr;
    }
    if (gps_arrow_bottom) {
        lv_obj_delete(gps_arrow_bottom);
        gps_arrow_bottom = nullptr;
    }
    
    if (notification_border) {
        lv_obj_delete(notification_border);
        notification_border = nullptr;
    }
    
    if (distance_label) {
        lv_obj_delete(distance_label);
        distance_label = nullptr;
    }
    
    if (km_distance_label) {
        lv_obj_delete(km_distance_label);
        km_distance_label = nullptr;
    }
    
    map_group = nullptr;  // Will be cleaned up when map_container is deleted
    gps_marker = nullptr;  // Will be cleaned up when map_container is deleted
    zoom_label = nullptr;
    zoom_plus_button = nullptr;
    zoom_minus_button = nullptr;
    settings_button = nullptr;
    
    // Clean up the map tiles component
    if (map_handle) {
        map_tiles_cleanup(map_handle);
        map_handle = nullptr;
    }
    
    // Clean up tile widgets array
    if (tile_widgets) {
        free(tile_widgets);
        tile_widgets = nullptr;
    }
    
    // Reset cached grid dimensions
    grid_cols = 0;
    grid_rows = 0;
    tile_count = 0;
    
    // Reset loading flag
    is_loading = false;
    last_scroll_time = 0;
    last_gps_update_time = 0;
    
    // Clean up remote markers
    for (int i = 0; i < remote_marker_count; i++) {
        if (remote_markers[i].marker_obj) {
            lv_obj_delete(remote_markers[i].marker_obj);
        }
        if (remote_markers[i].arrow_left) {
            lv_obj_delete(remote_markers[i].arrow_left);
        }
        if (remote_markers[i].arrow_right) {
            lv_obj_delete(remote_markers[i].arrow_right);
        }
        if (remote_markers[i].arrow_top) {
            lv_obj_delete(remote_markers[i].arrow_top);
        }
        if (remote_markers[i].arrow_bottom) {
            lv_obj_delete(remote_markers[i].arrow_bottom);
        }
    }
    remote_marker_count = 0;
    
    initialized = false;
}

// ===== REMOTE MARKER FUNCTIONS =====

UserMap::RemoteMarker* UserMap::find_remote_marker(uint16_t device_id) {
    for (int i = 0; i < remote_marker_count; i++) {
        if (remote_markers[i].device_id == device_id) {
            return &remote_markers[i];
        }
    }
    return nullptr;
}

UserMap::RemoteMarker* UserMap::get_or_create_remote_marker(uint16_t device_id) {
    // Try to find existing marker
    RemoteMarker* marker = find_remote_marker(device_id);
    if (marker) {
        return marker;
    }
    
    // Create new marker if space available
    if (remote_marker_count < MAX_REMOTE_MARKERS) {
        marker = &remote_markers[remote_marker_count++];
        marker->device_id = device_id;
        marker->latitude = 0.0;
        marker->longitude = 0.0;
        marker->last_update_ms = 0;
        marker->marker_obj = nullptr;
        marker->marker_icon = nullptr;
        marker->marker_label = nullptr;
        marker->arrow_left = nullptr;
        marker->arrow_right = nullptr;
        marker->arrow_top = nullptr;
        marker->arrow_bottom = nullptr;
        return marker;
    }
    
    return nullptr;
}

void UserMap::create_remote_marker_objects(RemoteMarker* marker) {
    if (!marker || !map_group) return;
    
    // Create marker container
    marker->marker_obj = lv_obj_create(map_group);
    lv_obj_set_size(marker->marker_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(marker->marker_obj, 0, 0);
    lv_obj_set_style_border_width(marker->marker_obj, 0, 0);
    lv_obj_set_style_bg_opa(marker->marker_obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(marker->marker_obj, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create marker icon using device-specific icon
    marker->marker_icon = lv_image_create(marker->marker_obj);
    const device_config_t* device_cfg = device_get_config((device_type_t)marker->device_type);
    lv_image_set_src(marker->marker_icon, device_cfg->icon);
    lv_obj_align(marker->marker_icon, LV_ALIGN_LEFT_MID, 0, 0);
    // Original size (64x64) - no scaling needed
    
    // Create label with device name, coordinates, and time
    marker->marker_label = lv_label_create(marker->marker_obj);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s (ID:%u)\\n%.6f,%.6f\\n0s ago", 
             marker->device_name, marker->device_id, marker->latitude, marker->longitude);
    lv_label_set_text(marker->marker_label, buf);
    lv_obj_align_to(marker->marker_label, marker->marker_icon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);
    lv_obj_set_style_bg_color(marker->marker_label, lv_color_hex(device_cfg->color), 0);  // Use device color
    lv_obj_set_style_bg_opa(marker->marker_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(marker->marker_label, 4, 0);
    lv_obj_set_style_radius(marker->marker_label, 4, 0);
    lv_obj_set_style_text_color(marker->marker_label, lv_color_white(), 0);
    
    // Create direction arrows (initially hidden) - optimized for rectangular 640x172 display
    if (parent_scr) {
        const device_config_t* device_cfg = device_get_config((device_type_t)marker->device_type);
        lv_color_t arrow_color = lv_color_hex(device_cfg->color);
        
        // Create LEFT arrow
        marker->arrow_left = lv_obj_create(parent_scr);
        lv_obj_set_size(marker->arrow_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(marker->arrow_left, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_bg_color(marker->arrow_left, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(marker->arrow_left, LV_OPA_70, 0);
        lv_obj_set_style_border_width(marker->arrow_left, 2, 0);
        lv_obj_set_style_border_color(marker->arrow_left, arrow_color, 0);
        lv_obj_set_style_radius(marker->arrow_left, 8, 0);
        lv_obj_set_style_pad_all(marker->arrow_left, 8, 0);
        lv_obj_add_flag(marker->arrow_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* left_lbl = lv_label_create(marker->arrow_left);
        lv_label_set_text(left_lbl, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(left_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(left_lbl, arrow_color, 0);
        
        // Create RIGHT arrow
        marker->arrow_right = lv_obj_create(parent_scr);
        lv_obj_set_size(marker->arrow_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(marker->arrow_right, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_bg_color(marker->arrow_right, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(marker->arrow_right, LV_OPA_70, 0);
        lv_obj_set_style_border_width(marker->arrow_right, 2, 0);
        lv_obj_set_style_border_color(marker->arrow_right, arrow_color, 0);
        lv_obj_set_style_radius(marker->arrow_right, 8, 0);
        lv_obj_set_style_pad_all(marker->arrow_right, 8, 0);
        lv_obj_add_flag(marker->arrow_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* right_lbl = lv_label_create(marker->arrow_right);
        lv_label_set_text(right_lbl, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(right_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(right_lbl, arrow_color, 0);
        
        // Create TOP arrow
        marker->arrow_top = lv_obj_create(parent_scr);
        lv_obj_set_size(marker->arrow_top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(marker->arrow_top, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_style_bg_color(marker->arrow_top, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(marker->arrow_top, LV_OPA_70, 0);
        lv_obj_set_style_border_width(marker->arrow_top, 2, 0);
        lv_obj_set_style_border_color(marker->arrow_top, arrow_color, 0);
        lv_obj_set_style_radius(marker->arrow_top, 8, 0);
        lv_obj_set_style_pad_all(marker->arrow_top, 8, 0);
        lv_obj_add_flag(marker->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* top_lbl = lv_label_create(marker->arrow_top);
        lv_label_set_text(top_lbl, LV_SYMBOL_UP);
        lv_obj_set_style_text_font(top_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(top_lbl, arrow_color, 0);
        
        // Create BOTTOM arrow
        marker->arrow_bottom = lv_obj_create(parent_scr);
        lv_obj_set_size(marker->arrow_bottom, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(marker->arrow_bottom, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_bg_color(marker->arrow_bottom, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(marker->arrow_bottom, LV_OPA_70, 0);
        lv_obj_set_style_border_width(marker->arrow_bottom, 2, 0);
        lv_obj_set_style_border_color(marker->arrow_bottom, arrow_color, 0);
        lv_obj_set_style_radius(marker->arrow_bottom, 8, 0);
        lv_obj_set_style_pad_all(marker->arrow_bottom, 8, 0);
        lv_obj_add_flag(marker->arrow_bottom, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* bottom_lbl = lv_label_create(marker->arrow_bottom);
        lv_label_set_text(bottom_lbl, LV_SYMBOL_DOWN);
        lv_obj_set_style_text_font(bottom_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(bottom_lbl, arrow_color, 0);
    }
    
    // Move marker to top layer
    lv_obj_move_to_index(marker->marker_obj, -1);
}

void UserMap::update_remote_marker_position(RemoteMarker* marker) {
    if (!marker || !marker->marker_obj || !map_handle || !map_group) return;
    
    double tile_x, tile_y;
    map_tiles_gps_to_tile_xy(map_handle, marker->latitude, marker->longitude, &tile_x, &tile_y);
    
    double abs_px = tile_x * MAP_TILES_TILE_SIZE;
    double abs_py = tile_y * MAP_TILES_TILE_SIZE;
    
    int top_left_tile_px_x = cached_tile_x * MAP_TILES_TILE_SIZE;
    int top_left_tile_px_y = cached_tile_y * MAP_TILES_TILE_SIZE;
    
    int marker_px_x = (int)(abs_px - top_left_tile_px_x);
    int marker_px_y = (int)(abs_py - top_left_tile_px_y);
    
    // Apply centering offset (64x64 icon, offset by half = 32px)
    const int icon_offset = 32;
    
    lv_obj_set_pos(marker->marker_obj, marker_px_x - icon_offset, marker_px_y - icon_offset);
    
    // Update label with time since last update
    if (marker->marker_label) {
        uint32_t current_time_ms = esp_timer_get_time() / 1000;
        uint32_t elapsed_sec = (current_time_ms - marker->last_update_ms) / 1000;
        
        char buf[96];
        if (elapsed_sec < 60) {
            snprintf(buf, sizeof(buf), "%s\n%.6f,%.6f\n%lus ago",
                     marker->device_name, marker->latitude, marker->longitude, elapsed_sec);
        } else {
            snprintf(buf, sizeof(buf), "%s\n%.6f,%.6f\n%lum ago",
                     marker->device_name, marker->latitude, marker->longitude, elapsed_sec / 60);
        }
        lv_label_set_text(marker->marker_label, buf);
    }
    
    // Update distance label
    update_distance_label();
}

bool UserMap::is_remote_marker_visible(RemoteMarker* marker) {
    if (!marker || !marker->marker_icon || !map_container) return false;
    
    lv_area_t marker_area;
    lv_area_t scroll_area;
    
    lv_obj_get_coords(marker->marker_icon, &marker_area);
    lv_obj_get_coords(map_container, &scroll_area);
    
    bool visible = 
        marker_area.x2 >= scroll_area.x1 &&
        marker_area.x1 <= scroll_area.x2 &&
        marker_area.y2 >= scroll_area.y1 &&
        marker_area.y1 <= scroll_area.y2;
    
    return visible;
}

void UserMap::update_remote_direction_arrows(RemoteMarker* marker) {
    if (!marker || !map_container || !marker->marker_obj) return;
    if (!marker->arrow_left || !marker->arrow_right || !marker->arrow_top || !marker->arrow_bottom) return;
    
    // Update arrow colors to match current device configuration
    const device_config_t* device_cfg = device_get_config((device_type_t)marker->device_type);
    lv_color_t arrow_color = lv_color_hex(device_cfg->color);
    
    lv_obj_set_style_border_color(marker->arrow_left, arrow_color, 0);
    lv_obj_set_style_border_color(marker->arrow_right, arrow_color, 0);
    lv_obj_set_style_border_color(marker->arrow_top, arrow_color, 0);
    lv_obj_set_style_border_color(marker->arrow_bottom, arrow_color, 0);
    
    lv_obj_t* left_lbl = lv_obj_get_child(marker->arrow_left, 0);
    lv_obj_t* right_lbl = lv_obj_get_child(marker->arrow_right, 0);
    lv_obj_t* top_lbl = lv_obj_get_child(marker->arrow_top, 0);
    lv_obj_t* bottom_lbl = lv_obj_get_child(marker->arrow_bottom, 0);
    
    if (left_lbl) lv_obj_set_style_text_color(left_lbl, arrow_color, 0);
    if (right_lbl) lv_obj_set_style_text_color(right_lbl, arrow_color, 0);
    if (top_lbl) lv_obj_set_style_text_color(top_lbl, arrow_color, 0);
    if (bottom_lbl) lv_obj_set_style_text_color(bottom_lbl, arrow_color, 0);
    
    // Hide all arrows by default
    lv_obj_add_flag(marker->arrow_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(marker->arrow_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(marker->arrow_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(marker->arrow_bottom, LV_OBJ_FLAG_HIDDEN);
    
    bool visible = is_remote_marker_visible(marker);
    
    if (visible) {
        return;
    }
    
    // Get screen bounds
    lv_area_t scroll_area;
    lv_obj_get_coords(map_container, &scroll_area);
    
    // Get marker position in screen coordinates
    lv_coord_t marker_x = lv_obj_get_x(marker->marker_obj);
    lv_coord_t marker_y = lv_obj_get_y(marker->marker_obj);
    lv_coord_t group_x = lv_obj_get_x(map_group);
    lv_coord_t group_y = lv_obj_get_y(map_group);
    lv_coord_t scroll_x = lv_obj_get_scroll_x(map_container);
    lv_coord_t scroll_y = lv_obj_get_scroll_y(map_container);
    
    int marker_screen_x = group_x + marker_x - scroll_x + scroll_area.x1;
    int marker_screen_y = group_y + marker_y - scroll_y + scroll_area.y1;
    
    // Show appropriate arrows based on marker position
    // Horizontal arrows (primary for 640x172 display)
    if (marker_screen_x < scroll_area.x1) {
        lv_obj_clear_flag(marker->arrow_left, LV_OBJ_FLAG_HIDDEN);
    } else if (marker_screen_x > scroll_area.x2) {
        lv_obj_clear_flag(marker->arrow_right, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Vertical arrows
    if (marker_screen_y < scroll_area.y1) {
        lv_obj_clear_flag(marker->arrow_top, LV_OBJ_FLAG_HIDDEN);
    } else if (marker_screen_y > scroll_area.y2) {
        lv_obj_clear_flag(marker->arrow_bottom, LV_OBJ_FLAG_HIDDEN);
    }
}

void UserMap::update_remote_marker(uint16_t device_id, uint8_t device_type, const char* device_name, double latitude, double longitude) {
    if (!initialized || !map_handle) return;
    
    RemoteMarker* marker = get_or_create_remote_marker(device_id);
    if (!marker) {
        return;
    }
    
    // Check if device type changed - need to update icon
    bool device_type_changed = (marker->marker_icon && marker->device_type != device_type);
    
    // Update device info
    marker->device_type = device_type;
    if (device_name) {
        strncpy(marker->device_name, device_name, sizeof(marker->device_name) - 1);
        marker->device_name[sizeof(marker->device_name) - 1] = '\0';
    }
    
    // Update position
    marker->latitude = latitude;
    marker->longitude = longitude;
    marker->last_update_ms = esp_timer_get_time() / 1000;
    
    // Create marker objects if they don't exist
    if (!marker->marker_obj) {
        create_remote_marker_objects(marker);
    } else if (device_type_changed) {
        const device_config_t* device_cfg = device_get_config((device_type_t)marker->device_type);
        if (marker->marker_icon) {
            lv_image_set_src(marker->marker_icon, device_cfg->icon);
        }
        // Update arrow colors
        if (marker->arrow_left) {
            lv_obj_set_style_border_color(marker->arrow_left, lv_color_hex(device_cfg->color), 0);
            lv_obj_t* left_lbl = lv_obj_get_child(marker->arrow_left, 0);
            if (left_lbl) lv_obj_set_style_text_color(left_lbl, lv_color_hex(device_cfg->color), 0);
        }
        if (marker->arrow_right) {
            lv_obj_set_style_border_color(marker->arrow_right, lv_color_hex(device_cfg->color), 0);
            lv_obj_t* right_lbl = lv_obj_get_child(marker->arrow_right, 0);
            if (right_lbl) lv_obj_set_style_text_color(right_lbl, lv_color_hex(device_cfg->color), 0);
        }
        if (marker->arrow_top) {
            lv_obj_set_style_border_color(marker->arrow_top, lv_color_hex(device_cfg->color), 0);
            lv_obj_t* top_lbl = lv_obj_get_child(marker->arrow_top, 0);
            if (top_lbl) lv_obj_set_style_text_color(top_lbl, lv_color_hex(device_cfg->color), 0);
        }
        if (marker->arrow_bottom) {
            lv_obj_set_style_border_color(marker->arrow_bottom, lv_color_hex(device_cfg->color), 0);
            lv_obj_t* bottom_lbl = lv_obj_get_child(marker->arrow_bottom, 0);
            if (bottom_lbl) lv_obj_set_style_text_color(bottom_lbl, lv_color_hex(device_cfg->color), 0);
        }
    }
    
    // Update position and direction arrows
    update_remote_marker_position(marker);
    update_remote_direction_arrows(marker);
}

void UserMap::remove_remote_marker(uint16_t device_id) {
    RemoteMarker* marker = find_remote_marker(device_id);
    if (!marker) return;
    
    // Delete LVGL objects
    if (marker->marker_obj) {
        lv_obj_delete(marker->marker_obj);
        marker->marker_obj = nullptr;
    }
    if (marker->arrow_left) {
        lv_obj_delete(marker->arrow_left);
        marker->arrow_left = nullptr;
    }
    if (marker->arrow_right) {
        lv_obj_delete(marker->arrow_right);
        marker->arrow_right = nullptr;
    }
    if (marker->arrow_top) {
        lv_obj_delete(marker->arrow_top);
        marker->arrow_top = nullptr;
    }
    if (marker->arrow_bottom) {
        lv_obj_delete(marker->arrow_bottom);
        marker->arrow_bottom = nullptr;
    }
    
    // Shift remaining markers down
    int index = marker - remote_markers;
    for (int i = index; i < remote_marker_count - 1; i++) {
        remote_markers[i] = remote_markers[i + 1];
    }
    remote_marker_count--;
}

void UserMap::update_all_remote_markers() {
    if (!initialized || !map_handle) return;
    
    for (int i = 0; i < remote_marker_count; i++) {
        update_remote_marker_position(&remote_markers[i]);
        update_remote_direction_arrows(&remote_markers[i]);
    }
}

void UserMap::create_distance_label() {
    if (!parent_scr) {
        return;
    }
    
    if (distance_label) {
        return;
    }
    
    // Create distance label for feet (large font)
    distance_label = lv_label_create(parent_scr);
    lv_label_set_text(distance_label, "-- ft");
    lv_obj_align(distance_label, LV_ALIGN_TOP_RIGHT, -10, 10);
    
    // Style the label with large font
    lv_obj_set_style_bg_color(distance_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(distance_label, LV_OPA_30, 0);  // 30% opacity (more transparent)
    lv_obj_set_style_text_color(distance_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(distance_label, LV_OPA_80, 0);  // 80% text opacity
    lv_obj_set_style_pad_all(distance_label, 12, 0);
    lv_obj_set_style_radius(distance_label, 8, 0);
    lv_obj_set_style_text_font(distance_label, &lv_font_montserrat_26, 0);  // Large font
    lv_obj_set_style_text_align(distance_label, LV_TEXT_ALIGN_RIGHT, 0);
    
    // Create km distance label (smaller font) below the ft label
    km_distance_label = lv_label_create(parent_scr);
    lv_label_set_text(km_distance_label, "(-- m)");
    lv_obj_align_to(km_distance_label, distance_label, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
    
    // Style the km label with smaller font
    lv_obj_set_style_bg_color(km_distance_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(km_distance_label, LV_OPA_30, 0);  // 30% opacity (more transparent)
    lv_obj_set_style_text_color(km_distance_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(km_distance_label, LV_OPA_80, 0);  // 80% text opacity
    lv_obj_set_style_pad_all(km_distance_label, 6, 0);
    lv_obj_set_style_radius(km_distance_label, 6, 0);
    lv_obj_set_style_text_font(km_distance_label, &lv_font_montserrat_16, 0);  // Smaller font
    lv_obj_set_style_text_align(km_distance_label, LV_TEXT_ALIGN_RIGHT, 0);
    
    // Realign km label after all styles are applied (styles can change size due to padding)
    lv_obj_align_to(km_distance_label, distance_label, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
    
    // Move both to top layer
    lv_obj_move_to_index(distance_label, -1);
    lv_obj_move_to_index(km_distance_label, -1);
    
    printf("UserMap: Distance labels created\n");
}

void UserMap::update_distance_label() {
    if (!distance_label || !km_distance_label) return;
    
    // Only show distance if we have at least one remote marker
    if (remote_marker_count == 0) {
        lv_label_set_text(distance_label, "-- ft");
        lv_label_set_text(km_distance_label, "(-- m)");
        return;
    }
    
    // Calculate distance to first remote marker using actual GPS position (not map center)
    RemoteMarker* marker = &remote_markers[0];
    double distance_km = calculate_distance(gps_lat, gps_lon, marker->latitude, marker->longitude);
    double distance_ft = distance_km * 3280.84;  // Convert km to feet
    
    char ft_buf[64];
    char metric_buf[64];
    
    if (distance_ft < 1000.0) {
        // Show in feet and meters
        double distance_m = distance_km * 1000.0;  // Convert to meters
        snprintf(ft_buf, sizeof(ft_buf), "%.0f ft", distance_ft);
        snprintf(metric_buf, sizeof(metric_buf), "(%.0f m)", distance_m);
    } else {
        // Show in miles and kilometers
        double distance_mi = distance_ft / 5280.0;  // Convert feet to miles
        snprintf(ft_buf, sizeof(ft_buf), "%.2f mi", distance_mi);
        snprintf(metric_buf, sizeof(metric_buf), "(%.2f km)", distance_km);
    }
    
    lv_label_set_text(distance_label, ft_buf);
    lv_label_set_text(km_distance_label, metric_buf);
    
    // Realign km label after text update
    lv_obj_align_to(km_distance_label, distance_label, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
}

double UserMap::calculate_distance(double lat1, double lon1, double lat2, double lon2) {
    // Haversine formula for calculating distance between two GPS coordinates
    const double R = 6371.0;  // Earth's radius in kilometers
    const double PI = 3.14159265358979323846;  // Use explicit PI constant
    
    // Convert degrees to radians
    double lat1_rad = lat1 * PI / 180.0;
    double lon1_rad = lon1 * PI / 180.0;
    double lat2_rad = lat2 * PI / 180.0;
    double lon2_rad = lon2 * PI / 180.0;
    
    // Differences
    double dlat = lat2_rad - lat1_rad;
    double dlon = lon2_rad - lon1_rad;
    
    // Haversine formula
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1_rad) * cos(lat2_rad) *
               sin(dlon / 2) * sin(dlon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    double distance = R * c;
    
    return distance;  // Returns distance in kilometers
}