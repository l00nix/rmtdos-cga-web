/*
 * Copyright 2022 Dennis Jenkins <dennis.jenkins.75@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __RMTDOS_SERVER_CONFIG_H
#define __RMTDOS_SERVER_CONFIG_H

/* Maximum count of Ethernet buffers to dynamically allocate before going
   resident.  In practice, we only need 2 or 3, but really slow machines might
   need more to avoid packet drops.
*/
#define MAX_BUFFERS 10

/* Count of buffers to allocate if not overridden on the command line. */
#define DEFAULT_BUFFERS 2

/* Enabling 'DEBUG' will considerably increase the resident memory usage. */
#define DEBUG 0

/* Int 28 ("DOS Idle") is used for DOS-safe file-transfer writes. */
#define HAS_INT28 1

#endif /* __RMTDOS_SERVER_CONFIG_H */
