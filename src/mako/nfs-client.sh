export nfs_master="10.1.0.48"
#mkdir -p eRPC mako rdma-core dpdk-stable-19.11.5 logs janus rolis-eurosys2022
#sudo umount /home/azureuser/eRPC
#sudo umount /home/azureuser/mako
#sudo umount /home/azureuser/rdma-core
#sudo umount /home/azureuser/dpdk-stable-19.11.5
#sudo umount /home/azureuser/logs
#sudo umount /home/azureuser/janus
#sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/eRPC /home/azureuser/eRPC
#sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/rolis-eurosys2022 /home/azureuser/rolis-eurosys2022
#sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/mako /home/azureuser/mako
#sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/rdma-core /home/azureuser/rdma-core
#sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/dpdk-stable-19.11.5 /home/azureuser/dpdk-stable-19.11.5
#sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/logs /home/azureuser/logs
mkdir -p janus rolis-eurosys2022 meerkat tapir janus-tapir
sudo umount /home/azureuser/janus
sudo umount /home/azureuser/janus-tapir
sudo umount /home/azureuser/tapir
sudo umount /home/azureuser/meerkat
sudo umount /home/azureuser/rolis-eurosys2022
sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/janus /home/azureuser/janus
sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/tapir /home/azureuser/tapir
sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/janus-tapir /home/azureuser/janus-tapir
sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/meerkat /home/azureuser/meerkat
sudo mount -t nfs -vvvv $nfs_master:/home/azureuser/rolis-eurosys2022 /home/azureuser/rolis-eurosys2022
