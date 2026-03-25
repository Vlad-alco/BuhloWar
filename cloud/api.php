<?php
/**
 * BuhloWar IoT Backend API
 * Receives telemetry from ESP32 and serves commands and push notifications
 */

header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type, Authorization');
header('Content-Type: application/json');

// Config
$config = [
    'db_file' => dirname(__FILE__) . '/buhlo_data.json',
    'commands_file' => dirname(__FILE__) . '/buhlo_commands.json',
    'devices_file' => dirname(__FILE__) . '/buhlo_devices.json',
    'api_key' => 'BUHLO_SECRET_KEY_2026',
    'service_account_file' => dirname(__FILE__) . '/buhlowar-a93cd-firebase-adminsdk-fbsvc-f8247fff19.json'
];

function readJson($file) {
    if (!file_exists($file)) return [];
    $content = file_get_contents($file);
    return json_decode($content, true) ?: [];
}

function writeJson($file, $data) {
    file_put_contents($file, json_encode($data, JSON_PRETTY_PRINT));
}

// Get OAuth2 access token from Service Account
function getAccessToken($serviceAccountFile) {
    $credentials = json_decode(file_get_contents($serviceAccountFile), true);
    
    $jwtHeader = base64_encode(json_encode(['alg' => 'RS256', 'typ' => 'JWT']));
    
    $now = time();
    $jwtClaims = base64_encode(json_encode([
        'iss' => $credentials['client_email'],
        'scope' => 'https://www.googleapis.com/auth/firebase.messaging',
        'aud' => $credentials['token_uri'],
        'iat' => $now,
        'exp' => $now + 3600
    ]));
    
    $privateKey = $credentials['private_key'];
    
    // Sign with RSA SHA256
    $signature = '';
    $data = "$jwtHeader.$jwtClaims";
    openssl_sign($data, $signature, $privateKey, OPENSSL_ALGO_SHA256);
    $jwt = "$data." . base64_encode($signature);
    
    // Exchange JWT for access token
    $ch = curl_init($credentials['token_uri']);
    curl_setopt($ch, CURLOPT_POST, true);
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_POSTFIELDS, http_build_query([
        'grant_type' => 'urn:ietf:params:oauth2:grant-type:jwt-bearer',
        'assertion' => $jwt
    ]));
    curl_setopt($ch, CURLOPT_SSL_VERIFYPEER, false);
    
    $response = curl_exec($ch);
    $result = json_decode($response, true);
    
    return $result['access_token'] ?? null;
}

// Send FCM notification using HTTP v1 API
function sendFCMNotification($accessToken, $token, $title, $body, $type = 'info') {
    $fcmUrl = 'https://fcm.googleapis.com/v1/projects/buhlowar-a93cd/messages:send';
    
    $message = [
        'message' => [
            'token' => $token,
            'notification' => [
                'title' => $title,
                'body' => $body
            ],
            'android' => [
                'priority' => $type === 'emergency' ? 'high' : 'normal',
                'notification' => [
                    'channel_id' => 'buhlowar_alerts',
                    'priority' => $type === 'emergency' ? 'max' : 'high',
                    'default_sound' => true,
                    'notification_priority' => $type === 'emergency' ? 'PRIORITY_MAX' : 'PRIORITY_HIGH'
                ]
            ],
            'data' => [
                'type' => $type,
                'title' => $title,
                'body' => $body,
                'timestamp' => (string)time()
            ]
        ]
    ];
    
    $headers = [
        'Authorization: Bearer ' . $accessToken,
        'Content-Type: application/json'
    ];
    
    $ch = curl_init($fcmUrl);
    curl_setopt($ch, CURLOPT_POST, true);
    curl_setopt($ch, CURLOPT_HTTPHEADER, $headers);
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_POSTFIELDS, json_encode($message));
    curl_setopt($ch, CURLOPT_SSL_VERIFYPEER, false);
    
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    return $httpCode === 200;
}

// Broadcast to all registered devices
function broadcastNotification($title, $body, $type = 'info') {
    global $config;
    
    if (!file_exists($config['service_account_file'])) {
        error_log("FCM: Service account file not found");
        return 0;
    }
    
    $accessToken = getAccessToken($config['service_account_file']);
    if (!$accessToken) {
        error_log("FCM: Failed to get access token");
        return 0;
    }
    
    $devices = readJson($config['devices_file']);
    $tokens = $devices['tokens'] ?? [];
    
    $sent = 0;
    foreach ($tokens as $token) {
        if (sendFCMNotification($accessToken, $token, $title, $body, $type)) {
            $sent++;
        }
    }
    
    return $sent;
}

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(200);
    exit;
}

$method = $_SERVER['REQUEST_METHOD'];

// ========== TELEMETRY ==========
if (isset($_GET['telemetry']) && $method === 'POST') {
    $input = file_get_contents('php://input');
    $data = json_decode($input, true);
    
    if ($data) {
        $telemetry = readJson($config['db_file']);
        $telemetry['last_update'] = time();
        $telemetry['data'] = $data;
        writeJson($config['db_file'], $telemetry);
        
        // === СОХРАНЕНИЕ ЛОГОВ ===
        if (isset($data['new_logs']) && strlen($data['new_logs']) > 0) {
            $logFile = dirname(__FILE__) . '/buhlo_cloud.log';
            $newLogs = $data['new_logs'];
            // Раскодируем \n обратно в переносы строк
            $newLogs = str_replace('\\n', "\n", $newLogs);
            // Дописываем в файл
            file_put_contents($logFile, $newLogs, FILE_APPEND);
            
            // Ограничиваем размер файла 100KB
            if (filesize($logFile) > 102400) {
                $content = file_get_contents($logFile);
                $content = substr($content, -81920); // Оставляем последние 80KB
                file_put_contents($logFile, $content);
            }
        }
        // ==========================
        
        // Check for alerts and send push
        checkAndSendAlerts($data);
        
        echo json_encode(['success' => true, 'timestamp' => time()]);
    } else {
        http_response_code(400);
        echo json_encode(['error' => 'Invalid JSON']);
    }
    exit;
}

// ========== SEND PUSH ==========
if (isset($_GET['push']) && $method === 'POST') {
    $input = file_get_contents('php://input');
    $data = json_decode($input, true);
    
    $title = $data['title'] ?? 'BuhloWar';
    $body = $data['body'] ?? '';
    $type = $data['type'] ?? 'info';
    
    $sent = broadcastNotification($title, $body, $type);
    
    echo json_encode(['success' => true, 'sent' => $sent]);
    exit;
}

// ========== REGISTER FCM TOKEN ==========
if (isset($_GET['register_device']) && $method === 'POST') {
    $input = file_get_contents('php://input');
    $data = json_decode($input, true);
    
    $token = $data['fcm_token'] ?? '';
    if (empty($token)) {
        http_response_code(400);
        echo json_encode(['error' => 'Token required']);
        exit;
    }
    
    $devices = readJson($config['devices_file']);
    if (!isset($devices['tokens'])) {
        $devices['tokens'] = [];
    }
    
    if (!in_array($token, $devices['tokens'])) {
        $devices['tokens'][] = $token;
        writeJson($config['devices_file'], $devices);
    }
    
    echo json_encode(['success' => true, 'registered' => true]);
    exit;
}

// ========== GET COMMANDS ==========
if (isset($_GET['commands'])) {
    $commands = readJson($config['commands_file']);
    $pending = $commands['pending'] ?? [];
    $commands['pending'] = [];
    writeJson($config['commands_file'], $commands);
    echo json_encode(['commands' => $pending, 'timestamp' => time()]);
    exit;
}

// ========== GET STATUS ==========
if (isset($_GET['status'])) {
    $telemetry = readJson($config['db_file']);
    $connected = isset($telemetry['last_update']) && 
                 (time() - $telemetry['last_update']) < 300;
    
    echo json_encode([
        'connected' => $connected,
        'last_update' => $telemetry['last_update'] ?? 0,
        'age' => $connected ? time() - $telemetry['last_update'] : null,
        'data' => $telemetry['data'] ?? null
    ]);
    exit;
}

// ========== POST COMMAND ==========
if (isset($_GET['cmd']) && $method === 'POST') {
    $cmd = $_GET['cmd'];
    $params = isset($_GET['params']) ? json_decode($_GET['params'], true) : [];
    
    $commands = readJson($config['commands_file']);
    $commands['pending'][] = [
        'command' => $cmd,
        'params' => $params,
        'timestamp' => time()
    ];
    writeJson($config['commands_file'], $commands);
    echo json_encode(['success' => true, 'queued' => count($commands['pending'])]);
    exit;
}

// ========== CHECK FOR ALERTS ==========
function checkAndSendAlerts($data) {
    // Поддержка обоих вариантов ключа: safety_code (веб) и safety (телеметрия)
    $safety = $data['safety_code'] ?? $data['safety'] ?? 0;
    $stage = $data['stage'] ?? '';
    $tsa = $data['tsa'] ?? 0;
    $tsar = $data['tsar'] ?? 0;
    
    if ($safety >= 2) {
        $title = '🚨 АВАРИЯ!';
        $body = "TSA: {$tsa}°C | ДТ: {$tsar}°C";
        broadcastNotification($title, $body, 'emergency');
    }
    elseif ($safety == 1) {
        $title = '⚠️ ВНИМАНИЕ';
        $body = "TSA: {$tsa}°C";
        broadcastNotification($title, $body, 'warning');
    }
}

// ========== GET SETTINGS ==========
if (isset($_GET['settings']) && $method === 'GET') {
    $settings = readJson($config['db_file']);
    echo json_encode([
        'settings' => $settings['settings'] ?? []
    ]);
    exit;
}

// ========== SAVE SETTINGS ==========
if (isset($_GET['settings']) && $method === 'POST') {
    $input = file_get_contents('php://input');
    $data = json_decode($input, true);
    
    if ($data) {
        $telemetry = readJson($config['db_file']);
        $telemetry['settings'] = $data;
        $telemetry['settings_last_update'] = time();
        writeJson($config['db_file'], $telemetry);
        echo json_encode(['success' => true]);
    } else {
        http_response_code(400);
        echo json_encode(['error' => 'Invalid JSON']);
    }
    exit;
}

// ========== GET LOGS ==========
if (isset($_GET['logs']) && $method === 'GET') {
    $logFile = dirname(__FILE__) . '/buhlo_cloud.log';
    if (file_exists($logFile)) {
        $logs = file_get_contents($logFile);
        header('Content-Type: text/plain; charset=utf-8');
        echo $logs;
    } else {
        echo "Логи отсутствуют";
    }
    exit;
}

// Default: status
$telemetry = readJson($config['db_file']);
echo json_encode([
    'connected' => isset($telemetry['last_update']),
    'last_update' => $telemetry['last_update'] ?? 0,
    'data' => $telemetry['data'] ?? null
]);
