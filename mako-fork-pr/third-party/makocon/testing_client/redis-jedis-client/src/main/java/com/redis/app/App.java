package com.redis.app;

import redis.clients.jedis.Jedis;

public class App {
    public static void main(String[] args) {
        // Connect to Redis
        try (Jedis jedis = new Jedis("localhost", 6380)) {
            // Set a key-value pair
            jedis.set("greeting", "Hello from Jedis!");

            // Retrieve the value
            String value = jedis.get("greeting");
            System.out.println("Retrieved 'greeting': " + value);
        } catch (Exception e) {
            System.err.println("Error: " + e.getMessage());
        }
    }
}
