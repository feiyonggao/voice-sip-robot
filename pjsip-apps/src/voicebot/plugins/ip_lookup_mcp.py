# IP 归属地查询 MCP 插件（stdio 传输）
#
# 用法：
#   1. 安装依赖：pip install "mcp[cli]" httpx
#   2. 在 voicebot 配置页「启用插件」下拉中勾选 ip_lookup_mcp.py
#      （无头模式在 INI 的 [tools] enabledPlugins 中填写文件名）
#   3. 保存配置后模型即可调用 lookup_ip 工具查询 IP 归属地与运营商。
#
# 查询策略（优先最快通道，失败自动降级）：
#   1. ipwho.is    免费、HTTPS、限流宽松，超时 2.5s（国家代码转中文）——国内访问最快
#   2. ipinfo.io   免费 50k/月、HTTPS、稳定，超时 3s
#   3. ip-api.com  免费、支持中文归属地、HTTP，超时 3s
# 同一 IP 查询结果缓存 5 分钟，避免重复请求导致累积延迟。
# ip 留空时查询运行本插件的主机出口公网 IP。

import json
import threading
import time

import httpx
from mcp.server.fastmcp import FastMCP

mcp = FastMCP("ip-lookup")

# 国家代码 → 中文名（覆盖常用国家/地区，其余返回英文原名）
_COUNTRY_NAMES = {
    "CN": "中国", "HK": "中国香港", "TW": "中国台湾", "MO": "中国澳门",
    "US": "美国", "JP": "日本", "KR": "韩国", "SG": "新加坡",
    "GB": "英国", "DE": "德国", "FR": "法国", "RU": "俄罗斯",
    "CA": "加拿大", "AU": "澳大利亚", "IN": "印度", "VN": "越南",
    "TH": "泰国", "MY": "马来西亚", "ID": "印度尼西亚", "PH": "菲律宾",
    "BR": "巴西", "MX": "墨西哥", "ES": "西班牙", "IT": "意大利",
    "NL": "荷兰", "SE": "瑞典", "CH": "瑞士", "AE": "阿联酋",
    "SA": "沙特阿拉伯", "TR": "土耳其", "ZA": "南非", "NZ": "新西兰",
}

_CACHE_TTL = 300          # 缓存 5 分钟
_CACHE_MAX = 256          # 最多缓存条数
_cache: dict[str, tuple[float, dict]] = {}
_cache_lock = threading.Lock()


def _is_ipv4(text: str) -> bool:
    parts = text.split(".")
    if len(parts) != 4:
        return False
    for part in parts:
        if not part.isdigit() or not 0 <= int(part) <= 255:
            return False
    return True


def _country_zh(code: str, en: str = "") -> str:
    return _COUNTRY_NAMES.get((code or "").upper(), en or code)


def _cache_get(ip: str) -> dict | None:
    with _cache_lock:
        item = _cache.get(ip)
        if not item:
            return None
        ts, data = item
        if time.time() - ts > _CACHE_TTL:
            _cache.pop(ip, None)
            return None
        return dict(data)


def _cache_set(ip: str, data: dict) -> None:
    with _cache_lock:
        if len(_cache) >= _CACHE_MAX:
            # 淘汰最早一条
            oldest = min(_cache, key=lambda k: _cache[k][0])
            _cache.pop(oldest, None)
        _cache[ip] = (time.time(), dict(data))


def _query_ipapi(ip: str) -> dict:
    # 首选：支持中文，HTTP（免费版不支持 HTTPS），超时收紧到 2.5s
    resp = httpx.get(
        f"http://ip-api.com/json/{ip}",
        params={
            "fields": "status,message,country,countryCode,regionName,city,isp,query",
            "lang": "zh-CN",
        },
        timeout=2.5,
    )
    resp.raise_for_status()
    body = resp.json()
    if body.get("status") != "success":
        raise RuntimeError(str(body.get("message") or "ip-api 查询失败"))
    return {
        "ip": body.get("query") or ip,
        "country": body.get("country", ""),
        "country_code": body.get("countryCode", ""),
        "region": body.get("regionName", ""),
        "city": body.get("city", ""),
        "isp": body.get("isp", ""),
        "source": "ip-api.com",
    }


def _query_ipwho(ip: str) -> dict:
    # 兜底：HTTPS、限流宽松，国家代码转中文
    resp = httpx.get(
        f"https://ipwho.is/{ip}",
        params={"fields": "ip,success,country,country_code,region,city,connection"},
        timeout=3.0,
    )
    resp.raise_for_status()
    body = resp.json()
    if not body.get("success"):
        raise RuntimeError(str(body.get("message") or "ipwho.is 查询失败"))
    conn = body.get("connection") or {}
    return {
        "ip": body.get("ip") or ip,
        "country": _country_zh(body.get("country_code", ""), body.get("country", "")),
        "country_code": body.get("country_code", ""),
        "region": body.get("region", ""),
        "city": body.get("city", ""),
        "isp": conn.get("isp", ""),
        "source": "ipwho.is",
    }


def _query_ipinfo(ip: str) -> dict:
    # 第三兜底：稳定，country 为代码需转中文
    resp = httpx.get(f"https://ipinfo.io/{ip}/json", timeout=3.0)
    resp.raise_for_status()
    body = resp.json()
    if "country" not in body:
        raise RuntimeError(str(body.get("error", {}).get("title") or "ipinfo 查询失败"))
    return {
        "ip": body.get("ip") or ip,
        "country": _country_zh(body.get("country", ""), body.get("country", "")),
        "country_code": body.get("country", ""),
        "region": body.get("region", ""),
        "city": body.get("city", ""),
        "isp": body.get("org", ""),
        "source": "ipinfo.io",
    }


_SOURCES = [
    ("ipwho.is", _query_ipwho),
    ("ipinfo.io", _query_ipinfo),
    ("ip-api.com", _query_ipapi),
]


@mcp.tool()
def lookup_ip(ip: str = "") -> str:
    """查询 IP 的归属地与运营商（ISP）信息。ip 为 IPv4 地址；留空则查询本机公网 IP。"""
    target = (ip or "").strip()
    if target and not _is_ipv4(target):
        return json.dumps(
            {"ok": False, "error": f"不是有效的 IPv4 地址：{target}"},
            ensure_ascii=False,
        )

    cached = _cache_get(target or "<self>")
    if cached:
        result = {"ok": True, **cached, "cached": True}
        return json.dumps(result, ensure_ascii=False)

    errors = []
    for name, query in _SOURCES:
        try:
            data = query(target)
            _cache_set(target or "<self>", data)
            return json.dumps({"ok": True, **data, "cached": False}, ensure_ascii=False)
        except Exception as exc:
            errors.append(f"{name}: {exc}")
            continue  # 自动降级到下一个数据源

    return json.dumps(
        {"ok": False, "error": "所有数据源均不可用：" + "；".join(errors)},
        ensure_ascii=False,
    )


if __name__ == "__main__":
    mcp.run()  # 默认 stdio 传输，由 voicebot 作为子进程拉起
