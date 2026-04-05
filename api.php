<?php
/**
 * ═══════════════════════════════════════════════════════════
 * LUMIN Traffic Controller — MySQL REST API for ESP32
 * 
 * This API acts as a bridge between the ESP32 and MySQL database.
 * The ESP32 makes HTTP requests here instead of Firebase.
 * 
 * Endpoints:
 *   GET  /api.php?action=config          → Read device_config
 *   POST /api.php?action=update          → Update device_config
 *   POST /api.php?action=log             → Add log entry
 *   POST /api.php?action=heartbeat       → Update device status
 * 
 * ═══════════════════════════════════════════════════════════
 */

// Database configuration — Uses environment variables for Vercel, falls back to defaults
$dbHost = getenv('MYSQL_HOST') ?: 'localhost';
$dbName = getenv('MYSQL_DB') ?: 'traffic_controller';
$dbUser = getenv('MYSQL_USER') ?: 'root';
$dbPass = getenv('MYSQL_PASS') ?: '';

define('DB_HOST', $dbHost);
define('DB_NAME', $dbName);
define('DB_USER', $dbUser);
define('DB_PASS', $dbPass);

// Response headers
header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

// Handle preflight
if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(200);
    exit;
}

// ═══════════════════════════════════════════════════════════
// DATABASE CONNECTION
// ═══════════════════════════════════════════════════════════
function getDB() {
    static $pdo = null;
    if ($pdo === null) {
        try {
            $pdo = new PDO(
                'mysql:host=' . DB_HOST . ';dbname=' . DB_NAME . ';charset=utf8mb4',
                DB_USER,
                DB_PASS,
                [
                    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
                    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
                    PDO::ATTR_EMULATE_PREPARES => false
                ]
            );
        } catch (PDOException $e) {
            jsonError('Database connection failed: ' . $e->getMessage());
        }
    }
    return $pdo;
}

function jsonError($msg, $code = 500) {
    http_response_code($code);
    echo json_encode(['success' => false, 'error' => $msg]);
    exit;
}

function jsonSuccess($data = []) {
    echo json_encode(['success' => true, 'data' => $data]);
    exit;
}

// ═══════════════════════════════════════════════════════════
// ACTIONS
// ═══════════════════════════════════════════════════════════

/**
 * GET /api.php?action=config
 * Returns current device configuration
 */
function actionConfig() {
    $pdo = getDB();
    $stmt = $pdo->query('SELECT * FROM device_config WHERE id = 1');
    $config = $stmt->fetch();
    
    if (!$config) {
        jsonError('Config not found', 404);
    }
    
    jsonSuccess($config);
}

/**
 * POST /api.php?action=update
 * Body: {enabled, mode, manual_light, current_light, remaining_time, updated_at}
 */
function actionUpdate() {
    $input = json_decode(file_get_contents('php://input'), true);
    error_log('API UPDATE INPUT: ' . print_r($input, true));
    if (!$input) {
        jsonError('Invalid JSON body', 400);
    }
    
    $pdo = getDB();
    
    $fields = [];
    $values = [];
    
    if (isset($input['enabled'])) {
        $fields[] = 'enabled = ?';
        $values[] = $input['enabled'] ? 1 : 0;
        error_log('API UPDATE: enabled=' . ($input['enabled'] ? 1 : 0));
    }
    if (isset($input['mode'])) {
        $fields[] = 'mode = ?';
        $values[] = $input['mode'];
    }
    if (isset($input['manual_light'])) {
        $fields[] = 'manual_light = ?';
        $values[] = $input['manual_light'];
    }
    if (isset($input['current_light'])) {
        $fields[] = 'current_light = ?';
        $values[] = $input['current_light'];
    }
    if (isset($input['remaining_time'])) {
        $fields[] = 'remaining_time = ?';
        $values[] = (int)$input['remaining_time'];
    }
    if (isset($input['updated_at'])) {
        $fields[] = 'updated_at = ?';
        $values[] = (int)$input['updated_at'];
    }
    if (isset($input['updated_by'])) {
        $fields[] = 'updated_by = ?';
        $values[] = $input['updated_by'];
    }
    
    if (empty($fields)) {
        jsonError('No fields to update', 400);
    }
    
    $sql = 'UPDATE device_config SET ' . implode(', ', $fields) . ' WHERE id = 1';
    error_log('API UPDATE SQL: ' . $sql);
    error_log('API UPDATE VALUES: ' . print_r($values, true));
    $stmt = $pdo->prepare($sql);
    $stmt->execute($values);
    error_log('API UPDATE ROWS: ' . $stmt->rowCount());
    
    jsonSuccess(['updated' => $stmt->rowCount()]);
}

/**
 * POST /api.php?action=log
 * Body: {light, mode, source, created_at}
 */
function actionLog() {
    $input = json_decode(file_get_contents('php://input'), true);
    if (!$input) {
        jsonError('Invalid JSON body', 400);
    }
    
    $pdo = getDB();
    $stmt = $pdo->prepare(
        'INSERT INTO traffic_log (light, mode, source, created_at) VALUES (?, ?, ?, ?)'
    );
    $stmt->execute([
        $input['light'] ?? 'off',
        $input['mode'] ?? 'auto',
        $input['source'] ?? 'device',
        $input['created_at'] ?? (int)(microtime(true) * 1000)
    ]);
    
    jsonSuccess(['id' => $pdo->lastInsertId()]);
}

/**
 * POST /api.php?action=heartbeat
 * Body: {online, last_seen, ip_address}
 */
function actionHeartbeat() {
    $input = json_decode(file_get_contents('php://input'), true);
    if (!$input) {
        $input = [];
    }
    
    $pdo = getDB();
    // Use server time for last_seen, not ESP32 time
    $now = (int)(microtime(true) * 1000);
    $stmt = $pdo->prepare(
        'INSERT INTO device_status (id, online, last_seen, ip_address) VALUES (1, ?, ?, ?) ON DUPLICATE KEY UPDATE online = ?, last_seen = ?, ip_address = ?'
    );
    $online = ($input['online'] ?? true) ? 1 : 0;
    $ip = $input['ip_address'] ?? $_SERVER['REMOTE_ADDR'] ?? null;
    $stmt->execute([$online, $now, $ip, $online, $now, $ip]);
    
    jsonSuccess(['updated' => $stmt->rowCount(), 'last_seen' => $now]);
}

/**
 * GET /api.php?action=status
 * Returns device online status
 */
function actionStatus() {
    $pdo = getDB();
    // Ensure row exists
    $pdo->query('INSERT IGNORE INTO device_status (id, online, last_seen, ip_address) VALUES (1, 0, 0, NULL)');
    $stmt = $pdo->query('SELECT * FROM device_status WHERE id = 1');
    $status = $stmt->fetch();
    
    if (!$status) {
        jsonError('Status not found', 404);
    }
    
    jsonSuccess($status);
}

/**
 * GET /api.php?action=logs&limit=50
 * Returns recent log entries
 */
function actionLogs() {
    $limit = isset($_GET['limit']) ? (int)$_GET['limit'] : 50;
    $limit = min(100, max(1, $limit));
    
    $pdo = getDB();
    $stmt = $pdo->prepare(
        'SELECT * FROM traffic_log ORDER BY created_at DESC LIMIT ?'
    );
    $stmt->execute([$limit]);
    
    jsonSuccess($stmt->fetchAll());
}

// ═══════════════════════════════════════════════════════════
// ROUTER
// ═══════════════════════════════════════════════════════════
$action = $_GET['action'] ?? '';

switch ($action) {
    case 'config':
        if ($_SERVER['REQUEST_METHOD'] !== 'GET') {
            jsonError('Method not allowed', 405);
        }
        actionConfig();
        break;
        
    case 'update':
        if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
            jsonError('Method not allowed', 405);
        }
        actionUpdate();
        break;
        
    case 'log':
        if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
            jsonError('Method not allowed', 405);
        }
        actionLog();
        break;
        
    case 'heartbeat':
        if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
            jsonError('Method not allowed', 405);
        }
        actionHeartbeat();
        break;
        
    case 'status':
        if ($_SERVER['REQUEST_METHOD'] !== 'GET') {
            jsonError('Method not allowed', 405);
        }
        actionStatus();
        break;
        
    case 'logs':
        if ($_SERVER['REQUEST_METHOD'] !== 'GET') {
            jsonError('Method not allowed', 405);
        }
        actionLogs();
        break;
        
    default:
        jsonError('Unknown action. Use: config|update|log|heartbeat|status|logs', 400);
}
