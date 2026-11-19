SELECT
    (SELECT COALESCE(SUM(cool_sec + fan_low_sec + fan_high_sec + offline_sec), 0)
     FROM ac_buckets
     WHERE ts_utc >= ? AND ts_utc < ?) AS raw_total_sec,
    (SELECT COALESCE(SUM(cool_sec + fan_low_sec + fan_high_sec + offline_sec), 0)
     FROM ac_buckets_hourly
     WHERE ts_utc >= ? AND ts_utc < ?) AS hourly_total_sec;
