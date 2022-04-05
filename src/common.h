#ifndef _COMMON_H_
#define _COMMON_H_

#define _DEFAULT_SOURCE

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <endian.h>
#include <time.h>
#include <ctype.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/resource.h>

#define TIME_FORMAT "%Y-%m-%d %H:%M:%S"
#define TIMEZONE_CHECK 1800.0
#endif
