#pragma once
#include "../gamestream/client.h"
#include "ui_draw.h"

/* Menu-scoped art session. begin() captures host/port and starts the
 * worker thread; end() stops it (menu exit). Draw calls use cached data
 * or a placeholder and schedule loads asynchronously; completed art is
 * persisted under <config_dir>/art/. */
void ui_art_session_begin(gs_server_t *server, const char *config_dir);
void ui_art_session_end(void);

/* Returns 1 when new art arrived since the previous call (redraw hint). */
int ui_art_pump(void);

void ui_art_clear(void);
void ui_art_draw(gs_server_t *server, int app_id, ui_surface_t *surface,
                 int x, int y, int width, int height);
void ui_art_draw_round(gs_server_t *server, int app_id, ui_surface_t *surface,
                      int x, int y, int width, int height, int radius);
void ui_landscape(ui_surface_t *surface, int x, int y, int w, int h, int radius);

/* Snap a square tile size onto the render-cache ladder (TILE_W..HERO_W,
 * 32 px steps, endpoints pinned). The Games row quantizes its glide-time
 * tile sizes with this so each (app, size) is scaled once and blitted
 * afterward; see the ladder note in ui_art.c. Sizes outside the ladder
 * clamp to the nearest endpoint. */
int ui_art_ladder_snap(int size);
