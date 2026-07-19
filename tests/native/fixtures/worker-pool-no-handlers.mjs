// Intentionally does not call exposeWorkerTasks(): WorkerPool.create() must
// reject at its configured startup boundary instead of waiting forever.
globalThis.__workerPoolNoHandlersLoaded = true;
