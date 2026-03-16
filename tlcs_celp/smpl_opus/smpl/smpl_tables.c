#include "smpl_tables.h" 

const int16_t smpl_cb_acbgains_lr_Q14[SMPL_ACBG_N*SMPL_ACBG_M] = {
    2812, 2484, 
    0, 0, 
    -362, 2465, 
    -337, 703, 
    3033, 1474, 
    13536, 220, 
    -2630, 9226, 
    6032, 3499, 
    -220, 441, 
    7661, 4243, 
    11521, 0, 
    1430, 779, 
    4495, 2724, 
    15535, 343, 
    -779, 1559, 
    480, 481
};

const uint8_t smpl_acbgains_dcmf_lr[(SMPL_ACBG_N+1)*(SMPL_ACBG_N)] = {
    103u, 70u, 48u, 3u, 122u, 135u, 47u, 192u, 2u, 255u, 99u, 96u, 186u, 194u, 4u, 28u, 
    161u, 90u, 76u, 3u, 181u, 60u, 37u, 219u, 2u, 132u, 81u, 146u, 255u, 43u, 3u, 36u, 
    114u, 222u, 55u, 6u, 203u, 34u, 42u, 154u, 6u, 255u, 33u, 209u, 225u, 78u, 6u, 45u, 
    198u, 161u, 110u, 8u, 239u, 26u, 35u, 162u, 4u, 117u, 42u, 214u, 255u, 33u, 6u, 72u, 
    55u, 255u, 124u, 55u, 124u, 55u, 55u, 55u, 55u, 78u, 55u, 215u, 111u, 55u, 55u, 167u, 
    154u, 136u, 77u, 4u, 220u, 33u, 38u, 166u, 2u, 144u, 50u, 196u, 255u, 43u, 4u, 41u, 
    56u, 21u, 19u, 3u, 48u, 255u, 38u, 220u, 2u, 225u, 107u, 31u, 122u, 227u, 2u, 11u, 
    63u, 38u, 23u, 4u, 77u, 85u, 58u, 190u, 4u, 255u, 53u, 53u, 145u, 138u, 4u, 14u, 
    95u, 47u, 33u, 2u, 110u, 146u, 53u, 255u, 2u, 219u, 79u, 73u, 198u, 122u, 2u, 15u, 
    84u, 255u, 84u, 84u, 147u, 84u, 84u, 84u, 84u, 120u, 84u, 120u, 84u, 84u, 84u, 84u, 
    73u, 58u, 25u, 1u, 95u, 99u, 52u, 175u, 1u, 255u, 48u, 69u, 151u, 184u, 1u, 15u, 
    105u, 32u, 43u, 2u, 84u, 225u, 34u, 255u, 2u, 156u, 129u, 49u, 189u, 124u, 3u, 19u, 
    152u, 230u, 89u, 6u, 253u, 28u, 40u, 153u, 2u, 195u, 31u, 255u, 249u, 58u, 5u, 61u, 
    138u, 84u, 54u, 3u, 173u, 96u, 45u, 247u, 2u, 176u, 83u, 128u, 255u, 69u, 2u, 26u, 
    22u, 17u, 8u, 1u, 23u, 106u, 26u, 88u, 1u, 182u, 37u, 18u, 50u, 255u, 1u, 6u, 
    218u, 174u, 228u, 65u, 186u, 65u, 65u, 92u, 65u, 65u, 65u, 255u, 174u, 65u, 65u, 174u, 
    117u, 255u, 101u, 16u, 180u, 20u, 33u, 94u, 10u, 131u, 20u, 222u, 143u, 38u, 15u, 105u
};

const int16_t smpl_cb_acbgains_hr_Q14[SMPL_ACBG_N*SMPL_ACBG_M] = {
    16039, 91, 
    0, 0, 
    4310, 4930, 
    -1431, 2862, 
    2893, 0, 
    8009, 4075, 
    2754, 4223, 
    8367, 354, 
    4640, 1254, 
    -176, 2734, 
    -1222, 5017, 
    -476, 1506, 
    11351, 567, 
    1243, 0, 
    10601, 22, 
    14088, 108
};

const uint8_t smpl_acbgains_dcmf_hr[(SMPL_ACBG_N+1)*(SMPL_ACBG_N)] = {
    254u, 105u, 212u, 26u, 110u, 255u, 202u, 93u, 152u, 121u, 110u, 43u, 150u, 20u, 81u, 176u, 
    255u, 28u, 100u, 5u, 26u, 184u, 61u, 29u, 36u, 26u, 28u, 9u, 61u, 4u, 27u, 116u, 
    121u, 255u, 161u, 39u, 195u, 215u, 191u, 75u, 186u, 178u, 119u, 82u, 68u, 41u, 43u, 56u, 
    188u, 65u, 243u, 15u, 74u, 255u, 205u, 79u, 123u, 84u, 95u, 26u, 139u, 13u, 67u, 154u, 
    81u, 219u, 173u, 70u, 219u, 165u, 234u, 102u, 231u, 255u, 191u, 119u, 87u, 60u, 62u, 59u, 
    106u, 255u, 182u, 49u, 242u, 196u, 233u, 95u, 247u, 228u, 152u, 96u, 81u, 45u, 54u, 61u, 
    236u, 55u, 178u, 10u, 56u, 255u, 131u, 54u, 85u, 58u, 59u, 18u, 93u, 9u, 43u, 133u, 
    123u, 95u, 224u, 24u, 113u, 202u, 255u, 105u, 186u, 134u, 135u, 38u, 141u, 18u, 82u, 111u, 
    126u, 97u, 204u, 34u, 126u, 186u, 255u, 141u, 210u, 147u, 149u, 46u, 165u, 22u, 113u, 122u, 
    96u, 156u, 185u, 42u, 188u, 178u, 255u, 116u, 248u, 199u, 157u, 66u, 109u, 29u, 69u, 75u, 
    102u, 207u, 194u, 57u, 224u, 193u, 255u, 107u, 253u, 242u, 180u, 95u, 97u, 44u, 60u, 64u, 
    105u, 119u, 202u, 39u, 140u, 189u, 255u, 110u, 207u, 173u, 165u, 54u, 119u, 24u, 75u, 85u, 
    74u, 255u, 142u, 59u, 214u, 150u, 182u, 76u, 194u, 215u, 138u, 122u, 61u, 56u, 41u, 45u, 
    200u, 53u, 255u, 17u, 66u, 238u, 222u, 109u, 129u, 78u, 101u, 21u, 227u, 11u, 110u, 243u, 
    74u, 255u, 128u, 50u, 187u, 149u, 154u, 63u, 165u, 184u, 115u, 101u, 52u, 47u, 37u, 34u, 
    159u, 66u, 232u, 26u, 86u, 196u, 255u, 146u, 171u, 113u, 134u, 31u, 245u, 16u, 145u, 190u, 
    255u, 29u, 182u, 7u, 33u, 235u, 115u, 55u, 59u, 37u, 47u, 11u, 139u, 6u, 60u, 234u
};

const uint8_t smpl_fcbg_v_dcmf[SMPL_FCBG_V_N] = 
{
    107u, 12u, 17u, 25u, 31u, 41u, 52u, 65u, 83u, 103u, 122u, 146u, 169u, 191u, 210u, 227u, 
    240u, 249u, 255u, 253u, 246u, 229u, 200u, 161u, 120u, 82u, 51u, 29u, 14u, 6u, 2u, 2u, 
    2u, 2u
};

const uint8_t smpl_fcbg_v_delta_dcmf[SMPL_FCBG_V_DELTA_N] = 
{
    1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 2u, 3u, 4u, 6u, 8u, 10u, 12u, 12u, 
    12u, 13u, 14u, 14u, 14u, 13u, 12u, 11u, 10u, 9u, 8u, 9u, 15u, 33u, 65u, 119u, 
    196u, 255u, 220u, 144u, 90u, 57u, 36u, 23u, 17u, 14u, 12u, 12u, 12u, 13u, 12u, 12u, 
    12u, 12u, 12u, 11u, 11u, 10u, 9u, 7u, 6u, 4u, 3u, 2u, 1u, 1u, 1u, 1u, 
    1u, 1u, 1u
};

const float smpl_interpol_kernel[2*SMPL_LTP_INTERPOL_DELAY] = 
{
    -6.3925986e-6f, 0.00011064114f, -0.0009153038f, 0.00484772f, -0.018698348f, 0.05759091f, -0.15997477f, 0.6170455f, 
    0.61704546f, -0.15997475f, 0.057590906f, -0.018698348f, 0.00484772f, -0.0009153038f, 0.000110641144f, -6.392598e-6f
};

const float smpl_plc_inject_coef[2] = {0.5f, -0.5f};
const float smpl_cng_emph_coef[2] = {1.0f, -0.9f};

const uint16_t smpl_lsf_interp_cmf[3] = {0, 5, 7};
// HP filter with cutoff at 50Hz
//const float smpl_hp_a2[SMPL_HP_A_LEN] = {1.0f, -1.97362f, 0.9740658f};
//const float smpl_hp_b2[SMPL_HP_A_LEN] = {0.986927f, -1.9738388f, 0.986927f};
const float smpl_filterbankL_coef[SMPL_FILTERBANK_A_LEN] = {1.0f, 0.60797656f, 0.036630828f};
const float smpl_filterbankH_coef[SMPL_FILTERBANK_A_LEN] = {1.0f, 1.1034178f, 0.2197291f};
const float smpl_ap_coefs_32_48[SMPL_AP_LEN_32_48] = {0.122f, 0.5579f};
const float smpl_fir_coefs_32_48[SMPL_FIR_M_32_48][SMPL_FIR_N_32_48] = {
    {0.0163442f, -0.0791814f, 0.121864f, -0.0224717f, -0.202269f, 1.06805f, 0.214212f, -0.251631f, 0.202548f, -0.0721714f, -0.00228103f, 0.00556359f }, 
    {0.012887f, -0.040294f, 0.00175059f, 0.161718f, -0.358857f, 0.722276f, 0.722276f, -0.358857f, 0.161718f, 0.00175059f, -0.040294f, 0.012887f }, 
    {0.00556359f, -0.00228103f, -0.0721714f, 0.202548f, -0.251631f, 0.214212f, 1.06805f, -0.202269f, -0.0224717f, 0.121864f, -0.0791814f, 0.0163442f }
}; 
const float smpl_hb_wght_coef[SMPL_HB_WGHT_LEN] = {-0.12f, 0.38f, -0.38f, 0.12f};
const float smpl_hb_post_coef[SMPL_HB_POST_LEN] = {0.95f, 0.05f};
const float smpl_lb_wght_coef[SMPL_LB_WGHT_LEN] = {-0.01026285f, 0.08698435f, 0.08000515f, -0.85401285f, 1.6944792f, -1.6944792f, 0.85401285f, -0.08000515f, -0.08698435f, 0.01026285f};
const float smpl_perc_emph_pitch = -0.82f;
const float smpl_perc_emph_v[ 2] = {-0.72f, -0.77f};
const float smpl_perc_emph_uv[2] = {-0.55f, -0.6f};

const float smpl_lsf_interpol_1 = 0.95f;
const float smpl_lsf_interpol_2[2][2] = {
    {0.75f, 1.0f},
    {0.4f, 0.95f}
};
const float smpl_lsf_interpol_4[2][4] = {
    {0.55f, 0.88f, 1.0f, 1.0f},
    {0.3f, 0.65f, 0.95f, 1.0f}
};

const float smpl_lsf_interpol_dtx_1 = 0.25f;
const float smpl_lsf_interpol_dtx_2[2] = {0.15f, 0.3f};
const float smpl_lsf_interpol_dtx_4[4] = {0.1f, 0.157f, 0.2f, 0.3f};

const float smpl_post_tilt_coefs[2][2] = {{1.0f, 0.0f},     // highRate
                                          {0.84f, 0.16f}};   // lowRate

const float smpl_uv_pulse_shaping_coefs[2][2][2] = {
    {{1.0f, 0.0f}, {1.0f, 0.0f}},     // highRate
    {{0.5f, 0.1665f}, {1.0f, -0.333f}}};  // lowRate

// please refer to the comment for the field `low_rate_thr` in config.jl (for the Julia version of the codec) if you manually adjust these values to find the valid ranges for these parameters
const float smpl_low_rate_thr[2][4] = {
    {12000, 10200, 8700, 8700},
    {12500, 10700, 9200, 9200}
};
const uint8_t smpl_max_pulses_per_frame[2][3] = {  // [lowRate][BACKGROUND_NOISE/UNVOICED/VOICED]
    {80, 160, 160}, {16, 32, 32}
};
const int smpl_fcb_tot_surv_20ms_max[2] = {100, 100};
const float smpl_vuv_weights[6] = {1.0f, 0.5f, 0.5f, 0.7f, 0.3f}; // weights on : corrs, vad, tilt, harmonicity, short lags
const uint16_t smpl_vuv_cmfs[3][3] = {
    {0, 5, 17}, // unconditional
    {0, 7,  9}, // prev_voiced = false
    {0, 1, 10}  // prev_voiced = true
};
const float smpl_rate_control_model_comp5[4][2][8] = { //[framelenidx][lowrate][]
    {{5.166876656946171f, -8.981699804753452f, 0.07280811614105594f, 0.1301196310618402f, -0.01597680442864421f, 1.7601470147884113f, -3.8161195433141755f, 0.3038629198331684f},
    {-71.71229978402292f, 14.197572549553076f, -0.9863630205846172f, 0.032124893286072924f, -0.0003538411576874928f, 1.803705259861388e-11f, 10.0f, 1.2454667523627154f},
    },
    {{32.5371190670542f, -41.270234279452104f, 10.490270829170875f, -1.102121269442237f, 0.03848319274046071f, 3.405326741403831f, -5.102658181889428f, 0.2141935195026695f},
    {-177.10486363500775f, 43.952329593498376f, -3.7049735533247454f, 0.14239771116996938f, -0.001919963993993193f, 7.953695588409639e-6f, 5.220317075476664f, 0.6435364076926223f},
    },
    {{-79.2663194911617f, 45.00981883522089f, -10.063311543498518f, 1.2311531056576501f, -0.06023559069137118f, 0.059204788212259364f, 3.033961466462233f, 1.0111383197827808f},
    {-122.04861900525415f, 31.62096398905459f, -2.613237037423586f, 0.10050433143234094f, -0.0013233009240188039f, 2.14859438836692e-7f, 1.9077791307787761f, 0.7059420500333776f},
    },
    {{-182.64255084224325f, 122.90780796179816f, -31.308790671748525f, 3.7850563849431462f, -0.1750480676903051f, 0.05399618467364628f, 3.009451055091342f, 1.1243365512229038f},
    {-132.4565456943888f, 34.361297004632966f, -2.7956546289118887f, 0.10428149547078584f, -0.001322667891395693f, 2.678747426340249e-6f, 6.9940208056381925f, 0.7551244069345737f},
    }};
const uint16_t smpl_rate_control_thrs_comp5[4][2] = {
    {7500u, 10000u }, 
    {4500u, 5750u }, 
    {4000u, 5000u }, 
    {4000u, 4750u }
}; 
const float smpl_plc_cng_init[SMPL_LPC_ORDER] = {
    0.065961175f, 0.21926339f, 0.40487507f, 0.59738964f, 
    0.7911506f, 0.98644555f, 1.1819322f, 1.3775148f, 
    1.573289f, 1.7692552f, 1.9650295f, 2.1610913f, 
    2.357345f, 2.5532153f, 2.7495646f, 2.94601f
};
