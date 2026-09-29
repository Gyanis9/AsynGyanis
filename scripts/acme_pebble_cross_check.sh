#!/usr/bin/env bash
# ACME 的跨实现验收：起一份 Pebble（Let's Encrypt 官方那套 ACME 测试服务端，Go 写的独立实现），
# 用本框架的 acme_issuance_probe 走完整台 RFC 8555 状态机，再拿 Pebble 本次启动生成的根去验那张链。
#
# 为什么需要这份脚本：进程内的桩机构虽然按「拿 JWK 重建公钥再验签」这条独立路径判，但它与本仓同源、
# 一起被改；真机构那条按环境变量门控的用例只走到「取目录 + 建号」，因为真签发要一台公网可达、域名又
# 解析到本机的机器。Pebble 把 HTTP-01 的取令牌端口配成 5002，于是回环就能走完整条链路：域名用
# 127.0.0.1.sslip.io 这类通配解析，对面解析回来的就是本机。
# 这一步已经真抓到过两个缺陷：ACME 请求漏 User-Agent（Boulder/Pebble 一律 400 malformed，连目录都取不到）、
# 机构复用已 valid 的授权时客户端又去触发那条挑战（Boulder 原文「Cannot update challenge with status valid」）。
# 两条都不是桩会拒的形状——桩当时不会复用授权，也不看 User-Agent。
#
# 用法：bash scripts/acme_pebble_cross_check.sh [构建目录] [Pebble 目录]
#   构建目录默认 build/asan（要带 acme_issuance_probe 这个目标）
#   Pebble 目录默认 /root/pebble-lab，里面要有 app 二进制与 test/config、test/certs（镜像里都有）
# 退出码分档：0 = 全部场景过；1 = 某个场景失败；2 = 前置缺件（不是实现的问题，别当成通过）
# 判据是「场景条数 + 收尾标记」，不是 grep 的退出码：grep 在零命中时返回 1，会把跑完的一轮读成失败
set -uo pipefail

build_dir="${1:-build/asan}"
pebble_dir="${2:-/root/pebble-lab}"
probe_binary="$build_dir/tests/Tools/acme_issuance_probe"
pebble_binary="$pebble_dir/app"
pebble_config="$pebble_dir/test/config/pebble-config.json"
# 通配解析到回环的域名：对面按它解析回来的就是本机，于是本机就能自证，不需要公网地址
domain="${ASYN_ACME_CROSS_DOMAIN:-127.0.0.1.sslip.io}"
# 令牌端口写进现造的配置，与探针那侧同一个数；对面按这个端口来取令牌
challenge_port="${ASYN_ACME_CROSS_HTTP_PORT:-15002}"
pebble_port="${ASYN_ACME_CROSS_PEBBLE_PORT:-24000}"
management_port="${ASYN_ACME_CROSS_MANAGEMENT_PORT:-25000}"
state_dir="$(mktemp -d)"
probe_log="${ASYN_ACME_CROSS_LOG:-$(mktemp)}"
pebble_log="$(mktemp)"
tag="ACME_CROSS_CHECK_OK-$(date +%s)"
scenarios=0

note() { echo "[acme-cross] $*"; }

# ---- 前置检查：缺件一律 exit 2，绝不与「场景失败」混成一档 ----
if [[ ! -x "$probe_binary" ]]; then
    note "缺探针可执行体 $probe_binary（先 ninja/cmake --build 出 acme_issuance_probe）"
    exit 2
fi
if [[ ! -x "$pebble_binary" || ! -f "$pebble_config" ]]; then
    note "缺 Pebble：$pebble_binary 或 $pebble_config 不在（镜像里在 /app 与 /test）"
    exit 2
fi
if ! command -v openssl >/dev/null 2>&1; then
    note "缺 openssl：这张链没法用独立工具验"
    exit 2
fi
# 对面的 API 是自签的（由镜像里那份 minica 签），本端只能显式把它当信任锚
if [[ ! -f "$pebble_dir/test/certs/pebble.minica.pem" ]]; then
    note "缺 pebble.minica.pem：连不上对面的 HTTPS 目录"
    exit 2
fi

# ---- 起对面 ----
# 配置按本次端口现造一份：直接复用镜像里那份会撞上一个还没退干净的实例——那时判的是别人的进程，
# 而标记一样会全绿。相对路径（test/certs、test/config）仍以 pebble_dir 为工作目录解析
# 端口必须空着：撞上别人（或自己上一轮泄漏的）实例时，判据就不再属于本次实现
for busy_port in "$pebble_port" "$management_port"; do
    if (exec 3<>"/dev/tcp/127.0.0.1/$busy_port") 2>/dev/null; then
        note "端口 $busy_port 已经有东西在听：那不是我起的实例，判出来的结论不属于本次实现"
        exit 2
    fi
done

cross_config="$pebble_dir/test/config/cross-check-config.json"
sed -e "s|\"listenAddress\": \"0.0.0.0:14000\"|\"listenAddress\": \"0.0.0.0:$pebble_port\"|" \
    -e "s|\"managementListenAddress\": \"0.0.0.0:15000\"|\"managementListenAddress\": \"0.0.0.0:$management_port\"|" \
    -e "s|\"httpPort\": 5002|\"httpPort\": $challenge_port|" \
    "$pebble_config" >"$cross_config"
grep -q "\"listenAddress\": \"0.0.0.0:$pebble_port\"" "$cross_config" || {
    note "现造的配置没把端口改过来（对面那份 pebble-config.json 的形状变了）：$cross_config"
    exit 2
}
# 用 ( cd ... && exec ... ) 起：exec 让子 shell 变成对面本体，$! 拿到的就是它的进程号。
# 早先写的是 ( cd && nohup app & )，$! 是那个子 shell，kill 打不到 app——实例会泄漏下来，
# 下一轮撞在「端口已被占用」上，判的其实是上一台残留 CA
( cd "$pebble_dir" && exec ./app -config "test/config/cross-check-config.json" ) >"$pebble_log" 2>&1 &
pebble_pid=$!
echo "$pebble_pid" >"$pebble_log.pid"
cleanup() {
    kill "$pebble_pid" >/dev/null 2>&1
    rm -f "$pebble_log.pid" "$cross_config"
    rm -rf "$state_dir"
}
trap cleanup EXIT

directory_ready=0
for _ in $(seq 1 60); do
    sleep 0.5
    if curl -sk --max-time 2 "https://localhost:$pebble_port/dir" | grep -q '"newNonce"'; then
        directory_ready=1
        break
    fi
done
if [[ "$directory_ready" != 1 ]]; then
    note "Pebble 在 30 秒内没把目录交出来，日志尾："
    tail -8 "$pebble_log"
    exit 1
fi
scenarios=$((scenarios + 1))
note "场景 1 过：对面起来了，目录可取"

# ---- 走完整台状态机（两轮：签发 + 续期）----
SSL_CERT_FILE="$pebble_dir/test/certs/pebble.minica.pem" \
    timeout 240 "$probe_binary" \
    --directory-url "https://localhost:$pebble_port/dir" \
    --domain "$domain" \
    --challenge-port "$challenge_port" \
    --state-dir "$state_dir" \
    --renew >"$probe_log" 2>&1
probe_rc=$?

# 先确认探针真的走到了签发那一步：这行标记没出现就说明开关根本没被消费，后面的判据都是空的
if ! grep -q '^ISSUED ' "$probe_log"; then
    note "探针没打出 ISSUED 标记（退出码 $probe_rc），它自己的输出："
    cat "$probe_log"
    tail -12 "$pebble_log"
    exit 1
fi
scenarios=$((scenarios + 1))
note "场景 2 过：对面接受了我们的 JWS 与 CSR，签出一张链"

grep -q '^RENEWED ' "$probe_log" || { note "续期那一轮没重签（看 RENEWED 标记）：$(cat "$probe_log")"; exit 1; }
scenarios=$((scenarios + 1))
note "场景 3 过：第二轮真的重签（授权复用那条出口没被踩）"

grep -q '^DONE$' "$probe_log" || { note "探针没收尾（DONE 缺失），退出码 $probe_rc"; exit 1; }
if [[ "$probe_rc" != 0 ]]; then
    note "探针退出码非 0：$probe_rc"; cat "$probe_log"; exit 1
fi
scenarios=$((scenarios + 1))
note "场景 4 过：探针退出码 0"

# 装回服务的动作必须真被调到两次（每签一张一次）：只落磁盘不装回是这个自动化最坏的失败形状
reload_count=$(grep -oE '^LOCAL_RELOAD_COUNT [0-9]+' "$probe_log" | awk '{print $2}')
if [[ "${reload_count:-0}" != "2" ]]; then
    note "装回回调被调了 ${reload_count:-0} 次，期望 2 次"; exit 1
fi
scenarios=$((scenarios + 1))
note "场景 5 过：两张证书都走了装回服务的动作"

# 两轮拿到的一定是不同的字节流：签名值与序列号都换过
first_identity=$(grep -oE '^ISSUED [0-9]+ IDENTITY [0-9a-f]+' "$probe_log" | awk '{print $4}')
second_identity=$(grep -oE '^RENEWED [0-9]+ IDENTITY [0-9a-f]+' "$probe_log" | awk '{print $4}')
if [[ -z "$first_identity" || "$first_identity" == "$second_identity" ]]; then
    note "两轮拿到的证书身份相同或没读到：'$first_identity' vs '$second_identity'"; exit 1
fi
scenarios=$((scenarios + 1))
note "场景 6 过：续期拿到的是另一张证书"

# ---- 用对面本次启动的根验那张链：这一步的判据完全在对面那侧 ----
# 签发根是每次启动新生成的，镜像里那份 minica 只签 API 证书，所以根要从管理接口取
curl -sk --max-time 5 "https://localhost:$management_port/roots/0" >"$state_dir/root.pem" || {
    note "取不到对面的根（管理接口 $management_port）"; exit 1
}
cd "$state_dir" || exit 1
csplit -z -f cert- chain.pem '/BEGIN CERTIFICATE/' '{*}' >/dev/null 2>&1
if [[ ! -f cert-00 ]]; then
    note "拆不出叶证书：链文件里 $(grep -c 'BEGIN CERTIFICATE' chain.pem) 段"
    exit 1
fi
cp cert-00 leaf.pem
cat cert-01* >inter.pem 2>/dev/null
verify_output=$(openssl verify -CAfile root.pem -untrusted inter.pem leaf.pem 2>&1)
if ! grep -q ': OK$' <<<"$verify_output"; then
    note "独立工具验不过这张链：$verify_output"
    exit 1
fi
scenarios=$((scenarios + 1))
note "场景 7 过：链能验到对面本次的根（$verify_output）"

# 证书里的身份必须就是下单的那个域名——签错了身份也算失败
san=$(openssl x509 -in leaf.pem -noout -ext subjectAltName 2>/dev/null)
if ! grep -q "DNS:$domain" <<<"$san"; then
    note "证书的 SAN 里没有下单的域名：$san"
    exit 1
fi
scenarios=$((scenarios + 1))
note "场景 8 过：SAN 就是下单的域名 $domain"

note "全部 $scenarios 个场景通过，标记 $tag"
echo "$tag"
