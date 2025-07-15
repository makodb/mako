wget https://www.memcached.org/files/memcached-1.6.17.tar.gz
tar -zxvf memcached-1.6.17.tar.gz
cd memcached-1.6.17
./configure
make -j10
sudo make install

cd ~
wget https://launchpadlibrarian.net/229086846/libmemcached_1.0.18.orig.tar.gz
tar -zxvf libmemcached_1.0.18.orig.tar.gz
sudo apt-get install libevent-dev 
cd libmemcached-1.0.18
./configure
sed -i 's/opt_servers == false/false/g' clients/memflush.cc
make -j10
sudo make install
echo "DONE"