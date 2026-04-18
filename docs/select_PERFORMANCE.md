# SELECT 阶段性能报告

## 最终性能

## 演进/排查/优化过程
1. 解决QPS暴跌95%问题
2. 解决
3. 

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

数据摘要：wrk -t12 -c100 -d30s
Latency Distribution
     50%    8.54ms
     75%    9.52ms
     90%   10.62ms
     99%  954.74ms
现象：
1. 并发数由100递增至1500的过程，QPS始终稳定在10000+
2. 100并发下P99抖动严重，范围是10ms-1000ms
3. 200并发-1800并发，P99稳定至1000ms

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

更改后现象：未得到缓解，与先前一样
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

先进行限制每次处理的请求量，实现如下：

1. 在Socket类中创建新的成员对象,string类型的recv_buffer, 并创建对应获取方法getRecv_buffer()。
2. 将原本的buffer变更为char[]类型的temp_buffer, 作为函数内的临时缓冲区，用于接收数据
3. 将temp_buffer的数据append到recv_buffer, 拆分过多的请求量，拼接不完整的请求，每次只处理一个请求

更改后现象：
1. 并发数由100递增至1500的过程，QPS始终稳定在10000+
2. 100并发下P99不再抖动，稳定至11ms-12ms间
3. 200并发-1800并发，P99依旧稳定在1000ms

怀疑依旧是调度不均或达到系统瓶颈、我需要先让脚本输出一下200并发以后的P99去判断是调度不均还是达到系统瓶颈

跑了一遍压测脚本，100并发的QPS从1w+跌倒7k+，P99也崩到了600ms，明明昨天跑了五六遍，现象都一致，我现在毫无头绪，我再去跑一遍

Running 30s test @ http://localhost:18080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    25.78ms   93.12ms 983.92ms   97.49%
    Req/Sec   653.28     89.38     0.90k    74.02%
  Latency Distribution
     50%   11.96ms
     75%   13.50ms
     90%   15.49ms
     99%  636.76ms🔺
  235394 requests in 31.06s, 21.55MB read
Requests/sec:   7578.63📍
Transfer/sec:    710.50KB

还有一个关键现象，就是随着并发数上升，P50、P75、P90也在递增，而且仅仅是P50都在高并发下崩的一塌糊涂

第二遍:
Running 30s test @ http://localhost:18080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    37.49ms  151.36ms   1.36s    96.64%
    Req/Sec   701.27     58.14     0.86k    82.36%
  Latency Distribution
     50%   11.34ms
     75%   12.56ms
     90%   13.89ms
     99%    1.02s 🔺
  252809 requests in 31.44s, 23.15MB read
Requests/sec:   8040.22📍
Transfer/sec:    753.77KB

第三遍：
Running 30s test @ http://localhost:18080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    25.70ms   91.44ms 962.77ms   97.50%
    Req/Sec   643.74     72.11   818.00     69.84%
  Latency Distribution
     50%   12.24ms
     75%   13.62ms
     90%   15.35ms
     99%  626.13ms🔺
  232055 requests in 31.05s, 21.25MB read
Requests/sec:   7474.11📍
Transfer/sec:    700.70KB

1. 先检测参数配置还在不在:

lizining@Y:~/projects/cpp-im-gateway$ sysctl net.ipv4.tcp_tw_reuse
net.ipv4.tcp_tw_reuse = 1
lizining@Y:~/projects/cpp-im-gateway$ sysctl net.ipv4.tcp_fin_timeout
net.ipv4.tcp_fin_timeout = 30
✅在

2. 回滚到创建单独缓冲区前判断一下，是缓冲区引发的问题还是环境问题，可是明明缓冲区改完之后跑了5-6遍，QPS都非常稳定，大概率问题出在环境上

3. 还没有尝试回滚，电脑提示进入省电模式，有可能是因为电脑未插电而且电量低！

充满电之后完美复现
  QPS: 11163.43  延迟:  8.49ms  P99: 11.96ms  错误:   0  \033[0;32m正常\033[0m
--- 测试 200 并发 ---
  QPS: 11318.97  延迟: 122.89ms  P99: 1.01s  错误:   0  \033[0;31m过载\033[0m
--- 测试 400 并发 ---
  QPS: 11358.20  延迟: 186.13ms  P99: 1.15s  错误:   0  \033[0;31m过载\033[0m
--- 测试 600 并发 ---
  QPS: 10758.29  延迟: 178.77ms  P99: 1.18s  错误:   0  \033[0;31m过载\033[0m
--- 测试 800 并发 ---
  QPS: 10306.50  延迟: 182.50ms  P99: 1.24s  错误:   0  \033[0;31m过载\033[0m
--- 测试 1000 并发 ---
  QPS: 10566.85  延迟: 170.18ms  P99: 1.24s  错误:   0  \033[0;31m过载\033[0m

惨痛教训：以后性能无故回退先检查最基础的问题、再检查参数等软问题
