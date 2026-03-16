// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <gtest/gtest.h>
#include <cstdio>
#include "base.h"
#include <vector>

#include <fstream>

#define MAX_REPACKED_SIZE 5000
#define MAX_DURATION_MS 120

// This test encodes individual frames and then repacketizes them using
// OpusRepacketizer. For decode part, we do two checks:
// 1. Decoding whole chunk in one go.
// 2. Parsing out valid frames back from repacketized packet and then
//    decoding each of them separately.
TEST_P(OpusSmplTestDefault, EncodePackDecode) {
  const int kDecBufSize = input_pcm_info_.samples_per_frame * MAX_DURATION_MS /
      params_.frame_size_ms;

  // Repacketizer DOESN'T copy the frame buffers offered via cat() calls and
  // the caller is supposed to maintain the original encoded buffers until the
  // the out() call. So we maintain a vector of encoded buffers here.
  std::vector<std::vector<unsigned char>> enc_bufs(
      MAX_DURATION_MS / params_.frame_size_ms);
  for (auto& elem : enc_bufs) {
    elem.resize(input_pcm_info_.bytes_per_frame);
  }

  // We use `decoder_` for decoding individual frames and use a new
  // `multi_decoder` for decoding the bundled multi frame packet. We use
  // separate decoder to not mess up the decoder state.
  OpusDecoder* multi_decoder = opus_decoder_create(
      (int)params_.codec_sample_rate,
      (int)input_pcm_info_.num_channels,
      nullptr);
  ASSERT_TRUE(multi_decoder != nullptr);
  opus_decoder_ctl(multi_decoder, OPUS_SET_USING_SMPL(GetUsingMLow()));

  // Create repacketizer instance.
  OpusRepacketizer* rp = opus_repacketizer_create();
  opus_repacketizer_init(rp);
  opus_repacketizer_set_using_mlow(rp, GetUsingMLow() ? 1 : 0);

  int pack_cnt = 0;
  unsigned char last_toc;

  // Encode/packetize/decode loop
  for (size_t start = 0;
       start + input_pcm_info_.samples_per_frame < num_input_samples_;
       start += input_pcm_info_.samples_per_frame) {
    auto& enc_buf = enc_bufs[pack_cnt];
    enc_buf.resize(input_pcm_info_.bytes_per_frame);
    const int encoded_size = opus_encode(
        encoder_,
        input_pcm_.data() + start,
        (int)input_pcm_info_.samples_per_frame,
        enc_buf.data(),
        (int)input_pcm_info_.bytes_per_frame);
    EXPECT_GE(encoded_size, 0);
    enc_buf.resize(encoded_size);
    int ret = opus_repacketizer_cat(rp, enc_buf.data(), encoded_size);
    // TOC change causes repack to fail. For Opus this is because TOC byte
    // is shared among all the frames to save space. For MLow although this
    // is not the case but we still enforce this constraint.
    EXPECT_TRUE(ret == OPUS_OK || last_toc != enc_buf[0]);
    last_toc = enc_buf[0];
    pack_cnt += (ret == OPUS_OK) ? 1 : 0;
    const int packaged_ms = pack_cnt * params_.frame_size_ms;

    // Only continue repacking if last repack operation was successful and
    // we haven't packed upto the maximum size we want to test for.
    if (ret == OPUS_OK &&
        packaged_ms + params_.frame_size_ms <= MAX_DURATION_MS) {
      continue;
    }

    /*********************************************
     * Let's validate the repacketized data here *
     *********************************************/
    std::vector<unsigned char> repack_out(MAX_REPACKED_SIZE);
    // Get the repacketized data out and reset it.
    {
      int out = opus_repacketizer_out(
          rp, repack_out.data(), (int)repack_out.capacity());
      repack_out.resize(out);
      EXPECT_GT(out, 0);
    }
    opus_repacketizer_init(rp);
    opus_repacketizer_set_using_mlow(rp, GetUsingMLow() ? 1 : 0);
    std::vector<int16_t> buf_dec_multi(kDecBufSize);
    std::vector<int16_t> buf_dec(kDecBufSize);

    // 1. Validate that we can decode the whole repacketized data.
    {
      int num_samples = opus_decode(
          multi_decoder,
          repack_out.data(),
          repack_out.size(),
          buf_dec_multi.data(),
          kDecBufSize,
          0);
      EXPECT_EQ(num_samples, input_pcm_info_.samples_per_frame * pack_cnt);
    }

    // 2. Validate that we can parse out valid packets from repacked data.
    EXPECT_EQ(
        OPUS_OK,
        opus_repacketizer_cat(rp, repack_out.data(), repack_out.size()));
    const int num_frames = opus_repacketizer_get_nb_frames(rp);
    // There can be more frames than we packaged because for some configuration
    // each frame we are packaging can already contain multiple sub frames.
    EXPECT_GE(num_frames, packaged_ms / params_.frame_size_ms);

    for (int index = 0; index < num_frames; index++) {
      std::vector<unsigned char> ebuf(input_pcm_info_.bytes_per_frame);
      const int out = opus_repacketizer_out_range(
          rp, index, index + 1, ebuf.data(), (int)ebuf.capacity());
      ebuf.resize(out);

      // Make sure it is exactly what we submitted to repacketizer.
      // Doing it for the case when the two are equal o/w it is bit
      // complicated and needs more state tracking.
      if (num_frames == pack_cnt) {
        EXPECT_EQ(ebuf.size(), enc_bufs[index].size());
        EXPECT_TRUE(
            std::equal(ebuf.begin(), ebuf.end(), enc_bufs[index].begin()));
      }

      const int out_samples = opus_decode(
          decoder_, ebuf.data(), ebuf.size(), buf_dec.data(), kDecBufSize, 0);
      EXPECT_EQ(
          out_samples,
          input_pcm_info_.samples_per_frame * pack_cnt / num_frames);
      buf_dec.resize(out_samples);

      // We expect the PCM samples to be very very similar.
      int mismatch = 0;
      for (int i = 0; i < buf_dec.size(); i++) {
        mismatch += (buf_dec[i] != buf_dec_multi[index * out_samples + i]);
      }
      EXPECT_NEAR(mismatch, 0, out_samples * 3.0 / 100); // 3% samples different
    }
    // Decoders should be in exactly same state at this point as they have
    // decoded the exactly same payload(s).
    {
      opus_uint32 r1, r2;
      opus_decoder_ctl(decoder_, OPUS_GET_FINAL_RANGE(&r1));
      opus_decoder_ctl(multi_decoder, OPUS_GET_FINAL_RANGE(&r2));
      EXPECT_EQ(r1, r2);
    }

    // 3. Done with validation. Now reset repacketizer and append the last
    // frame again if we failed to append before.
    opus_repacketizer_init(rp);
    opus_repacketizer_set_using_mlow(rp, GetUsingMLow() ? 1 : 0);
    if (ret != OPUS_OK) {
      enc_bufs[0].assign(enc_bufs[pack_cnt].begin(), enc_bufs[pack_cnt].end());
      EXPECT_EQ(
          OPUS_OK,
          opus_repacketizer_cat(rp, enc_bufs[0].data(), enc_bufs[0].size()));
      pack_cnt = 1;
      last_toc = enc_bufs[0][0];
    } else {
      pack_cnt = 0;
    }
  }

  opus_decoder_destroy(multi_decoder);
  opus_repacketizer_destroy(rp);
}

// Test to ensure that Opus Repack operation fails when TOC changes.
TEST(OpusSmplTestDefault, OpusRepackTocChange) {
  // Dummy Opus frames.
  const std::vector<unsigned char> kOpus_16KHz_20ms = {9 << 3, 0xFF};
  const std::vector<unsigned char> kOpus_16KHz_10ms = {8 << 3, 0xFF};

  auto append = [](OpusRepacketizer* rp,
                   const std::vector<unsigned char>& frame) {
    return opus_repacketizer_cat(rp, frame.data(), frame.size());
  };

  OpusRepacketizer* rp = opus_repacketizer_create();
  opus_repacketizer_init(rp);
  // Append packet with same TOC twice, no issues.
  EXPECT_EQ(OPUS_OK, append(rp, kOpus_16KHz_20ms));
  EXPECT_EQ(OPUS_OK, append(rp, kOpus_16KHz_20ms));
  EXPECT_EQ(2, opus_repacketizer_get_nb_frames(rp));

  // Append packet with different TOC, operation fails.
  EXPECT_EQ(OPUS_INVALID_PACKET, append(rp, kOpus_16KHz_10ms));
  EXPECT_EQ(OPUS_INVALID_PACKET, append(rp, kOpus_16KHz_10ms));
  EXPECT_EQ(2, opus_repacketizer_get_nb_frames(rp));

  // Resume appending with same TOC and it passes again.
  EXPECT_EQ(OPUS_OK, append(rp, kOpus_16KHz_20ms));
  EXPECT_EQ(3, opus_repacketizer_get_nb_frames(rp));
}

// Test to ensure that MLow Repack operation fails when TOC changes.
TEST(OpusSmplTestDefault, MLowRepackTocChange) {
  // Dummy MLow frames.
  const std::vector<unsigned char> kMLow_16KHz_60ms = {0x10, 0xFF};
  const std::vector<unsigned char> kMLow_32KHz_60ms = {0x30, 0xFF};

  auto append = [](OpusRepacketizer* rp,
                   const std::vector<unsigned char>& frame) {
    return opus_repacketizer_cat(rp, frame.data(), frame.size());
  };

  OpusRepacketizer* rp = opus_repacketizer_create();
  opus_repacketizer_init(rp);
  opus_repacketizer_set_using_mlow(rp, 1);

  // Append packet with same TOC twice, no issues.
  EXPECT_EQ(OPUS_OK, append(rp, kMLow_16KHz_60ms));
  EXPECT_EQ(OPUS_OK, append(rp, kMLow_16KHz_60ms));
  EXPECT_EQ(2, opus_repacketizer_get_nb_frames(rp));

  // Append packet with different TOC, operation fails.
  EXPECT_EQ(OPUS_INVALID_PACKET, append(rp, kMLow_32KHz_60ms));
  EXPECT_EQ(OPUS_INVALID_PACKET, append(rp, kMLow_32KHz_60ms));
  EXPECT_EQ(2, opus_repacketizer_get_nb_frames(rp));

  // Resume appending with same TOC and it passes again.
  EXPECT_EQ(OPUS_OK, append(rp, kMLow_16KHz_60ms));
  EXPECT_EQ(3, opus_repacketizer_get_nb_frames(rp));

  // MLow TOC check is should not compare volatile TOC info like
  // {FEC, VAD, SID}.
  opus_repacketizer_init(rp);
  opus_repacketizer_set_using_mlow(rp, 1);

  const std::vector<unsigned char> kMLow_16KHz_60ms_FEC = {0x12, 0xFF};
  EXPECT_EQ(OPUS_OK, append(rp, kMLow_16KHz_60ms_FEC));
  EXPECT_EQ(1, opus_repacketizer_get_nb_frames(rp));

  const std::vector<unsigned char> kMLow_16KHz_60ms_VAD = {0x50, 0xFF};
  EXPECT_EQ(OPUS_OK, append(rp, kMLow_16KHz_60ms_VAD));
  EXPECT_EQ(2, opus_repacketizer_get_nb_frames(rp));

  const std::vector<unsigned char> kMLow_16KHz_60ms_SID = {0x90, 0xFF};
  EXPECT_EQ(OPUS_OK, append(rp, kMLow_16KHz_60ms_SID));
  EXPECT_EQ(3, opus_repacketizer_get_nb_frames(rp));
}
