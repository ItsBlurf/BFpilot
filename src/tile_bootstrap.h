/*
 * After the web UI is up, optionally inject the separate launcher installer
 * so the home-screen tile gets installed/refreshed without linking AppInst
 * into bfpilot.elf.
 */

#pragma once

/* Non-blocking best-effort. Safe to call once per process start. */
void bfpilot_tile_bootstrap_try(void);
