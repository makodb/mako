//! Behavior recovered from rpc_client_pool_test.cc using real TCP connections.
#![allow(unsafe_code)]

use srpc::client::{deserialize_from, Client, ClientPool, FutureAttr, PoolConfig};
use srpc::load_balancer::LoadBalancingStrategy;
use srpc::reactor::PollThread;
use srpc::serializable::Serialize;
use srpc::server::{Request, Server, Service, WeakServerConnection};
use std::sync::Arc;

const RPC: i32 = 0x00e0_7101;

struct Reply;
impl Service for Reply {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        server.reg_fast_rpc(RPC, index)
    }
    fn __dispatch__(&self, _: i32, request: Box<Request>, connection: WeakServerConnection) {
        connection.upgrade().unwrap().reply(
            &request,
            0,
            Some(Box::new(|out| {
                Serialize::serialize(&73i64, out);
            })),
        );
    }
}

struct Fixture {
    pool: Option<ClientPool>,
    servers: Vec<Server>,
    server_poll: Arc<PollThread>,
    addresses: Vec<String>,
}

impl Fixture {
    fn new(addresses: usize) -> Self {
        let server_poll = PollThread::create();
        let mut fixture = Self {
            pool: Some(ClientPool::new(
                None,
                PoolConfig {
                    min_connections: 3,
                    max_connections: 3,
                    load_balancing: LoadBalancingStrategy::ROUND_ROBIN,
                    ..PoolConfig::defaults()
                },
            )),
            servers: Vec::new(),
            server_poll,
            addresses: Vec::new(),
        };
        for _ in 0..addresses {
            let mut server = Server::new(Some(fixture.server_poll.clone()));
            server.reg_service(Box::new(Reply));
            // The literal is NUL terminated and valid for this call.
            assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
            fixture
                .addresses
                .push(format!("127.0.0.1:{}", server.get_bound_port()));
            fixture.servers.push(server);
        }
        fixture
    }
    fn pool(&self) -> &ClientPool {
        self.pool.as_ref().unwrap()
    }
    fn clients(&self, address: usize) -> Vec<Arc<Client>> {
        let addr = &self.addresses[address];
        // Initial population chooses randomly. Subsequent calls use round robin.
        drop(self.pool().get_client(addr).expect("populate TCP pool"));
        (0..3)
            .map(|_| self.pool().get_client(addr).unwrap())
            .collect()
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        drop(self.pool.take());
        self.servers.clear();
        self.server_poll.shutdown();
    }
}

#[test]
fn pool_populates_minimum_reuses_connections_and_serves_real_requests() {
    let fixture = Fixture::new(1);
    let clients = fixture.clients(0);
    assert_eq!(fixture.pool().total_client_count(), 3);
    assert_eq!(fixture.pool().address_count(), 1);
    assert_eq!(
        fixture
            .pool()
            .get_healthy_client_count(&fixture.addresses[0]),
        3
    );
    for i in 0..3 {
        for j in 0..i {
            assert!(!Arc::ptr_eq(&clients[i], &clients[j]));
        }
        let reused = fixture.pool().get_client(&fixture.addresses[0]).unwrap();
        assert!(Arc::ptr_eq(&reused, &clients[i]));
        let reply = reused.request(RPC, &FutureAttr::default(), |_| {}).unwrap();
        reply.wait();
        assert_eq!(reply.get_error_code(), 0);
        let mut value = 0i64;
        deserialize_from(reply.get_reply(), &mut value);
        assert_eq!(value, 73);
    }
    assert_eq!(fixture.pool().total_client_count(), 3);
}

#[test]
fn unhealthy_pruning_respects_each_address_floor_and_lowered_configuration() {
    let fixture = Fixture::new(2);
    for address in 0..2 {
        for client in fixture.clients(address) {
            client.close();
        }
        assert_eq!(
            fixture
                .pool()
                .get_healthy_client_count(&fixture.addresses[address]),
            0
        );
    }
    assert_eq!(fixture.pool().remove_all_unhealthy(), 0);
    let mut config = fixture.pool().pool_config();
    config.min_connections = 1;
    fixture.pool().set_pool_config(config);
    assert_eq!(
        fixture
            .pool()
            .remove_unhealthy_clients(&fixture.addresses[0]),
        2
    );
    assert_eq!(fixture.pool().remove_all_unhealthy(), 2);
    assert_eq!(fixture.pool().total_client_count(), 2);
    assert_eq!(fixture.pool().address_count(), 2);
    assert_eq!(fixture.pool().remove_unhealthy_clients("absent"), 0);
}

#[test]
fn disabled_health_checks_and_idle_pruning_obey_configuration() {
    let fixture = Fixture::new(2);
    for address in 0..2 {
        drop(fixture.clients(address));
    }
    let mut config = fixture.pool().pool_config();
    config.min_connections = 1;
    config.idle_timeout_ms = 0;
    config.health_check_enabled = false;
    fixture.pool().set_pool_config(config);
    let client = fixture.pool().get_client(&fixture.addresses[0]).unwrap();
    client.close();
    assert_eq!(
        fixture
            .pool()
            .get_healthy_client_count(&fixture.addresses[0]),
        3
    );
    assert_eq!(fixture.pool().remove_all_unhealthy(), 0);
    assert_eq!(fixture.pool().close_all_idle(u64::MAX), 0);
    config.idle_timeout_ms = 1;
    fixture.pool().set_pool_config(config);
    assert_eq!(
        fixture
            .pool()
            .close_idle_clients(&fixture.addresses[0], u64::MAX),
        2
    );
    assert_eq!(fixture.pool().close_all_idle(u64::MAX), 2);
    assert_eq!(fixture.pool().total_client_count(), 2);
    assert_eq!(fixture.pool().close_idle_clients("absent", u64::MAX), 0);
}

#[test]
fn failed_population_does_not_leave_a_cached_address() {
    let pool = ClientPool::new(None, PoolConfig::defaults());
    assert!(pool.get_client("invalid-address").is_none());
    assert_eq!(pool.address_count(), 0);
    assert_eq!(pool.total_client_count(), 0);
}
