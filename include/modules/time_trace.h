#ifndef VITA2NATIVE_TIME_TRACE_H
#define VITA2NATIVE_TIME_TRACE_H

/*
 * Diagnostic counters for VITA2NATIVE_DEBUG_CLOCK_TRACE.
 *
 * A title that never reads a clock advances once per presented frame, so on a
 * host that only renders 30 fps it runs at half of real speed - while a title
 * that reads process time or the vblank count keeps real-time pacing and just
 * shows fewer frames. These counters are how the two are told apart: they are
 * printed once a second with the display's frame-rate line and then reset.
 */
extern unsigned int g_time_trace_process;
extern unsigned int g_time_trace_rtc;
extern unsigned int g_time_trace_vcount;

#endif /* VITA2NATIVE_TIME_TRACE_H */
