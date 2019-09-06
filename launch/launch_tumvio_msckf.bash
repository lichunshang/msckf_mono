#!/usr/bin/env bash

seq=$1
path="/home/cs4li/Dev/TUMVIO"
echo $seq

if [[ $seq == "corridor1" ]]; then
    roslaunch msckf_mono tumvio_msckf.launch data_set_path:=$path/dataset-corridor1_512_16/mav0 \
        stand_still_start:=1520531835551324812. stand_still_end:=1520531840551351829.
elif [[ $seq == "room4" ]]; then
    roslaunch msckf_mono tumvio_msckf.launch data_set_path:=$path/dataset-room4_512_16/mav0 \
        stand_still_start:=1520531125050497216. stand_still_end:=1520531129500751795.
fi
