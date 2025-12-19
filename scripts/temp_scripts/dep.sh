sudo apt-get --assume-yes update

sudo apt-get --assume-yes install -y \
    git \
    wget \
    silversearcher-ag \
    python2 \
    pkg-config \
    build-essential \
    clang \
    cmake \
    cgroup-tools \
    libapr1-dev libaprutil1-dev \
    libboost-all-dev \
    libyaml-cpp-dev \
    libjemalloc-dev \
    python3-dev \
    python3-pip \
    python3-wheel \
    python3-setuptools \
    libjpeg-dev \
    zlib1g-dev \
    libgoogle-perftools-dev

sudo wget https://github.com/mikefarah/yq/releases/download/v4.24.2/yq_linux_amd64 \
    -O /usr/bin/yq && sudo chmod +x /usr/bin/yq

pip install -U setuptools
pip3 install -r requirements.txt
pip3 install Pillow matplotlib pyyaml

ulimit -n 10000

# mongodb c++ driver: https://www.mongodb.com/community/forums/t/c-and-c-driver-for-debian-ubuntu-users/2732
sudo apt-get install -y libmongoc-dev --assume-yes
sudo apt-get install -y build-essential devscripts debian-keyring fakeroot debhelper cmake libssl-dev pkg-config python3-sphinx zlib1g-dev libicu-dev libsasl2-dev libsnappy-dev libzstd-dev --assume-yes

# mongodb client / mongocxx : https://www.mongodb.com/docs/languages/cpp/drivers/current/installation/linux/#std-label-cpp-installation-linux
cd ~
curl -OL https://github.com/mongodb/mongo-cxx-driver/releases/download/r3.10.1/mongo-cxx-driver-r3.10.1.tar.gz
tar -xzf mongo-cxx-driver-r3.10.1.tar.gz
cd mongo-cxx-driver-r3.10.1/build
cmake ..                                \
    -DCMAKE_BUILD_TYPE=Release          \
    -DMONGOCXX_OVERRIDE_DEFAULT_INSTALL_PREFIX=OFF
cmake ..                                            \
    -DCMAKE_BUILD_TYPE=Release                      \
    -DBSONCXX_POLY_USE_BOOST=1                      \
    -DMONGOCXX_OVERRIDE_DEFAULT_INSTALL_PREFIX=OFF
cmake --build .
sudo cmake --build . --target install

# Install MongoDB Community Edition on Ubuntu : https://www.mongodb.com/docs/manual/tutorial/install-mongodb-on-ubuntu/

sudo apt-get install gnupg curl
curl -fsSL https://www.mongodb.org/static/pgp/server-7.0.asc | \
   sudo gpg -o /usr/share/keyrings/mongodb-server-7.0.gpg \
   --dearmor
echo "deb [ arch=amd64,arm64 signed-by=/usr/share/keyrings/mongodb-server-7.0.gpg ] https://repo.mongodb.org/apt/ubuntu jammy/mongodb-org/7.0 multiverse" | sudo tee /etc/apt/sources.list.d/mongodb-org-7.0.list
sudo apt-get update
sudo apt-get install -y mongodb-org