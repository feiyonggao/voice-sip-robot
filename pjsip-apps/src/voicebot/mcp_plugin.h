#pragma once

#include <QList>
#include <QString>
#include <QStringList>

class QSettings;

namespace qwenbot {

// 单个 MCP 插件配置：stdio（本地脚本）或 http（远端服务），可混用。
// name 为唯一标识：本地插件取文件名，远端服务取用户自定义名。
struct McpPluginSpec {
    QString name;
    QString transport = QStringLiteral("stdio"); // stdio / http
    QString command;                             // stdio 完整启动命令行（空则由 name 解析）
    QString url;                                 // http 端点（Streamable HTTP 单端点）
    QStringList headers;                         // 附加请求头，如 "Authorization: Bearer xxx"

    // 兼容手工/旧版配置里的多种写法，统一按 HTTP 处理
    bool isHttp() const
    {
        const QString value = transport.trimmed().toLower();
        return value == QStringLiteral("http")
            || value == QStringLiteral("streamable_http")
            || value == QStringLiteral("streamable-http")
            || value == QStringLiteral("streamablehttp");
    }
    // 热插拔差异比对键：任一字段变化都视为新配置，需要重启
    QString key() const;
};

// tools/plugins（插件名称列表）+ tools/plugin/<名称>/{transport,url,headers,command}
// 的读写。兼容 QSettings INI 原生列表语义，避免 JSON 在 INI 中被当作字符串列表拆分。
QList<McpPluginSpec> loadMcpPlugins(QSettings &settings);
void saveMcpPlugins(QSettings &settings, const QList<McpPluginSpec> &plugins);

} // namespace qwenbot
