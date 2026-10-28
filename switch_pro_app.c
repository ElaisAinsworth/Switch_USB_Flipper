#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdio.h>

#include "pro_usb.h"

typedef struct {
    uint32_t btn;
    int8_t lx;
    int8_t ly;
} KeyMap;

typedef struct {
    const char* name;
    const char* hint1;
    const char* hint2;
    KeyMap keys[InputKeyMAX];
} Layer;

/* Hold OK to cycle layers, hold Back to exit. Edit this table to remap. */
static const Layer layers[] = {
    {
        .name = "D-PAD",
        .hint1 = "Arrows: D-pad",
        .hint2 = "OK: A   Back: B",
        .keys =
            {
                [InputKeyUp] = {PRO_BTN_UP, 0, 0},
                [InputKeyDown] = {PRO_BTN_DOWN, 0, 0},
                [InputKeyLeft] = {PRO_BTN_LEFT, 0, 0},
                [InputKeyRight] = {PRO_BTN_RIGHT, 0, 0},
                [InputKeyOk] = {PRO_BTN_A, 0, 0},
                [InputKeyBack] = {PRO_BTN_B, 0, 0},
            },
    },
    {
        .name = "FACE",
        .hint1 = "Up X Dn B Lf Y Rt A",
        .hint2 = "OK: R   Back: L",
        .keys =
            {
                [InputKeyUp] = {PRO_BTN_X, 0, 0},
                [InputKeyDown] = {PRO_BTN_B, 0, 0},
                [InputKeyLeft] = {PRO_BTN_Y, 0, 0},
                [InputKeyRight] = {PRO_BTN_A, 0, 0},
                [InputKeyOk] = {PRO_BTN_R, 0, 0},
                [InputKeyBack] = {PRO_BTN_L, 0, 0},
            },
    },
    {
        .name = "TRIGGERS",
        .hint1 = "Up ZR Dn ZL Lf L Rt R",
        .hint2 = "OK: A   Back: B",
        .keys =
            {
                [InputKeyUp] = {PRO_BTN_ZR, 0, 0},
                [InputKeyDown] = {PRO_BTN_ZL, 0, 0},
                [InputKeyLeft] = {PRO_BTN_L, 0, 0},
                [InputKeyRight] = {PRO_BTN_R, 0, 0},
                [InputKeyOk] = {PRO_BTN_A, 0, 0},
                [InputKeyBack] = {PRO_BTN_B, 0, 0},
            },
    },
    {
        .name = "L-STICK",
        .hint1 = "Arrows: left stick",
        .hint2 = "OK: A   Back: B",
        .keys =
            {
                [InputKeyUp] = {0, 0, 1},
                [InputKeyDown] = {0, 0, -1},
                [InputKeyLeft] = {0, -1, 0},
                [InputKeyRight] = {0, 1, 0},
                [InputKeyOk] = {PRO_BTN_A, 0, 0},
                [InputKeyBack] = {PRO_BTN_B, 0, 0},
            },
    },
    {
        .name = "SYSTEM",
        .hint1 = "Up + Dn - Lf Home Rt Cap",
        .hint2 = "OK: A   Back: B",
        .keys =
            {
                [InputKeyUp] = {PRO_BTN_PLUS, 0, 0},
                [InputKeyDown] = {PRO_BTN_MINUS, 0, 0},
                [InputKeyLeft] = {PRO_BTN_HOME, 0, 0},
                [InputKeyRight] = {PRO_BTN_CAPTURE, 0, 0},
                [InputKeyOk] = {PRO_BTN_A, 0, 0},
                [InputKeyBack] = {PRO_BTN_B, 0, 0},
            },
    },
};

#define LAYER_COUNT (sizeof(layers) / sizeof(layers[0]))

typedef struct {
    ViewPort* view_port;
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    size_t layer;
    KeyMap held[InputKeyMAX]; // what each physical key is currently holding down
} App;

static int8_t clamp1(int v) {
    return v > 1 ? 1 : (v < -1 ? -1 : v);
}

static void push_state(App* app) {
    uint32_t btn = 0;
    int lx = 0, ly = 0;
    for(int i = 0; i < InputKeyMAX; i++) {
        btn |= app->held[i].btn;
        lx += app->held[i].lx;
        ly += app->held[i].ly;
    }
    pro_usb_set_input(btn, clamp1(lx), clamp1(ly), 0, 0);
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const Layer* layer = &layers[app->layer];
    size_t idx = app->layer;
    furi_mutex_release(app->mutex);

    const char* status = "";
    switch(pro_usb_status()) {
    case ProUsbDisconnected:
        status = "USB: no host";
        break;
    case ProUsbEnumerated:
        status = "USB: waiting for Switch";
        break;
    case ProUsbHandshake:
        status = "USB: handshake...";
        break;
    case ProUsbActive:
        status = "USB: connected";
        break;
    }

    char line[32];
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, "Switch Pro Controller");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 20, status);
    snprintf(line, sizeof(line), "Layer %u/%u: %s", (unsigned)(idx + 1), (unsigned)LAYER_COUNT, layer->name);
    canvas_draw_str(canvas, 2, 31, line);
    canvas_draw_str(canvas, 2, 41, layer->hint1);
    canvas_draw_str(canvas, 2, 51, layer->hint2);
    canvas_draw_str(canvas, 2, 62, "Hold OK=layer  Back=exit");
}

static void input_cb(InputEvent* event, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t switch_pro_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_cb, app);
    view_port_input_callback_set(app->view_port, input_cb, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    pro_usb_start();

    bool running = true;
    InputEvent event;
    while(running) {
        if(furi_message_queue_get(app->queue, &event, 150) == FuriStatusOk &&
           event.key < InputKeyMAX) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            switch(event.type) {
            case InputTypePress:
                app->held[event.key] = layers[app->layer].keys[event.key];
                break;
            case InputTypeRelease:
                memset(&app->held[event.key], 0, sizeof(KeyMap));
                break;
            case InputTypeLong:
                if(event.key == InputKeyBack) {
                    running = false;
                } else if(event.key == InputKeyOk) {
                    memset(&app->held[InputKeyOk], 0, sizeof(KeyMap));
                    app->layer = (app->layer + 1) % LAYER_COUNT;
                }
                break;
            default:
                break;
            }
            push_state(app);
            furi_mutex_release(app->mutex);
        }
        view_port_update(app->view_port);
    }

    pro_usb_set_input(0, 0, 0, 0, 0);
    pro_usb_stop();

    gui_remove_view_port(gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
