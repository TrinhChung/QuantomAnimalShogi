import { createHash } from "node:crypto";
import { readFile, readdir } from "node:fs/promises";
import { loadEnvFile } from "node:process";

import mysql from "mysql2/promise";

const migrationDirectory = new URL("./migrations/", import.meta.url);

try {
  loadEnvFile("/etc/qas/cluster.env");
} catch (error) {
  if (error?.code !== "ENOENT") {
    throw error;
  }
}

const required = (environment, name) => {
  const value = environment[name]?.trim();
  if (!value) {
    throw new Error(`${name} is required`);
  }
  return value;
};

export const databaseConfigFromEnvironment = (environment = process.env) => {
  const port = Number(environment.QAS_DB_PORT ?? 3306);
  if (!Number.isInteger(port) || port < 1 || port > 65_535) {
    throw new Error("QAS_DB_PORT is invalid");
  }
  const database = required(environment, "QAS_DB_NAME");
  if (!/^[a-zA-Z0-9_]{1,64}$/.test(database)) {
    throw new Error("QAS_DB_NAME is invalid");
  }
  return {
    host: required(environment, "QAS_DB_HOST"),
    port,
    user: required(environment, "QAS_DB_USER"),
    password: environment.QAS_DB_PASSWORD ?? "",
    database,
  };
};

const applyMigrations = async (pool) => {
  await pool.query(`
    CREATE TABLE IF NOT EXISTS cluster_schema_migrations (
      version VARCHAR(100) NOT NULL,
      checksum CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
      applied_at TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
      PRIMARY KEY (version)
    ) ENGINE = InnoDB DEFAULT CHARACTER SET = utf8mb4 COLLATE = utf8mb4_0900_ai_ci
  `);
  const files = (await readdir(migrationDirectory))
    .filter((name) => /^\d+_[a-z0-9_]+\.sql$/.test(name))
    .sort();
  for (const name of files) {
    const sql = await readFile(new URL(name, migrationDirectory), "utf8");
    const checksum = createHash("sha256").update(sql).digest("hex");
    const [rows] = await pool.execute(
      "SELECT checksum FROM cluster_schema_migrations WHERE version = ?",
      [name],
    );
    if (rows[0]) {
      if (rows[0].checksum !== checksum) {
        throw new Error(`Applied cluster migration was modified: ${name}`);
      }
      continue;
    }
    await pool.query(sql);
    await pool.execute(
      "INSERT INTO cluster_schema_migrations (version, checksum) VALUES (?, ?)",
      [name, checksum],
    );
  }
};

export const createDatabaseFromEnvironment = async (
  environment = process.env,
) => {
  const pool = mysql.createPool({
    ...databaseConfigFromEnvironment(environment),
    connectionLimit: 8,
    enableKeepAlive: true,
    multipleStatements: true,
    timezone: "Z",
  });
  try {
    await pool.query("SET time_zone = '+00:00'");
    await applyMigrations(pool);
    return pool;
  } catch (error) {
    await pool.end();
    throw error;
  }
};
