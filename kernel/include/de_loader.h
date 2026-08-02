#pragma once

/* ============================================================================
 * PD-OS  —  DE Loader  (internal kernel API)
 * ============================================================================ */

#include "de_api.h"

/* Populate the pd_api_t struct and write its pointer to DE_API_PTR_ADDR.
 * Must be called before de_load_and_run() or the built-in GDE launches. */
void de_populate_api(void);

/* Try to load and run an external DE from PDFS.
 * Returns 0 if no external DE is configured — caller should use built-in GDE.
 * If an external DE is found its entry is called and this never returns. */
int de_load_and_run(void);
