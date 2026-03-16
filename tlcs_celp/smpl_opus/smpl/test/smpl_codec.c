#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smpl_api.h"
#include "analysis.h"
#include "cpu_support.h"
#include "silk/debug.h"

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
    fprintf(stderr, "Usage: %s [-e] <sampling rate (Hz)> <bits per second>  [options] <input> <output>\n", argv[0]);
    fprintf(stderr, "       %s -d <sampling rate (Hz)> [options] <input> <output>\n\n", argv[0]);
    fprintf(stderr, "options:\n");
    fprintf(stderr, "-e                           : only runs the encoder (output the bit-stream)\n");
    fprintf(stderr, "-d                           : only runs the decoder (reads the bit-stream as input)\n");
    fprintf(stderr, "-packetsize <10|20|60|120>   : packet size in ms; default: 20 \n");
    fprintf(stderr, "-channels <1|2>              : channels in <input> and <output> pcm files; default: 1 \n");
    fprintf(stderr, "-complexity <comp>           : complexity, 0 (lowest) ... 10 (highest); default: 10\n" );
    fprintf(stderr, "-dtx                         : enable SMPL DTX\n" );
    fprintf(stderr, "-lpc_postfilter              : enable SMPL lpc postfilter (default it is off)\n" );
    //fprintf(stderr, "-loss <perc>                 : simulate packet loss, in percent (0-100); default: 0\n" );
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
    char *in_file_name, *out_file_name;
    FILE *in_file = NULL, *out_file = NULL;
    void *enc = NULL, *dec = NULL;
    smpl_EncControlStruct encCtrl;
    smpl_DecControlStruct decCtrl;
    ec_enc ent_enc;
    ec_dec ent_dec;
    unsigned char *encoded_bytes = NULL;
    unsigned char toc_byte;
    opus_int32 encoded_no_bytes;
    int args;
    int arch;
    int len;
    int packetsize_ms, packet_no_samples, packet_no_samples_max;
    int complexity, bitrate_bps, sampling_rate, channels;
    int use_inbandfec, use_dtx, packet_loss_perc, use_lpc_postfilter;
    int hp_f_corner_Hz = SMPL_ENC_HP_FCORNER_3DB_HZ;
    short *samples_in = NULL;
    short *samples_out = NULL;
    // Statistics
    double tot_active_samples = 0.0;
    double tot_active_bytes = 0.0;
    double tot_bytes = 0.0;
    double max_bytes = 0.0;
    double tot_samples = 0;

    opus_uint64 tot_in, tot_out;
    int encode_only = 0, decode_only = 0;
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
    if (strcmp(argv[args], "-e") == 0)
    {
        encode_only = 1;
        args++;
    }
    else if (strcmp(argv[args], "-d") == 0)
    {
        decode_only = 1;
        args++;
    }
    if (!decode_only && argc < 5)
    {
        print_usage(argv);
        goto failure;
    }

    sampling_rate = (opus_int32)atol(argv[args]);
    args++;

    if (sampling_rate != 8000 && sampling_rate != 16000 &&
        sampling_rate != 32000 && sampling_rate != 48000)
    {
        fprintf(stderr, "Supported sampling rates are 8000, 16000, 32000 and 48000.\n");
        goto failure;
    }

    if (!decode_only)
    {
        bitrate_bps = (opus_int32)atol(argv[args]);
        args++;
    }

    // default values
    packetsize_ms = 20;
    complexity = 5;
    channels = 1;
    use_inbandfec = 0;
    use_dtx = 0;
    packet_loss_perc = 0;
    use_lpc_postfilter = 0;
    arch = opus_select_arch();

    while (args < argc - 2)
    {
        /* process the command line options */
        if (strcmp(argv[args], "-packetsize") == 0)
        {
            check_encoder_option(decode_only, "-packetsize");
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
        else if (strcmp(argv[args], "-complexity") == 0)
        {
            check_encoder_option(decode_only, "-complexity");
            complexity = atoi(argv[args + 1]);
            args += 2;
        }
        else if (strcmp(argv[args], "-channels") == 0)
        {
            channels = atoi(argv[args + 1]);
            args += 2;
        }
        else if (strcmp(argv[args], "-dtx") == 0)
        {
            check_encoder_option(decode_only, "-dtx");
            use_dtx = 1;
            args++;
        }
        else if (strcmp(argv[args], "-lpc_postfilter") == 0)
        {
            use_lpc_postfilter = 1;
            args++;
        }
        else if (strcmp(argv[args], "-loss") == 0)
        {
            packet_loss_perc = atoi(argv[args + 1]);
            args += 2;
        }
        else
        {
            printf("Error: unrecognized setting: %s\n\n", argv[args]);
            print_usage(argv);
            goto failure;
        }
    }

    packet_no_samples = (sampling_rate * packetsize_ms * channels) / 1000;
    packet_no_samples_max = (sampling_rate * 120 * channels) / 1000;

    in_file_name = argv[args];
    in_file = fopen(in_file_name, "rb");
    if (!in_file)
    {
        fprintf(stderr, "Could not open input file %s\n", argv[args]);
        goto failure;
    }
    args += 1;

    out_file_name = argv[args];
    out_file = fopen(out_file_name, "wb+");
    if (!out_file)
    {
        fprintf(stderr, "Could not open output file %s\n", argv[args]);
        goto failure;
    }

    err = smpl_CreateCodec();
    if (err != OPUS_OK)
    {
        fprintf(stderr, "Cannot initialize codec. Error: %i\n", err);
        goto failure;
    }

    if (!decode_only)
    {
        int enc_size;
        smpl_Get_Encoder_Size(&enc_size);
        enc = malloc(enc_size);
        encCtrl.API_sampleRate = sampling_rate;
        encCtrl.bitRate = bitrate_bps;
        encCtrl.complexity = complexity;
        encCtrl.payloadSize_ms = packetsize_ms;
        encCtrl.maxBits = MAX_PACKET;
        encCtrl.useDTX = use_dtx;
        encCtrl.nChannelsAPI = channels;
        encCtrl.nChannelsInternal = channels;
        encCtrl.internalSampleRate = sampling_rate;
        encCtrl.maxInternalSampleRate = sampling_rate;
        encCtrl.useInBandFEC = use_inbandfec;
        encCtrl.packetLossPercentage = packet_loss_perc;
        encCtrl.hp_f_corner_Hz = hp_f_corner_Hz;
        err = smpl_InitEncoder(enc, &encCtrl);

        if (err != OPUS_OK)
        {
            fprintf(stderr, "Cannot initialize encoder. Error: %i\n", err);
            goto failure;
        }
    }
    if (!encode_only)
    {
        int dec_size;
        smpl_Get_Decoder_Size(&dec_size);
        dec = malloc(dec_size);
        err = smpl_InitDecoder(dec);
        memset(&decCtrl, 0, sizeof(smpl_DecControlStruct));
        decCtrl.API_sampleRate = sampling_rate;
        decCtrl.internalSampleRate = sampling_rate;
        decCtrl.payloadSize_ms = packetsize_ms;
        decCtrl.nChannelsAPI = channels;
        decCtrl.LPC_postfilter_mode = (SmplLpcPostFilterMode)use_lpc_postfilter;
        if (err != OPUS_OK)
        {
            fprintf(stderr, "Cannot initialize decoder. Error: %i\n", err);
            goto failure;
        }
    }

    if (decode_only)
        fprintf(stderr, "Decoding with %i Hz output\n", sampling_rate);
    else
        fprintf(stderr, "Encoding %i Hz input at %.3f kb/s with %d ms packets.\n",
                sampling_rate, bitrate_bps * 0.001, packetsize_ms);

    samples_in = (short *)malloc(packet_no_samples * sizeof(short));
    samples_out = (short *)malloc(packet_no_samples_max * sizeof(short));
    encoded_bytes = (unsigned char *)malloc(MAX_PACKET);
    // tonality_analysis_init(&analysis, sampling_rate);

    done = 0;
    while (!done)
    {
        if (decode_only)
        {
            unsigned char ch[4];
            int read_items = fread(ch, 1, 4, in_file);
            if (read_items != 4) {
                done = 1;
                break;
            }
            encoded_no_bytes = char_to_int(ch);

            read_items = fread(encoded_bytes, 1, encoded_no_bytes, in_file);
            if (read_items != (size_t)encoded_no_bytes) {
                fprintf(stderr, "Error reading encoded file. Expecting %d bytes got %d\n",
                        encoded_no_bytes, (int)read_items);
                goto failure;
            }
        } else {
            int read_items = fread(samples_in, sizeof(short), packet_no_samples, in_file);
            if (read_items != packet_no_samples) {
                done = 1;
                break;
            }
            // run_analysis(&analysis, celt_mode, analysis_pcm, analysis_size, frame_size,
            //                 c1, c2, analysis_channels, sampling_rate,
            //                 lsb_depth, downmix, &analysis_info);
            TIC(enc)
            ec_enc_init(&ent_enc, encoded_bytes+1, MAX_PACKET-1);
            err = smpl_Encode(enc, &encCtrl, samples_in, packet_no_samples/encCtrl.nChannelsAPI, &ent_enc, &toc_byte, &encoded_no_bytes, 0, 1);
            TOC(enc)
            encoded_bytes[0] = toc_byte;
            if (err != OPUS_OK) {
                fprintf(stderr, "Error encoding. Error: %i\n", err);
                goto failure;
            }
            if (energy_active(samples_in, packet_no_samples)) {
                tot_active_samples += packet_no_samples;
                tot_active_bytes += encoded_no_bytes;
            }
            if (encoded_no_bytes > max_bytes)
                max_bytes = encoded_no_bytes;
            tot_bytes += encoded_no_bytes;
            ec_enc_done(&ent_enc);
        }
        if (encode_only)
        {
            unsigned char ch[4];
            int_to_char(encoded_no_bytes, ch);
            if (fwrite(ch, 1, 4, out_file) != 4)
            {
                fprintf(stderr, "Error writing encoded number of bytes\n");
                goto failure;
            }
            if (fwrite(encoded_bytes, 1, encoded_no_bytes, out_file) != encoded_no_bytes)
            {
                fprintf(stderr, "Error writing encoded bytes to file\n");
                goto failure;
            }
        } else {
            int lost_flag = (encoded_no_bytes==0);
            opus_int32 nsamples_out;

            TIC(dec)
            if (!lost_flag) {
                ec_dec_init(&ent_dec, encoded_bytes + 1, encoded_no_bytes - 1);
                nsamples_out = packet_no_samples_max/decCtrl.nChannelsAPI; // Indicate buffer size
                err = smpl_Decode(dec, &decCtrl, lost_flag, 1, &ent_dec, encoded_bytes[0], samples_out, &nsamples_out);
                nsamples_out *= decCtrl.nChannelsAPI;
                packet_no_samples = nsamples_out;
            } else {
                nsamples_out = packet_no_samples/decCtrl.nChannelsAPI; // Indicate PLC duration
                err = smpl_Decode(dec, &decCtrl, lost_flag, 1, NULL, 0, samples_out, &nsamples_out);
                nsamples_out *= decCtrl.nChannelsAPI;
            }
            TOC(dec)
            if (err != OPUS_OK) {
                fprintf(stderr, "Error decoding bytes with smpl decoder. Error: %i\n", err);
                goto failure;
            }
            if (fwrite(samples_out, sizeof(short), nsamples_out, out_file) != nsamples_out)
            {
                fprintf(stderr, "Error writing decoded samples to file.\n");
                goto failure;
            }
        }
        tot_samples += packet_no_samples;
    }

    if (!decode_only && tot_samples > 0) {
        fprintf(stdout, "Encoded audio duration:      %7.3f s\n", tot_samples / (channels * sampling_rate));
        /* Print bitrate statistics */
        fprintf(stdout, "average bitrate:             %7.3f kb/s\n",
                1e-3 * tot_bytes * 8.0 * sampling_rate / (tot_samples / channels));
        fprintf(stdout, "maximum bitrate:             %7.3f kb/s\n",
                1e-3 * max_bytes * 8.0 * sampling_rate / (packet_no_samples / channels));
        if (tot_active_samples > 0)
            fprintf(stdout, "active bitrate:              %7.3f kb/s\n",
                    1e-3 * tot_active_bytes * 8.0 * sampling_rate / (tot_active_samples / channels));
    }
failure:

    if (enc) {
        free(enc);
    }
    if (dec)
        free(dec);
    if (in_file != NULL)
        fclose(in_file);
    if (out_file != NULL)
        fclose(out_file);
    free(samples_in);
    free(samples_out);
    free(encoded_bytes);

    smpl_FreeCodec();

    if (decode_only) {
        silk_TimerSave("timer_dec.txt");
    } else if (encode_only) {
        silk_TimerSave("timer_enc.txt");
    } else {
        silk_TimerSave("timer.txt");
    }

    return 0;
}
