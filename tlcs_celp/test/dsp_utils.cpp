// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "dsp_utils.h"
#include <math.h>
#include <algorithm>
#include <kissfft.hh>

// FFT window size in no. of samples
#define NUM_SAMPLES 1024u
#define HALF (NUM_SAMPLES / 2)
// FFT frequency output bins.
#define NUM_BINS (HALF + 1)

// Implement PowerSpectrum from Audacity's SpectrumAnalyst::Calculate.
// https://github.com/audacity/audacity/blob/master/src/SpectrumAnalyst.cpp
std::map<int, float>
calc_energy_spectrum(const int16_t* pcm_in, size_t input_size, uint16_t rate) {
  // Calculate Hann window coeff upfront.
  float win[NUM_SAMPLES], win_sum = 0;
  for (size_t i = 0; i < NUM_SAMPLES; i++) {
    win[i] = 0.5 * (1 - cos(2 * PI * i / NUM_SAMPLES));
    win_sum += win[i];
  }
  // Scaling factor such that 1.0 in time domain shows 0dB in frequency domain.
  float wss = 4.0 / (win_sum * win_sum);

  // Compute FFT of size NUM_SAMPLES over each window and sum up the power
  // values. Initializing kissfft with NUM_SAMPLES/2 as we will use
  // fft.real_transform()
  kissfft<float> fft(NUM_SAMPLES / 2, false /* inverse */);

  size_t start = 0, num_windows = 0;

  float in[NUM_SAMPLES];
  std::complex<float> out[NUM_BINS];
  float energy[NUM_BINS] = {};
  while (start + NUM_SAMPLES <= input_size) {
    // Scale input PCM to float [-1.0 to 1.0] range and apply Hann window.
    for (size_t i = 0; i < NUM_SAMPLES; i++) {
      in[i] = pcm_in[start + i] * win[i] / MAX_PCM_VAL;
    }

    // Compute FFT and sum up energy for each frequency bin.
    fft.transform_real(in, out);
    for (size_t i = 1; i < HALF; i++) {
      energy[i] += std::norm(out[i]);
    }
    energy[0] += out[0].real() * out[0].real(); // DC bin
    energy[HALF] += out[0].imag() * out[0].imag(); // Nyquist bin

    start += HALF; // Shift window by 50% basically.
    num_windows++;
  }

  std::map<int, float> result;
  for (size_t i = 0; i < NUM_BINS; i++) {
    // - Divide by `num_windows` to take avg. energy across (summed above).
    // - Multiplication by `wss` is to scale dB value.
    // - Note we multiply by 10 and no 20 as we don't do sqrt above.
    auto db_val = ENG2DB(energy[i] * wss / num_windows);
    result[i * rate / NUM_SAMPLES] = db_val;
  }
  return result;
}

std::optional<float> calc_energy_band(
    const std::map<int, float> energy_spectrum,
    uint16_t lower,
    uint16_t upper) {
  float sum_energy = 0;
  // Convert from db to energy, sum up the energy, and then convert back to db
  for (auto eit = energy_spectrum.begin(); eit != energy_spectrum.end();
       eit++) {
    if (eit->first >= lower && eit->first < upper) {
      sum_energy += DB2ENG(eit->second);
    }
  }
  if (sum_energy > 0) {
    return ENG2DB(sum_energy);
  }
  return {};
}

float calc_energy(const int16_t* pcm_in, size_t input_size) {
  float pcm_energy = 0;
  for (size_t i = 0; i < input_size; i++) {
    float in_float = (float)pcm_in[i] / MAX_PCM_VAL;
    pcm_energy += in_float * in_float;
  }
  pcm_energy *= 1 / (float)input_size;
  return (pcm_energy) > 0 ? std::max<float>(ENG2DB(pcm_energy), MIN_ENERGY_DB)
                          : MIN_ENERGY_DB;
}
