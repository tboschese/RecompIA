#ifndef SNESRE_TRACE_H
#define SNESRE_TRACE_H

#include "port.h"

#ifdef __cplusplus
extern "C" {
#endif

void snesre_trace_save(void);

#ifdef __cplusplus
}
#endif

extern bool snesre_trace_active;
void snesre_trace_start(void);
void snesre_trace(uint32 pbpc, uint16 p, uint8 length);

#endif
