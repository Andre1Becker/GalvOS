#pragma once
/**
 * contract_features.h -- gates for Contract assertions whose API does not
 * exist yet.
 *
 * Two invariants (noSilentPointLoss, statsConsistent) are stated against
 * optimizer telemetry that wave B / prompt 1 introduces. A test cannot
 * reference a symbol that is not declared, so each gate defaults to 0 and the
 * dependent test fails with an explicit "not implemented" message instead.
 * That RED is the specification -- see CONTRACT.md.
 *
 * When the wave that provides the API lands, it defines the matching macro to
 * 1 (in the header that declares the API, or as a build flag) and the guarded
 * assertions compile in. Do not define one of these to 1 without the API
 * actually being there: that turns a specification into a silent pass.
 */

// P1 -- optimizer::gLastStats (emittedLit / emittedBlank / truncated /
// jumpCount / jumpDistanceTotal). Gates invariants 3 and 8.
#ifndef GALVOS_OPT_HAS_STATS
#define GALVOS_OPT_HAS_STATS 0
#endif
