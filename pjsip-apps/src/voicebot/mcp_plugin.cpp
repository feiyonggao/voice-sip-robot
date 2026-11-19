#include "mcp_plugin.h"

#include <QSettings>

namespace qwenbot {
namespace {

// 插件名可能被用作 QSettings 分组键，去掉会破坏层级或转义的字符
QString groupKey(const QString &name)
{
    QString key = name;
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    key.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return key;
}

QStringList splitHeaders(const QString &value)
{
    QStringList headers;
    for (const auto &part : value.split(QLatin1Char(';'))) {
        const QString header = part.trimmed();
        if (!header.isEmpty()) headers.append(header);
    }
    return headers;
}

} // namespace

QString McpPluginSpec::key() const
{
    return QStringList{transport.toLower(), name, command, url,
                       headers.join(QLatin1Char('\n'))}
        .join(QStringLiteral("\x1f"));
}

QList<McpPluginSpec> loadMcpPlugins(QSettings &settings)
{
    QList<McpPluginSpec> plugins;
    if (!settings.contains(QStringLiteral("tools/plugins"))) return plugins;
    const QStringList names = settings.value(QStringLiteral("tools/plugins")).toStringList();
    for (const auto &rawName : names) {
        const QString name = rawName.trimmed();
        if (name.isEmpty()) continue;
        const QString base = QStringLiteral("tools/plugin/") + groupKey(name) + QLatin1Char('/');
        McpPluginSpec spec;
        spec.name = name;
        spec.transport = settings.value(base + QStringLiteral("transport"),
                                        QStringLiteral("stdio")).toString();
        // 归一化：http / streamable_http 等写法统一成 http，其余一律视为 stdio，
        // 避免界面与运行时对同一份配置给出不同的传输方式
        spec.transport = spec.isHttp() ? QStringLiteral("http") : QStringLiteral("stdio");
        spec.command = settings.value(base + QStringLiteral("command")).toString();
        spec.url = settings.value(base + QStringLiteral("url")).toString().trimmed();
        spec.headers = splitHeaders(settings.value(base + QStringLiteral("headers")).toString());
        plugins.append(spec);
    }
    return plugins;
}

void saveMcpPlugins(QSettings &settings, const QList<McpPluginSpec> &plugins)
{
    // 先清空旧的插件分组，避免删除插件后残留配置
    settings.remove(QStringLiteral("tools/plugin"));
    QStringList names;
    for (const auto &spec : plugins) {
        if (spec.name.isEmpty()) continue;
        names.append(spec.name);
        const QString base =
            QStringLiteral("tools/plugin/") + groupKey(spec.name) + QLatin1Char('/');
        settings.setValue(base + QStringLiteral("transport"),
                          spec.isHttp() ? QStringLiteral("http") : QStringLiteral("stdio"));
        if (!spec.command.isEmpty())
            settings.setValue(base + QStringLiteral("command"), spec.command);
        if (!spec.url.isEmpty())
            settings.setValue(base + QStringLiteral("url"), spec.url);
        if (!spec.headers.isEmpty())
            settings.setValue(base + QStringLiteral("headers"),
                              spec.headers.join(QStringLiteral("; ")));
    }
    settings.setValue(QStringLiteral("tools/plugins"), names);
}

} // namespace qwenbot
