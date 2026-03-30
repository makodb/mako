#!/bin/bash

results="results.txt"

# # 1 shared timestamp
# int_array=(64 128 192 256 320 384 448 512)
# echo " " >> $results
# echo " ------- 1 shared timestamp ------ " >> $results
# # Loop through the array
# for i in "${int_array[@]}"; do
#     cmd="./occ $i 4"
#     echo $cmd
#     $cmd >> $results
#     echo " " >> $results
# done

# int_array=(64 128 192 256 320 384 448 512)
# # Loop through the array
# echo " " >> $results
# echo " ------- 64 shared timestamps ------ " >> $results
# for i in "${int_array[@]}"; do
#     cmd="./occ $i 256"
#     echo $cmd
#     $cmd >> $results
#     echo " " >> $results
# done

# # full-sized experiments
# echo " ------- full-sized shared timestamps ------ " >> $results
# int_array=(64 128 192 256 320 384 448 512)
# # Loop through the array
# for i in "${int_array[@]}"; do
#     cmd="./occ $i $((i * 4))"
#     echo $cmd
#     $cmd >> $results
#     echo " " >> $results
# done