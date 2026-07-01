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
 *   0 = menu       : PAYLOAD = journeys, one per line "label\tdirection"
 *   1 = departures : JOURNEY_INDEX + PAYLOAD = "header\nHH:MM\tLINE\tETA\n..."
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
#define TRACK_LEN   16
#define SUB_LEN     (ETA_LEN + TRACK_LEN + 6)
#define ERR_LEN     64

#define PAYLOAD_BUFFER_SIZE 512

#define PERSIST_KEY_MENU 1

// AppMessage request codes (watch -> phone). Non-zero so a 0 never reads as
// "no request" on the phone side.
#define REQ_MENU        1
#define REQ_DEPARTURES  2

// ============================================================================
// GLOBAL STATE
// ============================================================================

// --- Journeys (main menu) ---
static char s_journey_label[MAX_JOURNEYS][LABEL_LEN];
static char s_journey_dir[MAX_JOURNEYS][DIR_LEN];
static int  s_journey_count = 0;
static bool s_menu_loading  = true;

// --- Departures (detail) ---
static char s_dep_time[MAX_DEPARTURES][TIME_LEN];
static char s_dep_line[MAX_DEPARTURES][LINE_LEN];
static char s_dep_row[MAX_DEPARTURES][TIME_LEN + LINE_LEN + 4]; // "HH:MM  R11"
static char s_dep_sub[MAX_DEPARTURES][SUB_LEN];                 // "6 min · Spor 3"
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

// Menu payload: one journey per '\n' line, fields split by '\t':
//   "Oslo–Ski\t→ Ski"
static void parse_menu(const char *payload) {
    s_journey_count = 0;
    const char *p = payload;
    while (*p && s_journey_count < MAX_JOURNEYS) {
        const char *nl = strchr(p, '\n');
        int line_len = nl ? (int)(nl - p) : (int)strlen(p);
        const char *tab = memchr(p, '\t', line_len);
        int lab_len = tab ? (int)(tab - p) : line_len;
        int i = s_journey_count;
        copy_field(s_journey_label[i], LABEL_LEN, p, lab_len);
        if (tab) {
            copy_field(s_journey_dir[i], DIR_LEN, tab + 1, line_len - lab_len - 1);
        } else {
            s_journey_dir[i][0] = '\0';
        }
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

        // Split the row into up to 4 tab-separated fields:
        //   time \t line \t eta \t track
        const char *fs[4]; int fl[4]; int nf = 0;
        const char *f = p; int rem = line_len;
        while (nf < 4) {
            const char *t = memchr(f, '\t', rem);
            int l = t ? (int)(t - f) : rem;
            fs[nf] = f; fl[nf] = l; nf++;
            if (!t) break;
            rem -= l + 1; f = t + 1;
        }

        int i = s_dep_count;
        copy_field(s_dep_time[i], TIME_LEN, fs[0], fl[0]);
        s_dep_line[i][0] = '\0';
        char eta[ETA_LEN]   = "";
        char track[TRACK_LEN] = "";
        if (nf > 1) copy_field(s_dep_line[i], LINE_LEN, fs[1], fl[1]);
        if (nf > 2) copy_field(eta,   ETA_LEN,   fs[2], fl[2]);
        if (nf > 3) copy_field(track, TRACK_LEN, fs[3], fl[3]);

        // Precompute title + subtitle so the draw callback stays cheap.
        if (s_dep_line[i][0]) {
            snprintf(s_dep_row[i], sizeof(s_dep_row[i]), "%s  %s",
                     s_dep_time[i], s_dep_line[i]);
        } else {
            snprintf(s_dep_row[i], sizeof(s_dep_row[i]), "%s", s_dep_time[i]);
        }
        if (track[0]) {
            snprintf(s_dep_sub[i], sizeof(s_dep_sub[i]), "%s · %s", eta, track);
        } else {
            snprintf(s_dep_sub[i], sizeof(s_dep_sub[i]), "%s", eta);
        }
        s_dep_count++;
        if (!nl) break;
        p = nl + 1;
    }
}

// ============================================================================
// APPMESSAGE
// ============================================================================

static void request_menu(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST, REQ_MENU);
    app_message_outbox_send();
}

static void request_departures(int index) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST, REQ_DEPARTURES);
    dict_write_int32(iter, MESSAGE_KEY_JOURNEY_INDEX, index);
    app_message_outbox_send();
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
    Tuple *type_t = dict_find(iter, MESSAGE_KEY_MSG_TYPE);
    if (!type_t) return;
    int type = (int)type_t->value->int32;
    Tuple *payload_t = dict_find(iter, MESSAGE_KEY_PAYLOAD);

    if (type == 0) {                    // menu
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
        menu_cell_basic_draw(ctx, cell, "No data", s_dep_error, NULL);
        return;
    }
    if (s_dep_count == 0) {
        menu_cell_basic_draw(ctx, cell, "No departures",
                             "Nothing in the next hours", NULL);
        return;
    }
    int i = cell_index->row;
    menu_cell_basic_draw(ctx, cell, s_dep_row[i], s_dep_sub[i], NULL);
}

static void detail_select(MenuLayer *menu, MenuIndex *cell_index, void *ctx) {
    // SELECT refreshes the board.
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
    menu_layer_set_callbacks(s_detail_menu, NULL, (MenuLayerCallbacks) {
        .get_num_rows       = detail_num_rows,
        .get_header_height  = detail_header_height,
        .draw_header        = detail_draw_header,
        .draw_row           = detail_draw_row,
        .select_click       = detail_select,
    });
    menu_layer_set_click_config_onto_window(s_detail_menu, window);
    layer_add_child(root, menu_layer_get_layer(s_detail_menu));
}

static void detail_window_unload(Window *window) {
    menu_layer_destroy(s_detail_menu);
    status_bar_layer_destroy(s_detail_status_bar);
    window_destroy(window);
    s_detail_window  = NULL;
    s_detail_menu    = NULL;
    s_detail_index   = -1;
}

static void open_detail(int index) {
    s_detail_index   = index;
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
        request_menu();       // retry fetching the config
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
