<?php
/**
 * governance.php — Beacon Governance Worker Entry Point
 *
 * 此脚本由 C 扩展 beacon_governance_spawn() 通过 execl(php, "php", script) 启动。
 * 运行独立 PHP CLI 进程，带完整 VM，驱动 keepalive/discover 定时器。
 *
 * 零配置模式：内置 FileRegistry（文件注册中心），无需 composer 依赖。
 * 自定义模式：用户提供自己的 governance 脚本（通过 beacon.governance_script INI 指定），
 *            可使用 ReactPHP event loop + HTTP 适配器（etcd/Consul）。
 *
 * 设计依据：docs/design/governance-worker.md §四 治理 worker 主循环
 *           docs/design/spi.md §二 注册中心回调 + §2.2 FileRegistry
 * 业界对标：systemd service unit（fork+exec+monitor）、Swoole TaskWorker（独立进程+event loop）
 *
 * 事件循环设计决策（ADR）：
 *   MVP 用简单循环（usleep + time check），不依赖 ReactPHP。
 *   理由：文件模式用同步 I/O（file_put_contents/get_contents），2-3s 间隔足够快；
 *         ReactPHP 是 composer 依赖，零配置模式不应要求 composer install；
 *         HTTP 适配器（etcd/Consul）是 MVP+3，届时用户自定义脚本可用 ReactPHP。
 */

// ============================================================================
// 1. 信号处理与优雅退出
// ============================================================================

$running = true;
$deregistered = false;

// SIGTERM：FPM master 退出时通过 prctl(PR_SET_PDEATHSIG) 内核自动发送，或 MSHUTDOWN 主动 kill
pcntl_async_signals(true);
pcntl_signal(SIGTERM, function () use (&$running) {
    fwrite(STDERR, "[beacon-governance] received SIGTERM, shutting down\n");
    $running = false;
});

// ============================================================================
// 2. 配置读取（从 INI，治理 worker 加载 beacon 扩展后 ini_get 可用）
// ============================================================================

// 扩展检查：治理 worker 依赖 Beacon\Governance 类 API（calcHealth/storeNodes/commit）。
// spawn 时 C 层会传 -d extension=beacon.so；未加载说明扩展未安装到 extension-dir。
if (!extension_loaded('beacon')) {
    fwrite(STDERR, "[beacon-governance] beacon extension not loaded, exiting\n");
    exit(1);
}

$serviceName    = ini_get('beacon.service_name') ?: '';
$advertiseHost  = ini_get('beacon.advertise_host') ?: '';
$advertiseHostEnv = ini_get('beacon.advertise_host_env') ?: '';
$advertisePort  = ini_get('beacon.advertise_port') ?: '';
$registryEndpoint = ini_get('beacon.registry_endpoint') ?: '';
$keepaliveInterval = (int)(ini_get('beacon.keepalive_interval') ?: 3);
$pullInterval      = (int)(ini_get('beacon.pull_interval') ?: 2);
$heartbeatTtl      = (int)(ini_get('beacon.heartbeat_ttl') ?: 15);

if ($serviceName === '') {
    fwrite(STDERR, "[beacon-governance] beacon.service_name not configured, exiting\n");
    exit(1);
}

// ============================================================================
// 3. 地址解析（advertise_host 自动探测）
// ============================================================================

/**
 * 自动探测本机非 loopback IP 地址。
 *
 * 解析优先级（对标 docs/design/spi.md §六 AdvertiseResolver）：
 *   1. beacon.advertise_host_env → 从环境变量读（K8s downward API，如 POD_IP）
 *   2. beacon.advertise_host → INI 配置的地址
 *   3. 自动探测 → gethostbynamel(gethostname())，排除 127.*
 */
function detect_advertise_host(string $iniHost, string $envVar): string
{
    // 1. 从环境变量读（K8s downward API）
    if ($envVar !== '' && getenv($envVar) !== false) {
        return getenv($envVar);
    }

    // 2. INI 配置
    if ($iniHost !== '') {
        return $iniHost;
    }

    // 3. 自动探测
    $hostname = gethostname();
    if ($hostname === false) {
        return '127.0.0.1';
    }

    $ips = gethostbynamel($hostname);
    if (!$ips) {
        return '127.0.0.1';
    }

    // 排除 loopback 地址
    foreach ($ips as $ip) {
        if (!str_starts_with($ip, '127.')) {
            return $ip;
        }
    }

    return '127.0.0.1';
}

$advertiseHost = detect_advertise_host($advertiseHost, $advertiseHostEnv);
$advertisePort = $advertisePort !== '' ? (int)$advertisePort : 9000;

// ============================================================================
// 4. FileRegistry — 内置默认注册中心（零配置模式）
// ============================================================================

/**
 * 文件注册中心：用本地文件系统做服务注册与发现。
 *
 * 文件路径：{$dataDir}/{$service}.json
 * 文件格式：{"instance": {...}, "health": {...}, "registered_at": timestamp}
 *
 * 优势：零依赖、调试成本低（tail -f）、天然 L2 持久化。
 * 局限：单机模式（多机需共享文件系统或换 etcd/Consul）。
 *
 * 对标 docs/design/spi.md §2.2 FileRegistry。
 */
class FileRegistry
{
    private string $dataDir;

    public function __construct(string $dataDir = '/var/run/beacon')
    {
        $this->dataDir = $dataDir;
        if (!is_dir($dataDir)) {
            @mkdir($dataDir, 0755, true);
        }
    }

    /**
     * 注册服务实例（启动时调一次，keepalive 时覆盖写）。
     */
    public function register(array $ctx): void
    {
        $service = $ctx['service'];
        $file = "{$this->dataDir}/{$service}.json";

        $data = [
            'instance' => $ctx['instance'],
            'health' => $ctx['health'] ?? ['status' => 'not_ready'],
            'registered_at' => time(),
        ];

        if (@file_put_contents($file, json_encode($data, JSON_PRETTY_PRINT)) === false) {
            fwrite(STDERR, "[beacon-governance] register failed: cannot write {$file}\n");
        }
    }

    /**
     * 保活（覆盖写，更新 health 数据）。
     */
    public function keepalive(array $ctx): void
    {
        $this->register($ctx);
    }

    /**
     * 注销服务（删除文件）。
     */
    public function deregister(array $ctx): void
    {
        $service = $ctx['service'];
        $file = "{$this->dataDir}/{$service}.json";
        if (file_exists($file)) {
            @unlink($file);
        }
    }

    /**
     * 发现服务节点（读文件，返回节点数组）。
     *
     * 返回格式：[['id' => ..., 'host' => ..., 'port' => ..., 'status' => ..., 'weight' => ...], ...]
     */
    public function discover(string $service): array
    {
        $file = "{$this->dataDir}/{$service}.json";
        if (!file_exists($file)) {
            return [];
        }

        $content = @file_get_contents($file);
        if ($content === false) {
            return [];
        }

        $data = json_decode($content, true);
        if (!is_array($data) || !isset($data['instance'])) {
            return [];
        }

        // 单实例模式：文件中只有一个 instance
        $instance = $data['instance'];
        $health = $data['health'] ?? ['status' => 'ok'];

        // 合并 instance + health status
        $node = [
            'id' => $instance['id'] ?? ($service . '-1'),
            'host' => $instance['host'] ?? '127.0.0.1',
            'port' => $instance['port'] ?? 9000,
            'status' => $health['status'] ?? 'ok',
            'weight' => $instance['weight'] ?? 1,
        ];

        if (!empty($instance['methods'])) {
            $node['methods'] = $instance['methods'];
        }

        return [$node];
    }
}

// ============================================================================
// 5. 创建注册中心实例
// ============================================================================

// 零配置模式：registry_endpoint 空 → FileRegistry 默认路径 /var/run/beacon
// file:// 前缀：FileRegistry 指定路径（macOS 开发、容器等 /var/run 不可写场景）
// 其他 scheme（http/https）：用户应在自定义脚本中实现 HTTP 适配器
$registryDir = '/var/run/beacon';
if (str_starts_with($registryEndpoint, 'file://')) {
    $registryDir = substr($registryEndpoint, strlen('file://'));
}
$registry = new FileRegistry($registryDir);

// ============================================================================
// 6. 构建本服务实例信息
// ============================================================================

$instance = [
    'id' => $serviceName . '-' . getmypid(),
    'host' => $advertiseHost,
    'port' => $advertisePort,
    'status' => 'ok',
    'weight' => 1,
];

// ============================================================================
// 7. 启动注册（on_register）
// ============================================================================

$ctx = [
    'service' => $serviceName,
    'instance' => $instance,
    'health' => ['status' => 'not_ready'],
];

$registry->register($ctx);
fwrite(STDERR, "[beacon-governance] registered: service={$serviceName}, host={$advertiseHost}, port={$advertisePort}\n");

// ============================================================================
// 8. 主循环（keepalive + discover 定时器）
// ============================================================================

$lastKeepalive = 0;
$lastDiscover = 0;
// posix 扩展不可用时（--disable-posix 构建）跳过父进程检测，Linux 侧由 prctl 内核级兜底
$originalPpid = function_exists('posix_getppid') ? posix_getppid() : 0;

// 主循环 tick 间隔（100ms），对标设计文档 §5.2 的 100ms tick
$tickInterval = 100000; // microseconds

while ($running) {
    $now = time();

    // ---- 父进程死亡检测（getppid 兜底，对标 §5.2）----
    // prctl(PR_SET_PDEATHSIG) 是内核级检测，getppid 是用户态兜底
    if ($originalPpid > 0) {
        $currentPpid = posix_getppid();
        if ($currentPpid !== $originalPpid && $originalPpid !== 1) {
            fwrite(STDERR, "[beacon-governance] parent changed ({$originalPpid} -> {$currentPpid}), exiting\n");
            break;
        }
    }

    // ---- keepalive timer（每 keepalive_interval 秒）----
    if ($now - $lastKeepalive >= $keepaliveInterval) {
        try {
            // calcHealth() 读 shm 自计数 + 校准 busy + 更新 governance_alive 心跳
            $health = Beacon\Governance::calcHealth();

            $ctx = [
                'service' => $serviceName,
                'instance' => $instance,
                'health' => $health,
            ];

            $registry->keepalive($ctx);
        } catch (Throwable $e) {
            fwrite(STDERR, "[beacon-governance] keepalive error: {$e->getMessage()}\n");
        }
        $lastKeepalive = $now;
    }

    // ---- discover timer（每 pull_interval 秒）----
    if ($now - $lastDiscover >= $pullInterval) {
        try {
            // 发现 peer 节点 → 写 shm → commit
            $nodes = $registry->discover($serviceName);

            if (!empty($nodes)) {
                Beacon\Governance::storeNodes($serviceName, $nodes);
                Beacon\Governance::commit();
            }
        } catch (Throwable $e) {
            fwrite(STDERR, "[beacon-governance] discover error: {$e->getMessage()}\n");
        }
        $lastDiscover = $now;
    }

    usleep($tickInterval);
}

// ============================================================================
// 9. 优雅退出（on_deregister）
// ============================================================================

if (!$deregistered) {
    $deregistered = true;
    try {
        $ctx = [
            'service' => $serviceName,
            'instance' => $instance,
        ];
        $registry->deregister($ctx);
        fwrite(STDERR, "[beacon-governance] deregistered: service={$serviceName}\n");
    } catch (Throwable $e) {
        fwrite(STDERR, "[beacon-governance] deregister error: {$e->getMessage()}\n");
    }
}

fwrite(STDERR, "[beacon-governance] exiting\n");
exit(0);
