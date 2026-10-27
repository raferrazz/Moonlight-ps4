/* Declarations the menu sources expect from the Orbis video layer.
 * video.h guards its menu-UI section behind __ORBIS__; the host harness
 * implements the same functions, so re-declare them for the host build. */
#pragma once

#include <stdint.h>

int video_ui_begin(int w, int h);
int video_ui_present(const uint8_t *src, int src_pitch);
void video_ui_end(void);
