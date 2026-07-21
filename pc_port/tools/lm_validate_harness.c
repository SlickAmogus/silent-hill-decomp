/* Offline driver for lm_validate.c. Build ad hoc (not part of the CMake build):
 *   gcc -O2 -I../include -o lmv lm_validate_harness.c ../src/lm_validate.c
 * Usage: lmv [--tail N] [--prop] [--bones N] FILE...
 * One PASS/FAIL line per file; exit code = number of failures. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lm_validate.h"

int main(int argc, char** argv)
{
    s_LmValidateParams params;
    int                failures = 0;
    int                passes   = 0;
    int                i;

    params.tailSlackBytes = LM_VALIDATE_BUF_TAIL;
    params.anmBoneCount   = -1;
    params.version        = 6;
    params.skeletal       = 1;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--tail") == 0 && i + 1 < argc)
        {
            params.tailSlackBytes = (u32)strtoul(argv[++i], NULL, 0);
        }
        else if (strcmp(argv[i], "--prop") == 0)
        {
            params.skeletal = 0;
        }
        else if (strcmp(argv[i], "--v7") == 0)
        {
            params.version = 7;
        }
        else if (strcmp(argv[i], "--bones") == 0 && i + 1 < argc)
        {
            params.anmBoneCount = (s32)strtol(argv[++i], NULL, 0);
        }
        else
        {
            FILE* f = fopen(argv[i], "rb");
            long  size;
            u8*   bytes;
            char  err[160];
            int   rule;

            if (f == NULL)
            {
                printf("FAIL - %s: cannot open\n", argv[i]);
                failures++;
                continue;
            }
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fseek(f, 0, SEEK_SET);
            bytes = (u8*)malloc(size > 0 ? (size_t)size : 1);
            if (bytes == NULL || fread(bytes, 1, (size_t)size, f) != (size_t)size)
            {
                printf("FAIL - %s: cannot read\n", argv[i]);
                failures++;
                free(bytes);
                fclose(f);
                continue;
            }
            fclose(f);

            err[0] = '\0';
            rule   = Lm_Validate(bytes, (u32)size, &params, err, sizeof(err));
            if (rule == 0)
            {
                printf("PASS %s\n", argv[i]);
                passes++;
            }
            else
            {
                printf("FAIL V%d %s: %s\n", rule, argv[i], err);
                failures++;
            }
            free(bytes);
        }
    }

    printf("== %d pass, %d fail (tail=%u skeletal=%d bones=%d)\n",
           passes, failures, params.tailSlackBytes, params.skeletal, params.anmBoneCount);
    return failures;
}
