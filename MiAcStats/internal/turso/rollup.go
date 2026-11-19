package turso

import (
	"context"
	_ "embed"
	"fmt"
	"strconv"
	"strings"
	"time"
)

//go:embed sql/rollup_insert.sql
var rollupInsertSQL string

//go:embed sql/rollup_verify.sql
var rollupVerifySQL string

const (
	watermarkConfigKey = "rollup_watermark"
	lastRunConfigKey   = "rollup_last_run_at"
)

// GetRollupState queries current watermark, last run timestamp, and the oldest raw bucket timestamp.
func (c *Client) GetRollupState(ctx context.Context) (watermark int64, lastRunAt int64, minRawTs int64, err error) {
	stmts := []stmt{
		{
			SQL:  `SELECT value FROM app_config WHERE key = ? LIMIT 1`,
			Args: []arg{textArg(watermarkConfigKey)},
		},
		{
			SQL:  `SELECT value FROM app_config WHERE key = ? LIMIT 1`,
			Args: []arg{textArg(lastRunConfigKey)},
		},
		{
			SQL: `SELECT MIN(ts_utc) FROM ac_buckets`,
		},
	}

	results, err := c.execPipeline(ctx, stmts)
	if err != nil {
		return 0, 0, 0, err
	}

	if len(results) > 0 && len(results[0].Rows) > 0 && len(results[0].Rows[0]) > 0 {
		watermark = cellInt64(results[0].Rows[0][0])
	}
	if len(results) > 1 && len(results[1].Rows) > 0 && len(results[1].Rows[0]) > 0 {
		lastRunAt = cellInt64(results[1].Rows[0][0])
	}
	if len(results) > 2 && len(results[2].Rows) > 0 && len(results[2].Rows[0]) > 0 {
		minRawTs = cellInt64(results[2].Rows[0][0])
	}

	return watermark, lastRunAt, minRawTs, nil
}

// RollupWindow aggregates raw buckets in [fromUTC, toUTC) into ac_buckets_hourly,
// verifies aggregate consistency, advances rollup_watermark, and purges raw buckets.
func (c *Client) RollupWindow(ctx context.Context, fromUTC, toUTC int64, dryRun bool) error {
	// Step 1: Insert or replace aggregated records into ac_buckets_hourly
	insertStmt := stmt{
		SQL:  strings.TrimSpace(rollupInsertSQL),
		Args: []arg{intArg(fromUTC), intArg(toUTC)},
	}
	if _, err := c.execPipeline(ctx, []stmt{insertStmt}); err != nil {
		return fmt.Errorf("rollup insert failed in window [%d, %d): %w", fromUTC, toUTC, err)
	}

	// Step 1b: Verify sum of active and offline seconds matches between tables
	verifyStmt := stmt{
		SQL: strings.TrimSpace(rollupVerifySQL),
		Args: []arg{
			intArg(fromUTC), intArg(toUTC),
			intArg(fromUTC), intArg(toUTC),
		},
	}
	verifyResults, err := c.execPipeline(ctx, []stmt{verifyStmt})
	if err != nil {
		return fmt.Errorf("rollup verification query failed in window [%d, %d): %w", fromUTC, toUTC, err)
	}
	if len(verifyResults) == 0 || len(verifyResults[0].Rows) == 0 || len(verifyResults[0].Rows[0]) < 2 {
		return fmt.Errorf("rollup verification returned invalid row structure")
	}

	rawTotalSec := cellInt64(verifyResults[0].Rows[0][0])
	hourlyTotalSec := cellInt64(verifyResults[0].Rows[0][1])
	if rawTotalSec != hourlyTotalSec {
		return fmt.Errorf("rollup verification failed: raw total %d sec != hourly total %d sec in window [%d, %d)",
			rawTotalSec, hourlyTotalSec, fromUTC, toUTC)
	}

	if dryRun {
		return nil
	}

	// Step 2: Advance watermark monotonically
	watermarkStmt := stmt{
		SQL: `INSERT INTO app_config (key, value, updated_at) VALUES (?, ?, ?)
		      ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at
		      WHERE CAST(excluded.value AS INTEGER) > CAST(app_config.value AS INTEGER)`,
		Args: []arg{
			textArg(watermarkConfigKey),
			textArg(strconv.FormatInt(toUTC, 10)),
			intArg(time.Now().UTC().Unix()),
		},
	}
	if _, err := c.execPipeline(ctx, []stmt{watermarkStmt}); err != nil {
		return fmt.Errorf("advance watermark to %d failed: %w", toUTC, err)
	}

	// Step 3: Purge raw buckets older than cutoff
	deleteStmt := stmt{
		SQL:  `DELETE FROM ac_buckets WHERE ts_utc < ?`,
		Args: []arg{intArg(toUTC)},
	}
	if _, err := c.execPipeline(ctx, []stmt{deleteStmt}); err != nil {
		return fmt.Errorf("purge raw buckets before %d failed: %w", toUTC, err)
	}

	return nil
}

// SetRollupLastRunAt records the timestamp when rollup execution completed successfully.
func (c *Client) SetRollupLastRunAt(ctx context.Context, ts int64) error {
	s := stmt{
		SQL: `INSERT INTO app_config (key, value, updated_at) VALUES (?, ?, ?)
		      ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at`,
		Args: []arg{
			textArg(lastRunConfigKey),
			textArg(strconv.FormatInt(ts, 10)),
			intArg(ts),
		},
	}
	_, err := c.execPipeline(ctx, []stmt{s})
	return err
}
