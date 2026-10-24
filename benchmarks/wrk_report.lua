-- 把 wrk 的最终读数打成一行 JSON，供 perf_matrix.py 解析。
-- wrk 自带的 Latency Distribution 只到 50/75/90/99，没有 p95；done() 拿到的
-- latency 直方图可以直接取任意分位。
request = function()
    return wrk.format(nil, path)
end

done = function(summary, latency, requests)
    local errors = summary.errors.connect + summary.errors.read + summary.errors.write + summary.errors.timeout + summary.errors.status
    local throughput = summary.requests / (summary.duration / 1000000.0)
    io.write(string.format(
        "WRK_JSON {\"throughput\": %.3f, \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"errors\": %d}\n",
        throughput,
        latency:percentile(50.0),
        latency:percentile(95.0),
        latency:percentile(99.0),
        errors))
end
