/* SCOREQ-optimized TCX tuning (2026-03-15, full-corpus Phase 2 @ 9.6k)
 * Runtime-overridable via environment variables:
 *   export TUNE_NF_CODED=0.12  (before running encoder/decoder)
 * If env var is not set, falls back to compile-time default. */
#ifndef TLCS_TUNE_H
#define TLCS_TUNE_H

#include <stdlib.h>

static inline float tune_get_(const char *env_name, float default_val) {
    const char *s = getenv(env_name);
    if (s) return (float)atof(s);
    return default_val;
}

/* Each parameter: cached read from env on first call */
static inline float tune_alfe_boost(void)       { static float v=-999; if(v<-900) v=tune_get_("TUNE_ALFE_BOOST",       2.3941f); return v; }
static inline float tune_sbr_base_mult(void)    { static float v=-999; if(v<-900) v=tune_get_("TUNE_SBR_BASE_MULT",    0.9350f); return v; }
static inline float tune_sbr_depth_frac(void)   { static float v=-999; if(v<-900) v=tune_get_("TUNE_SBR_DEPTH_FRAC",   0.7803f); return v; }
static inline float tune_nf_coded(void)         { static float v=-999; if(v<-900) v=tune_get_("TUNE_NF_CODED",         0.0809f); return v; }
static inline float tune_nf_uncoded_hi(void)    { static float v=-999; if(v<-900) v=tune_get_("TUNE_NF_UNCODED_HI",    0.6322f); return v; }
static inline float tune_nf_uncoded_slope(void) { static float v=-999; if(v<-900) v=tune_get_("TUNE_NF_UNCODED_SLOPE", 0.0357f); return v; }
static inline float tune_pf_num_lr(void)        { static float v=-999; if(v<-900) v=tune_get_("TUNE_PF_NUM_LR",        0.7885f); return v; }
static inline float tune_pf_den_lr(void)        { static float v=-999; if(v<-900) v=tune_get_("TUNE_PF_DEN_LR",        0.8701f); return v; }
static inline float tune_pf_tilt_lr(void)       { static float v=-999; if(v<-900) v=tune_get_("TUNE_PF_TILT_LR",       0.0379f); return v; }
static inline float tune_dz_offset(void)        { static float v=-999; if(v<-900) v=tune_get_("TUNE_DZ_OFFSET",        0.5000f); return v; }

/* CELP-specific decoder params */
static inline float tune_celp_harm_str(void)    { static float v=-999; if(v<-900) v=tune_get_("TUNE_CELP_HARM_STR",    0.1112f); return v; }
static inline float tune_celp_pf_num(void)      { static float v=-999; if(v<-900) v=tune_get_("TUNE_CELP_PF_NUM",      0.8491f); return v; }
static inline float tune_celp_pf_den(void)      { static float v=-999; if(v<-900) v=tune_get_("TUNE_CELP_PF_DEN",      0.9313f); return v; }
static inline float tune_celp_pf_tilt(void)     { static float v=-999; if(v<-900) v=tune_get_("TUNE_CELP_PF_TILT",     0.1438f); return v; }
static inline float tune_celp_nf_voiced(void)   { static float v=-999; if(v<-900) v=tune_get_("TUNE_CELP_NF_VOICED",   0.0777f); return v; }
static inline float tune_celp_nf_unvoiced(void) { static float v=-999; if(v<-900) v=tune_get_("TUNE_CELP_NF_UNVOICED", 0.2673f); return v; }
static inline float tune_celp_gamma1(void)      { static float v=-999; if(v<-900) v=tune_get_("TUNE_CELP_GAMMA1",      0.9700f); return v; }
static inline float tune_celp_gamma2(void)      { static float v=-999; if(v<-900) v=tune_get_("TUNE_CELP_GAMMA2",      0.4800f); return v; }

/* Drop-in macro replacements */
#define TUNE_ALFE_BOOST       tune_alfe_boost()
#define TUNE_SBR_BASE_MULT    tune_sbr_base_mult()
#define TUNE_SBR_DEPTH_FRAC   tune_sbr_depth_frac()
#define TUNE_NF_CODED         tune_nf_coded()
#define TUNE_NF_UNCODED_HI    tune_nf_uncoded_hi()
#define TUNE_NF_UNCODED_SLOPE tune_nf_uncoded_slope()
#define TUNE_PF_NUM_LR        tune_pf_num_lr()
#define TUNE_PF_DEN_LR        tune_pf_den_lr()
#define TUNE_PF_TILT_LR       tune_pf_tilt_lr()
#define TUNE_DZ_OFFSET        tune_dz_offset()
#define TUNE_CELP_HARM_STR    tune_celp_harm_str()
#define TUNE_CELP_PF_NUM      tune_celp_pf_num()
#define TUNE_CELP_PF_DEN      tune_celp_pf_den()
#define TUNE_CELP_PF_TILT     tune_celp_pf_tilt()
#define TUNE_CELP_NF_VOICED   tune_celp_nf_voiced()
#define TUNE_CELP_NF_UNVOICED tune_celp_nf_unvoiced()
#define TUNE_CELP_GAMMA1      tune_celp_gamma1()
#define TUNE_CELP_GAMMA2      tune_celp_gamma2()

#endif
