#! /bin/bash

rm -f input.pcm
rm -f output.opus.wav
rm -f output.smpl.wav

ffmpeg -loglevel panic -i $1 -ar 16000 -f s16le -acodec pcm_s16le input.pcm
# Opus
./build/opus_demo -e voip 16000 1 9600 -complexity 5 input.pcm output.opus
./build/opus_demo -d 16000 1 output.opus output.opus.pcm
ffmpeg -loglevel panic -f s16le -ar 16k -ac 1 -i output.opus.pcm output.opus.wav

# SMPL
./build/opus_demo -e voip 16000 1 9600 -complexity 5 -smpl input.pcm output.smpl
./build/opus_demo -d 16000 1 -smpl output.smpl output.smpl.pcm
ffmpeg -loglevel panic -f s16le -ar 16k -ac 1 -i output.smpl.pcm output.smpl.wav

