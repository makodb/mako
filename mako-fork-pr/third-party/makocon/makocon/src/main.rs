use autocxx::prelude::*;
use std::collections::HashMap;
use std::sync::Mutex;
use redis_protocol::resp3::{types::BytesFrame, types::DecodedFrame};

include_cpp!{
    #include "wrapper.h"
    safety!(unsafe_ffi)
    generate!("Wrapper")
}

fn kv_handler(
    decoded: DecodedFrame<BytesFrame>,
    conn: &mut makocon::Conn,
    db: &Mutex<HashMap<Vec<u8>, Vec<u8>>>,
) {
    let frame = match decoded.into_complete_frame() {
        Ok(f) => f,
        Err(_) => {
            return;
        }
    };

    let parts = match frame {
        BytesFrame::Array { data, .. } => data,
        _ => {
            conn.write_error("ERR expected Array frame");
            return;
        }
    };

    let cmd = match parts.get(0) {
        Some(BytesFrame::BlobString { data, .. })
        | Some(BytesFrame::SimpleString { data, .. }) => data.clone().to_ascii_lowercase(),
        _ => {
            conn.write_error("ERR invalid command type");
            return;
        }
    };

    // Note: Each command creates a new wrapper instance
    // This means OPEN command effect is lost between calls
    // For a production system, you'd want persistent state
    let mut wrapper = ffi::Wrapper::new().within_unique_ptr();

    match &cmd[..] {
        b"get" if parts.len() == 2 => {
            if let BytesFrame::BlobString { data: key, .. } = &parts[1] {
                let db = db.lock().unwrap();
                let key_vec: Vec<u8> = key.clone().into();
                match db.get(&key_vec) {
                    Some(val) => conn.write_bulk(val),
                    None => conn.write_null(),
                }

                let key_string = String::from_utf8(key_vec).expect("UTF-8 error");
                
                // Initialize the KV store wrapper if needed and send GET request
                let initialized = wrapper.pin_mut().init();
                if initialized {
                    println!("KV store initialized for GET request");
                }
                
                let request = format!("get:{}", key_string);
                let req_id = wrapper.pin_mut().sendtoqueue(request);
                
                if req_id.0 >= 0 {
                    let response = wrapper.pin_mut().recvfromqueue(req_id);
                    println!("KV store response: {}", response);
                    // Note: response is now guaranteed to contain a result (blocking call)
                    // For GET operations, empty response means key not found
                } else {
                    println!("Failed to send GET request to KV store");
                }
            } else {
                conn.write_error("ERR invalid GET args");
            }
        }
        b"set" if parts.len() == 3 => {
            if let (BytesFrame::BlobString { data: key, .. },
                    BytesFrame::BlobString { data: val, .. }) =
                    (&parts[1], &parts[2]) {
                let mut db = db.lock().unwrap();
                let key_vec: Vec<u8> = key.clone().into();
                let val_vec: Vec<u8> = val.clone().into();
                db.insert(key_vec.clone(), val_vec.clone());
                conn.write_string("OK");

                //wrapper related - initialize first then send request
                let key_string = String::from_utf8(key_vec).expect("UTF-8 error");
                let val_string = String::from_utf8(val_vec).expect("UTF-8 error");
                
                // Initialize the KV store wrapper
                let initialized = wrapper.pin_mut().init();
                if initialized {
                    println!("KV store initialized successfully");
                    
                    // Send SET request to KV store via wrapper
                    let request = format!("put:{}:{}", key_string, val_string);
                    let req_id = wrapper.pin_mut().sendtoqueue(request);
                    
                    if req_id.0 >= 0 {
                        println!("SET request sent to KV store with ID: {}", req_id.0);
                        let response = wrapper.pin_mut().recvfromqueue(req_id);
                        println!("KV store SET response: {}", response);
                    } else {
                        println!("Failed to send SET request to KV store");
                    }
                } else {
                    println!("Failed to initialize KV store");
                }
            } else {
                conn.write_error("ERR invalid SET args");
            }
        }
        _ => {
            conn.write_error("ERR unsupported command or wrong arg count");
        }
    }
}

fn main() -> std::io::Result<()> {
    let db: Mutex<HashMap<Vec<u8>, Vec<u8>>> = Mutex::new(HashMap::new());
    let mut server = makocon::listen("127.0.0.1:6380", db).unwrap();
    server.command = Some(kv_handler);
    println!("Listening on {}", server.local_addr());
    println!("Note: KV store wrapper auto-initializes on first command in each request.");
    server.serve().unwrap();
    Ok(())
}
