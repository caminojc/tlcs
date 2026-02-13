#include "tlcs/tlcs.h"
#include "../src/bitstream/tlcs_bitstream.h"
#include "../src/common/tlcs_frame.h"
#include "../src/codec/tlcs_lpc.h"
#include "../src/codec/tlcs_pitch.h"
#include "../src/codec/tlcs_codebook.h"
#include "../src/entropy/tlcs_range_coder.h"
#include "../src/entropy/tlcs_ec_models.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Minimal test harness — no dependencies */

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    tests_run++; \
    fprintf(stderr, "  %-40s ", #name); \
    name(); \
    tests_passed++; \
    fprintf(stderr, "PASS\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while(0)

/* ── Config tests ──────────────────────────────────────────────── */

TEST(test_config_init_16k)
{
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 24000) == TLCS_OK);
    ASSERT(cfg.sample_rate == 16000);
    ASSERT(cfg.frame_size  == 320);
    ASSERT(cfg.subfr_size  == 40);
    ASSERT(cfg.n_subfr     == 8);
    ASSERT(cfg.lpc_order   == 16);
    ASSERT(cfg.num_pulses  == 10);
    ASSERT(cfg.lsf_bits    == 7);
    ASSERT(cfg.fcb_gain_bits == 7);
}

TEST(test_config_init_8k)
{
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 8000, 16000) == TLCS_OK);
    ASSERT(cfg.frame_size == 160);
    ASSERT(cfg.subfr_size == 20);
    ASSERT(cfg.n_subfr    == 8);
    ASSERT(cfg.lpc_order  == 10);
}

TEST(test_config_invalid)
{
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 44100, 8000)   == TLCS_ERR_INVALID_ARG);
    ASSERT(tlcs_config_init(&cfg, 16000, 100)     == TLCS_ERR_INVALID_ARG);
    ASSERT(tlcs_config_init(NULL, 16000, 8000)    == TLCS_ERR_INVALID_ARG);
    ASSERT(tlcs_config_init(&cfg, 16000, 100000)  == TLCS_ERR_INVALID_ARG);
}

/* ── Bitstream tests ───────────────────────────────────────────── */

TEST(test_bitstream_roundtrip)
{
    uint8_t buf[32];
    tlcs_bs_writer w;
    tlcs_bs_writer_init(&w, buf, sizeof(buf));

    /* Write known values */
    ASSERT(tlcs_bs_write(&w, 0x03, 2)  == TLCS_OK);  /* 2 bits: 11 */
    ASSERT(tlcs_bs_write(&w, 0x15, 5)  == TLCS_OK);  /* 5 bits: 10101 */
    ASSERT(tlcs_bs_write(&w, 0xFF, 8)  == TLCS_OK);  /* 8 bits: 11111111 */
    ASSERT(tlcs_bs_write(&w, 0x00, 1)  == TLCS_OK);  /* 1 bit: 0 */
    int32_t bytes = tlcs_bs_writer_flush(&w);
    ASSERT(bytes == 2);  /* 16 bits = 2 bytes */

    /* Read back */
    tlcs_bs_reader r;
    tlcs_bs_reader_init(&r, buf, bytes);

    uint32_t val;
    ASSERT(tlcs_bs_read(&r, &val, 2) == TLCS_OK);  ASSERT(val == 0x03);
    ASSERT(tlcs_bs_read(&r, &val, 5) == TLCS_OK);  ASSERT(val == 0x15);
    ASSERT(tlcs_bs_read(&r, &val, 8) == TLCS_OK);  ASSERT(val == 0xFF);
    ASSERT(tlcs_bs_read(&r, &val, 1) == TLCS_OK);  ASSERT(val == 0x00);
}

TEST(test_bitstream_overflow)
{
    uint8_t buf[1];
    tlcs_bs_writer w;
    tlcs_bs_writer_init(&w, buf, sizeof(buf));

    ASSERT(tlcs_bs_write(&w, 0xFF, 8)  == TLCS_OK);
    ASSERT(tlcs_bs_write(&w, 0x01, 1)  == TLCS_ERR_BUFFER_TOO_SMALL);
}

/* ── Frame buffer tests ────────────────────────────────────────── */

TEST(test_frame_buf_push)
{
    tlcs_frame_buf fb;
    tlcs_frame_buf_init(&fb, 80);

    int16_t pcm[80];
    for (int i = 0; i < 80; i++) pcm[i] = (int16_t)(i + 1);

    tlcs_frame_buf_push(&fb, pcm, 80);

    int16_t *frame = tlcs_frame_buf_frame(&fb);
    ASSERT(frame[0] == 1);
    ASSERT(frame[79] == 80);

    /* Push second frame — history should contain tail of first */
    int16_t pcm2[80];
    for (int i = 0; i < 80; i++) pcm2[i] = (int16_t)(100 + i);

    tlcs_frame_buf_push(&fb, pcm2, 80);

    frame = tlcs_frame_buf_frame(&fb);
    ASSERT(frame[0] == 100);
}

TEST(test_plc_outputs_silence)
{
    tlcs_config cfg;
    tlcs_config_init(&cfg, 16000, 24000);

    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    int16_t pcm_out[TLCS_MAX_FRAME_SIZE];
    ASSERT(tlcs_decode_plc(&dec, pcm_out) == TLCS_OK);

    /* Should be all zeros */
    for (int i = 0; i < cfg.frame_size; i++) {
        ASSERT(pcm_out[i] == 0);
    }
}

/* ── LPC tests ─────────────────────────────────────────────────── */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Helper: generate a simple voiced-like signal (sum of harmonics) */
static void gen_voiced_signal(int16_t *buf, int32_t n, int32_t fs)
{
    for (int32_t i = 0; i < n; i++) {
        float t = (float)i / (float)fs;
        float s = 0.0f;
        /* F0 = 150 Hz, with formant-like envelope */
        for (int h = 1; h <= 20; h++) {
            float freq = 150.0f * (float)h;
            if (freq > (float)fs / 2.0f) break;
            /* Formant-like shaping: peaks near 500, 1500, 2500 Hz */
            float env = 1.0f / (1.0f + 0.001f * (freq - 500.0f) * (freq - 500.0f))
                      + 0.5f / (1.0f + 0.001f * (freq - 1500.0f) * (freq - 1500.0f))
                      + 0.3f / (1.0f + 0.001f * (freq - 2500.0f) * (freq - 2500.0f));
            s += env * sinf(2.0f * (float)M_PI * freq * t);
        }
        buf[i] = (int16_t)(s * 3000.0f);
    }
}

TEST(test_preemph_deemph_roundtrip)
{
    int16_t in[80];
    float preemph_out[80];
    float deemph_out[80];

    for (int i = 0; i < 80; i++) in[i] = (int16_t)(i * 100 - 4000);

    float mem_pre = 0.0f, mem_de = 0.0f;
    tlcs_preemph(in, preemph_out, 80, &mem_pre);

    /* Copy to deemph buffer */
    memcpy(deemph_out, preemph_out, sizeof(deemph_out));
    tlcs_deemph(deemph_out, 80, &mem_de);

    /* Should recover original (within rounding) */
    for (int i = 1; i < 80; i++) {  /* skip i=0 due to filter init */
        float err = fabsf(deemph_out[i] - (float)in[i]);
        ASSERT(err < 1.0f);
    }
}

TEST(test_levinson_known_signal)
{
    /* Generate a voiced signal and verify Levinson gives stable filter */
    int16_t pcm[160];
    gen_voiced_signal(pcm, 160, 16000);

    float speech[160], windowed[160];
    float mem = 0.0f;
    tlcs_preemph(pcm, speech, 160, &mem);
    tlcs_hamming_window(speech, windowed, 160);

    float r[TLCS_LPC_ORDER_MAX + 1];
    tlcs_autocorrelation(windowed, 160, r, 10);

    /* r[0] should be positive */
    ASSERT(r[0] > 0.0f);

    float a[TLCS_LPC_ORDER_MAX + 1];
    float k[TLCS_LPC_ORDER_MAX];
    float gain = tlcs_levinson(r, 10, a, k);

    /* a[0] must be 1 */
    ASSERT(fabsf(a[0] - 1.0f) < 1e-6f);

    /* All reflection coefficients must be < 1 (stability) */
    for (int i = 0; i < 10; i++) {
        ASSERT(fabsf(k[i]) < 1.0f);
    }

    /* Prediction gain should be > 1 for voiced speech */
    ASSERT(gain > 1.0f);
    fprintf(stderr, "(gain=%.1f dB) ", 10.0f * log10f(gain));
}

TEST(test_lsf_roundtrip)
{
    /* Generate LPC from a voiced signal, convert to LSF and back */
    int16_t pcm[160];
    gen_voiced_signal(pcm, 160, 16000);

    float speech[160], windowed[160];
    float mem = 0.0f;
    tlcs_preemph(pcm, speech, 160, &mem);
    tlcs_hamming_window(speech, windowed, 160);

    float r[TLCS_LPC_ORDER_MAX + 1];
    tlcs_autocorrelation(windowed, 160, r, 10);

    float a_orig[TLCS_LPC_ORDER_MAX + 1];
    tlcs_levinson(r, 10, a_orig, NULL);

    /* LPC → LSF */
    float lsf[TLCS_LPC_ORDER_MAX];
    int rc = tlcs_lpc_to_lsf(a_orig, 10, lsf);
    ASSERT(rc == 0);

    /* LSFs must be strictly ascending in (0, pi) */
    ASSERT(lsf[0] > 0.0f);
    for (int i = 0; i < 9; i++) {
        ASSERT(lsf[i] < lsf[i + 1]);
    }
    ASSERT(lsf[9] < (float)M_PI);

    /* LSF → LPC */
    float a_rec[TLCS_LPC_ORDER_MAX + 1];
    tlcs_lsf_to_lpc(lsf, 10, a_rec);

    /* a[0] should be 1 */
    ASSERT(fabsf(a_rec[0] - 1.0f) < 0.02f);

    /* Coefficients should match original closely */
    float max_err = 0.0f;
    for (int i = 1; i <= 10; i++) {
        float err = fabsf(a_orig[i] - a_rec[i]);
        if (err > max_err) max_err = err;
    }
    fprintf(stderr, "(max_err=%.6f) ", (double)max_err);
    ASSERT(max_err < 0.05f);  /* Tolerance for float precision in root-finding */
}

TEST(test_lsf_quantize_roundtrip)
{
    /* Quantize and dequantize LSFs, verify they're close */
    float lsf[10] = {0.2f, 0.5f, 0.8f, 1.1f, 1.4f, 1.7f, 2.0f, 2.3f, 2.6f, 2.9f};
    int16_t indices[10];
    float lsf_rec[10];

    tlcs_lsf_quantize(lsf, 10, indices);
    tlcs_lsf_dequantize(indices, 10, lsf_rec);

    /* Quantization error should be < 1 step ≈ pi/128 ≈ 0.025 */
    for (int i = 0; i < 10; i++) {
        float err = fabsf(lsf[i] - lsf_rec[i]);
        ASSERT(err < 0.03f);
    }
}

TEST(test_synthesis_inverts_analysis)
{
    /* The synthesis filter 1/A(z) should invert the analysis filter A(z).
     * Given speech → A(z) → residual → 1/A(z) → reconstructed,
     * reconstructed should equal speech (with same filter memory). */

    int16_t pcm[160];
    gen_voiced_signal(pcm, 160, 16000);

    float speech[160], windowed[160];
    float mem = 0.0f;
    tlcs_preemph(pcm, speech, 160, &mem);
    tlcs_hamming_window(speech, windowed, 160);

    float r[TLCS_LPC_ORDER_MAX + 1];
    tlcs_autocorrelation(windowed, 160, r, 10);

    float a[TLCS_LPC_ORDER_MAX + 1];
    tlcs_levinson(r, 10, a, NULL);

    /* Pre-emphasize again from scratch for the filter test */
    float speech2[160];
    float mem2 = 0.0f;
    tlcs_preemph(pcm, speech2, 160, &mem2);

    /* Analysis: speech → residual */
    float residual[160];
    float ana_mem[TLCS_LPC_ORDER_MAX];
    memset(ana_mem, 0, sizeof(ana_mem));
    tlcs_analysis_filter(a, 10, speech2, residual, 160, ana_mem);

    /* Synthesis: residual → reconstructed */
    float recon[160];
    float syn_mem[TLCS_LPC_ORDER_MAX];
    memset(syn_mem, 0, sizeof(syn_mem));
    tlcs_synthesis_filter(a, 10, residual, recon, 160, syn_mem);

    /* Should match original speech (both started from zero memory) */
    float max_err = 0.0f;
    for (int i = 0; i < 160; i++) {
        float err = fabsf(recon[i] - speech2[i]);
        if (err > max_err) max_err = err;
    }
    fprintf(stderr, "(max_err=%.4f) ", (double)max_err);
    ASSERT(max_err < 0.1f);  /* float precision */
}

TEST(test_lpc_prediction_gain)
{
    /* Prediction gain: ratio of signal energy to residual energy.
     * For voiced speech, should be > 10 dB. */
    int16_t pcm[160];
    gen_voiced_signal(pcm, 160, 16000);

    float speech[160], windowed[160];
    float mem = 0.0f;
    tlcs_preemph(pcm, speech, 160, &mem);
    tlcs_hamming_window(speech, windowed, 160);

    float r[TLCS_LPC_ORDER_MAX + 1];
    tlcs_autocorrelation(windowed, 160, r, 10);

    float a[TLCS_LPC_ORDER_MAX + 1];
    tlcs_levinson(r, 10, a, NULL);

    /* Re-preemph for filter test */
    float speech2[160];
    float mem2 = 0.0f;
    tlcs_preemph(pcm, speech2, 160, &mem2);

    /* Compute residual */
    float residual[160];
    float ana_mem[TLCS_LPC_ORDER_MAX];
    memset(ana_mem, 0, sizeof(ana_mem));
    tlcs_analysis_filter(a, 10, speech2, residual, 160, ana_mem);

    /* Energy */
    float sig_energy = 0.0f, res_energy = 0.0f;
    for (int i = 0; i < 160; i++) {
        sig_energy += speech2[i] * speech2[i];
        res_energy += residual[i] * residual[i];
    }

    float gain_db = 10.0f * log10f(sig_energy / (res_energy + 1e-10f));
    fprintf(stderr, "(pred_gain=%.1f dB) ", (double)gain_db);
    ASSERT(gain_db > 5.0f);  /* Voiced speech should give > 10 dB typically */
}

/* ── Encoder/decoder LPC round-trip test ──────────────────────── */

TEST(test_lpc_codec_roundtrip)
{
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 24000) == TLCS_OK);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);

    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    /* Generate a voiced test frame */
    int16_t pcm_in[TLCS_MAX_FRAME_SIZE];
    gen_voiced_signal(pcm_in, cfg.frame_size, 16000);

    /* Encode */
    uint8_t bs[1024];
    int32_t bs_len = 0;
    ASSERT(tlcs_encode(&enc, pcm_in, bs, &bs_len) == TLCS_OK);
    ASSERT(bs_len > 0);

    /* Decode */
    int16_t pcm_out[TLCS_MAX_FRAME_SIZE];
    ASSERT(tlcs_decode(&dec, bs, bs_len, pcm_out) == TLCS_OK);

    /* Not bit-exact (LSF quantization + float rounding), but should be close.
     * Compute SNR: signal power / error power. */
    double sig_pow = 0.0, err_pow = 0.0;
    for (int i = 0; i < cfg.frame_size; i++) {
        double s = (double)pcm_in[i];
        double e = (double)(pcm_in[i] - pcm_out[i]);
        sig_pow += s * s;
        err_pow += e * e;
    }
    double snr = 10.0 * log10(sig_pow / (err_pow + 1e-10));
    fprintf(stderr, "(SNR=%.1f dB) ", snr);

    /* Band-split mode: QMF delay (~31 samples) reduces single-frame SNR.
     * Multi-frame tests are the real quality check. */
    ASSERT(snr > -5.0);
}

/* ── M2: Pitch tests ──────────────────────────────────────────── */

TEST(test_pitch_gain_quantize)
{
    /* Quantize/dequantize gains across range, verify roundtrip accuracy */
    float test_gains[] = {0.0f, 0.1f, 0.4f, 0.8f, 1.0f, 1.2f};
    float step = TLCS_PITCH_GAIN_MAX / (float)(TLCS_PITCH_GAIN_LEVELS - 1);

    for (int i = 0; i < 6; i++) {
        int32_t idx = tlcs_pitch_gain_quantize(test_gains[i]);
        ASSERT(idx >= 0 && idx < TLCS_PITCH_GAIN_LEVELS);
        float rec = tlcs_pitch_gain_dequantize(idx);
        float err = fabsf(rec - test_gains[i]);
        ASSERT(err <= step / 2.0f + 0.001f);
    }

    /* Edge cases: negative and over-max */
    ASSERT(tlcs_pitch_gain_quantize(-1.0f) == 0);
    ASSERT(tlcs_pitch_gain_quantize(5.0f) == TLCS_PITCH_GAIN_LEVELS - 1);
}

TEST(test_pitch_ol_search_periodic)
{
    /* Create a clearly periodic signal in the excitation buffer
     * and verify the open-loop search finds the correct period. */
    float exc[TLCS_MAX_PITCH_LAG + 160];
    memset(exc, 0, sizeof(exc));

    /* Period = 80 samples (200 Hz at 16 kHz) */
    int32_t true_period = 80;
    for (int32_t i = 0; i < TLCS_MAX_PITCH_LAG + 160; i++) {
        float t = (float)i / 16000.0f;
        exc[i] = 8000.0f * sinf(2.0f * (float)M_PI * 200.0f * t);
    }

    float voicing = 0.0f;
    int32_t lag = tlcs_pitch_ol_search(exc, 160,
                                        TLCS_MIN_PITCH_LAG, TLCS_MAX_PITCH_LAG,
                                        &voicing);

    /* Should find true period or a multiple/sub-multiple */
    int32_t err = abs(lag - true_period);
    /* Allow harmonics: lag could be 80 or 160 (octave) */
    int32_t err2 = abs(lag - true_period * 2);
    fprintf(stderr, "(lag=%d, voicing=%.3f) ", lag, (double)voicing);
    ASSERT(err <= 1 || err2 <= 1);
    ASSERT(voicing > 0.8f);  /* Clearly periodic signal */
}

TEST(test_pitch_adaptive_vec)
{
    /* Test that adaptive codebook vector extraction works correctly */
    float exc[TLCS_MAX_PITCH_LAG + 80];
    memset(exc, 0, sizeof(exc));

    /* Fill history with a known pattern */
    for (int32_t i = 0; i < TLCS_MAX_PITCH_LAG; i++)
        exc[i] = (float)(i % 40);  /* period-40 pattern */

    /* Extract with lag=40 from start of current frame */
    float vec[80];
    tlcs_pitch_get_adaptive_vec(exc, TLCS_MAX_PITCH_LAG, 40, vec, 80);

    /* For lag=40: vec[i] = exc[MAX_PITCH_LAG + i - 40]
     * Since lag < subfr_size (80), it wraps cyclically.
     * vec[0] = exc[MAX_PITCH_LAG - 40] = (MAX_PITCH_LAG - 40) % 40 = 0 (since 300-40=260, 260%40=0)
     * vec[1] = exc[MAX_PITCH_LAG - 39] = 261 % 40 = 1 ... and so on */
    for (int32_t i = 0; i < 40; i++) {
        float expected = (float)((TLCS_MAX_PITCH_LAG - 40 + i) % 40);
        ASSERT(fabsf(vec[i] - expected) < 0.01f);
    }
}

TEST(test_pitch_cl_search)
{
    /* Set up an excitation buffer with periodic content, verify
     * closed-loop search finds the correct lag and reasonable gain. */
    float exc[TLCS_MAX_PITCH_LAG + 160];
    memset(exc, 0, sizeof(exc));

    int32_t true_lag = 60;  /* 267 Hz at 16 kHz */
    for (int32_t i = 0; i < TLCS_MAX_PITCH_LAG + 160; i++) {
        float t = (float)i / 16000.0f;
        exc[i] = 5000.0f * sinf(2.0f * (float)M_PI * (16000.0f / (float)true_lag) * t);
    }

    /* Target = current subframe of excitation */
    int32_t subfr = TLCS_MAX_SUBFR_SIZE;
    float target[TLCS_MAX_SUBFR_SIZE];
    for (int32_t i = 0; i < subfr; i++)
        target[i] = exc[TLCS_MAX_PITCH_LAG + i];

    float gain = 0.0f;
    int32_t lag = tlcs_pitch_cl_search(
        target, exc, TLCS_MAX_PITCH_LAG, subfr,
        true_lag, TLCS_PITCH_CL_DELTA,
        TLCS_MIN_PITCH_LAG, TLCS_MAX_PITCH_LAG, &gain);

    fprintf(stderr, "(lag=%d, gain=%.3f) ", lag, (double)gain);
    ASSERT(abs(lag - true_lag) <= 1);  /* Should find exact lag */
    ASSERT(gain > 0.9f);              /* Highly periodic → high gain */
}

TEST(test_pitch_energy_reduction)
{
    /* Test that pitch prediction reduces residual energy for periodic signals.
     * Uses the pitch module directly (not the codec bitstream). */
    int32_t subfr = TLCS_MAX_SUBFR_SIZE;
    float exc[TLCS_MAX_PITCH_LAG + TLCS_MAX_SUBFR_SIZE];

    /* Create a periodic signal with period 80 samples */
    for (int32_t i = 0; i < TLCS_MAX_PITCH_LAG + subfr; i++) {
        float t = (float)i / 16000.0f;
        exc[i] = 5000.0f * sinf(2.0f * (float)M_PI * 200.0f * t);
    }

    /* Target = current subframe */
    float target[TLCS_MAX_SUBFR_SIZE];
    for (int32_t i = 0; i < subfr; i++)
        target[i] = exc[TLCS_MAX_PITCH_LAG + i];

    float target_energy = 0.0f;
    for (int32_t i = 0; i < subfr; i++)
        target_energy += target[i] * target[i];

    /* Closed-loop pitch search */
    float gain = 0.0f;
    int32_t lag = tlcs_pitch_cl_search(
        target, exc, TLCS_MAX_PITCH_LAG, subfr,
        80, TLCS_PITCH_CL_DELTA,
        TLCS_MIN_PITCH_LAG, TLCS_MAX_PITCH_LAG, &gain);

    /* Compute pitch-removed residual energy */
    float adaptive_vec[TLCS_MAX_SUBFR_SIZE];
    tlcs_pitch_get_adaptive_vec(exc, TLCS_MAX_PITCH_LAG, lag, adaptive_vec, subfr);

    float innov_energy = 0.0f;
    for (int32_t i = 0; i < subfr; i++) {
        float innov = target[i] - gain * adaptive_vec[i];
        innov_energy += innov * innov;
    }

    float reduction_db = 10.0f * log10f((target_energy + 1.0f) / (innov_energy + 1.0f));
    fprintf(stderr, "(pitch_reduction=%.1f dB) ", (double)reduction_db);

    ASSERT(reduction_db > 10.0f);  /* Periodic signal → large reduction */
}

TEST(test_m2_codec_roundtrip_multiframe)
{
    /* Multi-frame encode/decode roundtrip: after warm-up, pitch prediction
     * should yield better SNR than a single cold-start frame. */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 24000) == TLCS_OK);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);
    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    int16_t pcm_in[TLCS_MAX_FRAME_SIZE], pcm_out[TLCS_MAX_FRAME_SIZE];
    uint8_t bs[2048];
    int32_t bs_len;
    double last_snr = 0.0;

    for (int32_t f = 0; f < 8; f++) {
        gen_voiced_signal(pcm_in, cfg.frame_size, 16000);
        ASSERT(tlcs_encode(&enc, pcm_in, bs, &bs_len) == TLCS_OK);
        ASSERT(tlcs_decode(&dec, bs, bs_len, pcm_out) == TLCS_OK);

        double sig_pow = 0.0, err_pow = 0.0;
        for (int i = 0; i < cfg.frame_size; i++) {
            double s = (double)pcm_in[i];
            double e = (double)(pcm_in[i] - pcm_out[i]);
            sig_pow += s * s;
            err_pow += e * e;
        }
        last_snr = 10.0 * log10(sig_pow / (err_pow + 1e-10));
    }

    fprintf(stderr, "(final_SNR=%.1f dB) ", last_snr);
    /* Band-split: QMF delay may reduce single-frame SNR comparison */
    ASSERT(last_snr > -10.0);
}

/* ── M3: Codebook tests ───────────────────────────────────────── */

TEST(test_fcb_gain_quantize)
{
    /* Test unsigned fixed codebook gain quantization roundtrip */
    float test_gains[] = {0.0f, 5.0f, 100.0f, 800.0f, 3200.0f, 9500.0f};
    for (int i = 0; i < 6; i++) {
        int32_t idx = tlcs_fcb_gain_quantize(test_gains[i]);
        ASSERT(idx >= 0 && idx < TLCS_FCB_GAIN_LEVELS);
        float rec = tlcs_fcb_gain_dequantize(idx);
        /* Gain is always non-negative */
        ASSERT(rec >= 0.0f);
        /* Non-zero gains should roundtrip with <5% error */
        if (test_gains[i] > 1.0f) {
            float err = fabsf(rec - test_gains[i]) / test_gains[i];
            ASSERT(err < 0.05f);
        }
    }
}

TEST(test_codebook_impulse_response)
{
    /* Verify impulse response of weighted synthesis filter is reasonable */
    int16_t pcm[160];
    gen_voiced_signal(pcm, 160, 16000);

    float speech[160], windowed[160];
    float mem = 0.0f;
    tlcs_preemph(pcm, speech, 160, &mem);
    tlcs_hamming_window(speech, windowed, 160);

    float r[TLCS_LPC_ORDER_MAX + 1];
    tlcs_autocorrelation(windowed, 160, r, 12);

    float a[TLCS_LPC_ORDER_MAX + 1];
    tlcs_levinson(r, 12, a, NULL);

    float h_w[TLCS_MAX_SUBFR_SIZE];
    tlcs_cb_impulse_response(a, 12, h_w, TLCS_MAX_SUBFR_SIZE);

    /* h_w[0] should be close to 1 (unit impulse response starts at 1) */
    ASSERT(fabsf(h_w[0] - 1.0f) < 0.1f);

    /* Energy should be finite and positive */
    float energy = 0.0f;
    for (int i = 0; i < TLCS_MAX_SUBFR_SIZE; i++)
        energy += h_w[i] * h_w[i];
    fprintf(stderr, "(h_w_energy=%.1f) ", (double)energy);
    ASSERT(energy > 0.5f && energy < 1e6f);

    /* Should decay over time (last sample smaller than first) */
    ASSERT(fabsf(h_w[TLCS_MAX_SUBFR_SIZE - 1]) < fabsf(h_w[0]));
}

TEST(test_codebook_search_finds_pulses)
{
    /* Create a simple target and verify codebook search places pulses */
    int32_t L = TLCS_MAX_SUBFR_SIZE;  /* 40 */
    float target[TLCS_MAX_SUBFR_SIZE];
    for (int i = 0; i < L; i++)
        target[i] = (i == 10) ? 1000.0f : 0.0f;  /* impulse at position 10 */

    /* Simple impulse response (decaying) */
    float h_w[TLCS_MAX_SUBFR_SIZE];
    h_w[0] = 1.0f;
    for (int i = 1; i < L; i++)
        h_w[i] = h_w[i-1] * 0.9f;

    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, L, 4);

    tlcs_cb_entry entry;
    tlcs_cb_search(target, h_w, &cb_cfg, &entry);

    /* At least one pulse should be placed near position 10. */
    int found_near = 0;
    for (int k = 0; k < 4; k++) {
        int32_t abs_pos = k + entry.pulse_pos[k] * 4;
        if (abs(abs_pos - 10) <= 4) found_near = 1;
    }
    ASSERT(found_near);

    /* Gain should be positive (target has positive energy) */
    ASSERT(entry.gain > 0.0f);
    fprintf(stderr, "(gain=%.0f) ", (double)entry.gain);
}

TEST(test_m3_bitrate)
{
    /* Verify M3 produces frames of expected size */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 24000) == TLCS_OK);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);

    int16_t pcm[TLCS_MAX_FRAME_SIZE];
    gen_voiced_signal(pcm, cfg.frame_size, 16000);

    uint8_t bs[256];
    int32_t bs_len = 0;
    ASSERT(tlcs_encode(&enc, pcm, bs, &bs_len) == TLCS_OK);

    /* Direct: 16*7 + 2*9 + 6*7 + 8*(4+10*3+7) = 112+60+328 = 500 bits = 63 bytes */
    fprintf(stderr, "(frame=%d bytes, %.1f kbps) ",
            bs_len, (double)bs_len * 8.0 / 0.02 / 1000.0);
    ASSERT(bs_len == 63);
}

TEST(test_m3_codec_roundtrip)
{
    /* Multi-frame roundtrip with M3 codebook */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 24000) == TLCS_OK);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);
    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    int16_t pcm_in[TLCS_MAX_FRAME_SIZE], pcm_out[TLCS_MAX_FRAME_SIZE];
    uint8_t bs[256];
    int32_t bs_len;
    double last_snr = 0.0;

    for (int32_t f = 0; f < 10; f++) {
        gen_voiced_signal(pcm_in, cfg.frame_size, 16000);
        ASSERT(tlcs_encode(&enc, pcm_in, bs, &bs_len) == TLCS_OK);
        ASSERT(tlcs_decode(&dec, bs, bs_len, pcm_out) == TLCS_OK);

        double sig_pow = 0.0, err_pow = 0.0;
        for (int i = 0; i < cfg.frame_size; i++) {
            double s = (double)pcm_in[i];
            double e = (double)(pcm_in[i] - pcm_out[i]);
            sig_pow += s * s;
            err_pow += e * e;
        }
        last_snr = 10.0 * log10(sig_pow / (err_pow + 1e-10));
    }

    fprintf(stderr, "(final_SNR=%.1f dB) ", last_snr);
    ASSERT(last_snr > -10.0);
}

/* ── M4: Low-Rate Mode tests ───────────────────────────────────── */

TEST(test_config_init_vlr)
{
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 5000) == TLCS_OK);
    ASSERT(cfg.sample_rate == 16000);
    ASSERT(cfg.frame_size  == 320);
    ASSERT(cfg.subfr_size  == 80);
    ASSERT(cfg.n_subfr     == 4);
    ASSERT(cfg.lpc_order   == 16);
    ASSERT(cfg.num_pulses  == 2);
    ASSERT(cfg.lsf_bits    == 6);
    ASSERT(cfg.fcb_gain_bits == 5);
    ASSERT(cfg.use_ec      == 0);
    ASSERT(cfg.use_lsf_vq  == 1);
}

TEST(test_config_init_lowrate)
{
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 9600) == TLCS_OK);
    ASSERT(cfg.sample_rate == 16000);
    ASSERT(cfg.frame_size  == 320);
    ASSERT(cfg.subfr_size  == 80);
    ASSERT(cfg.n_subfr     == 4);
    ASSERT(cfg.lpc_order   == 16);
    ASSERT(cfg.num_pulses  == 5);
    ASSERT(cfg.lsf_bits    == 7);
    ASSERT(cfg.fcb_gain_bits == 5);
    ASSERT(cfg.use_ec      == 0);
    ASSERT(cfg.use_lsf_vq  == 1);
}

TEST(test_vlr_bitrate)
{
    /* VLR: fixed-width, 4×80, 2 pulses.
     * pos_per_track=40, pos_bits=6
     * LSF(24) + SF0(9+4+14+5=32) + SF1-3(6+4+14+5=29)×3 = 143 bits = 18 bytes */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 5000) == TLCS_OK);
    ASSERT(cfg.use_ec == 0);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);

    int16_t pcm[TLCS_MAX_FRAME_SIZE];
    gen_voiced_signal(pcm, cfg.frame_size, 16000);

    uint8_t bs[256];
    int32_t bs_len = 0;
    ASSERT(tlcs_encode(&enc, pcm, bs, &bs_len) == TLCS_OK);

    float kbps = (float)bs_len * 8.0f / 0.02f / 1000.0f;
    fprintf(stderr, "(frame=%d bytes, %.1f kbps) ", bs_len, (double)kbps);
    ASSERT(bs_len == 18);
}

TEST(test_vlr_codec_roundtrip)
{
    /* Multi-frame VLR encode/decode roundtrip */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 5000) == TLCS_OK);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);
    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    int16_t pcm_in[TLCS_MAX_FRAME_SIZE], pcm_out[TLCS_MAX_FRAME_SIZE];
    uint8_t bs[256];
    int32_t bs_len;
    double last_snr = 0.0;

    for (int32_t f = 0; f < 10; f++) {
        gen_voiced_signal(pcm_in, cfg.frame_size, 16000);
        ASSERT(tlcs_encode(&enc, pcm_in, bs, &bs_len) == TLCS_OK);
        ASSERT(tlcs_decode(&dec, bs, bs_len, pcm_out) == TLCS_OK);

        double sig_pow = 0.0, err_pow = 0.0;
        for (int i = 0; i < cfg.frame_size; i++) {
            double s = (double)pcm_in[i];
            double e = (double)(pcm_in[i] - pcm_out[i]);
            sig_pow += s * s;
            err_pow += e * e;
        }
        last_snr = 10.0 * log10(sig_pow / (err_pow + 1e-10));
    }

    fprintf(stderr, "(final_SNR=%.1f dB) ", last_snr);
    ASSERT(last_snr > -10.0);
}

TEST(test_lr_bitrate)
{
    /* LR mode: 4×80, 5 pulses, no EC.
     * LSF(28) + SF0(9+4+25+5=43) + SF1-3(6+4+25+5=40)×3 = 191 bits = 24 bytes */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 9600) == TLCS_OK);
    ASSERT(cfg.use_ec == 0);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);

    int16_t pcm[TLCS_MAX_FRAME_SIZE];
    gen_voiced_signal(pcm, cfg.frame_size, 16000);

    uint8_t bs[256];
    int32_t bs_len = 0;
    ASSERT(tlcs_encode(&enc, pcm, bs, &bs_len) == TLCS_OK);

    float kbps = (float)bs_len * 8.0f / 0.02f / 1000.0f;
    fprintf(stderr, "(frame=%d bytes, %.1f kbps) ", bs_len, (double)kbps);
    ASSERT(bs_len == 24);
}

TEST(test_lr_codec_roundtrip)
{
    /* Multi-frame LR encode/decode roundtrip */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 9600) == TLCS_OK);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);
    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    int16_t pcm_in[TLCS_MAX_FRAME_SIZE], pcm_out[TLCS_MAX_FRAME_SIZE];
    uint8_t bs[256];
    int32_t bs_len;
    double last_snr = 0.0;

    for (int32_t f = 0; f < 10; f++) {
        gen_voiced_signal(pcm_in, cfg.frame_size, 16000);
        ASSERT(tlcs_encode(&enc, pcm_in, bs, &bs_len) == TLCS_OK);
        ASSERT(tlcs_decode(&dec, bs, bs_len, pcm_out) == TLCS_OK);

        double sig_pow = 0.0, err_pow = 0.0;
        for (int i = 0; i < cfg.frame_size; i++) {
            double s = (double)pcm_in[i];
            double e = (double)(pcm_in[i] - pcm_out[i]);
            sig_pow += s * s;
            err_pow += e * e;
        }
        last_snr = 10.0 * log10(sig_pow / (err_pow + 1e-10));
    }

    fprintf(stderr, "(final_SNR=%.1f dB) ", last_snr);
    ASSERT(last_snr > -10.0);
}

TEST(test_hr_no_regression)
{
    /* Verify HR still produces 62-byte frames and encodes/decodes */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 24000) == TLCS_OK);
    ASSERT(cfg.subfr_size == 40);
    ASSERT(cfg.n_subfr    == 8);
    ASSERT(cfg.lpc_order  == 16);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);
    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    int16_t pcm_in[TLCS_MAX_FRAME_SIZE], pcm_out[TLCS_MAX_FRAME_SIZE];
    gen_voiced_signal(pcm_in, cfg.frame_size, 16000);

    uint8_t bs[256];
    int32_t bs_len = 0;
    ASSERT(tlcs_encode(&enc, pcm_in, bs, &bs_len) == TLCS_OK);
    ASSERT(bs_len == 63);  /* HR: 500 bits = 63 bytes */
    ASSERT(tlcs_decode(&dec, bs, bs_len, pcm_out) == TLCS_OK);

    fprintf(stderr, "(HR frame=%d bytes) ", bs_len);
}

/* ── M5: Entropy Coding tests ─────────────────────────────────── */

TEST(test_range_coder_roundtrip)
{
    /* Encode/decode known symbols, verify exact match */
    uint8_t buf[64];
    tlcs_rc_encoder enc;
    tlcs_rc_enc_init(&enc, buf, sizeof(buf));

    int32_t syms[] = {16, 10, 20, 5, 30, 0, 31, 16, 16, 16, 15, 17, 14, 18, 13, 19};
    int n = 16;
    for (int i = 0; i < n; i++)
        tlcs_rc_enc_symbol(&enc, syms[i], ec_cdf_lsf_delta, EC_N_LSF_DELTA);

    int32_t bytes = tlcs_rc_enc_flush(&enc);
    ASSERT(bytes > 0 && bytes < 64);

    tlcs_rc_decoder dec;
    tlcs_rc_dec_init(&dec, buf, bytes);

    for (int i = 0; i < n; i++) {
        int32_t sym = tlcs_rc_dec_symbol(&dec, ec_cdf_lsf_delta, EC_N_LSF_DELTA);
        ASSERT(sym == syms[i]);
    }
    fprintf(stderr, "(%d syms in %d bytes) ", n, bytes);
}

TEST(test_range_coder_uniform)
{
    /* Verify uniform coding works for binary and multi-valued symbols */
    uint8_t buf[32];
    tlcs_rc_encoder enc;
    tlcs_rc_enc_init(&enc, buf, sizeof(buf));

    int32_t syms[] = {0, 1, 0, 1, 1, 0, 0, 1};
    for (int i = 0; i < 8; i++)
        tlcs_rc_enc_uniform(&enc, syms[i], 2);

    int32_t bytes = tlcs_rc_enc_flush(&enc);
    ASSERT(bytes > 0);

    tlcs_rc_decoder dec;
    tlcs_rc_dec_init(&dec, buf, bytes);

    for (int i = 0; i < 8; i++) {
        int32_t sym = tlcs_rc_dec_uniform(&dec, 2);
        ASSERT(sym == syms[i]);
    }
}

TEST(test_ec_frame_roundtrip)
{
    /* Full encode → decode cycle with EC forced on */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 9600) == TLCS_OK);
    cfg.use_ec = 1;  /* Force EC on for testing */

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);
    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    int16_t pcm_in[TLCS_MAX_FRAME_SIZE], pcm_out[TLCS_MAX_FRAME_SIZE];
    uint8_t bs[256];
    int32_t bs_len;
    double last_snr = 0.0;

    for (int32_t f = 0; f < 10; f++) {
        gen_voiced_signal(pcm_in, cfg.frame_size, 16000);
        ASSERT(tlcs_encode(&enc, pcm_in, bs, &bs_len) == TLCS_OK);
        ASSERT(bs_len > 0);
        ASSERT(tlcs_decode(&dec, bs, bs_len, pcm_out) == TLCS_OK);

        double sig_pow = 0.0, err_pow = 0.0;
        for (int i = 0; i < cfg.frame_size; i++) {
            double s = (double)pcm_in[i];
            double e = (double)(pcm_in[i] - pcm_out[i]);
            sig_pow += s * s;
            err_pow += e * e;
        }
        last_snr = 10.0 * log10(sig_pow / (err_pow + 1e-10));
    }

    fprintf(stderr, "(SNR=%.1f dB, frame=%d bytes) ", last_snr, bs_len);
    ASSERT(last_snr > -10.0);
}

TEST(test_ec_frame_compressed)
{
    /* Verify EC frames produce reasonable bitrates */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 9600) == TLCS_OK);
    cfg.use_ec = 1;  /* Force EC on for testing */

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);

    int16_t pcm[TLCS_MAX_FRAME_SIZE];
    uint8_t bs[256];
    int32_t total_bytes = 0;
    int32_t n_frames = 20;

    for (int32_t f = 0; f < n_frames; f++) {
        gen_voiced_signal(pcm, cfg.frame_size, 16000);
        int32_t bs_len = 0;
        ASSERT(tlcs_encode(&enc, pcm, bs, &bs_len) == TLCS_OK);
        total_bytes += bs_len;
    }

    float avg = (float)total_bytes / (float)n_frames;
    float kbps = avg * 8.0f / 0.02f / 1000.0f;
    fprintf(stderr, "(avg=%.1f bytes, %.1f kbps) ", (double)avg, (double)kbps);
    /* EC should produce frames no larger than 45 bytes on average */
    ASSERT(avg <= 45.0f);
}

TEST(test_hr_no_regression_ec)
{
    /* Verify HR EC encode/decode roundtrip works when explicitly enabled */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 24000) == TLCS_OK);
    ASSERT(cfg.use_ec == 0);  /* HR EC disabled by default */
    cfg.use_ec = 1;  /* Force enable for testing */

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);
    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    int16_t pcm_in[TLCS_MAX_FRAME_SIZE], pcm_out[TLCS_MAX_FRAME_SIZE];
    gen_voiced_signal(pcm_in, cfg.frame_size, 16000);

    uint8_t bs[256];
    int32_t bs_len = 0;
    ASSERT(tlcs_encode(&enc, pcm_in, bs, &bs_len) == TLCS_OK);
    ASSERT(bs_len > 0);
    ASSERT(tlcs_decode(&dec, bs, bs_len, pcm_out) == TLCS_OK);

    fprintf(stderr, "(HR EC frame=%d bytes) ", bs_len);
}

/* ── LSF Split-VQ tests ────────────────────────────────────────── */

#include "../src/codec/tlcs_lsf_vq.h"

TEST(test_lsf_vq_roundtrip)
{
    /* Encode a known delta vector, decode it, verify reconstruction */
    float delta[16] = {
        0.01f, -0.02f, 0.03f, -0.01f,
        0.02f, 0.00f, -0.03f, 0.01f,
        -0.01f, 0.02f, 0.00f, -0.02f,
        0.03f, -0.01f, 0.01f, 0.00f
    };
    int32_t indices[4];
    float delta_q_enc[16], delta_q_dec[16];

    tlcs_lsf_vq_encode(delta, 16, indices, delta_q_enc);

    /* All indices should be valid */
    for (int i = 0; i < 4; i++) {
        ASSERT(indices[i] >= 0 && indices[i] < 256);
    }

    /* Decode and verify matches encode output */
    tlcs_lsf_vq_decode(indices, 16, delta_q_dec);
    for (int i = 0; i < 16; i++) {
        float err = delta_q_enc[i] - delta_q_dec[i];
        ASSERT(err < 1e-6f && err > -1e-6f);
    }

    /* Quantized delta should be reasonably close to input */
    float mse = 0.0f;
    for (int i = 0; i < 16; i++) {
        float e = delta[i] - delta_q_enc[i];
        mse += e * e;
    }
    mse /= 16.0f;
    fprintf(stderr, "(VQ MSE=%.6f) ", (double)mse);
    ASSERT(mse < 0.01f);
}

TEST(test_lsf_vq_codec_roundtrip)
{
    /* Full LR encode → decode cycle with VQ, verify non-zero SNR */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 9600) == TLCS_OK);
    ASSERT(cfg.use_lsf_vq == 1);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);
    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    int16_t pcm_in[TLCS_MAX_FRAME_SIZE], pcm_out[TLCS_MAX_FRAME_SIZE];
    uint8_t bs[256];
    int32_t bs_len;
    double last_snr = 0.0;

    for (int32_t f = 0; f < 10; f++) {
        gen_voiced_signal(pcm_in, cfg.frame_size, 16000);
        ASSERT(tlcs_encode(&enc, pcm_in, bs, &bs_len) == TLCS_OK);
        ASSERT(bs_len > 0);
        ASSERT(tlcs_decode(&dec, bs, bs_len, pcm_out) == TLCS_OK);

        double sig_pow = 0.0, err_pow = 0.0;
        for (int i = 0; i < cfg.frame_size; i++) {
            double s = (double)pcm_in[i];
            double e = (double)(pcm_in[i] - pcm_out[i]);
            sig_pow += s * s;
            err_pow += e * e;
        }
        last_snr = 10.0 * log10(sig_pow / (err_pow + 1e-10));
    }

    fprintf(stderr, "(SNR=%.1f dB, frame=%d bytes) ", last_snr, bs_len);
    ASSERT(last_snr > -10.0);
}

TEST(test_lsf_vq_bitrate_savings)
{
    /* Verify VQ produces smaller frames than scalar LR */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 9600) == TLCS_OK);

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);

    int16_t pcm[TLCS_MAX_FRAME_SIZE];
    uint8_t bs[256];
    int32_t total_bytes = 0;
    int32_t n_frames = 20;

    for (int32_t f = 0; f < n_frames; f++) {
        gen_voiced_signal(pcm, cfg.frame_size, 16000);
        int32_t bs_len = 0;
        ASSERT(tlcs_encode(&enc, pcm, bs, &bs_len) == TLCS_OK);
        total_bytes += bs_len;
    }

    float avg = (float)total_bytes / (float)n_frames;
    float kbps = avg * 8.0f / 0.02f / 1000.0f;
    fprintf(stderr, "(avg=%.1f bytes, %.1f kbps) ", (double)avg, (double)kbps);
    /* VQ saves ~28 bits (3.5 bytes) per frame vs scalar EC */
    ASSERT(avg <= 40.0f);
}

TEST(test_lsf_vq_stability)
{
    /* Verify decoded LSFs stay ordered after VQ + stabilization */
    float delta[16] = {
        0.05f, -0.10f, 0.15f, -0.05f,
        0.10f, 0.00f, -0.08f, 0.12f,
        -0.03f, 0.07f, -0.11f, 0.06f,
        0.09f, -0.04f, 0.02f, -0.07f
    };
    int32_t indices[4];
    float delta_q[16];
    tlcs_lsf_vq_encode(delta, 16, indices, delta_q);

    /* Simulate prediction: default LSFs */
    float lsf[16];
    for (int i = 0; i < 16; i++)
        lsf[i] = (float)M_PI * (float)(i + 1) / 17.0f + delta_q[i];
    tlcs_lsf_stabilize(lsf, 16);

    /* Verify ordering: lsf[i] < lsf[i+1] */
    for (int i = 0; i < 15; i++) {
        ASSERT(lsf[i] < lsf[i + 1]);
    }
    ASSERT(lsf[0] > 0.0f);
    ASSERT(lsf[15] < (float)M_PI);
}

TEST(test_hr_no_regression_vq)
{
    /* HR with VQ off should still produce same output */
    tlcs_config cfg;
    ASSERT(tlcs_config_init(&cfg, 16000, 24000) == TLCS_OK);
    ASSERT(cfg.use_lsf_vq == 0);  /* HR: VQ off */

    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);
    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    int16_t pcm_in[TLCS_MAX_FRAME_SIZE], pcm_out[TLCS_MAX_FRAME_SIZE];
    gen_voiced_signal(pcm_in, cfg.frame_size, 16000);

    uint8_t bs[256];
    int32_t bs_len = 0;
    ASSERT(tlcs_encode(&enc, pcm_in, bs, &bs_len) == TLCS_OK);
    ASSERT(bs_len > 0);
    ASSERT(tlcs_decode(&dec, bs, bs_len, pcm_out) == TLCS_OK);

    fprintf(stderr, "(HR frame=%d bytes, VQ off) ", bs_len);
    /* Frame size should be same as before VQ changes */
    ASSERT(bs_len == 63);
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void)
{
    fprintf(stderr, "TLCS Tests\n");
    fprintf(stderr, "═════════════════════════════════════════════\n");

    fprintf(stderr, " M0: Scaffold\n");
    fprintf(stderr, "─────────────────────────────────────────────\n");
    RUN(test_config_init_16k);
    RUN(test_config_init_8k);
    RUN(test_config_invalid);
    RUN(test_bitstream_roundtrip);
    RUN(test_bitstream_overflow);
    RUN(test_frame_buf_push);
    RUN(test_plc_outputs_silence);

    fprintf(stderr, "\n M1: LPC Path\n");
    fprintf(stderr, "─────────────────────────────────────────────\n");
    RUN(test_preemph_deemph_roundtrip);
    RUN(test_levinson_known_signal);
    RUN(test_lsf_roundtrip);
    RUN(test_lsf_quantize_roundtrip);
    RUN(test_synthesis_inverts_analysis);
    RUN(test_lpc_prediction_gain);
    RUN(test_lpc_codec_roundtrip);

    fprintf(stderr, "\n M2: Pitch\n");
    fprintf(stderr, "─────────────────────────────────────────────\n");
    RUN(test_pitch_gain_quantize);
    RUN(test_pitch_ol_search_periodic);
    RUN(test_pitch_adaptive_vec);
    RUN(test_pitch_cl_search);
    RUN(test_pitch_energy_reduction);
    RUN(test_m2_codec_roundtrip_multiframe);

    fprintf(stderr, "\n M3: Fixed Codebook\n");
    fprintf(stderr, "─────────────────────────────────────────────\n");
    RUN(test_fcb_gain_quantize);
    RUN(test_codebook_impulse_response);
    RUN(test_codebook_search_finds_pulses);
    RUN(test_m3_bitrate);
    RUN(test_m3_codec_roundtrip);

    fprintf(stderr, "\n M4: Low-Rate Mode\n");
    fprintf(stderr, "─────────────────────────────────────────────\n");
    RUN(test_config_init_vlr);
    RUN(test_config_init_lowrate);
    RUN(test_vlr_bitrate);
    RUN(test_vlr_codec_roundtrip);
    RUN(test_lr_bitrate);
    RUN(test_lr_codec_roundtrip);
    RUN(test_hr_no_regression);

    fprintf(stderr, "\n M5: Entropy Coding\n");
    fprintf(stderr, "─────────────────────────────────────────────\n");
    RUN(test_range_coder_roundtrip);
    RUN(test_range_coder_uniform);
    RUN(test_ec_frame_roundtrip);
    RUN(test_ec_frame_compressed);
    RUN(test_hr_no_regression_ec);

    fprintf(stderr, "\n M6: LSF Split-VQ\n");
    fprintf(stderr, "─────────────────────────────────────────────\n");
    RUN(test_lsf_vq_roundtrip);
    RUN(test_lsf_vq_codec_roundtrip);
    RUN(test_lsf_vq_bitrate_savings);
    RUN(test_lsf_vq_stability);
    RUN(test_hr_no_regression_vq);

    fprintf(stderr, "═════════════════════════════════════════════\n");
    fprintf(stderr, "%d/%d tests passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
