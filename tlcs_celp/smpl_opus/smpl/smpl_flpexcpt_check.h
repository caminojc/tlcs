#ifndef SMPL_FLPEXCEPT_CHECK_H
#define SMPL_FLPEXCEPT_CHECK_H

//#define ENABLE_FLP_CHECK // Uncomment to enable
#ifdef ENABLE_FLP_CHECK
#include <fenv.h>
#include "smpl_defines.h"
#define CLEAR_FLP_CHECK() feclearexcept(FE_ALL_EXCEPT)
#define FLP_CHECK_ALL() {int excpt_reg = fetestexcept((FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW)); if(excpt_reg != 0){printf("%s(%d) exception: %d \n", __FUNCTION__, __LINE__, excpt_reg); smpl_assert(0)}}
#define FLP_CHECK() {int excpt_reg = fetestexcept((FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW )); if(excpt_reg != 0){printf("%s(%d) exception: %d \n", __FUNCTION__, __LINE__, excpt_reg); smpl_assert(0)}}
#else
#define CLEAR_FLP_CHECK()
#define FLP_CHECK_ALL()
#define FLP_CHECK()
#endif

#endif
