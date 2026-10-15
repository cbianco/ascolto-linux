#include "ascolto/offline.h"
#include <stdlib.h>

/* --------------------------------------------------------------------------
 * Simulated Mic Monitor Backend
 * -------------------------------------------------------------------------- */
typedef struct {
    double active_start_s;
    double active_end_s;
    bool current_state;
} sim_mic_impl_t;

static bool sim_mic_is_active(ascolto_mic_monitor_t* self) {
    sim_mic_impl_t* impl = (sim_mic_impl_t*)self->impl;
    return impl ? impl->current_state : false;
}

static void sim_mic_step(ascolto_mic_monitor_t* self, double current_time_s) {
    sim_mic_impl_t* impl = (sim_mic_impl_t*)self->impl;
    if (!impl) return;
    bool next_state = (current_time_s >= impl->active_start_s && current_time_s < impl->active_end_s);
    if (next_state != impl->current_state) {
        impl->current_state = next_state;
        if (self->callback) {
            self->callback(next_state, current_time_s, self->userdata);
        }
    }
}

static void sim_mic_destroy(ascolto_mic_monitor_t* self) {
    if (!self) return;
    free(self->impl);
    free(self);
}

ascolto_mic_monitor_t* ascolto_mic_monitor_create_simulated(
    double active_start_s,
    double active_end_s,
    ascolto_mic_callback_t cb,
    void* userdata
) {
    ascolto_mic_monitor_t* m = (ascolto_mic_monitor_t*)calloc(1, sizeof(ascolto_mic_monitor_t));
    sim_mic_impl_t* impl = (sim_mic_impl_t*)calloc(1, sizeof(sim_mic_impl_t));
    if (!m || !impl) {
        free(m);
        free(impl);
        return NULL;
    }
    impl->active_start_s = active_start_s;
    impl->active_end_s = active_end_s;
    impl->current_state = false;

    m->backend_name = "simulated";
    m->is_active = sim_mic_is_active;
    m->step = sim_mic_step;
    m->destroy = sim_mic_destroy;
    m->callback = cb;
    m->userdata = userdata;
    m->impl = impl;
    return m;
}
