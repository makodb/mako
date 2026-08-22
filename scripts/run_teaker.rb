

array = 1.step(20).to_a + 30.step(100, 10).to_a

mode = ["rcc"]

for m in mode
  for i in array
    cmd = "./run.py -H ./config/hosts-teaker -f ./config/teaker-sosp/#{m}_#{i}.xml &> #{m}_#{i}.txt"
    value = `cmd`
  end
end
