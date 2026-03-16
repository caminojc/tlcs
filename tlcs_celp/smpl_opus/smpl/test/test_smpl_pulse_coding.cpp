#include <gtest/gtest.h>
#include "smpl_pulse_coding.h"
#include "smpl_errors.h"
#include "memory.h"
#include <math.h>
#include <fenv.h>
#include "stop_watch.h"
#include "smpl_entropy_wrapper.h"

TEST(SmplPulseCoding, Stirling)
{
    EXPECT_EQ(smpl_stirling(0), 0);
    EXPECT_EQ(smpl_stirling(1), 107);
    EXPECT_EQ(smpl_stirling(64 * 4), 55181249);
}

TEST(SmplPulseCoding, ProbSplitFast)
{
    EXPECT_EQ(smpl_prob_split_fast(0, 0), (int32_t)1 << 30);
    EXPECT_EQ(smpl_prob_split_fast(1, 1), (int32_t)1 << 29);
    EXPECT_EQ(smpl_prob_split_fast(1, 2), (int32_t)1 << 29);
    EXPECT_EQ(smpl_prob_split_fast(1, 4), (int32_t)1 << 28);
    EXPECT_EQ(smpl_prob_split_fast(32, 64), (int32_t)107374182);
    EXPECT_EQ(smpl_prob_split_fast(1, 64), 0);
}

TEST(SmplPulseCoding, CreateTables)
{
    void* cbks = smpl_create_pulse_tables();
    EXPECT_TRUE(cbks != nullptr);
}

static inline int random_wrap(int max_val)
{
    return (((int64_t)max_val * rand()) + (RAND_MAX/2)) / RAND_MAX;
}

static inline int calc_height(int prob, int max_h)
{
    int h = 1;
    while (random_wrap(prob) == 1 && h < max_h) {
        h++;
    }
    return h;
}

static inline void generate_pulses(int16_t pulses[SMPL_FRAME_LEN], int nSubfr)
{
    int subfrlen = SMPL_FRAME_LEN / nSubfr;
    for (int i = 0; i < nSubfr; i++) {
        int16_t* pulsesPtr = &pulses[subfrlen * i];
        memset(pulsesPtr, 0, subfrlen * sizeof(int16_t));
        int pulses_in_sf = random_wrap(4);
        for (int p = 0; p < pulses_in_sf; p++) {
            int pos = random_wrap(subfrlen-1);
            int sgn = (pulsesPtr[pos] == 0) ? random_wrap(1) : ((pulsesPtr[pos] > 0) ? 1 : -1);
            int h = calc_height(5, 2);
            pulsesPtr[pos] += sgn == 0 ? -h : h;
        }
    } 
}

TEST(SmplPulseCoding, EncodeDecodeAllZeros)
{
    EXPECT_TRUE(smpl_create_pulse_tables() != nullptr);

    for (int framelen : {160, 320}) {
        for (int lowRate : {SMPL_FALSE, SMPL_TRUE}) {
            for (int voiced : {SMPL_FALSE, SMPL_TRUE}) {
                for (int active : {SMPL_FALSE, SMPL_TRUE}) {
                    if (active == SMPL_FALSE && voiced == SMPL_TRUE) {
                        // Not a valid combination
                        continue;
                    }
                    void* ecEnc = smpl_create_ec_encoder(1024);
                    int16_t pulses_enc[SMPL_FRAME_LEN];
                    memset(pulses_enc, 0, sizeof(pulses_enc));

                    int num_subfr = (lowRate == SMPL_TRUE ? 2 : 4) * framelen / 320;
                    int16_t nPulses_enc[MAX_NUM_SUBFR];
                    // pulses_enc[1] = 2;
                    // pulses_enc[2] = 2;
                    // pulses_enc[3] = 2;
                    // pulses_enc[4] = 2;
                    // pulses_enc[5] = 2;
                    smpl_encode_pulses(smpl_ec_get_srange(ecEnc), pulses_enc, framelen, num_subfr, lowRate, voiced, active, nPulses_enc);

                    unsigned char payload[128];
                    int nbytes = smpl_ec_enc_done(ecEnc, payload, sizeof(payload));

                    void* ecDec = smpl_create_ec_decoder(payload, nbytes);
                    int16_t positions[ SMPL_MAX_N_SUBFR * SMPL_MAX_PULSES_PER_SF];
                    int16_t pos_pulses[SMPL_MAX_N_SUBFR * SMPL_MAX_PULSES_PER_SF];
                    int nPositions;
                    int n_pulses;
                    int16_t nPulses_dec[MAX_NUM_SUBFR];
                    smpl_decode_pulses(smpl_ec_get_srange(ecDec), framelen, num_subfr, lowRate, voiced, active, positions, pos_pulses, &nPositions, &n_pulses, nPulses_dec);
                    int16_t pulses_dec[SMPL_FRAME_LEN];
                    memset(pulses_dec, 0, sizeof(pulses_enc));
                    // printf("nPos = %d\n", nPositions);
                    for (int num_pos = 0; num_pos < nPositions; num_pos++) {
                        // printf("pos = %d, pulse = %d\n", positions[num_pos], pos_pulses[num_pos]);
                        pulses_dec[positions[num_pos]] = pos_pulses[num_pos];
                    }

                    for (int i = 0; i < num_subfr; i++) {
                        EXPECT_EQ(nPulses_enc[i], nPulses_dec[i]);
                    }

                    for (int i = 0; i < framelen; i++) {
                        EXPECT_EQ(pulses_enc[i], pulses_dec[i]);
                    }

                    smpl_free(ecEnc);
                    smpl_free(ecDec);
                }
            }
        }
    }
}

TEST(SmplPulseCoding, EncodeDecodeExtensive)
{
    #define timer_runs  10
    double res_enc = 1e9;
    double res_dec = 1e9;
    for (auto timer_run = 0; timer_run < timer_runs; timer_run++) {
        StopWatch timerEnc;
        StopWatch timerDec;
        srand(9769769); // Avoid flakiness

        EXPECT_TRUE(smpl_create_pulse_tables() != nullptr);

        for (int framelen : {160, 320}) {
            for (int lowRate : {SMPL_TRUE, SMPL_FALSE}) {
                for (int voiced : {SMPL_TRUE, SMPL_FALSE}) {
                    for (int active : {SMPL_TRUE, SMPL_FALSE}) {
                        if (active == SMPL_FALSE && voiced == SMPL_TRUE) {
                            // Not a valid combination
                            continue;
                        }
                        for (int rep = 0; rep < 100; rep++) {
                            void* ecEnc = smpl_create_ec_encoder(1024);

                            int16_t pulses_enc[SMPL_FRAME_LEN];
                            int num_subfr = (lowRate == SMPL_TRUE ? 2 : 4) * framelen / 320;
                            generate_pulses(pulses_enc, num_subfr);
                            int16_t nPulses_enc[MAX_NUM_SUBFR];
                            timerEnc.start();
                            smpl_encode_pulses(smpl_ec_get_srange(ecEnc), pulses_enc, framelen, num_subfr, lowRate, voiced, active, nPulses_enc);
                            timerEnc.stop();

                            unsigned char payload[128];
                            int nbytes = smpl_ec_enc_done(ecEnc, payload, sizeof(payload));

                            void* ecDec = smpl_create_ec_decoder(payload, nbytes);
                            int16_t positions[ SMPL_MAX_N_SUBFR * SMPL_MAX_PULSES_PER_SF];
                            int16_t pos_pulses[SMPL_MAX_N_SUBFR * SMPL_MAX_PULSES_PER_SF];
                            int nPositions;
                            int n_pulses;
                            int16_t nPulses_dec[MAX_NUM_SUBFR];
                            timerDec.start();
                            smpl_decode_pulses(smpl_ec_get_srange(ecDec), framelen, num_subfr, lowRate, voiced, active, positions, pos_pulses, &nPositions, &n_pulses, nPulses_dec);
                            timerDec.stop();
                            int16_t pulses_dec[SMPL_FRAME_LEN];
                            memset(pulses_dec, 0, sizeof(pulses_enc));
                            for (int num_pos = 0; num_pos < nPositions; num_pos++) {
                                pulses_dec[positions[num_pos]] = pos_pulses[num_pos];
                            }

                            for (int i = 0; i < num_subfr; i++) {
                                EXPECT_EQ(nPulses_enc[i], nPulses_dec[i]);
                            }
                            for (int i = 0; i < framelen; i++) {
                                EXPECT_EQ(pulses_enc[i], pulses_dec[i]);
                            }

                            smpl_free(ecEnc);
                            smpl_free(ecDec);
                        }
                    }
                }
            }
        }
        res_enc = SMPL_min(timerEnc.avg_lap_time_ns(), res_enc);
        res_dec = SMPL_min(timerDec.avg_lap_time_ns(), res_dec);
    }
    printf("  Pulse enc %5.2f, pulse dec %5.2f ns\n", res_enc, res_dec);
}
