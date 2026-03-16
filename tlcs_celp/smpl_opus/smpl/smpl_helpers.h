#include <stdint.h>

float smpl_get_normalized_bitrate(int num_pulses, int frame_length_16);
void smpl_dcmf_to_cmf(const uint8_t dcmf[], int dcmf_len, uint16_t cmf[]);   // cmf must be of length dcmf_len+1
void smpl_cmf_to_bits(const uint16_t cmf[], int cmf_len, float bits[]);
void smpl_unpack8(const uint8_t* in, float* out, int n, float scale, float min);
void smpl_unpack16(const uint16_t* in, float* out, int n, float scale, float min);
