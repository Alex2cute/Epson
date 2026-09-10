#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/canvas.h>
#include <input/input.h>
#include <storage/storage.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/infrared/signal/infrared_signal.h>

#define EPSON_DIR EXT_PATH("infrared/Epson")
#define MAX_NAME 128
#define MAX_PATH 256
#define QUEUE_SIZE 8
#define GAP_MS 180
#define MAX_UNIQUE 32

typedef enum {
    AppStateReady,
    AppStateSending,
    AppStateDone,
    AppStateError,
} AppState;

typedef struct {
    bool raw;
    union {
        InfraredMessage parsed;
        struct {
            uint32_t frequency;
            float duty_cycle;
            size_t timings_size;
            uint32_t* timings;
        } raw_signal;
    };
} UniqueSignal;

typedef struct {
    FuriMessageQueue* queue;
    ViewPort* view_port;
    Gui* gui;
    Storage* storage;
    AppState state;
    uint32_t sent;
    uint32_t unique_found;
    UniqueSignal unique[MAX_UNIQUE];
} EpsonApp;

static void draw_callback(Canvas* canvas, void* context) {
    EpsonApp* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignCenter, "EPSON UNIVERSAL");
    canvas_set_font(canvas, FontSecondary);

    if(app->state == AppStateReady) {
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "OK: SEND POWER");
        canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignCenter, "BACK: EXIT");
    } else if(app->state == AppStateSending) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Sent: %lu", (unsigned long)app->sent);
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, buf);
        canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignCenter, "PLEASE WAIT");
    } else if(app->state == AppStateDone) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lu UNIQUE CODES", (unsigned long)app->sent);
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "DONE");
        canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignCenter, buf);
    } else {
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "NO POWER SIGNALS");
        canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignCenter, "BACK: EXIT");
    }
}

static void input_callback(InputEvent* event, void* context) {
    EpsonApp* app = context;
    if(event->type == InputTypeShort) {
        furi_message_queue_put(app->queue, event, 0);
    }
}

static bool is_ir_file(const char* name) {
    size_t len = strlen(name);
    return len >= 3 && strcmp(name + len - 3, ".ir") == 0;
}

static bool same_signal(const UniqueSignal* a, const InfraredSignal* b) {
    if(a->raw != infrared_signal_is_raw(b)) return false;

    if(a->raw) {
        const InfraredRawSignal* raw = infrared_signal_get_raw_signal(b);
        if(a->raw_signal.frequency != raw->frequency) return false;
        if(a->raw_signal.duty_cycle != raw->duty_cycle) return false;
        if(a->raw_signal.timings_size != raw->timings_size) return false;
        return memcmp(
                   a->raw_signal.timings,
                   raw->timings,
                   raw->timings_size * sizeof(uint32_t)) == 0;
    } else {
        const InfraredMessage* msg = infrared_signal_get_message(b);
        return a->parsed.protocol == msg->protocol &&
               a->parsed.address == msg->address &&
               a->parsed.command == msg->command &&
               a->parsed.repeat == msg->repeat;
    }
}

static bool remember_unique(EpsonApp* app, const InfraredSignal* signal) {
    for(uint32_t i = 0; i < app->unique_found; i++) {
        if(same_signal(&app->unique[i], signal)) return false;
    }

    if(app->unique_found >= MAX_UNIQUE) return false;

    UniqueSignal* dst = &app->unique[app->unique_found];

    if(infrared_signal_is_raw(signal)) {
        const InfraredRawSignal* raw = infrared_signal_get_raw_signal(signal);
        dst->raw = true;
        dst->raw_signal.frequency = raw->frequency;
        dst->raw_signal.duty_cycle = raw->duty_cycle;
        dst->raw_signal.timings_size = raw->timings_size;
        dst->raw_signal.timings = malloc(raw->timings_size * sizeof(uint32_t));
        memcpy(
            dst->raw_signal.timings,
            raw->timings,
            raw->timings_size * sizeof(uint32_t));
    } else {
        dst->raw = false;
        dst->parsed = *infrared_signal_get_message(signal);
    }

    app->unique_found++;
    return true;
}

static void free_unique(EpsonApp* app) {
    for(uint32_t i = 0; i < app->unique_found; i++) {
        if(app->unique[i].raw) {
            free(app->unique[i].raw_signal.timings);
        }
    }
    app->unique_found = 0;
}

static void transmit_unique(EpsonApp* app) {
    InfraredSignal* signal = infrared_signal_alloc();

    for(uint32_t i = 0; i < app->unique_found; i++) {
        UniqueSignal* u = &app->unique[i];

        if(u->raw) {
            infrared_signal_set_raw_signal(
                signal,
                u->raw_signal.timings,
                u->raw_signal.timings_size,
                u->raw_signal.frequency,
                u->raw_signal.duty_cycle);
        } else {
            infrared_signal_set_message(signal, &u->parsed);
        }

        infrared_signal_transmit(signal);
        app->sent++;
        view_port_update(app->view_port);
        furi_delay_ms(GAP_MS);
    }

    infrared_signal_free(signal);
}

static void collect_power_signals(EpsonApp* app) {
    File* dir = storage_file_alloc(app->storage);
    FileInfo info;
    char filename[MAX_NAME];

    free_unique(app);
    app->sent = 0;

    if(!storage_dir_open(dir, EPSON_DIR)) {
        app->state = AppStateError;
        view_port_update(app->view_port);
        storage_file_free(dir);
        return;
    }

    while(storage_dir_read(dir, &info, filename, sizeof(filename))) {
        if(info.flags & FSF_DIRECTORY) continue;
        if(!is_ir_file(filename)) continue;

        char path[MAX_PATH];
        int n = snprintf(path, sizeof(path), "%s/%s", EPSON_DIR, filename);
        if(n < 0 || n >= (int)sizeof(path)) continue;

        FlipperFormat* ff = flipper_format_file_alloc(app->storage);
        if(!flipper_format_file_open_existing(ff, path)) {
            flipper_format_free(ff);
            continue;
        }

        FuriString* filetype = furi_string_alloc();
        uint32_t version = 0;
        if(!flipper_format_read_header(ff, filetype, &version)) {
            furi_string_free(filetype);
            flipper_format_free(ff);
            continue;
        }
        furi_string_free(filetype);

        FuriString* name = furi_string_alloc();
        InfraredSignal* signal = infrared_signal_alloc();

        while(infrared_signal_read(signal, ff, name) == InfraredErrorCodeNone) {
            if(strncmp(furi_string_get_cstr(name), "Power", 5) == 0) {
                remember_unique(app, signal);
            }
        }

        infrared_signal_free(signal);
        furi_string_free(name);
        flipper_format_free(ff);

        if(app->unique_found >= MAX_UNIQUE) break;
    }

    storage_dir_close(dir);
    storage_file_free(dir);
}

static void run(EpsonApp* app) {
    app->state = AppStateSending;
    app->sent = 0;
    view_port_update(app->view_port);

    collect_power_signals(app);

    if(app->unique_found == 0) {
        app->state = AppStateError;
    } else {
        transmit_unique(app);
        app->state = AppStateDone;
    }

    view_port_update(app->view_port);
}

int32_t epson_universal_power_app(void* context) {
    UNUSED(context);

    EpsonApp* app = malloc(sizeof(EpsonApp));
    memset(app, 0, sizeof(EpsonApp));

    app->queue = furi_message_queue_alloc(QUEUE_SIZE, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->state = AppStateReady;

    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    bool running = true;

    while(running) {
        InputEvent event;

        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        if(event.key == InputKeyBack) {
            running = false;
        } else if(event.key == InputKeyOk && app->state == AppStateReady) {
            run(app);
        }
    }

    free_unique(app);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->queue);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);

    return 0;
}
