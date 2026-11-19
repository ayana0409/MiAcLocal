package turso

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
)

func TestTursoRollupLifecycle(t *testing.T) {
	var executedQueries []string

	client := NewClient("https://demo-org.turso.io", "test-token")
	client.httpClient.Transport = roundTripFunc(func(r *http.Request) (*http.Response, error) {
		b, _ := io.ReadAll(r.Body)
		var req pipelineRequest
		_ = json.Unmarshal(b, &req)

		for _, item := range req.Requests {
			if item.Stmt != nil {
				executedQueries = append(executedQueries, item.Stmt.SQL)
			}
		}

		var respJSON string
		// When verifying (contains raw_total_sec), return equal values
		lastQuery := ""
		if len(req.Requests) > 0 && req.Requests[0].Stmt != nil {
			lastQuery = req.Requests[0].Stmt.SQL
		}

		if strings.Contains(lastQuery, "raw_total_sec") {
			respJSON = `{
				"results": [{
					"type": "ok",
					"response": {
						"type": "execute",
						"result": {
							"cols": [{"name":"raw_total_sec"}, {"name":"hourly_total_sec"}],
							"rows": [[
								{"type":"integer","value":"3600"},
								{"type":"integer","value":"3600"}
							]]
						}
					}
				}, {"type":"ok"}]
			}`
		} else {
			respJSON = `{"results":[{"type":"ok","response":{"type":"execute","result":{"cols":[],"rows":[]}}},{"type":"ok"}]}`
		}

		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body:       io.NopCloser(bytes.NewBufferString(respJSON)),
		}, nil
	})

	ctx := context.Background()
	err := client.RollupWindow(ctx, 1000, 2000, false)
	if err != nil {
		t.Fatalf("RollupWindow failed: %v", err)
	}

	if len(executedQueries) != 4 {
		t.Fatalf("expected 4 executed statements (insert, verify, watermark, delete), got %d: %v", len(executedQueries), executedQueries)
	}
	if !strings.Contains(executedQueries[0], "INSERT OR REPLACE INTO ac_buckets_hourly") {
		t.Errorf("step 1 must be rollup insert, got %s", executedQueries[0])
	}
	if !strings.Contains(executedQueries[1], "raw_total_sec") {
		t.Errorf("step 1b must be rollup verify, got %s", executedQueries[1])
	}
	if !strings.Contains(executedQueries[2], "app_config") {
		t.Errorf("step 2 must update watermark in app_config, got %s", executedQueries[2])
	}
	if !strings.Contains(executedQueries[3], "DELETE FROM ac_buckets WHERE ts_utc < ?") {
		t.Errorf("step 3 must delete raw buckets, got %s", executedQueries[3])
	}
}

func TestTursoRollupDryRun(t *testing.T) {
	var executedQueries []string

	client := NewClient("https://demo-org.turso.io", "test-token")
	client.httpClient.Transport = roundTripFunc(func(r *http.Request) (*http.Response, error) {
		b, _ := io.ReadAll(r.Body)
		var req pipelineRequest
		_ = json.Unmarshal(b, &req)

		for _, item := range req.Requests {
			if item.Stmt != nil {
				executedQueries = append(executedQueries, item.Stmt.SQL)
			}
		}

		respJSON := `{
			"results": [{
				"type": "ok",
				"response": {
					"type": "execute",
					"result": {
						"cols": [{"name":"raw_total_sec"}, {"name":"hourly_total_sec"}],
						"rows": [[
							{"type":"integer","value":"1800"},
							{"type":"integer","value":"1800"}
						]]
					}
				}
			}, {"type":"ok"}]
		}`

		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body:       io.NopCloser(bytes.NewBufferString(respJSON)),
		}, nil
	})

	ctx := context.Background()
	err := client.RollupWindow(ctx, 1000, 2000, true)
	if err != nil {
		t.Fatalf("RollupWindow dryRun failed: %v", err)
	}

	// Dry run must execute insert and verify only, never watermark or delete
	if len(executedQueries) != 2 {
		t.Fatalf("expected 2 statements in dry-run, got %d: %v", len(executedQueries), executedQueries)
	}
	if strings.Contains(executedQueries[0], "DELETE") || strings.Contains(executedQueries[1], "DELETE") {
		t.Errorf("DELETE statement executed during dryRun")
	}
}

func TestTursoRollupVerifyMismatch(t *testing.T) {
	var executedQueries []string

	client := NewClient("https://demo-org.turso.io", "test-token")
	client.httpClient.Transport = roundTripFunc(func(r *http.Request) (*http.Response, error) {
		b, _ := io.ReadAll(r.Body)
		var req pipelineRequest
		_ = json.Unmarshal(b, &req)

		for _, item := range req.Requests {
			if item.Stmt != nil {
				executedQueries = append(executedQueries, item.Stmt.SQL)
			}
		}

		// Return mismatch: 3600 raw sec vs 3540 hourly sec
		respJSON := `{
			"results": [{
				"type": "ok",
				"response": {
					"type": "execute",
					"result": {
						"cols": [{"name":"raw_total_sec"}, {"name":"hourly_total_sec"}],
						"rows": [[
							{"type":"integer","value":"3600"},
							{"type":"integer","value":"3540"}
						]]
					}
				}
			}, {"type":"ok"}]
		}`

		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body:       io.NopCloser(bytes.NewBufferString(respJSON)),
		}, nil
	})

	ctx := context.Background()
	err := client.RollupWindow(ctx, 1000, 2000, false)
	if err == nil {
		t.Fatal("expected verification mismatch error, got nil")
	}
	if !strings.Contains(err.Error(), "rollup verification failed") {
		t.Fatalf("unexpected error message: %v", err)
	}

	// Must abort before watermark and delete
	if len(executedQueries) != 2 {
		t.Fatalf("expected exactly 2 statements before aborting, got %d: %v", len(executedQueries), executedQueries)
	}
}

func TestTursoGetRollupStateAndLastRun(t *testing.T) {
	client := NewClient("https://demo-org.turso.io", "test-token")
	client.httpClient.Transport = roundTripFunc(func(r *http.Request) (*http.Response, error) {
		b, _ := io.ReadAll(r.Body)
		var req pipelineRequest
		_ = json.Unmarshal(b, &req)

		var respJSON string
		if len(req.Requests) >= 3 {
			// GetRollupState
			respJSON = `{
				"results": [
					{
						"type": "ok",
						"response": {
							"type": "execute",
							"result": {
								"cols": [{"name":"value"}],
								"rows": [[{"type":"text","value":"1760000000"}]]
							}
						}
					},
					{
						"type": "ok",
						"response": {
							"type": "execute",
							"result": {
								"cols": [{"name":"value"}],
								"rows": [[{"type":"text","value":"1760050000"}]]
							}
						}
					},
					{
						"type": "ok",
						"response": {
							"type": "execute",
							"result": {
								"cols": [{"name":"MIN(ts_utc)"}],
								"rows": [[{"type":"integer","value":"1759000000"}]]
							}
						}
					},
					{"type": "ok"}
				]
			}`
		} else {
			respJSON = `{"results":[{"type":"ok","response":{"type":"execute","result":{"cols":[],"rows":[]}}},{"type":"ok"}]}`
		}

		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body:       io.NopCloser(bytes.NewBufferString(respJSON)),
		}, nil
	})

	ctx := context.Background()
	wm, lastRun, minTs, err := client.GetRollupState(ctx)
	if err != nil {
		t.Fatalf("GetRollupState failed: %v", err)
	}
	if wm != 1760000000 || lastRun != 1760050000 || minTs != 1759000000 {
		t.Fatalf("unexpected state: wm=%d lastRun=%d minTs=%d", wm, lastRun, minTs)
	}

	if err := client.SetRollupLastRunAt(ctx, 1760100000); err != nil {
		t.Fatalf("SetRollupLastRunAt failed: %v", err)
	}
}
