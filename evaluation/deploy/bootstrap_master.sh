#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "bootstrap_master.sh must run as root" >&2
  exit 2
fi

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
deploy_directory="${repository_root}/evaluation/deploy"
environment_directory="/etc/qas"
cluster_environment="${environment_directory}/cluster.env"
worker_environment="${environment_directory}/worker.env"
grafana_environment="${environment_directory}/grafana.env"
backup_environment="${environment_directory}/backup.env"
mysql_container="${QAS_MYSQL_CONTAINER:-mysql_db}"

mkdir -p "${environment_directory}"
chmod 700 "${environment_directory}"

mysql_root_password="$(
  docker inspect --format '{{range .Config.Env}}{{println .}}{{end}}' "${mysql_container}" \
    | sed -n 's/^MYSQL_ROOT_PASSWORD=//p' \
    | head -n 1
)"
if [[ -z "${mysql_root_password}" ]]; then
  echo "MYSQL_ROOT_PASSWORD was not found in ${mysql_container}" >&2
  exit 2
fi

if [[ ! -f "${cluster_environment}" ]]; then
  database_password="$(openssl rand -hex 24)"
  cluster_token="$(openssl rand -hex 32)"
  cat >"${cluster_environment}" <<EOF
QAS_DB_HOST=127.0.0.1
QAS_DB_PORT=3306
QAS_DB_USER=qas_cluster
QAS_DB_PASSWORD=${database_password}
QAS_DB_NAME=quantum_animal_shogi
QAS_DB_REQUIRED=true
QAS_REDIS_URL=redis://127.0.0.1:6379
QAS_KOKORO_QUEUE_KEYS=rq:queue:default
QAS_CLUSTER_TOKEN=${cluster_token}
QAS_CLUSTER_REQUIRED=true
QAS_GIT_REMOTE_URL=git@github.com:TrinhChung/QuantomAnimalShogi.git
EOF
  chmod 600 "${cluster_environment}"
fi

if ! grep -q '^QAS_KOKORO_QUEUE_KEYS=' "${cluster_environment}"; then
  printf '%s\n' 'QAS_KOKORO_QUEUE_KEYS=rq:queue:default' >>"${cluster_environment}"
fi

database_password="$(sed -n 's/^QAS_DB_PASSWORD=//p' "${cluster_environment}" | head -n 1)"
if [[ -z "${database_password}" ]]; then
  echo "QAS_DB_PASSWORD is missing from ${cluster_environment}" >&2
  exit 2
fi
docker exec -i -e MYSQL_PWD="${mysql_root_password}" "${mysql_container}" \
  mysql -uroot --protocol=socket <<SQL
CREATE DATABASE IF NOT EXISTS quantum_animal_shogi
  CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
CREATE USER IF NOT EXISTS 'qas_cluster'@'%' IDENTIFIED BY '${database_password}';
ALTER USER 'qas_cluster'@'%' IDENTIFIED BY '${database_password}';
GRANT ALL PRIVILEGES ON quantum_animal_shogi.* TO 'qas_cluster'@'%';
FLUSH PRIVILEGES;
SQL

if [[ ! -f "${backup_environment}" ]]; then
  backup_password="$(openssl rand -hex 24)"
  cat >"${backup_environment}" <<EOF
QAS_BACKUP_USER=qas_backup
QAS_BACKUP_PASSWORD=${backup_password}
QAS_BACKUP_DATABASE=quantum_animal_shogi
QAS_MYSQL_CONTAINER=${mysql_container}
EOF
  chmod 600 "${backup_environment}"
fi

backup_password="$(sed -n 's/^QAS_BACKUP_PASSWORD=//p' "${backup_environment}" | head -n 1)"
if [[ -z "${backup_password}" ]]; then
  echo "QAS_BACKUP_PASSWORD is missing from ${backup_environment}" >&2
  exit 2
fi
docker exec -i -e MYSQL_PWD="${mysql_root_password}" "${mysql_container}" \
  mysql -uroot --protocol=socket <<SQL
CREATE USER IF NOT EXISTS 'qas_backup'@'%' IDENTIFIED BY '${backup_password}';
ALTER USER 'qas_backup'@'%' IDENTIFIED BY '${backup_password}';
GRANT SELECT, SHOW VIEW, TRIGGER, EVENT, LOCK TABLES ON quantum_animal_shogi.* TO 'qas_backup'@'%';
FLUSH PRIVILEGES;
SQL

cluster_token="$(sed -n 's/^QAS_CLUSTER_TOKEN=//p' "${cluster_environment}" | head -n 1)"
if [[ -z "${cluster_token}" ]]; then
  echo "QAS_CLUSTER_TOKEN is missing from ${cluster_environment}" >&2
  exit 2
fi
cat >"${worker_environment}" <<EOF
QAS_CLUSTER_TOKEN=${cluster_token}
QAS_GIT_REMOTE_URL=git@github.com:TrinhChung/QuantomAnimalShogi.git
EOF
chmod 600 "${worker_environment}"

if [[ ! -f "${grafana_environment}" ]]; then
  cat >"${grafana_environment}" <<EOF
GF_SECURITY_ADMIN_USER=admin
GF_SECURITY_ADMIN_PASSWORD=$(openssl rand -hex 20)
GF_USERS_ALLOW_SIGN_UP=false
GF_SERVER_DOMAIN=$(hostname -I | awk '{print $1}')
EOF
  chmod 600 "${grafana_environment}"
fi

redis-cli ping >/dev/null
docker inspect "${mysql_container}" >/dev/null

docker compose -f "${deploy_directory}/docker-compose.yml" up -d --build
install -m 644 "${deploy_directory}/nginx-qas-cluster.conf" /etc/nginx/sites-available/qas-cluster.conf
ln -sfn /etc/nginx/sites-available/qas-cluster.conf /etc/nginx/sites-enabled/qas-cluster.conf
nginx -t
systemctl reload nginx

echo "QAS dashboard SSH target: http://127.0.0.1:8331/"
echo "Grafana SSH target:       http://127.0.0.1:8331/grafana/"
echo "Secrets remain root-readable under ${environment_directory}."
docker compose -f "${deploy_directory}/docker-compose.yml" ps
