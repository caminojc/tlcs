#!/bin/bash

usage() { # Print a help message.
  echo "Usage: $0 -r <reference_binary> -n <new_binary>" 1>&2
}

while getopts r:n: options; do  # Loop and get the next option;
  case "${options}" in
    r)
      opus_demo_ref=${OPTARG}
      ;;
    n)
      opus_demo=${OPTARG}
      ;;
    *)                                    # unknown option
      usage
      exit 1                       # Exit abnormally.
      ;;
  esac
done

if [ -z "$opus_demo_ref" ] || [ -z "$opus_demo" ]; then
    echo "ref: $opus_demo_ref new: $opus_demo"
    usage
    exit 1
fi

exitOnError() { # Exit if log file has errors
  err=$(grep 'Error\|error\|Fatal\|fatal' out.log)
  if [ ! -z "$err" ]; then
     cat out.log
     exit 1
  fi
}

visqoldir="../../../EvaluateCodecs/visqol"
visqol_flags="--use_speech_mode --similarity_to_quality_model ${visqoldir}/model/lattice_tcditugenmeetpackhref_ls2_nl60_lr12_bs2048_learn.005_ep2400_train1_7_raw.tflite"

# Test for Wideband and Full band
for fs in 16000 48000; do
    if [ $fs -eq 16000 ]; then
        cp ../../test_signal_wb.wav input.wav
        ffmpeg -y -hide_banner -loglevel error -t 10.6 -i ../../test_signal_wb.wav input.wav
        declare -a bitrates=(4500 8000 16000 25000)
        min_mos=3.0
        compatibility_mos=4.0
    else
        ffmpeg -y -hide_banner -loglevel error -ss 18.5 -t 13 -i ../../test_signal_vctk.wav input.wav
        declare -a bitrates=(5500 10000 18000 28000)
        min_mos=3.0
        compatibility_mos=3.9
    fi
    ffmpeg -y -hide_banner -loglevel error -i input.wav -f s16le input_$fs.pcm

    for bitrate in ${bitrates[@]}; do
        # Run new codec
        echo "Running with $fs Hz at $bitrate bps"
        $opus_demo -e voip $fs 1 $bitrate -framesize 60 -dtx -smpl input_$fs.pcm out.bit > out.log 2>&1
        $opus_demo -d $fs 1 -smpl out.bit file.pcm >> out.log 2>&1
        ffmpeg -y -hide_banner -loglevel error -f s16le -ar $fs -ac 1 -i file.pcm out_$bitrate.wav
	exitOnError

        # Run compatibilty with reference codec binary
        $opus_demo_ref -d $fs 1 -smpl out.bit file.pcm >> out.log 2>&1
        ffmpeg -y -hide_banner -loglevel error -f s16le -ar $fs -ac 1 -i file.pcm out_newenc_refdec_$bitrate.wav
        $opus_demo_ref -e voip $fs 1 $bitrate -framesize 60 -dtx -smpl input_$fs.pcm out_ref.bit >> out.log 2>&1
        $opus_demo     -d $fs 1 -smpl out_ref.bit file.pcm >> out.log 2>&1
        ffmpeg -y -hide_banner -loglevel error -f s16le -ar $fs -ac 1 -i file.pcm out_refenc_newdec_$bitrate.wav
        $opus_demo_ref -d $fs 1 -smpl out_ref.bit file.pcm >> out.log 2>&1
        ffmpeg -y -hide_banner -loglevel error -f s16le -ar $fs -ac 1 -i file.pcm out_refenc_refdec_$bitrate.wav
	exitOnError

        # Get MOS of latest binary and "diff" MOS
        mos=$($visqoldir/visqol_linux $visqol_flags --reference_file input.wav --degraded_file out_$bitrate.wav | grep MOS | grep -Eo '[+-]?[0-9]+([.][0-9]+)?')
        if [ -z "$mos" ]; then
            echo "Error: no ViSQOL MOS received for latest binary"
            exit 1
        fi
        mos_newenc_diff=$($visqoldir/visqol_linux $visqol_flags --reference_file out_$bitrate.wav --degraded_file out_newenc_refdec_$bitrate.wav | grep MOS | grep -Eo '[+-]?[0-9]+([.][0-9]+)?')
        mos_refenc_diff=$($visqoldir/visqol_linux $visqol_flags --reference_file out_refenc_newdec_$bitrate.wav --degraded_file out_refenc_refdec_$bitrate.wav | grep MOS | grep -Eo '[+-]?[0-9]+([.][0-9]+)?')
        if [ -z "$mos_newenc_diff" ] || [ -z "$mos_refenc_diff" ]; then
            echo "Error: no MOS received for compatibility binary test"
            exit 1
        fi

        # Check that MOS increases as expected with bitrate
        if awk "BEGIN {exit ($mos > $min_mos)}"; then
            echo "Error: bitrate $bitrate - ViSQOL MOS $mos not bigger than previous $min_mos"
            exit 1
        else
            echo "OK: bitrate $bitrate - ViSQOL MOS $mos bigger than previous $min_mos"
        fi
        min_mos=$mos

        # Check that wav files in compatibility setup are very similar
        if awk "BEGIN {exit ($mos_newenc_diff > $compatibility_mos && $mos_refenc_diff > $compatibility_mos)}"; then
            echo "Error: Output not similar enough at $bitrate bps new encoder MOS: $mos_newenc_diff old encoder MOS: $mos_refenc_diff"
            exit 1
        else
            echo "OK: bitrate $bitrate - compatibility ViSQOL MOS (threshold $compatibility_mos)"
            echo "    ref enc: $mos_newenc_diff"
            echo "    ref dec: $mos_refenc_diff"
            echo ""
        fi

    done

    # Check that higest MOS is high enough
    if [ $fs -eq 16000 ]; then
        min_mos=4.1
    else
        min_mos=4.0
    fi
    if awk "BEGIN {exit ($mos > $min_mos)}"; then
        echo "Error: bitrate $bitrate - ViSQOL MOS $mos not bigger than $min_mos"
        exit 1
    else
        echo "OK: bitrate $bitrate - ViSQOL MOS $mos bigger than $min_mos"
    fi
done
