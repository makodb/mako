# Mako

## Compile (Debian 12)

Recursive clone everything 

```
$ git clone --recursive [git_repo_url]
```

Install all dependencies

```
$ bash apt_packages.sh
```

Manually compile eRPC (TODO: change to auto build)

```
$ cd third-party/erpc
$ make
```

# Compile
```
bash compile-cmake.sh 
```
TODO replace this script with standard cmake build

TODO
 - write a ut as much simple as possible

## notes
1. we use nfs to sync some data, e.g., we use nfs to control all worker threads execute at the roughly same time (we used memcached in the past and removed this external dependencies)
2. for erpc, we add pure ethernet support so that you can use widely adopted sockets
```
cd ./third-party/erpc
make clean
cmake . -DTRANSPORT=fake -DROCE=off -DPERF=off
make -j10

echo "eth" > env.txt
```