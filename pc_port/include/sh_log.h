/*
 * sh_log.h - Simple logging for Silent Hill PC port
 */
#ifndef SH_LOG_H
#define SH_LOG_H

#include <stdio.h>

#define SH_LOG(fmt, ...) printf("[SH] " fmt "\n", ##__VA_ARGS__)
#define SH_WARN(fmt, ...) printf("[SH WARN] " fmt "\n", ##__VA_ARGS__)
#define SH_ERR(fmt, ...) fprintf(stderr, "[SH ERROR] " fmt "\n", ##__VA_ARGS__)

#endif /* SH_LOG_H */
