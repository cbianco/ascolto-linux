#ifndef ASCOLTO_MACOS_CONTROL_H
#define ASCOLTO_MACOS_CONTROL_H
#include "ascolto.h"
#include <dispatch/dispatch.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct ascolto_control_s ascolto_control_t;
ascolto_control_t *ascolto_control_start(ascolto_live_app_t *app, dispatch_queue_t actions);
/* Call on a background queue, before destroying the app or its action queue. */
void ascolto_control_stop(ascolto_control_t *control);
#ifdef __cplusplus
}
#endif
#endif
