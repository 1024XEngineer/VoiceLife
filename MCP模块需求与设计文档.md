# MCP模块需求与设计文档

本文件皆在定义 VoliceLife 项目的 MCP 模块，帮助其他模块轻松完成工具定义与发现，我看了业界的一些做法（如：OpenClaw、ClaudeCode），并结合自身业务需求，提出了一套产品需求与设计。



## 0. 参考内容

1. OpenClaw：https://github.com/openclaw/openclaw
2. ClaudeCode：https://github.com/claude-code-best/claude-code
3. 灵矽：https://linx.qiniu.com/docs/xrobot/mcp/hardware-mcp



## 1. 核心目标

为VoiceLife项目提供“动手”能力，基于语音通信（ASR-LLM-TTS）这个基础 WebSocket 完成对灵矽与设备端的工具发现与调用。



## 2. 核心概念定义

定义该模块核心实体。

- 注册器：所有工具统一通过注册器来注册
  - 注册器需要验证工具的合法性
  - 注册器保证工具名称不重复
- 工具：OpenAI 工具定义格式+实际业务逻辑
  - 工具名称：灵矽平台向设备端发起工具调用时会返回注册时上传的工具名称
  - Tool definition：约束各个模块的工具定义



## 3. 核心业务流程

MCP模块的使用方式

### 流程一：工具初始化

1. 各个业务模块完成工具的定义

   示例：

   ```js
   export const userTools = [
     {
       name: 'get_user_name',
       description: '返回当前用户名称',
       inputSchema: {
         type: 'object',
         properties: {},
         additionalProperties: false,
       },
       async handler() {
         return '用户是大哥大';
       },
     },
   ];
   
   ```

2. 通过注册器完成工具的注册

   示例：

   ```js
   import { userTools } from '../tools/user/index.js';
   
   const toolGroups = [userTools];
   
   export function createToolRegistry() {
     const registry = new Map();
   
     for (const group of toolGroups) {
       for (const tool of group) {
         if (registry.has(tool.name)) {
           throw new Error('重复的工具名: ' + tool.name);
         }
         registry.set(tool.name, tool);
       }
     }
   
     return registry;
   }
   ```

3. 向灵矽平台发送工具列表

   示例：

   ```javascript
   if (method === 'tools/list') {
         sendMcp({
           jsonrpc: '2.0',
           id: payload.id,
           result: {
             tools: [...registry.values()].map(({ name, description, inputSchema }) => ({
               name,
               description,
               inputSchema,
             })),
           },
         });
         return true;
   }
   ```

   

### 流程二：工具回调

在后续用户与灵矽平台进行交互需要调用工具时的系列流程

1. 需要调工具了，灵矽平台通过 WebSocket 发送消息过来，消息中含有注册时上传的工具名称，通过工具名称确认需要调用的工具，完成调用，并将结果发送给灵矽平台。

   示例：

   ```javascript
   if (method === 'tools/call') {
         const name = payload.params?.name;
         const tool = registry.get(name);
         if (!tool) {
           sendMcp({
             jsonrpc: '2.0',
             id: payload.id,
             error: { code: -32601, message: 'unknown tool' },
           });
           return true;
         }
   
         const result = await tool.handler(payload.params?.arguments ?? {});
         sendMcp({
           jsonrpc: '2.0',
           id: payload.id,
           result: {
             content: [{ type: 'text', text: String(result) }],
             isError: false,
           },
         });
         return true;
       }
   ```



## 4.接口设计

### 接口总览

register_tool——注册一个工具定义

get_tool——根据工具名称查询已注册的工具

list_tools——获取全部已注册的工具

### 接口详情

#### 1）注册工具

**工具描述：**注册一个工具定义，并将其加入工具注册中心。注册成功后，该工具可以被查询、批量导出，并用于生成发送给模型的工具列表。

入参：

**name（String，必填）：**工具名称。建议使用命名空间格式，注册中心中不能存在同名工具。

**description（String，必填）：**工具功能描述，用于说明工具的用途，并会发送给模型。

**input（Object，可选）：**工具入参定义。每个字段需要包含字段名称、字段类型、是否必填、默认值和字段描述等信息。

**handler（Function，必填）：**模型调用回调函数

#### 2）查询工具

**工具描述：**根据工具名称查询已经注册的工具定义。

入参：

**name（String，必填）：**工具名称。

出参：

**tool（ToolDefinition，对象）：**查询到的工具定义。未找到时为空。

**found（Boolean）：**是否找到对应工具。

#### 3）获取工具列表

**工具描述：**获取工具注册中心中当前已经注册的全部工具。

入参：

无。

出参：

**tools（ToolDefinition[]）：**已经注册的工具定义列表。

**total（Integer）：**工具总数。





