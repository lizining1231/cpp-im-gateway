#!/bin/bash
# 压测脚本 benchmark.sh
# 使用方法: ./benchmark.sh [环境] [端口]

set -e  # 出错停止

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ==================== 环境切换 ====================
ENV=${1:-"test"}  # 默认使用 test 环境
PORT=${2:-8080}

echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}压测脚本 - 环境: $ENV, 端口: $PORT${NC}"
echo -e "${BLUE}========================================${NC}"

# 查找环境配置文件（支持多个可能的位置）
POSSIBLE_PATHS=(
    ".env.$ENV"
    ".docker/.env/.env.$ENV"
    "config/.env.$ENV"
    "env/.env.$ENV"
    "../.env.$ENV"
)

ENV_FILE=""
for path in "${POSSIBLE_PATHS[@]}"; do
    if [ -f "$path" ]; then
        ENV_FILE="$path"
        echo -e "${GREEN}✓ 找到配置文件: $path${NC}"
        break
    fi
done

if [ -z "$ENV_FILE" ]; then
    echo -e "${RED}错误: 找不到环境配置文件 .env.$ENV${NC}"
    echo "查找位置:"
    printf '  - %s\n' "${POSSIBLE_PATHS[@]}"
    exit 1
fi

# 复制配置文件
echo -e "${YELLOW}切换到 $ENV 环境...${NC}"
cp "$ENV_FILE" .env

# 读取环境变量
if [ -f ".env" ]; then
    # 读取宿主机端口 (EXTERNAL_PORT)
    HOST_PORT=$(grep -E '^EXTERNAL_PORT=' .env | cut -d= -f2 | tr -d '"' | tr -d "'")
    HOST_PORT=${HOST_PORT:-$PORT}
    
    # 读取容器内端口 (PORT)
    CONTAINER_PORT=$(grep -E '^PORT=' .env | cut -d= -f2 | tr -d '"' | tr -d "'")
    CONTAINER_PORT=${CONTAINER_PORT:-8080}
else
    HOST_PORT=$PORT
    CONTAINER_PORT=8080
fi

echo -e "${GREEN}✓ 宿主机端口: $HOST_PORT, 容器内端口: $CONTAINER_PORT${NC}"

# 重启容器使用新环境
echo -e "${YELLOW}重启容器应用新环境...${NC}"
docker compose down 2>/dev/null || true
docker compose up -d

# 等待容器启动
echo -e "${YELLOW}等待容器启动...${NC}"
sleep 5

# 检查服务是否自动启动（只检查进程，不检查端口）
if docker exec im-gateway-$ENV pgrep TCPserver > /dev/null; then
    SERVER_PID=$(docker exec im-gateway-$ENV pgrep TCPserver | head -1)
    echo -e "${GREEN}✓ TCPserver 已自动启动 (PID: $SERVER_PID)${NC}"
else
    echo -e "${RED}✗ TCPserver 未自动启动，查看日志:${NC}"
    docker logs im-gateway-$ENV --tail 50
    exit 1
fi

# ==================== 压测配置 ====================
URL="http://localhost:$HOST_PORT"
DURATION=30  # 每次测试30秒
THREADS=$(nproc)  # 自动获取CPU核心数
REPORT_FILE="benchmark_report_${ENV}_$(date +%Y%m%d_%H%M%S).txt"

echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}高性能压测脚本${NC}"
echo -e "${BLUE}========================================${NC}"
echo "测试环境: $ENV"
echo "测试目标: $URL"
echo "CPU核心数: $THREADS"
echo "测试时长: ${DURATION}s/次"
echo "报告文件: $REPORT_FILE"
echo -e "${BLUE}========================================${NC}\n"

# 检查wrk是否安装
if ! command -v wrk &> /dev/null; then
    echo -e "${RED}错误: wrk 未安装${NC}"
    echo "请执行: sudo apt-get install wrk"
    exit 1
fi

# 检查服务是否可用（使用curl测试实际响应）
echo -e "${YELLOW}检查服务状态...${NC}"
MAX_RETRIES=10
RETRY_COUNT=0
while ! curl -s -o /dev/null -w "%{http_code}" http://localhost:$HOST_PORT | grep -q "200"; do
    RETRY_COUNT=$((RETRY_COUNT + 1))
    if [ $RETRY_COUNT -ge $MAX_RETRIES ]; then
        echo -e "${RED}错误: 服务启动失败 (HTTP 非200响应)${NC}"
        echo -e "\n${YELLOW}最后一次响应:${NC}"
        curl -v http://localhost:$HOST_PORT 2>&1 || true
        echo -e "\n${YELLOW}容器日志:${NC}"
        docker logs im-gateway-$ENV --tail 20
        exit 1
    fi
    echo -e "${YELLOW}等待服务启动... ($RETRY_COUNT/$MAX_RETRIES)${NC}"
    sleep 2
done
echo -e "${GREEN}✓ 服务运行正常 (HTTP 200)${NC}\n"

# 初始化结果文件
cat > $REPORT_FILE << EOF
===============================================================================
                    压测报告 - $ENV 环境
===============================================================================
测试环境: $ENV
测试目标: $URL
CPU核心数: $THREADS
测试时长: ${DURATION}s/次
测试时间: $(date '+%Y-%m-%d %H:%M:%S')
===============================================================================

一、测试结果汇总
-------------------------------------------------------------------------------
并发数    QPS      平均延迟   P99延迟    错误数    状态
-------------------------------------------------------------------------------
EOF

# 1. 预热
echo -e "${YELLOW}[1/5] 预热环境 (60秒)${NC}"
wrk -t$THREADS -c100 -d60s $URL > /dev/null 2>&1 || true
echo -e "${GREEN}✓ 预热完成${NC}\n"
sleep 2

# 2. 基准测试
echo -e "${YELLOW}[2/5] 基准测试 (100并发)${NC}"
BASELINE=$(wrk -t$THREADS -c100 -d${DURATION}s --latency $URL 2>&1)
echo "$BASELINE" | grep "Requests/sec" | awk '{print "  QPS: "$2}'
echo "$BASELINE" >> $REPORT_FILE.tmp
sleep 2

# 3. 阶梯加压
echo -e "\n${YELLOW}[3/5] 阶梯加压测试${NC}"
RESULTS=()
CONCURRENCY=(100 200 400 600 800 1000 1200 1500)

for CONN in "${CONCURRENCY[@]}"; do
    echo -e "${BLUE}--- 测试 $CONN 并发 ---${NC}"
    
    # 如果是100并发，直接输出原始格式
    if [ "$CONN" -eq 100 ]; then
        echo -e "\n${GREEN}=== 100并发原始测试结果 ===${NC}"
        wrk -t$THREADS -c$CONN -d${DURATION}s --latency $URL
        echo -e "${GREEN}=============================${NC}\n"

        OUTPUT=$(wrk -t$THREADS -c$CONN -d${DURATION}s --latency $URL 2>&1)
    else
         # 其他并发正常执行并捕获输出
        OUTPUT=$(wrk -t$THREADS -c$CONN -d${DURATION}s --latency $URL 2>&1)
    fi
    
    # 提取数据
    QPS=$(echo "$OUTPUT" | grep "Requests/sec" | awk '{print $2}')
    LATENCY=$(echo "$OUTPUT" | grep "Latency" | head -1 | awk '{print $2}' | sed 's/ms//')
    P99=$(echo "$OUTPUT" | grep "99%" | awk '{print $2}')
    ERRORS=$(echo "$OUTPUT" | grep "Socket errors" | awk '{print $4}' | cut -d',' -f1)
    ERRORS=${ERRORS:-0}
    
    # 判断状态
    if [ $(echo "$LATENCY > 100" | bc) -eq 1 ] 2>/dev/null; then
        STATUS="${RED}过载${NC}"
        STATUS_TEXT="过载"
    elif [ $(echo "$LATENCY > 50" | bc) -eq 1 ] 2>/dev/null; then
        STATUS="${YELLOW}瓶颈${NC}"
        STATUS_TEXT="瓶颈"
    else
        STATUS="${GREEN}正常${NC}"
        STATUS_TEXT="正常"
    fi
    
    # 显示结果
    printf "  QPS: %8s  延迟: %5sms  P99: %5s  错误: %3s  %s\n" \
           "$QPS" "$LATENCY" "$P99" "$ERRORS" "$STATUS"
    
    # 保存结果
    printf "%-8s %-8s %-8s %-8s %-8s %s\n" \
           "$CONN" "$QPS" "${LATENCY}ms" "$P99" "$ERRORS" "$STATUS_TEXT" >> $REPORT_FILE
    
    RESULTS+=("$CONN:$QPS:$LATENCY:$ERRORS")
    sleep 3
done

# 4. 分析最佳并发
echo -e "\n${YELLOW}[4/5] 分析测试结果${NC}"

# 找出最高QPS点
BEST_CONN=100
BEST_QPS=0
for R in "${RESULTS[@]}"; do
    IFS=':' read -r CONN QPS LAT ERR <<< "$R"
    if (( $(echo "$QPS > $BEST_QPS" | bc -l) )); then
        BEST_QPS=$QPS
        BEST_CONN=$CONN
    fi
done

# 找出拐点（QPS开始下降的第一个点）
TURN_POINT=0
PREV_QPS=0
for R in "${RESULTS[@]}"; do
    IFS=':' read -r CONN QPS LAT ERR <<< "$R"
    if (( $(echo "$PREV_QPS > 0" | bc -l) )) && (( $(echo "$QPS < $PREV_QPS * 0.95" | bc -l) )); then
        TURN_POINT=$CONN
        break
    fi
    PREV_QPS=$QPS
done

# 5. 稳定性测试
echo -e "\n${YELLOW}[5/5] 稳定性测试 (最佳并发 $BEST_CONN 跑5分钟)${NC}"
STABILITY=$(wrk -t$THREADS -c$BEST_CONN -d300s --latency $URL 2>&1)
STABLE_QPS=$(echo "$STABILITY" | grep "Requests/sec" | awk '{print $2}')
STABLE_LAT=$(echo "$STABILITY" | grep "Latency" | head -1 | awk '{print $2}')

# 写入完整摘要
cat >> $REPORT_FILE << EOF

二、完整测试摘要
-------------------------------------------------------------------------------

【基准测试 - 100并发】
$(wrk -t$THREADS -c100 -d10s $URL 2>&1)

【最佳并发测试 - ${BEST_CONN}并发】
$(wrk -t$THREADS -c$BEST_CONN -d30s --latency $URL 2>&1)

【稳定性测试 - ${BEST_CONN}并发 5分钟】
QPS: $STABLE_QPS
延迟: $STABLE_LAT

三、数据分析
-------------------------------------------------------------------------------
最佳并发点: $BEST_CONN (最高QPS: $BEST_QPS)
系统拐点: $TURN_POINT (超过此并发QPS开始下降)
${TURN_POINT:+建议生产环境控制在 $TURN_POINT 并发以内}

四、测试结论
-------------------------------------------------------------------------------
$(date '+%Y-%m-%d %H:%M:%S') 完成压测

系统性能总结:
- 最大吞吐量: $BEST_QPS QPS @ ${BEST_CONN}并发
- 最佳延迟区间: 20ms以内
- 系统瓶颈: ${TURN_POINT}+ 并发时性能下降
- 稳定性: 5分钟长跑 QPS 稳定在 $STABLE_QPS

建议:
1. 生产环境建议并发控制在 $TURN_POINT 以内
2. 如需更高吞吐，考虑优化连接处理
3. 监控延迟超过50ms时触发告警
===============================================================================
EOF

# 清理测试环境（但不停止容器）
echo -e "\n${YELLOW}清理测试环境...${NC}"
docker exec im-gateway-$ENV pkill TCPserver 2>/dev/null || true

echo -e "\n${GREEN}✓ 测试完成！${NC}"
echo -e "${BLUE}报告已保存至: $REPORT_FILE${NC}"
echo -e "\n报告摘要:"
echo "----------------------------------------"
tail -n 15 $REPORT_FILE

echo -e "\n${YELLOW}提示: 容器仍在运行，可以继续使用:${NC}"
echo "  docker exec -it im-gateway-$ENV bash"
echo "  docker logs im-gateway-$ENV"
