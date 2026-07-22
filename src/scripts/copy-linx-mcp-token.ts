import "dotenv/config";
import { spawnSync } from "node:child_process";
import { fetchLinxProxyCredentials } from "../adapters/linx-mcp-proxy.js";

const apiKey = process.env.LINX_API_KEY;
if (!apiKey) throw new Error("LINX_API_KEY is missing from .env");

const credentials = await fetchLinxProxyCredentials(apiKey);
const copied = spawnSync("pbcopy", [], { input: credentials.token, encoding: "utf8" });
if (copied.status !== 0) throw new Error("Unable to copy X-MCP-Token with pbcopy");

console.log(`X-MCP-Token copied to clipboard. Expires at ${credentials.expiresAt}.`);
