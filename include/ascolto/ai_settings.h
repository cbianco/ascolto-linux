#ifndef ASCOLTO_AI_SETTINGS_H
#define ASCOLTO_AI_SETTINGS_H
#include <stdbool.h>
#include <stddef.h>
/* Shared process boundary. Neither argv nor this configuration contains secrets. */
int ascolto_ai_worker_paths(const char *script_name, char *script, size_t script_capacity,
                            char *python, size_t python_capacity);
int ascolto_ai_task_start(const char *config, const char *transcript, const char *task);
bool ascolto_ai_automatic_enabled(const char *config_path);
int ascolto_ai_recap_start(const char *config_path, const char *transcript, bool automatic);
#endif
