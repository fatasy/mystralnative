declare module 'mystral/game-loop' {
    export interface GameLoopOptions {
        simulationHz?: number;
        maxCatchUpTicks?: number;
        maxFrameDeltaMs?: number;
    }

    export interface GameTick {
        readonly tick: number;
        readonly deltaMs: number;
        readonly deltaSeconds: number;
        readonly simulationTimeMs: number;
    }

    export interface GameLoopState {
        readonly running: boolean;
        readonly paused: boolean;
        readonly tickCount: number;
        readonly simulationTimeMs: number;
        readonly interpolationAlpha: number;
        readonly timeScale: number;
        readonly pendingStepTicks: number;
        readonly simulationHz: number;
        readonly tickDeltaMs: number;
        readonly maxCatchUpTicks: number;
        readonly maxFrameDeltaMs: number;
    }

    export interface GameLoopStats {
        readonly ticksExecuted: number;
        readonly ticksDropped: number;
        readonly catchUpFrames: number;
        readonly clockClampFrames: number;
        readonly handlerFailures: number;
        readonly maxTicksPerFrame: number;
    }

    export type GameTickHandler = (tick: Readonly<GameTick>) => void;

    export interface GameLoop {
        configure(options?: GameLoopOptions): void;
        setTickHandler(handler: GameTickHandler | null): void;
        start(): void;
        stop(): void;
        pause(): void;
        resume(): void;
        step(count?: number): void;
        setTimeScale(scale: number): void;
        getState(): Readonly<GameLoopState>;
        getStats(): Readonly<GameLoopStats>;
    }

    export const gameLoop: GameLoop;
    export default gameLoop;
}

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
