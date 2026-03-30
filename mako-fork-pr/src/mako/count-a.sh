shards=$1
ratio=$2
cd ~/mako
cat ~/mako/results/exp-localhost-v14-$shards-24-*-samedc_$ratio.log | ag 'agg_throughput' | wc -l
echo ""
cat ~/mako/results/exp-localhost-v14-$shards-24-*-samedc_$ratio.log | ag 'agg_throughput'| awk '{sum += $2} END {print sum}'
ls -lh ~/mako/results/exp-localhost-v14-$shards-24-*-samedc_$ratio.log

