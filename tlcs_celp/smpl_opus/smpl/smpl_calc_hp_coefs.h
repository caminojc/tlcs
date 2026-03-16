#ifndef SMPL_CALC_HP_COEFS_H
#define SMPL_CALC_HP_COEFS_H

#ifdef __cplusplus
extern "C" {
#endif

void smpl_calc_hp_coefs(float maf, const float arf[2], const float arr[2], float f, float coef_ma[3], float coef_ar[3]);
void smpl_get_hp_coefs(float fcorner_3dB_Hz, float coef_ma[3], float coef_ar[3]);

#ifdef __cplusplus
}
#endif

#endif
