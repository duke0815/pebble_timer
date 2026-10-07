// Timer for Pebble Time 2 (emery)
//
// Setup:   two drum wheels (minutes | seconds), spun by finger or UP/DOWN.
//          Long SELECT (or a tap) switches the wheel the buttons act on.
//          SELECT (or the "Start" button) starts the countdown.
// Running: SELECT pauses/resumes, UP/DOWN add/remove a minute,
//          long SELECT stops. The app may be closed - a wakeup brings it back.
// Done:    vibrates, then offers "Neuer Timer" or "Beenden".

#include <pebble.h>

// ---------------------------------------------------------------------------
// Persistent state

#define PERSIST_KEY_STATE 1
#define PERSIST_KEY_LAST_DURATION 2

#define WAKEUP_COOKIE 0x7133

typedef enum {
  TIMER_IDLE = 0,
  TIMER_RUNNING = 1,
  TIMER_PAUSED = 2,
} TimerStatus;

typedef struct {
  uint8_t status;
  int64_t end_ms;        // absolute end time while running
  int32_t remaining_ms;  // remaining time while paused
  int32_t total_ms;      // duration for the progress ring
  WakeupId wakeup_id;
} TimerState;

static TimerState s_timer;
static int s_last_duration = 5 * 60;

// ---------------------------------------------------------------------------
// UI state

typedef enum {
  MODE_SETUP,
  MODE_RUNNING,
  MODE_DONE,
} Mode;

#define WHEEL_MIN 0
#define WHEEL_SEC 1

#define ROW_H 46
#define FP 16                      // fixed point factor for wheel offsets
#define FRAME_MS 30
#define DRAG_THRESHOLD 6           // px before a touch counts as a drag
#define VEL_STOP (FP / 2)          // below this the fling ends and snapping begins
#define MAX_VEL (ROW_H * FP * 2 / 3)

typedef struct {
  Layer *layer;
  int value;
  int range;
  int32_t offset;   // content offset in 1/FP px; positive = moved down
  int32_t vel;      // fling velocity in 1/FP px per frame
  bool dragging;
} Wheel;

static Window *s_window;
static Layer *s_setup_layer;
static Layer *s_run_layer;
static Layer *s_done_layer;
static Wheel s_wheels[2];
static int s_focus = WHEEL_MIN;
static Mode s_mode = MODE_SETUP;

static AppTimer *s_anim_timer;
static AppTimer *s_tick_timer;
static AppTimer *s_vibe_timer;
static int s_vibe_count;
static int s_done_choice;   // 0 = Neuer Timer, 1 = Beenden
static int64_t s_done_at_ms;

// touch tracking
static bool s_touch_active;
static bool s_touch_moved;
static int s_touch_target;  // wheel index, or -1
static int16_t s_touch_x0, s_touch_y0, s_touch_last_y;
static uint32_t s_touch_last_ms;
static int32_t s_touch_vel;  // 1/FP px per frame, smoothed

// layout
static GRect s_wheel_frames[2];
static GRect s_start_btn;
static GRect s_pause_btn;
static GRect s_stop_btn;
static GRect s_new_btn;
static GRect s_quit_btn;

static GFont s_font_wheel;
static GFont s_font_big;

static const VibePattern s_done_vibe = {
  .durations = (uint32_t[]) { 400, 200, 400, 200, 800 },
  .num_segments = 5,
};

// ---------------------------------------------------------------------------
// Helpers

static int64_t now_ms(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  return (int64_t)s * 1000 + ms;
}

static int wrap(int v, int range) {
  v %= range;
  return v < 0 ? v + range : v;
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static bool rect_contains(GRect r, int16_t x, int16_t y) {
  return x >= r.origin.x && x < r.origin.x + r.size.w &&
         y >= r.origin.y && y < r.origin.y + r.size.h;
}

static int setup_duration(void) {
  return s_wheels[WHEEL_MIN].value * 60 + s_wheels[WHEEL_SEC].value;
}

static int32_t timer_remaining_ms(void) {
  if (s_timer.status == TIMER_RUNNING) {
    int64_t r = s_timer.end_ms - now_ms();
    return r > 0 ? (int32_t)r : 0;
  }
  if (s_timer.status == TIMER_PAUSED) {
    return s_timer.remaining_ms;
  }
  return 0;
}

static void save_state(void) {
  persist_write_data(PERSIST_KEY_STATE, &s_timer, sizeof(s_timer));
  persist_write_int(PERSIST_KEY_LAST_DURATION, s_last_duration);
}

static void load_state(void) {
  memset(&s_timer, 0, sizeof(s_timer));
  if (persist_exists(PERSIST_KEY_STATE)) {
    persist_read_data(PERSIST_KEY_STATE, &s_timer, sizeof(s_timer));
  }
  if (persist_exists(PERSIST_KEY_LAST_DURATION)) {
    s_last_duration = persist_read_int(PERSIST_KEY_LAST_DURATION);
  }
  if (s_last_duration <= 0 || s_last_duration > 99 * 60 + 59) {
    s_last_duration = 5 * 60;
  }
}

static void draw_pill(GContext *ctx, GRect r, const char *text, bool filled, GColor color) {
  if (filled) {
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_rect(ctx, r, r.size.h / 2, GCornersAll);
  } else {
    graphics_context_set_stroke_color(ctx, color);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_round_rect(ctx, grect_inset(r, GEdgeInsets(1)), r.size.h / 2 - 1);
  }
  graphics_context_set_text_color(ctx, filled ? GColorBlack : color);
  GRect tr = GRect(r.origin.x, r.origin.y + (r.size.h - 30) / 2, r.size.w, 30);
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), tr,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Small hint marks next to the physical buttons on the right edge.
typedef enum { HINT_NONE, HINT_UP, HINT_DOWN, HINT_PLAY, HINT_PAUSE, HINT_PLUS, HINT_MINUS } Hint;

static void draw_hint(GContext *ctx, GRect bounds, int y, Hint hint) {
  int x = bounds.size.w - 6;
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_context_set_stroke_width(ctx, 2);
  switch (hint) {
    case HINT_UP:
    case HINT_DOWN: {
      int d = hint == HINT_UP ? -1 : 1;
      GPoint pts[3] = { GPoint(x - 4, y - 2 * d), GPoint(x + 4, y - 2 * d), GPoint(x, y + 3 * d) };
      GPathInfo info = { 3, pts };
      GPath *p = gpath_create(&info);
      gpath_draw_filled(ctx, p);
      gpath_destroy(p);
      break;
    }
    case HINT_PLAY: {
      GPoint pts[3] = { GPoint(x - 3, y - 5), GPoint(x - 3, y + 5), GPoint(x + 4, y) };
      GPathInfo info = { 3, pts };
      GPath *p = gpath_create(&info);
      gpath_draw_filled(ctx, p);
      gpath_destroy(p);
      break;
    }
    case HINT_PAUSE:
      graphics_fill_rect(ctx, GRect(x - 3, y - 5, 2, 10), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(x + 1, y - 5, 2, 10), 0, GCornerNone);
      break;
    case HINT_PLUS:
      graphics_draw_line(ctx, GPoint(x, y - 4), GPoint(x, y + 4));
      /* fallthrough */
    case HINT_MINUS:
      graphics_draw_line(ctx, GPoint(x - 4, y), GPoint(x + 4, y));
      break;
    default:
      break;
  }
}

static void draw_button_hints(GContext *ctx, GRect bounds, Hint up, Hint select, Hint down) {
  draw_hint(ctx, bounds, bounds.size.h * 27 / 100, up);
  draw_hint(ctx, bounds, bounds.size.h / 2, select);
  draw_hint(ctx, bounds, bounds.size.h * 73 / 100, down);
}

// ---------------------------------------------------------------------------
// Wheel physics

static void wheel_normalize(Wheel *w) {
  const int32_t half = ROW_H * FP / 2;
  while (w->offset > half) {
    w->value = wrap(w->value - 1, w->range);
    w->offset -= ROW_H * FP;
  }
  while (w->offset < -half) {
    w->value = wrap(w->value + 1, w->range);
    w->offset += ROW_H * FP;
  }
}

static void anim_tick(void *data);

static void anim_kick(void) {
  if (!s_anim_timer) {
    s_anim_timer = app_timer_register(FRAME_MS, anim_tick, NULL);
  }
}

static void anim_tick(void *data) {
  s_anim_timer = NULL;
  bool busy = false;

  for (int i = 0; i < 2; i++) {
    Wheel *w = &s_wheels[i];
    if (w->dragging) {
      continue;
    }
    if (w->vel > VEL_STOP || w->vel < -VEL_STOP) {
      w->offset += w->vel;
      w->vel = w->vel * 9 / 10;
      wheel_normalize(w);
      busy = true;
    } else {
      // Snap back to the row centre. No normalizing here: a button step
      // starts a full row away and has to slide in without being undone.
      w->vel = 0;
      if (w->offset != 0) {
        int32_t step = w->offset / 3;
        if (step == 0) {
          step = w->offset;
        }
        w->offset -= step;
        busy = true;
      }
    }
    layer_mark_dirty(w->layer);
  }

  if (busy) {
    anim_kick();
  }
}

static void wheel_step(int idx, int delta) {
  Wheel *w = &s_wheels[idx];
  w->vel = 0;
  w->value = wrap(w->value + delta, w->range);
  // Keep the old value where it was on screen and let it slide into place.
  w->offset = clamp32(w->offset + delta * ROW_H * FP, -ROW_H * FP, ROW_H * FP);
  anim_kick();
  layer_mark_dirty(w->layer);
}

static void set_focus(int idx) {
  if (s_focus != idx) {
    s_focus = idx;
    layer_mark_dirty(s_wheels[0].layer);
    layer_mark_dirty(s_wheels[1].layer);
  }
}

static void wheels_set_duration(int seconds) {
  s_wheels[WHEEL_MIN].value = wrap(seconds / 60, s_wheels[WHEEL_MIN].range);
  s_wheels[WHEEL_SEC].value = wrap(seconds % 60, s_wheels[WHEEL_SEC].range);
  for (int i = 0; i < 2; i++) {
    s_wheels[i].offset = 0;
    s_wheels[i].vel = 0;
    s_wheels[i].dragging = false;
  }
}

// ---------------------------------------------------------------------------
// Drawing

static void wheel_update_proc(Layer *layer, GContext *ctx) {
  int idx = *(int *)layer_get_data(layer);
  Wheel *w = &s_wheels[idx];
  GRect b = layer_get_bounds(layer);
  int cy = b.size.h / 2;
  bool focused = idx == s_focus;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 12, GCornersAll);

  // selection band
  GRect band = GRect(2, cy - ROW_H / 2, b.size.w - 4, ROW_H);
  graphics_context_set_fill_color(ctx, focused ? GColorOrange : GColorDarkGray);
  graphics_fill_rect(ctx, band, 10, GCornersAll);

  int32_t off = w->offset / FP;
  char buf[4];
  for (int k = -3; k <= 3; k++) {
    int y = cy + k * ROW_H + off;
    if (y < -ROW_H || y > b.size.h + ROW_H) {
      continue;
    }
    int dist = y - cy;
    if (dist < 0) {
      dist = -dist;
    }
    GColor c;
    if (dist < ROW_H / 2) {
      c = focused ? GColorBlack : GColorWhite;
    } else if (dist < ROW_H * 3 / 2) {
      c = GColorLightGray;
    } else {
      c = GColorDarkGray;
    }
    snprintf(buf, sizeof(buf), "%02d", wrap(w->value + k, w->range));
    graphics_context_set_text_color(ctx, c);
    // LECO numbers have ~8px of internal top padding at this size
    GRect tr = GRect(0, y - 24, b.size.w, 44);
    graphics_draw_text(ctx, buf, s_font_wheel, tr, GTextOverflowModeFill,
                       GTextAlignmentCenter, NULL);
  }

  // fade top and bottom edges
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 1);
  for (int i = 0; i < 14; i += 2) {
    graphics_draw_line(ctx, GPoint(0, i), GPoint(b.size.w, i));
    graphics_draw_line(ctx, GPoint(0, b.size.h - 1 - i), GPoint(b.size.w, b.size.h - 1 - i));
  }

  if (focused) {
    graphics_context_set_stroke_color(ctx, GColorOrange);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_round_rect(ctx, grect_inset(b, GEdgeInsets(1)), 11);
  } else {
    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_round_rect(ctx, b, 12);
  }
}

static void setup_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  graphics_context_set_text_color(ctx, GColorLightGray);
  GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  for (int i = 0; i < 2; i++) {
    GRect f = s_wheel_frames[i];
    graphics_draw_text(ctx, i == WHEEL_MIN ? "MIN" : "SEK", small,
                       GRect(f.origin.x, 2, f.size.w, 18), GTextOverflowModeFill,
                       GTextAlignmentCenter, NULL);
  }

  // colon between the wheels
  int cx = (s_wheel_frames[0].origin.x + s_wheel_frames[0].size.w + s_wheel_frames[1].origin.x) / 2;
  int cy = s_wheel_frames[0].origin.y + s_wheel_frames[0].size.h / 2;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(cx, cy - 8), 3);
  graphics_fill_circle(ctx, GPoint(cx, cy + 8), 3);

  bool can_start = setup_duration() > 0;
  draw_pill(ctx, s_start_btn, "Start", can_start, can_start ? GColorIslamicGreen : GColorDarkGray);

  draw_button_hints(ctx, b, HINT_UP, HINT_PLAY, HINT_DOWN);
}

static void format_mmss(char *buf, size_t len, int32_t seconds) {
  snprintf(buf, len, "%02d:%02d", (int)(seconds / 60), (int)(seconds % 60));
}

static void run_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  bool paused = s_timer.status == TIMER_PAUSED;
  int32_t rem_ms = timer_remaining_ms();
  int32_t rem_s = (rem_ms + 999) / 1000;

  // progress ring
  GRect ring = GRect(b.size.w / 2 - 84, 8, 168, 168);
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_radial(ctx, ring, GOvalScaleModeFitCircle, 10, 0, TRIG_MAX_ANGLE);
  if (s_timer.total_ms > 0 && rem_ms > 0) {
    int32_t angle = (int32_t)((int64_t)TRIG_MAX_ANGLE * rem_ms / s_timer.total_ms);
    if (angle > TRIG_MAX_ANGLE) {
      angle = TRIG_MAX_ANGLE;
    }
    GColor c = paused ? GColorChromeYellow : (rem_s <= 10 ? GColorRed : GColorOrange);
    graphics_context_set_fill_color(ctx, c);
    graphics_fill_radial(ctx, ring, GOvalScaleModeFitCircle, 10, 0, angle);
  }

  int cy = ring.origin.y + ring.size.h / 2;

  char buf[12];
  format_mmss(buf, sizeof(buf), rem_s);
  graphics_context_set_text_color(ctx, paused ? GColorChromeYellow : GColorWhite);
  graphics_draw_text(ctx, buf, s_font_big, GRect(0, cy - 30, b.size.w, 50),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  graphics_context_set_text_color(ctx, GColorLightGray);
  GFont f18 = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  if (paused) {
    graphics_draw_text(ctx, "PAUSE", f18, GRect(0, cy - 52, b.size.w, 22),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  } else {
    time_t end = (time_t)(s_timer.end_ms / 1000);
    struct tm *t = localtime(&end);
    char clock_buf[16];
    strftime(clock_buf, sizeof(clock_buf), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
    char line[24];
    snprintf(line, sizeof(line), "Ende %s", clock_buf);
    graphics_draw_text(ctx, line, f18, GRect(0, cy + 20, b.size.w, 22),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }

  draw_pill(ctx, s_pause_btn, paused ? "Weiter" : "Pause", true,
            paused ? GColorIslamicGreen : GColorChromeYellow);
  draw_pill(ctx, s_stop_btn, "Stopp", false, GColorRed);

  draw_button_hints(ctx, b, HINT_PLUS, paused ? HINT_PLAY : HINT_PAUSE, HINT_MINUS);
}

static void done_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  GRect banner = GRect(0, 0, b.size.w, 104);
  graphics_context_set_fill_color(ctx, GColorDarkCandyAppleRed);
  graphics_fill_rect(ctx, banner, 0, GCornerNone);

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "Zeit um!", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(0, 12, b.size.w, 34), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  int64_t over = (now_ms() - s_done_at_ms) / 1000;
  if (over < 0) {
    over = 0;
  }
  if (over > 99 * 60 + 59) {
    over = 99 * 60 + 59;
  }
  char buf[14] = "+";
  format_mmss(buf + 1, sizeof(buf) - 1, (int32_t)over);
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_LECO_32_BOLD_NUMBERS),
                     GRect(0, 48, b.size.w, 44), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  draw_pill(ctx, s_new_btn, "Neuer Timer", s_done_choice == 0, GColorOrange);
  draw_pill(ctx, s_quit_btn, "Beenden", s_done_choice == 1, GColorLightGray);

  draw_button_hints(ctx, b, HINT_UP, HINT_PLAY, HINT_DOWN);
}

// ---------------------------------------------------------------------------
// Timer logic

static void set_mode(Mode mode);

static void cancel_wakeup(void) {
  if (s_timer.wakeup_id > 0) {
    wakeup_cancel(s_timer.wakeup_id);
  }
  s_timer.wakeup_id = 0;
}

static void schedule_wakeup(void) {
  cancel_wakeup();
  time_t when = (time_t)((s_timer.end_ms + 999) / 1000);
  WakeupId id = wakeup_schedule(when, WAKEUP_COOKIE, true);
  s_timer.wakeup_id = id > 0 ? id : 0;
  if (id < 0) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "wakeup_schedule failed: %d", (int)id);
  }
}

static void stop_tick(void) {
  if (s_tick_timer) {
    app_timer_cancel(s_tick_timer);
    s_tick_timer = NULL;
  }
}

static void vibe_tick(void *data) {
  s_vibe_timer = NULL;
  if (s_mode != MODE_DONE) {
    return;
  }
  layer_mark_dirty(s_done_layer);
  if (s_vibe_count > 0) {
    s_vibe_count--;
    // repeat the alarm every ~3s until the user reacts
    if (s_vibe_count % 3 == 0) {
      vibes_enqueue_custom_pattern(s_done_vibe);
      light_enable_interaction();
    }
  }
  s_vibe_timer = app_timer_register(1000, vibe_tick, NULL);
}

static void silence_alarm(void) {
  s_vibe_count = 0;
  vibes_cancel();
}

static void timer_finish(void) {
  stop_tick();
  cancel_wakeup();
  s_done_at_ms = s_timer.status == TIMER_RUNNING ? s_timer.end_ms : now_ms();
  if (s_done_at_ms > now_ms()) {
    s_done_at_ms = now_ms();
  }
  s_timer.status = TIMER_IDLE;
  save_state();
  set_mode(MODE_DONE);
}

static void tick(void *data) {
  s_tick_timer = NULL;
  if (s_timer.status != TIMER_RUNNING) {
    return;
  }
  int32_t rem = timer_remaining_ms();
  if (rem <= 0) {
    timer_finish();
    return;
  }
  layer_mark_dirty(s_run_layer);
  // wake up exactly when the displayed second changes
  int32_t next = rem % 1000;
  s_tick_timer = app_timer_register(next == 0 ? 1000 : next + 5, tick, NULL);
}

static void start_tick(void) {
  stop_tick();
  s_tick_timer = app_timer_register(1, tick, NULL);
}

static void timer_start(int seconds) {
  if (seconds <= 0) {
    return;
  }
  s_last_duration = seconds;
  s_timer.status = TIMER_RUNNING;
  s_timer.total_ms = seconds * 1000;
  s_timer.end_ms = now_ms() + s_timer.total_ms;
  schedule_wakeup();
  save_state();
  set_mode(MODE_RUNNING);
}

static void timer_toggle_pause(void) {
  if (s_timer.status == TIMER_RUNNING) {
    s_timer.remaining_ms = timer_remaining_ms();
    s_timer.status = TIMER_PAUSED;
    cancel_wakeup();
    stop_tick();
  } else if (s_timer.status == TIMER_PAUSED) {
    s_timer.end_ms = now_ms() + s_timer.remaining_ms;
    s_timer.status = TIMER_RUNNING;
    schedule_wakeup();
    start_tick();
  }
  save_state();
  layer_mark_dirty(s_run_layer);
}

static void timer_adjust(int32_t delta_ms) {
  int32_t rem = timer_remaining_ms();
  int32_t updated = rem + delta_ms;
  if (updated < 1000 || updated > (99 * 60 + 59) * 1000) {
    return;
  }
  if (s_timer.status == TIMER_RUNNING) {
    s_timer.end_ms += delta_ms;
    schedule_wakeup();
    start_tick();
  } else {
    s_timer.remaining_ms = updated;
  }
  if (updated > s_timer.total_ms) {
    s_timer.total_ms = updated;
  }
  save_state();
  layer_mark_dirty(s_run_layer);
}

static void timer_stop(void) {
  stop_tick();
  cancel_wakeup();
  s_timer.status = TIMER_IDLE;
  save_state();
  set_mode(MODE_SETUP);
}

static void set_mode(Mode mode) {
  s_mode = mode;
  layer_set_hidden(s_setup_layer, mode != MODE_SETUP);
  layer_set_hidden(s_run_layer, mode != MODE_RUNNING);
  layer_set_hidden(s_done_layer, mode != MODE_DONE);

  if (s_vibe_timer) {
    app_timer_cancel(s_vibe_timer);
    s_vibe_timer = NULL;
  }

  switch (mode) {
    case MODE_SETUP:
      silence_alarm();
      wheels_set_duration(s_last_duration);
      layer_mark_dirty(s_setup_layer);
      break;
    case MODE_RUNNING:
      if (s_timer.status == TIMER_RUNNING) {
        start_tick();
      }
      layer_mark_dirty(s_run_layer);
      break;
    case MODE_DONE:
      s_done_choice = 0;
      s_vibe_count = 30;  // ~30s of alarm, one pattern every 3s
      vibes_enqueue_custom_pattern(s_done_vibe);
      light_enable_interaction();
      s_vibe_timer = app_timer_register(1000, vibe_tick, NULL);
      layer_mark_dirty(s_done_layer);
      break;
  }
}

static void done_confirm(void) {
  silence_alarm();
  if (s_done_choice == 0) {
    set_mode(MODE_SETUP);
  } else {
    window_stack_pop_all(true);
  }
}

// ---------------------------------------------------------------------------
// Buttons

static void up_handler(ClickRecognizerRef rec, void *ctx) {
  switch (s_mode) {
    case MODE_SETUP:
      wheel_step(s_focus, 1);
      layer_mark_dirty(s_setup_layer);
      break;
    case MODE_RUNNING:
      timer_adjust(60 * 1000);
      break;
    case MODE_DONE:
      silence_alarm();
      s_done_choice = 0;
      layer_mark_dirty(s_done_layer);
      break;
  }
}

static void down_handler(ClickRecognizerRef rec, void *ctx) {
  switch (s_mode) {
    case MODE_SETUP:
      wheel_step(s_focus, -1);
      layer_mark_dirty(s_setup_layer);
      break;
    case MODE_RUNNING:
      timer_adjust(-60 * 1000);
      break;
    case MODE_DONE:
      silence_alarm();
      s_done_choice = 1;
      layer_mark_dirty(s_done_layer);
      break;
  }
}

static void select_handler(ClickRecognizerRef rec, void *ctx) {
  switch (s_mode) {
    case MODE_SETUP:
      if (setup_duration() > 0) {
        timer_start(setup_duration());
      } else {
        vibes_short_pulse();
      }
      break;
    case MODE_RUNNING:
      timer_toggle_pause();
      break;
    case MODE_DONE:
      done_confirm();
      break;
  }
}

static void select_long_handler(ClickRecognizerRef rec, void *ctx) {
  switch (s_mode) {
    case MODE_SETUP:
      set_focus(s_focus == WHEEL_MIN ? WHEEL_SEC : WHEEL_MIN);
      break;
    case MODE_RUNNING:
      vibes_short_pulse();
      timer_stop();
      break;
    case MODE_DONE:
      done_confirm();
      break;
  }
}

static void back_handler(ClickRecognizerRef rec, void *ctx) {
  if (s_mode == MODE_DONE) {
    silence_alarm();
  }
  // Leaving while running keeps the wakeup armed; the app comes back at 0:00.
  window_stack_pop_all(true);
}

static void click_config_provider(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120, up_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120, down_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 600, select_long_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_handler);
}

// ---------------------------------------------------------------------------
// Touch

static int wheel_at(int16_t x, int16_t y) {
  for (int i = 0; i < 2; i++) {
    GRect f = s_wheel_frames[i];
    // generous hit area: the full column, a bit wider than the wheel
    GRect hit = GRect(f.origin.x - 8, f.origin.y - 16, f.size.w + 16, f.size.h + 24);
    if (rect_contains(hit, x, y)) {
      return i;
    }
  }
  return -1;
}

static void handle_tap(int16_t x, int16_t y) {
  switch (s_mode) {
    case MODE_SETUP:
      if (rect_contains(s_start_btn, x, y)) {
        select_handler(NULL, NULL);
      } else if (s_touch_target >= 0) {
        set_focus(s_touch_target);
      }
      break;
    case MODE_RUNNING:
      if (rect_contains(s_stop_btn, x, y)) {
        vibes_short_pulse();
        timer_stop();
      } else {
        // tapping the ring or the pause button toggles pause
        timer_toggle_pause();
      }
      break;
    case MODE_DONE:
      if (rect_contains(s_new_btn, x, y)) {
        s_done_choice = 0;
        done_confirm();
      } else if (rect_contains(s_quit_btn, x, y)) {
        s_done_choice = 1;
        done_confirm();
      } else {
        silence_alarm();
      }
      break;
  }
}

static void touch_handler(const TouchEvent *event, void *context) {
  uint32_t t;
  {
    time_t s;
    uint16_t ms;
    time_ms(&s, &ms);
    t = (uint32_t)s * 1000 + ms;
  }

  switch (event->type) {
    case TouchEvent_Touchdown: {
      s_touch_active = true;
      s_touch_moved = false;
      s_touch_x0 = event->x;
      s_touch_y0 = event->y;
      s_touch_last_y = event->y;
      s_touch_last_ms = t;
      s_touch_vel = 0;
      s_touch_target = s_mode == MODE_SETUP ? wheel_at(event->x, event->y) : -1;
      if (s_touch_target >= 0) {
        Wheel *w = &s_wheels[s_touch_target];
        w->dragging = true;
        w->vel = 0;  // catch a spinning wheel
      }
      break;
    }

    case TouchEvent_PositionUpdate: {
      if (!s_touch_active) {
        break;
      }
      int dx = event->x - s_touch_x0;
      int dy_total = event->y - s_touch_y0;
      if (!s_touch_moved && (dy_total > DRAG_THRESHOLD || dy_total < -DRAG_THRESHOLD ||
                             dx > DRAG_THRESHOLD || dx < -DRAG_THRESHOLD)) {
        s_touch_moved = true;
        if (s_touch_target >= 0) {
          set_focus(s_touch_target);
        }
      }
      if (s_touch_target >= 0 && s_touch_moved) {
        Wheel *w = &s_wheels[s_touch_target];
        int dy = event->y - s_touch_last_y;
        w->offset += dy * FP;
        wheel_normalize(w);

        uint32_t dt = t - s_touch_last_ms;
        if (dt == 0) {
          dt = 1;
        }
        int32_t inst = (int32_t)dy * FP * FRAME_MS / (int32_t)dt;
        s_touch_vel = (s_touch_vel + inst * 2) / 3;
        layer_mark_dirty(w->layer);
        layer_mark_dirty(s_setup_layer);
      }
      s_touch_last_y = event->y;
      s_touch_last_ms = t;
      break;
    }

    case TouchEvent_Liftoff: {
      if (!s_touch_active) {
        break;
      }
      s_touch_active = false;
      if (s_touch_target >= 0) {
        Wheel *w = &s_wheels[s_touch_target];
        w->dragging = false;
        // a finger resting before lift-off means no fling
        if (s_touch_moved && t - s_touch_last_ms < 80) {
          w->vel = clamp32(s_touch_vel, -MAX_VEL, MAX_VEL);
        } else {
          w->vel = 0;
        }
        anim_kick();
      }
      if (!s_touch_moved) {
        handle_tap(event->x, event->y);
      }
      if (s_mode == MODE_SETUP) {
        layer_mark_dirty(s_setup_layer);
      }
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Window

static void wakeup_handler(WakeupId id, int32_t cookie) {
  // The app was open when the timer ran out.
  if (s_timer.status == TIMER_RUNNING) {
    s_timer.wakeup_id = 0;
    timer_finish();
  }
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  int w = b.size.w;
  int h = b.size.h;

  // keep the right edge free for the button hints
  int usable = w - 14;
  int wheel_w = (usable - 8 - 18) / 2;
  int wheel_top = 22;
  int wheel_h = h - wheel_top - 52;
  s_wheel_frames[WHEEL_MIN] = GRect(4, wheel_top, wheel_w, wheel_h);
  s_wheel_frames[WHEEL_SEC] = GRect(4 + wheel_w + 18, wheel_top, wheel_w, wheel_h);

  s_start_btn = GRect(24, h - 44, usable - 40, 36);
  s_pause_btn = GRect(8, h - 44, (usable - 24) / 2, 36);
  s_stop_btn = GRect(16 + (usable - 24) / 2, h - 44, (usable - 24) / 2, 36);
  s_new_btn = GRect(12, 118, usable - 16, 40);
  s_quit_btn = GRect(12, 168, usable - 16, 40);

  s_setup_layer = layer_create(b);
  layer_set_update_proc(s_setup_layer, setup_update_proc);
  layer_add_child(root, s_setup_layer);

  int ranges[2] = { 100, 60 };
  for (int i = 0; i < 2; i++) {
    s_wheels[i].layer = layer_create_with_data(s_wheel_frames[i], sizeof(int));
    *(int *)layer_get_data(s_wheels[i].layer) = i;
    s_wheels[i].range = ranges[i];
    layer_set_update_proc(s_wheels[i].layer, wheel_update_proc);
    layer_add_child(s_setup_layer, s_wheels[i].layer);
  }

  s_run_layer = layer_create(b);
  layer_set_update_proc(s_run_layer, run_update_proc);
  layer_add_child(root, s_run_layer);

  s_done_layer = layer_create(b);
  layer_set_update_proc(s_done_layer, done_update_proc);
  layer_add_child(root, s_done_layer);

  // Decide where to start: an expired timer (e.g. launched by the wakeup),
  // a running/paused timer, or a fresh setup.
  if (s_timer.status == TIMER_RUNNING && timer_remaining_ms() <= 0) {
    timer_finish();
  } else if (s_timer.status == TIMER_RUNNING || s_timer.status == TIMER_PAUSED) {
    if (s_timer.status == TIMER_RUNNING) {
      schedule_wakeup();  // make sure it is armed
      save_state();
    }
    set_mode(MODE_RUNNING);
  } else if (launch_reason() == APP_LAUNCH_WAKEUP) {
    s_done_at_ms = now_ms();
    set_mode(MODE_DONE);
  } else {
    set_mode(MODE_SETUP);
  }
}

static void window_unload(Window *window) {
  for (int i = 0; i < 2; i++) {
    layer_destroy(s_wheels[i].layer);
  }
  layer_destroy(s_setup_layer);
  layer_destroy(s_run_layer);
  layer_destroy(s_done_layer);
}

static void init(void) {
  load_state();

  s_font_wheel = fonts_get_system_font(FONT_KEY_LECO_36_BOLD_NUMBERS);
  s_font_big = fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS);

  wakeup_service_subscribe(wakeup_handler);

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_set_touch_bridge_disabled(s_window, true);
  window_stack_push(s_window, true);

  touch_service_subscribe(touch_handler, NULL);
}

static void deinit(void) {
  touch_service_unsubscribe();
  if (s_anim_timer) {
    app_timer_cancel(s_anim_timer);
  }
  stop_tick();
  if (s_vibe_timer) {
    app_timer_cancel(s_vibe_timer);
  }
  if (s_mode == MODE_SETUP && setup_duration() > 0) {
    s_last_duration = setup_duration();
  }
  save_state();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
