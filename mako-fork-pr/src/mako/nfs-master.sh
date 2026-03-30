# nfs master
# edit /etc/exports
mkdir -p eRPC mako rdma-core dpdk-stable-19.11.5 logs janus meerkat tapir
sudo systemctl restart nfs-kernel-server
sudo systemctl status nfs-kernel-server
