// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <cstdint>
#include <map>
#include <optional>

#define PI 3.14159265358979324
#define MAX_PCM_VAL 32768u
#define MIN_ENERGY_DB (-120)
#define ENG2DB(A) (10 * log10((A)))
#define DB2ENG(A) (pow(10, (A) / 10))

/**
 * Calculates energy spectrum of given PCM input using Hann window
 * and Kiss FFT with window size of 1024 samples.
 *
 * @param pcm_in Input PCM signal
 * @param input_size size of input PCM array
 * @param rate Sampling rate of the input signal
 *
 * @return Map from frequency bin -> dB level where frequency bins are
 *         from [0, rate / 2] and dB levels from [-127, 0].
 */
std::map<int, float>
calc_energy_spectrum(const int16_t* pcm_in, size_t input_size, uint16_t rate);

/**
 * Calculates total energy in given frequency band range from the energy
 * spectrum. This can be used to approximate the energy in a given
 * frequency range, and is not affected by the number of bins in the band.
 *
 * @param energy_spectrum distribution of energy across all frequencies,
 *        can be obtained using `calc_energy_spectrum`.
 * @param lower lower frequency bound
 * @param upper upper frequency bound
 *
 * @return an optional avg energy value. It will have a valid value if any
 *         energy band from [lower, upper) is found in given spectrum else
 *         empty optional is returned.
 */
std::optional<float> calc_energy_band(
    const std::map<int, float> energy_spectrum,
    uint16_t lower,
    uint16_t upper);

/**
 * Calculates total energy in a given PCM input.
 *
 * @param pcm_in Input PCM signal
 * @param input_size size of input PCM array
 *
 * @return an optional avg energy value. It will have a valid value if any
 *         energy band from [lower, upper) is found in given spectrum else
 *         empty optional is returned.
 */
float calc_energy(const int16_t* pcm_in, size_t input_size);

/**
 * Dumps given energy spectrum to stdout.
 */
void dump_energy_spectrum(const std::map<int, float>& spectrum);
