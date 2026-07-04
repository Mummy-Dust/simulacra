#pragma once
#include <stdint.h>

typedef enum { ESCALATION_NEW = 0, ESCALATION_RECURRING, ESCALATION_PERSISTENT } detect_escalation_t;

#define DETECT_ESC_PERSIST_SESSIONS 3
#define DETECT_ESC_PERSIST_PLACES   2
#define DETECT_ESC_RECUR_SESSIONS   2
#define DETECT_ESC_RECUR_PLACES     3

static inline detect_escalation_t threat_escalation_level(uint8_t sessions_seen, uint8_t places_seen)
{
    if (sessions_seen >= DETECT_ESC_PERSIST_SESSIONS && places_seen >= DETECT_ESC_PERSIST_PLACES)
        return ESCALATION_PERSISTENT;
    if (sessions_seen >= DETECT_ESC_RECUR_SESSIONS || places_seen >= DETECT_ESC_RECUR_PLACES)
        return ESCALATION_RECURRING;
    return ESCALATION_NEW;
}
static inline const char *escalation_name(detect_escalation_t e)
{
    return e == ESCALATION_PERSISTENT ? "PERSISTENT" : e == ESCALATION_RECURRING ? "RECURRING" : "NEW";
}
