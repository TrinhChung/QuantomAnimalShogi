import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { fileURLToPath, pathToFileURL } from "node:url";

import {
  ClusterCoordinator,
  createRedisClusterQueueFromEnvironment,
} from "./cluster_queue.mjs";
import { ClusterRepository } from "./cluster_repository.mjs";
import {
  clusterTokenFromEnvironment,
  handleClusterRequest,
} from "./cluster_routes.mjs";
import { createDatabaseFromEnvironment } from "./database.mjs";

const maximumRequestBytes = 4 * 1024 * 1024;
const dashboardPath = fileURLToPath(
  new URL("./static/index.html", import.meta.url),
);

const readJson = (request) =>
  new Promise((resolve, reject) => {
    let body = "";
    request.setEncoding("utf8");
    request.on("data", (chunk) => {
      body += chunk;
      if (Buffer.byteLength(body) > maximumRequestBytes) {
        reject(new Error("request body is too large"));
        request.destroy();
      }
    });
    request.on("end", () => {
      try {
        resolve(body ? JSON.parse(body) : {});
      } catch {
        reject(new Error("request body is not valid JSON"));
      }
    });
    request.on("error", reject);
  });

const sendJson = (response, status, value) => {
  const body = JSON.stringify(value);
  response.writeHead(status, {
    "cache-control": "no-store",
    "content-length": Buffer.byteLength(body),
    "content-type": "application/json; charset=utf-8",
  });
  response.end(body);
};

const parsePort = (value) => {
  const port = Number(value ?? 8766);
  if (!Number.isInteger(port) || port < 1 || port > 65_535) {
    throw new Error("QAS_CLUSTER_PORT is invalid");
  }
  return port;
};

const start = async () => {
  const database = await createDatabaseFromEnvironment();
  const queue = await createRedisClusterQueueFromEnvironment();
  const token = clusterTokenFromEnvironment();
  if (!queue || !token) {
    throw new Error("QAS_REDIS_URL and QAS_CLUSTER_TOKEN are required");
  }
  const coordinator = new ClusterCoordinator(
    new ClusterRepository(database),
    queue,
  );
  await coordinator.hydrate();
  const dashboard = await readFile(dashboardPath);
  const server = createServer(async (request, response) => {
    try {
      if (request.method === "GET" && request.url === "/api/health") {
        await database.query("SELECT 1");
        sendJson(response, 200, {
          ok: true,
          mysql: true,
          redis: await queue.ping(),
        });
        return;
      }
      if (
        await handleClusterRequest({
          request,
          response,
          coordinator,
          expectedToken: token,
          readJson,
          sendJson,
        })
      ) {
        return;
      }
      if (
        request.method === "GET" &&
        ["/", "/index.html"].includes(request.url)
      ) {
        response.writeHead(200, {
          "cache-control": "no-cache",
          "content-length": dashboard.length,
          "content-type": "text/html; charset=utf-8",
        });
        response.end(dashboard);
        return;
      }
      sendJson(response, 404, { error: "not found" });
    } catch (error) {
      sendJson(response, Number.isInteger(error?.status) ? error.status : 400, {
        error: error instanceof Error ? error.message : "request failed",
      });
    }
  });
  const shutdown = () => {
    server.close(async () => {
      await queue.close();
      await database.end();
      process.exit(0);
    });
  };
  process.once("SIGINT", shutdown);
  process.once("SIGTERM", shutdown);
  const host = process.env.QAS_CLUSTER_HOST?.trim() || "127.0.0.1";
  const port = parsePort(process.env.QAS_CLUSTER_PORT);
  server.listen(port, host, () =>
    console.log(`QAS cluster master: http://${host}:${port}`),
  );
};

if (
  process.argv[1] &&
  import.meta.url === pathToFileURL(process.argv[1]).href
) {
  start().catch((error) => {
    console.error(error instanceof Error ? error.message : error);
    process.exitCode = 1;
  });
}
