shards=$1
ratio=$2
cd ~/srolis
cat ~/srolis/results/exp-localhost-v14-$shards-24-*-samedc_$ratio.log | ag 'agg_throughput' | wc -l
echo ""
cat ~/srolis/results/exp-localhost-v14-$shards-24-*-samedc_$ratio.log | ag 'agg_throughput'| awk '{sum += $2} END {print sum}'
ls -lh ~/srolis/results/exp-localhost-v14-$shards-24-*-samedc_$ratio.log

