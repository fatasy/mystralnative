import { Buffer } from "node:buffer";

type ProtocolResponse = {
  id: number;
  result?: unknown;
  error?: { message?: string };
};

const args = Bun.argv.slice(2);
let port = 9222;
const portIndex = args.indexOf("--port");
if (portIndex >= 0) {
  port = Number(args[portIndex + 1]);
  args.splice(portIndex, 2);
}

const command = args.shift();
if (!command || !Number.isFinite(port)) {
  console.error(`Usage: bun tools/mystral-debug-client.ts <command> [args] [--port 9222]

Commands:
  hello
  state
  evaluate <expression>
  screenshot <output.png>
  logs [since]
  pause
  resume
  quit
  step [count]
  wait <frame> [timeoutMs]
  press <key>
  type <text>
  click <x> <y> [left|middle|right]
  move <x> <y>
  agent-list
  agent-inspect <id> [inputJson]
  agent-act <id> [inputJson]
  raw <method> [paramsJson]`);
  process.exit(1);
}

const socket = new WebSocket(`ws://127.0.0.1:${port}`);
let nextId = 1;
const pending = new Map<
  number,
  { resolve: (value: unknown) => void; reject: (error: Error) => void }
>();

socket.addEventListener("message", (event) => {
  const message = JSON.parse(String(event.data)) as ProtocolResponse & { event?: string };
  if (message.event || typeof message.id !== "number") return;
  const request = pending.get(message.id);
  if (!request) return;
  pending.delete(message.id);
  if (message.error) {
    request.reject(new Error(message.error.message || "Debug protocol error"));
  } else {
    request.resolve(message.result);
  }
});

const opened = new Promise<void>((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error(`Timed out connecting to debug port ${port}`)), 5000);
  socket.addEventListener("open", () => {
    clearTimeout(timer);
    resolve();
  });
  socket.addEventListener("error", () => {
    clearTimeout(timer);
    reject(new Error(`Could not connect to ws://127.0.0.1:${port}`));
  });
});

function request(method: string, params: Record<string, unknown> = {}): Promise<any> {
  const id = nextId++;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`${method} timed out`));
    }, Number(params.timeoutMs || 10000) + 1000);
    pending.set(id, {
      resolve: (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      reject: (error) => {
        clearTimeout(timer);
        reject(error);
      },
    });
    socket.send(JSON.stringify({ id, method, params }));
  });
}

function numberArg(value: string | undefined, name: string): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) throw new Error(`${name} must be a number`);
  return parsed;
}

function agentParams(values: string[]): Record<string, unknown> {
  const id = values.shift();
  if (!id) throw new Error("agent command requires an id");
  const params: Record<string, unknown> = { id };
  if (values.length > 0) params.input = JSON.parse(values.join(" "));
  return params;
}

await opened;

try {
  let result: any;
  switch (command) {
    case "hello":
      result = await request("hello");
      break;
    case "state":
      result = await request("getFrameCount");
      break;
    case "evaluate":
      result = await request("evaluate", { expression: args.join(" ") });
      break;
    case "screenshot": {
      const outputPath = args[0];
      if (!outputPath) throw new Error("screenshot requires an output path");
      result = await request("screenshot");
      await Bun.write(outputPath, Buffer.from(result.data, "base64"));
      result = { path: outputPath, width: result.width, height: result.height };
      break;
    }
    case "logs":
      result = await request("getLogs", { since: Number(args[0] || 0) });
      break;
    case "pause":
      result = await request("pause");
      break;
    case "resume":
      result = await request("resume");
      break;
    case "quit":
      result = await request("quit");
      break;
    case "step": {
      const count = numberArg(args[0] || "1", "count");
      const scheduled = await request("stepFrames", { count });
      result = await request("waitForFrame", { frame: scheduled.targetFrame, timeoutMs: 10000 });
      break;
    }
    case "wait":
      result = await request("waitForFrame", {
        frame: numberArg(args[0], "frame"),
        timeoutMs: numberArg(args[1] || "10000", "timeoutMs"),
      });
      break;
    case "press":
      result = await request("keyboard.press", { key: args[0] });
      break;
    case "type":
      result = await request("keyboard.type", { text: args.join(" ") });
      break;
    case "click":
      result = await request("mouse.click", {
        x: numberArg(args[0], "x"),
        y: numberArg(args[1], "y"),
        button: args[2] || "left",
      });
      break;
    case "move":
      result = await request("mouse.move", {
        x: numberArg(args[0], "x"),
        y: numberArg(args[1], "y"),
      });
      break;
    case "agent-list":
      result = await request("agent.list");
      break;
    case "agent-inspect":
      result = await request("agent.inspect", agentParams(args));
      break;
    case "agent-act":
      result = await request("agent.act", agentParams(args));
      break;
    case "raw":
      if (!args[0]) throw new Error("raw requires a method");
      result = await request(args[0], args[1] ? JSON.parse(args.slice(1).join(" ")) : {});
      break;
    default:
      throw new Error(`Unknown command: ${command}`);
  }

  console.log(JSON.stringify(result, null, 2));
} finally {
  socket.close();
}
