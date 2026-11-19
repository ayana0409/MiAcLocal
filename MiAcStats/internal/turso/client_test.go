package turso

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"

	"miacstats/internal/config"
	"miacstats/internal/model"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) {
	return f(r)
}

func TestTursoPipelineQueryAndSaveSettings(t *testing.T) {
	var seenMethod string
	var seenAuth string
	var seenURL string
	var reqBodies []string

	client := NewClient("https://demo-org.turso.io", "test-token")
	client.httpClient.Transport = roundTripFunc(func(r *http.Request) (*http.Response, error) {
		seenMethod = r.Method
		seenAuth = r.Header.Get("Authorization")
		seenURL = r.URL.String()
		b, _ := io.ReadAll(r.Body)
		reqBodies = append(reqBodies, string(b))

		var respJSON string
		if len(reqBodies) == 1 {
			respJSON = `{
				"results": [{
					"type": "ok",
					"response": {
						"type": "execute",
						"result": {
							"cols": [
								{"name":"device_id"},{"name":"ts_utc"},{"name":"session_start_utc"},{"name":"duration_sec"},
								{"name":"cool_sec"},{"name":"fan_low_sec"},{"name":"fan_high_sec"},{"name":"offline_sec"},
								{"name":"room_temp_avg"},{"name":"room_temp_min"},{"name":"room_temp_max"},
								{"name":"ac_temp_avg"},{"name":"target_temp"},{"name":"auto_sec"}
							],
							"rows": [[
								{"type":"text","value":"miac-main"},
								{"type":"integer","value":"1762870800"},
								{"type":"integer","value":"1762870000"},
								{"type":"integer","value":"60"},
								{"type":"integer","value":"60"},
								{"type":"integer","value":"0"},
								{"type":"integer","value":"0"},
								{"type":"integer","value":"0"},
								{"type":"float","value":25.6},
								{"type":"float","value":25.4},
								{"type":"float","value":25.8},
								{"type":"float","value":26.5},
								{"type":"integer","value":"26"},
								{"type":"integer","value":"60"}
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

	buckets, err := client.QueryBuckets(context.Background(), "miac-main", 1762870000, 1762880000)
	if err != nil {
		t.Fatalf("QueryBuckets failed: %v", err)
	}
	if seenMethod != http.MethodPost || seenURL != "https://demo-org.turso.io/v2/pipeline" || seenAuth != "Bearer test-token" {
		t.Fatalf("unexpected HTTP call: %s %s auth=%s", seenMethod, seenURL, seenAuth)
	}
	if len(buckets) != 1 {
		t.Fatalf("expected 1 bucket, got %d", len(buckets))
	}
	if buckets[0].TsUTC != 1762870800 || buckets[0].CoolSec != 60 || buckets[0].RoomTempAvg == nil || *buckets[0].RoomTempAvg != 25.6 {
		t.Fatalf("unexpected decoded bucket: %+v", buckets[0])
	}

	// Verify integer arguments are encoded as strings per Hrana v2/pipeline spec
	var sent map[string]any
	if err := json.Unmarshal([]byte(reqBodies[0]), &sent); err != nil {
		t.Fatalf("invalid sent JSON: %v", err)
	}
	reqs := sent["requests"].([]any)
	stmtMap := reqs[0].(map[string]any)["stmt"].(map[string]any)
	sqlStr := stmtMap["sql"].(string)
	if !strings.Contains(sqlStr, "ac_buckets_hourly") {
		t.Fatalf("expected query to reference ac_buckets_hourly, got %s", sqlStr)
	}
	args := stmtMap["args"].([]any)
	if len(args) != 6 {
		t.Fatalf("expected 6 query args, got %d", len(args))
	}
	arg1 := args[1].(map[string]any)
	if arg1["type"] != "integer" || arg1["value"] != "1762870000" {
		t.Fatalf("expected string-encoded integer arg, got %+v", arg1)
	}

	if err := client.SaveBillingSettings(context.Background(), model.BillingSettings{
		CoolWatts:    1000,
		FanLowWatts:  35,
		FanHighWatts: 55,
		TariffMode:   "evn",
		VATPercent:   8,
		EvnTiers:     config.DefaultEvnTiers(),
	}); err != nil {
		t.Fatalf("SaveBillingSettings failed: %v", err)
	}
}

func TestTursoPipelineErrorsPropagate(t *testing.T) {
	client := NewClient("https://demo-org.turso.io", "test-token")
	client.httpClient.Transport = roundTripFunc(func(_ *http.Request) (*http.Response, error) {
		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body: io.NopCloser(bytes.NewBufferString(`{
				"results": [{"type": "error", "error": {"message": "no such table: ac_buckets", "code": "SQL_INPUT_ERROR"}}]
			}`)),
		}, nil
	})

	if _, err := client.QueryBuckets(context.Background(), "miac-main", 0, 100); err == nil {
		t.Fatal("expected error for turso error response")
	}
}
