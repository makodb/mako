//! Mako protocol over explicit storage, transport and durability contracts.
//! Lower implementations belong to mako-runtime and their own component gates.
#![forbid(unsafe_code)]
pub mod admission;
pub mod contracts;
pub mod epoch;
pub mod gateway_client;
pub mod participant;
pub mod redo;
pub mod remote;
pub mod wire;
