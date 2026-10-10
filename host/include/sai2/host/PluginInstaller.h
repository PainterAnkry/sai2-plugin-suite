#pragma once
// ============================================================================
// PluginInstaller.h - 插件安装 / 卸载（规格书第六节）
//
// 安装流程：选择插件 -> 读取 Manifest -> API/完整性验证 -> 安装到 plugins -> 注册
//
// 支持两种来源：
//   1. 目录（开发期调试）
//   2. .sai-plugin 包（ZIP 容器；本阶段支持 STORED 条目并做 CRC32 校验，
//      DEFLATE 条目将在接入 zlib 后支持，遇到时给出明确错误）
// ============================================================================

#include "sai2/host/PluginRegistry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sai2::host {

class PluginInstaller {
public:
    PluginInstaller(std::string pluginsRoot, PluginRegistry& registry,
                    uint32_t hostApiVersion);

    // 从目录安装（复制 manifest.json 与全部文件）
    bool InstallFromDirectory(const std::string& srcDir, std::string& error);

    // 从 .sai-plugin（ZIP）安装
    bool InstallFromPackage(const std::string& packagePath, std::string& error);

    // 卸载：删除插件目录并从注册表移除（加载中的插件由 PluginManager 先停止）
    bool Uninstall(const std::string& pluginId, std::string& error);

    // 从已解析的 Manifest 安装（供宿主程序化安装与测试使用）
    bool InstallFromManifest(const PluginManifest& manifest,
                             const std::string& srcDir, std::string& error);

private:
    std::string pluginsRoot_;
    PluginRegistry& registry_;
    uint32_t hostApiVersion_;
};

} // namespace sai2::host
