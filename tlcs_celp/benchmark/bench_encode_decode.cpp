/**
 * Benchmarking code to evaluate computation complexity of Encoder and Decoder.
 * Please pay special attention to any file IO or heap memory allocations to not interfere.
 */
#include <fstream>
#include <vector>
#include "opus.h"
#include "stop_watch.h"
#include <AudioFile.h>
#include <chrono>
#include <assert.h>

/**
 * Wrapper class to initialize Opus_Silk or Opus_SMPL encoder/decoder with appropriate settings.
 */
class EncodeDecode
{
public:
    EncodeDecode(int input_rate, int output_rate, int bitrate, int complexity, bool enc_dec_fec, bool use_smpl, bool use_opus_auto_bw)
        : input_rate_(input_rate),
          output_rate_(output_rate),
          enc_dec_fec_(enc_dec_fec)
    {
#if defined(ENABLE_SMPL)
        opus_global_create();
#endif
        // Encoder settings.
        int bandwidth = OPUS_AUTO;
        if (!use_opus_auto_bw) {
            bandwidth = find_opus_bandwidth(input_rate);
        }

        encoder_ = opus_encoder_create(input_rate, 1, OPUS_APPLICATION_VOIP, NULL);
        assert(encoder_);
        decoder_ = opus_decoder_create(output_rate, 1, NULL);
        assert(decoder_);
        assert(opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate)) != OPUS_BAD_ARG);
        assert(opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(complexity)) != OPUS_BAD_ARG);
        assert(opus_encoder_ctl(encoder_, OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);
        assert(opus_encoder_ctl(encoder_, OPUS_SET_BANDWIDTH(bandwidth)) != OPUS_BAD_ARG);
        assert(opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(enc_dec_fec)) != OPUS_BAD_ARG);

        assert(opus_decoder_ctl(decoder_, OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);
    }

    ~EncodeDecode()
    {
        opus_encoder_destroy(encoder_);
        opus_decoder_destroy(decoder_);
#if defined(ENABLE_SMPL)
        opus_global_free();
#endif
    }

    /**
     * Don't do any heap allocations such that it doesn't hurt performance.
     */
    int runEncoder(const opus_int16 *pcm_buf, opus_int16 *payload_buf, int num_samples, int frame_length_ms)
    {
        int samples_per_frame = frame_length_ms * input_rate_ / 1000;
        for (int i = 0; i + samples_per_frame <= num_samples; i += samples_per_frame)
        {
            payload_buf[0] = opus_encode(encoder_, pcm_buf + i, samples_per_frame, (unsigned char *)&payload_buf[1], 1000);
            assert(payload_buf[0] >= 0);
            payload_buf += 1 + ((payload_buf[0] + 1) >> 1);
        }
        return(num_samples/samples_per_frame);
    }

    void runDecoder(const opus_int16 *payload_buf, int num_frames, int frame_length_ms)
    {
        int samples_per_frame = frame_length_ms * input_rate_ / 1000;
        opus_int16 dec_buf[6000];    // Based on mono, 120ms x 48KHz
        for (int i = 0; i < num_frames; i++)
        {
            auto payload_len = payload_buf[0];
            payload_buf++;
            auto res = opus_decode(decoder_, (const unsigned char *)payload_buf, payload_len, (opus_int16 *)&dec_buf, samples_per_frame, 0);
            assert(res == samples_per_frame);
            payload_buf += ((payload_len + 1) >> 1);
        }
    }

private:
    int find_opus_bandwidth(const int input_rate) {
        switch (input_rate)
        {
        case 8000:
            return OPUS_BANDWIDTH_NARROWBAND;
        case 16000:
            return OPUS_BANDWIDTH_WIDEBAND;
        case 24000:
        case 32000:
            return OPUS_BANDWIDTH_SUPERWIDEBAND;
        case 48000:
        case 44100:
            return OPUS_BANDWIDTH_FULLBAND;
        default:
            std::cerr << "Error: Unsupported input rate " << input_rate << std::endl;
            exit(1);
            break;
        }
    }

    OpusEncoder *encoder_;
    OpusDecoder *decoder_;
    int input_rate_;
    int output_rate_;
    bool enc_dec_fec_;
};



int printUsage(char *program)
{
    std::cerr << "Usage: " << program << " <options> <input_wav_file>\n";
    std::cerr << "options:\n";
    std::cerr << "   -iters <iters>                    : number of iterations to do on input file\n";
    std::cerr << "   -bitrates <items> <bitrates>      : bitrates to test on i.e. -bitrates 3 9000 15000 20000 for {9000 15000 20000}\n";
    std::cerr << "   -complexities <items> <bitrates>  : complexities to test on i.e. -complexities 5 1 3 5 8 10 for {1 3 5 8 10}\n";
    std::cerr << "   -csv <csvfile>                    : store data to CSV file\n";
    std::cerr << "   -csvappend <csvfile>              : append data to CSV file (only one of -csv and -csvappend should be specified)\n";
    std::cerr << "   -opus_auto_bw                     : Force Opus to use AUTO bandwidth (NB at lowest bitrates)\n";
    return 0;
};


/**
 * Run the benchmark for a given input wav file and iteration count.
 */
int main(int argc, char **argv)
{
#ifdef DEBUG
    printf("WARN: Running benchmark in debug mode!\n");
#endif
    FILE *CSVfile = NULL;
    std::vector<int> bitrates{6000, 8000, 10000, 15000, 25000, 32000};
    std::vector<int> complexities{1, 3, 5, 8, 10};
    bool appendCSV = false;
    bool opus_auto_bw = false;
    int iters = 5;
    int args = 0;
    if (argc < 2)
    {
        printUsage(argv[0]);
        exit(1);
    }
 
    args++;
    while( args < argc - 1 ) {
        /* process command line options */
        if( strcmp( argv[ args ], "-csv" ) == 0 ) {
            args++;
            if (CSVfile == NULL) {
                CSVfile = fopen(argv[ args ], "w");
            } else {
                fclose(CSVfile);
                printUsage(argv[0]);
                exit(0);
            }
            args++;
        } else if( strcmp( argv[ args ], "-csvappend" ) == 0 ) {
            args++;
            if (CSVfile == NULL) {
                CSVfile = fopen(argv[ args ], "a");
                appendCSV = true;
            } else {
                fclose(CSVfile);
                printUsage(argv[0]);
                exit(0);
            }
            args++;
        } else if( strcmp( argv[ args ], "-iters" ) == 0 ) {
            args++;
            iters = atoi(argv[ args ]);
            args++;
        } else if( strcmp( argv[ args ], "-bitrates" ) == 0 ) {
            args++;
            int items = atoi(argv[ args ]);
            if ((args + items) > (argc - 2)) {
                printUsage(argv[0]);
                exit(0);
            }
            bitrates.clear();
            args++;
            for (int i=0; i<items; i++) {
                bitrates.push_back(atoi(argv[ args ]));
                args++;
            }
        } else if( strcmp( argv[ args ], "-complexities" ) == 0 ) {
            args++;
            int items = atoi(argv[ args ]);
            if ((args + items) > (argc - 2)) {
                printUsage(argv[0]);
                exit(0);
            }
            complexities.clear();
            args++;
            for (int i=0; i<items; i++) {
                complexities.push_back(atoi(argv[ args ]));
                args++;
            }
        } else if (strcmp(argv[args], "-opus_auto_bw") == 0) {
            args++;
            opus_auto_bw = true;
        } else {
            printf( "Error: unrecognized setting: %s\n\n", argv[ args ] );
            printUsage(argv[0]);
            exit(0);
        }
    }
    if (args != argc-1) {
        printUsage(argv[0]);
        exit(0);
    }
    const auto input_wav = argv[ args ];

    /* Read input audio file. */
    AudioFile<int16_t> wavReader;
    wavReader.load(input_wav);
    assert(wavReader.isMono()); // mono only for now.
    wavReader.printSummary();
    opus_int16 *pcm_buf = wavReader.samples.front().data();
    opus_int16 *payload_buf = new opus_int16[wavReader.getNumSamplesPerChannel()]; // Well overallocated

    if (CSVfile && !appendCSV)
        fprintf(CSVfile, "File SampleRate Complexity Bitrate Iters FrameSize SMPL us/sec Enc Dec\n");
    /* Initialize the benchmark and run it */
    for (auto use_smpl : {false, true})
    {
        for (auto bitrate : bitrates)
        {
            for (auto complexity : complexities)
            {
                for (auto frame_size: {20}) {   // Seems like this doesn't matter much?
                    EncodeDecode runner(wavReader.getSampleRate(), wavReader.getSampleRate(), bitrate, complexity, 1, use_smpl, opus_auto_bw);
                    StopWatch timer_enc, timer_dec;

                    // TODO: Currently we only benchmark both encode/decode together. Refactor as required.
                    for (int i = 0; i < iters; i++)
                    {
                        timer_enc.start();
                        auto no_frames = runner.runEncoder(pcm_buf, payload_buf, wavReader.getNumSamplesPerChannel(), frame_size);
                        timer_enc.stop();
                        timer_dec.start();
                        runner.runDecoder(payload_buf, no_frames, frame_size);
                        timer_dec.stop();
                    }
                    printf(
                        "File: %30s\tSampleRate: %5d\tComplexity: %2d\tBitrate: %5d\tIters: %2d\tFrameSize: %2d\tSMPL: %d => %7.2f us/sec\t(Enc: %7.2f us/sec Dec: %7.2f us/sec)\n",
                        input_wav, wavReader.getSampleRate(), complexity, bitrate, iters, frame_size, use_smpl,
                        (timer_enc.avg_lap_time().count() + timer_dec.avg_lap_time().count()) / wavReader.getLengthInSeconds(), 
                        timer_enc.avg_lap_time().count() / wavReader.getLengthInSeconds(),
                        timer_dec.avg_lap_time().count() / wavReader.getLengthInSeconds());
                    if (CSVfile)
                        fprintf(CSVfile, "%s %d %d %d %d %d %d %.2f %.2f %.2f\n",
                            input_wav, wavReader.getSampleRate(), complexity, bitrate, iters, frame_size, use_smpl,
                            (timer_enc.avg_lap_time().count() + timer_dec.avg_lap_time().count()) / wavReader.getLengthInSeconds(), 
                            timer_enc.avg_lap_time().count() / wavReader.getLengthInSeconds(),
                            timer_dec.avg_lap_time().count() / wavReader.getLengthInSeconds());
                }
            }
        }
    }
    if (CSVfile != NULL)
        fclose(CSVfile);

    delete[] payload_buf;
}
