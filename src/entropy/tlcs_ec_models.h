#ifndef TLCS_EC_MODELS_H
#define TLCS_EC_MODELS_H

#include <stdint.h>

/* ── Static CDF tables for entropy-coded LR mode ────────────────
 *
 * Each table is uint16_t cdf[N+1] where cdf[0]=0, cdf[N]=16384.
 * Used with the range coder for TLCS low-rate frame packing.
 */

/* LSF delta: 32 symbols (5-bit range), trained */
extern const uint16_t ec_cdf_lsf_delta[33];

/* LSF delta: 64 symbols (6-bit range), trained */
extern const uint16_t ec_cdf_lsf_delta_6bit[65];

/* LSF delta: 128 symbols (7-bit range), Laplacian lambda=0.15 */
extern const uint16_t ec_cdf_lsf_delta_7bit[129];

/* Pitch delta: 32 symbols (5-bit range), trained */
extern const uint16_t ec_cdf_pitch_delta[33];

/* Pitch delta: 64 symbols (6-bit range), Laplacian lambda=0.25 */
extern const uint16_t ec_cdf_pitch_delta_6bit[65];

/* Pitch delta: 128 symbols (7-bit range), Laplacian lambda=0.15 */
extern const uint16_t ec_cdf_pitch_delta_7bit[129];

/* Absolute pitch lag: 512 symbols (9-bit range), near-uniform */
extern const uint16_t ec_cdf_pitch_abs[513];

/* ACB VQ index: 8 symbols (3-bit), empirical distribution */
extern const uint16_t ec_cdf_acb_vq[9];

/* FCB gain delta: 32 symbols (5-bit range), Laplacian lambda=0.40 */
extern const uint16_t ec_cdf_fcb_gain[33];

/* FCB gain delta: 64 symbols (6-bit range), Laplacian lambda=0.35 */
extern const uint16_t ec_cdf_fcb_gain_6bit[65];

/* FCB gain delta: 128 symbols (7-bit range), Laplacian lambda=0.12 */
extern const uint16_t ec_cdf_fcb_gain_7bit[129];

/* Pulse position: 40 symbols, uniform (2 pulses, 80/2=40 positions/track) */
extern const uint16_t ec_cdf_pulse_pos[41];

/* Pulse position: 10 symbols, trained (8 pulses, 80/8=10 positions/track) */
extern const uint16_t ec_cdf_pulse_pos_10[11];

/* Pulse sign: 2 symbols, uniform */
extern const uint16_t ec_cdf_pulse_sign[3];

/* Voiced/unvoiced flag: 2 symbols (0=unvoiced, 1=voiced) */
extern const uint16_t ec_cdf_vuv[3];

/* Number of symbols for each model */
#define EC_N_LSF_DELTA    32
#define EC_N_LSF_DELTA_6  64
#define EC_N_LSF_DELTA_7 128
#define EC_N_PITCH_DELTA    32
#define EC_N_PITCH_DELTA_6  64
#define EC_N_PITCH_DELTA_7 128
#define EC_N_PITCH_ABS    512
#define EC_N_ACB_VQ         8
#define EC_N_FCB_GAIN      32
#define EC_N_FCB_GAIN_6    64
#define EC_N_FCB_GAIN_7   128
#define EC_N_PULSE_POS     40
#define EC_N_PULSE_SIGN     2
#define EC_N_VUV            2

#endif /* TLCS_EC_MODELS_H */
