#!/usr/bin/env bash

seq=$1
euroc_path="/home/cs4li/Dev/EUROC"
echo $seq

if [[ $seq == "MH_01" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/MH_01_easy \
        stand_still_start:=1403636600913555456. stand_still_end:=1403636623863555584.
elif [[ $seq == "MH_02" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/MH_02_easy \
        stand_still_start:=1403636885101666560. stand_still_end:=1403636897351666432.
elif [[ $seq == "MH_03" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/MH_03_medium \
        stand_still_start:=1403637142538319104. stand_still_end:=1403637149088318946.
elif [[ $seq == "MH_04" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/MH_04_difficult \
        stand_still_start:=1403638139095097088. stand_still_end:=1403638147645096960.
elif [[ $seq == "MH_05" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/MH_05_difficult/ \
        stand_still_start:=1403638532177829376. stand_still_end:=1403638538577829376.
elif [[ $seq == "V1_01" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/V1_01_easy \
        stand_still_start:=1403715273262142976. stand_still_end:=1403715278112143104.
elif [[ $seq == "V1_02" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/V1_02_medium \
        stand_still_start:=1403715523912143104. stand_still_end:=1403715528062142976.
elif [[ $seq == "V1_03" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/V1_03_difficult/ \
        stand_still_start:=1403715886584058112. stand_still_end:=1403715892534057984.
elif [[ $seq == "V2_01" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/V2_01_easy \
        stand_still_start:=1413393212255760384. stand_still_end:=1413393215505760512.
elif [[ $seq == "V2_02" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/V2_02_medium \
        stand_still_start:=1413393886005760512. stand_still_end:=1413393888855760384.
elif [[ $seq == "V2_03" ]]; then
    roslaunch msckf_mono asl_msckf.launch data_set_path:=$euroc_path/V2_03_difficult \
        stand_still_start:=1413394881555760384. stand_still_end:=1413394886255760384.
fi