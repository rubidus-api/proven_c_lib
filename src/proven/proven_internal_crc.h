#ifndef PROVEN_INTERNAL_CRC_H
#define PROVEN_INTERNAL_CRC_H

/* For tests: whether the CRCs fold with a carry-less multiply on this processor (detected and
 * checked against the tables once), and a switch that keeps them on the tables. */

#include "proven/types.h"

[[nodiscard]] bool proven_crc_is_hardware_(void);
void proven_crc_force_portable_(bool on);

#endif
