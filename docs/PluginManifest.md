# Plugin Manifest 规范

每个插件目录必须包含 `manifest.json`（规格书第三节）。

## 字段

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `id` | ✅ | 全局唯一 ID，字符集 `a-zA-Z0-9._-`，约定 `com.sai2.<name>` |
| `name` | ✅ | 显示名 |
| `version` | ✅ | 插件版本，`major[.minor[.patch]]` |
| `apiVersion` | ✅ | 依赖的插件 API 版本，如 `1.0` |
| `entry` | ✅ | 入口动态库文件名，如 `helloworld.dll` |
| `type` | ✅ | `image-effect` / `tool` / … |
| `author` |  | 作者 |
| `description` |  | 描述 |
| `minHostVersion` |  | 最低宿主版本 |
| `sha256` |  | 入口 DLL 的 SHA-256（小写 hex），声明后安装时强制校验 |

## 示例

```json
{
    "id": "com.sai2.liquify",
    "name": "Liquify",
    "version": "1.0.0",
    "author": "SAI2 Plugin Team",
    "apiVersion": "1.0",
    "minHostVersion": "0.1.0",
    "entry": "liquify.dll",
    "type": "image-effect",
    "description": "提供接近 Photoshop 液化工作流的独立窗口。"
}
```

## API 版本兼容策略

宿主版本号 `SAI2_PLUGIN_API_VERSION = (major<<16)|minor`：

- 插件 `apiVersion` 的**主版本必须等于**宿主主版本；
- 插件**次版本 ≤** 宿主次版本（旧插件可运行在新宿主上）；
- 不满足时拒绝加载并给出明确错误。

## 校验失败的行为

宿主扫描插件目录时，manifest 校验失败的目录会被**跳过**并写入
`logs/plugin.log`（警告级别），不会影响其他插件。
