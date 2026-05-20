#ifndef PROVEN_FLOAT_CONFIG_H
#define PROVEN_FLOAT_CONFIG_H

/*
 * Compile-time configuration for the float module scaffold.
 *
 * PROVEN_FMT_SHORTEST keeps the shortest-formatting mode available to the
 * policy API. The hosted default remains fixed-6 unless callers explicitly
 * request shortest mode.
 * PROVEN_ENABLE_FLOAT_FORMAT_RYU remains reserved for future table-backed
 * variants.
 */
#ifndef PROVEN_FMT_SHORTEST
#define PROVEN_FMT_SHORTEST 0
#endif

#ifndef PROVEN_ENABLE_FLOAT_FORMAT_RYU
#define PROVEN_ENABLE_FLOAT_FORMAT_RYU 0
#endif

#endif /* PROVEN_FLOAT_CONFIG_H */
