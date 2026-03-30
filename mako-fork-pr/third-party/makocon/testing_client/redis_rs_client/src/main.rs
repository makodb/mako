use redis::{Commands, RedisResult};

fn main() -> RedisResult<()> {
    // Connect to Redis
    let client = redis::Client::open("redis://127.0.0.1:6380/")?;
    let mut con = client.get_connection()?;

    // Set a key-value pair
    let _: () = con.set("greeting", "Hello from redis-rs")?;
    println!("Set 'greeting' to 'Hello from redis-rs'");

    // Retrieve the value
    let value: String = con.get("greeting")?;
    println!("Retrieved 'greeting': {}", value);

    // Set a key-value pair
    let _: () = con.set("byebye", "Bye from redis-rs")?;
    println!("Set 'byebye' to 'Bye from redis-rs'");

    // Retrieve the value
    let value: String = con.get("byebye")?;
    println!("Retrieved 'byebye': {}", value);

    Ok(())
}
