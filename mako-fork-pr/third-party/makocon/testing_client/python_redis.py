import redis

# Take an input {:port} from the command line
import argparse

# The default `sudo systemctl status redis-server` is on 6379
parser = argparse.ArgumentParser(description="Redis client")
parser.add_argument("--port", type=int, default=6380, help="Redis port")
args = parser.parse_args()

r = redis.Redis(host='localhost', port=args.port)

try:
    response = r.ping()
    print(f"Connected successfully:{response}")
except redis.ConnectionError as e:
    print(f"Failed:{e}")


def cleanup_redis():
    print("\n--- Cleanup Redis Data ---")
    try:
        # Get all keys and delete them
        keys = r.keys("*")
        if keys:
            deleted = r.delete(*keys)
            print(f"Deleted {deleted} keys")
        else:
            print("No keys to delete")
    except Exception as e:
        print(f"Cleanup not supported: {e}")


def basic_set_and_get():
    print("\n--- Basic SET and GET Operations ---")
    try:
        # Basic SET and GET operations
        r.set("greeting", "Hello from Redis-py")
        print(f"greeting: {r.get('greeting')}")

        # Overwriting existing key
        r.set("greeting", "Hello again!")
        print(f"greeting (updated): {r.get('greeting')}")
    except Exception as e:
        print(f"Basic operations not supported: {e}")


def numeric_operations():
    print("\n--- Numeric Operations ---")
    r.set("counter", "10")
    print(f"Initial counter: {r.get('counter')}")
    
    # Increment by 1
    try:
        r.incr("counter")
        print(f"After incr: {r.get('counter')}")
    except Exception as e:
        print(f"Incr not supported: {e}")
    
    # Increment by custom amount
    try:
        r.incrby("counter", 5)
        print(f"After incrby 5: {r.get('counter')}")
    except Exception as e:
        print(f"Incrby not supported: {e}")
    
    # Decrement by 1
    try:
        r.decr("counter")
        print(f"After decr: {r.get('counter')}")
    except Exception as e:
        print(f"Decr not supported: {e}")
    

def list_operations():
    print("\n--- List Operations ---")
    try:
        r.delete("mylist")  # Clear any existing list
        
        # Push elements to list
        r.lpush("mylist", "first", "second", "third")
        print(f"List after lpush: {r.lrange('mylist', 0, -1)}")
        
        # Push to right side
        r.rpush("mylist", "fourth")
        print(f"List after rpush: {r.lrange('mylist', 0, -1)}")
        
        # Pop from left
        left_item = r.lpop("mylist")
        print(f"Popped from left: {left_item}")
        print(f"List after lpop: {r.lrange('mylist', 0, -1)}")
        
        # Get list length
        length = r.llen("mylist")
        print(f"List length: {length}")
    except Exception as e:
        print(f"List operations not supported: {e}")


def hash_operations():
    print("\n--- Hash Operations ---")
    try:
        r.delete("user:1")  # Clear any existing hash
        
        # Set hash fields
        r.hset("user:1", "name", "Alice")
        r.hset("user:1", "age", "30")
        r.hset("user:1", "city", "New York")
        
        # Get single field
        name = r.hget("user:1", "name")
        print(f"User name: {name}")
        
        # Get all fields
        user_data = r.hgetall("user:1")
        print(f"All user data: {user_data}")
        
        # Get multiple fields
        fields = r.hmget("user:1", "name", "age")
        print(f"Name and age: {fields}")
        
        # Check if field exists
        exists = r.hexists("user:1", "email")
        print(f"Email field exists: {exists}")
    except Exception as e:
        print(f"Hash operations not supported: {e}")


def set_operations():
    print("\n--- Set Operations ---")
    try:
        r.delete("colors", "primary_colors")  # Clear any existing sets
        
        # Add members to set
        r.sadd("colors", "red", "green", "blue", "yellow")
        r.sadd("primary_colors", "red", "green", "blue")
        
        # Get all members
        all_colors = r.smembers("colors")
        print(f"All colors: {all_colors}")
        
        # Check membership
        is_member = r.sismember("colors", "red")
        print(f"Red is in colors: {is_member}")
        
        # Set intersection
        intersection = r.sinter("colors", "primary_colors")
        print(f"Intersection: {intersection}")
        
        # Set difference
        difference = r.sdiff("colors", "primary_colors")
        print(f"Difference: {difference}")
        
        # Set cardinality
        count = r.scard("colors")
        print(f"Number of colors: {count}")
    except Exception as e:
        print(f"Set operations not supported: {e}")


def key_management():
    print("\n--- Key Management ---")
    
    # Set some test keys
    r.set("temp1", "value1")
    r.set("temp2", "value2")
    
    # Check if key exists
    try:
        exists = r.exists("temp1")
        print(f"temp1 exists: {exists}")
    except Exception as e:
        print(f"Exists not supported: {e}")
    
    # Set expiration (TTL)
    try:
        r.expire("temp1", 10)
        ttl = r.ttl("temp1")
        print(f"temp1 TTL: {ttl} seconds")
    except Exception as e:
        print(f"Expire/TTL not supported: {e}")
    
    # Get all keys matching pattern
    try:
        keys = r.keys("temp*")
        print(f"Keys matching 'temp*': {keys}")
    except Exception as e:
        print(f"Keys pattern matching not supported: {e}")
    
    # Delete keys
    try:
        deleted = r.delete("temp2")
        print(f"Deleted temp2: {deleted}")
        
        # Check existence after deletion
        exists_after = r.exists("temp2")
        print(f"temp2 exists after deletion: {exists_after}")
    except Exception as e:
        print(f"Delete not supported: {e}")


def transaction_operations_1():
    # Example with direct MULTI/EXEC (no pipeline)
    try:
        print("\n--- Direct MULTI/EXEC (no pipeline) ---")
        r.execute_command("MULTI")
        r.execute_command("SET", "direct:key1", "value1")
        r.execute_command("SET", "direct:key2", "value2")
        r.execute_command("INCR", "direct:counter")
        r.execute_command("INCR", "direct:counter")
        results = r.execute_command("EXEC")
        print(f"Direct MULTI/EXEC results: {results}")
        print("direct:counter: ", r.get("direct:counter"))
        
    except Exception as e:
        print(f"Direct MULTI/EXEC not supported: {e}")


def transaction_operations_2():
    try:
        print("\n--- Transactions with pipeline ---")
        # Start a transaction
        pipe = r.pipeline()
        
        # Queue multiple operations
        pipe.set("account:1", "100")
        pipe.set("account:2", "50")
        pipe.incr("account:1")
        pipe.decr("account:2")
        
        # Execute all operations atomically
        results = pipe.execute()
        print(f"Transaction results: {results}")
        
        # Check final values
        account1 = r.get("account:1")
        account2 = r.get("account:2")
        print(f"Account 1: {account1}, Account 2: {account2}")
        
    except Exception as e:
        print(f"Transaction operations not supported: {e}")


def transaction_operations_3():
    # Example with MULTI/EXEC (alternative approach)
    try:
        print("\n--- MULTI/EXEC Transaction ---")
        pipe = r.pipeline()
        pipe.multi()
        
        pipe.set("tx:key1", "value1")
        pipe.set("tx:key2", "value2")
        pipe.get("tx:key1")
        
        results = pipe.execute()
        print(f"MULTI/EXEC results: {results}")
        
    except Exception as e:
        print(f"MULTI/EXEC not supported: {e}")


def transaction_operations_4():
    # Example with WATCH (optimistic locking)
    try:
        print("\n--- WATCH Transaction ---")
        r.set("balance", "100")
        
        # Watch a key for changes
        r.watch("balance")
        current_balance = int(r.get("balance"))
        
        # Start transaction
        pipe = r.pipeline()
        pipe.multi()
        
        # Modify the watched key
        new_balance = current_balance - 10
        pipe.set("balance", str(new_balance))
        
        # Execute (will fail if balance was modified by another client)
        results = pipe.execute()
        print(f"WATCH transaction results: {results}")
        print(f"Final balance: {r.get('balance')}")
        
    except Exception as e:
        print(f"WATCH transaction not supported: {e}")
    finally:
        # Always unwatch keys
        try:
            r.unwatch()
        except:
            pass


if __name__ == "__main__":
    cleanup_redis()
    basic_set_and_get()
    numeric_operations()
    list_operations()
    key_management()
    hash_operations()
    set_operations()
    transaction_operations_1()
    transaction_operations_2()
    transaction_operations_3()
    transaction_operations_4()