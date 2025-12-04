# CLAUDE.md - AI 助手工作指南

> 此文件用于指导 Claude Code 和其他 AI 编码助手在本项目中的工作方式。

## 项目概要

这是一个 **ESP-IDF C++ 嵌入式项目**，目标是构建基于 ESP32 的 AI 语音聊天机器人。

**关键信息**：
- 语言：C++（主要）、C
- SDK：ESP-IDF 5.4+
- 目标芯片：ESP32-S3（主要）、ESP32-C3、ESP32-P4
- 代码风格：Google C++ Style

## 常用命令

```bash
# 构建
idf.py build

# 烧录
idf.py -p COM<X> flash

# 监控
idf.py -p COM<X> monitor

# 一键操作
idf.py -p COM<X> flash monitor

# 清理
idf.py fullclean

# 配置菜单
idf.py menuconfig

# 检查大小
idf.py size
idf.py size-components
```

## 核心文件位置

| 功能模块 | 路径 |
|----------|------|
| 应用入口 | `main/main.cc`, `main/application.cc` |
| 音频处理 | `main/audio/` |
| 显示驱动 | `main/display/` |
| 通信协议 | `main/protocols/` |
| 板级支持 | `main/boards/<board-name>/` |
| MCP 服务 | `main/mcp_server.cc` |
| OTA 升级 | `main/ota.cc` |
| 配置管理 | `main/settings.cc` |

## 工作规则

### 必须遵守

1. **代码风格**：严格遵循 Google C++ Style Guide
2. **头文件**：使用 `#pragma once` 或 include guards
3. **命名**：类名 `PascalCase`，变量 `snake_case_`，常量 `kConstantName`
4. **注释**：英文注释，公共 API 使用 Doxygen 风格
5. **内存**：嵌入式环境，注意内存使用，避免不必要的堆分配

### 编码注意事项

- 使用 ESP-IDF 的日志宏：`ESP_LOGI()`, `ESP_LOGW()`, `ESP_LOGE()`
- FreeRTOS 任务和队列用于并发
- 优先使用 `std::unique_ptr` 管理动态内存
- 硬件相关代码放在 `boards/` 对应目录
- 协议实现放在 `protocols/` 目录

### 禁止操作

- ❌ 不要修改 `managed_components/` 下的托管组件
- ❌ 不要直接修改 `sdkconfig`，使用 `menuconfig`
- ❌ 不要在中断上下文中使用阻塞操作
- ❌ 不要使用 C++ 异常（ESP-IDF 默认禁用）

## 调试技巧

```cpp
// 打印日志
ESP_LOGI("TAG", "Info message: %d", value);
ESP_LOGW("TAG", "Warning message");
ESP_LOGE("TAG", "Error message");

// 打印内存使用
ESP_LOGI("MEM", "Free heap: %lu", esp_get_free_heap_size());
```

## Git 提交规范

使用 Conventional Commits 格式：

```
<emoji> <type>(<scope>): <subject>

WHAT: 简述做了什么
WHY: 为什么要做这个改动
HOW: 如何实现的（可选）
```

**Type 与 Emoji 映射**：
- ✨ feat - 新功能
- 🐛 fix - Bug 修复
- 📝 docs - 文档更新
- 🎨 style - 代码格式
- ♻️ refactor - 重构
- ⚡️ perf - 性能优化
- ✅ test - 测试
- 🏗️ build - 构建相关
- 🧹 chore - 杂务

## 需要更多上下文时

查阅以下文档获取详细信息：

- `docs/custom-board.md` - 自定义开发板
- `docs/mcp-protocol.md` - MCP 协议细节
- `docs/websocket.md` - WebSocket 通信
- `docs/mqtt-udp.md` - MQTT+UDP 混合通信
- `AGENTS.md` - 完整项目结构说明
