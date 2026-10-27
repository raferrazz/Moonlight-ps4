/* Host-side test harness shared by render_ui.c (scenario driver) and
 * host_stubs.c. Nothing here ships in the PS4 build.
 *
 * The harness compiles the REAL menu sources (ui_menu.c and friends) and
 * replaces only the hardware edges: VideoOut presentation (captured to
 * files), pad input (scripted event queue) and the TLS gamestream client
 * (deterministic stubs). Menu behavior, layout code, state transitions and
 * return codes that run are the production ones. */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* --- scripted input ------------------------------------------------ */
/* Queue one button-edge event, delivered by input_menu_poll() no earlier
 * than `delay_ms` from now (0 = as soon as polled). Repeated events with
 * the same mask are allowed (auto-repeat must come from the real pad
 * layer; the menu only consumes edges). */
void harness_press(unsigned mask, int delay_ms);
void harness_clear_input(void);
void harness_set_no_pad(int no_pad);
int harness_input_pending(void);

/* --- capture -------------------------------------------------------- */
/* Called from the test driver while a menu/loop is running (video_ui_*
 * active). Saves the most recently presented frame as a PPM (converted to
 * PNG by the build script). Thread-safe against presents. */
void harness_capture(const char *name);
/* Output directory for captures (set by the driver's main()). */
void harness_set_shot_dir(const char *dir);
int harness_frames_presented(void);
void harness_reset_frame_count(void);
long harness_present_ms(void);

/* --- deterministic gamestream stubs ---------------------------------- */
/* Behavior knobs for the background refresh job. */
void harness_gs_refresh(int mode); /* 0=fail fast, 1=fail slow, 2=succeed,
                                      3=succeed with 12 applist entries */
#define HARNESS_GS_FAIL_FAST 0
#define HARNESS_GS_FAIL_SLOW 1
#define HARNESS_GS_OK 2
#define HARNESS_GS_OK_LIST 3
