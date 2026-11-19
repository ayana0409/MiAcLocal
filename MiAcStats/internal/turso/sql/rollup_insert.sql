INSERT OR REPLACE INTO ac_buckets_hourly (
    device_id, ts_utc, session_start_utc, duration_sec, cool_sec, fan_low_sec, fan_high_sec,
    offline_sec, room_temp_avg, room_temp_min, room_temp_max, ac_temp_avg, target_temp, auto_sec
)
SELECT
    device_id,
    MIN(ts_utc),
    session_start_utc,
    SUM(duration_sec),
    SUM(cool_sec),
    SUM(fan_low_sec),
    SUM(fan_high_sec),
    SUM(offline_sec),
    SUM(CASE WHEN room_temp_avg IS NOT NULL THEN room_temp_avg * (cool_sec + fan_low_sec + fan_high_sec) END)
      / NULLIF(SUM(CASE WHEN room_temp_avg IS NOT NULL THEN cool_sec + fan_low_sec + fan_high_sec END), 0),
    MIN(COALESCE(room_temp_min, room_temp_avg)),
    MAX(COALESCE(room_temp_max, room_temp_avg)),
    AVG(ac_temp_avg),
    CAST(ROUND(
      SUM(CASE WHEN target_temp IS NOT NULL THEN target_temp * (cool_sec + fan_low_sec + fan_high_sec) END) * 1.0
      / NULLIF(SUM(CASE WHEN target_temp IS NOT NULL THEN cool_sec + fan_low_sec + fan_high_sec END), 0)
    ) AS INTEGER),
    SUM(auto_sec)
FROM ac_buckets
WHERE ts_utc >= ? AND ts_utc < ?
GROUP BY device_id, session_start_utc, ts_utc / 3600;
