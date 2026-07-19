declare module 'mystral/worker-pool' {
    export type WorkerPoolSchedule = 'static' | 'dynamic';
    export type WorkerPoolBuiltInReducer = 'sum' | 'count' | 'min' | 'max';

    export interface WorkerPoolOptions {
        size?: number;
        name?: string;
        maxQueuedRounds?: number;
        startupTimeoutMs?: number;
    }

    export interface WorkerPoolRangeOptions {
        start?: number;
        end?: number;
        length?: number;
        signal?: AbortSignal;
    }

    export interface WorkerPoolParallelOptions extends WorkerPoolRangeOptions {
        grainSize?: number;
        schedule?: WorkerPoolSchedule;
    }

    export interface WorkerPoolForEachOptions extends WorkerPoolRangeOptions {
        grainSize?: number;
    }

    export interface WorkerPoolTaskContext<TData = unknown> {
        data: TData;
        round: number;
        workerIndex: number;
        workerCount: number;
        chunkIndex: number;
        begin: number;
        end: number;
        isCancelled(): boolean;
    }

    export type WorkerPoolTaskHandler<TData = unknown, TResult = unknown> = (
        context: WorkerPoolTaskContext<TData>,
    ) => TResult | PromiseLike<TResult>;

    export type WorkerPoolReducer<TValue, TAccumulator = TValue> = (
        accumulator: TAccumulator,
        value: TValue,
        index: number,
        values: readonly TValue[],
    ) => TAccumulator;

    export interface WorkerPoolStats {
        readonly size: number;
        readonly ready: boolean;
        readonly busy: boolean;
        readonly queuedRounds: number;
        readonly broadcastRounds: number;
        readonly barrierRounds: number;
        readonly completedRounds: number;
    }

    export interface WorkerPoolTransferResult<TResult = unknown> {
        readonly $mystralWorkerPoolTransferResult: 1;
        readonly value: TResult;
        readonly transferList: readonly ArrayBuffer[];
    }

    export class WorkerPool {
        static create(workerUrl: string | URL, options?: WorkerPoolOptions): Promise<WorkerPool>;

        private constructor();

        readonly size: number;
        readonly maxQueuedRounds: number;
        readonly startupTimeoutMs: number;
        readonly busy: boolean;
        readonly queuedRounds: number;

        parallelFor<TData = unknown, TResult = unknown>(
            taskName: string,
            data: TData,
            options: WorkerPoolParallelOptions & {
                reducer?: undefined;
            },
        ): Promise<TResult[]>;

        parallelFor<TData = unknown>(
            taskName: string,
            data: TData,
            options: WorkerPoolParallelOptions & {
                reducer: WorkerPoolBuiltInReducer;
                initialValue?: number;
            },
        ): Promise<number>;

        parallelFor<TData = unknown, TValue = unknown, TAccumulator = TValue>(
            taskName: string,
            data: TData,
            options: WorkerPoolParallelOptions & {
                reducer: WorkerPoolReducer<TValue, TAccumulator>;
                initialValue?: TAccumulator;
            },
        ): Promise<TAccumulator>;

        forEach<TData = unknown>(
            taskName: string,
            data: TData,
            options: WorkerPoolForEachOptions,
        ): Promise<void>;

        resizeTable<TTable, TOptions>(
            definition: { resize(table: TTable, options: TOptions): TTable },
            table: TTable,
            options: TOptions,
        ): TTable;

        stats(): Readonly<WorkerPoolStats>;
        close(): Promise<void>;
    }

    export function exposeWorkerTasks<
        THandlers extends Record<string, WorkerPoolTaskHandler<any, any>>,
    >(handlers: THandlers): void;

    export function transferResult<TResult>(
        value: TResult,
        transferList?: readonly ArrayBuffer[],
    ): WorkerPoolTransferResult<TResult>;

    const api: {
        WorkerPool: typeof WorkerPool;
        exposeWorkerTasks: typeof exposeWorkerTasks;
        transferResult: typeof transferResult;
    };
    export default api;
}

declare module 'mystral/native-tasks' {
    export function runNativeTask<TResult = unknown, TData = unknown>(
        name: string,
        data: TData,
    ): Promise<TResult>;

    const api: { runNativeTask: typeof runNativeTask };
    export default api;
}
