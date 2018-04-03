#ifndef _DT_BINDINGS_NET_SAMSUNG_IPC_H
#define _DT_BINDINGS_NET_SAMSUNG_IPC_H 1

#define SAMSUNG_IPC_FORMAT_FMT 0 /* Modem commands */
#define SAMSUNG_IPC_FORMAT_RAW 1 /* Raw network data */
#define SAMSUNG_IPC_FORMAT_RFS 2 /* Remote Filesystem access */
#define SAMSUNG_IPC_FORMAT_MULTI_RAW 3 /* Multiplexed raw network data */
#define SAMSUNG_IPC_FORMAT_CMD 4 /* Link-layer control commands */
#define SAMSUNG_IPC_FORMAT_RAMDUMP 5 /* Modem RAM dump */
#define SAMSUNG_IPC_FORMAT_MAX 6 /* Invalid */

#define SAMSUNG_IPC_TYPE_MISC 0 /* miscdevice */
#define SAMSUNG_IPC_TYPE_NETDEV 1 /* network device */
#define SAMSUNG_IPC_TYPE_DUMMY 2 /* doesn't need userspace interface */
#define SAMSUNG_IPC_TYPE_MAX 3 /* Invalid */

#define SAMSUNG_IPC_VERSION_40 40
#define SAMSUNG_IPC_VERSION_41 41
#define SAMSUNG_IPC_VERSION_42 42
#endif
