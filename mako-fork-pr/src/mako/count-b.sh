ratio=$1
cd ~/mako
cat ~/mako/results/exp-localhost-v14-2-24-*-diffdc_$ratio.log | ag 'agg_throughput' | wc -l
echo ""
cat ~/mako/results/exp-localhost-v14-2-24-*-diffdc_$ratio.log | ag 'agg_throughput'| awk '{sum += $2} END {print sum}'
ls -lh ~/mako/results/exp-localhost-v14-2-24-*-diffdc_$ratio.log
