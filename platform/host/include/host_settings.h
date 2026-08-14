// What a person chose, as opposed to what a run was told.
//
// The launcher writes these; the port reads them at startup. Every one of them
// has an FRLG_ environment variable that came first and still takes precedence
// -- see the note in settings.c about why that order and not the other one.

#ifndef GUARD_HOST_SETTINGS_H
#define GUARD_HOST_SETTINGS_H

#include <stddef.h>

// Where settings live, created if it is not there.
int host_config_dir(char *buf, size_t len);

int host_setting_bool(const char *key, int fallback);
int host_setting_int(const char *key, int fallback);

#endif // GUARD_HOST_SETTINGS_H
