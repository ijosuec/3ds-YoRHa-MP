#pragma once

#include <stdbool.h>

void battery_init(void);
void battery_exit(void);

bool battery_read(int *percent, bool *charging);
