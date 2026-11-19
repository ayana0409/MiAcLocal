package turso

import (
	"bytes"
	"context"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"

	"miacstats/internal/model"
)

const billingConfigKey = "billing_settings_v1"

//go:embed sql/query_buckets.sql
var queryBucketsSQL string

// Store defines database operations required by MiAcStats handlers.
type Store interface {
	EnsureSchema(ctx context.Context) error
	QueryBuckets(ctx context.Context, deviceID string, fromUTC, toUTC int64) ([]model.Bucket, error)
	LoadBillingSettings(ctx context.Context, fallback model.BillingSettings) (model.BillingSettings, error)
	SaveBillingSettings(ctx context.Context, cfg model.BillingSettings) error
}

// Client implements Store using Turso's Hrana over HTTP (/v2/pipeline) API without CGO.
type Client struct {
	baseURL    string
	authToken  string
	httpClient *http.Client
}

// NewClient constructs a Turso HTTP pipeline client.
func NewClient(baseURL, authToken string) *Client {
	u := strings.TrimRight(strings.TrimSpace(baseURL), "/")
	if strings.HasPrefix(u, "libsql://") {
		u = "https://" + strings.TrimPrefix(u, "libsql://")
	}
	return &Client{
		baseURL:   u,
		authToken: strings.TrimSpace(authToken),
		httpClient: &http.Client{
			Timeout: 15 * time.Second,
		},
	}
}

type pipelineRequest struct {
	Requests []requestItem `json:"requests"`
}

type requestItem struct {
	Type string `json:"type"`
	Stmt *stmt  `json:"stmt,omitempty"`
}

type stmt struct {
	SQL  string `json:"sql"`
	Args []arg  `json:"args,omitempty"`
}

type arg struct {
	Type  string `json:"type"`
	Value any    `json:"value,omitempty"`
}

type pipelineResponse struct {
	Results []resultItem `json:"results"`
}

type resultItem struct {
	Type     string       `json:"type"`
	Error    *resultError `json:"error,omitempty"`
	Response *execWrap    `json:"response,omitempty"`
}

type resultError struct {
	Message string `json:"message"`
	Code    string `json:"code"`
}

type execWrap struct {
	Type   string     `json:"type"`
	Result execResult `json:"result"`
}

type execResult struct {
	Cols []colMeta   `json:"cols"`
	Rows [][]rawCell `json:"rows"`
}

type colMeta struct {
	Name string `json:"name"`
}

type rawCell struct {
	Type  string          `json:"type"`
	Value json.RawMessage `json:"value"`
}

// EnsureSchema creates tables and indexes idempotently.
func (c *Client) EnsureSchema(ctx context.Context) error {
	stmts := []stmt{
		{
			SQL: `CREATE TABLE IF NOT EXISTS ac_buckets (
				device_id          TEXT    NOT NULL,
				ts_utc             INTEGER NOT NULL,
				session_start_utc  INTEGER NOT NULL,
				duration_sec       INTEGER NOT NULL,
				cool_sec           INTEGER NOT NULL,
				fan_low_sec        INTEGER NOT NULL,
				fan_high_sec       INTEGER NOT NULL,
				offline_sec        INTEGER NOT NULL DEFAULT 0,
				room_temp_avg      REAL,
				room_temp_min      REAL,
				room_temp_max      REAL,
				ac_temp_avg        REAL,
				target_temp        INTEGER,
				auto_sec           INTEGER NOT NULL DEFAULT 0,
				PRIMARY KEY (device_id, ts_utc)
			)`,
		},
		{
			SQL: `CREATE INDEX IF NOT EXISTS idx_ac_buckets_ts ON ac_buckets (ts_utc)`,
		},
		{
			SQL: `CREATE INDEX IF NOT EXISTS idx_ac_buckets_session ON ac_buckets (device_id, session_start_utc)`,
		},
		{
			SQL: `CREATE TABLE IF NOT EXISTS app_config (
				key        TEXT PRIMARY KEY,
				value      TEXT NOT NULL,
				updated_at INTEGER NOT NULL
			)`,
		},
		{
			SQL: `CREATE TABLE IF NOT EXISTS ac_buckets_hourly (
				device_id          TEXT    NOT NULL,
				ts_utc             INTEGER NOT NULL,
				session_start_utc  INTEGER NOT NULL,
				duration_sec       INTEGER NOT NULL,
				cool_sec           INTEGER NOT NULL,
				fan_low_sec        INTEGER NOT NULL,
				fan_high_sec       INTEGER NOT NULL,
				offline_sec        INTEGER NOT NULL DEFAULT 0,
				room_temp_avg      REAL,
				room_temp_min      REAL,
				room_temp_max      REAL,
				ac_temp_avg        REAL,
				target_temp        INTEGER,
				auto_sec           INTEGER NOT NULL DEFAULT 0,
				PRIMARY KEY (device_id, ts_utc)
			)`,
		},
		{
			SQL: `CREATE INDEX IF NOT EXISTS idx_ac_buckets_hourly_ts ON ac_buckets_hourly (ts_utc)`,
		},
		{
			SQL: `CREATE TRIGGER IF NOT EXISTS trg_ac_buckets_after_watermark
				BEFORE INSERT ON ac_buckets
				WHEN NEW.ts_utc < COALESCE((SELECT CAST(value AS INTEGER) FROM app_config WHERE key = 'rollup_watermark'), 0)
				BEGIN
					SELECT RAISE(IGNORE);
				END;`,
		},
	}
	_, err := c.execPipeline(ctx, stmts)
	return err
}

// QueryBuckets returns time-ordered buckets inside [fromUTC, toUTC) for deviceID,
// reading aggregated hourly records below rollup_watermark and raw buckets from watermark onwards.
func (c *Client) QueryBuckets(ctx context.Context, deviceID string, fromUTC, toUTC int64) ([]model.Bucket, error) {
	query := stmt{
		SQL: strings.TrimSpace(queryBucketsSQL),
		Args: []arg{
			textArg(deviceID),
			intArg(fromUTC),
			intArg(toUTC),
			textArg(deviceID),
			intArg(fromUTC),
			intArg(toUTC),
		},
	}

	results, err := c.execPipeline(ctx, []stmt{query})
	if err != nil {
		return nil, err
	}
	if len(results) == 0 {
		return nil, nil
	}

	rows := results[0].Rows
	out := make([]model.Bucket, 0, len(rows))
	for _, r := range rows {
		if len(r) < 14 {
			continue
		}
		b := model.Bucket{
			DeviceID:        cellString(r[0]),
			TsUTC:           cellInt64(r[1]),
			SessionStartUTC: cellInt64(r[2]),
			DurationSec:     int(cellInt64(r[3])),
			CoolSec:         int(cellInt64(r[4])),
			FanLowSec:       int(cellInt64(r[5])),
			FanHighSec:      int(cellInt64(r[6])),
			OfflineSec:      int(cellInt64(r[7])),
			RoomTempAvg:     cellFloatPtr(r[8]),
			RoomTempMin:     cellFloatPtr(r[9]),
			RoomTempMax:     cellFloatPtr(r[10]),
			AcTempAvg:       cellFloatPtr(r[11]),
			TargetTemp:      cellIntPtr(r[12]),
			AutoSec:         int(cellInt64(r[13])),
		}
		out = append(out, b)
	}
	return out, nil
}

// LoadBillingSettings retrieves saved billing assumptions from app_config, falling back to defaults.
func (c *Client) LoadBillingSettings(ctx context.Context, fallback model.BillingSettings) (model.BillingSettings, error) {
	query := stmt{
		SQL:  `SELECT value FROM app_config WHERE key = ? LIMIT 1`,
		Args: []arg{textArg(billingConfigKey)},
	}
	results, err := c.execPipeline(ctx, []stmt{query})
	if err != nil {
		return fallback, err
	}
	if len(results) == 0 || len(results[0].Rows) == 0 || len(results[0].Rows[0]) == 0 {
		return fallback, nil
	}
	rawJSON := cellString(results[0].Rows[0][0])
	if rawJSON == "" {
		return fallback, nil
	}
	var parsed model.BillingSettings
	if err := json.Unmarshal([]byte(rawJSON), &parsed); err != nil {
		return fallback, err
	}
	if len(parsed.EvnTiers) == 0 {
		parsed.EvnTiers = fallback.EvnTiers
	}
	return parsed, nil
}

// SaveBillingSettings stores billing assumptions in app_config.
func (c *Client) SaveBillingSettings(ctx context.Context, cfg model.BillingSettings) error {
	payload, err := json.Marshal(cfg)
	if err != nil {
		return err
	}
	s := stmt{
		SQL: `INSERT INTO app_config (key, value, updated_at) VALUES (?, ?, ?)
		      ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at`,
		Args: []arg{
			textArg(billingConfigKey),
			textArg(string(payload)),
			intArg(time.Now().UTC().Unix()),
		},
	}
	_, err = c.execPipeline(ctx, []stmt{s})
	return err
}

func (c *Client) execPipeline(ctx context.Context, stmts []stmt) ([]execResult, error) {
	if c.baseURL == "" || c.authToken == "" {
		return nil, errors.New("TURSO_DATABASE_URL and TURSO_AUTH_TOKEN must be configured")
	}

	reqBody := pipelineRequest{
		Requests: make([]requestItem, 0, len(stmts)+1),
	}
	for i := range stmts {
		s := stmts[i]
		reqBody.Requests = append(reqBody.Requests, requestItem{
			Type: "execute",
			Stmt: &s,
		})
	}
	reqBody.Requests = append(reqBody.Requests, requestItem{Type: "close"})

	encoded, err := json.Marshal(reqBody)
	if err != nil {
		return nil, err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+"/v2/pipeline", bytes.NewReader(encoded))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+c.authToken)
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, err
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("turso HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}

	var parsed pipelineResponse
	if err := json.Unmarshal(body, &parsed); err != nil {
		return nil, fmt.Errorf("decode turso response: %w", err)
	}

	out := make([]execResult, 0, len(stmts))
	for i, item := range parsed.Results {
		if item.Type == "error" {
			msg := "unknown turso error"
			if item.Error != nil && item.Error.Message != "" {
				msg = item.Error.Message
			}
			return nil, fmt.Errorf("turso statement %d failed: %s", i, msg)
		}
		if item.Response != nil && item.Response.Type == "execute" {
			out = append(out, item.Response.Result)
		}
	}
	return out, nil
}

func textArg(v string) arg {
	return arg{Type: "text", Value: v}
}

func intArg(v int64) arg {
	return arg{Type: "integer", Value: strconv.FormatInt(v, 10)}
}

func cellString(c rawCell) string {
	if c.Type == "null" || len(c.Value) == 0 || string(c.Value) == "null" {
		return ""
	}
	var s string
	if err := json.Unmarshal(c.Value, &s); err == nil {
		return s
	}
	return strings.Trim(string(c.Value), `"`)
}

func cellInt64(c rawCell) int64 {
	if c.Type == "null" || len(c.Value) == 0 || string(c.Value) == "null" {
		return 0
	}
	var s string
	if err := json.Unmarshal(c.Value, &s); err == nil {
		v, err := strconv.ParseInt(s, 10, 64)
		if err == nil {
			return v
		}
	}
	var n int64
	if err := json.Unmarshal(c.Value, &n); err == nil {
		return n
	}
	return 0
}

func cellIntPtr(c rawCell) *int {
	if c.Type == "null" || len(c.Value) == 0 || string(c.Value) == "null" {
		return nil
	}
	v := int(cellInt64(c))
	return &v
}

func cellFloatPtr(c rawCell) *float64 {
	if c.Type == "null" || len(c.Value) == 0 || string(c.Value) == "null" {
		return nil
	}
	var f float64
	if err := json.Unmarshal(c.Value, &f); err == nil {
		return &f
	}
	var s string
	if err := json.Unmarshal(c.Value, &s); err == nil {
		if parsed, err := strconv.ParseFloat(s, 64); err == nil {
			return &parsed
		}
	}
	return nil
}
