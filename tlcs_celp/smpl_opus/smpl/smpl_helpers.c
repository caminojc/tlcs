#include "smpl_codec_util.h"
#include "smpl_typedef.h"
#include "smpl_tables.h"
#include "smpl_defines.h"
#include <string.h>
#include <float.h>

static const float smpl_pulses2normalized_bitrate[2] = { 1.4f, 6.5f };
float smpl_get_normalized_bitrate(int num_pulses, int frame_length_16)
{
    float pulses_per_20ms = (num_pulses * frame_length_16) / (20.0f * 16.0f);
    return smpl_sigmoid(smpl_pulses2normalized_bitrate[0] * log2f(pulses_per_20ms + 1.0f) - smpl_pulses2normalized_bitrate[1]);
}

void smpl_dcmf_to_cmf(const uint8_t dcmf[], int dcmf_len, uint16_t cmf[])   // cmf should be of length dcmf_len+1
{
    // cmf = (Int.(dcmf) .+ 1) .^ 2
    // cmf .= (cmf .* (32767 - length(cmf))) .÷ sum(cmf) .+ 1
    // UInt16[0; cumsum(cmf)]
    int sum = 0;
    for (int n = 0; n < dcmf_len; n++) {
        int tmp = dcmf[n];
        tmp += 1;
        tmp *= tmp;
        if (tmp > 65535) tmp = 65535;
        cmf[n+1] = (uint16_t)tmp;
        sum += tmp;
    }
    cmf[0] = 0;
    for (int n = 1; n < dcmf_len + 1; n++) {
        cmf[n] = cmf[n-1] + ((int)(cmf[n]) * (32767 - dcmf_len)) / sum + 1;
    }
}

void smpl_cmf_to_bits(const uint16_t cmf[], int cmf_len, float bits[]) {
    for (int i = 0; i < cmf_len - 1; i++) {
        bits[i] = -log2f((cmf[i+1] - cmf[i]) / (float)cmf[cmf_len - 1]);
    }
}

void smpl_unpack8(const uint8_t* in, float* out, int n, float scale, float min) {
    for (int i = 0; i < n; i++) {
        out[i] = min + in[i] * scale;
    }
}

void smpl_unpack16(const uint16_t* in, float* out, int n, float scale, float min) {
    for (int i = 0; i < n; i++) {
        out[i] = min + in[i] * scale;
    }
}
