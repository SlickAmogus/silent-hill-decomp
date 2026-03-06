/* PSY-Q sys/file.h shim for PC port */
#ifndef _PSYQ_COMPAT_SYS_FILE_H
#define _PSYQ_COMPAT_SYS_FILE_H

/* PSX file flags */
#define FREAD       0x0001
#define FWRITE      0x0002
#define FNBLOCK     0x0004
#define FCREAT      0x0200
#define FNBUF       0x4000
#define FASYNC      0x8000

#define O_RDONLY    FREAD
#define O_WRONLY    FWRITE
#define O_RDWR      (FREAD|FWRITE)
#define O_CREAT     FCREAT
#define O_NOBUF     FNBUF
#define O_NBLOCK    FNBLOCK
#define O_NOWAIT    FASYNC

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

#endif /* _PSYQ_COMPAT_SYS_FILE_H */
