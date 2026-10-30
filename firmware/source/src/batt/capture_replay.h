#ifndef CHIPLER_CAPTURE_REPLAY_H
#define CHIPLER_CAPTURE_REPLAY_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint32_t frames,valid,rejected,offline71,short_a8,errors; } capture_stats_t;
void capture_replay_start(void);
/* Work budget is a frame count; returns true once when a run completes. No I/O/alloc. */
bool capture_replay_step(unsigned budget);
capture_stats_t capture_replay_stats(void);
unsigned capture_replay_count(void);
#endif
