#!/bin/bash

BIN=../build/bin/test_simple
INPUT_MESH=../dat/beam.tet
INPUT_CONS=../dat/beam_fixed.fv
OUTPUT_FOLDER=../result/elastic/corotational

if [ ! -d "$OUTPUT_FOLDER" ]; then
    mkdir -p $OUTPUT_FOLDER
fi

$BIN -i $INPUT_MESH -c $INPUT_CONS -o $OUTPUT_FOLDER -t 0.03 --wg 0.0 --young_modulus 1e3
