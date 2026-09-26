/**
 * Entur Departures — interactive Pebble watchapp
 *
 * Shows next public-transport departures for a set of predefined JOURNEYS
 * (fixed stop pairs), where the travel DIRECTION is chosen automatically from
 * the phone's GPS location: standing near Oslo S shows Oslo S -> Ski trains,
 * standing near Ski shows Ski -> Oslo S. All network + geo work happens on the
 * phone in src/pkjs/index.js (Entur JourneyPlanner v3 GraphQL + Geocoder); this
 * C side is a thin display + navigation layer driven by AppMessage.
 *
 * Windows:
 *   Main   (MenuLayer) : list of journeys, each row "Label" + "-> Dest".
 *   Detail (MenuLayer) : departure board for the selected journey.
 *
 * Protocol (phone -> watch), keyed by MSG_TYPE:
 *   0 = menu       : PAYLOAD = journeys, one per line "label\tdirection\tbig"
 *                    big = "1" when that journey should use the large,
 *                    glanceable departure board (readable while cycling).
 *   1 = departures : JOURNEY_INDEX + PAYLOAD = "header\nHH:MM\tLINE\tETA\tTRACK\tBIKE\n..."
 *                    BIKE = "1" when the train is easy to take a bike on
 *                    (step-free, roomy entrance) — drawn as a bike symbol.
 *   2 = error      : JOURNEY_INDEX + PAYLOAD = message
 * Protocol (watch -> phone), keyed by REQUEST:
 *   1 = send menu
 *   2 = send departures for JOURNEY_INDEX
 *
 * Target: emery (200x228). All layout via layer_get_bounds().
 */

#include <pebble.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define MAX_JOURNEYS     8
#define MAX_DEPARTURES  10

#define LABEL_LEN   32
#define DIR_LEN     24
#define HEADER_LEN  48
#define TIME_LEN     8
#define LINE_LEN    12
#define ETA_LEN     16
#define TRACK_LEN   24   // "Spor 3 > 18" (boarding track > arrival track)
#define SUB_LEN     (ETA_LEN + TRACK_LEN + 6)
#define ERR_LEN     64

#define PAYLOAD_BUFFER_SIZE 512

// Big-mode row height: trades rows-on-screen for text readable at a glance.
// Normal mode registers no height callback at all, so it keeps the platform
// default rather than a hardcoded guess at it.
#define BIG_CELL_H          88
#define PLACEHOLDER_CELL_H  60   // loading / error / empty rows, plain text

#define PERSIST_KEY_MENU 1

// AppMessage request codes (watch -> phone). Non-zero so a 0 never reads as
// "no request" on the phone side.
#define REQ_MENU        1
#define REQ_DEPARTURES  2

// A request can go unanswered for reasons the watch cannot see: the phone-side
// JS may not be up yet, the outbox may be busy, or a geolocation callback may
// never fire. Without a watchdog that leaves the board spinning forever with no
// way out except force-quitting the app, so every request is retried and then
// surfaced as something the user can act on.
#define REQ_TIMEOUT_MS  5000
#define REQ_MAX_TRIES   3

// ============================================================================
// GLOBAL STATE
// ============================================================================

// --- Journeys (main menu) ---
static char s_journey_label[MAX_JOURNEYS][LABEL_LEN];
static char s_journey_dir[MAX_JOURNEYS][DIR_LEN];
static bool s_journey_big[MAX_JOURNEYS];   // large-text board for this journey
static int  s_journey_count = 0;
static bool s_menu_loading  = true;

// --- Departures (detail) ---
static char s_dep_time[MAX_DEPARTURES][TIME_LEN];
static char s_dep_line[MAX_DEPARTURES][LINE_LEN];
static char s_dep_eta[MAX_DEPARTURES][ETA_LEN];                 // "6 min"
static char s_dep_track[MAX_DEPARTURES][TRACK_LEN];             // "Spor 3 > 18"
static char s_dep_row[MAX_DEPARTURES][TIME_LEN + LINE_LEN + 4]; // "HH:MM  R11"
static char s_dep_sub[MAX_DEPARTURES][SUB_LEN];                 // "6 min · Spor 3"
static bool s_dep_bike[MAX_DEPARTURES];                         // bike-friendly train
static int  s_dep_count = 0;
static char s_dep_header[HEADER_LEN];
static char s_dep_error[ERR_LEN];
static int  s_detail_index   = -1;   // which journey the detail window shows
static bool s_detail_loading = false;

static char s_payload_buffer[PAYLOAD_BUFFER_SIZE];

// --- UI ---
static Window         *s_main_window;
static StatusBarLayer *s_main_status_bar;
static MenuLayer      *s_main_menu;

static Window         *s_detail_window;
static StatusBarLayer *s_detail_status_bar;
static MenuLayer      *s_detail_menu;

// ============================================================================
// PARSING (phone payload strings -> arrays)
// ============================================================================

static void copy_field(char *dst, int dstsize, const char *src, int len) {
    if (len < 0) len = 0;
    int n = (len < dstsize - 1) ? len : dstsize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Split a '\t'-separated line into at most `max` fields; returns the count.
static int split_fields(const char *line, int line_len, const char **fs, int *fl,
                        int max) {
    int nf = 0;
    const char *f = line; int rem = line_len;
    while (nf < max) {
        const char *t = memchr(f, '\t', rem);
        int l = t ? (int)(t - f) : rem;
        fs[nf] = f; fl[nf] = l; nf++;
        if (!t) break;
        rem -= l + 1; f = t + 1;
    }
    return nf;
}

// Menu payload: one journey per '\n' line, fields split by '\t':
//   "Oslo–Ski\t→ Ski\t1"
static void parse_menu(const char *payload) {
    s_journey_count = 0;
    const char *p = payload;
    while (*p && s_journey_count < MAX_JOURNEYS) {
        const char *nl = strchr(p, '\n');
        int line_len = nl ? (int)(nl - p) : (int)strlen(p);

        const char *fs[3]; int fl[3];
        int nf = split_fields(p, line_len, fs, fl, 3);

        int i = s_journey_count;
        copy_field(s_journey_label[i], LABEL_LEN, fs[0], fl[0]);
        if (nf > 1) copy_field(s_journey_dir[i], DIR_LEN, fs[1], fl[1]);
        else        s_journey_dir[i][0] = '\0';
        s_journey_big[i] = (nf > 2 && fl[2] > 0 && fs[2][0] == '1');

        s_journey_count++;
        if (!nl) break;
        p = nl + 1;
    }
}

// Departures payload: first line = header, then one departure per '\n' line,
// fields split by '\t': "17:45\tR11\t3 min"
static void parse_departures(const char *payload) {
    s_dep_count = 0;
    const char *p = payload;

    const char *nl = strchr(p, '\n');
    int hlen = nl ? (int)(nl - p) : (int)strlen(p);
    copy_field(s_dep_header, HEADER_LEN, p, hlen);
    if (!nl) return;
    p = nl + 1;

    while (*p && s_dep_count < MAX_DEPARTURES) {
        nl = strchr(p, '\n');
        int line_len = nl ? (int)(nl - p) : (int)strlen(p);

        // Split the row into up to 5 tab-separated fields:
        //   time \t line \t eta \t track \t bike
        const char *fs[5]; int fl[5];
        int nf = split_fields(p, line_len, fs, fl, 5);

        int i = s_dep_count;
        copy_field(s_dep_time[i], TIME_LEN, fs[0], fl[0]);
        s_dep_line[i][0]  = '\0';
        s_dep_eta[i][0]   = '\0';
        s_dep_track[i][0] = '\0';
        if (nf > 1) copy_field(s_dep_line[i],  LINE_LEN,  fs[1], fl[1]);
        if (nf > 2) copy_field(s_dep_eta[i],   ETA_LEN,   fs[2], fl[2]);
        if (nf > 3) copy_field(s_dep_track[i], TRACK_LEN, fs[3], fl[3]);
        s_dep_bike[i] = (nf > 4 && fl[4] > 0 && fs[4][0] == '1');

        // Precompute title + subtitle so the draw callback stays cheap.
        if (s_dep_line[i][0]) {
            snprintf(s_dep_row[i], sizeof(s_dep_row[i]), "%s  %s",
                     s_dep_time[i], s_dep_line[i]);
        } else {
            snprintf(s_dep_row[i], sizeof(s_dep_row[i]), "%s", s_dep_time[i]);
        }
        if (s_dep_track[i][0]) {
            snprintf(s_dep_sub[i], sizeof(s_dep_sub[i]), "%s · %s",
                     s_dep_eta[i], s_dep_track[i]);
        } else {
            snprintf(s_dep_sub[i], sizeof(s_dep_sub[i]), "%s", s_dep_eta[i]);
        }
        s_dep_count++;
        if (!nl) break;
        p = nl + 1;
    }
}

// ============================================================================
// APPMESSAGE
// ============================================================================

static AppTimer *s_menu_timer;
static AppTimer *s_dep_timer;
static int       s_menu_tries;
static int       s_dep_tries;

static void menu_timeout(void *data);
static void dep_timeout(void *data);

static void request_menu(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_uint8(iter, MESSAGE_KEY_REQUEST, REQ_MENU);
        app_message_outbox_send();
    }
    // Armed even when the send failed — a busy outbox is exactly the case that
    // needs retrying.
    if (s_menu_timer) app_timer_cancel(s_menu_timer);
    s_menu_timer = app_timer_register(REQ_TIMEOUT_MS, menu_timeout, NULL);
}

static void request_departures(int index) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_uint8(iter, MESSAGE_KEY_REQUEST, REQ_DEPARTURES);
        dict_write_int32(iter, MESSAGE_KEY_JOURNEY_INDEX, index);
        app_message_outbox_send();
    }
    if (s_dep_timer) app_timer_cancel(s_dep_timer);
    s_dep_timer = app_timer_register(REQ_TIMEOUT_MS, dep_timeout, NULL);
}

static void menu_timeout(void *data) {
    s_menu_timer = NULL;
    if (!s_menu_loading) return;              // already answered
    if (++s_menu_tries < REQ_MAX_TRIES) { request_menu(); return; }
    // Give up quietly: a persisted list is already on screen, and if there
    // isn't one the empty-state row explains what to do.
    s_menu_loading = false;
    if (s_main_menu) menu_layer_reload_data(s_main_menu);
}

static void dep_timeout(void *data) {
    s_dep_timer = NULL;
    if (s_detail_index < 0 || !s_detail_loading) return;
    if (++s_dep_tries < REQ_MAX_TRIES) {
        request_departures(s_detail_index);   // re-arms the timer
        return;
    }
    s_detail_loading = false;
    if (s_dep_count == 0 && !s_dep_error[0]) {
        snprintf(s_dep_error, sizeof(s_dep_error), "Phone didn't answer");
    }
    if (s_detail_menu) menu_layer_reload_data(s_detail_menu);
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
    Tuple *type_t = dict_find(iter, MESSAGE_KEY_MSG_TYPE);
    if (!type_t) return;
    int type = (int)type_t->value->int32;
    Tuple *payload_t = dict_find(iter, MESSAGE_KEY_PAYLOAD);

    if (type == 0) {                    // menu
        if (s_menu_timer) { app_timer_cancel(s_menu_timer); s_menu_timer = NULL; }
        if (payload_t) {
            parse_menu(payload_t->value->cstring);
            persist_write_string(PERSIST_KEY_MENU, payload_t->value->cstring);
        }
        s_menu_loading = false;
        if (s_main_menu) menu_layer_reload_data(s_main_menu);
        return;
    }

    // departures (1) and error (2) target a specific journey — ignore stale
    // replies for a journey the user has already navigated away from.
    Tuple *idx_t = dict_find(iter, MESSAGE_KEY_JOURNEY_INDEX);
    int idx = idx_t ? (int)idx_t->value->int32 : -1;
    if (idx != s_detail_index) return;

    if (s_dep_timer) { app_timer_cancel(s_dep_timer); s_dep_timer = NULL; }

    if (type == 1) {                    // departures
        if (payload_t) parse_departures(payload_t->value->cstring);
        s_dep_error[0]  = '\0';
        s_detail_loading = false;
    } else if (type == 2) {             // error
        s_dep_count = 0;
        s_dep_header[0] = '\0';
        copy_field(s_dep_error, ERR_LEN, payload_t ? payload_t->value->cstring : "Error",
                   payload_t ? (int)strlen(payload_t->value->cstring) : 5);
        s_detail_loading = false;
    }
    if (s_detail_menu) menu_layer_reload_data(s_detail_menu);
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "inbox dropped: %d", (int)reason);
}

static void outbox_failed_callback(DictionaryIterator *iter,
                                   AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "outbox failed: %d", (int)reason);
}

// ============================================================================
// DETAIL WINDOW (departure board for one journey)
// ============================================================================

// number of rows to draw when there are no real departures (loading/error/empty)
static int detail_placeholder_rows(void) { return 1; }

// Does the journey currently on screen want the large, glanceable board?
static bool detail_is_big(void) {
    return s_detail_index >= 0 && s_detail_index < MAX_JOURNEYS &&
           s_journey_big[s_detail_index];
}

// Only registered for big-mode journeys (see detail_window_load).
static int16_t detail_cell_height(MenuLayer *menu, MenuIndex *cell_index, void *c) {
    if (s_detail_loading || s_dep_error[0] || s_dep_count == 0) {
        return PLACEHOLDER_CELL_H;
    }
    return BIG_CELL_H;
}

// A bicycle drawn with primitives so it follows the cell's text colour
// (inverted when the row is highlighted). `half` is the scale in halves: 2 draws
// it BIKE_W x BIKE_H, 3 draws it half as large again for the big board.
#define BIKE_W 26
#define BIKE_H 16
#define BIKE_PT(o, px, py, half) GPoint((o).x + (px) * (half) / 2, (o).y + (py) * (half) / 2)

static void draw_bike(GContext *ctx, GPoint o, int half, GColor color) {
    GPoint rear  = BIKE_PT(o, 6,  10, half);
    GPoint front = BIKE_PT(o, 20, 10, half);
    GPoint crank = BIKE_PT(o, 12, 10, half);
    GPoint seat  = BIKE_PT(o, 10, 4,  half);
    GPoint head  = BIKE_PT(o, 18, 4,  half);
    int r = 5 * half / 2;

    graphics_context_set_stroke_color(ctx, color);
    graphics_context_set_stroke_width(ctx, half >= 3 ? 3 : 2);
    graphics_draw_circle(ctx, rear, r);
    graphics_draw_circle(ctx, front, r);
    graphics_draw_line(ctx, rear, crank);
    graphics_draw_line(ctx, rear, seat);
    graphics_draw_line(ctx, seat, crank);
    graphics_draw_line(ctx, seat, head);
    graphics_draw_line(ctx, crank, head);
    graphics_draw_line(ctx, head, front);
    graphics_draw_line(ctx, BIKE_PT(o, 8, 1, half), BIKE_PT(o, 12, 1, half));
    graphics_draw_line(ctx, head, BIKE_PT(o, 21, 1, half));
    graphics_context_set_stroke_width(ctx, 1);
}

static GColor cell_text_color(const Layer *cell) {
    return menu_cell_layer_is_highlighted(cell) ? GColorWhite : GColorBlack;
}

// Large layout: the countdown fills the row, the track sits under it in bold,
// and the clock time + line code drop to a small third line. Everything you
// need to decide at a glance is in the top two lines.
static void detail_draw_big_row(GContext *ctx, const Layer *cell, int i) {
    GRect b = layer_get_bounds(cell);
    GColor fg = cell_text_color(cell);
    graphics_context_set_text_color(ctx, fg);

    // Leave the countdown's right edge for the bike so they never collide.
    int eta_w = b.size.w - 8 - (s_dep_bike[i] ? BIKE_W * 3 / 2 + 4 : 0);
    graphics_draw_text(ctx, s_dep_eta[i],
        fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
        GRect(4, b.origin.y - 8, eta_w, 46),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    if (s_dep_bike[i]) {
        draw_bike(ctx, GPoint(b.size.w - BIKE_W * 3 / 2 - 6, b.origin.y + 8), 3, fg);
    }

    graphics_draw_text(ctx, s_dep_track[i],
        fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
        GRect(4, b.origin.y + 34, b.size.w - 8, 32),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    graphics_draw_text(ctx, s_dep_row[i],
        fonts_get_system_font(FONT_KEY_GOTHIC_18),
        GRect(4, b.origin.y + 62, b.size.w - 8, 22),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static uint16_t detail_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
    if (s_detail_loading || s_dep_error[0] || s_dep_count == 0) {
        return detail_placeholder_rows();
    }
    return s_dep_count;
}

static int16_t detail_header_height(MenuLayer *menu, uint16_t section, void *ctx) {
    return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void detail_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *c) {
    menu_cell_basic_header_draw(ctx, cell,
        s_dep_header[0] ? s_dep_header : "Departures");
}

static void detail_draw_row(GContext *ctx, const Layer *cell,
                            MenuIndex *cell_index, void *c) {
    if (s_detail_loading) {
        menu_cell_basic_draw(ctx, cell, "Loading…", NULL, NULL);
        return;
    }
    if (s_dep_error[0]) {
        menu_cell_basic_draw(ctx, cell, s_dep_error, "SELECT to retry", NULL);
        return;
    }
    if (s_dep_count == 0) {
        menu_cell_basic_draw(ctx, cell, "No departures",
                             "Nothing in the next hours", NULL);
        return;
    }
    int i = cell_index->row;
    if (detail_is_big()) {
        detail_draw_big_row(ctx, cell, i);
        return;
    }
    menu_cell_basic_draw(ctx, cell, s_dep_row[i], s_dep_sub[i], NULL);
    if (s_dep_bike[i]) {
        GRect b = layer_get_bounds(cell);
        draw_bike(ctx, GPoint(b.size.w - BIKE_W - 6, b.origin.y + 8), 2,
                  cell_text_color(cell));
    }
}

static void detail_select(MenuLayer *menu, MenuIndex *cell_index, void *ctx) {
    // SELECT refreshes the board — and is the way out of a failed load.
    s_dep_tries      = 0;
    s_dep_error[0]   = '\0';
    s_detail_loading = true;
    if (s_detail_menu) menu_layer_reload_data(s_detail_menu);
    request_departures(s_detail_index);
}

static void detail_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(root);

    s_detail_status_bar = status_bar_layer_create();
    layer_add_child(root, status_bar_layer_get_layer(s_detail_status_bar));

    GRect mbounds = GRect(0, STATUS_BAR_LAYER_HEIGHT, bounds.size.w,
                          bounds.size.h - STATUS_BAR_LAYER_HEIGHT);
    s_detail_menu = menu_layer_create(mbounds);
    MenuLayerCallbacks cbs = (MenuLayerCallbacks) {
        .get_num_rows       = detail_num_rows,
        .get_header_height  = detail_header_height,
        .draw_header        = detail_draw_header,
        .draw_row           = detail_draw_row,
        .select_click       = detail_select,
    };
    // Left NULL in normal mode so the platform's own cell height is used.
    if (detail_is_big()) cbs.get_cell_height = detail_cell_height;
    menu_layer_set_callbacks(s_detail_menu, NULL, cbs);
    menu_layer_set_click_config_onto_window(s_detail_menu, window);
    layer_add_child(root, menu_layer_get_layer(s_detail_menu));
}

static void detail_window_unload(Window *window) {
    if (s_dep_timer) { app_timer_cancel(s_dep_timer); s_dep_timer = NULL; }
    menu_layer_destroy(s_detail_menu);
    status_bar_layer_destroy(s_detail_status_bar);
    window_destroy(window);
    s_detail_window  = NULL;
    s_detail_menu    = NULL;
    s_detail_index   = -1;
}

static void open_detail(int index) {
    s_detail_index   = index;
    s_dep_tries      = 0;
    s_detail_loading = true;
    s_dep_count      = 0;
    s_dep_error[0]   = '\0';
    // Seed the header from the journey label so the board isn't blank while
    // the phone resolves direction + departures.
    snprintf(s_dep_header, sizeof(s_dep_header), "%s", s_journey_label[index]);

    s_detail_window = window_create();
    window_set_window_handlers(s_detail_window, (WindowHandlers) {
        .load   = detail_window_load,
        .unload = detail_window_unload,
    });
    window_stack_push(s_detail_window, true);

    request_departures(index);
}

// ============================================================================
// MAIN WINDOW (journeys list)
// ============================================================================

static uint16_t main_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
    if (s_journey_count == 0) return 1;   // placeholder row
    return s_journey_count;
}

static int16_t main_header_height(MenuLayer *menu, uint16_t section, void *ctx) {
    return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void main_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *c) {
    menu_cell_basic_header_draw(ctx, cell, "Journeys");
}

static void main_draw_row(GContext *ctx, const Layer *cell,
                          MenuIndex *cell_index, void *c) {
    if (s_journey_count == 0) {
        if (s_menu_loading) {
            menu_cell_basic_draw(ctx, cell, "Loading…", NULL, NULL);
        } else {
            menu_cell_basic_draw(ctx, cell, "No journeys",
                                 "Set them in phone settings", NULL);
        }
        return;
    }
    int i = cell_index->row;
    menu_cell_basic_draw(ctx, cell, s_journey_label[i],
                         s_journey_dir[i][0] ? s_journey_dir[i] : NULL, NULL);
}

static void main_select(MenuLayer *menu, MenuIndex *cell_index, void *ctx) {
    if (s_journey_count == 0) {
        s_menu_tries   = 0;
        s_menu_loading = true;
        request_menu();       // retry fetching the config
        menu_layer_reload_data(menu);
        return;
    }
    open_detail(cell_index->row);
}

static void main_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(root);

    s_main_status_bar = status_bar_layer_create();
    layer_add_child(root, status_bar_layer_get_layer(s_main_status_bar));

    GRect mbounds = GRect(0, STATUS_BAR_LAYER_HEIGHT, bounds.size.w,
                          bounds.size.h - STATUS_BAR_LAYER_HEIGHT);
    s_main_menu = menu_layer_create(mbounds);
    menu_layer_set_callbacks(s_main_menu, NULL, (MenuLayerCallbacks) {
        .get_num_rows      = main_num_rows,
        .get_header_height = main_header_height,
        .draw_header       = main_draw_header,
        .draw_row          = main_draw_row,
        .select_click      = main_select,
    });
    menu_layer_set_click_config_onto_window(s_main_menu, window);
    layer_add_child(root, menu_layer_get_layer(s_main_menu));
}

static void main_window_unload(Window *window) {
    menu_layer_destroy(s_main_menu);
    status_bar_layer_destroy(s_main_status_bar);
    s_main_menu = NULL;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

static void init(void) {
    // Restore last-known journey list so a cold launch shows something instantly.
    if (persist_exists(PERSIST_KEY_MENU)) {
        persist_read_string(PERSIST_KEY_MENU, s_payload_buffer,
                            sizeof(s_payload_buffer));
        parse_menu(s_payload_buffer);
        if (s_journey_count > 0) s_menu_loading = false;
    }

    s_main_window = window_create();
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load   = main_window_load,
        .unload = main_window_unload,
    });
    window_stack_push(s_main_window, true);

    // Register callbacks BEFORE opening — an early reply must not be dropped.
    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_open(PAYLOAD_BUFFER_SIZE + 128, 128);

    // Ask the phone for the current journey list. (The phone also pushes the
    // menu on its own 'ready' event, so either path populates the list.)
    request_menu();
}

static void deinit(void) {
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}
