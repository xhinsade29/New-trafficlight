<?php
// debug.php - Check database state
header('Content-Type: text/plain');

try {
    $pdo = new PDO('mysql:host=localhost;dbname=traffic_controller;charset=utf8mb4', 'root', '');
    $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    
    // Get current config
    $stmt = $pdo->query('SELECT * FROM device_config WHERE id = 1');
    $config = $stmt->fetch(PDO::FETCH_ASSOC);
    
    echo "=== DEVICE_CONFIG ===\n";
    print_r($config);
    
    echo "\n=== DEVICE_STATUS ===\n";
    $stmt = $pdo->query('SELECT * FROM device_status WHERE id = 1');
    $status = $stmt->fetch(PDO::FETCH_ASSOC);
    print_r($status);
    
    echo "\n=== RECENT LOGS ===\n";
    $stmt = $pdo->query('SELECT * FROM traffic_log ORDER BY created_at DESC LIMIT 5');
    $logs = $stmt->fetchAll(PDO::FETCH_ASSOC);
    print_r($logs);
    
} catch (Exception $e) {
    echo "ERROR: " . $e->getMessage();
}
