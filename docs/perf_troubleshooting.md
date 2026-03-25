# 连续两次QPS暴跌95%：面经打假+新手第一次性能排查连踩七个坑？

这篇文章是我送给自己的接触编程半周年的礼物。

本篇文章有图有字有数据，总计有7个大坑，另外还有基础认知错误，有归因错误，有歪打正着，性能回退，再附有3个演算公式，2道面试题，2个linux源码小片段，2个官方文档摘要。

字数在2.9w左右，篇幅较长，名词我用灰体标出，文章提纲放出来大家可以根据需求查阅，我个人认为四、(1)、4.问题最有价值，用Linux源码论证了一个大量面经中出现的错误。

以下是踩坑摘要：
1. 在 docker-compose.yml 中误用swarm集群下的deploy配置

2. docker-inspect为什么不显示配置，cgroup版本兼容问题

3. 单线程下的 % CPU (s) 反直觉

4. **25年后端开发面试: tcp_fin_timeout 可以缩短 TIME_WAIT时长，错**
5. **tcp_fin_timeout 可以减少 TIME-WAIT 状态的连接？也错**
6. 21 年微信面试：为什么 linux 默认关闭 tcp_tw_reuse?
7. SO_REUSEADDR 与 tcp_tw_reuse 是一个东西吗？不是

以下是本文结构：
![alt text](image-12.png)
## 第一次QPS从9500暴跌至400+

### 前提引入

在第一阶段的echo阻塞IO单线程服务器完成后，我进入了第二阶段，引入select多路复用，本想迅速产出《select瓶颈分析》的，但先做了两件事情：拆分业务逻辑; 更改测试环境。

测试环境方法从宿主机本地回环更改为从宿主机去压测容器服务，根据本地回环时9.5k的QPS，又考虑到`docker-proxy`路径会有一定性能损耗，所以我预期看见约为6k-8k的QPS，数据却显示：400+ QPS？性能暴跌！于是有了这篇博客。

### 三个数据现象
测试方法简介：控制变量、预热60s、三次压测取中位，每次间隔时间（宏TCP_TIME_WAIT +5）秒
具体环境信息见 补充：测试环境、方法说明

1. 宿主机本地回环测试正常
```text
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
```
2. 容器内本地回环测试正常
```text
root@6a4c76bec84b:/app# wrk -t12 -c100 -d30s http://localhost:8080
Running 30s test @ http://localhost:8080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     7.54ms    0.88ms  15.82ms   71.43%
    Req/Sec     1.05k   117.98     1.28k    72.37%
  379247 requests in 28.66s, 34.72MB read
  Socket errors: connect 0, read 0, write 0, timeout 94
Requests/sec:  13233.67   📍
Transfer/sec:      1.21MB
```
3. 从宿主机压测容器服务数据异常暴跌

```text
=== 100并发原始测试结果 ===
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
```
### 修复思路

#### (1) 两种本地回环数据均正常，首先排除程序本身的问题
#### (2) `docker-proxy`正常情况下性能损失应该在 5-10% 左右，不可能差 20 倍,其次排除普通的环境数据损耗
#### (3) 认知错误：怀疑CPU被其他进程占用/受限严重
虽然开头犯的错误很基础，但它让我一路追到了许多真实的坑。

讲清楚概念之前，先讲一下我的场景：top在容器里面分析，容器限制了4核CPU、容器内只有这一个进程、进程服务是单线程架构。
而top 命令中显示指的是整个系统（这里指容器）总体CPU占用率，可以简略计算为：

**系统CPU占用率 %CPU(s)≈ （核心1占用率+核心2占用率+⋯+核心n占用率）/核心总数量**

**​进程CPU占用率 %CPU≈进程使用的CPU时间片总和/平均采样时间**

解释一下这两个公式，系统CPU使用率是核心的CPU占用率的平均值，所以无论如何不会超过100%，在我的**单线程场景中，进程就算跑满也最多跑满1个核心**，均摊为4份也就是在纯单线程程序中，系统CPU占用率理应在25%以下，当占用率接近25%的时候进程已经到极限了！而不是说还有75%。

而进程CPU占用率是**进程在固定时间内占用了多长时间的CPU**，比如采样间隔时间是3秒，这三秒内进程吃了总计为2秒左右的CPU时间片，则这三秒的进程CPU占用率为66.7%。

但是，为什么进程CPU占用率可以超过100%？因为大部分程序涉及**多核并行**，比如进程开了四个线程，同时跑在4个核上，采样间隔时间依旧3秒，这三秒内进程吃了总计2+2+2+2秒的CPU时间片，那么这三秒进程则CPU占用率则为266.7%。这也可以说明为什么%CPU三秒切换一遍。

而我在排查中犯的错是**混淆了%CPU(s)的us和%CPU，盯着三秒变一次的%CPU以为是%CPU(s)**

猜测：是否容器CPU限制太死，因为我昨天刚在`docker-compose.yml`里面把CPU限制为4核，或者其他进程占用了CPU

思考：目前源码为单线程，所以QPS取决于单核性能，与宿主机的12核或者容器内4核无关，QPS理论值应为宿主机QPS的8500，我想观察一下CPU占用率，有可能严重低于25%

验证：这里用到top，**`watch -n 1 'docker exec im-gateway-test top -bn1 | head -20'`**，实时监控压测时的容器的资源使用情况和进程状态

预期：想看见总CPU使用率从始至终严重低于25%

现象：因为是压测过程中观察进程CPU使用率始终变动，我只摘要格式其他来口述，格式如下
```bash
Every 1.0s: docker exec im-gateway-test top -bn1 | he...  Y: Sun Mar 15 17:50:46 2026

top - 09:50:47 up  3:00,  0 users,  load average: 57.25, 31.66, 22.08
Tasks:   2 total,   2 running,   0 sleeping,   0 stopped,   0 zombie
%Cpu(s): 12.8 us📍, 39.1 sy,  0.0 ni, 36.3 id,  0.0 wa,  0.0 hi, 11.7 si,  0.0 st
MiB Mem :   7823.2 total,   5503.7 free,   1735.3 used,    584.2 buff/cache
MiB Swap:   2048.0 total,   2048.0 free,      0.0 used.   5923.5 avail Mem

    PID USER      PR  NI    VIRT    RES    SHR S  %CPU  %MEM     TIME+ COMMAND
      1 appuser   20   0    6056   3456   3328 R  86.7📍   0.0   0:22.80 TCPserver
    521 appuser   20   0    7184   3072   2688 R   0.0   0.0   0:00.05 top
```
我写了一个简单的脚本带了时间戳输出，我们现在只看%Cpu(s)中的us和%CPU
```bash
=================================================================
CPU监控报告 - 开始时间: Sun Mar 15 17:54:58 CST 2026
容器: im-gateway-test
采样间隔: 3秒
=================================================================
时间               us%            进程CPU%    进程名
-----------------------------------------------------------------
17:55:22             0.0           0.0      TCPserver
17:55:26             1.1           0.0      TCPserver
17:55:30             3.3           0.0      TCPserver
17:55:34             17.3          6.7      TCPserver
17:55:38             16.4          86.7     TCPserver
17:55:41             11.8          68.8     TCPserver
17:55:45             3.4           27.8     TCPserver
17:55:49             4.4           20.0     TCPserver
17:55:50             3.3           33.3     TCPserver
17:55:54             2.2           31.2     TCPserver
17:55:58             4.3           25.0     TCPserver
```

观察到的CPU使用率时而33.3%时而飙到97%，因为误判它是系统CPU使用率（本该是不超过25%），于是误判我的进程没有受到4核CPU的限制。
现在根据图表来看，我们概念计算是正确的，us%始终没有超出25%，而%CPU则可以接近100%
当时结论：不符合预期，（提前说明⚠️这是一个错误结论）认为进程没有受到CPU限制

#### (4)检查CPU限制是否生效

猜测：如果进程没有受到4核CPU的限制，容器偷偷用宿主机的核心，导致上下文切换过多然后性能暴跌？（提前说明⚠️这是一个错误结论导致的错误预测，因为单线程只能跑一个核，就算不给容器限制CPU也不会出现这种情况）

预期：CPU限制失效

验证1：检查`.env.test`和`docker-compose.yml`是否真的配置

`.env.test`片段如下
```bash
CPU_LIMIT=4                 # 限制4核CPU
MEMORY_LIMIT=2G             # 限制2G内存
CPU_RESERVATION=2           # 预留2核
MEMORY_RESERVATION=1G       # 预留1G内存
```
`docker-compose.yml`片段如下

```yaml
deploy:    # 具体见.env.test
      resources:
        limits:
          cpus: ${CPU_LIMIT:-0}        
          memory: ${MEMORY_LIMIT:-0}    
        reservations:
          cpus: ${CPU_RESERVATION:-0}
          memory: ${MEMORY_RESERVATION:-0}
```
结论1：配置上确实限制了CPU为4核

然后我就卡住了。CPU限制到底有没有生效？怎么验证？

于是我把问题告诉AI，提示词如下：

“我的采用纯单线程架构select模型写了一个TCPserver，遇到了一个现象：在`wrk -t12 -c100 -d30s`下，容器与宿主机内本地回环QPS约为8000+，而从宿主机压测容器内进程QPS为400+。我排除了程序本身的问题与普通的`docker-proxy`路径损耗。我怀疑是CPU受限制，结果用top看见CPU使用率始终变动，0→33.3→42→12→97，现在怀疑是CPU限制失效，可是我明明都在文件里标明了限制4核CPU？所以为什么明明限制了却显得生效，如果不是CPU限制失效，那还有可能是什么问题？”

并附上了我的`.env.test`和`docker-compose.yml`

AI回答我：“在 `docker-compose up` 时，`deploy` 只在 `swarm 模式`生效”

原来我踩了这样一个坑：误用`deploy`配置。
`deploy.resources`配置仅在部署到`Swarm集群`时生效，指令是`docker stack deploy`，而`docker compose up -d`下资源限制会被忽略，而且不报错

结论2：CPU限制了，但因为用的是`deploy`配置，没有生效

于是我修改`docker-compose.yml`，先注释掉原来的`deploy`配置，再直接加资源限制

`docker-compose.yml`片段如下
```yaml
    # 资源限制
    cpus: 4
    mem_limit: 2g
    mem_reservation: 1g
```
现在看一下，修改`docker-compose.yml`中的配置方式后，CPU是否受到限制？如果现在受到了限制，立即跑一下脚本看看压测数据是否能恢复？

验证：**`docker inspect im-gateway-test | grep -A 30"HostConfig"`**，查看容器的资源限制配置

现象如下：
```text
"HostConfig"
        "HostConfig": {
            "Binds": null,
            "ContainerIDFile": "",
            "LogConfig": {
                "Type": "json-file",
                "Config": {}
            },
            "NetworkMode": "cpp-im-gateway_im-network",
            "PortBindings": {
                "8080/tcp": [
                    {
                        "HostIp": "",
                        "HostPort": "18080"
                    }
                ]
            },
            "RestartPolicy": {
                "Name": "no",
                "MaximumRetryCount": 0
            },
            "AutoRemove": false,
            "VolumeDriver": "",
            "VolumesFrom": null,
            "ConsoleSize": [
                0,
                0
            ],
            "CapAdd": [
                "CAP_SYS_PTRACE"
            ],
            "CapDrop": null,
```

现象：可以看见`HostConfig`里面没有一条关于`NanoCpus、Memory`等字段，这让我更懵了，明明在`.env.test`和`docker-compose.yml`中限制了CPU，而且也没有采取`deploy`模式，为何CPU限制仍然没有生效？

思考：虽然我非常想拼尽全力让CPU限制生效，静下来想想，如果文件配置真的是正确的，排除语法错误、配置错误。有可能问题出在文件解析的层面或者显示层面？于是我去查询了资料，内容涉及docker与Linux内核在配置方面的交互。


最上层：你的 `docker-compose.yml`（写的配置）。
↑
中间层：`Docker Daemon`（负责解析配置，并翻译成内核指令）。
↑
最底层：Linux 内核的 `Cgroups`（真正执行限制的地方）。


猜测：也许配置正确，但是负责解析配置的`Docker Daemon`出错，导致其实`Cgroups`里面没有执行限制

验证3：直接查看`Cgroups`配置，如果配置未生效则说明中间出问题了

```text
docker exec im-gateway-test cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us
docker exec im-gateway-test cat /sys/fs/cgroup/cpu/cpu.cfs_period_us
docker exec im-gateway-test cat /sys/fs/cgroup/memory/memory.limit_in_bytes
```

预期：看见与容器资源限制不符合的，与系统资源一致的配置

结果：
```text
cat: /sys/fs/cgroup/cpu/cpu.cfs_quota_us: No such file or directory
cat: /sys/fs/cgroup/cpu/cpu.cfs_period_us: No such file or directory
cat: /sys/fs/cgroup/memory/memory.limit_in_bytes: No such file or directory
```
我意识到我用的不是`croups`，而是`croups v2`，查询指令后再次输入指令

验证4：
```text
docker exec im-gateway-test cat /sys/fs/cgroup/cpu.max
docker exec im-gateway-test cat /sys/fs/cgroup/memory.max
docker exec im-gateway-test cat /sys/fs/cgroup/memory.current
```
结果：
```text
400000 100000    CPU是4核！？
2147483648       内存为2G，也生效了
4866048          目前只占用了4.8M
```
这让我又蒙了，如果配置生效了，刚才在`docker inspect`指令下，在`"HostConfig"`怎么看不见？为什么我看到的系统CPU使用率一直在跳？经过查询，

问题1解答：`cgroup`版本兼容问题
虽然我自己是通过AI得知的答案，但我们可以看看AI的参考文献，以下是docker engine api的文档摘要

"online_cpus or cpu_stats.online_cpus is nil then for **compatibility with older daemons** the length of the corresponding cpu_usage.percpu_usage array should be used. **On a cgroup v2 host, the following fields are not set**

 * blkio_stats: all fields other than io_service_bytes_recursive * cpu_stats: cpu_usage.percpu_useage 
 * memory_stats: max_usage and failcnt **Also, memory_stats.stats fields are incompatible with cgroup v1.**"


 也就是说因为`cgroup v2`的文件结构全都变了，`cpu.cfs_quota_us`等等老字段在读取的时候正常读取，确保底层限制生效，又因为v1和v2的数据结构不兼容，所以API不兼容v1的那些字段，这就导致了明明生效了但是`docker inspect`中没有输出。

 问题2解答：一直在跳的CPU是有两个原因，是因为把进程CPU使用率误以为是系统CPU使用率，进程CPU使用率是隔一段采样时间变化一次，第二是用到的是`watch -n 1`去执行top指令，属于一个实时监控，每间隔一秒更新一次

猜测：刚才显示问题是个误会，`deploy`模式被修改后，我们通过`cgroup`看见容器CPU限制生效了，那么压测数据也应该恢复正常了吧！

验证：运行压测脚本`.scripts/benchmark.sh`如下
```bash
检查服务状态...
✓ 服务运行正常 (HTTP 200)

[1/5] 预热环境 (60秒)
✓ 预热完成

[2/5] 基准测试 (100并发)
  QPS: 1596.71

[3/5] 阶梯加压测试
--- 测试 100 并发 ---
  QPS:   487.55  延迟: 17.24ms  P99: 110.84ms  错误:   0  \033[0;32m正常\033[0m
  # 很抱歉我知道延迟和P99高的恐怖，
  # 这是我下一步要解决的问题
```
恐怖的数据，毫无好转。

事已至此，我们只能先排除CPU限制的问题。话又说回来，CPU使用率为什么会高于25%？经过资料搜查，我终于明白我看到的是进程CPU使用率，也就是说我因为搞混了两个使用率，导致排查绕了一大圈，但是在这条远路上学到了`deploy`配置问题和`docker inspect`问题还有简单的docker与linux在配置方面的交互。



#### (5)把`docker-proxy`这个因素从排除队列里拉回来

目前现状：很绝望，绕了一大圈，发现跟CPU没关系，排查就这样回到了原点。没关系，我们先来看一下`docker-proxy`这个可疑的对象在我们场景（宿主机内wrk客户端-容器内进程服务之间）的位置。

![alt text](image-3.png)

首先我们先看一下`docker-proxy`的详细信息，确认一下该进程有没有正常运行

验证1：**ps aux | grep docker-proxy**，看一下`docker-proxy`的详细信息

现象t
```text
lizining   25674  0.0  0.0   4096  1920 pts/6    S+   22:04   0:00 grep --color=auto docker-proxy
```
`grep --color=auto docker-proxy`这一句实际上是grep在查询`docker-proxy`进程时产生的进程。所以居然显示没有`docker-proxy`这个进程！这让我觉得不可能，万一又是什么因素导致其实有进程但是没有显示呢？于是我想再验证一下。

验证2：**docker port im-gateway-test**，看一下端口映射在不在
现象：终端无输出结果，也就是说没有端口映射

我这才想起来，是我手动输入了**docker stop im-gateway-test**，那么我们就先解决了启动问题再说，不然后续也无法测试。

#### (6)归因错误：启动`docker-proxy`

我删除了容器并且重建，
```text
docker compose down --remove-orphans
docker compose up -d --force-recreate
```
现在来验证一下容器是否重建成功

验证1:`ps aux | grep docker-proxy`
现象：
```text
root       26562  0.0  0.0 1746984 4480 ?        Sl   22:07   0:00 /usr/bin/docker-proxy -proto tcp -host-ip 0.0.0.0 -host-port 18080 -container-ip 172.18.0.2 -container-port 8080 -use-listen-fd
root       26569  0.0  0.0 1746984 4480 ?        Sl   22:07   0:00 /usr/bin/docker-proxy -proto tcp -host-ip :: -host-port 18080 -container-ip 172.18.0.2 -container-port 8080 -use-listen-fd
lizining   26778  0.0  0.0   4096  1920 pts/6    S+   22:08   0:00 grep --color=auto docker-proxy

```
两个都是`docker-proxy`进程，其中一个是IPv4代理，另一个是IPv6代理

验证2：**docker port im-gateway-test**看看端口映射
现象：
```text
8080/tcp -> 0.0.0.0:18080
8080/tcp -> [::]:18080
```
果然，容器很正常，只是被我手动关闭了所以显示未启动，现在重建后启动成功了。现在运行试试看好了，如果运行结果没有恢复性能，就可以排除启动与否的因素了。

#### (7)歪打正着：运行前顺便清理一下系统

我没有先去分析`docker-proxy`如何带来这么大的性能损耗，但我已经决定先把目标锁定在`docker-proxy`这条路径上，于是从想从bridge模式换成host模式，区别在于host模式没有`客户端→iptables→docker-proxy→veth pair→容器内进程`这条路径。这里打个小广告，对这条路径感兴趣的读者，我主页大概率会有一篇《宿主机-网络-容器全链路分析》，主要涉及docker与内核交互与计算这条路径性能损耗，欢迎观看！

如果切换为`host`模式性能可以恢复正常，我就可以把排查重点放在这条路径上面。在切换到`host`之前，AI提示我一个可能性———系统状态混乱，它的建议如下：
![alt text](image-4.png)

我有点怀疑，先是开了个新窗口确认一下“压测出现性能暴跌35倍，已排除CPU因素，除了`docker-proxy`因素外，有没有可能是之前的测试把系统状态搞乱了”，AI回答有可能，并且提供了数据库连接池、GC回收等一系列问题，我锁定了其中一条回答——TCP/IP 端口与 `TIME_WAIT` 堆积，我其实设置了`SO_REUSEADDR`，但是随手清理一下没有坏处。（提前说明⚠️这是一个错误行为）

于是我执行了AI的语句，去“清理系统状态”
```text
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
sudo sysctl -w net.ipv4.tcp_fin_timeout=30

sudo systemctl restart docker
docker compose up -d
```
现在，顺手清理系统之后可以检验"`docker-proxy`启动问题"了
运行压测脚本`./scripts/benchmark.sh`如下
```bash
[1/5] 预热环境 (60秒)
✓ 预热完成

[2/5] 基准测试 (100并发)
  QPS: 10755.12

[3/5] 阶梯加压测试
--- 测试 100 并发 ---
QPS: 10210.94  延迟: 28.52ms  P99: 820.85ms  错误:   0  \033[0;32m正常\033[0m
# 很抱歉延迟和P99都高的恐怖，这是我下一步要解决的问题
```
结果，**“启动`docker-proxy`”与“顺手清理系统”这两件事**做完后，数据终于恢复至1w+！而且居然比原来的8500 QPS还要高20%！这还真是歪打正着，幸好没有直接去查`docker-proxy`全链路，也没有先切换为host模式。所以，果然是`docker-proxy`未启动导致的！终于终结这个问题了！

### 修复后的commit message：
我这边翻译成中文方便大家阅读，注意这条commit里面可以体现当时我的4个错误与不严谨，我会在去## 踩坑与错误总结中重新审视这条commit并且提出修改措施。
```text
commit 5c74ce
作者: lizining1231 lizining1231@outlook.com
日期: 2026年2月19日 周四 22:57:56 +0800

fix(Docker): 解决因**`docker-proxy` 未启动**导致 QPS 降至 100+ 的问题

  • 问题: 在100并发下，本地主机访问达到8500 QPS，而从主机访问容器仅为187 QPS；尽管设置了4核
    制，**CPU使用率仍超过33.3%且不稳定。**
  • 原因: 在 docker-compose 中错误使用了 deploy.resources（该配置仅在 swarm 模式下生效）；错误的端口映射导致 `docker-proxy` 未启动。
  • 复现步骤:

    在 docker-compose.yml 中使用 deploy.resources

    启动容器: docker compose up -d

    验证 docker-proxy 缺失: ps aux | grep docker-proxy

    运行压力测试: wrk -t12 -c100 http://localhost:18080
  • 修复方案:

    将 deploy.resources 替换为标准 cpus 格式

    重建容器: docker compose down && docker compose up -d

  **调整内核参数**:
    sudo sysctl -w net.ipv4.tcp_tw_reuse=1
    sudo sysctl -w net.ipv4.tcp_fin_timeout=30
  • 结果: QPS从187恢复至10210，达到预期的4核容器性能。

```
## 修复QPS至1w+后隔天测试再次暴跌至400+

### 前提引入
在QPS恢复至1w+后，第二天打开电脑，又跑了一次脚本，令人绝望的事情来了，数据再次跌回400+

### 数据现象

运行压测脚本`./scripts/benchmark.sh`如下
```bash
检查服务状态...
✓ 服务运行正常 (HTTP 200)

[1/5] 预热环境 (60秒)
✓ 预热完成

[2/5] 基准测试 (100并发)
  QPS: 1762.03

[3/5] 阶梯加压测试
--- 测试 100 并发 ---
  QPS:   **410.56**  延迟: 8.86ms  P99: 10.88ms  错误:   0  \033[0;32m正常\033[0m
```
### 修复思路

1. 先查看是不是上次的修改重启后失效了
2. 其次考虑是否为环境、编译器缓存残留
3. 最后如果都不行就重新排查

### 排查过程与解决

猜测1：`docker-proxy`可能又没有运行！

验证1：**docker ps**
现象：
```text
CONTAINER ID   IMAGE                       COMMAND                  CREATED        STATUS          PORTS                                           NAMES
893898753d68   cpp-im-gateway-im-gateway   "/app/entrypoint.sh …"   35 hours ago   Up 12 seconds   0.0.0.0:18080->8080/tcp, [::]:18080->8080/tcp   im-gateway-test
```
端口映射正常
```text
lizining@Y:~/projects/cpp-im-gateway$ ps aux | grep docker-proxy
root        9661  0.0  0.0 1746984 4480 ?        Sl   14:46   0:00 /usr/bin/docker-proxy -proto tcp -host-ip 0.0.0.0 -host-port 18080 -container-ip 172.18.0.2 -container-port 8080 -use-listen-fd
root        9668  0.0  0.0 1746984 4352 ?        Sl   14:46   0:00 /usr/bin/docker-proxy -proto tcp -host-ip :: -host-port 18080 -container-ip 172.18.0.2 -container-port 8080 -use-listen-fd
lizining   10759  0.0  0.0   4096  1920 pts/4    S+   14:47   0:00 grep --color=auto docker-proxy
```
两个进程都在。
结论1：排除`docker-proxy`未启动因素

猜测2：查看`docker-compose`是否为标准 cpus 格式、以及底层CPU限制是否生效

验证：查看`docker-compose.yml`
```yaml
services:
  im-gateway:
    build: .
    container_name: "im-gateway-${NODE_ENV:-test}"
    ports:
      - "${EXTERNAL_PORT:-18080}:${PORT:-8080}"
    
    # 资源限制
    cpus: 4
    mem_limit: 2g
    mem_reservation: 1g
```

验证：底层CPU限制是否生效
```text
docker exec im-gateway-test cat /sys/fs/cgroup/cpu.max
docker exec im-gateway-test cat /sys/fs/cgroup/memory.max
docker exec im-gateway-test cat /sys/fs/cgroup/memory.current
```
现象：
```text
400000 100000   生效了！
2147483648
8892416
```
居然不是docker-proxy未启动的问题，也不是CPU限制的问题，那么，只剩下最后一条顺手修改了...

猜测3：也许跟内核参数有关系

验证：两个参数
```text
sysctl net.ipv4.tcp_tw_reuse
net.ipv4.tcp_tw_reuse = 2（默认值）

sysctl net.ipv4.tcp_fin_timeout
net.ipv4.tcp_fin_timeout = 60（默认值）
```
内核参数不在了！

于是我立即把参数调整为昨天的，
```text
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
sudo sysctl -w net.ipv4.tcp_fin_timeout=30
```
现在再来跑压测脚本，
重新设置内核参数后，数据再次恢复至1w+
```bash
[1/5] 预热环境 (60秒)
✓ 预热完成

[2/5] 基准测试 (100并发)
  QPS: 11106.67

[3/5] 阶梯加压测试
--- 测试 100 并发 ---
  **QPS: 10455.17**  延迟:  9.07ms  P99: 14.03ms  
```
内核参数在重启后恢复了默认值，原来现在的2和60才是导致我跑出400 QPS的罪魁祸首！也就是说AI建议我清理系统的时候，我以为1和30就是默认值，其实更改为1和30属于参数优化。**所以之前的`docker-proxy`启动也是一个归因错误**，从这次暴跌的验证中就可以看出来，在`docker-proxy`启动等相同前提下，只修改内核参数即可直接导致性能回升到正常1w+。

### 修复后的commit message
```text
fix(sysctl): 解决因参数优化失效导致QPS再次降低至400+的问题
• 问题：修复上一次QPS暴跌的一天后，在100并发下，本地主机访问达到8500 QPS，而从宿主机测试容器内服务则暴跌至400+。
• 原因: net.ipv4.tcp_tw_reuse和net.ipv4.tcp_fin_timeout在重启后恢复默认值
• 修复方案：

1. 创建独立配置文件etc/sysctl.d/99-local.conf，使得重启后参数仍然有效
2. 在项目内备份内核参数配置config/99-local.conf

• 结果: QPS从478恢复至10210，达到预期的性能。
• 反思：
1. 第一次修复时的数据多次测试实为400+，在commit message中仅采取了第一次测试数据187
2. 第一次修复不应该同时做"docker-proxy未启动"与"内核参数优化"两件事情，导致归因错误（归因为docker-proxy未启动）
3. 没有理解内核参数优化的重要性，以为仅仅是优化数据，实际上有时候是支撑系统
• 复现步骤:
1. 回滚
git reset --soft 5c74ce
2. 临时恢复参数默认值
sudo sysctl -w net.ipv4.tcp_tw_reuse=2
sudo sysctl -w net.ipv4.tcp_fin_timeout=60
```
## 文章核心：计算、实验、推演参数如何支撑进程⭐

疑惑来了，**明明只是环境的区别，参数如何起到如此大的作用的？如果是内核参数原因，为何本地回环没有受影响？**

### 三态参数tcp_tw_reuse

tcp_fin_timeout的解读与澄清我会放在坑点说明里面，解释这个性能问题更重要的是tcp_tw_reuse，我们需要先知道它是什么样的一个参数

1. `tcp_tw_reuse`：它可以绕过`TIME_WAIT`状态，让新连接可以复用旧的socket，这是最关键的。

根据2018年6月4日Linux的一个commit message摘要
```text
net-tcp: extend tcp_tw_reuse sysctl to enable loopback only optimization

This changes the /proc/sys/net/ipv4/tcp_tw_reuse from a boolean
to an integer.

It now takes the values 0, 1 and 2, where 0 and 1 behave as before,
while 2 enables timewait socket reuse only for sockets that we can
prove are loopback connections:
```
Linux4.4版本时这个补丁被合并，`tcp_tw_reuse` 早已扩展为一个**三态的**参数：

参数值    	含义
0	禁用	    不允许重用 `TIME-WAIT` 状态的socket
1	全局启用	允许为所有新的出站连接重用 `TIME-WAIT` socket
2	仅限回环	仅允许为回环（Loopback）连接重用 `TIME-WAIT` socket（默认值）

完美的解释了为什么两种本地回环的时候QPS正常，而从宿主机去压测容器内进程则性能崩塌，因为`tcp_tw_reuse`在我系统的默认值为2！不允许为我们场景的新连接复用socket。

难道从宿主机去压测容器进程这个场景不属于本地回环吗？不是在自己的本机上进行吗？注意，从宿主机到容器走的是内核网桥`（docker0）`，这对内核来说不是`loopback`而是跨设备通信，所以参数为2对我们来说就相当于禁用了这个功能。

所以参数修改前后本质上的区别是：**是否允许复用TIME_WAIT状态的socket**

### 利特尔定律QPS的计算与公式原理推演

我们或许可以来结合select模型推演一下，不允许我们场景复用`TIME_WAIT`下的socket（也就是当参数为0或2），对应的QPS约为多少？符合实际吗？
`tcp_tw_reuse`=2，`tcp_fin_timeout`=60

**最大可持续QPS = 可用端口数 ÷ TIME-WAIT持续时间**

这个公式来源于特尔法则，在我们短连接场景，**每秒连接数=每秒请求数QPS=每秒消耗端口数**，所以可以这样理解公式：每秒消耗端口数=总计可用端口数量÷每个端口用完需要冷却的数量。但即使这么说，我还是不太理解什么意思，所以我以我的理解讲一下。（我接下来的解释会区分 秒初 秒末 秒内 ）

说一下这个公式的具体理解：我们总共有28232个端口，每个端口使用完都需要“冷却”60秒（`TIME_WAIT`）才能再次使用，第一个端口在‘第一秒初’时刻用完要等到第‘六十秒末’时刻才能再次被用
也就是说在每一秒内，我们会导致一批端口进入TIME_WAIT模式，那么**设这批端口数量为x**，也就代表着每秒连接了多少端口（QPS）
在第一分钟内，无需考虑复用问题，因为第六十秒末这个时刻，才出现第一个冷却结束的可被复用的端口，也就是在第一秒初最早的被使用的那个，所以60秒内我们会用掉60批新端口
也就可以得到方程：60 x = 28232
为什么要拿第一分钟内的去推演之后所有时间下的公式？因为这个运作模式其实在**第六十秒末进入了循环**，之后的每一秒内都有x个端口被用掉，又有x个端口从冷却状态恢复（因为在 第x-60秒内 的时候用掉了x批端口），系统从此永远保持着60x个端口全部被占用

那么**QPS_MAX=28232÷60≈470**

因为目前没有锁竞争、CPU、网络等等瓶颈，我们只考虑端口这个瓶颈，470这个最大值还是很符合我们压测数据的。

那么我们数据怎么会稳定在100+呢

所以，当我们把`tcp_tw_reuse`这个参数设置为1，使得端口不需要度过`TIME_WAIT`就能被复用，就是打破了刚才的循环，突破470 QPS这个端口不可复用瓶颈。

### 两组对照实验来了解`tcp_tw_reuse`在系统与环境作用

 #### 以参数`tcp_tw_reuse`设置为变量

为了进行一个对照试验，
我这边把`tcp_fin_timeout`保持不变默认60，环境保持也不变为宿主机去压测容器进程，
1.当`tcp_tw_reuse`=0
```text
Running 30s test @ http://localhost:18080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    40.61ms   31.50ms 267.01ms   73.76%
    Req/Sec   124.64     91.98     0.92k    94.01%
  Latency Distribution
     50%   33.44ms
     75%   57.79ms
     90%   82.56ms
     99%  143.28ms
  44094 requests in 28.07s, 4.04MB read
  Socket errors: connect 0, read 0, write 0, timeout 41
Requests/sec:   1570.98   📍
Transfer/sec:    147.28KB
```
2.当`tcp_tw_reuse`=1
```text
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
```
3.当`tcp_tw_reuse`=2
```text
Running 30s test @ http://localhost:18080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    12.34ms   23.92ms 651.16ms   98.86%
    Req/Sec   606.27    318.73     0.89k    79.12%
  Latency Distribution
     50%    9.93ms
     75%   11.13ms
     90%   13.41ms
     99%   48.63ms
  5778 requests in 30.08s, 541.69KB read
  Socket errors: connect 0, read 11323, write 0, timeout 0
Requests/sec:    192.09    📍
Transfer/sec:     18.01KB   
```
为什么参数为2比参数为0又低那么多?不都是没有起到复用效果吗。实际上当参数设置为2的时候，内核还要额外去检查"是否为环回连接"的判定逻辑，这增加了额外的处理开销。而参数直接设置为0则是明确禁止了复用，无需检查。
#### 以环境为变量
内核参数保持优化后的`tcp_tw_reuse`=1 不变

1. 宿主机本地回环测试
```text
lizining@Y:~/projects/cpp-im-gateway/build$ wrk -t12 -c100 -d30s http://localhost:8080
Running 30s test @ http://localhost:8080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    12.04ms    1.09ms  28.17ms   70.75%
    Req/Sec   661.59     61.71     1.02k    81.84%
  238478 requests in 27.83s, 21.83MB read
  Socket errors: connect 0, read 0, write 0, timeout 96
Requests/sec:   8568.11   📍
Transfer/sec:    803.26KB
```
我们会发现，参数`tcp_tw_reuse`从2设置为1之后，宿主机本地回环的数据反倒从9.5k降到8.5k，这是正常的，因为本地回环路径有所优化

2. 容器内本地回环测试
```text
root@3e4d1d418263:/app# wrk -t12 -c100 -d60s http://localhost:8080 
Running 1m test @ http://localhost:8080
  12 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    10.91ms    1.83ms  40.94ms   78.00%
    Req/Sec   710.20    123.24     0.96k    87.36%
  59443 requests in 0.92m, 5.44MB read
  Socket errors: connect 0, read 0, write 0, timeout 96
Requests/sec:   1074.50   📍
Transfer/sec:    100.73KB
```
这个数据也很有意思，又是怎么回事呢？当tcp_tw_reuse=1，宿主机本地回环很正常，从宿主机压测容器内服务也很正常，为什么偏偏容器内本地回环又引发了性能暴跌？
这里我们要提及tcp_tw_reuse的原理，当**五元组完全一致 + 新连接的时间戳 > 旧连接的最后时间戳**这两个条件完全符合的时候才对socket进行复用，容器内回环路径极快，再加上容器**croup进行限制的时候容易导致进程暂停**，时间戳暂停增长，到了检查符合条件的时候 可能会出现：新连接的时间戳 <= 旧连接的最后时间戳 的情况，内核检查条件失败就会拒绝复用socket，从而导致连接失败。
为什么cgroup限制同样存在于宿主机压测容器内进程的场景，却没有引发这个问题，因为宿主机压测容器内进程路径较长，数据包时间戳间隔被拉大，容易满足条件。
为什么宿主机本地回环没有引发这个问题，因为没有cgroup限流，时间戳正常单调递增

3. 从宿主机压测容器内服务（条件一模一样，我复用一下1.2的数据哈）
```text
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
```
## 踩坑与错误总结

### 可能帮助到大家的坑点说明与对应方案⭐

#### 1. **在docker-compose.yml中误用deploy配置**

(1)如何识别：确保文件配置了，但不生效也不报错。这种场景可以把这个可能性考虑进去。

(2)解释说明：`deploy.resources`配置仅在部署到`Swarm集群`时生效，指令是`docker stack deploy`，而在`docker compose up -d`下资源限制会被忽略，而且也不会不报错

(3)解决方案：经过搜查，这边给出三个方案，我的场景采用的是方案一，大家可以根据场景进行权衡

方案一：
操作：使用 `Compose v2`格式	放弃`deploy`，使用v2版本的专用字段（如`mem_limit`）
场景：单机环境下的开发、测试或生产部署
优点：配置直观、生效明确，不存在被配置了却被忽略的问题
缺点：无法使用`Swarm`的集群管理功能

方案二：
操作：使用兼容性模式，在`docker-compose up`命令中添加`--compatibility`标志，尝试将`deploy`配置转换为v2格式的等效设置。
场景：希望保留v3文件格式，但是在单机环境下快速测试的行为
优点：无需修改配置文件，命令简单
缺点：转换不完全，效果不确定，不推荐用于生产环境


方案三：
操作：修改为`Swarm`集群模式，将部署目标切换到`Swarm集群`，并使用`docker stack deploy`命令，让`deploy.resources`配置完全生效
场景：需要高可用、服务伸缩和跨主机部署的生产环境
优点：配置完全生效，并能利用`Swarm`的滚动更新、负载均衡等高级功能
缺点：学习和搭建`Swarm集群`

#### 2. **`docker-inspect`不显示配置**

(1)如何识别：确保文件配置了，但是在`docker inspect`指令下看不见配置，可以考虑这个可能性。如果确保文件配置且用`cgroup`确保生效了，该点可能性大大上升。

(2)解释说明：`docker inspect`的代码逻辑是为`cgroup v1`设计的，它会去老路径（如`/sys/fs/cgroup/devices/devices`）下寻找信息。而在`cgroup v2`中，部分控制器不再通过传统文件接口实现，而`docker inspect`找不到预期的文件，所以就无法在输出中显示这项配置。

(3)解决方案：直接读取 `cgroup v2` 的实际文件（比如 `cat /sys/fs/cgroup/.../memory.max`）

#### 3. **单线程下的%CPU(s)反直觉**

(1)如何识别：进程已经跑满了，但是`%CPU(s)`并不接近100%

(2)解释说明：人类的直觉是`%CPU`接近100%则是跑满，但是如果单线程进程的 `%CPU` 已经接近 100%（对于 对于多线程应用是100% × 容器核数），说明它已经用满了分配给它的计算资源，此时系统的 `%CPU(s)` 只是一个平均后的数字，不代表还有空闲的 CPU 可以压榨。

同理我们也可以得出在多线程程序中`%CPU(s)`并不意味着跑满了CPU。

#### 4. **某后端开发面试：`tcp_fin_timeout`可以缩短`TIME_WAIT`时长?错**

看了几篇文章，我认为这是一个非常值得讲的坑，面试官经常会追问一个基础问题：“如何优化或规避`TIME_WAIT`影响？”

这是我看到并截图的一些常见错误回答（无针对含义，仅为了体现这是一个常见误区）
![](image-5.png)

![](image-6.png)

就像图上一样，很多人会回答“调整`tcp_fin_timeout`来缩短`MSL时长`从而缩短`TIME_WAIT`时间”

实际上这是错误的，**Linux内核的`TIME_WAIT`时长是写死的不能通过参数修改，而且`tcp_fin_timeout`和`MSL`没有任何关系**。话不多说我们直接看源码：
在`linux/v6.19.7/source/include/net/tcp.h`的第142行

```c
#define TCP_TIMEWAIT_LEN (60*HZ) /* how long to wait to destroy TIME-WAIT
				  * state, about 60 seconds	*/
          // 这一句可以理解为TCP_FIN_TIMEOUT的默认值为TCP_TIMEWAIT_LEN时长
#define TCP_FIN_TIMEOUT	TCP_TIMEWAIT_LEN
                                 /* BSD style FIN_WAIT2 deadlock breaker.
				  * It used to be 3min, new value is 60sec,
				  * to combine FIN-WAIT-2 timeout with
				  * TIME-WAIT timer.
				  */
```
先讲`TCP_TIME_WAIT宏`，字面意义上很清晰，注释也明确说了`TCP_TIME_WAIT`宏是结束`TIME-WAIT`状态的时间，大约为60秒。经查验Linux内核也没有去定义MSL，而是直接**用宏定义写死`TIME_WAIT`时长为60s，是不能通过sysctl参数来修改的**，大家所说Linux的`MSL`为30s也是根据`TCP_TIME_WAIT`宏的一个算出来的时长。

要想缩短`TIME_WAIT`时长，是要**修改内核源码**然后再次进行编译的！并非像大量面经中所说通过某`sysctl`参数修改。

对了，有趣的是，阿里云25年时在自家系统	Alibaba Cloud Linux 2（内核版本4.19.43-13.al7起）和 Alibaba Cloud Linux 3 里面提供了一个真的能修改`TIME-WAIT`时长的参数`net.ipv4.tcp_tw_timeout`，侧面印证了原本的Linux里面`TIME-WAIT`时长无法被修改，对这个参数感兴趣的可以自行搜阅。

多种证据都可以表明Linux中的TIME-WAIT时长无法被修改:
```bash
lizining@Y:~/projects/cpp-im-gateway$ sudo sysctl -w net.ipv4.tcp_time_wait
[sudo] password for lizining: 
sysctl: command line(0): invalid syntax, continuing...
```

刚好它的邻居正是`TCP_FIN_TIMEOUT宏`，这才是`sysctl`参数里`tcp_fin_timeout`所修改的对象，我们通过四次挥手流程图来区分这两个参数。

![alt text](image-7.png)

咱们四次挥手有个`FIN-WAIT-2`时间，是在二次挥手收到了对方的ACK包，正在等待对方FIN包的时长。

这个`FIN-WAIT-2`状态的最大时长为内核中的`TCP_FIN_TIMEOUT宏`，这个宏是可以通过`sysctl`的`tcp_fin_timeout`参数来修改的。


我们来看源码：
`linux/v6.19.7/source/net/ipv4/sysctl_net_ipv4.c`文件中的第1079行的结构体
```c
{
		.procname	= "tcp_fin_timeout",   // 这就是参数名称
		.data		= &init_net.ipv4.sysctl_tcp_fin_timeout,   // 存储的位置
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_jiffies,   // 通过这个函数进行用户时间单位（秒）→内核时间单位（jifiess）的转化
	}
```



考虑到实际面试中总不能说“大家说的都是错的，我查过源码...”之类的话，所以我提供了这个版本的回答！

关于`TIME_WAIT`优化，常用的方法是：
1. 调整 `tcp_fin_timeout` 参数（控制FIN-WAIT-2超时）   
// 不用强调是缩短MSL、TIME_WAIT时长，那是错的
2. 开启 `tcp_tw_reuse`（允许复用TIME_WAIT连接）
3. 增加本地端口范围 `ip_local_port_range`
4. 调整 `tcp_max_tw_buckets` 限制总数

进阶版也可以说出来在内核中发现实际上`tcp_fin_timeout`
有人也许会想补充：可以调整`tcp_tw_recycle`这个参数，可以再加一句“不过Linux 4.12已经	正式移除 `tcp_tw_recycle` 参数，原因是在NAT环境容易丢包”

#### 5. **`tcp_fin_timeout`可以减少`TIME-WAIT`状态的连接？也错**

既然我们已经知道`TCP_FIN_TIMEOUT`不会缩短`TIME_WAIT`，**那`TCP_FIN_TIMEOUT`到底是如何优化`TIME_WAIT`问题的？**

仔细想想，`FIN-WAIT-2(tcp_fin_timeout)`是二次挥手后等待对方的FIN包的时长，如果超时过了默认值60s了，直接`RST`关闭，但我觉得这很矛盾，这**难道不会导致加快socket进入`TIME-WAIT`状态，从而导致`TIME-WAIT`状态的连接更多吗？**为什么说它可以优化`TIME-WAIT`呢？

以下是红帽的官方文档《Changing tcp_fin_timeout and tcp_max_tw_buckets》中的关于修改`tcp_fin_timeout`的风险，可以侧面体现它的作用在于尽快释放资源。

```text
If you set too large value to tcp_fin_timeout, 
the system may become out of port, file-descripter and memory.
If you set too small value, the system may leak delayed packets
```

实际上，它并不能减少`TIME-WAIT`状态的连接，`tcp_fin_timeout`=30实际上是针对有大量少`TIME-WAIT`状态连接**背后的的高并发场景**进行的优化，它的优化作用有点类似于不直击本质的“亡羊补牢”，通过缩短等待FIN包的时间来清理掉那些中间态的僵尸连接，**尽快释放资源，防止系统资源耗尽**，加剧`TIME-WAIT`问题。


#### 6. **21年微信面试：为什么linux默认关闭`tcp_tw_reuse`?**
既然`net.ipv4.tcp_tw_reuse`可以快速复用`TIME_WAIT`状态的连接，为什么Linux默认是关闭状态？

这道题其实问的就是`TIME_WAIT`的重要性，除了老生常谈的回答还可以加两个角度：
实际上**TIME_WAIT状态的设计初衷**是：既要让旧报文在`2MSL`中消失，又要确保最后的ACK能被重传。
**作为开发者**，我们开启`tcp_tw_reuse`参数是在牺牲部分的可靠性去换性能
从**linux角度**出发是默认可靠性优先的，所以会默认关闭`tcp_tw_reuse`


#### 7. `SO_REUSEADDR`与`tcp_tw_reuse`是一个东西吗？

不。这也是一个经典误区，因为这个内核参数和这个套接字选项长得太像了，都关于`TIME-WAIT`，都有`REUSE`

本质上是其实是**复用port和复用socket的区别**

`SO_REUSEADDR`是我们设置套接字的一个选项，设置后可以让还处于`TIME_WAIT`状态的端口被`bind()`,而通过设置内核参数``tcp_tw_reuse``可以让主动提出关闭并处于`TIME_WAIT`状态的五元组被复用，是完全相同的连接。

7个坑分享过了，最后，我没有面试过，却来分享面试回答有点奇怪，以上分享仅为我个人学习见解，欢迎批评指正。

### 我在排查中犯的错误

1. 从commit message中这句“尽管设置了4核限制，CPU使用率仍超过33.3%且不稳定。”

可以看出，我没有分清楚系统CPU使用率和进程CPU使用率，以为超过33.3%是一个错误点，标注在了问题现象里。我觉得犯这个错误有两个原因，

一、用工具不懂原理，针对这一点以后用不熟练的工具要先把完整终端回复复制给AI搞懂所需指标的位置、以及如何解读各指标。

二、目前还没学《操作系统》，我原先信奉边做边学，只是口头承认知识体系很重要，虽然这次经历让我顺便学到很多，但也让我感受到知识漏洞带来的惨痛，我会把《CSAPP》加入到学习规划中。

3. “`docker-compose `中错误使用了 `deploy.resources`”

这句话没毛病，但是事实上无论是否使用`deploy`配置，用`docker ispect`都查不到CPU限制，如果想要验证该修复生效，应该回滚到`deploy`配置然后用`cgroup v2`的指令去查。
```text
docker exec im-gateway-test cat /sys/fs/cgroup/cpu.max
docker exec im-gateway-test cat /sys/fs/cgroup/memory.max
docker exec im-gateway-test cat /sys/fs/cgroup/memory.current
```
2. 未保存完整排查过程，指令，终端回复，导致需要回滚再次复现数据。以后修复性能问B题保留完整终端记录与每一个数据报告。

3. 在部分修改中没有进行控制变量，从“调整内核参数”可以看出，应该在`docker-proxy`启动之后就立即进行压测，而不是顺手调个参数再进行压测，这是没有控制变量导致的归因错误。

4. 没有查验默认参数，盲目相信AI给的参数，以为`sudo sysctl -w net.ipv4.tcp_fin_timeout=30`是默认值

5. 从这句“主机访问容器仅为187 QPS（后续为400+）”可以看出初期测试方法不严谨，采取第一次数据做结果，而非多次数据稳定结论

### 如果让我再来一次，我会怎么做

先从简单的验证开始！并且每一步变动都进行压测
依旧先排除两个因素：

(1) 两种本地回环数据均正常，首先排除程序本身的问题
(2) 查看参数，结合IO模型与源码考虑是否有可能参数导致，而且对470上下的数字尤为敏感。如果AI给予我参数，我先查验这个参数在我系统中原本是什么，再查验参数的作用，最后再尝试修改。
(3) 如果还没解决，用`top`看CPU是否被其他进程占用过多，重视数据的解读。如果要看限制是否生效不看`docker inspect`，看`cgroup`
(4) 如果还没解决，排查`docker-proxy`全链路中某一环节的路径损耗

## 补充：测试环境、方法说明

### 一、硬件环境

| 组件 | 配置 | 说明 |
|------|------|------|
| CPU | Intel Core i7-10750H @ 2.60GHz | 6物理核心 / 12逻辑线程 |
| 内存 | 7.6GB | WSL2 动态分配 |
| 磁盘 | 1TB 虚拟磁盘 | Windows 主机磁盘映射 |
| 网卡 | 虚拟网卡 | WSL2 虚拟网络 |

### 二、软件环境

| 组件 | 版本 | 说明 |
|------|------|------|
| 宿主机系统 | Windows 10/11 | WSL2 宿主 |
| WSL2 版本 | 2 |  |
| Linux 发行版 | Ubuntu 24.04.1 LTS (Noble) |  |
| Linux 内核 | 5.15.x | 可用 `uname -r` 查看 |
| Docker 版本 | 29.2.1 | 社区版 |
| Docker 组件 | containerd v2.2.1, runc v1.3.4 |  |
| 测试工具 | wrk 4.2.0 |  |
| 被测服务 | cpp-im-gateway | 容器化部署 |

### 三、网络参数

| 参数 | 值 | 说明 |
|------|-----|------|
| tcp_tw_reuse | 0/1/2 | 测试变量 |
| tcp_fin_timeout | 60/30 | 测试变量 |
| ip_local_port_range | 32768 60999 |  |

### 四、容器配置

| 配置项 | 值 | 说明 |
|------|-----|------|
| 容器名 | im-gateway-test | |
| 网络模式 | bridge | |
| 端口映射 | 18080:8080 | |
| **CPU限制** | **4 cores** | 容器最多使用4个CPU核心 |
| **内存限制** | **2GB** | 硬限制 |
| **内存预留** | **1GB** | 保证至少1GB可用 |


## 题外话

毕竟是第一次做排查，最后，我想说一点题外话。这次经历我收获非常大，深深感受到了性能排查的曲折与有趣。在做些事情之前，我很盲目自信，觉得系统不是黑盒，系统是可拆解的，无论遇到什么疑难问题，都一定能通过学习和搜查去解决的。

其实这个问题从出现到解决距离有9天，复盘与回溯又写报告是整整又花了净时长将近18个小时，改前改后是两个参数的差别，实在是没有性价比吧。步骤看着不多但很绝望，当时无论如何就是找不到原因，一股脑把提示词和现象丢给AI，每给AI一种现象，AI都会给出4-5种可能性与对应指令，如果用不好就会更加混乱。每一次以为终于找到原因了，就眼巴巴看着屏幕，紧张祈祷这次性能可以回到正常。甚至开始怀疑自己是否应该去仿写或跟敲，自己瞎倒腾的找不到答案的到底算什么？如果跟着视频敲代码早就可以做完了吧？这时候我终于才有一种危机感，原来不是所有问题都能被解决。

终于歪打正着之后解决，第二天性能回归我是感觉很懵的，其实AI提示过我“可能是参数重启后丢失了”，我查看参数确实丢了但我蠢蠢地不以为然，觉得不可能是参数的问题，两个参数而已怎么会导致二十多倍的性能差异呢，**我居然宁愿相信是复杂的底层机制出了问题，也不愿相信是默认的配置毁了进程服务**，因为潜意识里觉得——如果问题那么简单、那我之前绕的路算什么？结果又绕了一大圈。

中间又因为git使用不规范（把文档放stage里不commit就切分支开发、在空分支开发）导致文档丢失、数据丢失，陷入至暗时刻。那段时间刚好也是寒假，整整3天我什么也没干，就像有一根刺在心里，无论干什么都一直在想着为什么会、这样到底怎么样才能解决，我始终觉得有点伤心。最后还是冷静下来去先查看之前的修改，尝试把参数改回来性能才恢复正常水平，当时的感受，没有狂喜，只有庆幸。

这让我深刻记住了太多错误，以及commit的重要性，这让我学会敬畏系统参数与默认配置的力量，去分析为什么参数在链路中起到作用。