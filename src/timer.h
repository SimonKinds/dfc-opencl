#ifndef DFC_TIMING_H
#define DFC_TIMING_H

#include <stdint.h>

#define TIMER_ENVIRONMENT_SETUP 1
#define TIMER_ENVIRONMENT_TEARDOWN 2

#define TIMER_ADD_PATTERNS 4
#define TIMER_READ_DATA 5

#define TIMER_COMPILE_DFC 6

#define TIMER_WRITE_TO_DEVICE 7
#define TIMER_READ_FROM_DEVICE 8
#define TIMER_EXECUTE_KERNEL 9

#define TIMER_EXECUTE_HETEROGENEOUS 10
#define TIMER_PROCESS_MATCHES 11

#define TIMER_SEARCH 12

#ifdef __cplusplus
extern "C" {
#endif

void startTimer(int timer);
void stopTimer(int timer);
void resetTimer(int timer);

double readTimerMs(int timer);

#ifdef __cplusplus
}
#endif

#endif
