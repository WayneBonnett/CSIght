#include "csight.h"
#include <storage/storage.h>

#define TAG         "CSIght"
#define CONFIG_PATH EXT_PATH("apps_data/csight/config.bin")

// ─── Config struct (saved to SD) ─────────────────────────────────────────────
typedef struct {
    uint8_t preset_idx;
    uint8_t tx_pin;
    uint8_t rx_pin;
    uint8_t sensitivity;
    uint8_t display_mode;
    uint8_t _pad[3];
} CSIghtConfig;

// ─── Config I/O ──────────────────────────────────────────────────────────────
void csight_config_save(CSIghtApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("apps_data/csight"));

    File* file = storage_file_alloc(storage);
    if (storage_file_open(file, CONFIG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        CSIghtConfig cfg = {
            .preset_idx   = app->preset_idx,
            .tx_pin       = app->tx_pin,
            .rx_pin       = app->rx_pin,
            .sensitivity  = app->sensitivity,
            .display_mode = (uint8_t)app->display_mode,
        };
        storage_file_write(file, &cfg, sizeof(cfg));
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "Config saved");
}

void csight_config_load(CSIghtApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if (storage_file_open(file, CONFIG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        CSIghtConfig cfg;
        if (storage_file_read(file, &cfg, sizeof(cfg)) == sizeof(cfg)) {
            app->preset_idx   = cfg.preset_idx < BOARD_PRESET_COUNT ? cfg.preset_idx : 0;
            app->tx_pin       = cfg.tx_pin  ? cfg.tx_pin  : 13;
            app->rx_pin       = cfg.rx_pin  ? cfg.rx_pin  : 14;
            app->sensitivity  = cfg.sensitivity <= 10 ? cfg.sensitivity : 5;
            app->display_mode = cfg.display_mode <= DisplayModeProximity ?
                                (DisplayMode)cfg.display_mode : DisplayModeRadar;
            FURI_LOG_I(TAG, "Config loaded: preset=%d tx=%d rx=%d",
                       app->preset_idx, app->tx_pin, app->rx_pin);
        }
    } else {
        // Defaults
        app->preset_idx  = 0;
        app->tx_pin      = 13;
        app->rx_pin      = 14;
        app->sensitivity = 5;
        app->display_mode = DisplayModeRadar;
        FURI_LOG_I(TAG, "No config found, using defaults");
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

// ─── Blip logic ───────────────────────────────────────────────────────────────
void csight_add_blip(CSIghtApp* app, uint8_t intensity, uint8_t proximity) {
    // Find oldest slot
    int oldest = 0;
    for (int i = 1; i < MAX_BLIPS; i++) {
        if (app->blips[i].age < app->blips[oldest].age) oldest = i;
    }

    // Place blip near current sweep angle, distance based on proximity
    // proximity 0-100: 0=far edge, 100=center
    uint8_t a = app->sweep_angle & 63;
    // Extern trig lookup — use sweep angle with small random jitter
    // (jitter = low bits of intensity to vary blip placement slightly)
    uint8_t ja = (a + (intensity & 3)) & 63;

    // distance from center: proximity 100 → r=2, proximity 0 → r=RADAR_R-2
    int dist = 2 + ((100 - proximity) * (RADAR_R - 4)) / 100;

    extern const int8_t SIN64[64];
    extern const int8_t COS64[64];
    app->blips[oldest].x         = (int8_t)((COS64[ja] * dist) / 59);
    app->blips[oldest].y         = (int8_t)((SIN64[ja] * dist) / 59);
    app->blips[oldest].age       = 255;
    app->blips[oldest].intensity = intensity;
}

// ─── Tick — called from timer, ages blips + advances sweep ───────────────────
void csight_tick(CSIghtApp* app) {
    // Advance sweep
    app->sweep_angle = (app->sweep_angle + 1) & 63;

    // Age blips
    for (int i = 0; i < MAX_BLIPS; i++) {
        if (app->blips[i].age > 0) {
            app->blips[i].age -= 2;
        }
    }

    // Boot animation frame
    if (app->state == AppStateBooting) {
        app->boot_frame++;
        if (app->boot_frame > 40) {
            // Send handshake after boot animation
            csight_send_handshake(app);
            app->state = AppStateConnecting;
            app->boot_frame = 0;
        }
    }

    // Connecting timeout — if no handshake after ~3s, go to preset select
    if (app->state == AppStateConnecting) {
        app->boot_frame++;
        if (app->boot_frame > 90) {
            app->state = AppStatePresetSelect;
            app->boot_frame = 0;
        }
    }
}

// ─── Input handling ───────────────────────────────────────────────────────────
static void handle_input(CSIghtApp* app, InputKey key, InputType type) {
    if (type != InputTypeShort && type != InputTypeLong) return;

    switch (app->state) {

        case AppStateCompatCheck:
            if (key == InputKeyOk) {
                if (app->csi_support > 0) {
                    app->state = AppStatePresetSelect;
                }
            }
            break;

        case AppStatePresetSelect:
            if (key == InputKeyUp && app->preset_idx > 0) {
                app->preset_idx--;
            } else if (key == InputKeyDown && app->preset_idx < BOARD_PRESET_COUNT - 1) {
                app->preset_idx++;
            } else if (key == InputKeyOk) {
                if (app->preset_idx != BOARD_PRESET_CUSTOM) {
                    app->tx_pin = BOARD_PRESETS[app->preset_idx].tx_pin;
                    app->rx_pin = BOARD_PRESETS[app->preset_idx].rx_pin;
                    csight_config_save(app);
                    app->state = AppStateScanning;
                    csight_send_handshake(app);
                } else {
                    app->state = AppStatePinConfig;
                }
            }
            break;

        case AppStatePinConfig:
            // Simple: up/down adjusts TX pin, OK saves + starts
            if (key == InputKeyUp   && app->tx_pin < 39) app->tx_pin++;
            if (key == InputKeyDown && app->tx_pin > 0)  app->tx_pin--;
            if (key == InputKeyOk) {
                csight_config_save(app);
                app->state = AppStateScanning;
                csight_send_handshake(app);
            }
            break;

        case AppStateScanning:
            // Left/Right = switch display mode
            if (key == InputKeyLeft) {
                app->display_mode = (app->display_mode + 2) % 3;
                csight_send_mode(app);
            } else if (key == InputKeyRight) {
                app->display_mode = (app->display_mode + 1) % 3;
                csight_send_mode(app);
            }
            // Up/Down = sensitivity
            if (key == InputKeyUp   && app->sensitivity < 10) {
                app->sensitivity++;
                csight_send_sensitivity(app);
            }
            if (key == InputKeyDown && app->sensitivity > 0) {
                app->sensitivity--;
                csight_send_sensitivity(app);
            }
            break;

        default:
            break;
    }
}

// ─── Draw callback ────────────────────────────────────────────────────────────
static void draw_cb(Canvas* c, void* ctx) {
    CSIghtApp* app = (CSIghtApp*)ctx;

    switch (app->state) {
        case AppStateBooting:
        case AppStateConnecting:
            csight_draw_boot(c, app);
            break;
        case AppStateCompatCheck:
            csight_draw_compat(c, app);
            break;
        case AppStatePresetSelect:
        case AppStatePinConfig:
            csight_draw_preset(c, app);
            break;
        case AppStateScanning:
            switch (app->display_mode) {
                case DisplayModeRadar:     csight_draw_radar(c, app);     break;
                case DisplayModeWaterfall: csight_draw_waterfall(c, app); break;
                case DisplayModeProximity: csight_draw_proximity(c, app); break;
            }
            break;
    }
}

// ─── Input callback ───────────────────────────────────────────────────────────
static void input_cb(InputEvent* event, void* ctx) {
    CSIghtApp* app = (CSIghtApp*)ctx;
    furi_message_queue_put(app->event_queue, event, 0);
}

// ─── Timer callback ───────────────────────────────────────────────────────────
static void timer_cb(void* ctx) {
    CSIghtApp* app = (CSIghtApp*)ctx;
    csight_tick(app);
    view_port_update(app->view_port);
}

// ─── Alloc / Free ─────────────────────────────────────────────────────────────
CSIghtApp* csight_app_alloc(void) {
    CSIghtApp* app = malloc(sizeof(CSIghtApp));
    memset(app, 0, sizeof(CSIghtApp));

    app->state        = AppStateBooting;
    app->sensitivity  = 5;
    app->display_mode = DisplayModeRadar;
    app->event_queue  = furi_message_queue_alloc(8, sizeof(InputEvent));

    csight_config_load(app);

    app->gui       = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_cb, app);
    view_port_input_callback_set(app->view_port, input_cb, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    csight_uart_init(app);

    return app;
}

void csight_app_free(CSIghtApp* app) {
    csight_uart_deinit(app);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->event_queue);
    free(app);
}

// ─── Entry point ─────────────────────────────────────────────────────────────
int32_t csight_app(void* p) {
    UNUSED(p);
    CSIghtApp* app = csight_app_alloc();

    // 30fps tick timer
    FuriTimer* timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(timer, furi_kernel_get_tick_frequency() / 30);

    // Main loop
    InputEvent event;
    while (1) {
        if (furi_message_queue_get(app->event_queue, &event, 10) == FuriStatusOk) {
            if (event.key == InputKeyBack && event.type == InputTypeShort) {
                break; // exit app
            }
            handle_input(app, event.key, event.type);
        }
    }

    furi_timer_stop(timer);
    furi_timer_free(timer);
    csight_app_free(app);
    return 0;
}
