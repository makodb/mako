ratio=$1
cd ~/srolis
cat ~/srolis/results/exp-localhost-v14-2-24-*-diffdc_$ratio.log | ag 'agg_throughput' | wc -l
echo ""
cat ~/srolis/results/exp-localhost-v14-2-24-*-diffdc_$ratio.log | ag 'agg_throughput'| awk '{sum += $2} END {print sum}'
ls -lh ~/srolis/results/exp-localhost-v14-2-24-*-diffdc_$ratio.log
