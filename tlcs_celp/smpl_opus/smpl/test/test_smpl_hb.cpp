#include <gtest/gtest.h>

#include "smpl_bandwidth_extension.h"
#include "smpl_lsf_quant.h"
#include "smpl_codec_util.h"
#include "smpl_defines.h"
#include "smpl_lpc.h"
#include "smpl_errors.h"

TEST(SmplHbCoder, GainDequantZero)
{
    EXPECT_TRUE(smpl_load_hb_gain_CBks() != NULL);
    std::vector<int> num_hb_subframes = { 2, 4 };
	for (auto num : num_hb_subframes)
	{
		int gain_qi = 0;
		float lb_wght_mem[SMPL_LB_WGHT_LEN - 1];
		memset(&lb_wght_mem, 0, sizeof(lb_wght_mem));
		int voiced = SMPL_TRUE;
		int coded_as_active_voice = SMPL_TRUE;
		int low_rate = SMPL_FALSE;
		float y_pre_postfilter[SMPL_FRAME_LEN];
		memset(&y_pre_postfilter, 0, sizeof(y_pre_postfilter));
        float low_nrg_frame = 0.0f;

		float hb_gains[SMPL_MAX_HB_SUBFR];
		smpl_hb_gain_dequant(gain_qi, voiced, low_rate, num, hb_gains, y_pre_postfilter, low_nrg_frame);
	}
    smpl_free_hb_gain_CBks();
}

TEST(SmplHbCoder, GainDequant)
{
    EXPECT_TRUE(smpl_load_hb_gain_CBks() != NULL);
	int gain_qi = 170;
	int num_hb_subframes = 4;
    float lb_wght_mem[SMPL_LB_WGHT_LEN - 1] = { -0.05364123, -0.032051064,
        -0.024522506, -0.039946273, -0.06302048, -0.076956466,
        -0.08587833, -0.09678087, -0.113067314 };
	int voiced = SMPL_TRUE;
	int coded_as_active_voice = SMPL_TRUE;
	int low_rate = SMPL_FALSE;
    float y_pre_postfilter[(SMPL_LB_WGHT_LEN - 1) + SMPL_FRAME_LEN] = {
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -0.12498085, -0.11222966,
        -0.07964834, -0.051240433, -0.01855411, 0.027860062, 0.072726175,
        0.08896175, 0.09197608, 0.10768491, 0.13840094, 0.15630816, 0.1474774,
        0.12545878, 0.117536634, 0.121693015, 0.11672956, 0.101288944,
        0.10476971, 0.13135442, 0.15955523, 0.17450958, 0.17883536, 0.16446039,
        0.13834515, 0.12840581, 0.13179019, 0.10235345, 0.013159692,
        -0.08987275, -0.12589449, -0.084923804, -0.048303574, -0.088048756,
        -0.16378818, -0.1847311, -0.15059537, -0.1226711, -0.12713265,
        -0.13007438, -0.111293584, -0.093036324, -0.08772156, -0.08480632,
        -0.08020216, -0.07616487, -0.07631792, -0.06744376, -0.040415004,
        -0.019869454, -0.031038562, -0.0634615, -0.078218445, -0.06826159,
        -0.06637584, -0.079252765, -0.092451155, -0.08895752, -0.06670034,
        -0.038611263, -0.014453158, 0.007980928, 0.031852297, 0.052324366,
        0.06756905, 0.08238384, 0.10678017, 0.1251888, 0.12430653, 0.110029966,
        0.1067833, 0.11299276, 0.11128193, 0.09977579, 0.09323403, 0.10628274,
        0.13579604, 0.16431746, 0.17731118, 0.16422558, 0.13659267, 0.12261326,
        0.13499558, 0.13619727, 0.056701317, -0.08244064, -0.16602828,
        -0.12366303, -0.035955563, -0.03998473, -0.14348543, -0.22058703,
        -0.19276868, -0.12480305, -0.10418468, -0.11450333, -0.0950146,
        -0.05568531, -0.05015254, -0.074143976, -0.07408774, -0.053519264,
        -0.045549616, -0.053644493, -0.03744757, -0.0022291392, -0.001435766,
        -0.04748323, -0.08641654, -0.06954636, -0.031885445, -0.035198502,
        -0.07398439, -0.0944317, -0.0825894, -0.06962901, -0.06422666,
        -0.045353666, -0.0066934824, 0.03336887, 0.06106255, 0.07156068,
        0.083911166, 0.10597664, 0.11959872, 0.116037875, 0.112274736,
        0.121557266, 0.12129226, 0.10000971, 0.08526516, 0.09905729,
        0.13079399, 0.15571734, 0.16790256, 0.16619134, 0.14767516, 0.12155846,
        0.11715266, 0.11721578, 0.07192099, -0.03821318, -0.13613226,
        -0.13855597, -0.066177905, -0.03487265, -0.10437917, -0.19385728,
        -0.19871944, -0.13598466, -0.10136074, -0.12222788, -0.13801655,
        -0.11653978, -0.089957535, -0.08703597, -0.09391618, -0.08971883,
        -0.08070812, -0.0763946, -0.06182383, -0.031126186, -0.01167385,
        -0.033645876, -0.07393861, -0.07782994, -0.04634051, -0.028993849,
        -0.054442246, -0.084081784, -0.08244899, -0.064888105, -0.054140147,
        -0.041575424, -0.0133764995, 0.022533795, 0.05816182, 0.082560584,
        0.09074469, 0.09928766, 0.118350506, 0.12840939, 0.12282139,
        0.11939156, 0.12464762, 0.115002304, 0.09344882, 0.09147677,
        0.11582652, 0.1442412, 0.16207272, 0.16710871, 0.15221515, 0.121892124,
        0.107234776, 0.11637725, 0.098174974, 0.010508403, -0.104767956,
        -0.13324368, -0.052078724, 0.014705658, -0.04789731, -0.17204104,
        -0.19923621, -0.12362325, -0.070476055, -0.09597646, -0.1287804,
        -0.114335716, -0.08844796, -0.08636862, -0.0934494, -0.088059485,
        -0.07928799, -0.08053857, -0.07253356, -0.037934035, -0.012547739,
        -0.037334777, -0.08860857, -0.09448773, -0.053635746, -0.033452645,
        -0.06622772, -0.10080531, -0.09380959, -0.07092564, -0.06388953,
        -0.051944524, -0.019866869, 0.019593034, 0.05283322, 0.07563086,
        0.090161055, 0.10278964, 0.11621326, 0.12322006, 0.12343907,
        0.12774813, 0.12960899, 0.11479274, 0.09423922, 0.09705213, 0.12271048,
        0.14836578, 0.16351041, 0.17082709, 0.17432071, 0.16586211, 0.158984,
        0.1549475, 0.10692531, -0.0031828638, -0.11772301, -0.13302125,
        -0.04438579, 0.011396706, -0.0622391, -0.19222055, -0.21922997,
        -0.13759494, -0.078498244, -0.10347271, -0.13667232, -0.12036395,
        -0.092376694, -0.09303178, -0.10518044, -0.10292873, -0.09185742,
        -0.0860572, -0.07299788, -0.042811334, -0.0077329203, -0.007949322,
        -0.041676708, -0.057450563, -0.029821485, -0.008412525, -0.028802313,
        -0.062622756, -0.06545998, -0.051091, -0.039600782, -0.031075276,
        -0.0080714375, 0.026347088, 0.06356472, 0.088733986, 0.09568845,
        0.10092521, 0.12081075, 0.13129643, 0.121405005, 0.11236489, 0.1180754,
        0.114436984, 0.09182528, 0.08061382, 0.10217063, 0.1349131, 0.15551051,
        0.15693471, 0.14107063, 0.11307523, 0.10004991, 0.10822891, 0.08999249,
        0.007985994, -0.105977215, -0.13801166, -0.05857742, 0.012375131,
        -0.046398938, -0.16704549, -0.20044512, -0.1265252, -0.067620635,
        -0.08815673 };

    float hb_gains_expected[SMPL_MAX_HB_SUBFR] = { 4.2402266e-7, 6.0526236e-7, 6.02205e-7, 6.2790014e-7 };
    float hb_gains[SMPL_MAX_HB_SUBFR];
    float low_wght_frame_nrg = 0;
    float y_wght[SMPL_FRAME_LEN];
    smpl_hb_wght_lb(lb_wght_mem, num_hb_subframes, y_pre_postfilter + (SMPL_LB_WGHT_LEN - 1), y_wght, &low_wght_frame_nrg);
	smpl_hb_gain_dequant(gain_qi, voiced, low_rate, num_hb_subframes, hb_gains, y_wght, low_wght_frame_nrg);
    for (int i = 0; i < SMPL_MAX_HB_SUBFR; i++)
    {
        EXPECT_NEAR(hb_gains[i], hb_gains_expected[i], 1e-3);
    }
    smpl_free_hb_gain_CBks();
}

TEST(SmplHbCoder, LpcDequant)
{
    EXPECT_TRUE(smpl_load_hb_lsf_CBks() != NULL);
	int qi = 0;
	int voiced = SMPL_TRUE;
	int low_rate = SMPL_FALSE;

	float lsfs[SMPL_HB_LPC_ORDER];
	smpl_hb_lsf_dequant(qi, voiced, low_rate, lsfs);
    smpl_free_hb_lsf_CBks();
}

TEST(SmplHbCoder, SynthesisAllZero)
{
	HbDecoder hb_decoder;
	memset(&hb_decoder, 0, sizeof(hb_decoder));
	float exc[SMPL_FRAME_LEN];
	memset(&exc, 0, sizeof(exc));
	int voiced = SMPL_TRUE;
	int coded_as_active_voice = SMPL_TRUE;
	float nyquist_gain = 0;
	int num_hb_subframes = 4;
	int num_fcb_subframes = 4;
	float hb_gains[SMPL_MAX_HB_SUBFR];
	memset(&hb_gains[0], 0, SMPL_MAX_HB_SUBFR * sizeof(hb_gains[0]));
	float A[SMPL_MAX_N_SUBFR][SMPL_HB_LPC_ORDER + 1];
	memset(&A[0][0], 0, (SMPL_HB_LPC_ORDER + 1) * SMPL_MAX_N_SUBFR * sizeof(A[0][0]));
	for (int i = 0; i < SMPL_MAX_N_SUBFR; i++) {
		A[i][0] = 1;
	}

    float y[SMPL_FRAME_LEN + SMPL_TOT_POSTFILT_DELAY];
    float hb_exc_gains[SMPL_MAX_HB_SUBFR];
    int frame = 0;
    int num_frames = 1;
    int low_rate = SMPL_FALSE;
	smpl_hb_decode(&hb_decoder, exc, voiced, coded_as_active_voice, 
#if defined(SMPL_USE_LPC_POSTFILTER) || defined(SMPL_USE_TILT_POSTFILTER)
	                        nyquist_gain,
#endif
                            num_hb_subframes, num_fcb_subframes, hb_gains, &A[0][0],
                            frame, num_frames, y + SMPL_TOT_POSTFILT_DELAY, hb_exc_gains, low_rate);
}

TEST(SmplHbCoder, Synthesis)
{
    for (int i = 1; i <= 2; i++)
    {
        for (int activity = 0; activity <= 1; activity++)
        {
            HbDecoder hb_decoder;
            memset(&hb_decoder, 0, sizeof(hb_decoder));
            int coded_as_active_voice = activity;
            float exc[SMPL_FRAME_LEN] = { 1.0, 1.0, 1.0, 1.0, 1.0, -1.0, 1.0, 1.0,
                1.0, 1.0, -1.0, 1.0, 1.0, -1.0, -1.0, -1.0, -1.0, 1.0, -1.0, -1.0,
                1.0, 1.0, 1.0, -1.0, -1.0, 1.0, 1.0, -1.0, 1.0, 1.0, -1.0, -1.0,
                -1.0, 1.0, 1.0, -1.0, -1.0, -1.0, 1.0, -1.0, -1.0, 1.0, 1.0, 1.0,
                1.0, -1.0, -1.0, -1.0, -1.0, -1.0, 1.0, 1.0, -1.0, -1.0, 1.0, 1.0,
                1.0, 1.0, 1.0, -1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, -1.0,
                -1.0, -1.0, 1.0, 1.0, -1.0, -1.0, -1.0, -1.0, 1.0, -1.0, 1.0, -1.0,
                1.0, -1.0, 1.0, -1.0, -1.0, -1.0, 1.0, -1.0, -1.0, -1.0, 1.0, 1.0,
                -1.0, -1.0, 1.0, -1.0, 1.0, 1.0, -1.0, 1.0, 1.0, -1.0, 1.0, 1.0,
                -1.0, 1.0, -1.0, 1.0, -1.0, -1.0, -1.0, 1.0, -1.0, -1.0, 1.0, 1.0,
                1.0, 1.0, -1.0, -1.0, 1.0, -1.0, -1.0, 1.0, 1.0, -1.0, 1.0, -1.0,
                1.0, 1.0, 1.0, 1.0, -1.0, 1.0, -1.0, 1.0, -1.0, -1.0, 1.0, 1.0,
                -1.0, 1.0, 1.0, -1.0, -1.0, 1.0, -1.0, 1.0, -1.0, -1.0, -1.0, -1.0,
                1.0, 1.0, 1.0, -1.0, 1.0, 1.0, -1.0, 1.0, -1.0, -1.0, -1.0, -1.0,
                -1.0, -1.0, -1.0, -1.0, 1.0, 1.0, -1.0, -1.0, 1.0, 1.0, -1.0, -1.0,
                -1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, -1.0, 1.0, 1.0, 1.0,
                1.0, 1.0, -1.0, -1.0, -1.0, 1.0, 1.0, -1.0, -1.0, 1.0, -1.0, -1.0,
                -1.0, 1.0, -1.0, -1.0, -1.0, 1.0, 1.0, 1.0, 1.0, -1.0, 1.0, -1.0,
                -1.0, -1.0, -1.0, 1.0, -1.0, -1.0, 1.0, 1.0, -1.0, -1.0, -1.0,
                -1.0, -1.0, -1.0, -1.0, -1.0, 1.0, 1.0, -1.0, 1.0, 1.0, -1.0, 1.0,
                -1.0, 1.0, -1.0, -1.0, 1.0, 1.0, 1.0, -1.0, 1.0, 1.0, 1.0, -1.0,
                -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, 1.0, 1.0, -1.0, 1.0, 1.0, -1.0,
                1.0, -1.0, 1.0, -1.0, 1.0, 1.0, -1.0, 1.0, 1.0, -1.0, -1.0, -1.0,
                -1.0, -1.0, -1.0, 1.0, 1.0, -1.0, -1.0, -1.0, 1.0, 1.0, -1.0, 1.0,
                1.0, -1.0, 1.0, -1.0, -1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
                1.0, 1.0, -1.0, 1.0, -1.0, -1.0, 1.0, 1.0, 1.0, -1.0, 1.0, -1.0,
                -1.0, -1.0, 1.0, 1.0, 1.0, 1.0, 1.0, -1.0, -1.0, -1.0 };
            for (int ii = 0; ii < SMPL_FRAME_LEN; ii++)
            {
                exc[i] *= sin(ii);
            }
            int num_hb_subframes = 4;
            float hb_gains[SMPL_MAX_HB_SUBFR] = { 1, 2, 3, 4 };
            float A[SMPL_MAX_N_SUBFR][SMPL_HB_LPC_ORDER + 1];
            memset(&A[0][0], 0, (SMPL_HB_LPC_ORDER + 1) * SMPL_MAX_N_SUBFR * sizeof(A[0][0]));
            float a[SMPL_MAX_N_SUBFR] = { -0.7, -0.3, 0.3, 0.7 };
            for (int j = 0; j < SMPL_MAX_N_SUBFR; j++) {
                A[j][0] = 1;
                A[j][1] = a[j];
            }

            float y[SMPL_FRAME_LEN + SMPL_TOT_POSTFILT_DELAY];
            float hb_exc_gains[SMPL_MAX_HB_SUBFR];
            int frame = 0;
            int num_frames = 1;
            float nyquist_gain = 1.0f;
            int low_rate = SMPL_FALSE;
            smpl_hb_decode(&hb_decoder, exc, SMPL_TRUE, coded_as_active_voice,
#if defined(SMPL_USE_LPC_POSTFILTER) || defined(SMPL_USE_TILT_POSTFILTER)
	                                nyquist_gain,
#endif
                                    num_hb_subframes, 2 * i, hb_gains,
                                    &A[0][0], frame, num_frames, y + SMPL_TOT_POSTFILT_DELAY, hb_exc_gains, low_rate);

            const float* y_ = y + SMPL_TOT_POSTFILT_DELAY;
            for (int j = 0; j < SMPL_MAX_HB_SUBFR; j++)
            {
                // check output energy is in reasonable range
                EXPECT_GT(smpl_nrg(y_, SMPL_HB_SF_LEN), 1e2f);
                EXPECT_LT(smpl_nrg(y_, SMPL_HB_SF_LEN), 1e4f);
                y_ += SMPL_HB_SF_LEN;
            }
        }
    }
}

TEST(SmplHbCoder, AnalysisAllZero)
{
    void* pSt = smpl_create_hb_encoder();
    EXPECT_TRUE(smpl_create_lpc_tables() != nullptr);
    EXPECT_TRUE(smpl_create_lpc_windows() == SMPL_NO_ERROR);
    EXPECT_TRUE(smpl_load_hb_lsf_CBks() != NULL);
    EXPECT_TRUE(smpl_load_hb_gain_CBks() != NULL);

    HbQuantParams hb_params;
    memset(&hb_params, 0, sizeof(hb_params));

    float x_16_lb[SMPL_FRAME_LEN + 16];
    memset(x_16_lb, 0, sizeof(x_16_lb));
    float x_16_hb[SMPL_FRAME_LEN + 16];
    memset(x_16_hb, 0, sizeof(x_16_hb));

    smpl_hb_encoder(pSt, &hb_params, SMPL_TRUE, SMPL_FALSE, SMPL_FALSE, x_16_lb + 16, x_16_hb + 16, 4, 10000);

    smpl_free_hb_encoder(pSt);
    smpl_free_lpc_tables();
    smpl_free_lpc_windows();
    smpl_free_hb_lsf_CBks();
    smpl_free_hb_gain_CBks();
}
