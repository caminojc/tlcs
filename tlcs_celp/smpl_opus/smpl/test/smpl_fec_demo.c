#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus.h>
#include "smpl_api.h"
//#include "analysis.h"
//#include "cpu_support.h"
//#include "silk/debug.h"

#define MAX_PACKET 1500

static void int_to_char(opus_uint32 i, unsigned char ch[4])
{
    ch[0] = i >> 24;
    ch[1] = (i >> 16) & 0xFF;
    ch[2] = (i >> 8) & 0xFF;
    ch[3] = i & 0xFF;
}

static opus_uint32 char_to_int(unsigned char ch[4])
{
    return ((opus_uint32)ch[0] << 24) | ((opus_uint32)ch[1] << 16)
        | ((opus_uint32)ch[2] << 8) | (opus_uint32)ch[3];
}

void print_usage(char *argv[])
{
    fprintf(stderr, "Usage: %s <sampling rate (Hz)> <bits per second main> [options] <input> <lossfile> <output> \n", argv[0]);
    fprintf(stderr, "options:\n");
    fprintf(stderr, "-packetsize <10|20|60|120>   : packet size in ms; default: 20 \n");
    fprintf(stderr, "-fec_setting                 : outband Fec setting:  Rate, N_FEC, FEC_SHIFT: default 0 0 0 \n");
    fprintf(stderr, "-loss                        : Loss to set to encoder: default 0 0 0 \n");
    fprintf(stderr, "-smpl                        : Enable smpl: default 0 0 0 \n");
}

#define check_encoder_option(decode_only, opt)                        \
    do                                                                \
    {                                                                 \
        if (decode_only)                                              \
        {                                                             \
            fprintf(stderr, "option %s is only for encoding\n", opt); \
            goto failure;                                             \
        }                                                             \
    } while (0)

// OPUS definition of activity
static int energy_active(short *samples_in, int no_of_samples) {
    double nrg = 0.0;
    // Opus definition of activity
    for (int k = 0; k < no_of_samples; k++ ) {
        nrg += samples_in[ k ] * (double)samples_in[ k ];
    }
    nrg /= no_of_samples;
    if( nrg > 1e5 ) {
        return 1;
    } else {
        return 0;
    }
}

int main(int argc, char *argv[])
{
    int err;
    //char *in_file_name, *out_file_name;
    FILE *in_file = NULL, *out_file = NULL, *loss_file = NULL;
    unsigned char *encoded_bytes = NULL;
    unsigned char toc_byte;
    opus_int32 encoded_no_bytes;
    int args;
    int len;
    int packetsize_ms, packet_no_samples, packet_no_samples_max;
    int bitrate_bps, sampling_rate;
    int fec_bitrate_bps = 0, n_fec = 0, fec_offset = 0;
    short *samples_in = NULL;
    short *samples_out = NULL;
    // Statistics
    double tot_active_samples = 0.0;
    double tot_active_bytes = 0.0;
    double tot_bytes = 0.0;
    double max_bytes = 0.0;
    double tot_samples = 0;
    int use_smpl = 0;
    int use_inband_fec = 0;
    int loss_perc = 0;

    int fec_complexity = 1;

    opus_uint64 tot_in, tot_out;
    size_t num_read;
    int curr_read = 0;
    int done;
    // VAD sitting outside the SMPL codec
    // TonalityAnalysisState analysis;
    // AnalysisInfo analysis_info;

    if (argc < 5)
    {
        print_usage(argv);
        goto failure;
    }

    tot_in = tot_out = 0;

    args = 1;

    sampling_rate = (opus_int32)atol(argv[args]);
    args++;

    if (sampling_rate != 8000 && sampling_rate != 16000 &&
        sampling_rate != 32000 && sampling_rate != 48000)
    {
        fprintf(stderr, "Supported sampling rates are 8000, 16000, 32000 and 48000.\n");
        goto failure;
    }

    bitrate_bps = (opus_int32)atol(argv[args]);
    args++;

    // default values
    packetsize_ms = 20;

    while (args < argc - 3)
    {
        /* process the command line options */
        if (strcmp(argv[args], "-packetsize") == 0)
        {
            //check_encoder_option(decode_only, "-packetsize");
            if (strcmp(argv[args + 1], "10") == 0)
                packetsize_ms = 10;
            else if (strcmp(argv[args + 1], "20") == 0)
                packetsize_ms = 20;
            else if (strcmp(argv[args + 1], "60") == 0)
                packetsize_ms = 60;
            else if (strcmp(argv[args + 1], "120") == 0)
                packetsize_ms = 120;
            else
            {
                fprintf(stderr, "Unsupported packet size: %s ms. Supported are 10, 20, 60 and 120.\n", argv[args + 1]);
                goto failure;
            }
            args += 2;
        }
        else if (strcmp(argv[args], "-fec_setting") == 0)
        {
            fec_bitrate_bps = atoi(argv[args + 1]);
            n_fec = atoi(argv[args + 2]);
            fec_offset = atoi(argv[args + 3]);
            args += 4;
        }
        else if (strcmp(argv[args], "-loss") == 0)
        {
            loss_perc = atoi(argv[args + 1]);
            args += 2;
        }
        else if (strcmp(argv[args], "-fec_complexity") == 0)
        {
            fec_complexity = atoi(argv[args + 1]);
            args += 2;
        }        
        else if (strcmp(argv[args], "-smpl") == 0)
        {
            use_smpl = 1;
            args += 1;
        }
        else if (strcmp(argv[args], "-inband_fec") == 0)
        {
            use_inband_fec = 1;
            args += 1;
        }
        else
        {
            printf("Error: unrecognized setting: %s\n\n", argv[args]);
            print_usage(argv);
            goto failure;
        }
    }

    packet_no_samples = (sampling_rate * packetsize_ms) / 1000;
    packet_no_samples_max = (sampling_rate * 120) / 1000;

    in_file = fopen(argv[args], "rb");
    if (!in_file)
    {
        fprintf(stderr, "Could not open input file %s\n", argv[args]);
        goto failure;
    }
    args += 1;

    loss_file = fopen(argv[args], "rb");
    if (!loss_file)
    {
        fprintf(stderr, "Could not open loss file %s\n", argv[args]);
        goto failure;
    }
    args += 1;

    out_file = fopen(argv[args], "wb+");
    if (!out_file)
    {
        fprintf(stderr, "Could not open output file %s\n", argv[args]);
        goto failure;
    }

    opus_global_create();

    samples_in = (short *)malloc(packet_no_samples * sizeof(short));
    samples_out = (short *)malloc(packet_no_samples_max * sizeof(short));
    encoded_bytes = (unsigned char *)malloc(MAX_PACKET);

    OpusEncoder* encoder = opus_encoder_create(sampling_rate, 1, OPUS_APPLICATION_VOIP, NULL);
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate_bps));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(encoder, OPUS_SET_USING_SMPL(use_smpl));
    opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(loss_perc));
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(use_inband_fec));
    OpusDecoder* decoder = opus_decoder_create(sampling_rate, 1, NULL);
    opus_decoder_ctl(decoder, OPUS_SET_USING_SMPL(use_smpl));

    opus_int32 skip = 0;
    //opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&skip));

    opus_encoder_ctl(encoder, OPUS_SET_SECONDARY_BITRATE(fec_bitrate_bps));
    opus_encoder_ctl(encoder, OPUS_SET_SECONDARY_COMPLEXITY(fec_complexity));
    if (use_inband_fec) {
        n_fec = 1;
        fec_offset = 0;
    }
    int fec_delay = 1 + SMPL_max((n_fec - 1) * fec_offset, 0);
    if (fec_delay > 100) {
        printf("Cant handle more than 100 packets FEC delay \n");
        goto failure;
    }
    opus_int16 lossBuf[100+1];
    if (fread(lossBuf, sizeof(opus_int16), fec_delay, loss_file) != fec_delay) {
        printf("Cant read from loss file \n");
        goto failure;
    }
    int fecUsageHist[100];
    memset(fecUsageHist, 0, sizeof(fecUsageHist));

    done = 0;
    int nLost = 0, nPLC = 0;
    int N = 0, Nact = 0;
    int totBytesMain = 0, totBytesFec = 0;
    int totActBytesMain = 0, totActBytesFec = 0;
    int main_bytes[2], fec_bytes[2];
    unsigned char payload[2][1024], fec_payload[2][1024];
    memset(main_bytes, 0, sizeof(main_bytes));
    memset(fec_bytes, 0, sizeof(fec_bytes));
    opus_int16 decBuf[120 * 48000 / 1000];
    while (!done)
    {
        int read_items = fread(samples_in, sizeof(short), packet_no_samples, in_file);
        if (read_items != packet_no_samples) {
            done = 1;
            break;
        }

        main_bytes[1] = opus_encode(encoder, samples_in, packet_no_samples, payload[1], sizeof(payload[1]));
        totBytesMain += main_bytes[1];
        if (fec_bitrate_bps > 0) {
            if (fec_bitrate_bps == bitrate_bps) {
                fec_bytes[1] = main_bytes[1];
                memcpy(fec_payload[1], payload[1], fec_bytes[1] * sizeof(unsigned char));
            }else{
                fec_bytes[1] = opus_encode_secondary(encoder, fec_payload[1], sizeof(fec_payload[1]));
            }
        }
        totBytesFec += fec_bytes[1];
        if (energy_active(samples_in, packet_no_samples)) {
            totActBytesMain += main_bytes[1];
            totActBytesFec += fec_bytes[1];
            Nact++;
        }

        int nsamples_out = 0;

        if (lossBuf[0] == 0 || N < 2) { // Avoid Starting with a loss for first decode
            // No Loss decode Main
            nsamples_out = opus_decode(decoder, payload[0], main_bytes[0], decBuf, packet_no_samples, 0);
        }
        else {
            nLost++;
            int decode_fec = 0;
            for (int i = 0; i < n_fec; i++) {
                if (lossBuf[1 + i * fec_offset] == 0) {
                    // We have a fec with distance 1 + i * fec_dist
                    decode_fec = 1;
                    fecUsageHist[i]++;
                    break;
                }
            }
            if (decode_fec) {
                if (use_inband_fec) {
                    nsamples_out = opus_decode(decoder, payload[1], main_bytes[1], decBuf, packet_no_samples, 1);
                }
                else {
                    nsamples_out = opus_decode(decoder, fec_payload[0], fec_bytes[0], decBuf, packet_no_samples, 0);
                }
            }
            else {
                // Call PLC
                nPLC++;
                nsamples_out = opus_decode(decoder, NULL, 0, decBuf, packet_no_samples, 0);
            }
        }
        if (N > 0) {
            if (fwrite(&decBuf[skip], sizeof(short), nsamples_out - skip, out_file) != (nsamples_out - skip))
            {
                printf("Error writing decoded samples to file.\n");
                goto failure;
            }
            skip = 0;
        }
        memcpy(payload[0], payload[1], main_bytes[1] * sizeof(unsigned char));
        main_bytes[0] = main_bytes[1];
        memcpy(fec_payload[0], fec_payload[1], fec_bytes[1] * sizeof(unsigned char));
        fec_bytes[0] = fec_bytes[1];

        if (fec_delay > 0) {
            memmove(&lossBuf[0], &lossBuf[1], fec_delay * sizeof(opus_int16));
        }
        fread(&lossBuf[fec_delay], sizeof(opus_int16), 1, loss_file);
        N++;
    }
    memset(decBuf, 0, packet_no_samples * sizeof(opus_int16));
    fwrite(decBuf, sizeof(short), packet_no_samples, out_file); // Make output same length as input
    fwrite(decBuf, sizeof(short), skip, out_file); // Make output same length as input

    float mainRate = (totBytesMain * 8.0f * 1000.0/packetsize_ms) / N;
    float mainActRate = (totActBytesMain * 8.0f * 1000.0 / packetsize_ms) / Nact;
    float fecRate = (n_fec * totBytesFec * 8.0f * 1000.0 / packetsize_ms) / N;
    float fecActRate = (n_fec * totActBytesFec * 8.0f * 1000.0 / packetsize_ms) / Nact;
    printf("Encode %d frames \n", N);
    printf("Main rate avg:%.0f active:%.0f bps \n", mainRate, mainActRate);
    printf("Fec  rate avg:%.0f active:%.0f bps \n", fecRate, fecActRate);
    printf("Tot  rate avg:%.0f active:%.0f bps \n\n", mainRate + fecRate, mainActRate + fecActRate);
    printf("Decode %d frames with %f pct loss and %f pct PLC \n\n", N, 100.0f * nLost / N, 100.0f * nPLC / N);
    int totFec = 0;
    for (int i = 0; i < n_fec; i++) {
        totFec += fecUsageHist[i];
    }
    printf("Fec Delay Used: \n");
    for (int i = 0; i < n_fec; i++) {
        printf("%.2f pct %d ms \n", 100.0f * fecUsageHist[i] / totFec, (1 + i * fec_offset) * packetsize_ms);
    }
failure:

    if (in_file != NULL)
        fclose(in_file);
    if (out_file != NULL)
        fclose(out_file);
    free(samples_in);
    free(samples_out);
    free(encoded_bytes);

    opus_encoder_destroy(encoder);
    opus_decoder_destroy(decoder);

    opus_global_create();

    return 0;
}
