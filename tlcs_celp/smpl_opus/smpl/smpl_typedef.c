#include "smpl_typedef.h"

#if defined(ENABLE_ASSERTIONS) || defined(ENABLE_HARDENING)
#include <stdio.h>
#ifdef __GNUC__
__attribute__((noreturn))
#endif
void smpl_fatal(const char *str, const char *file, int line)
{
   fprintf (stderr, "Fatal (internal) error in %s, line %d: %s\n", file, line, str);
#if defined(_MSC_VER)
   _set_abort_behavior( 0, _WRITE_ABORT_MSG);
#endif
   abort();
}

#endif
