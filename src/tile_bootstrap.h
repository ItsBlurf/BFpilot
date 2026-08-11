/* Integrated BFpilot Media-tile bootstrap. */

#pragma once

/* Starts the one-shot installer thread without blocking the web listener. */
void bfpilot_tile_bootstrap_try(void);

int bfpilot_tile_bootstrap_busy(void);
