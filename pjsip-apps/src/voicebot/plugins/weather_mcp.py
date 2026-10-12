# 全球天气查询 MCP 插件示例（stdio 传输）
#
# 用法：
#   1. 安装依赖：pip install "mcp[cli]" httpx
#   2. 在 voicebot 配置页「启用插件」下拉中勾选 weather_mcp.py
#      （无头模式在 INI 的 [tools] enabledPlugins 中填写文件名）
#   3. 保存配置后模型即可调用 get_weather 工具查询全球城市当前天气与未来预报。
#
# 数据源：Open-Meteo（免费，无需 API Key）。支持全球城市（中英文均可），
# days 控制预报天数（1–7，默认 3）。

import json

import httpx
from mcp.server.fastmcp import FastMCP

mcp = FastMCP("weather")

# WMO 天气码 -> 中文文案（Open-Meteo weather_code 标准编码）
_WMO: dict[int, str] = {
    0: "晴",
    1: "晴间多云",
    2: "多云",
    3: "阴",
    45: "雾",
    48: "雾凇",
    51: "小毛毛雨",
    53: "毛毛雨",
    55: "大毛毛雨",
    56: "小冻雨",
    57: "冻雨",
    61: "小雨",
    63: "中雨",
    65: "大雨",
    66: "小冻雨",
    67: "大冻雨",
    71: "小雪",
    73: "中雪",
    75: "大雪",
    77: "雪粒",
    80: "小阵雨",
    81: "阵雨",
    82: "强阵雨",
    85: "小阵雪",
    86: "大阵雪",
    95: "雷暴",
    96: "雷暴伴小冰雹",
    99: "雷暴伴大冰雹",
}


def _wmo_label(code) -> str:
    try:
        n = int(code)
    except (TypeError, ValueError):
        return "未知"
    return _WMO.get(n, f"天气码{n}")


# Open-Meteo 地理编码不索引中文别名（"东京/纽约"只能匹配到中国同名村镇），
# 内置常用国际城市中文名 -> 英文拼写，保证说中文也能查到全球天气。
_INTERNATIONAL_ALIASES: dict[str, str] = {
    # 日本
    "东京": "Tokyo", "大阪": "Osaka", "京都": "Kyoto", "横滨": "Yokohama",
    "名古屋": "Nagoya", "札幌": "Sapporo", "福冈": "Fukuoka", "神户": "Kobe",
    # 韩国/朝鲜
    "首尔": "Seoul", "釜山": "Busan", "仁川": "Incheon", "平壤": "Pyongyang",
    # 东南亚
    "曼谷": "Bangkok", "新加坡": "Singapore", "吉隆坡": "Kuala Lumpur",
    "雅加达": "Jakarta", "马尼拉": "Manila", "河内": "Hanoi",
    "胡志明市": "Ho Chi Minh City", "金边": "Phnom Penh", "仰光": "Yangon",
    # 南亚/中东
    "新德里": "New Delhi", "孟买": "Mumbai", "班加罗尔": "Bangalore",
    "加尔各答": "Kolkata", "迪拜": "Dubai", "耶路撒冷": "Jerusalem",
    "特拉维夫": "Tel Aviv", "利雅得": "Riyadh", "多哈": "Doha",
    "伊斯坦布尔": "Istanbul", "德黑兰": "Tehran",
    # 欧洲
    "伦敦": "London", "巴黎": "Paris", "柏林": "Berlin", "罗马": "Rome",
    "米兰": "Milan", "马德里": "Madrid", "巴塞罗那": "Barcelona",
    "阿姆斯特丹": "Amsterdam", "莫斯科": "Moscow", "圣彼得堡": "Saint Petersburg",
    "维也纳": "Vienna", "布拉格": "Prague", "雅典": "Athens", "里斯本": "Lisbon",
    "布鲁塞尔": "Brussels", "苏黎世": "Zurich", "日内瓦": "Geneva",
    "哥本哈根": "Copenhagen", "斯德哥尔摩": "Stockholm", "奥斯陆": "Oslo",
    "赫尔辛基": "Helsinki", "华沙": "Warsaw", "布达佩斯": "Budapest",
    "慕尼黑": "Munich", "法兰克福": "Frankfurt", "威尼斯": "Venice",
    "佛罗伦萨": "Florence", "都柏林": "Dublin", "汉堡": "Hamburg",
    # 北美
    "纽约": "New York", "洛杉矶": "Los Angeles", "旧金山": "San Francisco",
    "西雅图": "Seattle", "芝加哥": "Chicago", "波士顿": "Boston",
    "华盛顿": "Washington", "温哥华": "Vancouver", "多伦多": "Toronto",
    "墨西哥城": "Mexico City", "迈阿密": "Miami", "拉斯维加斯": "Las Vegas",
    "休斯顿": "Houston", "达拉斯": "Dallas", "亚特兰大": "Atlanta",
    "蒙特利尔": "Montreal", "渥太华": "Ottawa",
    # 大洋洲
    "悉尼": "Sydney", "墨尔本": "Melbourne", "布里斯班": "Brisbane",
    "奥克兰": "Auckland", "堪培拉": "Canberra", "珀斯": "Perth",
    "惠灵顿": "Wellington",
    # 非洲/南美
    "开罗": "Cairo", "开普敦": "Cape Town", "约翰内斯堡": "Johannesburg",
    "内罗毕": "Nairobi", "拉各斯": "Lagos", "圣保罗": "Sao Paulo",
    "里约热内卢": "Rio de Janeiro", "布宜诺斯艾利斯": "Buenos Aires",
    "利马": "Lima", "圣地亚哥": "Santiago", "波哥大": "Bogota",
}


@mcp.tool()
def get_weather(city: str, days: int = 3) -> str:
    """查询全球城市的当前天气与未来多日预报。city 为城市名（中英文均可，如：北京、东京、London）；days 为预报天数（1–7，默认 3）。"""
    city = (city or "").strip()
    if not city:
        return json.dumps(
            {"ok": False, "error": "请提供城市名称，例如：北京、东京、London"},
            ensure_ascii=False,
        )
    day_n = max(1, min(int(days or 3), 7))
    try:
        # 1) 城市名 -> 经纬度。国际城市中文名先转英文拼写；英文拼写直接检索，
        #    二者用默认语言（返回英文原名，主要世界城市排第一、人口齐全）。
        #    纯中文且非国际别名的输入视为国内城市，加 language=zh 才能匹配
        #    （国内城市在库中默认名是拼音，如 Beijing）。
        # 先按名称首词精确过滤（New York 不选到 York），再按人口消歧，
        # 人口全缺时保留接口原始排序第一名。
        query_name = _INTERNATIONAL_ALIASES.get(city, city)
        geo_params = {"name": query_name, "count": 10, "format": "json"}
        if any(ord(ch) > 127 for ch in query_name):
            geo_params["language"] = "zh"
        geo = httpx.get(
            "https://geocoding-api.open-meteo.com/v1/search",
            params=geo_params,
            timeout=10,
        )
        geo.raise_for_status()
        results = geo.json().get("results") or []
        if not results:
            return json.dumps(
                {"ok": False, "error": f"未找到城市：{city}"}, ensure_ascii=False
            )
        # 先过滤名称以查询词首词开头的候选：避免 "New York" 选到 York（宾州小镇）、
        # "Washington DC" 选到其他华盛顿。再按人口消歧（人口字段覆盖不全，
        # 全缺时保留接口原始排序第一名）。
        first_word = query_name.split()[0].lower()
        exact = [
            item for item in results
            if str(item.get("name", "")).lower().startswith(first_word)
        ]
        candidates = exact or results
        if any(int(item.get("population") or 0) for item in candidates):
            location = max(
                candidates,
                key=lambda item: int(item.get("population") or 0),
            )
        else:
            location = candidates[0]

        # 2) 当前天气 + 逐日预报
        weather = httpx.get(
            "https://api.open-meteo.com/v1/forecast",
            params={
                "latitude": location["latitude"],
                "longitude": location["longitude"],
                "current": "temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m",
                "daily": (
                    "weather_code,temperature_2m_max,temperature_2m_min,"
                    "precipitation_probability_max"
                ),
                "forecast_days": day_n,
                "timezone": "auto",
            },
            timeout=10,
        )
        weather.raise_for_status()
        raw = weather.json()
    except Exception as exc:  # 网络异常等情况把原因交给模型，由其话术圆场
        return json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False)

    current = raw.get("current", {})
    daily = raw.get("daily", {})

    # 逐日预报行
    forecast = []
    dates = daily.get("time") or []
    codes = daily.get("weather_code") or []
    t_max = daily.get("temperature_2m_max") or []
    t_min = daily.get("temperature_2m_min") or []
    precip = daily.get("precipitation_probability_max") or []
    for i, date in enumerate(dates[:day_n]):
        forecast.append(
            {
                "date": date,
                "weather": _wmo_label(codes[i] if i < len(codes) else None),
                "temp_min_c": t_min[i] if i < len(t_min) else None,
                "temp_max_c": t_max[i] if i < len(t_max) else None,
                "precip_probability_percent":
                    precip[i] if i < len(precip) else None,
            }
        )

    return json.dumps(
        {
            "ok": True,
            "city": location.get("name", city),
            "country": location.get("country", ""),
            "current": {
                "temperature_c": current.get("temperature_2m"),
                "weather": _wmo_label(current.get("weather_code")),
                "humidity_percent": current.get("relative_humidity_2m"),
                "wind_kmh": current.get("wind_speed_10m"),
            },
            "forecast": forecast,
        },
        ensure_ascii=False,
    )


if __name__ == "__main__":
    mcp.run()  # 默认 stdio 传输，由 voicebot 作为子进程拉起
