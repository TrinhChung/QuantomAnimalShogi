import { randomUUID } from "node:crypto";

const jobKinds = new Set([
  "git_update",
  "benchmark",
  "tournament",
  "evaluation",
]);

const profilesByKind = {
  git_update: new Set(["none"]),
  benchmark: new Set([
    "infrastructure_smoke",
    "correctness_regression",
    "fixed_depth_quick",
    "fixed_depth_deep",
    "fixed_time_quick",
    "fixed_time_contest",
    "diagnostic_telemetry",
  ]),
  tournament: new Set([
    "strength_quick",
    "strength_candidate",
    "promotion_test",
    "reliability_soak",
  ]),
  evaluation: new Set([
    "infrastructure_smoke",
    "correctness_regression",
    "fixed_depth_quick",
    "fixed_depth_deep",
    "fixed_time_quick",
    "fixed_time_contest",
    "diagnostic_telemetry",
    "strength_quick",
    "strength_candidate",
    "promotion_test",
    "reliability_soak",
    "architecture_change_full",
  ]),
};

const changeCategories = new Set([
  "performance_only",
  "optimization_only",
  "move_ordering",
  "search_control",
  "selective_search",
  "pruning_change",
  "canonicalization_change",
  "evaluation_change",
  "rule_change",
  "architecture_change",
  "mixed",
]);

const defaultPriorities = {
  git_update: 50,
  benchmark: 100,
  evaluation: 150,
  tournament: 200,
};

export const parseJson = (value, fallback) => {
  if (value === null || value === undefined) {
    return fallback;
  }
  return typeof value === "string" ? JSON.parse(value) : value;
};

const boundedString = (value, fieldName, maximumLength, pattern = null) => {
  const result = String(value ?? "").trim();
  if (
    !result ||
    result.length > maximumLength ||
    (pattern && !pattern.test(result))
  ) {
    throw new Error(`${fieldName} is invalid`);
  }
  return result;
};

const boundedInteger = (value, fieldName, minimum, maximum) => {
  const result = Number(value);
  if (!Number.isInteger(result) || result < minimum || result > maximum) {
    throw new Error(`${fieldName} is invalid`);
  }
  return result;
};

const stringList = (value, fieldName, maximumItems = 32) => {
  if (!Array.isArray(value) || value.length > maximumItems) {
    throw new Error(`${fieldName} is invalid`);
  }
  return value.map((item) =>
    boundedString(item, fieldName, 100, /^[a-zA-Z0-9._:-]+$/),
  );
};

const validatePayload = (kind, value) => {
  const source = value && typeof value === "object" ? value : {};
  const profile =
    kind === "git_update"
      ? "none"
      : boundedString(source.profile, "profile", 80);
  if (!profilesByKind[kind].has(profile)) {
    throw new Error(`profile is not allowed for ${kind}`);
  }
  if (kind === "git_update") {
    return { profile };
  }
  const changeCategory = boundedString(
    source.changeCategory ?? "performance_only",
    "changeCategory",
    80,
  );
  if (!changeCategories.has(changeCategory)) {
    throw new Error("changeCategory is invalid");
  }
  const result = {
    profile,
    candidateName: boundedString(source.candidateName, "candidateName", 120),
    candidateVersionId: source.candidateVersionId
      ? boundedString(
          source.candidateVersionId,
          "candidateVersionId",
          80,
          /^[a-z0-9][a-z0-9._-]*$/,
        )
      : null,
    changeCategory,
    opponents: stringList(source.opponents ?? [], "opponents", 20),
    seed: boundedInteger(source.seed ?? 0x51415335, "seed", 0, 0x7fffffff),
  };
  if (kind === "tournament") {
    result.opponentGitCommit = boundedString(
      source.opponentGitCommit,
      "opponentGitCommit",
      40,
      /^[0-9a-f]{40}$/,
    );
    result.opponentName = boundedString(
      source.opponentName ?? "stage5-clean",
      "opponentName",
      80,
      /^[a-zA-Z0-9._-]+$/,
    );
  }
  return result;
};

export const validateJobSubmission = (value) => {
  if (!value || typeof value !== "object") {
    throw new Error("job is invalid");
  }
  const kind = boundedString(value.kind, "kind", 30);
  if (!jobKinds.has(kind)) {
    throw new Error("kind is invalid");
  }
  const labels = value.requirements?.labels ?? {};
  if (!labels || typeof labels !== "object" || Array.isArray(labels)) {
    throw new Error("requirements.labels is invalid");
  }
  const requiredLabels = Object.fromEntries(
    Object.entries(labels).map(([key, item]) => [
      boundedString(key, "requirement label", 50, /^[a-zA-Z0-9._-]+$/),
      boundedString(item, "requirement label value", 100),
    ]),
  );
  return {
    publicId: randomUUID(),
    idempotencyKey: value.idempotencyKey
      ? boundedString(
          value.idempotencyKey,
          "idempotencyKey",
          160,
          /^[a-zA-Z0-9._:-]+$/,
        )
      : randomUUID(),
    kind,
    priority: boundedInteger(
      value.priority ?? defaultPriorities[kind],
      "priority",
      1,
      1000,
    ),
    gitCommit: boundedString(
      value.gitCommit,
      "gitCommit",
      40,
      /^[0-9a-f]{40}$/,
    ),
    gitRemote: boundedString(
      value.gitRemote ?? "origin",
      "gitRemote",
      100,
      /^[a-zA-Z0-9][a-zA-Z0-9._-]*$/,
    ),
    payload: validatePayload(kind, value.payload),
    requirements: { capabilities: [kind], labels: requiredLabels },
    requestedBy: boundedString(value.requestedBy ?? "web", "requestedBy", 120),
    maxAttempts: boundedInteger(value.maxAttempts ?? 2, "maxAttempts", 1, 10),
  };
};

export const validateWorkerHeartbeat = (value) => {
  if (!value || typeof value !== "object") {
    throw new Error("worker heartbeat is invalid");
  }
  const labels =
    value.labels && typeof value.labels === "object" ? value.labels : {};
  return {
    workerKey: boundedString(
      value.workerId,
      "workerId",
      100,
      /^[a-zA-Z0-9._:-]+$/,
    ),
    displayName: boundedString(
      value.displayName ?? value.workerId,
      "displayName",
      255,
    ),
    hostname: boundedString(value.hostname, "hostname", 255),
    platform: boundedString(value.platform, "platform", 80),
    architecture: boundedString(value.architecture, "architecture", 80),
    cpuThreads: boundedInteger(value.cpuThreads, "cpuThreads", 1, 4096),
    memoryMb: boundedInteger(
      value.memoryMb,
      "memoryMb",
      128,
      Number.MAX_SAFE_INTEGER,
    ),
    resourceBusy: Boolean(value.resourceBusy),
    labels,
    capabilities: stringList(value.capabilities ?? [], "capabilities", 32),
    progress:
      value.progress && typeof value.progress === "object"
        ? value.progress
        : {},
    currentGitCommit: value.currentGitCommit
      ? boundedString(
          value.currentGitCommit,
          "currentGitCommit",
          40,
          /^[0-9a-f]{40}$/,
        )
      : null,
  };
};
