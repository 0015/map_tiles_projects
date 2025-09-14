#include "simple_map.hpp"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

// Static member definitions
lv_obj_t* SimpleMap::map_container = nullptr;
lv_obj_t* SimpleMap::map_group = nullptr;
lv_obj_t** SimpleMap::tile_widgets = nullptr;
int SimpleMap::grid_cols = 0;
int SimpleMap::grid_rows = 0;
int SimpleMap::tile_count = 0;
lv_obj_t* SimpleMap::input_panel = nullptr;
lv_obj_t* SimpleMap::lat_textarea = nullptr;
lv_obj_t* SimpleMap::lon_textarea = nullptr;
lv_obj_t* SimpleMap::zoom_slider = nullptr;
lv_obj_t* SimpleMap::zoom_label = nullptr;
lv_obj_t* SimpleMap::update_button = nullptr;
lv_obj_t* SimpleMap::keyboard = nullptr;
lv_obj_t* SimpleMap::loading_popup = nullptr;
double SimpleMap::current_lat = 0.0;
double SimpleMap::current_lon = 0.0;
int SimpleMap::current_zoom = 15;
bool SimpleMap::initialized = false;
bool SimpleMap::is_loading = false;
uint32_t SimpleMap::last_scroll_time = 0;
uint32_t SimpleMap::last_gps_update_time = 0;
map_tiles_handle_t SimpleMap::map_handle = nullptr;

bool SimpleMap::init(lv_obj_t* parent_screen) {
    if (initialized) return true;
    
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
        printf("SimpleMap: Failed to initialize map tiles component\n");
        return false;
    }
    
    // Cache grid dimensions for performance
    map_tiles_get_grid_size(map_handle, &grid_cols, &grid_rows);
    tile_count = map_tiles_get_tile_count(map_handle);
    
    printf("SimpleMap: Initialized with grid size %dx%d (%d tiles)\n", grid_cols, grid_rows, tile_count);

    lv_display_t *disp = lv_display_get_default();    
    lv_coord_t _width = lv_display_get_horizontal_resolution(disp);
    lv_coord_t _height = lv_display_get_vertical_resolution(disp);
    
    // Create scrollable map container (like map_scroll in map_display.cpp)
    map_container = lv_obj_create(parent_screen);
    lv_obj_set_size(map_container, _width, _height);  // Match device resolution
    lv_obj_center(map_container);
    lv_obj_clear_flag(map_container, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(map_container, LV_DIR_ALL);
    lv_obj_add_flag(map_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(map_container, 0, 0);
    lv_obj_add_event_cb(map_container, map_scroll_event_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(map_container, map_scroll_event_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_set_style_bg_color(map_container, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(map_container, LV_SCROLLBAR_MODE_OFF);
    
    // Create tile widgets
    create_tile_widgets();
    
    // Create input panel
    create_input_panel(parent_screen);
    
    initialized = true;
    return true;
}

void SimpleMap::create_tile_widgets() {
    // Use cached grid dimensions for better performance
    if (grid_cols <= 0 || grid_rows <= 0 || tile_count <= 0) {
        printf("SimpleMap: Invalid grid dimensions cached\n");
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

void SimpleMap::show_location(double latitude, double longitude, int zoom_level) {
    if (!initialized || !map_handle) return;
    
    // Update current position and zoom
    current_lat = latitude;
    current_lon = longitude;
    current_zoom = zoom_level;
    
    // Set zoom level in the component
    map_tiles_set_zoom(map_handle, zoom_level);
    
    // Update input panel widgets
    if (lat_textarea) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", latitude);
        lv_textarea_set_text(lat_textarea, buf);
    }
    if (lon_textarea) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", longitude);
        lv_textarea_set_text(lon_textarea, buf);
    }
    if (zoom_slider) {
        lv_slider_set_value(zoom_slider, zoom_level, LV_ANIM_OFF);
    }
    
    // Update zoom label
    update_zoom_label();
    
    // Set the tile center based on GPS coordinates
    map_tiles_set_center_from_gps(map_handle, latitude, longitude);
    
    load_map_tiles();
}

void SimpleMap::update_location(double latitude, double longitude) {
    show_location(latitude, longitude, current_zoom);
}

void SimpleMap::set_satellite_view(bool satellite) {
    if (!initialized || !map_handle) return;
    
    // Convert boolean to tile type (0 = regular, 1 = satellite)
    int tile_type = satellite ? 1 : 0;
    
    // Set tile type in the component
    if (map_tiles_set_tile_type(map_handle, tile_type)) {
        // Reload tiles with the new tile type
        load_map_tiles();
    }
}

bool SimpleMap::set_tile_type(int tile_type) {
    if (!initialized || !map_handle) return false;
    
    // Set tile type in the component
    if (map_tiles_set_tile_type(map_handle, tile_type)) {
        // Reload tiles with the new tile type
        load_map_tiles();
        return true;
    }
    return false;
}

int SimpleMap::get_tile_type() {
    if (!initialized || !map_handle) return -1;
    return map_tiles_get_tile_type(map_handle);
}

int SimpleMap::get_tile_type_count() {
    if (!initialized || !map_handle) return 0;
    return map_tiles_get_tile_type_count(map_handle);
}

const char* SimpleMap::get_tile_type_folder(int tile_type) {
    if (!initialized || !map_handle) return nullptr;
    return map_tiles_get_tile_type_folder(map_handle, tile_type);
}

void SimpleMap::load_map_tiles() {
    if (!initialized || !map_handle) return;
    
    // Check if already loading to prevent multiple simultaneous loads
    if (is_loading) {
        printf("SimpleMap: Already loading, skipping...\n");
        return;
    }
    
    is_loading = true;  // Set loading flag
    printf("SimpleMap: Starting tile loading...\n");
    
    // Show loading popup
    show_loading_popup();
    
    lv_timer_create([](lv_timer_t *t) {
        lv_timer_del(t);
        uint32_t start_time = esp_timer_get_time() / 1000;
        
        // Get current tile position (this is already the top-left tile position)
        int tx, ty;
        map_tiles_get_position(map_handle, &tx, &ty);
        
        printf("SimpleMap: Loading %dx%d grid at position (%d,%d) at zoom %d\n", 
               grid_cols, grid_rows, tx, ty, map_tiles_get_zoom(map_handle));
        
        // Don't clear the old tiles - keep them visible while new ones load
        // This provides a better user experience by avoiding blank screens
        
        // Add a subtle visual indication that loading is in progress
        // by adding a thin border to tiles that are about to be updated
        for (int i = 0; i < tile_count; i++) {
            if (tile_widgets[i]) {
                lv_obj_set_style_border_width(tile_widgets[i], 1, 0);
                lv_obj_set_style_border_color(tile_widgets[i], lv_color_make(100, 100, 255), 0);
                lv_obj_set_style_border_opa(tile_widgets[i], LV_OPA_30, 0);
            }
        }
        
        // Use cached grid dimensions for better performance
        // Load grid of tiles starting from the top-left position
        for (int row = 0; row < grid_rows; row++) {
            for (int col = 0; col < grid_cols; col++) {
                int index = row * grid_cols + col;
                int tile_x = tx + col;  // Start from top-left tile position
                int tile_y = ty + row;
                
                // Load tile from SD card
                bool loaded = map_tiles_load_tile(map_handle, index, tile_x, tile_y);
                
                if (loaded) {
                    // Get the tile image data and set it to the widget
                    lv_image_dsc_t* tile_img = map_tiles_get_image(map_handle, index);
                    if (tile_img && tile_img->data) {
                        lv_image_set_src(tile_widgets[index], (const void *)tile_img);
                        // Clear background color and loading indication since we have valid image data
                        lv_obj_set_style_bg_opa(tile_widgets[index], LV_OPA_TRANSP, 0);
                        lv_obj_set_style_border_width(tile_widgets[index], 0, 0);
                    } else {
                        // Keep old image if new tile data is invalid, but show a subtle indication
                        // Only show placeholder if there was no previous image
                        const void* current_src = lv_image_get_src(tile_widgets[index]);
                        if (!current_src) {
                            lv_obj_set_style_bg_color(tile_widgets[index], lv_color_make(150, 150, 150), 0);
                            lv_obj_set_style_bg_opa(tile_widgets[index], LV_OPA_COVER, 0);
                        }
                        lv_obj_set_style_border_width(tile_widgets[index], 0, 0);
                    }
                } else {
                    // Keep old image if tile failed to load, but show a subtle indication
                    // Only show error placeholder if there was no previous image
                    const void* current_src = lv_image_get_src(tile_widgets[index]);
                    if (!current_src) {
                        lv_obj_set_style_bg_color(tile_widgets[index], lv_color_make(255, 200, 200), 0);
                        lv_obj_set_style_bg_opa(tile_widgets[index], LV_OPA_50, 0);  // Semi-transparent error indicator
                    }
                    lv_obj_set_style_border_width(tile_widgets[index], 0, 0);
                }
            }
        }
        
        uint32_t end_time = esp_timer_get_time() / 1000;
        printf("SimpleMap: Tile loading completed in %lu ms\n", end_time - start_time);
        
        // Invalidate the entire map group once for better performance
        if (map_group) {
            lv_obj_invalidate(map_group);
        }
        
        // Hide loading popup after tiles are loaded
        hide_loading_popup();
        
        // Clear loading flag
        is_loading = false;
    }, 50, NULL);
    
    
}

void SimpleMap::map_scroll_event_cb(lv_event_t *e)
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
            printf("SimpleMap: at_left\n");
            tile_x -= 1;
            needUpdate = true;
        }
        else if (lv_obj_get_scroll_top(map_container) < -90)
        {
            printf("SimpleMap: at_top\n");
            tile_y -= 1;
            needUpdate = true;
        }
        else if (lv_obj_get_scroll_right(map_container) < -90)
        {
            printf("SimpleMap: at_right\n");
            tile_x += 1;
            needUpdate = true;
        }
        else if (lv_obj_get_scroll_bottom(map_container) < -90)
        {
            printf("SimpleMap: at_bottom\n");
            tile_y += 1;
            needUpdate = true;
        }

        if (needUpdate) {
            map_tiles_set_position(map_handle, tile_x, tile_y);
            load_map_tiles();
        }
        
        // Update GPS coordinates from current center (whether tiles updated or not)
        update_current_gps_from_map_center();
    }
}

void SimpleMap::create_input_panel(lv_obj_t* parent_screen)
{
    // Create input panel container - increased height for button
    input_panel = lv_obj_create(parent_screen);
    lv_obj_set_size(input_panel, 220, 180);
    lv_obj_align(input_panel, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_bg_color(input_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(input_panel, LV_OPA_30, 0);
    lv_obj_set_style_border_width(input_panel, 1, 0);
    lv_obj_set_style_border_color(input_panel, lv_color_white(), 0);
    lv_obj_set_style_radius(input_panel, 5, 0);
    lv_obj_set_style_pad_all(input_panel, 8, 0);

    // Create latitude label and textarea
    lv_obj_t* lat_label = lv_label_create(input_panel);
    lv_label_set_text(lat_label, "Latitude:");
    lv_obj_set_style_text_color(lat_label, lv_color_white(), 0);
    lv_obj_align(lat_label, LV_ALIGN_TOP_LEFT, 0, 10);

    lat_textarea = lv_textarea_create(input_panel);
    lv_obj_set_size(lat_textarea, 110, 25);
    lv_obj_align(lat_textarea, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_textarea_set_one_line(lat_textarea, true);
    lv_textarea_set_text(lat_textarea, "0.000000");
    lv_obj_add_event_cb(lat_textarea, textarea_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(lat_textarea, textarea_event_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(lat_textarea, textarea_event_cb, LV_EVENT_READY, NULL);

    // Create longitude label and textarea
    lv_obj_t* lon_label = lv_label_create(input_panel);
    lv_label_set_text(lon_label, "Longitude:");
    lv_obj_set_style_text_color(lon_label, lv_color_white(), 0);
    lv_obj_align_to(lon_label, lat_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 30);

    lon_textarea = lv_textarea_create(input_panel);
    lv_obj_set_size(lon_textarea, 110, 25);
    lv_obj_align_to(lon_textarea, lat_textarea, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_textarea_set_one_line(lon_textarea, true);
    lv_textarea_set_text(lon_textarea, "0.000000");
    lv_obj_add_event_cb(lon_textarea, textarea_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(lon_textarea, textarea_event_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(lon_textarea, textarea_event_cb, LV_EVENT_READY, NULL);

    // Create zoom label and slider
    zoom_label = lv_label_create(input_panel);
    lv_label_set_text_fmt(zoom_label, "Zoom: (%d)", current_zoom);
    lv_obj_set_style_text_color(zoom_label, lv_color_white(), 0);
    lv_obj_align_to(zoom_label, lon_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);

    zoom_slider = lv_slider_create(input_panel);
    lv_obj_set_size(zoom_slider, 100, 10);
    lv_obj_align_to(zoom_slider, lon_textarea, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_slider_set_range(zoom_slider, 10, 19);
    lv_slider_set_value(zoom_slider, current_zoom, LV_ANIM_OFF);
    lv_obj_add_event_cb(zoom_slider, zoom_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Create update button
    update_button = lv_btn_create(input_panel);
    lv_obj_set_size(update_button, 150, 30);
    lv_obj_align(update_button, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(update_button, update_button_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Button styling
    lv_obj_set_style_bg_color(update_button, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(update_button, LV_OPA_COVER, 0);
    
    // Button label
    lv_obj_t* btn_label = lv_label_create(update_button);
    lv_label_set_text(btn_label, "Update Map");
    lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
    lv_obj_center(btn_label);

    // Create keyboard (initially hidden)
    keyboard = lv_keyboard_create(lv_screen_active());
    lv_obj_set_size(keyboard, LV_HOR_RES, LV_VER_RES / 2);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);  // Set to number layout
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);  // Start hidden
}

void SimpleMap::zoom_slider_event_cb(lv_event_t *e)
{
    lv_obj_t* slider = (lv_obj_t*) lv_event_get_target(e);
    int selected_zoom = lv_slider_get_value(slider);
    
    // Update zoom label to show the selected value (not yet applied)
    if (zoom_label) {
        int component_zoom = map_tiles_get_zoom(map_handle);
        if (selected_zoom != component_zoom) {
            // Show that this is a selected but not yet applied zoom level
            lv_label_set_text_fmt(zoom_label, "Zoom: (%d*)", selected_zoom);
        } else {
            // Show that this zoom level is currently applied
            lv_label_set_text_fmt(zoom_label, "Zoom: (%d)", selected_zoom);
        }
    }
    
    printf("SimpleMap: Zoom slider changed to %d (use Update button to apply)\n", selected_zoom);
}

void SimpleMap::update_zoom_label()
{
    if (zoom_label) {
        // Show the actual applied zoom level (no asterisk means it's applied)
        lv_label_set_text_fmt(zoom_label, "Zoom: (%d)", current_zoom);
    }
}

void SimpleMap::textarea_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* textarea = (lv_obj_t*)lv_event_get_target(e);
    
    if (code == LV_EVENT_FOCUSED) {
        // Show keyboard when textarea is focused
        if (keyboard) {
            lv_keyboard_set_textarea(keyboard, textarea);
            lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(keyboard);
            printf("SimpleMap: Keyboard shown for textarea\n");
        }
    }
    else if (code == LV_EVENT_DEFOCUSED) {
        // Hide keyboard when textarea loses focus
        if (keyboard) {
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            printf("SimpleMap: Keyboard hidden\n");
        }
        
        // Validate and update coordinates when focus is lost
        const char* text = lv_textarea_get_text(textarea);
        
        if (textarea == lat_textarea) {
            double lat_val = atof(text);
            if (lat_val >= -90.0 && lat_val <= 90.0) {
                current_lat = lat_val;
                printf("SimpleMap: Latitude updated to %f\n", lat_val);
            }
        } else if (textarea == lon_textarea) {
            double lon_val = atof(text);
            if (lon_val >= -180.0 && lon_val <= 180.0) {
                current_lon = lon_val;
                printf("SimpleMap: Longitude updated to %f\n", lon_val);
            }
        }
    }
    else if (code == LV_EVENT_READY) {
        // Handle keyboard ready event (when user presses Enter or Done)
        if (keyboard) {
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            printf("SimpleMap: Keyboard hidden via READY event\n");
        }
        
        // Validate and update coordinates
        const char* text = lv_textarea_get_text(textarea);
        
        if (textarea == lat_textarea) {
            double lat_val = atof(text);
            if (lat_val >= -90.0 && lat_val <= 90.0) {
                current_lat = lat_val;
                printf("SimpleMap: Latitude ready: %f\n", lat_val);
            }
        } else if (textarea == lon_textarea) {
            double lon_val = atof(text);
            if (lon_val >= -180.0 && lon_val <= 180.0) {
                current_lon = lon_val;
                printf("SimpleMap: Longitude ready: %f\n", lon_val);
            }
        }
    }
}

void SimpleMap::update_button_event_cb(lv_event_t *e)
{
    printf("SimpleMap: Update button clicked\n");
    
    // Get values from input fields
    double new_lat = current_lat;
    double new_lon = current_lon;
    int new_zoom = current_zoom;
    bool valid_input = true;
    bool zoom_changed = false;
    
    // Get latitude from textarea
    if (lat_textarea) {
        const char* lat_text = lv_textarea_get_text(lat_textarea);
        double lat_val = atof(lat_text);
        if (lat_val >= -90.0 && lat_val <= 90.0) {
            new_lat = lat_val;
        } else {
            printf("SimpleMap: Invalid latitude: %s\n", lat_text);
            valid_input = false;
        }
    }
    
    // Get longitude from textarea
    if (lon_textarea) {
        const char* lon_text = lv_textarea_get_text(lon_textarea);
        double lon_val = atof(lon_text);
        if (lon_val >= -180.0 && lon_val <= 180.0) {
            new_lon = lon_val;
        } else {
            printf("SimpleMap: Invalid longitude: %s\n", lon_text);
            valid_input = false;
        }
    }
    
    // Get zoom from slider
    if (zoom_slider) {
        int slider_zoom = lv_slider_get_value(zoom_slider);
        // Check if zoom changed by comparing with the actual zoom level in the map tiles component
        int component_zoom = map_tiles_get_zoom(map_handle);
        if (slider_zoom != component_zoom) {
            zoom_changed = true;
        }
        new_zoom = slider_zoom;
    }
    
    // Update map if all inputs are valid
    if (valid_input) {
        printf("SimpleMap: Updating map to lat=%.6f, lon=%.6f, zoom=%d\n", new_lat, new_lon, new_zoom);
        
        // Update coordinates first
        current_lat = new_lat;
        current_lon = new_lon;
        
        // Update input panel to show the new coordinates
        if (lat_textarea) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6f", current_lat);
            lv_textarea_set_text(lat_textarea, buf);
        }
        if (lon_textarea) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6f", current_lon);
            lv_textarea_set_text(lon_textarea, buf);
        }
        
        if (zoom_changed) {
            // Change zoom level and reload tiles
            change_zoom_level(new_zoom);
        } else {
            // Just update the map position without changing zoom
            map_tiles_set_center_from_gps(map_handle, new_lat, new_lon);
            
            // Check if we need to load new tiles for the new position
            if (!map_tiles_is_gps_within_tiles(map_handle, new_lat, new_lon)) {
                load_map_tiles();
            }
            
            center_map_on_gps();
        }
    } else {
        printf("SimpleMap: Invalid input - map not updated\n");
    }
}

void SimpleMap::show_loading_popup()
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

void SimpleMap::hide_loading_popup()
{
    if (loading_popup) {
        lv_obj_delete(loading_popup);
        loading_popup = nullptr;
    }
}

void SimpleMap::center_map_on_gps()
{
    if (!initialized || !map_handle || !map_group || !map_container) {
        printf("SimpleMap: Map not properly initialized for centering\n");
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
    
    printf("SimpleMap: Centered map on GPS %.6f, %.6f (scroll: %d, %d)\n", 
           current_lat, current_lon, center_scroll_x, center_scroll_y);
}

void SimpleMap::update_current_gps_from_map_center()
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
    
    // Update input panel to show the new coordinates
    if (lat_textarea) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", current_lat);
        lv_textarea_set_text(lat_textarea, buf);
    }
    if (lon_textarea) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", current_lon);
        lv_textarea_set_text(lon_textarea, buf);
    }
    
    // GPS coordinates updated silently for better performance
}

void SimpleMap::change_zoom_level(int new_zoom) {
    if (!initialized || !map_handle) {
        printf("SimpleMap: Map not initialized, cannot change zoom\n");
        return;
    }
    
    if (new_zoom == current_zoom) {
        printf("SimpleMap: Zoom level already set to %d\n", new_zoom);
        return;
    }
    
    printf("SimpleMap: Changing zoom from %d to %d\n", current_zoom, new_zoom);
    
    // Set zoom level in the component first
    map_tiles_set_zoom(map_handle, new_zoom);
    
    // Update current zoom after setting it in the component
    current_zoom = new_zoom;
    
    // Update zoom slider to reflect the change
    if (zoom_slider) {
        lv_slider_set_value(zoom_slider, new_zoom, LV_ANIM_OFF);
    }
    
    // Update zoom label
    update_zoom_label();
    
    // Recalculate tile positions for the new zoom level centered on current GPS coordinates
    map_tiles_set_center_from_gps(map_handle, current_lat, current_lon);
    
    // Force reload all tiles for the new zoom level
    load_map_tiles();
    
    // Center the map view after tiles are loaded
    lv_timer_create([](lv_timer_t *t) {
        lv_timer_del(t);
        center_map_on_gps();
        printf("SimpleMap: Zoom change completed, map centered\n");
    }, 200, NULL);
}

void SimpleMap::get_current_location(double* latitude, double* longitude) {
    if (latitude) *latitude = current_lat;
    if (longitude) *longitude = current_lon;
}

int SimpleMap::get_current_zoom() {
    return current_zoom;
}

void SimpleMap::cleanup() {
    if (map_container) {
        lv_obj_delete(map_container);
        map_container = nullptr;
    }
    
    if (input_panel) {
        lv_obj_delete(input_panel);
        input_panel = nullptr;
    }
    
    if (keyboard) {
        lv_obj_delete(keyboard);
        keyboard = nullptr;
    }
    
    if (loading_popup) {
        lv_obj_delete(loading_popup);
        loading_popup = nullptr;
    }
    
    map_group = nullptr;  // Will be cleaned up when map_container is deleted
    lat_textarea = nullptr;
    lon_textarea = nullptr;
    zoom_slider = nullptr;
    zoom_label = nullptr;
    update_button = nullptr;
    
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
    
    initialized = false;
}