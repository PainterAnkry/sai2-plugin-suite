#pragma once
// ============================================================================
// AgentBridge.h - 桥面板侧的 Agent 访问层
//
// 把 sai2_agent 的管道 JSON（layers/text）解析成面板可直接使用的结构。
// 解析器只针对 agent 的输出格式（见 docs/SAI2Injection.md），不做通用 JSON。
// ============================================================================

#include "../agent/AgentClient.h"

#include <cstdio>
#include <string>
#include <vector>

namespace sai2bridge {

struct AgentLayerRow {
    int y = 0;          // 行文字参考点（面板客户区坐标）
    int screenX = 0;    // 屏幕坐标（点击选中用）
    int screenY = 0;
    long long tick = 0; // 最近绘制时刻（GetTickCount64）
    std::wstring text;  // 图层名（或该行其它文字）
};

struct AgentLayerGroup {
    long long hwnd = 0;
    std::wstring cls;
    bool isChild = false;
    RECT screenRect = {}; // 查询时的窗口屏幕矩形
    std::vector<AgentLayerRow> rows; // 按 y 升序
};

struct AgentLayers {
    bool ok = false;
    std::vector<AgentLayerGroup> groups;
    long long panelGuess = 0;
};

namespace detail {

// 在 [begin,end) 范围里找一个完整 JSON 对象（{...}，含嵌套配对）
inline bool NextObject(const std::string& s, size_t begin, size_t end,
                       size_t* objBegin, size_t* objEnd) {
    size_t i = begin;
    while (i < end && s[i] != '{') ++i;
    if (i >= end) return false;
    *objBegin = i;
    int depth = 0;
    bool inStr = false;
    for (; i < end; ++i) {
        const char c = s[i];
        if (inStr) {
            if (c == '\\') ++i;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) {
                *objEnd = i + 1;
                return true;
            }
        }
    }
    return false;
}

inline std::wstring Utf8ToWide(const std::string& s) {
    std::wstring out;
    if (s.empty()) return out;
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], n);
    return out;
}

} // namespace detail

// 解析 agent `layers` 响应
inline bool ParseLayers(const std::string& json, AgentLayers& out) {
    out.ok = json.find("\"ok\":true") != std::string::npos;
    if (!out.ok) return false;
    sai2agent::JsonFindInt(json, "panelGuess", out.panelGuess);

    const std::string groupsPat = "\"groups\":[";
    const size_t gp = json.find(groupsPat);
    if (gp == std::string::npos) return false;
    // groups 数组终点（配对 ']）
    size_t gEnd = gp + groupsPat.size();
    {
        int depth = 1;
        bool inStr = false;
        for (size_t i = gEnd; i < json.size(); ++i) {
            const char c = json[i];
            if (inStr) {
                if (c == '\\') ++i;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == '[') ++depth;
            else if (c == ']') {
                if (--depth == 0) { gEnd = i; break; }
            }
        }
    }

    size_t b = gp + groupsPat.size(), e = 0;
    while (detail::NextObject(json, b, gEnd, &b, &e)) {
        const std::string g = json.substr(b, e - b);
        AgentLayerGroup grp;
        sai2agent::JsonFindInt(g, "hwnd", grp.hwnd);
        {
            std::string cls;
            if (sai2agent::JsonFindString(g, "cls", cls))
                grp.cls = detail::Utf8ToWide(cls);
        }
        grp.isChild = g.find("\"isChild\":true") != std::string::npos;
        // screenRect:[l,t,r,b]
        const std::string srPat = "\"screenRect\":[";
        const size_t sp = g.find(srPat);
        if (sp != std::string::npos) {
            long long v[4] = {0, 0, 0, 0};
            size_t p = sp + srPat.size();
            bool okAll = true;
            for (int k = 0; k < 4; ++k) {
                // 每个数字后面跟 , 或 ]
                long long acc = 0, sign = 1;
                while (p < g.size() && (g[p] == ' ')) ++p;
                if (p < g.size() && g[p] == '-') { sign = -1; ++p; }
                if (p >= g.size() || g[p] < '0' || g[p] > '9') { okAll = false; break; }
                while (p < g.size() && g[p] >= '0' && g[p] <= '9') {
                    acc = acc * 10 + (g[p] - '0');
                    ++p;
                }
                v[k] = acc * sign;
                while (p < g.size() && (g[p] == ' ' || g[p] == ',')) ++p;
            }
            if (okAll) {
                grp.screenRect = {static_cast<LONG>(v[0]), static_cast<LONG>(v[1]),
                                  static_cast<LONG>(v[2]), static_cast<LONG>(v[3])};
            }
        }
        // rows
        const std::string rowsPat = "\"rows\":[";
        const size_t rp = g.find(rowsPat);
        if (rp != std::string::npos) {
            size_t rB = rp + rowsPat.size(), rE = 0;
            while (detail::NextObject(g, rB, e - b > 0 ? g.size() : g.size(),
                                      &rB, &rE) &&
                   rE <= g.size()) {
                // 行对象范围可能越过组末尾（'}'），限定到组字符串内
                if (rB >= g.size()) break;
                const std::string row = g.substr(rB, rE - rB);
                if (row.find("\"text\":") != std::string::npos) {
                    AgentLayerRow r;
                    long long y = 0, sx = 0, sy = 0, tick = 0;
                    sai2agent::JsonFindInt(row, "y", y);
                    sai2agent::JsonFindInt(row, "screenX", sx);
                    sai2agent::JsonFindInt(row, "screenY", sy);
                    sai2agent::JsonFindInt(row, "tick", tick);
                    r.y = static_cast<int>(y);
                    r.screenX = static_cast<int>(sx);
                    r.screenY = static_cast<int>(sy);
                    r.tick = tick;
                    std::string t8;
                    if (sai2agent::JsonFindString(row, "text", t8))
                        r.text = detail::Utf8ToWide(t8);
                    grp.rows.push_back(std::move(r));
                }
                rB = rE;
                if (rE >= g.size()) break;
            }
        }
        out.groups.push_back(std::move(grp));
        b = e;
    }
    return true;
}

// 查询图层（连接 → 命令 → 解析）
inline bool QueryLayers(AgentLayers& out, const wchar_t* pipeName = nullptr) {
    std::string resp;
    if (!sai2agent::AgentClient::Query("{\"cmd\":\"layers\"}", resp,
                                       pipeName)) {
        out.ok = false;
        return false;
    }
    return ParseLayers(resp, out);
}

// ping agent；成功时 info 形如 "pid=1234"
inline bool AgentPing(long long& pid) {
    std::string resp;
    if (!sai2agent::AgentClient::Query("{\"cmd\":\"ping\"}", resp)) return false;
    if (resp.find("\"ok\":true") == std::string::npos) return false;
    sai2agent::JsonFindInt(resp, "pid", pid);
    return true;
}

// ---- SAI2 内插件入口协议 ------------------------------------------------

// UTF-8 文本的 JSON 转义（输出含首尾引号）
inline std::string JsonEscapeUtf8(const std::string& u8) {
    std::string out;
    out += '"';
    for (unsigned char c : u8) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

// 把功能清单（模糊/滤镜/调整/液化）推给 agent —— SAI2 内浮窗按钮的数据源。
// id 用功能窗口索引（1..4，与 BridgeCore::kWin* 对应）。
struct AgentCategoryItem {
    int id = 0;         // 功能窗口索引
    std::string name;   // UTF-8 显示名
};

inline bool PushCategories(const std::vector<AgentCategoryItem>& items,
                           const wchar_t* pipeName = nullptr) {
    std::string json = "{\"cmd\":\"set.plugins\",\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) json += ",";
        json += "{\"i\":";
        json += std::to_string(items[i].id);
        json += ",\"g\":";
        json += JsonEscapeUtf8(items[i].name);
        json += ",\"n\":";
        json += JsonEscapeUtf8(items[i].name);
        json += "}";
    }
    json += "]}";
    std::string resp;
    return sai2agent::AgentClient::Query(json, resp, pipeName);
}

// 轮询"用户在 SAI2 浮窗里点了哪个功能"（无请求返回 false）；
// id 为功能窗口索引（1..4）
inline bool TakeCategoryRequest(int& winIdx, const wchar_t* pipeName = nullptr) {
    std::string resp;
    if (!sai2agent::AgentClient::Query("{\"cmd\":\"take.request\"}", resp,
                                       pipeName))
        return false;
    if (resp.find("\"request\":null") != std::string::npos) return false;
    long long idx = -1;
    if (!sai2agent::JsonFindInt(resp, "index", idx) || idx <= 0) return false;
    winIdx = static_cast<int>(idx);
    return true;
}

} // namespace sai2bridge
