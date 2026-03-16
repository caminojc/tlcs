#ifndef SMPL_TYPEDEF_H
#define SMPL_TYPEDEF_H

#include "opus_defines.h"
#include <stdlib.h>

#if defined(ENABLE_ASSERTIONS) || defined(ENABLE_HARDENING)
#ifdef __cplusplus
extern "C" {
#endif
#ifdef __GNUC__
__attribute__((noreturn))
#endif
void smpl_fatal(const char *str, const char *file, int line);
#ifdef __cplusplus
}
#endif

#define SMPL_FATAL(str) smpl_fatal(str, __FILE__, __LINE__);
#define smpl_assert(cond) {if (!(cond)) {SMPL_FATAL("assertion failed: " #cond);}}

#else
#define smpl_assert(cond)
#endif

#endif
