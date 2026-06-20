/* Minimal config.h for out-of-tree VLC 3.0.x plugins (macOS) */
#ifndef CONFIG_H
#define CONFIG_H

#define PACKAGE_VERSION "3.0.23"
#define VERSION "3.0.23"
#define PACKAGE_NAME "vlc-upnp-renderer"
#define PACKAGE_BUGREPORT ""

#define ENABLE_SOUT 1
#define HAVE_DYNAMIC_PLUGINS 1

#define HAVE_PTHREAD 1
#define HAVE_CLOCK_NSEC 1
#define HAVE_POLL 1
#define HAVE_ARPA_INET_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_NETDB_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_INET_PTON 1

/* Darwin provides these — prevent vlc_fixups redefinitions */
#define HAVE_LLDIV 1
#define HAVE_MAX_ALIGN_T 1
#define HAVE_FLOCKFILE 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_STRLCPY 1
#define HAVE_ASPRINTF 1
#define HAVE_GETENV 1
#define HAVE_LOCALTIME_R 1
#define HAVE_GMTIME_R 1
#define HAVE_TIMEGM 1
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_TIMESPEC_GET 1
#define HAVE_NANF 1

#define SYS_DARWIN 1

#include <vlc_fixups.h>

#endif /* CONFIG_H */