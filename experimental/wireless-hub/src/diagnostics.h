#pragma once
#include <stddef.h>
#include <stdbool.h>
bool hub_diagnostics_safe_mode(void);
void hub_diagnostic_stage(unsigned int stage);
void hub_diagnostics_status(char *out, size_t size);
