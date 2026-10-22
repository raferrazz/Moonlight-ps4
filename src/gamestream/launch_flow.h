#pragma once
/* Launch ladder with progress reporting and cancellation.
 *
 * This used to live in stream.c. It moved here so the host build (and the
 * mock-server tests) can exercise the exact ladder the PS4 runs, without
 * linking the video/input stack: launch_flow.c depends only on the
 * gamestream client and pthreads.
 *
 * Field reports against a Sunshine fork showed the
 * client-side half of "click a game -> the app looks frozen": the ladder
 * below can legitimately spend tens of seconds in bounded HTTP attempts
 * (fresh /launch + status re-check + retry + /cancel + final retry) with
 * NOTHING drawn on screen between the menu closing and the first video
 * frame. The progress text + cancellation here feed the animated launch
 * screen (ui_launch_screen_run) so that wait is visible and cancellable
 * instead of reading as a hang. */

#include "client.h"

/* Shared between the launch worker thread and the UI thread.
 *
 * Ordering contract: the worker writes phase[] and rc, then sets done=1;
 * the UI thread only reads phase[]/rc after observing done!=0 (and reads
 * phase[] opportunistically before that - worst case one stale line on a
 * cancellable status screen). cancelled is written only by the UI thread.
 * On x86-64 (PS4/host) the release-ordering of the volatile stores plus
 * the volatile read of done is sufficient for the fields the UI acts on.
 */
typedef struct {
    volatile int cancelled; /* UI thread requests abort */
    volatile int done;      /* worker finished; rc is final */
    volatile int rc;        /* 0 launched, -1 failed, -2 cancelled */
    char phase[128];        /* current human-readable step (worker writes) */
} launch_progress_t;

/* Full launch ladder for app_id. Same recovery logic as the
 * stream.c:launch_app(), plus per-step progress text and cancellation
 * checks between the (bounded) HTTP attempts. display_name is optional
 * (shown in the phase text); pass NULL for a generic wording.
 * Returns 0 on success, -1 on failure (gs_error has the reason),
 * -2 when progress->cancelled was set. */
int launch_app_flow(gs_server_t *server, STREAM_CONFIGURATION *stream,
                    int app_id, bool sops, bool local_audio,
                    const char *display_name, launch_progress_t *progress);

/* Recovery after LiStartConnection failed against a LIVE session (the
 * launch or resume succeeded server-side, but the stream handshake did
 * not): /cancel the stuck session, then a clean /launch. This is the
 * "first click fails, second works" pattern collapsed into ONE click.
 * Same return codes as launch_app_flow(). */
int launch_recover_flow(gs_server_t *server, STREAM_CONFIGURATION *stream,
                        int app_id, bool sops, bool local_audio,
                        const char *display_name, launch_progress_t *progress);
