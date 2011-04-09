#!/bin/bash

# Normalizes any WAV file to a slampler-compatible format
# (c) jzu@free.fr 2011

if [ $# -lt 2 ] 
then
  echo Usage: `basename $0`  SRC.wav DST.wav 1>&2
  exit 1
fi

sox $1 -c 1 -b 16 $2 rate -h 44100 dither -s
