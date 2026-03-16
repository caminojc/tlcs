#include "smpl_defines.h"
#include "smpl_typedef.h"

static inline float cos_approx(float x) {
    return 1.0f - 0.5f * x * x;
}

void smpl_calc_hp_coefs(float maf, const float arf[2], const float arr[2], float f, float coef_ma[3], float coef_ar[3])
{
    coef_ma[0] = 1.0f;
    coef_ma[1] = -2.0f * cos_approx(2.0f * SMPL_PI * maf * f);
    coef_ma[2] = 1.0f;
    const float far_ = arf[0] * f + arf[1] * f * f;
    const float rar_ = arr[0] * f + arr[1] * f * f;
    coef_ar[0] = 1.0f;
    coef_ar[1] = -2.0f * cos_approx(2.0f * SMPL_PI * far_) * (1.0f + rar_);
    coef_ar[2] = 1.0f + (2.0f * rar_ + rar_ * rar_);
    const float sc = (1.0f - coef_ar[1] + coef_ar[2]) / (1.0f - coef_ma[1] + coef_ma[2]);
    for (int i = 0; i < 3; i++) {
        coef_ma[i] *= sc;
    }
}

void smpl_get_hp_coefs(float fcorner_3dB_Hz, float coef_ma[3], float coef_ar[3]) {
    fcorner_3dB_Hz = SMPL_min(SMPL_max(fcorner_3dB_Hz, 5.0f), 1500.0f);
    const float maf = 0.1f;
    const float arf[2] = { 0.728508218f, 0.476039848f};
    const float arr[2] = {-4.363803713f, 8.441854006f};
    const float f = fcorner_3dB_Hz / 16000.0f;
    smpl_calc_hp_coefs(maf, arf, arr, f, coef_ma, coef_ar);
}
