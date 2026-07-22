import { timingSafeEqual } from "node:crypto";

class ClusterHttpError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

const bearerToken = (request) => {
  const authorization = String(request.headers.authorization ?? "");
  return authorization.startsWith("Bearer ") ? authorization.slice(7) : "";
};

const tokenMatches = (actual, expected) => {
  const actualBuffer = Buffer.from(actual);
  const expectedBuffer = Buffer.from(expected);
  return (
    actualBuffer.length === expectedBuffer.length &&
    timingSafeEqual(actualBuffer, expectedBuffer)
  );
};

const authenticate = (request, expectedToken) => {
  if (!expectedToken || !tokenMatches(bearerToken(request), expectedToken)) {
    throw new ClusterHttpError(401, "cluster bearer token is invalid");
  }
};

const requiredCoordinator = (coordinator) => {
  if (!coordinator) {
    throw new ClusterHttpError(503, "cluster orchestration is unavailable");
  }
  return coordinator;
};

const workerId = (body) => String(body.workerId ?? "").trim();

export const clusterTokenFromEnvironment = (environment = process.env) => {
  const token = environment.QAS_CLUSTER_TOKEN?.trim();
  if (!token) {
    return null;
  }
  if (token.length < 32 || token.length > 512) {
    throw new Error(
      "QAS_CLUSTER_TOKEN must contain between 32 and 512 characters",
    );
  }
  return token;
};

export const handleClusterRequest = async ({
  request,
  response,
  coordinator,
  expectedToken,
  readJson,
  sendJson,
}) => {
  if (!request.url?.startsWith("/api/cluster")) {
    return false;
  }
  const cluster = requiredCoordinator(coordinator);

  if (request.method === "GET" && request.url === "/api/cluster/metrics") {
    const body = await cluster.metrics();
    response.writeHead(200, {
      "cache-control": "no-store",
      "content-length": Buffer.byteLength(body),
      "content-type": "text/plain; version=0.0.4; charset=utf-8",
    });
    response.end(body);
    return true;
  }

  authenticate(request, expectedToken);

  if (
    request.method === "GET" &&
    request.url.startsWith("/api/cluster/state")
  ) {
    const url = new URL(request.url, "http://localhost");
    const limit = Number(url.searchParams.get("limit") ?? 100);
    sendJson(response, 200, await cluster.state(limit));
    return true;
  }

  if (request.method === "POST" && request.url === "/api/cluster/jobs") {
    const result = await cluster.enqueue(await readJson(request));
    sendJson(response, result.created ? 201 : 200, result);
    return true;
  }

  if (
    request.method === "POST" &&
    request.url === "/api/cluster/workers/heartbeat"
  ) {
    const worker = await cluster.heartbeatWorker(await readJson(request));
    sendJson(response, 200, { ok: true, workerId: worker.workerKey });
    return true;
  }

  if (request.method === "POST" && request.url === "/api/cluster/jobs/claim") {
    const result = await cluster.claim(await readJson(request));
    sendJson(response, 200, result);
    return true;
  }

  const heartbeatMatch = request.url.match(
    /^\/api\/cluster\/jobs\/([a-f0-9-]{36})\/heartbeat$/,
  );
  if (request.method === "POST" && heartbeatMatch) {
    const body = await readJson(request);
    const job = await cluster.heartbeatJob(
      heartbeatMatch[1],
      workerId(body),
      body.progress ?? {},
    );
    sendJson(response, 200, { ok: true, job });
    return true;
  }

  const completeMatch = request.url.match(
    /^\/api\/cluster\/jobs\/([a-f0-9-]{36})\/complete$/,
  );
  if (request.method === "POST" && completeMatch) {
    const body = await readJson(request);
    const result = await cluster.finish(completeMatch[1], workerId(body), {
      completed: true,
      progress: body.progress ?? {},
      result: body.result ?? {},
      errorMessage: null,
    });
    sendJson(response, 200, result);
    return true;
  }

  const failMatch = request.url.match(
    /^\/api\/cluster\/jobs\/([a-f0-9-]{36})\/fail$/,
  );
  if (request.method === "POST" && failMatch) {
    const body = await readJson(request);
    const result = await cluster.finish(failMatch[1], workerId(body), {
      completed: false,
      progress: body.progress ?? {},
      result: body.result ?? {},
      errorMessage: String(body.errorMessage ?? "worker failed").slice(
        0,
        8_000,
      ),
    });
    sendJson(response, 200, result);
    return true;
  }

  const cancelMatch = request.url.match(
    /^\/api\/cluster\/jobs\/([a-f0-9-]{36})\/cancel$/,
  );
  if (request.method === "POST" && cancelMatch) {
    sendJson(response, 200, { job: await cluster.cancel(cancelMatch[1]) });
    return true;
  }

  throw new ClusterHttpError(404, "cluster endpoint was not found");
};
