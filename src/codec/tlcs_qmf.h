#ifndef TLCS_QMF_H
#define TLCS_QMF_H

#include "tlcs/tlcs_types.h"

#define TLCS_QMF_TAPS    24
#define TLCS_QMF_HALF    (TLCS_QMF_TAPS / 2)  /* 12 */

/* Band-split LB parameters (8kHz) */
#define TLCS_LB_ORDER       10
#define TLCS_LB_FRAME       80
#define TLCS_LB_SUBFR       40
#define TLCS_LB_NSUBFR       2
#define TLCS_LB_MIN_LAG     20
#define TLCS_LB_MAX_LAG    150
#define TLCS_LB_NUM_PULSES  10

/* HB envelope coding parameters */
#define TLCS_HB_ORDER        6
#define TLCS_HB_LSF_BITS     4
#define TLCS_HB_ENERGY_BITS  5

/* ── QMF filter bank ──────────────────────────────────────────── */

void tlcs_qmf_init(void);

void tlcs_qmf_analyze(float *ana_mem,
                       const float *in, int32_t n_in,
                       float *lb, float *hb);

void tlcs_qmf_synthesize(float *lb_mem, float *hb_mem,
                          const float *lb, const float *hb,
                          float *out, int32_t n_sub);

#endif /* TLCS_QMF_H */
