# SELECT 阶段性能报告

## 最终性能

## 演进/排查/优化过程

### 性能基准1：宿主机本地回环

lizining@Y:~/projects/cpp-im-gateway/build$ wrk -t12 -c100 -d60s http://localhost:8080
Running 1m test @ http://localhost:8080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    10.42ms  822.84us  23.96ms   71.58%
    Req/Sec   764.67     65.41     0.89k    87.38%
  550473 requests in 0.98m, 50.40MB read
  Socket errors: connect 0, read 0, write 0, timeout 190
Requests/sec:   9329.73   📍
Transfer/sec:      0.85MB

### 更改环境：QPS暴跌95%

环境由宿主机本地回环切换至从宿主机压测容器内进程，bridge模式，更改后的性能：
Running 30s test @ http://localhost:18080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    13.25ms   14.36ms 362.62ms   99.33%
    Req/Sec   566.58    219.24   730.00     87.37%
  Latency Distribution
     50%   12.29ms
     75%   13.54ms
     90%   14.70ms
     99%   20.28ms
  11424 requests in 26.11s, 1.05MB read
  Socket errors: connect 0, read 7840, write 0, timeout 96
Requests/sec:    437.56    📍
Transfer/sec:     41.02KB

(排查过程见study_draft/perf_troubleshooting.md)

经排查，调整两个内核参数tcp_fin_timeout由60设置为30，tcp_tw_reuse由2设置为1，更改后的性能：
Running 30s test @ http://localhost:18080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     9.58ms    1.53ms  38.55ms   76.99%
    Req/Sec   826.73     67.76     1.27k    90.56%
  Latency Distribution
     50%    9.56ms
     75%   10.43ms
     90%   11.18ms
     99%   13.22ms
  297982 requests in 27.60s, 27.28MB read
  Socket errors: connect 0, read 0, write 0, timeout 95
Requests/sec:  10795.68   📍
Transfer/sec:      0.99MB
### P99优化

现状：
Latency Distribution
     50%    8.54ms
     75%    9.52ms
     90%   10.62ms
     99%  954.74ms

1. 并发数由100递增至1500的过程，QPS始终稳定在10000+
2. 还没有实现计时器
3. 平均延迟与P99都极高且有抖动症状
4. P90与P99之间差了几百毫秒

猜测：select模型的阻塞

思考：select的工作机制是收到新的请求的时候遍历所有FD，筛选出有请求的FD，并且进行逐一处理，然后回来继续阻塞，等待收到新的请求。关键在于select在“进行逐一处理”的这段时间内无法识别新请求，这时候有FD发起请求A会被忽略。然而，在结束“进行逐一处理”这段时间后，它依然无法察觉刚才有FD发起了请求A，只能继续阻塞，直到在阻塞的时候收到新的请求B，才能再次遍历，再次处理A、B等等连接。而这时候A已经等了两个时间段了，1是从到达select接收缓冲区-select处理完当时一批连接的这段时间，2是select处理完当时一批连接-等待有新请求的时间

解决方案：设置select为非阻塞，每1ms主动遍历一遍

操作：
```c++
    timeval tv;
    tv.tv_sec=0;
    tv.tv_usec=1000;
    
    // 防止select阻塞导致P99飙升
    int activity=select(max_fd+1,&read_fds,NULL,NULL,&tv);
```

现状：未得到缓解
P99在100并发下抖动剧烈（10ms-1000ms），稳定于200并发，随200并发递增，P99从1000ms递增至1250ms
QPS于400并发达到峰值

经由strace系统调用：
```bash
strace: Process 3671 attached

^Cstrace: Process 3671 detached
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 32.80   23.421204          37    629488           write
 20.35   14.533408          69    209842         2 close
 17.83   12.729731          60    209816           sendto
 13.48    9.628342          22    419658         2 recvfrom
  8.12    5.800347          27    209828           accept
  7.42    5.297787          25    210343           pselect6
------ ----------- ----------- --------- --------- ----------------
100.00   71.410819          37   1888975         4 total
```
close的单次调用时间太长了，write次数过多，高达62.9万次

但是目前应该不是系统调用导致了系统瓶颈，更关键的问题在于P99与P90差距太大了，是调度不均的问题
调度无非就是三件事情，调度一次处理的时长，调度的条件决定调度顺序

时长方面：一次处理太久了？那有两个策略，一个是每次只处理n个活跃连接的所有请求（n＜总活跃连接数量），和每次只处理所有活跃连接的m个请求（m＜单连接总请求数量）