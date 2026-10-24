#!/usr/bin/env bash
# 上传 Release 包到 ECS 并解包校验；主机与用户从环境变量取，不写进仓库。
set -euo pipefail

: "${ASYN_ECS_HOST:?需要 ASYN_ECS_HOST}"
: "${ASYN_ECS_USER:?需要 ASYN_ECS_USER}"
remote_dir="${ASYN_ECS_DIR:-/opt/asyngyanis-acceptance}"
port="${ASYN_ECS_PORT:-22}"
bundle="${1:?用法: deploy_ecs.sh <bundle.tar.gz>}"
connection_options=(-o BatchMode=yes -o IdentitiesOnly=yes)
if [[ -n "${ASYN_ECS_SSH_KEY:-}" ]]; then
    connection_options+=(-i "$ASYN_ECS_SSH_KEY")
fi
scp -P "$port" "${connection_options[@]}" "$bundle" "$ASYN_ECS_USER@$ASYN_ECS_HOST:/tmp/asyngyanis-bundle.tar.gz"
ssh -p "$port" "${connection_options[@]}" "$ASYN_ECS_USER@$ASYN_ECS_HOST" \
  "rm -rf '$remote_dir' && mkdir -p '$remote_dir' && tar -xzf /tmp/asyngyanis-bundle.tar.gz -C '$remote_dir' --strip-components=1 && rm -f /tmp/asyngyanis-bundle.tar.gz && cd '$remote_dir' && sha256sum -c SHA256SUMS"
echo "已部署到 $ASYN_ECS_USER@$ASYN_ECS_HOST:$remote_dir"
