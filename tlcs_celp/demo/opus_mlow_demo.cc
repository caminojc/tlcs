// Sample program to process a WAV file using Opus or MLow at same settings used
// by WhatsApp.
#include <algorithm>
#include <cstdio>
#include <errno.h>
#include <fstream>
#include <iostream>
#include <math.h>
#include <numeric>
#include <vector>

#include "opus.h"
#include <AudioFile.h>
#include <webrtc_utils.h>

#define kFrameDuration 60
#define kNumChannels 1
#define kSampleSizeBytes 2
#define kFramesPerSec (1000 / kFrameDuration)

#define THIS_FILE "opus_mlow_demo.cc"

#define OPUS_ENCODER_CTL(enc, param)           \
  {                                            \
    int status = opus_encoder_ctl(enc, param); \
    assert(OPUS_BAD_ARG != status);            \
  }
#define OPUS_DECODER_CTL(dec, param)           \
  {                                            \
    int status = opus_decoder_ctl(dec, param); \
    assert(OPUS_BAD_ARG != status);            \
  }

using namespace std;

struct CodecConfig {
  bool use_mlow; // Use MLow codec or Opus SILK.
  int target_bitrate; // Encoder target bitrate
  int complexity; // Encoder complexity
  bool enable_dtx; // Enable DTX feature or not
  int dtx_vad_threshold; // VAD detection threshold value which impacts DTX
  int non_speech_bitrate; // Bitrate for non active frames
  int loss_pct; // Packet loss percent for encoder inband FEC
  int sample_rate; // Sampling rate, default 16000
  int samples_per_frame; // Number of samples per frame
  int frame_size_bytes; // Size of one frame in bytes
  int mlow_sf_imp_factor; // MLow subframe importance factor
  int mlow_use_sp_act_flatner; // MLow use sp act flat mode
};

struct FrameDetails {
  int encoded_bytes; // Total number of encoded bytes (inband fec included if
                     // encoded)
  bool vad; // True if VAD detected activity in the frame
  bool nrg_active; // Frame active criteria similar to opus_demo (energy based)
  bool dtx; //  true if encoder is in DTX mode
  int encoded_inband_fec_bytes; // Number of bytes for inband FEC
};

struct Stats {
  std::vector<FrameDetails> frame_details;
};

float get_nrg(const int16_t* data, size_t len) {
  float sum = 0;
  for (size_t i = 0; i < len; ++i) {
    sum += data[i] * data[i];
  }
  return sum / len;
}

// Currently we are tracking bitrate for 960ms audio and this
// utility scales it to 1sec to correctly represent bps.
inline float scaleBps(float bps) {
  return bps * 1000.0 / (kFramesPerSec * kFrameDuration);
}

class WindowedRate {
 public:
  WindowedRate(int window_size)
      : window_size_(window_size), sum_(0), index_(0) {
    elems_.assign(window_size, 0);
  }

  void add(int val) {
    sum_ -= elems_[index_];
    elems_[index_] = val;
    sum_ += elems_[index_];
    index_ = (index_ + 1) % window_size_;
  }

  double get_bps() const {
    return scaleBps(sum_);
  }

 private:
  const int window_size_;
  int sum_;
  int index_;
  std::vector<int> elems_;
};

/**
 * Helper class to parse command line args.
 *
 * It removes the args which are retrieved so one can
 * check unprocessed/invalid args.
 */
class InputParser {
 public:
  InputParser(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
      tokens_.push_back(std::string(argv[i]));
  }

  int getCmdOption(const std::string& option, int def_value) {
    auto itr = std::find(tokens_.begin(), tokens_.end(), option);
    if (itr != tokens_.end() && itr + 1 != tokens_.end()) {
      int val = atoi((itr + 1)->c_str());
      tokens_.erase(itr, itr + 2);
      return val;
    }
    return def_value;
  }

  std::string getCmdOption(
      const std::string& option,
      const std::string& def_value) {
    auto itr = std::find(tokens_.begin(), tokens_.end(), option);
    if (itr != tokens_.end() && itr + 1 != tokens_.end()) {
      std::string val = *(itr + 1);
      tokens_.erase(itr, itr + 2);
      return val;
    }
    return def_value;
  }

  bool cmdOptionExists(const std::string& option) {
    auto itr = std::find(tokens_.begin(), tokens_.end(), option);
    if (itr != tokens_.end()) {
      tokens_.erase(itr);
      return true;
    }
    return false;
  }

  const std::vector<std::string>& getUnusedArgs() const {
    return tokens_;
  }

 private:
  std::vector<std::string> tokens_;
};

/**
 * Opus demo class to configure underlying Opus codec and perform
 * encode/decode operation on a given PCM input frame.
 */
class OpusMLowDemo {
 public:
  OpusMLowDemo(const CodecConfig& config);

  ~OpusMLowDemo();

  // Processes given input of PCM samples and populates output.
  void process(
      const int16_t* input,
      int16_t* output,
      const int in_samples_count);

  Stats getStats() const {
    return stats_;
  }

 private:
  const CodecConfig config_; // Use MLow speech codec if MLow is compiled in.
  OpusEncoder* encoder_;
  OpusDecoder* decoder_;
  Stats stats_;

  std::vector<uint8_t> encoded_buffer_;
};

OpusMLowDemo::OpusMLowDemo(const CodecConfig& config) : config_(config) {
  opus_global_create();

  int error;
  encoder_ = opus_encoder_create(
      config.sample_rate, kNumChannels, OPUS_APPLICATION_VOIP, &error);
  assert(OPUS_OK == error);
  decoder_ = opus_decoder_create(config.sample_rate, kNumChannels, &error);
  assert(OPUS_OK == error);

  OPUS_ENCODER_CTL(encoder_, OPUS_SET_USING_SMPL(config_.use_mlow ? 1 : 0));
  OPUS_DECODER_CTL(decoder_, OPUS_SET_USING_SMPL(config_.use_mlow ? 1 : 0));

  // Configure all the default settings.
  OPUS_ENCODER_CTL(encoder_, OPUS_SET_BITRATE(config_.target_bitrate));
  OPUS_ENCODER_CTL(encoder_, OPUS_SET_DTX(config_.enable_dtx ? 1 : 0));
  OPUS_ENCODER_CTL(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  OPUS_ENCODER_CTL(encoder_, OPUS_SET_VBR(1));
  OPUS_ENCODER_CTL(encoder_, OPUS_SET_COMPLEXITY(config_.complexity));
  OPUS_ENCODER_CTL(encoder_, OPUS_SET_FORCE_CHANNELS(kNumChannels));
  OPUS_ENCODER_CTL(encoder_, OPUS_SET_INBAND_FEC(1));
  OPUS_ENCODER_CTL(encoder_, OPUS_SET_PACKET_LOSS_PERC(config_.loss_pct));
  OPUS_ENCODER_CTL(encoder_, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
  OPUS_ENCODER_CTL(
      encoder_, OPUS_SET_MLOW_SUBFRAME_IMP(config_.mlow_sf_imp_factor));
  OPUS_ENCODER_CTL(
      encoder_, OPUS_SET_MLOW_USE_SP_ACT_FLAT(config_.mlow_use_sp_act_flatner));

  // TODO(jatin): Port these two internal extensions.
  // if (config_.non_speech_bitrate != 0) {
  //   OPUS_ENCODER_CTL(
  //       encoder_, OPUS_SET_WA_NON_SPEECH_BITRATE(config_.non_speech_bitrate));
  // }
  // if (config_.dtx_vad_threshold != 0) {
  //   OPUS_ENCODER_CTL(
  //       encoder_,
  //       OPUS_SET_WA_SPEECH_ACTIVITY_DTX_THRES(config_.dtx_vad_threshold));
  // }

  memset(&stats_, 0, sizeof(Stats));
  encoded_buffer_.resize(config.frame_size_bytes);
}

OpusMLowDemo::~OpusMLowDemo() {
  opus_encoder_destroy(encoder_);
  opus_decoder_destroy(decoder_);
  opus_global_free();
}

void OpusMLowDemo::process(
    const int16_t* input,
    int16_t* output,
    const int in_samples_count) {
  // Encode the input frame and save to encoded_buffer_.
  const int encoded_bytes = opus_encode(
      encoder_,
      (const opus_int16*)input,
      in_samples_count,
      encoded_buffer_.data(),
      encoded_buffer_.capacity());
  assert(encoded_bytes > 0);

  // Track frame properties
  int in_dtx = 0;
  OPUS_ENCODER_CTL(encoder_, OPUS_GET_IN_DTX(&in_dtx));
  if (encoded_bytes <= 2) {
    assert(in_dtx == 1);
  }
  const bool dtx = in_dtx == 1;
  int fec_size = 0;
  // OPUS_ENCODER_CTL(encoder_, OPUS_GET_LAST_ENCODED_FEC_AUDIO_BYTES(&fec_size));

  bool vad = false;
  bool fec = false;
  if (!dtx) {
    if (config_.use_mlow) {
      vad = (encoded_buffer_[0] >> 6 == 1) || (encoded_buffer_[0] >> 6 == 3);
    } else {
      vad = WebRtcOpus_PacketHasVoiceActivity(
                encoded_buffer_.data(), encoded_bytes) != 0;
    }

    if (config_.use_mlow) {
      fec = ((encoded_buffer_[0] & 0x2) == 0x2) &&
          (((encoded_buffer_[0] >> 6) & 0x1) == 0x1);
    } else {
      fec = WebRtcOpus_PacketHasFec(encoded_buffer_.data(), encoded_bytes) > 0;
    }
  }
  const bool nrg_active = get_nrg(input, in_samples_count) > 1e5;

  // Decode and save to output.
  const int decoded_samples = opus_decode(
      decoder_,
      encoded_buffer_.data(),
      encoded_bytes,
      (opus_int16*)output,
      in_samples_count,
      0);
  assert(decoded_samples == in_samples_count);

  // Update stats
  stats_.frame_details.push_back({encoded_bytes, vad, nrg_active, dtx, fec_size});
}

/**
 * Runner class to perform WAV file read/write in chunks and invoke
 * the Opus/MLow codec for encoding/decoding.
 *
 * NOTE: This can potentially be simplified to not use PJMedia WAV port.
 */
class Runner {
 public:
  Runner(
      const CodecConfig& config,
      const std::string& input_wav_file,
      const std::string& output_wav_file)
      : output_wav_file_(output_wav_file) {
    AudioFile<int16_t> wav_reader;
    wav_reader.load(input_wav_file.c_str());
    assert(wav_reader.isMono()); // mono only for now.
    int index = 0;
    input_buffer_ = wav_reader.samples[0];

    wav_writer_.setNumChannels(1);
    wav_writer_.setSampleRate(wav_reader.getSampleRate());
    output_buffer_.resize(wav_reader.getNumSamplesPerChannel());
  }

  ~Runner() {
    std::vector<std::vector<int16_t>> temp_buf = {output_buffer_};
    wav_writer_.setAudioBuffer(temp_buf);
    wav_writer_.save(output_wav_file_.c_str());
  }

  void run(OpusMLowDemo& codec_object, const CodecConfig& config) {
    for (int idx = 0; idx + config.samples_per_frame < input_buffer_.size();
         idx += config.samples_per_frame) {
      codec_object.process(
          input_buffer_.data() + idx,
          output_buffer_.data() + idx,
          config.samples_per_frame);
    }
  }

 private:
  const std::string output_wav_file_;

  AudioFile<int16_t> wav_writer_;
  std::vector<int16_t> input_buffer_;
  std::vector<int16_t> output_buffer_;
};

struct BitrateStat {
  float avg;
  float min;
  float max;
  float stddev;
};

// Given a list of frames sizes, compute the bitrate stats using 1 minute
// window. Return {avg, min, max, stddev}
BitrateStat computeBitrates(const std::vector<int>& sizes) {
  const static int kWinSize = kFramesPerSec; // no. of frames in 1 second ~ 16
  if (sizes.size() < kWinSize) {
    return {0, 0, 0, 0};
  }

  std::vector<float> bitrates;
  for (auto it = sizes.begin(); it + kWinSize < sizes.end(); ++it) {
    bitrates.push_back(scaleBps(std::accumulate(it, it + kWinSize, 0) * 8));
  }

  // basic stats on bitrates vector
  const float min_v = *std::min_element(bitrates.begin(), bitrates.end());
  const float max_v = *std::max_element(bitrates.begin(), bitrates.end());
  const float avg_v =
      std::accumulate(bitrates.begin(), bitrates.end(), 0) / bitrates.size();

  // stddev of bitrates vector
  float sum = 0;
  for (auto v : bitrates) {
    sum += (v - avg_v) * (v - avg_v);
  }
  const float stddev = sqrt(sum / bitrates.size());

  return {avg_v, min_v, max_v, stddev};
}

// clang-format off
void dumpStats(const Stats& stats, const std::string& frame_trace_file) {
  std::cout << "------------------- Stats Summary -------------------\n";

  // Helper method to evaluate bitrate stats for a given frame filter lambda.
  typedef bool (*FilterFunc)(const FrameDetails&);
  auto eval_helper = [&](FilterFunc lambda, std::string label, bool fec = false) {
    std::vector<int> frame_sizes;
    for (auto frame : stats.frame_details) {
      if (lambda(frame)) {
        frame_sizes.push_back(fec ? frame.encoded_inband_fec_bytes : frame.encoded_bytes);
      }
    }
    auto stats = computeBitrates(frame_sizes);
    printf(
        "%10s\t%7ld\t%7.0f\t%7.0f\t%7.0f\t%7.0f\n",
        label.c_str(), frame_sizes.size(),
        stats.avg, stats.min, stats.max, stats.stddev);
  };

  printf("%10s\t%7s\t%7s\t%7s\t%7s\t%7s\n", "State", "Count", "Avg", "Min", "Max", "StdDev");
  eval_helper([](const FrameDetails& f) { return f.vad; }, "Active Tot");
  eval_helper([](const FrameDetails& f) { return f.nrg_active; }, "Energy Act");
  eval_helper([](const FrameDetails& f) { return !f.dtx && !f.vad; }, "Non Active");
  eval_helper([](const FrameDetails& f) { return f.dtx; }, "DTX");
  // eval_helper([](const FrameDetails& f) { return f.encoded_inband_fec_bytes != 0; }, "FEC Only", true);

  if (!frame_trace_file.empty()) {
    // Open and write a header to TSV file and then the values.
    std::cout << "Dumping trace to: " << frame_trace_file << "\n";
    
    std::ofstream tsv_file;
    tsv_file.open(frame_trace_file, std::ofstream::out | std::ios::trunc);
    tsv_file << "Index\tVAD\tDTX\tFEC Bytes\tFEC Bitrate\tTotal Bytes\tTotal Bitrate\n";
    
    WindowedRate total_rate{kFramesPerSec};
    WindowedRate fec_rate{kFramesPerSec};
    int index = 1;
    for (auto frame : stats.frame_details) {
      total_rate.add(frame.encoded_bytes * 8);
      fec_rate.add(frame.encoded_inband_fec_bytes * 8);
      tsv_file << index++ << "\t" << frame.vad << "\t" << frame.dtx << "\t"
               << frame.encoded_inband_fec_bytes << "\t" << fec_rate.get_bps()
               << "\t" << frame.encoded_bytes << "\t" << total_rate.get_bps()
               << "\n";
    }
    tsv_file.close();
  }
}

void printUsage(char* progName) {
  std::cerr << "Usage: " << progName << " <options> <input_wav_file> <output_wav_file>\n";
  std::cerr << "options:\n";
  std::cerr << " -sample_rate <sample_rate>   : sample rate to test (default 16000)\n";
  std::cerr << " -bitrate <bitrate>           : bitrates to test (default 25000)\n";
  std::cerr << " -complexity <complexity>     : encoder complexity (default 5)\n";
  std::cerr << " -use_mlow                    : use MLow speech codec (default SILK)\n";
  std::cerr << " -no_dtx                      : Disable DTX feature (default enabled)\n";
  // std::cerr << " -dtx_vad_threshold           : DTX VAD threshold to use (default unchanged (5))\n";
  // std::cerr << " -non_speech_bitrate          : Non speech bitrate (default unchanged)\n";
  std::cerr << " -loss_pct                    : Packet loss percent for inband FEC (default 0)\n";
  std::cerr << " -mlow_sf_imp_factor          : MLow subframe importance factor (default 20)\n";
  std::cerr << " -mlow_use_sp_act_flatner     : MLow use speech activity flatner (default disabled)\n";
  std::cerr << " -trace_file <file_name.tsv>  : Dump frame level trace into a TSV file\n";
  return;
}
// clang-format on

int main(int argc, char* argv[]) {
  // Parse positional arguments.
  if (argc < 2) {
    printUsage(argv[0]);
    exit(0);
  }
  const std::string input_wav_file = argv[argc - 2];
  const std::string output_wav_file = argv[argc - 1];
  std::remove(output_wav_file.c_str());

  // Parse optional arguments.
  CodecConfig config;
  InputParser parser(argc - 2, argv);
  config.target_bitrate = parser.getCmdOption("-bitrate", 25000);
  config.sample_rate = parser.getCmdOption("-sample_rate", 16000);
  config.samples_per_frame = config.sample_rate * kFrameDuration / 1000;
  config.frame_size_bytes = config.samples_per_frame * kSampleSizeBytes;
  config.complexity = parser.getCmdOption("-complexity", 5);
  config.use_mlow = parser.cmdOptionExists("-use_mlow");
  config.enable_dtx = !parser.cmdOptionExists("-no_dtx");
  config.dtx_vad_threshold = parser.getCmdOption("-dtx_vad_threshold", 0);
  config.non_speech_bitrate = parser.getCmdOption("-non_speech_bitrate", 0);
  config.loss_pct = parser.getCmdOption("-loss_pct", 0);
  config.mlow_sf_imp_factor = parser.getCmdOption("-mlow_sf_imp_factor", 20);
  config.mlow_use_sp_act_flatner =
      parser.cmdOptionExists("-mlow_use_sp_act_flatner");
  const std::string trace_file = parser.getCmdOption("-trace_file", "");

  auto& unused = parser.getUnusedArgs();
  if (unused.size() > 0) {
    std::cerr << "Error: Unknown options: " << unused[0] << "\n";
    printUsage(argv[0]);
    exit(0);
  }

  Runner runner{config, input_wav_file, output_wav_file};
  OpusMLowDemo codec_object{config};
  runner.run(codec_object, config);

  dumpStats(codec_object.getStats(), trace_file);
  return 0;
}
