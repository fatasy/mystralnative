type ProtocolResponse = {
  id: number;
  result?: unknown;
  error?: { message?: string };
  event?: string;
};

type PendingRequest = {
  resolve: (value: unknown) => void;
  reject: (error: Error) => void;
};

export class MystralDebugClient {
  private socket?: WebSocket;
  private connecting?: Promise<void>;
  private nextId = 1;
  private readonly pending = new Map<number, PendingRequest>();

  constructor(
    readonly url = "ws://127.0.0.1:9222",
    readonly connectionTimeoutMs = 5000,
  ) {}

  static forPort(port: number): MystralDebugClient {
    return new MystralDebugClient(`ws://127.0.0.1:${port}`);
  }

  async connect(): Promise<void> {
    if (this.socket?.readyState === WebSocket.OPEN) return;
    if (this.connecting) return this.connecting;

    this.connecting = new Promise<void>((resolve, reject) => {
      const socket = new WebSocket(this.url);
      const timer = setTimeout(() => {
        socket.close();
        reject(new Error(`Timed out connecting to ${this.url}`));
      }, this.connectionTimeoutMs);

      socket.addEventListener("open", () => {
        clearTimeout(timer);
        this.socket = socket;
        resolve();
      });
      socket.addEventListener("message", (event) => this.handleMessage(event));
      socket.addEventListener("error", () => {
        clearTimeout(timer);
        reject(new Error(`Could not connect to ${this.url}`));
      });
      socket.addEventListener("close", () => {
        if (this.socket === socket) this.socket = undefined;
        for (const request of this.pending.values()) {
          request.reject(new Error("Mystral debug connection closed"));
        }
        this.pending.clear();
      });
    }).finally(() => {
      this.connecting = undefined;
    });

    return this.connecting;
  }

  async request<T = any>(method: string, params: Record<string, unknown> = {}): Promise<T> {
    await this.connect();
    const socket = this.socket;
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      throw new Error(`Not connected to ${this.url}`);
    }

    const id = this.nextId++;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`${method} timed out`));
      }, Number(params.timeoutMs || 10000) + 1000);
      this.pending.set(id, {
        resolve: (value) => {
          clearTimeout(timer);
          resolve(value as T);
        },
        reject: (error) => {
          clearTimeout(timer);
          reject(error);
        },
      });
      socket.send(JSON.stringify({ id, method, params }));
    });
  }

  close(): void {
    this.socket?.close();
    this.socket = undefined;
  }

  private handleMessage(event: MessageEvent): void {
    const message = JSON.parse(String(event.data)) as ProtocolResponse;
    if (message.event || typeof message.id !== "number") return;
    const request = this.pending.get(message.id);
    if (!request) return;
    this.pending.delete(message.id);
    if (message.error) request.reject(new Error(message.error.message || "Debug protocol error"));
    else request.resolve(message.result);
  }
}
